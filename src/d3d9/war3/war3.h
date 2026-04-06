// war3.h - War3 模块统一入口
// 包含此文件即可访问所有 War3 相关功能

#pragma once

// 核心模块
#include "core/war3_memory.h"
#include "core/war3_game_structs.h"
#include "core/war3_native_structs.h"

// Handle 模块
#include "handle/war3_handle_resolver.h"

// 渲染对象模块
#include "render/war3_render_objects.h"

// 调试模块
#include "debug/war3_debug.h"

namespace dxvk {
class D3D9DeviceEx;
class War3RenderPipeline;
struct War3RenderSettings;
}

namespace dxvk::war3 {

// 初始化所有 War3 模块
// gameDllBase: Game.dll 的基址，传 0 则自动获取
bool Initialize(uintptr_t gameDllBase = 0);

// 清理所有模块
void Shutdown();

// 检查是否已初始化
bool IsInitialized();

// 获取 Game.dll 基址
uintptr_t GetGameDllBase();

// 设置/获取当前活跃的 D3D9 设备
void SetActiveDevice(D3D9DeviceEx* device);
D3D9DeviceEx* GetActiveDevice();

// 获取当前渲染管线（可能为空）
War3RenderPipeline* GetActivePipeline();

// 获取可写的渲染设置（可能为空）
War3RenderSettings* GetMutableSettings();

} // namespace dxvk::war3
