#pragma once

#include "imgui.h"

#ifndef IMGUI_DISABLE

struct IDirect3DDevice9;

// 只供本模块 D3D9DeviceEx 自有的 UI 使用。设备由外层 owner 保活；
// 初始化/Reset/退出必须配套，不能用这些入口接管外部 D3D9 device。
// 实现从固定版本 upstream backend 生成，只适配引用所有权，不另存 renderer 副本。
IMGUI_IMPL_API bool War3ImGui_ImplDX9_Init(IDirect3DDevice9* device);
IMGUI_IMPL_API void War3ImGui_ImplDX9_Shutdown();
IMGUI_IMPL_API void War3ImGui_ImplDX9_NewFrame();
IMGUI_IMPL_API void War3ImGui_ImplDX9_RenderDrawData(ImDrawData* drawData);
IMGUI_IMPL_API bool War3ImGui_ImplDX9_CreateDeviceObjects();
IMGUI_IMPL_API void War3ImGui_ImplDX9_InvalidateDeviceObjects();
IMGUI_IMPL_API void War3ImGui_ImplDX9_UpdateTexture(ImTextureData* texture);

#endif
