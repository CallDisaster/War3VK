#include "war3_diagnostics_hub.h"

#include "../../d3d9_war3_debug.h"

#include "../platform/war3_module_api.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_events.h"
#include "../core/war3_memory.h"
#include "../core/war3_net_event_hook.h"
#include "../core/war3_runtime_profile.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../shadow/war3_shadow_runtime_contract.h"
#include "../state/war3_render_state.h"
#include "war3_perf_monitor.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>

namespace dxvk::war3::tools {

namespace {
using json = nlohmann::json;

std::atomic<bool> s_inGameRenderReady{false};

bool LooksLikeRuntimeModelForDiagnostics(void* candidate) {
  if (candidate == nullptr)
    return false;

  const uintptr_t value = reinterpret_cast<uintptr_t>(candidate);
  if (value < 0x10000u)
    return false;

  uint32_t runtimeGeosetCount = 0;
  void* runtimeGeosets = nullptr;
  const bool hasRuntimeGeosets =
      dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CModelOffsets::RuntimeGeosetCount,
          runtimeGeosetCount) &&
      runtimeGeosetCount > 0u && runtimeGeosetCount < 4096u &&
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::RuntimeGeosets,
          runtimeGeosets) &&
      runtimeGeosets != nullptr &&
      dxvk::war3::IsReadableRangeFast(
          runtimeGeosets,
          size_t((runtimeGeosetCount > 4u ? 4u : runtimeGeosetCount)) *
              sizeof(void*));

  uint32_t finalPoseMatrixCount = 0;
  void* finalPoseMatrixArray = nullptr;
  const bool hasFinalPoseArray =
      dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          finalPoseMatrixCount) &&
      finalPoseMatrixCount > 0u && finalPoseMatrixCount <= 512u &&
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          finalPoseMatrixArray) &&
      finalPoseMatrixArray != nullptr &&
      dxvk::war3::IsReadableRangeFast(finalPoseMatrixArray,
                                      sizeof(float) * 16u);

  return hasRuntimeGeosets || hasFinalPoseArray;
}

void* ScanRuntimeModelCandidateForDiagnostics(void* owner, size_t maxOffset,
                                              uint32_t& outOffset) {
  outOffset = 0xFFFFFFFFu;
  if (owner == nullptr)
    return nullptr;

  if (LooksLikeRuntimeModelForDiagnostics(owner)) {
    outOffset = 0u;
    return owner;
  }

  for (size_t offset = 0u; offset <= maxOffset; offset += sizeof(void*)) {
    void* candidate = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(owner, offset, candidate) ||
        candidate == nullptr) {
      continue;
    }
    if (!LooksLikeRuntimeModelForDiagnostics(candidate))
      continue;

    outOffset = static_cast<uint32_t>(offset);
    return candidate;
  }

  return nullptr;
}

bool CaptureGeosetDataSampleForDiagnostics(
    void* geosetDataPtr, War3RuntimeStatusFrameSnapshot& summary) {
  if (geosetDataPtr == nullptr)
    return false;

  uint32_t vertexCount = 0;
  uint32_t primitiveCount = 0;
  uint32_t matrixGroupCount = 0;
  uint32_t matrixIndexCount = 0;
  void* positions = nullptr;
  void* primitiveRecords = nullptr;
  void* matrixGroupSizes = nullptr;
  void* matrixIndices = nullptr;

  if (!dxvk::war3::SafeReadU32Fast(
          geosetDataPtr, dxvk::war3::CGeosetDataOffsets::VertexCount,
          vertexCount) ||
      !dxvk::war3::SafeReadPtrFast(
          geosetDataPtr, dxvk::war3::CGeosetDataOffsets::VertexPositions,
          positions) ||
      !dxvk::war3::SafeReadU32Fast(
          geosetDataPtr, dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
          primitiveCount) ||
      !dxvk::war3::SafeReadPtrFast(
          geosetDataPtr, dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
          primitiveRecords) ||
      !dxvk::war3::SafeReadU32Fast(
          geosetDataPtr, dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
          matrixGroupCount) ||
      !dxvk::war3::SafeReadPtrFast(
          geosetDataPtr, dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
          matrixGroupSizes) ||
      !dxvk::war3::SafeReadU32Fast(
          geosetDataPtr, dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
          matrixIndexCount) ||
      !dxvk::war3::SafeReadPtrFast(
          geosetDataPtr, dxvk::war3::CGeosetDataOffsets::MatrixIndices,
          matrixIndices)) {
    return false;
  }

  const bool sane =
      vertexCount > 0u && vertexCount < (1u << 20) &&
      primitiveCount > 0u && primitiveCount < (1u << 16) &&
      matrixGroupCount > 0u && matrixGroupCount < 4096u &&
      matrixIndexCount > 0u && matrixIndexCount < (1u << 16) &&
      positions != nullptr && primitiveRecords != nullptr &&
      matrixGroupSizes != nullptr && matrixIndices != nullptr &&
      dxvk::war3::IsReadableRangeFast(positions, 12u) &&
      dxvk::war3::IsReadableRangeFast(primitiveRecords, 8u) &&
      dxvk::war3::IsReadableRangeFast(matrixGroupSizes, sizeof(uint32_t)) &&
      dxvk::war3::IsReadableRangeFast(matrixIndices, sizeof(uint32_t));

  if (!sane)
    return false;

  summary.sampleUnitGeosetVertexCount = vertexCount;
  summary.sampleUnitGeosetPrimitiveCount = primitiveCount;
  summary.sampleUnitGeosetMatrixGroupCount = matrixGroupCount;
  summary.sampleUnitGeosetMatrixIndexCount = matrixIndexCount;
  summary.sampleUnitMeshDataLooksLikeGeosetData = true;
  return true;
}

void CaptureUnitMeshSample(const dxvk::war3::shadow::ShadowRenderableRecord& record,
                           War3RuntimeStatusFrameSnapshot& summary) {
  if (summary.sampleUnitMeshData != 0u || record.meshData == nullptr)
    return;

  summary.sampleUnitSceneNode =
      reinterpret_cast<uint64_t>(record.sceneNode);
  summary.sampleUnitWorldObjectEntry =
      reinterpret_cast<uint64_t>(record.worldObjectEntry);
  summary.sampleUnitUnitPtr = reinterpret_cast<uint64_t>(record.unitPtr);
  summary.sampleUnitMeshData = reinterpret_cast<uint64_t>(record.meshData);
  summary.sampleUnitRuntimeModel =
      reinterpret_cast<uint64_t>(record.runtimeModelPtr);
  summary.sampleUnitModelResource =
      reinterpret_cast<uint64_t>(record.modelResourcePtr);
  summary.sampleUnitJHandle = record.jHandle;
  summary.sampleUnitRawcode = record.rawcode;
  summary.sampleUnitGeosetIndex = record.geosetIndex;
  CaptureGeosetDataSampleForDiagnostics(record.meshData, summary);

  uint32_t sceneNodeRuntimeOffset = 0xFFFFFFFFu;
  if (void* sceneNodeRuntimeCandidate =
          ScanRuntimeModelCandidateForDiagnostics(record.sceneNode, 0x80u,
                                                 sceneNodeRuntimeOffset)) {
    summary.sampleUnitSceneNodeRuntimeCandidate =
        reinterpret_cast<uint64_t>(sceneNodeRuntimeCandidate);
    summary.sampleUnitSceneNodeRuntimeOffset = sceneNodeRuntimeOffset;
  }

  uint32_t entryRuntimeOffset = 0xFFFFFFFFu;
  if (void* entryRuntimeCandidate = ScanRuntimeModelCandidateForDiagnostics(
          record.worldObjectEntry, 0x80u, entryRuntimeOffset)) {
    summary.sampleUnitWorldObjectEntryRuntimeCandidate =
        reinterpret_cast<uint64_t>(entryRuntimeCandidate);
    summary.sampleUnitWorldObjectEntryRuntimeOffset = entryRuntimeOffset;
  }

  uint32_t meshIndex = 0;
  if (dxvk::war3::SafeReadU32Fast(
          record.meshData, dxvk::war3::MeshDataOffsets::MeshIndex,
          meshIndex)) {
    summary.sampleUnitMeshIndex = meshIndex;
    summary.sampleUnitMeshIndexReadable = true;
  }

  void* poseCtx = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(
          record.meshData, dxvk::war3::MeshDataOffsets::TransformOrPoseCtx,
          poseCtx) ||
      poseCtx == nullptr) {
    return;
  }

  summary.sampleUnitPoseCtx = reinterpret_cast<uint64_t>(poseCtx);
  if (LooksLikeRuntimeModelForDiagnostics(poseCtx)) {
    summary.sampleUnitPoseCtxRuntimeCandidate =
        reinterpret_cast<uint64_t>(poseCtx);
    summary.sampleUnitPoseCtxRuntimeOffset = 0u;
    return;
  }

  uint32_t poseRuntimeOffset = 0xFFFFFFFFu;
  if (void* poseRuntimeCandidate =
          ScanRuntimeModelCandidateForDiagnostics(poseCtx, 0x60u,
                                                 poseRuntimeOffset)) {
    summary.sampleUnitPoseCtxRuntimeCandidate =
        reinterpret_cast<uint64_t>(poseRuntimeCandidate);
    summary.sampleUnitPoseCtxRuntimeOffset = poseRuntimeOffset;
  }
}

const char* ModuleStateToString(war3module::War3ModuleRuntimeState state) {
  switch (state) {
  case war3module::War3ModuleRuntimeState::Cold:
    return "Cold";
  case war3module::War3ModuleRuntimeState::Running:
    return "Running";
  case war3module::War3ModuleRuntimeState::ShuttingDown:
    return "ShuttingDown";
  default:
    return "Unknown";
  }
}

std::string GetWarVkTempRuntimePath() {
  char exePath[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) <= 0)
    return {};
  std::string exeDir(exePath);
  size_t pos = exeDir.find_last_of("\\/");
  if (pos == std::string::npos)
    return {};
  exeDir = exeDir.substr(0, pos + 1);

  const std::string warvkDir = exeDir + "WarVK\\";
  const std::string tempDir = warvkDir + "Temp\\";
  CreateDirectoryA(warvkDir.c_str(), nullptr);
  CreateDirectoryA(tempDir.c_str(), nullptr);
  return tempDir + "runtime_status.json";
}

War3RuntimeStatusFrameSnapshot BuildFrameSnapshot() {
  War3RuntimeStatusFrameSnapshot summary = {};
  const auto manifest =
      dxvk::war3::shadow::ShadowRuntimeContractCache::instance()
          .snapshotManifestShared();
  if (manifest == nullptr)
    return summary;

  summary.frameNumber = manifest->frameSerial;
  summary.publishRevision = manifest->publishRevision;
  summary.visibleCount = manifest->visibleCount;
  summary.mainQueueCount = manifest->mainQueueCount;
  summary.transparentCount = manifest->transparentCount;

  for (const auto& record : manifest->records) {
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
      CaptureUnitMeshSample(record, summary);
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

War3RuntimeStatusShadowSnapshot BuildShadowSnapshot() {
  War3RuntimeStatusShadowSnapshot summary = {};
  const auto bridgeSummary = dxvk::war3::render::QueryShadowRuntimeBridgeSummary();
  summary.matrixPaletteCount = bridgeSummary.matrixPaletteCount;
  summary.shadowReadyGeosetCount = bridgeSummary.shadowReadyGeosetCount;
  summary.shadowModelResourceCount = bridgeSummary.shadowModelResourceCount;
  summary.shadowRuntimeModelCount = bridgeSummary.shadowRuntimeModelCount;
  summary.upperLayerResolveAuthoritativeRigid =
      bridgeSummary.upperLayerResolveAuthoritativeRigid;
  summary.upperLayerResolveAuthoritativeSkinned =
      bridgeSummary.upperLayerResolveAuthoritativeSkinned;
  summary.upperLayerResolvedAuthoritativeItems =
      bridgeSummary.upperLayerResolvedAuthoritativeItems;
  summary.upperLayerEmitted = bridgeSummary.upperLayerEmitted;
  summary.semanticCoreFrameSerial = bridgeSummary.semanticCoreFrameSerial;
  summary.semanticCoreResolved = bridgeSummary.semanticCoreResolved;
  summary.semanticCoreSkinnedResolved =
      bridgeSummary.semanticCoreSkinnedResolved;
  summary.semanticCoreExplicitResourceOwnerRigidResolved =
      bridgeSummary.semanticCoreExplicitResourceOwnerRigidResolved;
  summary.semanticCoreExplicitResourceOwnerRigidWorldTransformResolved =
      bridgeSummary.semanticCoreExplicitResourceOwnerRigidWorldTransformResolved;
  summary.semanticCoreExplicitResourceOwnerRigidNoMatrixPalette =
      bridgeSummary.semanticCoreExplicitResourceOwnerRigidNoMatrixPalette;
  summary.semanticCoreSubmittedDrawCount =
      bridgeSummary.semanticCoreSubmittedDrawCount;
  summary.semanticCoreSkippedNoRuntimeGroupPalette =
      bridgeSummary.semanticCoreSkippedNoRuntimeGroupPalette;
  summary.fallbackDrawCount = bridgeSummary.fallbackDrawCount;
  summary.fallbackDrawCountTerrain = bridgeSummary.fallbackDrawCountTerrain;
  summary.fallbackDrawCountWorldObject =
      bridgeSummary.fallbackDrawCountWorldObject;
  summary.fallbackDrawCountUnitObject =
      bridgeSummary.fallbackDrawCountUnitObject;
  summary.objectFallbackDrawCount = bridgeSummary.objectFallbackDrawCount;
  summary.semanticSceneSubmitted = bridgeSummary.semanticSceneSubmitted;
  summary.semanticSceneSubmittedUnit = bridgeSummary.semanticSceneSubmittedUnit;
  summary.semanticSceneSubmittedSkinned =
      bridgeSummary.semanticSceneSubmittedSkinned;
  summary.semanticSceneSubmittedFrameLocal =
      bridgeSummary.semanticSceneSubmittedFrameLocal;
  summary.semanticSceneSubmittedPersistent =
      bridgeSummary.semanticSceneSubmittedPersistent;
  summary.semanticSceneStatsPublishCount =
      bridgeSummary.semanticSceneStatsPublishCount;
  summary.semanticSceneLastFrameSerial =
      bridgeSummary.semanticSceneLastFrameSerial;
  summary.semanticSceneLastSelectedFrameSerial =
      bridgeSummary.semanticSceneLastSelectedFrameSerial;
  summary.semanticSceneLastReusableFrameSerial =
      bridgeSummary.semanticSceneLastReusableFrameSerial;
  summary.semanticSceneLastSourcePublishRevision =
      bridgeSummary.semanticSceneLastSourcePublishRevision;
  summary.semanticSceneLastTargetPublishRevision =
      bridgeSummary.semanticSceneLastTargetPublishRevision;
  summary.semanticSceneLastInputDrawCount =
      bridgeSummary.semanticSceneLastInputDrawCount;
  summary.semanticSceneLastSubmittedDrawCount =
      bridgeSummary.semanticSceneLastSubmittedDrawCount;
  summary.semanticSceneSelectedFrameEligibleZeroCount =
      bridgeSummary.semanticSceneSelectedFrameEligibleZeroCount;
  summary.semanticSceneReusableFrameForcedCount =
      bridgeSummary.semanticSceneReusableFrameForcedCount;
  summary.semanticSceneReusableFrameUnavailableCount =
      bridgeSummary.semanticSceneReusableFrameUnavailableCount;
  summary.semanticSceneReusableFrameRejectedNativeValidationCount =
      bridgeSummary.semanticSceneReusableFrameRejectedNativeValidationCount;
  summary.semanticScenePublishRevisionLag =
      bridgeSummary.semanticScenePublishRevisionLag;
  summary.semanticFallbackPruned = bridgeSummary.semanticFallbackPruned;
  summary.semanticCoreFrameFresh = bridgeSummary.semanticCoreFrameFresh;
  summary.semanticCoreBuildInProgress =
      bridgeSummary.semanticCoreBuildInProgress;
  summary.semanticCoreBuildRequestPending =
      bridgeSummary.semanticCoreBuildRequestPending;
  summary.semanticCoreBuildCurrentRecordIndex =
      bridgeSummary.semanticCoreBuildCurrentRecordIndex;
  summary.semanticCoreBuildRecordCount =
      bridgeSummary.semanticCoreBuildRecordCount;
  summary.semanticCoreBuildChunkCount =
      bridgeSummary.semanticCoreBuildChunkCount;
  summary.semanticStaticCandidateCount =
      bridgeSummary.semanticStaticCandidateCount;
  summary.semanticStaticCandidateBuildingCount =
      bridgeSummary.semanticStaticCandidateBuildingCount;
  summary.semanticStaticCandidateDestructibleCount =
      bridgeSummary.semanticStaticCandidateDestructibleCount;
  summary.semanticStaticCandidateMaybeDoodadOrEffectCount =
      bridgeSummary.semanticStaticCandidateMaybeDoodadOrEffectCount;
  summary.semanticStaticCandidateWithStableIdentity =
      bridgeSummary.semanticStaticCandidateWithStableIdentity;
  summary.semanticStaticCandidateWithMeshData =
      bridgeSummary.semanticStaticCandidateWithMeshData;
  summary.semanticStaticCandidateWithRuntimeModel =
      bridgeSummary.semanticStaticCandidateWithRuntimeModel;
  summary.semanticStaticCandidateWithModelResource =
      bridgeSummary.semanticStaticCandidateWithModelResource;
  summary.semanticStaticCandidateWithResolvedGeoset =
      bridgeSummary.semanticStaticCandidateWithResolvedGeoset;
  summary.semanticStaticCandidateRejectedUnitsOnlyFilter =
      bridgeSummary.semanticStaticCandidateRejectedUnitsOnlyFilter;
  summary.semanticStaticCandidateRejectedNoIdentity =
      bridgeSummary.semanticStaticCandidateRejectedNoIdentity;
  summary.semanticStaticCandidateRejectedNoMeshData =
      bridgeSummary.semanticStaticCandidateRejectedNoMeshData;
  summary.semanticStaticCandidateRejectedNoResource =
      bridgeSummary.semanticStaticCandidateRejectedNoResource;
  summary.semanticStaticCandidateRejectedNoGeoset =
      bridgeSummary.semanticStaticCandidateRejectedNoGeoset;
  summary.semanticStaticCandidateRejectedNonCanonicalKind =
      bridgeSummary.semanticStaticCandidateRejectedNonCanonicalKind;
  return summary;
}

json BuildRuntimeStatusJson(const War3RuntimeStatusSnapshot& snapshot) {
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
         {"sampleUnitSceneNode", snapshot.frame.sampleUnitSceneNode},
         {"sampleUnitWorldObjectEntry",
          snapshot.frame.sampleUnitWorldObjectEntry},
         {"sampleUnitUnitPtr", snapshot.frame.sampleUnitUnitPtr},
         {"sampleUnitMeshData", snapshot.frame.sampleUnitMeshData},
         {"sampleUnitRuntimeModel", snapshot.frame.sampleUnitRuntimeModel},
         {"sampleUnitModelResource", snapshot.frame.sampleUnitModelResource},
         {"sampleUnitPoseCtx", snapshot.frame.sampleUnitPoseCtx},
         {"sampleUnitPoseCtxRuntimeCandidate",
          snapshot.frame.sampleUnitPoseCtxRuntimeCandidate},
         {"sampleUnitSceneNodeRuntimeCandidate",
          snapshot.frame.sampleUnitSceneNodeRuntimeCandidate},
         {"sampleUnitWorldObjectEntryRuntimeCandidate",
          snapshot.frame.sampleUnitWorldObjectEntryRuntimeCandidate},
         {"sampleUnitJHandle", snapshot.frame.sampleUnitJHandle},
         {"sampleUnitRawcode", snapshot.frame.sampleUnitRawcode},
         {"sampleUnitMeshIndex", snapshot.frame.sampleUnitMeshIndex},
         {"sampleUnitGeosetIndex", snapshot.frame.sampleUnitGeosetIndex},
         {"sampleUnitPoseCtxRuntimeOffset",
          snapshot.frame.sampleUnitPoseCtxRuntimeOffset},
         {"sampleUnitSceneNodeRuntimeOffset",
          snapshot.frame.sampleUnitSceneNodeRuntimeOffset},
         {"sampleUnitWorldObjectEntryRuntimeOffset",
          snapshot.frame.sampleUnitWorldObjectEntryRuntimeOffset},
         {"sampleUnitGeosetVertexCount",
          snapshot.frame.sampleUnitGeosetVertexCount},
         {"sampleUnitGeosetPrimitiveCount",
          snapshot.frame.sampleUnitGeosetPrimitiveCount},
         {"sampleUnitGeosetMatrixGroupCount",
          snapshot.frame.sampleUnitGeosetMatrixGroupCount},
         {"sampleUnitGeosetMatrixIndexCount",
          snapshot.frame.sampleUnitGeosetMatrixIndexCount},
         {"sampleUnitMeshIndexReadable",
          snapshot.frame.sampleUnitMeshIndexReadable},
         {"sampleUnitMeshDataLooksLikeGeosetData",
          snapshot.frame.sampleUnitMeshDataLooksLikeGeosetData},
         {"itemCount", snapshot.frame.itemCount},
        {"effectCount", snapshot.frame.effectCount},
        {"unknownCount", snapshot.frame.unknownCount}}},
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
        {"fallbackDrawCount", snapshot.shadow.fallbackDrawCount},
        {"fallbackDrawCountTerrain", snapshot.shadow.fallbackDrawCountTerrain},
        {"fallbackDrawCountWorldObject",
         snapshot.shadow.fallbackDrawCountWorldObject},
        {"fallbackDrawCountUnitObject",
         snapshot.shadow.fallbackDrawCountUnitObject},
        {"objectFallbackDrawCount",
         snapshot.shadow.objectFallbackDrawCount},
        {"semanticSceneSubmitted", snapshot.shadow.semanticSceneSubmitted},
        {"semanticSceneSubmittedUnit",
         snapshot.shadow.semanticSceneSubmittedUnit},
        {"semanticSceneSubmittedSkinned",
         snapshot.shadow.semanticSceneSubmittedSkinned},
        {"semanticSceneSubmittedFrameLocal",
         snapshot.shadow.semanticSceneSubmittedFrameLocal},
        {"semanticSceneSubmittedPersistent",
         snapshot.shadow.semanticSceneSubmittedPersistent},
        {"semanticSceneStatsPublishCount",
         snapshot.shadow.semanticSceneStatsPublishCount},
        {"semanticSceneLastFrameSerial",
         snapshot.shadow.semanticSceneLastFrameSerial},
        {"semanticSceneLastSelectedFrameSerial",
         snapshot.shadow.semanticSceneLastSelectedFrameSerial},
        {"semanticSceneLastReusableFrameSerial",
         snapshot.shadow.semanticSceneLastReusableFrameSerial},
        {"semanticSceneLastSourcePublishRevision",
         snapshot.shadow.semanticSceneLastSourcePublishRevision},
        {"semanticSceneLastTargetPublishRevision",
         snapshot.shadow.semanticSceneLastTargetPublishRevision},
        {"semanticSceneLastInputDrawCount",
         snapshot.shadow.semanticSceneLastInputDrawCount},
        {"semanticSceneLastSubmittedDrawCount",
         snapshot.shadow.semanticSceneLastSubmittedDrawCount},
        {"semanticSceneSelectedFrameEligibleZeroCount",
         snapshot.shadow.semanticSceneSelectedFrameEligibleZeroCount},
        {"semanticSceneReusableFrameForcedCount",
         snapshot.shadow.semanticSceneReusableFrameForcedCount},
        {"semanticSceneReusableFrameUnavailableCount",
         snapshot.shadow.semanticSceneReusableFrameUnavailableCount},
        {"semanticSceneReusableFrameRejectedNativeValidationCount",
         snapshot.shadow
             .semanticSceneReusableFrameRejectedNativeValidationCount},
        {"semanticScenePublishRevisionLag",
         snapshot.shadow.semanticScenePublishRevisionLag},
        {"semanticFallbackPruned", snapshot.shadow.semanticFallbackPruned},
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
        {"semanticStaticCandidateCount",
         snapshot.shadow.semanticStaticCandidateCount},
        {"semanticStaticCandidateBuildingCount",
         snapshot.shadow.semanticStaticCandidateBuildingCount},
        {"semanticStaticCandidateDestructibleCount",
         snapshot.shadow.semanticStaticCandidateDestructibleCount},
        {"semanticStaticCandidateMaybeDoodadOrEffectCount",
         snapshot.shadow.semanticStaticCandidateMaybeDoodadOrEffectCount},
        {"semanticStaticCandidateWithStableIdentity",
         snapshot.shadow.semanticStaticCandidateWithStableIdentity},
        {"semanticStaticCandidateWithMeshData",
         snapshot.shadow.semanticStaticCandidateWithMeshData},
        {"semanticStaticCandidateWithRuntimeModel",
         snapshot.shadow.semanticStaticCandidateWithRuntimeModel},
        {"semanticStaticCandidateWithModelResource",
         snapshot.shadow.semanticStaticCandidateWithModelResource},
        {"semanticStaticCandidateWithResolvedGeoset",
         snapshot.shadow.semanticStaticCandidateWithResolvedGeoset},
        {"semanticStaticCandidateRejectedUnitsOnlyFilter",
         snapshot.shadow.semanticStaticCandidateRejectedUnitsOnlyFilter},
        {"semanticStaticCandidateRejectedNoIdentity",
         snapshot.shadow.semanticStaticCandidateRejectedNoIdentity},
        {"semanticStaticCandidateRejectedNoMeshData",
         snapshot.shadow.semanticStaticCandidateRejectedNoMeshData},
        {"semanticStaticCandidateRejectedNoResource",
         snapshot.shadow.semanticStaticCandidateRejectedNoResource},
        {"semanticStaticCandidateRejectedNoGeoset",
         snapshot.shadow.semanticStaticCandidateRejectedNoGeoset},
        {"semanticStaticCandidateRejectedNonCanonicalKind",
         snapshot.shadow.semanticStaticCandidateRejectedNonCanonicalKind}}}};
}

War3RuntimeStatusSnapshot BuildRuntimeStatusSnapshot(const char* source,
                                                     uint64_t frameIndex) {
  War3RuntimeStatusSnapshot snapshot = {};
  using namespace std::chrono;
  snapshot.timestampMs = static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
  snapshot.source = ((source && source[0]) ? source : "unknown");
  if (frameIndex == 0)
    frameIndex = dxvk::war3::state::RenderState::instance().getFrameIndex();
  snapshot.frameIndex = frameIndex;

  const auto stats = war3module::GetModuleRuntimeStats();
  auto& perf = War3PerfMonitor::instance();
  const bool jassReady = dxvk::war3::War3Events::get().isJassReady();
  const bool runtimeReady =
      dxvk::war3::NetEventHook::get().IsRuntimeReady() ||
      s_inGameRenderReady.load(std::memory_order_relaxed);

  snapshot.module.registered = static_cast<uint32_t>(stats.registeredModules);
  snapshot.module.loaded = static_cast<uint32_t>(stats.loadedModules);
  snapshot.module.dispatchCalls =
      static_cast<uint64_t>(stats.dispatchCalls);
  snapshot.module.handlers =
      static_cast<uint64_t>(stats.dispatchedHandlers);
  snapshot.module.callbackErrors =
      static_cast<uint64_t>(stats.callbackErrors);
  snapshot.module.state = ModuleStateToString(stats.state);

  snapshot.perf.enabled = perf.isEnabled();
  snapshot.perf.recording = perf.isRecording();

  snapshot.profile.name = dxvk::war3::runtime::GetWar3RuntimeProfileName();
  snapshot.profile.disabledModules =
      dxvk::war3::runtime::GetWar3RuntimeDisabledModulesCsv();
  snapshot.profile.enabledModules =
      dxvk::war3::runtime::GetWar3RuntimeEnabledModulesCsv();

  snapshot.runtime.runtimeReady = runtimeReady;
  snapshot.runtime.jassReady = jassReady;
  snapshot.runtime.gameStarted = dxvk::war3::War3Events::get().isGameStarted();

  auto& renderState = dxvk::war3::state::RenderState::instance();
  snapshot.render.inGameRenderReady =
      s_inGameRenderReady.load(std::memory_order_relaxed);
  snapshot.render.isInGame = renderState.isInGame();
  snapshot.render.isLoading = renderState.isLoading();
  snapshot.render.worldPtr =
      reinterpret_cast<uint64_t>(renderState.getWorldPointer());
  snapshot.frame = BuildFrameSnapshot();
  snapshot.shadow = BuildShadowSnapshot();
  return snapshot;
}

void WriteRuntimeStatusSnapshot(const War3RuntimeStatusSnapshot& snapshot) {
  const std::string outPath = GetWarVkTempRuntimePath();
  if (outPath.empty())
    return;

  std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
  if (!f.is_open())
    return;

  f << BuildRuntimeStatusJson(snapshot).dump(2) << '\n';
}
} // namespace

void ExportRuntimeStatusSnapshot(const char* source, uint64_t frameIndex) {
  WriteRuntimeStatusSnapshot(BuildRuntimeStatusSnapshot(source, frameIndex));
}

War3RuntimeStatusSnapshot QueryRuntimeStatusSnapshot(const char* source,
                                                     uint64_t frameIndex) {
  return BuildRuntimeStatusSnapshot(source, frameIndex);
}

void MarkInGameRenderReady(const char* source, uint64_t frameIndex) {
  auto& renderState = dxvk::war3::state::RenderState::instance();
  renderState.setIsInGame(true);
  renderState.setIsLoading(false);

  bool expected = false;
  if (!s_inGameRenderReady.compare_exchange_strong(
          expected, true, std::memory_order_relaxed))
    return;

  if (!dxvk::war3::War3Events::get().isGameStarted()) {
    war3dbg::Print(
        "DXVK War3Hook: Auto-fired OnGameStart via InGameRenderReady source=%s frame=%llu\n",
        (source && source[0]) ? source : "(unknown)",
        static_cast<unsigned long long>(frameIndex));
    dxvk::war3::War3Events::get().fireOnGameStart();
  }

  war3dbg::Print("DXVK War3Diag: InGameRenderReady source=%s frame=%llu\n",
                 (source && source[0]) ? source : "(unknown)",
                 static_cast<unsigned long long>(frameIndex));
  ExportRuntimeStatusSnapshot(source, frameIndex);
}

bool IsInGameRenderReady() {
  return s_inGameRenderReady.load(std::memory_order_relaxed);
}

void ResetRuntimeReadySignals() {
  s_inGameRenderReady.store(false, std::memory_order_relaxed);
}

void LogRuntimeSummaryOnce(const char* source) {
  static std::atomic<bool> s_logged{false};
  bool expected = false;
  if (!s_logged.compare_exchange_strong(expected, true))
    return;

  const auto stats = war3module::GetModuleRuntimeStats();
  auto& perf = War3PerfMonitor::instance();

  war3dbg::Print(
      "DXVK War3Diag: RuntimeSummary source=%s modules=%u loaded=%u "
      "state=%s dispatch=%llu handlers=%llu callbackErr=%llu perfEnabled=%d "
      "perfRecording=%d profile=%s disabled=%s\n",
      (source && source[0]) ? source : "(unknown)",
      static_cast<unsigned>(stats.registeredModules),
      static_cast<unsigned>(stats.loadedModules),
      ModuleStateToString(stats.state),
      static_cast<unsigned long long>(stats.dispatchCalls),
      static_cast<unsigned long long>(stats.dispatchedHandlers),
      static_cast<unsigned long long>(stats.callbackErrors),
      perf.isEnabled() ? 1 : 0, perf.isRecording() ? 1 : 0,
      dxvk::war3::runtime::GetWar3RuntimeProfileName(),
      dxvk::war3::runtime::GetWar3RuntimeDisabledModulesCsv().c_str());

  ExportRuntimeStatusSnapshot(source, 0);
}

void LogRuntimeHealthPeriodic(uint64_t frameIndex, uint32_t interval) {
  // 部分路径下 frameIndex 可能长期为 0，此时按取模会每次都命中，造成刷屏。
  // 这里做去重与零值防抖，保证日志频率稳定。
  static std::atomic<uint64_t> s_lastLoggedFrame{~uint64_t(0)};
  static std::atomic<uint64_t> s_zeroFrameTick{0};

  if (interval == 0)
    return;

  if (frameIndex == 0) {
    const uint64_t tick = s_zeroFrameTick.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((tick % interval) != 0)
      return;
  } else {
    if ((frameIndex % interval) != 0)
      return;

    const uint64_t prev =
        s_lastLoggedFrame.exchange(frameIndex, std::memory_order_relaxed);
    if (prev == frameIndex)
      return;
  }

  const auto stats = war3module::GetModuleRuntimeStats();
  auto& perf = War3PerfMonitor::instance();

  war3dbg::Print(
      "DXVK War3Diag: RuntimeHealth frame=%llu modules=%u loaded=%u state=%s "
      "dispatch=%llu handlers=%llu callbackErr=%llu perfEnabled=%d "
      "perfRecording=%d profile=%s\n",
      static_cast<unsigned long long>(frameIndex),
      static_cast<unsigned>(stats.registeredModules),
      static_cast<unsigned>(stats.loadedModules),
      ModuleStateToString(stats.state),
      static_cast<unsigned long long>(stats.dispatchCalls),
      static_cast<unsigned long long>(stats.dispatchedHandlers),
      static_cast<unsigned long long>(stats.callbackErrors),
      perf.isEnabled() ? 1 : 0, perf.isRecording() ? 1 : 0,
      dxvk::war3::runtime::GetWar3RuntimeProfileName());

  ExportRuntimeStatusSnapshot("periodic", frameIndex);
}

} // namespace dxvk::war3::tools
