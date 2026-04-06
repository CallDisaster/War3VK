#pragma once

#include <cstdint>
#include <string>

namespace dxvk::war3::tools {

struct War3InternalTestResult {
  bool handled = false;
  bool ok = false;
  std::string requestId;
  std::string command;
  std::string error;
};

bool IsInternalTestApiEnabled();
void ResetInternalTestApiState();
bool ProcessPendingInternalTestRequest(
    uint64_t frameIndex = 0,
    War3InternalTestResult* outResult = nullptr);

} // namespace dxvk::war3::tools
