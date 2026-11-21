// war3_proxy_mode.h - the product DLL without an in-process device
//
// In proxy mode this DLL runs only the data layer. The game's D3D9 calls go to
// a D3D9 front end in another DLL (tools/render_host/d3d9_proxy.cpp), which
// forwards them to the x64 render host; no D3D9DeviceEx and no Vulkan device
// exist in this process. This file is the seam: it loads the front end, hands
// the game the front end's objects, and connects the data layer to each device
// the game creates.
//
//   Game.dll --D3D9--> front end --command ring--> x64 host
//                         ^   |
//      data layer --------+   +--> observer (war3_d3d9_observer.h)
//      (WarVKD3D9* exports)
//
// See docs/plan/2026-11-16-x64-phase5-1-first-wiring.md.
#pragma once

#include "../../d3d9_include.h"
#include "../core/war3_d3d9_observer.h"

#include <cstdint>

namespace dxvk::war3::proxy {

// True when the environment names a front end (WARVK_D3D9_FRONT, the full path
// of its DLL) or war3.exe runs with WarVK\x64 beside this DLL
// (war3_proxy_auto.h). Otherwise this DLL behaves exactly as before. The front
// end itself refuses a game process without a game profile
// (d3d9_window_identity.h, GameSandboxProfile).
bool Requested() noexcept;

// Direct3DCreate9 in proxy mode: the front end's factory behind a wrapper
// that connects the data layer to the device the game creates from it.
// Null when the front end cannot be loaded or refuses. An automatically found
// proxy is first asked for the device caps, which starts the host; if that
// fails, Requested() turns false and the caller continues in process.
IDirect3D9* CreateFactory(UINT sdkVersion) noexcept;

// The front end's device while one is connected, else null. Owner thread
// (the game's D3D thread) only.
IDirect3DDevice9* ConnectedDevice() noexcept;

// The front end's exports that take the device, for the data layer's record
// producers. Each answers D3DERR_INVALIDCALL while no device is connected.
HRESULT SetFrameScene(uint32_t schema, const void* data, uint32_t bytes) noexcept;
HRESULT AnnotateNextDraw(uint32_t schema, const void* data, uint32_t bytes) noexcept;
HRESULT MarkStage(uint32_t stage) noexcept;
HRESULT RequestSessionReset(uint32_t reason, uint64_t generation) noexcept;
// The fixed-function lighting of the world scene (war3_d3d9_observer.h);
// null ends it.
HRESULT SetWorldLighting(const WarVKD3D9WorldLighting* lighting) noexcept;
// The settings panel's own resources on the connected device
// (war3/ui/war3_imgui_dx9_proxy_ownership.h, WarVKD3D9RetainPrivate):
// RetainPrivate trades the creation's public reference for a private one, so
// the resource no longer keeps the device alive; ReleasePrivate drops it.
// D3DERR_INVALIDCALL without a connected device or the front end's exports.
HRESULT RetainPrivate(IUnknown* resource) noexcept;
HRESULT ReleasePrivate(IUnknown* resource) noexcept;

// What the session has done, for the control plane and the probes.
struct Status {
  bool requested = false;
  bool frontLoaded = false;
  bool deviceConnected = false;
  bool terminal = false;
  uint32_t factories = 0;
  uint32_t devices = 0;
  uint64_t draws = 0;
  uint64_t presents = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  char terminalKind[24] = {};
  char terminalDetail[48] = {};
};
Status QueryStatus() noexcept;

} // namespace dxvk::war3::proxy
