#pragma once

#include "../../d3d9_include.h"

#include <cstdint>

namespace dxvk::war3::platform {

struct War3NativeDeviceResolveSnapshot {
  uintptr_t gameBase = 0;
  uintptr_t gxDevice = 0;
  uintptr_t d3d9Device = 0;
  uint64_t attemptCount = 0;
  uint64_t hitCount = 0;
  bool bound = false;
};

/**
 * Resolve War3's live CGxDeviceD3d -> IDirect3DDevice9* bridge and bind it to
 * native D3D9 consumers. This is used by non-proxy, mid-game LoadLibrary mode
 * where DXVK did not create the original device.
 */
bool TryBindNativeDeviceFromWar3Globals(const char* reason = nullptr,
                                        bool forceLog = false);

War3NativeDeviceResolveSnapshot GetNativeDeviceResolveSnapshot();

} // namespace dxvk::war3::platform
