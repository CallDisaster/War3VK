// war3_imgui_inprocess.cpp - the panel's in-process half
//
// Everything of War3Imgui that needs the in-process D3D9DeviceEx, its effects
// or the in-process renderer backend: the device-owned initialize, the cursor
// bitmap and the two device-only panel sections. Only device code calls in
// here (d3d9_swapchain.cpp, d3d9_device.cpp); the shared panel
// (war3_imgui.cpp), which proxy mode also runs from the data layer, reaches
// these sections through DevicePanels pointers and never links against them,
// so the x86 data-layer cut (AutoTest/run_x86_data_layer_cut_gate.py) does not
// pull device or effects code in with it.

#define WAR3_SHADER_API_INTERNAL

#include "war3_imgui.h"
#include "war3_imgui_dx9.h"
#include "../../d3d9_device.h"
#include "../../d3d9_war3_pipeline.h"
#include "../../war3_shaderpack.h"

namespace dxvk::war3 {

namespace {

// The device-owned variant of the generated backend (war3_imgui_dx9.h).
const ImGuiRenderer kDeviceRenderer = {
  &War3ImGui_ImplDX9_Init,
  &War3ImGui_ImplDX9_Shutdown,
  &War3ImGui_ImplDX9_NewFrame,
  &War3ImGui_ImplDX9_RenderDrawData,
  &War3ImGui_ImplDX9_CreateDeviceObjects,
  &War3ImGui_ImplDX9_InvalidateDeviceObjects,
  &War3ImGui_ImplDX9_UpdateTexture,
};

// "模块热插拔": the effects pipeline's pass switches.
void DrawPassToggles(D3D9DeviceEx* device) {
  auto *pipeline = device ? device->GetWar3Pipeline() : nullptr;
  if (!pipeline)
    return;
  bool shadowPass = pipeline->IsPassEnabled("ShadowReceiver");
  if (ImGui::Checkbox("ShadowReceiver", &shadowPass)) {
    pipeline->SetPassEnabled("ShadowReceiver", shadowPass);
  }

  bool ssaoPass = pipeline->IsPassEnabled("SSAO");
  if (ImGui::Checkbox("SSAO", &ssaoPass)) {
    pipeline->SetPassEnabled("SSAO", ssaoPass);
  }

  bool aaPass = pipeline->IsPassEnabled("AA");
  if (ImGui::Checkbox("AA", &aaPass)) {
    pipeline->SetPassEnabled("AA", aaPass);
  }

  bool volumetricPass = pipeline->IsPassEnabled("VolumetricLight");
  if (ImGui::Checkbox("VolumetricLight", &volumetricPass)) {
    pipeline->SetPassEnabled("VolumetricLight", volumetricPass);
  }
}

// "Vulkan ShaderPack": the raw SPIR-V pack of the in-process effects.
void DrawShaderPackPanel() {
  static char s_packPath[256] = "shaderpacks/vulkan_pack_template";
  static bool s_pass0Enabled = true;
  static bool s_pass1Enabled = true;
  static float s_renderScale = 1.0f;
  static bool s_shadowOverride = true;

  war3shader::ShaderPackInfo info = {};
  const auto infoResult = war3shader::GetShaderPackInfo(&info);
  const bool loaded = (info.flags & war3shader::PACK_FLAG_LOADED) != 0;
  const bool enabled = (info.flags & war3shader::PACK_FLAG_ENABLED) != 0;
  const bool hasError = (info.flags & war3shader::PACK_FLAG_HAS_ERROR) != 0;
  const bool rawShaderPackEnabled =
      war3shader::IsRawShaderPackLoadingEnabled();

  if (!rawShaderPackEnabled) {
    ImGui::TextWrapped(
        "发布构建已禁用原始 SPIR-V ShaderPack；仅显式开发构建可用。");
  }
  ImGui::BeginDisabled(!rawShaderPackEnabled);

  ImGui::InputText("Pack Path", s_packPath, sizeof(s_packPath));
  if (ImGui::Button("加载")) {
    war3shader::LoadShaderPack(s_packPath);
  }
  ImGui::SameLine();
  if (ImGui::Button("热重载")) {
    war3shader::ReloadShaderPack();
  }
  ImGui::SameLine();
  bool enableToggle = enabled;
  if (ImGui::Checkbox("启用", &enableToggle)) {
    war3shader::EnableShaderPack(enableToggle);
  }

  ImGui::Text("状态: %s", loaded ? "已加载" : "未加载");
  ImGui::Text("Pass 数量: %u", info.passCount);
  ImGui::Text("错误码: %s", war3shader::GetErrorString(info.lastError));
  if (hasError || infoResult != war3shader::ShaderPackError::OK) {
    const char *msg = war3shader::GetLastShaderError();
    if (msg && msg[0]) {
      ImGui::TextWrapped("错误信息: %s", msg);
    }
  }

  s_renderScale = war3shader::GetRenderScale();
  if (ImGui::SliderFloat("渲染缩放", &s_renderScale, 0.25f, 2.0f, "%.2f")) {
    war3shader::SetRenderScale(s_renderScale);
  }

  s_shadowOverride = war3shader::IsShadowReceiverOverrideEnabled();
  if (ImGui::Checkbox("Shadow Receiver 覆盖", &s_shadowOverride)) {
    war3shader::EnableShadowReceiverOverride(s_shadowOverride);
  }

  if (info.passCount > 0) {
    if (ImGui::Checkbox("Pass 0", &s_pass0Enabled)) {
      war3shader::SetPassEnabled(0, s_pass0Enabled);
    }
  }
  if (info.passCount > 1) {
    if (ImGui::Checkbox("Pass 1", &s_pass1Enabled)) {
      war3shader::SetPassEnabled(1, s_pass1Enabled);
    }
  }

  if (ImGui::Button("保存配置 (Save to pack.json)")) {
    war3shader::SaveShaderPack();
  }

  if (ImGui::TreeNode("用户参数 (User Params)")) {
    for (uint32_t i = 0; i < war3shader::SHADERPACK_MAX_PARAMS; ++i) {
      float v[4] = {0};
      war3shader::GetParamVec4((war3shader::ParamSlot)i, &v[0], &v[1],
                               &v[2], &v[3]);
      char label[32];
      std::snprintf(label, sizeof(label), "Param %u", i);
      if (ImGui::DragFloat4(label, v, 0.01f)) {
        war3shader::SetParamVec4((war3shader::ParamSlot)i, v[0], v[1], v[2],
                                 v[3]);
      }
    }
    ImGui::TreePop();
  }
  ImGui::EndDisabled();
}

} // namespace

void War3Imgui::initialize(HWND hwnd, D3D9DeviceEx *device) {
  if (m_initialized || !hwnd || !device)
    return;
  m_device = device;
  m_devicePanels.passToggles = &DrawPassToggles;
  m_devicePanels.shaderPack = &DrawShaderPackPanel;
  initializeContext(hwnd, device, kDeviceRenderer, 0);
}

void War3Imgui::setCursorBitmap(int width, int height, const void *bgraData,
                                int hotX, int hotY) {
  if (!m_device || m_retiring || width <= 0 || height <= 0 || !bgraData)
    return;

  // Release old texture if size changed (or just release to assume simplicity)
  // Optimization: Keep if size matches.
  if (m_cursorTexture != nullptr) {
    D3DSURFACE_DESC desc;
    m_cursorTexture->GetLevelDesc(0, &desc);
    if (desc.Width != (UINT)width || desc.Height != (UINT)height) {
      m_cursorTexture = nullptr;
    }
  }

  if (m_cursorTexture == nullptr) {
    Com<IDirect3DTexture9> texture;
    if (FAILED(m_device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED, &texture,
                                       NULL))) {
      Logger::err("War3Imgui: Failed to create cursor texture");
      return;
    }
    m_cursorTexture = static_cast<D3D9Texture2D*>(texture.ptr());
  }

  D3DLOCKED_RECT locked;
  if (SUCCEEDED(m_cursorTexture->LockRect(0, &locked, NULL, 0))) {
    const uint8_t *src = (const uint8_t *)bgraData;
    uint8_t *dst = (uint8_t *)locked.pBits;
    for (int y = 0; y < height; ++y) {
      memcpy(dst + y * locked.Pitch, src + y * width * 4, width * 4);
    }
    m_cursorTexture->UnlockRect(0);
  }

  m_cursorWidth = width;
  m_cursorHeight = height;
  m_cursorHotX = hotX;
  m_cursorHotY = hotY;
}

} // namespace dxvk::war3
