#pragma once

#include <cstdint>
#include <string>

namespace dxvk::war3::tools {

struct War3InternalTestRequest {
  std::string requestId;
  std::string command;
  std::string payloadJson;
  uint64_t issuedAtMs = 0;
};

struct War3InternalTestResult {
  bool handled = false;
  bool ok = false;
  std::string requestId;
  std::string command;
  std::string resultJson;
  std::string error;
};

bool IsInternalTestApiEnabled();
void ResetInternalTestApiState();
bool SubmitInternalTestRequest(const War3InternalTestRequest& request,
                               uint32_t timeoutMs = 6000,
                               War3InternalTestResult* outResult = nullptr);
bool ProcessPendingInternalTestRequest(
    uint64_t frameIndex = 0,
    War3InternalTestResult* outResult = nullptr);

} // namespace dxvk::war3::tools
