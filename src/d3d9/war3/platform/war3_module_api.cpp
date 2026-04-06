// war3_module_api.cpp - War3 渲染模块系统（增强版）

#include "war3_module_api.h"

#include "../../d3d9_war3_debug.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace war3example {
void RegisterUserExampleModule();
}

namespace war3module {

namespace {

struct ModuleEntry {
    War3ModuleInfo info = { };
    War3ModuleCallbacks callbacks = { };
    void* userData = nullptr;
    bool loaded = false;
};

std::mutex g_mutex;
std::vector<ModuleEntry> g_modules;
bool g_initialized = false;
std::once_flag g_registerBuiltinOnce;
std::atomic<uint64_t> g_dispatchCalls{0};
std::atomic<uint64_t> g_dispatchedHandlers{0};
std::atomic<uint64_t> g_callbackErrors{0};
std::atomic<uint32_t> g_runtimeState{
    static_cast<uint32_t>(War3ModuleRuntimeState::Cold)};

void EnsureBuiltinModulesRegistered() {
    std::call_once(g_registerBuiltinOnce, []() {
        war3example::RegisterUserExampleModule();
    });
}

} // namespace

uint32_t GetModuleApiVersion() {
    return WAR3_MODULE_API_VERSION;
}

bool RegisterModule(
    const War3ModuleInfo& info,
    const War3ModuleCallbacks& callbacks,
    void* userData) {
    if (!info.name || !info.name[0])
        return false;
    if (info.apiVersion != WAR3_MODULE_API_VERSION)
        return false;

    bool shouldLoadImmediately = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& mod : g_modules) {
            if (mod.info.name && std::string(mod.info.name) == info.name) {
                return false;
            }
        }

        ModuleEntry entry = {};
        entry.info = info;
        entry.callbacks = callbacks;
        entry.userData = userData;
        entry.loaded = false;

        // 运行期动态注册：若系统已初始化，则立即 onLoad，保证行为一致。
        if (g_initialized && entry.callbacks.onLoad) {
            entry.loaded = true;
            shouldLoadImmediately = true;
        }

        g_modules.emplace_back(entry);
    }

    if (shouldLoadImmediately) {
        try {
            callbacks.onLoad(userData);
        } catch (...) {
            g_callbackErrors.fetch_add(1, std::memory_order_relaxed);
            dxvk::war3dbg::Print("DXVK War3Module: onLoad crashed name=%s\n", info.name);
        }
    }
    return true;
}

bool HasModules() {
    EnsureBuiltinModulesRegistered();
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_modules.empty();
}

void InitializeModules() {
    EnsureBuiltinModulesRegistered();

    std::vector<ModuleEntry> toLoad;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_initialized) {
            return;
        }
        g_initialized = true;
        g_runtimeState.store(static_cast<uint32_t>(War3ModuleRuntimeState::Running),
                             std::memory_order_relaxed);
        toLoad.reserve(g_modules.size());
        for (auto& mod : g_modules) {
            if (!mod.loaded && mod.callbacks.onLoad) {
                mod.loaded = true;
                toLoad.push_back(mod);
            }
        }
    }

    for (auto& mod : toLoad) {
        try {
            mod.callbacks.onLoad(mod.userData);
        } catch (...) {
            g_callbackErrors.fetch_add(1, std::memory_order_relaxed);
            dxvk::war3dbg::Print("DXVK War3Module: onLoad crashed name=%s\n",
                           mod.info.name ? mod.info.name : "(null)");
        }
    }
}

void ShutdownModules() {
    g_runtimeState.store(static_cast<uint32_t>(War3ModuleRuntimeState::ShuttingDown),
                         std::memory_order_relaxed);

    std::vector<ModuleEntry> toUnload;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& mod : g_modules) {
            if (mod.loaded && mod.callbacks.onUnload) {
                toUnload.push_back(mod);
            }
            mod.loaded = false;
        }
        g_modules.clear();
        g_initialized = false;
    }

    for (auto& mod : toUnload) {
        try {
            mod.callbacks.onUnload(mod.userData);
        } catch (...) {
            g_callbackErrors.fetch_add(1, std::memory_order_relaxed);
            dxvk::war3dbg::Print("DXVK War3Module: onUnload crashed name=%s\n",
                           mod.info.name ? mod.info.name : "(null)");
        }
    }

    g_runtimeState.store(static_cast<uint32_t>(War3ModuleRuntimeState::Cold),
                         std::memory_order_relaxed);
}

void DispatchRenderEvent(
    war3shader::RenderEventID eventId,
    const war3shader::RenderContext* context) {
    InitializeModules();
    g_dispatchCalls.fetch_add(1, std::memory_order_relaxed);

    std::vector<ModuleEntry> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snapshot = g_modules;
    }

    for (const auto& mod : snapshot) {
        if (!mod.loaded || !mod.callbacks.onRenderEvent)
            continue;
        try {
            mod.callbacks.onRenderEvent(eventId, context, mod.userData);
            g_dispatchedHandlers.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            g_callbackErrors.fetch_add(1, std::memory_order_relaxed);
            dxvk::war3dbg::Print("DXVK War3Module: onRenderEvent crashed name=%s event=%u\n",
                           mod.info.name ? mod.info.name : "(null)",
                           static_cast<unsigned>(eventId));
        }
    }
}

War3ModuleRuntimeStats GetModuleRuntimeStats() {
    War3ModuleRuntimeStats stats = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        stats.registeredModules = static_cast<uint32_t>(g_modules.size());
        for (const auto& mod : g_modules) {
            if (mod.loaded)
                stats.loadedModules += 1;
        }
    }

    stats.dispatchCalls = g_dispatchCalls.load(std::memory_order_relaxed);
    stats.dispatchedHandlers =
        g_dispatchedHandlers.load(std::memory_order_relaxed);
    stats.callbackErrors = g_callbackErrors.load(std::memory_order_relaxed);
    stats.state = static_cast<War3ModuleRuntimeState>(
        g_runtimeState.load(std::memory_order_relaxed));
    return stats;
}

std::vector<War3ModuleInfo> ListRegisteredModules() {
    std::vector<War3ModuleInfo> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    out.reserve(g_modules.size());
    for (const auto& mod : g_modules) {
        out.push_back(mod.info);
    }
    return out;
}

} // namespace war3module
