#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace dxvk {
class D3D9DeviceEx;
class D3D9Surface;
}

namespace dxvk::war3::tools {

struct War3FrameCaptureResult {
  bool handled = false;
  bool ok = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t shadowFrameSerial = 0;
  uint64_t terrainS1CaptureAttemptCount = 0;
  uint64_t terrainS1CaptureAcceptedCount = 0;
  uint64_t terrainS1WorldIdentityLikeCount = 0;
  uint64_t terrainS1WorldNonIdentityCount = 0;
  uint64_t terrainS1WorldNonFiniteCount = 0;
  uint64_t terrainS1ForceIdentityWorldCount = 0;
  uint64_t terrainS1WorldMatrixHash = 0;
  uint64_t terrainS1WorldTranslationMilliMax = 0;
  std::array<uint64_t, 33> shadowCasterStageHistogram = {};
  std::array<uint64_t, 7> shadowCasterCategoryHistogram = {};
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
  uint64_t shadowCasterCount = 0;
  uint64_t shadowReplayDrawCount = 0;
  uint64_t shadowMapDrawnCasterCount = 0;
  uint64_t shadowReceiverActiveStrengthMilli = 0;
  uint64_t shadowReceiverUboStrengthMilli = 0;
  uint64_t shadowReceiverNeedPass = 0;
  uint64_t shadowMapExecutedThisFrame = 0;
  uint64_t shadowVisibilityExecutedThisFrame = 0;
  uint64_t shadowReceiverDrawExecutedThisFrame = 0;
  uint64_t shadowTaaMode = 0;
  uint64_t shadowHistoryValidBefore = 0;
  uint64_t shadowHistoryValidAfter = 0;
  uint64_t shadowHistoryReadIndex = 0;
  uint64_t shadowHistoryWriteIndex = 0;
  uint64_t shadowHistoryAdvancedThisFrame = 0;
  uint64_t shadowHistoryAdvanceSkippedIncomplete = 0;
  uint64_t shadowReceiverSampleSource = 0;
  uint64_t shadowMatrixSceneKey = 0;
  uint64_t receiverCameraHash = 0;
  uint64_t receiverSunDirectionHash = 0;
  uint64_t receiverCsmHash = 0;
  uint64_t receiverCameraDeltaNano = 0;
  uint64_t receiverSunDeltaNano = 0;
  uint64_t receiverCsmDeltaNano = 0;
  uint64_t receiverSnappedCenterDeltaTexelsNano = 0;
  uint64_t receiverTexelSizeDeltaNano = 0;
  uint64_t shadowHistoryInvalidationMask = 0;
  uint64_t replayBackingHash = 0;
  uint64_t stage13ReplayContentHash = 0;
  uint64_t stage13ReplayBackingHash = 0;
  uint64_t stage13ReplayDrawCount = 0;
  uint64_t shadowMapRenderSerial = 0;
  std::string requestId;
  std::string outputPath;
  std::string error;
};

bool SubmitFrameCaptureRequest(const std::string& requestId,
                               const std::string& outputPath,
                               uint32_t timeoutMs = 8000,
                               War3FrameCaptureResult* outResult = nullptr);

bool HasPendingFrameCaptureRequest();

bool ProcessPendingFrameCapture(D3D9DeviceEx* device, D3D9Surface* sourceSurface,
                                War3FrameCaptureResult* outResult = nullptr);

} // namespace dxvk::war3::tools
