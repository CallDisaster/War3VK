#include "war3_diagnostics_hub.h"

#include "../../d3d9_device.h"
#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_shadow.h"

#include "../war3.h"
#include "../memory/war3_cpu_readable_buffer_span.h"
#include "../memory/war3_shadow_arena.h"
#include "../platform/war3_module_api.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_events.h"
#include "../core/war3_memory.h"
#include "../core/war3_net_event_hook.h"
#include "../core/war3_runtime_profile.h"
#include "../gpu_skin/war3_persistent_gpu_package_d3d9_observe_owner.h"
#include "../render/war3_lightning_runtime.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../shadow/war3_shadow_runtime_contract.h"
#include "../state/war3_render_state.h"
#include "war3_perf_monitor.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <array>

namespace dxvk::war3::tools {

namespace {
using json = nlohmann::json;

std::atomic<bool> s_inGameRenderReady{false};
std::atomic<uint64_t> s_shadowEvidenceRetentionRevision{0u};
std::atomic<bool> s_shadowEvidenceCollectorAttached{false};
std::mutex s_gpuFlightMutex;
std::deque<GpuFlightFrame> s_gpuFlightFrames;
uint64_t s_gpuLastCompletedSerial = 0u;
std::chrono::steady_clock::time_point s_gpuLastProgressAt =
    std::chrono::steady_clock::now();
bool s_gpuIncidentLatched = false;
bool s_shadowArenaIncidentLatched = false;
uint64_t s_shadowArenaLastOverflowCount = 0u;
uint64_t s_shadowArenaLastAdmissionRejectedCount = 0u;
uint64_t s_shadowArenaLastPartialTransactionCount = 0u;
std::atomic<uint32_t> s_gpuFlightBreadcrumb{
    static_cast<uint32_t>(GpuFlightBreadcrumb::Idle)};
std::atomic<uint64_t> s_gpuFlightBreadcrumbSerial{0u};
std::atomic<uint32_t> s_gpuFlightActiveCsmCascade{0xFFFFFFFFu};
std::atomic<uint32_t> s_gpuFlightActivePointLight{0xFFFFFFFFu};
std::atomic<uint32_t> s_gpuFlightActivePointFace{0xFFFFFFFFu};
std::array<std::atomic<uint32_t>, 4u> s_gpuFlightCsmCascadeDrawCount = {};
std::array<std::atomic<uint64_t>, 4u> s_gpuFlightCsmCascadeTriangleCount = {};
std::atomic<uint32_t> s_gpuFlightPointShadowLightCount{0u};
std::array<std::atomic<uint32_t>, 24u>
    s_gpuFlightPointShadowFaceCandidateCount = {};
std::array<std::atomic<uint32_t>, 24u> s_gpuFlightPointShadowFaceKeptCount = {};
std::array<std::atomic<uint32_t>, 24u> s_gpuFlightPointShadowFaceDrawCount = {};
std::array<std::atomic<uint64_t>, 24u>
    s_gpuFlightPointShadowFaceTriangleCount = {};
std::atomic<uint32_t> s_gpuFlightAutoTestContextValid{0u};
std::atomic<uint32_t> s_gpuFlightAutoTestWaypointIndex{0xFFFFFFFFu};
std::array<std::atomic<uint32_t>, 11u> s_gpuFlightAutoTestFloatBits = {};
uint64_t s_gpuFlightLastArenaGeneration = 0u;
uint64_t s_gpuFlightLastArenaUsedBytes = 0u;

uint32_t FloatBits(float value) noexcept {
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float FloatFromBits(uint32_t bits) noexcept {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

const char* GpuFlightBreadcrumbName(GpuFlightBreadcrumb value) noexcept {
  switch (value) {
  case GpuFlightBreadcrumb::Idle: return "Idle";
  case GpuFlightBreadcrumb::PipelineBeforeUi: return "BeforeUi.Entry";
  case GpuFlightBreadcrumb::ShadowReceiverEntry: return "ShadowReceiver.Entry";
  case GpuFlightBreadcrumb::CsmPreflight: return "Shadow.CSM.Preflight";
  case GpuFlightBreadcrumb::CsmCascade: return "Shadow.CSM.Cascade";
  case GpuFlightBreadcrumb::CsmTerrainMask: return "Shadow.CSM.TerrainMask";
  case GpuFlightBreadcrumb::VolumeSunShadow: return "Shadow.VolumeSun";
  case GpuFlightBreadcrumb::PointShadowPlan: return "Shadow.Point.Plan";
  case GpuFlightBreadcrumb::PointShadowFace: return "Shadow.Point.Face";
  case GpuFlightBreadcrumb::ShadowCopy: return "Shadow.Copy";
  case GpuFlightBreadcrumb::ShadowMotionVectors: return "Shadow.MotionVectors";
  case GpuFlightBreadcrumb::ShadowVisibility: return "Shadow.Visibility";
  case GpuFlightBreadcrumb::ShadowReceiverDraw: return "Shadow.Receiver";
  case GpuFlightBreadcrumb::ShadowOutline: return "Shadow.Outline";
  case GpuFlightBreadcrumb::VolumetricLight: return "VolumetricLight";
  case GpuFlightBreadcrumb::Ssao: return "SSAO";
  case GpuFlightBreadcrumb::Aa: return "AA";
  case GpuFlightBreadcrumb::ShaderPack: return "ShaderPack";
  case GpuFlightBreadcrumb::BeforeUiPostEvents: return "BeforeUi.PostEvents";
  case GpuFlightBreadcrumb::BeforeUiComplete: return "BeforeUi.Complete";
  }
  return "Unknown";
}

uint64_t EpochMilliseconds() {
  return static_cast<uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

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

std::filesystem::path GetWarVkLogDirectory() {
  char exePath[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) <= 0)
    return {};
  std::filesystem::path directory =
      std::filesystem::path(exePath).parent_path() / "WarVK" / "Log";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return error ? std::filesystem::path{} : directory;
}

void WriteGpuIncidentSnapshot(const GpuIncidentSnapshot& incident) {
  const std::filesystem::path directory = GetWarVkLogDirectory();
  if (directory.empty())
    return;

  json payload = {
      {"timestampMs", incident.timestampMs},
      {"reason", incident.reason},
      {"queueResult", incident.queueResult},
      {"stalledMilliseconds", incident.stalledMilliseconds},
      {"lastRenderStage",
       incident.recentFrames.empty()
           ? std::string{}
           : incident.recentFrames.back().lastRenderStage},
      {"deviceFault", {
          {"supported", false},
          {"reason", "VK_EXT_device_fault is not exposed by this DXVK build"},
      }},
      {"frames", json::array()},
  };
  for (const auto& frame : incident.recentFrames) {
    payload["frames"].push_back({
        {"timestampMs", frame.timestampMs},
        {"frameSerial", frame.frameSerial},
        {"lastRenderStage", frame.lastRenderStage},
        {"breadcrumbSerial", frame.breadcrumbSerial},
        {"activeCsmCascade", frame.activeCsmCascade},
        {"activePointLight", frame.activePointLight},
        {"activePointFace", frame.activePointFace},
        {"autoTestContextValid", frame.autoTestContextValid},
        {"autoTestWaypointIndex", frame.autoTestWaypointIndex},
        {"autoTestTarget", {frame.autoTestTargetX, frame.autoTestTargetY}},
        {"autoTestPanSeconds", frame.autoTestPanSeconds},
        {"cameraTarget", {frame.cameraTargetX, frame.cameraTargetY}},
        {"cameraTargetDistance", frame.cameraTargetDistance},
        {"cameraAngleOfAttack", frame.cameraAngleOfAttack},
        {"worldBounds", {frame.worldMinX, frame.worldMinY,
                         frame.worldMaxX, frame.worldMaxY}},
        {"csmPlannedCasterCount", frame.csmPlannedCasterCount},
        {"csmValidatedCasterCount", frame.csmValidatedCasterCount},
        {"csmDrawnCasterCount", frame.csmDrawnCasterCount},
        {"csmLastRejectReason", frame.csmLastRejectReason},
        {"csmValidationRejectCount", frame.csmValidationRejectCount},
        {"csmPartialPreventedCount", frame.csmPartialPreventedCount},
        {"csmFirstCompleteLatencyFrames",
         frame.csmFirstCompleteLatencyFrames},
        {"csmCascadeDrawCount", frame.csmCascadeDrawCount},
        {"csmCascadeTriangleCount", frame.csmCascadeTriangleCount},
        {"pointShadowLightCount", frame.pointShadowLightCount},
        {"pointShadowFaceCandidateCount",
         frame.pointShadowFaceCandidateCount},
        {"pointShadowFaceKeptCount", frame.pointShadowFaceKeptCount},
        {"pointShadowFaceDrawCount", frame.pointShadowFaceDrawCount},
        {"pointShadowFaceTriangleCount",
         frame.pointShadowFaceTriangleCount},
        {"csmRequestedResolution", frame.csmRequestedResolution},
        {"csmEffectiveResolution", frame.csmEffectiveResolution},
        {"csmFallbackReason", frame.csmFallbackReason},
        {"csmFallbackLatched", frame.csmFallbackLatched},
        {"csmGeneration", frame.csmGeneration},
        {"csmMemoryBudgetBytes", frame.csmMemoryBudgetBytes},
        {"csmMemoryAvailableBytes", frame.csmMemoryAvailableBytes},
        {"taaRequestedMode", frame.taaRequestedMode},
        {"taaEffectiveMode", frame.taaEffectiveMode},
        {"taaShaderMode", frame.taaShaderMode},
        {"taaHistoryValid", frame.taaHistoryValid},
        {"taaHistoryGeneration", frame.taaHistoryGeneration},
        {"arenaUsedBytes", frame.arenaUsedBytes},
        {"arenaFrameUsedDeltaBytes", frame.arenaFrameUsedDeltaBytes},
        {"arenaResidentBytes", frame.arenaResidentBytes},
        {"arenaGeneration", frame.arenaGeneration},
        {"arenaQuarantineCount", frame.arenaQuarantineCount},
        {"arenaQuarantinedRetireSerial",
         frame.arenaQuarantinedRetireSerial},
        {"arenaBusyReuseRejectCount", frame.arenaBusyReuseRejectCount},
        {"arenaOverflowCount", frame.arenaOverflowCount},
        {"arenaReservedBytes", frame.arenaReservedBytes},
        {"arenaCommittedBytes", frame.arenaCommittedBytes},
        {"arenaRolledBackBytes", frame.arenaRolledBackBytes},
        {"arenaAdmissionRejectedCount", frame.arenaAdmissionRejectedCount},
        {"arenaPartialTransactionCount", frame.arenaPartialTransactionCount},
        {"arenaUniqueSourceBytes", frame.arenaUniqueSourceBytes},
        {"arenaDuplicateBytesSaved", frame.arenaDuplicateBytesSaved},
        {"arenaExactIndexTrimAcceptedCount",
         frame.arenaExactIndexTrimAcceptedCount},
        {"arenaExactIndexTrimRejectedCount",
         frame.arenaExactIndexTrimRejectedCount},
        {"arenaExactIndexTrimBytesSaved",
         frame.arenaExactIndexTrimBytesSaved},
        {"exactIndexDomainScannedBytes",
         frame.exactIndexDomainScannedBytes},
        {"exactIndexDomainNonHostCachedScanCount",
         frame.exactIndexDomainNonHostCachedScanCount},
        {"exactIndexDomainNonHostCachedScannedBytes",
         frame.exactIndexDomainNonHostCachedScannedBytes},
        {"exactIndexDomainBulkReadCount",
         frame.exactIndexDomainBulkReadCount},
        {"exactIndexDomainBulkReadBytes",
         frame.exactIndexDomainBulkReadBytes},
        {"exactIndexDomainDirectReadCount",
         frame.exactIndexDomainDirectReadCount},
        {"exactIndexDomainOversizeFallbackCount",
         frame.exactIndexDomainOversizeFallbackCount},
        {"arenaFrameIncomplete", frame.arenaFrameIncomplete},
        {"shadowMapEpoch", frame.shadowMapEpoch},
        {"shadowMapResetRequestedSerial",
         frame.shadowMapResetRequestedSerial},
        {"shadowMapResetAppliedSerial",
         frame.shadowMapResetAppliedSerial},
        {"shadowMapTransitionState", frame.shadowMapTransitionState},
        {"queueSubmittedSerial", frame.queueSubmittedSerial},
        {"queueCompletedSerial", frame.queueCompletedSerial},
        {"queueResult", frame.queueResult},
    });
  }

  const std::string filename =
      "gpu_incident_" + std::to_string(incident.timestampMs) + ".json";
  const std::filesystem::path target = directory / filename;
  const std::filesystem::path temporary = target.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
      return;
    stream << payload.dump(2);
    stream.flush();
    if (!stream)
      return;
  }
  MoveFileExA(temporary.string().c_str(), target.string().c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

  std::vector<std::filesystem::directory_entry> incidents;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (error)
      break;
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file() && name.rfind("gpu_incident_", 0u) == 0u &&
        entry.path().extension() == ".json") {
      incidents.push_back(entry);
    }
  }
  std::sort(incidents.begin(), incidents.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.last_write_time() < rhs.last_write_time();
            });
  while (incidents.size() > 4u) {
    std::filesystem::remove(incidents.front().path(), error);
    incidents.erase(incidents.begin());
  }
}

void RecordGpuFlightFrame(uint64_t frameSerial) {
  GpuFlightFrame frame = {};
  frame.timestampMs = EpochMilliseconds();
  frame.frameSerial = frameSerial;
  frame.lastRenderStage = GpuFlightBreadcrumbName(
      static_cast<GpuFlightBreadcrumb>(
          s_gpuFlightBreadcrumb.load(std::memory_order_acquire)));
  frame.breadcrumbSerial =
      s_gpuFlightBreadcrumbSerial.load(std::memory_order_acquire);
  frame.activeCsmCascade =
      s_gpuFlightActiveCsmCascade.load(std::memory_order_acquire);
  frame.activePointLight =
      s_gpuFlightActivePointLight.load(std::memory_order_acquire);
  frame.activePointFace =
      s_gpuFlightActivePointFace.load(std::memory_order_acquire);
  frame.autoTestContextValid =
      s_gpuFlightAutoTestContextValid.load(std::memory_order_acquire);
  frame.autoTestWaypointIndex =
      s_gpuFlightAutoTestWaypointIndex.load(std::memory_order_acquire);
  const auto loadAutoTestFloat = [](size_t index) {
    return FloatFromBits(s_gpuFlightAutoTestFloatBits[index].load(
        std::memory_order_acquire));
  };
  frame.autoTestTargetX = loadAutoTestFloat(0u);
  frame.autoTestTargetY = loadAutoTestFloat(1u);
  frame.autoTestPanSeconds = loadAutoTestFloat(2u);
  frame.cameraTargetX = loadAutoTestFloat(3u);
  frame.cameraTargetY = loadAutoTestFloat(4u);
  frame.cameraTargetDistance = loadAutoTestFloat(5u);
  frame.cameraAngleOfAttack = loadAutoTestFloat(6u);
  frame.worldMinX = loadAutoTestFloat(7u);
  frame.worldMinY = loadAutoTestFloat(8u);
  frame.worldMaxX = loadAutoTestFloat(9u);
  frame.worldMaxY = loadAutoTestFloat(10u);
  const auto replay = dxvk::QueryShadowReplayDiagnostics();
  frame.csmPlannedCasterCount = replay.plannedCasterCount;
  frame.csmValidatedCasterCount = replay.validatedCasterCount;
  frame.csmDrawnCasterCount = replay.drawnCasterCount;
  frame.csmLastRejectReason = replay.lastRejectReason;
  frame.csmValidationRejectCount = replay.validationRejectCount;
  frame.csmPartialPreventedCount = replay.partialPreventedCount;
  frame.csmFirstCompleteLatencyFrames = replay.firstCompleteLatencyFrames;
  for (size_t index = 0u; index < frame.csmCascadeDrawCount.size(); ++index) {
    frame.csmCascadeDrawCount[index] =
        s_gpuFlightCsmCascadeDrawCount[index].load(std::memory_order_acquire);
    frame.csmCascadeTriangleCount[index] =
        s_gpuFlightCsmCascadeTriangleCount[index].load(
            std::memory_order_acquire);
  }
  frame.pointShadowLightCount =
      s_gpuFlightPointShadowLightCount.load(std::memory_order_acquire);
  for (size_t index = 0u;
       index < frame.pointShadowFaceCandidateCount.size(); ++index) {
    frame.pointShadowFaceCandidateCount[index] =
        s_gpuFlightPointShadowFaceCandidateCount[index].load(
            std::memory_order_acquire);
    frame.pointShadowFaceKeptCount[index] =
        s_gpuFlightPointShadowFaceKeptCount[index].load(
            std::memory_order_acquire);
    frame.pointShadowFaceDrawCount[index] =
        s_gpuFlightPointShadowFaceDrawCount[index].load(
            std::memory_order_acquire);
    frame.pointShadowFaceTriangleCount[index] =
        s_gpuFlightPointShadowFaceTriangleCount[index].load(
            std::memory_order_acquire);
  }
  const auto taa = dxvk::QueryShadowTaaDiagnostics();
  const auto csm = dxvk::QueryCsmResolutionDiagnostics();
  const auto arena = dxvk::war3::memory::ShadowArena_QueryDiagnostics();
  frame.csmRequestedResolution = csm.requestedResolution;
  frame.csmEffectiveResolution = csm.effectiveResolution;
  frame.csmFallbackReason = csm.fallbackReason;
  frame.csmFallbackLatched = csm.fallbackLatched;
  frame.csmGeneration = csm.resourceGeneration;
  frame.csmMemoryBudgetBytes = csm.memoryBudgetBytes;
  frame.csmMemoryAvailableBytes = csm.memoryAvailableBytes;
  frame.taaRequestedMode = taa.requestedMode;
  frame.taaEffectiveMode = taa.effectiveMode;
  frame.taaShaderMode = taa.shaderMode;
  frame.taaHistoryValid = taa.historyValid;
  frame.taaHistoryGeneration = taa.historyGeneration;
  frame.arenaUsedBytes = arena.usedBytes;
  frame.arenaFrameUsedDeltaBytes =
      arena.generation == s_gpuFlightLastArenaGeneration &&
              arena.usedBytes >= s_gpuFlightLastArenaUsedBytes
          ? arena.usedBytes - s_gpuFlightLastArenaUsedBytes
          : arena.usedBytes;
  s_gpuFlightLastArenaGeneration = arena.generation;
  s_gpuFlightLastArenaUsedBytes = arena.usedBytes;
  frame.arenaResidentBytes = arena.residentBytes;
  frame.arenaGeneration = arena.generation;
  frame.arenaQuarantineCount = arena.quarantineCount;
  frame.arenaQuarantinedRetireSerial =
      arena.lastQuarantinedRetireSerial;
  frame.arenaBusyReuseRejectCount = arena.busyReuseRejectCount;
  frame.arenaOverflowCount = arena.overflowCount;
  frame.arenaReservedBytes = arena.reservedBytes;
  frame.arenaCommittedBytes = arena.committedBundleBytes;
  frame.arenaRolledBackBytes = arena.rolledBackBytes;
  frame.arenaAdmissionRejectedCount = arena.admissionRejectedCount;
  frame.arenaPartialTransactionCount = arena.partialTransactionCount;
  frame.arenaUniqueSourceBytes = arena.uniqueSourceBytes;
  frame.arenaDuplicateBytesSaved = arena.duplicateBytesSaved;
  frame.arenaExactIndexTrimAcceptedCount = arena.exactIndexTrimAcceptedCount;
  frame.arenaExactIndexTrimRejectedCount = arena.exactIndexTrimRejectedCount;
  frame.arenaExactIndexTrimBytesSaved = arena.exactIndexTrimBytesSaved;
  const auto cpuSpan =
      dxvk::war3::memory::QueryWar3CpuReadableSpanDiagnostics();
  frame.exactIndexDomainScannedBytes =
      cpuSpan.exactIndexDomainScannedBytes;
  frame.exactIndexDomainNonHostCachedScanCount =
      cpuSpan.exactIndexDomainNonHostCachedScanCount;
  frame.exactIndexDomainNonHostCachedScannedBytes =
      cpuSpan.exactIndexDomainNonHostCachedScannedBytes;
  frame.exactIndexDomainBulkReadCount =
      cpuSpan.exactIndexDomainBulkReadCount;
  frame.exactIndexDomainBulkReadBytes =
      cpuSpan.exactIndexDomainBulkReadBytes;
  frame.exactIndexDomainDirectReadCount =
      cpuSpan.exactIndexDomainDirectReadCount;
  frame.exactIndexDomainOversizeFallbackCount =
      cpuSpan.exactIndexDomainOversizeFallbackCount;
  frame.arenaFrameIncomplete = arena.frameIncomplete;
  dxvk::war3::RunWithActiveDevice([&](D3D9DeviceEx& device) {
    const auto lifecycle = device.QueryWar3ShadowLifecycleDiagnostics();
    frame.shadowMapEpoch = lifecycle.currentMapEpoch;
    frame.shadowMapResetRequestedSerial = lifecycle.requestedResetSerial;
    frame.shadowMapResetAppliedSerial = lifecycle.appliedResetSerial;
    frame.shadowMapTransitionState = lifecycle.transitionState;
    frame.queueResult = static_cast<int64_t>(
        device.GetDXVKDevice()->getDeviceStatus());
  });
  frame.queueSubmittedSerial = arena.submittedSerial;
  frame.queueCompletedSerial = arena.completedSerial;

  GpuIncidentSnapshot incident = {};
  bool writeIncident = false;
  {
    std::lock_guard<std::mutex> lock(s_gpuFlightMutex);
    s_gpuFlightFrames.push_back(frame);
    while (s_gpuFlightFrames.size() > 240u)
      s_gpuFlightFrames.pop_front();

    const auto now = std::chrono::steady_clock::now();
    if (frame.queueCompletedSerial != s_gpuLastCompletedSerial) {
      s_gpuLastCompletedSerial = frame.queueCompletedSerial;
      s_gpuLastProgressAt = now;
      if (frame.queueResult == VK_SUCCESS)
        s_gpuIncidentLatched = false;
    }
    const uint64_t stalledMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - s_gpuLastProgressAt).count());
    const bool queueFailed = frame.queueResult != VK_SUCCESS;
    const bool queueStalled =
        frame.queueSubmittedSerial > frame.queueCompletedSerial &&
        stalledMs >= 10000u;
    const bool arenaOverflow =
        frame.arenaOverflowCount > s_shadowArenaLastOverflowCount;
    const bool arenaAdmissionRejected = frame.arenaAdmissionRejectedCount >
        s_shadowArenaLastAdmissionRejectedCount;
    const bool arenaPartial = frame.arenaPartialTransactionCount >
        s_shadowArenaLastPartialTransactionCount;
    s_shadowArenaLastOverflowCount = frame.arenaOverflowCount;
    s_shadowArenaLastAdmissionRejectedCount =
        frame.arenaAdmissionRejectedCount;
    s_shadowArenaLastPartialTransactionCount =
        frame.arenaPartialTransactionCount;
    const bool queueIncident =
        (queueFailed || queueStalled) && !s_gpuIncidentLatched;
    const bool arenaIncident =
        (arenaOverflow || arenaAdmissionRejected || arenaPartial) &&
        !s_shadowArenaIncidentLatched;
    if (queueIncident || arenaIncident) {
      if (queueIncident)
        s_gpuIncidentLatched = true;
      if (arenaIncident)
        s_shadowArenaIncidentLatched = true;
      incident.timestampMs = frame.timestampMs;
      if (queueFailed)
        incident.reason = "queue-error";
      else if (queueStalled)
        incident.reason = "gpu-no-progress-10s";
      else if (arenaPartial)
        incident.reason = "shadow-arena-partial-transaction";
      else if (arenaOverflow)
        incident.reason = "shadow-arena-overflow";
      else
        incident.reason = "shadow-arena-admission-rejected";
      incident.queueResult = frame.queueResult;
      incident.stalledMilliseconds = stalledMs;
      incident.recentFrames.assign(s_gpuFlightFrames.begin(),
                                   s_gpuFlightFrames.end());
      writeIncident = true;
    }
  }
  if (writeIncident)
    WriteGpuIncidentSnapshot(incident);
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
  const auto gpuSkinVsShadow =
      dxvk::war3::render::QueryGpuSkinVsShadowRuntimeCounters();
  const auto taaDiagnostics = dxvk::QueryShadowTaaDiagnostics();
  const auto pointPersistentDiagnostics =
      dxvk::QueryPointShadowPersistentDiagnostics();
  const auto persistentPackageDiagnostics =
      dxvk::war3::gpu_skin::
          QueryPersistentGpuPackageD3D9RuntimeDiagnostics();
  const auto csmDiagnostics = dxvk::QueryCsmResolutionDiagnostics();
  const auto arenaDiagnostics =
      dxvk::war3::memory::ShadowArena_QueryDiagnostics();
  summary.shadowTaaRequestedMode = taaDiagnostics.requestedMode;
  summary.shadowTaaEffectiveMode = taaDiagnostics.effectiveMode;
  summary.shadowTaaShaderMode = taaDiagnostics.shaderMode;
  summary.shadowTaaHistoryValid = taaDiagnostics.historyValid;
  summary.shadowTaaHistoryReadable = taaDiagnostics.historyReadable;
  summary.shadowTaaHistoryGeneration = taaDiagnostics.historyGeneration;
  summary.shadowTaaLastInvalidationReason =
      taaDiagnostics.lastInvalidationReason;
  summary.shadowTaaFixedWallBypassCount =
      taaDiagnostics.fixedWallBypassCount;
  summary.pointShadowPersistentConfiguredMode =
      pointPersistentDiagnostics.configuredMode;
  summary.pointShadowPersistentEffectiveMode =
      pointPersistentDiagnostics.effectiveMode;
  summary.pointShadowPersistentLastBeginRejectReason =
      pointPersistentDiagnostics.lastBeginRejectReason;
  summary.pointShadowPersistentWorkerCreated =
      pointPersistentDiagnostics.workerCreated;
  summary.pointShadowPersistentWorkerAvailable =
      pointPersistentDiagnostics.workerAvailable;
  summary.pointShadowPersistentLastFrameSerial =
      pointPersistentDiagnostics.lastFrameSerial;
  summary.pointShadowPersistentBeginAttempts =
      pointPersistentDiagnostics.beginAttempts;
  summary.pointShadowPersistentBeginEligible =
      pointPersistentDiagnostics.beginEligible;
  summary.pointShadowPersistentWorkerCreateCount =
      pointPersistentDiagnostics.workerCreateCount;
  summary.pointShadowPersistentWorkerThreadStarts =
      pointPersistentDiagnostics.workerThreadStarts;
  summary.pointShadowPersistentAccepted =
      pointPersistentDiagnostics.accepted;
  summary.pointShadowPersistentReady = pointPersistentDiagnostics.ready;
  summary.pointShadowPersistentDeadlineFallback =
      pointPersistentDiagnostics.deadlineFallback;
  summary.pointShadowPersistentRejectedFallback =
      pointPersistentDiagnostics.rejectedFallback;
  summary.pointShadowPersistentObserveMatch =
      pointPersistentDiagnostics.observeMatch;
  summary.pointShadowPersistentMismatch =
      pointPersistentDiagnostics.mismatch;
  summary.pointShadowPersistentConsumed =
      pointPersistentDiagnostics.consumed;
  summary.pointShadowPersistentFailed = pointPersistentDiagnostics.failed;
  summary.pointShadowPersistentBusy = pointPersistentDiagnostics.busy;
  summary.persistentPackageConfiguredMode =
      persistentPackageDiagnostics.configuredMode;
  summary.persistentPackageEffectiveMode =
      persistentPackageDiagnostics.effectiveMode;
  summary.persistentPackageOwnerAlive =
      persistentPackageDiagnostics.ownerAlive;
  summary.persistentPackageObserveCalls =
      persistentPackageDiagnostics.observeCalls;
  summary.persistentPackageExactSourcesAccepted =
      persistentPackageDiagnostics.exactSourcesAccepted;
  summary.persistentPackageInvalidEvidence =
      persistentPackageDiagnostics.invalidEvidence;
  summary.persistentPackageInvalidSnapshots =
      persistentPackageDiagnostics.invalidSnapshots;
  summary.persistentPackageEpochRejects =
      persistentPackageDiagnostics.epochRejects;
  summary.persistentPackageReady =
      persistentPackageDiagnostics.readyObservations;
  summary.persistentPackageMiss =
      persistentPackageDiagnostics.missObservations;
  summary.persistentPackagePending =
      persistentPackageDiagnostics.pendingObservations;
  summary.persistentPackageStoreRejects =
      persistentPackageDiagnostics.storeRejects;
  summary.persistentPackageMultiPrimitive =
      persistentPackageDiagnostics.multiPrimitiveObservations;
  summary.persistentPackageSubmissionsBuilt =
      persistentPackageDiagnostics.submissionsBuilt;
  summary.persistentPackageSubmissionsCommitted =
      persistentPackageDiagnostics.submissionsCommitted;
  summary.persistentPackageSubmissionsRejected =
      persistentPackageDiagnostics.submissionsRejected;
  summary.persistentPackageUploadsCommitted =
      persistentPackageDiagnostics.uploadsCommitted;
  summary.persistentPackageUploadBytesCommitted =
      persistentPackageDiagnostics.uploadBytesCommitted;
  summary.persistentPackageProducerFenceSubmitted =
      persistentPackageDiagnostics.producerFenceSubmitted;
  summary.persistentPackageProducerFenceCompleted =
      persistentPackageDiagnostics.producerFenceCompleted;
  summary.persistentPackageStaticCacheHits =
      persistentPackageDiagnostics.staticCacheHits;
  summary.persistentPackageStaticCacheMisses =
      persistentPackageDiagnostics.staticCacheMisses;
  summary.persistentPackageStaticFallbacks =
      persistentPackageDiagnostics.staticFallbacks;
  summary.persistentPackageStaticUploadsCompleted =
      persistentPackageDiagnostics.staticUploadsCompleted;
  summary.persistentPackageStaticUploadCompletionsRejected =
      persistentPackageDiagnostics.staticUploadCompletionsRejected;
  summary.persistentPackageCurrentMapEpoch =
      persistentPackageDiagnostics.currentMapEpoch;
  summary.persistentPackageCurrentDeviceEpoch =
      persistentPackageDiagnostics.currentDeviceEpoch;
  summary.persistentPackageCurrentFrameSerial =
      persistentPackageDiagnostics.currentFrameSerial;
  summary.persistentPackageCurrentDrawConfiguredMode =
      persistentPackageDiagnostics.currentDrawConfiguredMode;
  summary.persistentPackageCurrentDrawEffectiveMode =
      persistentPackageDiagnostics.currentDrawEffectiveMode;
  summary.persistentPackageCurrentDrawObservations =
      persistentPackageDiagnostics.currentDrawObservations;
  summary.persistentPackageCurrentDrawExactMatches =
      persistentPackageDiagnostics.currentDrawExactMatches;
  summary.persistentPackageCurrentDrawWouldUseCsm =
      persistentPackageDiagnostics.currentDrawWouldUseCsm;
  summary.persistentPackageCurrentDrawRejected =
      persistentPackageDiagnostics.currentDrawRejected;
  summary.persistentPackageCurrentDrawNotRigidStatic =
      persistentPackageDiagnostics.currentDrawNotRigidStatic;
  summary.persistentPackageCurrentDrawMaterialRejected =
      persistentPackageDiagnostics.currentDrawMaterialRejected;
  summary.persistentPackageCurrentDrawSkinningRejected =
      persistentPackageDiagnostics.currentDrawSkinningRejected;
  summary.persistentPackageCurrentDrawGeometryRejected =
      persistentPackageDiagnostics.currentDrawGeometryRejected;
  summary.persistentPackageCurrentDrawGeometryPositionNotHostCached =
      persistentPackageDiagnostics.currentDrawGeometryPositionNotHostCached;
  summary.persistentPackageCurrentDrawGeometryIndexProofUnavailable =
      persistentPackageDiagnostics.currentDrawGeometryIndexProofUnavailable;
  summary.persistentPackageCurrentDrawBoundedIndexScans =
      persistentPackageDiagnostics.currentDrawBoundedIndexScans;
  summary.persistentPackageCurrentDrawBoundedIndexScanBytes =
      persistentPackageDiagnostics.currentDrawBoundedIndexScanBytes;
  summary.persistentPackageCurrentDrawBoundedIndexScanTicks =
      persistentPackageDiagnostics.currentDrawBoundedIndexScanTicks;
  summary.persistentPackageCurrentDrawBoundedPositionCopies =
      persistentPackageDiagnostics.currentDrawBoundedPositionCopies;
  summary.persistentPackageCurrentDrawBoundedPositionCopyBytes =
      persistentPackageDiagnostics.currentDrawBoundedPositionCopyBytes;
  summary.persistentPackageCurrentDrawBoundedPositionCopyTicks =
      persistentPackageDiagnostics.currentDrawBoundedPositionCopyTicks;
  summary.persistentPackageCurrentDrawContentHashBytes =
      persistentPackageDiagnostics.currentDrawContentHashBytes;
  summary.persistentPackageCurrentDrawContentHashTicks =
      persistentPackageDiagnostics.currentDrawContentHashTicks;
  summary.persistentPackageCurrentDrawProofBudgetRejected =
      persistentPackageDiagnostics.currentDrawProofBudgetRejected;
  summary.persistentPackageCaptureBoundedIndexScans =
      persistentPackageDiagnostics.capture.boundedIndexScans;
  summary.persistentPackageCaptureBoundedIndexScanBytes =
      persistentPackageDiagnostics.capture.boundedIndexScanBytes;
  summary.persistentPackageCaptureBoundedIndexScanTicks =
      persistentPackageDiagnostics.capture.boundedIndexScanTicks;
  summary.persistentPackageCaptureBoundedPositionCopies =
      persistentPackageDiagnostics.capture.boundedPositionCopies;
  summary.persistentPackageCaptureBoundedPositionCopyBytes =
      persistentPackageDiagnostics.capture.boundedPositionCopyBytes;
  summary.persistentPackageCaptureBoundedPositionCopyTicks =
      persistentPackageDiagnostics.capture.boundedPositionCopyTicks;
  summary.persistentPackageCaptureContentHashBytes =
      persistentPackageDiagnostics.capture.contentHashBytes;
  summary.persistentPackageCaptureContentHashTicks =
      persistentPackageDiagnostics.capture.contentHashTicks;
  summary.persistentPackageCaptureProofBudgetRejected =
      persistentPackageDiagnostics.capture.proofBudgetRejected;
  summary.persistentPackageCaptureTimerFrequency =
      persistentPackageDiagnostics.capture.timerFrequency;
  summary.persistentPackageCurrentDrawCpuSourceUnavailable =
      persistentPackageDiagnostics.currentDrawCpuSourceUnavailable;
  summary.persistentPackageCurrentDrawSourceGenerationMissing =
      persistentPackageDiagnostics.currentDrawSourceGenerationMissing;
  summary.persistentPackageCurrentDrawPackageNotReady =
      persistentPackageDiagnostics.currentDrawPackageNotReady;
  summary.persistentPackageCurrentDrawPackageInvalid =
      persistentPackageDiagnostics.currentDrawPackageInvalid;
  summary.persistentPackageCurrentDrawSnapshotMismatch =
      persistentPackageDiagnostics.currentDrawSnapshotMismatch;
  summary.persistentPackageCurrentDrawMultiPrimitiveRejected =
      persistentPackageDiagnostics.currentDrawMultiPrimitiveRejected;
  summary.persistentPackageCurrentDrawPackageLayoutMismatch =
      persistentPackageDiagnostics.currentDrawPackageLayoutMismatch;
  summary.persistentPackageCurrentDrawPositionMismatch =
      persistentPackageDiagnostics.currentDrawPositionMismatch;
  summary.persistentPackageCurrentDrawIndexMismatch =
      persistentPackageDiagnostics.currentDrawIndexMismatch;
  summary.persistentPackageCurrentDrawPrimitiveMismatch =
      persistentPackageDiagnostics.currentDrawPrimitiveMismatch;
  summary.persistentPackageCurrentDrawLastDisposition =
      persistentPackageDiagnostics.currentDrawLastDisposition;
  summary.persistentPackageGpuBindingAllowed =
      persistentPackageDiagnostics.gpuBindingAllowed;
  summary.persistentPackageDrawMutationAllowed =
      persistentPackageDiagnostics.drawMutationAllowed;
  summary.persistentPackageConsumerAuthorityPublished =
      persistentPackageDiagnostics.consumerAuthorityPublished;
  summary.persistentPackageConsumerLastUseFencePublished =
      persistentPackageDiagnostics.consumerLastUseFencePublished;
  summary.csmRequestedResolution = csmDiagnostics.requestedResolution;
  summary.csmEffectiveResolution = csmDiagnostics.effectiveResolution;
  summary.csmFallbackReason = csmDiagnostics.fallbackReason;
  summary.csmFallbackLatched = csmDiagnostics.fallbackLatched;
  summary.csmResourceGeneration = csmDiagnostics.resourceGeneration;
  summary.csmResourceRebuildCount = csmDiagnostics.resourceRebuildCount;
  summary.csmMemoryBudgetBytes = csmDiagnostics.memoryBudgetBytes;
  summary.csmMemoryAvailableBytes = csmDiagnostics.memoryAvailableBytes;
  summary.shadowArenaUsedBytes = arenaDiagnostics.usedBytes;
  summary.shadowArenaResidentBytes = arenaDiagnostics.residentBytes;
  summary.shadowArenaResidentLimitBytes =
      arenaDiagnostics.residentLimitBytes;
  summary.shadowArenaGeneration = arenaDiagnostics.generation;
  summary.shadowArenaQuarantineCount = arenaDiagnostics.quarantineCount;
  summary.shadowArenaLastQuarantinedGeneration =
      arenaDiagnostics.lastQuarantinedGeneration;
  summary.shadowArenaLastQuarantinedRetireSerial =
      arenaDiagnostics.lastQuarantinedRetireSerial;
  summary.shadowArenaBusyReuseRejectCount =
      arenaDiagnostics.busyReuseRejectCount;
  summary.shadowArenaOverflowCount = arenaDiagnostics.overflowCount;
  summary.shadowArenaReservedBytes = arenaDiagnostics.reservedBytes;
  summary.shadowArenaCommittedBytes =
      arenaDiagnostics.committedBundleBytes;
  summary.shadowArenaRolledBackBytes = arenaDiagnostics.rolledBackBytes;
  summary.shadowArenaAdmissionRejectedCount =
      arenaDiagnostics.admissionRejectedCount;
  summary.shadowArenaPartialTransactionCount =
      arenaDiagnostics.partialTransactionCount;
  summary.shadowArenaPageTailWasteBytes =
      arenaDiagnostics.pageTailWasteBytes;
  summary.shadowArenaPositionBytes = arenaDiagnostics.positionBytes;
  summary.shadowArenaBlendBytes = arenaDiagnostics.blendBytes;
  summary.shadowArenaUvBytes = arenaDiagnostics.uvBytes;
  summary.shadowArenaIndexBytes = arenaDiagnostics.indexBytes;
  summary.shadowArenaTerrainBytes = arenaDiagnostics.terrainBytes;
  summary.shadowArenaModelBytes = arenaDiagnostics.modelBytes;
  summary.shadowArenaSkinnedBytes = arenaDiagnostics.skinnedBytes;
  summary.shadowArenaUpBytes = arenaDiagnostics.upBytes;
  summary.shadowArenaUniqueSourceBytes = arenaDiagnostics.uniqueSourceBytes;
  summary.shadowArenaDuplicateBytesSaved =
      arenaDiagnostics.duplicateBytesSaved;
  summary.shadowArenaExactIndexTrimAcceptedCount =
      arenaDiagnostics.exactIndexTrimAcceptedCount;
  summary.shadowArenaExactIndexTrimRejectedCount =
      arenaDiagnostics.exactIndexTrimRejectedCount;
  summary.shadowArenaExactIndexTrimBytesSaved =
      arenaDiagnostics.exactIndexTrimBytesSaved;
  summary.shadowArenaFrameIncomplete = arenaDiagnostics.frameIncomplete;
  const auto cpuSpanDiagnostics =
      dxvk::war3::memory::QueryWar3CpuReadableSpanDiagnostics();
  summary.shadowCpuSpanAcceptedCount = cpuSpanDiagnostics.acceptedCount;
  summary.shadowCpuSpanRejectedCount = cpuSpanDiagnostics.rejectedCount;
  summary.shadowCpuSpanLastRejectReason =
      cpuSpanDiagnostics.lastRejectReason;
  summary.shadowCpuSpanLastAllocationBytes =
      cpuSpanDiagnostics.lastAllocationBytes;
  summary.shadowCpuSpanLastBindingOffset =
      cpuSpanDiagnostics.lastRequestedOffset;
  summary.shadowCpuSpanLastReadBytes = cpuSpanDiagnostics.lastRequestedBytes;
  summary.shadowCpuSpanLastSourceIdentityGeneration =
      cpuSpanDiagnostics.lastIdentityGeneration;
  summary.shadowCpuSpanLastAllocationGeneration =
      cpuSpanDiagnostics.lastAllocationGeneration;
  summary.shadowCpuSpanLastContentGeneration =
      cpuSpanDiagnostics.lastContentGeneration;
  summary.shadowExactIndexDomainScannedBytes =
      cpuSpanDiagnostics.exactIndexDomainScannedBytes;
  summary.shadowExactIndexDomainNonHostCachedScanCount =
      cpuSpanDiagnostics.exactIndexDomainNonHostCachedScanCount;
  summary.shadowExactIndexDomainNonHostCachedScannedBytes =
      cpuSpanDiagnostics.exactIndexDomainNonHostCachedScannedBytes;
  summary.shadowExactIndexDomainBulkReadCount =
      cpuSpanDiagnostics.exactIndexDomainBulkReadCount;
  summary.shadowExactIndexDomainBulkReadBytes =
      cpuSpanDiagnostics.exactIndexDomainBulkReadBytes;
  summary.shadowExactIndexDomainDirectReadCount =
      cpuSpanDiagnostics.exactIndexDomainDirectReadCount;
  summary.shadowExactIndexDomainOversizeFallbackCount =
      cpuSpanDiagnostics.exactIndexDomainOversizeFallbackCount;
  summary.queueSubmittedSerial = arenaDiagnostics.submittedSerial;
  summary.queueCompletedSerial = arenaDiagnostics.completedSerial;
  const auto replayDiagnostics = dxvk::QueryShadowReplayDiagnostics();
  summary.shadowStaleEpochConsumerRejectCount =
      replayDiagnostics.staleEpochConsumerRejectCount;
  summary.shadowReplayValidationRejectCount =
      replayDiagnostics.validationRejectCount;
  summary.shadowReplayPartialPreventedCount =
      replayDiagnostics.partialPreventedCount;
  summary.shadowPointWorkerCancelCount =
      replayDiagnostics.pointWorkerCancelCount;
  summary.shadowPointLateResultRejectCount =
      replayDiagnostics.pointLateResultRejectCount;
  summary.shadowFirstCompleteLatencyFrames =
      replayDiagnostics.firstCompleteLatencyFrames;
  summary.shadowReplayLastOffenderMapEpoch =
      replayDiagnostics.lastOffenderMapEpoch;
  summary.shadowReplayLastRequiredEnd = replayDiagnostics.lastRequiredEnd;
  summary.shadowReplayLastAvailableSize = replayDiagnostics.lastAvailableSize;
  summary.shadowReplayLastMinimumVertex = replayDiagnostics.lastMinimumVertex;
  summary.shadowReplayLastMaximumVertex = replayDiagnostics.lastMaximumVertex;
  summary.shadowReplayLastVertexOffset = replayDiagnostics.lastVertexOffset;
  summary.shadowReplayLastStage = replayDiagnostics.lastStage;
  summary.shadowReplayLastCategory = replayDiagnostics.lastCategory;
  summary.shadowReplayLastBatchTag = replayDiagnostics.lastBatchTag;
  summary.shadowReplayLastObjectKind = replayDiagnostics.lastObjectKind;
  summary.shadowReplayLastRawcode = replayDiagnostics.lastRawcode;
  summary.shadowReplayLastJHandle = replayDiagnostics.lastJHandle;
  summary.shadowReplayLastIndexCount = replayDiagnostics.lastIndexCount;
  summary.shadowReplayLastFirstIndex = replayDiagnostics.lastFirstIndex;
  summary.shadowReplayLastMinVertexIndex = replayDiagnostics.lastMinVertexIndex;
  summary.shadowReplayLastNumVertices = replayDiagnostics.lastNumVertices;
  summary.shadowReplayLastActualIndexMin = replayDiagnostics.lastActualIndexMin;
  summary.shadowReplayLastActualIndexMax = replayDiagnostics.lastActualIndexMax;
  summary.shadowReplayLastActualIndexDomainKnown =
      replayDiagnostics.lastActualIndexDomainKnown;
  summary.shadowReplayLastFullVertexDomainFallback =
      replayDiagnostics.lastFullVertexDomainFallback;
  summary.shadowReplayLastPositionSize = replayDiagnostics.lastPositionSize;
  summary.shadowReplayCandidateFrameSerial =
      replayDiagnostics.candidateFrameSerial;
  summary.shadowReplayPlannedCasterCount =
      replayDiagnostics.plannedCasterCount;
  summary.shadowReplayCasterCount = replayDiagnostics.replayCasterCount;
  summary.shadowReplayValidatedCasterCount =
      replayDiagnostics.validatedCasterCount;
  summary.shadowReplayDrawnCasterCount = replayDiagnostics.drawnCasterCount;
  summary.shadowReplayLastRejectReason = replayDiagnostics.lastRejectReason;
  dxvk::war3::RunWithActiveDevice([&](D3D9DeviceEx& device) {
    summary.queueLastResult = static_cast<int64_t>(
        device.GetDXVKDevice()->getDeviceStatus());
    const auto lifecycle = device.QueryWar3ShadowLifecycleDiagnostics();
    summary.shadowMapResetRequestedSerial = lifecycle.requestedResetSerial;
    summary.shadowMapResetAppliedSerial = lifecycle.appliedResetSerial;
    summary.shadowMapEpoch = lifecycle.currentMapEpoch;
    summary.shadowMapResetAppliedFrameSerial = lifecycle.appliedFrameSerial;
    summary.shadowMapQuarantinedRetireSerial =
        lifecycle.quarantinedRetireSerial;
    summary.shadowMapCompletedRetireSerial = lifecycle.completedRetireSerial;
    summary.shadowRetiredSessionCount = lifecycle.retiredSessionCount;
    summary.shadowRetiredSessionEntryCount =
        lifecycle.retiredSessionEntryCount;
    summary.shadowRetiredSessionAllocatorBytes =
        lifecycle.retiredSessionAllocatorBytes;
    summary.shadowRetiredSessionCachedGpuLogicalBytes =
        lifecycle.retiredSessionCachedGpuLogicalBytes;
    summary.shadowRetiredSessionCpuOwnedBytes =
        lifecycle.retiredSessionCpuOwnedBytes;
    summary.shadowRetiredSessionOldestRetireSerial =
        lifecycle.retiredSessionOldestRetireSerial;
    summary.shadowRetiredSessionCollectedCount =
        lifecycle.retiredSessionCollectedCount;
    summary.shadowRetiredLastMapEpoch = lifecycle.retiredLastMapEpoch;
    summary.shadowPendingProducerRejectCount =
        lifecycle.pendingProducerRejectCount;
    summary.shadowMapTransitionState = lifecycle.transitionState;
    summary.shadowMapProducerReady = lifecycle.producerReady;
  });
  summary.shadowEvidenceRetentionRevision =
      s_shadowEvidenceRetentionRevision.load(std::memory_order_acquire);
  summary.shadowEvidenceCollectorAttached =
      s_shadowEvidenceCollectorAttached.load(std::memory_order_acquire) ? 1u
                                                                       : 0u;
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
  summary.semanticSceneSubmittedSkinnedNonUnitResolvedCount =
      bridgeSummary.semanticSceneSubmittedSkinnedNonUnitResolvedCount;
  summary.semanticSceneSubmittedSkinnedUnknownPacketKindCount =
      bridgeSummary.semanticSceneSubmittedSkinnedUnknownPacketKindCount;
  summary.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount =
      bridgeSummary.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount;
  summary.semanticSceneSubmittedSkinnedGroupNonZeroCount =
      bridgeSummary.semanticSceneSubmittedSkinnedGroupNonZeroCount;
  summary.semanticSceneSubmittedSkinnedTransparentQueueCount =
      bridgeSummary.semanticSceneSubmittedSkinnedTransparentQueueCount;
  summary.semanticSceneSubmittedSkinnedMissingUnitPtrCount =
      bridgeSummary.semanticSceneSubmittedSkinnedMissingUnitPtrCount;
  summary.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount =
      bridgeSummary.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount;
  summary.semanticSceneSubmittedBuilding =
      bridgeSummary.semanticSceneSubmittedBuilding;
  summary.semanticSceneSubmittedDestructible =
      bridgeSummary.semanticSceneSubmittedDestructible;
  summary.semanticSceneSubmittedCutout =
      bridgeSummary.semanticSceneSubmittedCutout;
  summary.semanticSceneSubmittedAlphaBlend =
      bridgeSummary.semanticSceneSubmittedAlphaBlend;
  summary.semanticSceneMaterialObservedCutoutCount =
      bridgeSummary.semanticSceneMaterialObservedCutoutCount;
  summary.semanticSceneMaterialObservedAlphaBlendCount =
      bridgeSummary.semanticSceneMaterialObservedAlphaBlendCount;
  summary.semanticSceneRejectedCutoutSkinnedContract =
      bridgeSummary.semanticSceneRejectedCutoutSkinnedContract;
  summary.semanticSceneRejectedAlphaBlendSkinnedContract =
      bridgeSummary.semanticSceneRejectedAlphaBlendSkinnedContract;
  summary.semanticSceneRejectedCutoutGeometry =
      bridgeSummary.semanticSceneRejectedCutoutGeometry;
  summary.semanticSceneRejectedAlphaBlendGeometry =
      bridgeSummary.semanticSceneRejectedAlphaBlendGeometry;
  summary.semanticSceneRejectedCutoutVisualPolicy =
      bridgeSummary.semanticSceneRejectedCutoutVisualPolicy;
  summary.semanticSceneRejectedAlphaBlendVisualPolicy =
      bridgeSummary.semanticSceneRejectedAlphaBlendVisualPolicy;
  summary.semanticSceneMaterialLayerContractResolvedCount =
      bridgeSummary.semanticSceneMaterialLayerContractResolvedCount;
  summary.semanticSceneMaterialLayerContractFailedCount =
      bridgeSummary.semanticSceneMaterialLayerContractFailedCount;
  summary.semanticSceneMaterialBlendMode0Count =
      bridgeSummary.semanticSceneMaterialBlendMode0Count;
  summary.semanticSceneMaterialBlendMode1Count =
      bridgeSummary.semanticSceneMaterialBlendMode1Count;
  summary.semanticSceneMaterialBlendMode2PlusCount =
      bridgeSummary.semanticSceneMaterialBlendMode2PlusCount;
  summary.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount =
      bridgeSummary.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount;
  summary.semanticSceneMaterialLastMeshIndex =
      bridgeSummary.semanticSceneMaterialLastMeshIndex;
  summary.semanticSceneMaterialLastLayerIndex =
      bridgeSummary.semanticSceneMaterialLastLayerIndex;
  summary.semanticSceneMaterialLastLayerCount =
      bridgeSummary.semanticSceneMaterialLastLayerCount;
  summary.semanticSceneMaterialLastBlendOrDrawMode =
      bridgeSummary.semanticSceneMaterialLastBlendOrDrawMode;
  summary.semanticSceneSubmittedOwnedGroupSlots =
      bridgeSummary.semanticSceneSubmittedOwnedGroupSlots;
  summary.semanticSceneCurrentDrawContractKnownCount =
      bridgeSummary.semanticSceneCurrentDrawContractKnownCount;
  summary.semanticSceneCurrentDrawPaletteReadyCount =
      bridgeSummary.semanticSceneCurrentDrawPaletteReadyCount;
  summary.semanticSceneCurrentDrawGroupSlotReadyCount =
      bridgeSummary.semanticSceneCurrentDrawGroupSlotReadyCount;
  summary.semanticSceneCurrentDrawResolveReadyCount =
      bridgeSummary.semanticSceneCurrentDrawResolveReadyCount;
  summary.semanticSceneCurrentDrawMissNoContract =
      bridgeSummary.semanticSceneCurrentDrawMissNoContract;
  summary.semanticSceneCurrentDrawMissNoPalette =
      bridgeSummary.semanticSceneCurrentDrawMissNoPalette;
  summary.semanticSceneCurrentDrawMissNoGroupSlots =
      bridgeSummary.semanticSceneCurrentDrawMissNoGroupSlots;
  summary.semanticSceneCurrentDrawMissStaleVisibleFrame =
      bridgeSummary.semanticSceneCurrentDrawMissStaleVisibleFrame;
  summary.semanticSceneCurrentDrawResolveReadyRejectedCount =
      bridgeSummary.semanticSceneCurrentDrawResolveReadyRejectedCount;
  summary.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount =
      bridgeSummary.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount;
  summary.semanticScenePaletteOverrideNoComposeCount =
      bridgeSummary.semanticScenePaletteOverrideNoComposeCount;
  summary.semanticScenePaletteOverrideWouldComposeCount =
      bridgeSummary.semanticScenePaletteOverrideWouldComposeCount;
  summary.semanticScenePalettePacketWorldComposeCount =
      bridgeSummary.semanticScenePalettePacketWorldComposeCount;
  summary.semanticSceneCanonicalReadyCount =
      bridgeSummary.semanticSceneCanonicalReadyCount;
  summary.semanticSceneCanonicalReadyCutoutCount =
      bridgeSummary.semanticSceneCanonicalReadyCutoutCount;
  summary.semanticSceneCanonicalReadyAlphaBlendCount =
      bridgeSummary.semanticSceneCanonicalReadyAlphaBlendCount;
  summary.semanticSceneCanonicalRejectNoStableIdentity =
      bridgeSummary.semanticSceneCanonicalRejectNoStableIdentity;
  summary.semanticSceneCanonicalRejectNoMesh =
      bridgeSummary.semanticSceneCanonicalRejectNoMesh;
  summary.semanticSceneCanonicalRejectNoWorldTransform =
      bridgeSummary.semanticSceneCanonicalRejectNoWorldTransform;
  summary.semanticSceneCanonicalRejectNoPalette =
      bridgeSummary.semanticSceneCanonicalRejectNoPalette;
  summary.semanticSceneCanonicalRejectNoSlotContract =
      bridgeSummary.semanticSceneCanonicalRejectNoSlotContract;
  summary.semanticSceneCanonicalRejectStaleProducer =
      bridgeSummary.semanticSceneCanonicalRejectStaleProducer;
  summary.semanticSceneCanonicalRejectInvalidVertexIndex =
      bridgeSummary.semanticSceneCanonicalRejectInvalidVertexIndex;
  summary.semanticSceneCanonicalRejectExplicitBlendIncomplete =
      bridgeSummary.semanticSceneCanonicalRejectExplicitBlendIncomplete;
  summary.semanticSceneCanonicalRejectAfterReadyCount =
      bridgeSummary.semanticSceneCanonicalRejectAfterReadyCount;
  summary.currentDrawContractPublishAttemptCount =
      bridgeSummary.currentDrawContractPublishAttemptCount;
  summary.currentDrawContractPublishReadyCount =
      bridgeSummary.currentDrawContractPublishReadyCount;
  summary.currentDrawContractPublishSkippedNonWorldContext =
      bridgeSummary.currentDrawContractPublishSkippedNonWorldContext;
  summary.currentDrawContractPublishSkippedSmallViewport =
      bridgeSummary.currentDrawContractPublishSkippedSmallViewport;
  summary.currentDrawContractQueryAttemptCount =
      bridgeSummary.currentDrawContractQueryAttemptCount;
  summary.currentDrawContractQueryHitCount =
      bridgeSummary.currentDrawContractQueryHitCount;
  summary.currentDrawCapturedPaletteQueryAttemptCount =
      bridgeSummary.currentDrawCapturedPaletteQueryAttemptCount;
  summary.currentDrawCapturedPaletteQueryHitCount =
      bridgeSummary.currentDrawCapturedPaletteQueryHitCount;
  summary.currentDrawGroupSlotDecodeAttemptCount =
      bridgeSummary.currentDrawGroupSlotDecodeAttemptCount;
  summary.currentDrawGroupSlotDecodeHitCount =
      bridgeSummary.currentDrawGroupSlotDecodeHitCount;
  summary.currentDrawGroupSlotDecodeMissDisabledStream =
      bridgeSummary.currentDrawGroupSlotDecodeMissDisabledStream;
  summary.currentDrawPreparedSliceProbeAttemptCount =
      bridgeSummary.currentDrawPreparedSliceProbeAttemptCount;
  summary.currentDrawPreparedSliceProbeContextReadyCount =
      bridgeSummary.currentDrawPreparedSliceProbeContextReadyCount;
  summary.currentDrawPreparedSliceProbeBackingReadableCount =
      bridgeSummary.currentDrawPreparedSliceProbeBackingReadableCount;
  summary.currentDrawPreparedSliceRecordedCount =
      bridgeSummary.currentDrawPreparedSliceRecordedCount;
  summary.currentDrawPreparedSliceQueryAttemptCount =
      bridgeSummary.currentDrawPreparedSliceQueryAttemptCount;
  summary.currentDrawPreparedSliceQueryHitCount =
      bridgeSummary.currentDrawPreparedSliceQueryHitCount;
  summary.currentDrawPreparedSliceQueryMissCount =
      bridgeSummary.currentDrawPreparedSliceQueryMissCount;
  summary.currentDrawStream1PublishNoStreamCount =
      bridgeSummary.currentDrawStream1PublishNoStreamCount;
  summary.currentDrawStream1PublishStride0Count =
      bridgeSummary.currentDrawStream1PublishStride0Count;
  summary.currentDrawStream1PublishStride1Count =
      bridgeSummary.currentDrawStream1PublishStride1Count;
  summary.currentDrawStream1PublishStride8Count =
      bridgeSummary.currentDrawStream1PublishStride8Count;
  summary.currentDrawStream1PublishStride12Count =
      bridgeSummary.currentDrawStream1PublishStride12Count;
  summary.currentDrawStream1PublishStride16Count =
      bridgeSummary.currentDrawStream1PublishStride16Count;
  summary.currentDrawStream1PublishStride20Count =
      bridgeSummary.currentDrawStream1PublishStride20Count;
  summary.currentDrawStream1PublishStrideOtherCount =
      bridgeSummary.currentDrawStream1PublishStrideOtherCount;
  summary.currentDrawStream1PublishLastRawStride =
      bridgeSummary.currentDrawStream1PublishLastRawStride;
  summary.currentDrawStream1PublishMaxRawStride =
      bridgeSummary.currentDrawStream1PublishMaxRawStride;
  summary.currentDrawLastVisibleFrameSerial =
      bridgeSummary.currentDrawLastVisibleFrameSerial;
  summary.currentDrawLastRenderFrameIndex =
      bridgeSummary.currentDrawLastRenderFrameIndex;
  summary.currentDrawLastSmallViewportWidth =
      bridgeSummary.currentDrawLastSmallViewportWidth;
  summary.currentDrawLastSmallViewportHeight =
      bridgeSummary.currentDrawLastSmallViewportHeight;
  summary.currentDrawLastMissReason =
      bridgeSummary.currentDrawLastMissReason;
  summary.submitPaletteContentAgeLag0Count =
      bridgeSummary.submitPaletteContentAgeLag0Count;
  summary.submitPaletteContentAgeLag1Count =
      bridgeSummary.submitPaletteContentAgeLag1Count;
  summary.submitPaletteContentAgeLag2Count =
      bridgeSummary.submitPaletteContentAgeLag2Count;
  summary.submitPaletteContentAgeLag3To5Count =
      bridgeSummary.submitPaletteContentAgeLag3To5Count;
  summary.submitPaletteContentAgeLag6PlusCount =
      bridgeSummary.submitPaletteContentAgeLag6PlusCount;
  summary.submitPaletteContentAgeMax =
      bridgeSummary.submitPaletteContentAgeMax;
  summary.submitPaletteContentAgeSampleCount =
      bridgeSummary.submitPaletteContentAgeSampleCount;
  summary.submitPaletteContentAgeUnknownCount =
      bridgeSummary.submitPaletteContentAgeUnknownCount;
  summary.semanticScenePopulateLastReturnReason =
      bridgeSummary.semanticScenePopulateLastReturnReason;
  summary.semanticScenePopulateLastProducerPublishAttemptDelta =
      bridgeSummary.semanticScenePopulateLastProducerPublishAttemptDelta;
  summary.semanticScenePopulateLastProducerPublishReadyDelta =
      bridgeSummary.semanticScenePopulateLastProducerPublishReadyDelta;
  summary.semanticScenePopulateLastProducerQueryAttemptDelta =
      bridgeSummary.semanticScenePopulateLastProducerQueryAttemptDelta;
  summary.semanticScenePopulateLastProducerQueryHitDelta =
      bridgeSummary.semanticScenePopulateLastProducerQueryHitDelta;
  summary.semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta =
      bridgeSummary
          .semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta;
  summary.semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta =
      bridgeSummary
          .semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta;
  summary.semanticScenePopulateLastProducerGroupDecodeAttemptDelta =
      bridgeSummary
          .semanticScenePopulateLastProducerGroupDecodeAttemptDelta;
  summary.semanticScenePopulateLastProducerGroupDecodeHitDelta =
      bridgeSummary.semanticScenePopulateLastProducerGroupDecodeHitDelta;
  summary.semanticSceneSubmittedExplicitBlendContract =
      bridgeSummary.semanticSceneSubmittedExplicitBlendContract;
  summary.semanticSceneSubmittedSingleMatrixGroupSkinning =
      bridgeSummary.semanticSceneSubmittedSingleMatrixGroupSkinning;
  summary.semanticSceneSubmittedMultiGroupSlotSkinning =
      bridgeSummary.semanticSceneSubmittedMultiGroupSlotSkinning;
  summary.semanticSceneSkinnedMinUniqueGroupSlots =
      bridgeSummary.semanticSceneSkinnedMinUniqueGroupSlots;
  summary.semanticSceneSkinnedMaxUniqueGroupSlots =
      bridgeSummary.semanticSceneSkinnedMaxUniqueGroupSlots;
  summary.semanticSceneSkinnedGroupSlotsUnique1Count =
      bridgeSummary.semanticSceneSkinnedGroupSlotsUnique1Count;
  summary.semanticSceneSkinnedGroupSlotsUnique2To4Count =
      bridgeSummary.semanticSceneSkinnedGroupSlotsUnique2To4Count;
  summary.semanticSceneSkinnedGroupSlotsUnique5To8Count =
      bridgeSummary.semanticSceneSkinnedGroupSlotsUnique5To8Count;
  summary.semanticSceneSkinnedGroupSlotsUnique9To16Count =
      bridgeSummary.semanticSceneSkinnedGroupSlotsUnique9To16Count;
  summary.semanticSceneSkinnedGroupSlotsUnique17PlusCount =
      bridgeSummary.semanticSceneSkinnedGroupSlotsUnique17PlusCount;
  summary.semanticSceneExplicitBlendUnavailableCurrentDraw =
      bridgeSummary.semanticSceneExplicitBlendUnavailableCurrentDraw;
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
  // Phase 7.2: flicker diagnostics + reconciliation
  summary.semanticSceneDirectLastRawRecordCount =
      bridgeSummary.semanticSceneDirectLastRawRecordCount;
  summary.semanticSceneDirectLastEligibleRecordCount =
      bridgeSummary.semanticSceneDirectLastEligibleRecordCount;
  summary.semanticSceneCompactWorkTableMode =
      bridgeSummary.semanticSceneCompactWorkTableMode;
  summary.semanticSceneCompactWorkTableCandidateCount =
      bridgeSummary.semanticSceneCompactWorkTableCandidateCount;
  summary.semanticSceneCompactWorkTableSealedCount =
      bridgeSummary.semanticSceneCompactWorkTableSealedCount;
  summary.semanticSceneCompactWorkTableConsumedCount =
      bridgeSummary.semanticSceneCompactWorkTableConsumedCount;
  summary.semanticSceneCompactWorkTableFallbackCount =
      bridgeSummary.semanticSceneCompactWorkTableFallbackCount;
  summary.semanticSceneCompactWorkTableRejectStageCount =
      bridgeSummary.semanticSceneCompactWorkTableRejectStageCount;
  summary.semanticSceneCompactWorkTableRejectFreshnessCount =
      bridgeSummary.semanticSceneCompactWorkTableRejectFreshnessCount;
  summary.semanticSceneCompactWorkTableRejectPolicyCount =
      bridgeSummary.semanticSceneCompactWorkTableRejectPolicyCount;
  summary.semanticSceneCompactWorkTableRejectFrameCount =
      bridgeSummary.semanticSceneCompactWorkTableRejectFrameCount;
  summary.semanticSceneCompactWorkTableRejectIdentityCount =
      bridgeSummary.semanticSceneCompactWorkTableRejectIdentityCount;
  summary.semanticSceneCompactWorkTableMismatchCount =
      bridgeSummary.semanticSceneCompactWorkTableMismatchCount;
  summary.drawTimeSemanticProducerOwnedDirectGroupedSkipCount =
      bridgeSummary.drawTimeSemanticProducerOwnedDirectGroupedSkipCount;
  summary.semanticSceneDirectLastSubmittedRecordCount =
      bridgeSummary.semanticSceneDirectLastSubmittedRecordCount;
  summary.semanticSceneDirectLastUniqueObjectCount =
      bridgeSummary.semanticSceneDirectLastUniqueObjectCount;
  summary.semanticSceneDirectLastSubmittedObjectCount =
      bridgeSummary.semanticSceneDirectLastSubmittedObjectCount;
  summary.semanticSceneDirectLastRecordCapPartialObjectCount =
      bridgeSummary.semanticSceneDirectLastRecordCapPartialObjectCount;
  summary.semanticSceneDirectLastScanCapPartialObjectCount =
      bridgeSummary.semanticSceneDirectLastScanCapPartialObjectCount;
  summary.semanticSceneDirectLastMinGeosetsPerObject =
      bridgeSummary.semanticSceneDirectLastMinGeosetsPerObject;
  summary.semanticSceneDirectLastMaxGeosetsPerObject =
      bridgeSummary.semanticSceneDirectLastMaxGeosetsPerObject;
  summary.semanticSceneDirectLastSubmittedIdentityHash =
      bridgeSummary.semanticSceneDirectLastSubmittedIdentityHash;
  summary.semanticSceneDirectIdentityChurnCount =
      bridgeSummary.semanticSceneDirectIdentityChurnCount;
  summary.semanticSceneDirectRecordCapHitCount =
      bridgeSummary.semanticSceneDirectRecordCapHitCount;
  summary.semanticSceneDirectRecordCapTruncatedRecordCount =
      bridgeSummary.semanticSceneDirectRecordCapTruncatedRecordCount;
  summary.semanticSceneDirectScanCapHitCount =
      bridgeSummary.semanticSceneDirectScanCapHitCount;
  summary.semanticSceneDirectObjectGroupedSubmitCount =
      bridgeSummary.semanticSceneDirectObjectGroupedSubmitCount;
  summary.semanticSceneDirectObjectGroupedSkipCount =
      bridgeSummary.semanticSceneDirectObjectGroupedSkipCount;
  summary.semanticSceneDirectRecordCapSkipObjectCount =
      bridgeSummary.semanticSceneDirectRecordCapSkipObjectCount;
  summary.semanticSceneDirectRecordCapAppendFailCount =
      bridgeSummary.semanticSceneDirectRecordCapAppendFailCount;
  summary.semanticSceneDirectSelectionLeaseActiveKeyCount =
      bridgeSummary.semanticSceneDirectSelectionLeaseActiveKeyCount;
  summary.semanticSceneDirectSelectionLeasePrunedKeyCount =
      bridgeSummary.semanticSceneDirectSelectionLeasePrunedKeyCount;
  summary.semanticSceneDirectSelectionLeaseSubmittedKeyCount =
      bridgeSummary.semanticSceneDirectSelectionLeaseSubmittedKeyCount;
  summary.semanticSceneDirectStickyFillBudgetRecordCount =
      bridgeSummary.semanticSceneDirectStickyFillBudgetRecordCount;
  summary.semanticSceneDirectStickyFillAppendedCount =
      bridgeSummary.semanticSceneDirectStickyFillAppendedCount;
  summary.semanticSceneDirectStickyFillSubmittedCount =
      bridgeSummary.semanticSceneDirectStickyFillSubmittedCount;
  summary.semanticSceneDirectStickyFillMissedCount =
      bridgeSummary.semanticSceneDirectStickyFillMissedCount;
  summary.semanticSceneDirectPartLeaseRestoredCount =
      bridgeSummary.semanticSceneDirectPartLeaseRestoredCount;
  summary.semanticSceneDirectPartLeaseUpdatedCount =
      bridgeSummary.semanticSceneDirectPartLeaseUpdatedCount;
  summary.semanticSceneDirectPartLeaseExpiredCount =
      bridgeSummary.semanticSceneDirectPartLeaseExpiredCount;
  summary.semanticSceneDirectPartLeaseRejectedDynamicMeshCount =
      bridgeSummary.semanticSceneDirectPartLeaseRejectedDynamicMeshCount;
  summary.semanticSceneDirectPartLeaseRejectedNotSelfContainedCount =
      bridgeSummary
          .semanticSceneDirectPartLeaseRejectedNotSelfContainedCount;
  summary.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount =
      bridgeSummary.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount;
  summary.semanticSceneDirectPartLeaseRejectedSelfRenewCount =
      bridgeSummary.semanticSceneDirectPartLeaseRejectedSelfRenewCount;
  summary.semanticSceneDirectPartLeaseBudgetLimitCount =
      bridgeSummary.semanticSceneDirectPartLeaseBudgetLimitCount;
  summary.semanticSceneShadowManifestPartLeaseRestoredCount =
      bridgeSummary.semanticSceneShadowManifestPartLeaseRestoredCount;
  summary.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount =
      bridgeSummary.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount;
  summary.semanticSceneShadowManifestPartLeaseExpiredCount =
      bridgeSummary.semanticSceneShadowManifestPartLeaseExpiredCount;
  summary.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount =
      bridgeSummary.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount;
  summary.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount =
      bridgeSummary.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount;
  summary.semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount =
      bridgeSummary
          .semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount;
  summary
      .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount =
      bridgeSummary
          .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount;
  summary.semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount =
      bridgeSummary
          .semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount;
  summary.semanticSceneShadowManifestPartLeaseBudgetLimitCount =
      bridgeSummary.semanticSceneShadowManifestPartLeaseBudgetLimitCount;
  summary.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount =
      bridgeSummary
          .semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount;
  summary.semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount =
      bridgeSummary
          .semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount;
  summary.semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount =
      bridgeSummary
          .semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount;
  summary.semanticSceneShadowManifestObjectCoreCompleteCount =
      bridgeSummary.semanticSceneShadowManifestObjectCoreCompleteCount;
  summary.semanticSceneShadowManifestObjectCoreIncompleteSkipCount =
      bridgeSummary
          .semanticSceneShadowManifestObjectCoreIncompleteSkipCount;
  summary.semanticSceneShadowManifestPartOmittedIncompleteCoreCount =
      bridgeSummary
          .semanticSceneShadowManifestPartOmittedIncompleteCoreCount;
  // Phase 7.25 core epoch planner 专属计数器。
  summary.semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount =
      bridgeSummary
          .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount;
  summary.semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount =
      bridgeSummary
          .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount;
  summary.semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount =
      bridgeSummary
          .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount;
  summary.semanticSceneShadowManifestObjectCoreEpochMissingPartCount =
      bridgeSummary
          .semanticSceneShadowManifestObjectCoreEpochMissingPartCount;
  summary.semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount =
      bridgeSummary
          .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount;
  // Phase 7.28：skinned palette content stability probe。
  summary.semanticSceneSubmittedSkinnedPaletteSourceNoneCount =
      bridgeSummary.semanticSceneSubmittedSkinnedPaletteSourceNoneCount;
  summary.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount;
  summary.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount;
  summary.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount;
  summary
      .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount;
  summary
      .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount;
  summary.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount =
      bridgeSummary.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount;
  summary.semanticSceneSubmittedSkinnedPaletteHashChurnCount =
      bridgeSummary.semanticSceneSubmittedSkinnedPaletteHashChurnCount;
  summary.semanticSceneSubmittedSkinnedPaletteSourceChurnCount =
      bridgeSummary.semanticSceneSubmittedSkinnedPaletteSourceChurnCount;
  summary.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount;
  summary.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax;
  summary.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax;
  summary.semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount;
  summary.semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount;
  summary.semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount;
  summary.semanticSceneSubmittedSkinnedPaletteCountChurnCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteCountChurnCount;
  summary.semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount;
  summary.semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount;
  summary.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount;
  summary.semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount;
  summary.semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount;
  summary
      .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount;
  summary
      .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount;
  summary
      .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount;
  summary.semanticSceneDirectPaletteAttributionSnapshotHitCount =
      bridgeSummary.semanticSceneDirectPaletteAttributionSnapshotHitCount;
  summary.semanticSceneDirectPaletteCaptureTrustedSourceHitCount =
      bridgeSummary.semanticSceneDirectPaletteCaptureTrustedSourceHitCount;
  summary.semanticSceneDirectPaletteCaptureTrustedSourceMissCount =
      bridgeSummary.semanticSceneDirectPaletteCaptureTrustedSourceMissCount;
  // Phase 7.30 Step A：stale→live 过渡归因。
  summary.semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount;
  summary
      .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount;
  summary.semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount =
      bridgeSummary
          .semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount;
  summary.semanticSceneDirectManifestObjectCount =
      bridgeSummary.semanticSceneDirectManifestObjectCount;
  summary.semanticSceneDirectManifestObservedPartCount =
      bridgeSummary.semanticSceneDirectManifestObservedPartCount;
  summary.semanticSceneDirectManifestShadowEligiblePartCount =
      bridgeSummary.semanticSceneDirectManifestShadowEligiblePartCount;
  summary.semanticSceneDirectObjectCompleteEligibleCount =
      bridgeSummary.semanticSceneDirectObjectCompleteEligibleCount;
  summary.semanticSceneDirectObjectIncompleteByScanCapCount =
      bridgeSummary.semanticSceneDirectObjectIncompleteByScanCapCount;
  summary.semanticSceneDirectObjectIncompleteByAlphaPolicyCount =
      bridgeSummary.semanticSceneDirectObjectIncompleteByAlphaPolicyCount;
  summary.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount =
      bridgeSummary.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount;
  summary.semanticSceneDirectObjectIncompleteByPacketBuildFailCount =
      bridgeSummary.semanticSceneDirectObjectIncompleteByPacketBuildFailCount;
  summary.semanticSceneDirectObjectIncompleteByAppendFailCount =
      bridgeSummary.semanticSceneDirectObjectIncompleteByAppendFailCount;
  summary.semanticSceneDirectSubmittedCompleteObjectCount =
      bridgeSummary.semanticSceneDirectSubmittedCompleteObjectCount;
  summary.semanticSceneDirectSubmittedPartialObjectCount =
      bridgeSummary.semanticSceneDirectSubmittedPartialObjectCount;
  summary.semanticSceneDirectPreparedSliceAuthoritativeCount =
      bridgeSummary.semanticSceneDirectPreparedSliceAuthoritativeCount;
  summary.semanticSceneDirectPreparedSliceFallbackLayerIndexCount =
      bridgeSummary.semanticSceneDirectPreparedSliceFallbackLayerIndexCount;
  summary.semanticSceneDirectPreparedSliceMissingCount =
      bridgeSummary.semanticSceneDirectPreparedSliceMissingCount;
  summary.semanticScenePreparedProbeAttemptCount =
      bridgeSummary.semanticScenePreparedProbeAttemptCount;
  summary.semanticScenePreparedProbeContextReadyCount =
      bridgeSummary.semanticScenePreparedProbeContextReadyCount;
  summary.semanticScenePreparedProbeBackingReadableCount =
      bridgeSummary.semanticScenePreparedProbeBackingReadableCount;
  summary.semanticScenePreparedSliceRecordedCount =
      bridgeSummary.semanticScenePreparedSliceRecordedCount;
  summary.semanticScenePreparedSliceQueryAttemptCount =
      bridgeSummary.semanticScenePreparedSliceQueryAttemptCount;
  summary.semanticScenePreparedSliceQueryHitCount =
      bridgeSummary.semanticScenePreparedSliceQueryHitCount;
  summary.semanticScenePreparedSliceQueryMissCount =
      bridgeSummary.semanticScenePreparedSliceQueryMissCount;
  summary.semanticSceneShadowManifestObjectCount =
      bridgeSummary.semanticSceneShadowManifestObjectCount;
  summary.semanticSceneShadowManifestPartCount =
      bridgeSummary.semanticSceneShadowManifestPartCount;
  summary.semanticSceneShadowManifestStableObjectCount =
      bridgeSummary.semanticSceneShadowManifestStableObjectCount;
  summary.semanticSceneShadowManifestNewObjectCount =
      bridgeSummary.semanticSceneShadowManifestNewObjectCount;
  summary.semanticSceneShadowManifestExpiredObjectCount =
      bridgeSummary.semanticSceneShadowManifestExpiredObjectCount;
  summary.semanticSceneShadowManifestFreshPartCount =
      bridgeSummary.semanticSceneShadowManifestFreshPartCount;
  summary.semanticSceneShadowManifestLeaseablePartCount =
      bridgeSummary.semanticSceneShadowManifestLeaseablePartCount;
  summary.semanticSceneShadowManifestPoseStalePartCount =
      bridgeSummary.semanticSceneShadowManifestPoseStalePartCount;
  summary.semanticSceneShadowManifestSliceStalePartCount =
      bridgeSummary.semanticSceneShadowManifestSliceStalePartCount;
  summary.semanticSceneShadowManifestExpiredPartCount =
      bridgeSummary.semanticSceneShadowManifestExpiredPartCount;
  summary.semanticSceneShadowManifestMultiSlicePartCount =
      bridgeSummary.semanticSceneShadowManifestMultiSlicePartCount;
  summary.semanticSceneShadowManifestPayload11CChurnCount =
      bridgeSummary.semanticSceneShadowManifestPayload11CChurnCount;
  summary.semanticSceneShadowManifestRenderablePartChurnCount =
      bridgeSummary.semanticSceneShadowManifestRenderablePartChurnCount;
  summary.semanticSceneShadowManifestCModelPoseHitCount =
      bridgeSummary.semanticSceneShadowManifestCModelPoseHitCount;
  summary.semanticSceneShadowManifestCModelPoseMissCount =
      bridgeSummary.semanticSceneShadowManifestCModelPoseMissCount;
  summary.semanticSceneShadowManifestCModelPoseNoRuntimeCount =
      bridgeSummary.semanticSceneShadowManifestCModelPoseNoRuntimeCount;
  summary.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr =
      bridgeSummary.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr;
  summary.semanticSceneShadowManifestCModelPoseLastMatrixCount =
      bridgeSummary.semanticSceneShadowManifestCModelPoseLastMatrixCount;
  summary.semanticSceneShadowManifestCModelPoseLastMatrixHash =
      bridgeSummary.semanticSceneShadowManifestCModelPoseLastMatrixHash;
  summary.semanticSceneSubmittedObjectJaccardMilli =
      bridgeSummary.semanticSceneSubmittedObjectJaccardMilli;
  summary.semanticSceneSubmittedPartJaccardMilli =
      bridgeSummary.semanticSceneSubmittedPartJaccardMilli;
  summary.semanticSceneVisibleLookupPartLayerHitCount =
      bridgeSummary.semanticSceneVisibleLookupPartLayerHitCount;
  summary.semanticSceneVisibleLookupSingleFallbackCount =
      bridgeSummary.semanticSceneVisibleLookupSingleFallbackCount;
  summary.semanticSceneVisibleLookupMissCount =
      bridgeSummary.semanticSceneVisibleLookupMissCount;
  summary.semanticSceneDirectMainWorldBackingNotCheckedCount =
      bridgeSummary.semanticSceneDirectMainWorldBackingNotCheckedCount;
  summary.semanticSceneDirectMainWorldBackingPassCount =
      bridgeSummary.semanticSceneDirectMainWorldBackingPassCount;
  summary.semanticSceneDirectMainWorldBackingFailNoRenderablePartCount =
      bridgeSummary
          .semanticSceneDirectMainWorldBackingFailNoRenderablePartCount;
  summary.semanticSceneDirectMainWorldBackingFailLookupMissCount =
      bridgeSummary.semanticSceneDirectMainWorldBackingFailLookupMissCount;
  summary.semanticSceneDirectMainWorldBackingFailNonMainQueueCount =
      bridgeSummary.semanticSceneDirectMainWorldBackingFailNonMainQueueCount;
  summary.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount =
      bridgeSummary.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount;
  summary.semanticSceneDirectMainWorldBackingFailIdentityMismatchCount =
      bridgeSummary
          .semanticSceneDirectMainWorldBackingFailIdentityMismatchCount;
  summary.semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount =
      bridgeSummary
          .semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount;
  summary.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount =
      bridgeSummary.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount;
  summary.semanticSceneDirectPaletteHashChurnCount =
      bridgeSummary.semanticSceneDirectPaletteHashChurnCount;
  summary.semanticSceneDirectGroupHashChurnCount =
      bridgeSummary.semanticSceneDirectGroupHashChurnCount;
  summary.semanticSceneDirectStableGroupHashChurnCount =
      bridgeSummary.semanticSceneDirectStableGroupHashChurnCount;
  summary.semanticSceneDirectStream1PtrChurnCount =
      bridgeSummary.semanticSceneDirectStream1PtrChurnCount;
  summary.semanticSceneDirectGeometrySourceHashChurnCount =
      bridgeSummary.semanticSceneDirectGeometrySourceHashChurnCount;
  summary.semanticSceneDirectSameCasterComparisonCount =
      bridgeSummary.semanticSceneDirectSameCasterComparisonCount;
  summary.semanticSceneDirectIdentitySkippedChurnCount =
      bridgeSummary.semanticSceneDirectIdentitySkippedChurnCount;
  summary.semanticSceneDirectPaletteRootDeltaSampleCount =
      bridgeSummary.semanticSceneDirectPaletteRootDeltaSampleCount;
  summary.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount =
      bridgeSummary.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount;
  summary.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount =
      bridgeSummary.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount;
  summary.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount =
      bridgeSummary.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount;
  summary.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount =
      bridgeSummary.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount;
  summary.semanticSceneDirectPaletteRootMaxDeltaMilli =
      bridgeSummary.semanticSceneDirectPaletteRootMaxDeltaMilli;
  summary.semanticSceneDirectSelectionKeyUnitPtrCount =
      bridgeSummary.semanticSceneDirectSelectionKeyUnitPtrCount;
  summary.semanticSceneDirectSelectionKeyJHandleCount =
      bridgeSummary.semanticSceneDirectSelectionKeyJHandleCount;
  summary.semanticSceneDirectSelectionKeyRuntimeModelCount =
      bridgeSummary.semanticSceneDirectSelectionKeyRuntimeModelCount;
  summary.semanticSceneDirectSelectionKeyWorldObjectCount =
      bridgeSummary.semanticSceneDirectSelectionKeyWorldObjectCount;
  summary.semanticSceneDirectSelectionKeySceneNodeCount =
      bridgeSummary.semanticSceneDirectSelectionKeySceneNodeCount;
  summary.semanticSceneDirectSelectionKeyModelMeshCount =
      bridgeSummary.semanticSceneDirectSelectionKeyModelMeshCount;
  summary.semanticSceneDirectSelectionKeyRenderablePartCount =
      bridgeSummary.semanticSceneDirectSelectionKeyRenderablePartCount;
  summary.semanticSceneDirectLastSubmittedSceneNode =
      bridgeSummary.semanticSceneDirectLastSubmittedSceneNode;
  summary.semanticSceneDirectLastSubmittedRenderablePart =
      bridgeSummary.semanticSceneDirectLastSubmittedRenderablePart;
  summary.semanticSceneDirectLastSubmittedMeshData =
      bridgeSummary.semanticSceneDirectLastSubmittedMeshData;
  summary.semanticSceneDirectLastSubmittedPaletteHash =
      bridgeSummary.semanticSceneDirectLastSubmittedPaletteHash;
  summary.semanticSceneDirectLastSubmittedGroupHash =
      bridgeSummary.semanticSceneDirectLastSubmittedGroupHash;
  summary.semanticSceneDirectLastSubmittedStableGroupHash =
      bridgeSummary.semanticSceneDirectLastSubmittedStableGroupHash;
  summary.semanticSceneDirectLastSubmittedStream1Ptr =
      bridgeSummary.semanticSceneDirectLastSubmittedStream1Ptr;
  summary.semanticSceneDirectLastSubmittedGeometrySourceHash =
      bridgeSummary.semanticSceneDirectLastSubmittedGeometrySourceHash;
  summary.semanticSceneLastAppendedGeometrySourceHash =
      bridgeSummary.semanticSceneLastAppendedGeometrySourceHash;
  summary.semanticSceneLastAppendedGeometryId =
      bridgeSummary.semanticSceneLastAppendedGeometryId;
  summary.semanticSceneShadowCastersCount =
      bridgeSummary.semanticSceneShadowCastersCount;
  summary.semanticSceneReplayDrawsCount =
      bridgeSummary.semanticSceneReplayDrawsCount;
  summary.semanticSceneShadowMapDrawnCasters =
      bridgeSummary.semanticSceneShadowMapDrawnCasters;
  summary.semanticSceneShadowMapCascadeCulledCount =
      bridgeSummary.semanticSceneShadowMapCascadeCulledCount;
  summary.semanticSceneTerrainBoundsCullMode =
      bridgeSummary.semanticSceneTerrainBoundsCullMode;
  summary.semanticSceneTerrainBoundsCandidateCount =
      bridgeSummary.semanticSceneTerrainBoundsCandidateCount;
  summary.semanticSceneTerrainBoundsProofAcceptedCount =
      bridgeSummary.semanticSceneTerrainBoundsProofAcceptedCount;
  summary.semanticSceneTerrainBoundsFailVisibleCount =
      bridgeSummary.semanticSceneTerrainBoundsFailVisibleCount;
  summary.semanticSceneTerrainBoundsWouldCullCount =
      bridgeSummary.semanticSceneTerrainBoundsWouldCullCount;
  summary.semanticSceneTerrainBoundsAppliedCullCount =
      bridgeSummary.semanticSceneTerrainBoundsAppliedCullCount;
  summary.semanticSceneTerrainBoundsC0WouldCullCount =
      bridgeSummary.semanticSceneTerrainBoundsC0WouldCullCount;
  summary.semanticSceneTerrainBoundsC1WouldCullCount =
      bridgeSummary.semanticSceneTerrainBoundsC1WouldCullCount;
  summary.semanticSceneTerrainBoundsC2WouldCullCount =
      bridgeSummary.semanticSceneTerrainBoundsC2WouldCullCount;
  summary.semanticSceneTerrainBoundsC3WouldCullCount =
      bridgeSummary.semanticSceneTerrainBoundsC3WouldCullCount;
  summary.semanticSceneObjectBoundsCandidateCount =
      bridgeSummary.semanticSceneObjectBoundsCandidateCount;
  summary.semanticSceneObjectBoundsProofAcceptedCount =
      bridgeSummary.semanticSceneObjectBoundsProofAcceptedCount;
  summary.semanticSceneObjectBoundsFailVisibleCount =
      bridgeSummary.semanticSceneObjectBoundsFailVisibleCount;
  summary.semanticSceneObjectBoundsWouldCullCount =
      bridgeSummary.semanticSceneObjectBoundsWouldCullCount;
  summary.semanticSceneObjectBoundsAppliedCullCount =
      bridgeSummary.semanticSceneObjectBoundsAppliedCullCount;
  summary.semanticSceneShadowMapSkinnedCasterCount =
      bridgeSummary.semanticSceneShadowMapSkinnedCasterCount;
  summary.semanticSceneShadowMapSkinnedPreparedCount =
      bridgeSummary.semanticSceneShadowMapSkinnedPreparedCount;
  summary.semanticSceneShadowMapSkinnedInvalidBufferCount =
      bridgeSummary.semanticSceneShadowMapSkinnedInvalidBufferCount;
  summary.semanticSceneShadowMapSkinnedInvalidPipelineCount =
      bridgeSummary.semanticSceneShadowMapSkinnedInvalidPipelineCount;
  summary.semanticSceneShadowMapSkinnedDrawnCount =
      bridgeSummary.semanticSceneShadowMapSkinnedDrawnCount;
  summary.gpuSkinVsShadowDirectAttempts =
      gpuSkinVsShadow.directAttempts;
  summary.gpuSkinVsShadowDirectInputRejects =
      gpuSkinVsShadow.directInputRejects;
  summary.gpuSkinVsShadowDirectStateRejects =
      gpuSkinVsShadow.directStateRejects;
  summary.gpuSkinVsShadowDirectDrawsSubmitted =
      gpuSkinVsShadow.directDrawsSubmitted;
  summary.gpuSkinVsShadowReplayDirectional =
      gpuSkinVsShadow.replayDirectional;
  summary.gpuSkinVsShadowReplayPoint =
      gpuSkinVsShadow.replayPoint;
  summary.semanticSceneShadowTaaActive =
      bridgeSummary.semanticSceneShadowTaaActive;
  summary.semanticSceneReceiverReuseShadowMap =
      bridgeSummary.semanticSceneReceiverReuseShadowMap;
  summary.semanticSceneReceiverInputValid =
      bridgeSummary.semanticSceneReceiverInputValid;
  summary.semanticSceneReceiverInputRejectReason =
      bridgeSummary.semanticSceneReceiverInputRejectReason;
  summary.semanticSceneReceiverNeedPass =
      bridgeSummary.semanticSceneReceiverNeedPass;
  summary.semanticSceneReceiverNeedShadowMap =
      bridgeSummary.semanticSceneReceiverNeedShadowMap;
  summary.semanticSceneReceiverHasCompleteShadowMap =
      bridgeSummary.semanticSceneReceiverHasCompleteShadowMap;
  summary.semanticSceneReceiverHasUsableDirectionalShadow =
      bridgeSummary.semanticSceneReceiverHasUsableDirectionalShadow;
  summary.semanticSceneReceiverActiveStrengthMilli =
      bridgeSummary.semanticSceneReceiverActiveStrengthMilli;
  summary.semanticSceneReceiverUboStrengthMilli =
      bridgeSummary.semanticSceneReceiverUboStrengthMilli;
  summary.semanticSceneReceiverDebugMode =
      bridgeSummary.semanticSceneReceiverDebugMode;
  summary.semanticSceneReceiverCsmCascadeCount =
      bridgeSummary.semanticSceneReceiverCsmCascadeCount;
  summary.semanticSceneReceiverRunEntryFlags =
      bridgeSummary.semanticSceneReceiverRunEntryFlags;
  summary.semanticSceneReceiverRunEarlyReturnReason =
      bridgeSummary.semanticSceneReceiverRunEarlyReturnReason;
  summary.semanticSceneShadowMapExecutedThisFrame =
      bridgeSummary.semanticSceneShadowMapExecutedThisFrame;
  summary.semanticSceneReceiverSettingsShadowsEnabled =
      bridgeSummary.semanticSceneReceiverSettingsShadowsEnabled;
  summary.semanticSceneReceiverSettingsOutlineEnabled =
      bridgeSummary.semanticSceneReceiverSettingsOutlineEnabled;
  summary.semanticSceneReceiverSettingsRawStrengthMilli =
      bridgeSummary.semanticSceneReceiverSettingsRawStrengthMilli;
  summary.semanticSceneReceiverComputedShadowStrengthMilli =
      bridgeSummary.semanticSceneReceiverComputedShadowStrengthMilli;
  summary.semanticSceneReceiverHasSunShadow =
      bridgeSummary.semanticSceneReceiverHasSunShadow;
  summary.semanticSceneReceiverHasPointShadow =
      bridgeSummary.semanticSceneReceiverHasPointShadow;
  summary.semanticSceneReceiverNeedOutlinePass =
      bridgeSummary.semanticSceneReceiverNeedOutlinePass;
  summary.semanticSceneReceiverZeroStrengthFrameCount =
      bridgeSummary.semanticSceneReceiverZeroStrengthFrameCount;
  summary.semanticSceneReceiverDrawnWithZeroStrengthCount =
      bridgeSummary.semanticSceneReceiverDrawnWithZeroStrengthCount;
  summary.semanticSceneReceiverNoCompleteShadowMapCount =
      bridgeSummary.semanticSceneReceiverNoCompleteShadowMapCount;
  summary.semanticSceneReceiverNoShadowMapImageCount =
      bridgeSummary.semanticSceneReceiverNoShadowMapImageCount;
  summary.semanticSceneReceiverNoShadowMapSampleViewCount =
      bridgeSummary.semanticSceneReceiverNoShadowMapSampleViewCount;
  summary.semanticSceneReceiverNoCandidateCsmCount =
      bridgeSummary.semanticSceneReceiverNoCandidateCsmCount;
  summary.semanticSceneReceiverCsmFallbackToLastGoodCount =
      bridgeSummary.semanticSceneReceiverCsmFallbackToLastGoodCount;
  summary.semanticSceneReceiverHoldInvalidCsmCount =
      bridgeSummary.semanticSceneReceiverHoldInvalidCsmCount;
  summary.semanticSceneReceiverHoldEmptyReplayCount =
      bridgeSummary.semanticSceneReceiverHoldEmptyReplayCount;
  summary.semanticSceneReceiverHoldIdentityChurnCount =
      bridgeSummary.semanticSceneReceiverHoldIdentityChurnCount;
  summary.semanticSceneReceiverReuseInvalidatedAfterEnsureCount =
      bridgeSummary.semanticSceneReceiverReuseInvalidatedAfterEnsureCount;
  summary.semanticSceneShadowMapRenderSkippedNoResourcesCount =
      bridgeSummary.semanticSceneShadowMapRenderSkippedNoResourcesCount;
  summary.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount =
      bridgeSummary.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount;
  summary.semanticSceneReceiverViewportX =
      bridgeSummary.semanticSceneReceiverViewportX;
  summary.semanticSceneReceiverViewportY =
      bridgeSummary.semanticSceneReceiverViewportY;
  summary.semanticSceneReceiverViewportWidth =
      bridgeSummary.semanticSceneReceiverViewportWidth;
  summary.semanticSceneReceiverViewportHeight =
      bridgeSummary.semanticSceneReceiverViewportHeight;
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
      {"lightning",
       {{"activeCount", snapshot.lightning.activeCount},
        {"polylineActiveCount", snapshot.lightning.polylineActiveCount},
        {"templateCount", snapshot.lightning.templateCount},
        {"finalizedTemplateCount", snapshot.lightning.finalizedTemplateCount},
        {"textureCacheEntryCount", snapshot.lightning.textureCacheEntryCount},
        {"createCount", snapshot.lightning.createCount},
        {"polylineCreateCount", snapshot.lightning.polylineCreateCount},
        {"templateCreateCount", snapshot.lightning.templateCreateCount},
        {"templateFinalizeCount", snapshot.lightning.templateFinalizeCount},
        {"destroyCount", snapshot.lightning.destroyCount},
        {"commandFailureCount", snapshot.lightning.commandFailureCount},
        {"drawAttemptCount", snapshot.lightning.drawAttemptCount},
        {"drawSuccessCount", snapshot.lightning.drawSuccessCount},
        {"drawSkippedNoDeviceCount", snapshot.lightning.drawSkippedNoDeviceCount},
        {"drawSkippedNoActiveCount", snapshot.lightning.drawSkippedNoActiveCount},
        {"textureLoadAttemptCount", snapshot.lightning.textureLoadAttemptCount},
        {"textureLoadFallbackCount", snapshot.lightning.textureLoadFallbackCount},
        {"lastDrawVertexCount", snapshot.lightning.lastDrawVertexCount},
        {"lastDrawPrimitiveCount", snapshot.lightning.lastDrawPrimitiveCount},
        {"lastPolylinePointCount", snapshot.lightning.lastPolylinePointCount},
        {"hasDevice", snapshot.lightning.hasDevice},
        {"textureLoaded", snapshot.lightning.textureLoaded},
        {"textureFallback", snapshot.lightning.textureFallback}}},
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
        {"semanticSceneSubmittedSkinnedNonUnitResolvedCount",
         snapshot.shadow.semanticSceneSubmittedSkinnedNonUnitResolvedCount},
        {"semanticSceneSubmittedSkinnedUnknownPacketKindCount",
         snapshot.shadow.semanticSceneSubmittedSkinnedUnknownPacketKindCount},
        {"semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount},
        {"semanticSceneSubmittedSkinnedGroupNonZeroCount",
         snapshot.shadow.semanticSceneSubmittedSkinnedGroupNonZeroCount},
        {"semanticSceneSubmittedSkinnedTransparentQueueCount",
         snapshot.shadow.semanticSceneSubmittedSkinnedTransparentQueueCount},
        {"semanticSceneSubmittedSkinnedMissingUnitPtrCount",
         snapshot.shadow.semanticSceneSubmittedSkinnedMissingUnitPtrCount},
        {"semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount},
        {"semanticSceneSubmittedBuilding",
         snapshot.shadow.semanticSceneSubmittedBuilding},
        {"semanticSceneSubmittedDestructible",
         snapshot.shadow.semanticSceneSubmittedDestructible},
        {"semanticSceneSubmittedCutout",
         snapshot.shadow.semanticSceneSubmittedCutout},
        {"semanticSceneSubmittedAlphaBlend",
         snapshot.shadow.semanticSceneSubmittedAlphaBlend},
        {"semanticSceneMaterialObservedCutoutCount",
         snapshot.shadow.semanticSceneMaterialObservedCutoutCount},
        {"semanticSceneMaterialObservedAlphaBlendCount",
         snapshot.shadow.semanticSceneMaterialObservedAlphaBlendCount},
        {"semanticSceneRejectedCutoutSkinnedContract",
         snapshot.shadow.semanticSceneRejectedCutoutSkinnedContract},
        {"semanticSceneRejectedAlphaBlendSkinnedContract",
         snapshot.shadow.semanticSceneRejectedAlphaBlendSkinnedContract},
        {"semanticSceneRejectedCutoutGeometry",
         snapshot.shadow.semanticSceneRejectedCutoutGeometry},
        {"semanticSceneRejectedAlphaBlendGeometry",
         snapshot.shadow.semanticSceneRejectedAlphaBlendGeometry},
        {"semanticSceneRejectedCutoutVisualPolicy",
         snapshot.shadow.semanticSceneRejectedCutoutVisualPolicy},
        {"semanticSceneRejectedAlphaBlendVisualPolicy",
         snapshot.shadow.semanticSceneRejectedAlphaBlendVisualPolicy},
        {"semanticSceneMaterialLayerContractResolvedCount",
         snapshot.shadow.semanticSceneMaterialLayerContractResolvedCount},
        {"semanticSceneMaterialLayerContractFailedCount",
         snapshot.shadow.semanticSceneMaterialLayerContractFailedCount},
        {"semanticSceneMaterialBlendMode0Count",
         snapshot.shadow.semanticSceneMaterialBlendMode0Count},
        {"semanticSceneMaterialBlendMode1Count",
         snapshot.shadow.semanticSceneMaterialBlendMode1Count},
        {"semanticSceneMaterialBlendMode2PlusCount",
         snapshot.shadow.semanticSceneMaterialBlendMode2PlusCount},
        {"semanticSceneDirectCurrentDrawLayerIndexNonZeroCount",
         snapshot.shadow.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount},
        {"semanticSceneMaterialLastMeshIndex",
         snapshot.shadow.semanticSceneMaterialLastMeshIndex},
        {"semanticSceneMaterialLastLayerIndex",
         snapshot.shadow.semanticSceneMaterialLastLayerIndex},
        {"semanticSceneMaterialLastLayerCount",
         snapshot.shadow.semanticSceneMaterialLastLayerCount},
        {"semanticSceneMaterialLastBlendOrDrawMode",
         snapshot.shadow.semanticSceneMaterialLastBlendOrDrawMode},
        {"semanticSceneSubmittedOwnedGroupSlots",
         snapshot.shadow.semanticSceneSubmittedOwnedGroupSlots},
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
        {"semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount",
         snapshot.shadow
             .semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount},
        {"semanticScenePaletteOverrideNoComposeCount",
         snapshot.shadow.semanticScenePaletteOverrideNoComposeCount},
        {"semanticScenePaletteOverrideWouldComposeCount",
         snapshot.shadow.semanticScenePaletteOverrideWouldComposeCount},
        {"semanticScenePalettePacketWorldComposeCount",
         snapshot.shadow.semanticScenePalettePacketWorldComposeCount},
        {"semanticSceneCanonicalReadyCount",
         snapshot.shadow.semanticSceneCanonicalReadyCount},
        {"semanticSceneCanonicalReadyCutoutCount",
         snapshot.shadow.semanticSceneCanonicalReadyCutoutCount},
        {"semanticSceneCanonicalReadyAlphaBlendCount",
         snapshot.shadow.semanticSceneCanonicalReadyAlphaBlendCount},
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
        {"semanticSceneShadowManifestObjectCount",
         snapshot.shadow.semanticSceneShadowManifestObjectCount},
        {"semanticSceneShadowManifestPartCount",
         snapshot.shadow.semanticSceneShadowManifestPartCount},
        {"semanticSceneShadowManifestStableObjectCount",
         snapshot.shadow.semanticSceneShadowManifestStableObjectCount},
        {"semanticSceneShadowManifestNewObjectCount",
         snapshot.shadow.semanticSceneShadowManifestNewObjectCount},
        {"semanticSceneShadowManifestExpiredObjectCount",
         snapshot.shadow.semanticSceneShadowManifestExpiredObjectCount},
        {"semanticSceneShadowManifestFreshPartCount",
         snapshot.shadow.semanticSceneShadowManifestFreshPartCount},
        {"semanticSceneShadowManifestLeaseablePartCount",
         snapshot.shadow.semanticSceneShadowManifestLeaseablePartCount},
        {"semanticSceneShadowManifestPoseStalePartCount",
         snapshot.shadow.semanticSceneShadowManifestPoseStalePartCount},
        {"semanticSceneShadowManifestSliceStalePartCount",
         snapshot.shadow.semanticSceneShadowManifestSliceStalePartCount},
        {"semanticSceneShadowManifestExpiredPartCount",
         snapshot.shadow.semanticSceneShadowManifestExpiredPartCount},
        {"semanticSceneShadowManifestMultiSlicePartCount",
         snapshot.shadow.semanticSceneShadowManifestMultiSlicePartCount},
        {"semanticSceneShadowManifestPayload11CChurnCount",
         snapshot.shadow.semanticSceneShadowManifestPayload11CChurnCount},
        {"semanticSceneShadowManifestRenderablePartChurnCount",
         snapshot.shadow
             .semanticSceneShadowManifestRenderablePartChurnCount},
        {"semanticSceneShadowManifestCModelPoseHitCount",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseHitCount},
        {"semanticSceneShadowManifestCModelPoseMissCount",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseMissCount},
        {"semanticSceneShadowManifestCModelPoseNoRuntimeCount",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseNoRuntimeCount},
        {"semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr",
         snapshot.shadow
             .semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr},
        {"semanticSceneShadowManifestCModelPoseLastMatrixCount",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseLastMatrixCount},
        {"semanticSceneShadowManifestCModelPoseLastMatrixHash",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseLastMatrixHash},
        {"semanticSceneSubmittedObjectJaccardMilli",
         snapshot.shadow.semanticSceneSubmittedObjectJaccardMilli},
        {"semanticSceneSubmittedPartJaccardMilli",
         snapshot.shadow.semanticSceneSubmittedPartJaccardMilli},
        {"semanticSceneVisibleLookupPartLayerHitCount",
         snapshot.shadow.semanticSceneVisibleLookupPartLayerHitCount},
        {"semanticSceneVisibleLookupSingleFallbackCount",
         snapshot.shadow.semanticSceneVisibleLookupSingleFallbackCount},
        {"semanticSceneVisibleLookupMissCount",
         snapshot.shadow.semanticSceneVisibleLookupMissCount},
        {"semanticSceneDirectMainWorldBackingNotCheckedCount",
         snapshot.shadow.semanticSceneDirectMainWorldBackingNotCheckedCount},
        {"semanticSceneDirectMainWorldBackingPassCount",
         snapshot.shadow.semanticSceneDirectMainWorldBackingPassCount},
        {"semanticSceneDirectMainWorldBackingFailNoRenderablePartCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailNoRenderablePartCount},
        {"semanticSceneDirectMainWorldBackingFailLookupMissCount",
         snapshot.shadow.semanticSceneDirectMainWorldBackingFailLookupMissCount},
        {"semanticSceneDirectMainWorldBackingFailNonMainQueueCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailNonMainQueueCount},
        {"semanticSceneDirectMainWorldBackingFailNonWorldGroupCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailNonWorldGroupCount},
        {"semanticSceneDirectMainWorldBackingFailIdentityMismatchCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailIdentityMismatchCount},
        {"semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount},
        {"semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount},
        {"currentDrawStream1PublishNoStreamCount",
         snapshot.shadow.currentDrawStream1PublishNoStreamCount},
        {"currentDrawStream1PublishStride0Count",
         snapshot.shadow.currentDrawStream1PublishStride0Count},
        {"currentDrawStream1PublishStride1Count",
         snapshot.shadow.currentDrawStream1PublishStride1Count},
        {"currentDrawStream1PublishStride8Count",
         snapshot.shadow.currentDrawStream1PublishStride8Count},
        {"currentDrawStream1PublishStride12Count",
         snapshot.shadow.currentDrawStream1PublishStride12Count},
        {"currentDrawStream1PublishStride16Count",
         snapshot.shadow.currentDrawStream1PublishStride16Count},
        {"currentDrawStream1PublishStride20Count",
         snapshot.shadow.currentDrawStream1PublishStride20Count},
        {"currentDrawStream1PublishStrideOtherCount",
         snapshot.shadow.currentDrawStream1PublishStrideOtherCount},
        {"currentDrawStream1PublishLastRawStride",
         snapshot.shadow.currentDrawStream1PublishLastRawStride},
        {"currentDrawStream1PublishMaxRawStride",
         snapshot.shadow.currentDrawStream1PublishMaxRawStride},
        {"currentDrawLastVisibleFrameSerial",
         snapshot.shadow.currentDrawLastVisibleFrameSerial},
        {"currentDrawLastRenderFrameIndex",
         snapshot.shadow.currentDrawLastRenderFrameIndex},
        {"currentDrawLastSmallViewportWidth",
         snapshot.shadow.currentDrawLastSmallViewportWidth},
        {"currentDrawLastSmallViewportHeight",
         snapshot.shadow.currentDrawLastSmallViewportHeight},
        {"currentDrawLastMissReason",
         snapshot.shadow.currentDrawLastMissReason},
        {"submitPaletteContentAgeLag0Count",
         snapshot.shadow.submitPaletteContentAgeLag0Count},
        {"submitPaletteContentAgeLag1Count",
         snapshot.shadow.submitPaletteContentAgeLag1Count},
        {"submitPaletteContentAgeLag2Count",
         snapshot.shadow.submitPaletteContentAgeLag2Count},
        {"submitPaletteContentAgeLag3To5Count",
         snapshot.shadow.submitPaletteContentAgeLag3To5Count},
        {"submitPaletteContentAgeLag6PlusCount",
         snapshot.shadow.submitPaletteContentAgeLag6PlusCount},
        {"submitPaletteContentAgeMax",
         snapshot.shadow.submitPaletteContentAgeMax},
        {"submitPaletteContentAgeSampleCount",
         snapshot.shadow.submitPaletteContentAgeSampleCount},
        {"submitPaletteContentAgeUnknownCount",
         snapshot.shadow.submitPaletteContentAgeUnknownCount},
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
        {"semanticSceneSubmittedExplicitBlendContract",
         snapshot.shadow.semanticSceneSubmittedExplicitBlendContract},
        {"semanticSceneSubmittedSingleMatrixGroupSkinning",
         snapshot.shadow.semanticSceneSubmittedSingleMatrixGroupSkinning},
        {"semanticSceneSubmittedMultiGroupSlotSkinning",
         snapshot.shadow.semanticSceneSubmittedMultiGroupSlotSkinning},
        {"semanticSceneSkinnedMinUniqueGroupSlots",
         snapshot.shadow.semanticSceneSkinnedMinUniqueGroupSlots},
        {"semanticSceneSkinnedMaxUniqueGroupSlots",
         snapshot.shadow.semanticSceneSkinnedMaxUniqueGroupSlots},
        {"semanticSceneSkinnedGroupSlotsUnique1Count",
         snapshot.shadow.semanticSceneSkinnedGroupSlotsUnique1Count},
        {"semanticSceneSkinnedGroupSlotsUnique2To4Count",
         snapshot.shadow.semanticSceneSkinnedGroupSlotsUnique2To4Count},
        {"semanticSceneSkinnedGroupSlotsUnique5To8Count",
         snapshot.shadow.semanticSceneSkinnedGroupSlotsUnique5To8Count},
        {"semanticSceneSkinnedGroupSlotsUnique9To16Count",
         snapshot.shadow.semanticSceneSkinnedGroupSlotsUnique9To16Count},
        {"semanticSceneSkinnedGroupSlotsUnique17PlusCount",
         snapshot.shadow.semanticSceneSkinnedGroupSlotsUnique17PlusCount},
        {"semanticSceneExplicitBlendUnavailableCurrentDraw",
         snapshot.shadow.semanticSceneExplicitBlendUnavailableCurrentDraw},
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
         snapshot.shadow.semanticStaticCandidateRejectedNonCanonicalKind},
        // Phase 7.2: flicker diagnostics + reconciliation
        {"semanticSceneDirectLastRawRecordCount",
         snapshot.shadow.semanticSceneDirectLastRawRecordCount},
        {"semanticSceneDirectLastEligibleRecordCount",
         snapshot.shadow.semanticSceneDirectLastEligibleRecordCount},
        {"semanticSceneCompactWorkTableMode",
         snapshot.shadow.semanticSceneCompactWorkTableMode},
        {"semanticSceneCompactWorkTableCandidateCount",
         snapshot.shadow.semanticSceneCompactWorkTableCandidateCount},
        {"semanticSceneCompactWorkTableSealedCount",
         snapshot.shadow.semanticSceneCompactWorkTableSealedCount},
        {"semanticSceneCompactWorkTableConsumedCount",
         snapshot.shadow.semanticSceneCompactWorkTableConsumedCount},
        {"semanticSceneCompactWorkTableFallbackCount",
         snapshot.shadow.semanticSceneCompactWorkTableFallbackCount},
        {"semanticSceneCompactWorkTableRejectStageCount",
         snapshot.shadow.semanticSceneCompactWorkTableRejectStageCount},
        {"semanticSceneCompactWorkTableRejectFreshnessCount",
         snapshot.shadow.semanticSceneCompactWorkTableRejectFreshnessCount},
        {"semanticSceneCompactWorkTableRejectPolicyCount",
         snapshot.shadow.semanticSceneCompactWorkTableRejectPolicyCount},
        {"semanticSceneCompactWorkTableRejectFrameCount",
         snapshot.shadow.semanticSceneCompactWorkTableRejectFrameCount},
        {"semanticSceneCompactWorkTableRejectIdentityCount",
         snapshot.shadow.semanticSceneCompactWorkTableRejectIdentityCount},
        {"semanticSceneCompactWorkTableMismatchCount",
         snapshot.shadow.semanticSceneCompactWorkTableMismatchCount},
        {"drawTimeSemanticProducerOwnedDirectGroupedSkipCount",
         snapshot.shadow
             .drawTimeSemanticProducerOwnedDirectGroupedSkipCount},
        {"semanticSceneDirectLastSubmittedRecordCount",
         snapshot.shadow.semanticSceneDirectLastSubmittedRecordCount},
        {"semanticSceneDirectLastUniqueObjectCount",
         snapshot.shadow.semanticSceneDirectLastUniqueObjectCount},
        {"semanticSceneDirectLastSubmittedObjectCount",
         snapshot.shadow.semanticSceneDirectLastSubmittedObjectCount},
        {"semanticSceneDirectLastRecordCapPartialObjectCount",
         snapshot.shadow.semanticSceneDirectLastRecordCapPartialObjectCount},
        {"semanticSceneDirectLastScanCapPartialObjectCount",
         snapshot.shadow.semanticSceneDirectLastScanCapPartialObjectCount},
        {"semanticSceneDirectLastMinGeosetsPerObject",
         snapshot.shadow.semanticSceneDirectLastMinGeosetsPerObject},
        {"semanticSceneDirectLastMaxGeosetsPerObject",
         snapshot.shadow.semanticSceneDirectLastMaxGeosetsPerObject},
        {"semanticSceneDirectLastSubmittedIdentityHash",
         snapshot.shadow.semanticSceneDirectLastSubmittedIdentityHash},
        {"semanticSceneDirectIdentityChurnCount",
         snapshot.shadow.semanticSceneDirectIdentityChurnCount},
        {"semanticSceneDirectRecordCapHitCount",
         snapshot.shadow.semanticSceneDirectRecordCapHitCount},
        {"semanticSceneDirectRecordCapTruncatedRecordCount",
         snapshot.shadow.semanticSceneDirectRecordCapTruncatedRecordCount},
        {"semanticSceneDirectScanCapHitCount",
         snapshot.shadow.semanticSceneDirectScanCapHitCount},
        {"semanticSceneDirectObjectGroupedSubmitCount",
         snapshot.shadow.semanticSceneDirectObjectGroupedSubmitCount},
        {"semanticSceneDirectObjectGroupedSkipCount",
         snapshot.shadow.semanticSceneDirectObjectGroupedSkipCount},
        {"semanticSceneDirectRecordCapSkipObjectCount",
         snapshot.shadow.semanticSceneDirectRecordCapSkipObjectCount},
        {"semanticSceneDirectRecordCapAppendFailCount",
         snapshot.shadow.semanticSceneDirectRecordCapAppendFailCount},
        {"semanticSceneDirectSelectionLeaseActiveKeyCount",
         snapshot.shadow.semanticSceneDirectSelectionLeaseActiveKeyCount},
        {"semanticSceneDirectSelectionLeasePrunedKeyCount",
         snapshot.shadow.semanticSceneDirectSelectionLeasePrunedKeyCount},
        {"semanticSceneDirectSelectionLeaseSubmittedKeyCount",
         snapshot.shadow.semanticSceneDirectSelectionLeaseSubmittedKeyCount},
        {"semanticSceneDirectStickyFillBudgetRecordCount",
         snapshot.shadow.semanticSceneDirectStickyFillBudgetRecordCount},
        {"semanticSceneDirectStickyFillAppendedCount",
         snapshot.shadow.semanticSceneDirectStickyFillAppendedCount},
        {"semanticSceneDirectStickyFillSubmittedCount",
         snapshot.shadow.semanticSceneDirectStickyFillSubmittedCount},
        {"semanticSceneDirectStickyFillMissedCount",
         snapshot.shadow.semanticSceneDirectStickyFillMissedCount},
        {"semanticSceneDirectPartLeaseRestoredCount",
         snapshot.shadow.semanticSceneDirectPartLeaseRestoredCount},
        {"semanticSceneDirectPartLeaseUpdatedCount",
         snapshot.shadow.semanticSceneDirectPartLeaseUpdatedCount},
        {"semanticSceneDirectPartLeaseExpiredCount",
         snapshot.shadow.semanticSceneDirectPartLeaseExpiredCount},
        {"semanticSceneDirectPartLeaseRejectedDynamicMeshCount",
         snapshot.shadow.semanticSceneDirectPartLeaseRejectedDynamicMeshCount},
        {"semanticSceneDirectPartLeaseRejectedNotSelfContainedCount",
         snapshot.shadow
             .semanticSceneDirectPartLeaseRejectedNotSelfContainedCount},
        {"semanticSceneDirectPartLeaseRejectedUnsafeBackingCount",
         snapshot.shadow.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount},
        {"semanticSceneDirectPartLeaseRejectedSelfRenewCount",
         snapshot.shadow.semanticSceneDirectPartLeaseRejectedSelfRenewCount},
        {"semanticSceneDirectPartLeaseBudgetLimitCount",
         snapshot.shadow.semanticSceneDirectPartLeaseBudgetLimitCount},
        {"semanticSceneShadowManifestPartLeaseRestoredCount",
         snapshot.shadow.semanticSceneShadowManifestPartLeaseRestoredCount},
        {"semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount},
        {"semanticSceneShadowManifestPartLeaseExpiredCount",
         snapshot.shadow.semanticSceneShadowManifestPartLeaseExpiredCount},
        {"semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount},
        {"semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount},
        {"semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount},
        {"semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount},
        {"semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount},
        {"semanticSceneShadowManifestPartLeaseBudgetLimitCount",
         snapshot.shadow.semanticSceneShadowManifestPartLeaseBudgetLimitCount},
        {"semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount},
        {"semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount},
        {"semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount},
        {"semanticSceneShadowManifestObjectCoreCompleteCount",
         snapshot.shadow.semanticSceneShadowManifestObjectCoreCompleteCount},
        {"semanticSceneShadowManifestObjectCoreIncompleteSkipCount",
         snapshot.shadow
             .semanticSceneShadowManifestObjectCoreIncompleteSkipCount},
        {"semanticSceneShadowManifestPartOmittedIncompleteCoreCount",
         snapshot.shadow
             .semanticSceneShadowManifestPartOmittedIncompleteCoreCount},
        {"semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount",
         snapshot.shadow
             .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount},
        {"semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount",
         snapshot.shadow
             .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount},
        {"semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount",
         snapshot.shadow
             .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount},
        {"semanticSceneShadowManifestObjectCoreEpochMissingPartCount",
         snapshot.shadow
             .semanticSceneShadowManifestObjectCoreEpochMissingPartCount},
        {"semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount",
         snapshot.shadow
             .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount},
        // Phase 7.28：skinned palette content stability probe。
        {"semanticSceneSubmittedSkinnedPaletteSourceNoneCount",
         snapshot.shadow.semanticSceneSubmittedSkinnedPaletteSourceNoneCount},
        {"semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount},
        {"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount},
        {"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount},
        {"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount},
        {"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount},
        {"semanticSceneSubmittedSkinnedPaletteStablePartSampleCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteStablePartSampleCount},
        {"semanticSceneSubmittedSkinnedPaletteHashChurnCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteHashChurnCount},
        {"semanticSceneSubmittedSkinnedPaletteSourceChurnCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteSourceChurnCount},
        {"semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount},
        {"semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax},
        {"semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax},
        {"semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount},
        {"semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount},
        {"semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount},
        {"semanticSceneSubmittedSkinnedPaletteCountChurnCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteCountChurnCount},
        {"semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount},
        {"semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount},
        {"semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount},
        {"semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount},
        {"semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount},
        {"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount},
        {"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount},
        {"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount},
        {"semanticSceneDirectPaletteAttributionSnapshotHitCount",
         snapshot.shadow
             .semanticSceneDirectPaletteAttributionSnapshotHitCount},
        {"semanticSceneDirectPaletteCaptureTrustedSourceHitCount",
         snapshot.shadow
             .semanticSceneDirectPaletteCaptureTrustedSourceHitCount},
        {"semanticSceneDirectPaletteCaptureTrustedSourceMissCount",
         snapshot.shadow
             .semanticSceneDirectPaletteCaptureTrustedSourceMissCount},
        {"semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount},
        {"semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount},
        {"semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount",
         snapshot.shadow
             .semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount},
        {"semanticSceneDirectManifestObjectCount",
         snapshot.shadow.semanticSceneDirectManifestObjectCount},
        {"semanticSceneDirectManifestObservedPartCount",
         snapshot.shadow.semanticSceneDirectManifestObservedPartCount},
        {"semanticSceneDirectManifestShadowEligiblePartCount",
         snapshot.shadow.semanticSceneDirectManifestShadowEligiblePartCount},
        {"semanticSceneDirectObjectCompleteEligibleCount",
         snapshot.shadow.semanticSceneDirectObjectCompleteEligibleCount},
        {"semanticSceneDirectObjectIncompleteByScanCapCount",
         snapshot.shadow.semanticSceneDirectObjectIncompleteByScanCapCount},
        {"semanticSceneDirectObjectIncompleteByAlphaPolicyCount",
         snapshot.shadow.semanticSceneDirectObjectIncompleteByAlphaPolicyCount},
        {"semanticSceneDirectObjectIncompleteBySliceUnresolvedCount",
         snapshot.shadow
             .semanticSceneDirectObjectIncompleteBySliceUnresolvedCount},
        {"semanticSceneDirectObjectIncompleteByPacketBuildFailCount",
         snapshot.shadow
             .semanticSceneDirectObjectIncompleteByPacketBuildFailCount},
        {"semanticSceneDirectObjectIncompleteByAppendFailCount",
         snapshot.shadow.semanticSceneDirectObjectIncompleteByAppendFailCount},
        {"semanticSceneDirectSubmittedCompleteObjectCount",
         snapshot.shadow.semanticSceneDirectSubmittedCompleteObjectCount},
        {"semanticSceneDirectSubmittedPartialObjectCount",
         snapshot.shadow.semanticSceneDirectSubmittedPartialObjectCount},
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
        {"semanticSceneShadowManifestObjectCount",
         snapshot.shadow.semanticSceneShadowManifestObjectCount},
        {"semanticSceneShadowManifestPartCount",
         snapshot.shadow.semanticSceneShadowManifestPartCount},
        {"semanticSceneShadowManifestStableObjectCount",
         snapshot.shadow.semanticSceneShadowManifestStableObjectCount},
        {"semanticSceneShadowManifestNewObjectCount",
         snapshot.shadow.semanticSceneShadowManifestNewObjectCount},
        {"semanticSceneShadowManifestExpiredObjectCount",
         snapshot.shadow.semanticSceneShadowManifestExpiredObjectCount},
        {"semanticSceneShadowManifestFreshPartCount",
         snapshot.shadow.semanticSceneShadowManifestFreshPartCount},
        {"semanticSceneShadowManifestLeaseablePartCount",
         snapshot.shadow.semanticSceneShadowManifestLeaseablePartCount},
        {"semanticSceneShadowManifestPoseStalePartCount",
         snapshot.shadow.semanticSceneShadowManifestPoseStalePartCount},
        {"semanticSceneShadowManifestSliceStalePartCount",
         snapshot.shadow.semanticSceneShadowManifestSliceStalePartCount},
        {"semanticSceneShadowManifestExpiredPartCount",
         snapshot.shadow.semanticSceneShadowManifestExpiredPartCount},
        {"semanticSceneShadowManifestMultiSlicePartCount",
         snapshot.shadow.semanticSceneShadowManifestMultiSlicePartCount},
        {"semanticSceneShadowManifestPayload11CChurnCount",
         snapshot.shadow.semanticSceneShadowManifestPayload11CChurnCount},
        {"semanticSceneShadowManifestRenderablePartChurnCount",
         snapshot.shadow
             .semanticSceneShadowManifestRenderablePartChurnCount},
        {"semanticSceneShadowManifestCModelPoseHitCount",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseHitCount},
        {"semanticSceneShadowManifestCModelPoseMissCount",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseMissCount},
        {"semanticSceneShadowManifestCModelPoseNoRuntimeCount",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseNoRuntimeCount},
        {"semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr",
         snapshot.shadow
             .semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr},
        {"semanticSceneShadowManifestCModelPoseLastMatrixCount",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseLastMatrixCount},
        {"semanticSceneShadowManifestCModelPoseLastMatrixHash",
         snapshot.shadow.semanticSceneShadowManifestCModelPoseLastMatrixHash},
        {"semanticSceneSubmittedObjectJaccardMilli",
         snapshot.shadow.semanticSceneSubmittedObjectJaccardMilli},
        {"semanticSceneSubmittedPartJaccardMilli",
         snapshot.shadow.semanticSceneSubmittedPartJaccardMilli},
        {"semanticSceneVisibleLookupPartLayerHitCount",
         snapshot.shadow.semanticSceneVisibleLookupPartLayerHitCount},
        {"semanticSceneVisibleLookupSingleFallbackCount",
         snapshot.shadow.semanticSceneVisibleLookupSingleFallbackCount},
        {"semanticSceneVisibleLookupMissCount",
         snapshot.shadow.semanticSceneVisibleLookupMissCount},
        {"semanticSceneDirectMainWorldBackingNotCheckedCount",
         snapshot.shadow.semanticSceneDirectMainWorldBackingNotCheckedCount},
        {"semanticSceneDirectMainWorldBackingPassCount",
         snapshot.shadow.semanticSceneDirectMainWorldBackingPassCount},
        {"semanticSceneDirectMainWorldBackingFailNoRenderablePartCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailNoRenderablePartCount},
        {"semanticSceneDirectMainWorldBackingFailLookupMissCount",
         snapshot.shadow.semanticSceneDirectMainWorldBackingFailLookupMissCount},
        {"semanticSceneDirectMainWorldBackingFailNonMainQueueCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailNonMainQueueCount},
        {"semanticSceneDirectMainWorldBackingFailNonWorldGroupCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailNonWorldGroupCount},
        {"semanticSceneDirectMainWorldBackingFailIdentityMismatchCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailIdentityMismatchCount},
        {"semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount},
        {"semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount",
         snapshot.shadow
             .semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount},
        {"semanticSceneDirectPaletteHashChurnCount",
         snapshot.shadow.semanticSceneDirectPaletteHashChurnCount},
        {"semanticSceneDirectGroupHashChurnCount",
         snapshot.shadow.semanticSceneDirectGroupHashChurnCount},
        {"semanticSceneDirectStableGroupHashChurnCount",
         snapshot.shadow.semanticSceneDirectStableGroupHashChurnCount},
        {"semanticSceneDirectStream1PtrChurnCount",
         snapshot.shadow.semanticSceneDirectStream1PtrChurnCount},
        {"semanticSceneDirectGeometrySourceHashChurnCount",
         snapshot.shadow.semanticSceneDirectGeometrySourceHashChurnCount},
        {"semanticSceneDirectSameCasterComparisonCount",
         snapshot.shadow.semanticSceneDirectSameCasterComparisonCount},
        {"semanticSceneDirectIdentitySkippedChurnCount",
         snapshot.shadow.semanticSceneDirectIdentitySkippedChurnCount},
        {"semanticSceneDirectPaletteRootDeltaSampleCount",
         snapshot.shadow.semanticSceneDirectPaletteRootDeltaSampleCount},
        {"semanticSceneDirectPaletteRootHashChangedTinyDeltaCount",
         snapshot.shadow
             .semanticSceneDirectPaletteRootHashChangedTinyDeltaCount},
        {"semanticSceneDirectPaletteRootHashChangedSmallDeltaCount",
         snapshot.shadow
             .semanticSceneDirectPaletteRootHashChangedSmallDeltaCount},
        {"semanticSceneDirectPaletteRootHashChangedMediumDeltaCount",
         snapshot.shadow
             .semanticSceneDirectPaletteRootHashChangedMediumDeltaCount},
        {"semanticSceneDirectPaletteRootHashChangedLargeDeltaCount",
         snapshot.shadow
             .semanticSceneDirectPaletteRootHashChangedLargeDeltaCount},
        {"semanticSceneDirectPaletteRootMaxDeltaMilli",
         snapshot.shadow.semanticSceneDirectPaletteRootMaxDeltaMilli},
        {"semanticSceneDirectSelectionKeyUnitPtrCount",
         snapshot.shadow.semanticSceneDirectSelectionKeyUnitPtrCount},
        {"semanticSceneDirectSelectionKeyJHandleCount",
         snapshot.shadow.semanticSceneDirectSelectionKeyJHandleCount},
        {"semanticSceneDirectSelectionKeyRuntimeModelCount",
         snapshot.shadow.semanticSceneDirectSelectionKeyRuntimeModelCount},
        {"semanticSceneDirectSelectionKeyWorldObjectCount",
         snapshot.shadow.semanticSceneDirectSelectionKeyWorldObjectCount},
        {"semanticSceneDirectSelectionKeySceneNodeCount",
         snapshot.shadow.semanticSceneDirectSelectionKeySceneNodeCount},
        {"semanticSceneDirectSelectionKeyModelMeshCount",
         snapshot.shadow.semanticSceneDirectSelectionKeyModelMeshCount},
        {"semanticSceneDirectSelectionKeyRenderablePartCount",
         snapshot.shadow.semanticSceneDirectSelectionKeyRenderablePartCount},
        {"semanticSceneDirectLastSubmittedSceneNode",
         snapshot.shadow.semanticSceneDirectLastSubmittedSceneNode},
        {"semanticSceneDirectLastSubmittedPaletteHash",
         snapshot.shadow.semanticSceneDirectLastSubmittedPaletteHash},
        {"semanticSceneDirectLastSubmittedGroupHash",
         snapshot.shadow.semanticSceneDirectLastSubmittedGroupHash},
        {"semanticSceneDirectLastSubmittedStableGroupHash",
         snapshot.shadow.semanticSceneDirectLastSubmittedStableGroupHash},
        {"semanticSceneLastAppendedGeometrySourceHash",
         snapshot.shadow.semanticSceneLastAppendedGeometrySourceHash},
        {"semanticSceneLastAppendedGeometryId",
         snapshot.shadow.semanticSceneLastAppendedGeometryId},
        {"semanticSceneShadowCastersCount",
         snapshot.shadow.semanticSceneShadowCastersCount},
        {"semanticSceneReplayDrawsCount",
         snapshot.shadow.semanticSceneReplayDrawsCount},
        {"semanticSceneShadowMapDrawnCasters",
         snapshot.shadow.semanticSceneShadowMapDrawnCasters},
        {"semanticSceneShadowMapCascadeCulledCount",
         snapshot.shadow.semanticSceneShadowMapCascadeCulledCount},
        {"semanticSceneTerrainBoundsCullMode",
         snapshot.shadow.semanticSceneTerrainBoundsCullMode},
        {"semanticSceneTerrainBoundsCandidateCount",
         snapshot.shadow.semanticSceneTerrainBoundsCandidateCount},
        {"semanticSceneTerrainBoundsProofAcceptedCount",
         snapshot.shadow.semanticSceneTerrainBoundsProofAcceptedCount},
        {"semanticSceneTerrainBoundsFailVisibleCount",
         snapshot.shadow.semanticSceneTerrainBoundsFailVisibleCount},
        {"semanticSceneTerrainBoundsWouldCullCount",
         snapshot.shadow.semanticSceneTerrainBoundsWouldCullCount},
        {"semanticSceneTerrainBoundsAppliedCullCount",
         snapshot.shadow.semanticSceneTerrainBoundsAppliedCullCount},
        {"semanticSceneTerrainBoundsC0WouldCullCount",
         snapshot.shadow.semanticSceneTerrainBoundsC0WouldCullCount},
        {"semanticSceneTerrainBoundsC1WouldCullCount",
         snapshot.shadow.semanticSceneTerrainBoundsC1WouldCullCount},
        {"semanticSceneTerrainBoundsC2WouldCullCount",
         snapshot.shadow.semanticSceneTerrainBoundsC2WouldCullCount},
        {"semanticSceneTerrainBoundsC3WouldCullCount",
         snapshot.shadow.semanticSceneTerrainBoundsC3WouldCullCount},
        {"semanticSceneObjectBoundsCandidateCount",
         snapshot.shadow.semanticSceneObjectBoundsCandidateCount},
        {"semanticSceneObjectBoundsProofAcceptedCount",
         snapshot.shadow.semanticSceneObjectBoundsProofAcceptedCount},
        {"semanticSceneObjectBoundsFailVisibleCount",
         snapshot.shadow.semanticSceneObjectBoundsFailVisibleCount},
        {"semanticSceneObjectBoundsWouldCullCount",
         snapshot.shadow.semanticSceneObjectBoundsWouldCullCount},
        {"semanticSceneObjectBoundsAppliedCullCount",
         snapshot.shadow.semanticSceneObjectBoundsAppliedCullCount},
        {"semanticSceneShadowMapSkinnedCasterCount",
         snapshot.shadow.semanticSceneShadowMapSkinnedCasterCount},
        {"semanticSceneShadowMapSkinnedPreparedCount",
         snapshot.shadow.semanticSceneShadowMapSkinnedPreparedCount},
        {"semanticSceneShadowMapSkinnedInvalidBufferCount",
         snapshot.shadow.semanticSceneShadowMapSkinnedInvalidBufferCount},
        {"semanticSceneShadowMapSkinnedInvalidPipelineCount",
         snapshot.shadow.semanticSceneShadowMapSkinnedInvalidPipelineCount},
        {"semanticSceneShadowMapSkinnedDrawnCount",
         snapshot.shadow.semanticSceneShadowMapSkinnedDrawnCount},
        {"gpuSkinVsShadowDirectAttempts",
         snapshot.shadow.gpuSkinVsShadowDirectAttempts},
        {"gpuSkinVsShadowDirectInputRejects",
         snapshot.shadow.gpuSkinVsShadowDirectInputRejects},
        {"gpuSkinVsShadowDirectStateRejects",
         snapshot.shadow.gpuSkinVsShadowDirectStateRejects},
        {"gpuSkinVsShadowDirectDrawsSubmitted",
         snapshot.shadow.gpuSkinVsShadowDirectDrawsSubmitted},
        {"gpuSkinVsShadowReplayDirectional",
         snapshot.shadow.gpuSkinVsShadowReplayDirectional},
        {"gpuSkinVsShadowReplayPoint",
         snapshot.shadow.gpuSkinVsShadowReplayPoint},
        {"semanticSceneShadowTaaActive",
         snapshot.shadow.semanticSceneShadowTaaActive},
        {"shadowTaaRequestedMode", snapshot.shadow.shadowTaaRequestedMode},
        {"shadowTaaEffectiveMode", snapshot.shadow.shadowTaaEffectiveMode},
        {"shadowTaaShaderMode", snapshot.shadow.shadowTaaShaderMode},
        {"shadowTaaHistoryValid", snapshot.shadow.shadowTaaHistoryValid},
        {"shadowTaaHistoryReadable", snapshot.shadow.shadowTaaHistoryReadable},
        {"shadowTaaHistoryGeneration",
         snapshot.shadow.shadowTaaHistoryGeneration},
        {"shadowTaaLastInvalidationReason",
         snapshot.shadow.shadowTaaLastInvalidationReason},
        {"shadowTaaFixedWallBypassCount",
         snapshot.shadow.shadowTaaFixedWallBypassCount},
        {"pointShadowPersistentConfiguredMode",
         snapshot.shadow.pointShadowPersistentConfiguredMode},
        {"pointShadowPersistentEffectiveMode",
         snapshot.shadow.pointShadowPersistentEffectiveMode},
        {"pointShadowPersistentLastBeginRejectReason",
         snapshot.shadow.pointShadowPersistentLastBeginRejectReason},
        {"pointShadowPersistentWorkerCreated",
         snapshot.shadow.pointShadowPersistentWorkerCreated},
        {"pointShadowPersistentWorkerAvailable",
         snapshot.shadow.pointShadowPersistentWorkerAvailable},
        {"pointShadowPersistentLastFrameSerial",
         snapshot.shadow.pointShadowPersistentLastFrameSerial},
        {"pointShadowPersistentBeginAttempts",
         snapshot.shadow.pointShadowPersistentBeginAttempts},
        {"pointShadowPersistentBeginEligible",
         snapshot.shadow.pointShadowPersistentBeginEligible},
        {"pointShadowPersistentWorkerCreateCount",
         snapshot.shadow.pointShadowPersistentWorkerCreateCount},
        {"pointShadowPersistentWorkerThreadStarts",
         snapshot.shadow.pointShadowPersistentWorkerThreadStarts},
        {"pointShadowPersistentAccepted",
         snapshot.shadow.pointShadowPersistentAccepted},
        {"pointShadowPersistentReady",
         snapshot.shadow.pointShadowPersistentReady},
        {"pointShadowPersistentDeadlineFallback",
         snapshot.shadow.pointShadowPersistentDeadlineFallback},
        {"pointShadowPersistentRejectedFallback",
         snapshot.shadow.pointShadowPersistentRejectedFallback},
        {"pointShadowPersistentObserveMatch",
         snapshot.shadow.pointShadowPersistentObserveMatch},
        {"pointShadowPersistentMismatch",
         snapshot.shadow.pointShadowPersistentMismatch},
        {"pointShadowPersistentConsumed",
         snapshot.shadow.pointShadowPersistentConsumed},
        {"pointShadowPersistentFailed",
         snapshot.shadow.pointShadowPersistentFailed},
        {"pointShadowPersistentBusy",
         snapshot.shadow.pointShadowPersistentBusy},
        {"persistentPackageConfiguredMode",
         snapshot.shadow.persistentPackageConfiguredMode},
        {"persistentPackageEffectiveMode",
         snapshot.shadow.persistentPackageEffectiveMode},
        {"persistentPackageOwnerAlive",
         snapshot.shadow.persistentPackageOwnerAlive},
        {"persistentPackageObserveCalls",
         snapshot.shadow.persistentPackageObserveCalls},
        {"persistentPackageExactSourcesAccepted",
         snapshot.shadow.persistentPackageExactSourcesAccepted},
        {"persistentPackageInvalidEvidence",
         snapshot.shadow.persistentPackageInvalidEvidence},
        {"persistentPackageInvalidSnapshots",
         snapshot.shadow.persistentPackageInvalidSnapshots},
        {"persistentPackageEpochRejects",
         snapshot.shadow.persistentPackageEpochRejects},
        {"persistentPackageReady", snapshot.shadow.persistentPackageReady},
        {"persistentPackageMiss", snapshot.shadow.persistentPackageMiss},
        {"persistentPackagePending",
         snapshot.shadow.persistentPackagePending},
        {"persistentPackageStoreRejects",
         snapshot.shadow.persistentPackageStoreRejects},
        {"persistentPackageMultiPrimitive",
         snapshot.shadow.persistentPackageMultiPrimitive},
        {"persistentPackageSubmissionsBuilt",
         snapshot.shadow.persistentPackageSubmissionsBuilt},
        {"persistentPackageSubmissionsCommitted",
         snapshot.shadow.persistentPackageSubmissionsCommitted},
        {"persistentPackageSubmissionsRejected",
         snapshot.shadow.persistentPackageSubmissionsRejected},
        {"persistentPackageUploadsCommitted",
         snapshot.shadow.persistentPackageUploadsCommitted},
        {"persistentPackageUploadBytesCommitted",
         snapshot.shadow.persistentPackageUploadBytesCommitted},
        {"persistentPackageProducerFenceSubmitted",
         snapshot.shadow.persistentPackageProducerFenceSubmitted},
        {"persistentPackageProducerFenceCompleted",
         snapshot.shadow.persistentPackageProducerFenceCompleted},
        {"persistentPackageStaticCacheHits",
         snapshot.shadow.persistentPackageStaticCacheHits},
        {"persistentPackageStaticCacheMisses",
         snapshot.shadow.persistentPackageStaticCacheMisses},
        {"persistentPackageStaticFallbacks",
         snapshot.shadow.persistentPackageStaticFallbacks},
        {"persistentPackageStaticUploadsCompleted",
         snapshot.shadow.persistentPackageStaticUploadsCompleted},
        {"persistentPackageStaticUploadCompletionsRejected",
         snapshot.shadow.persistentPackageStaticUploadCompletionsRejected},
        {"persistentPackageCurrentMapEpoch",
         snapshot.shadow.persistentPackageCurrentMapEpoch},
        {"persistentPackageCurrentDeviceEpoch",
         snapshot.shadow.persistentPackageCurrentDeviceEpoch},
        {"persistentPackageCurrentFrameSerial",
         snapshot.shadow.persistentPackageCurrentFrameSerial},
        {"persistentPackageCurrentDrawConfiguredMode",
         snapshot.shadow.persistentPackageCurrentDrawConfiguredMode},
        {"persistentPackageCurrentDrawEffectiveMode",
         snapshot.shadow.persistentPackageCurrentDrawEffectiveMode},
        {"persistentPackageCurrentDrawObservations",
         snapshot.shadow.persistentPackageCurrentDrawObservations},
        {"persistentPackageCurrentDrawExactMatches",
         snapshot.shadow.persistentPackageCurrentDrawExactMatches},
        {"persistentPackageCurrentDrawWouldUseCsm",
         snapshot.shadow.persistentPackageCurrentDrawWouldUseCsm},
        {"persistentPackageCurrentDrawRejected",
         snapshot.shadow.persistentPackageCurrentDrawRejected},
        {"persistentPackageCurrentDrawNotRigidStatic",
         snapshot.shadow.persistentPackageCurrentDrawNotRigidStatic},
        {"persistentPackageCurrentDrawMaterialRejected",
         snapshot.shadow.persistentPackageCurrentDrawMaterialRejected},
        {"persistentPackageCurrentDrawSkinningRejected",
         snapshot.shadow.persistentPackageCurrentDrawSkinningRejected},
        {"persistentPackageCurrentDrawGeometryRejected",
         snapshot.shadow.persistentPackageCurrentDrawGeometryRejected},
        {"persistentPackageCurrentDrawGeometryPositionNotHostCached",
         snapshot.shadow
             .persistentPackageCurrentDrawGeometryPositionNotHostCached},
        {"persistentPackageCurrentDrawGeometryIndexProofUnavailable",
         snapshot.shadow
             .persistentPackageCurrentDrawGeometryIndexProofUnavailable},
        {"persistentPackageCurrentDrawBoundedIndexScans",
         snapshot.shadow.persistentPackageCurrentDrawBoundedIndexScans},
        {"persistentPackageCurrentDrawBoundedIndexScanBytes",
         snapshot.shadow.persistentPackageCurrentDrawBoundedIndexScanBytes},
        {"persistentPackageCurrentDrawBoundedIndexScanTicks",
         snapshot.shadow.persistentPackageCurrentDrawBoundedIndexScanTicks},
        {"persistentPackageCurrentDrawBoundedPositionCopies",
         snapshot.shadow.persistentPackageCurrentDrawBoundedPositionCopies},
        {"persistentPackageCurrentDrawBoundedPositionCopyBytes",
         snapshot.shadow
             .persistentPackageCurrentDrawBoundedPositionCopyBytes},
        {"persistentPackageCurrentDrawBoundedPositionCopyTicks",
         snapshot.shadow
             .persistentPackageCurrentDrawBoundedPositionCopyTicks},
        {"persistentPackageCurrentDrawContentHashBytes",
         snapshot.shadow.persistentPackageCurrentDrawContentHashBytes},
        {"persistentPackageCurrentDrawContentHashTicks",
         snapshot.shadow.persistentPackageCurrentDrawContentHashTicks},
        {"persistentPackageCurrentDrawProofBudgetRejected",
         snapshot.shadow.persistentPackageCurrentDrawProofBudgetRejected},
        {"persistentPackageCaptureBoundedIndexScans",
         snapshot.shadow.persistentPackageCaptureBoundedIndexScans},
        {"persistentPackageCaptureBoundedIndexScanBytes",
         snapshot.shadow.persistentPackageCaptureBoundedIndexScanBytes},
        {"persistentPackageCaptureBoundedIndexScanTicks",
         snapshot.shadow.persistentPackageCaptureBoundedIndexScanTicks},
        {"persistentPackageCaptureBoundedPositionCopies",
         snapshot.shadow.persistentPackageCaptureBoundedPositionCopies},
        {"persistentPackageCaptureBoundedPositionCopyBytes",
         snapshot.shadow.persistentPackageCaptureBoundedPositionCopyBytes},
        {"persistentPackageCaptureBoundedPositionCopyTicks",
         snapshot.shadow.persistentPackageCaptureBoundedPositionCopyTicks},
        {"persistentPackageCaptureContentHashBytes",
         snapshot.shadow.persistentPackageCaptureContentHashBytes},
        {"persistentPackageCaptureContentHashTicks",
         snapshot.shadow.persistentPackageCaptureContentHashTicks},
        {"persistentPackageCaptureProofBudgetRejected",
         snapshot.shadow.persistentPackageCaptureProofBudgetRejected},
        {"persistentPackageCaptureTimerFrequency",
         snapshot.shadow.persistentPackageCaptureTimerFrequency},
        {"persistentPackageCurrentDrawCpuSourceUnavailable",
         snapshot.shadow.persistentPackageCurrentDrawCpuSourceUnavailable},
        {"persistentPackageCurrentDrawSourceGenerationMissing",
         snapshot.shadow.persistentPackageCurrentDrawSourceGenerationMissing},
        {"persistentPackageCurrentDrawPackageNotReady",
         snapshot.shadow.persistentPackageCurrentDrawPackageNotReady},
        {"persistentPackageCurrentDrawPackageInvalid",
         snapshot.shadow.persistentPackageCurrentDrawPackageInvalid},
        {"persistentPackageCurrentDrawSnapshotMismatch",
         snapshot.shadow.persistentPackageCurrentDrawSnapshotMismatch},
        {"persistentPackageCurrentDrawMultiPrimitiveRejected",
         snapshot.shadow.persistentPackageCurrentDrawMultiPrimitiveRejected},
        {"persistentPackageCurrentDrawPackageLayoutMismatch",
         snapshot.shadow.persistentPackageCurrentDrawPackageLayoutMismatch},
        {"persistentPackageCurrentDrawPositionMismatch",
         snapshot.shadow.persistentPackageCurrentDrawPositionMismatch},
        {"persistentPackageCurrentDrawIndexMismatch",
         snapshot.shadow.persistentPackageCurrentDrawIndexMismatch},
        {"persistentPackageCurrentDrawPrimitiveMismatch",
         snapshot.shadow.persistentPackageCurrentDrawPrimitiveMismatch},
        {"persistentPackageCurrentDrawLastDisposition",
         snapshot.shadow.persistentPackageCurrentDrawLastDisposition},
        {"persistentPackageGpuBindingAllowed",
         snapshot.shadow.persistentPackageGpuBindingAllowed},
        {"persistentPackageDrawMutationAllowed",
         snapshot.shadow.persistentPackageDrawMutationAllowed},
        {"persistentPackageConsumerAuthorityPublished",
         snapshot.shadow.persistentPackageConsumerAuthorityPublished},
        {"persistentPackageConsumerLastUseFencePublished",
         snapshot.shadow.persistentPackageConsumerLastUseFencePublished},
        {"csmRequestedResolution", snapshot.shadow.csmRequestedResolution},
        {"csmEffectiveResolution", snapshot.shadow.csmEffectiveResolution},
        {"csmFallbackReason", snapshot.shadow.csmFallbackReason},
        {"csmFallbackLatched", snapshot.shadow.csmFallbackLatched},
        {"csmResourceGeneration", snapshot.shadow.csmResourceGeneration},
        {"csmResourceRebuildCount", snapshot.shadow.csmResourceRebuildCount},
        {"csmMemoryBudgetBytes", snapshot.shadow.csmMemoryBudgetBytes},
        {"csmMemoryAvailableBytes", snapshot.shadow.csmMemoryAvailableBytes},
        {"shadowArenaUsedBytes", snapshot.shadow.shadowArenaUsedBytes},
        {"shadowArenaResidentBytes", snapshot.shadow.shadowArenaResidentBytes},
        {"shadowArenaResidentLimitBytes",
         snapshot.shadow.shadowArenaResidentLimitBytes},
        {"shadowArenaGeneration", snapshot.shadow.shadowArenaGeneration},
        {"shadowArenaQuarantineCount",
         snapshot.shadow.shadowArenaQuarantineCount},
        {"shadowArenaLastQuarantinedGeneration",
         snapshot.shadow.shadowArenaLastQuarantinedGeneration},
        {"shadowArenaLastQuarantinedRetireSerial",
         snapshot.shadow.shadowArenaLastQuarantinedRetireSerial},
        {"shadowArenaBusyReuseRejectCount",
         snapshot.shadow.shadowArenaBusyReuseRejectCount},
        {"shadowArenaOverflowCount",
         snapshot.shadow.shadowArenaOverflowCount},
        {"shadowArenaReservedBytes",
         snapshot.shadow.shadowArenaReservedBytes},
        {"shadowArenaCommittedBytes",
         snapshot.shadow.shadowArenaCommittedBytes},
        {"shadowArenaRolledBackBytes",
         snapshot.shadow.shadowArenaRolledBackBytes},
        {"shadowArenaAdmissionRejectedCount",
         snapshot.shadow.shadowArenaAdmissionRejectedCount},
        {"shadowArenaPartialTransactionCount",
         snapshot.shadow.shadowArenaPartialTransactionCount},
        {"shadowArenaPageTailWasteBytes",
         snapshot.shadow.shadowArenaPageTailWasteBytes},
        {"shadowArenaPositionBytes", snapshot.shadow.shadowArenaPositionBytes},
        {"shadowArenaBlendBytes", snapshot.shadow.shadowArenaBlendBytes},
        {"shadowArenaUvBytes", snapshot.shadow.shadowArenaUvBytes},
        {"shadowArenaIndexBytes", snapshot.shadow.shadowArenaIndexBytes},
        {"shadowArenaTerrainBytes", snapshot.shadow.shadowArenaTerrainBytes},
        {"shadowArenaModelBytes", snapshot.shadow.shadowArenaModelBytes},
        {"shadowArenaSkinnedBytes", snapshot.shadow.shadowArenaSkinnedBytes},
        {"shadowArenaUpBytes", snapshot.shadow.shadowArenaUpBytes},
        {"shadowArenaUniqueSourceBytes",
         snapshot.shadow.shadowArenaUniqueSourceBytes},
        {"shadowArenaDuplicateBytesSaved",
         snapshot.shadow.shadowArenaDuplicateBytesSaved},
        {"shadowArenaExactIndexTrimAcceptedCount",
         snapshot.shadow.shadowArenaExactIndexTrimAcceptedCount},
        {"shadowArenaExactIndexTrimRejectedCount",
         snapshot.shadow.shadowArenaExactIndexTrimRejectedCount},
        {"shadowArenaExactIndexTrimBytesSaved",
         snapshot.shadow.shadowArenaExactIndexTrimBytesSaved},
        {"shadowArenaFrameIncomplete",
         snapshot.shadow.shadowArenaFrameIncomplete},
        {"shadowCpuSpanAcceptedCount",
         snapshot.shadow.shadowCpuSpanAcceptedCount},
        {"shadowCpuSpanRejectedCount",
         snapshot.shadow.shadowCpuSpanRejectedCount},
        {"shadowCpuSpanLastRejectReason",
         snapshot.shadow.shadowCpuSpanLastRejectReason},
        {"shadowCpuSpanLastAllocationBytes",
         snapshot.shadow.shadowCpuSpanLastAllocationBytes},
        {"shadowCpuSpanLastBindingOffset",
         snapshot.shadow.shadowCpuSpanLastBindingOffset},
        {"shadowCpuSpanLastReadBytes",
         snapshot.shadow.shadowCpuSpanLastReadBytes},
        {"shadowCpuSpanLastSourceIdentityGeneration",
         snapshot.shadow.shadowCpuSpanLastSourceIdentityGeneration},
        {"shadowCpuSpanLastAllocationGeneration",
         snapshot.shadow.shadowCpuSpanLastAllocationGeneration},
        {"shadowCpuSpanLastContentGeneration",
         snapshot.shadow.shadowCpuSpanLastContentGeneration},
        {"shadowExactIndexDomainScannedBytes",
         snapshot.shadow.shadowExactIndexDomainScannedBytes},
        {"shadowExactIndexDomainNonHostCachedScanCount",
         snapshot.shadow.shadowExactIndexDomainNonHostCachedScanCount},
        {"shadowExactIndexDomainNonHostCachedScannedBytes",
         snapshot.shadow.shadowExactIndexDomainNonHostCachedScannedBytes},
        {"shadowExactIndexDomainBulkReadCount",
         snapshot.shadow.shadowExactIndexDomainBulkReadCount},
        {"shadowExactIndexDomainBulkReadBytes",
         snapshot.shadow.shadowExactIndexDomainBulkReadBytes},
        {"shadowExactIndexDomainDirectReadCount",
         snapshot.shadow.shadowExactIndexDomainDirectReadCount},
        {"shadowExactIndexDomainOversizeFallbackCount",
         snapshot.shadow.shadowExactIndexDomainOversizeFallbackCount},
        {"queueSubmittedSerial", snapshot.shadow.queueSubmittedSerial},
        {"queueCompletedSerial", snapshot.shadow.queueCompletedSerial},
        {"queueLastResult", snapshot.shadow.queueLastResult},
        {"shadowMapResetRequestedSerial",
         snapshot.shadow.shadowMapResetRequestedSerial},
        {"shadowMapResetAppliedSerial",
         snapshot.shadow.shadowMapResetAppliedSerial},
        {"shadowMapEpoch", snapshot.shadow.shadowMapEpoch},
        {"shadowMapResetAppliedFrameSerial",
         snapshot.shadow.shadowMapResetAppliedFrameSerial},
        {"shadowMapQuarantinedRetireSerial",
         snapshot.shadow.shadowMapQuarantinedRetireSerial},
        {"shadowMapCompletedRetireSerial",
         snapshot.shadow.shadowMapCompletedRetireSerial},
        {"shadowRetiredSessionCount",
         snapshot.shadow.shadowRetiredSessionCount},
        {"shadowRetiredSessionEntryCount",
         snapshot.shadow.shadowRetiredSessionEntryCount},
        {"shadowRetiredSessionAllocatorBytes",
         snapshot.shadow.shadowRetiredSessionAllocatorBytes},
        {"shadowRetiredSessionCachedGpuLogicalBytes",
         snapshot.shadow.shadowRetiredSessionCachedGpuLogicalBytes},
        {"shadowRetiredSessionCpuOwnedBytes",
         snapshot.shadow.shadowRetiredSessionCpuOwnedBytes},
        {"shadowRetiredSessionOldestRetireSerial",
         snapshot.shadow.shadowRetiredSessionOldestRetireSerial},
        {"shadowRetiredSessionCollectedCount",
         snapshot.shadow.shadowRetiredSessionCollectedCount},
        {"shadowRetiredLastMapEpoch",
         snapshot.shadow.shadowRetiredLastMapEpoch},
        {"shadowPendingProducerRejectCount",
         snapshot.shadow.shadowPendingProducerRejectCount},
        {"shadowStaleEpochConsumerRejectCount",
         snapshot.shadow.shadowStaleEpochConsumerRejectCount},
        {"shadowMapTransitionState",
         snapshot.shadow.shadowMapTransitionState},
        {"shadowMapProducerReady",
         snapshot.shadow.shadowMapProducerReady},
        {"shadowReplayCandidateFrameSerial",
         snapshot.shadow.shadowReplayCandidateFrameSerial},
        {"shadowReplayPlannedCasterCount",
         snapshot.shadow.shadowReplayPlannedCasterCount},
        {"shadowReplayCasterCount",
         snapshot.shadow.shadowReplayCasterCount},
        {"shadowReplayValidatedCasterCount",
         snapshot.shadow.shadowReplayValidatedCasterCount},
        {"shadowReplayDrawnCasterCount",
         snapshot.shadow.shadowReplayDrawnCasterCount},
        {"shadowReplayValidationRejectCount",
         snapshot.shadow.shadowReplayValidationRejectCount},
        {"shadowReplayPartialPreventedCount",
         snapshot.shadow.shadowReplayPartialPreventedCount},
        {"shadowReplayLastRejectReason",
         snapshot.shadow.shadowReplayLastRejectReason},
        {"shadowReplayLastOffenderMapEpoch",
         snapshot.shadow.shadowReplayLastOffenderMapEpoch},
        {"shadowReplayLastRequiredEnd",
         snapshot.shadow.shadowReplayLastRequiredEnd},
        {"shadowReplayLastAvailableSize",
         snapshot.shadow.shadowReplayLastAvailableSize},
        {"shadowReplayLastMinimumVertex",
         snapshot.shadow.shadowReplayLastMinimumVertex},
        {"shadowReplayLastMaximumVertex",
         snapshot.shadow.shadowReplayLastMaximumVertex},
        {"shadowReplayLastVertexOffset",
         snapshot.shadow.shadowReplayLastVertexOffset},
        {"shadowReplayLastStage", snapshot.shadow.shadowReplayLastStage},
        {"shadowReplayLastCategory",
         snapshot.shadow.shadowReplayLastCategory},
        {"shadowReplayLastBatchTag",
         snapshot.shadow.shadowReplayLastBatchTag},
        {"shadowReplayLastObjectKind",
         snapshot.shadow.shadowReplayLastObjectKind},
        {"shadowReplayLastRawcode", snapshot.shadow.shadowReplayLastRawcode},
        {"shadowReplayLastJHandle",
         snapshot.shadow.shadowReplayLastJHandle},
        {"shadowReplayLastIndexCount",
         snapshot.shadow.shadowReplayLastIndexCount},
        {"shadowReplayLastFirstIndex",
         snapshot.shadow.shadowReplayLastFirstIndex},
        {"shadowReplayLastMinVertexIndex",
         snapshot.shadow.shadowReplayLastMinVertexIndex},
        {"shadowReplayLastNumVertices",
         snapshot.shadow.shadowReplayLastNumVertices},
        {"shadowReplayLastActualIndexMin",
         snapshot.shadow.shadowReplayLastActualIndexMin},
        {"shadowReplayLastActualIndexMax",
         snapshot.shadow.shadowReplayLastActualIndexMax},
        {"shadowReplayLastActualIndexDomainKnown",
         snapshot.shadow.shadowReplayLastActualIndexDomainKnown},
        {"shadowReplayLastFullVertexDomainFallback",
         snapshot.shadow.shadowReplayLastFullVertexDomainFallback},
        {"shadowReplayLastPositionSize",
         snapshot.shadow.shadowReplayLastPositionSize},
        {"shadowFirstCompleteLatencyFrames",
         snapshot.shadow.shadowFirstCompleteLatencyFrames},
        {"shadowPointWorkerCancelCount",
         snapshot.shadow.shadowPointWorkerCancelCount},
        {"shadowPointLateResultRejectCount",
         snapshot.shadow.shadowPointLateResultRejectCount},
        {"shadowEvidenceRetentionRevision",
         snapshot.shadow.shadowEvidenceRetentionRevision},
        {"shadowEvidenceCollectorAttached",
         snapshot.shadow.shadowEvidenceCollectorAttached},
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
  const auto lightning =
      dxvk::war3::render::War3LightningRuntime::instance().snapshot();
  snapshot.lightning.activeCount = lightning.activeCount;
  snapshot.lightning.polylineActiveCount = lightning.polylineActiveCount;
  snapshot.lightning.templateCount = lightning.templateCount;
  snapshot.lightning.finalizedTemplateCount = lightning.finalizedTemplateCount;
  snapshot.lightning.textureCacheEntryCount = lightning.textureCacheEntryCount;
  snapshot.lightning.createCount = lightning.createCount;
  snapshot.lightning.polylineCreateCount = lightning.polylineCreateCount;
  snapshot.lightning.templateCreateCount = lightning.templateCreateCount;
  snapshot.lightning.templateFinalizeCount = lightning.templateFinalizeCount;
  snapshot.lightning.destroyCount = lightning.destroyCount;
  snapshot.lightning.commandFailureCount = lightning.commandFailureCount;
  snapshot.lightning.drawAttemptCount = lightning.drawAttemptCount;
  snapshot.lightning.drawSuccessCount = lightning.drawSuccessCount;
  snapshot.lightning.drawSkippedNoDeviceCount = lightning.drawSkippedNoDeviceCount;
  snapshot.lightning.drawSkippedNoActiveCount = lightning.drawSkippedNoActiveCount;
  snapshot.lightning.textureLoadAttemptCount = lightning.textureLoadAttemptCount;
  snapshot.lightning.textureLoadFallbackCount = lightning.textureLoadFallbackCount;
  snapshot.lightning.lastDrawVertexCount = lightning.lastDrawVertexCount;
  snapshot.lightning.lastDrawPrimitiveCount = lightning.lastDrawPrimitiveCount;
  snapshot.lightning.lastPolylinePointCount = lightning.lastPolylinePointCount;
  snapshot.lightning.hasDevice = lightning.hasDevice;
  snapshot.lightning.textureLoaded = lightning.textureLoaded;
  snapshot.lightning.textureFallback = lightning.textureFallback;
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

void SetGpuFlightBreadcrumb(
    GpuFlightBreadcrumb breadcrumb, uint32_t csmCascade,
    uint32_t pointLight, uint32_t pointFace) noexcept {
  s_gpuFlightActiveCsmCascade.store(csmCascade, std::memory_order_release);
  s_gpuFlightActivePointLight.store(pointLight, std::memory_order_release);
  s_gpuFlightActivePointFace.store(pointFace, std::memory_order_release);
  s_gpuFlightBreadcrumb.store(
      static_cast<uint32_t>(breadcrumb), std::memory_order_release);
  s_gpuFlightBreadcrumbSerial.fetch_add(1u, std::memory_order_acq_rel);
}

void ResetGpuFlightCsmWork() noexcept {
  for (size_t index = 0u; index < s_gpuFlightCsmCascadeDrawCount.size();
       ++index) {
    s_gpuFlightCsmCascadeDrawCount[index].store(0u, std::memory_order_release);
    s_gpuFlightCsmCascadeTriangleCount[index].store(
        0u, std::memory_order_release);
  }
}

void SetGpuFlightCsmCascadeWork(
    uint32_t cascade, uint32_t drawCount, uint64_t triangleCount) noexcept {
  if (cascade >= s_gpuFlightCsmCascadeDrawCount.size())
    return;
  s_gpuFlightCsmCascadeDrawCount[cascade].store(
      drawCount, std::memory_order_release);
  s_gpuFlightCsmCascadeTriangleCount[cascade].store(
      triangleCount, std::memory_order_release);
}

void ResetGpuFlightPointShadowWork(uint32_t lightCount) noexcept {
  s_gpuFlightPointShadowLightCount.store(lightCount, std::memory_order_release);
  for (size_t index = 0u;
       index < s_gpuFlightPointShadowFaceCandidateCount.size(); ++index) {
    s_gpuFlightPointShadowFaceCandidateCount[index].store(
        0u, std::memory_order_release);
    s_gpuFlightPointShadowFaceKeptCount[index].store(
        0u, std::memory_order_release);
    s_gpuFlightPointShadowFaceDrawCount[index].store(
        0u, std::memory_order_release);
    s_gpuFlightPointShadowFaceTriangleCount[index].store(
        0u, std::memory_order_release);
  }
}

void SetGpuFlightPointShadowFacePlan(
    uint32_t light, uint32_t face, uint32_t candidateCount,
    uint32_t keptCount) noexcept {
  if (light >= 4u || face >= 6u)
    return;
  const size_t index = size_t(light) * 6u + face;
  s_gpuFlightPointShadowFaceCandidateCount[index].store(
      candidateCount, std::memory_order_release);
  s_gpuFlightPointShadowFaceKeptCount[index].store(
      keptCount, std::memory_order_release);
}

void SetGpuFlightPointShadowFaceWork(
    uint32_t light, uint32_t face, uint32_t drawCount,
    uint64_t triangleCount) noexcept {
  if (light >= 4u || face >= 6u)
    return;
  const size_t index = size_t(light) * 6u + face;
  s_gpuFlightPointShadowFaceDrawCount[index].store(
      drawCount, std::memory_order_release);
  s_gpuFlightPointShadowFaceTriangleCount[index].store(
      triangleCount, std::memory_order_release);
}

void SetGpuFlightAutoTestContext(
    uint32_t waypointIndex, float targetX, float targetY, float panSeconds,
    float cameraTargetX, float cameraTargetY, float cameraTargetDistance,
    float cameraAngleOfAttack, float worldMinX, float worldMinY,
    float worldMaxX, float worldMaxY) noexcept {
  const std::array<float, 11u> values = {
      targetX, targetY, panSeconds, cameraTargetX, cameraTargetY,
      cameraTargetDistance, cameraAngleOfAttack, worldMinX, worldMinY,
      worldMaxX, worldMaxY};
  s_gpuFlightAutoTestContextValid.store(0u, std::memory_order_release);
  for (size_t index = 0u; index < values.size(); ++index) {
    s_gpuFlightAutoTestFloatBits[index].store(
        FloatBits(values[index]), std::memory_order_release);
  }
  s_gpuFlightAutoTestWaypointIndex.store(
      waypointIndex, std::memory_order_release);
  s_gpuFlightAutoTestContextValid.store(1u, std::memory_order_release);
}

void ClearGpuFlightAutoTestContext() noexcept {
  s_gpuFlightAutoTestContextValid.store(0u, std::memory_order_release);
  s_gpuFlightAutoTestWaypointIndex.store(
      0xFFFFFFFFu, std::memory_order_release);
}

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
  RecordGpuFlightFrame(frameIndex);
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

uint64_t RequestShadowEvidenceRetention() {
  return s_shadowEvidenceRetentionRevision.fetch_add(
             1u, std::memory_order_acq_rel) +
         1u;
}

uint64_t QueryShadowEvidenceRetentionRevision() {
  return s_shadowEvidenceRetentionRevision.load(std::memory_order_acquire);
}

void SetShadowEvidenceCollectorAttached(bool attached) {
  s_shadowEvidenceCollectorAttached.store(attached,
                                          std::memory_order_release);
}

bool IsShadowEvidenceCollectorAttached() {
  return s_shadowEvidenceCollectorAttached.load(std::memory_order_acquire);
}

} // namespace dxvk::war3::tools
