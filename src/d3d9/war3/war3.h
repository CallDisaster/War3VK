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

#include <cstddef>
#include <functional>
#include <memory>

namespace dxvk {
class D3D9DeviceEx;
class War3RenderPipeline;
struct War3RenderSettings;
struct War3RenderSettingsMailbox;
}

namespace dxvk::war3 {

// Scoped, serialized editor for map/JAPI/UI authored render settings. The
// returned pointer is valid only for the lifetime of this guard and never
// points at the render-owner copy consumed by the DXVK CS thread.
class War3SettingsWrite final {
public:
    War3SettingsWrite() noexcept;
    War3SettingsWrite(War3SettingsWrite&& other) noexcept;
    War3SettingsWrite& operator=(War3SettingsWrite&& other) noexcept;
    ~War3SettingsWrite();

    War3SettingsWrite(const War3SettingsWrite&) = delete;
    War3SettingsWrite& operator=(const War3SettingsWrite&) = delete;

    explicit operator bool() const noexcept;
    bool operator==(std::nullptr_t) const noexcept;
    bool operator!=(std::nullptr_t) const noexcept;
    War3RenderSettings* get() const noexcept;
    War3RenderSettings* operator->() const noexcept;
    War3RenderSettings& operator*() const noexcept;

private:
    struct Impl;
    explicit War3SettingsWrite(
        std::shared_ptr<War3RenderSettingsMailbox> mailbox);
    void Commit() noexcept;

    std::unique_ptr<Impl> m_impl;

    friend War3SettingsWrite GetMutableSettings();
};

// 初始化所有 War3 模块
// gameDllBase: Game.dll 的基址，传 0 则自动获取
bool Initialize(uintptr_t gameDllBase = 0);

// 清理所有模块
void Shutdown();

// 检查是否已初始化
bool IsInitialized();

// 获取 Game.dll 基址
uintptr_t GetGameDllBase();

// Publish/revoke the active D3D9 owner only through lifecycle transactions.
void PublishActiveDeviceAfter(
    D3D9DeviceEx* device, const std::function<void()>& beforePublish);
bool ClearActiveDeviceIfCurrent(D3D9DeviceEx* device);
bool IsActiveDevice(const D3D9DeviceEx* expected);
bool HasActivePipeline();
bool RunWithActiveDevice(
    const std::function<void(D3D9DeviceEx&)>& transaction);
bool RunWithActiveDevicePublication(
    D3D9DeviceEx* expected, const std::function<void()>& transaction);
void RequestActiveDeviceShadowMapResetOrCpuFallback();

// 获取有生命周期约束的可写渲染设置；离开作用域时提交到下一帧安全点。
War3SettingsWrite GetMutableSettings();

// 复制当前 author/pending 设置，供只读查询使用。
bool GetSettingsSnapshot(War3RenderSettings& out);

// DXVK validation host helper: consume the latest semantic shadow scene on the
// active device without exposing D3D9DeviceEx to render-layer code.
bool ExecuteSemanticShadowSceneForValidation(
    bool unitsOnly,
    bool executeNativeBackendValidation = true);

} // namespace dxvk::war3
