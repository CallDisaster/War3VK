// war3.cpp - War3 模块统一入口实现

#include "war3.h"
#include "core/war3_storm.h"
#include "core/war3_file_manager.h"
#include "core/war3_net_event_hook.h"
#include "../d3d9_device.h"
#include "../d3d9_war3_pipeline.h"
#include <windows.h>
#include <atomic>

namespace dxvk::war3 {

static bool s_initialized = false;
static uintptr_t s_gameDllBase = 0;
static std::atomic<D3D9DeviceEx*> s_activeDevice{ nullptr };

bool Initialize(uintptr_t gameDllBase) {
    if (s_initialized)
        return true;
    
    // 获取 Game.dll 基址
    if (gameDllBase == 0) {
        HMODULE hGame = GetModuleHandleA("Game.dll");
        if (!hGame) {
            WAR3_LOG_ERROR("Failed to get Game.dll handle\n");
            return false;
        }
        gameDllBase = reinterpret_cast<uintptr_t>(hGame);
    }
    
    s_gameDllBase = gameDllBase;
    
    // 初始化 Handle 解析器
    if (!HandleResolver::instance().initialize(gameDllBase)) {
        WAR3_LOG_WARN("HandleResolver initialization failed (will use fallback)\n");
    }

    // 初始化 Storm
    HMODULE hStorm = GetModuleHandleA("Storm.dll");
    if (hStorm) {
        War3Storm::get().init(hStorm);
        FileManager::get().initialize(hStorm);
    } else {
        WAR3_LOG_WARN("Failed to get Storm.dll handle\n");
    }
    
    // NetEventHook 由 executeJassFunction 首次触发后初始化，避免过早访问未就绪对象
    
    s_initialized = true;
    WAR3_LOG_INFO("War3 module initialized, Game.dll base=0x%08X\n", 
        static_cast<unsigned>(gameDllBase));
    
    return true;
}

void Shutdown() {
    s_initialized = false;
    s_gameDllBase = 0;
    WAR3_LOG_INFO("War3 module shutdown\n");
}

bool IsInitialized() {
    return s_initialized;
}

uintptr_t GetGameDllBase() {
    return s_gameDllBase;
}

void SetActiveDevice(D3D9DeviceEx* device) {
    s_activeDevice.store(device, std::memory_order_release);
}

D3D9DeviceEx* GetActiveDevice() {
    return s_activeDevice.load(std::memory_order_acquire);
}

War3RenderPipeline* GetActivePipeline() {
    auto* device = GetActiveDevice();
    return device ? device->GetWar3Pipeline() : nullptr;
}

War3RenderSettings* GetMutableSettings() {
    auto* pipeline = GetActivePipeline();
    return pipeline ? &pipeline->MutableSettings() : nullptr;
}

} // namespace dxvk::war3
