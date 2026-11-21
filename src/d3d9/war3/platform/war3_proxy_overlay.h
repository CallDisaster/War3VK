// war3_proxy_overlay.h - the settings panel in proxy mode
//
// In process the panel (War3Imgui, war3/ui/war3_imgui.h) is drawn by the swap
// chain's Present and fed by the device's window procedure. In proxy mode
// neither exists in this process. Here the data layer draws it itself through
// the front end's IDirect3DDevice9, from the observer's `present` callback,
// so the front end does not observe its draws and nothing of the panel reaches
// the frame's records; and a subclass of the device window takes Ctrl+F1 and,
// while the panel wants them, the mouse and keyboard.
//
// The panel exists only while it is open: the first Ctrl+F1 creates the
// context, loads the font and builds the atlas at the next Present; closing it
// releases all of that again. A player who never opens it pays nothing.
//
// Every function runs on the device's owner thread (the game's window thread),
// except QueryOverlayStatus and NoteOverlayResource, which any thread may call.
// See docs/research/2026-11-17-x64-overlay-proxy.md.
#pragma once

#include "../../d3d9_include.h"

#include <cstdint>

namespace dxvk::war3::proxy {

// After the device is connected (war3_proxy_mode.cpp). `supported`: the front
// end has the private-reference exports and took an observer with `reset`.
void OverlayConnect(IDirect3DDevice9* device, bool supported);
// From the present callback, after the frame's records and the frame capture.
void OverlayPresent(IDirect3DDevice9* device);
// From the reset callback: the panel's buffers go before the game's Reset.
void OverlayReset(IDirect3DDevice9* device);
// From the released callback, before the data layer disconnects the device.
void OverlayReleased(IDirect3DDevice9* device);

// A panel resource created through the front end
// (war3/ui/war3_imgui_dx9_proxy_ownership.h), for the status counters.
void NoteOverlayResource(HRESULT result) noexcept;

// ui.panel_open / ui.panel_close: a private message through the subclassed
// device window, taken as Ctrl+F1 would be. False when the overlay is not
// attached or the window did not answer.
bool RoutePanelRequest(bool open) noexcept;

struct OverlayStatus {
  bool available = false;   // the overlay attached to the connected device
  bool wantOpen = false;    // an opening waits for the next Present
  uint64_t routedRequests = 0;
  uint64_t toggles = 0;     // Ctrl+F1 and routed requests that changed the state
  uint64_t swallowedInput = 0;
  uint64_t opens = 0;
  uint64_t closes = 0;
  uint64_t presentsDrawn = 0;
  uint64_t resourcesCreated = 0;
  uint64_t resourceFailures = 0;
  int32_t lastResourceResult = 0;
  // This process at the query: private commit and used address space.
  uint64_t privateBytes = 0;
  uint64_t virtualUsed = 0;
};
OverlayStatus QueryOverlayStatus() noexcept;

} // namespace dxvk::war3::proxy
