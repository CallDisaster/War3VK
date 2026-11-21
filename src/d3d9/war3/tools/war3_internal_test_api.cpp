#include "war3_internal_test_api.h"
#include "war3_async_screenshot.h"
#include "war3_frame_history.h"
#include "../hooks/war3_native_capture.h"

#include "../../d3d9_device.h"
#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_settings.h"
#include "../../d3d9_war3_volumetric_light.h"
#include "../../d3d9_war3_weather.h"

#include "../gpu_skin/war3_gpu_skin_native_bridge.h"
#include "../core/war3_internal_test_config.h"
#include "../game/war3_unit_animation_api.h"
#include "../hooks/war3_jass_command_bridge.h"
#include "../platform/war3_proxy_mode.h"
#include "../platform/war3_proxy_overlay.h"
#include "../ui/war3_imgui.h"
#include "../render/war3_render_objects.h"
#include "../render/war3_render_state.h"
#include "../platform/war3_proxy_status.h"
#include "../shader/war3_shader_manager.h"
#include "../state/war3_fog_volume.h"
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
#include <vector>

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

// outline.pick / outline.clear / outline.status: development probes that
// register one object for the outline exactly as a map's JASS does through
// AddOutlineHandle, so both modes can be compared at the same camera. The
// outline settings they touch are restored by outline.clear.
struct OutlinePickState {
  bool settingsCaptured = false;
  dxvk::War3OccludedOutlineSettings settingsBefore = {};
  // The scene collector registers only tracked handles (outline/bloom) unless
  // every object is tracked: the first pick turns the all-objects tracking on
  // so the registry has candidates on the next frames; the pick that then
  // succeeds (or outline.clear) puts it back.
  bool trackingArmed = false;
  bool allObjectsBefore = false;
};

OutlinePickState s_outlinePick;

void RestoreOutlineTracking() {
  if (!s_outlinePick.trackingArmed)
    return;
  dxvk::War3RenderState::SetOutlineDebugAllObjectsEnabled(s_outlinePick.allObjectsBefore);
  s_outlinePick.trackingArmed = false;
}

std::string FourCcText(uint32_t code) {
  std::string text;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const char c = char((code >> shift) & 0xFFu);
    if (c < 0x20 || c > 0x7E)
      return std::to_string(code);
    text.push_back(c);
  }
  return text;
}

json OutlineSettingsJson(const dxvk::War3OccludedOutlineSettings& s) {
  return json{{"enabled", s.enabled}, {"mode", uint32_t(s.mode)},
              {"useScreenSpace", s.useScreenSpace}, {"widthPx", s.widthPx},
              {"showVisible", s.showVisible}, {"showOccluded", s.showOccluded},
              {"color", {s.colorR, s.colorG, s.colorB, s.colorA}}};
}

void OutlineCommand(const std::string& command, const json& payload,
                    json& response, War3InternalTestResult& result) {
  using dxvk::War3RenderState;
  namespace render = dxvk::war3::render;
  auto settings = dxvk::war3::GetMutableSettings();
  if (command == "outline.clear") {
    RestoreOutlineTracking();
    War3RenderState::ClearOutlineHandles();
    if (s_outlinePick.settingsCaptured && settings != nullptr)
      settings->occludedOutline = s_outlinePick.settingsBefore;
    s_outlinePick = {};
  } else if (command == "outline.pick" && payload.value("rangeCount", 0u) != 0u) {
    // A contiguous window of JASS handles, one AddOutlineHandle each as a
    // map's script registers them: the same set in both modes, without the
    // render object registry (which the product may leave empty).
    const uint32_t lo = payload.value("rangeLo", 0x100000u);
    const uint32_t count = std::min(payload.value("rangeCount", 0u), 1024u);
    if (settings != nullptr && !s_outlinePick.settingsCaptured) {
      s_outlinePick.settingsBefore = settings->occludedOutline;
      s_outlinePick.settingsCaptured = true;
    }
    for (uint32_t i = 0; i < count; ++i)
      War3RenderState::AddOutlineHandle(lo + i);
    if (settings != nullptr) {
      auto& outline = settings->occludedOutline;
      outline.enabled = true;
      outline.showVisible = payload.value("showVisible", outline.showVisible);
      outline.showOccluded = payload.value("showOccluded", outline.showOccluded);
      outline.widthPx = std::clamp(payload.value("widthPx", outline.widthPx), 1.0f, 16.0f);
    }
    response["rangeLo"] = lo;
    response["rangeCount"] = count;
    result.ok = settings != nullptr && War3RenderState::GetOutlineHandleCount() >= count;
    if (!result.ok)
      result.error = "outline handle range registration failed";
  } else if (command == "outline.pick") {
    const std::string kind = payload.value("kind", std::string("building"));
    const uint32_t index = payload.value("index", 0u);
    uint32_t handle = payload.value("handle", 0u);
    std::vector<uint32_t> rawcodes;
    if (payload.contains("rawcodes") && payload["rawcodes"].is_array()) {
      for (const auto& value : payload["rawcodes"]) {
        const std::string text = value.is_string() ? value.get<std::string>() : std::string();
        if (text.size() == 4u)
          rawcodes.push_back(uint32_t(uint8_t(text[0])) << 24 | uint32_t(uint8_t(text[1])) << 16 |
                             uint32_t(uint8_t(text[2])) << 8 | uint32_t(uint8_t(text[3])));
      }
    }
    const auto wanted = [&](const render::RenderObjectInfo& info) {
      if (info.jHandle == 0u)
        return false;
      if (!rawcodes.empty() &&
          std::find(rawcodes.begin(), rawcodes.end(), info.rawcode) == rawcodes.end())
        return false;
      return kind == "any" ||
             (kind == "unit" && info.kind == render::ObjectKind::Unit) ||
             (kind == "building" && info.kind == render::ObjectKind::Building) ||
             (kind == "destructible" && info.kind == render::ObjectKind::Destructible);
    };
    const auto objects = render::RenderObjectRegistry::instance().getAllObjects();
    std::vector<render::RenderObjectInfo> candidates;
    for (const auto& info : objects) {
      if (wanted(info) && std::none_of(candidates.begin(), candidates.end(),
                                       [&](const auto& c) { return c.jHandle == info.jHandle; }))
        candidates.push_back(info);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.jHandle < b.jHandle; });
    response["registryObjects"] = objects.size();
    response["candidateCount"] = candidates.size();
    json listed = json::array();
    for (size_t i = 0; i < candidates.size() && i < 24u; ++i)
      listed.push_back({{"handle", candidates[i].jHandle}, {"rawcode", FourCcText(candidates[i].rawcode)},
                        {"kind", render::ObjectKindToString(candidates[i].kind)}});
    response["candidates"] = listed;
    if (handle == 0u) {
      if (index >= candidates.size()) {
        if (!s_outlinePick.trackingArmed) {
          s_outlinePick.allObjectsBefore = War3RenderState::IsOutlineDebugAllObjectsEnabled();
          War3RenderState::SetOutlineDebugAllObjectsEnabled(true);
          s_outlinePick.trackingArmed = true;
        }
        response["trackingArmed"] = true;
        result.ok = false;
        result.error = "no outline candidate yet; object tracking armed, retry";
        return;
      }
      handle = candidates[index].jHandle;
      response["rawcode"] = FourCcText(candidates[index].rawcode);
      response["kind"] = render::ObjectKindToString(candidates[index].kind);
    }
    if (settings != nullptr && !s_outlinePick.settingsCaptured) {
      s_outlinePick.settingsBefore = settings->occludedOutline;
      s_outlinePick.settingsCaptured = true;
    }
    War3RenderState::AddOutlineHandle(handle);
    RestoreOutlineTracking();
    if (settings != nullptr) {
      auto& outline = settings->occludedOutline;
      outline.enabled = true;
      outline.showVisible = payload.value("showVisible", outline.showVisible);
      outline.showOccluded = payload.value("showOccluded", outline.showOccluded);
      outline.widthPx = std::clamp(payload.value("widthPx", outline.widthPx), 1.0f, 16.0f);
    }
    response["picked"] = handle;
    result.ok = settings != nullptr && War3RenderState::IsOutlineHandle(handle);
    if (!result.ok)
      result.error = "outline handle registration failed";
  }
  if (command != "outline.pick")
    result.ok = true;
  response["handleCount"] = War3RenderState::GetOutlineHandleCount();
  if (settings != nullptr)
    response["settings"] = OutlineSettingsJson(settings->occludedOutline);
  // Proxy mode: the casters' handle and outline-match account (null in process).
  response["proxyMode"] = dxvk::war3::proxy::StatusJson();
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
  if (state->request.command == "weather-status" ||
      state->request.command == "weather.status") {
    const auto draw = ProcessServices().QueryWar3WeatherDiagnostics();
    const auto weather = render::WeatherRuntime::instance().snapshot(
        frameIndex, draw.mapEpoch, draw.deviceEpoch);
    response["runtime"] = {{"id", weather.id}, {"enabled", weather.enabled},
        {"observationKind", "owner-control-live-state-not-render-input"},
        {"paused", weather.paused}, {"timeSeconds", weather.timeSeconds},
        {"revision", weather.revision}, {"historyRevision", weather.historyRevision},
        {"frameSerial", weather.frameSerial}, {"mapEpoch", weather.mapEpoch},
        {"deviceEpoch", weather.deviceEpoch},
        {"params", {{"snowfall", weather.params.snowfall},
                    {"windAngle", weather.params.windAngle},
                    {"windSpeed", weather.params.windSpeed},
                    {"gust", weather.params.gust},
                    {"haze", weather.params.haze}, {"lowSnow", weather.params.lowSnow}}}};
    response["render"] = {{"commandsRecorded", draw.submitted},
        {"id", draw.id}, {"timeSeconds", draw.timeSeconds},
        {"mapEpoch", draw.mapEpoch}, {"deviceEpoch", draw.deviceEpoch},
        {"particleCount", draw.activeParticles}, {"capacity", draw.capacity},
        {"resourceBytes", draw.resourceBytes}, {"frameSerial", draw.frameSerial},
        {"stage", War3WeatherDrawStageName(draw.stage)}};
    const auto& frozen = draw.frozenInput;
    response["consumedCameraDomain"] = {
        {"observationKind", "render-consumed-camera-domain"},
        {"frameSerial", draw.frameSerial}, {"mapEpoch", draw.mapEpoch},
        {"deviceEpoch", draw.deviceEpoch}, {"cameraFrameMatches", draw.cameraFrameMatches},
        {"viewport", {{"x", draw.worldViewport.X}, {"y", draw.worldViewport.Y},
                      {"width", draw.worldViewport.Width}, {"height", draw.worldViewport.Height},
                      {"minZ", draw.worldViewport.MinZ}, {"maxZ", draw.worldViewport.MaxZ}}},
        {"depthWidth", draw.depthWidth}, {"depthHeight", draw.depthHeight}};
    response["frozenInput"] = {{"observationKind", "render-consumed-value-snapshot"},
        {"id", frozen.id}, {"enabled", frozen.enabled}, {"paused", frozen.paused},
        {"timeSeconds", frozen.timeSeconds}, {"frameSerial", frozen.frameSerial},
        {"mapEpoch", frozen.mapEpoch}, {"deviceEpoch", frozen.deviceEpoch},
        {"revision", frozen.revision}, {"historyRevision", frozen.historyRevision},
        {"params", {{"snowfall", frozen.params.snowfall},
                    {"windAngle", frozen.params.windAngle},
                    {"windSpeed", frozen.params.windSpeed},
                    {"gust", frozen.params.gust},
                    {"haze", frozen.params.haze}, {"lowSnow", frozen.params.lowSnow}}}};
    const auto volume = ProcessServices().QueryWar3WeatherVolumeDiagnostics();
    response["volumetric"] = {{"compositeSubmitted", volume.compositeSubmitted},
        {"stage", volume.stage}, {"frameSerial", volume.frameSerial},
        {"mapEpoch", volume.mapEpoch}, {"deviceEpoch", volume.deviceEpoch},
        {"resourceBytes", volume.resourceBytes},
        {"width", volume.width}, {"height", volume.height},
        {"admittedShaderWork", volume.admittedShaderWork}};
    const auto ordinary =
        ProcessServices().QueryWar3VolumetricExecutionDiagnostics();
    response["ordinaryVolumetric"] = {
        {"frameSerial", ordinary.frameSerial}, {"mapEpoch", ordinary.mapEpoch},
        {"deviceEpoch", ordinary.deviceEpoch}, {"compositeSubmitted", ordinary.compositeSubmitted},
        {"stage", ordinary.stage}, {"width", ordinary.width}, {"height", ordinary.height},
        {"requestedBackend", ordinary.requestedBackend}, {"effectiveBackend", ordinary.effectiveBackend},
        // The old owner does not publish current byte accounting. Unknown
        // must not become a plausible-looking measured zero.
        {"resourceBytes", nullptr}, {"currentResourceAccountingCovered", false}};
    War3RenderSettings author;
    if (dxvk::war3::GetSettingsSnapshot(author)) {
      const auto& ordinarySettings = author.postFx.volumetricLight;
      response["authorSettings"] = {
          {"observationKind", "owner-settings-live-state-not-render-input"},
          {"sunEnabled", author.sun.enabled},
          {"ordinaryVolumetricEnabled", ordinarySettings.enabled},
          {"globalMediumEnabled", ordinarySettings.globalMediumEnabled},
          {"volumetricDensity", ordinarySettings.density},
          {"volumetricBackend", static_cast<uint32_t>(ordinarySettings.quality)}};
    }
    result.ok = true;
  } else if (state->request.command == "runtime.log_marker") {
    const std::string marker = payload.value("marker", std::string());
    war3dbg::Print("DXVK War3Test: marker=%s frame=%llu\n", marker.c_str(),
                   static_cast<unsigned long long>(frameIndex));
    response["marker"] = marker;
    response["frameIndex"] = frameIndex;
    result.ok = true;
  } else if (state->request.command == "native_frame_sync.status") {
    const auto s = ::dxvk::war3::hooks::QueryNativeFrameSyncStatus();
    response["persistent"] = s.persistent;
    response["installed"] = s.installed;
    response["ownerAtQuery"] = s.owner;
    response["consoleAttached"] = ::GetConsoleWindow() != nullptr;
    response["calls"] = s.calls;
    response["elided"] = s.elided;
    response["retained"] = s.retained;
    response["presents"] = s.presents;
    response["queryTicks"] = std::to_string(s.queryTicks);
    response["frequency"] = s.frequency;
    result.ok = true;
  } else if (state->request.command == "frame_history.test_shortcut") {
    result.ok=ProcessServices().TestFrameHistoryWindowShortcut();
    response["route"]="installed-window-proc-private-test-message";
    const auto recorder=QueryFrameHistoryHud();
    response["notice"]=recorder.notice;
    response["recorderState"]=recorder.state;
    response["selfContained"]=recorder.selfContained;
    response["bufferedMs"]=recorder.bufferedMs;
    response["requiredMs"]=recorder.requiredMs;
    response["recorderError"]=recorder.error;
    response["globalInputUsed"]=false;
    if(!result.ok)result.error="installed window route unavailable";
  } else if (state->request.command == "ui.panel_open" ||
             state->request.command == "ui.panel_close") {
    // The settings panel (Ctrl+F1). Proxy mode: a private message through the
    // overlay's window subclass, which opens it at the next Present. In
    // process: the panel's own toggle on this (the game's window) thread.
    const bool open = state->request.command == "ui.panel_open";
    auto& panel = War3Imgui::get();
    if (proxy::Requested()) {
      result.ok = proxy::RoutePanelRequest(open);
      response["route"] = "installed-window-proc-private-test-message";
      if (!result.ok) result.error = "overlay window route unavailable";
    } else {
      result.ok = panel.isInitialized();
      if (result.ok && panel.isVisible() != open) panel.toggleVisible();
      response["route"] = "in-process-owner-thread-toggle";
      if (!result.ok) result.error = "in-process panel not initialized";
    }
    response["requested"] = open ? "open" : "close";
    response["globalInputUsed"] = false;
  } else if (state->request.command == "ui.panel_status") {
    const auto panel = War3Imgui::get().status();
    response["mode"] = proxy::Requested() ? "proxy" : "in-process";
    response["initialized"] = panel.initialized;
    response["visible"] = panel.visible;
    response["proxyPanel"] = panel.proxy;
    response["framesDrawn"] = panel.framesDrawn;
    response["framesSkipped"] = panel.framesSkipped;
    response["textureFailures"] = panel.textureFailures;
    response["lastCommands"] = panel.lastCommands;
    response["lastVertices"] = panel.lastVertices;
    response["atlasWidth"] = panel.atlasWidth;
    response["atlasHeight"] = panel.atlasHeight;
    response["wantCaptureMouse"] = panel.wantCaptureMouse;
    response["wantCaptureKeyboard"] = panel.wantCaptureKeyboard;
    if (proxy::Requested()) {
      const auto overlay = proxy::QueryOverlayStatus();
      response["overlay"] = {{"available", overlay.available}, {"wantOpen", overlay.wantOpen},
          {"routedRequests", overlay.routedRequests}, {"toggles", overlay.toggles},
          {"swallowedInput", overlay.swallowedInput}, {"opens", overlay.opens},
          {"closes", overlay.closes}, {"presentsDrawn", overlay.presentsDrawn},
          {"resourcesCreated", overlay.resourcesCreated},
          {"resourceFailures", overlay.resourceFailures},
          {"lastResourceResult", overlay.lastResourceResult},
          {"privateBytes", overlay.privateBytes}, {"virtualUsed", overlay.virtualUsed}};
    }
    result.ok = true;
  } else if (state->request.command == "screenshot.flicker_burst") {
    result.ok = ::dxvk::war3::tools::ArmAsyncScreenshotBurstForTest();
    if (!result.ok) result.error = "burst requires owner/gate and all three free slots";
    response["requestedConsecutiveFrames"] = 3;
    response["saved"] = false;
    response["globalInputUsed"] = false;
    response["completion"] = "verify-three-files-and-consecutive-present-ordinals";
  } else if (state->request.command == "screenshot.native_async") {
    const auto count = payload.value("count", json(1));
    if (!count.is_number_integer() || count.is_boolean() ||
        count < 1 || count > 4) {
      result.ok = false;
      result.error = "screenshot count must be an integer in [1,4]";
    } else {
      uint32_t dispatched = 0;
      for (uint32_t n = 0; n < count.get<uint32_t>(); ++n) {
        if (!::dxvk::war3::hooks::InvokeNativeScreenshotEventForTest()) break;
        ++dispatched;
      }
      result.ok = dispatched == count.get<uint32_t>();
      if (!result.ok) result.error = "native screenshot hook/gate/owner unavailable";
      response["dispatched"] = dispatched;
      response["frameIndex"] = frameIndex;
      response["saved"] = false;
      response["completion"] = "verify-new-file-and-worker-log";
      response["globalInputUsed"] = false;
    }

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
  } else if (state->request.command == "outline.pick" ||
             state->request.command == "outline.clear" ||
             state->request.command == "outline.status") {
    OutlineCommand(state->request.command, payload, response, result);
  } else if (state->request.command == "unit.walk.snapshot") {
    const auto diag = game::QueryUnitWalkDiagnostics();
    response["evidenceKind"] = "bounded-cpu-hook-counters-not-animation-visual-proof";
    response["installStatus"] = diag.installStatus;
    response["setterCalls"] = diag.setterCalls;
    response["setterAccepted"] = diag.setterAccepted;
    response["setterRejected"] = diag.setterRejected;
    response["lastSetterHandle"] = diag.lastSetterHandle;
    response["lastSetterFailure"] = diag.lastSetterFailure;
    response["lastSetterObservedType"] = diag.lastSetterObservedType;
    response["lastSetterDirectRawcode"] = diag.lastSetterDirectRawcode;
    response["lastSetterDirectMethodRva"] = diag.lastSetterDirectMethodRva;
    response["lastSetterCurrentProperty"] = diag.lastSetterCurrentProperty;
    response["lastSetterCurrentPropertyKnown"] =
        diag.lastSetterCurrentPropertyKnown;
    response["hookCalls"] = diag.hookCalls;
    response["autoCallerSeen"] = diag.autoCallerSeen;
    response["autoRequestMatched"] = diag.autoRequestMatched;
    response["autoRequestSuppressed"] = diag.autoRequestSuppressed;
    response["lastSuppressedCurrentProperty"] =
        diag.lastSuppressedCurrentProperty;
    response["lastSuppressedCurrentPropertyKnown"] =
        diag.lastSuppressedCurrentPropertyKnown;
    response["autoWithoutDisabledEntry"] = diag.autoWithoutDisabledEntry;
    response["autoIdentityRejected"] = diag.autoIdentityRejected;
    response["otherWalkRequests"] = diag.otherWalkRequests;
    response["lastOtherWalkCallerRva"] = diag.lastOtherWalkCallerRva;
    response["disabledUnitCount"] = diag.disabledUnitCount;
    result.ok = true;
  } else if (state->request.command == "jass.public_api") {
    const auto test = dxvk::war3::hooks::InvokeJassPublicApiForTest(
        payload.value("carrier", std::string()),
        payload.value("payload", std::string()));
    response["evidenceKind"] = "native-carrier-not-jass-bytecode";
    response["invoked"] = test.invoked;
    response["errorCode"] = test.errorCode;
    response["integer"] = test.integer;
    response["text"] = test.text;
    // A rejected API is a successfully observed test command, not API success.
    response["apiAccepted"] = test.invoked && test.errorCode == 0;
    result.ok = test.invoked;
    result.error = test.error;
  } else if (state->request.command == "visual.snapshot") {
    War3RenderSettings settings;
    result.ok = dxvk::war3::GetSettingsSnapshot(settings);
    if (!result.ok) {
      result.error = "settings mailbox unavailable";
    } else {
      const auto& vol = settings.postFx.volumetricLight;
      response["settingsEvidence"] = "author-pending-not-gpu-completion";
      response["sunEnabled"] = settings.sun.enabled;
      response["sunIntensity"] = settings.sun.intensity;
      response["sunColor"] = {settings.sun.color.x, settings.sun.color.y, settings.sun.color.z};
      response["csmEnabled"] = settings.shadows.enabled;
      response["postFxEnabled"] = settings.postFx.enabled;
      response["volumetricEnabled"] = vol.enabled;
      response["requestedBackend"] = static_cast<uint32_t>(vol.quality);
      response["globalMediumEnabled"] = vol.globalMediumEnabled;
      response["density"] = vol.density;
      response["heightFogEnabled"] = vol.heightFogEnabled;
      response["heightFog"] = {vol.heightFogBase, vol.heightFogFalloff, vol.heightFogStrength};
      response["fogCount"] = War3FogVolumeManager::Instance().GetVolumeCount();
      response["fogGeneration"] = War3FogVolumeManager::Instance().GetGeneration();
      const auto execution =
          ProcessServices().QueryWar3VolumetricExecutionDiagnostics();
      response["execution"] = {
          {"frameSerial", execution.frameSerial}, {"mapEpoch", execution.mapEpoch},
          {"deviceEpoch", execution.deviceEpoch}, {"requestedBackend", execution.requestedBackend},
          {"effectiveBackend", execution.effectiveBackend}, {"stage", execution.stage},
          {"compositeSubmitted", execution.compositeSubmitted},
          {"fogVolumes", execution.fogVolumes}, {"width", execution.width},
          {"height", execution.height}, {"observedFrames", execution.observedFrames}};
    }
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
    const bool activeDeviceAvailable =
        dxvk::war3::RunWithActiveDeviceServices(
        [&](IDeviceServices& device) {
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
        dxvk::war3::RunWithActiveDeviceServices([&](IDeviceServices& device) {
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
  } else if (state->request.command == "game.end_for_exit_test") {
    // Separate exact opt-in. Not part of the public author API or normal exit.
    if (env::getEnvVar("DXVK_WAR3_INTERNAL_EXIT_TEST") != "1") {
      result.error = "native exit test not enabled";
    } else {
      result.ok = hooks::EndJassGameForExitTest(result.error);
    }
    response["stockEndGameInvoked"] = result.ok;
    response["processExitProven"] = false;
  } else if (state->request.command == "game.restart_for_map_test") {
    // The same opt-in as the exit test: the stock RestartGame unloads the map
    // and loads it again, which is the map transition the tests need.
    if (env::getEnvVar("DXVK_WAR3_INTERNAL_EXIT_TEST") != "1") {
      result.error = "native exit test not enabled";
    } else {
      result.ok = hooks::RestartJassGameForMapTest(result.error);
    }
    response["stockRestartGameInvoked"] = result.ok;
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
