#include "war3_hook_install_util.h"

#include <MinHook.h>

#include "../../d3d9_war3_debug.h"

namespace dxvk::war3::hooks {

bool InstallMinHook(LPVOID target, LPVOID detour, LPVOID* original,
                    const char* domain, const char* hookName,
                    bool ensureInitialized, bool logSuccess) {
  const char* safeDomain = (domain && domain[0]) ? domain : "Unknown";
  const char* safeName = (hookName && hookName[0]) ? hookName : "UnnamedHook";

  if (!target || !detour || !original) {
    war3dbg::Print(
        "DXVK War3Hook[%s]: Skip %s (invalid args target=%p detour=%p original=%p)\n",
        safeDomain, safeName, target, detour, original);
    return false;
  }

  if (ensureInitialized) {
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
      war3dbg::Print(
          "DXVK War3Hook[%s]: 安装 %s 失败 (MH_Initialize=%d)\n",
          safeDomain, safeName, static_cast<int>(initStatus));
      return false;
    }
  }

  const MH_STATUS createStatus = MH_CreateHook(target, detour, original);
  if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
    war3dbg::Print(
        "DXVK War3Hook[%s]: 安装 %s 失败 (MH_CreateHook=%d, addr=%p)\n",
        safeDomain, safeName, static_cast<int>(createStatus), target);
    return false;
  }

  const MH_STATUS enableStatus = MH_EnableHook(target);
  if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
    war3dbg::Print(
        "DXVK War3Hook[%s]: 启用 %s 失败 (MH_EnableHook=%d, addr=%p)\n",
        safeDomain, safeName, static_cast<int>(enableStatus), target);
    return false;
  }

  if (logSuccess) {
    war3dbg::Print("DXVK War3Hook[%s]: %s Hook Success (addr=%p)\n", safeDomain,
                   safeName, target);
  }
  return true;
}

}  // namespace dxvk::war3::hooks

