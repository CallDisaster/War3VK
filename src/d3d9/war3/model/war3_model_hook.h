// war3_model_hook.h - 魔兽模型加载探针与路径重映射
#pragma once

#include <cstdint>

namespace dxvk {
struct Matrix4;
namespace war3 {
namespace model {

struct RuntimeOverrideOutputProbeSummary {
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
  uint64_t attachmentChildLineageBootstrapAttemptCount = 0;
  uint64_t attachmentChildLineageBootstrapSuccessCount = 0;
  uint64_t attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount = 0;
  uint64_t attachmentChildLineageBootstrapMissNoModelDataLinksCount = 0;
  uint64_t attachmentChildLineageBootstrapMissNoUniqueChildCount = 0;
  uint64_t attachmentAncestorIdentityHintWriteCount = 0;
  uint64_t sourceObjectRenderBridgeResolvedByEntryCount = 0;
  uint64_t sourceObjectRenderBridgeResolvedBySceneNodeCount = 0;
  uint64_t spriteHostBindCount = 0;
  uint64_t spriteHostBindResolvedIdentityCount = 0;
  uint64_t spriteHostBindResolvedUnitCount = 0;
  uint64_t spriteHostBindResolvedHandleCount = 0;
  uint64_t spriteHostBindResolvedRawcodeCount = 0;
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
  uint64_t sampleFrame = 0;
  uint64_t lastActiveFrame = 0;
  uint64_t primaryPresetWriteCount = 0;
  uint64_t sharedPresetWriteCount = 0;
  uint64_t localPointWriteCount = 0;
  uint64_t localPointNonZeroWriteCount = 0;
  uint64_t localPointObservedChildLinkWriteCount = 0;
  uint64_t localPointMatchedChildLinkWriteCount = 0;
  uint64_t localPointMatchedChildPaletteReadyWriteCount = 0;
  uint64_t localPointMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t localPointMatchedChildPaletteReadyBySourceRecordWriteCount = 0;
  uint64_t localPointContextRuntimeWithChildLinksWriteCount = 0;
  uint64_t localPointContextMatchedChildLinkWriteCount = 0;
  uint64_t localPointContextMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t localPointContextMatchedChildPaletteReadyBySourceRecordWriteCount = 0;
  uint64_t localPointScratchRootRuntimeWithChildLinksWriteCount = 0;
  uint64_t localPointScratchRootMatchedChildLinkWriteCount = 0;
  uint64_t localPointScratchRootMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t localPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount = 0;
  uint64_t localPointArgBlockRuntimeWithChildLinksWriteCount = 0;
  uint64_t localPointArgBlockMatchedChildLinkWriteCount = 0;
  uint64_t localPointArgBlockMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t localPointArgBlockIdentityHintWriteCount = 0;
  uint64_t localPointArg4BlockRuntimeWithChildLinksWriteCount = 0;
  uint64_t localPointArg4BlockMatchedChildLinkWriteCount = 0;
  uint64_t localPointArg4BlockMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t localPointArg4BlockIdentityHintWriteCount = 0;
  uint64_t localPointChildSourceMetaIdentityHintWriteCount = 0;
  uint64_t localPointSpriteBoundCandidateWriteCount = 0;
  uint64_t localPointParentSpriteIdentityHintWriteCount = 0;
  uint64_t localPointRootRuntimeHitWriteCount = 0;
  uint64_t localPointRootRuntimeWithChildLinksWriteCount = 0;
  uint64_t localPointRootRuntimeMatchedChildLinkWriteCount = 0;
  uint64_t localPointRootRuntimeMatchedChildPaletteReadyWriteCount = 0;
  uint64_t localPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount = 0;
  uint64_t localPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount = 0;
  uint64_t attachmentRigidPublishedCount = 0;
  uint32_t maxPrimaryPresetSlotIndex = 0;
  uint32_t maxSharedPresetSlotIndex = 0;
  uint32_t maxLocalPointSlotIndex = 0;
  uint32_t maxObservedChildLinkCount = 0;
  uint32_t maxObservedChildLinkTag = 0;
  uint64_t lastPrimaryPresetHash = 0;
  uint64_t lastSharedPresetHash = 0;
  uint64_t lastRuntimeModelPtr = 0;
  uint64_t lastMatchedChildRuntimeModelPtr = 0;
  uint64_t lastMatchedChildBySourceRecordRuntimeModelPtr = 0;
  uint64_t lastContextRuntimeWithChildLinksPtr = 0;
  uint64_t lastScratchRootPtr = 0;
  uint64_t lastScratchRootRuntimeModelPtr = 0;
  uint64_t lastArgBlockPtr = 0;
  uint64_t lastArgBlockRuntimeModelPtr = 0;
  uint64_t lastArgBlockIdentityHintPtr = 0;
  uint64_t lastArg4BlockPtr = 0;
  uint64_t lastArg4BlockRuntimeModelPtr = 0;
  uint64_t lastArg4BlockIdentityHintPtr = 0;
  uint64_t lastChildSourceMetaPtr = 0;
  uint64_t lastChildSourceMetaRuntimeModelPtr = 0;
  uint64_t lastSpriteBoundCandidateSpritePtr = 0;
  uint64_t lastSpriteBoundCandidateRuntimeModelPtr = 0;
  uint64_t lastParentSpriteIdentityHintSpritePtr = 0;
  uint64_t lastParentSpriteIdentityHintRuntimeModelPtr = 0;
  uint64_t lastRootRuntimeModelPtr = 0;
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
  uint32_t lastLocalPointSlotIndex = 0;
  uint32_t lastLocalPointSourceRecordIndex = 0;
  uint32_t lastObservedChildLinkCount = 0;
  uint32_t lastMatchedChildLinkCount = 0;
  uint32_t lastMatchedChildMatrixCount = 0;
  uint32_t lastMatchedChildBySourceRecordLinkCount = 0;
  uint32_t lastMatchedChildBySourceRecordMatrixCount = 0;
  uint32_t lastContextRuntimeWithChildLinksOffset = 0;
  uint32_t lastContextRuntimeWithChildLinksCount = 0;
  uint32_t lastContextRuntimeWithChildLinksMaxTag = 0;
  uint32_t lastScratchRootRuntimeChildLinkCount = 0;
  uint32_t lastScratchRootRuntimeMaxTag = 0;
  uint32_t lastArgBlockRuntimeOffset = 0;
  uint32_t lastArgBlockRuntimeChildLinkCount = 0;
  uint32_t lastArgBlockRuntimeMaxTag = 0;
  uint32_t lastArgBlockIdentityHintOffset = 0;
  uint32_t lastArg4BlockRuntimeOffset = 0;
  uint32_t lastArg4BlockRuntimeChildLinkCount = 0;
  uint32_t lastArg4BlockRuntimeMaxTag = 0;
  uint32_t lastArg4BlockIdentityHintOffset = 0;
  uint32_t lastRootRuntimeChildLinkCount = 0;
  uint32_t lastRootRuntimeMaxTag = 0;
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
  float lastLocalPointX = 0.0f;
  float lastLocalPointY = 0.0f;
  float lastLocalPointZ = 0.0f;
};

struct RuntimeParentLinkQueryResult {
  bool known = false;
  uint64_t parentRuntimeModelPtr = 0;
  uint32_t sourceMeta = 0;
  uint32_t bucketIndex = 0;
  uint64_t lastSeenFrame = 0;
};

// 初始化模型 Hook（仅在 1.27a 偏移下验证）
// bootstrapOnly=true 时仅安装最早期 provenance 必需 hooks，避免把
// sprite/pose/local-point 这类高侵入 hook 提前到 runtime 激活之前。
void Init(uintptr_t gameBase, bool bootstrapOnly = false);

// 关闭/重置状态（当前仅占位）
void Shutdown();

// 查询是否已启用模型 Hook
bool IsActive();

// 查询高成本的 pose / matrix 运行时 Hook 是否启用
bool IsPoseHookEnabled();

// Lightweight dirty signal from War3 final matrix publisher hooks.
uint64_t RuntimeMatrixPublisherPoseRevision();

// 查询 override output probe 摘要（controller/local-point/shared-preset）。
RuntimeOverrideOutputProbeSummary QueryRuntimeOverrideOutputProbeSummary();

// 查询 child runtime 当前是否已经被 BuildChildRuntimeModelLinks 链路登记到
// runtime parent-link 图里，用于 attachment sample 对账。
bool QueryRuntimeParentLink(void* childRuntimeModelPtr,
                            RuntimeParentLinkQueryResult& out);

bool TryBootstrapRuntimeChildLineageFromParentModelData(
    void* parentRuntimeModelPtr, void* parentModelDataPtr,
    void* childRuntimeModelPtr, uint32_t sourceMeta, uint32_t bucketIndex,
    void*& outChildModelDataPtr, void*& outChildModelResourcePtr);

// 通过 slotIndex 查询 Hook_RuntimeMatrixWrite 捕获的混合调色板
bool QueryBlendedPaletteBySlotIndex(uint32_t slotIndex,
                                     void* outPaletteVec,
                                     uint32_t& outGroupCount);

} // namespace model
} // namespace war3
} // namespace dxvk
