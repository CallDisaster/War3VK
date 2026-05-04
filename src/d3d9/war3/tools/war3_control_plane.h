#pragma once

#include "war3_diagnostics_hub.h"

#include "../render/war3_shadow_runtime_bridge.h"

#include <cstdint>
#include <string>

namespace dxvk::war3::tools {

struct War3FrameManifestSummary {
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
  uint64_t itemCount = 0;
  uint64_t effectCount = 0;
  uint64_t unknownCount = 0;
  uint64_t rootUnitSupplementSeedCount = 0;
  uint64_t rootUnitSupplementUnitSeedCount = 0;
  uint64_t rootUnitSupplementSkippedNoIdentity = 0;
  uint64_t rootUnitSupplementSkippedAttachmentChild = 0;
  uint64_t rootUnitSupplementSkippedNoPose = 0;
  uint64_t rootUnitSupplementSkippedNoResource = 0;
  uint64_t rootUnitSupplementSkippedNoGeoset = 0;
  uint64_t rootUnitSupplementSkippedNoGeosetZeroCount = 0;
  uint64_t rootUnitSupplementSkippedNoGeosetStoreMiss = 0;
  uint64_t rootUnitSupplementSkippedNoGeosetNotReady = 0;
  uint64_t rootUnitSupplementSkippedDuplicate = 0;
  uint64_t rootUnitSupplementAppended = 0;
  uint64_t rootUnitSupplementReusedFromPrior = 0;
  uint64_t rootUnitSupplementResourceCacheMiss = 0;
  uint64_t rootUnitSupplementResourceCacheNotReady = 0;
  uint64_t rootUnitSupplementResourceSemanticKeyResolved = 0;
  uint64_t rootUnitSupplementResourceSemanticKeyReady = 0;
  uint64_t rootUnitSupplementGeosetCacheFallback = 0;
  uint64_t directPoseSupplementAttemptCount = 0;
  uint64_t directPoseSupplementResolvedCount = 0;
  uint64_t directPoseSupplementSkippedExisting = 0;
  uint64_t directPoseSupplementSkippedInvalid = 0;
};

void InitializeWar3ControlPlane();
void ShutdownWar3ControlPlane();
void ResetWar3ControlPlaneState();
bool IsWar3ControlPlaneRunning();
std::string GetWar3ControlPlanePipeName();

War3FrameManifestSummary QueryFrameManifestSummary();
render::ShadowRuntimeBridgeSummary QueryShadowRuntimeSummary(
    bool refreshSemanticFrameIfStale = false);

} // namespace dxvk::war3::tools
