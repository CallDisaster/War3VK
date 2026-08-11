#pragma once

#include "../../d3d9_war3_scene.h"
#include "war3_render_objects.h"

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

namespace dxvk::war3::render {

struct CurrentDrawContractRecord;

enum class SemanticDataPerfTag : uint32_t {
  Unknown = 0,
  ModelHook = 1,
  PoseHook = 2,
  AttachmentHook = 3,
  FrameRegistryPublish = 4,
  ContractCapture = 5,
  ConsumerBuild = 6,
  ModelBuildChildPreScan = 7,
  ModelBuildChildPostScan = 8,
  ModelRuntimeChildLinkBuild = 9,
  ModelPromoteRuntime = 10,
  PoseRuntimePose = 11,
  PoseRuntimePaletteTree = 12,
  PoseRuntimeMatrixPalette = 13,
  PoseSpriteFrameSourceIdentity = 14,
  PoseSpriteFramePose = 15,
  PoseRuntimeMatrixPublisher = 16,
  ModelRuntimeChildCollect = 17,
  ModelRuntimeChildBootstrap = 18,
  ModelRuntimeChildParentMap = 19,
  ModelRuntimeChildOwnerPropagate = 20,
  PoseSpriteAttachmentHit = 21,
  PoseSpriteTransformRead = 22,
  PoseSpriteIdentityLookup = 23,
  PoseSpriteBaseAlias = 24,
  PoseSpritePublishPose = 25,
  PoseSpritePaletteGate = 26,
  ModelSpriteHostBind = 27,
  ModelRuntimeModelBinding = 28,
  ModelGeosetResource = 29,
  ModelRuntimeCtor = 30,
  ModelRuntimeResolve = 31,
  ModelRuntimeInitCopy = 32,
  AttachmentAttachedEffectInit = 33,
  AttachmentAttachedEffectDirect = 34,
  AttachmentAttachModelToPoint = 35,
  AttachmentOverrideSharedPreset = 36,
  AttachmentOverrideLocalPoint = 37,
  AttachmentOverridePrimaryPreset = 38,
  // control-plane 摘要查询触发的异步 contract 刷新请求。曾与 ConsumerBuild
  // 共用标签，导致 populate 热点归因被查询频率污染；现独立计量。
  SummaryRefreshRequest = 39,
  // Safe current-frame-only shadow metadata capture. The hot draw path uses
  // sampled raw QPC and publishes one aggregate sample per rendered frame, so
  // this tag's avgUsPerCall is the metadata self time per active frame rather
  // than a per-draw instrumenting scope.
  ShadowMetadataCapture = 40,
  Count = 41,
};

struct ShadowRuntimeCadenceSample {
  uint64_t serial = 0;
  uint64_t frameIndex = 0;
  uint64_t sceneFrameSerial = 0;
  uint64_t selectedFrameSerial = 0;
  uint64_t reusableFrameSerial = 0;
  uint64_t sourcePublishRevision = 0;
  uint64_t targetPublishRevision = 0;
  uint64_t populateReturnReason = 0;
  uint64_t inputDrawCount = 0;
  uint64_t inputSkinnedCount = 0;
  uint64_t submittedDrawCount = 0;
  uint64_t submittedSkinnedCount = 0;
  uint64_t directSubmittedRecordCount = 0;
  uint64_t directSubmittedObjectCount = 0;
  uint64_t shadowCastersCount = 0;
  uint64_t replayDrawsCount = 0;
  uint64_t shadowMapDrawnCasters = 0;
  uint64_t shadowMapSkinnedDrawnCount = 0;
  uint64_t receiverNeedShadowMap = 0;
  uint64_t receiverHasCompleteShadowMap = 0;
  uint64_t receiverReuseShadowMap = 0;
  uint64_t shadowMapExecutedThisFrame = 0;
  uint64_t receiverRunEarlyReturnReason = 0;
  uint64_t receiverRunEntryFlags = 0;
  uint64_t receiverActiveStrengthMilli = 0;
  uint64_t receiverCsmCascadeCount = 0;
  uint64_t receiverHoldInvalidCsm = 0;
  uint64_t receiverHoldEmptyReplay = 0;
  uint64_t receiverHoldIdentityChurn = 0;
  uint64_t dynamicPoseSignature = 0;
  uint64_t submittedIdentityHash = 0;
  uint64_t lastSubmittedPaletteHash = 0;
  uint64_t lastSubmittedGroupHash = 0;
  uint64_t currentDrawPublishReadyCount = 0;
  uint64_t currentDrawQueryHitCount = 0;
  uint64_t currentDrawLastRenderFrameIndex = 0;
  uint64_t currentDrawLastFrameTag = 0;
  uint64_t submitPaletteContentAgeSampleCount = 0;
  uint64_t submitPaletteContentAgeLag3PlusCount = 0;
  uint64_t shadowMatrixSceneKey = 0;
  uint64_t shadowMatrixUploadSerial = 0;
  uint64_t shadowMatrixBufferObjectPtr = 0;
  uint64_t shadowMatrixBufferOffset = 0;
  uint64_t shadowMatrixBufferSize = 0;
  uint64_t shadowMatrixBufferGpuAddress = 0;
  uint64_t receiverCameraHash = 0;
  uint64_t receiverSunDirectionHash = 0;
  uint64_t receiverCsmHash = 0;
  uint64_t receiverCameraDeltaNano = 0;
  uint64_t receiverSunDeltaNano = 0;
  uint64_t receiverCsmDeltaNano = 0;
  uint64_t receiverSnappedCenterDeltaTexelsNano = 0;
  uint64_t receiverTexelSizeDeltaNano = 0;
  uint64_t replayBackingHash = 0;
  uint64_t stage13ReplayContentHash = 0;
  uint64_t stage13ReplayBackingHash = 0;
  uint64_t stage13ReplayDrawCount = 0;
  uint64_t shadowMapRenderSerial = 0;
  uint64_t shadowMapImagePtr = 0;
  uint64_t shadowMapSampleViewPtr = 0;
  uint64_t shadowCurrentImagePtr = 0;
  uint64_t shadowCurrentViewPtr = 0;
  uint64_t shadowHistoryReadImagePtr = 0;
  uint64_t shadowHistoryReadViewPtr = 0;
  uint64_t shadowHistoryWriteImagePtr = 0;
  uint64_t shadowHistoryWriteViewPtr = 0;
  uint64_t shadowVisibilityExecutedThisFrame = 0;
  uint64_t receiverDrawExecutedThisFrame = 0;
  uint64_t shadowTaaMode = 0;
  uint64_t shadowHistoryValidBefore = 0;
  uint64_t shadowHistoryValidAfter = 0;
  uint64_t shadowHistoryReadIndex = 0;
  uint64_t shadowHistoryWriteIndex = 0;
  uint64_t shadowHistoryAdvancedThisFrame = 0;
  uint64_t shadowHistoryAdvanceSkippedIncomplete = 0;
  uint64_t shadowHistoryInvalidationMask = 0;
  uint64_t shadowReceiverSampleSource = 0;
};

inline constexpr size_t kShadowRuntimeCadenceSampleCapacity = 128u;

struct ShadowPoseFullTraceStatus {
  bool enabled = false;
  bool active = false;
  bool includePoseRecords = true;
  bool includeShadowObjectRecords = true;
  bool includeCurrentDrawRecords = true;
  bool includeFinalCasterRecords = true;
  bool includeMatrixBytes = false;
  bool stoppedByLimit = false;
  uint32_t maxSeconds = 15u;
  uint32_t maxPoseRecords = 0u;
  uint32_t maxShadowObjectRecords = 0u;
  uint32_t maxCurrentDrawRecords = 0u;
  uint32_t maxFinalCasterRecords = 0u;
  uint64_t traceEpoch = 0u;
  uint64_t frameEventsWritten = 0u;
  uint64_t recordEventsWritten = 0u;
  std::string path;
};

// Compact, by-value snapshot for the per-frame GPU flight recorder.  The full
// bridge summary contains hundreds of unrelated counters and is reserved for
// low-frequency control-plane queries.
struct ShadowProducerRuntimeDiagnostics {
  uint64_t producerSealFrameSerial = 0;
  uint64_t producerSealMapEpoch = 0;
  uint64_t producerSealDeviceEpoch = 0;
  uint64_t producerRequiredCasterOmissionCount = 0;
  uint64_t producerExactBudgetDeferredUniqueCasterCount = 0;
  uint64_t producerPositionAllocBudgetCount = 0;
  uint64_t producerUvAllocBudgetCount = 0;
  uint64_t producerIndexAllocBudgetCount = 0;
  uint64_t producerAllocationFailureCount = 0;
  uint64_t producerFallbackByteBudgetCount = 0;
  uint64_t producerArenaAdmissionCount = 0;
  uint64_t producerFreezeFailureCount = 0;
  uint64_t producerSoftPriorityBudgetCount = 0;
  uint64_t producerCompletenessReasonMask = 0;
  uint64_t producerCompletenessSealed = 0;
  uint64_t producerCompletenessCounterOverflow = 0;
  uint64_t drawTimeVBCacheStaticLiveBytes = 0;
  uint64_t drawTimeVBCacheStaticProtectedBytes = 0;
  uint64_t drawTimeVBCacheStaticOverCapBytes = 0;
  uint64_t drawTimeVBCacheStaticOverCapFrameCount = 0;
  uint64_t drawTimeVBCacheStaticEvictedBytes = 0;
  uint64_t drawTimeVBCacheStaticEvictedEntryCount = 0;
  uint64_t drawTimeSnapshotPageResidentBytes = 0;
  uint64_t drawTimeSnapshotPageUsedBytes = 0;
  uint64_t drawTimeSnapshotPageCreateCount = 0;
  uint64_t drawTimeSnapshotSuballocationCount = 0;
  uint64_t drawTimeSnapshotSuballocationBytes = 0;
  uint64_t drawTimeSnapshotPageReclaimedCount = 0;
  uint64_t drawTimeSnapshotPageCapacityRejectCount = 0;
  uint64_t drawTimeSnapshotPageAllocationFailureCount = 0;
  uint64_t drawTimeVBCacheIndexedUnknownRangeFallbackCount = 0;
  uint64_t drawTimeGenerationBackedPositionReuseCount = 0;
  uint64_t drawTimeGenerationBackedUvReuseCount = 0;
  uint64_t drawTimeGenerationBackedIndexReuseCount = 0;
  uint64_t drawTimeGenerationBackedCopyBytesSaved = 0;
};

struct ShadowRuntimeBridgeSummary {
  uint64_t runtimeChildLinkBuildCount = 0;
  uint64_t runtimeChildLinkBuiltChildCount = 0;
  uint64_t runtimeChildBuildTimeDirectPublishCount = 0;
  uint64_t runtimeChildBuildTimeDirectPublishWithResourceCount = 0;
  uint64_t runtimeChildBuildModelDataPreLinkCount = 0;
  uint64_t runtimeChildBuildModelDataPostLinkCount = 0;
  uint64_t runtimeChildBuildModelDataPreUnreadableLinkCount = 0;
  uint64_t runtimeChildBuildModelDataPostUnreadableLinkCount = 0;
  uint64_t runtimeMatrixRangeCopyCount = 0;
  uint64_t runtimeMatrixFlushCount = 0;
  uint64_t runtimeMatrixPublisherPaletteReadyCount = 0;
  uint64_t runtimePoseUpdatePalettePublishCount = 0;
  uint64_t runtimePoseUpdateLastRuntimeModelPtr = 0;
  uint64_t runtimePoseUpdateLastMatrixCount = 0;
  uint64_t runtimePoseUpdateLastMatrixHash = 0;
  uint64_t runtimeMatrixWriteCount = 0;
  uint64_t runtimeMatrixWritePublishCount = 0;
  uint64_t runtimeMatrixWriteMissCount = 0;
  uint64_t runtimeGroupPaletteWrapperCallCount = 0;
  uint64_t runtimeGroupPaletteWrapperPartCount = 0;
  uint64_t runtimeGroupPaletteWrapperBindingCount = 0;
  uint64_t runtimeSimpleGroupPaletteCallCount = 0;
  uint64_t runtimeSimpleGroupPaletteSlotCapturedCount = 0;
  uint64_t runtimeSimpleGroupPaletteSlotUnreadableCount = 0;
  uint64_t renderablePartPaletteBindingQueryHitCount = 0;
  uint64_t renderablePartPaletteBindingQueryMissCount = 0;
  uint64_t renderablePartPaletteSnapshotCapturedCount = 0;
  uint64_t renderablePartPaletteSnapshotTooLargeCount = 0;
  uint64_t renderablePartPaletteSnapshotUnreadableCount = 0;
  uint64_t renderablePartPaletteSnapshotQueryHitCount = 0;
  uint64_t renderablePartPaletteSnapshotQueryMissCount = 0;
  // Phase 7.47 dt gate probe
  uint64_t spriteUberPreRenderTotalCount = 0;
  uint64_t spriteUberPreRenderDtZeroCount = 0;
  uint64_t spriteUberPreRenderDtBelowEpsilonCount = 0;
  uint64_t spriteUberPreRenderDtPositiveCount = 0;
  uint64_t spriteUberPreRenderDtNegativeCount = 0;
  uint64_t spriteUberPreRenderLastDtBits = 0;
  uint64_t spriteUberPreRenderLastZeroDtFrameTag = 0;
  uint64_t spriteUberPreRenderLastPositiveDtFrameTag = 0;
  uint64_t runtimeMatrixWriteFramesWithHitCount = 0;
  uint64_t runtimeMatrixWriteFramesEmptyCount = 0;
  uint64_t runtimeGroupPaletteWrapperFramesWithHitCount = 0;
  uint64_t runtimeGroupPaletteWrapperFramesEmptyCount = 0;
  uint64_t runtimeSimpleGroupPaletteFramesWithHitCount = 0;
  uint64_t runtimeSimpleGroupPaletteFramesEmptyCount = 0;
  uint64_t runtimeMatrixWriteLastRuntimeModelPtr = 0;
  uint64_t runtimeMatrixWriteLastMatrixIndex = 0;
  uint64_t runtimeMatrixWriteLastMatrixCount = 0;
  uint64_t runtimeMatrixWriteLastMatrixHash = 0;
  uint64_t runtimeMatrixRangeCopyPalettePublishHitCount = 0;
  uint64_t runtimeMatrixRangeCopyPalettePublishMissCount = 0;
  uint64_t runtimeMatrixRangeCopyPaletteFallbackCModelCount = 0;
  uint64_t runtimeMatrixFlushPaletteSuppressedCount = 0;
  uint64_t runtimeMatrixRangeCopyLastRuntimeModelPtr = 0;
  uint64_t runtimeMatrixRangeCopyLastContextPtr = 0;
  uint64_t runtimeMatrixRangeCopyLastSourceBasePtr = 0;
  uint64_t runtimeMatrixRangeCopyLastMatrixCount = 0;
  uint64_t runtimeMatrixRangeCopyLastMatrixHash = 0;
  uint64_t runtimeMatrixPublisherAttachmentRootHitCount = 0;
  uint64_t runtimeMatrixPublisherAttachmentOwnerHitCount = 0;
  uint64_t runtimeMatrixPublisherAttachmentChildHitCount = 0;
  uint64_t runtimeMatrixPublisherAttachmentAliasHitCount = 0;
  uint64_t runtimeMatrixPublisherAttachmentRootPaletteReadyCount = 0;
  uint64_t runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount = 0;
  uint64_t runtimeMatrixPublisherAttachmentChildPaletteReadyCount = 0;
  uint64_t attachmentAncestorIdentityHintWriteCount = 0;
  uint64_t modelRegistryCount = 0;
  uint64_t instanceRegistryCount = 0;
  uint64_t runtimeBoundCount = 0;
  uint64_t runtimeCreationProvenanceCount = 0;
  uint64_t runtimeResolveProvenanceCount = 0;
  uint64_t runtimeSourceObjectCount = 0;
  uint64_t runtimeOwnerIdentityCount = 0;
  uint64_t completeIdentityCount = 0;
  uint64_t poseReadyCount = 0;
  uint64_t spriteFramePoseCount = 0;
  uint64_t matrixPaletteCount = 0;
  uint64_t shadowGeosetResourceCount = 0;
  uint64_t shadowReadyGeosetCount = 0;
  uint64_t shadowModelResourceCount = 0;
  uint64_t shadowRuntimeModelCount = 0;
  uint64_t visibleRenderableCount = 0;
  uint64_t visibleRenderableMainCount = 0;
  uint64_t visibleRenderableTransparentCount = 0;
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
  uint64_t visibleRenderableSample0WorldObjectEntryPtr = 0;
  uint64_t visibleRenderableSample0SceneNodePtr = 0;
  uint64_t visibleRenderableSample0UnitPtr = 0;
  uint64_t visibleRenderableSample0JHandle = 0;
  uint64_t visibleRenderableSample0Rawcode = 0;
  uint64_t visibleRenderableSample0RuntimeModelPtr = 0;
  uint64_t visibleRenderableSample0ModelResourcePtr = 0;
  uint64_t visibleRenderableSample0RuntimeGeosetPtr = 0;
  uint64_t visibleRenderableSample0RuntimeGeosetDataPtr = 0;
  uint64_t visibleRenderableSample0SceneInstanceRuntimeModelPtr = 0;
  uint64_t visibleRenderableSample0SceneInstanceModelResourcePtr = 0;
  uint64_t visibleRenderableSample0SceneShadowRuntimeModelPtr = 0;
  uint64_t visibleRenderableSample0SceneShadowModelResourcePtr = 0;
  uint64_t visibleRenderableSample0ScenePoseMatrixCount = 0;
  uint64_t visibleRenderableSample0GeosetModelResourcePtr = 0;
  uint64_t visibleRenderableSample0GeosetModelKey = 0;
  uint64_t visibleRenderableSample1WorldObjectEntryPtr = 0;
  uint64_t visibleRenderableSample1SceneNodePtr = 0;
  uint64_t visibleRenderableSample1UnitPtr = 0;
  uint64_t visibleRenderableSample1JHandle = 0;
  uint64_t visibleRenderableSample1Rawcode = 0;
  uint64_t visibleRenderableSample1RuntimeModelPtr = 0;
  uint64_t visibleRenderableSample1ModelResourcePtr = 0;
  uint64_t visibleRenderableSample1RuntimeGeosetPtr = 0;
  uint64_t visibleRenderableSample1RuntimeGeosetDataPtr = 0;
  uint64_t visibleRenderableSample1SceneInstanceRuntimeModelPtr = 0;
  uint64_t visibleRenderableSample1SceneInstanceModelResourcePtr = 0;
  uint64_t visibleRenderableSample1SceneShadowRuntimeModelPtr = 0;
  uint64_t visibleRenderableSample1SceneShadowModelResourcePtr = 0;
  uint64_t visibleRenderableSample1ScenePoseMatrixCount = 0;
  uint64_t visibleRenderableSample1GeosetModelResourcePtr = 0;
  uint64_t visibleRenderableSample1GeosetModelKey = 0;
  bool sceneCollectorGroupLocalAggregationEnabled = false;
  uint64_t worldObjectListEntryCount = 0;
  uint64_t worldObjectListNullEntryCount = 0;
  uint64_t worldObjectListOwnerHintZeroCount = 0;
  uint64_t worldObjectListOwnerHintNonzeroCount = 0;
  uint64_t worldObjectListOwnerHintHandleCount = 0;
  uint64_t worldObjectListOwnerHintUnitPtrCount = 0;
  uint64_t worldObjectListOwnerHintZeroContextAcceptedCount = 0;
  uint64_t worldObjectListAcceptedIdentityCount = 0;
  uint64_t lastWorldObjectListEntryWorldObjectEntryPtr = 0;
  uint64_t lastWorldObjectListEntryOwnerHintValue = 0;
  uint64_t lastWorldObjectListEntrySceneNodePtr = 0;
  bool renderIdentityFullDiagnostics = false;
  bool worldObjectListEntryWriteProbeHookInstalled = false;
  bool worldObjectEntryRenderContextHookInstalled = false;
  bool worldObjectEntryRenderPrePostProbeEnabled = false;
  bool renderQueueIdentityPrimingHookInstalled = false;
  uint64_t worldObjectEntryRenderCallCount = 0;
  uint64_t worldObjectEntryRenderSceneNodeReadyBeforeCount = 0;
  uint64_t worldObjectEntryRenderSceneNodeReadyAfterCount = 0;
  uint64_t worldObjectEntryRenderSceneNodeFilledByCallCount = 0;
  uint64_t worldObjectEntryRenderSceneNodeChangedCount = 0;
  uint64_t worldObjectEntryRenderKnownListOwnerHintZeroCount = 0;
  uint64_t worldObjectEntryRenderKnownListOwnerHintNonzeroCount = 0;
  uint64_t worldObjectEntryRenderUnknownListOwnerHintCount = 0;
  uint64_t worldObjectListEntryWriteCallCount = 0;
  uint64_t worldObjectListEntryWriteOwnerHintZeroCount = 0;
  uint64_t worldObjectListEntryWriteOwnerHintNonzeroCount = 0;
  uint64_t worldObjectListEntryWriteOwnerHintHandleCount = 0;
  uint64_t worldObjectListEntryWriteOwnerHintUnitPtrCount = 0;
  uint64_t lastWorldObjectEntryRenderEntryPtr = 0;
  uint64_t lastWorldObjectEntryRenderResolvedListOwnerHintValue = 0;
  uint64_t lastWorldObjectListEntryWriteListPtr = 0;
  uint64_t lastWorldObjectListEntryWriteWorldObjectEntryPtr = 0;
  uint64_t lastWorldObjectListEntryWriteOwnerHintValue = 0;
  uint64_t lastWorldObjectEntryRenderSceneNodeBeforePtr = 0;
  uint64_t lastWorldObjectEntryRenderSceneNodeAfterPtr = 0;
  uint64_t shadowRuntimeBoundCount = 0;
  uint64_t shadowIdentityCount = 0;
  uint64_t shadowPoseReadyCount = 0;
  uint64_t attachmentRigidCount = 0;
  uint64_t attachmentRigidCountWithSourceObject = 0;
  uint64_t attachmentRigidCountWithAnyIdentity = 0;
  uint64_t attachmentRigidCountWithWorldObjectEntry = 0;
  uint64_t attachmentRigidCountWithSceneNode = 0;
  uint64_t attachmentRigidCountWithUnitPtr = 0;
  uint64_t attachmentRigidChildRuntimeCreateCallerKnownCount = 0;
  uint64_t attachmentRigidOwnerRuntimeCreateCallerKnownCount = 0;
  uint64_t attachmentRigidRootRuntimeCreateCallerKnownCount = 0;
  uint64_t attachmentRigidChildRuntimeCreateHandleKnownCount = 0;
  uint64_t attachmentRigidOwnerRuntimeCreateHandleKnownCount = 0;
  uint64_t attachmentRigidRootRuntimeCreateHandleKnownCount = 0;
  uint64_t attachmentRigidChildRuntimeResolveCallerKnownCount = 0;
  uint64_t attachmentRigidOwnerRuntimeResolveCallerKnownCount = 0;
  uint64_t attachmentRigidRootRuntimeResolveCallerKnownCount = 0;
  uint64_t attachmentRigidChildRuntimeOwnerIdentityCount = 0;
  uint64_t attachmentRigidOwnerRuntimeOwnerIdentityCount = 0;
  uint64_t attachmentRigidRootRuntimeOwnerIdentityCount = 0;
  uint64_t attachmentRigidOwnerRuntimeRecordKnownCount = 0;
  uint64_t attachmentRigidRootRuntimeRecordKnownCount = 0;
  uint64_t attachmentRigidChildRuntimeParentLinkKnownCount = 0;
  uint64_t attachmentRigidChildRuntimeRecordKnownCount = 0;
  uint64_t attachmentRigidChildRuntimeModelResourceKnownCount = 0;
  uint64_t attachmentRigidChildRuntimePoseKnownCount = 0;
  uint64_t attachmentRigidChildRuntimeMatchesAttachedEffectInitCount = 0;
  uint64_t attachmentRigidChildRuntimeMatchesAttachModelToPointCount = 0;
  uint64_t contractAttachmentRigidCount = 0;
  uint64_t contractAttachmentRigidCountWithSourceObject = 0;
  uint64_t contractAttachmentRigidCountWithAnyIdentity = 0;
  uint64_t contractAttachmentRigidCountWithWorldObjectEntry = 0;
  uint64_t contractAttachmentRigidCountWithSceneNode = 0;
  uint64_t contractAttachmentRigidCountWithUnitPtr = 0;
  uint64_t contractAttachmentRigidChildRuntimeCreateCallerKnownCount = 0;
  uint64_t contractAttachmentRigidOwnerRuntimeCreateCallerKnownCount = 0;
  uint64_t contractAttachmentRigidRootRuntimeCreateCallerKnownCount = 0;
  uint64_t contractAttachmentRigidChildRuntimeCreateHandleKnownCount = 0;
  uint64_t contractAttachmentRigidOwnerRuntimeCreateHandleKnownCount = 0;
  uint64_t contractAttachmentRigidRootRuntimeCreateHandleKnownCount = 0;
  uint64_t contractAttachmentRigidChildRuntimeResolveCallerKnownCount = 0;
  uint64_t contractAttachmentRigidOwnerRuntimeResolveCallerKnownCount = 0;
  uint64_t contractAttachmentRigidRootRuntimeResolveCallerKnownCount = 0;
  uint64_t contractAttachmentRigidChildRuntimeOwnerIdentityCount = 0;
  uint64_t contractAttachmentRigidOwnerRuntimeOwnerIdentityCount = 0;
  uint64_t contractAttachmentRigidRootRuntimeOwnerIdentityCount = 0;
  uint64_t contractAttachmentRigidOwnerRuntimeRecordKnownCount = 0;
  uint64_t contractAttachmentRigidRootRuntimeRecordKnownCount = 0;
  uint64_t contractAttachmentRigidChildRuntimeParentLinkKnownCount = 0;
  uint64_t contractAttachmentRigidChildRuntimeRecordKnownCount = 0;
  uint64_t contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0;
  uint64_t contractAttachmentRigidChildRuntimePoseKnownCount = 0;
  uint64_t contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount = 0;
  uint64_t contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount = 0;
  uint64_t upperLayerResolveAttempts = 0;
  uint64_t upperLayerResolveVisibleMiss = 0;
  uint64_t upperLayerResolveVisibleUnresolvedGeoset = 0;
  uint64_t upperLayerResolveGeosetMiss = 0;
  uint64_t upperLayerResolvePoseMiss = 0;
  uint64_t upperLayerResolveRuntimeGroupPaletteMiss = 0;
  uint64_t upperLayerResolveAuthoritativeRigid = 0;
  uint64_t upperLayerResolveAuthoritativeSkinned = 0;
  uint64_t upperLayerResolvedAuthoritativeItems = 0;
  uint64_t upperLayerEmitted = 0;
  uint64_t upperLayerDuplicateOrSuppressed = 0;
  uint64_t semanticDataModuleEnabled = 0;
  uint64_t semanticModelProducerEnabled = 0;
  uint64_t semanticPoseProducerEnabled = 0;
  uint64_t semanticAttachmentProducerEnabled = 0;
  uint64_t semanticFrameRegistriesEnabled = 0;
  uint64_t semanticContractCaptureEnabled = 0;
  uint64_t semanticConsumerEnabled = 0;
  uint64_t semanticBuildSkippedReason = 0;
  uint64_t semanticModelHookCalls = 0;
  uint64_t semanticModelHookUs = 0;
  uint64_t semanticPoseHookCalls = 0;
  uint64_t semanticPoseHookUs = 0;
  uint64_t semanticAttachmentHookCalls = 0;
  uint64_t semanticAttachmentHookUs = 0;
  uint64_t semanticFrameRegistryPublishCalls = 0;
  uint64_t semanticFrameRegistryPublishUs = 0;
  uint64_t semanticContractCaptureCalls = 0;
  uint64_t semanticContractCaptureUs = 0;
  uint64_t semanticVisibleDirectUnitCandidateAccepted = 0;
  uint64_t semanticVisibleDirectUnitRejectedNotUnitLike = 0;
  uint64_t semanticVisibleDirectUnitRejectedGroup = 0;
  uint64_t semanticVisibleDirectUnitRejectedNoUnitPtr = 0;
  uint64_t semanticVisibleDirectUnitRejectedNoIdentity = 0;
  uint64_t semanticVisibleDirectUnitRejectedNoMesh = 0;
  uint64_t semanticVisibleDirectUnitRejectedBuilding = 0;
  uint64_t semanticVisibleDirectUnitRejectedNoGeoset = 0;
  uint64_t semanticContractCaptureSkippedStableSameFrame = 0;
  uint64_t semanticContractCaptureSkippedEmpty = 0;
  uint64_t semanticContractCaptureSkippedDuplicateSameFrame = 0;
  // Phase 7.98 诊断：widget identity hook 实时计数（mini probe，无人值守用）。
  uint64_t widgetIdentityEnterCount = 0;
  uint64_t widgetIdentityMagicMatchedCount = 0;
  uint64_t widgetIdentityMagicMismatchCount = 0;
  uint64_t widgetIdentityCacheInsertCount = 0;
  uint64_t widgetIdentityCacheUpdateCount = 0;
  uint64_t widgetIdentityHandleResolvedCount = 0;
  uint64_t widgetIdentityHandleMissingCount = 0;
  uint64_t widgetIdentityCacheSize = 0;
  uint64_t widgetIdentityInstallAttempted = 0;
  uint64_t widgetIdentityInstallSucceeded = 0;
  uint64_t widgetIdentityInstallFailedAddrNull = 0;
  uint64_t widgetIdentityInstallFailedEnvDisabled = 0;
  uint64_t widgetIdentityInstallFailedMinHook = 0;
  // Phase 7.99：path blocker 拦截分桶（让 trace 直接看到 path blocker
  // 在哪条出口被挡住）。
  uint64_t semanticSceneRejectedPathBlockerCount = 0;
  uint64_t semanticSceneRejectedPathBlockerEarlyBypassCount = 0;
  uint64_t semanticSceneRejectedPathBlockerEligibilityGateCount = 0;
  uint64_t semanticSceneRejectedPathBlockerAppendEntryCount = 0;
  uint64_t semanticSceneRejectedPathBlockerAppendEntryByJHandleCount = 0;
  uint64_t semanticSceneRejectedPathBlockerAppendVbBlendCount = 0;
  uint64_t semanticSceneRejectedPathBlockerFastAppendCount = 0;
  uint64_t semanticSceneRejectedPathBlockerDirectGroupedCount = 0;
  uint64_t semanticSceneRejectedPathBlockerProducerCount = 0;
  uint64_t semanticSceneRejectedPathBlockerStaticSupplementCount = 0;
  uint64_t semanticSceneRejectedPathBlockerLegacyCaptureCount = 0;
  // Phase 7.100：建筑/装饰物静态阴影 mask 写入拦截统计。
  uint64_t writeMaskRegionEnterCount = 0;
  uint64_t writeMaskRegionRejectedIdx3Count = 0;
  uint64_t writeMaskRegionPassFogCount = 0;
  uint64_t writeMaskRegionPassLosCount = 0;
  uint64_t writeMaskRegionPassPathCount = 0;
  uint64_t writeMaskRegionPassOtherCount = 0;

  // Phase 7.112：caller-aware 静态阴影屏蔽诊断。
  uint64_t writeMaskRegionFromBuildingStampCount = 0;
  uint64_t writeMaskRegionRejectedBuildingCount = 0;
  uint64_t writeMaskRegionFromRegisterFootprintCount = 0;
  uint64_t writeMaskRegionFromRebuildMaskCount = 0;
  uint64_t writeMaskRegionFromActorRuntimeCount = 0;
  uint64_t writeMaskRegionFromForObjectCount = 0;
  uint64_t writeMaskRegionFromOtherCallerCount = 0;

  // Phase 7.116：DispatchToShape (建筑/装饰物/可破坏物原生静态阴影) 屏蔽诊断。
  uint64_t dispatchToShapeEnterCount = 0;
  uint64_t dispatchToShapeRejectedCount = 0;
  uint64_t dispatchToShapeFromRebuildMaskCount = 0;
  uint64_t dispatchToShapeFromShadowSetupCount = 0;
  uint64_t dispatchToShapeFromOtherCallerCount = 0;

  // Phase 7.108：ShadowProjector 永久 atomic 计数（独立于 D3D9 mesh draw 路径）。
  uint64_t projectorAddFromObjectEnterCount = 0;
  uint64_t projectorAddFromObjectBlockedCount = 0;
  uint64_t projectorAddFromObjectFourCCExtractedCount = 0;
  uint64_t projectorAddFromObjectFourCCMissCount = 0;
  uint64_t projectorAddFromObjectBlockedFourCCCount = 0;
  uint64_t projectorAddSimpleEnterCount = 0;
  uint64_t projectorAddSimpleBlockedCount = 0;
  // 前 8 个 unique observed / blocked fourcc。
  uint32_t projectorObservedFourCCSamples[8] = {};
  uint32_t projectorBlockedFourCCSamples[8] = {};

  // Phase 7.108b：Shadow caster append survey — 实际进入 shadowCasters 的 rawcode 分布。
  // 用于直接验证：是否有 path blocker 的 fourcc 还能漏到 caster 集合里（绕开
  // 9 个拦截点）。如果用户实机仍看到阴影但这里没有 path blocker fourcc，
  // 那 path blocker 阴影就**不来自我们的 CSM caster 集合**。
  uint32_t shadowAppendRawcodeSamples[16] = {};
  uint64_t shadowAppendRawcodeUniqueCount = 0;
  uint64_t shadowAppendTotalCount = 0;
  // Phase 7.97 诊断：ManifestCopy 路径实际处理了多少 visible record
  // 与最终 push 进 manifest 的数量。bridge 透传到 control plane，配合
  // perf 报告里的 ManifestCopy avgCpuMs，能量化"per-record 成本 = 总 ms / scan 数"。
  uint64_t semanticManifestCopyVisibleScanned = 0;
  uint64_t semanticManifestCopyAppended = 0;
  uint64_t semanticManifestCopyDeduplicatedSkipped = 0;
  uint64_t semanticManifestCopyRejectedSkipped = 0;
  // sameFrameDataNotGrowing 早退累计（atomic，实时）。
  uint64_t semanticManifestCopySkipStableCount = 0;
  uint64_t semanticManifestCopyEnterCount = 0;
  uint64_t semanticManifestCopyMaxScanned = 0;
  uint64_t semanticManifestCopyTotalScanned = 0;
  uint64_t semanticManifestCopyTotalChronoNs = 0;
  uint64_t semanticManifestCopyMaxChronoNs = 0;
  uint64_t semanticManifestResolveSourceCompleteSkipCount = 0;
  uint64_t semanticManifestResolveLegacyCacheHitCount = 0;
  uint64_t semanticManifestResolveRawScanCount = 0;
  uint64_t semanticManifestResolveRawScanEntryVisitCount = 0;
  uint64_t semanticManifestResolveRawScanMissCount = 0;
  uint64_t semanticManifestResolveVerifierAttemptCount = 0;
  uint64_t semanticManifestResolveVerifierMismatchCount = 0;
  uint64_t semanticManifestResolveMaxRuntimeGeosetCount = 0;
  uint64_t semanticManifestModelResourceAttemptCount = 0;
  uint64_t semanticManifestModelResourceCacheHitCount = 0;
  uint64_t semanticManifestModelResourceDeepResolveCount = 0;
  uint64_t semanticManifestModelResourceNullResultCount = 0;
  uint64_t semanticManifestModelResourceVerifierAttemptCount = 0;
  uint64_t semanticManifestModelResourceVerifierMismatchCount = 0;
  uint64_t semanticConsumerBuildCalls = 0;
  uint64_t semanticConsumerBuildUs = 0;
  uint64_t semanticConsumerBuildSkippedFresh = 0;
  uint64_t semanticSummaryRefreshRequestCalls = 0;
  uint64_t semanticSummaryRefreshRequestUs = 0;
  uint64_t shadowMetadataCaptureFrameCalls = 0;
  uint64_t shadowMetadataCaptureUs = 0;
  uint64_t semanticLastHotFunctionTag = 0;
  uint64_t semanticLastHotFunctionUs = 0;
  uint64_t semanticModelBuildChildPreScanCalls = 0;
  uint64_t semanticModelBuildChildPreScanUs = 0;
  uint64_t semanticModelRuntimeChildCollectCalls = 0;
  uint64_t semanticModelRuntimeChildCollectUs = 0;
  uint64_t semanticModelRuntimeChildBootstrapCalls = 0;
  uint64_t semanticModelRuntimeChildBootstrapUs = 0;
  uint64_t semanticModelRuntimeChildParentMapCalls = 0;
  uint64_t semanticModelRuntimeChildParentMapUs = 0;
  uint64_t semanticModelRuntimeChildOwnerPropagateCalls = 0;
  uint64_t semanticModelRuntimeChildOwnerPropagateUs = 0;
  uint64_t semanticModelPromoteRuntimeCalls = 0;
  uint64_t semanticModelPromoteRuntimeUs = 0;
  uint64_t semanticModelSpriteHostBindCalls = 0;
  uint64_t semanticModelSpriteHostBindUs = 0;
  uint64_t semanticModelRuntimeModelBindingCalls = 0;
  uint64_t semanticModelRuntimeModelBindingUs = 0;
  uint64_t semanticModelGeosetResourceCalls = 0;
  uint64_t semanticModelGeosetResourceUs = 0;
  uint64_t semanticModelRuntimeCtorCalls = 0;
  uint64_t semanticModelRuntimeCtorUs = 0;
  uint64_t semanticModelRuntimeResolveCalls = 0;
  uint64_t semanticModelRuntimeResolveUs = 0;
  uint64_t semanticModelRuntimeInitCopyCalls = 0;
  uint64_t semanticModelRuntimeInitCopyUs = 0;
  uint64_t semanticPoseRuntimePoseCalls = 0;
  uint64_t semanticPoseRuntimePoseUs = 0;
  uint64_t semanticPoseRuntimePaletteTreeCalls = 0;
  uint64_t semanticPoseRuntimePaletteTreeUs = 0;
  uint64_t semanticPoseRuntimeMatrixPaletteCalls = 0;
  uint64_t semanticPoseRuntimeMatrixPaletteUs = 0;
  uint64_t semanticPoseSpriteFrameSourceIdentityCalls = 0;
  uint64_t semanticPoseSpriteFrameSourceIdentityUs = 0;
  uint64_t semanticPoseSpriteFramePoseCalls = 0;
  uint64_t semanticPoseSpriteFramePoseUs = 0;
  uint64_t semanticPoseRuntimeMatrixPublisherCalls = 0;
  uint64_t semanticPoseRuntimeMatrixPublisherUs = 0;
  uint64_t semanticPoseSpriteAttachmentHitCalls = 0;
  uint64_t semanticPoseSpriteAttachmentHitUs = 0;
  uint64_t semanticPoseSpriteTransformReadCalls = 0;
  uint64_t semanticPoseSpriteTransformReadUs = 0;
  uint64_t semanticPoseSpriteIdentityLookupCalls = 0;
  uint64_t semanticPoseSpriteIdentityLookupUs = 0;
  uint64_t semanticPoseSpriteBaseAliasCalls = 0;
  uint64_t semanticPoseSpriteBaseAliasUs = 0;
  uint64_t semanticPoseSpritePublishPoseCalls = 0;
  uint64_t semanticPoseSpritePublishPoseUs = 0;
  uint64_t semanticPoseSpritePaletteGateCalls = 0;
  uint64_t semanticPoseSpritePaletteGateUs = 0;
  uint64_t semanticAttachmentAttachedEffectInitCalls = 0;
  uint64_t semanticAttachmentAttachedEffectInitUs = 0;
  uint64_t semanticAttachmentAttachedEffectDirectCalls = 0;
  uint64_t semanticAttachmentAttachedEffectDirectUs = 0;
  uint64_t semanticAttachmentAttachModelToPointCalls = 0;
  uint64_t semanticAttachmentAttachModelToPointUs = 0;
  uint64_t semanticAttachmentOverrideSharedPresetCalls = 0;
  uint64_t semanticAttachmentOverrideSharedPresetUs = 0;
  uint64_t semanticAttachmentOverrideLocalPointCalls = 0;
  uint64_t semanticAttachmentOverrideLocalPointUs = 0;
  uint64_t semanticAttachmentOverridePrimaryPresetCalls = 0;
  uint64_t semanticAttachmentOverridePrimaryPresetUs = 0;
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
  uint64_t semanticSceneLivePaletteRefreshAttemptCount = 0;
  uint64_t semanticSceneLivePaletteRefreshHitCount = 0;
  uint64_t semanticSceneLivePaletteRefreshMissCount = 0;
  uint64_t semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount = 0;
  uint64_t semanticScenePaletteOverrideNoComposeCount = 0;
  uint64_t semanticScenePaletteOverrideWouldComposeCount = 0;
  uint64_t semanticScenePalettePacketWorldComposeCount = 0;
  uint64_t semanticSceneLivePaletteRefreshLastRuntimeModelPtr = 0;
  uint64_t semanticSceneLivePaletteRefreshLastMatrixCount = 0;
  uint64_t semanticSceneLivePaletteRefreshLastMatrixHash = 0;
  uint64_t semanticSceneLivePaletteMotionSampleCount = 0;
  uint64_t semanticSceneLivePaletteMotionNewRuntimeCount = 0;
  uint64_t semanticSceneLivePaletteMotionRawChangedCount = 0;
  uint64_t semanticSceneLivePaletteMotionRawStableCount = 0;
  uint64_t semanticSceneLivePaletteMotionGroupChangedCount = 0;
  uint64_t semanticSceneLivePaletteMotionGroupStableCount = 0;
  uint64_t semanticSceneLivePaletteMotionLastRuntimeModelPtr = 0;
  uint64_t semanticSceneLivePaletteMotionLastPrevRawHash = 0;
  uint64_t semanticSceneLivePaletteMotionLastRawHash = 0;
  uint64_t semanticSceneLivePaletteMotionLastPrevGroupHash = 0;
  uint64_t semanticSceneLivePaletteMotionLastGroupHash = 0;
  uint64_t semanticSceneDrawTimePoseAttemptCount = 0;
  uint64_t semanticSceneDrawTimePosePublishedCount = 0;
  uint64_t semanticSceneDrawTimePoseChangedCount = 0;
  uint64_t semanticSceneDrawTimePoseStableCount = 0;
  uint64_t semanticSceneDrawTimePoseLastRuntimeModelPtr = 0;
  uint64_t semanticSceneDrawTimePoseLastPrevHash = 0;
  uint64_t semanticSceneDrawTimePoseLastHash = 0;
  uint64_t semanticSceneSubmittedPaletteMotionSampleCount = 0;
  uint64_t semanticSceneSubmittedPaletteMotionNewRuntimeCount = 0;
  uint64_t semanticSceneSubmittedPaletteMotionChangedCount = 0;
  uint64_t semanticSceneSubmittedPaletteMotionStableCount = 0;
  uint64_t semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr = 0;
  uint64_t semanticSceneSubmittedPaletteMotionLastPrevHash = 0;
  uint64_t semanticSceneSubmittedPaletteMotionLastHash = 0;
  uint64_t semanticSceneSkinnedDynamicIndexSliceCount = 0;
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
  uint64_t semanticSceneSkinnedFullIndexFallbackCount = 0;
  uint64_t semanticSceneSkinnedMissingVisibleIndexSliceRejectCount = 0;
  uint64_t semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr = 0;
  uint64_t semanticSceneSkinnedFullIndexFallbackLastIndexCount = 0;
  uint64_t semanticSceneSubmittedFrameLocal = 0;
  uint64_t semanticSceneSubmittedPersistent = 0;
  uint64_t semanticSceneStatsPublishCount = 0;
  uint64_t semanticSceneInputDrawCount = 0;
  uint64_t semanticSceneInputSkinnedCount = 0;
  uint64_t semanticSceneTailBoundaryCandidateCount = 0;
  uint64_t semanticSceneTailBoundaryCommitCount = 0;
  uint64_t semanticScenePopulateAttemptCount = 0;
  uint64_t semanticScenePopulateUnitsOnlyCount = 0;
  uint64_t semanticScenePopulateLastReturnReason = 0;
  uint64_t semanticScenePopulateLastProducerPublishAttemptDelta = 0;
  uint64_t semanticScenePopulateLastProducerPublishReadyDelta = 0;
  uint64_t semanticScenePopulateLastProducerQueryAttemptDelta = 0;
  uint64_t semanticScenePopulateLastProducerQueryHitDelta = 0;
  uint64_t semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta = 0;
  uint64_t semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta = 0;
  uint64_t semanticScenePopulateLastProducerGroupDecodeAttemptDelta = 0;
  uint64_t semanticScenePopulateLastProducerGroupDecodeHitDelta = 0;
  uint64_t semanticSceneDirectCurrentDrawRecordCount = 0;
  uint64_t semanticSceneDirectCurrentDrawBuiltPacketCount = 0;
  uint64_t semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount = 0;
  uint64_t semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount = 0;
  uint64_t semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount = 0;
  uint64_t semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount = 0;
  // === Phase 7.1: caster selection stability diagnostics ===
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
  uint64_t drawTimeSemanticProducerOwnedDirectGroupedSkipCount = 0;
  uint64_t semanticSceneDirectLastSubmittedRecordCount = 0;
  uint64_t semanticSceneDirectLastUniqueObjectCount = 0;
  uint64_t semanticSceneDirectLastSubmittedObjectCount = 0;
  uint64_t semanticSceneDirectLastRecordCapPartialObjectCount = 0;
  uint64_t semanticSceneDirectLastScanCapPartialObjectCount = 0;
  uint64_t semanticSceneDirectLastMinGeosetsPerObject = 0;
  uint64_t semanticSceneDirectLastMaxGeosetsPerObject = 0;
  uint64_t semanticSceneDirectLastSubmittedIdentityHash = 0;
  uint64_t semanticSceneDirectRecordCapHitCount = 0;
  uint64_t semanticSceneDirectRecordCapTruncatedRecordCount = 0;
  uint64_t semanticSceneDirectScanCapHitCount = 0;
  uint64_t semanticSceneDirectIdentityChurnCount = 0;
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
  uint64_t semanticSceneShadowManifestPartLeasePaletteRefreshAttemptCount = 0;
  uint64_t semanticSceneShadowManifestPartLeasePaletteRefreshHitCount = 0;
  uint64_t semanticSceneShadowManifestPartLeasePaletteRefreshMissCount = 0;
  uint64_t semanticSceneShadowManifestPartLeasePaletteRefreshAppliedCount = 0;
  uint64_t semanticSceneShadowManifestPartLeasePaletteRefreshFallbackCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreCompleteCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreIncompleteSkipCount = 0;
  uint64_t semanticSceneShadowManifestPartOmittedIncompleteCoreCount = 0;
  // Phase 7.25 core epoch planner 专属计数器（跨层导出）。
  uint64_t semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreEpochMissingPartCount = 0;
  uint64_t semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount = 0;
  uint64_t semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount = 0;
  uint64_t semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount = 0;
  uint64_t semanticSceneShadowManifestLeaseExpiredBackingOnlyCount = 0;
  uint64_t semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount = 0;
  uint64_t semanticSceneShadowManifestMissingRequiredPartCount = 0;
  uint64_t semanticSceneShadowManifestGraceUsedCount = 0;
  uint64_t semanticSceneShadowManifestTombstoneRetiredCount = 0;
  // Phase 7.28：skinned palette content stability probe（跨层导出）。
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
  uint64_t semanticSceneDirectStickyPartSelectionRetainedCount = 0;
  uint64_t semanticSceneDirectStickyPartSelectionDroppedCount = 0;
  uint64_t semanticSceneDirectStickyPartSelectionFallbackCount = 0;
  // === Phase 7.5: object completeness diagnostics ===
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
  // === Phase 7.2: single-caster flicker diagnostics ===
  uint64_t semanticSceneDirectLastSubmittedSceneNode = 0;
  uint64_t semanticSceneDirectLastSubmittedPaletteHash = 0;
  uint64_t semanticSceneDirectLastSubmittedGroupHash = 0;
  uint64_t semanticSceneDirectLastSubmittedStableGroupHash = 0;
  uint64_t semanticSceneDirectLastSubmittedStream1Ptr = 0;
  uint64_t semanticSceneDirectLastSubmittedGeometrySourceHash = 0;
  uint64_t semanticSceneDirectLastSubmittedRenderablePart = 0;
  uint64_t semanticSceneDirectLastSubmittedMeshData = 0;
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
  uint64_t semanticSceneLastAppendedGeometrySourceHash = 0;
  uint64_t semanticSceneLastAppendedGeometryId = 0;
  uint64_t semanticSceneLastAppendedRenderablePart = 0;
  uint64_t semanticSceneLastAppendedMeshData = 0;
  // Stage1 terrain/bridge world-transform diagnostics. Bridges and ramps are
  // emitted by the native renderer in the Stage1 terrain family rather than
  // the Stage10 decoration family, so these gauges must travel with the
  // published frame instead of being inferred from the later replay count.
  uint64_t terrainS1CaptureAttemptCount = 0;
  uint64_t terrainS1CaptureAcceptedCount = 0;
  uint64_t terrainS1WorldIdentityLikeCount = 0;
  uint64_t terrainS1WorldNonIdentityCount = 0;
  uint64_t terrainS1WorldNonFiniteCount = 0;
  uint64_t terrainS1ForceIdentityWorldCount = 0;
  uint64_t terrainS1WorldMatrixHash = 0;
  uint64_t terrainS1WorldTranslationMilliMax = 0;
  std::array<uint64_t, kWar3ShadowStageHistogramBinCount>
      shadowCasterStageHistogram = {};
  std::array<uint64_t, kWar3ShadowCategoryHistogramBinCount>
      shadowCasterCategoryHistogram = {};
  uint64_t stage13CaptureAttemptCount = 0;
  uint64_t stage13CaptureRejectedNoDemandCount = 0;
  uint64_t stage13CaptureRejectedAfterBeforeUiCount = 0;
  uint64_t stage13CaptureConsideredCount = 0;
  uint64_t beforeUiStage13BoundaryCandidateCount = 0;
  uint64_t beforeUiStage13BoundaryCommitCount = 0;
  uint64_t stage13RetentionBaseEligibleCount = 0;
  uint64_t stage13SourcePositionInvalidCount = 0;
  uint64_t stage13SourceIndexInvalidCount = 0;
  uint64_t stage13SourceIdentityValidCount = 0;
  uint64_t stage13SourceIdentityHitCount = 0;
  uint64_t stage13SourceIdentityMissCount = 0;
  uint64_t stage13StrongScanCount = 0;
  uint64_t stage13SnapshotBuildCount = 0;
  uint64_t stage13SnapshotContentRekeyCount = 0;
  uint64_t stage13FreezeCopyBytes = 0;
  uint64_t stage13CpuSnapshotCopyBytes = 0;
  uint64_t stage13RetentionSnapshotBytes = 0;
  uint64_t stage13RetainedEntryCountMax = 0;
  uint64_t stage13RetainedContentMatchCount = 0;
  uint64_t stage13RetainedIdentityMatchCount = 0;
  uint64_t stage13RetainedWorldMatchCount = 0;
  uint64_t stage13RetainedMaterialMatchCount = 0;
  uint64_t stage13RetainedLayoutMatchCount = 0;
  uint64_t stage13RetainedAllSemanticMatchCount = 0;
  // --- submitted/replay/executed reconciliation ---
  uint64_t semanticSceneShadowCastersCount = 0;
  uint64_t semanticSceneReplayDrawsCount = 0;
  uint64_t semanticSceneShadowMapDrawnCasters = 0;
  uint64_t semanticSceneShadowMapCascadeCulledCount = 0;
  uint64_t producerSealFrameSerial = 0;
  uint64_t producerSealMapEpoch = 0;
  uint64_t producerSealDeviceEpoch = 0;
  uint64_t producerRequiredCasterOmissionCount = 0;
  uint64_t producerExactBudgetDeferredUniqueCasterCount = 0;
  uint64_t producerPositionAllocBudgetCount = 0;
  uint64_t producerUvAllocBudgetCount = 0;
  uint64_t producerIndexAllocBudgetCount = 0;
  uint64_t producerAllocationFailureCount = 0;
  uint64_t producerFallbackByteBudgetCount = 0;
  uint64_t producerArenaAdmissionCount = 0;
  uint64_t producerFreezeFailureCount = 0;
  uint64_t producerSoftPriorityBudgetCount = 0;
  uint64_t producerCompletenessReasonMask = 0;
  uint64_t producerCompletenessSealed = 0;
  uint64_t producerCompletenessCounterOverflow = 0;
  uint64_t drawTimeVBCacheStaticLiveBytes = 0;
  uint64_t drawTimeVBCacheStaticProtectedBytes = 0;
  uint64_t drawTimeVBCacheStaticOverCapBytes = 0;
  uint64_t drawTimeVBCacheStaticOverCapFrameCount = 0;
  uint64_t drawTimeVBCacheStaticEvictedBytes = 0;
  uint64_t drawTimeVBCacheStaticEvictedEntryCount = 0;
  uint64_t drawTimeSnapshotPageResidentBytes = 0;
  uint64_t drawTimeSnapshotPageUsedBytes = 0;
  uint64_t drawTimeSnapshotPageCreateCount = 0;
  uint64_t drawTimeSnapshotSuballocationCount = 0;
  uint64_t drawTimeSnapshotSuballocationBytes = 0;
  uint64_t drawTimeSnapshotPageReclaimedCount = 0;
  uint64_t drawTimeSnapshotPageCapacityRejectCount = 0;
  uint64_t drawTimeSnapshotPageAllocationFailureCount = 0;
  uint64_t drawTimeVBCacheIndexedUnknownRangeFallbackCount = 0;
  uint64_t drawTimeGenerationBackedPositionReuseCount = 0;
  uint64_t drawTimeGenerationBackedUvReuseCount = 0;
  uint64_t drawTimeGenerationBackedIndexReuseCount = 0;
  uint64_t drawTimeGenerationBackedCopyBytesSaved = 0;
  uint64_t semanticSceneTerrainBoundsCullMode = 0;
  uint64_t semanticSceneTerrainBoundsCandidateCount = 0;
  uint64_t semanticSceneTerrainBoundsProofAcceptedCount = 0;
  uint64_t semanticSceneTerrainBoundsFailVisibleCount = 0;
  uint64_t semanticSceneTerrainBoundsWouldCullCount = 0;
  uint64_t semanticSceneTerrainBoundsAppliedCullCount = 0;
  uint64_t semanticSceneTerrainBoundsC0WouldCullCount = 0;
  uint64_t semanticSceneTerrainBoundsC1WouldCullCount = 0;
  uint64_t semanticSceneTerrainBoundsC2WouldCullCount = 0;
  uint64_t semanticSceneTerrainBoundsC3WouldCullCount = 0;
  uint64_t semanticSceneObjectBoundsCandidateCount = 0;
  uint64_t semanticSceneObjectBoundsProofAcceptedCount = 0;
  uint64_t semanticSceneObjectBoundsFailVisibleCount = 0;
  uint64_t semanticSceneObjectBoundsWouldCullCount = 0;
  uint64_t semanticSceneObjectBoundsAppliedCullCount = 0;
  uint64_t semanticSceneShadowMapSkinnedCasterCount = 0;
  uint64_t semanticSceneShadowMapSkinnedPreparedCount = 0;
  uint64_t semanticSceneShadowMapSkinnedInvalidBufferCount = 0;
  uint64_t semanticSceneShadowMapSkinnedInvalidPipelineCount = 0;
  uint64_t semanticSceneShadowMapSkinnedDrawnCount = 0;
  uint64_t gpuSkinVsShadowDirectAttempts = 0;
  uint64_t gpuSkinVsShadowDirectInputRejects = 0;
  uint64_t gpuSkinVsShadowDirectStateRejects = 0;
  uint64_t gpuSkinVsShadowDirectDrawsSubmitted = 0;
  uint64_t gpuSkinVsShadowReplayDirectional = 0;
  uint64_t gpuSkinVsShadowReplayPoint = 0;
  uint64_t semanticSceneShadowTaaActive = 0;
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
  uint64_t dynamicPoseSignature = 0;
  uint64_t shadowCadenceSampleSerial = 0;
  uint64_t shadowCadenceSampleCountTotal = 0;
  uint64_t shadowCadenceSameDynamicPoseStreak = 0;
  uint64_t shadowCadenceSameDynamicPoseStreakMax = 0;
  uint64_t shadowCadenceSameSceneFrameStreak = 0;
  uint64_t shadowCadenceSameSceneFrameStreakMax = 0;
  uint64_t shadowCadenceShadowMapReuseStreak = 0;
  uint64_t shadowCadenceShadowMapReuseStreakMax = 0;
  uint32_t shadowCadenceSampleCount = 0;
  std::array<ShadowRuntimeCadenceSample,
             kShadowRuntimeCadenceSampleCapacity>
      shadowCadenceSamples = {};
  uint64_t semanticSceneLastInputDrawCount = 0;
  uint64_t semanticSceneLastInputSkinnedCount = 0;
  uint64_t semanticSceneLastSubmittedDrawCount = 0;
  uint64_t semanticSceneLastUnitsOnlyFilteredCount = 0;
  uint64_t semanticSceneCatchupAttemptCount = 0;
  uint64_t semanticSceneCatchupSuccessCount = 0;
  uint64_t semanticSceneSkippedEmptyFrameCount = 0;
  uint64_t semanticSceneZeroSubmitCount = 0;
  uint64_t semanticSceneSelectedFrameEligibleZeroCount = 0;
  uint64_t semanticSceneReusableFrameForcedCount = 0;
  uint64_t semanticSceneReusableFrameUnavailableCount = 0;
  uint64_t semanticSceneReusableFrameRejectedNativeValidationCount = 0;
  uint64_t semanticSceneLastFrameSerial = 0;
  uint64_t semanticSceneLastSelectedFrameSerial = 0;
  uint64_t semanticSceneLastReusableFrameSerial = 0;
  uint64_t semanticSceneLastSourcePublishRevision = 0;
  uint64_t semanticSceneLastTargetPublishRevision = 0;
  uint64_t semanticScenePublishRevisionLag = 0;
  uint64_t semanticSceneBypassUnitLikeCount = 0;
  uint64_t semanticSceneBypassUnitLikeWithRuntimeModel = 0;
  uint64_t semanticSceneBypassUnitLikeWithModelResource = 0;
  uint64_t semanticSceneBypassUnitLikeWithPose = 0;
  uint64_t semanticSceneBypassUnitLikeWithRenderable = 0;
  uint64_t semanticSceneBypassPublishedVisibleCandidate = 0;
  uint64_t semanticSceneBypassPublishMiss = 0;
  uint64_t semanticSceneSkippedUnitsOnlyFilter = 0;
  uint64_t semanticSceneAcceptedExplicitResourceOwnerRigid = 0;
  uint64_t semanticSceneRejectedGeometry = 0;
  uint64_t semanticSceneRejectedGeometryFrameLocal = 0;
  uint64_t semanticSceneRejectedGeometryPersistent = 0;
  uint64_t currentDrawContractPublishAttemptCount = 0;
  uint64_t currentDrawContractPublishReadyCount = 0;
  uint64_t currentDrawContractPublishMissNoRenderablePart = 0;
  uint64_t currentDrawContractPublishMissNoMeshPayload = 0;
  uint64_t currentDrawContractPublishMissInvalidPaletteSlot = 0;
  uint64_t currentDrawContractPublishMissInvalidPaletteCount = 0;
  uint64_t currentDrawContractPublishMissNoGlobalPalette = 0;
  uint64_t currentDrawContractPublishSkippedNonWorldContext = 0;
  uint64_t currentDrawContractPublishSkippedSmallViewport = 0;
  uint64_t currentDrawContractQueryAttemptCount = 0;
  uint64_t currentDrawContractQueryHitCount = 0;
  uint64_t currentDrawContractQueryMissNoRecord = 0;
  uint64_t currentDrawContractQueryMissFrameTagMismatch = 0;
  uint64_t currentDrawContractQueryMissCacheCollision = 0;
  uint64_t currentDrawCapturedPaletteQueryAttemptCount = 0;
  uint64_t currentDrawCapturedPaletteQueryHitCount = 0;
  uint64_t currentDrawCapturedPaletteMissNoContract = 0;
  uint64_t currentDrawCapturedPaletteMissInvalidCount = 0;
  uint64_t currentDrawCapturedPaletteMissNoSnapshot = 0;
  uint64_t currentDrawCapturedPaletteMissUnreadablePalette = 0;
  uint64_t currentDrawGroupSlotDecodeAttemptCount = 0;
  uint64_t currentDrawGroupSlotDecodeHitCount = 0;
  uint64_t currentDrawGroupSlotDecodeMissDisabledStream = 0;
  uint64_t currentDrawGroupSlotDecodeMissNoStream = 0;
  uint64_t currentDrawGroupSlotDecodeMissUnreadableStream = 0;
  uint64_t currentDrawGroupSlotDecodeMissGroupOutOfRange = 0;
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
  uint64_t currentDrawLastRenderablePart = 0;
  uint64_t currentDrawLastSceneNode = 0;
  uint64_t currentDrawLastMeshPayloadPtr = 0;
  uint64_t currentDrawLastPaletteAddress = 0;
  uint64_t currentDrawLastStream1Ptr = 0;
  uint64_t currentDrawLastCaptureSerial = 0;
  uint64_t currentDrawLastPaletteSlotIndex = 0;
  uint64_t currentDrawLastCapturedPaletteCount = 0;
  uint64_t currentDrawLastStream1Stride = 0;
  uint64_t currentDrawLastFrameTag = 0;
  uint64_t currentDrawLastVisibleFrameSerial = 0;
  uint64_t currentDrawLastRenderFrameIndex = 0;
  uint64_t currentDrawLastSmallViewportWidth = 0;
  uint64_t currentDrawLastSmallViewportHeight = 0;
  uint64_t currentDrawLastMissReason = 0;
  // Phase 7.35 Pose-lag 诊断：submit 端 palette 时间滞后分布。
  // 解读：Lag0=本帧 capture 当场提交；LagN=比当前帧旧 N 帧；
  // 占比(Lag>=1)/SampleCount 越高说明 stale palette 被沿用得越多，
  // 直接对应视觉上 "Pose 卡顿 / 前一秒冻结再追帧"。
  uint64_t submitPaletteFrameLag0Count = 0;
  uint64_t submitPaletteFrameLag1Count = 0;
  uint64_t submitPaletteFrameLag2Count = 0;
  uint64_t submitPaletteFrameLag3To5Count = 0;
  uint64_t submitPaletteFrameLag6PlusCount = 0;
  uint64_t submitPaletteFrameLagMax = 0;
  uint64_t submitPaletteFrameLagSampleCount = 0;
  // Phase 7.39：actual palette-content frameTag age, not record age.
  uint64_t submitPaletteContentAgeLag0Count = 0;
  uint64_t submitPaletteContentAgeLag1Count = 0;
  uint64_t submitPaletteContentAgeLag2Count = 0;
  uint64_t submitPaletteContentAgeLag3To5Count = 0;
  uint64_t submitPaletteContentAgeLag6PlusCount = 0;
  uint64_t submitPaletteContentAgeMax = 0;
  uint64_t submitPaletteContentAgeSampleCount = 0;
  uint64_t submitPaletteContentAgeUnknownCount = 0;
  // Phase 7.35 路径 1 诊断：QueryBlendedPaletteBySlotIndexExact 命中/miss。
  // FrameTagMismatchMiss 是路径 1 主攻方向（capture miss 的主因）。
  uint64_t paletteCaptureExactHitCount = 0;
  uint64_t paletteCaptureBestEffortHitCount = 0;
  uint64_t paletteCaptureSlotOverflowMissCount = 0;
  uint64_t paletteCaptureInvalidEntryMissCount = 0;
  uint64_t paletteCaptureFrameTagMismatchMissCount = 0;
  uint64_t paletteCaptureShortResultMissCount = 0;
  // Phase 7.35 路径 2 诊断：submit-side live palette rebuild。
  // Attempt=lag 触发尝试次数；Hit=PoseRegistry 命中拿到 fresh palette；
  // Miss=PoseRegistry 无 fresh；Applied=覆盖 packet palette 成功的次数。
  // Applied / Attempt 的占比应接近当前 PoseRegistry publish hit rate(~43%)。
  uint64_t submitLiveRebuildAttemptCount = 0;
  uint64_t submitLiveRebuildHitCount = 0;
  uint64_t submitLiveRebuildMissCount = 0;
  uint64_t submitLiveRebuildAppliedCount = 0;
  // AlphaTest payload plumbing 诊断（Claude AlphaTest lane, Phase B）。
  //
  // 消费端（semantic packet append 查表）：
  //   Attempt=本帧 cutout/alphablend packet 触发过的 payload 查找次数。
  //   Hit=查找命中且 payload.valid()。
  //   MissNoUv=命中但 UV snapshot 不完整。
  //   MissNoDiffuse=命中但缺 diffuse view。
  //   MissStageInvalid=identity key 无法构造或 payload 完全不在缓存中。
  //   Applied=payload 被真正灌进 candidate/geometry/draw 的次数。
  //   FallbackReject=payload 缺失时走 safety reject 的次数（保护 full-opaque
  //   方形卡片不回归）。
  //
  // 生产端（draw-time stash）：
  //   StashCaptured=成功 stash 一条 payload。
  //   StashSkipNoSemanticKey=draw 没有稳定身份。
  //   StashSkipNoUv=UV 流不可用。
  //   StashSkipNoDiffuse=Stage0 纹理缺失或 descriptor 解析失败。
  //   StashSkipNoUpload=freeze upload 失败。
  //   CacheEvicted=TTL/软上限淘汰数。
  //   CacheSize=当前缓存规模。
  uint64_t shadowAlphaTestPayloadAttemptCount = 0;
  uint64_t shadowAlphaTestPayloadHitCount = 0;
  uint64_t shadowAlphaTestPayloadMissNoUvCount = 0;
  uint64_t shadowAlphaTestPayloadMissNoDiffuseCount = 0;
  uint64_t shadowAlphaTestPayloadMissStageInvalidCount = 0;
  uint64_t shadowAlphaTestPayloadAppliedCount = 0;
  uint64_t shadowAlphaTestPayloadFallbackRejectCount = 0;
  uint64_t shadowAlphaTestPayloadStashCapturedCount = 0;
  uint64_t shadowAlphaTestPayloadStashSkipNoSemanticKeyCount = 0;
  uint64_t shadowAlphaTestPayloadStashSkipNoUvCount = 0;
  uint64_t shadowAlphaTestPayloadStashSkipNoDiffuseCount = 0;
  uint64_t shadowAlphaTestPayloadStashSkipNoUploadCount = 0;
  uint64_t shadowAlphaTestPayloadCacheEvictedCount = 0;
  uint64_t shadowAlphaTestPayloadCacheSizeGauge = 0;
  uint64_t shadowMetadataClassifiedCount = 0;
  uint64_t shadowMetadataCapturedCount = 0;
  uint64_t shadowMetadataAppliedCount = 0;
  uint64_t shadowMetadataRejectedFrameCount = 0;
  uint64_t shadowMetadataRejectedGenerationCount = 0;
  uint64_t shadowMetadataAmbiguousCount = 0;
  uint64_t shadowMetadataRejectedNoMaterialCount = 0;
  uint64_t shadowMetadataRejectedOpaqueCount = 0;
  uint64_t shadowMetadataRejectedNoUvCount = 0;
  uint64_t shadowMetadataRejectedNoDiffuseCount = 0;
  uint64_t shadowMetadataRejectedUploadCount = 0;
  uint64_t shadowMetadataRejectedDuplicateCount = 0;
  uint64_t shadowMetadataBlockerKnownRawcodeCount = 0;
  uint64_t shadowMetadataBlockerWidgetIdentityCount = 0;
  uint64_t shadowMetadataBlockerSmallFlatCount = 0;
  uint64_t shadowMetadataBlockerBelowGroundCount = 0;
  uint64_t shadowMetadataBlockerUnreadableCount = 0;
  uint64_t shadowMetadataBlockerFinalLeakCount = 0;
  uint64_t semanticFallbackPruned = 0;
  uint64_t semanticFallbackPrunedByHandle = 0;
  uint64_t semanticFallbackPrunedByWorldObjectEntry = 0;
  uint64_t semanticFallbackPrunedBySceneNode = 0;
  uint64_t semanticFallbackPrunedByRuntimeModel = 0;
  uint64_t persistentGeometryCount = 0;
  uint64_t duplicateGeometryInstances = 0;
  uint64_t instancedGeometryDrawsSaved = 0;
  uint64_t semanticCoreManifestFrameSerial = 0;
  uint64_t semanticCoreManifestPublishRevision = 0;
  uint64_t semanticCoreFrameSerial = 0;
  uint64_t semanticCoreSourcePublishRevision = 0;
  uint64_t semanticCoreSourceVisibleCount = 0;
  uint64_t semanticCoreSourceStableIdentityCount = 0;
  uint64_t semanticCoreSourceResolvedGeosetCount = 0;
  uint64_t semanticCoreSourceUnitCount = 0;
  uint64_t semanticCorePublishRevisionLag = 0;
  uint64_t semanticCoreFrameLag = 0;
  uint64_t semanticCoreConsidered = 0;
  uint64_t semanticCoreResolved = 0;
  uint64_t semanticCoreRigidResolved = 0;
  uint64_t semanticCoreAttachmentRigidResolved = 0;
  uint64_t semanticCoreSlowestRecordResolveUs = 0;
  uint64_t semanticCoreSlowestRecordIndex = 0;
  uint64_t semanticCoreSlowestRecordRuntimeModelPtr = 0;
  uint64_t semanticCoreSlowestRecordModelResourcePtr = 0;
  uint64_t semanticCoreSlowestRecordRuntimeGeosetPtr = 0;
  uint64_t semanticCoreSlowestRecordRuntimeGeosetDataPtr = 0;
  uint64_t semanticCoreSlowestRecordGeosetIndex = 0;
  uint64_t semanticCoreSlowestRecordObjectKind = 0;
  uint64_t semanticCoreSlowestResourceLookupUs = 0;
  uint64_t semanticCoreSlowestPoseResolveUs = 0;
  uint64_t semanticCoreSlowestPoseDirectLookupUs = 0;
  uint64_t semanticCoreSlowestPoseOwnerLookupUs = 0;
  uint64_t semanticCoreSlowestPoseSpriteLookupUs = 0;
  uint64_t semanticCoreSlowestPoseInstanceRegistryUs = 0;
  uint64_t semanticCoreSlowestPoseShadowRegistryUs = 0;
  uint64_t semanticCoreSlowestPoseRenderRegistryUs = 0;
  uint64_t semanticCoreSlowestPoseRuntimeRootsUs = 0;
  uint64_t semanticCoreSlowestPoseMeshPoseContextUs = 0;
  uint64_t semanticCoreSlowestPoseMissDiagnosticUs = 0;
  uint64_t semanticCoreSlowestLayerContractUs = 0;
  uint64_t semanticCoreSlowestRuntimeGroupPaletteUs = 0;
  uint64_t semanticCoreSlowestRuntimeGroupPaletteRescueUs = 0;
  uint64_t semanticCoreSlowestAttachmentRigidResolveUs = 0;
  uint64_t semanticCoreRigidCandidateCount = 0;
  uint64_t semanticCoreSkinnedCandidateCount = 0;
  uint64_t semanticCoreSkinnedCandidatePoseReadyCount = 0;
  uint64_t semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount = 0;
  uint64_t semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissNoSkinningData = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissNoPosePalette = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissNoVertexGroups = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissInvalidGroupTable = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissMatrixIndexOutOfRange = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissVertexGroupOutOfRange = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissFallbacksFailed = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissLastPoseCount = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissLastGroupCount = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissLastMaxVertexGroupSlot = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissLastMatrixIndexCount = 0;
  uint64_t semanticCoreRuntimeGroupPaletteMissLastMatrixIndex = 0;
  uint64_t semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext = 0;
  uint64_t semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose = 0;
  uint64_t semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot = 0;
  uint64_t semanticCoreRuntimeGroupPaletteRescueByChildRuntime = 0;
  uint64_t semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime = 0;
  uint64_t semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed = 0;
  uint64_t semanticCoreAttachmentRigidSupplementalAttachmentCount = 0;
  uint64_t semanticCoreAttachmentRigidSupplementalResourceCandidateCount = 0;
  uint64_t semanticCoreAttachmentRigidSupplementalResolvedCount = 0;
  uint64_t semanticCoreAttachmentRigidSupplementalResourceMissCount = 0;
  uint64_t semanticCoreSkinnedResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidWorldTransformResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidNoMatrixPalette = 0;
  uint64_t semanticCoreAttachmentRigidMatchByChildRuntimeModel = 0;
  uint64_t semanticCoreAttachmentRigidMatchByChildSprite = 0;
  uint64_t semanticCoreAttachmentRigidMatchByChildRuntimeGeoset = 0;
  uint64_t semanticCoreAttachmentRigidMatchByChildSpriteRuntimeGeoset = 0;
  uint64_t semanticCoreAttachmentRigidMatchByOwnerRuntimeGeoset = 0;
  uint64_t semanticCoreAttachmentRigidMatchByRootRuntimeGeoset = 0;
  uint64_t semanticCoreAttachmentRigidMatchByResourceRuntimeOwner = 0;
  uint64_t semanticCoreAttachmentRigidMatchByRenderableRuntimeRoot = 0;
  uint64_t semanticCoreAttachmentRigidMatchByWorldObjectEntry = 0;
  uint64_t semanticCoreAttachmentRigidMatchBySceneNode = 0;
  uint64_t semanticCoreAttachmentRigidMatchByUnitPtr = 0;
  uint64_t semanticCoreAttachmentRigidMatchByHandle = 0;
  uint64_t semanticCoreAttachmentRigidMatchByChildModelResource = 0;
  uint64_t semanticCoreAttachmentRigidMatchByUniqueIdentity = 0;
  uint64_t semanticCoreAttachmentRigidMatchMiss = 0;
  uint64_t lastAttachmentRigidMatchMissRuntimeModelPtr = 0;
  uint64_t lastAttachmentRigidMatchMissModelResourcePtr = 0;
  uint64_t lastAttachmentRigidMatchMissRuntimeGeosetPtr = 0;
  uint64_t lastAttachmentRigidMatchMissRuntimeGeosetDataPtr = 0;
  uint64_t lastAttachmentRigidMatchMissGeosetIndex = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerWorldObjectEntryPtr =
      0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerSceneNodePtr = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerUnitPtr = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr = 0;
  uint64_t lastAttachmentRigidMatchMissOwnerSpriteContractHit = 0;
  uint64_t lastAttachmentRigidMatchMissOwnerSpriteContractChildRuntimeModelPtr = 0;
  uint64_t lastAttachmentRigidMatchMissOwnerSpriteContractOwnerRuntimeModelPtr = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceObjectPtr = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceSpriteObjectPtr =
      0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateModelDataPtr =
      0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateHandlePtr = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateCallerRva = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerResolveCallerRva = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerModelResourcePtr = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerModelKey = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerPoseMatrixCount = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0Ptr = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceObjectPtr =
      0;
  uint64_t
      lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceSpriteObjectPtr =
          0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0PoseMatrixCount =
      0;
  uint64_t
      lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0HasWorldTransform = 0;
  uint64_t semanticCoreAttachmentRigidPoseMissNoRecord = 0;
  uint64_t semanticCoreAttachmentRigidPoseMissMissingRuntimes = 0;
  uint64_t semanticCoreAttachmentRigidPoseMissNoRootPose = 0;
  uint64_t semanticCoreAttachmentRigidPoseMissNoRootWorldTransform = 0;
  uint64_t semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromLivePose =
      0;
  uint64_t semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromShadowRegistry =
      0;
  uint64_t semanticCoreExplicitBlendAttempts = 0;
  uint64_t semanticCoreExplicitBlendAttemptWithSpanRemapTable = 0;
  uint64_t semanticCoreExplicitBlendResolved = 0;
  uint64_t semanticCoreExplicitBlendSpanRemapResolved = 0;
  uint64_t semanticCoreExplicitBlendStrideSearchMiss = 0;
  uint64_t semanticCoreExplicitBlendFinalDecodeMiss = 0;
  uint64_t semanticCoreCoreDrawPacketCount = 0;
  uint64_t semanticCoreUpperLayerResolvedItems = 0;
  uint64_t semanticCoreSupplementalUpperLayerDrawPacketCount = 0;
  uint64_t semanticCoreDrawPacketCount = 0;
  uint64_t semanticCoreSubmittedDrawCount = 0;
  uint64_t semanticCoreLastFrameSourcePublishRevision = 0;
  uint64_t semanticCoreLastFrameDrawCount = 0;
  uint64_t semanticCoreLastFrameSkinnedDrawCount = 0;
  uint64_t semanticCoreRenderableFrameSourcePublishRevision = 0;
  uint64_t semanticCoreRenderableFrameDrawCount = 0;
  uint64_t semanticCoreRenderableFrameSkinnedDrawCount = 0;
  uint64_t semanticCoreSkippedNoIdentity = 0;
  uint64_t semanticCoreSkippedNoResolvedGeoset = 0;
  uint64_t semanticCoreSkippedNoGeoset = 0;
  uint64_t semanticCoreSkippedResourceMiss = 0;
  uint64_t semanticCoreSkippedResourceNotReady = 0;
  uint64_t semanticCoreSkippedNoPose = 0;
  uint64_t semanticCoreSkippedNoPoseNoContext = 0;
  uint64_t semanticCoreSkippedNoPoseAnonymousSubpart = 0;
  uint64_t semanticCoreSkippedNoPoseLookupMiss = 0;
  uint64_t semanticCoreSkippedNoRuntimeGroupPalette = 0;
  uint64_t semanticCoreBuildDurationUs = 0;
  uint64_t semanticCoreBuildFrameSerial = 0;
  uint64_t semanticCoreBuildPublishRevision = 0;
  uint64_t semanticCorePendingFrameSerial = 0;
  uint64_t semanticCorePendingPublishRevision = 0;
  uint64_t semanticCoreBuildCurrentRecordIndex = 0;
  uint64_t semanticCoreBuildRecordCount = 0;
  uint64_t semanticCoreBuildChunkCount = 0;
  uint64_t semanticCoreStalePendingBuildClearedCount = 0;
  uint64_t semanticSceneRejectedNoVertex = 0;
  uint64_t semanticSceneRejectedSkinnedContract = 0;
  uint64_t nativeD3D9BackendFrameSerial = 0;
  uint64_t nativeD3D9BackendSourcePublishRevision = 0;
  uint64_t nativeD3D9BackendSubmittedDrawCount = 0;
  uint64_t nativeD3D9BackendSubmittedRigidDrawCount = 0;
  uint64_t nativeD3D9BackendSubmittedSkinnedDrawCount = 0;
  uint64_t nativeD3D9BackendExecutedFrameSerial = 0;
  uint64_t nativeD3D9BackendExecutedDrawCount = 0;
  uint64_t nativeD3D9BackendExecutedRigidDrawCount = 0;
  uint64_t nativeD3D9BackendExecutedSkinnedDrawCount = 0;
  uint64_t nativeD3D9BackendExecuteAttemptCount = 0;
  uint64_t nativeD3D9BackendExecuteSuccessCount = 0;
  uint64_t nativeD3D9BackendLastSuccessfulExecutedFrameSerial = 0;
  uint64_t nativeD3D9BackendLastSuccessfulExecutedDrawCount = 0;
  uint64_t nativeD3D9BackendExecuteSkippedNoDeviceCount = 0;
  uint64_t nativeD3D9BackendExecuteSkippedNoDrawsCount = 0;
  uint64_t nativeD3D9BackendLastExecuteSubmittedDrawCount = 0;
  uint64_t nativeD3D9BackendLastExecuteFailedDrawCount = 0;
  uint64_t nativeD3D9BackendLastExecuteSubmittedRigidDrawCount = 0;
  uint64_t nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount = 0;
  uint64_t nativeD3D9BackendLastExecuteExecutedRigidDrawCount = 0;
  uint64_t nativeD3D9BackendLastExecuteExecutedSkinnedDrawCount = 0;
  uint64_t nativeD3D9BackendGeometryCount = 0;
  uint64_t nativeD3D9BackendPaletteCount = 0;
  uint64_t nativeD3D9BackendMaterialCount = 0;
  uint64_t nativeD3D9BackendCanonicalDrawCount = 0;
  uint64_t nativeD3D9BackendCanonicalFrameSerial = 0;
  uint64_t nativeD3D9BackendCanonicalPublishCount = 0;
  uint64_t nativeD3D9BackendCanonicalPublishRejectNotReadyCount = 0;
  uint64_t nativeD3D9BackendCanonicalPublishRejectNoPositionsCount = 0;
  uint64_t nativeD3D9BackendGeometryRejectCount = 0;
  uint64_t nativeD3D9BackendPaletteRejectCount = 0;
  uint64_t nativeD3D9BackendMaterialRejectCount = 0;
  uint64_t nativeD3D9BackendSubmitRejectCount = 0;
  bool nativeD3D9BackendUsedCanonicalFrame = false;
  uint64_t nativeSemanticWorldStageCandidateCount = 0;
  uint64_t nativeSemanticWorldStageCandidatePrepareCount = 0;
  uint64_t nativeSemanticWorldStageCandidateRefreshCount = 0;
  uint64_t nativeSemanticWorldStageCandidateExecuteCount = 0;
  uint64_t nativeSemanticWorldStageSkippedRuntimeNotReadyCount = 0;
  uint64_t nativeSemanticWorldStageLastCandidateStage = 0;
  uint64_t nativeSemanticWorldStageLastCandidateA3 = 0;
  uint64_t nativeSemanticWorldStageLastCandidateA4 = 0;
  uint64_t nativeSemanticWorldStageLastCandidateA5 = 0;
  uint64_t nativeSemanticWorldStageLastCandidateJassReady = 0;
  uint64_t nativeSemanticWorldStageLastCandidateGameStarted = 0;
  uint64_t nativeSemanticWorldStageLastCandidateRuntimeFrame = 0;
  uint64_t nativeSemanticWorldStagePrepareAttemptCount = 0;
  uint64_t nativeSemanticWorldStagePrepareSuccessCount = 0;
  uint64_t nativeSemanticWorldStageExecuteAttemptCount = 0;
  uint64_t nativeSemanticWorldStageExecuteSuccessCount = 0;
  uint64_t nativeSemanticWorldStageLastPrepareStage = 0;
  uint64_t nativeSemanticWorldStageLastExecuteStage = 0;
  uint64_t nativeSemanticWorldStageLastPrepareFrameSerial = 0;
  uint64_t nativeSemanticWorldStageLastExecuteFrameSerial = 0;
  uint64_t nativeSemanticWorldStageLastPrepareDrawCount = 0;
  uint64_t nativeSemanticWorldStageLastExecuteDrawCount = 0;
  uint64_t sourceObjectRenderBridgeResolvedByEntryCount = 0;
  uint64_t sourceObjectRenderBridgeResolvedBySceneNodeCount = 0;
  uint64_t spriteHostBindCount = 0;
  uint64_t runtimeModelCtorCount = 0;
  uint64_t runtimeModelComplexCtorCount = 0;
  uint64_t runtimeModelPlainCtorCount = 0;
  uint64_t runtimeModelCtorCallerPromoteCount = 0;
  uint64_t runtimeModelCtorCallerOtherCount = 0;
  uint64_t runtimeModelCreateCount = 0;
  uint64_t runtimeModelResolveCount = 0;
  uint64_t runtimeModelResolveResolvedIdentityCount = 0;
  uint64_t runtimeModelCreateCallerBuildChildLinksCount = 0;
  uint64_t runtimeModelCreateCallerCreateSpriteRuntimeCount = 0;
  uint64_t runtimeModelCreateCallerOtherCount = 0;
  uint64_t runtimeModelInitCopyCount = 0;
  uint64_t runtimeModelInitCopyPublishedFallbackCount = 0;
  uint64_t spriteHostBindResolvedIdentityCount = 0;
  uint64_t attachmentChildLineageBootstrapAttemptCount = 0;
  uint64_t attachmentChildLineageBootstrapSuccessCount = 0;
  uint64_t attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount = 0;
  uint64_t attachmentChildLineageBootstrapMissNoModelDataLinksCount = 0;
  uint64_t attachmentChildLineageBootstrapMissNoUniqueChildCount = 0;
  uint64_t spriteHostBindResolvedUnitCount = 0;
  uint64_t spriteHostBindResolvedHandleCount = 0;
  uint64_t spriteHostBindResolvedRawcodeCount = 0;
  // Phase 7.105 opening-skip 诊断字段。
  uint64_t spriteHostBindOpeningSkipCount = 0;
  uint64_t runtimePaletteTreeOpeningSkipCount = 0;
  uint64_t spriteFrameSourceHintCount = 0;
  uint64_t spriteFrameSourceResolvedIdentityCount = 0;
  uint64_t spriteFrameSourceResolvedUnitCount = 0;
  uint64_t spriteFrameSourceResolvedHandleCount = 0;
  uint64_t spriteFrameSourceResolvedRawcodeCount = 0;
  uint64_t spriteFrameSourceBaseAliasPublishCount = 0;
  uint64_t spriteFrameSourceDeepIdentityResolvedCount = 0;
  uint64_t spriteFrameSourceObjectRuntimeFieldCandidateCount = 0;
  uint64_t spriteFrameSourceObjectRegistryFieldHitCount = 0;
  uint64_t spriteFramePoseBaseAliasPublishCount = 0;
  uint64_t spriteFramePoseBaseAliasMatrixPaletteCount = 0;
  uint64_t spriteFrameAttachmentRootRuntimeHitCount = 0;
  uint64_t spriteFrameAttachmentOwnerRuntimeHitCount = 0;
  uint64_t spriteFrameAttachmentChildRuntimeHitCount = 0;
  uint64_t spriteFrameAttachmentContextHintCount = 0;
  uint64_t spriteFrameAttachmentFullUpdateHitCount = 0;
  uint64_t spriteFrameAttachmentLiteUpdateHitCount = 0;
  uint64_t spriteFrameAttachmentCallerKnownCount = 0;
  uint64_t spriteFrameAttachmentCallerChangedCount = 0;
  uint64_t spriteFrameAttachmentAttachScopeHitCount = 0;
  uint64_t spriteFrameAttachmentAttachScopeOwnerHitCount = 0;
  uint64_t spriteFrameAttachmentAttachScopeParentRuntimeMatchCount = 0;
  uint64_t attachedEffectInitBindCount = 0;
  uint64_t attachedEffectInitResolvedIdentityCount = 0;
  uint64_t attachedEffectInitResolvedUnitCount = 0;
  uint64_t attachedEffectInitResolvedHandleCount = 0;
  uint64_t attachedEffectInitResolvedRawcodeCount = 0;
  uint64_t attachedEffectInitParentRuntimeOwnerPublishCount = 0;
  uint64_t attachedEffectDirectBindCount = 0;
  uint64_t attachedEffectDirectResolvedIdentityCount = 0;
  uint64_t attachedEffectDirectResolvedUnitCount = 0;
  uint64_t attachedEffectDirectResolvedHandleCount = 0;
  uint64_t attachedEffectDirectResolvedRawcodeCount = 0;
  uint64_t attachModelToPointBindCount = 0;
  uint64_t attachModelToPointResolvedIdentityCount = 0;
  uint64_t attachModelToPointResolvedUnitCount = 0;
  uint64_t attachModelToPointResolvedHandleCount = 0;
  uint64_t attachModelToPointResolvedRawcodeCount = 0;
  uint64_t attachModelToPointPromotedAttachmentChildRuntimeCount = 0;
  uint64_t attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount = 0;
  uint64_t currentRenderIdentityHintCount = 0;
  uint64_t currentRenderIdentityResolvedCount = 0;
  uint64_t sourceObjectIdentityHintResolvedCount = 0;
  uint64_t runtimeSourceObjectPublishCount = 0;
  uint64_t attachmentRigidPublishedWithSourceObjectCount = 0;
  uint64_t attachmentRigidSourceObjectFromChildRuntimeCount = 0;
  uint64_t attachmentRigidSourceObjectFromOwnerRuntimeCount = 0;
  uint64_t attachmentRigidSourceObjectFromRootRuntimeCount = 0;
  uint64_t overrideOutputSampleFrame = 0;
  uint64_t overrideOutputLastActiveFrame = 0;
  uint64_t overridePrimaryPresetWriteCount = 0;
  uint64_t overrideSharedPresetWriteCount = 0;
  uint64_t overrideLocalPointWriteCount = 0;
  uint64_t overrideLocalPointNonZeroWriteCount = 0;
  uint64_t overrideLocalPointObservedChildLinkWriteCount = 0;
  uint64_t overrideLocalPointMatchedChildLinkWriteCount = 0;
  uint64_t overrideLocalPointMatchedChildPaletteReadyWriteCount = 0;
  uint64_t overrideLocalPointMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointContextRuntimeWithChildLinksWriteCount = 0;
  uint64_t overrideLocalPointContextMatchedChildLinkWriteCount = 0;
  uint64_t overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount = 0;
  uint64_t overrideLocalPointScratchRootMatchedChildLinkWriteCount = 0;
  uint64_t overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount = 0;
  uint64_t overrideLocalPointArgBlockMatchedChildLinkWriteCount = 0;
  uint64_t overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointArgBlockIdentityHintWriteCount = 0;
  uint64_t overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount = 0;
  uint64_t overrideLocalPointArg4BlockMatchedChildLinkWriteCount = 0;
  uint64_t overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointArg4BlockIdentityHintWriteCount = 0;
  uint64_t overrideLocalPointChildSourceMetaIdentityHintWriteCount = 0;
  uint64_t overrideLocalPointSpriteBoundCandidateWriteCount = 0;
  uint64_t overrideLocalPointParentSpriteIdentityHintWriteCount = 0;
  uint64_t overrideLocalPointRootRuntimeHitWriteCount = 0;
  uint64_t overrideLocalPointRootRuntimeWithChildLinksWriteCount = 0;
  uint64_t overrideLocalPointRootRuntimeMatchedChildLinkWriteCount = 0;
  uint64_t overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount = 0;
  uint64_t overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount = 0;
  uint64_t attachmentRigidPublishedCount = 0;
  uint64_t overrideLastPrimaryPresetHash = 0;
  uint64_t overrideLastSharedPresetHash = 0;
  uint64_t overrideLastRuntimeModelPtr = 0;
  uint64_t overrideLastMatchedChildRuntimeModelPtr = 0;
  uint64_t overrideLastMatchedChildBySourceRecordRuntimeModelPtr = 0;
  uint64_t overrideLastContextRuntimeWithChildLinksPtr = 0;
  uint64_t overrideLastScratchRootPtr = 0;
  uint64_t overrideLastScratchRootRuntimeModelPtr = 0;
  uint64_t overrideLastArgBlockPtr = 0;
  uint64_t overrideLastArgBlockRuntimeModelPtr = 0;
  uint64_t overrideLastArgBlockIdentityHintPtr = 0;
  uint64_t overrideLastArg4BlockPtr = 0;
  uint64_t overrideLastArg4BlockRuntimeModelPtr = 0;
  uint64_t overrideLastArg4BlockIdentityHintPtr = 0;
  uint64_t overrideLastChildSourceMetaPtr = 0;
  uint64_t overrideLastChildSourceMetaRuntimeModelPtr = 0;
  uint64_t overrideLastSpriteBoundCandidateSpritePtr = 0;
  uint64_t overrideLastSpriteBoundCandidateRuntimeModelPtr = 0;
  uint64_t overrideLastParentSpriteIdentityHintSpritePtr = 0;
  uint64_t overrideLastParentSpriteIdentityHintRuntimeModelPtr = 0;
  uint64_t overrideLastRootRuntimeModelPtr = 0;
  uint64_t lastSourceObjectRenderBridgeSourceObjectPtr = 0;
  uint64_t lastSourceObjectRenderBridgeSceneNodePtr = 0;
  uint64_t lastSourceObjectIdentityHintSourceObjectPtr = 0;
  uint64_t lastSourceObjectIdentityHintCandidatePtr = 0;
  uint64_t lastSpriteHostSourceObjectPtr = 0;
  uint64_t lastSpriteHostSpritePtr = 0;
  uint64_t lastSpriteHostRuntimeModelPtr = 0;
  uint64_t lastSpriteHostUnitPtr = 0;
  uint64_t lastSpriteFrameSourceObjectPtr = 0;
  uint64_t lastSpriteFrameSourceRuntimeModelPtr = 0;
  uint64_t lastSpriteFrameSourceBaseRuntimeModelPtr = 0;
  uint64_t lastSpriteFrameSourceObjectVtablePtr = 0;
  uint64_t lastSpriteFrameSourceObjectSceneNodeCandidatePtr = 0;
  uint64_t lastSpriteFrameSourceObjectSpriteCandidatePtr = 0;
  uint64_t lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr = 0;
  uint64_t lastSpriteFrameSourceObjectRegistryFieldCandidatePtr = 0;
  uint64_t lastSpriteFrameSourceDeepIdentityCandidatePtr = 0;
  uint64_t lastSpriteFrameSourceWorldObjectEntryPtr = 0;
  uint64_t lastSpriteFrameSourceSceneNodePtr = 0;
  uint64_t lastSpriteFrameSourceUnitPtr = 0;
  uint64_t lastSpriteFramePoseBaseRuntimeModelPtr = 0;
  uint64_t lastSpriteFramePoseBaseMatrixCount = 0;
  uint64_t lastSpriteFrameAttachmentSpritePtr = 0;
  uint64_t lastSpriteFrameAttachmentRuntimeModelPtr = 0;
  uint64_t lastSpriteFrameAttachmentContextPtr = 0;
  uint64_t lastAttachedEffectInitOwnerWidgetPtr = 0;
  uint64_t lastAttachedEffectInitChildSpritePtr = 0;
  uint64_t lastAttachedEffectInitChildRuntimeModelPtr = 0;
  uint64_t lastAttachedEffectInitParentRuntimeModelPtr = 0;
  uint64_t lastAttachedEffectInitUnitPtr = 0;
  uint64_t lastAttachedEffectDirectOwnerWidgetPtr = 0;
  uint64_t lastAttachedEffectDirectChildSpritePtr = 0;
  uint64_t lastAttachedEffectDirectChildRuntimeModelPtr = 0;
  uint64_t lastAttachedEffectDirectUnitPtr = 0;
  uint64_t attachmentRigidSample0RootRuntimeModelPtr = 0;
  uint64_t attachmentRigidSample0OwnerRuntimeModelPtr = 0;
  uint64_t attachmentRigidSample0ChildRuntimeModelPtr = 0;
  uint64_t attachmentRigidSample0ChildSpritePtr = 0;
  uint64_t attachmentRigidSample0SourceObjectPtr = 0;
  uint64_t attachmentRigidSample0RootRuntimeCreateHandlePtr = 0;
  uint64_t attachmentRigidSample0OwnerRuntimeCreateHandlePtr = 0;
  uint64_t attachmentRigidSample0ChildRuntimeCreateHandlePtr = 0;
  uint64_t attachmentRigidSample0RootRuntimeCreateCallerRva = 0;
  uint64_t attachmentRigidSample0OwnerRuntimeCreateCallerRva = 0;
  uint64_t attachmentRigidSample0ChildRuntimeCreateCallerRva = 0;
  uint64_t attachmentRigidSample0RootRuntimeResolveCallerRva = 0;
  uint64_t attachmentRigidSample0OwnerRuntimeResolveCallerRva = 0;
  uint64_t attachmentRigidSample0ChildRuntimeResolveCallerRva = 0;
  uint64_t attachmentRigidSample0RootRuntimeCreateModelDataPtr = 0;
  uint64_t attachmentRigidSample0OwnerRuntimeCreateModelDataPtr = 0;
  uint64_t attachmentRigidSample0RootRuntimeSourceObjectPtr = 0;
  uint64_t attachmentRigidSample0OwnerRuntimeSourceObjectPtr = 0;
  uint64_t attachmentRigidSample0RootRuntimeSourceSpriteObjectPtr = 0;
  uint64_t attachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr = 0;
  uint64_t attachmentRigidSample0ChildRuntimeParentRuntimeModelPtr = 0;
  uint64_t attachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame = 0;
  uint64_t attachmentRigidSample0ChildRuntimeParentLinkSourceMeta = 0;
  uint64_t attachmentRigidSample0ChildRuntimeCreateModelDataPtr = 0;
  uint64_t attachmentRigidSample0ChildRuntimeSourceObjectPtr = 0;
  uint64_t attachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr = 0;
  uint64_t attachmentRigidSample0ChildRuntimeModelResourcePtr = 0;
  uint64_t attachmentRigidSample0ChildRuntimeModelKey = 0;
  uint64_t attachmentRigidSample0ChildRuntimePoseMatrixCount = 0;
  uint64_t attachmentRigidSample0FirstSeenFrame = 0;
  uint64_t attachmentRigidSample0LastSeenFrame = 0;
  uint64_t attachmentRigidSample1RootRuntimeModelPtr = 0;
  uint64_t attachmentRigidSample1OwnerRuntimeModelPtr = 0;
  uint64_t attachmentRigidSample1ChildRuntimeModelPtr = 0;
  uint64_t attachmentRigidSample1ChildSpritePtr = 0;
  uint64_t attachmentRigidSample1SourceObjectPtr = 0;
  uint64_t attachmentRigidSample1RootRuntimeCreateHandlePtr = 0;
  uint64_t attachmentRigidSample1OwnerRuntimeCreateHandlePtr = 0;
  uint64_t attachmentRigidSample1ChildRuntimeCreateHandlePtr = 0;
  uint64_t attachmentRigidSample1RootRuntimeCreateCallerRva = 0;
  uint64_t attachmentRigidSample1OwnerRuntimeCreateCallerRva = 0;
  uint64_t attachmentRigidSample1ChildRuntimeCreateCallerRva = 0;
  uint64_t attachmentRigidSample1RootRuntimeResolveCallerRva = 0;
  uint64_t attachmentRigidSample1OwnerRuntimeResolveCallerRva = 0;
  uint64_t attachmentRigidSample1ChildRuntimeResolveCallerRva = 0;
  uint64_t attachmentRigidSample1RootRuntimeCreateModelDataPtr = 0;
  uint64_t attachmentRigidSample1OwnerRuntimeCreateModelDataPtr = 0;
  uint64_t attachmentRigidSample1RootRuntimeSourceObjectPtr = 0;
  uint64_t attachmentRigidSample1OwnerRuntimeSourceObjectPtr = 0;
  uint64_t attachmentRigidSample1RootRuntimeSourceSpriteObjectPtr = 0;
  uint64_t attachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr = 0;
  uint64_t attachmentRigidSample1ChildRuntimeParentRuntimeModelPtr = 0;
  uint64_t attachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame = 0;
  uint64_t attachmentRigidSample1ChildRuntimeParentLinkSourceMeta = 0;
  uint64_t attachmentRigidSample1ChildRuntimeCreateModelDataPtr = 0;
  uint64_t attachmentRigidSample1ChildRuntimeSourceObjectPtr = 0;
  uint64_t attachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr = 0;
  uint64_t attachmentRigidSample1ChildRuntimeModelResourcePtr = 0;
  uint64_t attachmentRigidSample1ChildRuntimeModelKey = 0;
  uint64_t attachmentRigidSample1ChildRuntimePoseMatrixCount = 0;
  uint64_t attachmentRigidSample1FirstSeenFrame = 0;
  uint64_t attachmentRigidSample1LastSeenFrame = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeModelPtr = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeModelPtr = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeModelPtr = 0;
  uint64_t contractAttachmentRigidSample0ChildSpritePtr = 0;
  uint64_t contractAttachmentRigidSample0SourceObjectPtr = 0;
  uint64_t contractAttachmentRigidSample0WorldObjectEntryPtr = 0;
  uint64_t contractAttachmentRigidSample0SceneNodePtr = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeCreateHandlePtr = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeCreateHandlePtr = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeCreateHandlePtr = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeCreateCallerRva = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeCreateCallerRva = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeCreateCallerRva = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeResolveCallerRva = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeResolveCallerRva = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeResolveCallerRva = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeCreateModelDataPtr = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeCreateModelDataPtr = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeWorldObjectEntryPtr = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeSceneNodePtr = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeWorldObjectEntryPtr = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeSceneNodePtr = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeSourceObjectPtr = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeSourceObjectPtr = 0;
  uint64_t contractAttachmentRigidSample0RootRuntimeSourceSpriteObjectPtr = 0;
  uint64_t contractAttachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeParentRuntimeModelPtr = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeParentLinkSourceMeta = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeCreateModelDataPtr = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeSourceObjectPtr = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeModelResourcePtr = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimeModelKey = 0;
  uint64_t contractAttachmentRigidSample0ChildRuntimePoseMatrixCount = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeModelPtr = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeModelPtr = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeModelPtr = 0;
  uint64_t contractAttachmentRigidSample1ChildSpritePtr = 0;
  uint64_t contractAttachmentRigidSample1SourceObjectPtr = 0;
  uint64_t contractAttachmentRigidSample1WorldObjectEntryPtr = 0;
  uint64_t contractAttachmentRigidSample1SceneNodePtr = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeCreateHandlePtr = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeCreateHandlePtr = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeCreateHandlePtr = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeCreateCallerRva = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeCreateCallerRva = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeCreateCallerRva = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeResolveCallerRva = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeResolveCallerRva = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeResolveCallerRva = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeCreateModelDataPtr = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeCreateModelDataPtr = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeWorldObjectEntryPtr = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeSceneNodePtr = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeWorldObjectEntryPtr = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeSceneNodePtr = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeSourceObjectPtr = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeSourceObjectPtr = 0;
  uint64_t contractAttachmentRigidSample1RootRuntimeSourceSpriteObjectPtr = 0;
  uint64_t contractAttachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeParentRuntimeModelPtr = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeParentLinkSourceMeta = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeCreateModelDataPtr = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeSourceObjectPtr = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeModelResourcePtr = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimeModelKey = 0;
  uint64_t contractAttachmentRigidSample1ChildRuntimePoseMatrixCount = 0;
  uint64_t lastAttachModelToPointParentSpritePtr = 0;
  uint64_t lastAttachModelToPointChildSpritePtr = 0;
  uint64_t lastAttachModelToPointChildRuntimeModelPtr = 0;
  uint64_t lastAttachModelToPointPromotedOwnerRuntimeModelPtr = 0;
  uint64_t lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr = 0;
  uint64_t lastAttachModelToPointPromotedChildRuntimeModelPtr = 0;
  uint64_t lastAttachModelToPointPromotedChildModelResourcePtr = 0;
  uint64_t lastAttachModelToPointUnitPtr = 0;
  uint64_t lastAttachScopeParentSpritePtr = 0;
  uint64_t lastAttachScopeParentRuntimeModelPtr = 0;
  uint64_t lastAttachScopeChildSpritePtr = 0;
  uint64_t lastAttachScopeChildRuntimeModelPtr = 0;
  uint64_t lastAttachScopeHitRuntimeModelPtr = 0;
  uint64_t lastCurrentRenderIdentityWorldObjectEntryPtr = 0;
  uint64_t lastCurrentRenderIdentitySceneNodePtr = 0;
  uint64_t lastCurrentRenderIdentityUnitPtr = 0;
  uint64_t lastRuntimeSourceObjectPtr = 0;
  uint64_t lastRuntimeSourceSpriteObjectPtr = 0;
  uint64_t lastRuntimeSourceRuntimeModelPtr = 0;
  uint64_t lastRuntimeModelResolveRuntimeModelPtr = 0;
  uint64_t lastRuntimeModelResolveHandlePtr = 0;
  uint64_t lastRuntimeModelCreateRuntimeModelPtr = 0;
  uint64_t lastRuntimeModelCreateModelDataPtr = 0;
  uint64_t lastRuntimeModelInitRuntimeModelPtr = 0;
  uint64_t lastRuntimeModelInitModelDataPtr = 0;
  uint64_t lastAttachmentRigidSourceObjectPtr = 0;
  uint64_t lastAttachmentRigidSourceSpriteObjectPtr = 0;
  uint64_t lastRuntimeChildLinkBuildParentRuntimeModelPtr = 0;
  uint64_t lastRuntimeChildLinkBuildChildRuntimeModelPtr = 0;
  uint64_t lastRuntimeChildLinkBuildModelDataPtr = 0;
  uint64_t lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr = 0;
  uint64_t lastRuntimeChildBuildTimeDirectParentModelDataPtr = 0;
  uint64_t lastRuntimeChildBuildTimeDirectRuntimeModelPtr = 0;
  uint64_t lastRuntimeChildBuildTimeDirectModelDataPtr = 0;
  uint64_t lastRuntimeChildBuildTimeDirectModelResourcePtr = 0;
  uint64_t lastRuntimeChildBuildModelDataParentRuntimeModelPtr = 0;
  uint64_t lastRuntimeChildBuildModelDataPtr = 0;
  uint64_t lastRuntimeChildBuildModelDataGroupRecordsPtr = 0;
  uint64_t lastRuntimeChildBuildModelDataHeadPtr = 0;
  uint64_t lastRuntimeChildBuildModelDataLinkNodePtr = 0;
  uint64_t lastRuntimeChildBuildModelDataChildModelDataPtr = 0;
  uint64_t lastRuntimeChildBuildModelDataChildModelResourcePtr = 0;
  uint64_t lastRuntimeMatrixPublisherRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherMatchedRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherMatrixCount = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr = 0;
  uint64_t lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount = 0;
  uint64_t lastAttachmentChildLineageBootstrapParentRuntimeModelPtr = 0;
  uint64_t lastAttachmentChildLineageBootstrapChildRuntimeModelPtr = 0;
  uint64_t lastAttachmentChildLineageBootstrapParentModelDataPtr = 0;
  uint64_t lastAttachmentChildLineageBootstrapChildModelDataPtr = 0;
  uint64_t lastAttachmentChildLineageBootstrapChildModelResourcePtr = 0;
  uint64_t lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr = 0;
  uint64_t lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr = 0;
  uint64_t lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr = 0;
  uint64_t lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr = 0;
  uint64_t lastAttachmentAncestorFromRuntimeModelPtr = 0;
  uint64_t lastAttachmentAncestorRuntimeModelPtr = 0;
  uint64_t poseFrame = 0;
  uint32_t overrideMaxPrimaryPresetSlotIndex = 0;
  uint32_t overrideMaxSharedPresetSlotIndex = 0;
  uint32_t overrideMaxLocalPointSlotIndex = 0;
  uint32_t overrideMaxObservedChildLinkCount = 0;
  uint32_t overrideMaxObservedChildLinkTag = 0;
  uint32_t overrideLastLocalPointSlotIndex = 0;
  uint32_t overrideLastLocalPointSourceRecordIndex = 0;
  uint32_t overrideLastObservedChildLinkCount = 0;
  uint32_t overrideLastMatchedChildLinkCount = 0;
  uint32_t overrideLastMatchedChildMatrixCount = 0;
  uint32_t overrideLastMatchedChildBySourceRecordLinkCount = 0;
  uint32_t overrideLastMatchedChildBySourceRecordMatrixCount = 0;
  uint32_t overrideLastContextRuntimeWithChildLinksOffset = 0;
  uint32_t overrideLastContextRuntimeWithChildLinksCount = 0;
  uint32_t overrideLastContextRuntimeWithChildLinksMaxTag = 0;
  uint32_t overrideLastScratchRootRuntimeChildLinkCount = 0;
  uint32_t overrideLastScratchRootRuntimeMaxTag = 0;
  uint32_t overrideLastArgBlockRuntimeOffset = 0;
  uint32_t overrideLastArgBlockRuntimeChildLinkCount = 0;
  uint32_t overrideLastArgBlockRuntimeMaxTag = 0;
  uint32_t overrideLastArgBlockIdentityHintOffset = 0;
  uint32_t overrideLastArg4BlockRuntimeOffset = 0;
  uint32_t overrideLastArg4BlockRuntimeChildLinkCount = 0;
  uint32_t overrideLastArg4BlockRuntimeMaxTag = 0;
  uint32_t overrideLastArg4BlockIdentityHintOffset = 0;
  uint32_t overrideLastRootRuntimeChildLinkCount = 0;
  uint32_t overrideLastRootRuntimeMaxTag = 0;
  uint32_t lastSpriteHostJHandle = 0;
  uint32_t lastSpriteHostRawcode = 0;
  uint32_t lastSpriteFrameSourceJHandle = 0;
  uint32_t lastSpriteFrameSourceRawcode = 0;
  uint32_t lastSpriteFrameSourceObjectRuntimeFieldOffset = 0;
  uint32_t lastSpriteFrameSourceObjectRegistryFieldOffset = 0;
  uint32_t lastSpriteFrameSourceDeepIdentityOffset = 0;
  uint32_t lastSpriteFrameAttachmentRoleMask = 0;
  uint32_t lastSpriteFrameAttachmentUpdateKind = 0;
  uint32_t lastSpriteFrameAttachmentCallerRva = 0;
  uint32_t lastSourceObjectIdentityHintOffset = 0;
  uint32_t lastAttachedEffectInitJHandle = 0;
  uint32_t lastAttachedEffectInitRawcode = 0;
  uint32_t lastAttachedEffectDirectJHandle = 0;
  uint32_t lastAttachedEffectDirectRawcode = 0;
  uint32_t lastAttachModelToPointJHandle = 0;
  uint32_t lastAttachModelToPointRawcode = 0;
  uint32_t lastAttachModelToPointAttachPointIndex = 0;
  uint32_t lastAttachScopeCallerRva = 0;
  uint32_t lastAttachScopeHitRoleMask = 0;
  uint64_t lastRuntimeModelCtorRuntimeModelPtr = 0;
  uint32_t lastRuntimeModelCtorCallerRva = 0;
  uint32_t lastRuntimeModelCtorKind = 0;
  uint32_t lastRuntimeModelResolveCallerRva = 0;
  uint32_t lastRuntimeModelCreateCallerRva = 0;
  uint32_t lastRuntimeModelInitCallerRva = 0;
  uint32_t lastRuntimeChildLinkBuildSourceMeta = 0;
  uint32_t lastRuntimeChildBuildModelDataPhase = 0;
  uint32_t lastRuntimeChildBuildModelDataGroupCount = 0;
  uint32_t lastRuntimeChildBuildModelDataLinkCount = 0;
  uint32_t lastRuntimeChildBuildModelDataUnreadableLinkCount = 0;
  uint32_t lastRuntimeChildBuildModelDataSourceMeta = 0;
  uint32_t lastRuntimeMatrixPublisherKind = 0;
  uint32_t lastRuntimeMatrixPublisherRoleMask = 0;
  uint32_t lastAttachmentChildLineageBootstrapSourceMeta = 0;
  uint32_t lastAttachmentChildLineageBootstrapBucketIndex = 0;
  uint32_t lastAttachmentChildLineageBootstrapModelDataLinkCount = 0;
  uint32_t lastAttachmentChildLineageBootstrapRuntimeLinkCount = 0;
  uint32_t lastAttachmentChildLineageBootstrapStrictCandidateCount = 0;
  uint32_t lastAttachmentChildLineageBootstrapSourceCandidateCount = 0;
  uint32_t lastAttachmentChildLineageBootstrapBucketCandidateCount = 0;
  uint32_t lastAttachmentChildLineageBootstrapAllCandidateCount = 0;
  uint32_t lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal = 0;
  uint32_t lastAttachmentChildLineageBootstrapModelDataBucketCount = 0;
  uint32_t lastAttachmentAncestorDepth = 0;
  float overrideLastLocalPointX = 0.0f;
  float overrideLastLocalPointY = 0.0f;
  float overrideLastLocalPointZ = 0.0f;
  bool semanticCoreFrameFresh = false;
  bool semanticCoreBuildInProgress = false;
  bool semanticCoreBuildRequestPending = false;
  bool nativeD3D9BackendHasDevice = false;
  bool runtimePoseHooksActive = false;
  bool runtimeChainWarm = false;
  bool runtimeChainNeedsRepair = false;
};

struct ShadowRuntimeBridgeTrackingDecision {
  bool wantsObjectIdentity = false;
  bool wantsFallbackBridge = false;
  // 低扰动健康路径证据：aggregateReadPasses 表示 O(1) 聚合读取，
  // Verifier* 只在显式 brute-force 校验开启时表示真实全表扫描。
  // 这些字段都是次数/记录数，不是计时。
  uint64_t trackingHealthFastPathCalls = 0u;
  uint64_t trackingHealthFullSummaryCompatibilityCalls = 0u;
  uint64_t trackingHealthModelInstanceAggregateReadPasses = 0u;
  uint64_t trackingHealthPoseAggregateReadPasses = 0u;
  uint64_t trackingHealthModelInstanceVerifierScanPasses = 0u;
  uint64_t trackingHealthPoseVerifierScanPasses = 0u;
  uint64_t trackingHealthModelInstanceVerifierRecordsScanned = 0u;
  uint64_t trackingHealthPoseVerifierRecordsScanned = 0u;
  uint64_t trackingHealthModelInstanceVerifierMismatchCount = 0u;
  uint64_t trackingHealthPoseVerifierMismatchCount = 0u;
  uint32_t trackingHealthModelInstanceVerifierMismatchMask = 0u;
  uint32_t trackingHealthPoseVerifierMismatchMask = 0u;
};

// Phase 1 low-disturbance telemetry for the periodic WorldObjects identity
// refresh.  All structures are fixed-size POD snapshots: the hot path never
// formats text, allocates, or performs per-object timing.
enum class WorldObjectsPhase1TrackingReason : uint32_t {
  None = 0,
  ColdBootstrap = 1,
  Warmup = 2,
  PeriodicMaintenance = 3,
  RepairBurst = 4,
  RuntimeChainRepair = 5,
  Count = 6,
};

enum class WorldObjectsPhase1CollectorOutcome : uint32_t {
  Unclassified = 0,
  ValidNonEmpty = 1,
  ValidEmptyAfterFilter = 2,
  NullWorld = 3,
  InvalidGroup = 4,
  UnreadableWorld = 5,
  NullList = 6,
  UnreadableList = 7,
  NullData = 8,
  CountZero = 9,
  CountCap = 10,
  UnreadableData = 11,
  FilteredNoTargets = 12,
  Count = 13,
};

// Game.dll's authoritative maintenance fan-out is group 0..2.  Legacy group
// 3 remains functionally untouched but is intentionally outside this Phase1
// attribution contract.
constexpr uint32_t kWorldObjectsPhase1GroupCount = 3u;
constexpr uint32_t kWorldObjectsPhase1EventSlotCount = 16u;
constexpr uint32_t kWorldObjectsPhase1TrackingReasonCount =
    static_cast<uint32_t>(WorldObjectsPhase1TrackingReason::Count);
constexpr uint32_t kWorldObjectsPhase1CollectorOutcomeCount =
    static_cast<uint32_t>(WorldObjectsPhase1CollectorOutcome::Count);
constexpr uint32_t kWorldObjectsPhase1GetTagStageMaxProbes = 16u;

struct WorldObjectsPhase1RawTiming {
  uint64_t calls = 0u;
  uint64_t ticks = 0u;
  uint64_t maxTicks = 0u;
};

enum class WorldObjectsPhase1PairedTimingStage : uint32_t {
  PresentPreTracking = 0,
  WorldHookInclusive,
  WorldCollector,
  WorldOriginal,
  WorldTrackNewBatches,
  FlushRoot,
  FlushNotify,
  FlushTransactionBegin,
  FlushOriginalBody,
  FlushReimplOpaque,
  FlushReimplTransparent,
  FlushTransactionEnd,
  DispatchRoot,
  DispatchResolveSemantic,
  DispatchNativeBegin,
  DispatchExecBegin,
  DispatchOriginal,
  DispatchPublishVisible,
  DispatchExecEnd,
  DispatchNativeEnd,
  ReimplExecBegin,
  ReimplExecEnd,
  Count,
};

constexpr uint32_t kWorldObjectsPhase1PairedTimingStageCount =
    static_cast<uint32_t>(WorldObjectsPhase1PairedTimingStage::Count);

enum class WorldObjectsPhase1FlushTerminal : uint32_t {
  Unclassified = 0,
  MissingGlobalsOriginal,
  MissingDispatchOriginal,
  DecisionFallbackOriginal,
  OpaqueFailureOriginal,
  TransparentFailureOriginal,
  TakeoverSuccess,
  Count,
};

constexpr uint32_t kWorldObjectsPhase1FlushTerminalCount =
    static_cast<uint32_t>(WorldObjectsPhase1FlushTerminal::Count);

enum class WorldObjectsPhase1DispatchCaptureKind : uint32_t {
  None = 0,
  PurePeriodic,
  PostPeriodicControl,
};

// Dispatch-side attribution is armed for a pure periodic-maintenance frame
// and, when the next decision is an exact reason=None frame, its adjacent
// post-periodic control. Producers accumulate into render-thread TLS and
// publish this POD block once, when the next Present tracking boundary closes
// the preceding frame. The no-new-decision path must settle the same TLS state
// explicitly. No dispatch performs locking, formatting, allocation, or timing
// while the capture is inactive.
struct WorldObjectsPhase1PeriodicDispatch {
  uint64_t commonCalls = 0u;
  uint64_t specialCalls = 0u;
  // Entry TLS tag == WorldObjects is group0; every other entry tag is kept in
  // one explicit residual bucket rather than reclassified after tracker lookup.
  uint64_t group0Calls = 0u;
  uint64_t otherStageCalls = 0u;
  // Inclusive Hook_FlushSortedItems envelopes.  One root timer covers the
  // contiguous dispatch traversal, avoiding an added QPC pair per dispatch.
  uint64_t dispatchRootCalls = 0u;
  uint64_t dispatchRootTicks = 0u;
  // "eligible" deliberately means eligible if the identity-only gate were
  // removed; blockedByIdentity is therefore an exact subset.
  uint64_t worldFastEligibleIgnoringIdentity = 0u;
  uint64_t worldFastBlockedByIdentity = 0u;
  uint64_t getTagStageCalls = 0u;
  uint64_t getTagStageHits = 0u;
  uint64_t getTagStageMisses = 0u;
  uint64_t getTagStageConflicts = 0u;
  uint64_t getTagStageProbes = 0u;
  uint64_t getTagStageTicks = 0u;
  uint64_t captureFrameSerial = 0u;
  uint64_t ownerThreadId = 0u;
  uint64_t qpcReadCount = 0u;
  uint64_t flushTopologyCalls = 0u;
  uint64_t opaqueCountTotal = 0u;
  uint64_t transparentCountTotal = 0u;
  uint64_t flushTopologyHash = 0u;
  std::array<uint64_t, kWorldObjectsPhase1FlushTerminalCount>
      flushTerminalCounts{};
  std::array<WorldObjectsPhase1RawTiming,
             kWorldObjectsPhase1PairedTimingStageCount>
      stageTimings{};
  bool captureRequested = false;
  bool finalized = false;
  bool dispatchPathClosureClean = false;
  bool dispatchRootClosureClean = false;
  bool worldFastClosureClean = false;
  bool getTagStageClosureClean = false;
  bool rawTimingClosureClean = false;
  bool qpcReadClosureClean = false;
  bool pairedTimingClosureClean = false;
  bool flushTopologyClosureClean = false;
  bool closureClean = false;
};

struct WorldObjectsPhase1CollectorObservation {
  WorldObjectsPhase1CollectorOutcome outcome =
      WorldObjectsPhase1CollectorOutcome::Unclassified;
  uint32_t listEntries = 0u;
  uint32_t acceptedEntries = 0u;
  uint32_t sceneNodeEntries = 0u;
  uint32_t handleEntries = 0u;
  uint64_t inclusiveTicks = 0u;
  uint64_t setupTicks = 0u;
  uint64_t iterateTicks = 0u;
  uint64_t registerTicks = 0u;
  uint64_t tailTicks = 0u;
};

struct WorldObjectsPhase1EventGroup {
  uint64_t hookInclusiveTicks = 0u;
  uint64_t collectorInclusiveTicks = 0u;
  uint64_t collectorSetupTicks = 0u;
  uint64_t collectorIterateTicks = 0u;
  uint64_t collectorRegisterTicks = 0u;
  uint64_t collectorTailTicks = 0u;
  uint64_t modelFeedTicks = 0u;
  uint64_t shadowFeedTicks = 0u;
  uint64_t listEntries = 0u;
  uint64_t acceptedEntries = 0u;
  uint64_t sceneNodeEntries = 0u;
  uint64_t handleEntries = 0u;
  uint32_t hookCalls = 0u;
  uint32_t collectorCalls = 0u;
  uint32_t modelFeedCalls = 0u;
  uint32_t shadowFeedCalls = 0u;
  std::array<uint32_t, kWorldObjectsPhase1CollectorOutcomeCount>
      outcomeCounts{};
  bool observed = false;
  bool outcomeClosureClean = false;
  bool collectorPartitionClean = false;
  bool hookCollectorCallClosureClean = false;
  bool hookContainsCollector = false;
  bool registerContainsFeeds = false;
  bool entryCountBoundsClean = false;
  bool unobservedZeroClean = false;
};

struct WorldObjectsPhase1Event {
  uint64_t sequence = 0u;
  // frameSerial is the registry serial observed by the decision; an active
  // decision is applied before BeginFrame, so collectionFrameSerial is the
  // following serial consumed by CollectWorldObjects.
  uint64_t frameSerial = 0u;
  uint64_t collectionFrameSerial = 0u;
  uint64_t poseSerial = 0u;
  uint64_t trackingInclusiveTicks = 0u;
  uint64_t trackingQueryTicks = 0u;
  uint64_t trackingDecisionTicks = 0u;
  uint64_t refreshPeriod = 0u;
  uint64_t warmupFrames = 0u;
  WorldObjectsPhase1TrackingReason reason =
      WorldObjectsPhase1TrackingReason::None;
  uint32_t reasonMask = 0u;
  uint32_t collectorGroupMask = 0u;
  uint32_t hookGroupMask = 0u;
  uint32_t observedGroupMask = 0u;
  uint32_t duplicateCollectorGroupMask = 0u;
  uint32_t duplicateHookGroupMask = 0u;
  bool wantsObjectIdentity = false;
  bool wantsFallbackBridge = false;
  bool trackingPartitionClean = false;
  bool completeObservedGroups = false;
  bool unobservedGroupsZeroClean = false;
  bool groupClosureClean = false;
  WorldObjectsPhase1PeriodicDispatch periodicDispatch{};
  WorldObjectsPhase1PeriodicDispatch postPeriodicControl{};
  bool pairLifecycleClosureClean = false;
  bool periodicEventSubsetClosureClean = false;
  bool pairQpcBalancedExcludingGetTag = false;
  bool pairQpcBalancedIncludingGetTag = false;
  bool pairTopologyComparable = false;
  bool pairComparable = false;
  std::array<WorldObjectsPhase1EventGroup,
             kWorldObjectsPhase1GroupCount>
      groups{};
};

struct WorldObjectsPhase1GroupSummary {
  WorldObjectsPhase1RawTiming hookInclusive;
  WorldObjectsPhase1RawTiming collectorInclusive;
  WorldObjectsPhase1RawTiming collectorSetup;
  WorldObjectsPhase1RawTiming collectorIterate;
  WorldObjectsPhase1RawTiming collectorRegister;
  WorldObjectsPhase1RawTiming collectorTail;
  WorldObjectsPhase1RawTiming modelFeed;
  WorldObjectsPhase1RawTiming shadowFeed;
  uint64_t listEntries = 0u;
  uint64_t acceptedEntries = 0u;
  uint64_t sceneNodeEntries = 0u;
  uint64_t handleEntries = 0u;
  uint64_t collectorPartitionMismatchCount = 0u;
  uint64_t hookContainmentViolationCount = 0u;
  uint64_t registerFeedContainmentViolationCount = 0u;
  uint64_t acceptedCountViolationCount = 0u;
  uint64_t sceneNodeCountViolationCount = 0u;
  uint64_t handleCountViolationCount = 0u;
  std::array<uint64_t, kWorldObjectsPhase1CollectorOutcomeCount>
      outcomeCounts{};
  bool observed = false;
  bool outcomeClosureClean = false;
  bool collectorPartitionClean = false;
  bool hookCollectorCallClosureClean = false;
  bool containmentClean = false;
  bool entryCountBoundsClean = false;
  bool unobservedZeroClean = false;
};

struct WorldObjectsPhase1TelemetrySummary {
  uint64_t qpcFrequency = 0u;
  uint64_t snapshotGenerationBefore = 0u;
  uint64_t snapshotGenerationAfter = 0u;
  uint64_t snapshotWritesStartedBefore = 0u;
  uint64_t snapshotWritesStartedAfter = 0u;
  uint64_t snapshotWritesCompletedBefore = 0u;
  uint64_t snapshotWritesCompletedAfter = 0u;
  uint32_t snapshotWritersBefore = 0u;
  uint32_t snapshotWritersAfter = 0u;
  uint64_t trackingAttempts = 0u;
  uint64_t trackingHealthFastPathCalls = 0u;
  uint64_t trackingHealthFullSummaryCompatibilityCalls = 0u;
  uint64_t trackingHealthModelInstanceAggregateReadPasses = 0u;
  uint64_t trackingHealthPoseAggregateReadPasses = 0u;
  uint64_t trackingHealthModelInstanceVerifierScanPasses = 0u;
  uint64_t trackingHealthPoseVerifierScanPasses = 0u;
  uint64_t trackingHealthModelInstanceVerifierRecordsScanned = 0u;
  uint64_t trackingHealthPoseVerifierRecordsScanned = 0u;
  uint64_t trackingHealthModelInstanceVerifierMismatchCount = 0u;
  uint64_t trackingHealthPoseVerifierMismatchCount = 0u;
  uint32_t trackingHealthModelInstanceVerifierMismatchMask = 0u;
  uint32_t trackingHealthPoseVerifierMismatchMask = 0u;
  uint64_t identityRequests = 0u;
  uint64_t fallbackRequests = 0u;
  uint64_t eventCountLifetime = 0u;
  uint64_t latestEventSequence = 0u;
  uint64_t collectorWithoutEventCount = 0u;
  uint64_t collectorReentryCount = 0u;
  uint64_t collectorWithoutHookCount = 0u;
  uint64_t hookWithoutCollectorCount = 0u;
  uint64_t registryFeedOutsideCollectorCount = 0u;
  uint64_t unexpectedGroupCount = 0u;
  uint64_t pairedCaptureDuplicatePublishCount = 0u;
  uint64_t pairedCaptureLostPublishCount = 0u;
  uint64_t pairedCaptureSlotMismatchCount = 0u;
  uint32_t lifetimeCollectorGroupMask = 0u;
  uint32_t lifetimeHookGroupMask = 0u;
  uint32_t lifetimeObservedGroupMask = 0u;
  uint32_t retainedEventExpectedCount = 0u;
  uint32_t retainedEventCount = 0u;
  uint32_t missingRetainedEventCount = 0u;
  WorldObjectsPhase1RawTiming trackingInclusive;
  WorldObjectsPhase1RawTiming trackingQuery;
  WorldObjectsPhase1RawTiming trackingDecision;
  std::array<uint64_t, kWorldObjectsPhase1TrackingReasonCount>
      reasonCounts{};
  std::array<WorldObjectsPhase1GroupSummary,
             kWorldObjectsPhase1GroupCount>
      groups{};
  std::array<WorldObjectsPhase1Event,
             kWorldObjectsPhase1EventSlotCount>
      events{};
  bool snapshotStable = false;
  bool trackingHealthPathClosureClean = false;
  bool trackingReasonClosureClean = false;
  bool eventCountClosureClean = false;
  bool lifecycleClosureClean = false;
  bool lifetimeObservedGroupsClosureClean = false;
  bool lifetimeUnobservedGroupsZeroClean = false;
  bool retainedEventsClosureClean = false;
  bool overallClosureClean = false;
};

static_assert(std::is_trivially_copyable<
                  WorldObjectsPhase1CollectorObservation>::value,
              "WorldObjects Phase1 observation must remain POD telemetry");
static_assert(std::is_trivially_copyable<
                  WorldObjectsPhase1Event>::value,
              "WorldObjects Phase1 event ring must remain POD telemetry");
static_assert(std::is_trivially_copyable<
                  WorldObjectsPhase1TelemetrySummary>::value,
              "WorldObjects Phase1 snapshot must remain POD telemetry");

bool IsWorldObjectsPhase1CaptureActive(int groupIdx) noexcept;
bool IsWorldObjectsPhase1CollectorCaptureActive() noexcept;
extern thread_local uint64_t
    g_worldObjectsPhase1PurePeriodicDispatchSequence;
extern thread_local WorldObjectsPhase1DispatchCaptureKind
    g_worldObjectsPhase1DispatchCaptureKind;
inline bool IsWorldObjectsPhase1PurePeriodicDispatchCaptureActive() noexcept {
  return g_worldObjectsPhase1PurePeriodicDispatchSequence != 0u;
}
inline uint64_t CurrentWorldObjectsPhase1PurePeriodicDispatchSequence()
    noexcept {
  return g_worldObjectsPhase1PurePeriodicDispatchSequence;
}
bool BeginWorldObjectsPhase1Collector(int groupIdx) noexcept;
void CompleteWorldObjectsPhase1Collector(
    const WorldObjectsPhase1CollectorObservation& observation) noexcept;
void RecordWorldObjectsPhase1HookInclusive(int groupIdx,
                                           uint64_t ticks) noexcept;
void RecordWorldObjectsPhase1RegistryFeed(uint64_t modelTicks,
                                          uint64_t shadowTicks) noexcept;
void RecordWorldObjectsPhase1PeriodicDispatch(
    uint64_t eventSequence, bool special, bool group0Stage,
    bool worldFastEligibleIgnoringIdentity,
    bool worldFastBlockedByIdentity) noexcept;
void RecordWorldObjectsPhase1PeriodicDispatchRoot(
    uint64_t eventSequence, uint64_t ticks) noexcept;
void RecordWorldObjectsPhase1PeriodicGetTagStage(
    uint64_t eventSequence, bool hit, bool conflict,
    uint32_t probes, uint64_t ticks) noexcept;
void RecordWorldObjectsPhase1PairedTiming(
    uint64_t eventSequence, WorldObjectsPhase1PairedTimingStage stage,
    uint64_t ticks) noexcept;
void RecordWorldObjectsPhase1PairedQpcReads(
    uint64_t eventSequence, uint64_t reads) noexcept;
void RecordWorldObjectsPhase1PairedFlushTopology(
    uint64_t eventSequence, uint32_t opaqueCount,
    uint32_t transparentCount) noexcept;
void RecordWorldObjectsPhase1PairedFlushTerminal(
    uint64_t eventSequence,
    WorldObjectsPhase1FlushTerminal terminal) noexcept;
WorldObjectsPhase1TelemetrySummary QueryWorldObjectsPhase1Telemetry();

void NoteShadowRuntimeRenderObject(const RenderObjectInfo& info);
void NoteShadowRuntimeRenderObjectsBatch(
    const std::vector<const RenderObjectInfo*>& infos);
void NoteShadowRuntimeIdentity(void* worldObjectEntry, void* sceneNode,
                               void* unitPtr, void* spritePtr,
                               uint32_t jHandle, uint32_t rawcode,
                               ObjectKind kind);
void NoteShadowRuntimeModelBinding(void* spritePtr, void* runtimeModelPtr,
                                   void* modelResourcePtr,
                                   const std::string& modelPath,
                                   uint32_t modelType, uint32_t modelFlags,
                                   uint64_t modelKey);
void NoteShadowRuntimePose(void* runtimeModelPtr, void* sceneNode, void* unitPtr,
                           uint32_t sequenceId, float sequenceTime, float scale,
                           float yaw, float pitch, float roll, float height,
                           bool hasWorldTransform = false,
                           const Matrix4* worldTransform = nullptr,
                           uint32_t matrixCount = 0,
                           uint64_t matrixHash = 0);

// VS-S1 阴影重放的跨线程累计快照。写端来自唯一 ShadowReceiver，
// 读端用于 GPU-skin clean-pair；八个字段必须在同一 shared_mutex 下复制。
struct GpuSkinVsShadowRuntimeCounters {
  uint64_t directAttempts = 0u;
  uint64_t directInputRejects = 0u;
  uint64_t directStateRejects = 0u;
  uint64_t directDrawsSubmitted = 0u;
  uint64_t directBindingsCleared = 0u;
  uint64_t replayDirectional = 0u;
  uint64_t replayPoint = 0u;
  uint64_t replayUnknown = 0u;
};

void NoteShadowSceneStats(const War3ShadowCaptureStats& stats);

// Publishes the authoritative post-receiver reconciliation for a render
// frame. Later command-list/prepass snapshots may still update producer
// counters, but must not replace this terminal receiver state with their
// immutable pre-receiver copy.
void NoteShadowSceneTerminalStats(const War3ShadowCaptureStats& stats);
GpuSkinVsShadowRuntimeCounters QueryGpuSkinVsShadowRuntimeCounters();
void NoteShadowFrameCadenceSample(uint64_t frameIndex,
                                  const War3ShadowCaptureStats& stats);
// Records the exact, post-policy caster set consumed by both directional and
// point-shadow rendering. This is deliberately called at the sole replay
// choke point rather than at producer sites, so an anomalous captured frame
// can be joined to the actual geometry/backing/world tuple that reached the
// shadow map. Output is part of the opt-in shadow pose full-trace JSONL.
void NoteFinalShadowCasterFrame(
    const War3FrameScene& scene,
    const std::vector<const War3ShadowCasterDraw*>& draws,
    uint64_t frameSerial);
// Records the exact CurrentDraw snapshot consumed by semantic populate before
// policy, freshness and lease filtering. Unlike the late registry snapshot in
// the cadence reporter, this survives direct-only slot rotation and therefore
// lets a missing final caster be attributed to upstream native submission or
// to a WarVK downstream rejection.
void NoteCurrentDrawSnapshotFrame(
    const std::vector<CurrentDrawContractRecord>& records,
    uint64_t frameSerial);
void StartShadowPoseFullTrace(uint32_t maxSeconds = 15u,
                              bool includeMatrixBytes = false,
                              uint32_t maxPoseRecords = 0u,
                              uint32_t maxShadowObjectRecords = 0u,
                              uint32_t maxCurrentDrawRecords = 0u);
void StopShadowPoseFullTrace();
ShadowPoseFullTraceStatus QueryShadowPoseFullTraceStatus();
void NoteSemanticDataPerf(SemanticDataPerfTag tag, uint64_t durationUs);
void NoteNativeSemanticWorldStageCandidate(int stage, int a3, int a4, int a5,
                                           bool jassReady, bool gameStarted);
void NoteNativeSemanticWorldStageSkippedRuntimeNotReady(int stage);
void NoteNativeSemanticWorldStagePrepare(int stage, bool success);
void NoteNativeSemanticWorldStageExecute(int stage, bool success);
void NoteShadowRuntimeSpriteFramePose(void* runtimeModelPtr, void* spritePtr,
                                      void* sceneNode, void* unitPtr, float dt,
                                      uint32_t sequenceId, float sequenceTime,
                                      float scale, float yaw, float pitch,
                                      float roll, float height,
                                      bool hasWorldTransform = false,
                                      const Matrix4* worldTransform = nullptr,
                                      uint32_t matrixCount = 0,
                                      uint64_t matrixHash = 0);

ShadowRuntimeBridgeSummary QueryShadowRuntimeBridgeSummary(
    bool refreshSemanticFrameIfStale = false);
ShadowProducerRuntimeDiagnostics QueryShadowProducerRuntimeDiagnostics();
ShadowRuntimeBridgeTrackingDecision ComputeShadowRuntimeBridgeTracking();
void FinalizeWorldObjectsPhase1PreviousFrameWithoutNewDecision() noexcept;
void ResetShadowRuntimeBridgeState();

struct SemanticAugmentTlsCacheStats {
  bool enabled = false;
  bool telemetryEnabled = false;
  uint32_t capacityPerRegistry = 0u;
  uint64_t modelLookups = 0u;
  uint64_t modelHits = 0u;
  uint64_t modelNegativeHits = 0u;
  uint64_t modelMisses = 0u;
  uint64_t modelGenerationMismatches = 0u;
  uint64_t modelCollisions = 0u;
  uint64_t shadowLookups = 0u;
  uint64_t shadowHits = 0u;
  uint64_t shadowNegativeHits = 0u;
  uint64_t shadowMisses = 0u;
  uint64_t shadowGenerationMismatches = 0u;
  uint64_t shadowCollisions = 0u;
  uint64_t modelRegistryGeneration = 0u;
  uint64_t shadowRegistryGeneration = 0u;
};

// Read-only cumulative telemetry for the fixed-capacity semantic augment TLS
// caches.  The perf monitor can consume this later without coupling either
// registry to the report implementation.
SemanticAugmentTlsCacheStats QuerySemanticAugmentTlsCacheStats() noexcept;

enum class ShadowSemanticAugmentTracePhase : uint8_t {
  ModelInstance = 0u,
  ShadowObject,
  Pose,
  RenderObject,
  Finalize,
};

struct ShadowSemanticAugmentTrace {
  using EnterFn = void (*)(void*, ShadowSemanticAugmentTracePhase);

  void* context = nullptr;
  EnterFn enterFn = nullptr;

  inline void enter(ShadowSemanticAugmentTracePhase phase) const {
    if (enterFn != nullptr)
      enterFn(context, phase);
  }
};

bool AugmentShadowSemanticContext(dxvk::War3ShadowSemanticContext& semantic,
                                  const RenderObjectInfo* currentObj,
                                  const ShadowSemanticAugmentTrace* trace = nullptr);

} // namespace dxvk::war3::render
