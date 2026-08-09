#include "war3_internal_test_api.h"

#include "../../d3d9_device.h"
#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_settings.h"

#include "../gpu_skin/war3_gpu_skin_native_bridge.h"
#include "../core/war3_internal_test_config.h"
#include "../hooks/war3_jass_command_bridge.h"
#include "../render/war3_render_state.h"
#include "../shader/war3_shader_manager.h"
#include "../war3.h"
#include "war3_diagnostics_hub.h"

#include "../../../util/log/log.h"
#include "../../../util/util_env.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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

  auto settings = dxvk::war3::GetMutableSettings();
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
  static const bool enabled = []() {
    if constexpr (dxvk::war3::internal::kNativeInternalTestApiEnabled)
      return true;
    const std::string value =
        dxvk::env::getEnvVar("DXVK_WAR3_INTERNAL_TEST_API");
    return value == "1" || value == "true" || value == "TRUE" ||
        value == "on" || value == "ON" || value == "yes" ||
        value == "YES";
  }();
  return enabled;
}

void ResetInternalTestApiState() {
  ClearGpuFlightAutoTestContext();
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
  if (!IsInternalTestApiEnabled()) {
    War3InternalTestResult disabled = {};
    disabled.handled = true;
    disabled.ok = false;
    disabled.requestId = request.requestId;
    disabled.command = request.command;
    disabled.error = "internal test api is disabled";
    if (outResult)
      *outResult = disabled;
    return false;
  }
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
  } else if (state->request.command == "autotest.waypoint") {
    const bool active = payload.value("active", true);
    if (!active) {
      ClearGpuFlightAutoTestContext();
      response["active"] = false;
      response["frameIndex"] = frameIndex;
      result.ok = true;
    } else {
      const uint32_t index = payload.value("index", 0u);
      const auto bounded = [&payload](const char* name, float fallback) {
        const float value = payload.value(name, fallback);
        return std::isfinite(value)
            ? std::clamp(value, -100000.0f, 100000.0f)
            : fallback;
      };
      const float targetX = bounded("targetX", 0.0f);
      const float targetY = bounded("targetY", 0.0f);
      const float panSeconds = std::clamp(
          bounded("panSeconds", 0.0f), 0.0f, 30.0f);
      SetGpuFlightAutoTestContext(
          index, targetX, targetY, panSeconds,
          bounded("cameraTargetX", targetX),
          bounded("cameraTargetY", targetY),
          std::clamp(bounded("cameraTargetDistance", 0.0f), 0.0f, 20000.0f),
          std::clamp(bounded("cameraAngleOfAttack", 0.0f), -3600.0f, 3600.0f),
          bounded("worldMinX", 0.0f), bounded("worldMinY", 0.0f),
          bounded("worldMaxX", 0.0f), bounded("worldMaxY", 0.0f));
      war3dbg::Print(
          "DXVK War3Test: waypoint=%u target=(%.3f,%.3f) seconds=%.3f "
          "frame=%llu\n",
          index, static_cast<double>(targetX), static_cast<double>(targetY),
          static_cast<double>(panSeconds),
          static_cast<unsigned long long>(frameIndex));
      response["active"] = true;
      response["index"] = index;
      response["targetX"] = targetX;
      response["targetY"] = targetY;
      response["panSeconds"] = panSeconds;
      response["frameIndex"] = frameIndex;
      result.ok = true;
    }
  } else if (state->request.command == "shadow.debug_mode") {
    const uint32_t mode = payload.value("mode", 0u);
    auto settings = dxvk::war3::GetMutableSettings();
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
    auto settings = dxvk::war3::GetMutableSettings();
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
  } else if (state->request.command == "camera.snapshot") {
    const auto cameraResult =
        dxvk::war3::hooks::SnapshotJassCameraForTest();
    response["targetX"] = cameraResult.targetX;
    response["targetY"] = cameraResult.targetY;
    response["targetZ"] = cameraResult.targetZ;
    response["targetDistance"] = cameraResult.targetDistance;
    response["farZ"] = cameraResult.farZ;
    response["angleOfAttack"] = cameraResult.angleOfAttack;
    response["fieldOfView"] = cameraResult.fieldOfView;
    response["roll"] = cameraResult.roll;
    response["rotation"] = cameraResult.rotation;
    response["zOffset"] = cameraResult.zOffset;
    response["resolved"] = cameraResult.resolved;
    response["signaturesValidated"] = cameraResult.signaturesValidated;
    response["invoked"] = cameraResult.invoked;
    response["fieldInvocations"] = cameraResult.fieldInvocations;
    response["executionThread"] = "war3-main-loop";
    result.ok = cameraResult.invoked;
    if (!result.ok)
      result.error = cameraResult.error;
  } else if (state->request.command == "camera.apply") {
    const auto snapshot =
        dxvk::war3::hooks::SnapshotJassCameraForTest();
    if (!snapshot.invoked) {
      response["snapshotInvoked"] = false;
      result.ok = false;
      result.error = snapshot.error;
    } else {
      const float targetX = std::clamp(
          payload.value("targetX", snapshot.targetX),
          -100000.0f, 100000.0f);
      const float targetY = std::clamp(
          payload.value("targetY", snapshot.targetY),
          -100000.0f, 100000.0f);
      const float targetDistance = std::clamp(
          payload.value("targetDistance", snapshot.targetDistance),
          64.0f, 20000.0f);
      const float angleOfAttack = std::clamp(
          payload.value("angleOfAttack", snapshot.angleOfAttack),
          -3600.0f, 3600.0f);
      const float rotation = std::clamp(
          payload.value("rotation", snapshot.rotation),
          -3600.0f, 3600.0f);
      const float fieldOfView = std::clamp(
          payload.value("fieldOfView", snapshot.fieldOfView),
          1.0f, 170.0f);
      const float farZ = std::clamp(
          payload.value("farZ", snapshot.farZ),
          256.0f, 100000.0f);
      const float roll = std::clamp(
          payload.value("roll", snapshot.roll),
          -360.0f, 360.0f);
      const float zOffset = std::clamp(
          payload.value("zOffset", snapshot.zOffset),
          -10000.0f, 10000.0f);
      const float duration = std::clamp(
          payload.value("duration", 0.0f), 0.0f, 30.0f);
      const bool quickPosition = payload.value("quickPosition", true);
      const auto cameraResult =
          dxvk::war3::hooks::ApplyJassCameraForTest(
              targetX, targetY, targetDistance, angleOfAttack, rotation,
              fieldOfView, farZ, roll, zOffset, duration, quickPosition);
      response["snapshotInvoked"] = true;
      response["targetX"] = targetX;
      response["targetY"] = targetY;
      response["targetDistance"] = targetDistance;
      response["angleOfAttack"] = angleOfAttack;
      response["rotation"] = rotation;
      response["fieldOfView"] = fieldOfView;
      response["farZ"] = farZ;
      response["roll"] = roll;
      response["zOffset"] = zOffset;
      response["duration"] = duration;
      response["quickPosition"] = quickPosition;
      response["resolved"] = cameraResult.resolved;
      response["signaturesValidated"] =
          cameraResult.signaturesValidated;
      response["positionInvoked"] = cameraResult.positionInvoked;
      response["fieldInvocations"] = cameraResult.fieldInvocations;
      response["invoked"] = cameraResult.invoked;
      response["executionThread"] = "war3-main-loop";
      result.ok = cameraResult.invoked;
      if (!result.ok)
        result.error = cameraResult.error;
    }
  } else if (state->request.command == "camera.pan_to") {
    const float targetX = std::clamp(
        payload.value("targetX", 0.0f), -100000.0f, 100000.0f);
    const float targetY = std::clamp(
        payload.value("targetY", 0.0f), -100000.0f, 100000.0f);
    const float duration = std::clamp(
        payload.value("duration", 1.0f), 0.0f, 30.0f);
    const auto cameraResult =
        dxvk::war3::hooks::PanJassCameraForTest(targetX, targetY, duration);
    response["targetX"] = cameraResult.targetX;
    response["targetY"] = cameraResult.targetY;
    response["duration"] = cameraResult.duration;
    response["resolved"] = cameraResult.resolved;
    response["signatureValidated"] = cameraResult.signatureValidated;
    response["signature"] = cameraResult.signature;
    response["invoked"] = cameraResult.invoked;
    response["executionThread"] = "war3-main-loop";
    result.ok = cameraResult.invoked;
    if (!result.ok)
      result.error = cameraResult.error;
  } else if (state->request.command == "camera.world_bounds") {
    const auto boundsResult =
        dxvk::war3::hooks::QueryJassWorldBoundsForTest();
    response["rectHandle"] = boundsResult.rectHandle;
    response["minX"] = boundsResult.minX;
    response["minY"] = boundsResult.minY;
    response["maxX"] = boundsResult.maxX;
    response["maxY"] = boundsResult.maxY;
    response["resolved"] = boundsResult.resolved;
    response["signaturesValidated"] =
        boundsResult.signaturesValidated;
    response["invoked"] = boundsResult.invoked;
    response["executionThread"] = "war3-main-loop";
    result.ok = boundsResult.invoked;
    if (!result.ok)
      result.error = boundsResult.error;
  } else if (state->request.command == "visibility.full_map") {
    const bool enabled = payload.value("enabled", true);
    const auto visibilityResult =
        dxvk::war3::hooks::SetJassFullMapVisibilityForTest(enabled);
    response["enabled"] = visibilityResult.enabled;
    response["leaseActive"] = visibilityResult.leaseActive;
    response["capturedOriginal"] = visibilityResult.capturedOriginal;
    response["fogBefore"] = visibilityResult.fogBefore;
    response["fogMaskBefore"] = visibilityResult.fogMaskBefore;
    response["fogAfter"] = visibilityResult.fogAfter;
    response["fogMaskAfter"] = visibilityResult.fogMaskAfter;
    response["resolved"] = visibilityResult.resolved;
    response["signaturesValidated"] =
        visibilityResult.signaturesValidated;
    response["invoked"] = visibilityResult.invoked;
    response["executionThread"] = "war3-main-loop";
    result.ok = visibilityResult.invoked;
    if (!result.ok)
      result.error = visibilityResult.error;
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
    auto settings = dxvk::war3::GetMutableSettings();
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
    char snapshotMarker[256] = {};
    std::snprintf(
        snapshotMarker, sizeof(snapshotMarker),
        "DXVK War3GpuSkin: diagSnapshot begin=%s", snapshotId.c_str());
    war3dbg::Print("%s\n", snapshotMarker);
    ::dxvk::Logger::info(snapshotMarker);
    bool quiescent = false;
    bool logged = false;
    const bool activeDeviceAvailable = dxvk::war3::RunWithActiveDevice(
        [&](D3D9DeviceEx& device) {
          logged = device.War3LogGpuSkinDiagnosticsForTest(
              requireQuiescent, &quiescent);
        });
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
    if (!logged && !activeDeviceAvailable) {
      result.error = "active D3D9 device unavailable";
    } else if (!logged && requireQuiescent) {
      result.error = "GPU skin diagnostics not quiescent";
    } else if (!logged) {
      result.error = "GPU skin diagnostics unavailable";
    }
  } else if (state->request.command == "gpu_skin.reset_bridge") {
    const std::string scope = payload.value("scope", std::string());
    const auto before =
        dxvk::war3::gpu_skin::GetNativeBridgeQuiescenceSnapshot();
    const bool validScope = scope == "map" || scope == "device";
    bool accepted = false;
    uint64_t shadowMapResetRequestedBefore = 0u;
    uint64_t shadowMapResetRequestedAfter = 0u;
    const bool activeDeviceAvailable = validScope &&
        dxvk::war3::RunWithActiveDevice([&](D3D9DeviceEx& device) {
          shadowMapResetRequestedBefore =
              device.QueryWar3ShadowLifecycleDiagnostics()
                  .requestedResetSerial;
          accepted = device.War3ResetGpuSkinBridgeForTest(scope == "device");
          shadowMapResetRequestedAfter =
              device.QueryWar3ShadowLifecycleDiagnostics()
                  .requestedResetSerial;
        });
    const auto after =
        dxvk::war3::gpu_skin::GetNativeBridgeQuiescenceSnapshot();
    response["scope"] = scope;
    response["accepted"] = accepted;
    response["requestedGenerationBefore"] =
        before.requestedResetGeneration;
    response["requestedGenerationAfter"] =
        after.requestedResetGeneration;
    response["shadowMapResetRequestedBefore"] =
        shadowMapResetRequestedBefore;
    response["shadowMapResetRequestedAfter"] =
        shadowMapResetRequestedAfter;
    response["completedGenerationAfter"] =
        after.completedResetGeneration;
    response["acknowledgedGenerationAfter"] =
        after.ownerRetiredGeneration;
    response["resetPendingAfter"] = after.resetPending;
    response["poisonOutstandingAfter"] =
        after.poisonRangesOutstanding;
    response["retirementPendingAfter"] =
        after.retirementEventsPending;
    const bool transitionPublished = scope == "map"
        ? shadowMapResetRequestedAfter > shadowMapResetRequestedBefore
        : after.requestedResetGeneration > before.requestedResetGeneration;
    response["transitionPublished"] = transitionPublished;
    result.ok = accepted && transitionPublished;
    if (!result.ok) {
      if (!validScope)
        result.error = "gpu skin reset scope must be map or device";
      else if (!activeDeviceAvailable)
        result.error = "active D3D9 device unavailable";
      else if (!accepted)
        result.error = "GPU skin bridge reset was not accepted";
      else
        result.error = scope == "map"
            ? "shadow map reset request serial did not advance"
            : "GPU skin bridge reset generation did not advance";
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
