#include "war3_shadow_runtime_bridge.h"

#include "../../d3d9_war3_hook.h"
#include "../model/war3_model_hook.h"
#include "../model/war3_model_resource_cache.h"
#include "../model/war3_model_registry.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../core/war3_runtime_profile.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../hooks/war3_hook_render_identity.h"
#include "../shadow/war3_shadow_native_runtime.h"
#include "../shadow/war3_shadow_runtime_contract.h"
#include "../shadow/war3_shadow_renderer_core.h"
#include "war3_scene_collector.h"
#include "war3_shadow_object_registry.h"
#include "war3_upper_layer_shadow.h"
#include "war3_visible_renderables.h"
#include "../state/war3_render_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

namespace dxvk::war3::render {

namespace {

enum class SemanticBuildSkippedReason : uint64_t {
  None = 0,
  SemanticDataModuleDisabled = 1,
  ModelProducerDisabled = 2,
  FrameRegistriesDisabled = 3,
  ContractCaptureDisabled = 4,
  ConsumerDisabled = 5,
  SceneSubmissionDisabled = 6,
};

bool IsSemanticDataModuleEnabled() {
  return runtime::IsWar3RuntimeModuleEnabled(
      runtime::War3RuntimeModule::SemanticData);
}

bool IsSemanticModelProducerEnabled() {
  return IsSemanticDataModuleEnabled() &&
         internal::kWar3RuntimeConfigSemanticModelProducerEffective;
}

bool IsSemanticFrameRegistriesEnabled() {
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

SemanticBuildSkippedReason CurrentSemanticBuildSkippedReason() {
  if (!IsSemanticDataModuleEnabled())
    return SemanticBuildSkippedReason::SemanticDataModuleDisabled;
  if (!IsSemanticModelProducerEnabled())
    return SemanticBuildSkippedReason::ModelProducerDisabled;
  if (!IsSemanticFrameRegistriesEnabled())
    return SemanticBuildSkippedReason::FrameRegistriesDisabled;
  if (!IsSemanticContractCaptureEnabled())
    return SemanticBuildSkippedReason::ContractCaptureDisabled;
  if (!IsSemanticConsumerEnabled())
    return SemanticBuildSkippedReason::ConsumerDisabled;
  if (!internal::IsSemanticSceneSubmissionRuntimeEnabled())
    return SemanticBuildSkippedReason::SceneSubmissionDisabled;
  return SemanticBuildSkippedReason::None;
}

std::atomic<uint64_t> g_shadowBridgeRepairUntilFrame{0u};
std::atomic<uint64_t> g_shadowBridgeRepairCooldownUntilFrame{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStagePrepareAttemptCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStagePrepareSuccessCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageExecuteAttemptCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageExecuteSuccessCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageCandidateCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageCandidatePrepareCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageCandidateRefreshCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageCandidateExecuteCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageSkippedRuntimeNotReadyCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateStage{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateA3{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateA4{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateA5{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateJassReady{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateGameStarted{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateRuntimeFrame{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastPrepareStage{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastExecuteStage{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastPrepareFrameSerial{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastExecuteFrameSerial{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastPrepareDrawCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastExecuteDrawCount{0u};
std::mutex g_shadowSceneStatsMutex;
War3ShadowCaptureStats g_shadowSceneStats = {};
uint64_t g_shadowSceneStatsPublishCount = 0u;
constexpr size_t kSemanticPerfTagCount =
    static_cast<size_t>(SemanticDataPerfTag::Count);
std::array<std::atomic<uint64_t>, kSemanticPerfTagCount>
    g_semanticPerfCalls = {};
std::array<std::atomic<uint64_t>, kSemanticPerfTagCount> g_semanticPerfUs = {};
std::atomic<uint64_t> g_semanticConsumerBuildSkippedFresh{0u};
std::atomic<uint64_t> g_semanticSummaryRefreshFrameSerial{0u};
std::atomic<uint64_t> g_semanticSummaryRefreshPublishRevision{0u};
std::atomic<uint64_t> g_semanticLastHotFunctionTag{0u};
std::atomic<uint64_t> g_semanticLastHotFunctionUs{0u};

constexpr uint64_t kShadowBridgeRepairBurstFrames = 24u;
constexpr uint64_t kShadowBridgeRepairCooldownFrames = 120u;

uint64_t CurrentRuntimeBridgeFrame() {
  if (model::IsPoseHookEnabled())
    return model::PoseRegistry::instance().frameNumber();
  return model::ModelInstanceRegistry::instance().frameNumber();
}

uint64_t CountSkinnedSubmissionDraws(
    const shadow::ShadowSubmissionFrame* frame) {
  if (frame == nullptr)
    return 0u;

  uint64_t count = 0u;
  for (const auto& draw : frame->draws) {
    if (draw.path == shadow::ShadowDrawPath::Skinned)
      ++count;
  }
  return count;
}

void RequestShadowBridgeRepairBurst(uint64_t nowFrame) {
  if (nowFrame == 0u)
    return;

  const uint64_t activeUntil =
      g_shadowBridgeRepairUntilFrame.load(std::memory_order_relaxed);
  if (nowFrame <= activeUntil)
    return;

  const uint64_t cooldownUntil =
      g_shadowBridgeRepairCooldownUntilFrame.load(std::memory_order_relaxed);
  if (nowFrame < cooldownUntil)
    return;

  g_shadowBridgeRepairCooldownUntilFrame.store(
      nowFrame + kShadowBridgeRepairCooldownFrames,
      std::memory_order_relaxed);
  g_shadowBridgeRepairUntilFrame.store(nowFrame + kShadowBridgeRepairBurstFrames,
                                       std::memory_order_relaxed);
}

bool IsRegistryFrameFresh(uint64_t sampleFrame, uint64_t registryFrame) {
  return sampleFrame != 0 && sampleFrame + 1 >= registryFrame;
}

bool HasRuntimeOwnerIdentity(const model::ModelInstanceRegistry& instanceRegistry,
                             void* runtimeModelPtr) {
  if (runtimeModelPtr == nullptr)
    return false;

  model::ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findOwnerByRuntimeModel(runtimeModelPtr, instanceRecord)) {
    return instanceRecord.worldObjectEntry != nullptr ||
           instanceRecord.sceneNode != nullptr || instanceRecord.unitPtr != nullptr ||
           instanceRecord.jHandle != 0u || instanceRecord.rawcode != 0u;
  }
  if (instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord)) {
    return instanceRecord.worldObjectEntry != nullptr ||
           instanceRecord.sceneNode != nullptr || instanceRecord.unitPtr != nullptr ||
           instanceRecord.jHandle != 0u || instanceRecord.rawcode != 0u;
  }
  return false;
}

bool TryGetRuntimeCreateCallerRva(
    const model::ModelInstanceRegistry& instanceRegistry, void* runtimeModelPtr,
    uint32_t& outCallerRva) {
  outCallerRva = 0u;
  if (runtimeModelPtr == nullptr)
    return false;

  model::ModelInstanceRecord instanceRecord = {};
  if (!instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord))
    return false;

  outCallerRva = instanceRecord.runtimeCreatorCallerRva;
  return outCallerRva != 0u ||
         instanceRecord.runtimeCreatorModelDataPtr != nullptr;
}

bool TryGetRuntimeResolveProvenance(
    const model::ModelInstanceRegistry& instanceRegistry, void* runtimeModelPtr,
    void*& outCreateHandlePtr, uint32_t& outResolveCallerRva) {
  outCreateHandlePtr = nullptr;
  outResolveCallerRva = 0u;
  if (runtimeModelPtr == nullptr)
    return false;

  model::ModelInstanceRecord instanceRecord = {};
  if (!instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord))
    return false;

  outCreateHandlePtr = instanceRecord.runtimeCreatorHandlePtr;
  outResolveCallerRva = instanceRecord.runtimeResolveCallerRva;
  return outCreateHandlePtr != nullptr || outResolveCallerRva != 0u;
}

bool TryGetRuntimeRecordSnapshot(
    const model::ModelInstanceRegistry& instanceRegistry, void* runtimeModelPtr,
    model::ModelInstanceRecord& outRecord) {
  outRecord = {};
  if (runtimeModelPtr == nullptr)
    return false;
  return instanceRegistry.findByRuntimeModel(runtimeModelPtr, outRecord);
}

bool TryGetRuntimeModelResourceSnapshot(void* runtimeModelPtr,
                                        model::ModelResourceRecord& outRecord) {
  outRecord = {};
  if (runtimeModelPtr == nullptr)
    return false;
  if (model::ModelRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                          outRecord)) {
    return true;
  }

  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0u;
  model::ShadowModelResourceRecord resourceRecord = {};
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (resourceCache.findRuntimeModelResource(runtimeModelPtr, resourceRecord)) {
    modelResourcePtr = resourceRecord.modelResourcePtr;
    modelKey = resourceRecord.modelKey;
  } else {
    void* ownedModelDataHandle = nullptr;
    if (dxvk::war3::SafeReadPtrFast(
            runtimeModelPtr, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
            ownedModelDataHandle) &&
        ownedModelDataHandle != nullptr) {
      modelResourcePtr =
          resourceCache.resolveDirectModelResourcePtr(ownedModelDataHandle);
      if (modelResourcePtr != nullptr &&
          resourceCache.findModelResource(modelResourcePtr, resourceRecord)) {
        modelKey = resourceRecord.modelKey;
      }
    }
  }

  if (modelResourcePtr == nullptr && modelKey == 0u)
    return false;

  outRecord.runtimeModelPtr = runtimeModelPtr;
  outRecord.modelResourcePtr = modelResourcePtr;
  outRecord.modelKey = modelKey;
  return true;
}

bool TryGetRuntimePoseSnapshot(void* runtimeModelPtr,
                               model::PoseRecord& outRecord) {
  outRecord = {};
  if (runtimeModelPtr == nullptr)
    return false;
  return model::PoseRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                            outRecord);
}

void MaybeApplyPoseSnapshot(dxvk::War3ShadowSemanticContext& semantic,
                            bool hasTransform, const Matrix4& transform,
                            float scale, float height, bool fromSpriteFrame,
                            uint64_t sampleFrame, uint64_t& bestFrame) {
  if (!hasTransform || sampleFrame == 0 || sampleFrame < bestFrame)
    return;
  if (sampleFrame == bestFrame && !fromSpriteFrame &&
      semantic.hasPoseTransform && semantic.poseFromSpriteFrame) {
    // 对飞行单位/挂点模型，sprite-frame pose 通常比 root pose 更接近
    // 本帧最终渲染姿态。相同 frame 的情况下不允许 root pose 把它覆盖掉。
    return;
  }

  semantic.hasPoseTransform = true;
  semantic.poseFromSpriteFrame = fromSpriteFrame;
  semantic.poseTransform = transform;
  semantic.poseScale = scale;
  semantic.poseHeight = height;
  bestFrame = sampleFrame;
}

void MaybeApplyPoseMatrices(dxvk::War3ShadowSemanticContext& semantic,
                            uint32_t matrixCount, uint64_t matrixHash,
                            uint64_t sampleFrame, uint64_t& bestFrame) {
  if (matrixCount == 0 || sampleFrame == 0 || sampleFrame < bestFrame)
    return;

  semantic.poseMatrixCount = matrixCount;
  semantic.poseMatrixHash = matrixHash;
  bestFrame = sampleFrame;
}

void MergeRenderObject(dxvk::War3ShadowSemanticContext& semantic,
                       const RenderObjectInfo* object) {
  if (object == nullptr)
    return;

  if (semantic.object == nullptr)
    semantic.object = object;
  if (semantic.sceneNode == nullptr)
    semantic.sceneNode = object->sceneNode;
  if (semantic.worldObjectEntry == nullptr)
    semantic.worldObjectEntry = object->worldObjectEntry;
  if (semantic.jHandle == 0u)
    semantic.jHandle = object->jHandle;
  if (semantic.rawcode == 0u)
    semantic.rawcode = object->rawcode;
  if (static_cast<uint32_t>(semantic.objectKind) == 0u)
    semantic.objectKind = object->kind;
}

} // namespace

void NoteShadowRuntimeRenderObject(const RenderObjectInfo& info) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().noteRenderObject(info);
  ShadowObjectRegistry::instance().noteRenderObject(info);
}

void NoteShadowRuntimeRenderObjectsBatch(
    const std::vector<const RenderObjectInfo*>& infos) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled || infos.empty())
    return;
  model::ModelInstanceRegistry::instance().noteRenderObjectsBatch(infos);
  ShadowObjectRegistry::instance().noteRenderObjectsBatch(infos);
}

void NoteShadowRuntimeIdentity(void* worldObjectEntry, void* sceneNode,
                               void* unitPtr, void* spritePtr,
                               uint32_t jHandle, uint32_t rawcode,
                               ObjectKind kind) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().noteInstanceIdentity(
      worldObjectEntry, sceneNode, unitPtr, spritePtr, jHandle, rawcode);
  ShadowObjectRegistry::instance().noteInstanceIdentity(
      worldObjectEntry, sceneNode, unitPtr, spritePtr, jHandle, rawcode, kind);
}

void NoteShadowRuntimeModelBinding(void* spritePtr, void* runtimeModelPtr,
                                   void* modelResourcePtr,
                                   const std::string& modelPath,
                                   uint32_t modelType, uint32_t modelFlags,
                                   uint64_t modelKey) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().bindRuntimeModelToSprite(
      spritePtr, runtimeModelPtr, modelKey, modelResourcePtr);
  ShadowObjectRegistry::instance().noteModelBinding(
      spritePtr, runtimeModelPtr, modelResourcePtr, modelPath, modelType,
      modelFlags, modelKey);
}

void NoteShadowRuntimePose(void* runtimeModelPtr, void* sceneNode, void* unitPtr,
                           uint32_t sequenceId, float sequenceTime, float scale,
                           float yaw, float pitch, float roll, float height,
                           bool hasWorldTransform,
                           const Matrix4* worldTransform, uint32_t matrixCount,
                           uint64_t matrixHash) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::PoseRegistry::instance().recordPose(
      runtimeModelPtr, sceneNode, unitPtr, sequenceId, sequenceTime, scale, yaw,
      pitch, roll, height, hasWorldTransform, worldTransform);
  ShadowObjectRegistry::instance().notePose(
      runtimeModelPtr, sceneNode, unitPtr, sequenceId, sequenceTime, scale, yaw,
      pitch, roll, height, hasWorldTransform, worldTransform, matrixCount,
      matrixHash);
}

void NoteShadowSceneStats(const War3ShadowCaptureStats& stats) {
  std::lock_guard<std::mutex> lock(g_shadowSceneStatsMutex);
  g_shadowSceneStats = stats;
  ++g_shadowSceneStatsPublishCount;
}

void NoteSemanticDataPerf(SemanticDataPerfTag tag, uint64_t durationUs) {
  const auto index = static_cast<size_t>(tag);
  if (index == 0u || index >= kSemanticPerfTagCount)
    return;
  g_semanticPerfCalls[index].fetch_add(1u, std::memory_order_relaxed);
  g_semanticPerfUs[index].fetch_add(durationUs, std::memory_order_relaxed);

  uint64_t lastHotUs =
      g_semanticLastHotFunctionUs.load(std::memory_order_relaxed);
  while (durationUs > lastHotUs &&
         !g_semanticLastHotFunctionUs.compare_exchange_weak(
             lastHotUs, durationUs, std::memory_order_relaxed)) {
  }
  if (durationUs >= lastHotUs)
    g_semanticLastHotFunctionTag.store(static_cast<uint64_t>(tag),
                                       std::memory_order_relaxed);
}

void NoteNativeSemanticWorldStageCandidate(int stage, int a3, int a4, int a5,
                                           bool jassReady, bool gameStarted) {
  g_nativeSemanticWorldStageCandidateCount.fetch_add(
      1u, std::memory_order_relaxed);
  if (stage == dxvk::war3::internal::kNativeSemanticShadowPrepareStage) {
    g_nativeSemanticWorldStageCandidatePrepareCount.fetch_add(
        1u, std::memory_order_relaxed);
  } else if (
      stage ==
      dxvk::war3::internal::kNativeSemanticShadowRefreshPrepareStage) {
    g_nativeSemanticWorldStageCandidateRefreshCount.fetch_add(
        1u, std::memory_order_relaxed);
  } else if (stage ==
             dxvk::war3::internal::kNativeSemanticShadowExecuteStage) {
    g_nativeSemanticWorldStageCandidateExecuteCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  g_nativeSemanticWorldStageLastCandidateStage.store(
      static_cast<uint64_t>(stage), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA3.store(
      static_cast<uint32_t>(a3), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA4.store(
      static_cast<uint32_t>(a4), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA5.store(
      static_cast<uint32_t>(a5), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateJassReady.store(
      jassReady ? 1u : 0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateGameStarted.store(
      gameStarted ? 1u : 0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateRuntimeFrame.store(
      CurrentRuntimeBridgeFrame(), std::memory_order_relaxed);
}

void NoteNativeSemanticWorldStageSkippedRuntimeNotReady(int stage) {
  (void)stage;
  g_nativeSemanticWorldStageSkippedRuntimeNotReadyCount.fetch_add(
      1u, std::memory_order_relaxed);
}

void NoteNativeSemanticWorldStagePrepare(int stage, bool success) {
  g_nativeSemanticWorldStagePrepareAttemptCount.fetch_add(
      1u, std::memory_order_relaxed);
  if (success) {
    g_nativeSemanticWorldStagePrepareSuccessCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  const auto nativeSummary =
      shadow::NativeD3D9BackendRuntime::instance().snapshot();
  g_nativeSemanticWorldStageLastPrepareStage.store(
      static_cast<uint64_t>(stage), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareFrameSerial.store(
      nativeSummary.frameSerial, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareDrawCount.store(
      nativeSummary.submittedDrawCount, std::memory_order_relaxed);
}

void NoteNativeSemanticWorldStageExecute(int stage, bool success) {
  g_nativeSemanticWorldStageExecuteAttemptCount.fetch_add(
      1u, std::memory_order_relaxed);
  if (success) {
    g_nativeSemanticWorldStageExecuteSuccessCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  const auto nativeSummary =
      shadow::NativeD3D9BackendRuntime::instance().snapshot();
  const uint64_t executedDrawCount =
      nativeSummary.lastSuccessfulExecutedDrawCount != 0u
          ? nativeSummary.lastSuccessfulExecutedDrawCount
          : nativeSummary.executedDrawCount;
  const uint64_t executedFrameSerial =
      nativeSummary.lastSuccessfulExecutedFrameSerial != 0u
          ? nativeSummary.lastSuccessfulExecutedFrameSerial
          : nativeSummary.executedFrameSerial;
  g_nativeSemanticWorldStageLastExecuteStage.store(
      static_cast<uint64_t>(stage), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteFrameSerial.store(
      executedFrameSerial, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteDrawCount.store(
      executedDrawCount, std::memory_order_relaxed);
}

void NoteShadowRuntimeSpriteFramePose(void* runtimeModelPtr, void* spritePtr,
                                      void* sceneNode, void* unitPtr, float dt,
                                      uint32_t sequenceId, float sequenceTime,
                                      float scale, float yaw, float pitch,
                                      float roll, float height,
                                      bool hasWorldTransform,
                                      const Matrix4* worldTransform,
                                      uint32_t matrixCount,
                                      uint64_t matrixHash) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::PoseRegistry::instance().recordSpriteFramePose(
      runtimeModelPtr, spritePtr, sceneNode, unitPtr, dt, sequenceId,
      sequenceTime, scale, yaw, pitch, roll, height, hasWorldTransform,
      worldTransform);
  ShadowObjectRegistry::instance().noteSpriteFramePose(
      runtimeModelPtr, spritePtr, sceneNode, unitPtr, dt, sequenceId,
      sequenceTime, scale, yaw, pitch, roll, height, hasWorldTransform,
      worldTransform, matrixCount, matrixHash);
}

ShadowRuntimeBridgeSummary QueryShadowRuntimeBridgeSummary(
    bool refreshSemanticFrameIfStale) {
  ShadowRuntimeBridgeSummary summary = {};
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return summary;
  const auto semanticSkipReason = CurrentSemanticBuildSkippedReason();
  const auto contractStats =
      shadow::ShadowRuntimeContractCache::instance().snapshotStats();
  summary.semanticDataModuleEnabled = IsSemanticDataModuleEnabled() ? 1u : 0u;
  summary.semanticModelProducerEnabled =
      IsSemanticModelProducerEnabled() ? 1u : 0u;
  summary.semanticPoseProducerEnabled =
      (IsSemanticModelProducerEnabled() &&
       internal::kWar3RuntimeConfigSemanticPoseProducerEffective)
          ? 1u
          : 0u;
  summary.semanticAttachmentProducerEnabled =
      (IsSemanticModelProducerEnabled() &&
       internal::kWar3RuntimeConfigSemanticAttachmentProducerEffective)
          ? 1u
          : 0u;
  summary.semanticFrameRegistriesEnabled =
      IsSemanticFrameRegistriesEnabled() ? 1u : 0u;
  summary.semanticContractCaptureEnabled =
      IsSemanticContractCaptureEnabled() ? 1u : 0u;
  summary.semanticConsumerEnabled = IsSemanticConsumerEnabled() ? 1u : 0u;
  summary.semanticBuildSkippedReason =
      static_cast<uint64_t>(semanticSkipReason);
  auto& validationRuntime = shadow::ShadowValidationRuntime::instance();
  if (refreshSemanticFrameIfStale &&
      semanticSkipReason == SemanticBuildSkippedReason::None) {
    bool alreadyFresh = false;
    const auto preCore = validationRuntime.snapshot();
    const auto preBuildState = validationRuntime.buildStateSnapshot();
    const auto preBundle =
        shadow::ShadowRuntimeContractCache::instance().snapshotBundleShared();
    const uint64_t targetFrameSerial =
        preBundle.valid() && preBundle.manifest != nullptr
            ? preBundle.manifest->frameSerial
            : 0u;
    const uint64_t targetPublishRevision =
        preBundle.valid() && preBundle.manifest != nullptr
            ? preBundle.manifest->publishRevision
            : 0u;
    if (preBundle.valid() && preBundle.manifest != nullptr &&
        preCore.frameSerial != 0u &&
        preCore.sourcePublishRevision != 0u &&
        !preBuildState.buildInProgress &&
        !preBuildState.buildRequestPending) {
      const uint64_t frameLag =
          preBundle.manifest->frameSerial >= preCore.frameSerial
              ? preBundle.manifest->frameSerial - preCore.frameSerial
              : 0u;
      const uint64_t publishRevisionLag =
          preBundle.manifest->publishRevision >= preCore.sourcePublishRevision
              ? preBundle.manifest->publishRevision -
                    preCore.sourcePublishRevision
              : 0u;
      const bool requiresExactPublishRevision =
          preBundle.stats.rootUnitSupplementAppended != 0u ||
          preBundle.stats.rootUnitSupplementReusedFromPrior != 0u;
      alreadyFresh =
          frameLag <= 1u &&
          (requiresExactPublishRevision ? publishRevisionLag == 0u
                                        : publishRevisionLag <= 16u);
    }

    if (alreadyFresh) {
      g_semanticConsumerBuildSkippedFresh.fetch_add(
          1u, std::memory_order_relaxed);
    } else {
      const bool buildAlreadyActive =
          preBuildState.buildInProgress || preBuildState.buildRequestPending;
      const bool sameSummaryRefresh =
          targetFrameSerial != 0u && targetPublishRevision != 0u &&
          g_semanticSummaryRefreshFrameSerial.load(std::memory_order_relaxed) ==
              targetFrameSerial &&
          g_semanticSummaryRefreshPublishRevision.load(
              std::memory_order_relaxed) == targetPublishRevision;
      if (buildAlreadyActive || sameSummaryRefresh) {
        g_semanticConsumerBuildSkippedFresh.fetch_add(
            1u, std::memory_order_relaxed);
      } else {
        g_semanticSummaryRefreshFrameSerial.store(
            targetFrameSerial, std::memory_order_relaxed);
        g_semanticSummaryRefreshPublishRevision.store(
            targetPublishRevision, std::memory_order_relaxed);
      const auto semanticRefreshStart = std::chrono::steady_clock::now();
      // scene-submission 模式下只发“异步追最新 contract”的请求。
      // control-plane 不能同步替 render thread 消费 semantic build；否则
      // pipe 请求会把 buildFrameChunk 压到控制线程上，低压图 tail 状态下
      // 很容易复现 3s 响应超时。真正的消费必须发生在 scene submit /
      // EndFrame 的 render-thread 小步推进里。
      validationRuntime.requestLatestFrameBuild();

      const auto semanticRefreshElapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - semanticRefreshStart)
              .count();
      NoteSemanticDataPerf(
          SemanticDataPerfTag::ConsumerBuild,
          semanticRefreshElapsed > 0
              ? static_cast<uint64_t>(semanticRefreshElapsed)
              : 0u);
      }
    }
  }
  summary.runtimePoseHooksActive = model::IsPoseHookEnabled();
  summary.modelRegistryCount = model::ModelRegistry::instance().recordCount();
  summary.instanceRegistryCount =
      model::ModelInstanceRegistry::instance().recordCount();
  summary.runtimeBoundCount =
      model::ModelInstanceRegistry::instance().runtimeBoundCount();
  summary.runtimeCreationProvenanceCount =
      model::ModelInstanceRegistry::instance().runtimeCreationProvenanceCount();
  summary.runtimeResolveProvenanceCount =
      model::ModelInstanceRegistry::instance().runtimeResolveProvenanceCount();
  summary.runtimeOwnerIdentityCount =
      model::ModelInstanceRegistry::instance().runtimeOwnerIdentityCount();
  summary.completeIdentityCount =
      model::ModelInstanceRegistry::instance().completeIdentityCount();
  summary.poseReadyCount = model::PoseRegistry::instance().readyPoseCount();
  summary.spriteFramePoseCount =
      model::PoseRegistry::instance().spriteFramePoseCount();
  summary.matrixPaletteCount = (std::max)(
      uint64_t(model::PoseRegistry::instance().matrixPaletteCount()),
      contractStats.matrixPaletteCount);
  summary.shadowGeosetResourceCount =
      model::ShadowModelResourceCache::instance().geosetRecordCount();
  summary.shadowReadyGeosetCount =
      model::ShadowModelResourceCache::instance().readyGeosetCount();
  summary.shadowModelResourceCount =
      model::ShadowModelResourceCache::instance().modelResourceCount();
  summary.shadowRuntimeModelCount =
      model::ShadowModelResourceCache::instance().runtimeModelRecordCount();
  summary.visibleRenderableCount =
      VisibleRenderableRegistry::instance().getVisibleCount();
  summary.visibleRenderableMainCount =
      VisibleRenderableRegistry::instance().getMainQueueCount();
  summary.visibleRenderableTransparentCount =
      VisibleRenderableRegistry::instance().getTransparentCount();
  {
    const auto& visibleRecords =
        VisibleRenderableRegistry::instance().getAllVisibleView();
    auto& instanceRegistry = model::ModelInstanceRegistry::instance();
    auto& shadowRegistry = ShadowObjectRegistry::instance();
    auto& poseRegistry = model::PoseRegistry::instance();
    auto& resourceCache = model::ShadowModelResourceCache::instance();
    for (const auto& record : visibleRecords) {
      const bool isBuilding =
          record.identity.kind == ObjectKind::Building;
      const bool isDestructible =
          record.identity.kind == ObjectKind::Destructible;
      const bool maybeDoodadOrEffect =
          record.identity.kind != ObjectKind::Unit &&
          record.identity.kind != ObjectKind::Building &&
          record.identity.kind != ObjectKind::Destructible &&
          record.identity.unitPtr == nullptr &&
          (record.identity.groupIdx >= 2 || record.meshData != nullptr ||
           record.sceneNode != nullptr || record.identity.sceneNode != nullptr);
      const bool isStaticCandidate =
          isBuilding || isDestructible || maybeDoodadOrEffect;
      if (!isStaticCandidate)
        continue;

      summary.semanticStaticCandidateCount++;
      if (isBuilding)
        summary.semanticStaticCandidateBuildingCount++;
      if (isDestructible)
        summary.semanticStaticCandidateDestructibleCount++;
      if (maybeDoodadOrEffect)
        summary.semanticStaticCandidateMaybeDoodadOrEffectCount++;

      const bool hasStableIdentity = record.HasStableIdentity();
      const bool hasMeshData = record.meshData != nullptr;
      const bool hasRuntimeModel = record.runtimeModelPtr != nullptr;
      const bool hasModelResource =
          record.modelResourcePtr != nullptr || record.modelKey != 0u;
      const bool hasResolvedGeoset = record.HasResolvedGeoset();

      if (hasStableIdentity)
        summary.semanticStaticCandidateWithStableIdentity++;
      if (hasMeshData)
        summary.semanticStaticCandidateWithMeshData++;
      if (hasRuntimeModel)
        summary.semanticStaticCandidateWithRuntimeModel++;
      if (hasModelResource)
        summary.semanticStaticCandidateWithModelResource++;
      if (hasResolvedGeoset)
        summary.semanticStaticCandidateWithResolvedGeoset++;

      if (internal::kShadowSemanticCoreSceneUnitsOnly)
        summary.semanticStaticCandidateRejectedUnitsOnlyFilter++;
      if (!hasStableIdentity)
        summary.semanticStaticCandidateRejectedNoIdentity++;
      if (!hasMeshData)
        summary.semanticStaticCandidateRejectedNoMeshData++;
      if (!hasModelResource)
        summary.semanticStaticCandidateRejectedNoResource++;
      if (!hasResolvedGeoset)
        summary.semanticStaticCandidateRejectedNoGeoset++;
      if (maybeDoodadOrEffect && !isBuilding && !isDestructible)
        summary.semanticStaticCandidateRejectedNonCanonicalKind++;
    }
    auto writeVisibleSample = [&](const VisibleRenderableRecord& record,
                                  bool sample0) {
      const uint64_t worldObjectEntry =
          reinterpret_cast<uint64_t>(record.identity.worldObjectEntry);
      const uint64_t sceneNode = reinterpret_cast<uint64_t>(
          record.identity.sceneNode != nullptr ? record.identity.sceneNode
                                               : record.sceneNode);
      const uint64_t unitPtr =
          reinterpret_cast<uint64_t>(record.identity.unitPtr);
      const uint64_t runtimeModelPtr =
          reinterpret_cast<uint64_t>(record.runtimeModelPtr);
      const uint64_t modelResourcePtr =
          reinterpret_cast<uint64_t>(record.modelResourcePtr);
      const uint64_t runtimeGeosetPtr =
          reinterpret_cast<uint64_t>(record.runtimeGeosetPtr);
      const uint64_t runtimeGeosetDataPtr =
          reinterpret_cast<uint64_t>(record.runtimeGeosetDataPtr);
      model::ModelInstanceRecord instanceRecord = {};
      const bool instanceHit =
          sceneNode != 0u &&
          instanceRegistry.findBySceneNode(reinterpret_cast<void*>(sceneNode),
                                           instanceRecord);
      ShadowObjectRecord shadowRecord = {};
      const bool shadowHit =
          sceneNode != 0u &&
          shadowRegistry.findBySceneNode(reinterpret_cast<void*>(sceneNode),
                                         shadowRecord);
      model::PoseRecord poseRecord = {};
      const bool poseHit =
          sceneNode != 0u &&
          poseRegistry.findBySceneNode(reinterpret_cast<void*>(sceneNode),
                                       poseRecord);
      model::ShadowGeosetResourceRecord geosetRecord = {};
      bool geosetHit = false;
      if (record.runtimeGeosetDataPtr != nullptr) {
        geosetHit =
            resourceCache.findGeosetByData(record.runtimeGeosetDataPtr, geosetRecord);
      }
      if (!geosetHit && record.runtimeGeosetPtr != nullptr) {
        geosetHit =
            resourceCache.findGeosetByPtr(record.runtimeGeosetPtr, geosetRecord);
      }
      const uint64_t sceneInstanceRuntimeModelPtr = reinterpret_cast<uint64_t>(
          instanceHit ? instanceRecord.runtimeModelPtr : nullptr);
      const uint64_t sceneInstanceModelResourcePtr = reinterpret_cast<uint64_t>(
          instanceHit ? instanceRecord.modelResourcePtr : nullptr);
      const uint64_t sceneShadowRuntimeModelPtr = reinterpret_cast<uint64_t>(
          shadowHit ? shadowRecord.runtimeModelPtr : nullptr);
      const uint64_t sceneShadowModelResourcePtr = reinterpret_cast<uint64_t>(
          shadowHit ? shadowRecord.modelResourcePtr : nullptr);
      const uint64_t scenePoseMatrixCount =
          poseHit ? uint64_t(poseRecord.matrixCount) : 0u;
      const uint64_t geosetModelResourcePtr = reinterpret_cast<uint64_t>(
          geosetHit ? geosetRecord.modelResourcePtr : nullptr);
      const uint64_t geosetModelKey =
          geosetHit ? geosetRecord.modelKey : 0u;
      if (sample0) {
        summary.visibleRenderableSample0WorldObjectEntryPtr = worldObjectEntry;
        summary.visibleRenderableSample0SceneNodePtr = sceneNode;
        summary.visibleRenderableSample0UnitPtr = unitPtr;
        summary.visibleRenderableSample0JHandle = record.identity.jHandle;
        summary.visibleRenderableSample0Rawcode = record.identity.rawcode;
        summary.visibleRenderableSample0RuntimeModelPtr = runtimeModelPtr;
        summary.visibleRenderableSample0ModelResourcePtr = modelResourcePtr;
        summary.visibleRenderableSample0RuntimeGeosetPtr = runtimeGeosetPtr;
        summary.visibleRenderableSample0RuntimeGeosetDataPtr =
            runtimeGeosetDataPtr;
        summary.visibleRenderableSample0SceneInstanceRuntimeModelPtr =
            sceneInstanceRuntimeModelPtr;
        summary.visibleRenderableSample0SceneInstanceModelResourcePtr =
            sceneInstanceModelResourcePtr;
        summary.visibleRenderableSample0SceneShadowRuntimeModelPtr =
            sceneShadowRuntimeModelPtr;
        summary.visibleRenderableSample0SceneShadowModelResourcePtr =
            sceneShadowModelResourcePtr;
        summary.visibleRenderableSample0ScenePoseMatrixCount =
            scenePoseMatrixCount;
        summary.visibleRenderableSample0GeosetModelResourcePtr =
            geosetModelResourcePtr;
        summary.visibleRenderableSample0GeosetModelKey = geosetModelKey;
      } else {
        summary.visibleRenderableSample1WorldObjectEntryPtr = worldObjectEntry;
        summary.visibleRenderableSample1SceneNodePtr = sceneNode;
        summary.visibleRenderableSample1UnitPtr = unitPtr;
        summary.visibleRenderableSample1JHandle = record.identity.jHandle;
        summary.visibleRenderableSample1Rawcode = record.identity.rawcode;
        summary.visibleRenderableSample1RuntimeModelPtr = runtimeModelPtr;
        summary.visibleRenderableSample1ModelResourcePtr = modelResourcePtr;
        summary.visibleRenderableSample1RuntimeGeosetPtr = runtimeGeosetPtr;
        summary.visibleRenderableSample1RuntimeGeosetDataPtr =
            runtimeGeosetDataPtr;
        summary.visibleRenderableSample1SceneInstanceRuntimeModelPtr =
            sceneInstanceRuntimeModelPtr;
        summary.visibleRenderableSample1SceneInstanceModelResourcePtr =
            sceneInstanceModelResourcePtr;
        summary.visibleRenderableSample1SceneShadowRuntimeModelPtr =
            sceneShadowRuntimeModelPtr;
        summary.visibleRenderableSample1SceneShadowModelResourcePtr =
            sceneShadowModelResourcePtr;
        summary.visibleRenderableSample1ScenePoseMatrixCount =
            scenePoseMatrixCount;
        summary.visibleRenderableSample1GeosetModelResourcePtr =
            geosetModelResourcePtr;
        summary.visibleRenderableSample1GeosetModelKey = geosetModelKey;
      }
    };
    if (!visibleRecords.empty())
      writeVisibleSample(visibleRecords[0], true);
    if (visibleRecords.size() > 1u)
      writeVisibleSample(visibleRecords[1], false);
  }
  const auto sceneCollectorSummary = QuerySceneCollectorIdentityProbeSummary();
  summary.worldObjectListEntryCount =
      sceneCollectorSummary.worldObjectListEntryCount;
  summary.worldObjectListNullEntryCount =
      sceneCollectorSummary.worldObjectListNullEntryCount;
  summary.worldObjectListOwnerHintZeroCount =
      sceneCollectorSummary.worldObjectListOwnerHintZeroCount;
  summary.worldObjectListOwnerHintNonzeroCount =
      sceneCollectorSummary.worldObjectListOwnerHintNonzeroCount;
  summary.worldObjectListOwnerHintHandleCount =
      sceneCollectorSummary.worldObjectListOwnerHintHandleCount;
  summary.worldObjectListOwnerHintUnitPtrCount =
      sceneCollectorSummary.worldObjectListOwnerHintUnitPtrCount;
  summary.worldObjectListOwnerHintZeroContextAcceptedCount =
      sceneCollectorSummary.worldObjectListOwnerHintZeroContextAcceptedCount;
  summary.worldObjectListAcceptedIdentityCount =
      sceneCollectorSummary.worldObjectListAcceptedIdentityCount;
  summary.lastWorldObjectListEntryWorldObjectEntryPtr =
      sceneCollectorSummary.lastWorldObjectListEntryWorldObjectEntryPtr;
  summary.lastWorldObjectListEntryOwnerHintValue =
      sceneCollectorSummary.lastWorldObjectListEntryOwnerHintValue;
  summary.lastWorldObjectListEntrySceneNodePtr =
      sceneCollectorSummary.lastWorldObjectListEntrySceneNodePtr;
  const auto renderIdentitySummary =
      hooks::QueryRenderIdentityLifecycleProbeSummary();
  summary.worldObjectEntryRenderCallCount =
      renderIdentitySummary.worldObjectEntryRenderCallCount;
  summary.worldObjectEntryRenderSceneNodeReadyBeforeCount =
      renderIdentitySummary.worldObjectEntryRenderSceneNodeReadyBeforeCount;
  summary.worldObjectEntryRenderSceneNodeReadyAfterCount =
      renderIdentitySummary.worldObjectEntryRenderSceneNodeReadyAfterCount;
  summary.worldObjectEntryRenderSceneNodeFilledByCallCount =
      renderIdentitySummary.worldObjectEntryRenderSceneNodeFilledByCallCount;
  summary.worldObjectEntryRenderSceneNodeChangedCount =
      renderIdentitySummary.worldObjectEntryRenderSceneNodeChangedCount;
  summary.worldObjectEntryRenderKnownListOwnerHintZeroCount =
      renderIdentitySummary.worldObjectEntryRenderKnownListOwnerHintZeroCount;
  summary.worldObjectEntryRenderKnownListOwnerHintNonzeroCount =
      renderIdentitySummary.worldObjectEntryRenderKnownListOwnerHintNonzeroCount;
  summary.worldObjectEntryRenderUnknownListOwnerHintCount =
      renderIdentitySummary.worldObjectEntryRenderUnknownListOwnerHintCount;
  summary.worldObjectListEntryWriteCallCount =
      renderIdentitySummary.worldObjectListEntryWriteCallCount;
  summary.worldObjectListEntryWriteOwnerHintZeroCount =
      renderIdentitySummary.worldObjectListEntryWriteOwnerHintZeroCount;
  summary.worldObjectListEntryWriteOwnerHintNonzeroCount =
      renderIdentitySummary.worldObjectListEntryWriteOwnerHintNonzeroCount;
  summary.worldObjectListEntryWriteOwnerHintHandleCount =
      renderIdentitySummary.worldObjectListEntryWriteOwnerHintHandleCount;
  summary.worldObjectListEntryWriteOwnerHintUnitPtrCount =
      renderIdentitySummary.worldObjectListEntryWriteOwnerHintUnitPtrCount;
  summary.lastWorldObjectEntryRenderEntryPtr =
      renderIdentitySummary.lastWorldObjectEntryRenderEntryPtr;
  summary.lastWorldObjectEntryRenderResolvedListOwnerHintValue =
      renderIdentitySummary.lastWorldObjectEntryRenderResolvedListOwnerHintValue;
  summary.lastWorldObjectListEntryWriteListPtr =
      renderIdentitySummary.lastWorldObjectListEntryWriteListPtr;
  summary.lastWorldObjectListEntryWriteWorldObjectEntryPtr =
      renderIdentitySummary.lastWorldObjectListEntryWriteWorldObjectEntryPtr;
  summary.lastWorldObjectListEntryWriteOwnerHintValue =
      renderIdentitySummary.lastWorldObjectListEntryWriteOwnerHintValue;
  summary.lastWorldObjectEntryRenderSceneNodeBeforePtr =
      renderIdentitySummary.lastWorldObjectEntryRenderSceneNodeBeforePtr;
  summary.lastWorldObjectEntryRenderSceneNodeAfterPtr =
      renderIdentitySummary.lastWorldObjectEntryRenderSceneNodeAfterPtr;
  summary.shadowRuntimeBoundCount =
      ShadowObjectRegistry::instance().runtimeBoundCount();
  summary.shadowIdentityCount =
      ShadowObjectRegistry::instance().completeIdentityCount();
  summary.shadowPoseReadyCount =
      ShadowObjectRegistry::instance().poseReadyCount();
  const auto overrideSummary = model::QueryRuntimeOverrideOutputProbeSummary();
  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  summary.runtimeModelCtorCount = overrideSummary.runtimeModelCtorCount;
  summary.runtimeModelComplexCtorCount =
      overrideSummary.runtimeModelComplexCtorCount;
  summary.runtimeModelPlainCtorCount =
      overrideSummary.runtimeModelPlainCtorCount;
  summary.runtimeModelCtorCallerPromoteCount =
      overrideSummary.runtimeModelCtorCallerPromoteCount;
  summary.runtimeModelCtorCallerOtherCount =
      overrideSummary.runtimeModelCtorCallerOtherCount;
  summary.runtimeModelCreateCount = overrideSummary.runtimeModelCreateCount;
  summary.runtimeModelResolveCount = overrideSummary.runtimeModelResolveCount;
  summary.runtimeModelResolveResolvedIdentityCount =
      overrideSummary.runtimeModelResolveResolvedIdentityCount;
  summary.runtimeModelCreateCallerBuildChildLinksCount =
      overrideSummary.runtimeModelCreateCallerBuildChildLinksCount;
  summary.runtimeModelCreateCallerCreateSpriteRuntimeCount =
      overrideSummary.runtimeModelCreateCallerCreateSpriteRuntimeCount;
  summary.runtimeModelCreateCallerOtherCount =
      overrideSummary.runtimeModelCreateCallerOtherCount;
  summary.runtimeModelInitCopyCount =
      overrideSummary.runtimeModelInitCopyCount;
  summary.runtimeModelInitCopyPublishedFallbackCount =
      overrideSummary.runtimeModelInitCopyPublishedFallbackCount;
  summary.attachmentChildLineageBootstrapAttemptCount =
      overrideSummary.attachmentChildLineageBootstrapAttemptCount;
  summary.attachmentChildLineageBootstrapSuccessCount =
      overrideSummary.attachmentChildLineageBootstrapSuccessCount;
  summary.attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount =
      overrideSummary.attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount;
  summary.attachmentChildLineageBootstrapMissNoModelDataLinksCount =
      overrideSummary.attachmentChildLineageBootstrapMissNoModelDataLinksCount;
  summary.attachmentChildLineageBootstrapMissNoUniqueChildCount =
      overrideSummary.attachmentChildLineageBootstrapMissNoUniqueChildCount;
  summary.runtimeSourceObjectCount = instanceRegistry.runtimeSourceObjectCount();
  const auto attachmentRecords =
      model::AttachmentRigidRegistry::instance().snapshot();
  summary.attachmentRigidCount = attachmentRecords.size();
  for (size_t i = 0u; i < attachmentRecords.size(); ++i) {
    const auto& attachment = attachmentRecords[i];
    uint32_t rootCreateCallerRva = 0u;
    uint32_t ownerCreateCallerRva = 0u;
    uint32_t childCreateCallerRva = 0u;
    void* rootCreateHandlePtr = nullptr;
    void* ownerCreateHandlePtr = nullptr;
    void* childCreateHandlePtr = nullptr;
    uint32_t rootResolveCallerRva = 0u;
    uint32_t ownerResolveCallerRva = 0u;
    uint32_t childResolveCallerRva = 0u;
    if (attachment.sourceObjectPtr != nullptr ||
        attachment.sourceSpriteObjectPtr != nullptr) {
      summary.attachmentRigidCountWithSourceObject++;
    }
    const bool hasAnyIdentity =
        attachment.worldObjectEntry != nullptr ||
        attachment.sceneNode != nullptr ||
        attachment.unitPtr != nullptr ||
        attachment.jHandle != 0u ||
        attachment.rawcode != 0u;
    if (hasAnyIdentity)
      summary.attachmentRigidCountWithAnyIdentity++;
    if (attachment.worldObjectEntry != nullptr)
      summary.attachmentRigidCountWithWorldObjectEntry++;
    if (attachment.sceneNode != nullptr)
      summary.attachmentRigidCountWithSceneNode++;
    if (attachment.unitPtr != nullptr)
      summary.attachmentRigidCountWithUnitPtr++;
    if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                     attachment.childRuntimeModelPtr,
                                     childCreateCallerRva)) {
      summary.attachmentRigidChildRuntimeCreateCallerKnownCount++;
    }
    if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                       attachment.childRuntimeModelPtr,
                                       childCreateHandlePtr,
                                       childResolveCallerRva)) {
      if (childCreateHandlePtr != nullptr)
        summary.attachmentRigidChildRuntimeCreateHandleKnownCount++;
      if (childResolveCallerRva != 0u)
        summary.attachmentRigidChildRuntimeResolveCallerKnownCount++;
    }
    if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                     attachment.ownerRuntimeModelPtr,
                                     ownerCreateCallerRva)) {
      summary.attachmentRigidOwnerRuntimeCreateCallerKnownCount++;
    }
    if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                       attachment.ownerRuntimeModelPtr,
                                       ownerCreateHandlePtr,
                                       ownerResolveCallerRva)) {
      if (ownerCreateHandlePtr != nullptr)
        summary.attachmentRigidOwnerRuntimeCreateHandleKnownCount++;
      if (ownerResolveCallerRva != 0u)
        summary.attachmentRigidOwnerRuntimeResolveCallerKnownCount++;
    }
    if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                     attachment.rootRuntimeModelPtr,
                                     rootCreateCallerRva)) {
      summary.attachmentRigidRootRuntimeCreateCallerKnownCount++;
    }
    if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                       attachment.rootRuntimeModelPtr,
                                       rootCreateHandlePtr,
                                       rootResolveCallerRva)) {
      if (rootCreateHandlePtr != nullptr)
        summary.attachmentRigidRootRuntimeCreateHandleKnownCount++;
      if (rootResolveCallerRva != 0u)
        summary.attachmentRigidRootRuntimeResolveCallerKnownCount++;
    }
    if (HasRuntimeOwnerIdentity(instanceRegistry, attachment.childRuntimeModelPtr))
      summary.attachmentRigidChildRuntimeOwnerIdentityCount++;
    if (HasRuntimeOwnerIdentity(instanceRegistry, attachment.ownerRuntimeModelPtr))
      summary.attachmentRigidOwnerRuntimeOwnerIdentityCount++;
    if (HasRuntimeOwnerIdentity(instanceRegistry, attachment.rootRuntimeModelPtr))
      summary.attachmentRigidRootRuntimeOwnerIdentityCount++;
    model::ModelInstanceRecord childRuntimeRecord = {};
    model::ModelInstanceRecord ownerRuntimeRecord = {};
    model::ModelInstanceRecord rootRuntimeRecord = {};
    model::ModelResourceRecord childRuntimeResource = {};
    model::PoseRecord childRuntimePose = {};
    const bool childRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
        instanceRegistry, attachment.childRuntimeModelPtr, childRuntimeRecord);
    const bool ownerRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
        instanceRegistry, attachment.ownerRuntimeModelPtr, ownerRuntimeRecord);
    const bool rootRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
        instanceRegistry, attachment.rootRuntimeModelPtr, rootRuntimeRecord);
    const bool childRuntimeResourceKnown = TryGetRuntimeModelResourceSnapshot(
        attachment.childRuntimeModelPtr, childRuntimeResource);
    const bool childRuntimePoseKnown = TryGetRuntimePoseSnapshot(
        attachment.childRuntimeModelPtr, childRuntimePose);
    if (childRuntimeRecordKnown)
      summary.attachmentRigidChildRuntimeRecordKnownCount++;
    if (ownerRuntimeRecordKnown)
      summary.attachmentRigidOwnerRuntimeRecordKnownCount++;
    if (rootRuntimeRecordKnown)
      summary.attachmentRigidRootRuntimeRecordKnownCount++;
    if (childRuntimeResourceKnown)
      summary.attachmentRigidChildRuntimeModelResourceKnownCount++;
    if (childRuntimePoseKnown)
      summary.attachmentRigidChildRuntimePoseKnownCount++;
    model::RuntimeParentLinkQueryResult childParentLink = {};
    const bool childParentLinkKnown = model::QueryRuntimeParentLink(
        attachment.childRuntimeModelPtr, childParentLink);
    if (childParentLinkKnown)
      summary.attachmentRigidChildRuntimeParentLinkKnownCount++;
    if (attachment.childRuntimeModelPtr != nullptr &&
        uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr)) ==
            overrideSummary.lastAttachedEffectInitChildRuntimeModelPtr) {
      summary.attachmentRigidChildRuntimeMatchesAttachedEffectInitCount++;
    }
    if (attachment.childRuntimeModelPtr != nullptr &&
        uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr)) ==
            overrideSummary.lastAttachModelToPointChildRuntimeModelPtr) {
      summary.attachmentRigidChildRuntimeMatchesAttachModelToPointCount++;
    }
    if (i == 0u) {
      summary.attachmentRigidSample0RootRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.rootRuntimeModelPtr));
      summary.attachmentRigidSample0OwnerRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.ownerRuntimeModelPtr));
      summary.attachmentRigidSample0ChildRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr));
      summary.attachmentRigidSample0ChildSpritePtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childSpritePtr));
      summary.attachmentRigidSample0SourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.sourceObjectPtr));
      summary.attachmentRigidSample0RootRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(rootCreateHandlePtr));
      summary.attachmentRigidSample0OwnerRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(ownerCreateHandlePtr));
      summary.attachmentRigidSample0ChildRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(childCreateHandlePtr));
      summary.attachmentRigidSample0RootRuntimeCreateCallerRva =
          rootCreateCallerRva;
      summary.attachmentRigidSample0OwnerRuntimeCreateCallerRva =
          ownerCreateCallerRva;
      summary.attachmentRigidSample0ChildRuntimeCreateCallerRva =
          childCreateCallerRva;
      summary.attachmentRigidSample0RootRuntimeResolveCallerRva =
          rootResolveCallerRva;
      summary.attachmentRigidSample0OwnerRuntimeResolveCallerRva =
          ownerResolveCallerRva;
      summary.attachmentRigidSample0ChildRuntimeResolveCallerRva =
          childResolveCallerRva;
      summary.attachmentRigidSample0RootRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample0OwnerRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample0RootRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample0OwnerRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample0RootRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample0ChildRuntimeParentRuntimeModelPtr =
          childParentLink.parentRuntimeModelPtr;
      summary.attachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame =
          childParentLink.lastSeenFrame;
      summary.attachmentRigidSample0ChildRuntimeParentLinkSourceMeta =
          childParentLink.sourceMeta;
      summary.attachmentRigidSample0ChildRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample0ChildRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample0ChildRuntimeModelResourcePtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeResource.modelResourcePtr));
      summary.attachmentRigidSample0ChildRuntimeModelKey =
          childRuntimeResource.modelKey;
      summary.attachmentRigidSample0ChildRuntimePoseMatrixCount =
          childRuntimePose.matrixCount;
      summary.attachmentRigidSample0FirstSeenFrame = attachment.firstSeenFrame;
      summary.attachmentRigidSample0LastSeenFrame = attachment.lastSeenFrame;
    } else if (i == 1u) {
      summary.attachmentRigidSample1RootRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.rootRuntimeModelPtr));
      summary.attachmentRigidSample1OwnerRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.ownerRuntimeModelPtr));
      summary.attachmentRigidSample1ChildRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr));
      summary.attachmentRigidSample1ChildSpritePtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childSpritePtr));
      summary.attachmentRigidSample1SourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.sourceObjectPtr));
      summary.attachmentRigidSample1RootRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(rootCreateHandlePtr));
      summary.attachmentRigidSample1OwnerRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(ownerCreateHandlePtr));
      summary.attachmentRigidSample1ChildRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(childCreateHandlePtr));
      summary.attachmentRigidSample1RootRuntimeCreateCallerRva =
          rootCreateCallerRva;
      summary.attachmentRigidSample1OwnerRuntimeCreateCallerRva =
          ownerCreateCallerRva;
      summary.attachmentRigidSample1ChildRuntimeCreateCallerRva =
          childCreateCallerRva;
      summary.attachmentRigidSample1RootRuntimeResolveCallerRva =
          rootResolveCallerRva;
      summary.attachmentRigidSample1OwnerRuntimeResolveCallerRva =
          ownerResolveCallerRva;
      summary.attachmentRigidSample1ChildRuntimeResolveCallerRva =
          childResolveCallerRva;
      summary.attachmentRigidSample1RootRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample1OwnerRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample1RootRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample1OwnerRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample1RootRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample1ChildRuntimeParentRuntimeModelPtr =
          childParentLink.parentRuntimeModelPtr;
      summary.attachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame =
          childParentLink.lastSeenFrame;
      summary.attachmentRigidSample1ChildRuntimeParentLinkSourceMeta =
          childParentLink.sourceMeta;
      summary.attachmentRigidSample1ChildRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample1ChildRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample1ChildRuntimeModelResourcePtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeResource.modelResourcePtr));
      summary.attachmentRigidSample1ChildRuntimeModelKey =
          childRuntimeResource.modelKey;
      summary.attachmentRigidSample1ChildRuntimePoseMatrixCount =
          childRuntimePose.matrixCount;
      summary.attachmentRigidSample1FirstSeenFrame = attachment.firstSeenFrame;
      summary.attachmentRigidSample1LastSeenFrame = attachment.lastSeenFrame;
    }
  }
  const auto contractAttachments =
      shadow::ShadowRuntimeContractCache::instance().snapshotAttachmentsShared();
  if (contractAttachments != nullptr) {
    summary.contractAttachmentRigidCount = contractAttachments->records().size();
    const auto& records = contractAttachments->records();
    for (size_t i = 0u; i < records.size(); ++i) {
      const auto& attachment = records[i];
      uint32_t rootCreateCallerRva = 0u;
      uint32_t ownerCreateCallerRva = 0u;
      uint32_t childCreateCallerRva = 0u;
      void* rootCreateHandlePtr = nullptr;
      void* ownerCreateHandlePtr = nullptr;
      void* childCreateHandlePtr = nullptr;
      uint32_t rootResolveCallerRva = 0u;
      uint32_t ownerResolveCallerRva = 0u;
      uint32_t childResolveCallerRva = 0u;
      if (attachment.sourceObjectPtr != nullptr ||
          attachment.sourceSpriteObjectPtr != nullptr) {
        summary.contractAttachmentRigidCountWithSourceObject++;
      }
      const bool hasAnyIdentity =
          attachment.worldObjectEntry != nullptr ||
          attachment.sceneNode != nullptr ||
          attachment.unitPtr != nullptr ||
          attachment.jHandle != 0u ||
          attachment.rawcode != 0u;
      if (hasAnyIdentity)
        summary.contractAttachmentRigidCountWithAnyIdentity++;
      if (attachment.worldObjectEntry != nullptr)
        summary.contractAttachmentRigidCountWithWorldObjectEntry++;
      if (attachment.sceneNode != nullptr)
        summary.contractAttachmentRigidCountWithSceneNode++;
      if (attachment.unitPtr != nullptr)
        summary.contractAttachmentRigidCountWithUnitPtr++;
      if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                       attachment.childRuntimeModelPtr,
                                       childCreateCallerRva)) {
        summary.contractAttachmentRigidChildRuntimeCreateCallerKnownCount++;
      }
      if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                         attachment.childRuntimeModelPtr,
                                         childCreateHandlePtr,
                                         childResolveCallerRva)) {
        if (childCreateHandlePtr != nullptr)
          summary.contractAttachmentRigidChildRuntimeCreateHandleKnownCount++;
        if (childResolveCallerRva != 0u)
          summary.contractAttachmentRigidChildRuntimeResolveCallerKnownCount++;
      }
      if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                       attachment.ownerRuntimeModelPtr,
                                       ownerCreateCallerRva)) {
        summary.contractAttachmentRigidOwnerRuntimeCreateCallerKnownCount++;
      }
      if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                         attachment.ownerRuntimeModelPtr,
                                         ownerCreateHandlePtr,
                                         ownerResolveCallerRva)) {
        if (ownerCreateHandlePtr != nullptr)
          summary.contractAttachmentRigidOwnerRuntimeCreateHandleKnownCount++;
        if (ownerResolveCallerRva != 0u)
          summary.contractAttachmentRigidOwnerRuntimeResolveCallerKnownCount++;
      }
      if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                       attachment.rootRuntimeModelPtr,
                                       rootCreateCallerRva)) {
        summary.contractAttachmentRigidRootRuntimeCreateCallerKnownCount++;
      }
      if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                         attachment.rootRuntimeModelPtr,
                                         rootCreateHandlePtr,
                                         rootResolveCallerRva)) {
        if (rootCreateHandlePtr != nullptr)
          summary.contractAttachmentRigidRootRuntimeCreateHandleKnownCount++;
        if (rootResolveCallerRva != 0u)
          summary.contractAttachmentRigidRootRuntimeResolveCallerKnownCount++;
      }
      if (HasRuntimeOwnerIdentity(instanceRegistry,
                                  attachment.childRuntimeModelPtr)) {
        summary.contractAttachmentRigidChildRuntimeOwnerIdentityCount++;
      }
      if (HasRuntimeOwnerIdentity(instanceRegistry,
                                  attachment.ownerRuntimeModelPtr)) {
        summary.contractAttachmentRigidOwnerRuntimeOwnerIdentityCount++;
      }
      if (HasRuntimeOwnerIdentity(instanceRegistry,
                                  attachment.rootRuntimeModelPtr)) {
        summary.contractAttachmentRigidRootRuntimeOwnerIdentityCount++;
      }
      model::ModelInstanceRecord childRuntimeRecord = {};
      model::ModelInstanceRecord ownerRuntimeRecord = {};
      model::ModelInstanceRecord rootRuntimeRecord = {};
      model::ModelResourceRecord childRuntimeResource = {};
      model::PoseRecord childRuntimePose = {};
      const bool childRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
          instanceRegistry, attachment.childRuntimeModelPtr, childRuntimeRecord);
      const bool ownerRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
          instanceRegistry, attachment.ownerRuntimeModelPtr, ownerRuntimeRecord);
      const bool rootRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
          instanceRegistry, attachment.rootRuntimeModelPtr, rootRuntimeRecord);
      const bool childRuntimeResourceKnown = TryGetRuntimeModelResourceSnapshot(
          attachment.childRuntimeModelPtr, childRuntimeResource);
      const bool childRuntimePoseKnown = TryGetRuntimePoseSnapshot(
          attachment.childRuntimeModelPtr, childRuntimePose);
      if (childRuntimeRecordKnown)
        summary.contractAttachmentRigidChildRuntimeRecordKnownCount++;
      if (ownerRuntimeRecordKnown)
        summary.contractAttachmentRigidOwnerRuntimeRecordKnownCount++;
      if (rootRuntimeRecordKnown)
        summary.contractAttachmentRigidRootRuntimeRecordKnownCount++;
      if (childRuntimeResourceKnown)
        summary.contractAttachmentRigidChildRuntimeModelResourceKnownCount++;
      if (childRuntimePoseKnown)
        summary.contractAttachmentRigidChildRuntimePoseKnownCount++;
      model::RuntimeParentLinkQueryResult childParentLink = {};
      const bool childParentLinkKnown = model::QueryRuntimeParentLink(
          attachment.childRuntimeModelPtr, childParentLink);
      if (childParentLinkKnown)
        summary.contractAttachmentRigidChildRuntimeParentLinkKnownCount++;
      if (attachment.childRuntimeModelPtr != nullptr &&
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr)) ==
              overrideSummary.lastAttachedEffectInitChildRuntimeModelPtr) {
        summary
            .contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount++;
      }
      if (attachment.childRuntimeModelPtr != nullptr &&
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr)) ==
              overrideSummary.lastAttachModelToPointChildRuntimeModelPtr) {
        summary
            .contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount++;
      }
      if (i == 0u) {
        summary.contractAttachmentRigidSample0RootRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.rootRuntimeModelPtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                attachment.ownerRuntimeModelPtr));
        summary.contractAttachmentRigidSample0ChildRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr));
        summary.contractAttachmentRigidSample0ChildSpritePtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.childSpritePtr));
        summary.contractAttachmentRigidSample0SourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.sourceObjectPtr));
        summary.contractAttachmentRigidSample0WorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.worldObjectEntry));
        summary.contractAttachmentRigidSample0SceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.sceneNode));
        summary.contractAttachmentRigidSample0RootRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(rootCreateHandlePtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(ownerCreateHandlePtr));
        summary.contractAttachmentRigidSample0ChildRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(childCreateHandlePtr));
        summary.contractAttachmentRigidSample0RootRuntimeCreateCallerRva =
            rootCreateCallerRva;
        summary.contractAttachmentRigidSample0OwnerRuntimeCreateCallerRva =
            ownerCreateCallerRva;
        summary.contractAttachmentRigidSample0ChildRuntimeCreateCallerRva =
            childCreateCallerRva;
        summary.contractAttachmentRigidSample0RootRuntimeResolveCallerRva =
            rootResolveCallerRva;
        summary.contractAttachmentRigidSample0OwnerRuntimeResolveCallerRva =
            ownerResolveCallerRva;
        summary.contractAttachmentRigidSample0ChildRuntimeResolveCallerRva =
            childResolveCallerRva;
        summary.contractAttachmentRigidSample0RootRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample0RootRuntimeWorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.worldObjectEntry));
        summary.contractAttachmentRigidSample0RootRuntimeSceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(rootRuntimeRecord.sceneNode));
        summary.contractAttachmentRigidSample0OwnerRuntimeWorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.worldObjectEntry));
        summary.contractAttachmentRigidSample0OwnerRuntimeSceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(ownerRuntimeRecord.sceneNode));
        summary.contractAttachmentRigidSample0RootRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample0RootRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.sourceSpriteObjectPtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.sourceSpriteObjectPtr));
        summary
            .contractAttachmentRigidSample0ChildRuntimeParentRuntimeModelPtr =
            childParentLink.parentRuntimeModelPtr;
        summary.contractAttachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame =
            childParentLink.lastSeenFrame;
        summary.contractAttachmentRigidSample0ChildRuntimeParentLinkSourceMeta =
            childParentLink.sourceMeta;
        summary.contractAttachmentRigidSample0ChildRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample0ChildRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.sourceSpriteObjectPtr));
        summary.contractAttachmentRigidSample0ChildRuntimeModelResourcePtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeResource.modelResourcePtr));
        summary.contractAttachmentRigidSample0ChildRuntimeModelKey =
            childRuntimeResource.modelKey;
        summary.contractAttachmentRigidSample0ChildRuntimePoseMatrixCount =
            childRuntimePose.matrixCount;
      } else if (i == 1u) {
        summary.contractAttachmentRigidSample1RootRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.rootRuntimeModelPtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                attachment.ownerRuntimeModelPtr));
        summary.contractAttachmentRigidSample1ChildRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr));
        summary.contractAttachmentRigidSample1ChildSpritePtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.childSpritePtr));
        summary.contractAttachmentRigidSample1SourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.sourceObjectPtr));
        summary.contractAttachmentRigidSample1WorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.worldObjectEntry));
        summary.contractAttachmentRigidSample1SceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.sceneNode));
        summary.contractAttachmentRigidSample1RootRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(rootCreateHandlePtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(ownerCreateHandlePtr));
        summary.contractAttachmentRigidSample1ChildRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(childCreateHandlePtr));
        summary.contractAttachmentRigidSample1RootRuntimeCreateCallerRva =
            rootCreateCallerRva;
        summary.contractAttachmentRigidSample1OwnerRuntimeCreateCallerRva =
            ownerCreateCallerRva;
        summary.contractAttachmentRigidSample1ChildRuntimeCreateCallerRva =
            childCreateCallerRva;
        summary.contractAttachmentRigidSample1RootRuntimeResolveCallerRva =
            rootResolveCallerRva;
        summary.contractAttachmentRigidSample1OwnerRuntimeResolveCallerRva =
            ownerResolveCallerRva;
        summary.contractAttachmentRigidSample1ChildRuntimeResolveCallerRva =
            childResolveCallerRva;
        summary.contractAttachmentRigidSample1RootRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample1RootRuntimeWorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.worldObjectEntry));
        summary.contractAttachmentRigidSample1RootRuntimeSceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(rootRuntimeRecord.sceneNode));
        summary.contractAttachmentRigidSample1OwnerRuntimeWorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.worldObjectEntry));
        summary.contractAttachmentRigidSample1OwnerRuntimeSceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(ownerRuntimeRecord.sceneNode));
        summary.contractAttachmentRigidSample1RootRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample1RootRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.sourceSpriteObjectPtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.sourceSpriteObjectPtr));
        summary
            .contractAttachmentRigidSample1ChildRuntimeParentRuntimeModelPtr =
            childParentLink.parentRuntimeModelPtr;
        summary.contractAttachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame =
            childParentLink.lastSeenFrame;
        summary.contractAttachmentRigidSample1ChildRuntimeParentLinkSourceMeta =
            childParentLink.sourceMeta;
        summary.contractAttachmentRigidSample1ChildRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample1ChildRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.sourceSpriteObjectPtr));
        summary.contractAttachmentRigidSample1ChildRuntimeModelResourcePtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeResource.modelResourcePtr));
        summary.contractAttachmentRigidSample1ChildRuntimeModelKey =
            childRuntimeResource.modelKey;
        summary.contractAttachmentRigidSample1ChildRuntimePoseMatrixCount =
            childRuntimePose.matrixCount;
      }
    }
  }
  const auto upperLayerStats = UpperLayerShadowRegistry::instance().snapshotStats();
  summary.upperLayerResolveAttempts = upperLayerStats.resolveAttempts;
  summary.upperLayerResolveVisibleMiss = upperLayerStats.resolveVisibleMiss;
  summary.upperLayerResolveVisibleUnresolvedGeoset =
      upperLayerStats.resolveVisibleUnresolvedGeoset;
  summary.upperLayerResolveGeosetMiss = upperLayerStats.resolveGeosetMiss;
  summary.upperLayerResolvePoseMiss = upperLayerStats.resolvePoseMiss;
  summary.upperLayerResolveRuntimeGroupPaletteMiss =
      upperLayerStats.resolveRuntimeGroupPaletteMiss;
  summary.upperLayerResolveAuthoritativeRigid =
      upperLayerStats.resolveAuthoritativeRigid;
  summary.upperLayerResolveAuthoritativeSkinned =
      upperLayerStats.resolveAuthoritativeSkinned;
  summary.upperLayerResolvedAuthoritativeItems =
      upperLayerStats.resolvedAuthoritativeItems;
  summary.upperLayerEmitted = upperLayerStats.emitted;
  summary.upperLayerDuplicateOrSuppressed =
      upperLayerStats.duplicateOrSuppressed;
  {
    std::lock_guard<std::mutex> lock(g_shadowSceneStatsMutex);
    summary.fallbackDrawCount = g_shadowSceneStats.fallbackDrawCount;
    summary.fallbackDrawCountTerrain =
        g_shadowSceneStats.fallbackDrawCountTerrain;
    summary.fallbackDrawCountWorldObject =
        g_shadowSceneStats.fallbackDrawCountWorldObject;
    summary.fallbackDrawCountUnitObject =
        g_shadowSceneStats.fallbackDrawCountUnitObject;
    summary.objectFallbackDrawCount =
        g_shadowSceneStats.fallbackDrawCountWorldObject +
        g_shadowSceneStats.fallbackDrawCountUnitObject;
    summary.semanticSceneSubmitted = g_shadowSceneStats.semanticSceneSubmitted;
    summary.semanticSceneSubmittedUnit =
        g_shadowSceneStats.semanticSceneSubmittedUnit;
    summary.semanticSceneSubmittedSkinned =
        g_shadowSceneStats.semanticSceneSubmittedSkinned;
    summary.semanticSceneLivePaletteRefreshAttemptCount =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshAttemptCount;
    summary.semanticSceneLivePaletteRefreshHitCount =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshHitCount;
    summary.semanticSceneLivePaletteRefreshMissCount =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshMissCount;
    summary.semanticSceneLivePaletteRefreshLastRuntimeModelPtr =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshLastRuntimeModelPtr;
    summary.semanticSceneLivePaletteRefreshLastMatrixCount =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshLastMatrixCount;
    summary.semanticSceneLivePaletteRefreshLastMatrixHash =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshLastMatrixHash;
    summary.semanticSceneLivePaletteMotionSampleCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionSampleCount;
    summary.semanticSceneLivePaletteMotionNewRuntimeCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionNewRuntimeCount;
    summary.semanticSceneLivePaletteMotionRawChangedCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionRawChangedCount;
    summary.semanticSceneLivePaletteMotionRawStableCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionRawStableCount;
    summary.semanticSceneLivePaletteMotionGroupChangedCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionGroupChangedCount;
    summary.semanticSceneLivePaletteMotionGroupStableCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionGroupStableCount;
    summary.semanticSceneLivePaletteMotionLastRuntimeModelPtr =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastRuntimeModelPtr;
    summary.semanticSceneLivePaletteMotionLastPrevRawHash =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastPrevRawHash;
    summary.semanticSceneLivePaletteMotionLastRawHash =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastRawHash;
    summary.semanticSceneLivePaletteMotionLastPrevGroupHash =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastPrevGroupHash;
    summary.semanticSceneLivePaletteMotionLastGroupHash =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastGroupHash;
    summary.semanticSceneDrawTimePoseAttemptCount =
        g_shadowSceneStats.semanticSceneDrawTimePoseAttemptCount;
    summary.semanticSceneDrawTimePosePublishedCount =
        g_shadowSceneStats.semanticSceneDrawTimePosePublishedCount;
    summary.semanticSceneDrawTimePoseChangedCount =
        g_shadowSceneStats.semanticSceneDrawTimePoseChangedCount;
    summary.semanticSceneDrawTimePoseStableCount =
        g_shadowSceneStats.semanticSceneDrawTimePoseStableCount;
    summary.semanticSceneDrawTimePoseLastRuntimeModelPtr =
        g_shadowSceneStats.semanticSceneDrawTimePoseLastRuntimeModelPtr;
    summary.semanticSceneDrawTimePoseLastPrevHash =
        g_shadowSceneStats.semanticSceneDrawTimePoseLastPrevHash;
    summary.semanticSceneDrawTimePoseLastHash =
        g_shadowSceneStats.semanticSceneDrawTimePoseLastHash;
    summary.semanticSceneSubmittedPaletteMotionSampleCount =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionSampleCount;
    summary.semanticSceneSubmittedPaletteMotionNewRuntimeCount =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionNewRuntimeCount;
    summary.semanticSceneSubmittedPaletteMotionChangedCount =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionChangedCount;
    summary.semanticSceneSubmittedPaletteMotionStableCount =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionStableCount;
    summary.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr;
    summary.semanticSceneSubmittedPaletteMotionLastPrevHash =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionLastPrevHash;
    summary.semanticSceneSubmittedPaletteMotionLastHash =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionLastHash;
    summary.semanticSceneSkinnedDynamicIndexSliceCount =
        g_shadowSceneStats.semanticSceneSkinnedDynamicIndexSliceCount;
    summary.semanticSceneSkinnedFullIndexFallbackCount =
        g_shadowSceneStats.semanticSceneSkinnedFullIndexFallbackCount;
    summary.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount =
        g_shadowSceneStats
            .semanticSceneSkinnedMissingVisibleIndexSliceRejectCount;
    summary.semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr =
        g_shadowSceneStats
            .semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr;
    summary.semanticSceneSkinnedFullIndexFallbackLastIndexCount =
        g_shadowSceneStats
            .semanticSceneSkinnedFullIndexFallbackLastIndexCount;
    summary.semanticSceneSubmittedFrameLocal =
        g_shadowSceneStats.semanticSceneSubmittedFrameLocal;
    summary.semanticSceneSubmittedPersistent =
        g_shadowSceneStats.semanticSceneSubmitted >
                g_shadowSceneStats.semanticSceneSubmittedFrameLocal
            ? (g_shadowSceneStats.semanticSceneSubmitted -
               g_shadowSceneStats.semanticSceneSubmittedFrameLocal)
            : 0u;
    summary.semanticSceneStatsPublishCount = g_shadowSceneStatsPublishCount;
    summary.semanticSceneInputDrawCount =
        g_shadowSceneStats.semanticSceneInputDrawCount;
    summary.semanticSceneInputSkinnedCount =
        g_shadowSceneStats.semanticSceneInputSkinnedCount;
    summary.semanticSceneTailBoundaryCandidateCount =
        g_shadowSceneStats.semanticSceneTailBoundaryCandidateCount;
    summary.semanticSceneTailBoundaryCommitCount =
        g_shadowSceneStats.semanticSceneTailBoundaryCommitCount;
    summary.semanticScenePopulateAttemptCount =
        g_shadowSceneStats.semanticScenePopulateAttemptCount;
    summary.semanticScenePopulateUnitsOnlyCount =
        g_shadowSceneStats.semanticScenePopulateUnitsOnlyCount;
    summary.semanticSceneLastInputDrawCount =
        g_shadowSceneStats.semanticSceneLastInputDrawCount;
    summary.semanticSceneLastInputSkinnedCount =
        g_shadowSceneStats.semanticSceneLastInputSkinnedCount;
    summary.semanticSceneLastSubmittedDrawCount =
        g_shadowSceneStats.semanticSceneLastSubmittedDrawCount;
    summary.semanticSceneLastUnitsOnlyFilteredCount =
        g_shadowSceneStats.semanticSceneLastUnitsOnlyFilteredCount;
    summary.semanticSceneCatchupAttemptCount =
        g_shadowSceneStats.semanticSceneCatchupAttemptCount;
    summary.semanticSceneCatchupSuccessCount =
        g_shadowSceneStats.semanticSceneCatchupSuccessCount;
    summary.semanticSceneSkippedEmptyFrameCount =
        g_shadowSceneStats.semanticSceneSkippedEmptyFrameCount;
    summary.semanticSceneZeroSubmitCount =
        g_shadowSceneStats.semanticSceneZeroSubmitCount;
    summary.semanticSceneSelectedFrameEligibleZeroCount =
        g_shadowSceneStats.semanticSceneSelectedFrameEligibleZeroCount;
    summary.semanticSceneReusableFrameForcedCount =
        g_shadowSceneStats.semanticSceneReusableFrameForcedCount;
    summary.semanticSceneReusableFrameUnavailableCount =
        g_shadowSceneStats.semanticSceneReusableFrameUnavailableCount;
    summary.semanticSceneReusableFrameRejectedNativeValidationCount =
        g_shadowSceneStats
            .semanticSceneReusableFrameRejectedNativeValidationCount;
    summary.semanticSceneLastFrameSerial =
        g_shadowSceneStats.semanticSceneLastFrameSerial;
    summary.semanticSceneLastSelectedFrameSerial =
        g_shadowSceneStats.semanticSceneLastSelectedFrameSerial;
    summary.semanticSceneLastReusableFrameSerial =
        g_shadowSceneStats.semanticSceneLastReusableFrameSerial;
    summary.semanticSceneLastSourcePublishRevision =
        g_shadowSceneStats.semanticSceneLastSourcePublishRevision;
    summary.semanticSceneLastTargetPublishRevision =
        g_shadowSceneStats.semanticSceneLastTargetPublishRevision;
    summary.semanticSceneBypassUnitLikeCount =
        g_shadowSceneStats.semanticSceneBypassUnitLikeCount;
    summary.semanticSceneBypassUnitLikeWithRuntimeModel =
        g_shadowSceneStats.semanticSceneBypassUnitLikeWithRuntimeModel;
    summary.semanticSceneBypassUnitLikeWithModelResource =
        g_shadowSceneStats.semanticSceneBypassUnitLikeWithModelResource;
    summary.semanticSceneBypassUnitLikeWithPose =
        g_shadowSceneStats.semanticSceneBypassUnitLikeWithPose;
    summary.semanticSceneBypassUnitLikeWithRenderable =
        g_shadowSceneStats.semanticSceneBypassUnitLikeWithRenderable;
    summary.semanticSceneBypassPublishedVisibleCandidate =
        g_shadowSceneStats.semanticSceneBypassPublishedVisibleCandidate;
    summary.semanticSceneBypassPublishMiss =
        g_shadowSceneStats.semanticSceneBypassPublishMiss;
    summary.semanticSceneSkippedUnitsOnlyFilter =
        g_shadowSceneStats.semanticSceneSkippedUnitsOnlyFilter;
    summary.semanticSceneAcceptedExplicitResourceOwnerRigid =
        g_shadowSceneStats.semanticSceneAcceptedExplicitResourceOwnerRigid;
    summary.semanticSceneRejectedGeometry =
        g_shadowSceneStats.semanticSceneRejectedGeometry;
    summary.semanticSceneRejectedGeometryFrameLocal =
        g_shadowSceneStats.semanticSceneRejectedGeometryFrameLocal;
    summary.semanticSceneRejectedGeometryPersistent =
        g_shadowSceneStats.semanticSceneRejectedGeometryPersistent;
    summary.semanticFallbackPruned = g_shadowSceneStats.semanticFallbackPruned;
    summary.semanticFallbackPrunedByHandle =
        g_shadowSceneStats.semanticFallbackPrunedByHandle;
    summary.semanticFallbackPrunedByWorldObjectEntry =
        g_shadowSceneStats.semanticFallbackPrunedByWorldObjectEntry;
    summary.semanticFallbackPrunedBySceneNode =
        g_shadowSceneStats.semanticFallbackPrunedBySceneNode;
    summary.semanticFallbackPrunedByRuntimeModel =
        g_shadowSceneStats.semanticFallbackPrunedByRuntimeModel;
    summary.persistentGeometryCount =
        g_shadowSceneStats.persistentGeometryCount;
    summary.duplicateGeometryInstances =
        g_shadowSceneStats.duplicateGeometryInstances;
    summary.instancedGeometryDrawsSaved =
        g_shadowSceneStats.instancedGeometryDrawsSaved;
  }
  const auto semanticCore =
      shadow::ShadowValidationRuntime::instance().snapshot();
  const auto buildState =
      shadow::ShadowValidationRuntime::instance().buildStateSnapshot();
  const auto publishedBundle =
      shadow::ShadowRuntimeContractCache::instance().snapshotBundleShared();
  const auto& publishedManifest = publishedBundle.manifest;
  summary.semanticContractCaptureSkippedStableSameFrame =
      publishedBundle.stats.contractCaptureSkippedStableSameFrame;
  summary.semanticContractCaptureSkippedEmpty =
      publishedBundle.stats.contractCaptureSkippedEmpty;
  summary.semanticContractCaptureSkippedDuplicateSameFrame =
      publishedBundle.stats.contractCaptureSkippedDuplicateSameFrame;
  summary.semanticVisibleDirectUnitCandidateAccepted =
      publishedBundle.stats.visibleDirectUnitCandidateAccepted;
  summary.semanticVisibleDirectUnitRejectedNotUnitLike =
      publishedBundle.stats.visibleDirectUnitRejectedNotUnitLike;
  summary.semanticVisibleDirectUnitRejectedGroup =
      publishedBundle.stats.visibleDirectUnitRejectedGroup;
  summary.semanticVisibleDirectUnitRejectedNoUnitPtr =
      publishedBundle.stats.visibleDirectUnitRejectedNoUnitPtr;
  summary.semanticVisibleDirectUnitRejectedNoIdentity =
      publishedBundle.stats.visibleDirectUnitRejectedNoIdentity;
  summary.semanticVisibleDirectUnitRejectedNoMesh =
      publishedBundle.stats.visibleDirectUnitRejectedNoMesh;
  summary.semanticVisibleDirectUnitRejectedBuilding =
      publishedBundle.stats.visibleDirectUnitRejectedBuilding;
  summary.semanticVisibleDirectUnitRejectedNoGeoset =
      publishedBundle.stats.visibleDirectUnitRejectedNoGeoset;
  if (publishedManifest != nullptr) {
    summary.semanticCoreManifestFrameSerial = publishedManifest->frameSerial;
    summary.semanticCoreManifestPublishRevision =
        publishedManifest->publishRevision;
  }
  summary.semanticCoreFrameSerial = semanticCore.frameSerial;
  summary.semanticCoreSourcePublishRevision =
      semanticCore.sourcePublishRevision;
  if (summary.semanticCoreSourcePublishRevision >
      summary.semanticSceneLastSourcePublishRevision) {
    summary.semanticScenePublishRevisionLag =
        summary.semanticCoreSourcePublishRevision -
        summary.semanticSceneLastSourcePublishRevision;
  }
  summary.semanticCoreSourceVisibleCount = semanticCore.sourceVisibleCount;
  summary.semanticCoreSourceStableIdentityCount =
      semanticCore.sourceStableIdentityCount;
  summary.semanticCoreSourceResolvedGeosetCount =
      semanticCore.sourceResolvedGeosetCount;
  summary.semanticCoreSourceUnitCount = semanticCore.sourceUnitCount;
  if (summary.semanticCoreManifestFrameSerial >= summary.semanticCoreFrameSerial) {
    summary.semanticCoreFrameLag =
        summary.semanticCoreManifestFrameSerial - summary.semanticCoreFrameSerial;
  }
  if (summary.semanticCoreManifestPublishRevision >=
      summary.semanticCoreSourcePublishRevision) {
    summary.semanticCorePublishRevisionLag =
        summary.semanticCoreManifestPublishRevision -
        summary.semanticCoreSourcePublishRevision;
  }
  const bool publishLagFreshEnough =
      summary.semanticCorePublishRevisionLag <= 16u;
  summary.semanticCoreFrameFresh =
      summary.semanticCoreFrameSerial != 0u &&
      summary.semanticCoreSourcePublishRevision != 0u &&
      !buildState.buildInProgress &&
      summary.semanticCoreFrameLag <= 1u &&
      publishLagFreshEnough;
  summary.semanticCoreConsidered = semanticCore.resolve.considered;
  summary.semanticCoreResolved = semanticCore.resolve.resolved;
  summary.semanticCoreRigidResolved = semanticCore.resolve.rigidResolved;
  summary.semanticCoreSkinnedResolved = semanticCore.resolve.skinnedResolved;
  summary.semanticCoreSlowestRecordResolveUs =
      semanticCore.resolve.slowestRecordResolveUs;
  summary.semanticCoreSlowestRecordIndex =
      semanticCore.resolve.slowestRecordIndex;
  summary.semanticCoreSlowestRecordRuntimeModelPtr =
      semanticCore.resolve.slowestRecordRuntimeModelPtr;
  summary.semanticCoreSlowestRecordModelResourcePtr =
      semanticCore.resolve.slowestRecordModelResourcePtr;
  summary.semanticCoreSlowestRecordRuntimeGeosetPtr =
      semanticCore.resolve.slowestRecordRuntimeGeosetPtr;
  summary.semanticCoreSlowestRecordRuntimeGeosetDataPtr =
      semanticCore.resolve.slowestRecordRuntimeGeosetDataPtr;
  summary.semanticCoreSlowestRecordGeosetIndex =
      semanticCore.resolve.slowestRecordGeosetIndex;
  summary.semanticCoreSlowestRecordObjectKind =
      semanticCore.resolve.slowestRecordObjectKind;
  summary.semanticCoreSlowestResourceLookupUs =
      semanticCore.resolve.slowestResourceLookupUs;
  summary.semanticCoreSlowestPoseResolveUs =
      semanticCore.resolve.slowestPoseResolveUs;
  summary.semanticCoreSlowestPoseDirectLookupUs =
      semanticCore.resolve.slowestPoseDirectLookupUs;
  summary.semanticCoreSlowestPoseOwnerLookupUs =
      semanticCore.resolve.slowestPoseOwnerLookupUs;
  summary.semanticCoreSlowestPoseSpriteLookupUs =
      semanticCore.resolve.slowestPoseSpriteLookupUs;
  summary.semanticCoreSlowestPoseInstanceRegistryUs =
      semanticCore.resolve.slowestPoseInstanceRegistryUs;
  summary.semanticCoreSlowestPoseShadowRegistryUs =
      semanticCore.resolve.slowestPoseShadowRegistryUs;
  summary.semanticCoreSlowestPoseRenderRegistryUs =
      semanticCore.resolve.slowestPoseRenderRegistryUs;
  summary.semanticCoreSlowestPoseRuntimeRootsUs =
      semanticCore.resolve.slowestPoseRuntimeRootsUs;
  summary.semanticCoreSlowestPoseMeshPoseContextUs =
      semanticCore.resolve.slowestPoseMeshPoseContextUs;
  summary.semanticCoreSlowestPoseMissDiagnosticUs =
      semanticCore.resolve.slowestPoseMissDiagnosticUs;
  summary.semanticCoreSlowestLayerContractUs =
      semanticCore.resolve.slowestLayerContractUs;
  summary.semanticCoreSlowestRuntimeGroupPaletteUs =
      semanticCore.resolve.slowestRuntimeGroupPaletteUs;
  summary.semanticCoreSlowestRuntimeGroupPaletteRescueUs =
      semanticCore.resolve.slowestRuntimeGroupPaletteRescueUs;
  summary.semanticCoreSlowestAttachmentRigidResolveUs =
      semanticCore.resolve.slowestAttachmentRigidResolveUs;
  summary.semanticCoreRigidCandidateCount =
      semanticCore.resolve.rigidCandidateCount;
  summary.semanticCoreSkinnedCandidateCount =
      semanticCore.resolve.skinnedCandidateCount;
  summary.semanticCoreSkinnedCandidatePoseReadyCount =
      semanticCore.resolve.skinnedCandidatePoseReadyCount;
  summary.semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount =
      semanticCore.resolve.skinnedCandidateRuntimeGroupPaletteReadyCount;
  summary.semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount =
      semanticCore.resolve.skinnedCandidateResolvedAsAttachmentRigidCount;
  summary.semanticCoreRuntimeGroupPaletteMissNoSkinningData =
      semanticCore.resolve.runtimeGroupPaletteMissNoSkinningData;
  summary.semanticCoreRuntimeGroupPaletteMissNoPosePalette =
      semanticCore.resolve.runtimeGroupPaletteMissNoPosePalette;
  summary.semanticCoreRuntimeGroupPaletteMissNoVertexGroups =
      semanticCore.resolve.runtimeGroupPaletteMissNoVertexGroups;
  summary.semanticCoreRuntimeGroupPaletteMissInvalidGroupTable =
      semanticCore.resolve.runtimeGroupPaletteMissInvalidGroupTable;
  summary.semanticCoreRuntimeGroupPaletteMissMatrixIndexOutOfRange =
      semanticCore.resolve.runtimeGroupPaletteMissMatrixIndexOutOfRange;
  summary.semanticCoreRuntimeGroupPaletteMissVertexGroupOutOfRange =
      semanticCore.resolve.runtimeGroupPaletteMissVertexGroupOutOfRange;
  summary.semanticCoreRuntimeGroupPaletteMissFallbacksFailed =
      semanticCore.resolve.runtimeGroupPaletteMissFallbacksFailed;
  summary.semanticCoreRuntimeGroupPaletteMissLastPoseCount =
      semanticCore.resolve.runtimeGroupPaletteMissLastPoseCount;
  summary.semanticCoreRuntimeGroupPaletteMissLastGroupCount =
      semanticCore.resolve.runtimeGroupPaletteMissLastGroupCount;
  summary.semanticCoreRuntimeGroupPaletteMissLastMaxVertexGroupSlot =
      semanticCore.resolve.runtimeGroupPaletteMissLastMaxVertexGroupSlot;
  summary.semanticCoreRuntimeGroupPaletteMissLastMatrixIndexCount =
      semanticCore.resolve.runtimeGroupPaletteMissLastMatrixIndexCount;
  summary.semanticCoreRuntimeGroupPaletteMissLastMatrixIndex =
      semanticCore.resolve.runtimeGroupPaletteMissLastMatrixIndex;
  summary.semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext =
      semanticCore.resolve.runtimeGroupPaletteRescueByMeshPoseContext;
  summary.semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose =
      semanticCore.resolve.runtimeGroupPaletteRescueByResourceMatchedPose;
  summary.semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot =
      semanticCore.resolve.runtimeGroupPaletteRescueByRuntimeRoot;
  summary.semanticCoreRuntimeGroupPaletteRescueByChildRuntime =
      semanticCore.resolve.runtimeGroupPaletteRescueByChildRuntime;
  summary.semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime =
      semanticCore.resolve.runtimeGroupPaletteRescueByDescendantRuntime;
  summary.semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed =
      semanticCore.resolve.runtimeGroupPaletteResourceMatchedPoseSuppressed;
  summary.semanticCoreExplicitResourceOwnerRigidResolved =
      semanticCore.resolve.explicitResourceOwnerRigidResolved;
  summary.semanticCoreExplicitResourceOwnerRigidWorldTransformResolved =
      semanticCore.resolve.explicitResourceOwnerRigidWorldTransformResolved;
  summary.semanticCoreExplicitResourceOwnerRigidNoMatrixPalette =
      semanticCore.resolve.explicitResourceOwnerRigidNoMatrixPalette;
  summary.semanticCoreAttachmentRigidMatchByChildRuntimeModel =
      semanticCore.resolve.attachmentRigidMatchByChildRuntimeModel;
  summary.semanticCoreAttachmentRigidMatchByChildSprite =
      semanticCore.resolve.attachmentRigidMatchByChildSprite;
  summary.semanticCoreAttachmentRigidMatchByChildRuntimeGeoset =
      semanticCore.resolve.attachmentRigidMatchByChildRuntimeGeoset;
  summary.semanticCoreAttachmentRigidMatchByChildSpriteRuntimeGeoset =
      semanticCore.resolve.attachmentRigidMatchByChildSpriteRuntimeGeoset;
  summary.semanticCoreAttachmentRigidMatchByOwnerRuntimeGeoset =
      semanticCore.resolve.attachmentRigidMatchByOwnerRuntimeGeoset;
  summary.semanticCoreAttachmentRigidMatchByRootRuntimeGeoset =
      semanticCore.resolve.attachmentRigidMatchByRootRuntimeGeoset;
  summary.semanticCoreAttachmentRigidMatchByResourceRuntimeOwner =
      semanticCore.resolve.attachmentRigidMatchByResourceRuntimeOwner;
  summary.semanticCoreAttachmentRigidMatchByRenderableRuntimeRoot =
      semanticCore.resolve.attachmentRigidMatchByRenderableRuntimeRoot;
  summary.semanticCoreAttachmentRigidMatchByWorldObjectEntry =
      semanticCore.resolve.attachmentRigidMatchByWorldObjectEntry;
  summary.semanticCoreAttachmentRigidMatchBySceneNode =
      semanticCore.resolve.attachmentRigidMatchBySceneNode;
  summary.semanticCoreAttachmentRigidMatchByUnitPtr =
      semanticCore.resolve.attachmentRigidMatchByUnitPtr;
  summary.semanticCoreAttachmentRigidMatchByHandle =
      semanticCore.resolve.attachmentRigidMatchByHandle;
  summary.semanticCoreAttachmentRigidMatchByChildModelResource =
      semanticCore.resolve.attachmentRigidMatchByChildModelResource;
  summary.semanticCoreAttachmentRigidMatchByUniqueIdentity =
      semanticCore.resolve.attachmentRigidMatchByUniqueIdentity;
  summary.semanticCoreAttachmentRigidMatchMiss =
      semanticCore.resolve.attachmentRigidMatchMiss;
  summary.lastAttachmentRigidMatchMissRuntimeModelPtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissRuntimeModelPtr;
  summary.lastAttachmentRigidMatchMissModelResourcePtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissModelResourcePtr;
  summary.lastAttachmentRigidMatchMissRuntimeGeosetPtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissRuntimeGeosetPtr;
  summary.lastAttachmentRigidMatchMissRuntimeGeosetDataPtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissRuntimeGeosetDataPtr;
  summary.lastAttachmentRigidMatchMissGeosetIndex =
      semanticCore.resolve.lastAttachmentRigidMatchMissGeosetIndex;
  summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr;
  if (summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr != 0u) {
    const auto missRuntimeOwnerPtr = reinterpret_cast<void*>(
        static_cast<uintptr_t>(
            summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr));
    model::ModelInstanceRecord runtimeOwnerRecord = {};
    if (model::ModelInstanceRegistry::instance().findByRuntimeModel(
            missRuntimeOwnerPtr, runtimeOwnerRecord) ||
        model::ModelInstanceRegistry::instance().findOwnerByRuntimeModel(
            missRuntimeOwnerPtr, runtimeOwnerRecord)) {
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerWorldObjectEntryPtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.worldObjectEntry);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSceneNodePtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.sceneNode);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerUnitPtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.unitPtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.spritePtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceObjectPtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.sourceObjectPtr);
      summary
          .lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceSpriteObjectPtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.sourceSpriteObjectPtr);
      summary
          .lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateModelDataPtr =
          reinterpret_cast<uint64_t>(
              runtimeOwnerRecord.runtimeCreatorModelDataPtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateHandlePtr =
          reinterpret_cast<uint64_t>(
              runtimeOwnerRecord.runtimeCreatorHandlePtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateCallerRva =
          runtimeOwnerRecord.runtimeCreatorCallerRva;
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerResolveCallerRva =
          runtimeOwnerRecord.runtimeResolveCallerRva;
    }

    if (summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr != 0u) {
      const auto contractAttachments =
          shadow::ShadowRuntimeContractCache::instance().snapshotAttachmentsShared();
      if (contractAttachments != nullptr) {
        shadow::ShadowAttachmentRigidRecord contractAttachment = {};
        const auto missOwnerSpritePtr = reinterpret_cast<void*>(
            static_cast<uintptr_t>(
                summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr));
        if (contractAttachments->findByChildSpritePtr(missOwnerSpritePtr,
                                                      contractAttachment)) {
          summary.lastAttachmentRigidMatchMissOwnerSpriteContractHit = 1u;
          summary
              .lastAttachmentRigidMatchMissOwnerSpriteContractChildRuntimeModelPtr =
              reinterpret_cast<uint64_t>(contractAttachment.childRuntimeModelPtr);
          summary
              .lastAttachmentRigidMatchMissOwnerSpriteContractOwnerRuntimeModelPtr =
              reinterpret_cast<uint64_t>(contractAttachment.ownerRuntimeModelPtr);
        }
      }
    }

    model::ShadowModelResourceRecord runtimeOwnerResource = {};
    if (model::ShadowModelResourceCache::instance().findRuntimeModelResource(
            missRuntimeOwnerPtr, runtimeOwnerResource)) {
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerModelResourcePtr =
          reinterpret_cast<uint64_t>(runtimeOwnerResource.modelResourcePtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerModelKey =
          runtimeOwnerResource.modelKey;
    }

    model::PoseRecord runtimeOwnerPose = {};
    if (model::PoseRegistry::instance().findByRuntimeModel(missRuntimeOwnerPtr,
                                                           runtimeOwnerPose)) {
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPoseMatrixCount =
          runtimeOwnerPose.matrixCount;
    }

    const uintptr_t missRuntimeOwnerValue =
        reinterpret_cast<uintptr_t>(missRuntimeOwnerPtr);
    constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
    if (missRuntimeOwnerValue >= 0x10000u) {
      void* plusA0Ptr = reinterpret_cast<void*>(
          missRuntimeOwnerValue + kCModelComplexExtensionOffset);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0Ptr =
          reinterpret_cast<uint64_t>(plusA0Ptr);
      model::ModelInstanceRecord plusA0Record = {};
      if (model::ModelInstanceRegistry::instance().findByRuntimeModel(
              plusA0Ptr, plusA0Record) ||
          model::ModelInstanceRegistry::instance().findOwnerByRuntimeModel(
              plusA0Ptr, plusA0Record)) {
        summary
            .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceObjectPtr =
            reinterpret_cast<uint64_t>(plusA0Record.sourceObjectPtr);
        summary
            .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceSpriteObjectPtr =
            reinterpret_cast<uint64_t>(plusA0Record.sourceSpriteObjectPtr);
      }
      model::PoseRecord plusA0Pose = {};
      if (model::PoseRegistry::instance().findByRuntimeModel(plusA0Ptr,
                                                             plusA0Pose)) {
        summary
            .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0PoseMatrixCount =
            plusA0Pose.matrixCount;
        summary
            .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0HasWorldTransform =
            (plusA0Pose.hasWorldTransform ||
             plusA0Pose.hasSpriteFrameTransform)
                ? 1u
                : 0u;
      }
    }
  }
  summary.semanticCoreAttachmentRigidPoseMissNoRecord =
      semanticCore.resolve.attachmentRigidPoseMissNoRecord;
  summary.semanticCoreAttachmentRigidPoseMissMissingRuntimes =
      semanticCore.resolve.attachmentRigidPoseMissMissingRuntimes;
  summary.semanticCoreAttachmentRigidPoseMissNoRootPose =
      semanticCore.resolve.attachmentRigidPoseMissNoRootPose;
  summary.semanticCoreAttachmentRigidPoseMissNoRootWorldTransform =
      semanticCore.resolve.attachmentRigidPoseMissNoRootWorldTransform;
  summary.semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromLivePose =
      semanticCore.resolve
          .attachmentRigidPoseRecoveredWorldTransformFromLivePose;
  summary
      .semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromShadowRegistry =
      semanticCore.resolve
          .attachmentRigidPoseRecoveredWorldTransformFromShadowRegistry;
  summary.semanticCoreExplicitBlendAttempts =
      semanticCore.resolve.explicitBlendAttempts;
  summary.semanticCoreExplicitBlendAttemptWithSpanRemapTable =
      semanticCore.resolve.explicitBlendAttemptWithSpanRemapTable;
  summary.semanticCoreExplicitBlendResolved =
      semanticCore.resolve.explicitBlendResolved;
  summary.semanticCoreExplicitBlendSpanRemapResolved =
      semanticCore.resolve.explicitBlendSpanRemapResolved;
  summary.semanticCoreExplicitBlendStrideSearchMiss =
      semanticCore.resolve.explicitBlendStrideSearchMiss;
  summary.semanticCoreExplicitBlendFinalDecodeMiss =
      semanticCore.resolve.explicitBlendFinalDecodeMiss;
  summary.semanticCoreCoreDrawPacketCount = semanticCore.coreDrawPacketCount;
  summary.semanticCoreUpperLayerResolvedItems =
      semanticCore.upperLayerResolvedItems;
  summary.semanticCoreSupplementalUpperLayerDrawPacketCount =
      semanticCore.supplementalUpperLayerDrawPacketCount;
  summary.semanticCoreDrawPacketCount = semanticCore.drawPacketCount;
  summary.semanticCoreSubmittedDrawCount = semanticCore.submittedDrawCount;
  {
    const auto lastFrame =
        shadow::ShadowValidationRuntime::instance().snapshotFrameShared();
    if (lastFrame != nullptr) {
      summary.semanticCoreLastFrameSourcePublishRevision =
          lastFrame->sourcePublishRevision;
      summary.semanticCoreLastFrameDrawCount = lastFrame->draws.size();
      summary.semanticCoreLastFrameSkinnedDrawCount =
          CountSkinnedSubmissionDraws(lastFrame.get());
      if (summary.semanticCoreSubmittedDrawCount == 0u &&
          summary.semanticSceneSubmittedSkinned != 0u) {
        // DXVK scene submission can reuse the last completed semantic frame and
        // refresh CModel palettes at submit time. In that mode the validation
        // runtime's submitted counter may be zero even though the scene path
        // emitted semantic skinned draws this frame, so expose the effective
        // reusable packet count rather than making the summary look idle.
        summary.semanticCoreSubmittedDrawCount =
            static_cast<uint64_t>(lastFrame->draws.size());
      }
    }

    const auto renderableFrame = shadow::ShadowValidationRuntime::instance()
                                     .snapshotRenderableFrameShared();
    if (renderableFrame != nullptr) {
      summary.semanticCoreRenderableFrameSourcePublishRevision =
          renderableFrame->sourcePublishRevision;
      summary.semanticCoreRenderableFrameDrawCount =
          renderableFrame->draws.size();
      summary.semanticCoreRenderableFrameSkinnedDrawCount =
          CountSkinnedSubmissionDraws(renderableFrame.get());
    }
  }
  summary.semanticCoreSkippedNoIdentity =
      semanticCore.resolve.skippedNoIdentity;
  summary.semanticCoreSkippedNoResolvedGeoset =
      semanticCore.resolve.skippedNoResolvedGeoset;
  summary.semanticCoreSkippedNoGeoset =
      semanticCore.resolve.skippedNoGeoset;
  summary.semanticCoreSkippedResourceMiss =
      semanticCore.resolve.skippedResourceMiss;
  summary.semanticCoreSkippedResourceNotReady =
      semanticCore.resolve.skippedResourceNotReady;
  summary.semanticCoreSkippedNoPose =
      semanticCore.resolve.skippedNoPose;
  summary.semanticCoreSkippedNoPoseNoContext =
      semanticCore.resolve.skippedNoPoseNoContext;
  summary.semanticCoreSkippedNoPoseAnonymousSubpart =
      semanticCore.resolve.skippedNoPoseAnonymousSubpart;
  summary.semanticCoreSkippedNoPoseLookupMiss =
      semanticCore.resolve.skippedNoPoseLookupMiss;
  summary.semanticCoreSkippedNoRuntimeGroupPalette =
      semanticCore.resolve.skippedNoRuntimeGroupPalette;
  summary.semanticCoreAttachmentRigidResolved =
      semanticCore.resolve.attachmentRigidResolved;
  summary.semanticCoreAttachmentRigidSupplementalAttachmentCount =
      semanticCore.resolve.attachmentRigidSupplementalAttachmentCount;
  summary.semanticCoreAttachmentRigidSupplementalResourceCandidateCount =
      semanticCore.resolve.attachmentRigidSupplementalResourceCandidateCount;
  summary.semanticCoreAttachmentRigidSupplementalResolvedCount =
      semanticCore.resolve.attachmentRigidSupplementalResolvedCount;
  summary.semanticCoreAttachmentRigidSupplementalResourceMissCount =
      semanticCore.resolve.attachmentRigidSupplementalResourceMissCount;
  summary.semanticCoreBuildDurationUs = semanticCore.buildDurationUs;
  summary.semanticCoreBuildInProgress = buildState.buildInProgress;
  summary.semanticCoreBuildRequestPending = buildState.buildRequestPending;
  summary.semanticCoreBuildFrameSerial = buildState.buildFrameSerial;
  summary.semanticCoreBuildPublishRevision = buildState.buildPublishRevision;
  summary.semanticCorePendingFrameSerial = buildState.pendingFrameSerial;
  summary.semanticCorePendingPublishRevision =
      buildState.pendingPublishRevision;
  summary.semanticCoreBuildCurrentRecordIndex =
      buildState.buildCurrentRecordIndex;
  summary.semanticCoreBuildRecordCount = buildState.buildRecordCount;
  summary.semanticCoreBuildChunkCount = buildState.buildChunkCount;
  summary.semanticCoreStalePendingBuildClearedCount =
      buildState.stalePendingBuildClearedCount;
  auto semanticPerfCalls = [](SemanticDataPerfTag tag) {
    return g_semanticPerfCalls[static_cast<size_t>(tag)].load(
        std::memory_order_relaxed);
  };
  auto semanticPerfUs = [](SemanticDataPerfTag tag) {
    return g_semanticPerfUs[static_cast<size_t>(tag)].load(
        std::memory_order_relaxed);
  };
  summary.semanticModelHookCalls =
      overrideSummary.runtimeModelCtorCount +
      overrideSummary.runtimeModelCreateCount +
      overrideSummary.runtimeModelResolveCount +
      overrideSummary.runtimeModelInitCopyCount +
      overrideSummary.runtimeChildLinkBuildCount +
      overrideSummary.spriteHostBindCount +
      overrideSummary.runtimeSourceObjectPublishCount;
  summary.semanticModelHookUs =
      semanticPerfUs(SemanticDataPerfTag::ModelHook);
  summary.semanticPoseHookCalls =
      overrideSummary.runtimeMatrixRangeCopyCount +
      overrideSummary.runtimeMatrixFlushCount +
      overrideSummary.primaryPresetWriteCount +
      overrideSummary.sharedPresetWriteCount +
      overrideSummary.localPointWriteCount +
      overrideSummary.spriteFramePoseBaseAliasPublishCount +
      overrideSummary.spriteFramePoseBaseAliasMatrixPaletteCount;
  summary.semanticPoseHookUs =
      semanticPerfUs(SemanticDataPerfTag::PoseHook);
  summary.semanticAttachmentHookCalls =
      overrideSummary.attachedEffectInitBindCount +
      overrideSummary.attachedEffectDirectBindCount +
      overrideSummary.attachModelToPointBindCount +
      overrideSummary.attachmentRigidPublishedWithSourceObjectCount +
      overrideSummary.spriteFrameAttachmentRootRuntimeHitCount +
      overrideSummary.spriteFrameAttachmentOwnerRuntimeHitCount +
      overrideSummary.spriteFrameAttachmentChildRuntimeHitCount;
  summary.semanticAttachmentHookUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentHook);
  summary.semanticFrameRegistryPublishCalls =
      semanticPerfCalls(SemanticDataPerfTag::FrameRegistryPublish);
  summary.semanticFrameRegistryPublishUs =
      semanticPerfUs(SemanticDataPerfTag::FrameRegistryPublish);
  summary.semanticContractCaptureCalls =
      semanticPerfCalls(SemanticDataPerfTag::ContractCapture);
  summary.semanticContractCaptureUs =
      semanticPerfUs(SemanticDataPerfTag::ContractCapture);
  summary.semanticConsumerBuildCalls =
      semanticPerfCalls(SemanticDataPerfTag::ConsumerBuild);
  summary.semanticConsumerBuildUs =
      semanticPerfUs(SemanticDataPerfTag::ConsumerBuild);
  summary.semanticConsumerBuildSkippedFresh =
      g_semanticConsumerBuildSkippedFresh.load(std::memory_order_relaxed);
  summary.semanticLastHotFunctionTag =
      g_semanticLastHotFunctionTag.load(std::memory_order_relaxed);
  summary.semanticLastHotFunctionUs =
      g_semanticLastHotFunctionUs.load(std::memory_order_relaxed);
  summary.semanticModelBuildChildPreScanCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelBuildChildPreScan);
  summary.semanticModelBuildChildPreScanUs =
      semanticPerfUs(SemanticDataPerfTag::ModelBuildChildPreScan);
  summary.semanticModelRuntimeChildCollectCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeChildCollect);
  summary.semanticModelRuntimeChildCollectUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeChildCollect);
  summary.semanticModelRuntimeChildBootstrapCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeChildBootstrap);
  summary.semanticModelRuntimeChildBootstrapUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeChildBootstrap);
  summary.semanticModelRuntimeChildParentMapCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeChildParentMap);
  summary.semanticModelRuntimeChildParentMapUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeChildParentMap);
  summary.semanticModelRuntimeChildOwnerPropagateCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeChildOwnerPropagate);
  summary.semanticModelRuntimeChildOwnerPropagateUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeChildOwnerPropagate);
  summary.semanticModelPromoteRuntimeCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelPromoteRuntime);
  summary.semanticModelPromoteRuntimeUs =
      semanticPerfUs(SemanticDataPerfTag::ModelPromoteRuntime);
  summary.semanticModelSpriteHostBindCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelSpriteHostBind);
  summary.semanticModelSpriteHostBindUs =
      semanticPerfUs(SemanticDataPerfTag::ModelSpriteHostBind);
  summary.semanticModelRuntimeModelBindingCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeModelBinding);
  summary.semanticModelRuntimeModelBindingUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeModelBinding);
  summary.semanticModelGeosetResourceCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelGeosetResource);
  summary.semanticModelGeosetResourceUs =
      semanticPerfUs(SemanticDataPerfTag::ModelGeosetResource);
  summary.semanticModelRuntimeCtorCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeCtor);
  summary.semanticModelRuntimeCtorUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeCtor);
  summary.semanticModelRuntimeResolveCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeResolve);
  summary.semanticModelRuntimeResolveUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeResolve);
  summary.semanticModelRuntimeInitCopyCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeInitCopy);
  summary.semanticModelRuntimeInitCopyUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeInitCopy);
  summary.semanticPoseRuntimePoseCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseRuntimePose);
  summary.semanticPoseRuntimePoseUs =
      semanticPerfUs(SemanticDataPerfTag::PoseRuntimePose);
  summary.semanticPoseRuntimePaletteTreeCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseRuntimePaletteTree);
  summary.semanticPoseRuntimePaletteTreeUs =
      semanticPerfUs(SemanticDataPerfTag::PoseRuntimePaletteTree);
  summary.semanticPoseRuntimeMatrixPaletteCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseRuntimeMatrixPalette);
  summary.semanticPoseRuntimeMatrixPaletteUs =
      semanticPerfUs(SemanticDataPerfTag::PoseRuntimeMatrixPalette);
  summary.semanticPoseSpriteFrameSourceIdentityCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteFrameSourceIdentity);
  summary.semanticPoseSpriteFrameSourceIdentityUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteFrameSourceIdentity);
  summary.semanticPoseSpriteFramePoseCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteFramePose);
  summary.semanticPoseSpriteFramePoseUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteFramePose);
  summary.semanticPoseRuntimeMatrixPublisherCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseRuntimeMatrixPublisher);
  summary.semanticPoseRuntimeMatrixPublisherUs =
      semanticPerfUs(SemanticDataPerfTag::PoseRuntimeMatrixPublisher);
  summary.semanticPoseSpriteAttachmentHitCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteAttachmentHit);
  summary.semanticPoseSpriteAttachmentHitUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteAttachmentHit);
  summary.semanticPoseSpriteTransformReadCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteTransformRead);
  summary.semanticPoseSpriteTransformReadUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteTransformRead);
  summary.semanticPoseSpriteIdentityLookupCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteIdentityLookup);
  summary.semanticPoseSpriteIdentityLookupUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteIdentityLookup);
  summary.semanticPoseSpriteBaseAliasCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteBaseAlias);
  summary.semanticPoseSpriteBaseAliasUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteBaseAlias);
  summary.semanticPoseSpritePublishPoseCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpritePublishPose);
  summary.semanticPoseSpritePublishPoseUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpritePublishPose);
  summary.semanticPoseSpritePaletteGateCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpritePaletteGate);
  summary.semanticPoseSpritePaletteGateUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpritePaletteGate);
  summary.semanticAttachmentAttachedEffectInitCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentAttachedEffectInit);
  summary.semanticAttachmentAttachedEffectInitUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentAttachedEffectInit);
  summary.semanticAttachmentAttachedEffectDirectCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentAttachedEffectDirect);
  summary.semanticAttachmentAttachedEffectDirectUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentAttachedEffectDirect);
  summary.semanticAttachmentAttachModelToPointCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentAttachModelToPoint);
  summary.semanticAttachmentAttachModelToPointUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentAttachModelToPoint);
  summary.semanticAttachmentOverrideSharedPresetCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentOverrideSharedPreset);
  summary.semanticAttachmentOverrideSharedPresetUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentOverrideSharedPreset);
  summary.semanticAttachmentOverrideLocalPointCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentOverrideLocalPoint);
  summary.semanticAttachmentOverrideLocalPointUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentOverrideLocalPoint);
  summary.semanticAttachmentOverridePrimaryPresetCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentOverridePrimaryPreset);
  summary.semanticAttachmentOverridePrimaryPresetUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentOverridePrimaryPreset);
  if (semanticSkipReason == SemanticBuildSkippedReason::None)
    shadow::NativeD3D9BackendRuntime::instance().buildLatestFrame();
  const auto nativeSummary =
      shadow::NativeD3D9BackendRuntime::instance().snapshot();
  summary.nativeD3D9BackendFrameSerial = nativeSummary.frameSerial;
  summary.nativeD3D9BackendSourcePublishRevision =
      nativeSummary.sourcePublishRevision;
  summary.nativeD3D9BackendSubmittedDrawCount =
      nativeSummary.submittedDrawCount;
  summary.nativeD3D9BackendSubmittedRigidDrawCount =
      nativeSummary.submittedRigidDrawCount;
  summary.nativeD3D9BackendSubmittedSkinnedDrawCount =
      nativeSummary.submittedSkinnedDrawCount;
  summary.nativeD3D9BackendExecutedFrameSerial =
      nativeSummary.executedFrameSerial;
  summary.nativeD3D9BackendExecutedDrawCount =
      nativeSummary.executedDrawCount;
  summary.nativeD3D9BackendExecutedRigidDrawCount =
      nativeSummary.executedRigidDrawCount;
  summary.nativeD3D9BackendExecutedSkinnedDrawCount =
      nativeSummary.executedSkinnedDrawCount;
  summary.nativeD3D9BackendExecuteAttemptCount =
      nativeSummary.executeAttemptCount;
  summary.nativeD3D9BackendExecuteSuccessCount =
      nativeSummary.executeSuccessCount;
  summary.nativeD3D9BackendLastSuccessfulExecutedFrameSerial =
      nativeSummary.lastSuccessfulExecutedFrameSerial;
  summary.nativeD3D9BackendLastSuccessfulExecutedDrawCount =
      nativeSummary.lastSuccessfulExecutedDrawCount;
  summary.nativeD3D9BackendExecuteSkippedNoDeviceCount =
      nativeSummary.executeSkippedNoDeviceCount;
  summary.nativeD3D9BackendExecuteSkippedNoDrawsCount =
      nativeSummary.executeSkippedNoDrawsCount;
  summary.nativeD3D9BackendLastExecuteSubmittedDrawCount =
      nativeSummary.lastExecuteSubmittedDrawCount;
  summary.nativeD3D9BackendLastExecuteFailedDrawCount =
      nativeSummary.lastExecuteFailedDrawCount;
  summary.nativeD3D9BackendLastExecuteSubmittedRigidDrawCount =
      nativeSummary.lastExecuteSubmittedRigidDrawCount;
  summary.nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount =
      nativeSummary.lastExecuteSubmittedSkinnedDrawCount;
  summary.nativeD3D9BackendLastExecuteExecutedRigidDrawCount =
      nativeSummary.lastExecuteExecutedRigidDrawCount;
  summary.nativeD3D9BackendLastExecuteExecutedSkinnedDrawCount =
      nativeSummary.lastExecuteExecutedSkinnedDrawCount;
    summary.nativeD3D9BackendGeometryCount = nativeSummary.geometryCount;
    summary.nativeD3D9BackendPaletteCount = nativeSummary.paletteCount;
    summary.nativeD3D9BackendMaterialCount = nativeSummary.materialCount;
    summary.nativeD3D9BackendHasDevice = nativeSummary.hasDevice;
    summary.nativeSemanticWorldStageCandidateCount =
        g_nativeSemanticWorldStageCandidateCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageCandidatePrepareCount =
        g_nativeSemanticWorldStageCandidatePrepareCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageCandidateRefreshCount =
        g_nativeSemanticWorldStageCandidateRefreshCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageCandidateExecuteCount =
        g_nativeSemanticWorldStageCandidateExecuteCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageSkippedRuntimeNotReadyCount =
        g_nativeSemanticWorldStageSkippedRuntimeNotReadyCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateStage =
        g_nativeSemanticWorldStageLastCandidateStage.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateA3 =
        g_nativeSemanticWorldStageLastCandidateA3.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateA4 =
        g_nativeSemanticWorldStageLastCandidateA4.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateA5 =
        g_nativeSemanticWorldStageLastCandidateA5.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateJassReady =
        g_nativeSemanticWorldStageLastCandidateJassReady.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateGameStarted =
        g_nativeSemanticWorldStageLastCandidateGameStarted.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateRuntimeFrame =
        g_nativeSemanticWorldStageLastCandidateRuntimeFrame.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStagePrepareAttemptCount =
        g_nativeSemanticWorldStagePrepareAttemptCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStagePrepareSuccessCount =
        g_nativeSemanticWorldStagePrepareSuccessCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageExecuteAttemptCount =
        g_nativeSemanticWorldStageExecuteAttemptCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageExecuteSuccessCount =
        g_nativeSemanticWorldStageExecuteSuccessCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastPrepareStage =
        g_nativeSemanticWorldStageLastPrepareStage.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastExecuteStage =
        g_nativeSemanticWorldStageLastExecuteStage.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastPrepareFrameSerial =
        g_nativeSemanticWorldStageLastPrepareFrameSerial.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastExecuteFrameSerial =
        g_nativeSemanticWorldStageLastExecuteFrameSerial.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastPrepareDrawCount =
        g_nativeSemanticWorldStageLastPrepareDrawCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastExecuteDrawCount =
        g_nativeSemanticWorldStageLastExecuteDrawCount.load(
            std::memory_order_relaxed);
    summary.runtimeChildLinkBuildCount =
        overrideSummary.runtimeChildLinkBuildCount;
  summary.runtimeChildLinkBuiltChildCount =
      overrideSummary.runtimeChildLinkBuiltChildCount;
  summary.runtimeChildBuildTimeDirectPublishCount =
      overrideSummary.runtimeChildBuildTimeDirectPublishCount;
  summary.runtimeChildBuildTimeDirectPublishWithResourceCount =
      overrideSummary.runtimeChildBuildTimeDirectPublishWithResourceCount;
  summary.runtimeChildBuildModelDataPreLinkCount =
      overrideSummary.runtimeChildBuildModelDataPreLinkCount;
  summary.runtimeChildBuildModelDataPostLinkCount =
      overrideSummary.runtimeChildBuildModelDataPostLinkCount;
  summary.runtimeChildBuildModelDataPreUnreadableLinkCount =
      overrideSummary.runtimeChildBuildModelDataPreUnreadableLinkCount;
  summary.runtimeChildBuildModelDataPostUnreadableLinkCount =
      overrideSummary.runtimeChildBuildModelDataPostUnreadableLinkCount;
  summary.runtimeMatrixRangeCopyCount =
      overrideSummary.runtimeMatrixRangeCopyCount;
  summary.runtimeMatrixFlushCount =
      overrideSummary.runtimeMatrixFlushCount;
  summary.runtimeMatrixPublisherPaletteReadyCount =
      overrideSummary.runtimeMatrixPublisherPaletteReadyCount;
  summary.runtimePoseUpdatePalettePublishCount =
      overrideSummary.runtimePoseUpdatePalettePublishCount;
  summary.runtimePoseUpdateLastRuntimeModelPtr =
      overrideSummary.runtimePoseUpdateLastRuntimeModelPtr;
  summary.runtimePoseUpdateLastMatrixCount =
      overrideSummary.runtimePoseUpdateLastMatrixCount;
  summary.runtimePoseUpdateLastMatrixHash =
      overrideSummary.runtimePoseUpdateLastMatrixHash;
  summary.runtimeMatrixWriteCount =
      overrideSummary.runtimeMatrixWriteCount;
  summary.runtimeMatrixWritePublishCount =
      overrideSummary.runtimeMatrixWritePublishCount;
  summary.runtimeMatrixWriteMissCount =
      overrideSummary.runtimeMatrixWriteMissCount;
  summary.runtimeMatrixWriteLastRuntimeModelPtr =
      overrideSummary.runtimeMatrixWriteLastRuntimeModelPtr;
  summary.runtimeMatrixWriteLastMatrixIndex =
      overrideSummary.runtimeMatrixWriteLastMatrixIndex;
  summary.runtimeMatrixWriteLastMatrixCount =
      overrideSummary.runtimeMatrixWriteLastMatrixCount;
  summary.runtimeMatrixWriteLastMatrixHash =
      overrideSummary.runtimeMatrixWriteLastMatrixHash;
  summary.runtimeMatrixRangeCopyPalettePublishHitCount =
      overrideSummary.runtimeMatrixRangeCopyPalettePublishHitCount;
  summary.runtimeMatrixRangeCopyPalettePublishMissCount =
      overrideSummary.runtimeMatrixRangeCopyPalettePublishMissCount;
  summary.runtimeMatrixRangeCopyPaletteFallbackCModelCount =
      overrideSummary.runtimeMatrixRangeCopyPaletteFallbackCModelCount;
  summary.runtimeMatrixFlushPaletteSuppressedCount =
      overrideSummary.runtimeMatrixFlushPaletteSuppressedCount;
  summary.runtimeMatrixRangeCopyLastRuntimeModelPtr =
      overrideSummary.runtimeMatrixRangeCopyLastRuntimeModelPtr;
  summary.runtimeMatrixRangeCopyLastContextPtr =
      overrideSummary.runtimeMatrixRangeCopyLastContextPtr;
  summary.runtimeMatrixRangeCopyLastSourceBasePtr =
      overrideSummary.runtimeMatrixRangeCopyLastSourceBasePtr;
  summary.runtimeMatrixRangeCopyLastMatrixCount =
      overrideSummary.runtimeMatrixRangeCopyLastMatrixCount;
  summary.runtimeMatrixRangeCopyLastMatrixHash =
      overrideSummary.runtimeMatrixRangeCopyLastMatrixHash;
  summary.runtimeMatrixPublisherAttachmentRootHitCount =
      overrideSummary.runtimeMatrixPublisherAttachmentRootHitCount;
  summary.runtimeMatrixPublisherAttachmentOwnerHitCount =
      overrideSummary.runtimeMatrixPublisherAttachmentOwnerHitCount;
  summary.runtimeMatrixPublisherAttachmentChildHitCount =
      overrideSummary.runtimeMatrixPublisherAttachmentChildHitCount;
  summary.runtimeMatrixPublisherAttachmentAliasHitCount =
      overrideSummary.runtimeMatrixPublisherAttachmentAliasHitCount;
  summary.runtimeMatrixPublisherAttachmentRootPaletteReadyCount =
      overrideSummary.runtimeMatrixPublisherAttachmentRootPaletteReadyCount;
  summary.runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount =
      overrideSummary.runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount;
  summary.runtimeMatrixPublisherAttachmentChildPaletteReadyCount =
      overrideSummary.runtimeMatrixPublisherAttachmentChildPaletteReadyCount;
  summary.attachmentAncestorIdentityHintWriteCount =
      overrideSummary.attachmentAncestorIdentityHintWriteCount;
  summary.sourceObjectRenderBridgeResolvedByEntryCount =
      overrideSummary.sourceObjectRenderBridgeResolvedByEntryCount;
  summary.sourceObjectRenderBridgeResolvedBySceneNodeCount =
      overrideSummary.sourceObjectRenderBridgeResolvedBySceneNodeCount;
  summary.spriteHostBindCount = overrideSummary.spriteHostBindCount;
  summary.spriteHostBindResolvedIdentityCount =
      overrideSummary.spriteHostBindResolvedIdentityCount;
  summary.spriteHostBindResolvedUnitCount =
      overrideSummary.spriteHostBindResolvedUnitCount;
  summary.spriteHostBindResolvedHandleCount =
      overrideSummary.spriteHostBindResolvedHandleCount;
  summary.spriteHostBindResolvedRawcodeCount =
      overrideSummary.spriteHostBindResolvedRawcodeCount;
  summary.spriteFrameSourceHintCount =
      overrideSummary.spriteFrameSourceHintCount;
  summary.spriteFrameSourceResolvedIdentityCount =
      overrideSummary.spriteFrameSourceResolvedIdentityCount;
  summary.spriteFrameSourceResolvedUnitCount =
      overrideSummary.spriteFrameSourceResolvedUnitCount;
  summary.spriteFrameSourceResolvedHandleCount =
      overrideSummary.spriteFrameSourceResolvedHandleCount;
  summary.spriteFrameSourceResolvedRawcodeCount =
      overrideSummary.spriteFrameSourceResolvedRawcodeCount;
  summary.spriteFrameSourceBaseAliasPublishCount =
      overrideSummary.spriteFrameSourceBaseAliasPublishCount;
  summary.spriteFrameSourceDeepIdentityResolvedCount =
      overrideSummary.spriteFrameSourceDeepIdentityResolvedCount;
  summary.spriteFrameSourceObjectRuntimeFieldCandidateCount =
      overrideSummary.spriteFrameSourceObjectRuntimeFieldCandidateCount;
  summary.spriteFrameSourceObjectRegistryFieldHitCount =
      overrideSummary.spriteFrameSourceObjectRegistryFieldHitCount;
  summary.spriteFramePoseBaseAliasPublishCount =
      overrideSummary.spriteFramePoseBaseAliasPublishCount;
  summary.spriteFramePoseBaseAliasMatrixPaletteCount =
      overrideSummary.spriteFramePoseBaseAliasMatrixPaletteCount;
  summary.spriteFrameAttachmentRootRuntimeHitCount =
      overrideSummary.spriteFrameAttachmentRootRuntimeHitCount;
  summary.spriteFrameAttachmentOwnerRuntimeHitCount =
      overrideSummary.spriteFrameAttachmentOwnerRuntimeHitCount;
  summary.spriteFrameAttachmentChildRuntimeHitCount =
      overrideSummary.spriteFrameAttachmentChildRuntimeHitCount;
  summary.spriteFrameAttachmentContextHintCount =
      overrideSummary.spriteFrameAttachmentContextHintCount;
  summary.spriteFrameAttachmentFullUpdateHitCount =
      overrideSummary.spriteFrameAttachmentFullUpdateHitCount;
  summary.spriteFrameAttachmentLiteUpdateHitCount =
      overrideSummary.spriteFrameAttachmentLiteUpdateHitCount;
  summary.spriteFrameAttachmentCallerKnownCount =
      overrideSummary.spriteFrameAttachmentCallerKnownCount;
  summary.spriteFrameAttachmentCallerChangedCount =
      overrideSummary.spriteFrameAttachmentCallerChangedCount;
  summary.spriteFrameAttachmentAttachScopeHitCount =
      overrideSummary.spriteFrameAttachmentAttachScopeHitCount;
  summary.spriteFrameAttachmentAttachScopeOwnerHitCount =
      overrideSummary.spriteFrameAttachmentAttachScopeOwnerHitCount;
  summary.spriteFrameAttachmentAttachScopeParentRuntimeMatchCount =
      overrideSummary.spriteFrameAttachmentAttachScopeParentRuntimeMatchCount;
  summary.attachedEffectInitBindCount =
      overrideSummary.attachedEffectInitBindCount;
  summary.attachedEffectInitResolvedIdentityCount =
      overrideSummary.attachedEffectInitResolvedIdentityCount;
  summary.attachedEffectInitResolvedUnitCount =
      overrideSummary.attachedEffectInitResolvedUnitCount;
  summary.attachedEffectInitResolvedHandleCount =
      overrideSummary.attachedEffectInitResolvedHandleCount;
  summary.attachedEffectInitResolvedRawcodeCount =
      overrideSummary.attachedEffectInitResolvedRawcodeCount;
  summary.attachedEffectInitParentRuntimeOwnerPublishCount =
      overrideSummary.attachedEffectInitParentRuntimeOwnerPublishCount;
  summary.attachedEffectDirectBindCount =
      overrideSummary.attachedEffectDirectBindCount;
  summary.attachedEffectDirectResolvedIdentityCount =
      overrideSummary.attachedEffectDirectResolvedIdentityCount;
  summary.attachedEffectDirectResolvedUnitCount =
      overrideSummary.attachedEffectDirectResolvedUnitCount;
  summary.attachedEffectDirectResolvedHandleCount =
      overrideSummary.attachedEffectDirectResolvedHandleCount;
  summary.attachedEffectDirectResolvedRawcodeCount =
      overrideSummary.attachedEffectDirectResolvedRawcodeCount;
  summary.attachModelToPointBindCount =
      overrideSummary.attachModelToPointBindCount;
  summary.attachModelToPointResolvedIdentityCount =
      overrideSummary.attachModelToPointResolvedIdentityCount;
  summary.attachModelToPointResolvedUnitCount =
      overrideSummary.attachModelToPointResolvedUnitCount;
  summary.attachModelToPointResolvedHandleCount =
      overrideSummary.attachModelToPointResolvedHandleCount;
  summary.attachModelToPointResolvedRawcodeCount =
      overrideSummary.attachModelToPointResolvedRawcodeCount;
  summary.attachModelToPointPromotedAttachmentChildRuntimeCount =
      overrideSummary.attachModelToPointPromotedAttachmentChildRuntimeCount;
  summary.attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount =
      overrideSummary
          .attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount;
  summary.currentRenderIdentityHintCount =
      overrideSummary.currentRenderIdentityHintCount;
  summary.currentRenderIdentityResolvedCount =
      overrideSummary.currentRenderIdentityResolvedCount;
  summary.sourceObjectIdentityHintResolvedCount =
      overrideSummary.sourceObjectIdentityHintResolvedCount;
  summary.runtimeSourceObjectPublishCount =
      overrideSummary.runtimeSourceObjectPublishCount;
  summary.runtimeModelResolveCount = overrideSummary.runtimeModelResolveCount;
  summary.runtimeModelResolveResolvedIdentityCount =
      overrideSummary.runtimeModelResolveResolvedIdentityCount;
  summary.attachmentRigidPublishedWithSourceObjectCount =
      overrideSummary.attachmentRigidPublishedWithSourceObjectCount;
  summary.attachmentRigidSourceObjectFromChildRuntimeCount =
      overrideSummary.attachmentRigidSourceObjectFromChildRuntimeCount;
  summary.attachmentRigidSourceObjectFromOwnerRuntimeCount =
      overrideSummary.attachmentRigidSourceObjectFromOwnerRuntimeCount;
  summary.attachmentRigidSourceObjectFromRootRuntimeCount =
      overrideSummary.attachmentRigidSourceObjectFromRootRuntimeCount;
  summary.overrideOutputSampleFrame = overrideSummary.sampleFrame;
  summary.overrideOutputLastActiveFrame = overrideSummary.lastActiveFrame;
  summary.overridePrimaryPresetWriteCount =
      overrideSummary.primaryPresetWriteCount;
  summary.overrideSharedPresetWriteCount =
      overrideSummary.sharedPresetWriteCount;
  summary.overrideLocalPointWriteCount =
      overrideSummary.localPointWriteCount;
  summary.overrideLocalPointNonZeroWriteCount =
      overrideSummary.localPointNonZeroWriteCount;
  summary.overrideLocalPointObservedChildLinkWriteCount =
      overrideSummary.localPointObservedChildLinkWriteCount;
  summary.overrideLocalPointMatchedChildLinkWriteCount =
      overrideSummary.localPointMatchedChildLinkWriteCount;
  summary.overrideLocalPointMatchedChildPaletteReadyWriteCount =
      overrideSummary.localPointMatchedChildPaletteReadyWriteCount;
  summary.overrideLocalPointMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary.localPointMatchedChildLinkBySourceRecordWriteCount;
  summary.overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount =
      overrideSummary.localPointMatchedChildPaletteReadyBySourceRecordWriteCount;
  summary.overrideLocalPointContextRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointContextRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointContextMatchedChildLinkWriteCount =
      overrideSummary.localPointContextMatchedChildLinkWriteCount;
  summary.overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary.localPointContextMatchedChildLinkBySourceRecordWriteCount;
  summary.overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount =
      overrideSummary
          .localPointContextMatchedChildPaletteReadyBySourceRecordWriteCount;
  summary.overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointScratchRootRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointScratchRootMatchedChildLinkWriteCount =
      overrideSummary.localPointScratchRootMatchedChildLinkWriteCount;
  summary.overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary
          .localPointScratchRootMatchedChildLinkBySourceRecordWriteCount;
  summary
      .overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount =
      overrideSummary
          .localPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount;
  summary.overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointArgBlockRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointArgBlockMatchedChildLinkWriteCount =
      overrideSummary.localPointArgBlockMatchedChildLinkWriteCount;
  summary.overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary
          .localPointArgBlockMatchedChildLinkBySourceRecordWriteCount;
  summary.overrideLocalPointArgBlockIdentityHintWriteCount =
      overrideSummary.localPointArgBlockIdentityHintWriteCount;
  summary.overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointArg4BlockRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointArg4BlockMatchedChildLinkWriteCount =
      overrideSummary.localPointArg4BlockMatchedChildLinkWriteCount;
  summary.overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary
          .localPointArg4BlockMatchedChildLinkBySourceRecordWriteCount;
  summary.overrideLocalPointArg4BlockIdentityHintWriteCount =
      overrideSummary.localPointArg4BlockIdentityHintWriteCount;
  summary.overrideLocalPointChildSourceMetaIdentityHintWriteCount =
      overrideSummary.localPointChildSourceMetaIdentityHintWriteCount;
  summary.overrideLocalPointSpriteBoundCandidateWriteCount =
      overrideSummary.localPointSpriteBoundCandidateWriteCount;
  summary.overrideLocalPointParentSpriteIdentityHintWriteCount =
      overrideSummary.localPointParentSpriteIdentityHintWriteCount;
  summary.overrideLocalPointRootRuntimeHitWriteCount =
      overrideSummary.localPointRootRuntimeHitWriteCount;
  summary.overrideLocalPointRootRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointRootRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointRootRuntimeMatchedChildLinkWriteCount =
      overrideSummary.localPointRootRuntimeMatchedChildLinkWriteCount;
  summary.overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount =
      overrideSummary.localPointRootRuntimeMatchedChildPaletteReadyWriteCount;
  summary.overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary
          .localPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount;
  summary
      .overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount =
      overrideSummary
          .localPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount;
  summary.attachmentRigidPublishedCount =
      overrideSummary.attachmentRigidPublishedCount;
  summary.overrideMaxPrimaryPresetSlotIndex =
      overrideSummary.maxPrimaryPresetSlotIndex;
  summary.overrideMaxSharedPresetSlotIndex =
      overrideSummary.maxSharedPresetSlotIndex;
  summary.overrideMaxLocalPointSlotIndex =
      overrideSummary.maxLocalPointSlotIndex;
  summary.overrideMaxObservedChildLinkCount =
      overrideSummary.maxObservedChildLinkCount;
  summary.overrideMaxObservedChildLinkTag =
      overrideSummary.maxObservedChildLinkTag;
  summary.overrideLastPrimaryPresetHash =
      overrideSummary.lastPrimaryPresetHash;
  summary.overrideLastSharedPresetHash =
      overrideSummary.lastSharedPresetHash;
  summary.overrideLastRuntimeModelPtr =
      overrideSummary.lastRuntimeModelPtr;
  summary.overrideLastMatchedChildRuntimeModelPtr =
      overrideSummary.lastMatchedChildRuntimeModelPtr;
  summary.overrideLastMatchedChildBySourceRecordRuntimeModelPtr =
      overrideSummary.lastMatchedChildBySourceRecordRuntimeModelPtr;
  summary.overrideLastContextRuntimeWithChildLinksPtr =
      overrideSummary.lastContextRuntimeWithChildLinksPtr;
  summary.overrideLastScratchRootPtr = overrideSummary.lastScratchRootPtr;
  summary.overrideLastScratchRootRuntimeModelPtr =
      overrideSummary.lastScratchRootRuntimeModelPtr;
  summary.overrideLastArgBlockPtr = overrideSummary.lastArgBlockPtr;
  summary.overrideLastArgBlockRuntimeModelPtr =
      overrideSummary.lastArgBlockRuntimeModelPtr;
  summary.overrideLastArgBlockIdentityHintPtr =
      overrideSummary.lastArgBlockIdentityHintPtr;
  summary.overrideLastArg4BlockPtr = overrideSummary.lastArg4BlockPtr;
  summary.overrideLastArg4BlockRuntimeModelPtr =
      overrideSummary.lastArg4BlockRuntimeModelPtr;
  summary.overrideLastArg4BlockIdentityHintPtr =
      overrideSummary.lastArg4BlockIdentityHintPtr;
  summary.overrideLastChildSourceMetaPtr =
      overrideSummary.lastChildSourceMetaPtr;
  summary.overrideLastChildSourceMetaRuntimeModelPtr =
      overrideSummary.lastChildSourceMetaRuntimeModelPtr;
  summary.overrideLastSpriteBoundCandidateSpritePtr =
      overrideSummary.lastSpriteBoundCandidateSpritePtr;
  summary.overrideLastSpriteBoundCandidateRuntimeModelPtr =
      overrideSummary.lastSpriteBoundCandidateRuntimeModelPtr;
  summary.overrideLastParentSpriteIdentityHintSpritePtr =
      overrideSummary.lastParentSpriteIdentityHintSpritePtr;
  summary.overrideLastParentSpriteIdentityHintRuntimeModelPtr =
      overrideSummary.lastParentSpriteIdentityHintRuntimeModelPtr;
  summary.overrideLastRootRuntimeModelPtr =
      overrideSummary.lastRootRuntimeModelPtr;
  summary.lastSourceObjectRenderBridgeSourceObjectPtr =
      overrideSummary.lastSourceObjectRenderBridgeSourceObjectPtr;
  summary.lastSourceObjectRenderBridgeSceneNodePtr =
      overrideSummary.lastSourceObjectRenderBridgeSceneNodePtr;
  summary.lastSourceObjectIdentityHintSourceObjectPtr =
      overrideSummary.lastSourceObjectIdentityHintSourceObjectPtr;
  summary.lastSourceObjectIdentityHintCandidatePtr =
      overrideSummary.lastSourceObjectIdentityHintCandidatePtr;
  summary.lastSpriteHostSourceObjectPtr =
      overrideSummary.lastSpriteHostSourceObjectPtr;
  summary.lastSpriteHostSpritePtr = overrideSummary.lastSpriteHostSpritePtr;
  summary.lastSpriteHostRuntimeModelPtr =
      overrideSummary.lastSpriteHostRuntimeModelPtr;
  summary.lastSpriteHostUnitPtr = overrideSummary.lastSpriteHostUnitPtr;
  summary.lastSpriteFrameSourceObjectPtr =
      overrideSummary.lastSpriteFrameSourceObjectPtr;
  summary.lastSpriteFrameSourceRuntimeModelPtr =
      overrideSummary.lastSpriteFrameSourceRuntimeModelPtr;
  summary.lastSpriteFrameSourceBaseRuntimeModelPtr =
      overrideSummary.lastSpriteFrameSourceBaseRuntimeModelPtr;
  summary.lastSpriteFrameSourceObjectVtablePtr =
      overrideSummary.lastSpriteFrameSourceObjectVtablePtr;
  summary.lastSpriteFrameSourceObjectSceneNodeCandidatePtr =
      overrideSummary.lastSpriteFrameSourceObjectSceneNodeCandidatePtr;
  summary.lastSpriteFrameSourceObjectSpriteCandidatePtr =
      overrideSummary.lastSpriteFrameSourceObjectSpriteCandidatePtr;
  summary.lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr =
      overrideSummary.lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr;
  summary.lastSpriteFrameSourceObjectRegistryFieldCandidatePtr =
      overrideSummary.lastSpriteFrameSourceObjectRegistryFieldCandidatePtr;
  summary.lastSpriteFrameSourceDeepIdentityCandidatePtr =
      overrideSummary.lastSpriteFrameSourceDeepIdentityCandidatePtr;
  summary.lastSpriteFrameSourceWorldObjectEntryPtr =
      overrideSummary.lastSpriteFrameSourceWorldObjectEntryPtr;
  summary.lastSpriteFrameSourceSceneNodePtr =
      overrideSummary.lastSpriteFrameSourceSceneNodePtr;
  summary.lastSpriteFrameSourceUnitPtr =
      overrideSummary.lastSpriteFrameSourceUnitPtr;
  summary.lastSpriteFramePoseBaseRuntimeModelPtr =
      overrideSummary.lastSpriteFramePoseBaseRuntimeModelPtr;
  summary.lastSpriteFramePoseBaseMatrixCount =
      overrideSummary.lastSpriteFramePoseBaseMatrixCount;
  summary.lastSpriteFrameAttachmentSpritePtr =
      overrideSummary.lastSpriteFrameAttachmentSpritePtr;
  summary.lastSpriteFrameAttachmentRuntimeModelPtr =
      overrideSummary.lastSpriteFrameAttachmentRuntimeModelPtr;
  summary.lastSpriteFrameAttachmentContextPtr =
      overrideSummary.lastSpriteFrameAttachmentContextPtr;
  summary.lastAttachedEffectInitOwnerWidgetPtr =
      overrideSummary.lastAttachedEffectInitOwnerWidgetPtr;
  summary.lastAttachedEffectInitChildSpritePtr =
      overrideSummary.lastAttachedEffectInitChildSpritePtr;
  summary.lastAttachedEffectInitChildRuntimeModelPtr =
      overrideSummary.lastAttachedEffectInitChildRuntimeModelPtr;
  summary.lastAttachedEffectInitUnitPtr =
      overrideSummary.lastAttachedEffectInitUnitPtr;
  summary.lastAttachedEffectDirectOwnerWidgetPtr =
      overrideSummary.lastAttachedEffectDirectOwnerWidgetPtr;
  summary.lastAttachedEffectDirectChildSpritePtr =
      overrideSummary.lastAttachedEffectDirectChildSpritePtr;
  summary.lastAttachedEffectDirectChildRuntimeModelPtr =
      overrideSummary.lastAttachedEffectDirectChildRuntimeModelPtr;
  summary.lastAttachedEffectDirectUnitPtr =
      overrideSummary.lastAttachedEffectDirectUnitPtr;
  summary.lastAttachModelToPointParentSpritePtr =
      overrideSummary.lastAttachModelToPointParentSpritePtr;
  summary.lastAttachModelToPointChildSpritePtr =
      overrideSummary.lastAttachModelToPointChildSpritePtr;
  summary.lastAttachModelToPointChildRuntimeModelPtr =
      overrideSummary.lastAttachModelToPointChildRuntimeModelPtr;
  summary.lastAttachModelToPointPromotedOwnerRuntimeModelPtr =
      overrideSummary.lastAttachModelToPointPromotedOwnerRuntimeModelPtr;
  summary.lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr =
      overrideSummary
          .lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr;
  summary.lastAttachModelToPointPromotedChildRuntimeModelPtr =
      overrideSummary.lastAttachModelToPointPromotedChildRuntimeModelPtr;
  summary.lastAttachModelToPointPromotedChildModelResourcePtr =
      overrideSummary.lastAttachModelToPointPromotedChildModelResourcePtr;
  summary.lastAttachModelToPointUnitPtr =
      overrideSummary.lastAttachModelToPointUnitPtr;
  summary.lastAttachScopeParentSpritePtr =
      overrideSummary.lastAttachScopeParentSpritePtr;
  summary.lastAttachScopeParentRuntimeModelPtr =
      overrideSummary.lastAttachScopeParentRuntimeModelPtr;
  summary.lastAttachScopeChildSpritePtr =
      overrideSummary.lastAttachScopeChildSpritePtr;
  summary.lastAttachScopeChildRuntimeModelPtr =
      overrideSummary.lastAttachScopeChildRuntimeModelPtr;
  summary.lastAttachScopeHitRuntimeModelPtr =
      overrideSummary.lastAttachScopeHitRuntimeModelPtr;
  summary.lastCurrentRenderIdentityWorldObjectEntryPtr =
      overrideSummary.lastCurrentRenderIdentityWorldObjectEntryPtr;
  summary.lastCurrentRenderIdentitySceneNodePtr =
      overrideSummary.lastCurrentRenderIdentitySceneNodePtr;
  summary.lastCurrentRenderIdentityUnitPtr =
      overrideSummary.lastCurrentRenderIdentityUnitPtr;
  summary.lastRuntimeSourceObjectPtr =
      overrideSummary.lastRuntimeSourceObjectPtr;
  summary.lastRuntimeSourceSpriteObjectPtr =
      overrideSummary.lastRuntimeSourceSpriteObjectPtr;
  summary.lastRuntimeSourceRuntimeModelPtr =
      overrideSummary.lastRuntimeSourceRuntimeModelPtr;
  summary.lastRuntimeModelResolveRuntimeModelPtr =
      overrideSummary.lastRuntimeModelResolveRuntimeModelPtr;
  summary.lastRuntimeModelResolveHandlePtr =
      overrideSummary.lastRuntimeModelResolveHandlePtr;
  summary.lastRuntimeModelCreateRuntimeModelPtr =
      overrideSummary.lastRuntimeModelCreateRuntimeModelPtr;
  summary.lastRuntimeModelCreateModelDataPtr =
      overrideSummary.lastRuntimeModelCreateModelDataPtr;
  summary.lastRuntimeModelInitRuntimeModelPtr =
      overrideSummary.lastRuntimeModelInitRuntimeModelPtr;
  summary.lastRuntimeModelInitModelDataPtr =
      overrideSummary.lastRuntimeModelInitModelDataPtr;
  summary.lastAttachmentRigidSourceObjectPtr =
      overrideSummary.lastAttachmentRigidSourceObjectPtr;
  summary.lastAttachmentRigidSourceSpriteObjectPtr =
      overrideSummary.lastAttachmentRigidSourceSpriteObjectPtr;
  summary.lastRuntimeChildLinkBuildParentRuntimeModelPtr =
      overrideSummary.lastRuntimeChildLinkBuildParentRuntimeModelPtr;
  summary.lastRuntimeChildLinkBuildChildRuntimeModelPtr =
      overrideSummary.lastRuntimeChildLinkBuildChildRuntimeModelPtr;
  summary.lastRuntimeChildLinkBuildModelDataPtr =
      overrideSummary.lastRuntimeChildLinkBuildModelDataPtr;
  summary.lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr;
  summary.lastRuntimeChildBuildTimeDirectParentModelDataPtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectParentModelDataPtr;
  summary.lastRuntimeChildBuildTimeDirectRuntimeModelPtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectRuntimeModelPtr;
  summary.lastRuntimeChildBuildTimeDirectModelDataPtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectModelDataPtr;
  summary.lastRuntimeChildBuildTimeDirectModelResourcePtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectModelResourcePtr;
  summary.lastRuntimeChildBuildModelDataParentRuntimeModelPtr =
      overrideSummary.lastRuntimeChildBuildModelDataParentRuntimeModelPtr;
  summary.lastRuntimeChildBuildModelDataPtr =
      overrideSummary.lastRuntimeChildBuildModelDataPtr;
  summary.lastRuntimeChildBuildModelDataGroupRecordsPtr =
      overrideSummary.lastRuntimeChildBuildModelDataGroupRecordsPtr;
  summary.lastRuntimeChildBuildModelDataHeadPtr =
      overrideSummary.lastRuntimeChildBuildModelDataHeadPtr;
  summary.lastRuntimeChildBuildModelDataLinkNodePtr =
      overrideSummary.lastRuntimeChildBuildModelDataLinkNodePtr;
  summary.lastRuntimeChildBuildModelDataChildModelDataPtr =
      overrideSummary.lastRuntimeChildBuildModelDataChildModelDataPtr;
  summary.lastRuntimeChildBuildModelDataChildModelResourcePtr =
      overrideSummary.lastRuntimeChildBuildModelDataChildModelResourcePtr;
  summary.lastRuntimeMatrixPublisherRuntimeModelPtr =
      overrideSummary.lastRuntimeMatrixPublisherRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherMatchedRuntimeModelPtr =
      overrideSummary.lastRuntimeMatrixPublisherMatchedRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherMatrixCount =
      overrideSummary.lastRuntimeMatrixPublisherMatrixCount;
  summary.lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr =
      overrideSummary.lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount =
      overrideSummary.lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount;
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount =
      overrideSummary.lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount;
  summary.lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount =
      overrideSummary.lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount;
  summary.lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr;
  summary.lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr =
      overrideSummary
          .lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr;
  summary.lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr;
  summary.lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr =
      overrideSummary
          .lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr;
  summary.lastAttachmentChildLineageBootstrapParentRuntimeModelPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapParentRuntimeModelPtr;
  summary.lastAttachmentChildLineageBootstrapChildRuntimeModelPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapChildRuntimeModelPtr;
  summary.lastAttachmentChildLineageBootstrapParentModelDataPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapParentModelDataPtr;
  summary.lastAttachmentChildLineageBootstrapChildModelDataPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapChildModelDataPtr;
  summary.lastAttachmentChildLineageBootstrapChildModelResourcePtr =
      overrideSummary.lastAttachmentChildLineageBootstrapChildModelResourcePtr;
  summary.lastAttachmentAncestorFromRuntimeModelPtr =
      overrideSummary.lastAttachmentAncestorFromRuntimeModelPtr;
  summary.lastAttachmentAncestorRuntimeModelPtr =
      overrideSummary.lastAttachmentAncestorRuntimeModelPtr;
  summary.overrideLastLocalPointSlotIndex =
      overrideSummary.lastLocalPointSlotIndex;
  summary.overrideLastLocalPointSourceRecordIndex =
      overrideSummary.lastLocalPointSourceRecordIndex;
  summary.overrideLastObservedChildLinkCount =
      overrideSummary.lastObservedChildLinkCount;
  summary.overrideLastMatchedChildLinkCount =
      overrideSummary.lastMatchedChildLinkCount;
  summary.overrideLastMatchedChildMatrixCount =
      overrideSummary.lastMatchedChildMatrixCount;
  summary.overrideLastMatchedChildBySourceRecordLinkCount =
      overrideSummary.lastMatchedChildBySourceRecordLinkCount;
  summary.overrideLastMatchedChildBySourceRecordMatrixCount =
      overrideSummary.lastMatchedChildBySourceRecordMatrixCount;
  summary.overrideLastContextRuntimeWithChildLinksOffset =
      overrideSummary.lastContextRuntimeWithChildLinksOffset;
  summary.overrideLastContextRuntimeWithChildLinksCount =
      overrideSummary.lastContextRuntimeWithChildLinksCount;
  summary.overrideLastContextRuntimeWithChildLinksMaxTag =
      overrideSummary.lastContextRuntimeWithChildLinksMaxTag;
  summary.overrideLastScratchRootRuntimeChildLinkCount =
      overrideSummary.lastScratchRootRuntimeChildLinkCount;
  summary.overrideLastScratchRootRuntimeMaxTag =
      overrideSummary.lastScratchRootRuntimeMaxTag;
  summary.overrideLastArgBlockRuntimeOffset =
      overrideSummary.lastArgBlockRuntimeOffset;
  summary.overrideLastArgBlockRuntimeChildLinkCount =
      overrideSummary.lastArgBlockRuntimeChildLinkCount;
  summary.overrideLastArgBlockRuntimeMaxTag =
      overrideSummary.lastArgBlockRuntimeMaxTag;
  summary.overrideLastArgBlockIdentityHintOffset =
      overrideSummary.lastArgBlockIdentityHintOffset;
  summary.overrideLastArg4BlockRuntimeOffset =
      overrideSummary.lastArg4BlockRuntimeOffset;
  summary.overrideLastArg4BlockRuntimeChildLinkCount =
      overrideSummary.lastArg4BlockRuntimeChildLinkCount;
  summary.overrideLastArg4BlockRuntimeMaxTag =
      overrideSummary.lastArg4BlockRuntimeMaxTag;
  summary.overrideLastArg4BlockIdentityHintOffset =
      overrideSummary.lastArg4BlockIdentityHintOffset;
  summary.overrideLastRootRuntimeChildLinkCount =
      overrideSummary.lastRootRuntimeChildLinkCount;
  summary.overrideLastRootRuntimeMaxTag =
      overrideSummary.lastRootRuntimeMaxTag;
  summary.lastSpriteHostJHandle = overrideSummary.lastSpriteHostJHandle;
  summary.lastSpriteHostRawcode = overrideSummary.lastSpriteHostRawcode;
  summary.lastSpriteFrameSourceJHandle =
      overrideSummary.lastSpriteFrameSourceJHandle;
  summary.lastSpriteFrameSourceRawcode =
      overrideSummary.lastSpriteFrameSourceRawcode;
  summary.lastSpriteFrameSourceObjectRuntimeFieldOffset =
      overrideSummary.lastSpriteFrameSourceObjectRuntimeFieldOffset;
  summary.lastSpriteFrameSourceObjectRegistryFieldOffset =
      overrideSummary.lastSpriteFrameSourceObjectRegistryFieldOffset;
  summary.lastSpriteFrameSourceDeepIdentityOffset =
      overrideSummary.lastSpriteFrameSourceDeepIdentityOffset;
  summary.lastSpriteFrameAttachmentRoleMask =
      overrideSummary.lastSpriteFrameAttachmentRoleMask;
  summary.lastSpriteFrameAttachmentUpdateKind =
      overrideSummary.lastSpriteFrameAttachmentUpdateKind;
  summary.lastSpriteFrameAttachmentCallerRva =
      overrideSummary.lastSpriteFrameAttachmentCallerRva;
  summary.lastSourceObjectIdentityHintOffset =
      overrideSummary.lastSourceObjectIdentityHintOffset;
  summary.lastAttachedEffectInitJHandle =
      overrideSummary.lastAttachedEffectInitJHandle;
  summary.lastAttachedEffectInitRawcode =
      overrideSummary.lastAttachedEffectInitRawcode;
  summary.lastAttachedEffectDirectJHandle =
      overrideSummary.lastAttachedEffectDirectJHandle;
  summary.lastAttachedEffectDirectRawcode =
      overrideSummary.lastAttachedEffectDirectRawcode;
  summary.lastAttachModelToPointJHandle =
      overrideSummary.lastAttachModelToPointJHandle;
  summary.lastAttachModelToPointRawcode =
      overrideSummary.lastAttachModelToPointRawcode;
  summary.lastAttachModelToPointAttachPointIndex =
      overrideSummary.lastAttachModelToPointAttachPointIndex;
  summary.lastAttachScopeCallerRva =
      overrideSummary.lastAttachScopeCallerRva;
  summary.lastAttachScopeHitRoleMask =
      overrideSummary.lastAttachScopeHitRoleMask;
  summary.lastAttachedEffectInitParentRuntimeModelPtr =
      overrideSummary.lastAttachedEffectInitParentRuntimeModelPtr;
  summary.lastRuntimeModelCtorRuntimeModelPtr =
      overrideSummary.lastRuntimeModelCtorRuntimeModelPtr;
  summary.lastRuntimeModelCtorCallerRva =
      overrideSummary.lastRuntimeModelCtorCallerRva;
  summary.lastRuntimeModelCtorKind =
      overrideSummary.lastRuntimeModelCtorKind;
  summary.lastRuntimeModelResolveCallerRva =
      overrideSummary.lastRuntimeModelResolveCallerRva;
  summary.lastRuntimeModelCreateCallerRva =
      overrideSummary.lastRuntimeModelCreateCallerRva;
  summary.lastRuntimeModelInitCallerRva =
      overrideSummary.lastRuntimeModelInitCallerRva;
  summary.lastRuntimeChildLinkBuildSourceMeta =
      overrideSummary.lastRuntimeChildLinkBuildSourceMeta;
  summary.lastRuntimeChildBuildModelDataPhase =
      overrideSummary.lastRuntimeChildBuildModelDataPhase;
  summary.lastRuntimeChildBuildModelDataGroupCount =
      overrideSummary.lastRuntimeChildBuildModelDataGroupCount;
  summary.lastRuntimeChildBuildModelDataLinkCount =
      overrideSummary.lastRuntimeChildBuildModelDataLinkCount;
  summary.lastRuntimeChildBuildModelDataUnreadableLinkCount =
      overrideSummary.lastRuntimeChildBuildModelDataUnreadableLinkCount;
  summary.lastRuntimeChildBuildModelDataSourceMeta =
      overrideSummary.lastRuntimeChildBuildModelDataSourceMeta;
  summary.lastRuntimeMatrixPublisherKind =
      overrideSummary.lastRuntimeMatrixPublisherKind;
  summary.lastRuntimeMatrixPublisherRoleMask =
      overrideSummary.lastRuntimeMatrixPublisherRoleMask;
  summary.lastAttachmentChildLineageBootstrapSourceMeta =
      overrideSummary.lastAttachmentChildLineageBootstrapSourceMeta;
  summary.lastAttachmentChildLineageBootstrapBucketIndex =
      overrideSummary.lastAttachmentChildLineageBootstrapBucketIndex;
  summary.lastAttachmentChildLineageBootstrapModelDataLinkCount =
      overrideSummary.lastAttachmentChildLineageBootstrapModelDataLinkCount;
  summary.lastAttachmentChildLineageBootstrapRuntimeLinkCount =
      overrideSummary.lastAttachmentChildLineageBootstrapRuntimeLinkCount;
  summary.lastAttachmentChildLineageBootstrapStrictCandidateCount =
      overrideSummary.lastAttachmentChildLineageBootstrapStrictCandidateCount;
  summary.lastAttachmentChildLineageBootstrapSourceCandidateCount =
      overrideSummary.lastAttachmentChildLineageBootstrapSourceCandidateCount;
  summary.lastAttachmentChildLineageBootstrapBucketCandidateCount =
      overrideSummary.lastAttachmentChildLineageBootstrapBucketCandidateCount;
  summary.lastAttachmentChildLineageBootstrapAllCandidateCount =
      overrideSummary.lastAttachmentChildLineageBootstrapAllCandidateCount;
  summary.lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal =
      overrideSummary.lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal;
  summary.lastAttachmentChildLineageBootstrapModelDataBucketCount =
      overrideSummary.lastAttachmentChildLineageBootstrapModelDataBucketCount;
  summary.lastAttachmentAncestorDepth =
      overrideSummary.lastAttachmentAncestorDepth;
  summary.overrideLastLocalPointX = overrideSummary.lastLocalPointX;
  summary.overrideLastLocalPointY = overrideSummary.lastLocalPointY;
  summary.overrideLastLocalPointZ = overrideSummary.lastLocalPointZ;
  summary.poseFrame = summary.runtimePoseHooksActive
                          ? model::PoseRegistry::instance().frameNumber()
                          : model::ModelInstanceRegistry::instance().frameNumber();

  summary.runtimeChainWarm =
      summary.modelRegistryCount >= 16 &&
      summary.runtimeBoundCount >= 16 &&
      summary.completeIdentityCount + 4 >= summary.runtimeBoundCount &&
      (!summary.runtimePoseHooksActive ||
       (summary.poseReadyCount >= 16 &&
        (summary.spriteFramePoseCount + summary.matrixPaletteCount) >= 8));

  const uint64_t identityGap =
      summary.runtimeBoundCount > summary.completeIdentityCount
          ? (summary.runtimeBoundCount - summary.completeIdentityCount)
          : 0u;
  const uint64_t runtimeBindGap =
      summary.modelRegistryCount > summary.runtimeBoundCount
          ? (summary.modelRegistryCount - summary.runtimeBoundCount)
          : 0u;
  const uint64_t poseGap =
      summary.runtimeBoundCount > summary.poseReadyCount
          ? (summary.runtimeBoundCount - summary.poseReadyCount)
          : 0u;

  const bool identityNeedsRepair =
      summary.runtimeBoundCount >= 24u && identityGap > 12u &&
      identityGap * 4u > summary.runtimeBoundCount;
  const bool runtimeBindNeedsRepair =
      summary.modelRegistryCount >= 32u && runtimeBindGap > 8u &&
      runtimeBindGap * 2u > summary.modelRegistryCount;
  const bool poseNeedsRepair =
      summary.runtimePoseHooksActive && summary.runtimeBoundCount >= 24u &&
      ((poseGap > 12u && poseGap * 4u > summary.runtimeBoundCount) ||
       (summary.spriteFramePoseCount + summary.matrixPaletteCount) == 0u);

  summary.runtimeChainNeedsRepair =
      identityNeedsRepair || runtimeBindNeedsRepair || poseNeedsRepair;
  const bool nativeTakeoverWarm =
      summary.runtimeChainWarm &&
      summary.semanticCoreFrameFresh &&
      summary.nativeD3D9BackendHasDevice &&
      summary.nativeD3D9BackendSubmittedDrawCount != 0u;
  if (nativeTakeoverWarm) {
    dxvk::War3Hook::MaybeInstallNativeRendererTakeover(
        "runtime-bridge-warm");
  }
  return summary;
}

ShadowRuntimeBridgeTrackingDecision ComputeShadowRuntimeBridgeTracking() {
  ShadowRuntimeBridgeTrackingDecision decision = {};
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return decision;
  const auto summary = QueryShadowRuntimeBridgeSummary();
  const bool semanticSceneOwnsUnits =
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
      dxvk::war3::internal::
          IsSemanticSceneBypassLegacyUnitCaptureRuntimeEnabled() &&
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly;
  const uint64_t refreshPeriod = summary.runtimePoseHooksActive ? 240u : 300u;
  uint64_t warmupFrames = summary.runtimePoseHooksActive ? 60u : 24u;
  if (semanticSceneOwnsUnits) {
    // 语义 scene 已经能直接消费 runtimeModel + pose palette 时，
    // 不再需要在低 FPS 阶段坚持几十帧的“全量 identity warmup”。
    // 否则 poseFrame 会长时间卡在 warmup 区间，导致每帧重复 CollectWorldObjects，
    // 形成 FPS 越低、warmup 越走不完的恶性循环。
    warmupFrames = summary.runtimeChainWarm ? 4u : 8u;
  }
  const bool repairBurstActive =
      summary.poseFrame != 0u &&
      summary.poseFrame <=
          g_shadowBridgeRepairUntilFrame.load(std::memory_order_relaxed);
  const bool shouldRefreshIdentity =
      summary.poseFrame < warmupFrames ||
      ((summary.poseFrame % refreshPeriod) == 0u &&
       summary.poseFrame >= warmupFrames) ||
      repairBurstActive ||
      summary.runtimeChainNeedsRepair;

  decision.wantsObjectIdentity =
      shouldRefreshIdentity || !summary.runtimeChainWarm;
  decision.wantsFallbackBridge =
      !summary.runtimeChainWarm || summary.poseFrame < warmupFrames ||
      repairBurstActive ||
      summary.runtimeChainNeedsRepair;

  if (semanticSceneOwnsUnits && summary.runtimeChainWarm &&
      !repairBurstActive && !summary.runtimeChainNeedsRepair) {
    // units-first semantic scene 已接管后，legacy fallback 只保留修复/诊断角色。
    decision.wantsFallbackBridge = false;
  }
  return decision;
}

void ResetShadowRuntimeBridgeState() {
  g_shadowBridgeRepairUntilFrame.store(0u, std::memory_order_relaxed);
  g_shadowBridgeRepairCooldownUntilFrame.store(0u,
                                               std::memory_order_relaxed);
  g_nativeSemanticWorldStageCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageCandidatePrepareCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageCandidateRefreshCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageCandidateExecuteCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageSkippedRuntimeNotReadyCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateStage.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA3.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA4.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA5.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateJassReady.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateGameStarted.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateRuntimeFrame.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStagePrepareAttemptCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStagePrepareSuccessCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageExecuteAttemptCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageExecuteSuccessCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareStage.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteStage.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareFrameSerial.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteFrameSerial.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareDrawCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteDrawCount.store(
      0u, std::memory_order_relaxed);
  g_semanticSummaryRefreshFrameSerial.store(0u, std::memory_order_relaxed);
  g_semanticSummaryRefreshPublishRevision.store(0u,
                                                std::memory_order_relaxed);
  shadow::NativeD3D9BackendRuntime::instance().reset();
}

bool AugmentShadowSemanticContext(dxvk::War3ShadowSemanticContext& semantic,
                                  const RenderObjectInfo* currentObj) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return false;
  const void* currentUnitPtr = currentObj != nullptr ? currentObj->unitPtr : nullptr;
  uint64_t bestPoseFrame = 0;
  uint64_t bestMatrixFrame = 0;

  const bool runtimePoseHooksActive = model::IsPoseHookEnabled();
  const bool needsRuntimePoseAugment =
      runtimePoseHooksActive &&
      (currentUnitPtr != nullptr ||
       semantic.objectKind == ObjectKind::Unit ||
       (currentObj != nullptr && currentObj->kind == ObjectKind::Unit) ||
       semantic.runtimeModelPtr != nullptr);
  const bool needsStableIdentityAugment =
      semantic.object == nullptr || semantic.sceneNode == nullptr ||
      semantic.worldObjectEntry == nullptr || semantic.jHandle == 0u ||
      semantic.rawcode == 0u || semantic.objectKind == ObjectKind::Unknown;
  const bool needsRuntimeRecovery =
      needsStableIdentityAugment &&
      (semantic.sceneNode != nullptr || semantic.worldObjectEntry != nullptr ||
       semantic.jHandle != 0u || currentUnitPtr != nullptr);
  const bool allowRuntimeRegistryLookup =
      needsRuntimePoseAugment || needsStableIdentityAugment ||
      needsRuntimeRecovery;

  if (allowRuntimeRegistryLookup) {
    model::ModelInstanceRecord instanceRecord = {};
    bool instanceRecordHit = false;
    auto& instanceRegistry = model::ModelInstanceRegistry::instance();
    if (!instanceRecordHit && semantic.worldObjectEntry != nullptr)
      instanceRecordHit = instanceRegistry.findByWorldObjectEntry(
          semantic.worldObjectEntry, instanceRecord);
    if (!instanceRecordHit && semantic.sceneNode != nullptr)
      instanceRecordHit =
          instanceRegistry.findBySceneNode(semantic.sceneNode, instanceRecord);
    if (!instanceRecordHit && semantic.object != nullptr &&
        semantic.object->unitPtr != nullptr) {
      instanceRecordHit =
          instanceRegistry.findByUnitPtr(semantic.object->unitPtr, instanceRecord);
    }
    if (!instanceRecordHit && currentUnitPtr != nullptr)
      instanceRecordHit = instanceRegistry.findByUnitPtr(
          const_cast<void*>(currentUnitPtr), instanceRecord);
    if (!instanceRecordHit && semantic.jHandle != 0u)
      instanceRecordHit =
          instanceRegistry.findByHandle(semantic.jHandle, instanceRecord);

    if (instanceRecordHit) {
      if (semantic.sceneNode == nullptr)
        semantic.sceneNode = instanceRecord.sceneNode;
      if (semantic.worldObjectEntry == nullptr)
        semantic.worldObjectEntry = instanceRecord.worldObjectEntry;
      if (semantic.runtimeModelPtr == nullptr)
        semantic.runtimeModelPtr = instanceRecord.runtimeModelPtr;
      if (semantic.modelResourcePtr == nullptr)
        semantic.modelResourcePtr = instanceRecord.modelResourcePtr;
      if (semantic.modelKey == 0u)
        semantic.modelKey = instanceRecord.modelKey;
      if (semantic.jHandle == 0u)
        semantic.jHandle = instanceRecord.jHandle;
      if (semantic.rawcode == 0u)
        semantic.rawcode = instanceRecord.rawcode;
    }

    const bool needsShadowRegistryRecovery =
        needsRuntimePoseAugment || semantic.sceneNode == nullptr ||
        semantic.worldObjectEntry == nullptr || semantic.jHandle == 0u ||
        semantic.rawcode == 0u || semantic.objectKind == ObjectKind::Unknown;
    if (needsShadowRegistryRecovery) {
      ShadowObjectRecord shadowRecord = {};
      bool shadowRecordHit = false;
      auto& shadowRegistry = ShadowObjectRegistry::instance();
      const uint64_t shadowRegistryFrame = shadowRegistry.frameNumber();
      if (!shadowRecordHit && semantic.worldObjectEntry != nullptr)
        shadowRecordHit = shadowRegistry.findByWorldObjectEntry(
            semantic.worldObjectEntry, shadowRecord);
      if (!shadowRecordHit && semantic.sceneNode != nullptr)
        shadowRecordHit =
            shadowRegistry.findBySceneNode(semantic.sceneNode, shadowRecord);
      if (!shadowRecordHit && semantic.object != nullptr &&
          semantic.object->unitPtr != nullptr) {
        shadowRecordHit =
            shadowRegistry.findByUnitPtr(semantic.object->unitPtr, shadowRecord);
      }
      if (!shadowRecordHit && currentObj != nullptr && currentObj->unitPtr != nullptr)
        shadowRecordHit =
            shadowRegistry.findByUnitPtr(currentObj->unitPtr, shadowRecord);
      if (!shadowRecordHit && semantic.jHandle != 0u)
        shadowRecordHit =
            shadowRegistry.findByHandle(semantic.jHandle, shadowRecord);
      if (!shadowRecordHit && semantic.runtimeModelPtr != nullptr)
        shadowRecordHit =
            shadowRegistry.findByRuntimeModel(semantic.runtimeModelPtr, shadowRecord);

      if (shadowRecordHit) {
        if (semantic.sceneNode == nullptr)
          semantic.sceneNode = shadowRecord.sceneNode;
        if (semantic.worldObjectEntry == nullptr)
          semantic.worldObjectEntry = shadowRecord.worldObjectEntry;
        if (semantic.runtimeModelPtr == nullptr)
          semantic.runtimeModelPtr = shadowRecord.runtimeModelPtr;
        if (semantic.modelResourcePtr == nullptr)
          semantic.modelResourcePtr = shadowRecord.modelResourcePtr;
        if (semantic.jHandle == 0u)
          semantic.jHandle = shadowRecord.jHandle;
        if (semantic.rawcode == 0u)
          semantic.rawcode = shadowRecord.rawcode;
        if (semantic.modelKey == 0u)
          semantic.modelKey = shadowRecord.modelKey;
        if (semantic.objectKind == ObjectKind::Unknown)
          semantic.objectKind = shadowRecord.kind;
        if (IsRegistryFrameFresh(shadowRecord.lastSpriteFramePoseFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, shadowRecord.hasSpriteFrameTransform,
                                 shadowRecord.spriteFrameTransform,
                                 shadowRecord.scale, shadowRecord.height, true,
                                 shadowRecord.lastSpriteFramePoseFrame,
                                 bestPoseFrame);
        if (IsRegistryFrameFresh(shadowRecord.lastRootPoseFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, shadowRecord.hasWorldTransform,
                                 shadowRecord.worldTransform,
                                 shadowRecord.scale, shadowRecord.height, false,
                                 shadowRecord.lastRootPoseFrame, bestPoseFrame);
        if (IsRegistryFrameFresh(shadowRecord.lastMatrixPaletteFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseMatrices(semantic, shadowRecord.matrixCount,
                                 shadowRecord.matrixHash,
                                 shadowRecord.lastMatrixPaletteFrame,
                                 bestMatrixFrame);
      }
    }

    if (needsRuntimePoseAugment) {
      model::PoseRecord poseRecord = {};
      bool poseRecordHit = false;
      auto& poseRegistry = model::PoseRegistry::instance();
      const uint64_t poseRegistryFrame = poseRegistry.frameNumber();
      if (semantic.runtimeModelPtr != nullptr)
        poseRecordHit =
            poseRegistry.findByRuntimeModel(semantic.runtimeModelPtr, poseRecord);
      if (!poseRecordHit && semantic.sceneNode != nullptr)
        poseRecordHit = poseRegistry.findBySceneNode(semantic.sceneNode, poseRecord);
      if (!poseRecordHit && semantic.object != nullptr &&
          semantic.object->unitPtr != nullptr) {
        poseRecordHit =
            poseRegistry.findByUnitPtr(semantic.object->unitPtr, poseRecord);
      }
      if (!poseRecordHit && currentUnitPtr != nullptr)
        poseRecordHit = poseRegistry.findByUnitPtr(
            const_cast<void*>(currentUnitPtr), poseRecord);

      if (poseRecordHit) {
        if (IsRegistryFrameFresh(poseRecord.lastSpriteFramePoseFrame,
                                 poseRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, poseRecord.hasSpriteFrameTransform,
                                 poseRecord.spriteFrameTransform,
                                 poseRecord.scale, poseRecord.height, true,
                                 poseRecord.lastSpriteFramePoseFrame,
                                 bestPoseFrame);
        if (IsRegistryFrameFresh(poseRecord.lastRootPoseFrame, poseRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, poseRecord.hasWorldTransform,
                                 poseRecord.worldTransform, poseRecord.scale,
                                 poseRecord.height, false,
                                 poseRecord.lastRootPoseFrame, bestPoseFrame);
        if (IsRegistryFrameFresh(poseRecord.lastMatrixPaletteFrame,
                                 poseRegistryFrame))
          MaybeApplyPoseMatrices(semantic, poseRecord.matrixCount,
                                 poseRecord.matrixHash,
                                 poseRecord.lastMatrixPaletteFrame,
                                 bestMatrixFrame);
      }
    }
  }

  if (semantic.object == nullptr) {
    auto& registry = RenderObjectRegistry::instance();
    const RenderObjectInfo* object = nullptr;
    if (semantic.sceneNode != nullptr)
      object = registry.findBySceneNode(semantic.sceneNode);
    if (object == nullptr && semantic.worldObjectEntry != nullptr)
      object = registry.findByEntry(semantic.worldObjectEntry);
    if (object == nullptr && semantic.jHandle != 0u)
      object = registry.findByHandle(semantic.jHandle);
    MergeRenderObject(semantic, object);
  }

  const bool hasMeaningfulContext =
      semantic.sceneNode != nullptr || semantic.worldObjectEntry != nullptr ||
      semantic.jHandle != 0u || currentUnitPtr != nullptr ||
      semantic.runtimeModelPtr != nullptr || semantic.object != nullptr;
  const bool bridgeReady = semantic.HasStableIdentity() || semantic.hasPoseTransform ||
                           semantic.poseMatrixCount != 0u;

  // 某个对象第一次进入视野时，如果这帧仍拿不到稳定身份/姿态，
  // 不要等到全局周期刷新再补票。这里对“当前缺失对象”
  // 触发一个很短的 repair burst，但带冷却，避免在超大地图上
  // 长时间把全量对象身份桥常驻打开。
  if (!bridgeReady && hasMeaningfulContext) {
    const uint64_t nowFrame = CurrentRuntimeBridgeFrame();
    RequestShadowBridgeRepairBurst(nowFrame);
  }

  return bridgeReady;
}

} // namespace dxvk::war3::render
