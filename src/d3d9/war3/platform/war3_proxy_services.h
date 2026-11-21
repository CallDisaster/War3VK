// war3_proxy_services.h - the data layer's side of a proxy-mode device
//
// war3_proxy_mode.cpp owns the front end and the factory; this file owns what
// the data layer does with the device the game created: the IDeviceServices
// it publishes, and the work at the observer's four moments that the
// in-process device used to do inline (readiness, frame capture, the frame's
// records). Everything here runs on the game's D3D thread.
#pragma once

#include "../../d3d9_include.h"
#include "../core/war3_d3d9_observer.h"

#include <cstdint>

namespace dxvk::war3::proxy {

// Publishes the proxy implementation of IDeviceServices for `device`.
void ConnectDevice(IDirect3DDevice9* device);
// Withdraws it; the device is about to be destroyed.
void DisconnectDevice(IDirect3DDevice9* device);

void ObserveDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts);
void ObservePresent(IDirect3DDevice9* device);
void ObserveResized(IDirect3DDevice9* device, uint32_t previousWidth, uint32_t previousHeight,
                    uint32_t width, uint32_t height);
// The session with the host ended: the player is told and the process ends
// (war3_proxy_host_lost.h). Does not return.
void ObserveTerminal(IDirect3DDevice9* device, const WarVKD3D9TerminalFacts& facts);

// World RenderScene scope as the game last reported it (owner thread).
bool InWorldScene() noexcept;

} // namespace dxvk::war3::proxy
