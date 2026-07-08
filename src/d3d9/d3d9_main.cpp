#include "../dxvk/dxvk_instance.h"

#include "d3d9_war3_hook.h"
#include "d3d9_interface.h"
#include "d3d9_shader_validator.h"
#include "war3/hooks/war3_hook_jass.h"
#include "war3/platform/war3_native_device_resolver.h"

#include "d3d9_annotation.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

class D3DFE_PROCESSVERTICES;
using PSGPERRORID = UINT;

namespace dxvk {
Logger Logger::s_instance("d3d9.log");
D3D9GlobalAnnotationList D3D9GlobalAnnotationList::s_instance;

HRESULT CreateD3D9(bool Extended, IDirect3D9Ex **ppDirect3D9Ex,
                   const D3D9ON12_ARGS *pOverrideList, uint32_t OverrideCount) {
  if (!ppDirect3D9Ex)
    return D3DERR_INVALIDCALL;

  *ppDirect3D9Ex =
      ref(new D3D9InterfaceEx(Extended, pOverrideList, OverrideCount));
  return D3D_OK;
}
} // namespace dxvk

namespace {

std::atomic<bool> g_warVkDirectLoadBootstrapStarted{false};

void AppendWarVkBootstrapMarker(const char *phase, const char *source) {
  char tempPath[MAX_PATH] = {};
  const DWORD tempLen = ::GetTempPathA(sizeof(tempPath), tempPath);
  if (tempLen == 0 || tempLen >= sizeof(tempPath))
    return;

  char markerPath[MAX_PATH] = {};
  std::snprintf(markerPath, sizeof(markerPath),
                "%swarvk_bootstrap_marker_%lu.log", tempPath,
                static_cast<unsigned long>(::GetCurrentProcessId()));

  HANDLE file = ::CreateFileA(markerPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;

  char line[256] = {};
  const int len = std::snprintf(
      line, sizeof(line), "tick=%lu phase=%s source=%s gameDll=%p\n",
      static_cast<unsigned long>(::GetTickCount()),
      phase ? phase : "<null>", source ? source : "<null>",
      ::GetModuleHandleA("Game.dll"));
  if (len > 0) {
    DWORD written = 0;
    ::WriteFile(file, line, static_cast<DWORD>(std::min<int>(len, sizeof(line))),
                &written, nullptr);
  }
  ::CloseHandle(file);
}

DWORD WINAPI WarVkDirectLoadBootstrapThread(LPVOID) {
  AppendWarVkBootstrapMarker("thread-start", "direct-load");
  OutputDebugStringA("DXVK WarVK: direct-load bootstrap thread started\n");

  // JASS-side loaders may LoadLibrary this DLL after Game.dll is already
  // present, but waiting a little makes the entry safe for earlier injectors too.
  uintptr_t gameBase = 0;
  for (uint32_t i = 0; i < 200; ++i) {
    gameBase = reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
    if (gameBase != 0)
      break;
    ::Sleep(50);
  }

  if (gameBase != 0) {
    AppendWarVkBootstrapMarker("activate-runtime-begin", "direct-load");
    dxvk::TryInstallShadowHooksEarly(gameBase, "DirectLoadBootstrap_PreActivate");
    dxvk::ActivateWar3Runtime(gameBase, "DirectLoadBootstrap");
    AppendWarVkBootstrapMarker("activate-runtime-end", "direct-load");
    const bool bridgeInstalled =
        dxvk::war3::hooks::War3HookJass::InstallCommandBridgeOnly(
            gameBase, "DirectLoadBootstrap");
    AppendWarVkBootstrapMarker(bridgeInstalled ? "jass-bridge-installed"
                                               : "jass-bridge-incomplete",
                               "direct-load");
  } else {
    AppendWarVkBootstrapMarker("no-game-dll", "direct-load");
  }

  // In proxy mode the device is passed through Direct3DCreate9. In mid-game
  // LoadLibrary mode War3 already owns the native D3D9 device, so resolve the
  // CGxDeviceD3d singleton instead. Keep this bounded and non-fatal.
  for (uint32_t i = 0; i < 240; ++i) {
    if (dxvk::war3::platform::TryBindNativeDeviceFromWar3Globals(
            "DirectLoadBootstrap", i == 0u)) {
      AppendWarVkBootstrapMarker("native-device-bound", "direct-load");
      break;
    }
    ::Sleep(500);
  }
  return 0;
}

void StartWarVkDirectLoadBootstrap(const char *source) {
  bool expected = false;
  if (!g_warVkDirectLoadBootstrapStarted.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }

  char buffer[160] = {};
  std::snprintf(buffer, sizeof(buffer),
                "DXVK WarVK: direct-load bootstrap requested source=%s\n",
                source ? source : "<unknown>");
  OutputDebugStringA(buffer);
  AppendWarVkBootstrapMarker("request", source);

  HANDLE thread =
      ::CreateThread(nullptr, 0, &WarVkDirectLoadBootstrapThread, nullptr, 0,
                     nullptr);
  if (thread) {
    ::CloseHandle(thread);
  } else {
    g_warVkDirectLoadBootstrapStarted.store(false, std::memory_order_release);
    AppendWarVkBootstrapMarker("thread-create-failed", source);
  }
}

bool ShouldBootstrapFromDllMain() {
  const char *value = std::getenv("DXVK_WARVK_BOOTSTRAP_ON_DLLMAIN");
  if (!value)
    return false;
  return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
         std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "True") == 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    ::DisableThreadLibraryCalls(instance);
    if (ShouldBootstrapFromDllMain())
      StartWarVkDirectLoadBootstrap("DllMain");
  }
  return TRUE;
}

extern "C" {

DLLEXPORT int __stdcall WarVK_Initialize(void) {
  AppendWarVkBootstrapMarker("export-call", "WarVK_Initialize");
  StartWarVkDirectLoadBootstrap("WarVK_Initialize");
  return 1;
}

DLLEXPORT void __stdcall Initialize(void) {
  AppendWarVkBootstrapMarker("export-call", "Initialize");
  StartWarVkDirectLoadBootstrap("Initialize");
}

DLLEXPORT IDirect3D9 *__stdcall Direct3DCreate9(UINT nSDKVersion) {
  OutputDebugStringA("DXVK: Direct3DCreate9 called\n");
  IDirect3D9Ex *pDirect3D = nullptr;
  dxvk::CreateD3D9(false, &pDirect3D, nullptr, 0);

  return pDirect3D;
}

DLLEXPORT HRESULT __stdcall Direct3DCreate9Ex(UINT nSDKVersion,
                                              IDirect3D9Ex **ppDirect3D9Ex) {
  return dxvk::CreateD3D9(true, ppDirect3D9Ex, nullptr, 0);
}

DLLEXPORT int __stdcall D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) {
  return dxvk::D3D9GlobalAnnotationList::Instance().BeginEvent(col, wszName);
}

DLLEXPORT int __stdcall D3DPERF_EndEvent(void) {
  return dxvk::D3D9GlobalAnnotationList::Instance().EndEvent();
}

DLLEXPORT void __stdcall D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName) {
  dxvk::D3D9GlobalAnnotationList::Instance().SetMarker(col, wszName);
}

DLLEXPORT void __stdcall D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName) {
  dxvk::D3D9GlobalAnnotationList::Instance().SetRegion(col, wszName);
}

DLLEXPORT BOOL __stdcall D3DPERF_QueryRepeatFrame(void) {
  return dxvk::D3D9GlobalAnnotationList::Instance().QueryRepeatFrame();
}

DLLEXPORT void __stdcall D3DPERF_SetOptions(DWORD dwOptions) {
  dxvk::D3D9GlobalAnnotationList::Instance().SetOptions(dwOptions);
}

DLLEXPORT DWORD __stdcall D3DPERF_GetStatus(void) {
  return dxvk::D3D9GlobalAnnotationList::Instance().GetStatus();
}

DLLEXPORT void __stdcall DebugSetMute(void) {}

DLLEXPORT int __stdcall DebugSetLevel(void) { return 0; }

// Processor Specific Geometry Pipeline
// for P3 SIMD/AMD 3DNow.

DLLEXPORT void __stdcall PSGPError(D3DFE_PROCESSVERTICES *a, PSGPERRORID b,
                                   UINT c) {}

DLLEXPORT void __stdcall PSGPSampleTexture(D3DFE_PROCESSVERTICES *a, UINT b,
                                           float (*const c)[4], UINT d,
                                           float (*const e)[4]) {}

DLLEXPORT dxvk::D3D9ShaderValidator *__stdcall
Direct3DShaderValidatorCreate9(void) {
  return ref(new dxvk::D3D9ShaderValidator());
}

static std::atomic<int> g_d3d9MaximizedWindowedModeShim = 0;

DLLEXPORT int __stdcall Direct3D9EnableMaximizedWindowedModeShim(UINT a) {
  const int enabled = a ? 1 : 0;
  g_d3d9MaximizedWindowedModeShim.store(enabled, std::memory_order_relaxed);
  dxvk::Logger::info(dxvk::str::format(
      "Direct3D9EnableMaximizedWindowedModeShim: enabled=", enabled));
  return enabled;
}

DLLEXPORT void __stdcall
DXVK_RegisterAnnotation(IDXVKUserDefinedAnnotation *annotation) {
  dxvk::D3D9GlobalAnnotationList::Instance().RegisterAnnotator(annotation);
}

DLLEXPORT void __stdcall
DXVK_UnRegisterAnnotation(IDXVKUserDefinedAnnotation *annotation) {
  dxvk::D3D9GlobalAnnotationList::Instance().UnregisterAnnotator(annotation);
}

DLLEXPORT void __stdcall Direct3D9ForceHybridEnumeration(UINT uHybrid) {}

DLLEXPORT IDirect3D9 *__stdcall
Direct3DCreate9On12(UINT sdk_version, D3D9ON12_ARGS *override_list,
                    UINT override_entry_count) {
  dxvk::Logger::warn(
      "Direct3DCreate9On12: 9On12 functionality is unimplemented.");

  IDirect3D9Ex *pDirect3D = nullptr;
  dxvk::CreateD3D9(false, &pDirect3D, override_list, override_entry_count);

  return pDirect3D;
}

DLLEXPORT HRESULT __stdcall Direct3DCreate9On12Ex(UINT sdk_version,
                                                  D3D9ON12_ARGS *override_list,
                                                  UINT override_entry_count,
                                                  IDirect3D9Ex **output) {
  dxvk::Logger::warn(
      "Direct3DCreate9On12Ex: 9On12 functionality is unimplemented.");
  return dxvk::CreateD3D9(true, output, override_list, override_entry_count);
}
}
