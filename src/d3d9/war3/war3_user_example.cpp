// war3_user_example.cpp - 外部用户示例模块（内置）

#define WAR3_SHADER_API_INTERNAL
#include "../war3_sdk.h"
#include "../../util/util_env.h"
#include "../../util/log/log.h"

#include <cmath>

namespace {

struct UserExampleState {
    bool enabled = false;
    uint32_t uiCallbackId = 0;
    float timeAccum = 0.0f;
};

UserExampleState g_state;

bool ShouldEnable() {
    const std::string value = dxvk::env::getEnvVar("DXVK_WAR3_USER_EXAMPLE");
    return value == "1" || value == "true" || value == "TRUE";
}

void OnUiDraw(const war3shader::RenderContext* context, void* userData) {
    (void)context;
    (void)userData;
    if (!g_state.enabled)
        return;

    war3shader::UiSetLayer(war3shader::UiLayer::Foreground);
    war3shader::UiDrawText({ 20.0f, 20.0f }, { 1.0f, 0.9f, 0.2f, 1.0f }, "War3 User Example");
}

void OnLoad(void* userData) {
    (void)userData;
    g_state.enabled = ShouldEnable();
    if (!g_state.enabled) {
        dxvk::Logger::info("UserExample: disabled (set DXVK_WAR3_USER_EXAMPLE=1 to enable)");
        return;
    }

    const char* packPath = "shaderpacks/vulkan_pack_template";
    const auto err = war3shader::LoadShaderPack(packPath);
    if (err != war3shader::ShaderPackError::OK) {
        dxvk::Logger::warn(dxvk::str::format("UserExample: ShaderPack load failed: ",
                                             war3shader::GetErrorString(err)));
    } else {
        war3shader::EnableShaderPack(true);
        war3shader::SetParamVec4(war3shader::ParamSlot::PARAMS0, 0.35f, 0.4f, 0.0f, 0.0f);
        dxvk::Logger::info("UserExample: ShaderPack enabled");
    }

    g_state.uiCallbackId = war3shader::RegisterUiDrawCallback(&OnUiDraw, nullptr);
}

void OnUnload(void* userData) {
    (void)userData;
    if (g_state.uiCallbackId != 0) {
        war3shader::UnregisterUiDrawCallback(g_state.uiCallbackId);
    }
    g_state = {};
}

void OnRenderEvent(war3shader::RenderEventID eventId,
                   const war3shader::RenderContext* context,
                   void* userData) {
    (void)userData;
    if (!g_state.enabled || !context)
        return;

    if (eventId == war3shader::RenderEventID::POST_PROCESS_BEGIN) {
        g_state.timeAccum = context->frameTimeCounter;
        const float strength = 0.35f + 0.35f * std::sin(g_state.timeAccum * 0.8f);
        const float vignette = 0.2f + 0.6f * (0.5f + 0.5f * std::sin(g_state.timeAccum * 0.4f));
        war3shader::SetParamVec4(war3shader::ParamSlot::PARAMS0, strength, vignette, 0.0f, 0.0f);
    }
}

bool RegisterUserExampleModuleInternal() {
    war3module::War3ModuleInfo info = {};
    info.name = "UserExample";
    info.version = 0x000100;
    info.apiVersion = war3module::WAR3_MODULE_API_VERSION;

    war3module::War3ModuleCallbacks callbacks = {};
    callbacks.onLoad = &OnLoad;
    callbacks.onUnload = &OnUnload;
    callbacks.onRenderEvent = &OnRenderEvent;

    return war3module::RegisterModule(info, callbacks, nullptr);
}

} // namespace

namespace war3example {

void RegisterUserExampleModule() {
    static bool s_registered = false;
    if (s_registered)
        return;
    s_registered = RegisterUserExampleModuleInternal();
}

} // namespace war3example
