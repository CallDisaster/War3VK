#include "war3_internal_test_api.h"

namespace dxvk::war3::tools {

bool IsInternalTestApiEnabled() {
  return false;
}

void ResetInternalTestApiState() {
}

bool ProcessPendingInternalTestRequest(uint64_t, War3InternalTestResult* outResult) {
  if (outResult) {
    outResult->handled = false;
    outResult->ok = false;
    outResult->requestId.clear();
    outResult->command.clear();
    outResult->error.clear();
  }
  return false;
}

} // namespace dxvk::war3::tools
