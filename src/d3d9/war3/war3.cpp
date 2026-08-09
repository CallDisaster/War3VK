// war3.cpp - War3 模块统一入口实现

#include "war3.h"
#include "core/war3_storm.h"
#include "core/war3_file_manager.h"
#include "core/war3_net_event_hook.h"
#include "../d3d9_device.h"
#include "../d3d9_war3_pipeline.h"
#include <windows.h>
#include <atomic>
#include <utility>

namespace dxvk::war3 {

static bool s_initialized = false;
static uintptr_t s_gameDllBase = 0;
static std::atomic<D3D9DeviceEx*> s_activeDevice{ nullptr };
// Some active-device transactions re-enter through shadow allocation while a
// render hook is executing. Keep publication exclusion across the complete
// callback without deadlocking that same render thread.
static std::recursive_mutex s_activePublicationMutex;
static std::shared_ptr<War3RenderSettingsMailbox> s_activeSettingsMailbox;

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

namespace {

void PublishActiveDeviceLocked(D3D9DeviceEx* device) {
    std::shared_ptr<War3RenderSettingsMailbox> mailbox;
    if (device != nullptr && device->GetWar3Pipeline() != nullptr)
        mailbox = device->GetWar3Pipeline()->GetSettingsMailbox();
    // Availability is the primary publication. A writer that races this
    // handoff may briefly fail to acquire the new mailbox, but must never see
    // a mailbox for a device that has not yet been published.
    s_activeDevice.store(device, std::memory_order_release);
    std::atomic_store_explicit(
        &s_activeSettingsMailbox, std::move(mailbox),
        std::memory_order_release);
}

} // namespace

void PublishActiveDeviceAfter(
    D3D9DeviceEx* device, const std::function<void()>& beforePublish) {
    std::lock_guard<std::recursive_mutex> lock(s_activePublicationMutex);
    if (beforePublish)
        beforePublish();
    PublishActiveDeviceLocked(device);
}

bool ClearActiveDeviceIfCurrent(D3D9DeviceEx* device) {
    std::lock_guard<std::recursive_mutex> lock(s_activePublicationMutex);
    if (s_activeDevice.load(std::memory_order_acquire) != device)
        return false;
    // Revoke new settings writers before the device publication. Guards that
    // already captured the shared mailbox remain memory-safe and commit only
    // into the retired mailbox, never into a destroyed pipeline object.
    std::atomic_store_explicit(
        &s_activeSettingsMailbox,
        std::shared_ptr<War3RenderSettingsMailbox>{},
        std::memory_order_release);
    s_activeDevice.store(nullptr, std::memory_order_release);
    return true;
}

bool IsActiveDevice(const D3D9DeviceEx* expected) {
    // This is an identity snapshot only: callers already own or callback-pin
    // expected, and no pointer is returned for a later dereference.
    return expected != nullptr &&
           s_activeDevice.load(std::memory_order_acquire) == expected;
}

bool RunWithActiveDevice(
    const std::function<void(D3D9DeviceEx&)>& transaction) {
    std::lock_guard<std::recursive_mutex> lock(s_activePublicationMutex);
    auto* const device = s_activeDevice.load(std::memory_order_acquire);
    if (device == nullptr)
        return false;
    if (transaction)
        transaction(*device);
    return true;
}

bool HasActivePipeline() {
    bool available = false;
    RunWithActiveDevice([&available](D3D9DeviceEx& device) {
        available = device.GetWar3Pipeline() != nullptr;
    });
    return available;
}

bool RunWithActiveDevicePublication(
    D3D9DeviceEx* expected, const std::function<void()>& transaction) {
    std::lock_guard<std::recursive_mutex> lock(s_activePublicationMutex);
    if (expected == nullptr ||
        s_activeDevice.load(std::memory_order_acquire) != expected) {
        return false;
    }
    if (transaction)
        transaction();
    return true;
}

void RequestActiveDeviceShadowMapResetOrCpuFallback() {
    std::lock_guard<std::recursive_mutex> lock(s_activePublicationMutex);
    if (auto* const device =
            s_activeDevice.load(std::memory_order_acquire)) {
        device->War3RequestShadowMapEpochReset();
    } else {
        D3D9DeviceEx::War3ResetCpuSemanticMapSession();
    }
}

struct War3SettingsWrite::Impl {
    explicit Impl(std::shared_ptr<War3RenderSettingsMailbox> owner)
    : mailbox(std::move(owner)), lock(mailbox->mutex),
      value(&mailbox->pending), beforeExposure(value->postFx.exposure) {
    }

    std::shared_ptr<War3RenderSettingsMailbox> mailbox;
    std::unique_lock<std::mutex> lock;
    War3RenderSettings* value = nullptr;
    float beforeExposure = 0.0f;
};

War3SettingsWrite::War3SettingsWrite() noexcept = default;

War3SettingsWrite::War3SettingsWrite(
    std::shared_ptr<War3RenderSettingsMailbox> mailbox) {
    if (mailbox != nullptr)
        m_impl = std::make_unique<Impl>(std::move(mailbox));
}

War3SettingsWrite::War3SettingsWrite(War3SettingsWrite&& other) noexcept =
    default;

War3SettingsWrite& War3SettingsWrite::operator=(
    War3SettingsWrite&& other) noexcept {
    if (this != &other) {
        Commit();
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

War3SettingsWrite::~War3SettingsWrite() {
    Commit();
}

War3SettingsWrite::operator bool() const noexcept {
    return m_impl != nullptr && m_impl->value != nullptr;
}

bool War3SettingsWrite::operator==(std::nullptr_t) const noexcept {
    return !static_cast<bool>(*this);
}

bool War3SettingsWrite::operator!=(std::nullptr_t) const noexcept {
    return static_cast<bool>(*this);
}

War3RenderSettings* War3SettingsWrite::get() const noexcept {
    return m_impl != nullptr ? m_impl->value : nullptr;
}

War3RenderSettings* War3SettingsWrite::operator->() const noexcept {
    return get();
}

War3RenderSettings& War3SettingsWrite::operator*() const noexcept {
    return *get();
}

void War3SettingsWrite::Commit() noexcept {
    if (m_impl == nullptr)
        return;

    const auto mailbox = m_impl->mailbox;
    if (mailbox != nullptr && m_impl->value != nullptr) {
        ++mailbox->pendingRevision;
        if (m_impl->beforeExposure != m_impl->value->postFx.exposure)
            ++mailbox->pendingExposureRevision;
    }
    m_impl.reset();
}

War3SettingsWrite GetMutableSettings() {
    return War3SettingsWrite(std::atomic_load_explicit(
        &s_activeSettingsMailbox, std::memory_order_acquire));
}

bool GetSettingsSnapshot(War3RenderSettings& out) {
    const auto mailbox = std::atomic_load_explicit(
        &s_activeSettingsMailbox, std::memory_order_acquire);
    if (mailbox == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(mailbox->mutex);
    out = mailbox->pending;
    return true;
}

bool ExecuteSemanticShadowSceneForValidation(
    bool unitsOnly,
    bool executeNativeBackendValidation) {
    bool executed = false;
    RunWithActiveDevice([&](D3D9DeviceEx& device) {
        executed = device.War3ExecuteSemanticShadowSceneForValidation(
            unitsOnly, executeNativeBackendValidation);
    });
    return executed;
}

} // namespace dxvk::war3
