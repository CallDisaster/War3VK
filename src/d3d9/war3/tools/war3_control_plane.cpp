#include "war3_control_plane.h"

#include "../../d3d9_war3_debug.h"
#include "war3_frame_capture.h"
#include "war3_internal_test_api.h"

#include "../render/war3_render_objects.h"
#include "../render/war3_current_draw_contract.h"
#include "../shadow/war3_shadow_runtime_contract.h"
#include "../shadow/war3_shadow_renderer_core.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace dxvk::war3::tools {

namespace {

using json = nlohmann::json;

std::atomic<bool> g_running = false;
std::atomic<bool> g_stopRequested = false;
std::thread g_serverThread;
std::atomic<uint32_t> g_activeClientCount = 0u;
std::mutex g_semanticBuildRequestMutex;
uint64_t g_lastSemanticBuildRequestMs = 0u;
uint64_t g_lastSemanticBuildRequestFrameIndex = 0u;

uint64_t GetEpochMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

std::string EscapeBackslashes(const std::string& value) {
  std::string result = value;
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}

std::string BuildPipeName() {
  return "\\\\.\\pipe\\War3ControlPlane_" +
         std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
}

War3FrameManifestSummary BuildManifestSummary() {
  War3FrameManifestSummary summary = {};
  const auto visibleDebug =
      dxvk::war3::render::VisibleRenderableRegistry::instance()
          .queryDebugSummary();
  const auto bundle =
      dxvk::war3::shadow::ShadowRuntimeContractCache::instance()
          .snapshotBundleShared();
  const auto& manifest = bundle.manifest;
  if (manifest == nullptr)
    return summary;
  summary.frameNumber = manifest->frameSerial;
  summary.publishRevision = manifest->publishRevision;
  summary.visibleCount = manifest->visibleCount;
  summary.mainQueueCount = manifest->mainQueueCount;
  summary.transparentCount = manifest->transparentCount;
  const auto& stats = bundle.stats;
  summary.rootUnitSupplementSeedCount =
      stats.rootUnitSupplementSeedCount;
  summary.rootUnitSupplementUnitSeedCount =
      stats.rootUnitSupplementUnitSeedCount;
  summary.rootUnitSupplementSkippedNoIdentity =
      stats.rootUnitSupplementSkippedNoIdentity;
  summary.rootUnitSupplementSkippedAttachmentChild =
      stats.rootUnitSupplementSkippedAttachmentChild;
  summary.rootUnitSupplementSkippedNoPose =
      stats.rootUnitSupplementSkippedNoPose;
  summary.rootUnitSupplementSkippedNoResource =
      stats.rootUnitSupplementSkippedNoResource;
  summary.rootUnitSupplementSkippedNoGeoset =
      stats.rootUnitSupplementSkippedNoGeoset;
  summary.rootUnitSupplementSkippedNoGeosetZeroCount =
      stats.rootUnitSupplementSkippedNoGeosetZeroCount;
  summary.rootUnitSupplementSkippedNoGeosetStoreMiss =
      stats.rootUnitSupplementSkippedNoGeosetStoreMiss;
  summary.rootUnitSupplementSkippedNoGeosetNotReady =
      stats.rootUnitSupplementSkippedNoGeosetNotReady;
  summary.rootUnitSupplementSkippedDuplicate =
      stats.rootUnitSupplementSkippedDuplicate;
  summary.rootUnitSupplementAppended =
      stats.rootUnitSupplementAppended;
  summary.rootUnitSupplementReusedFromPrior =
      stats.rootUnitSupplementReusedFromPrior;
  summary.rootUnitSupplementResourceCacheMiss =
      stats.rootUnitSupplementResourceCacheMiss;
  summary.rootUnitSupplementResourceCacheNotReady =
      stats.rootUnitSupplementResourceCacheNotReady;
  summary.rootUnitSupplementResourceSemanticKeyResolved =
      stats.rootUnitSupplementResourceSemanticKeyResolved;
  summary.rootUnitSupplementResourceSemanticKeyReady =
      stats.rootUnitSupplementResourceSemanticKeyReady;
  summary.rootUnitSupplementGeosetCacheFallback =
      stats.rootUnitSupplementGeosetCacheFallback;
  summary.directPoseSupplementAttemptCount =
      stats.directPoseSupplementAttemptCount;
  summary.directPoseSupplementResolvedCount =
      stats.directPoseSupplementResolvedCount;
  summary.directPoseSupplementSkippedExisting =
      stats.directPoseSupplementSkippedExisting;
  summary.directPoseSupplementSkippedInvalid =
      stats.directPoseSupplementSkippedInvalid;
  summary.visibleMainQueueRangeCallCount =
      visibleDebug.mainQueueRangeCallCount;
  summary.visibleMainQueueRangeRecordCount =
      visibleDebug.mainQueueRangeRecordCount;
  summary.visibleSemanticCandidateCallCount =
      visibleDebug.semanticCandidateCallCount;
  summary.visibleSemanticCandidateMergedCount =
      visibleDebug.semanticCandidateMergedCount;
  summary.visibleSemanticCandidateAppendedCount =
      visibleDebug.semanticCandidateAppendedCount;
  summary.visibleSemanticMergeFallbackCallCount =
      visibleDebug.semanticMergeFallbackCallCount;
  summary.visibleSemanticMergeIndexLookupCount =
      visibleDebug.semanticMergeIndexLookupCount;
  summary.visibleSemanticMergeIndexHitCount =
      visibleDebug.semanticMergeIndexHitCount;
  summary.visibleSemanticMergeIndexCandidateVisitCount =
      visibleDebug.semanticMergeIndexCandidateVisitCount;
  summary.visibleSemanticMergeLegacyScanCallCount =
      visibleDebug.semanticMergeLegacyScanCallCount;
  summary.visibleSemanticMergeLegacyScanRecordVisitCount =
      visibleDebug.semanticMergeLegacyScanRecordVisitCount;
  summary.visibleSemanticMergeVerifierCallCount =
      visibleDebug.semanticMergeVerifierCallCount;
  summary.visibleSemanticMergeVerifierLegacyScanRecordVisitCount =
      visibleDebug.semanticMergeVerifierLegacyScanRecordVisitCount;
  summary.visibleSemanticMergeVerifierMismatchCount =
      visibleDebug.semanticMergeVerifierMismatchCount;
  summary.visibleSemanticMergeVerifierSelectionMismatchCount =
      visibleDebug.semanticMergeVerifierSelectionMismatchCount;
  summary.visibleSemanticMergeVerifierAuxIndexCheckCount =
      visibleDebug.semanticMergeVerifierAuxIndexCheckCount;
  summary.visibleSemanticMergeVerifierAuxIndexMismatchCount =
      visibleDebug.semanticMergeVerifierAuxIndexMismatchCount;
  summary.visibleSemanticMergeIndexEntryCount =
      visibleDebug.semanticMergeIndexEntryCount;
  summary.visibleTransparentEntryCallCount =
      visibleDebug.transparentEntryCallCount;

  const auto& records = manifest->records;
  for (const auto& record : records) {
    if (record.hasStableIdentity())
      summary.recordsWithStableIdentity++;
    if (record.hasResolvedGeoset())
      summary.recordsWithResolvedGeoset++;
    if (record.runtimeModelPtr != nullptr)
      summary.recordsWithRuntimeModel++;
    if (record.modelResourcePtr != nullptr)
      summary.recordsWithModelResource++;

    switch (record.objectKind) {
    case dxvk::war3::render::ObjectKind::Unit:
      summary.unitCount++;
      if (record.hasResolvedGeoset())
        summary.unitWithResolvedGeoset++;
      if (record.meshData != nullptr)
        summary.unitWithMeshData++;
      if (record.modelResourcePtr != nullptr)
        summary.unitWithModelResource++;
      break;
    case dxvk::war3::render::ObjectKind::Building:
      summary.buildingCount++;
      if (record.hasResolvedGeoset())
        summary.buildingWithResolvedGeoset++;
      if (record.meshData != nullptr)
        summary.buildingWithMeshData++;
      if (record.modelResourcePtr != nullptr)
        summary.buildingWithModelResource++;
      break;
    case dxvk::war3::render::ObjectKind::Destructible:
      summary.destructibleCount++;
      if (record.hasResolvedGeoset())
        summary.destructibleWithResolvedGeoset++;
      if (record.meshData != nullptr)
        summary.destructibleWithMeshData++;
      if (record.modelResourcePtr != nullptr)
        summary.destructibleWithModelResource++;
      break;
    case dxvk::war3::render::ObjectKind::Item:
      summary.itemCount++;
      break;
    case dxvk::war3::render::ObjectKind::Effect:
      summary.effectCount++;
      break;
    default:
      summary.unknownCount++;
      break;
    }
  }
  return summary;
}

json ToJson(const War3RuntimeStatusSnapshot& snapshot) {
  return json{
      {"timestampMs", snapshot.timestampMs},
      {"source", snapshot.source},
      {"frameIndex", snapshot.frameIndex},
      {"module",
       {{"registered", snapshot.module.registered},
        {"loaded", snapshot.module.loaded},
        {"dispatchCalls", snapshot.module.dispatchCalls},
        {"handlers", snapshot.module.handlers},
        {"callbackErrors", snapshot.module.callbackErrors},
        {"state", snapshot.module.state}}},
      {"perf",
       {{"enabled", snapshot.perf.enabled},
        {"recording", snapshot.perf.recording}}},
      {"profile",
       {{"name", snapshot.profile.name},
        {"disabledModules", snapshot.profile.disabledModules},
        {"enabledModules", snapshot.profile.enabledModules}}},
      {"runtime",
       {{"runtimeReady", snapshot.runtime.runtimeReady},
        {"jassReady", snapshot.runtime.jassReady},
        {"gameStarted", snapshot.runtime.gameStarted}}},
      {"render",
       {{"inGameRenderReady", snapshot.render.inGameRenderReady},
        {"isInGame", snapshot.render.isInGame},
        {"isLoading", snapshot.render.isLoading},
        {"worldPtr", snapshot.render.worldPtr}}},
      {"frame",
       {{"frameNumber", snapshot.frame.frameNumber},
        {"publishRevision", snapshot.frame.publishRevision},
        {"visibleCount", snapshot.frame.visibleCount},
        {"mainQueueCount", snapshot.frame.mainQueueCount},
        {"transparentCount", snapshot.frame.transparentCount},
        {"recordsWithStableIdentity",
         snapshot.frame.recordsWithStableIdentity},
        {"recordsWithResolvedGeoset",
         snapshot.frame.recordsWithResolvedGeoset},
        {"recordsWithRuntimeModel", snapshot.frame.recordsWithRuntimeModel},
        {"recordsWithModelResource", snapshot.frame.recordsWithModelResource},
         {"unitCount", snapshot.frame.unitCount},
         {"buildingCount", snapshot.frame.buildingCount},
         {"destructibleCount", snapshot.frame.destructibleCount},
         {"unitWithResolvedGeoset",
          snapshot.frame.unitWithResolvedGeoset},
         {"buildingWithResolvedGeoset",
          snapshot.frame.buildingWithResolvedGeoset},
         {"destructibleWithResolvedGeoset",
          snapshot.frame.destructibleWithResolvedGeoset},
         {"unitWithMeshData", snapshot.frame.unitWithMeshData},
         {"buildingWithMeshData", snapshot.frame.buildingWithMeshData},
         {"destructibleWithMeshData",
          snapshot.frame.destructibleWithMeshData},
         {"unitWithModelResource", snapshot.frame.unitWithModelResource},
         {"buildingWithModelResource",
          snapshot.frame.buildingWithModelResource},
         {"destructibleWithModelResource",
          snapshot.frame.destructibleWithModelResource},
         {"itemCount", snapshot.frame.itemCount},
        {"effectCount", snapshot.frame.effectCount},
        {"unknownCount", snapshot.frame.unknownCount},
        {"visibleMainQueueRangeCallCount",
         snapshot.frame.visibleMainQueueRangeCallCount},
        {"visibleMainQueueRangeRecordCount",
         snapshot.frame.visibleMainQueueRangeRecordCount},
        {"visibleSemanticCandidateCallCount",
         snapshot.frame.visibleSemanticCandidateCallCount},
        {"visibleSemanticCandidateMergedCount",
         snapshot.frame.visibleSemanticCandidateMergedCount},
        {"visibleSemanticCandidateAppendedCount",
         snapshot.frame.visibleSemanticCandidateAppendedCount},
        {"visibleTransparentEntryCallCount",
         snapshot.frame.visibleTransparentEntryCallCount}}},
      {"shadow",
       {{"matrixPaletteCount", snapshot.shadow.matrixPaletteCount},
        {"shadowReadyGeosetCount", snapshot.shadow.shadowReadyGeosetCount},
        {"shadowModelResourceCount", snapshot.shadow.shadowModelResourceCount},
        {"shadowRuntimeModelCount", snapshot.shadow.shadowRuntimeModelCount},
        {"upperLayerResolveAuthoritativeRigid",
         snapshot.shadow.upperLayerResolveAuthoritativeRigid},
        {"upperLayerResolveAuthoritativeSkinned",
         snapshot.shadow.upperLayerResolveAuthoritativeSkinned},
        {"upperLayerResolvedAuthoritativeItems",
         snapshot.shadow.upperLayerResolvedAuthoritativeItems},
        {"upperLayerEmitted", snapshot.shadow.upperLayerEmitted},
        {"semanticCoreFrameSerial", snapshot.shadow.semanticCoreFrameSerial},
        {"semanticCoreResolved", snapshot.shadow.semanticCoreResolved},
        {"semanticCoreSkinnedResolved",
         snapshot.shadow.semanticCoreSkinnedResolved},
        {"semanticCoreExplicitResourceOwnerRigidResolved",
         snapshot.shadow.semanticCoreExplicitResourceOwnerRigidResolved},
        {"semanticCoreExplicitResourceOwnerRigidWorldTransformResolved",
         snapshot.shadow
             .semanticCoreExplicitResourceOwnerRigidWorldTransformResolved},
        {"semanticCoreExplicitResourceOwnerRigidNoMatrixPalette",
         snapshot.shadow.semanticCoreExplicitResourceOwnerRigidNoMatrixPalette},
        {"semanticCoreSubmittedDrawCount",
         snapshot.shadow.semanticCoreSubmittedDrawCount},
        {"semanticCoreSkippedNoRuntimeGroupPalette",
         snapshot.shadow.semanticCoreSkippedNoRuntimeGroupPalette},
        {"semanticCoreFrameFresh", snapshot.shadow.semanticCoreFrameFresh},
        {"semanticCoreBuildInProgress",
         snapshot.shadow.semanticCoreBuildInProgress},
        {"semanticCoreBuildRequestPending",
         snapshot.shadow.semanticCoreBuildRequestPending},
        {"semanticCoreBuildCurrentRecordIndex",
         snapshot.shadow.semanticCoreBuildCurrentRecordIndex},
        {"semanticCoreBuildRecordCount",
         snapshot.shadow.semanticCoreBuildRecordCount},
        {"semanticCoreBuildChunkCount",
         snapshot.shadow.semanticCoreBuildChunkCount},
        {"semanticSceneCurrentDrawContractKnownCount",
         snapshot.shadow.semanticSceneCurrentDrawContractKnownCount},
        {"semanticSceneCurrentDrawPaletteReadyCount",
         snapshot.shadow.semanticSceneCurrentDrawPaletteReadyCount},
        {"semanticSceneCurrentDrawGroupSlotReadyCount",
         snapshot.shadow.semanticSceneCurrentDrawGroupSlotReadyCount},
        {"semanticSceneCurrentDrawResolveReadyCount",
         snapshot.shadow.semanticSceneCurrentDrawResolveReadyCount},
        {"semanticSceneCurrentDrawMissNoContract",
         snapshot.shadow.semanticSceneCurrentDrawMissNoContract},
        {"semanticSceneCurrentDrawMissNoPalette",
         snapshot.shadow.semanticSceneCurrentDrawMissNoPalette},
        {"semanticSceneCurrentDrawMissNoGroupSlots",
         snapshot.shadow.semanticSceneCurrentDrawMissNoGroupSlots},
        {"semanticSceneCurrentDrawMissStaleVisibleFrame",
         snapshot.shadow.semanticSceneCurrentDrawMissStaleVisibleFrame},
        {"semanticSceneCurrentDrawResolveReadyRejectedCount",
         snapshot.shadow.semanticSceneCurrentDrawResolveReadyRejectedCount},
        {"semanticSceneCanonicalReadyCount",
         snapshot.shadow.semanticSceneCanonicalReadyCount},
        {"semanticSceneCanonicalRejectNoStableIdentity",
         snapshot.shadow.semanticSceneCanonicalRejectNoStableIdentity},
        {"semanticSceneCanonicalRejectNoMesh",
         snapshot.shadow.semanticSceneCanonicalRejectNoMesh},
        {"semanticSceneCanonicalRejectNoWorldTransform",
         snapshot.shadow.semanticSceneCanonicalRejectNoWorldTransform},
        {"semanticSceneCanonicalRejectNoPalette",
         snapshot.shadow.semanticSceneCanonicalRejectNoPalette},
        {"semanticSceneCanonicalRejectNoSlotContract",
         snapshot.shadow.semanticSceneCanonicalRejectNoSlotContract},
        {"semanticSceneCanonicalRejectStaleProducer",
         snapshot.shadow.semanticSceneCanonicalRejectStaleProducer},
        {"semanticSceneCanonicalRejectInvalidVertexIndex",
         snapshot.shadow.semanticSceneCanonicalRejectInvalidVertexIndex},
        {"semanticSceneCanonicalRejectExplicitBlendIncomplete",
         snapshot.shadow.semanticSceneCanonicalRejectExplicitBlendIncomplete},
        {"semanticSceneCanonicalRejectAfterReadyCount",
         snapshot.shadow.semanticSceneCanonicalRejectAfterReadyCount},
        {"currentDrawContractPublishAttemptCount",
         snapshot.shadow.currentDrawContractPublishAttemptCount},
        {"currentDrawContractPublishReadyCount",
         snapshot.shadow.currentDrawContractPublishReadyCount},
        {"currentDrawContractPublishSkippedNonWorldContext",
         snapshot.shadow.currentDrawContractPublishSkippedNonWorldContext},
        {"currentDrawContractPublishSkippedSmallViewport",
         snapshot.shadow.currentDrawContractPublishSkippedSmallViewport},
        {"currentDrawContractQueryAttemptCount",
         snapshot.shadow.currentDrawContractQueryAttemptCount},
        {"currentDrawContractQueryHitCount",
         snapshot.shadow.currentDrawContractQueryHitCount},
        {"currentDrawCapturedPaletteQueryAttemptCount",
         snapshot.shadow.currentDrawCapturedPaletteQueryAttemptCount},
        {"currentDrawCapturedPaletteQueryHitCount",
         snapshot.shadow.currentDrawCapturedPaletteQueryHitCount},
        {"currentDrawGroupSlotDecodeAttemptCount",
         snapshot.shadow.currentDrawGroupSlotDecodeAttemptCount},
        {"currentDrawGroupSlotDecodeHitCount",
         snapshot.shadow.currentDrawGroupSlotDecodeHitCount},
        {"currentDrawGroupSlotDecodeMissDisabledStream",
         snapshot.shadow.currentDrawGroupSlotDecodeMissDisabledStream},
        {"currentDrawPreparedSliceProbeAttemptCount",
         snapshot.shadow.currentDrawPreparedSliceProbeAttemptCount},
        {"currentDrawPreparedSliceProbeContextReadyCount",
         snapshot.shadow.currentDrawPreparedSliceProbeContextReadyCount},
        {"currentDrawPreparedSliceProbeBackingReadableCount",
         snapshot.shadow.currentDrawPreparedSliceProbeBackingReadableCount},
        {"currentDrawPreparedSliceRecordedCount",
         snapshot.shadow.currentDrawPreparedSliceRecordedCount},
        {"currentDrawPreparedSliceQueryAttemptCount",
         snapshot.shadow.currentDrawPreparedSliceQueryAttemptCount},
        {"currentDrawPreparedSliceQueryHitCount",
         snapshot.shadow.currentDrawPreparedSliceQueryHitCount},
        {"currentDrawPreparedSliceQueryMissCount",
         snapshot.shadow.currentDrawPreparedSliceQueryMissCount},
        {"semanticSceneDirectPreparedSliceAuthoritativeCount",
         snapshot.shadow.semanticSceneDirectPreparedSliceAuthoritativeCount},
        {"semanticSceneDirectPreparedSliceFallbackLayerIndexCount",
         snapshot.shadow
             .semanticSceneDirectPreparedSliceFallbackLayerIndexCount},
        {"semanticSceneDirectPreparedSliceMissingCount",
         snapshot.shadow.semanticSceneDirectPreparedSliceMissingCount},
        {"semanticScenePreparedProbeAttemptCount",
         snapshot.shadow.semanticScenePreparedProbeAttemptCount},
        {"semanticScenePreparedProbeContextReadyCount",
         snapshot.shadow.semanticScenePreparedProbeContextReadyCount},
        {"semanticScenePreparedProbeBackingReadableCount",
         snapshot.shadow.semanticScenePreparedProbeBackingReadableCount},
        {"semanticScenePreparedSliceRecordedCount",
         snapshot.shadow.semanticScenePreparedSliceRecordedCount},
        {"semanticScenePreparedSliceQueryAttemptCount",
         snapshot.shadow.semanticScenePreparedSliceQueryAttemptCount},
        {"semanticScenePreparedSliceQueryHitCount",
         snapshot.shadow.semanticScenePreparedSliceQueryHitCount},
        {"semanticScenePreparedSliceQueryMissCount",
         snapshot.shadow.semanticScenePreparedSliceQueryMissCount},
        {"currentDrawLastVisibleFrameSerial",
         snapshot.shadow.currentDrawLastVisibleFrameSerial},
        {"currentDrawLastRenderFrameIndex",
         snapshot.shadow.currentDrawLastRenderFrameIndex},
        {"currentDrawLastMissReason",
         snapshot.shadow.currentDrawLastMissReason},
        {"semanticScenePopulateLastReturnReason",
         snapshot.shadow.semanticScenePopulateLastReturnReason},
        {"semanticScenePopulateLastProducerPublishAttemptDelta",
         snapshot.shadow.semanticScenePopulateLastProducerPublishAttemptDelta},
        {"semanticScenePopulateLastProducerPublishReadyDelta",
         snapshot.shadow.semanticScenePopulateLastProducerPublishReadyDelta},
        {"semanticScenePopulateLastProducerQueryAttemptDelta",
         snapshot.shadow.semanticScenePopulateLastProducerQueryAttemptDelta},
        {"semanticScenePopulateLastProducerQueryHitDelta",
         snapshot.shadow.semanticScenePopulateLastProducerQueryHitDelta},
        {"semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta",
         snapshot.shadow
             .semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta},
        {"semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta",
         snapshot.shadow
             .semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta},
        {"semanticScenePopulateLastProducerGroupDecodeAttemptDelta",
         snapshot.shadow.semanticScenePopulateLastProducerGroupDecodeAttemptDelta},
        {"semanticScenePopulateLastProducerGroupDecodeHitDelta",
         snapshot.shadow.semanticScenePopulateLastProducerGroupDecodeHitDelta},
        {"currentDrawLastMissReasonName",
         dxvk::war3::render::DescribeCurrentDrawMissReason(
             static_cast<uint32_t>(snapshot.shadow.currentDrawLastMissReason))},
        {"semanticSceneShadowCastersCount",
         snapshot.shadow.semanticSceneShadowCastersCount},
        {"semanticSceneReplayDrawsCount",
         snapshot.shadow.semanticSceneReplayDrawsCount},
        {"semanticSceneShadowMapDrawnCasters",
         snapshot.shadow.semanticSceneShadowMapDrawnCasters},
        {"semanticSceneShadowMapSkinnedDrawnCount",
         snapshot.shadow.semanticSceneShadowMapSkinnedDrawnCount},
        {"semanticSceneShadowTaaActive",
         snapshot.shadow.semanticSceneShadowTaaActive},
        {"semanticSceneReceiverReuseShadowMap",
         snapshot.shadow.semanticSceneReceiverReuseShadowMap},
        {"semanticSceneReceiverInputValid",
         snapshot.shadow.semanticSceneReceiverInputValid},
        {"semanticSceneReceiverInputRejectReason",
         snapshot.shadow.semanticSceneReceiverInputRejectReason},
        {"semanticSceneReceiverNeedPass",
         snapshot.shadow.semanticSceneReceiverNeedPass},
        {"semanticSceneReceiverNeedShadowMap",
         snapshot.shadow.semanticSceneReceiverNeedShadowMap},
        {"semanticSceneReceiverHasCompleteShadowMap",
         snapshot.shadow.semanticSceneReceiverHasCompleteShadowMap},
        {"semanticSceneReceiverHasUsableDirectionalShadow",
         snapshot.shadow.semanticSceneReceiverHasUsableDirectionalShadow},
        {"semanticSceneReceiverActiveStrengthMilli",
         snapshot.shadow.semanticSceneReceiverActiveStrengthMilli},
        {"semanticSceneReceiverUboStrengthMilli",
         snapshot.shadow.semanticSceneReceiverUboStrengthMilli},
        {"semanticSceneReceiverDebugMode",
         snapshot.shadow.semanticSceneReceiverDebugMode},
        {"semanticSceneReceiverCsmCascadeCount",
         snapshot.shadow.semanticSceneReceiverCsmCascadeCount},
        {"semanticSceneReceiverRunEntryFlags",
         snapshot.shadow.semanticSceneReceiverRunEntryFlags},
        {"semanticSceneReceiverRunEarlyReturnReason",
         snapshot.shadow.semanticSceneReceiverRunEarlyReturnReason},
        {"semanticSceneShadowMapExecutedThisFrame",
         snapshot.shadow.semanticSceneShadowMapExecutedThisFrame},
        {"semanticSceneReceiverSettingsShadowsEnabled",
         snapshot.shadow.semanticSceneReceiverSettingsShadowsEnabled},
        {"semanticSceneReceiverSettingsOutlineEnabled",
         snapshot.shadow.semanticSceneReceiverSettingsOutlineEnabled},
        {"semanticSceneReceiverSettingsRawStrengthMilli",
         snapshot.shadow.semanticSceneReceiverSettingsRawStrengthMilli},
        {"semanticSceneReceiverComputedShadowStrengthMilli",
         snapshot.shadow.semanticSceneReceiverComputedShadowStrengthMilli},
        {"semanticSceneReceiverHasSunShadow",
         snapshot.shadow.semanticSceneReceiverHasSunShadow},
        {"semanticSceneReceiverHasPointShadow",
         snapshot.shadow.semanticSceneReceiverHasPointShadow},
        {"semanticSceneReceiverNeedOutlinePass",
         snapshot.shadow.semanticSceneReceiverNeedOutlinePass},
        {"semanticSceneReceiverZeroStrengthFrameCount",
         snapshot.shadow.semanticSceneReceiverZeroStrengthFrameCount},
        {"semanticSceneReceiverDrawnWithZeroStrengthCount",
         snapshot.shadow.semanticSceneReceiverDrawnWithZeroStrengthCount},
        {"semanticSceneReceiverNoCompleteShadowMapCount",
         snapshot.shadow.semanticSceneReceiverNoCompleteShadowMapCount},
        {"semanticSceneReceiverNoShadowMapImageCount",
         snapshot.shadow.semanticSceneReceiverNoShadowMapImageCount},
        {"semanticSceneReceiverNoShadowMapSampleViewCount",
         snapshot.shadow.semanticSceneReceiverNoShadowMapSampleViewCount},
        {"semanticSceneReceiverNoCandidateCsmCount",
         snapshot.shadow.semanticSceneReceiverNoCandidateCsmCount},
        {"semanticSceneReceiverCsmFallbackToLastGoodCount",
         snapshot.shadow.semanticSceneReceiverCsmFallbackToLastGoodCount},
        {"semanticSceneReceiverHoldInvalidCsmCount",
         snapshot.shadow.semanticSceneReceiverHoldInvalidCsmCount},
        {"semanticSceneReceiverHoldEmptyReplayCount",
         snapshot.shadow.semanticSceneReceiverHoldEmptyReplayCount},
        {"semanticSceneReceiverHoldIdentityChurnCount",
         snapshot.shadow.semanticSceneReceiverHoldIdentityChurnCount},
        {"semanticSceneReceiverReuseInvalidatedAfterEnsureCount",
         snapshot.shadow.semanticSceneReceiverReuseInvalidatedAfterEnsureCount},
        {"semanticSceneShadowMapRenderSkippedNoResourcesCount",
         snapshot.shadow.semanticSceneShadowMapRenderSkippedNoResourcesCount},
        {"semanticSceneShadowMapRenderSkippedNoMatrixBufferCount",
         snapshot.shadow
             .semanticSceneShadowMapRenderSkippedNoMatrixBufferCount},
        {"semanticSceneReceiverViewportX",
         snapshot.shadow.semanticSceneReceiverViewportX},
        {"semanticSceneReceiverViewportY",
         snapshot.shadow.semanticSceneReceiverViewportY},
        {"semanticSceneReceiverViewportWidth",
         snapshot.shadow.semanticSceneReceiverViewportWidth},
        {"semanticSceneReceiverViewportHeight",
         snapshot.shadow.semanticSceneReceiverViewportHeight}}}};
}

json ToJson(const War3FrameManifestSummary& summary) {
  return json{
      {"frameNumber", summary.frameNumber},
      {"publishRevision", summary.publishRevision},
      {"visibleCount", summary.visibleCount},
      {"mainQueueCount", summary.mainQueueCount},
      {"transparentCount", summary.transparentCount},
      {"recordsWithStableIdentity", summary.recordsWithStableIdentity},
      {"recordsWithResolvedGeoset", summary.recordsWithResolvedGeoset},
      {"recordsWithRuntimeModel", summary.recordsWithRuntimeModel},
      {"recordsWithModelResource", summary.recordsWithModelResource},
      {"unitCount", summary.unitCount},
      {"buildingCount", summary.buildingCount},
      {"destructibleCount", summary.destructibleCount},
      {"unitWithResolvedGeoset", summary.unitWithResolvedGeoset},
      {"buildingWithResolvedGeoset", summary.buildingWithResolvedGeoset},
      {"destructibleWithResolvedGeoset",
       summary.destructibleWithResolvedGeoset},
      {"unitWithMeshData", summary.unitWithMeshData},
      {"buildingWithMeshData", summary.buildingWithMeshData},
      {"destructibleWithMeshData", summary.destructibleWithMeshData},
      {"unitWithModelResource", summary.unitWithModelResource},
      {"buildingWithModelResource", summary.buildingWithModelResource},
      {"destructibleWithModelResource",
       summary.destructibleWithModelResource},
      {"itemCount", summary.itemCount},
      {"effectCount", summary.effectCount},
      {"unknownCount", summary.unknownCount},
      {"visibleMainQueueRangeCallCount",
       summary.visibleMainQueueRangeCallCount},
      {"visibleMainQueueRangeRecordCount",
       summary.visibleMainQueueRangeRecordCount},
      {"visibleSemanticCandidateCallCount",
       summary.visibleSemanticCandidateCallCount},
      {"visibleSemanticCandidateMergedCount",
       summary.visibleSemanticCandidateMergedCount},
      {"visibleSemanticCandidateAppendedCount",
       summary.visibleSemanticCandidateAppendedCount},
      {"visibleSemanticMergeFallbackCallCount",
       summary.visibleSemanticMergeFallbackCallCount},
      {"visibleSemanticMergeIndexLookupCount",
       summary.visibleSemanticMergeIndexLookupCount},
      {"visibleSemanticMergeIndexHitCount",
       summary.visibleSemanticMergeIndexHitCount},
      {"visibleSemanticMergeIndexCandidateVisitCount",
       summary.visibleSemanticMergeIndexCandidateVisitCount},
      {"visibleSemanticMergeLegacyScanCallCount",
       summary.visibleSemanticMergeLegacyScanCallCount},
      {"visibleSemanticMergeLegacyScanRecordVisitCount",
       summary.visibleSemanticMergeLegacyScanRecordVisitCount},
      {"visibleSemanticMergeVerifierCallCount",
       summary.visibleSemanticMergeVerifierCallCount},
      {"visibleSemanticMergeVerifierLegacyScanRecordVisitCount",
       summary.visibleSemanticMergeVerifierLegacyScanRecordVisitCount},
      {"visibleSemanticMergeVerifierMismatchCount",
       summary.visibleSemanticMergeVerifierMismatchCount},
      {"visibleSemanticMergeVerifierSelectionMismatchCount",
       summary.visibleSemanticMergeVerifierSelectionMismatchCount},
      {"visibleSemanticMergeVerifierAuxIndexCheckCount",
       summary.visibleSemanticMergeVerifierAuxIndexCheckCount},
      {"visibleSemanticMergeVerifierAuxIndexMismatchCount",
       summary.visibleSemanticMergeVerifierAuxIndexMismatchCount},
      {"visibleSemanticMergeIndexEntryCount",
       summary.visibleSemanticMergeIndexEntryCount},
      {"visibleTransparentEntryCallCount",
       summary.visibleTransparentEntryCallCount},
      {"rootUnitSupplementSeedCount",
       summary.rootUnitSupplementSeedCount},
      {"rootUnitSupplementUnitSeedCount",
       summary.rootUnitSupplementUnitSeedCount},
      {"rootUnitSupplementSkippedNoIdentity",
       summary.rootUnitSupplementSkippedNoIdentity},
      {"rootUnitSupplementSkippedAttachmentChild",
       summary.rootUnitSupplementSkippedAttachmentChild},
      {"rootUnitSupplementSkippedNoPose",
       summary.rootUnitSupplementSkippedNoPose},
      {"rootUnitSupplementSkippedNoResource",
       summary.rootUnitSupplementSkippedNoResource},
      {"rootUnitSupplementSkippedNoGeoset",
       summary.rootUnitSupplementSkippedNoGeoset},
      {"rootUnitSupplementSkippedNoGeosetZeroCount",
       summary.rootUnitSupplementSkippedNoGeosetZeroCount},
      {"rootUnitSupplementSkippedNoGeosetStoreMiss",
       summary.rootUnitSupplementSkippedNoGeosetStoreMiss},
      {"rootUnitSupplementSkippedNoGeosetNotReady",
       summary.rootUnitSupplementSkippedNoGeosetNotReady},
      {"rootUnitSupplementSkippedDuplicate",
       summary.rootUnitSupplementSkippedDuplicate},
      {"rootUnitSupplementAppended",
       summary.rootUnitSupplementAppended},
      {"rootUnitSupplementReusedFromPrior",
       summary.rootUnitSupplementReusedFromPrior},
      {"rootUnitSupplementResourceCacheMiss",
       summary.rootUnitSupplementResourceCacheMiss},
      {"rootUnitSupplementResourceCacheNotReady",
       summary.rootUnitSupplementResourceCacheNotReady},
      {"rootUnitSupplementResourceSemanticKeyResolved",
       summary.rootUnitSupplementResourceSemanticKeyResolved},
      {"rootUnitSupplementResourceSemanticKeyReady",
       summary.rootUnitSupplementResourceSemanticKeyReady},
      {"rootUnitSupplementGeosetCacheFallback",
       summary.rootUnitSupplementGeosetCacheFallback},
      {"directPoseSupplementAttemptCount",
       summary.directPoseSupplementAttemptCount},
      {"directPoseSupplementResolvedCount",
       summary.directPoseSupplementResolvedCount},
      {"directPoseSupplementSkippedExisting",
       summary.directPoseSupplementSkippedExisting},
      {"directPoseSupplementSkippedInvalid",
       summary.directPoseSupplementSkippedInvalid},
  };
}

json ToJson(const render::ShadowRuntimeCadenceSample& sample) {
  return json{
      {"serial", sample.serial},
      {"frameIndex", sample.frameIndex},
      {"sceneFrameSerial", sample.sceneFrameSerial},
      {"selectedFrameSerial", sample.selectedFrameSerial},
      {"reusableFrameSerial", sample.reusableFrameSerial},
      {"sourcePublishRevision", sample.sourcePublishRevision},
      {"targetPublishRevision", sample.targetPublishRevision},
      {"populateReturnReason", sample.populateReturnReason},
      {"inputDrawCount", sample.inputDrawCount},
      {"inputSkinnedCount", sample.inputSkinnedCount},
      {"submittedDrawCount", sample.submittedDrawCount},
      {"submittedSkinnedCount", sample.submittedSkinnedCount},
      {"directSubmittedRecordCount", sample.directSubmittedRecordCount},
      {"directSubmittedObjectCount", sample.directSubmittedObjectCount},
      {"shadowCastersCount", sample.shadowCastersCount},
      {"replayDrawsCount", sample.replayDrawsCount},
      {"shadowMapDrawnCasters", sample.shadowMapDrawnCasters},
      {"shadowMapSkinnedDrawnCount", sample.shadowMapSkinnedDrawnCount},
      {"receiverNeedShadowMap", sample.receiverNeedShadowMap},
      {"receiverHasCompleteShadowMap", sample.receiverHasCompleteShadowMap},
      {"receiverReuseShadowMap", sample.receiverReuseShadowMap},
      {"shadowMapExecutedThisFrame", sample.shadowMapExecutedThisFrame},
      {"receiverRunEarlyReturnReason", sample.receiverRunEarlyReturnReason},
      {"receiverRunEntryFlags", sample.receiverRunEntryFlags},
      {"receiverActiveStrengthMilli", sample.receiverActiveStrengthMilli},
      {"receiverCsmCascadeCount", sample.receiverCsmCascadeCount},
      {"receiverHoldInvalidCsm", sample.receiverHoldInvalidCsm},
      {"receiverHoldEmptyReplay", sample.receiverHoldEmptyReplay},
      {"receiverHoldIdentityChurn", sample.receiverHoldIdentityChurn},
      {"dynamicPoseSignature", sample.dynamicPoseSignature},
      {"submittedIdentityHash", sample.submittedIdentityHash},
      {"lastSubmittedPaletteHash", sample.lastSubmittedPaletteHash},
      {"lastSubmittedGroupHash", sample.lastSubmittedGroupHash},
      {"currentDrawPublishReadyCount", sample.currentDrawPublishReadyCount},
      {"currentDrawQueryHitCount", sample.currentDrawQueryHitCount},
      {"currentDrawLastRenderFrameIndex",
       sample.currentDrawLastRenderFrameIndex},
      {"currentDrawLastFrameTag", sample.currentDrawLastFrameTag},
      {"submitPaletteContentAgeSampleCount",
       sample.submitPaletteContentAgeSampleCount},
      {"submitPaletteContentAgeLag3PlusCount",
       sample.submitPaletteContentAgeLag3PlusCount},
      {"shadowMatrixSceneKey", sample.shadowMatrixSceneKey},
      {"shadowMatrixUploadSerial", sample.shadowMatrixUploadSerial},
      {"shadowMatrixBufferObjectPtr", sample.shadowMatrixBufferObjectPtr},
      {"shadowMatrixBufferOffset", sample.shadowMatrixBufferOffset},
      {"shadowMatrixBufferSize", sample.shadowMatrixBufferSize},
      {"shadowMatrixBufferGpuAddress", sample.shadowMatrixBufferGpuAddress},
      {"receiverCameraHash", sample.receiverCameraHash},
      {"receiverSunDirectionHash", sample.receiverSunDirectionHash},
      {"receiverCsmHash", sample.receiverCsmHash},
      {"receiverCameraDeltaNano", sample.receiverCameraDeltaNano},
      {"receiverSunDeltaNano", sample.receiverSunDeltaNano},
      {"receiverCsmDeltaNano", sample.receiverCsmDeltaNano},
      {"receiverSnappedCenterDeltaTexelsNano",
       sample.receiverSnappedCenterDeltaTexelsNano},
      {"receiverTexelSizeDeltaNano", sample.receiverTexelSizeDeltaNano},
      {"replayBackingHash", sample.replayBackingHash},
      {"stage13ReplayContentHash", sample.stage13ReplayContentHash},
      {"stage13ReplayBackingHash", sample.stage13ReplayBackingHash},
      {"stage13ReplayDrawCount", sample.stage13ReplayDrawCount},
      {"shadowMapRenderSerial", sample.shadowMapRenderSerial},
      {"shadowMapImagePtr", sample.shadowMapImagePtr},
      {"shadowMapSampleViewPtr", sample.shadowMapSampleViewPtr},
      {"shadowCurrentImagePtr", sample.shadowCurrentImagePtr},
      {"shadowCurrentViewPtr", sample.shadowCurrentViewPtr},
      {"shadowHistoryReadImagePtr", sample.shadowHistoryReadImagePtr},
      {"shadowHistoryReadViewPtr", sample.shadowHistoryReadViewPtr},
      {"shadowHistoryWriteImagePtr", sample.shadowHistoryWriteImagePtr},
      {"shadowHistoryWriteViewPtr", sample.shadowHistoryWriteViewPtr},
      {"shadowVisibilityExecutedThisFrame",
       sample.shadowVisibilityExecutedThisFrame},
      {"receiverDrawExecutedThisFrame", sample.receiverDrawExecutedThisFrame},
      {"shadowTaaMode", sample.shadowTaaMode},
      {"shadowHistoryValidBefore", sample.shadowHistoryValidBefore},
      {"shadowHistoryValidAfter", sample.shadowHistoryValidAfter},
      {"shadowHistoryReadIndex", sample.shadowHistoryReadIndex},
      {"shadowHistoryWriteIndex", sample.shadowHistoryWriteIndex},
      {"shadowHistoryAdvancedThisFrame",
       sample.shadowHistoryAdvancedThisFrame},
      {"shadowHistoryAdvanceSkippedIncomplete",
       sample.shadowHistoryAdvanceSkippedIncomplete},
      {"shadowHistoryInvalidationMask",
       sample.shadowHistoryInvalidationMask},
      {"shadowReceiverSampleSource", sample.shadowReceiverSampleSource},
  };
}

json ToJson(const render::ShadowPoseFullTraceStatus& status) {
  return json{
      {"enabled", status.enabled},
      {"active", status.active},
      {"includePoseRecords", status.includePoseRecords},
      {"includeShadowObjectRecords", status.includeShadowObjectRecords},
      {"includeCurrentDrawRecords", status.includeCurrentDrawRecords},
      {"includeFinalCasterRecords", status.includeFinalCasterRecords},
      {"includeMatrixBytes", status.includeMatrixBytes},
      {"stoppedByLimit", status.stoppedByLimit},
      {"maxSeconds", status.maxSeconds},
      {"maxPoseRecords", status.maxPoseRecords},
      {"maxShadowObjectRecords", status.maxShadowObjectRecords},
      {"maxCurrentDrawRecords", status.maxCurrentDrawRecords},
      {"maxFinalCasterRecords", status.maxFinalCasterRecords},
      {"traceEpoch", status.traceEpoch},
      {"frameEventsWritten", status.frameEventsWritten},
      {"recordEventsWritten", status.recordEventsWritten},
      {"path", status.path},
  };
}

json ToJson(const render::ShadowRuntimeBridgeSummary& summary) {
  json cadenceSamples = json::array();
  const uint32_t cadenceSampleCount =
      std::min<uint32_t>(summary.shadowCadenceSampleCount,
                         dxvk::war3::render::
                             kShadowRuntimeCadenceSampleCapacity);
  for (uint32_t i = 0u; i < cadenceSampleCount; ++i)
    cadenceSamples.push_back(ToJson(summary.shadowCadenceSamples[i]));

  return json{
      {"modelRegistryCount", summary.modelRegistryCount},
      {"instanceRegistryCount", summary.instanceRegistryCount},
      {"runtimeBoundCount", summary.runtimeBoundCount},
      {"runtimeOwnerIdentityCount", summary.runtimeOwnerIdentityCount},
      {"completeIdentityCount", summary.completeIdentityCount},
      {"poseReadyCount", summary.poseReadyCount},
      {"spriteFramePoseCount", summary.spriteFramePoseCount},
      {"matrixPaletteCount", summary.matrixPaletteCount},
      {"shadowGeosetResourceCount", summary.shadowGeosetResourceCount},
      {"shadowReadyGeosetCount", summary.shadowReadyGeosetCount},
      {"shadowModelResourceCount", summary.shadowModelResourceCount},
      {"shadowRuntimeModelCount", summary.shadowRuntimeModelCount},
      {"visibleRenderableCount", summary.visibleRenderableCount},
      {"visibleRenderableMainCount", summary.visibleRenderableMainCount},
      {"visibleRenderableTransparentCount",
       summary.visibleRenderableTransparentCount},
      {"semanticStaticCandidateCount",
       summary.semanticStaticCandidateCount},
      {"semanticStaticCandidateBuildingCount",
       summary.semanticStaticCandidateBuildingCount},
      {"semanticStaticCandidateDestructibleCount",
       summary.semanticStaticCandidateDestructibleCount},
      {"semanticStaticCandidateMaybeDoodadOrEffectCount",
       summary.semanticStaticCandidateMaybeDoodadOrEffectCount},
      {"semanticStaticCandidateWithStableIdentity",
       summary.semanticStaticCandidateWithStableIdentity},
      {"semanticStaticCandidateWithMeshData",
       summary.semanticStaticCandidateWithMeshData},
      {"semanticStaticCandidateWithRuntimeModel",
       summary.semanticStaticCandidateWithRuntimeModel},
      {"semanticStaticCandidateWithModelResource",
       summary.semanticStaticCandidateWithModelResource},
      {"semanticStaticCandidateWithResolvedGeoset",
       summary.semanticStaticCandidateWithResolvedGeoset},
      {"semanticStaticCandidateRejectedUnitsOnlyFilter",
       summary.semanticStaticCandidateRejectedUnitsOnlyFilter},
      {"semanticStaticCandidateRejectedNoIdentity",
       summary.semanticStaticCandidateRejectedNoIdentity},
      {"semanticStaticCandidateRejectedNoMeshData",
       summary.semanticStaticCandidateRejectedNoMeshData},
      {"semanticStaticCandidateRejectedNoResource",
       summary.semanticStaticCandidateRejectedNoResource},
      {"semanticStaticCandidateRejectedNoGeoset",
       summary.semanticStaticCandidateRejectedNoGeoset},
      {"semanticStaticCandidateRejectedNonCanonicalKind",
       summary.semanticStaticCandidateRejectedNonCanonicalKind},
      {"visibleRenderableSample0WorldObjectEntryPtr",
       summary.visibleRenderableSample0WorldObjectEntryPtr},
      {"visibleRenderableSample0SceneNodePtr",
       summary.visibleRenderableSample0SceneNodePtr},
      {"visibleRenderableSample0UnitPtr",
       summary.visibleRenderableSample0UnitPtr},
      {"visibleRenderableSample0JHandle",
       summary.visibleRenderableSample0JHandle},
      {"visibleRenderableSample0Rawcode",
       summary.visibleRenderableSample0Rawcode},
      {"visibleRenderableSample0RuntimeModelPtr",
       summary.visibleRenderableSample0RuntimeModelPtr},
      {"visibleRenderableSample0ModelResourcePtr",
       summary.visibleRenderableSample0ModelResourcePtr},
      {"visibleRenderableSample0RuntimeGeosetPtr",
       summary.visibleRenderableSample0RuntimeGeosetPtr},
      {"visibleRenderableSample0RuntimeGeosetDataPtr",
       summary.visibleRenderableSample0RuntimeGeosetDataPtr},
      {"visibleRenderableSample0SceneInstanceRuntimeModelPtr",
       summary.visibleRenderableSample0SceneInstanceRuntimeModelPtr},
      {"visibleRenderableSample0SceneInstanceModelResourcePtr",
       summary.visibleRenderableSample0SceneInstanceModelResourcePtr},
      {"visibleRenderableSample0SceneShadowRuntimeModelPtr",
       summary.visibleRenderableSample0SceneShadowRuntimeModelPtr},
      {"visibleRenderableSample0SceneShadowModelResourcePtr",
       summary.visibleRenderableSample0SceneShadowModelResourcePtr},
      {"visibleRenderableSample0ScenePoseMatrixCount",
       summary.visibleRenderableSample0ScenePoseMatrixCount},
      {"visibleRenderableSample0GeosetModelResourcePtr",
       summary.visibleRenderableSample0GeosetModelResourcePtr},
      {"visibleRenderableSample0GeosetModelKey",
       summary.visibleRenderableSample0GeosetModelKey},
      {"visibleRenderableSample1WorldObjectEntryPtr",
       summary.visibleRenderableSample1WorldObjectEntryPtr},
      {"visibleRenderableSample1SceneNodePtr",
       summary.visibleRenderableSample1SceneNodePtr},
      {"visibleRenderableSample1UnitPtr",
       summary.visibleRenderableSample1UnitPtr},
      {"visibleRenderableSample1JHandle",
       summary.visibleRenderableSample1JHandle},
      {"visibleRenderableSample1Rawcode",
       summary.visibleRenderableSample1Rawcode},
      {"visibleRenderableSample1RuntimeModelPtr",
       summary.visibleRenderableSample1RuntimeModelPtr},
      {"visibleRenderableSample1ModelResourcePtr",
       summary.visibleRenderableSample1ModelResourcePtr},
      {"visibleRenderableSample1RuntimeGeosetPtr",
       summary.visibleRenderableSample1RuntimeGeosetPtr},
      {"visibleRenderableSample1RuntimeGeosetDataPtr",
       summary.visibleRenderableSample1RuntimeGeosetDataPtr},
      {"visibleRenderableSample1SceneInstanceRuntimeModelPtr",
       summary.visibleRenderableSample1SceneInstanceRuntimeModelPtr},
      {"visibleRenderableSample1SceneInstanceModelResourcePtr",
       summary.visibleRenderableSample1SceneInstanceModelResourcePtr},
      {"visibleRenderableSample1SceneShadowRuntimeModelPtr",
       summary.visibleRenderableSample1SceneShadowRuntimeModelPtr},
      {"visibleRenderableSample1SceneShadowModelResourcePtr",
       summary.visibleRenderableSample1SceneShadowModelResourcePtr},
      {"visibleRenderableSample1ScenePoseMatrixCount",
       summary.visibleRenderableSample1ScenePoseMatrixCount},
      {"visibleRenderableSample1GeosetModelResourcePtr",
       summary.visibleRenderableSample1GeosetModelResourcePtr},
      {"visibleRenderableSample1GeosetModelKey",
       summary.visibleRenderableSample1GeosetModelKey},
      {"sceneCollectorGroupLocalAggregationEnabled",
       summary.sceneCollectorGroupLocalAggregationEnabled},
      {"worldObjectListEntryCount", summary.worldObjectListEntryCount},
      {"worldObjectListNullEntryCount", summary.worldObjectListNullEntryCount},
      {"worldObjectListOwnerHintZeroCount",
       summary.worldObjectListOwnerHintZeroCount},
      {"worldObjectListOwnerHintNonzeroCount",
       summary.worldObjectListOwnerHintNonzeroCount},
      {"worldObjectListOwnerHintHandleCount",
       summary.worldObjectListOwnerHintHandleCount},
      {"worldObjectListOwnerHintUnitPtrCount",
       summary.worldObjectListOwnerHintUnitPtrCount},
      {"worldObjectListOwnerHintZeroContextAcceptedCount",
       summary.worldObjectListOwnerHintZeroContextAcceptedCount},
      {"worldObjectListAcceptedIdentityCount",
       summary.worldObjectListAcceptedIdentityCount},
      {"lastWorldObjectListEntryWorldObjectEntryPtr",
       summary.lastWorldObjectListEntryWorldObjectEntryPtr},
      {"lastWorldObjectListEntryOwnerHintValue",
       summary.lastWorldObjectListEntryOwnerHintValue},
      {"lastWorldObjectListEntrySceneNodePtr",
       summary.lastWorldObjectListEntrySceneNodePtr},
      {"renderIdentityFullDiagnostics",
       summary.renderIdentityFullDiagnostics},
      {"worldObjectListEntryWriteProbeHookInstalled",
       summary.worldObjectListEntryWriteProbeHookInstalled},
      {"worldObjectEntryRenderContextHookInstalled",
       summary.worldObjectEntryRenderContextHookInstalled},
      {"worldObjectEntryRenderPrePostProbeEnabled",
       summary.worldObjectEntryRenderPrePostProbeEnabled},
      {"renderQueueIdentityPrimingHookInstalled",
       summary.renderQueueIdentityPrimingHookInstalled},
      {"worldObjectEntryRenderCallCount",
       summary.worldObjectEntryRenderCallCount},
      {"worldObjectEntryRenderSceneNodeReadyBeforeCount",
       summary.worldObjectEntryRenderSceneNodeReadyBeforeCount},
      {"worldObjectEntryRenderSceneNodeReadyAfterCount",
       summary.worldObjectEntryRenderSceneNodeReadyAfterCount},
      {"worldObjectEntryRenderSceneNodeFilledByCallCount",
       summary.worldObjectEntryRenderSceneNodeFilledByCallCount},
      {"worldObjectEntryRenderSceneNodeChangedCount",
       summary.worldObjectEntryRenderSceneNodeChangedCount},
      {"worldObjectEntryRenderKnownListOwnerHintZeroCount",
       summary.worldObjectEntryRenderKnownListOwnerHintZeroCount},
      {"worldObjectEntryRenderKnownListOwnerHintNonzeroCount",
       summary.worldObjectEntryRenderKnownListOwnerHintNonzeroCount},
      {"worldObjectEntryRenderUnknownListOwnerHintCount",
       summary.worldObjectEntryRenderUnknownListOwnerHintCount},
      {"worldObjectListEntryWriteCallCount",
       summary.worldObjectListEntryWriteCallCount},
      {"worldObjectListEntryWriteOwnerHintZeroCount",
       summary.worldObjectListEntryWriteOwnerHintZeroCount},
      {"worldObjectListEntryWriteOwnerHintNonzeroCount",
       summary.worldObjectListEntryWriteOwnerHintNonzeroCount},
      {"worldObjectListEntryWriteOwnerHintHandleCount",
       summary.worldObjectListEntryWriteOwnerHintHandleCount},
      {"worldObjectListEntryWriteOwnerHintUnitPtrCount",
       summary.worldObjectListEntryWriteOwnerHintUnitPtrCount},
      {"lastWorldObjectEntryRenderEntryPtr",
       summary.lastWorldObjectEntryRenderEntryPtr},
      {"lastWorldObjectEntryRenderResolvedListOwnerHintValue",
       summary.lastWorldObjectEntryRenderResolvedListOwnerHintValue},
      {"lastWorldObjectListEntryWriteListPtr",
       summary.lastWorldObjectListEntryWriteListPtr},
      {"lastWorldObjectListEntryWriteWorldObjectEntryPtr",
       summary.lastWorldObjectListEntryWriteWorldObjectEntryPtr},
      {"lastWorldObjectListEntryWriteOwnerHintValue",
       summary.lastWorldObjectListEntryWriteOwnerHintValue},
      {"lastWorldObjectEntryRenderSceneNodeBeforePtr",
       summary.lastWorldObjectEntryRenderSceneNodeBeforePtr},
      {"lastWorldObjectEntryRenderSceneNodeAfterPtr",
       summary.lastWorldObjectEntryRenderSceneNodeAfterPtr},
      {"shadowRuntimeBoundCount", summary.shadowRuntimeBoundCount},
      {"shadowIdentityCount", summary.shadowIdentityCount},
      {"shadowPoseReadyCount", summary.shadowPoseReadyCount},
      {"runtimeCreationProvenanceCount",
       summary.runtimeCreationProvenanceCount},
      {"runtimeResolveProvenanceCount",
       summary.runtimeResolveProvenanceCount},
      {"runtimeSourceObjectCount", summary.runtimeSourceObjectCount},
      {"runtimeModelCtorCount", summary.runtimeModelCtorCount},
      {"runtimeModelComplexCtorCount", summary.runtimeModelComplexCtorCount},
      {"runtimeModelPlainCtorCount", summary.runtimeModelPlainCtorCount},
      {"runtimeModelCtorCallerPromoteCount",
       summary.runtimeModelCtorCallerPromoteCount},
      {"runtimeModelCtorCallerOtherCount",
       summary.runtimeModelCtorCallerOtherCount},
      {"runtimeModelCreateCount", summary.runtimeModelCreateCount},
      {"runtimeModelResolveCount", summary.runtimeModelResolveCount},
      {"runtimeModelResolveResolvedIdentityCount",
       summary.runtimeModelResolveResolvedIdentityCount},
      {"runtimeModelCreateCallerBuildChildLinksCount",
       summary.runtimeModelCreateCallerBuildChildLinksCount},
      {"runtimeModelCreateCallerCreateSpriteRuntimeCount",
       summary.runtimeModelCreateCallerCreateSpriteRuntimeCount},
      {"runtimeModelCreateCallerOtherCount",
       summary.runtimeModelCreateCallerOtherCount},
      {"runtimeModelInitCopyCount", summary.runtimeModelInitCopyCount},
      {"runtimeModelInitCopyPublishedFallbackCount",
       summary.runtimeModelInitCopyPublishedFallbackCount},
      {"attachmentChildLineageBootstrapAttemptCount",
       summary.attachmentChildLineageBootstrapAttemptCount},
      {"attachmentChildLineageBootstrapSuccessCount",
       summary.attachmentChildLineageBootstrapSuccessCount},
      {"attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount",
       summary.attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount},
      {"attachmentChildLineageBootstrapMissNoModelDataLinksCount",
       summary.attachmentChildLineageBootstrapMissNoModelDataLinksCount},
      {"attachmentChildLineageBootstrapMissNoUniqueChildCount",
       summary.attachmentChildLineageBootstrapMissNoUniqueChildCount},
      {"runtimeChildLinkBuildCount", summary.runtimeChildLinkBuildCount},
      {"runtimeChildLinkBuiltChildCount",
       summary.runtimeChildLinkBuiltChildCount},
      {"runtimeChildBuildTimeDirectPublishCount",
       summary.runtimeChildBuildTimeDirectPublishCount},
      {"runtimeChildBuildTimeDirectPublishWithResourceCount",
       summary.runtimeChildBuildTimeDirectPublishWithResourceCount},
      {"runtimeChildBuildModelDataPreLinkCount",
       summary.runtimeChildBuildModelDataPreLinkCount},
      {"runtimeChildBuildModelDataPostLinkCount",
       summary.runtimeChildBuildModelDataPostLinkCount},
      {"runtimeChildBuildModelDataPreUnreadableLinkCount",
       summary.runtimeChildBuildModelDataPreUnreadableLinkCount},
      {"runtimeChildBuildModelDataPostUnreadableLinkCount",
       summary.runtimeChildBuildModelDataPostUnreadableLinkCount},
      {"runtimeMatrixRangeCopyCount", summary.runtimeMatrixRangeCopyCount},
      {"runtimeMatrixFlushCount", summary.runtimeMatrixFlushCount},
      {"runtimeMatrixPublisherPaletteReadyCount",
       summary.runtimeMatrixPublisherPaletteReadyCount},
      {"runtimePoseUpdatePalettePublishCount",
       summary.runtimePoseUpdatePalettePublishCount},
      {"runtimePoseUpdateLastRuntimeModelPtr",
       summary.runtimePoseUpdateLastRuntimeModelPtr},
      {"runtimePoseUpdateLastMatrixCount",
       summary.runtimePoseUpdateLastMatrixCount},
      {"runtimePoseUpdateLastMatrixHash",
       summary.runtimePoseUpdateLastMatrixHash},
      {"runtimeMatrixWriteCount", summary.runtimeMatrixWriteCount},
      {"runtimeMatrixWritePublishCount",
       summary.runtimeMatrixWritePublishCount},
      {"runtimeMatrixWriteMissCount", summary.runtimeMatrixWriteMissCount},
      {"runtimeGroupPaletteWrapperCallCount",
       summary.runtimeGroupPaletteWrapperCallCount},
      {"runtimeGroupPaletteWrapperPartCount",
       summary.runtimeGroupPaletteWrapperPartCount},
      {"runtimeGroupPaletteWrapperBindingCount",
       summary.runtimeGroupPaletteWrapperBindingCount},
      {"runtimeSimpleGroupPaletteCallCount",
       summary.runtimeSimpleGroupPaletteCallCount},
      {"runtimeSimpleGroupPaletteSlotCapturedCount",
       summary.runtimeSimpleGroupPaletteSlotCapturedCount},
      {"runtimeSimpleGroupPaletteSlotUnreadableCount",
       summary.runtimeSimpleGroupPaletteSlotUnreadableCount},
      {"renderablePartPaletteBindingQueryHitCount",
       summary.renderablePartPaletteBindingQueryHitCount},
      {"renderablePartPaletteBindingQueryMissCount",
       summary.renderablePartPaletteBindingQueryMissCount},
      {"renderablePartPaletteSnapshotCapturedCount",
       summary.renderablePartPaletteSnapshotCapturedCount},
      {"renderablePartPaletteSnapshotTooLargeCount",
       summary.renderablePartPaletteSnapshotTooLargeCount},
      {"renderablePartPaletteSnapshotUnreadableCount",
       summary.renderablePartPaletteSnapshotUnreadableCount},
      {"renderablePartPaletteSnapshotQueryHitCount",
       summary.renderablePartPaletteSnapshotQueryHitCount},
      {"renderablePartPaletteSnapshotQueryMissCount",
       summary.renderablePartPaletteSnapshotQueryMissCount},
      // Phase 7.47 dt gate probe
      {"spriteUberPreRenderTotalCount",
       summary.spriteUberPreRenderTotalCount},
      {"spriteUberPreRenderDtZeroCount",
       summary.spriteUberPreRenderDtZeroCount},
      {"spriteUberPreRenderDtBelowEpsilonCount",
       summary.spriteUberPreRenderDtBelowEpsilonCount},
      {"spriteUberPreRenderDtPositiveCount",
       summary.spriteUberPreRenderDtPositiveCount},
      {"spriteUberPreRenderDtNegativeCount",
       summary.spriteUberPreRenderDtNegativeCount},
      {"spriteUberPreRenderLastDtBits",
       summary.spriteUberPreRenderLastDtBits},
      {"spriteUberPreRenderLastZeroDtFrameTag",
       summary.spriteUberPreRenderLastZeroDtFrameTag},
      {"spriteUberPreRenderLastPositiveDtFrameTag",
       summary.spriteUberPreRenderLastPositiveDtFrameTag},
      {"runtimeMatrixWriteFramesWithHitCount",
       summary.runtimeMatrixWriteFramesWithHitCount},
      {"runtimeMatrixWriteFramesEmptyCount",
       summary.runtimeMatrixWriteFramesEmptyCount},
      {"runtimeGroupPaletteWrapperFramesWithHitCount",
       summary.runtimeGroupPaletteWrapperFramesWithHitCount},
      {"runtimeGroupPaletteWrapperFramesEmptyCount",
       summary.runtimeGroupPaletteWrapperFramesEmptyCount},
      {"runtimeSimpleGroupPaletteFramesWithHitCount",
       summary.runtimeSimpleGroupPaletteFramesWithHitCount},
      {"runtimeSimpleGroupPaletteFramesEmptyCount",
       summary.runtimeSimpleGroupPaletteFramesEmptyCount},
      {"runtimeMatrixWriteLastRuntimeModelPtr",
       summary.runtimeMatrixWriteLastRuntimeModelPtr},
      {"runtimeMatrixWriteLastMatrixIndex",
       summary.runtimeMatrixWriteLastMatrixIndex},
      {"runtimeMatrixWriteLastMatrixCount",
       summary.runtimeMatrixWriteLastMatrixCount},
      {"runtimeMatrixWriteLastMatrixHash",
       summary.runtimeMatrixWriteLastMatrixHash},
      {"runtimeMatrixRangeCopyPalettePublishHitCount",
       summary.runtimeMatrixRangeCopyPalettePublishHitCount},
      {"runtimeMatrixRangeCopyPalettePublishMissCount",
       summary.runtimeMatrixRangeCopyPalettePublishMissCount},
      {"runtimeMatrixRangeCopyPaletteFallbackCModelCount",
       summary.runtimeMatrixRangeCopyPaletteFallbackCModelCount},
      {"runtimeMatrixFlushPaletteSuppressedCount",
       summary.runtimeMatrixFlushPaletteSuppressedCount},
      {"runtimeMatrixRangeCopyLastRuntimeModelPtr",
       summary.runtimeMatrixRangeCopyLastRuntimeModelPtr},
      {"runtimeMatrixRangeCopyLastContextPtr",
       summary.runtimeMatrixRangeCopyLastContextPtr},
      {"runtimeMatrixRangeCopyLastSourceBasePtr",
       summary.runtimeMatrixRangeCopyLastSourceBasePtr},
      {"runtimeMatrixRangeCopyLastMatrixCount",
       summary.runtimeMatrixRangeCopyLastMatrixCount},
      {"runtimeMatrixRangeCopyLastMatrixHash",
       summary.runtimeMatrixRangeCopyLastMatrixHash},
      {"runtimeMatrixPublisherAttachmentRootHitCount",
       summary.runtimeMatrixPublisherAttachmentRootHitCount},
      {"runtimeMatrixPublisherAttachmentOwnerHitCount",
       summary.runtimeMatrixPublisherAttachmentOwnerHitCount},
      {"runtimeMatrixPublisherAttachmentChildHitCount",
       summary.runtimeMatrixPublisherAttachmentChildHitCount},
      {"runtimeMatrixPublisherAttachmentAliasHitCount",
       summary.runtimeMatrixPublisherAttachmentAliasHitCount},
      {"runtimeMatrixPublisherAttachmentRootPaletteReadyCount",
       summary.runtimeMatrixPublisherAttachmentRootPaletteReadyCount},
      {"runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount",
       summary.runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount},
      {"runtimeMatrixPublisherAttachmentChildPaletteReadyCount",
       summary.runtimeMatrixPublisherAttachmentChildPaletteReadyCount},
      {"attachmentAncestorIdentityHintWriteCount",
       summary.attachmentAncestorIdentityHintWriteCount},
      {"attachmentRigidCount", summary.attachmentRigidCount},
      {"attachmentRigidCountWithSourceObject",
       summary.attachmentRigidCountWithSourceObject},
      {"attachmentRigidCountWithAnyIdentity",
       summary.attachmentRigidCountWithAnyIdentity},
      {"attachmentRigidCountWithWorldObjectEntry",
       summary.attachmentRigidCountWithWorldObjectEntry},
      {"attachmentRigidCountWithSceneNode",
       summary.attachmentRigidCountWithSceneNode},
      {"attachmentRigidCountWithUnitPtr",
       summary.attachmentRigidCountWithUnitPtr},
      {"attachmentRigidChildRuntimeCreateCallerKnownCount",
       summary.attachmentRigidChildRuntimeCreateCallerKnownCount},
      {"attachmentRigidOwnerRuntimeCreateCallerKnownCount",
       summary.attachmentRigidOwnerRuntimeCreateCallerKnownCount},
      {"attachmentRigidRootRuntimeCreateCallerKnownCount",
       summary.attachmentRigidRootRuntimeCreateCallerKnownCount},
      {"attachmentRigidChildRuntimeCreateHandleKnownCount",
       summary.attachmentRigidChildRuntimeCreateHandleKnownCount},
      {"attachmentRigidOwnerRuntimeCreateHandleKnownCount",
       summary.attachmentRigidOwnerRuntimeCreateHandleKnownCount},
      {"attachmentRigidRootRuntimeCreateHandleKnownCount",
       summary.attachmentRigidRootRuntimeCreateHandleKnownCount},
      {"attachmentRigidChildRuntimeResolveCallerKnownCount",
       summary.attachmentRigidChildRuntimeResolveCallerKnownCount},
      {"attachmentRigidOwnerRuntimeResolveCallerKnownCount",
       summary.attachmentRigidOwnerRuntimeResolveCallerKnownCount},
      {"attachmentRigidRootRuntimeResolveCallerKnownCount",
       summary.attachmentRigidRootRuntimeResolveCallerKnownCount},
      {"attachmentRigidChildRuntimeOwnerIdentityCount",
       summary.attachmentRigidChildRuntimeOwnerIdentityCount},
      {"attachmentRigidOwnerRuntimeOwnerIdentityCount",
       summary.attachmentRigidOwnerRuntimeOwnerIdentityCount},
      {"attachmentRigidRootRuntimeOwnerIdentityCount",
       summary.attachmentRigidRootRuntimeOwnerIdentityCount},
      {"attachmentRigidOwnerRuntimeRecordKnownCount",
       summary.attachmentRigidOwnerRuntimeRecordKnownCount},
      {"attachmentRigidRootRuntimeRecordKnownCount",
       summary.attachmentRigidRootRuntimeRecordKnownCount},
      {"attachmentRigidChildRuntimeParentLinkKnownCount",
       summary.attachmentRigidChildRuntimeParentLinkKnownCount},
      {"attachmentRigidChildRuntimeRecordKnownCount",
       summary.attachmentRigidChildRuntimeRecordKnownCount},
      {"attachmentRigidChildRuntimeModelResourceKnownCount",
       summary.attachmentRigidChildRuntimeModelResourceKnownCount},
      {"attachmentRigidChildRuntimePoseKnownCount",
       summary.attachmentRigidChildRuntimePoseKnownCount},
      {"attachmentRigidChildRuntimeMatchesAttachedEffectInitCount",
       summary.attachmentRigidChildRuntimeMatchesAttachedEffectInitCount},
      {"attachmentRigidChildRuntimeMatchesAttachModelToPointCount",
       summary.attachmentRigidChildRuntimeMatchesAttachModelToPointCount},
      {"contractAttachmentRigidCount",
       summary.contractAttachmentRigidCount},
      {"contractAttachmentRigidCountWithSourceObject",
       summary.contractAttachmentRigidCountWithSourceObject},
      {"contractAttachmentRigidCountWithAnyIdentity",
       summary.contractAttachmentRigidCountWithAnyIdentity},
      {"contractAttachmentRigidCountWithWorldObjectEntry",
       summary.contractAttachmentRigidCountWithWorldObjectEntry},
      {"contractAttachmentRigidCountWithSceneNode",
       summary.contractAttachmentRigidCountWithSceneNode},
      {"contractAttachmentRigidCountWithUnitPtr",
       summary.contractAttachmentRigidCountWithUnitPtr},
      {"contractAttachmentRigidChildRuntimeCreateCallerKnownCount",
       summary.contractAttachmentRigidChildRuntimeCreateCallerKnownCount},
      {"contractAttachmentRigidOwnerRuntimeCreateCallerKnownCount",
       summary.contractAttachmentRigidOwnerRuntimeCreateCallerKnownCount},
      {"contractAttachmentRigidRootRuntimeCreateCallerKnownCount",
       summary.contractAttachmentRigidRootRuntimeCreateCallerKnownCount},
      {"contractAttachmentRigidChildRuntimeCreateHandleKnownCount",
       summary.contractAttachmentRigidChildRuntimeCreateHandleKnownCount},
      {"contractAttachmentRigidOwnerRuntimeCreateHandleKnownCount",
       summary.contractAttachmentRigidOwnerRuntimeCreateHandleKnownCount},
      {"contractAttachmentRigidRootRuntimeCreateHandleKnownCount",
       summary.contractAttachmentRigidRootRuntimeCreateHandleKnownCount},
      {"contractAttachmentRigidChildRuntimeResolveCallerKnownCount",
       summary.contractAttachmentRigidChildRuntimeResolveCallerKnownCount},
      {"contractAttachmentRigidOwnerRuntimeResolveCallerKnownCount",
       summary.contractAttachmentRigidOwnerRuntimeResolveCallerKnownCount},
      {"contractAttachmentRigidRootRuntimeResolveCallerKnownCount",
       summary.contractAttachmentRigidRootRuntimeResolveCallerKnownCount},
      {"contractAttachmentRigidChildRuntimeOwnerIdentityCount",
       summary.contractAttachmentRigidChildRuntimeOwnerIdentityCount},
      {"contractAttachmentRigidOwnerRuntimeOwnerIdentityCount",
       summary.contractAttachmentRigidOwnerRuntimeOwnerIdentityCount},
      {"contractAttachmentRigidRootRuntimeOwnerIdentityCount",
       summary.contractAttachmentRigidRootRuntimeOwnerIdentityCount},
      {"contractAttachmentRigidOwnerRuntimeRecordKnownCount",
       summary.contractAttachmentRigidOwnerRuntimeRecordKnownCount},
      {"contractAttachmentRigidRootRuntimeRecordKnownCount",
       summary.contractAttachmentRigidRootRuntimeRecordKnownCount},
      {"contractAttachmentRigidChildRuntimeParentLinkKnownCount",
       summary.contractAttachmentRigidChildRuntimeParentLinkKnownCount},
      {"contractAttachmentRigidChildRuntimeRecordKnownCount",
       summary.contractAttachmentRigidChildRuntimeRecordKnownCount},
      {"contractAttachmentRigidChildRuntimeModelResourceKnownCount",
       summary.contractAttachmentRigidChildRuntimeModelResourceKnownCount},
      {"contractAttachmentRigidChildRuntimePoseKnownCount",
       summary.contractAttachmentRigidChildRuntimePoseKnownCount},
      {"contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount",
       summary
           .contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount},
      {"contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount",
       summary
           .contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount},
      {"upperLayerResolveAttempts", summary.upperLayerResolveAttempts},
      {"upperLayerResolveVisibleMiss", summary.upperLayerResolveVisibleMiss},
      {"upperLayerResolveVisibleUnresolvedGeoset",
       summary.upperLayerResolveVisibleUnresolvedGeoset},
      {"upperLayerResolveGeosetMiss", summary.upperLayerResolveGeosetMiss},
      {"upperLayerResolvePoseMiss", summary.upperLayerResolvePoseMiss},
      {"upperLayerResolveRuntimeGroupPaletteMiss",
       summary.upperLayerResolveRuntimeGroupPaletteMiss},
      {"upperLayerResolveAuthoritativeRigid",
       summary.upperLayerResolveAuthoritativeRigid},
      {"upperLayerResolveAuthoritativeSkinned",
       summary.upperLayerResolveAuthoritativeSkinned},
      {"upperLayerResolvedAuthoritativeItems",
       summary.upperLayerResolvedAuthoritativeItems},
      {"upperLayerEmitted", summary.upperLayerEmitted},
      {"upperLayerDuplicateOrSuppressed",
       summary.upperLayerDuplicateOrSuppressed},
      {"semanticDataModuleEnabled", summary.semanticDataModuleEnabled},
      {"semanticModelProducerEnabled",
       summary.semanticModelProducerEnabled},
      {"semanticPoseProducerEnabled", summary.semanticPoseProducerEnabled},
      {"semanticAttachmentProducerEnabled",
       summary.semanticAttachmentProducerEnabled},
      {"semanticFrameRegistriesEnabled",
       summary.semanticFrameRegistriesEnabled},
      {"semanticContractCaptureEnabled",
       summary.semanticContractCaptureEnabled},
      {"semanticConsumerEnabled", summary.semanticConsumerEnabled},
      {"semanticBuildSkippedReason", summary.semanticBuildSkippedReason},
      {"semanticModelHookCalls", summary.semanticModelHookCalls},
      {"semanticModelHookUs", summary.semanticModelHookUs},
      {"semanticPoseHookCalls", summary.semanticPoseHookCalls},
      {"semanticPoseHookUs", summary.semanticPoseHookUs},
      {"semanticAttachmentHookCalls", summary.semanticAttachmentHookCalls},
      {"semanticAttachmentHookUs", summary.semanticAttachmentHookUs},
      {"semanticFrameRegistryPublishCalls",
       summary.semanticFrameRegistryPublishCalls},
      {"semanticFrameRegistryPublishUs",
       summary.semanticFrameRegistryPublishUs},
      {"semanticContractCaptureCalls",
       summary.semanticContractCaptureCalls},
      {"semanticContractCaptureUs", summary.semanticContractCaptureUs},
      {"semanticContractCaptureSkippedStableSameFrame",
       summary.semanticContractCaptureSkippedStableSameFrame},
      {"semanticContractCaptureSkippedEmpty",
       summary.semanticContractCaptureSkippedEmpty},
      {"semanticContractCaptureSkippedDuplicateSameFrame",
       summary.semanticContractCaptureSkippedDuplicateSameFrame},
      {"widgetIdentityEnterCount",
       summary.widgetIdentityEnterCount},
      {"widgetIdentityMagicMatchedCount",
       summary.widgetIdentityMagicMatchedCount},
      {"widgetIdentityMagicMismatchCount",
       summary.widgetIdentityMagicMismatchCount},
      {"widgetIdentityCacheInsertCount",
       summary.widgetIdentityCacheInsertCount},
      {"widgetIdentityCacheUpdateCount",
       summary.widgetIdentityCacheUpdateCount},
      {"widgetIdentityHandleResolvedCount",
       summary.widgetIdentityHandleResolvedCount},
      {"widgetIdentityHandleMissingCount",
       summary.widgetIdentityHandleMissingCount},
      {"widgetIdentityCacheSize",
       summary.widgetIdentityCacheSize},
      {"widgetIdentityInstallAttempted",
       summary.widgetIdentityInstallAttempted},
      {"widgetIdentityInstallSucceeded",
       summary.widgetIdentityInstallSucceeded},
      {"widgetIdentityInstallFailedAddrNull",
       summary.widgetIdentityInstallFailedAddrNull},
      {"widgetIdentityInstallFailedEnvDisabled",
       summary.widgetIdentityInstallFailedEnvDisabled},
      {"widgetIdentityInstallFailedMinHook",
       summary.widgetIdentityInstallFailedMinHook},
      {"semanticSceneRejectedPathBlockerCount",
       summary.semanticSceneRejectedPathBlockerCount},
      {"semanticSceneRejectedPathBlockerEarlyBypassCount",
       summary.semanticSceneRejectedPathBlockerEarlyBypassCount},
      {"semanticSceneRejectedPathBlockerEligibilityGateCount",
       summary.semanticSceneRejectedPathBlockerEligibilityGateCount},
      {"semanticSceneRejectedPathBlockerAppendEntryCount",
       summary.semanticSceneRejectedPathBlockerAppendEntryCount},
      {"semanticSceneRejectedPathBlockerAppendEntryByJHandleCount",
       summary.semanticSceneRejectedPathBlockerAppendEntryByJHandleCount},
      {"semanticSceneRejectedPathBlockerAppendVbBlendCount",
       summary.semanticSceneRejectedPathBlockerAppendVbBlendCount},
      {"semanticSceneRejectedPathBlockerFastAppendCount",
       summary.semanticSceneRejectedPathBlockerFastAppendCount},
      {"semanticSceneRejectedPathBlockerDirectGroupedCount",
       summary.semanticSceneRejectedPathBlockerDirectGroupedCount},
      {"semanticSceneRejectedPathBlockerProducerCount",
       summary.semanticSceneRejectedPathBlockerProducerCount},
      {"semanticSceneRejectedPathBlockerStaticSupplementCount",
       summary.semanticSceneRejectedPathBlockerStaticSupplementCount},
      {"semanticSceneRejectedPathBlockerLegacyCaptureCount",
       summary.semanticSceneRejectedPathBlockerLegacyCaptureCount},
      {"writeMaskRegionEnterCount",
       summary.writeMaskRegionEnterCount},
      {"writeMaskRegionRejectedIdx3Count",
       summary.writeMaskRegionRejectedIdx3Count},
      {"writeMaskRegionPassFogCount",
       summary.writeMaskRegionPassFogCount},
      {"writeMaskRegionPassLosCount",
       summary.writeMaskRegionPassLosCount},
      {"writeMaskRegionPassPathCount",
       summary.writeMaskRegionPassPathCount},
      {"writeMaskRegionPassOtherCount",
       summary.writeMaskRegionPassOtherCount},
      // Phase 7.112：caller-aware 静态阴影屏蔽。
      {"writeMaskRegionFromBuildingStampCount",
       summary.writeMaskRegionFromBuildingStampCount},
      {"writeMaskRegionRejectedBuildingCount",
       summary.writeMaskRegionRejectedBuildingCount},
      {"writeMaskRegionFromRegisterFootprintCount",
       summary.writeMaskRegionFromRegisterFootprintCount},
      {"writeMaskRegionFromRebuildMaskCount",
       summary.writeMaskRegionFromRebuildMaskCount},
      {"writeMaskRegionFromActorRuntimeCount",
       summary.writeMaskRegionFromActorRuntimeCount},
      {"writeMaskRegionFromForObjectCount",
       summary.writeMaskRegionFromForObjectCount},
      {"writeMaskRegionFromOtherCallerCount",
       summary.writeMaskRegionFromOtherCallerCount},
      // Phase 7.116：DispatchToShape (建筑/装饰物/可破坏物原生静态阴影)。
      {"dispatchToShapeEnterCount",
       summary.dispatchToShapeEnterCount},
      {"dispatchToShapeRejectedCount",
       summary.dispatchToShapeRejectedCount},
      {"dispatchToShapeFromRebuildMaskCount",
       summary.dispatchToShapeFromRebuildMaskCount},
      {"dispatchToShapeFromShadowSetupCount",
       summary.dispatchToShapeFromShadowSetupCount},
      {"dispatchToShapeFromOtherCallerCount",
       summary.dispatchToShapeFromOtherCallerCount},
      // Phase 7.108：ShadowProjector 永久统计。
      {"projectorAddFromObjectEnterCount",
       summary.projectorAddFromObjectEnterCount},
      {"projectorAddFromObjectBlockedCount",
       summary.projectorAddFromObjectBlockedCount},
      {"projectorAddFromObjectFourCCExtractedCount",
       summary.projectorAddFromObjectFourCCExtractedCount},
      {"projectorAddFromObjectFourCCMissCount",
       summary.projectorAddFromObjectFourCCMissCount},
      {"projectorAddFromObjectBlockedFourCCCount",
       summary.projectorAddFromObjectBlockedFourCCCount},
      {"projectorAddSimpleEnterCount",
       summary.projectorAddSimpleEnterCount},
      {"projectorAddSimpleBlockedCount",
       summary.projectorAddSimpleBlockedCount},
      {"projectorObservedFourCCSample0",
       static_cast<uint64_t>(summary.projectorObservedFourCCSamples[0])},
      {"projectorObservedFourCCSample1",
       static_cast<uint64_t>(summary.projectorObservedFourCCSamples[1])},
      {"projectorObservedFourCCSample2",
       static_cast<uint64_t>(summary.projectorObservedFourCCSamples[2])},
      {"projectorObservedFourCCSample3",
       static_cast<uint64_t>(summary.projectorObservedFourCCSamples[3])},
      {"projectorObservedFourCCSample4",
       static_cast<uint64_t>(summary.projectorObservedFourCCSamples[4])},
      {"projectorObservedFourCCSample5",
       static_cast<uint64_t>(summary.projectorObservedFourCCSamples[5])},
      {"projectorObservedFourCCSample6",
       static_cast<uint64_t>(summary.projectorObservedFourCCSamples[6])},
      {"projectorObservedFourCCSample7",
       static_cast<uint64_t>(summary.projectorObservedFourCCSamples[7])},
      {"projectorBlockedFourCCSample0",
       static_cast<uint64_t>(summary.projectorBlockedFourCCSamples[0])},
      {"projectorBlockedFourCCSample1",
       static_cast<uint64_t>(summary.projectorBlockedFourCCSamples[1])},
      {"projectorBlockedFourCCSample2",
       static_cast<uint64_t>(summary.projectorBlockedFourCCSamples[2])},
      {"projectorBlockedFourCCSample3",
       static_cast<uint64_t>(summary.projectorBlockedFourCCSamples[3])},
      // Phase 7.108b：shadowCasters append survey。
      {"shadowAppendTotalCount", summary.shadowAppendTotalCount},
      {"shadowAppendRawcodeUniqueCount",
       summary.shadowAppendRawcodeUniqueCount},
      {"shadowAppendRawcode0",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[0])},
      {"shadowAppendRawcode1",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[1])},
      {"shadowAppendRawcode2",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[2])},
      {"shadowAppendRawcode3",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[3])},
      {"shadowAppendRawcode4",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[4])},
      {"shadowAppendRawcode5",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[5])},
      {"shadowAppendRawcode6",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[6])},
      {"shadowAppendRawcode7",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[7])},
      {"shadowAppendRawcode8",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[8])},
      {"shadowAppendRawcode9",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[9])},
      {"shadowAppendRawcode10",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[10])},
      {"shadowAppendRawcode11",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[11])},
      {"shadowAppendRawcode12",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[12])},
      {"shadowAppendRawcode13",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[13])},
      {"shadowAppendRawcode14",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[14])},
      {"shadowAppendRawcode15",
       static_cast<uint64_t>(summary.shadowAppendRawcodeSamples[15])},
      {"semanticManifestCopyVisibleScanned",
       summary.semanticManifestCopyVisibleScanned},
      {"semanticManifestCopyAppended",
       summary.semanticManifestCopyAppended},
      {"semanticManifestCopyDeduplicatedSkipped",
       summary.semanticManifestCopyDeduplicatedSkipped},
      {"semanticManifestCopyRejectedSkipped",
       summary.semanticManifestCopyRejectedSkipped},
      {"semanticManifestCopySkipStableCount",
       summary.semanticManifestCopySkipStableCount},
      {"semanticManifestCopyEnterCount",
       summary.semanticManifestCopyEnterCount},
      {"semanticManifestCopyMaxScanned",
       summary.semanticManifestCopyMaxScanned},
      {"semanticManifestCopyTotalScanned",
       summary.semanticManifestCopyTotalScanned},
      {"semanticManifestCopyTotalChronoNs",
       summary.semanticManifestCopyTotalChronoNs},
      {"semanticManifestCopyMaxChronoNs",
       summary.semanticManifestCopyMaxChronoNs},
      {"semanticManifestResolveSourceCompleteSkipCount",
       summary.semanticManifestResolveSourceCompleteSkipCount},
      {"semanticManifestResolveLegacyCacheHitCount",
       summary.semanticManifestResolveLegacyCacheHitCount},
      {"semanticManifestResolveRawScanCount",
       summary.semanticManifestResolveRawScanCount},
      {"semanticManifestResolveRawScanEntryVisitCount",
       summary.semanticManifestResolveRawScanEntryVisitCount},
      {"semanticManifestResolveRawScanMissCount",
       summary.semanticManifestResolveRawScanMissCount},
      {"semanticManifestResolveVerifierAttemptCount",
       summary.semanticManifestResolveVerifierAttemptCount},
      {"semanticManifestResolveVerifierMismatchCount",
       summary.semanticManifestResolveVerifierMismatchCount},
      {"semanticManifestResolveMaxRuntimeGeosetCount",
       summary.semanticManifestResolveMaxRuntimeGeosetCount},
      {"semanticManifestModelResourceAttemptCount",
       summary.semanticManifestModelResourceAttemptCount},
      {"semanticManifestModelResourceCacheHitCount",
       summary.semanticManifestModelResourceCacheHitCount},
      {"semanticManifestModelResourceDeepResolveCount",
       summary.semanticManifestModelResourceDeepResolveCount},
      {"semanticManifestModelResourceNullResultCount",
       summary.semanticManifestModelResourceNullResultCount},
      {"semanticManifestModelResourceVerifierAttemptCount",
       summary.semanticManifestModelResourceVerifierAttemptCount},
      {"semanticManifestModelResourceVerifierMismatchCount",
       summary.semanticManifestModelResourceVerifierMismatchCount},
      {"semanticConsumerBuildCalls", summary.semanticConsumerBuildCalls},
      {"semanticConsumerBuildUs", summary.semanticConsumerBuildUs},
      {"semanticConsumerBuildSkippedFresh",
       summary.semanticConsumerBuildSkippedFresh},
      {"semanticLastHotFunctionTag", summary.semanticLastHotFunctionTag},
      {"semanticLastHotFunctionUs", summary.semanticLastHotFunctionUs},
      {"semanticModelBuildChildPreScanCalls",
       summary.semanticModelBuildChildPreScanCalls},
      {"semanticModelBuildChildPreScanUs",
       summary.semanticModelBuildChildPreScanUs},
      {"semanticModelRuntimeChildCollectCalls",
       summary.semanticModelRuntimeChildCollectCalls},
      {"semanticModelRuntimeChildCollectUs",
       summary.semanticModelRuntimeChildCollectUs},
      {"semanticModelRuntimeChildBootstrapCalls",
       summary.semanticModelRuntimeChildBootstrapCalls},
      {"semanticModelRuntimeChildBootstrapUs",
       summary.semanticModelRuntimeChildBootstrapUs},
      {"semanticModelRuntimeChildParentMapCalls",
       summary.semanticModelRuntimeChildParentMapCalls},
      {"semanticModelRuntimeChildParentMapUs",
       summary.semanticModelRuntimeChildParentMapUs},
      {"semanticModelRuntimeChildOwnerPropagateCalls",
       summary.semanticModelRuntimeChildOwnerPropagateCalls},
      {"semanticModelRuntimeChildOwnerPropagateUs",
       summary.semanticModelRuntimeChildOwnerPropagateUs},
      {"semanticModelPromoteRuntimeCalls",
       summary.semanticModelPromoteRuntimeCalls},
      {"semanticModelPromoteRuntimeUs",
       summary.semanticModelPromoteRuntimeUs},
      {"semanticModelSpriteHostBindCalls",
       summary.semanticModelSpriteHostBindCalls},
      {"semanticModelSpriteHostBindUs",
       summary.semanticModelSpriteHostBindUs},
      {"semanticModelRuntimeModelBindingCalls",
       summary.semanticModelRuntimeModelBindingCalls},
      {"semanticModelRuntimeModelBindingUs",
       summary.semanticModelRuntimeModelBindingUs},
      {"semanticModelGeosetResourceCalls",
       summary.semanticModelGeosetResourceCalls},
      {"semanticModelGeosetResourceUs",
       summary.semanticModelGeosetResourceUs},
      {"semanticModelRuntimeCtorCalls",
       summary.semanticModelRuntimeCtorCalls},
      {"semanticModelRuntimeCtorUs",
       summary.semanticModelRuntimeCtorUs},
      {"semanticModelRuntimeResolveCalls",
       summary.semanticModelRuntimeResolveCalls},
      {"semanticModelRuntimeResolveUs",
       summary.semanticModelRuntimeResolveUs},
      {"semanticModelRuntimeInitCopyCalls",
       summary.semanticModelRuntimeInitCopyCalls},
      {"semanticModelRuntimeInitCopyUs",
       summary.semanticModelRuntimeInitCopyUs},
      {"semanticPoseRuntimePoseCalls",
       summary.semanticPoseRuntimePoseCalls},
      {"semanticPoseRuntimePoseUs",
       summary.semanticPoseRuntimePoseUs},
      {"semanticPoseRuntimePaletteTreeCalls",
       summary.semanticPoseRuntimePaletteTreeCalls},
      {"semanticPoseRuntimePaletteTreeUs",
       summary.semanticPoseRuntimePaletteTreeUs},
      {"semanticPoseRuntimeMatrixPaletteCalls",
       summary.semanticPoseRuntimeMatrixPaletteCalls},
      {"semanticPoseRuntimeMatrixPaletteUs",
       summary.semanticPoseRuntimeMatrixPaletteUs},
      {"semanticPoseSpriteFrameSourceIdentityCalls",
       summary.semanticPoseSpriteFrameSourceIdentityCalls},
      {"semanticPoseSpriteFrameSourceIdentityUs",
       summary.semanticPoseSpriteFrameSourceIdentityUs},
      {"semanticPoseSpriteFramePoseCalls",
       summary.semanticPoseSpriteFramePoseCalls},
      {"semanticPoseSpriteFramePoseUs",
       summary.semanticPoseSpriteFramePoseUs},
      {"semanticPoseRuntimeMatrixPublisherCalls",
       summary.semanticPoseRuntimeMatrixPublisherCalls},
      {"semanticPoseRuntimeMatrixPublisherUs",
       summary.semanticPoseRuntimeMatrixPublisherUs},
      {"semanticPoseSpriteAttachmentHitCalls",
       summary.semanticPoseSpriteAttachmentHitCalls},
      {"semanticPoseSpriteAttachmentHitUs",
       summary.semanticPoseSpriteAttachmentHitUs},
      {"semanticPoseSpriteTransformReadCalls",
       summary.semanticPoseSpriteTransformReadCalls},
      {"semanticPoseSpriteTransformReadUs",
       summary.semanticPoseSpriteTransformReadUs},
      {"semanticPoseSpriteIdentityLookupCalls",
       summary.semanticPoseSpriteIdentityLookupCalls},
      {"semanticPoseSpriteIdentityLookupUs",
       summary.semanticPoseSpriteIdentityLookupUs},
      {"semanticPoseSpriteBaseAliasCalls",
       summary.semanticPoseSpriteBaseAliasCalls},
      {"semanticPoseSpriteBaseAliasUs",
       summary.semanticPoseSpriteBaseAliasUs},
      {"semanticPoseSpritePublishPoseCalls",
       summary.semanticPoseSpritePublishPoseCalls},
      {"semanticPoseSpritePublishPoseUs",
       summary.semanticPoseSpritePublishPoseUs},
      {"semanticPoseSpritePaletteGateCalls",
       summary.semanticPoseSpritePaletteGateCalls},
      {"semanticPoseSpritePaletteGateUs",
       summary.semanticPoseSpritePaletteGateUs},
      {"semanticAttachmentAttachedEffectInitCalls",
       summary.semanticAttachmentAttachedEffectInitCalls},
      {"semanticAttachmentAttachedEffectInitUs",
       summary.semanticAttachmentAttachedEffectInitUs},
      {"semanticAttachmentAttachedEffectDirectCalls",
       summary.semanticAttachmentAttachedEffectDirectCalls},
      {"semanticAttachmentAttachedEffectDirectUs",
       summary.semanticAttachmentAttachedEffectDirectUs},
      {"semanticAttachmentAttachModelToPointCalls",
       summary.semanticAttachmentAttachModelToPointCalls},
      {"semanticAttachmentAttachModelToPointUs",
       summary.semanticAttachmentAttachModelToPointUs},
      {"semanticAttachmentOverrideSharedPresetCalls",
       summary.semanticAttachmentOverrideSharedPresetCalls},
      {"semanticAttachmentOverrideSharedPresetUs",
       summary.semanticAttachmentOverrideSharedPresetUs},
      {"semanticAttachmentOverrideLocalPointCalls",
       summary.semanticAttachmentOverrideLocalPointCalls},
      {"semanticAttachmentOverrideLocalPointUs",
       summary.semanticAttachmentOverrideLocalPointUs},
      {"semanticAttachmentOverridePrimaryPresetCalls",
       summary.semanticAttachmentOverridePrimaryPresetCalls},
      {"semanticAttachmentOverridePrimaryPresetUs",
       summary.semanticAttachmentOverridePrimaryPresetUs},
      {"fallbackDrawCount", summary.fallbackDrawCount},
      {"fallbackDrawCountTerrain", summary.fallbackDrawCountTerrain},
      {"fallbackDrawCountWorldObject", summary.fallbackDrawCountWorldObject},
      {"fallbackDrawCountUnitObject", summary.fallbackDrawCountUnitObject},
      {"objectFallbackDrawCount", summary.objectFallbackDrawCount},
      {"semanticSceneSubmitted", summary.semanticSceneSubmitted},
      {"semanticSceneSubmittedUnit", summary.semanticSceneSubmittedUnit},
      {"semanticSceneSubmittedSkinned", summary.semanticSceneSubmittedSkinned},
      {"semanticSceneSubmittedSkinnedNonUnitResolvedCount",
       summary.semanticSceneSubmittedSkinnedNonUnitResolvedCount},
      {"semanticSceneSubmittedSkinnedUnknownPacketKindCount",
       summary.semanticSceneSubmittedSkinnedUnknownPacketKindCount},
      {"semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount",
       summary.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount},
      {"semanticSceneSubmittedSkinnedGroupNonZeroCount",
       summary.semanticSceneSubmittedSkinnedGroupNonZeroCount},
      {"semanticSceneSubmittedSkinnedTransparentQueueCount",
       summary.semanticSceneSubmittedSkinnedTransparentQueueCount},
      {"semanticSceneSubmittedSkinnedMissingUnitPtrCount",
       summary.semanticSceneSubmittedSkinnedMissingUnitPtrCount},
      {"semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount",
       summary.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount},
      {"semanticSceneSubmittedBuilding", summary.semanticSceneSubmittedBuilding},
      {"semanticSceneSubmittedDestructible",
       summary.semanticSceneSubmittedDestructible},
      {"semanticSceneSubmittedCutout", summary.semanticSceneSubmittedCutout},
      {"semanticSceneSubmittedAlphaBlend",
       summary.semanticSceneSubmittedAlphaBlend},
      {"semanticSceneMaterialObservedCutoutCount",
       summary.semanticSceneMaterialObservedCutoutCount},
      {"semanticSceneMaterialObservedAlphaBlendCount",
       summary.semanticSceneMaterialObservedAlphaBlendCount},
      {"semanticSceneRejectedCutoutSkinnedContract",
       summary.semanticSceneRejectedCutoutSkinnedContract},
      {"semanticSceneRejectedAlphaBlendSkinnedContract",
       summary.semanticSceneRejectedAlphaBlendSkinnedContract},
      {"semanticSceneRejectedCutoutGeometry",
       summary.semanticSceneRejectedCutoutGeometry},
      {"semanticSceneRejectedAlphaBlendGeometry",
       summary.semanticSceneRejectedAlphaBlendGeometry},
      {"semanticSceneRejectedCutoutVisualPolicy",
       summary.semanticSceneRejectedCutoutVisualPolicy},
      {"semanticSceneRejectedAlphaBlendVisualPolicy",
       summary.semanticSceneRejectedAlphaBlendVisualPolicy},
      {"semanticSceneMaterialLayerContractResolvedCount",
       summary.semanticSceneMaterialLayerContractResolvedCount},
      {"semanticSceneMaterialLayerContractFailedCount",
       summary.semanticSceneMaterialLayerContractFailedCount},
      {"semanticSceneMaterialBlendMode0Count",
       summary.semanticSceneMaterialBlendMode0Count},
      {"semanticSceneMaterialBlendMode1Count",
       summary.semanticSceneMaterialBlendMode1Count},
      {"semanticSceneMaterialBlendMode2PlusCount",
       summary.semanticSceneMaterialBlendMode2PlusCount},
      {"semanticSceneDirectCurrentDrawLayerIndexNonZeroCount",
       summary.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount},
      {"semanticSceneMaterialLastMeshIndex",
       summary.semanticSceneMaterialLastMeshIndex},
      {"semanticSceneMaterialLastLayerIndex",
       summary.semanticSceneMaterialLastLayerIndex},
      {"semanticSceneMaterialLastLayerCount",
       summary.semanticSceneMaterialLastLayerCount},
      {"semanticSceneMaterialLastBlendOrDrawMode",
       summary.semanticSceneMaterialLastBlendOrDrawMode},
      {"semanticSceneLivePaletteRefreshAttemptCount",
       summary.semanticSceneLivePaletteRefreshAttemptCount},
      {"semanticSceneLivePaletteRefreshHitCount",
       summary.semanticSceneLivePaletteRefreshHitCount},
      {"semanticSceneLivePaletteRefreshMissCount",
       summary.semanticSceneLivePaletteRefreshMissCount},
      {"semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount",
       summary.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount},
      {"semanticScenePaletteOverrideNoComposeCount",
       summary.semanticScenePaletteOverrideNoComposeCount},
      {"semanticScenePaletteOverrideWouldComposeCount",
       summary.semanticScenePaletteOverrideWouldComposeCount},
      {"semanticScenePalettePacketWorldComposeCount",
       summary.semanticScenePalettePacketWorldComposeCount},
      {"semanticSceneLivePaletteRefreshLastRuntimeModelPtr",
       summary.semanticSceneLivePaletteRefreshLastRuntimeModelPtr},
      {"semanticSceneLivePaletteRefreshLastMatrixCount",
       summary.semanticSceneLivePaletteRefreshLastMatrixCount},
      {"semanticSceneLivePaletteRefreshLastMatrixHash",
       summary.semanticSceneLivePaletteRefreshLastMatrixHash},
      {"semanticSceneLivePaletteMotionSampleCount",
       summary.semanticSceneLivePaletteMotionSampleCount},
      {"semanticSceneLivePaletteMotionNewRuntimeCount",
       summary.semanticSceneLivePaletteMotionNewRuntimeCount},
      {"semanticSceneLivePaletteMotionRawChangedCount",
       summary.semanticSceneLivePaletteMotionRawChangedCount},
      {"semanticSceneLivePaletteMotionRawStableCount",
       summary.semanticSceneLivePaletteMotionRawStableCount},
      {"semanticSceneLivePaletteMotionGroupChangedCount",
       summary.semanticSceneLivePaletteMotionGroupChangedCount},
      {"semanticSceneLivePaletteMotionGroupStableCount",
       summary.semanticSceneLivePaletteMotionGroupStableCount},
      {"semanticSceneLivePaletteMotionLastRuntimeModelPtr",
       summary.semanticSceneLivePaletteMotionLastRuntimeModelPtr},
      {"semanticSceneLivePaletteMotionLastPrevRawHash",
       summary.semanticSceneLivePaletteMotionLastPrevRawHash},
      {"semanticSceneLivePaletteMotionLastRawHash",
       summary.semanticSceneLivePaletteMotionLastRawHash},
      {"semanticSceneLivePaletteMotionLastPrevGroupHash",
       summary.semanticSceneLivePaletteMotionLastPrevGroupHash},
      {"semanticSceneLivePaletteMotionLastGroupHash",
       summary.semanticSceneLivePaletteMotionLastGroupHash},
      {"semanticSceneDrawTimePoseAttemptCount",
       summary.semanticSceneDrawTimePoseAttemptCount},
      {"semanticSceneDrawTimePosePublishedCount",
       summary.semanticSceneDrawTimePosePublishedCount},
      {"semanticSceneDrawTimePoseChangedCount",
       summary.semanticSceneDrawTimePoseChangedCount},
      {"semanticSceneDrawTimePoseStableCount",
       summary.semanticSceneDrawTimePoseStableCount},
      {"semanticSceneDrawTimePoseLastRuntimeModelPtr",
       summary.semanticSceneDrawTimePoseLastRuntimeModelPtr},
      {"semanticSceneDrawTimePoseLastPrevHash",
       summary.semanticSceneDrawTimePoseLastPrevHash},
      {"semanticSceneDrawTimePoseLastHash",
       summary.semanticSceneDrawTimePoseLastHash},
      {"semanticSceneSubmittedPaletteMotionSampleCount",
       summary.semanticSceneSubmittedPaletteMotionSampleCount},
      {"semanticSceneSubmittedPaletteMotionNewRuntimeCount",
       summary.semanticSceneSubmittedPaletteMotionNewRuntimeCount},
      {"semanticSceneSubmittedPaletteMotionChangedCount",
       summary.semanticSceneSubmittedPaletteMotionChangedCount},
      {"semanticSceneSubmittedPaletteMotionStableCount",
       summary.semanticSceneSubmittedPaletteMotionStableCount},
      {"semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr",
       summary.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr},
      {"semanticSceneSubmittedPaletteMotionLastPrevHash",
       summary.semanticSceneSubmittedPaletteMotionLastPrevHash},
      {"semanticSceneSubmittedPaletteMotionLastHash",
       summary.semanticSceneSubmittedPaletteMotionLastHash},
      {"semanticSceneSkinnedDynamicIndexSliceCount",
       summary.semanticSceneSkinnedDynamicIndexSliceCount},
      {"semanticSceneSubmittedOwnedGroupSlots",
       summary.semanticSceneSubmittedOwnedGroupSlots},
      {"semanticSceneCurrentDrawContractKnownCount",
       summary.semanticSceneCurrentDrawContractKnownCount},
      {"semanticSceneCurrentDrawPaletteReadyCount",
       summary.semanticSceneCurrentDrawPaletteReadyCount},
      {"semanticSceneCurrentDrawGroupSlotReadyCount",
       summary.semanticSceneCurrentDrawGroupSlotReadyCount},
      {"semanticSceneCurrentDrawResolveReadyCount",
       summary.semanticSceneCurrentDrawResolveReadyCount},
      {"semanticSceneCurrentDrawMissNoContract",
       summary.semanticSceneCurrentDrawMissNoContract},
      {"semanticSceneCurrentDrawMissNoPalette",
       summary.semanticSceneCurrentDrawMissNoPalette},
      {"semanticSceneCurrentDrawMissNoGroupSlots",
       summary.semanticSceneCurrentDrawMissNoGroupSlots},
      {"semanticSceneCurrentDrawMissStaleVisibleFrame",
       summary.semanticSceneCurrentDrawMissStaleVisibleFrame},
      {"semanticSceneCurrentDrawResolveReadyRejectedCount",
       summary.semanticSceneCurrentDrawResolveReadyRejectedCount},
      {"semanticSceneCanonicalReadyCount",
       summary.semanticSceneCanonicalReadyCount},
      {"semanticSceneCanonicalReadyCutoutCount",
       summary.semanticSceneCanonicalReadyCutoutCount},
      {"semanticSceneCanonicalReadyAlphaBlendCount",
       summary.semanticSceneCanonicalReadyAlphaBlendCount},
      {"semanticSceneCanonicalRejectNoStableIdentity",
       summary.semanticSceneCanonicalRejectNoStableIdentity},
      {"semanticSceneCanonicalRejectNoMesh",
       summary.semanticSceneCanonicalRejectNoMesh},
      {"semanticSceneCanonicalRejectNoWorldTransform",
       summary.semanticSceneCanonicalRejectNoWorldTransform},
      {"semanticSceneCanonicalRejectNoPalette",
       summary.semanticSceneCanonicalRejectNoPalette},
      {"semanticSceneCanonicalRejectNoSlotContract",
       summary.semanticSceneCanonicalRejectNoSlotContract},
      {"semanticSceneCanonicalRejectStaleProducer",
       summary.semanticSceneCanonicalRejectStaleProducer},
      {"semanticSceneCanonicalRejectInvalidVertexIndex",
       summary.semanticSceneCanonicalRejectInvalidVertexIndex},
      {"semanticSceneCanonicalRejectExplicitBlendIncomplete",
       summary.semanticSceneCanonicalRejectExplicitBlendIncomplete},
      {"semanticSceneCanonicalRejectAfterReadyCount",
       summary.semanticSceneCanonicalRejectAfterReadyCount},
      {"currentDrawContractPublishAttemptCount",
       summary.currentDrawContractPublishAttemptCount},
      {"currentDrawContractPublishReadyCount",
       summary.currentDrawContractPublishReadyCount},
      {"currentDrawContractPublishSkippedNonWorldContext",
       summary.currentDrawContractPublishSkippedNonWorldContext},
      {"currentDrawContractPublishSkippedSmallViewport",
       summary.currentDrawContractPublishSkippedSmallViewport},
      {"currentDrawContractQueryAttemptCount",
       summary.currentDrawContractQueryAttemptCount},
      {"currentDrawContractQueryHitCount",
       summary.currentDrawContractQueryHitCount},
      {"currentDrawCapturedPaletteQueryAttemptCount",
       summary.currentDrawCapturedPaletteQueryAttemptCount},
      {"currentDrawCapturedPaletteQueryHitCount",
       summary.currentDrawCapturedPaletteQueryHitCount},
      {"currentDrawGroupSlotDecodeAttemptCount",
       summary.currentDrawGroupSlotDecodeAttemptCount},
      {"currentDrawGroupSlotDecodeHitCount",
       summary.currentDrawGroupSlotDecodeHitCount},
      {"currentDrawGroupSlotDecodeMissDisabledStream",
       summary.currentDrawGroupSlotDecodeMissDisabledStream},
      {"currentDrawPreparedSliceProbeAttemptCount",
       summary.currentDrawPreparedSliceProbeAttemptCount},
      {"currentDrawPreparedSliceProbeContextReadyCount",
       summary.currentDrawPreparedSliceProbeContextReadyCount},
      {"currentDrawPreparedSliceProbeBackingReadableCount",
       summary.currentDrawPreparedSliceProbeBackingReadableCount},
      {"currentDrawPreparedSliceRecordedCount",
       summary.currentDrawPreparedSliceRecordedCount},
      {"currentDrawPreparedSliceQueryAttemptCount",
       summary.currentDrawPreparedSliceQueryAttemptCount},
      {"currentDrawPreparedSliceQueryHitCount",
       summary.currentDrawPreparedSliceQueryHitCount},
      {"currentDrawPreparedSliceQueryMissCount",
       summary.currentDrawPreparedSliceQueryMissCount},
      {"currentDrawStream1PublishNoStreamCount",
       summary.currentDrawStream1PublishNoStreamCount},
      {"currentDrawStream1PublishStride0Count",
       summary.currentDrawStream1PublishStride0Count},
      {"currentDrawStream1PublishStride1Count",
       summary.currentDrawStream1PublishStride1Count},
      {"currentDrawStream1PublishStride8Count",
       summary.currentDrawStream1PublishStride8Count},
      {"currentDrawStream1PublishStride12Count",
       summary.currentDrawStream1PublishStride12Count},
      {"currentDrawStream1PublishStride16Count",
       summary.currentDrawStream1PublishStride16Count},
      {"currentDrawStream1PublishStride20Count",
       summary.currentDrawStream1PublishStride20Count},
      {"currentDrawStream1PublishStrideOtherCount",
       summary.currentDrawStream1PublishStrideOtherCount},
      {"currentDrawStream1PublishLastRawStride",
       summary.currentDrawStream1PublishLastRawStride},
      {"currentDrawStream1PublishMaxRawStride",
       summary.currentDrawStream1PublishMaxRawStride},
      {"currentDrawLastVisibleFrameSerial",
       summary.currentDrawLastVisibleFrameSerial},
      {"currentDrawLastRenderFrameIndex",
       summary.currentDrawLastRenderFrameIndex},
      {"currentDrawLastSmallViewportWidth",
       summary.currentDrawLastSmallViewportWidth},
      {"currentDrawLastSmallViewportHeight",
       summary.currentDrawLastSmallViewportHeight},
      {"currentDrawLastMissReason",
       summary.currentDrawLastMissReason},
      // Phase 7.35 Pose-lag 诊断：submit 端 palette 时间滞后分布。
      {"submitPaletteFrameLag0Count",
       summary.submitPaletteFrameLag0Count},
      {"submitPaletteFrameLag1Count",
       summary.submitPaletteFrameLag1Count},
      {"submitPaletteFrameLag2Count",
       summary.submitPaletteFrameLag2Count},
      {"submitPaletteFrameLag3To5Count",
       summary.submitPaletteFrameLag3To5Count},
      {"submitPaletteFrameLag6PlusCount",
       summary.submitPaletteFrameLag6PlusCount},
      {"submitPaletteFrameLagMax",
       summary.submitPaletteFrameLagMax},
      {"submitPaletteFrameLagSampleCount",
       summary.submitPaletteFrameLagSampleCount},
      // Phase 7.39：实际 palette 内容年龄（Game.dll frameTag 口径）。
      {"submitPaletteContentAgeLag0Count",
       summary.submitPaletteContentAgeLag0Count},
      {"submitPaletteContentAgeLag1Count",
       summary.submitPaletteContentAgeLag1Count},
      {"submitPaletteContentAgeLag2Count",
       summary.submitPaletteContentAgeLag2Count},
      {"submitPaletteContentAgeLag3To5Count",
       summary.submitPaletteContentAgeLag3To5Count},
      {"submitPaletteContentAgeLag6PlusCount",
       summary.submitPaletteContentAgeLag6PlusCount},
      {"submitPaletteContentAgeMax",
       summary.submitPaletteContentAgeMax},
      {"submitPaletteContentAgeSampleCount",
       summary.submitPaletteContentAgeSampleCount},
      {"submitPaletteContentAgeUnknownCount",
       summary.submitPaletteContentAgeUnknownCount},
      // Phase 7.35 路径 1 诊断：capture 端 Exact 查询分布。
      {"paletteCaptureExactHitCount",
       summary.paletteCaptureExactHitCount},
      {"paletteCaptureBestEffortHitCount",
       summary.paletteCaptureBestEffortHitCount},
      {"paletteCaptureSlotOverflowMissCount",
       summary.paletteCaptureSlotOverflowMissCount},
      {"paletteCaptureInvalidEntryMissCount",
       summary.paletteCaptureInvalidEntryMissCount},
      {"paletteCaptureFrameTagMismatchMissCount",
       summary.paletteCaptureFrameTagMismatchMissCount},
      {"paletteCaptureShortResultMissCount",
       summary.paletteCaptureShortResultMissCount},
      // Phase 7.35 路径 2 诊断：submit-side live palette rebuild。
      {"submitLiveRebuildAttemptCount",
       summary.submitLiveRebuildAttemptCount},
      {"submitLiveRebuildHitCount",
       summary.submitLiveRebuildHitCount},
      {"submitLiveRebuildMissCount",
       summary.submitLiveRebuildMissCount},
      {"submitLiveRebuildAppliedCount",
       summary.submitLiveRebuildAppliedCount},
      // AlphaTest payload plumbing 诊断（Claude AlphaTest lane, Phase B）。
      // Attempt/Hit/Applied 应当随 cutout/alphablend 场景单调增长；
      // FallbackReject 代表当前帧仍有多少 cutout 物体因 payload 缺失被拒，
      // StashCaptured 应当稳定大于 0；若 AutoTest 报告里 Attempt=0 且
      // StashCaptured=0，说明场景根本没有 alpha-test 物体走到 shadow
      // caster 链路，并非 plumbing 失效。
      {"shadowAlphaTestPayloadAttemptCount",
       summary.shadowAlphaTestPayloadAttemptCount},
      {"shadowAlphaTestPayloadHitCount",
       summary.shadowAlphaTestPayloadHitCount},
      {"shadowAlphaTestPayloadMissNoUvCount",
       summary.shadowAlphaTestPayloadMissNoUvCount},
      {"shadowAlphaTestPayloadMissNoDiffuseCount",
       summary.shadowAlphaTestPayloadMissNoDiffuseCount},
      {"shadowAlphaTestPayloadMissStageInvalidCount",
       summary.shadowAlphaTestPayloadMissStageInvalidCount},
      {"shadowAlphaTestPayloadAppliedCount",
       summary.shadowAlphaTestPayloadAppliedCount},
      {"shadowAlphaTestPayloadFallbackRejectCount",
       summary.shadowAlphaTestPayloadFallbackRejectCount},
      {"shadowAlphaTestPayloadStashCapturedCount",
       summary.shadowAlphaTestPayloadStashCapturedCount},
      {"shadowAlphaTestPayloadStashSkipNoSemanticKeyCount",
       summary.shadowAlphaTestPayloadStashSkipNoSemanticKeyCount},
      {"shadowAlphaTestPayloadStashSkipNoUvCount",
       summary.shadowAlphaTestPayloadStashSkipNoUvCount},
      {"shadowAlphaTestPayloadStashSkipNoDiffuseCount",
       summary.shadowAlphaTestPayloadStashSkipNoDiffuseCount},
      {"shadowAlphaTestPayloadStashSkipNoUploadCount",
       summary.shadowAlphaTestPayloadStashSkipNoUploadCount},
      {"shadowAlphaTestPayloadCacheEvictedCount",
       summary.shadowAlphaTestPayloadCacheEvictedCount},
      {"shadowAlphaTestPayloadCacheSizeGauge",
       summary.shadowAlphaTestPayloadCacheSizeGauge},
      {"semanticScenePopulateLastReturnReason",
       summary.semanticScenePopulateLastReturnReason},
      {"semanticScenePopulateLastProducerPublishAttemptDelta",
       summary.semanticScenePopulateLastProducerPublishAttemptDelta},
      {"semanticScenePopulateLastProducerPublishReadyDelta",
       summary.semanticScenePopulateLastProducerPublishReadyDelta},
      {"semanticScenePopulateLastProducerQueryAttemptDelta",
       summary.semanticScenePopulateLastProducerQueryAttemptDelta},
      {"semanticScenePopulateLastProducerQueryHitDelta",
       summary.semanticScenePopulateLastProducerQueryHitDelta},
      {"semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta",
       summary.semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta},
      {"semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta",
       summary.semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta},
      {"semanticScenePopulateLastProducerGroupDecodeAttemptDelta",
       summary.semanticScenePopulateLastProducerGroupDecodeAttemptDelta},
      {"semanticScenePopulateLastProducerGroupDecodeHitDelta",
       summary.semanticScenePopulateLastProducerGroupDecodeHitDelta},
      {"semanticSceneDirectCurrentDrawRecordCount",
       summary.semanticSceneDirectCurrentDrawRecordCount},
      {"semanticSceneDirectCurrentDrawBuiltPacketCount",
       summary.semanticSceneDirectCurrentDrawBuiltPacketCount},
      {"semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount",
       summary.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount},
      {"semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount",
       summary.semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount},
      {"semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount",
       summary.semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount},
      {"semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount",
       summary.semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount},
      // Phase 7.2: flicker diagnostics + reconciliation（shadowRuntimeSummary）
      {"semanticSceneDirectLastRawRecordCount",
       summary.semanticSceneDirectLastRawRecordCount},
      {"semanticSceneDirectLastEligibleRecordCount",
       summary.semanticSceneDirectLastEligibleRecordCount},
      {"semanticSceneDirectLastSubmittedRecordCount",
       summary.semanticSceneDirectLastSubmittedRecordCount},
      {"semanticSceneDirectLastUniqueObjectCount",
       summary.semanticSceneDirectLastUniqueObjectCount},
      {"semanticSceneDirectLastSubmittedObjectCount",
       summary.semanticSceneDirectLastSubmittedObjectCount},
      {"semanticSceneDirectLastRecordCapPartialObjectCount",
       summary.semanticSceneDirectLastRecordCapPartialObjectCount},
      {"semanticSceneDirectLastScanCapPartialObjectCount",
       summary.semanticSceneDirectLastScanCapPartialObjectCount},
      {"semanticSceneDirectLastMinGeosetsPerObject",
       summary.semanticSceneDirectLastMinGeosetsPerObject},
      {"semanticSceneDirectLastMaxGeosetsPerObject",
       summary.semanticSceneDirectLastMaxGeosetsPerObject},
      {"semanticSceneDirectLastSubmittedIdentityHash",
       summary.semanticSceneDirectLastSubmittedIdentityHash},
      {"semanticSceneDirectIdentityChurnCount",
       summary.semanticSceneDirectIdentityChurnCount},
      {"semanticSceneDirectRecordCapHitCount",
       summary.semanticSceneDirectRecordCapHitCount},
      {"semanticSceneDirectRecordCapTruncatedRecordCount",
       summary.semanticSceneDirectRecordCapTruncatedRecordCount},
      {"semanticSceneDirectScanCapHitCount",
       summary.semanticSceneDirectScanCapHitCount},
      {"semanticSceneDirectObjectGroupedSubmitCount",
       summary.semanticSceneDirectObjectGroupedSubmitCount},
      {"semanticSceneDirectObjectGroupedSkipCount",
       summary.semanticSceneDirectObjectGroupedSkipCount},
      {"semanticSceneDirectRecordCapSkipObjectCount",
       summary.semanticSceneDirectRecordCapSkipObjectCount},
      {"semanticSceneDirectRecordCapAppendFailCount",
       summary.semanticSceneDirectRecordCapAppendFailCount},
      {"semanticSceneDirectSelectionLeaseActiveKeyCount",
       summary.semanticSceneDirectSelectionLeaseActiveKeyCount},
      {"semanticSceneDirectSelectionLeasePrunedKeyCount",
       summary.semanticSceneDirectSelectionLeasePrunedKeyCount},
      {"semanticSceneDirectSelectionLeaseSubmittedKeyCount",
       summary.semanticSceneDirectSelectionLeaseSubmittedKeyCount},
      {"semanticSceneDirectStickyFillBudgetRecordCount",
       summary.semanticSceneDirectStickyFillBudgetRecordCount},
      {"semanticSceneDirectStickyFillAppendedCount",
       summary.semanticSceneDirectStickyFillAppendedCount},
      {"semanticSceneDirectStickyFillSubmittedCount",
       summary.semanticSceneDirectStickyFillSubmittedCount},
      {"semanticSceneDirectStickyFillMissedCount",
       summary.semanticSceneDirectStickyFillMissedCount},
      {"semanticSceneDirectPartLeaseRestoredCount",
       summary.semanticSceneDirectPartLeaseRestoredCount},
      {"semanticSceneDirectPartLeaseUpdatedCount",
       summary.semanticSceneDirectPartLeaseUpdatedCount},
      {"semanticSceneDirectPartLeaseExpiredCount",
       summary.semanticSceneDirectPartLeaseExpiredCount},
      {"semanticSceneDirectPartLeaseRejectedDynamicMeshCount",
       summary.semanticSceneDirectPartLeaseRejectedDynamicMeshCount},
      {"semanticSceneDirectPartLeaseRejectedNotSelfContainedCount",
       summary.semanticSceneDirectPartLeaseRejectedNotSelfContainedCount},
      {"semanticSceneDirectPartLeaseRejectedUnsafeBackingCount",
       summary.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount},
      {"semanticSceneDirectPartLeaseRejectedSelfRenewCount",
       summary.semanticSceneDirectPartLeaseRejectedSelfRenewCount},
      {"semanticSceneDirectPartLeaseBudgetLimitCount",
       summary.semanticSceneDirectPartLeaseBudgetLimitCount},
      {"semanticSceneShadowManifestPartLeaseRestoredCount",
       summary.semanticSceneShadowManifestPartLeaseRestoredCount},
      {"semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount",
       summary.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount},
      {"semanticSceneShadowManifestPartLeaseExpiredCount",
       summary.semanticSceneShadowManifestPartLeaseExpiredCount},
      {"semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount",
       summary.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount},
      {"semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount",
       summary.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount},
      {"semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount",
       summary.semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount},
      {"semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount",
       summary
           .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount},
      {"semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount",
       summary.semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount},
      {"semanticSceneShadowManifestPartLeaseBudgetLimitCount",
       summary.semanticSceneShadowManifestPartLeaseBudgetLimitCount},
      {"semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount",
       summary.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount},
      {"semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount",
       summary.semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount},
      {"semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount",
       summary.semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount},
      {"semanticSceneShadowManifestPartLeasePaletteRefreshAttemptCount",
       summary
           .semanticSceneShadowManifestPartLeasePaletteRefreshAttemptCount},
      {"semanticSceneShadowManifestPartLeasePaletteRefreshHitCount",
       summary.semanticSceneShadowManifestPartLeasePaletteRefreshHitCount},
      {"semanticSceneShadowManifestPartLeasePaletteRefreshMissCount",
       summary.semanticSceneShadowManifestPartLeasePaletteRefreshMissCount},
      {"semanticSceneShadowManifestPartLeasePaletteRefreshAppliedCount",
       summary.semanticSceneShadowManifestPartLeasePaletteRefreshAppliedCount},
      {"semanticSceneShadowManifestPartLeasePaletteRefreshFallbackCount",
       summary
           .semanticSceneShadowManifestPartLeasePaletteRefreshFallbackCount},
      {"semanticSceneShadowManifestObjectCoreCompleteCount",
       summary.semanticSceneShadowManifestObjectCoreCompleteCount},
      {"semanticSceneShadowManifestObjectCoreIncompleteSkipCount",
       summary.semanticSceneShadowManifestObjectCoreIncompleteSkipCount},
      {"semanticSceneShadowManifestPartOmittedIncompleteCoreCount",
       summary.semanticSceneShadowManifestPartOmittedIncompleteCoreCount},
      {"semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount",
       summary
           .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount},
      {"semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount",
       summary
           .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount},
      {"semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount",
       summary
           .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount},
      {"semanticSceneShadowManifestObjectCoreEpochMissingPartCount",
       summary.semanticSceneShadowManifestObjectCoreEpochMissingPartCount},
      {"semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount",
       summary
           .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount},
      // Phase 7.28：skinned palette content stability probe。
      {"semanticSceneSubmittedSkinnedPaletteSourceNoneCount",
       summary.semanticSceneSubmittedSkinnedPaletteSourceNoneCount},
      {"semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount},
      {"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount},
      {"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount},
      {"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount},
      {"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount},
      {"semanticSceneSubmittedSkinnedPaletteStablePartSampleCount",
       summary.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount},
      {"semanticSceneSubmittedSkinnedPaletteHashChurnCount",
       summary.semanticSceneSubmittedSkinnedPaletteHashChurnCount},
      {"semanticSceneSubmittedSkinnedPaletteSourceChurnCount",
       summary.semanticSceneSubmittedSkinnedPaletteSourceChurnCount},
      {"semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount",
       summary.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount},
      {"semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax",
       summary.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax},
      {"semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax",
       summary
           .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax},
      {"semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount},
      {"semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount},
      {"semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount},
      {"semanticSceneSubmittedSkinnedPaletteCountChurnCount",
       summary.semanticSceneSubmittedSkinnedPaletteCountChurnCount},
      {"semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount},
      {"semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount},
      {"semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount",
       summary.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount},
      {"semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount},
      {"semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount},
      {"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount},
      {"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount},
      {"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount},
      {"semanticSceneDirectPaletteAttributionSnapshotHitCount",
       summary.semanticSceneDirectPaletteAttributionSnapshotHitCount},
      {"semanticSceneDirectPaletteCaptureTrustedSourceHitCount",
       summary.semanticSceneDirectPaletteCaptureTrustedSourceHitCount},
      {"semanticSceneDirectPaletteCaptureTrustedSourceMissCount",
       summary.semanticSceneDirectPaletteCaptureTrustedSourceMissCount},
      // Phase 7.30 Step A：stale→live 过渡归因。
      {"semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount",
       summary.semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount},
      {"semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount",
       summary
           .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount},
      {"semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount",
       summary.semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount},
      {"semanticSceneDirectStickyPartSelectionRetainedCount",
       summary.semanticSceneDirectStickyPartSelectionRetainedCount},
      {"semanticSceneDirectStickyPartSelectionDroppedCount",
       summary.semanticSceneDirectStickyPartSelectionDroppedCount},
      {"semanticSceneDirectStickyPartSelectionFallbackCount",
       summary.semanticSceneDirectStickyPartSelectionFallbackCount},
      {"semanticSceneDirectManifestObjectCount",
       summary.semanticSceneDirectManifestObjectCount},
      {"semanticSceneDirectManifestObservedPartCount",
       summary.semanticSceneDirectManifestObservedPartCount},
      {"semanticSceneDirectManifestShadowEligiblePartCount",
       summary.semanticSceneDirectManifestShadowEligiblePartCount},
      {"semanticSceneDirectObjectCompleteEligibleCount",
       summary.semanticSceneDirectObjectCompleteEligibleCount},
      {"semanticSceneDirectObjectIncompleteByScanCapCount",
       summary.semanticSceneDirectObjectIncompleteByScanCapCount},
      {"semanticSceneDirectObjectIncompleteByAlphaPolicyCount",
       summary.semanticSceneDirectObjectIncompleteByAlphaPolicyCount},
      {"semanticSceneDirectObjectIncompleteBySliceUnresolvedCount",
       summary.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount},
      {"semanticSceneDirectObjectIncompleteByPacketBuildFailCount",
       summary.semanticSceneDirectObjectIncompleteByPacketBuildFailCount},
      {"semanticSceneDirectObjectIncompleteByAppendFailCount",
       summary.semanticSceneDirectObjectIncompleteByAppendFailCount},
      {"semanticSceneDirectSubmittedCompleteObjectCount",
       summary.semanticSceneDirectSubmittedCompleteObjectCount},
      {"semanticSceneDirectSubmittedPartialObjectCount",
       summary.semanticSceneDirectSubmittedPartialObjectCount},
      {"semanticSceneDirectPreparedSliceAuthoritativeCount",
       summary.semanticSceneDirectPreparedSliceAuthoritativeCount},
      {"semanticSceneDirectPreparedSliceFallbackLayerIndexCount",
       summary.semanticSceneDirectPreparedSliceFallbackLayerIndexCount},
      {"semanticSceneDirectPreparedSliceMissingCount",
       summary.semanticSceneDirectPreparedSliceMissingCount},
      {"semanticScenePreparedProbeAttemptCount",
       summary.semanticScenePreparedProbeAttemptCount},
      {"semanticScenePreparedProbeContextReadyCount",
       summary.semanticScenePreparedProbeContextReadyCount},
      {"semanticScenePreparedProbeBackingReadableCount",
       summary.semanticScenePreparedProbeBackingReadableCount},
      {"semanticScenePreparedSliceRecordedCount",
       summary.semanticScenePreparedSliceRecordedCount},
      {"semanticScenePreparedSliceQueryAttemptCount",
       summary.semanticScenePreparedSliceQueryAttemptCount},
      {"semanticScenePreparedSliceQueryHitCount",
       summary.semanticScenePreparedSliceQueryHitCount},
      {"semanticScenePreparedSliceQueryMissCount",
       summary.semanticScenePreparedSliceQueryMissCount},
      {"semanticSceneShadowManifestObjectCount",
       summary.semanticSceneShadowManifestObjectCount},
      {"semanticSceneShadowManifestPartCount",
       summary.semanticSceneShadowManifestPartCount},
      {"semanticSceneShadowManifestStableObjectCount",
       summary.semanticSceneShadowManifestStableObjectCount},
      {"semanticSceneShadowManifestNewObjectCount",
       summary.semanticSceneShadowManifestNewObjectCount},
      {"semanticSceneShadowManifestExpiredObjectCount",
       summary.semanticSceneShadowManifestExpiredObjectCount},
      {"semanticSceneShadowManifestFreshPartCount",
       summary.semanticSceneShadowManifestFreshPartCount},
      {"semanticSceneShadowManifestLeaseablePartCount",
       summary.semanticSceneShadowManifestLeaseablePartCount},
      {"semanticSceneShadowManifestPoseStalePartCount",
       summary.semanticSceneShadowManifestPoseStalePartCount},
      {"semanticSceneShadowManifestSliceStalePartCount",
       summary.semanticSceneShadowManifestSliceStalePartCount},
      {"semanticSceneShadowManifestExpiredPartCount",
       summary.semanticSceneShadowManifestExpiredPartCount},
      {"semanticSceneShadowManifestMultiSlicePartCount",
       summary.semanticSceneShadowManifestMultiSlicePartCount},
      {"semanticSceneShadowManifestPayload11CChurnCount",
       summary.semanticSceneShadowManifestPayload11CChurnCount},
      {"semanticSceneShadowManifestRenderablePartChurnCount",
       summary.semanticSceneShadowManifestRenderablePartChurnCount},
      {"semanticSceneShadowManifestCModelPoseHitCount",
       summary.semanticSceneShadowManifestCModelPoseHitCount},
      {"semanticSceneShadowManifestCModelPoseMissCount",
       summary.semanticSceneShadowManifestCModelPoseMissCount},
      {"semanticSceneShadowManifestCModelPoseNoRuntimeCount",
       summary.semanticSceneShadowManifestCModelPoseNoRuntimeCount},
      {"semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr",
       summary.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr},
      {"semanticSceneShadowManifestCModelPoseLastMatrixCount",
       summary.semanticSceneShadowManifestCModelPoseLastMatrixCount},
      {"semanticSceneShadowManifestCModelPoseLastMatrixHash",
       summary.semanticSceneShadowManifestCModelPoseLastMatrixHash},
      {"semanticSceneSubmittedObjectJaccardMilli",
       summary.semanticSceneSubmittedObjectJaccardMilli},
      {"semanticSceneSubmittedPartJaccardMilli",
       summary.semanticSceneSubmittedPartJaccardMilli},
      {"semanticSceneVisibleLookupPartLayerHitCount",
       summary.semanticSceneVisibleLookupPartLayerHitCount},
      {"semanticSceneVisibleLookupSingleFallbackCount",
       summary.semanticSceneVisibleLookupSingleFallbackCount},
      {"semanticSceneVisibleLookupMissCount",
       summary.semanticSceneVisibleLookupMissCount},
      {"semanticSceneDirectMainWorldBackingNotCheckedCount",
       summary.semanticSceneDirectMainWorldBackingNotCheckedCount},
      {"semanticSceneDirectMainWorldBackingPassCount",
       summary.semanticSceneDirectMainWorldBackingPassCount},
      {"semanticSceneDirectMainWorldBackingFailNoRenderablePartCount",
       summary.semanticSceneDirectMainWorldBackingFailNoRenderablePartCount},
      {"semanticSceneDirectMainWorldBackingFailLookupMissCount",
       summary.semanticSceneDirectMainWorldBackingFailLookupMissCount},
      {"semanticSceneDirectMainWorldBackingFailNonMainQueueCount",
       summary.semanticSceneDirectMainWorldBackingFailNonMainQueueCount},
      {"semanticSceneDirectMainWorldBackingFailNonWorldGroupCount",
       summary.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount},
      {"semanticSceneDirectMainWorldBackingFailIdentityMismatchCount",
       summary.semanticSceneDirectMainWorldBackingFailIdentityMismatchCount},
      {"semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount",
       summary.semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount},
      {"semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount",
       summary.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount},
      {"semanticSceneDirectPaletteHashChurnCount",
       summary.semanticSceneDirectPaletteHashChurnCount},
      {"semanticSceneDirectGroupHashChurnCount",
       summary.semanticSceneDirectGroupHashChurnCount},
      {"semanticSceneDirectStableGroupHashChurnCount",
       summary.semanticSceneDirectStableGroupHashChurnCount},
      {"semanticSceneDirectStream1PtrChurnCount",
       summary.semanticSceneDirectStream1PtrChurnCount},
      {"semanticSceneDirectGeometrySourceHashChurnCount",
       summary.semanticSceneDirectGeometrySourceHashChurnCount},
      {"semanticSceneDirectSameCasterComparisonCount",
       summary.semanticSceneDirectSameCasterComparisonCount},
      {"semanticSceneDirectIdentitySkippedChurnCount",
       summary.semanticSceneDirectIdentitySkippedChurnCount},
      {"semanticSceneDirectPaletteRootDeltaSampleCount",
       summary.semanticSceneDirectPaletteRootDeltaSampleCount},
      {"semanticSceneDirectPaletteRootHashChangedTinyDeltaCount",
       summary.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount},
      {"semanticSceneDirectPaletteRootHashChangedSmallDeltaCount",
       summary.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount},
      {"semanticSceneDirectPaletteRootHashChangedMediumDeltaCount",
       summary.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount},
      {"semanticSceneDirectPaletteRootHashChangedLargeDeltaCount",
       summary.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount},
      {"semanticSceneDirectPaletteRootMaxDeltaMilli",
       summary.semanticSceneDirectPaletteRootMaxDeltaMilli},
      {"semanticSceneDirectSelectionKeyUnitPtrCount",
       summary.semanticSceneDirectSelectionKeyUnitPtrCount},
      {"semanticSceneDirectSelectionKeyJHandleCount",
       summary.semanticSceneDirectSelectionKeyJHandleCount},
      {"semanticSceneDirectSelectionKeyRuntimeModelCount",
       summary.semanticSceneDirectSelectionKeyRuntimeModelCount},
      {"semanticSceneDirectSelectionKeyWorldObjectCount",
       summary.semanticSceneDirectSelectionKeyWorldObjectCount},
      {"semanticSceneDirectSelectionKeySceneNodeCount",
       summary.semanticSceneDirectSelectionKeySceneNodeCount},
      {"semanticSceneDirectSelectionKeyModelMeshCount",
       summary.semanticSceneDirectSelectionKeyModelMeshCount},
      {"semanticSceneDirectSelectionKeyRenderablePartCount",
       summary.semanticSceneDirectSelectionKeyRenderablePartCount},
      {"semanticSceneDirectLastSubmittedSceneNode",
       summary.semanticSceneDirectLastSubmittedSceneNode},
      {"semanticSceneDirectLastSubmittedRenderablePart",
       summary.semanticSceneDirectLastSubmittedRenderablePart},
      {"semanticSceneDirectLastSubmittedMeshData",
       summary.semanticSceneDirectLastSubmittedMeshData},
      {"semanticSceneDirectLastSubmittedPaletteHash",
       summary.semanticSceneDirectLastSubmittedPaletteHash},
      {"semanticSceneDirectLastSubmittedGroupHash",
       summary.semanticSceneDirectLastSubmittedGroupHash},
      {"semanticSceneDirectLastSubmittedStableGroupHash",
       summary.semanticSceneDirectLastSubmittedStableGroupHash},
      {"semanticSceneDirectLastSubmittedStream1Ptr",
       summary.semanticSceneDirectLastSubmittedStream1Ptr},
      {"semanticSceneDirectLastSubmittedGeometrySourceHash",
       summary.semanticSceneDirectLastSubmittedGeometrySourceHash},
      {"semanticSceneLastAppendedGeometrySourceHash",
       summary.semanticSceneLastAppendedGeometrySourceHash},
      {"semanticSceneLastAppendedGeometryId",
       summary.semanticSceneLastAppendedGeometryId},
      {"terrainS1CaptureAttemptCount",
       summary.terrainS1CaptureAttemptCount},
      {"terrainS1CaptureAcceptedCount",
       summary.terrainS1CaptureAcceptedCount},
      {"terrainS1WorldIdentityLikeCount",
       summary.terrainS1WorldIdentityLikeCount},
      {"terrainS1WorldNonIdentityCount",
       summary.terrainS1WorldNonIdentityCount},
      {"terrainS1WorldNonFiniteCount",
       summary.terrainS1WorldNonFiniteCount},
      {"terrainS1ForceIdentityWorldCount",
       summary.terrainS1ForceIdentityWorldCount},
      {"terrainS1WorldMatrixHash",
       summary.terrainS1WorldMatrixHash},
      {"terrainS1WorldTranslationMilliMax",
       summary.terrainS1WorldTranslationMilliMax},
      {"shadowCasterStageHistogram",
       summary.shadowCasterStageHistogram},
      {"shadowCasterCategoryHistogram",
       summary.shadowCasterCategoryHistogram},
      {"stage13CaptureAttemptCount", summary.stage13CaptureAttemptCount},
      {"stage13CaptureRejectedNoDemandCount",
       summary.stage13CaptureRejectedNoDemandCount},
      {"stage13CaptureRejectedAfterBeforeUiCount",
       summary.stage13CaptureRejectedAfterBeforeUiCount},
      {"stage13CaptureConsideredCount",
       summary.stage13CaptureConsideredCount},
      {"beforeUiStage13BoundaryCandidateCount",
       summary.beforeUiStage13BoundaryCandidateCount},
      {"beforeUiStage13BoundaryCommitCount",
       summary.beforeUiStage13BoundaryCommitCount},
      {"stage13RetentionBaseEligibleCount",
       summary.stage13RetentionBaseEligibleCount},
      {"stage13SourcePositionInvalidCount",
       summary.stage13SourcePositionInvalidCount},
      {"stage13SourceIndexInvalidCount",
       summary.stage13SourceIndexInvalidCount},
      {"stage13SourceIdentityValidCount",
       summary.stage13SourceIdentityValidCount},
      {"stage13SourceIdentityHitCount",
       summary.stage13SourceIdentityHitCount},
      {"stage13SourceIdentityMissCount",
       summary.stage13SourceIdentityMissCount},
      {"stage13StrongScanCount", summary.stage13StrongScanCount},
      {"stage13SnapshotBuildCount", summary.stage13SnapshotBuildCount},
      {"stage13SnapshotContentRekeyCount",
       summary.stage13SnapshotContentRekeyCount},
      {"stage13FreezeCopyBytes", summary.stage13FreezeCopyBytes},
      {"stage13CpuSnapshotCopyBytes",
       summary.stage13CpuSnapshotCopyBytes},
      {"stage13RetentionSnapshotBytes",
       summary.stage13RetentionSnapshotBytes},
      {"stage13RetainedEntryCountMax",
       summary.stage13RetainedEntryCountMax},
      {"stage13RetainedContentMatchCount",
       summary.stage13RetainedContentMatchCount},
      {"stage13RetainedIdentityMatchCount",
       summary.stage13RetainedIdentityMatchCount},
      {"stage13RetainedWorldMatchCount",
       summary.stage13RetainedWorldMatchCount},
      {"stage13RetainedMaterialMatchCount",
       summary.stage13RetainedMaterialMatchCount},
      {"stage13RetainedLayoutMatchCount",
       summary.stage13RetainedLayoutMatchCount},
      {"stage13RetainedAllSemanticMatchCount",
       summary.stage13RetainedAllSemanticMatchCount},
      {"semanticSceneShadowCastersCount",
       summary.semanticSceneShadowCastersCount},
      {"semanticSceneReplayDrawsCount",
       summary.semanticSceneReplayDrawsCount},
      {"semanticSceneShadowMapDrawnCasters",
       summary.semanticSceneShadowMapDrawnCasters},
      {"semanticSceneShadowMapCascadeCulledCount",
       summary.semanticSceneShadowMapCascadeCulledCount},
      {"semanticSceneShadowMapSkinnedCasterCount",
       summary.semanticSceneShadowMapSkinnedCasterCount},
      {"semanticSceneShadowMapSkinnedPreparedCount",
       summary.semanticSceneShadowMapSkinnedPreparedCount},
      {"semanticSceneShadowMapSkinnedInvalidBufferCount",
       summary.semanticSceneShadowMapSkinnedInvalidBufferCount},
      {"semanticSceneShadowMapSkinnedInvalidPipelineCount",
       summary.semanticSceneShadowMapSkinnedInvalidPipelineCount},
      {"semanticSceneShadowMapSkinnedDrawnCount",
       summary.semanticSceneShadowMapSkinnedDrawnCount},
      {"semanticSceneShadowTaaActive",
       summary.semanticSceneShadowTaaActive},
      {"semanticSceneReceiverReuseShadowMap",
       summary.semanticSceneReceiverReuseShadowMap},
      {"semanticSceneReceiverInputValid",
       summary.semanticSceneReceiverInputValid},
      {"semanticSceneReceiverInputRejectReason",
       summary.semanticSceneReceiverInputRejectReason},
      {"semanticSceneReceiverNeedPass",
       summary.semanticSceneReceiverNeedPass},
      {"semanticSceneReceiverNeedShadowMap",
       summary.semanticSceneReceiverNeedShadowMap},
      {"semanticSceneReceiverHasCompleteShadowMap",
       summary.semanticSceneReceiverHasCompleteShadowMap},
      {"semanticSceneReceiverHasUsableDirectionalShadow",
       summary.semanticSceneReceiverHasUsableDirectionalShadow},
      {"semanticSceneReceiverActiveStrengthMilli",
       summary.semanticSceneReceiverActiveStrengthMilli},
      {"semanticSceneReceiverUboStrengthMilli",
       summary.semanticSceneReceiverUboStrengthMilli},
      {"semanticSceneReceiverDebugMode",
       summary.semanticSceneReceiverDebugMode},
      {"semanticSceneReceiverCsmCascadeCount",
       summary.semanticSceneReceiverCsmCascadeCount},
      {"semanticSceneReceiverRunEntryFlags",
       summary.semanticSceneReceiverRunEntryFlags},
      {"semanticSceneReceiverRunEarlyReturnReason",
       summary.semanticSceneReceiverRunEarlyReturnReason},
      {"semanticSceneShadowMapExecutedThisFrame",
       summary.semanticSceneShadowMapExecutedThisFrame},
      {"semanticSceneReceiverSettingsShadowsEnabled",
       summary.semanticSceneReceiverSettingsShadowsEnabled},
      {"semanticSceneReceiverSettingsOutlineEnabled",
       summary.semanticSceneReceiverSettingsOutlineEnabled},
      {"semanticSceneReceiverSettingsRawStrengthMilli",
       summary.semanticSceneReceiverSettingsRawStrengthMilli},
      {"semanticSceneReceiverComputedShadowStrengthMilli",
       summary.semanticSceneReceiverComputedShadowStrengthMilli},
      {"semanticSceneReceiverHasSunShadow",
       summary.semanticSceneReceiverHasSunShadow},
      {"semanticSceneReceiverHasPointShadow",
       summary.semanticSceneReceiverHasPointShadow},
      {"semanticSceneReceiverNeedOutlinePass",
       summary.semanticSceneReceiverNeedOutlinePass},
      {"semanticSceneReceiverZeroStrengthFrameCount",
       summary.semanticSceneReceiverZeroStrengthFrameCount},
      {"semanticSceneReceiverDrawnWithZeroStrengthCount",
       summary.semanticSceneReceiverDrawnWithZeroStrengthCount},
      {"semanticSceneReceiverNoCompleteShadowMapCount",
       summary.semanticSceneReceiverNoCompleteShadowMapCount},
      {"semanticSceneReceiverNoShadowMapImageCount",
       summary.semanticSceneReceiverNoShadowMapImageCount},
      {"semanticSceneReceiverNoShadowMapSampleViewCount",
       summary.semanticSceneReceiverNoShadowMapSampleViewCount},
      {"semanticSceneReceiverNoCandidateCsmCount",
       summary.semanticSceneReceiverNoCandidateCsmCount},
      {"semanticSceneReceiverCsmFallbackToLastGoodCount",
       summary.semanticSceneReceiverCsmFallbackToLastGoodCount},
      {"semanticSceneReceiverHoldInvalidCsmCount",
       summary.semanticSceneReceiverHoldInvalidCsmCount},
      {"semanticSceneReceiverHoldEmptyReplayCount",
       summary.semanticSceneReceiverHoldEmptyReplayCount},
      {"semanticSceneReceiverHoldIdentityChurnCount",
       summary.semanticSceneReceiverHoldIdentityChurnCount},
      {"semanticSceneReceiverReuseInvalidatedAfterEnsureCount",
       summary.semanticSceneReceiverReuseInvalidatedAfterEnsureCount},
      {"semanticSceneShadowMapRenderSkippedNoResourcesCount",
       summary.semanticSceneShadowMapRenderSkippedNoResourcesCount},
      {"semanticSceneShadowMapRenderSkippedNoMatrixBufferCount",
       summary.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount},
      {"semanticSceneReceiverViewportX",
       summary.semanticSceneReceiverViewportX},
      {"semanticSceneReceiverViewportY",
       summary.semanticSceneReceiverViewportY},
      {"semanticSceneReceiverViewportWidth",
       summary.semanticSceneReceiverViewportWidth},
      {"semanticSceneReceiverViewportHeight",
       summary.semanticSceneReceiverViewportHeight},
      {"dynamicPoseSignature", summary.dynamicPoseSignature},
      {"shadowCadenceSampleSerial", summary.shadowCadenceSampleSerial},
      {"shadowCadenceSampleCountTotal",
       summary.shadowCadenceSampleCountTotal},
      {"shadowCadenceSameDynamicPoseStreak",
       summary.shadowCadenceSameDynamicPoseStreak},
      {"shadowCadenceSameDynamicPoseStreakMax",
       summary.shadowCadenceSameDynamicPoseStreakMax},
      {"shadowCadenceSameSceneFrameStreak",
       summary.shadowCadenceSameSceneFrameStreak},
      {"shadowCadenceSameSceneFrameStreakMax",
       summary.shadowCadenceSameSceneFrameStreakMax},
      {"shadowCadenceShadowMapReuseStreak",
       summary.shadowCadenceShadowMapReuseStreak},
      {"shadowCadenceShadowMapReuseStreakMax",
       summary.shadowCadenceShadowMapReuseStreakMax},
      {"shadowCadenceSampleCount", summary.shadowCadenceSampleCount},
      {"shadowCadenceSamples", cadenceSamples},
      {"currentDrawLastMissReasonName",
       dxvk::war3::render::DescribeCurrentDrawMissReason(
           static_cast<uint32_t>(summary.currentDrawLastMissReason))},
      {"semanticSceneSubmittedExplicitBlendContract",
       summary.semanticSceneSubmittedExplicitBlendContract},
      {"semanticSceneSubmittedSingleMatrixGroupSkinning",
       summary.semanticSceneSubmittedSingleMatrixGroupSkinning},
      {"semanticSceneSubmittedMultiGroupSlotSkinning",
       summary.semanticSceneSubmittedMultiGroupSlotSkinning},
      {"semanticSceneSkinnedMinUniqueGroupSlots",
       summary.semanticSceneSkinnedMinUniqueGroupSlots},
      {"semanticSceneSkinnedMaxUniqueGroupSlots",
       summary.semanticSceneSkinnedMaxUniqueGroupSlots},
      {"semanticSceneSkinnedGroupSlotsUnique1Count",
       summary.semanticSceneSkinnedGroupSlotsUnique1Count},
      {"semanticSceneSkinnedGroupSlotsUnique2To4Count",
       summary.semanticSceneSkinnedGroupSlotsUnique2To4Count},
      {"semanticSceneSkinnedGroupSlotsUnique5To8Count",
       summary.semanticSceneSkinnedGroupSlotsUnique5To8Count},
      {"semanticSceneSkinnedGroupSlotsUnique9To16Count",
       summary.semanticSceneSkinnedGroupSlotsUnique9To16Count},
      {"semanticSceneSkinnedGroupSlotsUnique17PlusCount",
       summary.semanticSceneSkinnedGroupSlotsUnique17PlusCount},
      {"semanticSceneExplicitBlendUnavailableCurrentDraw",
       summary.semanticSceneExplicitBlendUnavailableCurrentDraw},
      {"semanticSceneSkinnedFullIndexFallbackCount",
       summary.semanticSceneSkinnedFullIndexFallbackCount},
      {"semanticSceneSkinnedMissingVisibleIndexSliceRejectCount",
       summary.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount},
      {"semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr",
       summary.semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr},
      {"semanticSceneSkinnedFullIndexFallbackLastIndexCount",
       summary.semanticSceneSkinnedFullIndexFallbackLastIndexCount},
      {"semanticSceneSubmittedFrameLocal",
       summary.semanticSceneSubmittedFrameLocal},
      {"semanticSceneSubmittedPersistent",
       summary.semanticSceneSubmittedPersistent},
      {"semanticSceneStatsPublishCount",
       summary.semanticSceneStatsPublishCount},
      {"semanticSceneInputDrawCount", summary.semanticSceneInputDrawCount},
      {"semanticSceneInputSkinnedCount",
       summary.semanticSceneInputSkinnedCount},
      {"semanticSceneTailBoundaryCandidateCount",
       summary.semanticSceneTailBoundaryCandidateCount},
      {"semanticSceneTailBoundaryCommitCount",
       summary.semanticSceneTailBoundaryCommitCount},
      {"semanticScenePopulateAttemptCount",
       summary.semanticScenePopulateAttemptCount},
      {"semanticScenePopulateUnitsOnlyCount",
       summary.semanticScenePopulateUnitsOnlyCount},
      {"semanticSceneLastInputDrawCount",
       summary.semanticSceneLastInputDrawCount},
      {"semanticSceneLastInputSkinnedCount",
       summary.semanticSceneLastInputSkinnedCount},
      {"semanticSceneLastSubmittedDrawCount",
       summary.semanticSceneLastSubmittedDrawCount},
      {"semanticSceneLastUnitsOnlyFilteredCount",
       summary.semanticSceneLastUnitsOnlyFilteredCount},
      {"semanticSceneCatchupAttemptCount",
       summary.semanticSceneCatchupAttemptCount},
      {"semanticSceneCatchupSuccessCount",
       summary.semanticSceneCatchupSuccessCount},
      {"semanticSceneSkippedEmptyFrameCount",
       summary.semanticSceneSkippedEmptyFrameCount},
      {"semanticSceneZeroSubmitCount", summary.semanticSceneZeroSubmitCount},
      {"semanticSceneSelectedFrameEligibleZeroCount",
       summary.semanticSceneSelectedFrameEligibleZeroCount},
      {"semanticSceneReusableFrameForcedCount",
       summary.semanticSceneReusableFrameForcedCount},
      {"semanticSceneReusableFrameUnavailableCount",
       summary.semanticSceneReusableFrameUnavailableCount},
      {"semanticSceneReusableFrameRejectedNativeValidationCount",
       summary.semanticSceneReusableFrameRejectedNativeValidationCount},
      {"semanticSceneLastFrameSerial",
       summary.semanticSceneLastFrameSerial},
      {"semanticSceneLastSelectedFrameSerial",
       summary.semanticSceneLastSelectedFrameSerial},
      {"semanticSceneLastReusableFrameSerial",
       summary.semanticSceneLastReusableFrameSerial},
      {"semanticSceneLastSourcePublishRevision",
       summary.semanticSceneLastSourcePublishRevision},
      {"semanticSceneLastTargetPublishRevision",
       summary.semanticSceneLastTargetPublishRevision},
      {"semanticScenePublishRevisionLag",
       summary.semanticScenePublishRevisionLag},
      {"semanticSceneBypassUnitLikeCount",
       summary.semanticSceneBypassUnitLikeCount},
      {"semanticSceneBypassUnitLikeWithRuntimeModel",
       summary.semanticSceneBypassUnitLikeWithRuntimeModel},
      {"semanticSceneBypassUnitLikeWithModelResource",
       summary.semanticSceneBypassUnitLikeWithModelResource},
      {"semanticSceneBypassUnitLikeWithPose",
       summary.semanticSceneBypassUnitLikeWithPose},
      {"semanticSceneBypassUnitLikeWithRenderable",
       summary.semanticSceneBypassUnitLikeWithRenderable},
      {"semanticSceneBypassPublishedVisibleCandidate",
       summary.semanticSceneBypassPublishedVisibleCandidate},
      {"semanticSceneBypassPublishMiss",
       summary.semanticSceneBypassPublishMiss},
      {"semanticSceneSkippedUnitsOnlyFilter",
       summary.semanticSceneSkippedUnitsOnlyFilter},
      {"semanticSceneAcceptedExplicitResourceOwnerRigid",
       summary.semanticSceneAcceptedExplicitResourceOwnerRigid},
      {"semanticSceneRejectedGeometry",
       summary.semanticSceneRejectedGeometry},
      {"semanticSceneRejectedGeometryFrameLocal",
       summary.semanticSceneRejectedGeometryFrameLocal},
      {"semanticSceneRejectedGeometryPersistent",
       summary.semanticSceneRejectedGeometryPersistent},
      {"semanticFallbackPruned", summary.semanticFallbackPruned},
      {"semanticFallbackPrunedByHandle",
       summary.semanticFallbackPrunedByHandle},
      {"semanticFallbackPrunedByWorldObjectEntry",
       summary.semanticFallbackPrunedByWorldObjectEntry},
      {"semanticFallbackPrunedBySceneNode",
       summary.semanticFallbackPrunedBySceneNode},
      {"semanticFallbackPrunedByRuntimeModel",
       summary.semanticFallbackPrunedByRuntimeModel},
      {"persistentGeometryCount", summary.persistentGeometryCount},
      {"duplicateGeometryInstances", summary.duplicateGeometryInstances},
      {"instancedGeometryDrawsSaved", summary.instancedGeometryDrawsSaved},
      {"semanticCoreManifestFrameSerial",
       summary.semanticCoreManifestFrameSerial},
      {"semanticCoreManifestPublishRevision",
       summary.semanticCoreManifestPublishRevision},
      {"semanticCoreFrameSerial", summary.semanticCoreFrameSerial},
      {"semanticCoreSourcePublishRevision",
       summary.semanticCoreSourcePublishRevision},
      {"semanticCoreSourceVisibleCount",
       summary.semanticCoreSourceVisibleCount},
      {"semanticCoreSourceStableIdentityCount",
       summary.semanticCoreSourceStableIdentityCount},
      {"semanticCoreSourceResolvedGeosetCount",
       summary.semanticCoreSourceResolvedGeosetCount},
      {"semanticCoreSourceUnitCount", summary.semanticCoreSourceUnitCount},
      {"semanticCorePublishRevisionLag",
       summary.semanticCorePublishRevisionLag},
      {"semanticCoreFrameLag", summary.semanticCoreFrameLag},
      {"semanticCoreFrameFresh", summary.semanticCoreFrameFresh},
      {"semanticCoreConsidered", summary.semanticCoreConsidered},
      {"semanticCoreResolved", summary.semanticCoreResolved},
      {"semanticCoreRigidResolved", summary.semanticCoreRigidResolved},
      {"semanticCoreSlowestRecordResolveUs",
       summary.semanticCoreSlowestRecordResolveUs},
      {"semanticCoreSlowestRecordIndex",
       summary.semanticCoreSlowestRecordIndex},
      {"semanticCoreSlowestRecordRuntimeModelPtr",
       summary.semanticCoreSlowestRecordRuntimeModelPtr},
      {"semanticCoreSlowestRecordModelResourcePtr",
       summary.semanticCoreSlowestRecordModelResourcePtr},
      {"semanticCoreSlowestRecordRuntimeGeosetPtr",
       summary.semanticCoreSlowestRecordRuntimeGeosetPtr},
      {"semanticCoreSlowestRecordRuntimeGeosetDataPtr",
       summary.semanticCoreSlowestRecordRuntimeGeosetDataPtr},
      {"semanticCoreSlowestRecordGeosetIndex",
       summary.semanticCoreSlowestRecordGeosetIndex},
      {"semanticCoreSlowestRecordObjectKind",
       summary.semanticCoreSlowestRecordObjectKind},
      {"semanticCoreSlowestResourceLookupUs",
       summary.semanticCoreSlowestResourceLookupUs},
      {"semanticCoreSlowestPoseResolveUs",
       summary.semanticCoreSlowestPoseResolveUs},
      {"semanticCoreSlowestPoseDirectLookupUs",
       summary.semanticCoreSlowestPoseDirectLookupUs},
      {"semanticCoreSlowestPoseOwnerLookupUs",
       summary.semanticCoreSlowestPoseOwnerLookupUs},
      {"semanticCoreSlowestPoseSpriteLookupUs",
       summary.semanticCoreSlowestPoseSpriteLookupUs},
      {"semanticCoreSlowestPoseInstanceRegistryUs",
       summary.semanticCoreSlowestPoseInstanceRegistryUs},
      {"semanticCoreSlowestPoseShadowRegistryUs",
       summary.semanticCoreSlowestPoseShadowRegistryUs},
      {"semanticCoreSlowestPoseRenderRegistryUs",
       summary.semanticCoreSlowestPoseRenderRegistryUs},
      {"semanticCoreSlowestPoseRuntimeRootsUs",
       summary.semanticCoreSlowestPoseRuntimeRootsUs},
      {"semanticCoreSlowestPoseMeshPoseContextUs",
       summary.semanticCoreSlowestPoseMeshPoseContextUs},
      {"semanticCoreSlowestPoseMissDiagnosticUs",
       summary.semanticCoreSlowestPoseMissDiagnosticUs},
      {"semanticCoreSlowestLayerContractUs",
       summary.semanticCoreSlowestLayerContractUs},
      {"semanticCoreSlowestRuntimeGroupPaletteUs",
       summary.semanticCoreSlowestRuntimeGroupPaletteUs},
      {"semanticCoreSlowestRuntimeGroupPaletteRescueUs",
       summary.semanticCoreSlowestRuntimeGroupPaletteRescueUs},
      {"semanticCoreSlowestAttachmentRigidResolveUs",
       summary.semanticCoreSlowestAttachmentRigidResolveUs},
      {"semanticCoreExplicitResourceOwnerRigidResolved",
       summary.semanticCoreExplicitResourceOwnerRigidResolved},
      {"semanticCoreExplicitResourceOwnerRigidWorldTransformResolved",
       summary.semanticCoreExplicitResourceOwnerRigidWorldTransformResolved},
      {"semanticCoreExplicitResourceOwnerRigidNoMatrixPalette",
       summary.semanticCoreExplicitResourceOwnerRigidNoMatrixPalette},
      {"semanticCoreAttachmentRigidResolved",
       summary.semanticCoreAttachmentRigidResolved},
      {"semanticCoreAttachmentRigidSupplementalAttachmentCount",
       summary.semanticCoreAttachmentRigidSupplementalAttachmentCount},
      {"semanticCoreAttachmentRigidSupplementalResourceCandidateCount",
       summary.semanticCoreAttachmentRigidSupplementalResourceCandidateCount},
      {"semanticCoreAttachmentRigidSupplementalResolvedCount",
       summary.semanticCoreAttachmentRigidSupplementalResolvedCount},
      {"semanticCoreAttachmentRigidSupplementalResourceMissCount",
       summary.semanticCoreAttachmentRigidSupplementalResourceMissCount},
      {"semanticCoreSkinnedResolved", summary.semanticCoreSkinnedResolved},
      {"semanticCoreRigidCandidateCount",
       summary.semanticCoreRigidCandidateCount},
      {"semanticCoreSkinnedCandidateCount",
       summary.semanticCoreSkinnedCandidateCount},
      {"semanticCoreSkinnedCandidatePoseReadyCount",
       summary.semanticCoreSkinnedCandidatePoseReadyCount},
      {"semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount",
       summary.semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount},
      {"semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount",
       summary.semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount},
      {"semanticCoreRuntimeGroupPaletteMissNoSkinningData",
       summary.semanticCoreRuntimeGroupPaletteMissNoSkinningData},
      {"semanticCoreRuntimeGroupPaletteMissNoPosePalette",
       summary.semanticCoreRuntimeGroupPaletteMissNoPosePalette},
      {"semanticCoreRuntimeGroupPaletteMissNoVertexGroups",
       summary.semanticCoreRuntimeGroupPaletteMissNoVertexGroups},
      {"semanticCoreRuntimeGroupPaletteMissInvalidGroupTable",
       summary.semanticCoreRuntimeGroupPaletteMissInvalidGroupTable},
      {"semanticCoreRuntimeGroupPaletteMissMatrixIndexOutOfRange",
       summary.semanticCoreRuntimeGroupPaletteMissMatrixIndexOutOfRange},
      {"semanticCoreRuntimeGroupPaletteMissVertexGroupOutOfRange",
       summary.semanticCoreRuntimeGroupPaletteMissVertexGroupOutOfRange},
      {"semanticCoreRuntimeGroupPaletteMissFallbacksFailed",
       summary.semanticCoreRuntimeGroupPaletteMissFallbacksFailed},
      {"semanticCoreRuntimeGroupPaletteMissLastPoseCount",
       summary.semanticCoreRuntimeGroupPaletteMissLastPoseCount},
      {"semanticCoreRuntimeGroupPaletteMissLastGroupCount",
       summary.semanticCoreRuntimeGroupPaletteMissLastGroupCount},
      {"semanticCoreRuntimeGroupPaletteMissLastMaxVertexGroupSlot",
       summary.semanticCoreRuntimeGroupPaletteMissLastMaxVertexGroupSlot},
      {"semanticCoreRuntimeGroupPaletteMissLastMatrixIndexCount",
       summary.semanticCoreRuntimeGroupPaletteMissLastMatrixIndexCount},
      {"semanticCoreRuntimeGroupPaletteMissLastMatrixIndex",
       summary.semanticCoreRuntimeGroupPaletteMissLastMatrixIndex},
      {"semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext",
       summary.semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext},
      {"semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose",
       summary.semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose},
      {"semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot",
       summary.semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot},
      {"semanticCoreRuntimeGroupPaletteRescueByChildRuntime",
       summary.semanticCoreRuntimeGroupPaletteRescueByChildRuntime},
      {"semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime",
       summary.semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime},
      {"semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed",
       summary.semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed},
      {"semanticCoreAttachmentRigidMatchByChildSprite",
       summary.semanticCoreAttachmentRigidMatchByChildSprite},
      {"semanticCoreAttachmentRigidMatchByChildRuntimeModel",
       summary.semanticCoreAttachmentRigidMatchByChildRuntimeModel},
      {"semanticCoreAttachmentRigidMatchByChildRuntimeGeoset",
       summary.semanticCoreAttachmentRigidMatchByChildRuntimeGeoset},
      {"semanticCoreAttachmentRigidMatchByChildSpriteRuntimeGeoset",
       summary.semanticCoreAttachmentRigidMatchByChildSpriteRuntimeGeoset},
      {"semanticCoreAttachmentRigidMatchByOwnerRuntimeGeoset",
       summary.semanticCoreAttachmentRigidMatchByOwnerRuntimeGeoset},
      {"semanticCoreAttachmentRigidMatchByRootRuntimeGeoset",
       summary.semanticCoreAttachmentRigidMatchByRootRuntimeGeoset},
      {"semanticCoreAttachmentRigidMatchByResourceRuntimeOwner",
       summary.semanticCoreAttachmentRigidMatchByResourceRuntimeOwner},
      {"semanticCoreAttachmentRigidMatchByRenderableRuntimeRoot",
       summary.semanticCoreAttachmentRigidMatchByRenderableRuntimeRoot},
      {"semanticCoreAttachmentRigidMatchByWorldObjectEntry",
       summary.semanticCoreAttachmentRigidMatchByWorldObjectEntry},
      {"semanticCoreAttachmentRigidMatchBySceneNode",
       summary.semanticCoreAttachmentRigidMatchBySceneNode},
      {"semanticCoreAttachmentRigidMatchByUnitPtr",
       summary.semanticCoreAttachmentRigidMatchByUnitPtr},
      {"semanticCoreAttachmentRigidMatchByHandle",
       summary.semanticCoreAttachmentRigidMatchByHandle},
      {"semanticCoreAttachmentRigidMatchByChildModelResource",
       summary.semanticCoreAttachmentRigidMatchByChildModelResource},
      {"semanticCoreAttachmentRigidMatchByUniqueIdentity",
       summary.semanticCoreAttachmentRigidMatchByUniqueIdentity},
      {"semanticCoreAttachmentRigidMatchMiss",
       summary.semanticCoreAttachmentRigidMatchMiss},
      {"lastAttachmentRigidMatchMissRuntimeModelPtr",
       summary.lastAttachmentRigidMatchMissRuntimeModelPtr},
      {"lastAttachmentRigidMatchMissModelResourcePtr",
       summary.lastAttachmentRigidMatchMissModelResourcePtr},
      {"lastAttachmentRigidMatchMissRuntimeGeosetPtr",
       summary.lastAttachmentRigidMatchMissRuntimeGeosetPtr},
      {"lastAttachmentRigidMatchMissRuntimeGeosetDataPtr",
       summary.lastAttachmentRigidMatchMissRuntimeGeosetDataPtr},
      {"lastAttachmentRigidMatchMissGeosetIndex",
       summary.lastAttachmentRigidMatchMissGeosetIndex},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr",
       summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerWorldObjectEntryPtr",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerWorldObjectEntryPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerSceneNodePtr",
       summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSceneNodePtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerUnitPtr",
       summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerUnitPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr",
       summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr},
      {"lastAttachmentRigidMatchMissOwnerSpriteContractHit",
       summary.lastAttachmentRigidMatchMissOwnerSpriteContractHit},
      {"lastAttachmentRigidMatchMissOwnerSpriteContractChildRuntimeModelPtr",
       summary
           .lastAttachmentRigidMatchMissOwnerSpriteContractChildRuntimeModelPtr},
      {"lastAttachmentRigidMatchMissOwnerSpriteContractOwnerRuntimeModelPtr",
       summary
           .lastAttachmentRigidMatchMissOwnerSpriteContractOwnerRuntimeModelPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceObjectPtr",
       summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceObjectPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceSpriteObjectPtr",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceSpriteObjectPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateModelDataPtr",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateModelDataPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateHandlePtr",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateHandlePtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateCallerRva",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateCallerRva},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerResolveCallerRva",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerResolveCallerRva},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerModelResourcePtr",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerModelResourcePtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerModelKey",
       summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerModelKey},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerPoseMatrixCount",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerPoseMatrixCount},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0Ptr",
       summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0Ptr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceObjectPtr",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceObjectPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceSpriteObjectPtr",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceSpriteObjectPtr},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0PoseMatrixCount",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0PoseMatrixCount},
      {"lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0HasWorldTransform",
       summary
           .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0HasWorldTransform},
      {"semanticCoreAttachmentRigidPoseMissNoRecord",
       summary.semanticCoreAttachmentRigidPoseMissNoRecord},
      {"semanticCoreAttachmentRigidPoseMissMissingRuntimes",
       summary.semanticCoreAttachmentRigidPoseMissMissingRuntimes},
      {"semanticCoreAttachmentRigidPoseMissNoRootPose",
       summary.semanticCoreAttachmentRigidPoseMissNoRootPose},
      {"semanticCoreAttachmentRigidPoseMissNoRootWorldTransform",
       summary.semanticCoreAttachmentRigidPoseMissNoRootWorldTransform},
      {"semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromLivePose",
       summary
           .semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromLivePose},
      {"semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromShadowRegistry",
       summary
           .semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromShadowRegistry},
      {"semanticCoreExplicitBlendAttempts",
       summary.semanticCoreExplicitBlendAttempts},
      {"semanticCoreExplicitBlendAttemptWithSpanRemapTable",
       summary.semanticCoreExplicitBlendAttemptWithSpanRemapTable},
      {"semanticCoreExplicitBlendResolved",
       summary.semanticCoreExplicitBlendResolved},
      {"semanticCoreExplicitBlendSpanRemapResolved",
       summary.semanticCoreExplicitBlendSpanRemapResolved},
      {"semanticCoreExplicitBlendStrideSearchMiss",
       summary.semanticCoreExplicitBlendStrideSearchMiss},
      {"semanticCoreExplicitBlendFinalDecodeMiss",
       summary.semanticCoreExplicitBlendFinalDecodeMiss},
      {"semanticCoreCoreDrawPacketCount",
       summary.semanticCoreCoreDrawPacketCount},
      {"semanticCoreUpperLayerResolvedItems",
       summary.semanticCoreUpperLayerResolvedItems},
      {"semanticCoreSupplementalUpperLayerDrawPacketCount",
       summary.semanticCoreSupplementalUpperLayerDrawPacketCount},
      {"semanticCoreDrawPacketCount", summary.semanticCoreDrawPacketCount},
      {"semanticCoreSubmittedDrawCount",
       summary.semanticCoreSubmittedDrawCount},
      {"semanticCoreLastFrameSourcePublishRevision",
       summary.semanticCoreLastFrameSourcePublishRevision},
      {"semanticCoreLastFrameDrawCount",
       summary.semanticCoreLastFrameDrawCount},
      {"semanticCoreLastFrameSkinnedDrawCount",
       summary.semanticCoreLastFrameSkinnedDrawCount},
      {"semanticCoreRenderableFrameSourcePublishRevision",
       summary.semanticCoreRenderableFrameSourcePublishRevision},
      {"semanticCoreRenderableFrameDrawCount",
       summary.semanticCoreRenderableFrameDrawCount},
      {"semanticCoreRenderableFrameSkinnedDrawCount",
       summary.semanticCoreRenderableFrameSkinnedDrawCount},
      {"nativeD3D9BackendFrameSerial", summary.nativeD3D9BackendFrameSerial},
      {"nativeD3D9BackendSourcePublishRevision",
       summary.nativeD3D9BackendSourcePublishRevision},
      {"nativeD3D9BackendSubmittedDrawCount",
       summary.nativeD3D9BackendSubmittedDrawCount},
      {"nativeD3D9BackendSubmittedRigidDrawCount",
       summary.nativeD3D9BackendSubmittedRigidDrawCount},
      {"nativeD3D9BackendSubmittedSkinnedDrawCount",
       summary.nativeD3D9BackendSubmittedSkinnedDrawCount},
      {"nativeD3D9BackendExecutedFrameSerial",
       summary.nativeD3D9BackendExecutedFrameSerial},
      {"nativeD3D9BackendExecutedDrawCount",
       summary.nativeD3D9BackendExecutedDrawCount},
      {"nativeD3D9BackendExecutedRigidDrawCount",
       summary.nativeD3D9BackendExecutedRigidDrawCount},
      {"nativeD3D9BackendExecutedSkinnedDrawCount",
       summary.nativeD3D9BackendExecutedSkinnedDrawCount},
      {"nativeD3D9BackendExecuteAttemptCount",
       summary.nativeD3D9BackendExecuteAttemptCount},
      {"nativeD3D9BackendExecuteSuccessCount",
       summary.nativeD3D9BackendExecuteSuccessCount},
      {"nativeD3D9BackendLastSuccessfulExecutedFrameSerial",
       summary.nativeD3D9BackendLastSuccessfulExecutedFrameSerial},
      {"nativeD3D9BackendLastSuccessfulExecutedDrawCount",
       summary.nativeD3D9BackendLastSuccessfulExecutedDrawCount},
      {"nativeD3D9BackendExecuteSkippedNoDeviceCount",
       summary.nativeD3D9BackendExecuteSkippedNoDeviceCount},
      {"nativeD3D9BackendExecuteSkippedNoDrawsCount",
       summary.nativeD3D9BackendExecuteSkippedNoDrawsCount},
      {"nativeD3D9BackendLastExecuteSubmittedDrawCount",
       summary.nativeD3D9BackendLastExecuteSubmittedDrawCount},
      {"nativeD3D9BackendLastExecuteFailedDrawCount",
       summary.nativeD3D9BackendLastExecuteFailedDrawCount},
      {"nativeD3D9BackendLastExecuteSubmittedRigidDrawCount",
       summary.nativeD3D9BackendLastExecuteSubmittedRigidDrawCount},
      {"nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount",
       summary.nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount},
      {"nativeD3D9BackendLastExecuteExecutedRigidDrawCount",
       summary.nativeD3D9BackendLastExecuteExecutedRigidDrawCount},
      {"nativeD3D9BackendLastExecuteExecutedSkinnedDrawCount",
       summary.nativeD3D9BackendLastExecuteExecutedSkinnedDrawCount},
      {"nativeD3D9BackendGeometryCount",
       summary.nativeD3D9BackendGeometryCount},
      {"nativeD3D9BackendPaletteCount",
       summary.nativeD3D9BackendPaletteCount},
      {"nativeD3D9BackendMaterialCount",
       summary.nativeD3D9BackendMaterialCount},
      {"nativeD3D9BackendCanonicalDrawCount",
       summary.nativeD3D9BackendCanonicalDrawCount},
      {"nativeD3D9BackendCanonicalFrameSerial",
       summary.nativeD3D9BackendCanonicalFrameSerial},
      {"nativeD3D9BackendCanonicalPublishCount",
       summary.nativeD3D9BackendCanonicalPublishCount},
      {"nativeD3D9BackendCanonicalPublishRejectNotReadyCount",
       summary.nativeD3D9BackendCanonicalPublishRejectNotReadyCount},
      {"nativeD3D9BackendCanonicalPublishRejectNoPositionsCount",
       summary.nativeD3D9BackendCanonicalPublishRejectNoPositionsCount},
      {"nativeD3D9BackendGeometryRejectCount",
       summary.nativeD3D9BackendGeometryRejectCount},
      {"nativeD3D9BackendPaletteRejectCount",
       summary.nativeD3D9BackendPaletteRejectCount},
      {"nativeD3D9BackendMaterialRejectCount",
       summary.nativeD3D9BackendMaterialRejectCount},
      {"nativeD3D9BackendSubmitRejectCount",
       summary.nativeD3D9BackendSubmitRejectCount},
      {"nativeD3D9BackendUsedCanonicalFrame",
       summary.nativeD3D9BackendUsedCanonicalFrame},
      {"nativeSemanticWorldStageCandidateCount",
       summary.nativeSemanticWorldStageCandidateCount},
      {"nativeSemanticWorldStageCandidatePrepareCount",
       summary.nativeSemanticWorldStageCandidatePrepareCount},
      {"nativeSemanticWorldStageCandidateRefreshCount",
       summary.nativeSemanticWorldStageCandidateRefreshCount},
      {"nativeSemanticWorldStageCandidateExecuteCount",
       summary.nativeSemanticWorldStageCandidateExecuteCount},
      {"nativeSemanticWorldStageSkippedRuntimeNotReadyCount",
       summary.nativeSemanticWorldStageSkippedRuntimeNotReadyCount},
      {"nativeSemanticWorldStageLastCandidateStage",
       summary.nativeSemanticWorldStageLastCandidateStage},
      {"nativeSemanticWorldStageLastCandidateA3",
       summary.nativeSemanticWorldStageLastCandidateA3},
      {"nativeSemanticWorldStageLastCandidateA4",
       summary.nativeSemanticWorldStageLastCandidateA4},
      {"nativeSemanticWorldStageLastCandidateA5",
       summary.nativeSemanticWorldStageLastCandidateA5},
      {"nativeSemanticWorldStageLastCandidateJassReady",
       summary.nativeSemanticWorldStageLastCandidateJassReady},
      {"nativeSemanticWorldStageLastCandidateGameStarted",
       summary.nativeSemanticWorldStageLastCandidateGameStarted},
      {"nativeSemanticWorldStageLastCandidateRuntimeFrame",
       summary.nativeSemanticWorldStageLastCandidateRuntimeFrame},
      {"nativeSemanticWorldStagePrepareAttemptCount",
       summary.nativeSemanticWorldStagePrepareAttemptCount},
      {"nativeSemanticWorldStagePrepareSuccessCount",
       summary.nativeSemanticWorldStagePrepareSuccessCount},
      {"nativeSemanticWorldStageExecuteAttemptCount",
       summary.nativeSemanticWorldStageExecuteAttemptCount},
      {"nativeSemanticWorldStageExecuteSuccessCount",
       summary.nativeSemanticWorldStageExecuteSuccessCount},
      {"nativeSemanticWorldStageLastPrepareStage",
       summary.nativeSemanticWorldStageLastPrepareStage},
      {"nativeSemanticWorldStageLastExecuteStage",
       summary.nativeSemanticWorldStageLastExecuteStage},
      {"nativeSemanticWorldStageLastPrepareFrameSerial",
       summary.nativeSemanticWorldStageLastPrepareFrameSerial},
      {"nativeSemanticWorldStageLastExecuteFrameSerial",
       summary.nativeSemanticWorldStageLastExecuteFrameSerial},
      {"nativeSemanticWorldStageLastPrepareDrawCount",
       summary.nativeSemanticWorldStageLastPrepareDrawCount},
      {"nativeSemanticWorldStageLastExecuteDrawCount",
       summary.nativeSemanticWorldStageLastExecuteDrawCount},
      {"sourceObjectRenderBridgeResolvedByEntryCount",
       summary.sourceObjectRenderBridgeResolvedByEntryCount},
      {"sourceObjectRenderBridgeResolvedBySceneNodeCount",
       summary.sourceObjectRenderBridgeResolvedBySceneNodeCount},
      {"spriteHostBindCount", summary.spriteHostBindCount},
      {"lastAttachmentChildLineageBootstrapParentRuntimeModelPtr",
       summary.lastAttachmentChildLineageBootstrapParentRuntimeModelPtr},
      {"lastAttachmentChildLineageBootstrapChildRuntimeModelPtr",
       summary.lastAttachmentChildLineageBootstrapChildRuntimeModelPtr},
      {"lastAttachmentChildLineageBootstrapParentModelDataPtr",
       summary.lastAttachmentChildLineageBootstrapParentModelDataPtr},
      {"lastAttachmentChildLineageBootstrapChildModelDataPtr",
       summary.lastAttachmentChildLineageBootstrapChildModelDataPtr},
      {"lastAttachmentChildLineageBootstrapChildModelResourcePtr",
       summary.lastAttachmentChildLineageBootstrapChildModelResourcePtr},
      {"lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr",
       summary.lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr},
      {"lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr",
       summary.lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr},
      {"lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr",
       summary.lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr},
      {"lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr",
       summary.lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr},
      {"lastAttachmentChildLineageBootstrapSourceMeta",
       summary.lastAttachmentChildLineageBootstrapSourceMeta},
      {"lastAttachmentChildLineageBootstrapBucketIndex",
       summary.lastAttachmentChildLineageBootstrapBucketIndex},
      {"lastAttachmentChildLineageBootstrapModelDataLinkCount",
       summary.lastAttachmentChildLineageBootstrapModelDataLinkCount},
      {"lastAttachmentChildLineageBootstrapRuntimeLinkCount",
       summary.lastAttachmentChildLineageBootstrapRuntimeLinkCount},
      {"lastAttachmentChildLineageBootstrapStrictCandidateCount",
       summary.lastAttachmentChildLineageBootstrapStrictCandidateCount},
      {"lastAttachmentChildLineageBootstrapSourceCandidateCount",
       summary.lastAttachmentChildLineageBootstrapSourceCandidateCount},
      {"lastAttachmentChildLineageBootstrapBucketCandidateCount",
       summary.lastAttachmentChildLineageBootstrapBucketCandidateCount},
      {"lastAttachmentChildLineageBootstrapAllCandidateCount",
       summary.lastAttachmentChildLineageBootstrapAllCandidateCount},
      {"lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal",
       summary.lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal},
      {"lastAttachmentChildLineageBootstrapModelDataBucketCount",
       summary.lastAttachmentChildLineageBootstrapModelDataBucketCount},
      {"spriteHostBindResolvedIdentityCount",
       summary.spriteHostBindResolvedIdentityCount},
      {"spriteHostBindResolvedUnitCount",
       summary.spriteHostBindResolvedUnitCount},
      {"spriteHostBindResolvedHandleCount",
       summary.spriteHostBindResolvedHandleCount},
      {"spriteHostBindResolvedRawcodeCount",
       summary.spriteHostBindResolvedRawcodeCount},
      // Phase 7.105 opening-skip 诊断。
      {"spriteHostBindOpeningSkipCount",
       summary.spriteHostBindOpeningSkipCount},
      {"runtimePaletteTreeOpeningSkipCount",
       summary.runtimePaletteTreeOpeningSkipCount},
      {"spriteFrameSourceHintCount", summary.spriteFrameSourceHintCount},
      {"spriteFrameSourceResolvedIdentityCount",
       summary.spriteFrameSourceResolvedIdentityCount},
      {"spriteFrameSourceResolvedUnitCount",
       summary.spriteFrameSourceResolvedUnitCount},
      {"spriteFrameSourceResolvedHandleCount",
       summary.spriteFrameSourceResolvedHandleCount},
      {"spriteFrameSourceResolvedRawcodeCount",
       summary.spriteFrameSourceResolvedRawcodeCount},
      {"spriteFrameSourceBaseAliasPublishCount",
       summary.spriteFrameSourceBaseAliasPublishCount},
      {"spriteFrameSourceDeepIdentityResolvedCount",
       summary.spriteFrameSourceDeepIdentityResolvedCount},
      {"spriteFrameSourceObjectRuntimeFieldCandidateCount",
       summary.spriteFrameSourceObjectRuntimeFieldCandidateCount},
      {"spriteFrameSourceObjectRegistryFieldHitCount",
       summary.spriteFrameSourceObjectRegistryFieldHitCount},
      {"spriteFramePoseBaseAliasPublishCount",
       summary.spriteFramePoseBaseAliasPublishCount},
      {"spriteFramePoseBaseAliasMatrixPaletteCount",
       summary.spriteFramePoseBaseAliasMatrixPaletteCount},
      {"spriteFrameAttachmentRootRuntimeHitCount",
       summary.spriteFrameAttachmentRootRuntimeHitCount},
      {"spriteFrameAttachmentOwnerRuntimeHitCount",
       summary.spriteFrameAttachmentOwnerRuntimeHitCount},
      {"spriteFrameAttachmentChildRuntimeHitCount",
       summary.spriteFrameAttachmentChildRuntimeHitCount},
      {"spriteFrameAttachmentContextHintCount",
       summary.spriteFrameAttachmentContextHintCount},
      {"spriteFrameAttachmentFullUpdateHitCount",
       summary.spriteFrameAttachmentFullUpdateHitCount},
      {"spriteFrameAttachmentLiteUpdateHitCount",
       summary.spriteFrameAttachmentLiteUpdateHitCount},
      {"spriteFrameAttachmentCallerKnownCount",
       summary.spriteFrameAttachmentCallerKnownCount},
      {"spriteFrameAttachmentCallerChangedCount",
       summary.spriteFrameAttachmentCallerChangedCount},
      {"spriteFrameAttachmentAttachScopeHitCount",
       summary.spriteFrameAttachmentAttachScopeHitCount},
      {"spriteFrameAttachmentAttachScopeOwnerHitCount",
       summary.spriteFrameAttachmentAttachScopeOwnerHitCount},
      {"spriteFrameAttachmentAttachScopeParentRuntimeMatchCount",
       summary.spriteFrameAttachmentAttachScopeParentRuntimeMatchCount},
      {"attachedEffectInitBindCount", summary.attachedEffectInitBindCount},
      {"attachedEffectInitResolvedIdentityCount",
       summary.attachedEffectInitResolvedIdentityCount},
      {"attachedEffectInitResolvedUnitCount",
       summary.attachedEffectInitResolvedUnitCount},
      {"attachedEffectInitResolvedHandleCount",
       summary.attachedEffectInitResolvedHandleCount},
      {"attachedEffectInitResolvedRawcodeCount",
       summary.attachedEffectInitResolvedRawcodeCount},
      {"attachedEffectInitParentRuntimeOwnerPublishCount",
       summary.attachedEffectInitParentRuntimeOwnerPublishCount},
      {"attachedEffectDirectBindCount", summary.attachedEffectDirectBindCount},
      {"attachedEffectDirectResolvedIdentityCount",
       summary.attachedEffectDirectResolvedIdentityCount},
      {"attachedEffectDirectResolvedUnitCount",
       summary.attachedEffectDirectResolvedUnitCount},
      {"attachedEffectDirectResolvedHandleCount",
       summary.attachedEffectDirectResolvedHandleCount},
      {"attachedEffectDirectResolvedRawcodeCount",
       summary.attachedEffectDirectResolvedRawcodeCount},
      {"attachModelToPointBindCount", summary.attachModelToPointBindCount},
      {"attachModelToPointResolvedIdentityCount",
       summary.attachModelToPointResolvedIdentityCount},
      {"attachModelToPointResolvedUnitCount",
       summary.attachModelToPointResolvedUnitCount},
      {"attachModelToPointResolvedHandleCount",
       summary.attachModelToPointResolvedHandleCount},
      {"attachModelToPointResolvedRawcodeCount",
       summary.attachModelToPointResolvedRawcodeCount},
      {"attachModelToPointPromotedAttachmentChildRuntimeCount",
       summary.attachModelToPointPromotedAttachmentChildRuntimeCount},
      {"attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount",
       summary
           .attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount},
      {"currentRenderIdentityHintCount",
       summary.currentRenderIdentityHintCount},
      {"currentRenderIdentityResolvedCount",
       summary.currentRenderIdentityResolvedCount},
      {"sourceObjectIdentityHintResolvedCount",
       summary.sourceObjectIdentityHintResolvedCount},
      {"runtimeSourceObjectPublishCount",
       summary.runtimeSourceObjectPublishCount},
      {"attachmentRigidPublishedWithSourceObjectCount",
       summary.attachmentRigidPublishedWithSourceObjectCount},
      {"attachmentRigidSourceObjectFromChildRuntimeCount",
       summary.attachmentRigidSourceObjectFromChildRuntimeCount},
      {"attachmentRigidSourceObjectFromOwnerRuntimeCount",
       summary.attachmentRigidSourceObjectFromOwnerRuntimeCount},
      {"attachmentRigidSourceObjectFromRootRuntimeCount",
       summary.attachmentRigidSourceObjectFromRootRuntimeCount},
      {"overrideOutputSampleFrame", summary.overrideOutputSampleFrame},
      {"overrideOutputLastActiveFrame",
       summary.overrideOutputLastActiveFrame},
      {"overridePrimaryPresetWriteCount",
       summary.overridePrimaryPresetWriteCount},
      {"overrideSharedPresetWriteCount",
      summary.overrideSharedPresetWriteCount},
      {"overrideLocalPointWriteCount",
       summary.overrideLocalPointWriteCount},
      {"overrideLocalPointNonZeroWriteCount",
       summary.overrideLocalPointNonZeroWriteCount},
      {"overrideLocalPointObservedChildLinkWriteCount",
       summary.overrideLocalPointObservedChildLinkWriteCount},
      {"overrideLocalPointMatchedChildLinkWriteCount",
       summary.overrideLocalPointMatchedChildLinkWriteCount},
      {"overrideLocalPointMatchedChildPaletteReadyWriteCount",
       summary.overrideLocalPointMatchedChildPaletteReadyWriteCount},
      {"overrideLocalPointMatchedChildLinkBySourceRecordWriteCount",
       summary.overrideLocalPointMatchedChildLinkBySourceRecordWriteCount},
      {"overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount",
       summary.overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount},
      {"overrideLocalPointContextRuntimeWithChildLinksWriteCount",
       summary.overrideLocalPointContextRuntimeWithChildLinksWriteCount},
      {"overrideLocalPointContextMatchedChildLinkWriteCount",
       summary.overrideLocalPointContextMatchedChildLinkWriteCount},
      {"overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount",
       summary.overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount},
      {"overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount",
       summary.overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount},
      {"overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount",
       summary.overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount},
      {"overrideLocalPointScratchRootMatchedChildLinkWriteCount",
       summary.overrideLocalPointScratchRootMatchedChildLinkWriteCount},
      {"overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount",
       summary.overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount},
      {"overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount",
       summary.overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount},
      {"overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount",
       summary.overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount},
      {"overrideLocalPointArgBlockMatchedChildLinkWriteCount",
       summary.overrideLocalPointArgBlockMatchedChildLinkWriteCount},
      {"overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount",
       summary.overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount},
      {"overrideLocalPointArgBlockIdentityHintWriteCount",
       summary.overrideLocalPointArgBlockIdentityHintWriteCount},
      {"overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount",
       summary.overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount},
      {"overrideLocalPointArg4BlockMatchedChildLinkWriteCount",
       summary.overrideLocalPointArg4BlockMatchedChildLinkWriteCount},
      {"overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount",
       summary.overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount},
      {"overrideLocalPointArg4BlockIdentityHintWriteCount",
       summary.overrideLocalPointArg4BlockIdentityHintWriteCount},
      {"overrideLocalPointChildSourceMetaIdentityHintWriteCount",
       summary.overrideLocalPointChildSourceMetaIdentityHintWriteCount},
      {"overrideLocalPointSpriteBoundCandidateWriteCount",
       summary.overrideLocalPointSpriteBoundCandidateWriteCount},
      {"overrideLocalPointParentSpriteIdentityHintWriteCount",
       summary.overrideLocalPointParentSpriteIdentityHintWriteCount},
      {"overrideLocalPointRootRuntimeHitWriteCount",
       summary.overrideLocalPointRootRuntimeHitWriteCount},
      {"overrideLocalPointRootRuntimeWithChildLinksWriteCount",
       summary.overrideLocalPointRootRuntimeWithChildLinksWriteCount},
      {"overrideLocalPointRootRuntimeMatchedChildLinkWriteCount",
       summary.overrideLocalPointRootRuntimeMatchedChildLinkWriteCount},
      {"overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount",
       summary.overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount},
      {"overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount",
       summary.overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount},
      {"overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount",
       summary.overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount},
      {"attachmentRigidPublishedCount",
       summary.attachmentRigidPublishedCount},
      {"overrideMaxPrimaryPresetSlotIndex",
       summary.overrideMaxPrimaryPresetSlotIndex},
      {"overrideMaxSharedPresetSlotIndex",
       summary.overrideMaxSharedPresetSlotIndex},
      {"overrideMaxLocalPointSlotIndex",
       summary.overrideMaxLocalPointSlotIndex},
      {"overrideMaxObservedChildLinkCount",
       summary.overrideMaxObservedChildLinkCount},
      {"overrideMaxObservedChildLinkTag",
       summary.overrideMaxObservedChildLinkTag},
      {"overrideLastPrimaryPresetHash",
       summary.overrideLastPrimaryPresetHash},
      {"overrideLastSharedPresetHash",
       summary.overrideLastSharedPresetHash},
      {"overrideLastRuntimeModelPtr", summary.overrideLastRuntimeModelPtr},
      {"overrideLastMatchedChildRuntimeModelPtr",
       summary.overrideLastMatchedChildRuntimeModelPtr},
      {"overrideLastMatchedChildBySourceRecordRuntimeModelPtr",
       summary.overrideLastMatchedChildBySourceRecordRuntimeModelPtr},
      {"overrideLastContextRuntimeWithChildLinksPtr",
       summary.overrideLastContextRuntimeWithChildLinksPtr},
      {"overrideLastScratchRootPtr",
       summary.overrideLastScratchRootPtr},
      {"overrideLastScratchRootRuntimeModelPtr",
       summary.overrideLastScratchRootRuntimeModelPtr},
      {"overrideLastArgBlockPtr",
       summary.overrideLastArgBlockPtr},
      {"overrideLastArgBlockRuntimeModelPtr",
       summary.overrideLastArgBlockRuntimeModelPtr},
      {"overrideLastArgBlockIdentityHintPtr",
       summary.overrideLastArgBlockIdentityHintPtr},
      {"overrideLastArg4BlockPtr",
       summary.overrideLastArg4BlockPtr},
      {"overrideLastArg4BlockRuntimeModelPtr",
       summary.overrideLastArg4BlockRuntimeModelPtr},
      {"overrideLastArg4BlockIdentityHintPtr",
       summary.overrideLastArg4BlockIdentityHintPtr},
      {"overrideLastSpriteBoundCandidateSpritePtr",
       summary.overrideLastSpriteBoundCandidateSpritePtr},
      {"overrideLastSpriteBoundCandidateRuntimeModelPtr",
       summary.overrideLastSpriteBoundCandidateRuntimeModelPtr},
      {"overrideLastParentSpriteIdentityHintSpritePtr",
       summary.overrideLastParentSpriteIdentityHintSpritePtr},
      {"overrideLastParentSpriteIdentityHintRuntimeModelPtr",
       summary.overrideLastParentSpriteIdentityHintRuntimeModelPtr},
      {"overrideLastRootRuntimeModelPtr",
       summary.overrideLastRootRuntimeModelPtr},
      {"lastSourceObjectRenderBridgeSourceObjectPtr",
       summary.lastSourceObjectRenderBridgeSourceObjectPtr},
      {"lastSourceObjectRenderBridgeSceneNodePtr",
       summary.lastSourceObjectRenderBridgeSceneNodePtr},
      {"lastSourceObjectIdentityHintSourceObjectPtr",
       summary.lastSourceObjectIdentityHintSourceObjectPtr},
      {"lastSourceObjectIdentityHintCandidatePtr",
       summary.lastSourceObjectIdentityHintCandidatePtr},
      {"lastSpriteHostSourceObjectPtr", summary.lastSpriteHostSourceObjectPtr},
      {"lastSpriteHostSpritePtr", summary.lastSpriteHostSpritePtr},
      {"lastSpriteHostRuntimeModelPtr", summary.lastSpriteHostRuntimeModelPtr},
      {"lastSpriteHostUnitPtr", summary.lastSpriteHostUnitPtr},
      {"lastSpriteFrameSourceObjectPtr",
       summary.lastSpriteFrameSourceObjectPtr},
      {"lastSpriteFrameSourceRuntimeModelPtr",
       summary.lastSpriteFrameSourceRuntimeModelPtr},
      {"lastSpriteFrameSourceBaseRuntimeModelPtr",
       summary.lastSpriteFrameSourceBaseRuntimeModelPtr},
      {"lastSpriteFrameSourceObjectVtablePtr",
       summary.lastSpriteFrameSourceObjectVtablePtr},
      {"lastSpriteFrameSourceObjectSceneNodeCandidatePtr",
       summary.lastSpriteFrameSourceObjectSceneNodeCandidatePtr},
      {"lastSpriteFrameSourceObjectSpriteCandidatePtr",
       summary.lastSpriteFrameSourceObjectSpriteCandidatePtr},
      {"lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr",
       summary.lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr},
      {"lastSpriteFrameSourceObjectRegistryFieldCandidatePtr",
       summary.lastSpriteFrameSourceObjectRegistryFieldCandidatePtr},
      {"lastSpriteFrameSourceDeepIdentityCandidatePtr",
       summary.lastSpriteFrameSourceDeepIdentityCandidatePtr},
      {"lastSpriteFrameSourceWorldObjectEntryPtr",
       summary.lastSpriteFrameSourceWorldObjectEntryPtr},
      {"lastSpriteFrameSourceSceneNodePtr",
       summary.lastSpriteFrameSourceSceneNodePtr},
      {"lastSpriteFrameSourceUnitPtr", summary.lastSpriteFrameSourceUnitPtr},
      {"lastSpriteFramePoseBaseRuntimeModelPtr",
       summary.lastSpriteFramePoseBaseRuntimeModelPtr},
      {"lastSpriteFramePoseBaseMatrixCount",
       summary.lastSpriteFramePoseBaseMatrixCount},
      {"lastSpriteFrameAttachmentSpritePtr",
       summary.lastSpriteFrameAttachmentSpritePtr},
      {"lastSpriteFrameAttachmentRuntimeModelPtr",
       summary.lastSpriteFrameAttachmentRuntimeModelPtr},
      {"lastSpriteFrameAttachmentContextPtr",
       summary.lastSpriteFrameAttachmentContextPtr},
      {"lastAttachedEffectInitOwnerWidgetPtr",
       summary.lastAttachedEffectInitOwnerWidgetPtr},
      {"lastAttachedEffectInitChildSpritePtr",
       summary.lastAttachedEffectInitChildSpritePtr},
      {"lastAttachedEffectInitChildRuntimeModelPtr",
       summary.lastAttachedEffectInitChildRuntimeModelPtr},
      {"lastAttachedEffectInitUnitPtr",
       summary.lastAttachedEffectInitUnitPtr},
      {"lastAttachedEffectDirectOwnerWidgetPtr",
       summary.lastAttachedEffectDirectOwnerWidgetPtr},
      {"lastAttachedEffectDirectChildSpritePtr",
       summary.lastAttachedEffectDirectChildSpritePtr},
      {"lastAttachedEffectDirectChildRuntimeModelPtr",
       summary.lastAttachedEffectDirectChildRuntimeModelPtr},
      {"lastAttachedEffectDirectUnitPtr",
       summary.lastAttachedEffectDirectUnitPtr},
      {"attachmentRigidSample0RootRuntimeModelPtr",
       summary.attachmentRigidSample0RootRuntimeModelPtr},
      {"attachmentRigidSample0OwnerRuntimeModelPtr",
       summary.attachmentRigidSample0OwnerRuntimeModelPtr},
      {"attachmentRigidSample0ChildRuntimeModelPtr",
       summary.attachmentRigidSample0ChildRuntimeModelPtr},
      {"attachmentRigidSample0ChildSpritePtr",
       summary.attachmentRigidSample0ChildSpritePtr},
      {"attachmentRigidSample0SourceObjectPtr",
       summary.attachmentRigidSample0SourceObjectPtr},
      {"attachmentRigidSample0RootRuntimeCreateHandlePtr",
       summary.attachmentRigidSample0RootRuntimeCreateHandlePtr},
      {"attachmentRigidSample0OwnerRuntimeCreateHandlePtr",
       summary.attachmentRigidSample0OwnerRuntimeCreateHandlePtr},
      {"attachmentRigidSample0ChildRuntimeCreateHandlePtr",
       summary.attachmentRigidSample0ChildRuntimeCreateHandlePtr},
      {"attachmentRigidSample0RootRuntimeCreateCallerRva",
       summary.attachmentRigidSample0RootRuntimeCreateCallerRva},
      {"attachmentRigidSample0OwnerRuntimeCreateCallerRva",
       summary.attachmentRigidSample0OwnerRuntimeCreateCallerRva},
      {"attachmentRigidSample0ChildRuntimeCreateCallerRva",
       summary.attachmentRigidSample0ChildRuntimeCreateCallerRva},
      {"attachmentRigidSample0RootRuntimeResolveCallerRva",
       summary.attachmentRigidSample0RootRuntimeResolveCallerRva},
      {"attachmentRigidSample0OwnerRuntimeResolveCallerRva",
       summary.attachmentRigidSample0OwnerRuntimeResolveCallerRva},
      {"attachmentRigidSample0ChildRuntimeResolveCallerRva",
       summary.attachmentRigidSample0ChildRuntimeResolveCallerRva},
      {"attachmentRigidSample0RootRuntimeCreateModelDataPtr",
       summary.attachmentRigidSample0RootRuntimeCreateModelDataPtr},
      {"attachmentRigidSample0OwnerRuntimeCreateModelDataPtr",
       summary.attachmentRigidSample0OwnerRuntimeCreateModelDataPtr},
      {"attachmentRigidSample0RootRuntimeSourceObjectPtr",
       summary.attachmentRigidSample0RootRuntimeSourceObjectPtr},
      {"attachmentRigidSample0OwnerRuntimeSourceObjectPtr",
       summary.attachmentRigidSample0OwnerRuntimeSourceObjectPtr},
      {"attachmentRigidSample0RootRuntimeSourceSpriteObjectPtr",
       summary.attachmentRigidSample0RootRuntimeSourceSpriteObjectPtr},
      {"attachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr",
       summary.attachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr},
      {"attachmentRigidSample0ChildRuntimeParentRuntimeModelPtr",
       summary.attachmentRigidSample0ChildRuntimeParentRuntimeModelPtr},
      {"attachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame",
       summary.attachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame},
      {"attachmentRigidSample0ChildRuntimeParentLinkSourceMeta",
       summary.attachmentRigidSample0ChildRuntimeParentLinkSourceMeta},
      {"attachmentRigidSample0ChildRuntimeCreateModelDataPtr",
       summary.attachmentRigidSample0ChildRuntimeCreateModelDataPtr},
      {"attachmentRigidSample0ChildRuntimeSourceObjectPtr",
       summary.attachmentRigidSample0ChildRuntimeSourceObjectPtr},
      {"attachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr",
       summary.attachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr},
      {"attachmentRigidSample0ChildRuntimeModelResourcePtr",
       summary.attachmentRigidSample0ChildRuntimeModelResourcePtr},
      {"attachmentRigidSample0ChildRuntimeModelKey",
       summary.attachmentRigidSample0ChildRuntimeModelKey},
      {"attachmentRigidSample0ChildRuntimePoseMatrixCount",
       summary.attachmentRigidSample0ChildRuntimePoseMatrixCount},
      {"attachmentRigidSample0FirstSeenFrame",
       summary.attachmentRigidSample0FirstSeenFrame},
      {"attachmentRigidSample0LastSeenFrame",
       summary.attachmentRigidSample0LastSeenFrame},
      {"attachmentRigidSample1RootRuntimeModelPtr",
       summary.attachmentRigidSample1RootRuntimeModelPtr},
      {"attachmentRigidSample1OwnerRuntimeModelPtr",
       summary.attachmentRigidSample1OwnerRuntimeModelPtr},
      {"attachmentRigidSample1ChildRuntimeModelPtr",
       summary.attachmentRigidSample1ChildRuntimeModelPtr},
      {"attachmentRigidSample1ChildSpritePtr",
       summary.attachmentRigidSample1ChildSpritePtr},
      {"attachmentRigidSample1SourceObjectPtr",
       summary.attachmentRigidSample1SourceObjectPtr},
      {"attachmentRigidSample1RootRuntimeCreateHandlePtr",
       summary.attachmentRigidSample1RootRuntimeCreateHandlePtr},
      {"attachmentRigidSample1OwnerRuntimeCreateHandlePtr",
       summary.attachmentRigidSample1OwnerRuntimeCreateHandlePtr},
      {"attachmentRigidSample1ChildRuntimeCreateHandlePtr",
       summary.attachmentRigidSample1ChildRuntimeCreateHandlePtr},
      {"attachmentRigidSample1RootRuntimeCreateCallerRva",
       summary.attachmentRigidSample1RootRuntimeCreateCallerRva},
      {"attachmentRigidSample1OwnerRuntimeCreateCallerRva",
       summary.attachmentRigidSample1OwnerRuntimeCreateCallerRva},
      {"attachmentRigidSample1ChildRuntimeCreateCallerRva",
       summary.attachmentRigidSample1ChildRuntimeCreateCallerRva},
      {"attachmentRigidSample1RootRuntimeResolveCallerRva",
       summary.attachmentRigidSample1RootRuntimeResolveCallerRva},
      {"attachmentRigidSample1OwnerRuntimeResolveCallerRva",
       summary.attachmentRigidSample1OwnerRuntimeResolveCallerRva},
      {"attachmentRigidSample1ChildRuntimeResolveCallerRva",
       summary.attachmentRigidSample1ChildRuntimeResolveCallerRva},
      {"attachmentRigidSample1RootRuntimeCreateModelDataPtr",
       summary.attachmentRigidSample1RootRuntimeCreateModelDataPtr},
      {"attachmentRigidSample1OwnerRuntimeCreateModelDataPtr",
       summary.attachmentRigidSample1OwnerRuntimeCreateModelDataPtr},
      {"attachmentRigidSample1RootRuntimeSourceObjectPtr",
       summary.attachmentRigidSample1RootRuntimeSourceObjectPtr},
      {"attachmentRigidSample1OwnerRuntimeSourceObjectPtr",
       summary.attachmentRigidSample1OwnerRuntimeSourceObjectPtr},
      {"attachmentRigidSample1RootRuntimeSourceSpriteObjectPtr",
       summary.attachmentRigidSample1RootRuntimeSourceSpriteObjectPtr},
      {"attachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr",
       summary.attachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr},
      {"attachmentRigidSample1ChildRuntimeParentRuntimeModelPtr",
       summary.attachmentRigidSample1ChildRuntimeParentRuntimeModelPtr},
      {"attachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame",
       summary.attachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame},
      {"attachmentRigidSample1ChildRuntimeParentLinkSourceMeta",
       summary.attachmentRigidSample1ChildRuntimeParentLinkSourceMeta},
      {"attachmentRigidSample1ChildRuntimeCreateModelDataPtr",
       summary.attachmentRigidSample1ChildRuntimeCreateModelDataPtr},
      {"attachmentRigidSample1ChildRuntimeSourceObjectPtr",
       summary.attachmentRigidSample1ChildRuntimeSourceObjectPtr},
      {"attachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr",
       summary.attachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr},
      {"attachmentRigidSample1ChildRuntimeModelResourcePtr",
       summary.attachmentRigidSample1ChildRuntimeModelResourcePtr},
      {"attachmentRigidSample1ChildRuntimeModelKey",
       summary.attachmentRigidSample1ChildRuntimeModelKey},
      {"attachmentRigidSample1ChildRuntimePoseMatrixCount",
       summary.attachmentRigidSample1ChildRuntimePoseMatrixCount},
      {"attachmentRigidSample1FirstSeenFrame",
       summary.attachmentRigidSample1FirstSeenFrame},
      {"attachmentRigidSample1LastSeenFrame",
       summary.attachmentRigidSample1LastSeenFrame},
      {"contractAttachmentRigidSample0RootRuntimeModelPtr",
       summary.contractAttachmentRigidSample0RootRuntimeModelPtr},
      {"contractAttachmentRigidSample0OwnerRuntimeModelPtr",
       summary.contractAttachmentRigidSample0OwnerRuntimeModelPtr},
      {"contractAttachmentRigidSample0ChildRuntimeModelPtr",
       summary.contractAttachmentRigidSample0ChildRuntimeModelPtr},
      {"contractAttachmentRigidSample0ChildSpritePtr",
       summary.contractAttachmentRigidSample0ChildSpritePtr},
      {"contractAttachmentRigidSample0SourceObjectPtr",
       summary.contractAttachmentRigidSample0SourceObjectPtr},
      {"contractAttachmentRigidSample0WorldObjectEntryPtr",
       summary.contractAttachmentRigidSample0WorldObjectEntryPtr},
      {"contractAttachmentRigidSample0SceneNodePtr",
       summary.contractAttachmentRigidSample0SceneNodePtr},
      {"contractAttachmentRigidSample0RootRuntimeCreateHandlePtr",
       summary.contractAttachmentRigidSample0RootRuntimeCreateHandlePtr},
      {"contractAttachmentRigidSample0OwnerRuntimeCreateHandlePtr",
       summary.contractAttachmentRigidSample0OwnerRuntimeCreateHandlePtr},
      {"contractAttachmentRigidSample0ChildRuntimeCreateHandlePtr",
       summary.contractAttachmentRigidSample0ChildRuntimeCreateHandlePtr},
      {"contractAttachmentRigidSample0RootRuntimeCreateCallerRva",
       summary.contractAttachmentRigidSample0RootRuntimeCreateCallerRva},
      {"contractAttachmentRigidSample0OwnerRuntimeCreateCallerRva",
       summary.contractAttachmentRigidSample0OwnerRuntimeCreateCallerRva},
      {"contractAttachmentRigidSample0ChildRuntimeCreateCallerRva",
       summary.contractAttachmentRigidSample0ChildRuntimeCreateCallerRva},
      {"contractAttachmentRigidSample0RootRuntimeResolveCallerRva",
       summary.contractAttachmentRigidSample0RootRuntimeResolveCallerRva},
      {"contractAttachmentRigidSample0OwnerRuntimeResolveCallerRva",
       summary.contractAttachmentRigidSample0OwnerRuntimeResolveCallerRva},
      {"contractAttachmentRigidSample0ChildRuntimeResolveCallerRva",
       summary.contractAttachmentRigidSample0ChildRuntimeResolveCallerRva},
      {"contractAttachmentRigidSample0RootRuntimeCreateModelDataPtr",
       summary.contractAttachmentRigidSample0RootRuntimeCreateModelDataPtr},
      {"contractAttachmentRigidSample0OwnerRuntimeCreateModelDataPtr",
       summary.contractAttachmentRigidSample0OwnerRuntimeCreateModelDataPtr},
      {"contractAttachmentRigidSample0RootRuntimeWorldObjectEntryPtr",
       summary.contractAttachmentRigidSample0RootRuntimeWorldObjectEntryPtr},
      {"contractAttachmentRigidSample0RootRuntimeSceneNodePtr",
       summary.contractAttachmentRigidSample0RootRuntimeSceneNodePtr},
      {"contractAttachmentRigidSample0OwnerRuntimeWorldObjectEntryPtr",
       summary.contractAttachmentRigidSample0OwnerRuntimeWorldObjectEntryPtr},
      {"contractAttachmentRigidSample0OwnerRuntimeSceneNodePtr",
       summary.contractAttachmentRigidSample0OwnerRuntimeSceneNodePtr},
      {"contractAttachmentRigidSample0RootRuntimeSourceObjectPtr",
       summary.contractAttachmentRigidSample0RootRuntimeSourceObjectPtr},
      {"contractAttachmentRigidSample0OwnerRuntimeSourceObjectPtr",
       summary.contractAttachmentRigidSample0OwnerRuntimeSourceObjectPtr},
      {"contractAttachmentRigidSample0RootRuntimeSourceSpriteObjectPtr",
       summary.contractAttachmentRigidSample0RootRuntimeSourceSpriteObjectPtr},
      {"contractAttachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr",
       summary.contractAttachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr},
      {"contractAttachmentRigidSample0ChildRuntimeParentRuntimeModelPtr",
       summary.contractAttachmentRigidSample0ChildRuntimeParentRuntimeModelPtr},
      {"contractAttachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame",
       summary.contractAttachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame},
      {"contractAttachmentRigidSample0ChildRuntimeParentLinkSourceMeta",
       summary.contractAttachmentRigidSample0ChildRuntimeParentLinkSourceMeta},
      {"contractAttachmentRigidSample0ChildRuntimeCreateModelDataPtr",
       summary.contractAttachmentRigidSample0ChildRuntimeCreateModelDataPtr},
      {"contractAttachmentRigidSample0ChildRuntimeSourceObjectPtr",
       summary.contractAttachmentRigidSample0ChildRuntimeSourceObjectPtr},
      {"contractAttachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr",
       summary.contractAttachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr},
      {"contractAttachmentRigidSample0ChildRuntimeModelResourcePtr",
       summary.contractAttachmentRigidSample0ChildRuntimeModelResourcePtr},
      {"contractAttachmentRigidSample0ChildRuntimeModelKey",
       summary.contractAttachmentRigidSample0ChildRuntimeModelKey},
      {"contractAttachmentRigidSample0ChildRuntimePoseMatrixCount",
       summary.contractAttachmentRigidSample0ChildRuntimePoseMatrixCount},
      {"contractAttachmentRigidSample1RootRuntimeModelPtr",
       summary.contractAttachmentRigidSample1RootRuntimeModelPtr},
      {"contractAttachmentRigidSample1OwnerRuntimeModelPtr",
       summary.contractAttachmentRigidSample1OwnerRuntimeModelPtr},
      {"contractAttachmentRigidSample1ChildRuntimeModelPtr",
       summary.contractAttachmentRigidSample1ChildRuntimeModelPtr},
      {"contractAttachmentRigidSample1ChildSpritePtr",
       summary.contractAttachmentRigidSample1ChildSpritePtr},
      {"contractAttachmentRigidSample1SourceObjectPtr",
       summary.contractAttachmentRigidSample1SourceObjectPtr},
      {"contractAttachmentRigidSample1WorldObjectEntryPtr",
       summary.contractAttachmentRigidSample1WorldObjectEntryPtr},
      {"contractAttachmentRigidSample1SceneNodePtr",
       summary.contractAttachmentRigidSample1SceneNodePtr},
      {"contractAttachmentRigidSample1RootRuntimeCreateHandlePtr",
       summary.contractAttachmentRigidSample1RootRuntimeCreateHandlePtr},
      {"contractAttachmentRigidSample1OwnerRuntimeCreateHandlePtr",
       summary.contractAttachmentRigidSample1OwnerRuntimeCreateHandlePtr},
      {"contractAttachmentRigidSample1ChildRuntimeCreateHandlePtr",
       summary.contractAttachmentRigidSample1ChildRuntimeCreateHandlePtr},
      {"contractAttachmentRigidSample1RootRuntimeCreateCallerRva",
       summary.contractAttachmentRigidSample1RootRuntimeCreateCallerRva},
      {"contractAttachmentRigidSample1OwnerRuntimeCreateCallerRva",
       summary.contractAttachmentRigidSample1OwnerRuntimeCreateCallerRva},
      {"contractAttachmentRigidSample1ChildRuntimeCreateCallerRva",
       summary.contractAttachmentRigidSample1ChildRuntimeCreateCallerRva},
      {"contractAttachmentRigidSample1RootRuntimeResolveCallerRva",
       summary.contractAttachmentRigidSample1RootRuntimeResolveCallerRva},
      {"contractAttachmentRigidSample1OwnerRuntimeResolveCallerRva",
       summary.contractAttachmentRigidSample1OwnerRuntimeResolveCallerRva},
      {"contractAttachmentRigidSample1ChildRuntimeResolveCallerRva",
       summary.contractAttachmentRigidSample1ChildRuntimeResolveCallerRva},
      {"contractAttachmentRigidSample1RootRuntimeCreateModelDataPtr",
       summary.contractAttachmentRigidSample1RootRuntimeCreateModelDataPtr},
      {"contractAttachmentRigidSample1OwnerRuntimeCreateModelDataPtr",
       summary.contractAttachmentRigidSample1OwnerRuntimeCreateModelDataPtr},
      {"contractAttachmentRigidSample1RootRuntimeWorldObjectEntryPtr",
       summary.contractAttachmentRigidSample1RootRuntimeWorldObjectEntryPtr},
      {"contractAttachmentRigidSample1RootRuntimeSceneNodePtr",
       summary.contractAttachmentRigidSample1RootRuntimeSceneNodePtr},
      {"contractAttachmentRigidSample1OwnerRuntimeWorldObjectEntryPtr",
       summary.contractAttachmentRigidSample1OwnerRuntimeWorldObjectEntryPtr},
      {"contractAttachmentRigidSample1OwnerRuntimeSceneNodePtr",
       summary.contractAttachmentRigidSample1OwnerRuntimeSceneNodePtr},
      {"contractAttachmentRigidSample1RootRuntimeSourceObjectPtr",
       summary.contractAttachmentRigidSample1RootRuntimeSourceObjectPtr},
      {"contractAttachmentRigidSample1OwnerRuntimeSourceObjectPtr",
       summary.contractAttachmentRigidSample1OwnerRuntimeSourceObjectPtr},
      {"contractAttachmentRigidSample1RootRuntimeSourceSpriteObjectPtr",
       summary.contractAttachmentRigidSample1RootRuntimeSourceSpriteObjectPtr},
      {"contractAttachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr",
       summary.contractAttachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr},
      {"contractAttachmentRigidSample1ChildRuntimeParentRuntimeModelPtr",
       summary.contractAttachmentRigidSample1ChildRuntimeParentRuntimeModelPtr},
      {"contractAttachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame",
       summary.contractAttachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame},
      {"contractAttachmentRigidSample1ChildRuntimeParentLinkSourceMeta",
       summary.contractAttachmentRigidSample1ChildRuntimeParentLinkSourceMeta},
      {"contractAttachmentRigidSample1ChildRuntimeCreateModelDataPtr",
       summary.contractAttachmentRigidSample1ChildRuntimeCreateModelDataPtr},
      {"contractAttachmentRigidSample1ChildRuntimeSourceObjectPtr",
       summary.contractAttachmentRigidSample1ChildRuntimeSourceObjectPtr},
      {"contractAttachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr",
       summary.contractAttachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr},
      {"contractAttachmentRigidSample1ChildRuntimeModelResourcePtr",
       summary.contractAttachmentRigidSample1ChildRuntimeModelResourcePtr},
      {"contractAttachmentRigidSample1ChildRuntimeModelKey",
       summary.contractAttachmentRigidSample1ChildRuntimeModelKey},
      {"contractAttachmentRigidSample1ChildRuntimePoseMatrixCount",
       summary.contractAttachmentRigidSample1ChildRuntimePoseMatrixCount},
      {"lastAttachModelToPointParentSpritePtr",
       summary.lastAttachModelToPointParentSpritePtr},
      {"lastAttachModelToPointChildSpritePtr",
       summary.lastAttachModelToPointChildSpritePtr},
      {"lastAttachModelToPointChildRuntimeModelPtr",
       summary.lastAttachModelToPointChildRuntimeModelPtr},
      {"lastAttachModelToPointPromotedOwnerRuntimeModelPtr",
       summary.lastAttachModelToPointPromotedOwnerRuntimeModelPtr},
      {"lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr",
       summary.lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr},
      {"lastAttachModelToPointPromotedChildRuntimeModelPtr",
       summary.lastAttachModelToPointPromotedChildRuntimeModelPtr},
      {"lastAttachModelToPointPromotedChildModelResourcePtr",
       summary.lastAttachModelToPointPromotedChildModelResourcePtr},
      {"lastAttachModelToPointUnitPtr",
       summary.lastAttachModelToPointUnitPtr},
      {"lastAttachScopeParentSpritePtr",
       summary.lastAttachScopeParentSpritePtr},
      {"lastAttachScopeParentRuntimeModelPtr",
       summary.lastAttachScopeParentRuntimeModelPtr},
      {"lastAttachScopeChildSpritePtr",
       summary.lastAttachScopeChildSpritePtr},
      {"lastAttachScopeChildRuntimeModelPtr",
       summary.lastAttachScopeChildRuntimeModelPtr},
      {"lastAttachScopeHitRuntimeModelPtr",
       summary.lastAttachScopeHitRuntimeModelPtr},
      {"lastCurrentRenderIdentityWorldObjectEntryPtr",
       summary.lastCurrentRenderIdentityWorldObjectEntryPtr},
      {"lastCurrentRenderIdentitySceneNodePtr",
       summary.lastCurrentRenderIdentitySceneNodePtr},
      {"lastCurrentRenderIdentityUnitPtr",
       summary.lastCurrentRenderIdentityUnitPtr},
      {"lastRuntimeSourceObjectPtr", summary.lastRuntimeSourceObjectPtr},
      {"lastRuntimeSourceSpriteObjectPtr",
       summary.lastRuntimeSourceSpriteObjectPtr},
      {"lastRuntimeSourceRuntimeModelPtr",
       summary.lastRuntimeSourceRuntimeModelPtr},
      {"lastRuntimeModelResolveRuntimeModelPtr",
       summary.lastRuntimeModelResolveRuntimeModelPtr},
      {"lastRuntimeModelResolveHandlePtr",
       summary.lastRuntimeModelResolveHandlePtr},
      {"lastRuntimeModelCreateRuntimeModelPtr",
       summary.lastRuntimeModelCreateRuntimeModelPtr},
      {"lastRuntimeModelCreateModelDataPtr",
       summary.lastRuntimeModelCreateModelDataPtr},
      {"lastRuntimeModelInitRuntimeModelPtr",
       summary.lastRuntimeModelInitRuntimeModelPtr},
      {"lastRuntimeModelInitModelDataPtr",
       summary.lastRuntimeModelInitModelDataPtr},
      {"lastAttachmentRigidSourceObjectPtr",
       summary.lastAttachmentRigidSourceObjectPtr},
      {"lastAttachmentRigidSourceSpriteObjectPtr",
       summary.lastAttachmentRigidSourceSpriteObjectPtr},
      {"lastRuntimeChildLinkBuildParentRuntimeModelPtr",
       summary.lastRuntimeChildLinkBuildParentRuntimeModelPtr},
      {"lastRuntimeChildLinkBuildChildRuntimeModelPtr",
       summary.lastRuntimeChildLinkBuildChildRuntimeModelPtr},
      {"lastRuntimeChildLinkBuildModelDataPtr",
       summary.lastRuntimeChildLinkBuildModelDataPtr},
      {"lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr",
       summary.lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr},
      {"lastRuntimeChildBuildTimeDirectParentModelDataPtr",
       summary.lastRuntimeChildBuildTimeDirectParentModelDataPtr},
      {"lastRuntimeChildBuildTimeDirectRuntimeModelPtr",
       summary.lastRuntimeChildBuildTimeDirectRuntimeModelPtr},
      {"lastRuntimeChildBuildTimeDirectModelDataPtr",
       summary.lastRuntimeChildBuildTimeDirectModelDataPtr},
      {"lastRuntimeChildBuildTimeDirectModelResourcePtr",
       summary.lastRuntimeChildBuildTimeDirectModelResourcePtr},
      {"lastRuntimeChildBuildModelDataParentRuntimeModelPtr",
       summary.lastRuntimeChildBuildModelDataParentRuntimeModelPtr},
      {"lastRuntimeChildBuildModelDataPtr",
       summary.lastRuntimeChildBuildModelDataPtr},
      {"lastRuntimeChildBuildModelDataGroupRecordsPtr",
       summary.lastRuntimeChildBuildModelDataGroupRecordsPtr},
      {"lastRuntimeChildBuildModelDataHeadPtr",
       summary.lastRuntimeChildBuildModelDataHeadPtr},
      {"lastRuntimeChildBuildModelDataLinkNodePtr",
       summary.lastRuntimeChildBuildModelDataLinkNodePtr},
      {"lastRuntimeChildBuildModelDataChildModelDataPtr",
       summary.lastRuntimeChildBuildModelDataChildModelDataPtr},
      {"lastRuntimeChildBuildModelDataChildModelResourcePtr",
       summary.lastRuntimeChildBuildModelDataChildModelResourcePtr},
      {"lastRuntimeMatrixPublisherRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherMatchedRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherMatchedRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherMatrixCount",
       summary.lastRuntimeMatrixPublisherMatrixCount},
      {"lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount",
       summary.lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount},
      {"lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount",
       summary.lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount},
      {"lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr",
       summary.lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr},
      {"lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount",
       summary.lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount},
      {"lastAttachmentAncestorFromRuntimeModelPtr",
       summary.lastAttachmentAncestorFromRuntimeModelPtr},
      {"lastAttachmentAncestorRuntimeModelPtr",
       summary.lastAttachmentAncestorRuntimeModelPtr},
      {"overrideLastLocalPointSlotIndex",
       summary.overrideLastLocalPointSlotIndex},
      {"overrideLastLocalPointSourceRecordIndex",
       summary.overrideLastLocalPointSourceRecordIndex},
      {"overrideLastObservedChildLinkCount",
       summary.overrideLastObservedChildLinkCount},
      {"overrideLastMatchedChildLinkCount",
       summary.overrideLastMatchedChildLinkCount},
      {"overrideLastMatchedChildMatrixCount",
       summary.overrideLastMatchedChildMatrixCount},
      {"overrideLastMatchedChildBySourceRecordLinkCount",
       summary.overrideLastMatchedChildBySourceRecordLinkCount},
      {"overrideLastMatchedChildBySourceRecordMatrixCount",
       summary.overrideLastMatchedChildBySourceRecordMatrixCount},
      {"overrideLastContextRuntimeWithChildLinksOffset",
       summary.overrideLastContextRuntimeWithChildLinksOffset},
      {"overrideLastContextRuntimeWithChildLinksCount",
       summary.overrideLastContextRuntimeWithChildLinksCount},
      {"overrideLastContextRuntimeWithChildLinksMaxTag",
       summary.overrideLastContextRuntimeWithChildLinksMaxTag},
      {"overrideLastScratchRootRuntimeChildLinkCount",
       summary.overrideLastScratchRootRuntimeChildLinkCount},
      {"overrideLastScratchRootRuntimeMaxTag",
       summary.overrideLastScratchRootRuntimeMaxTag},
      {"overrideLastArgBlockRuntimeOffset",
       summary.overrideLastArgBlockRuntimeOffset},
      {"overrideLastArgBlockRuntimeChildLinkCount",
       summary.overrideLastArgBlockRuntimeChildLinkCount},
      {"overrideLastArgBlockRuntimeMaxTag",
       summary.overrideLastArgBlockRuntimeMaxTag},
      {"overrideLastArgBlockIdentityHintOffset",
       summary.overrideLastArgBlockIdentityHintOffset},
      {"overrideLastArg4BlockRuntimeOffset",
       summary.overrideLastArg4BlockRuntimeOffset},
      {"overrideLastArg4BlockRuntimeChildLinkCount",
       summary.overrideLastArg4BlockRuntimeChildLinkCount},
      {"overrideLastArg4BlockRuntimeMaxTag",
       summary.overrideLastArg4BlockRuntimeMaxTag},
      {"overrideLastArg4BlockIdentityHintOffset",
       summary.overrideLastArg4BlockIdentityHintOffset},
      {"overrideLastChildSourceMetaPtr",
       summary.overrideLastChildSourceMetaPtr},
      {"overrideLastChildSourceMetaRuntimeModelPtr",
       summary.overrideLastChildSourceMetaRuntimeModelPtr},
      {"overrideLastRootRuntimeChildLinkCount",
       summary.overrideLastRootRuntimeChildLinkCount},
      {"overrideLastRootRuntimeMaxTag",
       summary.overrideLastRootRuntimeMaxTag},
      {"lastSpriteHostJHandle", summary.lastSpriteHostJHandle},
      {"lastSpriteHostRawcode", summary.lastSpriteHostRawcode},
      {"lastSpriteFrameSourceJHandle", summary.lastSpriteFrameSourceJHandle},
      {"lastSpriteFrameSourceRawcode", summary.lastSpriteFrameSourceRawcode},
      {"lastSpriteFrameSourceObjectRuntimeFieldOffset",
       summary.lastSpriteFrameSourceObjectRuntimeFieldOffset},
      {"lastSpriteFrameSourceObjectRegistryFieldOffset",
       summary.lastSpriteFrameSourceObjectRegistryFieldOffset},
      {"lastSpriteFrameSourceDeepIdentityOffset",
       summary.lastSpriteFrameSourceDeepIdentityOffset},
      {"lastSpriteFrameAttachmentRoleMask",
       summary.lastSpriteFrameAttachmentRoleMask},
      {"lastSpriteFrameAttachmentUpdateKind",
       summary.lastSpriteFrameAttachmentUpdateKind},
      {"lastSpriteFrameAttachmentCallerRva",
       summary.lastSpriteFrameAttachmentCallerRva},
      {"lastSourceObjectIdentityHintOffset",
       summary.lastSourceObjectIdentityHintOffset},
      {"lastAttachedEffectInitJHandle",
       summary.lastAttachedEffectInitJHandle},
      {"lastAttachedEffectInitRawcode",
       summary.lastAttachedEffectInitRawcode},
      {"lastAttachedEffectDirectJHandle",
       summary.lastAttachedEffectDirectJHandle},
      {"lastAttachedEffectDirectRawcode",
       summary.lastAttachedEffectDirectRawcode},
      {"lastAttachModelToPointJHandle",
       summary.lastAttachModelToPointJHandle},
      {"lastAttachModelToPointRawcode",
       summary.lastAttachModelToPointRawcode},
      {"lastAttachModelToPointAttachPointIndex",
       summary.lastAttachModelToPointAttachPointIndex},
      {"lastAttachScopeCallerRva",
       summary.lastAttachScopeCallerRva},
      {"lastAttachScopeHitRoleMask",
       summary.lastAttachScopeHitRoleMask},
      {"lastAttachedEffectInitParentRuntimeModelPtr",
       summary.lastAttachedEffectInitParentRuntimeModelPtr},
      {"lastRuntimeModelCtorRuntimeModelPtr",
       summary.lastRuntimeModelCtorRuntimeModelPtr},
      {"lastRuntimeModelCtorCallerRva",
       summary.lastRuntimeModelCtorCallerRva},
      {"lastRuntimeModelCtorKind",
       summary.lastRuntimeModelCtorKind},
      {"lastRuntimeModelResolveCallerRva",
       summary.lastRuntimeModelResolveCallerRva},
      {"lastRuntimeModelCreateCallerRva",
       summary.lastRuntimeModelCreateCallerRva},
      {"lastRuntimeModelInitCallerRva",
       summary.lastRuntimeModelInitCallerRva},
      {"lastRuntimeChildLinkBuildSourceMeta",
       summary.lastRuntimeChildLinkBuildSourceMeta},
      {"lastRuntimeChildBuildModelDataPhase",
       summary.lastRuntimeChildBuildModelDataPhase},
      {"lastRuntimeChildBuildModelDataGroupCount",
       summary.lastRuntimeChildBuildModelDataGroupCount},
      {"lastRuntimeChildBuildModelDataLinkCount",
       summary.lastRuntimeChildBuildModelDataLinkCount},
      {"lastRuntimeChildBuildModelDataUnreadableLinkCount",
       summary.lastRuntimeChildBuildModelDataUnreadableLinkCount},
      {"lastRuntimeChildBuildModelDataSourceMeta",
       summary.lastRuntimeChildBuildModelDataSourceMeta},
      {"lastRuntimeMatrixPublisherKind",
       summary.lastRuntimeMatrixPublisherKind},
      {"lastRuntimeMatrixPublisherRoleMask",
       summary.lastRuntimeMatrixPublisherRoleMask},
      {"lastAttachmentAncestorDepth",
       summary.lastAttachmentAncestorDepth},
      {"overrideLastLocalPointX", summary.overrideLastLocalPointX},
      {"overrideLastLocalPointY", summary.overrideLastLocalPointY},
      {"overrideLastLocalPointZ", summary.overrideLastLocalPointZ},
      {"semanticCoreSkippedNoIdentity",
       summary.semanticCoreSkippedNoIdentity},
      {"semanticCoreSkippedNoResolvedGeoset",
       summary.semanticCoreSkippedNoResolvedGeoset},
      {"semanticCoreSkippedNoGeoset", summary.semanticCoreSkippedNoGeoset},
      {"semanticCoreSkippedResourceMiss",
       summary.semanticCoreSkippedResourceMiss},
      {"semanticCoreSkippedResourceNotReady",
       summary.semanticCoreSkippedResourceNotReady},
      {"semanticCoreSkippedNoPose", summary.semanticCoreSkippedNoPose},
      {"semanticCoreSkippedNoPoseNoContext",
       summary.semanticCoreSkippedNoPoseNoContext},
      {"semanticCoreSkippedNoPoseAnonymousSubpart",
       summary.semanticCoreSkippedNoPoseAnonymousSubpart},
      {"semanticCoreSkippedNoPoseLookupMiss",
       summary.semanticCoreSkippedNoPoseLookupMiss},
      {"semanticCoreSkippedNoRuntimeGroupPalette",
       summary.semanticCoreSkippedNoRuntimeGroupPalette},
      {"semanticCoreBuildDurationUs", summary.semanticCoreBuildDurationUs},
      {"semanticCoreBuildInProgress", summary.semanticCoreBuildInProgress},
      {"semanticCoreBuildRequestPending",
       summary.semanticCoreBuildRequestPending},
      {"semanticCoreBuildFrameSerial", summary.semanticCoreBuildFrameSerial},
      {"semanticCoreBuildPublishRevision",
       summary.semanticCoreBuildPublishRevision},
      {"semanticCorePendingFrameSerial", summary.semanticCorePendingFrameSerial},
      {"semanticCorePendingPublishRevision",
       summary.semanticCorePendingPublishRevision},
      {"semanticCoreBuildCurrentRecordIndex",
       summary.semanticCoreBuildCurrentRecordIndex},
      {"semanticCoreBuildRecordCount", summary.semanticCoreBuildRecordCount},
      {"semanticCoreBuildChunkCount", summary.semanticCoreBuildChunkCount},
      {"semanticCoreStalePendingBuildClearedCount",
       summary.semanticCoreStalePendingBuildClearedCount},
      {"poseFrame", summary.poseFrame},
      {"nativeD3D9BackendHasDevice", summary.nativeD3D9BackendHasDevice},
      {"runtimePoseHooksActive", summary.runtimePoseHooksActive},
      {"runtimeChainWarm", summary.runtimeChainWarm},
      {"runtimeChainNeedsRepair", summary.runtimeChainNeedsRepair},
  };
}

bool IsReadySnapshot(const War3RuntimeStatusSnapshot& snapshot) {
  return snapshot.module.state == "Running" && snapshot.runtime.jassReady &&
         snapshot.runtime.runtimeReady && snapshot.runtime.gameStarted &&
         snapshot.render.inGameRenderReady;
}

bool MeetsHotFrameRequirements(
    const json& payload, const War3RuntimeStatusSnapshot& snapshot,
    const War3FrameManifestSummary& manifest,
    const render::ShadowRuntimeBridgeSummary& shadow,
    uint64_t readyFrameBaseline) {
  const uint64_t minFrameIndex =
      payload.value("minFrameIndex", uint64_t(0));
  const bool requireFrameAdvance =
      payload.value("requireFrameAdvance", false);
  const uint64_t minFrameAdvance =
      (std::max)(uint64_t(1), payload.value("minFrameAdvance", uint64_t(1)));
  const bool requireSemanticFrameFresh =
      payload.value("requireSemanticFrameFresh", false);
  const bool requireSemanticSceneConsumed =
      payload.value("requireSemanticSceneConsumed", false);
  const uint64_t minVisibleCount =
      payload.value("minVisibleCount", uint64_t(0));
  const uint64_t minStableIdentityCount =
      payload.value("minStableIdentityCount", uint64_t(0));
  const uint64_t minResolvedGeosetCount =
      payload.value("minResolvedGeosetCount", uint64_t(0));
  const uint64_t minUnitCount =
      payload.value("minUnitCount", uint64_t(0));
  const uint64_t minRuntimeModelCount =
      payload.value("minRuntimeModelCount", uint64_t(0));
  const uint64_t minModelResourceCount =
      payload.value("minModelResourceCount", uint64_t(0));
  const uint64_t minSemanticResolved =
      payload.value("minSemanticResolved", uint64_t(0));
  const uint64_t minSemanticSkinnedResolved =
      payload.value("minSemanticSkinnedResolved", uint64_t(0));
  const uint64_t minExplicitBlendAttempts =
      payload.value("minExplicitBlendAttempts", uint64_t(0));
  const uint64_t minExplicitBlendResolved =
      payload.value("minExplicitBlendResolved", uint64_t(0));
  const uint64_t minNativeExecutedDrawCount =
      payload.value("minNativeExecutedDrawCount", uint64_t(0));

  if (snapshot.frameIndex < minFrameIndex)
    return false;
  const bool semanticProgressCanReplaceFrameAdvance =
      payload.value("requestSemanticFrameBuild", false) &&
      shadow.semanticCoreFrameFresh &&
      shadow.semanticCoreResolved >= minSemanticResolved &&
      shadow.semanticCoreSkinnedResolved >= minSemanticSkinnedResolved;
  if (requireFrameAdvance &&
      snapshot.frameIndex < (readyFrameBaseline + minFrameAdvance) &&
      !semanticProgressCanReplaceFrameAdvance) {
    return false;
  }
  if (requireSemanticFrameFresh && !shadow.semanticCoreFrameFresh)
    return false;
  const bool semanticSceneRevisionConsumed =
      shadow.semanticSceneLastSubmittedDrawCount != 0u &&
      shadow.semanticScenePublishRevisionLag == 0u;
  const bool semanticSceneSameFrameConsumed =
      shadow.semanticSceneLastSubmittedDrawCount != 0u &&
      shadow.semanticSceneLastFrameSerial != 0u &&
      shadow.semanticCoreFrameSerial != 0u &&
      shadow.semanticSceneLastFrameSerial >= shadow.semanticCoreFrameSerial;
  if (requireSemanticSceneConsumed && !semanticSceneRevisionConsumed &&
      !semanticSceneSameFrameConsumed) {
    return false;
  }
  if (manifest.visibleCount < minVisibleCount)
    return false;
  if (manifest.recordsWithStableIdentity < minStableIdentityCount)
    return false;
  if (manifest.recordsWithResolvedGeoset < minResolvedGeosetCount)
    return false;
  if (manifest.unitCount < minUnitCount)
    return false;
  if (shadow.shadowRuntimeModelCount < minRuntimeModelCount)
    return false;
  if (shadow.shadowModelResourceCount < minModelResourceCount)
    return false;
  if (shadow.semanticCoreResolved < minSemanticResolved)
    return false;
  if (shadow.semanticCoreSkinnedResolved < minSemanticSkinnedResolved)
    return false;
  if (shadow.semanticCoreExplicitBlendAttempts < minExplicitBlendAttempts)
    return false;
  if (shadow.semanticCoreExplicitBlendResolved < minExplicitBlendResolved)
    return false;
  if (shadow.nativeD3D9BackendExecutedDrawCount < minNativeExecutedDrawCount)
    return false;
  return true;
}

bool WantsSemanticFrameBuild(const json& payload, bool defaultValue = false) {
  return payload.value("requestSemanticFrameBuild", defaultValue) ||
         payload.value("refreshSemanticFrameIfStale", false);
}

bool IsHotSemanticBuildWaitPayload(const json& payload) {
  return payload.value("requestSemanticFrameBuild", false) &&
         (payload.value("requireFrameAdvance", false) ||
          payload.value("requireSemanticFrameFresh", false) ||
          payload.value("requireSemanticSceneConsumed", false) ||
          payload.value("minSemanticResolved", uint64_t(0)) > 0u ||
          payload.value("minSemanticSkinnedResolved", uint64_t(0)) > 0u ||
          payload.value("minNativeExecutedDrawCount", uint64_t(0)) > 0u);
}

bool AllowsControlPlaneSemanticDrain(const json& payload) {
  return payload.value("allowControlPlaneSemanticDrain", false) ||
         IsHotSemanticBuildWaitPayload(payload);
}

bool ShouldRequestSemanticBuild(
    const War3RuntimeStatusSnapshot& snapshot,
    const render::ShadowRuntimeBridgeSummary& shadow, const json& payload,
    uint64_t readyFrameBaseline = 0u) {
  if (!WantsSemanticFrameBuild(payload))
    return false;

  // 安全优先：只要当前还没进入 in-game world render，就不要从 control-plane
  // 继续推动 semantic build。之前的卡死问题就是在“首帧假热/非 in-game”状态下，
  // probe 线程反复请求新 build，把宿主拖到 100% CPU。
  const bool allowPreInGameBuild =
      payload.value("allowPreInGameSemanticBuild", false);
  if (!snapshot.render.isInGame && !allowPreInGameBuild)
    return false;

  if (snapshot.module.state != "Running" || !snapshot.runtime.jassReady ||
      !snapshot.runtime.runtimeReady) {
    return false;
  }

  if (!snapshot.render.inGameRenderReady || !snapshot.runtime.gameStarted)
    return false;

  if (snapshot.frameIndex == 0u && snapshot.frame.frameNumber == 0u)
    return false;

  if (shadow.semanticCoreFrameFresh)
    return false;
  if (shadow.semanticCoreBuildInProgress || shadow.semanticCoreBuildRequestPending)
    return false;

  const bool requireFrameAdvance =
      payload.value("requireFrameAdvance", false) ||
      payload.value("requireSemanticFrameFresh", false);
  if (requireFrameAdvance && snapshot.frameIndex <= readyFrameBaseline)
    return false;

  return true;
}

bool TryRequestSemanticBuild(
    const War3RuntimeStatusSnapshot& snapshot,
    const render::ShadowRuntimeBridgeSummary& shadow, const json& payload,
    uint64_t readyFrameBaseline, std::string& outReason) {
  if (!ShouldRequestSemanticBuild(snapshot, shadow, payload,
                                  readyFrameBaseline)) {
    outReason = "blocked_by_safety_gate";
    return false;
  }

  const bool forceRequest =
      payload.value("forceSemanticFrameBuild", false) ||
      AllowsControlPlaneSemanticDrain(payload);
  const uint64_t minIntervalMs =
      forceRequest
          ? uint64_t(0)
          : (std::max)(uint64_t(250),
                       payload.value("semanticBuildMinIntervalMs",
                                     uint64_t(750)));
  const uint64_t nowMs = GetEpochMs();
  std::lock_guard<std::mutex> lock(g_semanticBuildRequestMutex);
  if (!forceRequest && g_lastSemanticBuildRequestMs != 0u &&
      nowMs < g_lastSemanticBuildRequestMs + minIntervalMs) {
    outReason = "throttled_by_interval";
    return false;
  }
  if (!forceRequest && snapshot.frameIndex != 0u &&
      g_lastSemanticBuildRequestFrameIndex != 0u &&
      snapshot.frameIndex <= g_lastSemanticBuildRequestFrameIndex) {
    outReason = "throttled_same_frame";
    return false;
  }

  g_lastSemanticBuildRequestMs = nowMs;
  g_lastSemanticBuildRequestFrameIndex = snapshot.frameIndex;
  outReason = "requested";
  return true;
}

bool CanDrainSemanticBuildFromControlPlane(
    const War3RuntimeStatusSnapshot& snapshot, const json& payload) {
  if (!AllowsControlPlaneSemanticDrain(payload))
    return false;
  if (!WantsSemanticFrameBuild(payload))
    return false;

  const bool allowPreInGameBuild =
      payload.value("allowPreInGameSemanticBuild", false);
  if (!snapshot.render.isInGame && !allowPreInGameBuild)
    return false;
  if (snapshot.module.state != "Running" || !snapshot.runtime.jassReady ||
      !snapshot.runtime.runtimeReady || !snapshot.runtime.gameStarted ||
      !snapshot.render.inGameRenderReady) {
    return false;
  }
  if (snapshot.frameIndex == 0u && snapshot.frame.frameNumber == 0u)
    return false;
  return true;
}

render::ShadowRuntimeBridgeSummary DrainSemanticBuildFromControlPlaneIfAllowed(
    const War3RuntimeStatusSnapshot& snapshot,
    render::ShadowRuntimeBridgeSummary summary, const json& payload,
    std::string& inOutReason, bool ensureRequest) {
  if (!CanDrainSemanticBuildFromControlPlane(snapshot, payload))
    return summary;

  auto& validationRuntime = shadow::ShadowValidationRuntime::instance();
  if (ensureRequest)
    validationRuntime.requestLatestFrameBuild();

  const auto before = validationRuntime.buildStateSnapshot();
  if (!before.buildInProgress && !before.buildRequestPending)
    return summary;

  const uint32_t maxChunks = (std::max)(
      uint32_t(1), payload.value("semanticBuildDrainMaxChunks", uint32_t(32)));
  const uint64_t maxBudgetUs = (std::max)(
      uint64_t(1000),
      payload.value("semanticBuildDrainBudgetUs", uint64_t(50000)));
  const uint64_t recordCeiling =
      payload.value("semanticBuildDrainRecordCeiling", uint64_t(1024));
  validationRuntime.drainPendingBuildForControlPlane(maxChunks, maxBudgetUs,
                                                     recordCeiling);

  const auto after = validationRuntime.buildStateSnapshot();
  const bool madeProgress =
      before.buildInProgress != after.buildInProgress ||
      before.buildRequestPending != after.buildRequestPending ||
      before.buildCurrentRecordIndex != after.buildCurrentRecordIndex ||
      before.buildChunkCount != after.buildChunkCount ||
      before.pendingPublishRevision != after.pendingPublishRevision ||
      before.buildPublishRevision != after.buildPublishRevision;
  if (madeProgress) {
    inOutReason = after.buildInProgress || after.buildRequestPending
                      ? "drained_semantic_build_partial"
                      : "drained_semantic_build_complete";
  }
  return QueryShadowRuntimeSummary(false);
}

render::ShadowRuntimeBridgeSummary RefreshShadowRuntimeSummaryIfSafe(
    const War3RuntimeStatusSnapshot& snapshot,
    render::ShadowRuntimeBridgeSummary summary, const json& payload,
    uint64_t readyFrameBaseline, bool& outRequested, std::string& outReason) {
  outRequested = false;
  outReason.clear();
  const bool wantsRefresh =
      payload.value("refreshSemanticFrameIfStale", false) ||
      payload.value("requestSemanticFrameBuild", false);

  // 当 semanticCore 已经挂着 pending contract，但 render 线程没有继续推进时，
  // 不能再把 get_shadow_runtime_summary 直接挡在 safety gate 外面；
  // 否则 control-plane 永远只能看到“差最后一帧”的旧 build。
  // 这里单独返回“observe pending build”路径：
  // - 不把它记成新的 build request
  // - 不再调用 QueryShadowRuntimeSummary(true)，避免 pipe 线程在 pending
  //   状态下同步跑语义 refresh / summary 重路径导致 3s 响应超时。
  if (wantsRefresh && summary.semanticCoreBuildRequestPending &&
      !summary.semanticCoreBuildInProgress) {
    outReason = "observe_pending_build";
    summary = DrainSemanticBuildFromControlPlaneIfAllowed(
        snapshot, summary, payload, outReason, false);
    return summary;
  }
  if (wantsRefresh && summary.semanticCoreBuildInProgress) {
    outReason = "observe_in_progress_build";
    summary = DrainSemanticBuildFromControlPlaneIfAllowed(
        snapshot, summary, payload, outReason, false);
    return summary;
  }

  if (!TryRequestSemanticBuild(snapshot, summary, payload, readyFrameBaseline,
                               outReason)) {
    return summary;
  }

  outRequested = true;
  summary = QueryShadowRuntimeSummary(true);
  summary = DrainSemanticBuildFromControlPlaneIfAllowed(
      snapshot, summary, payload, outReason, true);
  return summary;
}

json HandleCommand(const json& request) {
  const std::string command = request.value("command", std::string());
  const std::string requestId = request.value("requestId", std::string());
  const json payload = request.value("payload", json::object());

  json response = {
      {"timestampMs", GetEpochMs()},
      {"requestId", requestId},
      {"command", command},
      {"ok", false},
      {"error", ""},
      {"result", json::object()},
  };

  if (command == "ping") {
    response["ok"] = true;
    response["result"] = {
        {"pid", static_cast<uint32_t>(GetCurrentProcessId())},
        {"pipeName", EscapeBackslashes(BuildPipeName())},
        {"protocolVersion", 1},
    };
    return response;
  }

  if (command == "get_runtime_status") {
    response["ok"] = true;
    response["result"] =
        ToJson(QueryRuntimeStatusSnapshot("control-plane",
                                          payload.value("frameIndex", uint64_t(0))));
    return response;
  }

  if (command == "wait_until") {
    const double timeoutSec =
        payload.value("timeoutSec", 120.0);
    const uint32_t pollIntervalMs =
        (std::max)(uint32_t(10), payload.value("pollIntervalMs", 50u));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              static_cast<int64_t>(timeoutSec * 1000.0));

    War3RuntimeStatusSnapshot snapshot = {};
    War3FrameManifestSummary manifestSummary = {};
    render::ShadowRuntimeBridgeSummary shadowSummary = {};
    bool hasHotFrameSummary = false;
    bool readySeen = false;
    uint64_t readyFrameBaseline = 0u;
    uint64_t lastAdvancedFrameIndex = 0u;
    auto lastAdvancedFrameAt = std::chrono::steady_clock::now();
    uint64_t lastSemanticBuildFrameSerial = 0u;
    uint64_t lastSemanticBuildPublishRevision = 0u;
    uint64_t lastSemanticBuildRecordIndex = 0u;
    uint64_t lastSemanticBuildChunkCount = 0u;
    auto lastSemanticBuildProgressAt = std::chrono::steady_clock::now();
    const uint64_t stalledFrameTimeoutMs =
        (std::max)(uint64_t(1000),
                   payload.value("stalledFrameTimeoutMs", uint64_t(3000)));
    const bool requiresHotFrameChecks =
        payload.value("requireFrameAdvance", false) ||
        payload.value("requireSemanticFrameFresh", false) ||
        payload.value("requireSemanticSceneConsumed", false) ||
        payload.value("minFrameIndex", uint64_t(0)) > 0u ||
        payload.value("minVisibleCount", uint64_t(0)) > 0u ||
        payload.value("minStableIdentityCount", uint64_t(0)) > 0u ||
        payload.value("minResolvedGeosetCount", uint64_t(0)) > 0u ||
        payload.value("minUnitCount", uint64_t(0)) > 0u ||
        payload.value("minRuntimeModelCount", uint64_t(0)) > 0u ||
        payload.value("minModelResourceCount", uint64_t(0)) > 0u ||
        payload.value("minSemanticResolved", uint64_t(0)) > 0u ||
        payload.value("minSemanticSkinnedResolved", uint64_t(0)) > 0u ||
        payload.value("minExplicitBlendAttempts", uint64_t(0)) > 0u ||
        payload.value("minExplicitBlendResolved", uint64_t(0)) > 0u ||
        payload.value("minNativeExecutedDrawCount", uint64_t(0)) > 0u;
    do {
      const auto loopNow = std::chrono::steady_clock::now();
      snapshot = QueryRuntimeStatusSnapshot("control-plane/wait_until");
      if (snapshot.frameIndex > lastAdvancedFrameIndex) {
        lastAdvancedFrameIndex = snapshot.frameIndex;
        lastAdvancedFrameAt = loopNow;
      }
      if (IsReadySnapshot(snapshot)) {
        if (!readySeen) {
          readySeen = true;
          readyFrameBaseline = snapshot.frameIndex;
          lastAdvancedFrameIndex = snapshot.frameIndex;
          lastAdvancedFrameAt = loopNow;
        }
        if (!requiresHotFrameChecks) {
          response["ok"] = true;
          response["result"] = {
              {"ready", true},
              {"runtimeStatus", ToJson(snapshot)},
          };
          return response;
        }

        manifestSummary = QueryFrameManifestSummary();
        shadowSummary = QueryShadowRuntimeSummary(false);
        bool requestedSemanticBuild = false;
        std::string requestReason;
        shadowSummary = RefreshShadowRuntimeSummaryIfSafe(
            snapshot, shadowSummary, payload, readyFrameBaseline,
            requestedSemanticBuild, requestReason);
        hasHotFrameSummary = true;
        if (shadowSummary.semanticCoreBuildInProgress ||
            shadowSummary.semanticCoreBuildRequestPending) {
          const bool semanticBuildProgressed =
              shadowSummary.semanticCoreBuildFrameSerial !=
                  lastSemanticBuildFrameSerial ||
              shadowSummary.semanticCoreBuildPublishRevision !=
                  lastSemanticBuildPublishRevision ||
              shadowSummary.semanticCoreBuildCurrentRecordIndex !=
                  lastSemanticBuildRecordIndex ||
              shadowSummary.semanticCoreBuildChunkCount !=
                  lastSemanticBuildChunkCount;
          if (semanticBuildProgressed) {
            lastSemanticBuildFrameSerial =
                shadowSummary.semanticCoreBuildFrameSerial;
            lastSemanticBuildPublishRevision =
                shadowSummary.semanticCoreBuildPublishRevision;
            lastSemanticBuildRecordIndex =
                shadowSummary.semanticCoreBuildCurrentRecordIndex;
            lastSemanticBuildChunkCount =
                shadowSummary.semanticCoreBuildChunkCount;
            lastSemanticBuildProgressAt = loopNow;
          }
        }
        const bool frameStalled =
            uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                         loopNow - lastAdvancedFrameAt)
                         .count()) >= stalledFrameTimeoutMs;
        const bool semanticBuildStalled =
            shadowSummary.semanticCoreBuildInProgress ||
            shadowSummary.semanticCoreBuildRequestPending;
        const bool semanticBuildRecentlyProgressed =
            semanticBuildStalled &&
            uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                         loopNow - lastSemanticBuildProgressAt)
                         .count()) < stalledFrameTimeoutMs;
        if (readySeen && frameStalled &&
            (!snapshot.render.isInGame || !semanticBuildStalled ||
             !semanticBuildRecentlyProgressed)) {
          const bool requireSemanticSceneConsumed =
              payload.value("requireSemanticSceneConsumed", false);
          const bool semanticSceneRevisionConsumed =
              shadowSummary.semanticSceneLastSubmittedDrawCount != 0u &&
              shadowSummary.semanticScenePublishRevisionLag == 0u;
          const bool semanticSceneSameFrameConsumed =
              shadowSummary.semanticSceneLastSubmittedDrawCount != 0u &&
              shadowSummary.semanticSceneLastFrameSerial != 0u &&
              shadowSummary.semanticCoreFrameSerial != 0u &&
              shadowSummary.semanticSceneLastFrameSerial >=
                  shadowSummary.semanticCoreFrameSerial;
          const bool semanticSceneWaitingForRenderPass =
              shadowSummary.semanticCoreSubmittedDrawCount > 0u &&
              !semanticSceneRevisionConsumed && !semanticSceneSameFrameConsumed;
          response["ok"] = false;
          response["error"] = "wait_until stalled";
          response["result"] = {
              {"ready", false},
              {"runtimeStatus", ToJson(snapshot)},
              {"frameManifestSummary", ToJson(manifestSummary)},
              {"shadowRuntimeSummary", ToJson(shadowSummary)},
              {"readyFrameBaseline", readyFrameBaseline},
              {"stalledFrameTimeoutMs", stalledFrameTimeoutMs},
              {"frameStalled", frameStalled},
              {"semanticBuildStalled", semanticBuildStalled},
              {"requestedSemanticFrameBuild", requestedSemanticBuild},
              {"semanticBuildRequestReason", requestReason},
              {"semanticBuildRecentlyProgressed", semanticBuildRecentlyProgressed},
              {"requireSemanticSceneConsumed", requireSemanticSceneConsumed},
              {"semanticSceneWaitingForRenderPass",
               semanticSceneWaitingForRenderPass},
          };
          return response;
        }
        if (MeetsHotFrameRequirements(payload, snapshot, manifestSummary,
                                      shadowSummary, readyFrameBaseline)) {
          response["ok"] = true;
          response["result"] = {
              {"ready", true},
              {"runtimeStatus", ToJson(snapshot)},
              {"frameManifestSummary", ToJson(manifestSummary)},
              {"shadowRuntimeSummary", ToJson(shadowSummary)},
              {"readyFrameBaseline", readyFrameBaseline},
              {"requestedSemanticFrameBuild", requestedSemanticBuild},
              {"semanticBuildRequestReason", requestReason},
          };
          return response;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    } while (!g_stopRequested.load(std::memory_order_relaxed) &&
             std::chrono::steady_clock::now() < deadline);

    response["ok"] = false;
    response["error"] = "wait_until timeout";
    response["result"] = {
        {"ready", false},
        {"runtimeStatus", ToJson(snapshot)},
        {"readyFrameBaseline", readyFrameBaseline},
    };
    if (hasHotFrameSummary) {
      response["result"]["frameManifestSummary"] = ToJson(manifestSummary);
      response["result"]["shadowRuntimeSummary"] = ToJson(shadowSummary);
    }
    return response;
  }

  if (command == "get_shadow_runtime_summary") {
    const auto runtime = QueryRuntimeStatusSnapshot("control-plane");
    auto summary = QueryShadowRuntimeSummary(false);
    bool requestedSemanticBuild = false;
    std::string requestReason;
    summary = RefreshShadowRuntimeSummaryIfSafe(
        runtime, summary, payload, 0u, requestedSemanticBuild, requestReason);
    response["ok"] = true;
    response["result"] = ToJson(summary);
    response["result"]["requestedSemanticFrameBuild"] = requestedSemanticBuild;
    response["result"]["semanticBuildRequestReason"] = requestReason;
    return response;
  }

  if (command == "set_shadow_evidence_collector") {
    const bool attached = payload.value("attached", false);
    SetShadowEvidenceCollectorAttached(attached);
    response["ok"] = true;
    response["result"] = {
        {"attached", IsShadowEvidenceCollectorAttached()},
        {"retentionRevision", QueryShadowEvidenceRetentionRevision()},
    };
    return response;
  }

  if (command == "start_shadow_pose_full_trace") {
    const uint32_t maxSeconds =
        (std::max)(uint32_t(1), payload.value("maxSeconds", 15u));
    const bool includeMatrixBytes =
        payload.value("includeMatrixBytes", false);
    const uint32_t maxPoseRecords =
        payload.value("maxPoseRecords", 0u);
    const uint32_t maxShadowObjectRecords =
        payload.value("maxShadowObjectRecords", 0u);
    const uint32_t maxCurrentDrawRecords =
        payload.value("maxCurrentDrawRecords", 0u);
    render::StartShadowPoseFullTrace(maxSeconds, includeMatrixBytes,
                                     maxPoseRecords, maxShadowObjectRecords,
                                     maxCurrentDrawRecords);
    response["ok"] = true;
    response["result"] = ToJson(render::QueryShadowPoseFullTraceStatus());
    return response;
  }

  if (command == "stop_shadow_pose_full_trace") {
    render::StopShadowPoseFullTrace();
    response["ok"] = true;
    response["result"] = ToJson(render::QueryShadowPoseFullTraceStatus());
    return response;
  }

  if (command == "get_shadow_pose_full_trace_status") {
    response["ok"] = true;
    response["result"] = ToJson(render::QueryShadowPoseFullTraceStatus());
    return response;
  }

  if (command == "get_frame_manifest_summary") {
    response["ok"] = true;
    response["result"] = ToJson(QueryFrameManifestSummary());
    return response;
  }

  if (command == "get_hot_shadow_probe") {
    const auto runtime = QueryRuntimeStatusSnapshot("control-plane");
    const auto manifest = QueryFrameManifestSummary();
    auto shadow = QueryShadowRuntimeSummary(false);
    bool requestedSemanticFrameBuild = false;
    std::string requestReason;
    shadow = RefreshShadowRuntimeSummaryIfSafe(
        runtime, shadow, payload, 0u, requestedSemanticFrameBuild,
        requestReason);
    response["ok"] = true;
    response["result"] = json{
        {"runtimeStatus", ToJson(runtime)},
        {"frameManifestSummary", ToJson(manifest)},
        {"shadowRuntimeSummary", ToJson(shadow)},
        {"requestedSemanticFrameBuild", requestedSemanticFrameBuild},
        {"semanticBuildRequestReason", requestReason},
    };
    return response;
  }

  if (command == "capture_final_frame") {
    War3FrameCaptureResult capture = {};
    const std::string outputPath = payload.value("outputPath", std::string());
    const uint32_t timeoutMs =
        (std::max)(uint32_t(500), payload.value("timeoutMs", 8000u));
    const bool ok = SubmitFrameCaptureRequest(requestId, outputPath, timeoutMs,
                                              &capture);
    response["ok"] = ok;
    response["error"] = capture.error;
    response["result"] = {
        {"requestId", capture.requestId},
        {"outputPath", capture.outputPath},
        {"width", capture.width},
        {"height", capture.height},
        {"shadowFrameSerial", capture.shadowFrameSerial},
        {"terrainS1CaptureAttemptCount",
         capture.terrainS1CaptureAttemptCount},
        {"terrainS1CaptureAcceptedCount",
         capture.terrainS1CaptureAcceptedCount},
        {"terrainS1WorldIdentityLikeCount",
         capture.terrainS1WorldIdentityLikeCount},
        {"terrainS1WorldNonIdentityCount",
         capture.terrainS1WorldNonIdentityCount},
        {"terrainS1WorldNonFiniteCount",
         capture.terrainS1WorldNonFiniteCount},
        {"terrainS1ForceIdentityWorldCount",
         capture.terrainS1ForceIdentityWorldCount},
        {"terrainS1WorldMatrixHash", capture.terrainS1WorldMatrixHash},
        {"terrainS1WorldTranslationMilliMax",
         capture.terrainS1WorldTranslationMilliMax},
        {"shadowCasterStageHistogram",
         capture.shadowCasterStageHistogram},
        {"shadowCasterCategoryHistogram",
         capture.shadowCasterCategoryHistogram},
        {"stage13CaptureAttemptCount", capture.stage13CaptureAttemptCount},
        {"stage13CaptureRejectedNoDemandCount",
         capture.stage13CaptureRejectedNoDemandCount},
        {"stage13CaptureRejectedAfterBeforeUiCount",
         capture.stage13CaptureRejectedAfterBeforeUiCount},
        {"stage13CaptureConsideredCount",
         capture.stage13CaptureConsideredCount},
        {"beforeUiStage13BoundaryCandidateCount",
         capture.beforeUiStage13BoundaryCandidateCount},
        {"beforeUiStage13BoundaryCommitCount",
         capture.beforeUiStage13BoundaryCommitCount},
        {"stage13RetentionBaseEligibleCount",
         capture.stage13RetentionBaseEligibleCount},
        {"stage13SourcePositionInvalidCount",
         capture.stage13SourcePositionInvalidCount},
        {"stage13SourceIndexInvalidCount",
         capture.stage13SourceIndexInvalidCount},
        {"stage13SourceIdentityValidCount",
         capture.stage13SourceIdentityValidCount},
        {"stage13SourceIdentityHitCount",
         capture.stage13SourceIdentityHitCount},
        {"stage13SourceIdentityMissCount",
         capture.stage13SourceIdentityMissCount},
        {"stage13StrongScanCount", capture.stage13StrongScanCount},
        {"stage13SnapshotBuildCount", capture.stage13SnapshotBuildCount},
        {"stage13SnapshotContentRekeyCount",
         capture.stage13SnapshotContentRekeyCount},
        {"stage13FreezeCopyBytes", capture.stage13FreezeCopyBytes},
        {"stage13CpuSnapshotCopyBytes",
         capture.stage13CpuSnapshotCopyBytes},
        {"stage13RetentionSnapshotBytes",
         capture.stage13RetentionSnapshotBytes},
        {"stage13RetainedEntryCountMax",
         capture.stage13RetainedEntryCountMax},
        {"stage13RetainedContentMatchCount",
         capture.stage13RetainedContentMatchCount},
        {"stage13RetainedIdentityMatchCount",
         capture.stage13RetainedIdentityMatchCount},
        {"stage13RetainedWorldMatchCount",
         capture.stage13RetainedWorldMatchCount},
        {"stage13RetainedMaterialMatchCount",
         capture.stage13RetainedMaterialMatchCount},
        {"stage13RetainedLayoutMatchCount",
         capture.stage13RetainedLayoutMatchCount},
        {"stage13RetainedAllSemanticMatchCount",
         capture.stage13RetainedAllSemanticMatchCount},
        {"shadowCasterCount", capture.shadowCasterCount},
        {"shadowReplayDrawCount", capture.shadowReplayDrawCount},
        {"shadowMapDrawnCasterCount", capture.shadowMapDrawnCasterCount},
        {"shadowReceiverActiveStrengthMilli",
         capture.shadowReceiverActiveStrengthMilli},
        {"shadowReceiverUboStrengthMilli",
         capture.shadowReceiverUboStrengthMilli},
        {"shadowReceiverNeedPass", capture.shadowReceiverNeedPass},
        {"shadowMapExecutedThisFrame",
         capture.shadowMapExecutedThisFrame},
        {"shadowVisibilityExecutedThisFrame",
         capture.shadowVisibilityExecutedThisFrame},
        {"shadowReceiverDrawExecutedThisFrame",
         capture.shadowReceiverDrawExecutedThisFrame},
        {"shadowTaaMode", capture.shadowTaaMode},
        {"shadowHistoryValidBefore", capture.shadowHistoryValidBefore},
        {"shadowHistoryValidAfter", capture.shadowHistoryValidAfter},
        {"shadowHistoryReadIndex", capture.shadowHistoryReadIndex},
        {"shadowHistoryWriteIndex", capture.shadowHistoryWriteIndex},
        {"shadowHistoryAdvancedThisFrame",
         capture.shadowHistoryAdvancedThisFrame},
        {"shadowHistoryAdvanceSkippedIncomplete",
         capture.shadowHistoryAdvanceSkippedIncomplete},
        {"shadowReceiverSampleSource", capture.shadowReceiverSampleSource},
        {"shadowMatrixSceneKey", capture.shadowMatrixSceneKey},
        {"receiverCameraHash", capture.receiverCameraHash},
        {"receiverSunDirectionHash", capture.receiverSunDirectionHash},
        {"receiverCsmHash", capture.receiverCsmHash},
        {"receiverCameraDeltaNano", capture.receiverCameraDeltaNano},
        {"receiverSunDeltaNano", capture.receiverSunDeltaNano},
        {"receiverCsmDeltaNano", capture.receiverCsmDeltaNano},
        {"receiverSnappedCenterDeltaTexelsNano",
         capture.receiverSnappedCenterDeltaTexelsNano},
        {"receiverTexelSizeDeltaNano",
         capture.receiverTexelSizeDeltaNano},
        {"shadowHistoryInvalidationMask",
         capture.shadowHistoryInvalidationMask},
        {"replayBackingHash", capture.replayBackingHash},
        {"stage13ReplayContentHash", capture.stage13ReplayContentHash},
        {"stage13ReplayBackingHash", capture.stage13ReplayBackingHash},
        {"stage13ReplayDrawCount", capture.stage13ReplayDrawCount},
        {"shadowMapRenderSerial", capture.shadowMapRenderSerial},
        {"handled", capture.handled},
        {"ok", capture.ok},
    };
    return response;
  }

  if (command == "invoke_test_command") {
    War3InternalTestRequest testRequest = {};
    testRequest.requestId = requestId;
    testRequest.command = payload.value("command", std::string());
    testRequest.payloadJson =
        payload.value("payload", json::object()).dump();
    War3InternalTestResult testResult = {};
    const uint32_t timeoutMs =
        (std::max)(uint32_t(100), payload.value("timeoutMs", 6000u));
    const bool ok = SubmitInternalTestRequest(testRequest, timeoutMs,
                                              &testResult);
    response["ok"] = ok;
    response["error"] = testResult.error;
    if (!testResult.resultJson.empty()) {
      try {
        response["result"] = json::parse(testResult.resultJson);
      } catch (...) {
        response["result"] = {{"raw", testResult.resultJson}};
      }
    } else {
      response["result"] = json::object();
    }
    response["result"]["handled"] = testResult.handled;
    response["result"]["command"] = testResult.command;
    return response;
  }

  if (command == "shutdown_session") {
    War3InternalTestRequest testRequest = {};
    testRequest.requestId = requestId;
    testRequest.command = "shutdown.session";
    testRequest.payloadJson = json::object().dump();
    War3InternalTestResult testResult = {};
    const uint32_t timeoutMs =
        (std::max)(uint32_t(100), payload.value("timeoutMs", 1500u));
    const bool ok = SubmitInternalTestRequest(testRequest, timeoutMs,
                                              &testResult);
    response["ok"] = ok;
    response["error"] = testResult.error;
    if (!testResult.resultJson.empty()) {
      try {
        response["result"] = json::parse(testResult.resultJson);
      } catch (...) {
        response["result"] = {{"raw", testResult.resultJson}};
      }
    } else {
      response["result"] = json::object();
    }
    response["result"]["handled"] = testResult.handled;
    response["result"]["command"] = testResult.command;
    return response;
  }

  response["ok"] = false;
  response["error"] = "unsupported command";
  return response;
}

void WriteAll(HANDLE pipe, const std::string& bytes) {
  const char* data = bytes.data();
  DWORD remaining = static_cast<DWORD>(bytes.size());
  while (remaining != 0u) {
    DWORD written = 0u;
    if (!WriteFile(pipe, data, remaining, &written, nullptr))
      break;
    data += written;
    remaining -= written;
  }
}

std::string ReadAll(HANDLE pipe) {
  std::string bytes;
  char buffer[4096];
  while (true) {
    DWORD readBytes = 0u;
    const BOOL ok = ReadFile(pipe, buffer, DWORD(sizeof(buffer)), &readBytes,
                             nullptr);
    if (ok) {
      bytes.append(buffer, buffer + readBytes);
      break;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_MORE_DATA) {
      bytes.append(buffer, buffer + readBytes);
      continue;
    }
    break;
  }
  return bytes;
}

void HandlePipeClient(HANDLE pipe) {
  g_activeClientCount.fetch_add(1u, std::memory_order_relaxed);

  json response = {
      {"timestampMs", GetEpochMs()},
      {"ok", false},
      {"error", "invalid request"},
      {"result", json::object()},
  };

  try {
    const std::string requestBytes = ReadAll(pipe);
    response = HandleCommand(json::parse(requestBytes));
  } catch (const std::exception& e) {
    response["error"] = std::string("request parse failed: ") + e.what();
  }

  WriteAll(pipe, response.dump());
  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);

  g_activeClientCount.fetch_sub(1u, std::memory_order_relaxed);
}

void ServerLoop() {
  const std::string pipeName = BuildPipeName();
  g_running.store(true, std::memory_order_relaxed);
  war3dbg::Print("DXVK War3Control: pipe=%s online\n", pipeName.c_str());

  while (!g_stopRequested.load(std::memory_order_relaxed)) {
    HANDLE pipe = CreateNamedPipeA(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        64 * 1024,
        64 * 1024,
        0,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      war3dbg::Print("DXVK War3Control: CreateNamedPipe failed err=%lu\n",
                     GetLastError());
      break;
    }

    const BOOL connected =
        ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }

    std::thread(HandlePipeClient, pipe).detach();
  }

  g_running.store(false, std::memory_order_relaxed);
  war3dbg::Print("DXVK War3Control: offline\n");
}

} // namespace

void InitializeWar3ControlPlane() {
  if (g_running.load(std::memory_order_relaxed) || g_serverThread.joinable())
    return;

  g_stopRequested.store(false, std::memory_order_relaxed);
  g_serverThread = std::thread(ServerLoop);
}

void ShutdownWar3ControlPlane() {
  g_stopRequested.store(true, std::memory_order_relaxed);

  const std::string pipeName = BuildPipeName();
  HANDLE wake = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  if (wake != INVALID_HANDLE_VALUE)
    CloseHandle(wake);

  if (g_serverThread.joinable())
    g_serverThread.join();
}

void ResetWar3ControlPlaneState() {
  ResetInternalTestApiState();
  std::lock_guard<std::mutex> lock(g_semanticBuildRequestMutex);
  g_lastSemanticBuildRequestMs = 0u;
  g_lastSemanticBuildRequestFrameIndex = 0u;
}

bool IsWar3ControlPlaneRunning() {
  return g_running.load(std::memory_order_relaxed);
}

std::string GetWar3ControlPlanePipeName() {
  return BuildPipeName();
}

War3FrameManifestSummary QueryFrameManifestSummary() {
  return BuildManifestSummary();
}

render::ShadowRuntimeBridgeSummary QueryShadowRuntimeSummary(
    bool refreshSemanticFrameIfStale) {
  // 默认只返回最新已发布的 semantic 快照，让调用方通过 freshness/lag
  // 字段判断是否过期。不要在 control-plane 线程里同步追热帧：
  // `ensureFrameBuiltForContract(...)` 在 scene-submission 模式下会走完整
  // semantic build，隔离桌面卡住/首帧假热时容易把 War3 主机拖到高 CPU，
  // 甚至导致 hot probe 与 summary 请求整体超时。
  return render::QueryShadowRuntimeBridgeSummary(refreshSemanticFrameIfStale);
}

} // namespace dxvk::war3::tools
