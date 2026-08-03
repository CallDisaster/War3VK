#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dxvk::war3::tools {

struct War3RuntimeStatusModuleSnapshot {
  uint32_t registered = 0;
  uint32_t loaded = 0;
  uint64_t dispatchCalls = 0;
  uint64_t handlers = 0;
  uint64_t callbackErrors = 0;
  std::string state;
};

struct War3RuntimeStatusPerfSnapshot {
  bool enabled = false;
  bool recording = false;
};

struct War3RuntimeStatusProfileSnapshot {
  std::string name;
  std::string disabledModules;
  std::string enabledModules;
};

struct GpuFlightFrame {
  uint64_t timestampMs = 0u;
  uint64_t frameSerial = 0u;
  std::string lastRenderStage;
  uint32_t csmRequestedResolution = 0u;
  uint32_t csmEffectiveResolution = 0u;
  uint32_t csmFallbackReason = 0u;
  uint32_t csmFallbackLatched = 0u;
  uint64_t csmGeneration = 0u;
  uint64_t csmMemoryBudgetBytes = 0u;
  uint64_t csmMemoryAvailableBytes = 0u;
  uint32_t taaRequestedMode = 0u;
  uint32_t taaEffectiveMode = 0u;
  uint32_t taaShaderMode = 0u;
  uint32_t taaHistoryValid = 0u;
  uint64_t taaHistoryGeneration = 0u;
  uint64_t arenaUsedBytes = 0u;
  uint64_t arenaResidentBytes = 0u;
  uint64_t arenaGeneration = 0u;
  uint64_t arenaBusyReuseRejectCount = 0u;
  uint64_t arenaOverflowCount = 0u;
  uint64_t arenaReservedBytes = 0u;
  uint64_t arenaCommittedBytes = 0u;
  uint64_t arenaRolledBackBytes = 0u;
  uint64_t arenaAdmissionRejectedCount = 0u;
  uint64_t arenaPartialTransactionCount = 0u;
  uint64_t arenaUniqueSourceBytes = 0u;
  uint64_t arenaDuplicateBytesSaved = 0u;
  uint64_t arenaExactIndexTrimAcceptedCount = 0u;
  uint64_t arenaExactIndexTrimRejectedCount = 0u;
  uint64_t arenaExactIndexTrimBytesSaved = 0u;
  uint32_t arenaFrameIncomplete = 0u;
  uint64_t queueSubmittedSerial = 0u;
  uint64_t queueCompletedSerial = 0u;
  int64_t queueResult = 0;
};

struct GpuIncidentSnapshot {
  uint64_t timestampMs = 0u;
  std::string reason;
  int64_t queueResult = 0;
  uint64_t stalledMilliseconds = 0u;
  std::vector<GpuFlightFrame> recentFrames;
};

struct War3RuntimeStatusRuntimeSnapshot {
  bool runtimeReady = false;
  bool jassReady = false;
  bool gameStarted = false;
};

struct War3RuntimeStatusRenderSnapshot {
  bool inGameRenderReady = false;
  bool isInGame = false;
  bool isLoading = false;
  uint64_t worldPtr = 0;
};

struct War3RuntimeStatusFrameSnapshot {
  uint64_t frameNumber = 0;
  uint64_t publishRevision = 0;
  uint64_t visibleCount = 0;
  uint64_t mainQueueCount = 0;
  uint64_t transparentCount = 0;
  uint64_t recordsWithStableIdentity = 0;
  uint64_t recordsWithResolvedGeoset = 0;
  uint64_t recordsWithRuntimeModel = 0;
  uint64_t recordsWithModelResource = 0;
  uint64_t unitCount = 0;
  uint64_t buildingCount = 0;
  uint64_t destructibleCount = 0;
  uint64_t unitWithResolvedGeoset = 0;
  uint64_t buildingWithResolvedGeoset = 0;
  uint64_t destructibleWithResolvedGeoset = 0;
  uint64_t unitWithMeshData = 0;
  uint64_t buildingWithMeshData = 0;
  uint64_t destructibleWithMeshData = 0;
  uint64_t unitWithModelResource = 0;
  uint64_t buildingWithModelResource = 0;
  uint64_t destructibleWithModelResource = 0;
  uint64_t sampleUnitSceneNode = 0;
  uint64_t sampleUnitWorldObjectEntry = 0;
  uint64_t sampleUnitUnitPtr = 0;
  uint64_t sampleUnitMeshData = 0;
  uint64_t sampleUnitRuntimeModel = 0;
  uint64_t sampleUnitModelResource = 0;
  uint64_t sampleUnitPoseCtx = 0;
  uint64_t sampleUnitPoseCtxRuntimeCandidate = 0;
  uint64_t sampleUnitSceneNodeRuntimeCandidate = 0;
  uint64_t sampleUnitWorldObjectEntryRuntimeCandidate = 0;
  uint32_t sampleUnitJHandle = 0;
  uint32_t sampleUnitRawcode = 0;
  uint32_t sampleUnitMeshIndex = 0xFFFFFFFFu;
  uint32_t sampleUnitGeosetIndex = 0xFFFFFFFFu;
  uint32_t sampleUnitPoseCtxRuntimeOffset = 0xFFFFFFFFu;
  uint32_t sampleUnitSceneNodeRuntimeOffset = 0xFFFFFFFFu;
  uint32_t sampleUnitWorldObjectEntryRuntimeOffset = 0xFFFFFFFFu;
  uint32_t sampleUnitGeosetVertexCount = 0;
  uint32_t sampleUnitGeosetPrimitiveCount = 0;
  uint32_t sampleUnitGeosetMatrixGroupCount = 0;
  uint32_t sampleUnitGeosetMatrixIndexCount = 0;
  bool sampleUnitMeshIndexReadable = false;
  bool sampleUnitMeshDataLooksLikeGeosetData = false;
  uint64_t itemCount = 0;
  uint64_t effectCount = 0;
  uint64_t unknownCount = 0;
  uint64_t visibleMainQueueRangeCallCount = 0;
  uint64_t visibleMainQueueRangeRecordCount = 0;
  uint64_t visibleSemanticCandidateCallCount = 0;
  uint64_t visibleSemanticCandidateMergedCount = 0;
  uint64_t visibleSemanticCandidateAppendedCount = 0;
  uint64_t visibleTransparentEntryCallCount = 0;
};

struct War3RuntimeStatusShadowSnapshot {
  uint64_t matrixPaletteCount = 0;
  uint64_t shadowReadyGeosetCount = 0;
  uint64_t shadowModelResourceCount = 0;
  uint64_t shadowRuntimeModelCount = 0;
  uint64_t upperLayerResolveAuthoritativeRigid = 0;
  uint64_t upperLayerResolveAuthoritativeSkinned = 0;
  uint64_t upperLayerResolvedAuthoritativeItems = 0;
  uint64_t upperLayerEmitted = 0;
  uint64_t semanticCoreFrameSerial = 0;
  uint64_t semanticCoreResolved = 0;
  uint64_t semanticCoreSkinnedResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidWorldTransformResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidNoMatrixPalette = 0;
  uint64_t semanticCoreSubmittedDrawCount = 0;
  uint64_t semanticCoreSkippedNoRuntimeGroupPalette = 0;
  uint64_t fallbackDrawCount = 0;
  uint64_t fallbackDrawCountTerrain = 0;
  uint64_t fallbackDrawCountWorldObject = 0;
  uint64_t fallbackDrawCountUnitObject = 0;
  uint64_t objectFallbackDrawCount = 0;
  uint64_t semanticSceneSubmitted = 0;
  uint64_t semanticSceneSubmittedUnit = 0;
  uint64_t semanticSceneSubmittedSkinned = 0;
  uint64_t semanticSceneSubmittedSkinnedNonUnitResolvedCount = 0;
  uint64_t semanticSceneSubmittedSkinnedUnknownPacketKindCount = 0;
  uint64_t semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount = 0;
  uint64_t semanticSceneSubmittedSkinnedGroupNonZeroCount = 0;
  uint64_t semanticSceneSubmittedSkinnedTransparentQueueCount = 0;
  uint64_t semanticSceneSubmittedSkinnedMissingUnitPtrCount = 0;
  uint64_t semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount = 0;
  uint64_t semanticSceneSubmittedBuilding = 0;
  uint64_t semanticSceneSubmittedDestructible = 0;
  uint64_t semanticSceneSubmittedCutout = 0;
  uint64_t semanticSceneSubmittedAlphaBlend = 0;
  uint64_t semanticSceneMaterialObservedCutoutCount = 0;
  uint64_t semanticSceneMaterialObservedAlphaBlendCount = 0;
  uint64_t semanticSceneRejectedCutoutSkinnedContract = 0;
  uint64_t semanticSceneRejectedAlphaBlendSkinnedContract = 0;
  uint64_t semanticSceneRejectedCutoutGeometry = 0;
  uint64_t semanticSceneRejectedAlphaBlendGeometry = 0;
  uint64_t semanticSceneRejectedCutoutVisualPolicy = 0;
  uint64_t semanticSceneRejectedAlphaBlendVisualPolicy = 0;
  uint64_t semanticSceneMaterialLayerContractResolvedCount = 0;
  uint64_t semanticSceneMaterialLayerContractFailedCount = 0;
  uint64_t semanticSceneMaterialBlendMode0Count = 0;
  uint64_t semanticSceneMaterialBlendMode1Count = 0;
  uint64_t semanticSceneMaterialBlendMode2PlusCount = 0;
  uint64_t semanticSceneDirectCurrentDrawLayerIndexNonZeroCount = 0;
  uint64_t semanticSceneMaterialLastMeshIndex = 0;
  uint64_t semanticSceneMaterialLastLayerIndex = 0;
  uint64_t semanticSceneMaterialLastLayerCount = 0;
  uint64_t semanticSceneMaterialLastBlendOrDrawMode = 0;
  uint64_t semanticSceneSubmittedOwnedGroupSlots = 0;
  uint64_t semanticSceneSubmittedExplicitBlendContract = 0;
  uint64_t semanticSceneSubmittedSingleMatrixGroupSkinning = 0;
  uint64_t semanticSceneSubmittedMultiGroupSlotSkinning = 0;
  uint64_t semanticSceneSkinnedMinUniqueGroupSlots = 0;
  uint64_t semanticSceneSkinnedMaxUniqueGroupSlots = 0;
  uint64_t semanticSceneSkinnedGroupSlotsUnique1Count = 0;
  uint64_t semanticSceneSkinnedGroupSlotsUnique2To4Count = 0;
  uint64_t semanticSceneSkinnedGroupSlotsUnique5To8Count = 0;
  uint64_t semanticSceneSkinnedGroupSlotsUnique9To16Count = 0;
  uint64_t semanticSceneSkinnedGroupSlotsUnique17PlusCount = 0;
  uint64_t semanticSceneExplicitBlendUnavailableCurrentDraw = 0;
  uint64_t semanticSceneCurrentDrawContractKnownCount = 0;
  uint64_t semanticSceneCurrentDrawPaletteReadyCount = 0;
  uint64_t semanticSceneCurrentDrawGroupSlotReadyCount = 0;
  uint64_t semanticSceneCurrentDrawResolveReadyCount = 0;
  uint64_t semanticSceneCurrentDrawMissNoContract = 0;
  uint64_t semanticSceneCurrentDrawMissNoPalette = 0;
  uint64_t semanticSceneCurrentDrawMissNoGroupSlots = 0;
  uint64_t semanticSceneCurrentDrawMissStaleVisibleFrame = 0;
  uint64_t semanticSceneCurrentDrawResolveReadyRejectedCount = 0;
  uint64_t semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount = 0;
  uint64_t semanticScenePaletteOverrideNoComposeCount = 0;
  uint64_t semanticScenePaletteOverrideWouldComposeCount = 0;
  uint64_t semanticScenePalettePacketWorldComposeCount = 0;
  uint64_t semanticSceneCanonicalReadyCount = 0;
  uint64_t semanticSceneCanonicalReadyCutoutCount = 0;
  uint64_t semanticSceneCanonicalReadyAlphaBlendCount = 0;
  uint64_t semanticSceneCanonicalRejectNoStableIdentity = 0;
  uint64_t semanticSceneCanonicalRejectNoMesh = 0;
  uint64_t semanticSceneCanonicalRejectNoWorldTransform = 0;
  uint64_t semanticSceneCanonicalRejectNoPalette = 0;
  uint64_t semanticSceneCanonicalRejectNoSlotContract = 0;
  uint64_t semanticSceneCanonicalRejectStaleProducer = 0;
  uint64_t semanticSceneCanonicalRejectInvalidVertexIndex = 0;
  uint64_t semanticSceneCanonicalRejectExplicitBlendIncomplete = 0;
  uint64_t semanticSceneCanonicalRejectAfterReadyCount = 0;
  uint64_t currentDrawContractPublishAttemptCount = 0;
  uint64_t currentDrawContractPublishReadyCount = 0;
  uint64_t currentDrawContractPublishSkippedNonWorldContext = 0;
  uint64_t currentDrawContractPublishSkippedSmallViewport = 0;
  uint64_t currentDrawContractQueryAttemptCount = 0;
  uint64_t currentDrawContractQueryHitCount = 0;
  uint64_t currentDrawCapturedPaletteQueryAttemptCount = 0;
  uint64_t currentDrawCapturedPaletteQueryHitCount = 0;
  uint64_t currentDrawGroupSlotDecodeAttemptCount = 0;
  uint64_t currentDrawGroupSlotDecodeHitCount = 0;
  uint64_t currentDrawGroupSlotDecodeMissDisabledStream = 0;
  uint64_t currentDrawPreparedSliceProbeAttemptCount = 0;
  uint64_t currentDrawPreparedSliceProbeContextReadyCount = 0;
  uint64_t currentDrawPreparedSliceProbeBackingReadableCount = 0;
  uint64_t currentDrawPreparedSliceRecordedCount = 0;
  uint64_t currentDrawPreparedSliceQueryAttemptCount = 0;
  uint64_t currentDrawPreparedSliceQueryHitCount = 0;
  uint64_t currentDrawPreparedSliceQueryMissCount = 0;
  uint64_t currentDrawStream1PublishNoStreamCount = 0;
  uint64_t currentDrawStream1PublishStride0Count = 0;
  uint64_t currentDrawStream1PublishStride1Count = 0;
  uint64_t currentDrawStream1PublishStride8Count = 0;
  uint64_t currentDrawStream1PublishStride12Count = 0;
  uint64_t currentDrawStream1PublishStride16Count = 0;
  uint64_t currentDrawStream1PublishStride20Count = 0;
  uint64_t currentDrawStream1PublishStrideOtherCount = 0;
  uint64_t currentDrawStream1PublishLastRawStride = 0;
  uint64_t currentDrawStream1PublishMaxRawStride = 0;
  uint64_t currentDrawLastVisibleFrameSerial = 0;
  uint64_t currentDrawLastRenderFrameIndex = 0;
  uint64_t currentDrawLastSmallViewportWidth = 0;
  uint64_t currentDrawLastSmallViewportHeight = 0;
  uint64_t currentDrawLastMissReason = 0;
  uint64_t submitPaletteContentAgeLag0Count = 0;
  uint64_t submitPaletteContentAgeLag1Count = 0;
  uint64_t submitPaletteContentAgeLag2Count = 0;
  uint64_t submitPaletteContentAgeLag3To5Count = 0;
  uint64_t submitPaletteContentAgeLag6PlusCount = 0;
  uint64_t submitPaletteContentAgeMax = 0;
  uint64_t submitPaletteContentAgeSampleCount = 0;
  uint64_t submitPaletteContentAgeUnknownCount = 0;
  uint64_t semanticScenePopulateLastReturnReason = 0;
  uint64_t semanticScenePopulateLastProducerPublishAttemptDelta = 0;
  uint64_t semanticScenePopulateLastProducerPublishReadyDelta = 0;
  uint64_t semanticScenePopulateLastProducerQueryAttemptDelta = 0;
  uint64_t semanticScenePopulateLastProducerQueryHitDelta = 0;
  uint64_t semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta = 0;
  uint64_t semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta = 0;
  uint64_t semanticScenePopulateLastProducerGroupDecodeAttemptDelta = 0;
  uint64_t semanticScenePopulateLastProducerGroupDecodeHitDelta = 0;
  uint64_t semanticSceneSubmittedFrameLocal = 0;
  uint64_t semanticSceneSubmittedPersistent = 0;
  uint64_t semanticSceneStatsPublishCount = 0;
  uint64_t semanticSceneLastFrameSerial = 0;
  uint64_t semanticSceneLastSelectedFrameSerial = 0;
  uint64_t semanticSceneLastReusableFrameSerial = 0;
  uint64_t semanticSceneLastSourcePublishRevision = 0;
  uint64_t semanticSceneLastTargetPublishRevision = 0;
  uint64_t semanticSceneLastInputDrawCount = 0;
  uint64_t semanticSceneLastSubmittedDrawCount = 0;
  uint64_t semanticSceneSelectedFrameEligibleZeroCount = 0;
  uint64_t semanticSceneReusableFrameForcedCount = 0;
  uint64_t semanticSceneReusableFrameUnavailableCount = 0;
  uint64_t semanticSceneReusableFrameRejectedNativeValidationCount = 0;
  uint64_t semanticScenePublishRevisionLag = 0;
  uint64_t semanticFallbackPruned = 0;
  bool semanticCoreFrameFresh = false;
  bool semanticCoreBuildInProgress = false;
  bool semanticCoreBuildRequestPending = false;
  uint64_t semanticCoreBuildCurrentRecordIndex = 0;
  uint64_t semanticCoreBuildRecordCount = 0;
  uint64_t semanticCoreBuildChunkCount = 0;
  uint64_t semanticStaticCandidateCount = 0;
  uint64_t semanticStaticCandidateBuildingCount = 0;
  uint64_t semanticStaticCandidateDestructibleCount = 0;
  uint64_t semanticStaticCandidateMaybeDoodadOrEffectCount = 0;
  uint64_t semanticStaticCandidateWithStableIdentity = 0;
  uint64_t semanticStaticCandidateWithMeshData = 0;
  uint64_t semanticStaticCandidateWithRuntimeModel = 0;
  uint64_t semanticStaticCandidateWithModelResource = 0;
  uint64_t semanticStaticCandidateWithResolvedGeoset = 0;
  uint64_t semanticStaticCandidateRejectedUnitsOnlyFilter = 0;
  uint64_t semanticStaticCandidateRejectedNoIdentity = 0;
  uint64_t semanticStaticCandidateRejectedNoMeshData = 0;
  uint64_t semanticStaticCandidateRejectedNoResource = 0;
  uint64_t semanticStaticCandidateRejectedNoGeoset = 0;
  uint64_t semanticStaticCandidateRejectedNonCanonicalKind = 0;
  // Phase 7.2: flicker diagnostics + reconciliation
  uint64_t semanticSceneDirectLastRawRecordCount = 0;
  uint64_t semanticSceneDirectLastEligibleRecordCount = 0;
  uint64_t semanticSceneCompactWorkTableMode = 0;
  uint64_t semanticSceneCompactWorkTableCandidateCount = 0;
  uint64_t semanticSceneCompactWorkTableSealedCount = 0;
  uint64_t semanticSceneCompactWorkTableConsumedCount = 0;
  uint64_t semanticSceneCompactWorkTableFallbackCount = 0;
  uint64_t semanticSceneCompactWorkTableRejectStageCount = 0;
  uint64_t semanticSceneCompactWorkTableRejectFreshnessCount = 0;
  uint64_t semanticSceneCompactWorkTableRejectPolicyCount = 0;
  uint64_t semanticSceneCompactWorkTableRejectFrameCount = 0;
  uint64_t semanticSceneCompactWorkTableRejectIdentityCount = 0;
  uint64_t semanticSceneCompactWorkTableMismatchCount = 0;
  uint64_t semanticSceneDirectLastSubmittedRecordCount = 0;
  uint64_t semanticSceneDirectLastUniqueObjectCount = 0;
  uint64_t semanticSceneDirectLastSubmittedObjectCount = 0;
  uint64_t semanticSceneDirectLastRecordCapPartialObjectCount = 0;
  uint64_t semanticSceneDirectLastScanCapPartialObjectCount = 0;
  uint64_t semanticSceneDirectLastMinGeosetsPerObject = 0;
  uint64_t semanticSceneDirectLastMaxGeosetsPerObject = 0;
  uint64_t semanticSceneDirectLastSubmittedIdentityHash = 0;
  uint64_t semanticSceneDirectIdentityChurnCount = 0;
  uint64_t semanticSceneDirectRecordCapHitCount = 0;
  uint64_t semanticSceneDirectRecordCapTruncatedRecordCount = 0;
  uint64_t semanticSceneDirectScanCapHitCount = 0;
  uint64_t semanticSceneDirectObjectGroupedSubmitCount = 0;
  uint64_t semanticSceneDirectObjectGroupedSkipCount = 0;
  uint64_t semanticSceneDirectRecordCapSkipObjectCount = 0;
  uint64_t semanticSceneDirectRecordCapAppendFailCount = 0;
  uint64_t semanticSceneDirectSelectionLeaseActiveKeyCount = 0;
  uint64_t semanticSceneDirectSelectionLeasePrunedKeyCount = 0;
  uint64_t semanticSceneDirectSelectionLeaseSubmittedKeyCount = 0;
  uint64_t semanticSceneDirectStickyFillBudgetRecordCount = 0;
  uint64_t semanticSceneDirectStickyFillAppendedCount = 0;
  uint64_t semanticSceneDirectStickyFillSubmittedCount = 0;
  uint64_t semanticSceneDirectStickyFillMissedCount = 0;
  uint64_t semanticSceneDirectPartLeaseRestoredCount = 0;
  uint64_t semanticSceneDirectPartLeaseUpdatedCount = 0;
  uint64_t semanticSceneDirectPartLeaseExpiredCount = 0;
  uint64_t semanticSceneDirectPartLeaseRejectedDynamicMeshCount = 0;
  uint64_t semanticSceneDirectPartLeaseRejectedNotSelfContainedCount = 0;
  uint64_t semanticSceneDirectPartLeaseRejectedUnsafeBackingCount = 0;
  uint64_t semanticSceneDirectPartLeaseRejectedSelfRenewCount = 0;
  uint64_t semanticSceneDirectPartLeaseBudgetLimitCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseRestoredCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseExpiredCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseBudgetLimitCount = 0;
  uint64_t semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount = 0;
  uint64_t semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount = 0;
  uint64_t semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreCompleteCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreIncompleteSkipCount = 0;
  uint64_t semanticSceneShadowManifestPartOmittedIncompleteCoreCount = 0;
  // Phase 7.25 core epoch planner 专属计数器。
  uint64_t semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreEpochMissingPartCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount = 0;
  // Phase 7.28：skinned palette content stability probe。
  uint64_t semanticSceneSubmittedSkinnedPaletteSourceNoneCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteStablePartSampleCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteHashChurnCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteSourceChurnCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteCountChurnCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount = 0;
  uint64_t semanticSceneDirectPaletteAttributionSnapshotHitCount = 0;
  uint64_t semanticSceneDirectPaletteCaptureTrustedSourceHitCount = 0;
  uint64_t semanticSceneDirectPaletteCaptureTrustedSourceMissCount = 0;
  // Phase 7.30 Step A：stale→live 过渡归因。
  uint64_t semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount = 0;
  uint64_t semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount = 0;
  uint64_t semanticSceneDirectManifestObjectCount = 0;
  uint64_t semanticSceneDirectManifestObservedPartCount = 0;
  uint64_t semanticSceneDirectManifestShadowEligiblePartCount = 0;
  uint64_t semanticSceneDirectObjectCompleteEligibleCount = 0;
  uint64_t semanticSceneDirectObjectIncompleteByScanCapCount = 0;
  uint64_t semanticSceneDirectObjectIncompleteByAlphaPolicyCount = 0;
  uint64_t semanticSceneDirectObjectIncompleteBySliceUnresolvedCount = 0;
  uint64_t semanticSceneDirectObjectIncompleteByPacketBuildFailCount = 0;
  uint64_t semanticSceneDirectObjectIncompleteByAppendFailCount = 0;
  uint64_t semanticSceneDirectSubmittedCompleteObjectCount = 0;
  uint64_t semanticSceneDirectSubmittedPartialObjectCount = 0;
  uint64_t semanticSceneDirectPreparedSliceAuthoritativeCount = 0;
  uint64_t semanticSceneDirectPreparedSliceFallbackLayerIndexCount = 0;
  uint64_t semanticSceneDirectPreparedSliceMissingCount = 0;
  uint64_t semanticScenePreparedProbeAttemptCount = 0;
  uint64_t semanticScenePreparedProbeContextReadyCount = 0;
  uint64_t semanticScenePreparedProbeBackingReadableCount = 0;
  uint64_t semanticScenePreparedSliceRecordedCount = 0;
  uint64_t semanticScenePreparedSliceQueryAttemptCount = 0;
  uint64_t semanticScenePreparedSliceQueryHitCount = 0;
  uint64_t semanticScenePreparedSliceQueryMissCount = 0;
  uint64_t semanticSceneShadowManifestObjectCount = 0;
  uint64_t semanticSceneShadowManifestPartCount = 0;
  uint64_t semanticSceneShadowManifestStableObjectCount = 0;
  uint64_t semanticSceneShadowManifestNewObjectCount = 0;
  uint64_t semanticSceneShadowManifestExpiredObjectCount = 0;
  uint64_t semanticSceneShadowManifestFreshPartCount = 0;
  uint64_t semanticSceneShadowManifestLeaseablePartCount = 0;
  uint64_t semanticSceneShadowManifestPoseStalePartCount = 0;
  uint64_t semanticSceneShadowManifestSliceStalePartCount = 0;
  uint64_t semanticSceneShadowManifestExpiredPartCount = 0;
  uint64_t semanticSceneShadowManifestMultiSlicePartCount = 0;
  uint64_t semanticSceneShadowManifestPayload11CChurnCount = 0;
  uint64_t semanticSceneShadowManifestRenderablePartChurnCount = 0;
  uint64_t semanticSceneShadowManifestCModelPoseHitCount = 0;
  uint64_t semanticSceneShadowManifestCModelPoseMissCount = 0;
  uint64_t semanticSceneShadowManifestCModelPoseNoRuntimeCount = 0;
  uint64_t semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr = 0;
  uint64_t semanticSceneShadowManifestCModelPoseLastMatrixCount = 0;
  uint64_t semanticSceneShadowManifestCModelPoseLastMatrixHash = 0;
  uint64_t semanticSceneSubmittedObjectJaccardMilli = 0;
  uint64_t semanticSceneSubmittedPartJaccardMilli = 0;
  uint64_t semanticSceneVisibleLookupPartLayerHitCount = 0;
  uint64_t semanticSceneVisibleLookupSingleFallbackCount = 0;
  uint64_t semanticSceneVisibleLookupMissCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingNotCheckedCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingPassCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingFailNoRenderablePartCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingFailLookupMissCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingFailNonMainQueueCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingFailNonWorldGroupCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingFailIdentityMismatchCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount = 0;
  uint64_t semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount = 0;
  uint64_t semanticSceneDirectPaletteHashChurnCount = 0;
  uint64_t semanticSceneDirectGroupHashChurnCount = 0;
  uint64_t semanticSceneDirectStableGroupHashChurnCount = 0;
  uint64_t semanticSceneDirectStream1PtrChurnCount = 0;
  uint64_t semanticSceneDirectGeometrySourceHashChurnCount = 0;
  uint64_t semanticSceneDirectSameCasterComparisonCount = 0;
  uint64_t semanticSceneDirectIdentitySkippedChurnCount = 0;
  uint64_t semanticSceneDirectPaletteRootDeltaSampleCount = 0;
  uint64_t semanticSceneDirectPaletteRootHashChangedTinyDeltaCount = 0;
  uint64_t semanticSceneDirectPaletteRootHashChangedSmallDeltaCount = 0;
  uint64_t semanticSceneDirectPaletteRootHashChangedMediumDeltaCount = 0;
  uint64_t semanticSceneDirectPaletteRootHashChangedLargeDeltaCount = 0;
  uint64_t semanticSceneDirectPaletteRootMaxDeltaMilli = 0;
  uint64_t semanticSceneDirectSelectionKeyUnitPtrCount = 0;
  uint64_t semanticSceneDirectSelectionKeyJHandleCount = 0;
  uint64_t semanticSceneDirectSelectionKeyRuntimeModelCount = 0;
  uint64_t semanticSceneDirectSelectionKeyWorldObjectCount = 0;
  uint64_t semanticSceneDirectSelectionKeySceneNodeCount = 0;
  uint64_t semanticSceneDirectSelectionKeyModelMeshCount = 0;
  uint64_t semanticSceneDirectSelectionKeyRenderablePartCount = 0;
  uint64_t semanticSceneDirectLastSubmittedSceneNode = 0;
  uint64_t semanticSceneDirectLastSubmittedRenderablePart = 0;
  uint64_t semanticSceneDirectLastSubmittedMeshData = 0;
  uint64_t semanticSceneDirectLastSubmittedPaletteHash = 0;
  uint64_t semanticSceneDirectLastSubmittedGroupHash = 0;
  uint64_t semanticSceneDirectLastSubmittedStableGroupHash = 0;
  uint64_t semanticSceneDirectLastSubmittedStream1Ptr = 0;
  uint64_t semanticSceneDirectLastSubmittedGeometrySourceHash = 0;
  uint64_t semanticSceneLastAppendedGeometrySourceHash = 0;
  uint64_t semanticSceneLastAppendedGeometryId = 0;
  uint64_t semanticSceneShadowCastersCount = 0;
  uint64_t semanticSceneReplayDrawsCount = 0;
  uint64_t semanticSceneShadowMapDrawnCasters = 0;
  uint64_t semanticSceneShadowMapCascadeCulledCount = 0;
  uint64_t semanticSceneShadowMapSkinnedCasterCount = 0;
  uint64_t semanticSceneShadowMapSkinnedPreparedCount = 0;
  uint64_t semanticSceneShadowMapSkinnedInvalidBufferCount = 0;
  uint64_t semanticSceneShadowMapSkinnedInvalidPipelineCount = 0;
  uint64_t semanticSceneShadowMapSkinnedDrawnCount = 0;
  uint64_t semanticSceneShadowTaaActive = 0;
  uint64_t shadowTaaRequestedMode = 0;
  uint64_t shadowTaaEffectiveMode = 0;
  uint64_t shadowTaaShaderMode = 0;
  uint64_t shadowTaaHistoryValid = 0;
  uint64_t shadowTaaHistoryReadable = 0;
  uint64_t shadowTaaHistoryGeneration = 0;
  uint64_t shadowTaaLastInvalidationReason = 0;
  uint64_t shadowTaaFixedWallBypassCount = 0;
  uint64_t pointShadowPersistentConfiguredMode = 0;
  uint64_t pointShadowPersistentEffectiveMode = 0;
  uint64_t pointShadowPersistentLastBeginRejectReason = 0;
  uint64_t pointShadowPersistentWorkerCreated = 0;
  uint64_t pointShadowPersistentWorkerAvailable = 0;
  uint64_t pointShadowPersistentLastFrameSerial = 0;
  uint64_t pointShadowPersistentBeginAttempts = 0;
  uint64_t pointShadowPersistentBeginEligible = 0;
  uint64_t pointShadowPersistentWorkerCreateCount = 0;
  uint64_t pointShadowPersistentWorkerThreadStarts = 0;
  uint64_t pointShadowPersistentAccepted = 0;
  uint64_t pointShadowPersistentReady = 0;
  uint64_t pointShadowPersistentDeadlineFallback = 0;
  uint64_t pointShadowPersistentRejectedFallback = 0;
  uint64_t pointShadowPersistentObserveMatch = 0;
  uint64_t pointShadowPersistentMismatch = 0;
  uint64_t pointShadowPersistentConsumed = 0;
  uint64_t pointShadowPersistentFailed = 0;
  uint64_t pointShadowPersistentBusy = 0;
  uint64_t csmRequestedResolution = 0;
  uint64_t csmEffectiveResolution = 0;
  uint64_t csmFallbackReason = 0;
  uint64_t csmFallbackLatched = 0;
  uint64_t csmResourceGeneration = 0;
  uint64_t csmResourceRebuildCount = 0;
  uint64_t csmMemoryBudgetBytes = 0;
  uint64_t csmMemoryAvailableBytes = 0;
  uint64_t shadowArenaUsedBytes = 0;
  uint64_t shadowArenaResidentBytes = 0;
  uint64_t shadowArenaResidentLimitBytes = 0;
  uint64_t shadowArenaGeneration = 0;
  uint64_t shadowArenaBusyReuseRejectCount = 0;
  uint64_t shadowArenaOverflowCount = 0;
  uint64_t shadowArenaReservedBytes = 0;
  uint64_t shadowArenaCommittedBytes = 0;
  uint64_t shadowArenaRolledBackBytes = 0;
  uint64_t shadowArenaAdmissionRejectedCount = 0;
  uint64_t shadowArenaPartialTransactionCount = 0;
  uint64_t shadowArenaPageTailWasteBytes = 0;
  uint64_t shadowArenaPositionBytes = 0;
  uint64_t shadowArenaBlendBytes = 0;
  uint64_t shadowArenaUvBytes = 0;
  uint64_t shadowArenaIndexBytes = 0;
  uint64_t shadowArenaTerrainBytes = 0;
  uint64_t shadowArenaModelBytes = 0;
  uint64_t shadowArenaSkinnedBytes = 0;
  uint64_t shadowArenaUpBytes = 0;
  uint64_t shadowArenaUniqueSourceBytes = 0;
  uint64_t shadowArenaDuplicateBytesSaved = 0;
  uint64_t shadowArenaExactIndexTrimAcceptedCount = 0;
  uint64_t shadowArenaExactIndexTrimRejectedCount = 0;
  uint64_t shadowArenaExactIndexTrimBytesSaved = 0;
  uint64_t shadowArenaFrameIncomplete = 0;
  uint64_t shadowCpuSpanAcceptedCount = 0;
  uint64_t shadowCpuSpanRejectedCount = 0;
  uint64_t shadowCpuSpanLastRejectReason = 0;
  uint64_t shadowCpuSpanLastAllocationBytes = 0;
  uint64_t shadowCpuSpanLastBindingOffset = 0;
  uint64_t shadowCpuSpanLastReadBytes = 0;
  uint64_t shadowCpuSpanLastSourceIdentityGeneration = 0;
  uint64_t shadowCpuSpanLastAllocationGeneration = 0;
  uint64_t shadowCpuSpanLastContentGeneration = 0;
  uint64_t queueSubmittedSerial = 0;
  uint64_t queueCompletedSerial = 0;
  int64_t queueLastResult = 0;
  uint64_t shadowEvidenceRetentionRevision = 0;
  uint64_t shadowEvidenceCollectorAttached = 0;
  uint64_t semanticSceneReceiverReuseShadowMap = 0;
  uint64_t semanticSceneReceiverInputValid = 0;
  uint64_t semanticSceneReceiverInputRejectReason = 0;
  uint64_t semanticSceneReceiverNeedPass = 0;
  uint64_t semanticSceneReceiverNeedShadowMap = 0;
  uint64_t semanticSceneReceiverHasCompleteShadowMap = 0;
  uint64_t semanticSceneReceiverHasUsableDirectionalShadow = 0;
  uint64_t semanticSceneReceiverActiveStrengthMilli = 0;
  uint64_t semanticSceneReceiverUboStrengthMilli = 0;
  uint64_t semanticSceneReceiverDebugMode = 0;
  uint64_t semanticSceneReceiverCsmCascadeCount = 0;
  uint64_t semanticSceneReceiverRunEntryFlags = 0;
  uint64_t semanticSceneReceiverRunEarlyReturnReason = 0;
  uint64_t semanticSceneShadowMapExecutedThisFrame = 0;
  uint64_t semanticSceneReceiverSettingsShadowsEnabled = 0;
  uint64_t semanticSceneReceiverSettingsOutlineEnabled = 0;
  uint64_t semanticSceneReceiverSettingsRawStrengthMilli = 0;
  uint64_t semanticSceneReceiverComputedShadowStrengthMilli = 0;
  uint64_t semanticSceneReceiverHasSunShadow = 0;
  uint64_t semanticSceneReceiverHasPointShadow = 0;
  uint64_t semanticSceneReceiverNeedOutlinePass = 0;
  uint64_t semanticSceneReceiverZeroStrengthFrameCount = 0;
  uint64_t semanticSceneReceiverDrawnWithZeroStrengthCount = 0;
  uint64_t semanticSceneReceiverNoCompleteShadowMapCount = 0;
  uint64_t semanticSceneReceiverNoShadowMapImageCount = 0;
  uint64_t semanticSceneReceiverNoShadowMapSampleViewCount = 0;
  uint64_t semanticSceneReceiverNoCandidateCsmCount = 0;
  uint64_t semanticSceneReceiverCsmFallbackToLastGoodCount = 0;
  uint64_t semanticSceneReceiverHoldInvalidCsmCount = 0;
  uint64_t semanticSceneReceiverHoldEmptyReplayCount = 0;
  uint64_t semanticSceneReceiverHoldIdentityChurnCount = 0;
  uint64_t semanticSceneReceiverReuseInvalidatedAfterEnsureCount = 0;
  uint64_t semanticSceneShadowMapRenderSkippedNoResourcesCount = 0;
  uint64_t semanticSceneShadowMapRenderSkippedNoMatrixBufferCount = 0;
  uint64_t semanticSceneReceiverViewportX = 0;
  uint64_t semanticSceneReceiverViewportY = 0;
  uint64_t semanticSceneReceiverViewportWidth = 0;
  uint64_t semanticSceneReceiverViewportHeight = 0;
};

struct War3RuntimeStatusSnapshot {
  uint64_t timestampMs = 0;
  std::string source;
  uint64_t frameIndex = 0;
  War3RuntimeStatusModuleSnapshot module = {};
  War3RuntimeStatusPerfSnapshot perf = {};
  War3RuntimeStatusProfileSnapshot profile = {};
  War3RuntimeStatusRuntimeSnapshot runtime = {};
  War3RuntimeStatusRenderSnapshot render = {};
  War3RuntimeStatusFrameSnapshot frame = {};
  War3RuntimeStatusShadowSnapshot shadow = {};
};

/**
 * @brief 记录运行时初始化总览（仅首次打印）。
 *
 * 输出内容包含：
 * - 模块系统统计；
 * - PerfMonitor 开关状态。
 *
 * @param source 触发来源（例如 `ActivateWar3Runtime`）。
 */
void LogRuntimeSummaryOnce(const char* source);

/**
 * @brief 低频记录运行时健康状态。
 *
 * @param frameIndex 当前帧号。
 * @param interval 每隔多少帧输出一次。
 */
void LogRuntimeHealthPeriodic(uint64_t frameIndex, uint32_t interval = 1200);

/**
 * @brief 立即写出 runtime 状态快照到 `WarVK/Temp/runtime_status.json`。
 *
 * 用于外部自动化（MCP）按文件轮询“运行时就绪/进图状态”。
 *
 * @param source 来源标签（例如 `War3Events/OnGameStart`）。
 * @param frameIndex 当前帧号（未知时传 0）。
 */
void ExportRuntimeStatusSnapshot(const char* source, uint64_t frameIndex = 0);

/**
 * @brief 查询当前运行时状态快照。
 *
 * 该接口是 control plane 与自动化的统一状态来源。
 */
War3RuntimeStatusSnapshot QueryRuntimeStatusSnapshot(
    const char* source = nullptr, uint64_t frameIndex = 0);

/**
 * @brief 标记“已看到正式进游戏渲染信号”。
 *
 * 用于修正仅靠 NetEvent runtimeReady 不足以覆盖的场景。
 */
void MarkInGameRenderReady(const char* source, uint64_t frameIndex = 0);

/**
 * @brief 查询“已看到正式进游戏渲染信号”标记。
 */
bool IsInGameRenderReady();

/**
 * @brief 重置运行态就绪补充信号。
 */
void ResetRuntimeReadySignals();

uint64_t RequestShadowEvidenceRetention();
uint64_t QueryShadowEvidenceRetentionRevision();
void SetShadowEvidenceCollectorAttached(bool attached);
bool IsShadowEvidenceCollectorAttached();

} // namespace dxvk::war3::tools
