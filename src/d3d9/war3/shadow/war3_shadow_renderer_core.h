#pragma once

#include "war3_shadow_backend.h"
#include "war3_shadow_runtime_contract.h"

#include <cstdint>
#include <array>
#include <memory>
#include <mutex>
#include <vector>

namespace dxvk::war3::shadow {

enum class ShadowDrawPath : uint8_t {
  Rigid = 0,
  Skinned = 1,
};

struct ShadowPacketResource {
  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0;
  uint32_t geosetIndex = kInvalidShadowContractGeosetIndex;
  uint32_t vertexCount = 0;
  uint32_t primitiveRecordCount = 0;
  uint8_t explicitBlendCount = 0;
  uint64_t contentHash = 0;
  ShadowPrimitiveTopology topology = ShadowPrimitiveTopology::TriangleList;
  const std::vector<float>* positions = nullptr;
  const std::vector<uint8_t>* vertexGroupIndices = nullptr;
  const std::vector<std::array<float, 3>>* vertexBlendWeights = nullptr;
  const std::vector<std::array<uint8_t, 4>>* vertexBlendIndices = nullptr;
  const std::vector<uint16_t>* indices = nullptr;
  const std::vector<uint32_t>* matrixGroupSizes = nullptr;
  const std::vector<uint32_t>* matrixIndices = nullptr;
  const void* dynamicPositionStream = nullptr;
  uint32_t dynamicPositionStride = 0;
  const uint16_t* dynamicIndexStream = nullptr;
  uint32_t dynamicIndexCount = 0;
  uint64_t dynamicIndexHash = 0;
  uint32_t dynamicPrimitiveBaseIndex = 0;
  std::shared_ptr<const std::vector<uint16_t>> ownedDynamicIndices;
  std::vector<float> ownedPositions;
  std::vector<uint8_t> ownedVertexGroupIndices;
  std::vector<std::array<float, 3>> ownedVertexBlendWeights;
  std::vector<std::array<uint8_t, 4>> ownedVertexBlendIndices;
  std::vector<uint16_t> ownedIndices;
  std::vector<uint32_t> ownedMatrixGroupSizes;
  std::vector<uint32_t> ownedMatrixIndices;

  const std::vector<float>& positionVec() const {
    static const std::vector<float> kEmpty;
    return positions != nullptr ? *positions : kEmpty;
  }

  const std::vector<uint8_t>& vertexGroupIndexVec() const {
    static const std::vector<uint8_t> kEmpty;
    return vertexGroupIndices != nullptr ? *vertexGroupIndices : kEmpty;
  }

  const std::vector<std::array<float, 3>>& vertexBlendWeightVec() const {
    static const std::vector<std::array<float, 3>> kEmpty;
    return vertexBlendWeights != nullptr ? *vertexBlendWeights : kEmpty;
  }

  const std::vector<std::array<uint8_t, 4>>& vertexBlendIndexVec() const {
    static const std::vector<std::array<uint8_t, 4>> kEmpty;
    return vertexBlendIndices != nullptr ? *vertexBlendIndices : kEmpty;
  }

  const std::vector<uint16_t>& indexVec() const {
    static const std::vector<uint16_t> kEmpty;
    return indices != nullptr ? *indices : kEmpty;
  }

  const std::vector<uint32_t>& matrixGroupSizeVec() const {
    static const std::vector<uint32_t> kEmpty;
    return matrixGroupSizes != nullptr ? *matrixGroupSizes : kEmpty;
  }

  const std::vector<uint32_t>& matrixIndexVec() const {
    static const std::vector<uint32_t> kEmpty;
    return matrixIndices != nullptr ? *matrixIndices : kEmpty;
  }
};

struct ShadowDrawPacket {
  ShadowRenderableRecord renderable = {};
  ShadowPacketResource resource = {};
  ShadowPoseRecord pose = {};
  ShadowMaterialSignature material = {};
  ShadowDrawPath path = ShadowDrawPath::Rigid;
  bool usesDynamicMeshPositions = false;
  bool hasRuntimeGroupPalette = false;
  bool matrixGroupsUseAveraging = false;
  uint32_t maxVertexGroupSlot = 0;
  uint64_t runtimeGroupPaletteHash = 0;
  std::vector<Matrix4> runtimeGroupPalette;
};

struct ShadowSubmissionFrame {
  uint64_t frameSerial = 0;
  uint64_t sourcePublishRevision = 0;
  std::shared_ptr<const ShadowModelResourceStore> resourceStore;
  std::vector<ShadowDrawPacket> draws;
};

struct ShadowResolveStats {
  uint64_t frameSerial = 0;
  uint64_t considered = 0;
  uint64_t resolved = 0;
  uint64_t rigidResolved = 0;
  uint64_t skinnedResolved = 0;
  uint64_t rigidCandidateCount = 0;
  uint64_t skinnedCandidateCount = 0;
  uint64_t skinnedCandidatePoseReadyCount = 0;
  uint64_t skinnedCandidateRuntimeGroupPaletteReadyCount = 0;
  uint64_t skinnedCandidateResolvedAsAttachmentRigidCount = 0;
  uint64_t runtimeGroupPaletteMissNoSkinningData = 0;
  uint64_t runtimeGroupPaletteMissNoPosePalette = 0;
  uint64_t runtimeGroupPaletteMissNoVertexGroups = 0;
  uint64_t runtimeGroupPaletteMissInvalidGroupTable = 0;
  uint64_t runtimeGroupPaletteMissMatrixIndexOutOfRange = 0;
  uint64_t runtimeGroupPaletteMissVertexGroupOutOfRange = 0;
  uint64_t runtimeGroupPaletteMissFallbacksFailed = 0;
  uint64_t runtimeGroupPaletteMissLastPoseCount = 0;
  uint64_t runtimeGroupPaletteMissLastGroupCount = 0;
  uint64_t runtimeGroupPaletteMissLastMaxVertexGroupSlot = 0;
  uint64_t runtimeGroupPaletteMissLastMatrixIndexCount = 0;
  uint64_t runtimeGroupPaletteMissLastMatrixIndex = 0;
  uint64_t runtimeGroupPaletteRescueByMeshPoseContext = 0;
  uint64_t runtimeGroupPaletteRescueByResourceMatchedPose = 0;
  uint64_t runtimeGroupPaletteRescueByRuntimeRoot = 0;
  uint64_t runtimeGroupPaletteRescueByChildRuntime = 0;
  uint64_t runtimeGroupPaletteRescueByDescendantRuntime = 0;
  uint64_t runtimeGroupPaletteResourceMatchedPoseSuppressed = 0;
  uint64_t explicitResourceOwnerRigidResolved = 0;
  uint64_t explicitResourceOwnerRigidWorldTransformResolved = 0;
  uint64_t explicitResourceOwnerRigidNoMatrixPalette = 0;
  uint64_t explicitBlendAttempts = 0;
  uint64_t explicitBlendAttemptWithSpanRemapTable = 0;
  uint64_t explicitBlendResolved = 0;
  uint64_t explicitBlendSpanRemapResolved = 0;
  uint64_t explicitBlendStrideSearchMiss = 0;
  uint64_t explicitBlendFinalDecodeMiss = 0;
  uint64_t attachmentRigidMatchByChildRuntimeModel = 0;
  uint64_t attachmentRigidMatchByChildSprite = 0;
  uint64_t attachmentRigidMatchByChildRuntimeGeoset = 0;
  uint64_t attachmentRigidMatchByChildSpriteRuntimeGeoset = 0;
  uint64_t attachmentRigidMatchByOwnerRuntimeGeoset = 0;
  uint64_t attachmentRigidMatchByRootRuntimeGeoset = 0;
  uint64_t attachmentRigidMatchByResourceRuntimeOwner = 0;
  uint64_t attachmentRigidMatchByRenderableRuntimeRoot = 0;
  uint64_t attachmentRigidMatchByWorldObjectEntry = 0;
  uint64_t attachmentRigidMatchBySceneNode = 0;
  uint64_t attachmentRigidMatchByUnitPtr = 0;
  uint64_t attachmentRigidMatchByHandle = 0;
  uint64_t attachmentRigidMatchByChildModelResource = 0;
  uint64_t attachmentRigidMatchByUniqueIdentity = 0;
  uint64_t attachmentRigidMatchMiss = 0;
  uint64_t lastAttachmentRigidMatchMissRuntimeModelPtr = 0;
  uint64_t lastAttachmentRigidMatchMissModelResourcePtr = 0;
  uint64_t lastAttachmentRigidMatchMissRuntimeGeosetPtr = 0;
  uint64_t lastAttachmentRigidMatchMissRuntimeGeosetDataPtr = 0;
  uint64_t lastAttachmentRigidMatchMissGeosetIndex = 0;
  uint64_t lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr = 0;
  uint64_t attachmentRigidResolved = 0;
  uint64_t attachmentRigidSupplementalAttachmentCount = 0;
  uint64_t attachmentRigidSupplementalResourceCandidateCount = 0;
  uint64_t attachmentRigidSupplementalResolvedCount = 0;
  uint64_t attachmentRigidSupplementalResourceMissCount = 0;
  uint64_t attachmentRigidPoseMissNoRecord = 0;
  uint64_t attachmentRigidPoseMissMissingRuntimes = 0;
  uint64_t attachmentRigidPoseMissNoRootPose = 0;
  uint64_t attachmentRigidPoseMissNoRootWorldTransform = 0;
  uint64_t attachmentRigidPoseRecoveredWorldTransformFromLivePose = 0;
  uint64_t attachmentRigidPoseRecoveredWorldTransformFromShadowRegistry = 0;
  uint64_t skippedNoIdentity = 0;
  uint64_t skippedNoResolvedGeoset = 0;
  uint64_t skippedNoGeoset = 0;
  uint64_t skippedResourceMiss = 0;
  uint64_t skippedResourceNotReady = 0;
  uint64_t skippedNoPose = 0;
  uint64_t skippedNoPoseNoContext = 0;
  uint64_t skippedNoPoseAnonymousSubpart = 0;
  uint64_t skippedNoPoseLookupMiss = 0;
  uint64_t skippedNoRuntimeGroupPalette = 0;
  uint64_t slowestRecordResolveUs = 0;
  uint64_t slowestRecordIndex = 0;
  uint64_t slowestRecordRuntimeModelPtr = 0;
  uint64_t slowestRecordModelResourcePtr = 0;
  uint64_t slowestRecordRuntimeGeosetPtr = 0;
  uint64_t slowestRecordRuntimeGeosetDataPtr = 0;
  uint64_t slowestRecordGeosetIndex = 0;
  uint64_t slowestRecordObjectKind = 0;
  uint64_t slowestResourceLookupUs = 0;
  uint64_t slowestPoseResolveUs = 0;
  uint64_t slowestPoseDirectLookupUs = 0;
  uint64_t slowestPoseOwnerLookupUs = 0;
  uint64_t slowestPoseSpriteLookupUs = 0;
  uint64_t slowestPoseInstanceRegistryUs = 0;
  uint64_t slowestPoseShadowRegistryUs = 0;
  uint64_t slowestPoseRenderRegistryUs = 0;
  uint64_t slowestPoseRuntimeRootsUs = 0;
  uint64_t slowestPoseMeshPoseContextUs = 0;
  uint64_t slowestPoseMissDiagnosticUs = 0;
  uint64_t slowestLayerContractUs = 0;
  uint64_t slowestRuntimeGroupPaletteUs = 0;
  uint64_t slowestRuntimeGroupPaletteRescueUs = 0;
  uint64_t slowestAttachmentRigidResolveUs = 0;
};

class ShadowRendererCore {
public:
  ShadowResolveStats buildFrame(const ShadowFrameManifest& manifest,
                                const ShadowModelResourceStore& resources,
                                const ShadowPoseStore& poses,
                                const ShadowAttachmentRigidStore& attachments,
                                ShadowSubmissionFrame& outFrame) const;
  size_t buildFrameChunk(const ShadowFrameManifest& manifest,
                         const ShadowModelResourceStore& resources,
                         const ShadowPoseStore& poses,
                         const ShadowAttachmentRigidStore& attachments,
                         size_t startIndex,
                         uint32_t maxRecords, uint64_t maxDurationUs,
                         ShadowSubmissionFrame& ioFrame,
                         ShadowResolveStats& ioStats) const;

  bool submitFrame(const ShadowSubmissionFrame& frame,
                   IShadowRenderBackend& backend) const;
  bool submitFrameLimited(const ShadowSubmissionFrame& frame,
                          IShadowRenderBackend& backend,
                          uint32_t maxSubmittedDraws) const;

private:
  bool resolveRecord(const ShadowRenderableRecord& record,
                     const ShadowModelResourceStore& resources,
                     const ShadowPoseStore& poses,
                     const ShadowAttachmentRigidStore& attachments,
                     ShadowDrawPacket& outPacket,
                     ShadowResolveStats& ioStats) const;
};

struct ShadowValidationFrameStats {
  uint64_t frameSerial = 0;
  uint64_t sourcePublishRevision = 0;
  uint64_t sourceVisibleCount = 0;
  uint64_t sourceStableIdentityCount = 0;
  uint64_t sourceResolvedGeosetCount = 0;
  uint64_t sourceUnitCount = 0;
  ShadowResolveStats resolve = {};
  uint64_t coreDrawPacketCount = 0;
  uint64_t upperLayerResolvedItems = 0;
  uint64_t supplementalUpperLayerDrawPacketCount = 0;
  uint64_t drawPacketCount = 0;
  uint64_t submittedDrawCount = 0;
  uint64_t buildDurationUs = 0;
};

struct ShadowValidationBuildState {
  bool buildInProgress = false;
  bool buildRequestPending = false;
  uint64_t buildFrameSerial = 0;
  uint64_t buildPublishRevision = 0;
  uint64_t pendingFrameSerial = 0;
  uint64_t pendingPublishRevision = 0;
  uint64_t lastBuildDurationUs = 0;
  uint64_t buildCurrentRecordIndex = 0;
  uint64_t buildRecordCount = 0;
  uint64_t buildChunkCount = 0;
  uint64_t stalePendingBuildClearedCount = 0;
};

struct ShadowValidationBuildWork {
  std::shared_ptr<const ShadowFrameManifest> manifest;
  std::shared_ptr<const ShadowModelResourceStore> resources;
  std::shared_ptr<const ShadowPoseStore> poses;
  std::shared_ptr<const ShadowAttachmentRigidStore> attachments;
  ShadowSubmissionFrame frame = {};
  ShadowValidationFrameStats stats = {};
  uint64_t totalBuildDurationUs = 0;
  size_t nextRecordIndex = 0;
  uint64_t chunkCount = 0;
};

class ShadowValidationRuntime {
public:
  static ShadowValidationRuntime& instance();

  void requestLatestFrameBuild();
  void requestFrameBuildForContract(
      std::shared_ptr<const ShadowFrameManifest> manifest,
      std::shared_ptr<const ShadowModelResourceStore> resources,
      std::shared_ptr<const ShadowPoseStore> poses,
      std::shared_ptr<const ShadowAttachmentRigidStore> attachments);
  void ensureLatestFrameBuilt();
  void drainPendingBuildForControlPlane(uint32_t maxChunks,
                                        uint64_t maxTotalBudgetUs,
                                        uint64_t recordCeiling);
  void ensureFrameBuiltForContract(
      std::shared_ptr<const ShadowFrameManifest> manifest,
      std::shared_ptr<const ShadowModelResourceStore> resources,
      std::shared_ptr<const ShadowPoseStore> poses,
      std::shared_ptr<const ShadowAttachmentRigidStore> attachments);
  void runObserveValidation();
  void reset();

  ShadowValidationFrameStats snapshot() const;
  ShadowValidationBuildState buildStateSnapshot() const;
  ShadowSubmissionFrame snapshotFrame() const;
  std::shared_ptr<const ShadowSubmissionFrame> snapshotFrameShared() const;
  std::shared_ptr<const ShadowSubmissionFrame> snapshotRenderableFrameShared()
      const;

private:
  ShadowValidationRuntime() = default;

  void clearPendingBuildLocked();

  mutable std::mutex m_mutex;
  bool m_buildInProgress = false;
  uint64_t m_buildFrameSerial = 0;
  uint64_t m_buildPublishRevision = 0;
  std::shared_ptr<const ShadowFrameManifest> m_pendingManifest;
  std::shared_ptr<const ShadowModelResourceStore> m_pendingResources;
  std::shared_ptr<const ShadowPoseStore> m_pendingPoses;
  std::shared_ptr<const ShadowAttachmentRigidStore> m_pendingAttachments;
  std::shared_ptr<ShadowValidationBuildWork> m_buildWork;
  uint64_t m_lastBuildDurationUs = 0;
  uint64_t m_stalePendingBuildClearedCount = 0;
  ShadowValidationFrameStats m_lastStats = {};
  std::shared_ptr<ShadowSubmissionFrame> m_lastFrame =
      std::make_shared<ShadowSubmissionFrame>();
  std::shared_ptr<ShadowSubmissionFrame> m_lastRenderableFrame =
      std::make_shared<ShadowSubmissionFrame>();
  ShadowRendererCore m_core = {};
};

} // namespace dxvk::war3::shadow
