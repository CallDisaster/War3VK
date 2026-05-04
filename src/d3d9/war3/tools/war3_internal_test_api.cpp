#include "war3_internal_test_api.h"

#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_settings.h"

#include "../war3.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
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
          (std::min)(mode, 3u));
    }
    const bool applied = settings != nullptr;
    response["mode"] = mode;
    response["applied"] = applied;
    result.ok = applied;
    if (!applied)
      result.error = "SetShadowDebugMode failed";
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
