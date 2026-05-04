#pragma once

#include <cstdint>
#include <string>

namespace dxvk {
class D3D9DeviceEx;
class D3D9Surface;
}

namespace dxvk::war3::tools {

struct War3FrameCaptureResult {
  bool handled = false;
  bool ok = false;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string requestId;
  std::string outputPath;
  std::string error;
};

bool SubmitFrameCaptureRequest(const std::string& requestId,
                               const std::string& outputPath,
                               uint32_t timeoutMs = 8000,
                               War3FrameCaptureResult* outResult = nullptr);

bool HasPendingFrameCaptureRequest();

bool ProcessPendingFrameCapture(D3D9DeviceEx* device, D3D9Surface* sourceSurface,
                                War3FrameCaptureResult* outResult = nullptr);

} // namespace dxvk::war3::tools
