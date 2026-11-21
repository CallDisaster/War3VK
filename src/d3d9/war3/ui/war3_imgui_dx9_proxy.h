#pragma once

#include "imgui.h"

#ifndef IMGUI_DISABLE

struct IDirect3DDevice9;

// Proxy mode only: the panel drawn through the proxy front end's device
// (tools/render_host/d3d9_proxy.cpp) from the data layer's present callback.
// Generated from the same pinned upstream backend as war3_imgui_dx9.h
// (AutoTest/prepare_warvk_imgui_dx9.py --variant proxy); ownership goes through
// war3_imgui_dx9_proxy_ownership.h. Initialization, Reset and shutdown are
// paired by War3Imgui on the device's owner thread.
IMGUI_IMPL_API bool War3ProxyImGui_ImplDX9_Init(IDirect3DDevice9* device);
IMGUI_IMPL_API void War3ProxyImGui_ImplDX9_Shutdown();
IMGUI_IMPL_API void War3ProxyImGui_ImplDX9_NewFrame();
IMGUI_IMPL_API void War3ProxyImGui_ImplDX9_RenderDrawData(ImDrawData* drawData);
IMGUI_IMPL_API bool War3ProxyImGui_ImplDX9_CreateDeviceObjects();
IMGUI_IMPL_API void War3ProxyImGui_ImplDX9_InvalidateDeviceObjects();
IMGUI_IMPL_API void War3ProxyImGui_ImplDX9_UpdateTexture(ImTextureData* texture);

#endif
