/**
 * @file war3_shader_api.cpp
 * @brief War3MapReforge 外部 Shader API 实现
 * 
 * @version 1.1.0
 * @date 2024-12-21
 */

// 内部编译，不使用 dllimport/dllexport
#define WAR3_SHADER_API_INTERNAL

#include "war3_shader_api.h"
#include "war3/platform/war3_module_api.h"
#include "d3d9_war3_pipeline.h"
#include "d3d9_war3_scene.h"
#include "d3d9_war3_light.h"
#include "d3d9_war3_settings.h"
#include "war3/render/war3_render_state.h"
#include "war3/render/war3_render_objects.h"
#include "war3/war3.h"
#include "imgui.h"

#include <vector>
#include <array>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cmath>
#include <chrono>

namespace war3shader {

//=============================================================================
// 内部状态
//=============================================================================

namespace {
uint32_t NormalizeJHandle(uint32_t handle) {
    if (handle && handle < 0x100000u)
        return handle | 0x100000u;
    return handle;
}

// 回调注册表
struct CallbackEntry {
    uint32_t id;
    RenderEventID eventId;
    War3RenderEventFn callback;
    void* userData;
};

std::mutex g_callbackMutex;
std::vector<CallbackEntry> g_callbacks;
std::atomic<uint32_t> g_nextCallbackId{ 1 };

struct UiCallbackEntry {
    uint32_t id;
    War3UiDrawFn callback;
    void* userData;
};

std::mutex g_uiCallbackMutex;
std::vector<UiCallbackEntry> g_uiCallbacks;
std::atomic<uint32_t> g_nextUiCallbackId{ 1 };

enum class UiCommandType : uint8_t {
    Line,
    Rect,
    RectFilled,
    Circle,
    CircleFilled,
    Triangle,
    TriangleFilled,
    Polyline,
    BezierCubic,
    Text,
    Image,
    PushClip,
    PopClip,
};

struct UiCommand {
    UiCommandType type = UiCommandType::Line;
    UiLayer layer = UiLayer::Foreground;
    UiColor color = { 1.0f, 1.0f, 1.0f, 1.0f };
    UiVec2 p1 = { 0.0f, 0.0f };
    UiVec2 p2 = { 0.0f, 0.0f };
    UiVec2 p3 = { 0.0f, 0.0f };
    UiVec2 p4 = { 0.0f, 0.0f };
    UiVec2 uvMin = { 0.0f, 0.0f };
    UiVec2 uvMax = { 1.0f, 1.0f };
    float thickness = 1.0f;
    float rounding = 0.0f;
    float radius = 0.0f;
    int segments = 0;
    bool closed = false;
    bool clipIntersect = false;
    void* textureId = nullptr;
    std::vector<UiVec2> points;
    std::string text;
};

std::vector<UiCommand> g_uiCommands;
UiLayer g_uiCurrentLayer = UiLayer::Foreground;
bool g_uiFrameActive = false;
ImGuiContext* g_imguiContext = nullptr;

// 当前渲染上下文
RenderContext g_currentContext = {};
std::atomic<bool> g_contextValid{ false };

// 禁用开关
std::atomic<bool> g_disableNativeShadows{ false };
std::atomic<bool> g_disableNativeLighting{ false };
std::atomic<bool> g_disableNativePostProcess{ false };

// 帧缓冲缓存
FrameBuffer g_rawColorBuffer = {};
FrameBuffer g_rawDepthBuffer = {};

// 渲染上下文互斥（防止多线程读写冲突）
std::mutex g_contextMutex;

// Draw Call 缓存 (Ring Buffer to prevent CS thread race condition)
static const int kRingBufferSize = 3;
static const uint32_t kMaxDrawCallCount = 32768;
static std::atomic<uint32_t> g_ringBufferIndex{ 0 };

struct DrawCallBuffer {
    std::array<DrawCall, kMaxDrawCallCount> items;
    uint32_t count = 0;
};

DrawCallBuffer g_drawCallBuffers[kRingBufferSize];

// 光源缓存
std::vector<LightData> g_pointLightBuffers[kRingBufferSize];

// Vulkan 句柄缓存
void* g_vkDevice = nullptr;
void* g_vkPhysicalDevice = nullptr;
void* g_vkInstance = nullptr;
std::atomic<void*> g_vkCommandBuffer{ nullptr };

// 帧时间统计
std::chrono::steady_clock::time_point g_lastFrameTime;
double g_frameTimeCounter = 0.0;
bool g_timeInitialized = false;

dxvk::War3RenderSettings* GetMutableSettings() {
    return dxvk::war3::GetMutableSettings();
}

RenderStageId GetStageId(RenderEventID eventId) {
    switch (eventId) {
        case RenderEventID::FRAME_BEGIN:
        case RenderEventID::FRAME_END:
            return RenderStageId::Frame;
        case RenderEventID::WORLD_RENDER_BEGIN:
        case RenderEventID::WORLD_RENDER_END:
            return RenderStageId::World;
        case RenderEventID::SHADOW_PASS_BEGIN:
        case RenderEventID::SHADOW_PASS_END:
            return RenderStageId::Shadow;
        case RenderEventID::POST_PROCESS_BEGIN:
        case RenderEventID::POST_PROCESS_END:
            return RenderStageId::PostProcess;
        case RenderEventID::UI_RENDER_BEGIN:
        case RenderEventID::UI_RENDER_END:
            return RenderStageId::Ui;
        default:
            return RenderStageId::Unknown;
    }
}

uint32_t BuildStageCaps(RenderEventID eventId) {
    uint32_t caps = STAGE_CAP_NONE;

    if (g_rawColorBuffer.data)
        caps |= STAGE_CAP_COLOR;
    if (g_rawDepthBuffer.data)
        caps |= STAGE_CAP_DEPTH;
    if (g_currentContext.drawCallCount > 0)
        caps |= STAGE_CAP_DRAW_CALLS;
    if (g_currentContext.pointLightCount > 0 || g_currentContext.pointLights)
        caps |= STAGE_CAP_LIGHTS;

    if (eventId == RenderEventID::SHADOW_PASS_BEGIN || eventId == RenderEventID::SHADOW_PASS_END)
        caps |= STAGE_CAP_SHADOWMAP;
    if (eventId == RenderEventID::UI_RENDER_BEGIN || eventId == RenderEventID::UI_RENDER_END)
        caps |= STAGE_CAP_UI;

    return caps;
}

Matrix4x4 ToMatrix4x4(const dxvk::Matrix4& src) {
    Matrix4x4 dst = {};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            dst.m[i][j] = src[i][j];
        }
    }
    return dst;
}

VertexFormat MapVertexFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32G32_SFLOAT:
            return VertexFormat::FLOAT2;
        case VK_FORMAT_R32G32B32_SFLOAT:
            return VertexFormat::FLOAT3;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return VertexFormat::FLOAT4;
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_UINT:
            return VertexFormat::SHORT2;
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_UINT:
            return VertexFormat::SHORT4;
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
            return VertexFormat::UBYTE4;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
            return VertexFormat::UBYTE4N;
        default:
            return VertexFormat::UNKNOWN;
    }
}

PrimitiveType MapPrimitiveType(VkPrimitiveTopology topology) {
    switch (topology) {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
            return PrimitiveType::TRIANGLES;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
            return PrimitiveType::TRIANGLE_STRIP;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
            return PrimitiveType::TRIANGLE_FAN;
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
            return PrimitiveType::LINES;
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
            return PrimitiveType::LINE_STRIP;
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
            return PrimitiveType::POINTS;
        default:
            return PrimitiveType::TRIANGLES;
    }
}

const void* ResolveBufferHandle(const dxvk::DxvkResourceBufferInfo& info, const dxvk::Rc<dxvk::DxvkBuffer>& storage) {
    // 优先返回对象句柄，避免触碰可能已失效的映射指针
    if (storage != nullptr)
        return storage.ptr();
    return info.mapPtr;
}

ImVec2 ToImVec2(const UiVec2& v) {
    return ImVec2(v.x, v.y);
}

ImU32 ToImColor(const UiColor& c) {
    ImVec4 v(c.r, c.g, c.b, c.a);
    return ImGui::ColorConvertFloat4ToU32(v);
}

struct ImGuiContextScope {
    ImGuiContext* prev = nullptr;
    explicit ImGuiContextScope(ImGuiContext* ctx) {
        prev = ImGui::GetCurrentContext();
        if (ctx != nullptr && ctx != prev)
            ImGui::SetCurrentContext(ctx);
    }
    ~ImGuiContextScope() {
        if (prev != ImGui::GetCurrentContext())
            ImGui::SetCurrentContext(prev);
    }
};

} // 匿名命名空间

//=============================================================================
// API 实现
//=============================================================================

WAR3_SHADER_API uint32_t GetAPIVersion() {
    return (API_VERSION_MAJOR << 16) | (API_VERSION_MINOR << 8) | API_VERSION_PATCH;
}

WAR3_SHADER_API uint32_t RegisterRenderEvent(RenderEventID eventId, War3RenderEventFn callback, void* userData) {
    if (!callback || eventId >= RenderEventID::COUNT) {
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    
    CallbackEntry entry;
    entry.id = g_nextCallbackId++;
    entry.eventId = eventId;
    entry.callback = callback;
    entry.userData = userData;
    
    g_callbacks.push_back(entry);
    
    return entry.id;
}

WAR3_SHADER_API bool UnregisterRenderEvent(uint32_t callbackId) {
    if (callbackId == 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    
    for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
        if (it->id == callbackId) {
            g_callbacks.erase(it);
            return true;
        }
    }
    
    return false;
}

WAR3_SHADER_API bool DisableNativeShadows(bool disable) {
    bool prev = g_disableNativeShadows.exchange(disable);
    return prev;
}

WAR3_SHADER_API bool DisableNativeLighting(bool disable) {
    bool prev = g_disableNativeLighting.exchange(disable);
    return prev;
}

WAR3_SHADER_API bool DisableNativePostProcess(bool disable) {
    bool prev = g_disableNativePostProcess.exchange(disable);
    return prev;
}

WAR3_SHADER_API void AddOutlineHandle(uint32_t handle) {
    dxvk::War3RenderState::AddOutlineHandle(handle);
}

WAR3_SHADER_API bool AddOutlineLastRenderHandle() {
    uint32_t handle = dxvk::War3RenderState::GetLastRenderHandle();
    if (!handle)
        return false;
    dxvk::War3RenderState::AddOutlineHandle(handle);
    return true;
}

WAR3_SHADER_API bool AddOutlineHandleByIndex(uint32_t index) {
    dxvk::war3::render::RenderObjectInfo info;
    if (!dxvk::war3::render::RenderObjectRegistry::instance().getObjectByIndex(
            index, info))
        return false;
    if (info.jHandle == 0)
        return false;
    dxvk::War3RenderState::AddOutlineHandle(info.jHandle);
    return true;
}

WAR3_SHADER_API void RemoveOutlineHandle(uint32_t handle) {
    dxvk::War3RenderState::RemoveOutlineHandle(handle);
}

WAR3_SHADER_API void ClearOutlineHandles() {
    dxvk::War3RenderState::ClearOutlineHandles();
}

WAR3_SHADER_API uint32_t GetOutlineHandleCount() {
    return dxvk::War3RenderState::GetOutlineHandleCount();
}

WAR3_SHADER_API bool IsOutlineHandle(uint32_t handle) {
    return dxvk::War3RenderState::IsOutlineHandle(handle);
}

WAR3_SHADER_API bool SetOutlineAllObjectsEnabled(bool enabled) {
    dxvk::War3RenderState::SetOutlineDebugAllObjectsEnabled(enabled);
    return true;
}

WAR3_SHADER_API bool SetOutlineForceEnabled(bool enabled) {
    dxvk::War3RenderState::SetOutlineForceEnabled(enabled);
    return true;
}

WAR3_SHADER_API void AddBloomHandle(uint32_t handle, float boost) {
    dxvk::War3RenderState::AddBloomHandle(handle, boost);
}

WAR3_SHADER_API void RemoveBloomHandle(uint32_t handle) {
    dxvk::War3RenderState::RemoveBloomHandle(handle);
}

WAR3_SHADER_API void ClearBloomHandles() {
    dxvk::War3RenderState::ClearBloomHandles();
}

WAR3_SHADER_API float GetBloomBoost(uint32_t handle) {
    return dxvk::War3RenderState::GetBloomBoost(handle);
}

WAR3_SHADER_API uint32_t GetBloomHandleCount() {
    return dxvk::War3RenderState::GetBloomHandleCount();
}

WAR3_SHADER_API bool IsRenderHandleTracked(uint32_t handle) {
    const uint32_t normalized = NormalizeJHandle(handle);
    if (!normalized)
        return false;
    return dxvk::war3::render::RenderObjectRegistry::instance().findByHandle(normalized) != nullptr;
}

WAR3_SHADER_API uint32_t GetRenderObjectCount() {
    return static_cast<uint32_t>(
        dxvk::war3::render::RenderObjectRegistry::instance().getObjectCount());
}

WAR3_SHADER_API uint32_t GetLastRenderHandle() {
    return dxvk::War3RenderState::GetLastRenderHandle();
}

WAR3_SHADER_API uint32_t GetRenderObjectHandleByIndex(uint32_t index) {
    dxvk::war3::render::RenderObjectInfo info;
    if (!dxvk::war3::render::RenderObjectRegistry::instance().getObjectByIndex(
            index, info))
        return 0;
    return info.jHandle;
}

WAR3_SHADER_API uint32_t GetRenderObjectKindByIndex(uint32_t index) {
    dxvk::war3::render::RenderObjectInfo info;
    if (!dxvk::war3::render::RenderObjectRegistry::instance().getObjectByIndex(
            index, info))
        return 0;
    return static_cast<uint32_t>(info.kind);
}

WAR3_SHADER_API uint32_t GetRenderObjectRawcodeByIndex(uint32_t index) {
    dxvk::war3::render::RenderObjectInfo info;
    if (!dxvk::war3::render::RenderObjectRegistry::instance().getObjectByIndex(
            index, info))
        return 0;
    return info.rawcode;
}

WAR3_SHADER_API int32_t AddPointLight(
    float x, float y, float z,
    float range,
    float r, float g, float b,
    float intensity,
    float shadowIntensity) {
    const int32_t id = dxvk::War3LightManager::Instance().AddPointLight(
        x, y, z, range, r, g, b, intensity, shadowIntensity);
    if (id != 0) {
        if (auto* settings = GetMutableSettings()) {
            settings->shadows.pointLightsEnabled = true;
            if (shadowIntensity > 0.0f)
                settings->shadows.pointShadowEnabled = true;
        }
    }
    return id;
}

WAR3_SHADER_API bool UpdatePointLight(
    int32_t id,
    float x, float y, float z,
    float range,
    float r, float g, float b,
    float intensity) {
    return dxvk::War3LightManager::Instance().UpdatePointLight(
        id, x, y, z, range, r, g, b, intensity);
}

WAR3_SHADER_API bool UpdatePointLightEx(
    int32_t id,
    float x, float y, float z,
    float range,
    float r, float g, float b,
    float intensity,
    float shadowIntensity) {
    const bool ok = dxvk::War3LightManager::Instance().UpdatePointLightEx(
        id, x, y, z, range, r, g, b, intensity, shadowIntensity);
    if (ok && shadowIntensity > 0.0f) {
        if (auto* settings = GetMutableSettings()) {
            settings->shadows.pointLightsEnabled = true;
            settings->shadows.pointShadowEnabled = true;
        }
    }
    return ok;
}

WAR3_SHADER_API bool SetPointLightShadowIntensity(int32_t id,
                                                  float shadowIntensity) {
    const bool ok = dxvk::War3LightManager::Instance().SetPointLightShadowIntensity(
        id, shadowIntensity);
    if (ok && shadowIntensity > 0.0f) {
        if (auto* settings = GetMutableSettings()) {
            settings->shadows.pointLightsEnabled = true;
            settings->shadows.pointShadowEnabled = true;
        }
    }
    return ok;
}

WAR3_SHADER_API bool RemovePointLight(int32_t id) {
    return dxvk::War3LightManager::Instance().RemovePointLight(id);
}

WAR3_SHADER_API void ClearPointLights() {
    dxvk::War3LightManager::Instance().ClearLights();
}

WAR3_SHADER_API uint32_t GetPointLightCount() {
    return dxvk::War3LightManager::Instance().GetLightCount();
}

WAR3_SHADER_API bool SetLightingEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->sun.enabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetSunDirection(float x, float y, float z) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->sun.direction = dxvk::Vector4(x, y, z, 0.0f);
    return true;
}

WAR3_SHADER_API bool SetSunColor(float r, float g, float b) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->sun.color = dxvk::Vector4(r, g, b, 0.0f);
    return true;
}

WAR3_SHADER_API bool SetSunIntensity(float intensity) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->sun.intensity = intensity;
    return true;
}

WAR3_SHADER_API bool SetShadowEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.enabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetShadowStrength(float strength) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.strength = std::max(0.0f, std::min(1.0f, strength));
    return true;
}

WAR3_SHADER_API bool SetShadowBias(float bias) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.receiverBias = std::max(0.0f, bias);
    return true;
}

WAR3_SHADER_API bool SetShadowCasterBias(float constantBias, float slopeBias,
                                         float clamp) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.casterDepthBias = std::max(0.0f, constantBias);
    settings->shadows.casterSlopeBias = std::max(0.0f, slopeBias);
    settings->shadows.casterBiasClamp = std::max(0.0f, clamp);
    return true;
}

WAR3_SHADER_API bool SetShadowDepthRangeMargin(float margin) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.csm.depthRangeMargin = std::max(0.0f, margin);
    return true;
}

WAR3_SHADER_API bool SetShadowPcfRadius(float radius) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.pcfRadius = std::max(0.0f, radius);
    return true;
}

WAR3_SHADER_API bool SetShadowPcfKernel(uint32_t kernel) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    const uint32_t maxKernel =
        static_cast<uint32_t>(dxvk::War3ShadowPcfKernel::Poisson25);
    if (kernel > maxKernel)
        kernel = maxKernel;
    settings->shadows.pcfKernel =
        static_cast<dxvk::War3ShadowPcfKernel>(kernel);
    return true;
}

WAR3_SHADER_API bool SetShadowPcfRotate(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.pcfRotate = enabled;
    return true;
}

WAR3_SHADER_API bool SetShadowPcfRotateMode(uint32_t mode) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    if (mode > 2u)
        mode = 2u;
    settings->shadows.pcfRotateMode =
        static_cast<dxvk::War3ShadowPcfRotateMode>(mode);
    settings->shadows.pcfRotate = (mode != 0u);
    return true;
}

WAR3_SHADER_API bool SetShadowPcssSearchKernel(uint32_t kernel) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    if (kernel > 1u)
        kernel = 1u;
    settings->shadows.pcssSearchKernel =
        static_cast<dxvk::War3ShadowPcssSearchKernel>(kernel);
    return true;
}

WAR3_SHADER_API bool SetShadowCascadeBiasScale(float scale) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.cascadeBiasScale = std::clamp(scale, 0.0f, 1.0f);
    return true;
}

WAR3_SHADER_API bool SetShadowPcfCascadeRadiusScale(float scale) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.pcfCascadeRadiusScale = std::clamp(scale, 0.0f, 1.0f);
    return true;
}

WAR3_SHADER_API bool SetShadowAlphaHashed(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.alphaShadowHashed = enabled;
    return true;
}

WAR3_SHADER_API bool SetShadowAlphaUseMip(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.alphaShadowUseMip = enabled;
    return true;
}

WAR3_SHADER_API bool SetShadowAlphaMipLodBias(float bias) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.alphaShadowMipLodBias = std::clamp(bias, -4.0f, 4.0f);
    return true;
}

WAR3_SHADER_API bool SetShadowAlphaFarAlphaRefBias(float bias) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.alphaShadowFarAlphaRefBias = std::max(0.0f, bias);
    return true;
}

WAR3_SHADER_API bool SetNativeShadowMode(uint32_t mode) {
    dxvk::War3RenderState::SetNativeShadowMode(mode);
    return true;
}

WAR3_SHADER_API bool SetShadowLockSun(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.lockSun = enabled;
    return true;
}

WAR3_SHADER_API bool SetShadowLockSunTime(float time01) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.lockSunTime = std::clamp(time01, 0.0f, 1.0f);
    return true;
}

WAR3_SHADER_API bool SetShadowStableSnapWhenSunMoving(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.stableSnapWhenSunMoving = enabled;
    return true;
}

WAR3_SHADER_API bool SetShadowDebugMode(uint32_t mode) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    if (mode > 6u)
        mode = 6u;
    settings->shadows.debugMode = static_cast<dxvk::War3ShadowDebugMode>(mode);
    return true;
}

WAR3_SHADER_API bool SetShadowReceiverMode(uint32_t mode) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    if (mode > 2u)
        mode = 2u;
    settings->shadows.receiverMode =
        static_cast<dxvk::War3ShadowReceiverMode>(mode);
    return true;
}

WAR3_SHADER_API bool SetShadowNormalBiasScale(float scale) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.normalBiasScale = std::max(0.0f, scale);
    return true;
}

WAR3_SHADER_API bool SetShadowFilterMode(uint32_t mode) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    if (mode > 1u)
        mode = 1u;
    settings->shadows.filterMode =
        static_cast<dxvk::War3ShadowFilterMode>(mode);
    return true;
}

WAR3_SHADER_API bool SetShadowAltitudeMode(uint32_t mode) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    if (mode > 1u)
        mode = 1u;
    settings->shadows.altitudeMode =
        static_cast<dxvk::War3ShadowAltitudeMode>(mode);
    return true;
}

WAR3_SHADER_API bool SetShadowLengthScale(float scale) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.shadowLengthScale = std::max(0.1f, scale);
    return true;
}

WAR3_SHADER_API bool SetShadowMaxLengthScale(float scale) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.shadowMaxLengthScale = std::max(0.1f, scale);
    return true;
}

WAR3_SHADER_API bool SetShadowRimIntensity(float intensity) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.rimIntensity = std::max(0.0f, intensity);
    return true;
}

WAR3_SHADER_API bool SetShadowRimPower(float power) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.rimPower = std::max(0.1f, power);
    return true;
}

WAR3_SHADER_API bool SetPointLightsEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.pointLightsEnabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetPointShadowEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.pointShadowEnabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetPointShadowBias(float bias) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->shadows.pointShadowBias = std::max(0.0f, bias);
    return true;
}

WAR3_SHADER_API bool SetVolumetricLightEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.volumetricLight.enabled = enabled;
    if (enabled)
        settings->postFx.enabled = true;
    return true;
}

WAR3_SHADER_API bool SetVolumetricLightParams(
    float intensity, float density, float weight, float decay,
    uint32_t sampleCount) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    auto& volumetric = settings->postFx.volumetricLight;
    volumetric.intensity = std::max(0.0f, intensity);
    volumetric.density = std::max(0.0f, density);
    volumetric.weight = std::max(0.0f, weight);
    volumetric.decay = std::clamp(decay, 0.70f, 0.999f);
    volumetric.sampleCount = static_cast<int>(std::clamp<uint32_t>(
        sampleCount, 4u, 16u));
    return true;
}

WAR3_SHADER_API bool SetVolumetricLightFade(float fadeNear, float fadeFar,
                                            float maxRayDistance) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    auto& volumetric = settings->postFx.volumetricLight;
    volumetric.fadeNear = std::clamp(fadeNear, 0.0f, 0.95f);
    volumetric.fadeFar =
        std::clamp(fadeFar, volumetric.fadeNear + 0.01f, 1.0f);
    volumetric.maxRayDistance = std::max(0.05f, maxRayDistance);
    return true;
}

WAR3_SHADER_API bool SetVolumetricHeightFog(float baseHeight, float falloff,
                                            float strength) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    auto& volumetric = settings->postFx.volumetricLight;
    volumetric.heightFogBase = baseHeight;
    volumetric.heightFogFalloff = std::max(0.0f, falloff);
    volumetric.heightFogStrength = std::max(0.0f, strength);
    return true;
}

WAR3_SHADER_API bool SetVolumetricResolutionDivisor(uint32_t divisor) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.volumetricLight.resolutionDivisor =
        std::clamp<uint32_t>(divisor, 4u, 8u);
    return true;
}

WAR3_SHADER_API bool SetOutlineEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->occludedOutline.enabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetOutlineWidth(float widthPx) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->occludedOutline.widthPx = std::max(0.0f, widthPx);
    return true;
}

WAR3_SHADER_API bool SetOutlineColor(float r, float g, float b, float a) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->occludedOutline.colorR = r;
    settings->occludedOutline.colorG = g;
    settings->occludedOutline.colorB = b;
    settings->occludedOutline.colorA = a;
    return true;
}

WAR3_SHADER_API bool SetOutlineMode(uint32_t mode) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    if (mode > 1u)
        mode = 1u;
    settings->occludedOutline.mode = static_cast<dxvk::War3OutlineMode>(mode);
    return true;
}

WAR3_SHADER_API bool SetOutlineVisibility(bool showVisible, bool showOccluded) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->occludedOutline.showVisible = showVisible;
    settings->occludedOutline.showOccluded = showOccluded;
    return true;
}

WAR3_SHADER_API bool SetOutlineScreenSpace(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->occludedOutline.useScreenSpace = enabled;
    return true;
}

WAR3_SHADER_API bool SetPostFxEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.enabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetExposure(float exposure) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.exposure = exposure;
    settings->dayNight.affectExposure = false;
    return true;
}

WAR3_SHADER_API bool SetBloomEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.bloom.enabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetBloomParams(float threshold, float softKnee, float intensity) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.bloom.threshold = threshold;
    settings->postFx.bloom.softKnee = softKnee;
    settings->postFx.bloom.intensity = intensity;
    return true;
}

WAR3_SHADER_API bool SetAcesEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.bloom.acesToneMap = enabled;
    return true;
}

WAR3_SHADER_API bool SetSsaoEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.ssao.enabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetSsaoParams(float radiusPx, float strength, float bias, float power) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.ssao.radiusPx = radiusPx;
    settings->postFx.ssao.strength = strength;
    settings->postFx.ssao.bias = bias;
    settings->postFx.ssao.power = power;
    return true;
}

WAR3_SHADER_API bool SetAaMode(uint32_t mode) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    if (mode > 5u)
        mode = 5u;
    settings->postFx.aa.mode = static_cast<dxvk::War3AAMode>(mode);
    return true;
}

WAR3_SHADER_API bool SetFxaaParams(float subpix, float edgeThreshold, float edgeThresholdMin) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.aa.fxaaQualitySubpix = subpix;
    settings->postFx.aa.fxaaQualityEdgeThreshold = edgeThreshold;
    settings->postFx.aa.fxaaQualityEdgeThresholdMin = edgeThresholdMin;
    return true;
}

WAR3_SHADER_API bool SetSmaaParams(float threshold, int32_t search, int32_t diagSearch) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->postFx.aa.smaaThreshold = threshold;
    settings->postFx.aa.smaaMaxSearchSteps = search;
    settings->postFx.aa.smaaMaxSearchStepsDiag = diagSearch;
    return true;
}

WAR3_SHADER_API bool SetDayNightEnabled(bool enabled) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->dayNight.enabled = enabled;
    return true;
}

WAR3_SHADER_API bool SetDayNightMinFactor(float minFactor) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->dayNight.transitionMinFactor = std::max(0.0f, std::min(1.0f, minFactor));
    return true;
}

WAR3_SHADER_API bool SetDayNightAmbient(
    float dayR, float dayG, float dayB,
    float nightR, float nightG, float nightB) {
    auto* settings = GetMutableSettings();
    if (!settings)
        return false;
    settings->dayNight.dayAmbient = dxvk::Vector4(dayR, dayG, dayB, 1.0f);
    settings->dayNight.nightAmbient = dxvk::Vector4(nightR, nightG, nightB, 1.0f);
    return true;
}

WAR3_SHADER_API const RenderContext* GetCurrentRenderContext() {
    if (!g_contextValid)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_contextMutex);
    return &g_currentContext;
}

WAR3_SHADER_API const FrameBuffer* GetRawColorBuffer() {
    if (!g_contextValid)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_contextMutex);
    return &g_rawColorBuffer;
}

WAR3_SHADER_API const FrameBuffer* GetRawDepthBuffer() {
    if (!g_contextValid)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_contextMutex);
    return &g_rawDepthBuffer;
}

WAR3_SHADER_API const DrawCall* GetDrawCalls(uint32_t* outCount) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    // [Plan A] Read the last published (fully written) frame index
    uint32_t ringIndex = g_ringBufferIndex.load(std::memory_order_acquire);
    auto& calls = g_drawCallBuffers[ringIndex];
    if (outCount) {
        *outCount = calls.count;
    }
    return calls.count == 0 ? nullptr : calls.items.data();
}

WAR3_SHADER_API const CameraData* GetCameraData() {
    return g_contextValid ? &g_currentContext.camera : nullptr;
}

WAR3_SHADER_API const LightData* GetSunLight() {
    return g_contextValid ? &g_currentContext.sunLight : nullptr;
}

WAR3_SHADER_API const LightData* GetPointLights(uint32_t* outCount) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    // [Plan A] Read the last published (fully written) frame index
    uint32_t ringIndex = g_ringBufferIndex.load(std::memory_order_acquire);
    auto& lights = g_pointLightBuffers[ringIndex];
    if (outCount) {
        *outCount = static_cast<uint32_t>(lights.size());
    }
    return lights.empty() ? nullptr : lights.data();
}

WAR3_SHADER_API float GetGameTime() {
    return dxvk::War3RenderState::GetGameTime();
}

WAR3_SHADER_API float GetFrameTime() {
    return g_currentContext.frameTime;
}

WAR3_SHADER_API uint32_t RegisterUiDrawCallback(War3UiDrawFn callback, void* userData) {
    if (!callback)
        return 0;
    std::lock_guard<std::mutex> lock(g_uiCallbackMutex);
    UiCallbackEntry entry;
    entry.id = g_nextUiCallbackId++;
    entry.callback = callback;
    entry.userData = userData;
    g_uiCallbacks.push_back(entry);
    return entry.id;
}

WAR3_SHADER_API bool UnregisterUiDrawCallback(uint32_t callbackId) {
    if (callbackId == 0)
        return false;
    std::lock_guard<std::mutex> lock(g_uiCallbackMutex);
    for (auto it = g_uiCallbacks.begin(); it != g_uiCallbacks.end(); ++it) {
        if (it->id == callbackId) {
            g_uiCallbacks.erase(it);
            return true;
        }
    }
    return false;
}

WAR3_SHADER_API UiVec2 UiGetDisplaySize() {
    if (!g_imguiContext)
        return { 0.0f, 0.0f };
    ImGuiContextScope scope(g_imguiContext);
    ImVec2 size = ImGui::GetIO().DisplaySize;
    return { size.x, size.y };
}

WAR3_SHADER_API void UiSetLayer(UiLayer layer) {
    g_uiCurrentLayer = layer;
}

WAR3_SHADER_API void UiDrawLine(UiVec2 p1, UiVec2 p2, UiColor color, float thickness) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::Line;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = p1;
    cmd.p2 = p2;
    cmd.thickness = thickness;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiDrawRect(UiRect rect, UiColor color, float rounding, float thickness) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::Rect;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = rect.min;
    cmd.p2 = rect.max;
    cmd.rounding = rounding;
    cmd.thickness = thickness;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiFillRect(UiRect rect, UiColor color, float rounding) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::RectFilled;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = rect.min;
    cmd.p2 = rect.max;
    cmd.rounding = rounding;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiDrawCircle(UiVec2 center, float radius, UiColor color, int segments, float thickness) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::Circle;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = center;
    cmd.radius = radius;
    cmd.segments = segments;
    cmd.thickness = thickness;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiFillCircle(UiVec2 center, float radius, UiColor color, int segments) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::CircleFilled;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = center;
    cmd.radius = radius;
    cmd.segments = segments;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiDrawTriangle(UiVec2 p1, UiVec2 p2, UiVec2 p3, UiColor color, float thickness) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::Triangle;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = p1;
    cmd.p2 = p2;
    cmd.p3 = p3;
    cmd.thickness = thickness;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiFillTriangle(UiVec2 p1, UiVec2 p2, UiVec2 p3, UiColor color) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::TriangleFilled;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = p1;
    cmd.p2 = p2;
    cmd.p3 = p3;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiDrawPolyline(const UiVec2* points, uint32_t count, UiColor color, bool closed, float thickness) {
    if (!g_uiFrameActive || !points || count == 0)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::Polyline;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.closed = closed;
    cmd.thickness = thickness;
    cmd.points.assign(points, points + count);
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiDrawBezierCubic(UiVec2 p1, UiVec2 p2, UiVec2 p3, UiVec2 p4, UiColor color, float thickness, int segments) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::BezierCubic;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = p1;
    cmd.p2 = p2;
    cmd.p3 = p3;
    cmd.p4 = p4;
    cmd.thickness = thickness;
    cmd.segments = segments;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiDrawText(UiVec2 pos, UiColor color, const char* text) {
    if (!g_uiFrameActive || !text)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::Text;
    cmd.layer = g_uiCurrentLayer;
    cmd.color = color;
    cmd.p1 = pos;
    cmd.text = text;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiDrawImage(void* textureId, UiVec2 min, UiVec2 max, UiVec2 uvMin, UiVec2 uvMax, UiColor tint) {
    if (!g_uiFrameActive || !textureId)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::Image;
    cmd.layer = g_uiCurrentLayer;
    cmd.textureId = textureId;
    cmd.color = tint;
    cmd.p1 = min;
    cmd.p2 = max;
    cmd.uvMin = uvMin;
    cmd.uvMax = uvMax;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiPushClipRect(UiVec2 min, UiVec2 max, bool intersect) {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::PushClip;
    cmd.layer = g_uiCurrentLayer;
    cmd.p1 = min;
    cmd.p2 = max;
    cmd.clipIntersect = intersect;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void UiPopClipRect() {
    if (!g_uiFrameActive)
        return;
    UiCommand cmd;
    cmd.type = UiCommandType::PopClip;
    cmd.layer = g_uiCurrentLayer;
    g_uiCommands.push_back(std::move(cmd));
}

WAR3_SHADER_API void* GetImGuiContext() {
    return g_imguiContext;
}

WAR3_SHADER_API void* GetVulkanDevice() {
    return g_vkDevice;
}

WAR3_SHADER_API void* GetVulkanPhysicalDevice() {
    return g_vkPhysicalDevice;
}

WAR3_SHADER_API void* GetVulkanInstance() {
    return g_vkInstance;
}

WAR3_SHADER_API void* GetVulkanCommandBuffer() {
    return g_vkCommandBuffer.load();
}

//=============================================================================
// 内部接口（供渲染器调用）
//=============================================================================

namespace internal {

/**
 * @brief 检查是否禁用内置阴影
 */
bool IsNativeShadowsDisabled() {
    return g_disableNativeShadows.load();
}

/**
 * @brief 检查是否禁用内置光照
 */
bool IsNativeLightingDisabled() {
    return g_disableNativeLighting.load();
}

/**
 * @brief 检查是否禁用内置后处理
 */
bool IsNativePostProcessDisabled() {
    return g_disableNativePostProcess.load();
}

/**
 * @brief 是否存在任何渲染事件监听者
 */
bool HasAnyRenderListeners() {
    if (war3module::HasModules())
        return true;
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    return !g_callbacks.empty();
}

/**
 * @brief 分发渲染事件
 * 
 * @note 避免死锁：先复制回调列表，解锁后再调用
 */
void DispatchRenderEvent(RenderEventID eventId) {
    g_currentContext.eventId = eventId;
    g_currentContext.stageId = GetStageId(eventId);
    g_currentContext.stageCaps = BuildStageCaps(eventId);

    // 复制回调列表，避免死锁（回调中可能调用 Register/Unregister）
    std::vector<CallbackEntry> callbacksCopy;
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        callbacksCopy = g_callbacks;
    }
    
    // 解锁后调用回调
    for (const auto& entry : callbacksCopy) {
        if (entry.eventId == eventId && entry.callback) {
            try {
                entry.callback(eventId, &g_currentContext, entry.userData);
            } catch (...) {
                // 防止回调异常影响渲染
                // TODO: 记录错误日志
            }
        }
    }

    // 模块系统分发（内置/外部模块统一入口）
    war3module::DispatchRenderEvent(eventId, &g_currentContext);
}

// 崩溃规避：避免触碰可能失效的地址
static bool IsLikelyValidHandle(const void* ptr) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    if (value == 0) return false;
    if (value < 0x10000) return false; // 避免空页
    if (value == 0xFFFFFFFFu) return false;
    return true;
}

/**
 * @brief 更新渲染上下文
 */
void UpdateRenderContext(const dxvk::War3PipelineInput& input) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    g_contextValid = false;
    g_currentContext.eventId = RenderEventID::FRAME_BEGIN;
    g_currentContext.stageId = RenderStageId::Frame;
    g_currentContext.stageCaps = STAGE_CAP_NONE;
    
    // 更新时间
    g_currentContext.gameTime = dxvk::War3RenderState::GetGameTime();
    
    // 更新相机
    if (input.scene.worldCamera.valid) {
        auto& cam = g_currentContext.camera;

        const dxvk::Matrix4 view = input.scene.worldCamera.view;
        const dxvk::Matrix4 proj = input.scene.worldCamera.proj;
        const dxvk::Matrix4 viewProj = input.scene.worldCamera.viewProj;
        const dxvk::Matrix4 invView = dxvk::inverse(view);
        const dxvk::Matrix4 invProj = dxvk::inverse(proj);
        const dxvk::Matrix4 invViewProj = input.scene.worldCamera.invViewProj;

        cam.view = ToMatrix4x4(view);
        cam.projection = ToMatrix4x4(proj);
        cam.viewProjection = ToMatrix4x4(viewProj);
        cam.invView = ToMatrix4x4(invView);
        cam.invProjection = ToMatrix4x4(invProj);
        cam.invViewProjection = ToMatrix4x4(invViewProj);

        cam.position = { invView[3].x, invView[3].y, invView[3].z };
        cam.right = { invView[0].x, invView[0].y, invView[0].z };
        cam.up = { invView[1].x, invView[1].y, invView[1].z };
        cam.forward = { invView[2].x, invView[2].y, invView[2].z };

        const float m00 = proj[0][0];
        const float m11 = proj[1][1];
        if (std::abs(m00) > 1e-6f && std::abs(m11) > 1e-6f) {
            cam.aspectRatio = m11 / m00;
            cam.fov = 2.0f * std::atan(1.0f / m11);
        }

        const float m22 = proj[2][2];
        const float m32 = proj[3][2];
        if (std::abs(m22) > 1e-6f)
            cam.nearPlane = -m32 / m22;
        if (std::abs(1.0f - m22) > 1e-6f)
            cam.farPlane = m32 / (1.0f - m22);

        // 视口
        cam.viewport.x = input.scene.worldCamera.viewport.X;
        cam.viewport.y = input.scene.worldCamera.viewport.Y;
        cam.viewport.width = input.scene.worldCamera.viewport.Width;
        cam.viewport.height = input.scene.worldCamera.viewport.Height;
        cam.viewport.minDepth = input.scene.worldCamera.viewport.MinZ;
        cam.viewport.maxDepth = input.scene.worldCamera.viewport.MaxZ;
    }

    if (input.colorView) {
        auto& cam = g_currentContext.camera;
        if (!input.scene.worldCamera.valid || cam.viewport.width == 0 || cam.viewport.height == 0) {
            const auto extent = input.colorView->image()->info().extent;
            cam.viewport.x = 0;
            cam.viewport.y = 0;
            cam.viewport.width = static_cast<uint32_t>(extent.width);
            cam.viewport.height = static_cast<uint32_t>(extent.height);
            cam.viewport.minDepth = 0.0f;
            cam.viewport.maxDepth = 1.0f;
            if (extent.width > 0 && extent.height > 0) {
                cam.aspectRatio = static_cast<float>(extent.width) / static_cast<float>(extent.height);
            }
        }
    }
    
    // 更新光源：使用帧快照，避免每帧 vector 拷贝 + 堆分配。
    uint32_t ringIndex = input.frameIndex % kRingBufferSize;
    auto& currentPointLights = g_pointLightBuffers[ringIndex];
    currentPointLights.clear();
    if (dxvk::War3LightManager::Instance().HasActiveLights()) {
      dxvk::Vector4 cameraPos(0.0f, 0.0f, 0.0f, 1.0f);
      const auto snap = dxvk::War3LightManager::Instance().GetFrameSnapshot(
          input.frameSerial, cameraPos);
      currentPointLights.reserve(snap.count);
      for (uint32_t i = 0; i < snap.count; ++i) {
        const auto& light = snap.lights[i];
        LightData ld = {};
        ld.type = LightType::POINT;
        ld.position = { light.position.x, light.position.y, light.position.z };
        ld.color = { light.color.x, light.color.y, light.color.z, light.color.w };
        ld.range = light.position.w;
        ld.intensity = light.color.w;
        ld.flags = LIGHT_FLAG_ACTIVE;
        currentPointLights.push_back(ld);
      }
    }
    g_currentContext.pointLightCount =
        static_cast<uint32_t>(currentPointLights.size());
    g_currentContext.pointLights =
        currentPointLights.empty() ? nullptr : currentPointLights.data();

    
    
    // 更新 Draw Calls
    auto& currentDrawCalls = g_drawCallBuffers[ringIndex];
    currentDrawCalls.count = 0;
    
    // 安全阈值：数量异常时很可能是脏数据或越界
    if (input.scene.shadowCasters.size() > 100000) {
        return;
    }
    const uint32_t maxCalls = static_cast<uint32_t>(
        std::min<size_t>(input.scene.shadowCasters.size(), kMaxDrawCallCount));

    // 遍历投影体，使用更严格的校验以防崩溃
    for (const auto& caster : input.scene.shadowCasters) {
        DrawCall dc = {};
        // 顶点数据校验
        if (!caster.positionStorage || caster.positionStride == 0 || caster.positionFormat == VK_FORMAT_UNDEFINED) {
            continue;
        }
        dc.vertexData = ResolveBufferHandle(caster.positionInfo, caster.positionStorage);
        if (!IsLikelyValidHandle(dc.vertexData)) {
            continue; // 无效顶点句柄直接跳过
        }
        
        dc.vertexCount = caster.vertexCount;
        dc.vertexStride = caster.positionStride;
        dc.positionFormat = MapVertexFormat(caster.positionFormat);
        dc.positionOffset = caster.positionOffset;
        
        // 索引数据校验（若存在）
        if (caster.indexed) {
             if (!caster.indexStorage) {
                 continue;
             }
             const void* idxData = ResolveBufferHandle(caster.indexInfo, caster.indexStorage);
             if (IsLikelyValidHandle(idxData)) {
                 dc.indexData = idxData;
                 dc.indexCount = caster.indexCount;
             } else {
                 dc.indexData = nullptr;
                 dc.indexCount = 0; // 索引数据异常，直接跳过
                 continue; 
             }
        } else {
            dc.indexData = nullptr;
            dc.indexCount = 0;
        }

        dc.primitiveType = MapPrimitiveType(caster.topology);

        // 填充世界矩阵
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                dc.worldMatrix[i][j] = caster.worldMatrix[i][j];
            }
        }

        // 填充额外字段
        dc.objectId = caster.batchHandle;
        uint32_t layerMask = DRAWCALL_LAYER_NONE;
        switch (caster.category) {
            case dxvk::War3RenderState::StageCategory::Terrain:
                layerMask |= DRAWCALL_LAYER_TERRAIN | DRAWCALL_LAYER_WORLD;
                break;
            case dxvk::War3RenderState::StageCategory::WorldObject:
                layerMask |= DRAWCALL_LAYER_WORLD;
                break;
            case dxvk::War3RenderState::StageCategory::Effect:
                layerMask |= DRAWCALL_LAYER_EFFECT | DRAWCALL_LAYER_WORLD;
                break;
            case dxvk::War3RenderState::StageCategory::PostProcess:
                layerMask |= DRAWCALL_LAYER_POSTPROCESS;
                break;
            case dxvk::War3RenderState::StageCategory::UI:
                layerMask |= DRAWCALL_LAYER_UI;
                break;
            default:
                break;
        }

        switch (caster.batchTag) {
            case dxvk::War3BatchTag::Decorations:
                layerMask |= DRAWCALL_LAYER_DOODAD | DRAWCALL_LAYER_WORLD;
                break;
            case dxvk::War3BatchTag::SelectionOverlay:
                layerMask |= DRAWCALL_LAYER_EFFECT | DRAWCALL_LAYER_TRANSPARENT;
                break;
            case dxvk::War3BatchTag::UI:
                layerMask |= DRAWCALL_LAYER_UI;
                break;
            default:
                break;
        }

        switch (caster.objectKind) {
            case 1: // Unit
                layerMask |= DRAWCALL_LAYER_UNIT | DRAWCALL_LAYER_WORLD;
                break;
            case 2: // Building
                layerMask |= DRAWCALL_LAYER_BUILDING | DRAWCALL_LAYER_WORLD;
                break;
            case 3: // Destructible
                layerMask |= DRAWCALL_LAYER_DESTRUCTIBLE | DRAWCALL_LAYER_WORLD;
                break;
            case 4: // Item
                layerMask |= DRAWCALL_LAYER_ITEM | DRAWCALL_LAYER_WORLD;
                break;
            case 5: // Effect
                layerMask |= DRAWCALL_LAYER_EFFECT | DRAWCALL_LAYER_TRANSPARENT;
                break;
            default:
                break;
        }

        if (caster.alphaBlendEnabled)
            layerMask |= DRAWCALL_LAYER_TRANSPARENT;
        if (caster.additiveBlend)
            layerMask |= DRAWCALL_LAYER_EFFECT | DRAWCALL_LAYER_TRANSPARENT;

        if (layerMask == DRAWCALL_LAYER_NONE)
            layerMask = DRAWCALL_LAYER_WORLD;

        dc.layerMask = layerMask;
        dc.flags = DRAWCALL_FLAG_NONE;
        if (caster.depthTestEnabled) dc.flags |= DRAWCALL_FLAG_DEPTH_TEST;
        if (caster.depthWriteEnabled) dc.flags |= DRAWCALL_FLAG_DEPTH_WRITE;
        if (caster.alphaTestEnabled) dc.flags |= DRAWCALL_FLAG_ALPHA_TEST;
        if (caster.alphaBlendEnabled) dc.flags |= DRAWCALL_FLAG_ALPHA_BLEND;
        if (caster.additiveBlend) dc.flags |= DRAWCALL_FLAG_ADDITIVE_BLEND;
        if (caster.indexCount > 0 && caster.indexType == VK_INDEX_TYPE_UINT32) dc.flags |= DRAWCALL_FLAG_32BIT_INDEX;
        if (dxvk::War3RenderState::GetBloomBoost(caster.batchHandle) > 0.0f)
            dc.flags |= DRAWCALL_FLAG_BLOOM_HINT;
        dc.alphaRef = caster.alphaRef;

        // 避免在此处解引用纹理对象，降低跨线程风险（仅暴露句柄）
        const void* texHandle = caster.diffuseTexture.ptr();
        if (IsLikelyValidHandle(texHandle)) {
            dc.textureHandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texHandle));
            dc.textureWidth = 0;
            dc.textureHeight = 0;
        } else {
            dc.textureHandle = 0;
            dc.textureWidth = 0;
            dc.textureHeight = 0;
        }
        
        currentDrawCalls.items[currentDrawCalls.count++] = dc;
        if (currentDrawCalls.count >= maxCalls)
            break;
    }
    g_currentContext.drawCallCount = currentDrawCalls.count;
    g_currentContext.drawCalls = currentDrawCalls.count == 0 ? nullptr : currentDrawCalls.items.data();
    
    // 更新禁用状态
    g_currentContext.nativeShadowsEnabled = !g_disableNativeShadows.load();
    g_currentContext.nativeLightingEnabled = !g_disableNativeLighting.load();
    g_currentContext.nativePostProcessEnabled = !g_disableNativePostProcess.load();
    
    g_contextValid = true;

    // 发布已完成帧，供读侧安全读取
    g_ringBufferIndex.store(ringIndex, std::memory_order_release);
}

/**
 * @brief 更新帧缓冲信息
 */
void UpdateFrameBuffers(
    const void* colorData, uint32_t colorWidth, uint32_t colorHeight, TextureFormat colorFormat,
    const void* depthData, uint32_t depthWidth, uint32_t depthHeight, TextureFormat depthFormat,
    void* colorImage, void* colorView, uint32_t colorLayout,
    void* depthImage, void* depthView, uint32_t depthLayout)
{
    std::lock_guard<std::mutex> lock(g_contextMutex);
    g_rawColorBuffer.data = colorData;
    g_rawColorBuffer.width = colorWidth;
    g_rawColorBuffer.height = colorHeight;
    g_rawColorBuffer.format = colorFormat;
    g_rawColorBuffer.mipLevels = 1;
    g_rawColorBuffer.arrayLayers = 1;
    g_rawColorBuffer.vkImage = colorImage;
    g_rawColorBuffer.vkImageView = colorView;
    g_rawColorBuffer.vkLayout = colorLayout;
    
    g_rawDepthBuffer.data = depthData;
    g_rawDepthBuffer.width = depthWidth;
    g_rawDepthBuffer.height = depthHeight;
    g_rawDepthBuffer.format = depthFormat;
    g_rawDepthBuffer.mipLevels = 1;
    g_rawDepthBuffer.arrayLayers = 1;
    g_rawDepthBuffer.vkImage = depthImage;
    g_rawDepthBuffer.vkImageView = depthView;
    g_rawDepthBuffer.vkLayout = depthLayout;

    g_currentContext.colorBuffer = g_rawColorBuffer;
    g_currentContext.depthBuffer = g_rawDepthBuffer;
}

void SetVulkanHandles(void* instance, void* physicalDevice, void* device) {
    g_vkInstance = instance;
    g_vkPhysicalDevice = physicalDevice;
    g_vkDevice = device;
}

void SetVulkanCommandBuffer(void* commandBuffer) {
    g_vkCommandBuffer.store(commandBuffer);
}

void BeginFrame() {
    const uint64_t nextFrame = g_currentContext.frameIndex + 1;
    g_currentContext = {};
    g_currentContext.frameIndex = nextFrame;
    // g_ringBufferIndex = (g_ringBufferIndex + 1) % kRingBufferSize; // REMOVED: Managed by UpdateRenderContext publish
    g_currentContext.eventId = RenderEventID::FRAME_BEGIN;
    g_currentContext.stageId = RenderStageId::Frame;
    g_currentContext.stageCaps = STAGE_CAP_NONE;
    g_currentContext.gameTime = dxvk::War3RenderState::GetGameTime();
    g_vkCommandBuffer.store(nullptr);
    auto now = std::chrono::steady_clock::now();
    if (!g_timeInitialized) {
        g_timeInitialized = true;
        g_lastFrameTime = now;
        g_currentContext.frameTime = 0.0f;
    } else {
        const float dt = std::chrono::duration<float>(now - g_lastFrameTime).count();
        g_lastFrameTime = now;
        g_frameTimeCounter += dt;
        g_currentContext.frameTime = dt;
    }
    g_currentContext.frameTimeCounter = static_cast<float>(g_frameTimeCounter);
    g_currentContext.nativeShadowsEnabled = !g_disableNativeShadows.load();
    g_currentContext.nativeLightingEnabled = !g_disableNativeLighting.load();
    g_currentContext.nativePostProcessEnabled = !g_disableNativePostProcess.load();
    g_rawColorBuffer = {};
    g_rawDepthBuffer = {};
    
    // Advance Ring Buffer
    // g_ringBufferIndex = (g_ringBufferIndex + 1) % kRingBufferSize; // REMOVED: Already incremented at start of function
    
    // Clear current buffer (though UpdateRenderContext will clear it again, good for safety)
    // g_drawCallBuffers[g_ringBufferIndex].clear(); // REMOVED: Managed by CS Thread
    // g_pointLightBuffers[g_ringBufferIndex].clear(); // REMOVED: Managed by CS Thread
    
    g_currentContext.drawCallCount = 0;
    g_currentContext.drawCalls = nullptr;
    g_currentContext.pointLightCount = 0;
    g_currentContext.pointLights = nullptr;
    g_contextValid = true;
}

void SetImGuiContext(void* context) {
    g_imguiContext = reinterpret_cast<ImGuiContext*>(context);
}

void BeginUiFrame() {
    g_uiFrameActive = true;
    g_uiCommands.clear();
    g_uiCurrentLayer = UiLayer::Foreground;
}

void DispatchUiCallbacks() {
    if (!g_uiFrameActive)
        return;
    std::vector<UiCallbackEntry> callbacksCopy;
    {
        std::lock_guard<std::mutex> lock(g_uiCallbackMutex);
        callbacksCopy = g_uiCallbacks;
    }
    const RenderContext* ctx = g_contextValid ? &g_currentContext : nullptr;
    for (const auto& entry : callbacksCopy) {
        if (!entry.callback)
            continue;
        try {
            entry.callback(ctx, entry.userData);
        } catch (...) {
            // 防止回调异常影响渲染
        }
    }
}

void FlushUiCommands() {
    if (!g_uiFrameActive || !g_imguiContext)
        return;
    ImGuiContextScope scope(g_imguiContext);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    for (const auto& cmd : g_uiCommands) {
        ImDrawList* list = (cmd.layer == UiLayer::Background) ? bg : fg;
        ImU32 col = ToImColor(cmd.color);
        switch (cmd.type) {
            case UiCommandType::Line:
                list->AddLine(ToImVec2(cmd.p1), ToImVec2(cmd.p2), col, cmd.thickness);
                break;
            case UiCommandType::Rect:
                list->AddRect(ToImVec2(cmd.p1), ToImVec2(cmd.p2), col, cmd.rounding, 0, cmd.thickness);
                break;
            case UiCommandType::RectFilled:
                list->AddRectFilled(ToImVec2(cmd.p1), ToImVec2(cmd.p2), col, cmd.rounding, 0);
                break;
            case UiCommandType::Circle:
                list->AddCircle(ToImVec2(cmd.p1), cmd.radius, col, cmd.segments, cmd.thickness);
                break;
            case UiCommandType::CircleFilled:
                list->AddCircleFilled(ToImVec2(cmd.p1), cmd.radius, col, cmd.segments);
                break;
            case UiCommandType::Triangle:
                list->AddTriangle(ToImVec2(cmd.p1), ToImVec2(cmd.p2), ToImVec2(cmd.p3), col, cmd.thickness);
                break;
            case UiCommandType::TriangleFilled:
                list->AddTriangleFilled(ToImVec2(cmd.p1), ToImVec2(cmd.p2), ToImVec2(cmd.p3), col);
                break;
            case UiCommandType::Polyline: {
                if (cmd.points.size() < 2)
                    break;
                std::vector<ImVec2> points;
                points.reserve(cmd.points.size());
                for (const auto& p : cmd.points)
                    points.push_back(ToImVec2(p));
                list->AddPolyline(points.data(), static_cast<int>(points.size()), col, cmd.closed, cmd.thickness);
                break;
            }
            case UiCommandType::BezierCubic:
                list->AddBezierCubic(ToImVec2(cmd.p1), ToImVec2(cmd.p2), ToImVec2(cmd.p3), ToImVec2(cmd.p4),
                                     col, cmd.thickness, cmd.segments);
                break;
            case UiCommandType::Text:
                list->AddText(ToImVec2(cmd.p1), col, cmd.text.c_str());
                break;
            case UiCommandType::Image:
                list->AddImage(reinterpret_cast<ImTextureID>(cmd.textureId), ToImVec2(cmd.p1), ToImVec2(cmd.p2),
                               ToImVec2(cmd.uvMin), ToImVec2(cmd.uvMax), col);
                break;
            case UiCommandType::PushClip:
                list->PushClipRect(ToImVec2(cmd.p1), ToImVec2(cmd.p2), cmd.clipIntersect);
                break;
            case UiCommandType::PopClip:
                list->PopClipRect();
                break;
            default:
                break;
        }
    }
    g_uiCommands.clear();
}

void EndUiFrame() {
    g_uiFrameActive = false;
    g_uiCommands.clear();
}

} // namespace internal

} // namespace war3shader
