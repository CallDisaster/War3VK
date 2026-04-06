// war3_module_api.h - War3 渲染模块系统（基础版）
// 说明：当前用于建立“模块化生态”的稳定入口，暂不包含动态加载。

#pragma once

#include <cstdint>
#include <vector>

#include "../../war3_shader_api.h"

namespace war3module {

// 模块 API 版本（用于兼容检查）
constexpr uint32_t WAR3_MODULE_API_VERSION = 0x010000; // 1.0.0

// 模块元信息
struct War3ModuleInfo {
    const char* name;         // 模块名字（必须唯一）
    uint32_t    version;      // 模块版本（自定义）
    uint32_t    apiVersion;   // 模块依赖的 API 版本
};

// 模块回调
struct War3ModuleCallbacks {
    void (*onLoad)(void* userData);
    void (*onUnload)(void* userData);
    void (*onRenderEvent)(
        war3shader::RenderEventID eventId,
        const war3shader::RenderContext* context,
        void* userData);
};

// 模块系统运行态
enum class War3ModuleRuntimeState : uint8_t {
    Cold = 0,         // 未初始化
    Running = 1,      // 运行中
    ShuttingDown = 2, // 关闭中
};

// 模块系统统计
struct War3ModuleRuntimeStats {
    uint32_t registeredModules = 0;
    uint32_t loadedModules = 0;
    uint64_t dispatchCalls = 0;
    uint64_t dispatchedHandlers = 0;
    uint64_t callbackErrors = 0;
    War3ModuleRuntimeState state = War3ModuleRuntimeState::Cold;
};

// 获取当前模块 API 版本
uint32_t GetModuleApiVersion();

// 注册模块（内部/内置模块使用，后续可扩展为动态模块）
bool RegisterModule(
    const War3ModuleInfo& info,
    const War3ModuleCallbacks& callbacks,
    void* userData);

// 是否存在已注册模块（内部使用）
bool HasModules();

// 初始化与关闭（由渲染层驱动）
void InitializeModules();
void ShutdownModules();

// 渲染事件分发（由渲染层调用）
void DispatchRenderEvent(
    war3shader::RenderEventID eventId,
    const war3shader::RenderContext* context);

// 运行时统计（诊断用途）
War3ModuleRuntimeStats GetModuleRuntimeStats();

// 已注册模块清单（用于诊断和前端展示）
std::vector<War3ModuleInfo> ListRegisteredModules();

} // namespace war3module
