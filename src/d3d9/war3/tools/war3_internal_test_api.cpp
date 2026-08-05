#include "war3_internal_test_api.h"

#include "../../d3d9_device.h"
#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_settings.h"

#include "../gpu_skin/war3_gpu_skin_native_bridge.h"
#include "../hooks/war3_jass_command_bridge.h"
#include "../render/war3_render_state.h"
#include "../shader/war3_shader_manager.h"
#include "../war3.h"

#include "../../../util/log/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>

namespace dxvk::war3::tools {

namespace {

using json = nlohmann::json;

struct PendingInternalTestRequest {
  War3InternalTestRequest request = {};
  War3InternalTestResult result = {};
  bool done = false;
  std::condition_variable cv;
};

std::mutex s_mutex;
std::deque<std::shared_ptr<PendingInternalTestRequest>> s_pending;

struct OutlineTestModeState {
  bool active = false;
  uint64_t leaseId = 0u;
  uint64_t generation = 0u;
  bool allObjectsBefore = false;
  bool forceBefore = false;
  bool settingsCaptured = false;
  uint32_t activeStageMaskBefore = 0u;
  dxvk::War3OccludedOutlineSettings settingsBefore = {};
};

OutlineTestModeState s_outlineTestMode;

struct OutlineTestRestoreResult {
  bool attempted = false;
  bool shaderRestored = false;
  bool testLeaseActiveAfterRestore = false;
  bool restoreScopeClean = false;
  bool settingsRestored = false;
  bool ownershipRetainedForRetry = false;
  bool ok = false;
  uint32_t stageMaskBefore = 0u;
  uint32_t stageMaskAfter = 0u;
};

bool SameOutlineSettings(
    const dxvk::War3OccludedOutlineSettings& lhs,
    const dxvk::War3OccludedOutlineSettings& rhs) {
  return lhs.enabled == rhs.enabled && lhs.mode == rhs.mode &&
      lhs.useScreenSpace == rhs.useScreenSpace &&
      lhs.widthPx == rhs.widthPx &&
      lhs.showVisible == rhs.showVisible &&
      lhs.showOccluded == rhs.showOccluded &&
      lhs.colorR == rhs.colorR && lhs.colorG == rhs.colorG &&
      lhs.colorB == rhs.colorB && lhs.colorA == rhs.colorA;
}

OutlineTestRestoreResult RestoreOutlineTestModeState() {
  OutlineTestRestoreResult result;
  const OutlineTestModeState previous = s_outlineTestMode;
  result.attempted = previous.active;
  result.stageMaskBefore = previous.activeStageMaskBefore;
  if (!previous.active) {
    s_outlineTestMode = {};
    return result;
  }

  auto* settings = dxvk::war3::GetMutableSettings();
  if (previous.settingsCaptured && settings != nullptr)
    settings->occludedOutline = previous.settingsBefore;
  dxvk::War3RenderState::SetOutlineDebugAllObjectsEnabled(
      previous.allObjectsBefore);
  dxvk::War3RenderState::SetOutlineForceEnabled(previous.forceBefore);

  auto& shaderManager = dxvk::war3::ShaderManager::get();
  result.shaderRestored = shaderManager.restoreStageOverrideForTest(
      war3shader::RenderStageId::Outline,
      previous.leaseId, previous.generation);
  result.testLeaseActiveAfterRestore =
      shaderManager.isStageOverrideTestLeaseActive(
          war3shader::RenderStageId::Outline,
          previous.leaseId, previous.generation);
  result.stageMaskAfter = shaderManager.activeOverrideMaskForTest();
  result.restoreScopeClean = !result.testLeaseActiveAfterRestore &&
      result.stageMaskAfter == result.stageMaskBefore;
  const bool flagsRestored =
      dxvk::War3RenderState::IsOutlineDebugAllObjectsEnabled() ==
          previous.allObjectsBefore &&
      dxvk::War3RenderState::IsOutlineForceEnabledForTest() ==
          previous.forceBefore;
  result.settingsRestored = flagsRestored &&
      (!previous.settingsCaptured ||
       (settings != nullptr && SameOutlineSettings(
            settings->occludedOutline, previous.settingsBefore)));
  result.ok = result.shaderRestored && result.restoreScopeClean &&
      result.settingsRestored;
  if (result.testLeaseActiveAfterRestore) {
    // Preserve exact ownership so a later reset/disable can retry. Never leave
    // a live test overlay without its lease metadata.
    s_outlineTestMode = previous;
    result.ownershipRetainedForRetry = true;
  } else {
    s_outlineTestMode = {};
  }
  return result;
}

uint64_t GetEpochMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

void SetResult(std::shared_ptr<PendingInternalTestRequest>& state,
               const War3InternalTestResult& result) {
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    state->result = result;
    state->done = true;
  }
  state->cv.notify_all();
}

} // namespace

bool IsInternalTestApiEnabled() {
  return true;
}

void ResetInternalTestApiState() {
  std::deque<std::shared_ptr<PendingInternalTestRequest>> stale;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    stale.swap(s_pending);
  }

  for (auto& request : stale) {
    request->result.handled = true;
    request->result.ok = false;
    request->result.requestId = request->request.requestId;
    request->result.command = request->request.command;
    request->result.error = "internal test api reset";
    request->done = true;
    request->cv.notify_all();
  }

  const auto outlineRestore = RestoreOutlineTestModeState();
  if (outlineRestore.attempted && !outlineRestore.ok) {
    war3dbg::Print(
        "DXVK War3OutlineTest: reset restore failed shader=%u lease=%u "
        "scope=%u settings=%u mask=%u/%u\n",
        outlineRestore.shaderRestored ? 1u : 0u,
        outlineRestore.testLeaseActiveAfterRestore ? 1u : 0u,
        outlineRestore.restoreScopeClean ? 1u : 0u,
        outlineRestore.settingsRestored ? 1u : 0u,
        outlineRestore.stageMaskBefore,
        outlineRestore.stageMaskAfter);
  }
}

bool SubmitInternalTestRequest(const War3InternalTestRequest& request,
                               uint32_t timeoutMs,
                               War3InternalTestResult* outResult) {
  auto state = std::make_shared<PendingInternalTestRequest>();
  state->request = request;
  if (state->request.requestId.empty())
    state->request.requestId =
        "internal_test_" + std::to_string(GetEpochMs());
  state->request.issuedAtMs =
      state->request.issuedAtMs != 0 ? state->request.issuedAtMs : GetEpochMs();

  {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_pending.push_back(state);
  }

  War3InternalTestResult localResult = {};
  {
    std::unique_lock<std::mutex> lock(s_mutex);
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds((std::max)(uint32_t(1), timeoutMs));
    state->cv.wait_until(lock, deadline, [&]() { return state->done; });
    if (state->done) {
      localResult = state->result;
    } else {
      auto it =
          std::find(s_pending.begin(), s_pending.end(), state);
      if (it != s_pending.end())
        s_pending.erase(it);
      localResult.handled = true;
      localResult.ok = false;
      localResult.requestId = state->request.requestId;
      localResult.command = state->request.command;
      localResult.error = "internal test request timed out";
    }
  }

  if (outResult)
    *outResult = localResult;
  return localResult.ok;
}

bool ProcessPendingInternalTestRequest(uint64_t frameIndex,
                                       War3InternalTestResult* outResult) {
  std::shared_ptr<PendingInternalTestRequest> state;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_pending.empty()) {
      state = s_pending.front();
      s_pending.pop_front();
    }
  }

  War3InternalTestResult result = {};
  if (!state) {
    if (outResult)
      *outResult = result;
    return false;
  }

  result.handled = true;
  result.requestId = state->request.requestId;
  result.command = state->request.command;

  json payload = json::object();
  try {
    if (!state->request.payloadJson.empty())
      payload = json::parse(state->request.payloadJson);
  } catch (const std::exception& e) {
    result.ok = false;
    result.error = std::string("payload parse failed: ") + e.what();
    SetResult(state, result);
    if (outResult)
      *outResult = result;
    return true;
  }

  json response = json::object();
  if (state->request.command == "runtime.log_marker") {
    const std::string marker = payload.value("marker", std::string());
    war3dbg::Print("DXVK War3Test: marker=%s frame=%llu\n", marker.c_str(),
                   static_cast<unsigned long long>(frameIndex));
    response["marker"] = marker;
    response["frameIndex"] = frameIndex;
    result.ok = true;
  } else if (state->request.command == "shadow.debug_mode") {
    const uint32_t mode = payload.value("mode", 0u);
    auto* settings = dxvk::war3::GetMutableSettings();
    if (settings != nullptr) {
      settings->shadows.debugMode = static_cast<dxvk::War3ShadowDebugMode>(
          (std::min)(mode, 9u));
    }
    const bool applied = settings != nullptr;
    response["mode"] = mode;
    response["applied"] = applied;
    result.ok = applied;
    if (!applied)
      result.error = "SetShadowDebugMode failed";
  } else if (state->request.command == "shadow.lock_sun") {
    const bool enabled = payload.value("enabled", true);
    const float time01 =
        std::clamp(payload.value("time01", 0.5f), 0.0f, 1.0f);
    auto* settings = dxvk::war3::GetMutableSettings();
    if (settings != nullptr) {
      settings->shadows.lockSun = enabled;
      settings->shadows.lockSunTime = time01;
    }
    const bool applied = settings != nullptr;
    response["enabled"] = enabled;
    response["time01"] = time01;
    response["applied"] = applied;
    result.ok = applied;
    if (!applied)
      result.error = "SetShadowLockSun failed";
  } else if (state->request.command == "game.pause") {
    const bool paused = payload.value("paused", true);
    const auto pauseResult =
        dxvk::war3::hooks::SetJassGamePausedForTest(paused);
    response["paused"] = pauseResult.paused;
    response["resolved"] = pauseResult.resolved;
    response["signatureValidated"] = pauseResult.signatureValidated;
    response["invoked"] = pauseResult.invoked;
    response["paramCount"] = pauseResult.paramCount;
    response["returnType"] = pauseResult.returnType;
    response["signature"] = pauseResult.signature;
    result.ok = pauseResult.invoked;
    if (!result.ok)
      result.error = pauseResult.error;
  } else if (state->request.command == "camera.angle_of_attack") {
    const float angleDegrees =
        std::clamp(payload.value("angleDegrees", 304.0f), 250.0f, 359.0f);
    const auto cameraResult =
        dxvk::war3::hooks::SetJassCameraAngleOfAttackForTest(angleDegrees);
    response["angleDegrees"] = cameraResult.angleDegrees;
    response["convertResolved"] = cameraResult.convertResolved;
    response["setResolved"] = cameraResult.setResolved;
    response["signaturesValidated"] = cameraResult.signaturesValidated;
    response["invoked"] = cameraResult.invoked;
    response["cameraFieldHandle"] = cameraResult.cameraFieldHandle;
    response["convertParamCount"] = cameraResult.convertParamCount;
    response["convertReturnType"] = cameraResult.convertReturnType;
    response["setParamCount"] = cameraResult.setParamCount;
    response["setReturnType"] = cameraResult.setReturnType;
    response["convertSignature"] = cameraResult.convertSignature;
    response["setSignature"] = cameraResult.setSignature;
    result.ok = cameraResult.invoked;
    if (!result.ok)
      result.error = cameraResult.error;
  } else if (state->request.command == "camera.fixed") {
    const float targetX =
        std::clamp(payload.value("targetX", 200.0f), -100000.0f, 100000.0f);
    const float targetY =
        std::clamp(payload.value("targetY", -275.6f), -100000.0f, 100000.0f);
    const float targetDistance =
        std::clamp(payload.value("targetDistance", 1650.0f), 64.0f, 20000.0f);
    const float angleDegrees =
        std::clamp(payload.value("angleDegrees", 304.0f), 250.0f, 359.0f);
    const float rotationDegrees =
        std::clamp(payload.value("rotationDegrees", 90.0f), -3600.0f, 3600.0f);
    const float fieldOfViewDegrees =
        std::clamp(payload.value("fieldOfViewDegrees", 70.0f), 1.0f, 170.0f);
    const float farZ =
        std::clamp(payload.value("farZ", 5000.0f), 256.0f, 100000.0f);
    const float rollDegrees =
        std::clamp(payload.value("rollDegrees", 0.0f), -360.0f, 360.0f);
    const float zOffset =
        std::clamp(payload.value("zOffset", 0.0f), -10000.0f, 10000.0f);
    const auto cameraResult =
        dxvk::war3::hooks::SetJassFixedCameraForTest(
            targetX, targetY, targetDistance, angleDegrees, rotationDegrees,
            fieldOfViewDegrees, farZ, rollDegrees, zOffset);
    response["targetX"] = cameraResult.targetX;
    response["targetY"] = cameraResult.targetY;
    response["targetDistance"] = cameraResult.targetDistance;
    response["angleDegrees"] = cameraResult.angleDegrees;
    response["rotationDegrees"] = cameraResult.rotationDegrees;
    response["fieldOfViewDegrees"] = cameraResult.fieldOfViewDegrees;
    response["farZ"] = cameraResult.farZ;
    response["rollDegrees"] = cameraResult.rollDegrees;
    response["zOffset"] = cameraResult.zOffset;
    response["convertResolved"] = cameraResult.convertResolved;
    response["setResolved"] = cameraResult.setResolved;
    response["positionResolved"] = cameraResult.positionResolved;
    response["signaturesValidated"] = cameraResult.signaturesValidated;
    response["positionInvoked"] = cameraResult.positionInvoked;
    response["invoked"] = cameraResult.invoked;
    response["fieldInvocations"] = cameraResult.fieldInvocations;
    response["cameraFieldHandles"] = nlohmann::json::array();
    for (const uint32_t handle : cameraResult.cameraFieldHandles)
      response["cameraFieldHandles"].push_back(handle);
    response["convertParamCount"] = cameraResult.convertParamCount;
    response["convertReturnType"] = cameraResult.convertReturnType;
    response["setParamCount"] = cameraResult.setParamCount;
    response["setReturnType"] = cameraResult.setReturnType;
    response["positionParamCount"] = cameraResult.positionParamCount;
    response["positionReturnType"] = cameraResult.positionReturnType;
    response["convertSignature"] = cameraResult.convertSignature;
    response["setSignature"] = cameraResult.setSignature;
    response["positionSignature"] = cameraResult.positionSignature;
    result.ok = cameraResult.invoked;
    if (!result.ok)
      result.error = cameraResult.error;
  } else if (state->request.command == "gpu_skin.outline_test_mode") {
    const bool enabled = payload.value("enabled", true);
    auto* settings = dxvk::war3::GetMutableSettings();
    dxvk::war3::ShaderStageOverrideTestStatus shaderStatus = {};
    bool restoreApplied = false;
    bool shaderRestored = false;
    bool testLeaseActiveAfterRestore = false;
    bool restoreScopeClean = false;
    bool ownershipRetainedForRetry = false;
    uint32_t restoreStageMaskBefore = 0u;
    uint32_t restoreStageMaskAfter = 0u;
    bool applied = false;

    if (enabled) {
      shaderStatus =
          dxvk::war3::ShaderManager::get().activateStageOverrideForTest(
              war3shader::RenderStageId::Outline, "Default Override");
      if (shaderStatus.stageActivationApplied) {
        if (!s_outlineTestMode.active) {
          s_outlineTestMode.allObjectsBefore =
              dxvk::War3RenderState::IsOutlineDebugAllObjectsEnabled();
          s_outlineTestMode.forceBefore =
              dxvk::War3RenderState::IsOutlineForceEnabledForTest();
          if (settings != nullptr) {
            s_outlineTestMode.settingsBefore = settings->occludedOutline;
            s_outlineTestMode.settingsCaptured = true;
          }
          s_outlineTestMode.activeStageMaskBefore =
              shaderStatus.activeStageMaskBefore;
        }
        s_outlineTestMode.leaseId = shaderStatus.leaseId;
        s_outlineTestMode.generation = shaderStatus.generation;
        s_outlineTestMode.active = true;

        if (settings != nullptr) {
          dxvk::War3RenderState::SetOutlineDebugAllObjectsEnabled(true);
          dxvk::War3RenderState::SetOutlineForceEnabled(true);
          settings->occludedOutline.enabled = true;
          settings->occludedOutline.showVisible = true;
          settings->occludedOutline.showOccluded = true;
          settings->occludedOutline.widthPx = 2.0f;

          applied =
              dxvk::War3RenderState::IsOutlineDebugAllObjectsEnabled() &&
              dxvk::War3RenderState::IsOutlineForceEnabledForTest() &&
              dxvk::War3RenderState::HasOutlineHandles() &&
              settings->occludedOutline.enabled;
        }
      }

      if (!applied && shaderStatus.leaseId != 0u) {
        const auto rollback = RestoreOutlineTestModeState();
        ownershipRetainedForRetry = rollback.ownershipRetainedForRetry;
      }
    } else {
      const auto restored = RestoreOutlineTestModeState();
      restoreApplied = restored.ok;
      shaderRestored = restored.shaderRestored;
      testLeaseActiveAfterRestore =
          restored.testLeaseActiveAfterRestore;
      restoreScopeClean = restored.restoreScopeClean;
      ownershipRetainedForRetry = restored.ownershipRetainedForRetry;
      restoreStageMaskBefore = restored.stageMaskBefore;
      restoreStageMaskAfter = restored.stageMaskAfter;
      applied = restoreApplied;
    }

    const bool allObjectsEnabled =
        dxvk::War3RenderState::IsOutlineDebugAllObjectsEnabled();
    const bool hasOutlineTargets =
        dxvk::War3RenderState::HasOutlineHandles();
    const bool settingsEnabled =
        settings != nullptr && settings->occludedOutline.enabled;
    response["enabled"] = enabled;
    response["applied"] = applied;
    response["restoreApplied"] = restoreApplied;
    response["shaderRestored"] = shaderRestored;
    response["testLeaseActiveAfterRestore"] =
        testLeaseActiveAfterRestore;
    response["restoreScopeClean"] = restoreScopeClean;
    response["ownershipRetainedForRetry"] =
        ownershipRetainedForRetry;
    response["testModeStateActive"] = s_outlineTestMode.active;
    response["restoreStageMaskBefore"] = restoreStageMaskBefore;
    response["restoreStageMaskAfter"] = restoreStageMaskAfter;
    response["allObjectsEnabled"] = allObjectsEnabled;
    response["hasOutlineTargets"] = hasOutlineTargets;
    response["settingsAvailable"] = settings != nullptr;
    response["settingsOutlineEnabled"] = settingsEnabled;
    response["activatedStage"] = "Outline";
    response["exactPackMatched"] = shaderStatus.exactPackMatched;
    response["stageActivationApplied"] =
        shaderStatus.stageActivationApplied;
    response["materialExists"] = shaderStatus.materialExists;
    response["overrideActive"] = enabled && applied &&
        shaderStatus.overrideActive;
    response["leaseActive"] = enabled && applied && shaderStatus.leaseActive;
    response["leaseConflict"] = shaderStatus.leaseConflict;
    response["materialCompiled"] = shaderStatus.materialCompiled;
    response["materialCompileFailed"] =
        shaderStatus.materialCompileFailed;
    response["otherStageOverridesChanged"] =
        shaderStatus.otherStageOverridesChanged;
    response["packEnabledMutated"] = shaderStatus.packEnabledMutated;
    response["worldOverrideBefore"] = shaderStatus.worldOverrideBefore;
    response["worldOverrideAfter"] = shaderStatus.worldOverrideAfter;
    response["activeStageMaskBefore"] =
        shaderStatus.activeStageMaskBefore;
    response["activeStageMaskAfter"] =
        shaderStatus.activeStageMaskAfter;
    response["leaseId"] = shaderStatus.leaseId;
    response["generation"] = shaderStatus.generation;
    response["sourcePack"] = shaderStatus.sourcePack;
    response["materialName"] = shaderStatus.materialName;
    response["materialError"] = shaderStatus.materialError;
    response["frameIndex"] = frameIndex;
    result.ok = applied;
    war3dbg::Print(
        "DXVK War3OutlineTest: enabled=%u applied=%u all=%u targets=%u "
        "settings=%u settingsEnabled=%u material=%u override=%u "
        "compiled=%u compileFailed=%u lease=%llu/%llu/%u "
        "stageOnly=%u packMutated=%u world=%u/%u mask=%u/%u "
        "restore=%u/%u frame=%llu\n",
        enabled ? 1u : 0u, applied ? 1u : 0u,
        allObjectsEnabled ? 1u : 0u, hasOutlineTargets ? 1u : 0u,
        settings != nullptr ? 1u : 0u, settingsEnabled ? 1u : 0u,
        shaderStatus.materialExists ? 1u : 0u,
        shaderStatus.overrideActive ? 1u : 0u,
        shaderStatus.materialCompiled ? 1u : 0u,
        shaderStatus.materialCompileFailed ? 1u : 0u,
        static_cast<unsigned long long>(shaderStatus.leaseId),
        static_cast<unsigned long long>(shaderStatus.generation),
        shaderStatus.leaseActive ? 1u : 0u,
        shaderStatus.otherStageOverridesChanged ? 0u : 1u,
        shaderStatus.packEnabledMutated ? 1u : 0u,
        shaderStatus.worldOverrideBefore ? 1u : 0u,
        shaderStatus.worldOverrideAfter ? 1u : 0u,
        shaderStatus.activeStageMaskBefore,
        shaderStatus.activeStageMaskAfter,
        restoreApplied ? 1u : 0u, shaderRestored ? 1u : 0u,
        static_cast<unsigned long long>(frameIndex));
    if (!applied)
      result.error = "GPU skin outline test mode actuation failed";
  } else if (state->request.command == "jass.bridge_selftest") {
    const bool displayText = payload.value("displayText", false);
    const auto test =
        dxvk::war3::hooks::RunJassCommandBridgeSelfTest(displayText);
    response["installed"] = test.installed;
    response["preloaderOk"] = test.preloaderOk;
    response["typedTransportInstalled"] = test.typedTransportInstalled;
    response["intQueryOk"] = test.intQueryOk;
    response["stringQueryOk"] = test.stringQueryOk;
    response["publicV1Ok"] = test.publicV1Ok;
    response["displayTextAttempted"] = test.displayTextAttempted;
    response["displayTextOk"] = test.displayTextOk;
    response["pingCode"] = test.pingCode;
    response["publicProtocolVersion"] = test.publicProtocolVersion;
    response["versionStringHandle"] = test.versionStringHandle;
    response["publicVersionStringHandle"] = test.publicVersionStringHandle;
    response["versionText"] = test.versionText;
    response["publicVersionText"] = test.publicVersionText;
    response["playerHandle"] = test.playerHandle;
    result.ok = test.installed && test.preloaderOk && test.intQueryOk &&
                test.stringQueryOk && test.publicV1Ok &&
                (!test.displayTextAttempted || test.displayTextOk);
    if (!result.ok)
      result.error = test.error.empty() ? "jass bridge selftest failed"
                                        : test.error;
  } else if (state->request.command == "gpu_skin.log_diagnostics") {
    const std::string snapshotId = state->request.requestId;
    const bool requireQuiescent =
        payload.value("requireQuiescent", false);
    auto* device = dxvk::war3::GetActiveDevice();
    char snapshotMarker[256] = {};
    std::snprintf(
        snapshotMarker, sizeof(snapshotMarker),
        "DXVK War3GpuSkin: diagSnapshot begin=%s", snapshotId.c_str());
    war3dbg::Print("%s\n", snapshotMarker);
    ::dxvk::Logger::info(snapshotMarker);
    bool quiescent = false;
    const bool logged = device != nullptr &&
        device->War3LogGpuSkinDiagnosticsForTest(
            requireQuiescent, &quiescent);
    response["logged"] = logged;
    response["requireQuiescent"] = requireQuiescent;
    response["quiescent"] = quiescent;
    response["frameIndex"] = frameIndex;
    response["snapshotId"] = snapshotId;
    result.ok = logged;
    // The isolated conductor parses only the exact-PID block between this
    // request-unique pair. complete=0 remains fail-closed evidence.
    std::snprintf(
        snapshotMarker, sizeof(snapshotMarker),
        "DXVK War3GpuSkin: diagSnapshot end=%s complete=%u",
        snapshotId.c_str(), logged ? 1u : 0u);
    war3dbg::Print("%s\n", snapshotMarker);
    ::dxvk::Logger::info(snapshotMarker);
    if (!logged && device == nullptr) {
      result.error = "active D3D9 device unavailable";
    } else if (!logged && requireQuiescent) {
      result.error = "GPU skin diagnostics not quiescent";
    } else if (!logged) {
      result.error = "GPU skin diagnostics unavailable";
    }
  } else if (state->request.command == "gpu_skin.reset_bridge") {
    const std::string scope = payload.value("scope", std::string());
    auto* device = dxvk::war3::GetActiveDevice();
    const auto before =
        dxvk::war3::gpu_skin::GetNativeBridgeQuiescenceSnapshot();
    const bool validScope = scope == "map" || scope == "device";
    const bool accepted = validScope && device != nullptr &&
        device->War3ResetGpuSkinBridgeForTest(scope == "device");
    const auto after =
        dxvk::war3::gpu_skin::GetNativeBridgeQuiescenceSnapshot();
    response["scope"] = scope;
    response["accepted"] = accepted;
    response["requestedGenerationBefore"] =
        before.requestedResetGeneration;
    response["requestedGenerationAfter"] =
        after.requestedResetGeneration;
    response["completedGenerationAfter"] =
        after.completedResetGeneration;
    response["acknowledgedGenerationAfter"] =
        after.ownerRetiredGeneration;
    response["resetPendingAfter"] = after.resetPending;
    response["poisonOutstandingAfter"] =
        after.poisonRangesOutstanding;
    response["retirementPendingAfter"] =
        after.retirementEventsPending;
    result.ok = accepted &&
        after.requestedResetGeneration > before.requestedResetGeneration;
    if (!result.ok) {
      if (!validScope)
        result.error = "gpu skin reset scope must be map or device";
      else if (device == nullptr)
        result.error = "active D3D9 device unavailable";
      else if (!accepted)
        result.error = "GPU skin bridge reset was not accepted";
      else
        result.error = "GPU skin bridge reset generation did not advance";
    }
  } else if (state->request.command == "shutdown.session") {
    response["accepted"] = true;
    response["softOnly"] = true;
    response["frameIndex"] = frameIndex;
    result.ok = true;
  } else {
    result.ok = false;
    result.error = "unsupported internal test command";
    response["unsupported"] = state->request.command;
  }

  result.resultJson = response.dump();
  SetResult(state, result);
  if (outResult)
    *outResult = result;
  return true;
}

} // namespace dxvk::war3::tools
