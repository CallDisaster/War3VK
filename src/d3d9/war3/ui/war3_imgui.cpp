#define WAR3_SHADER_API_INTERNAL

#include "war3_imgui.h"
#include "../../d3d9_device.h"
#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_shadow.h"
#include "../../d3d9_war3_weather.h"
#include "../../d3d9_war3_volumetric_light.h"
#include "../../war3_shader_api.h"
#include "war3_imgui_dx9_proxy.h"
#include "imgui_impl_win32.h"

#include "../core/war3_execution_services.h"
#include "../core/war3_internal_test_config.h"
#include "../debug/war3_debug.h"
#include "../war3.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_render_state.h"
#include "../shader/war3_shader_manager.h"
#include "../tools/war3_perf_monitor.h"
#include "../tools/war3_diagnostics_hub.h"
#include "../tools/war3_frame_history.h"
#include "../tools/war3_frame_evidence.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../render/war3_shadow_display_stats.h"
#include "../render/war3_stage11_snapshot_page_policy.h"
#include "../memory/war3_shadow_arena_stats.h"
#include "../platform/war3_player_settings.h"
#include "../platform/war3_proxy_diag.h"
#include "../platform/war3_proxy_frame.h"
#include "../effects/war3_sun_cycle.h"

#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace dxvk::war3 {

// 静态成员初始化 - 默认启用 FPS 解锁
std::atomic<bool> War3Imgui::s_fpsUnlocked{true};

War3Imgui &War3Imgui::get() {
  // Process-lifetime registry, not a CRT GPU owner. Normal device shutdown
  // explicitly releases every device resource after its GPU drain. At DLL
  // detach do not run Com destructors under loader lock/after parent teardown.
  static War3Imgui* instance = new War3Imgui;
  return *instance;
}

// The proxy variant of the generated backend (war3_imgui_dx9_proxy.h).
static const ImGuiRenderer kProxyRenderer = {
  &War3ProxyImGui_ImplDX9_Init,
  &War3ProxyImGui_ImplDX9_Shutdown,
  &War3ProxyImGui_ImplDX9_NewFrame,
  &War3ProxyImGui_ImplDX9_RenderDrawData,
  &War3ProxyImGui_ImplDX9_CreateDeviceObjects,
  &War3ProxyImGui_ImplDX9_InvalidateDeviceObjects,
  &War3ProxyImGui_ImplDX9_UpdateTexture,
};

// The in-process initialize (war3_imgui_inprocess.cpp) sets m_device and the
// device panels first; proxy mode sets m_proxy. On failure nothing survives.
void War3Imgui::initializeContext(HWND hwnd, IDirect3DDevice9 *target,
                                  const ImGuiRenderer &renderer,
                                  int textureLimit) {
  m_hwnd = hwnd;
  m_target = target;
  m_renderer = &renderer;
  m_retiring = false;
  m_counters = Status{};

  ImGuiContext* previous = ImGui::GetCurrentContext();
  // No borrowed device/context may survive a failed swapchain constructor.
  // Backend data is published by each backend before its remaining setup.
  const auto abandonInitialization = [&] {
    if (m_context) {
      ImGui::SetCurrentContext(m_context);
      if (ImGui::GetIO().BackendRendererUserData)
        renderer.shutdown();
      if (ImGui::GetIO().BackendPlatformUserData)
        ImGui_ImplWin32_Shutdown();
      ImGui::DestroyContext(m_context);
    }
    ImGui::SetCurrentContext(previous);
    m_context = nullptr;
    m_device = nullptr;
    m_target = nullptr;
    m_renderer = nullptr;
    m_devicePanels = DevicePanels{};
    m_proxy = false;
    m_hwnd = nullptr;
    m_initialized = false;
  };
  try {
  IMGUI_CHECKVERSION();
  m_context = ImGui::CreateContext();
  ImGui::SetCurrentContext(m_context);
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigFlags |=
      ImGuiConfigFlags_NoMouseCursorChange; // Prevent ImGui from messing with
                                            // OS cursor
  io.MouseDrawCursor =
      false; // Disable ImGui software cursor, let game handle it
  // Before the first font: the atlas grows only up to this.
  if (textureLimit > 0)
    io.Fonts->TexMaxWidth = io.Fonts->TexMaxHeight = textureLimit;

  // Style
  ImGui::StyleColorsDark();

  // Load Chinese Font (Microsoft YaHei) for Chinese characters
  // If failed, it acts as default.
  ImFontConfig font_config;
  io.Fonts->AddFontFromFileTTF("c:\\windows\\fonts\\msyh.ttc", 18.0f,
                               &font_config,
                               io.Fonts->GetGlyphRangesChineseFull());

  // Platform/Renderer backends
  if (!ImGui_ImplWin32_Init(hwnd)) {
    abandonInitialization();
    return;
  }
  if (!renderer.init(target)) {
    abandonInitialization();
    return;
  }

  m_initialized = true;
  Logger::info("War3Imgui initialized.");
  } catch (...) {
    abandonInitialization();
    throw;
  }
}

bool War3Imgui::initializeProxy(HWND hwnd, IDirect3DDevice9 *device) {
  if (m_initialized || !hwnd || !device)
    return false;
  m_proxy = true;
  // The front end's textures stop at 1024 texels a side (d3d9_texture_wire.h).
  initializeContext(hwnd, device, kProxyRenderer, 1024);
  if (!m_initialized)
    return false;
  // Opened on request: the panel is visible from its first frame.
  m_visible = true;
  return true;
}

void War3Imgui::retire() {
  m_retiring = true;
  war3shader::internal::EndUiFrame();
  war3shader::internal::SetImGuiContext(nullptr);
}

// The context and everything made with it; the owner checks come first.
void War3Imgui::destroyContext() {
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(m_context);
  m_renderer->shutdown();
  m_cursorTexture = nullptr;
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext(m_context);
  ImGui::SetCurrentContext(previous == m_context ? nullptr : previous);
  m_context = nullptr;
  m_device = nullptr;
  m_target = nullptr;
  m_renderer = nullptr;
  m_devicePanels = DevicePanels{};
  m_proxy = false;
  m_hwnd = nullptr;
  m_frameStarted = false;
  m_hasRendered = false;
  m_visible = false;
  m_retiring = false;
  m_cursorWidth = m_cursorHeight = m_cursorHotX = m_cursorHotY = 0;
  m_initialized = false;
}

void War3Imgui::beginDeviceTeardown(D3D9DeviceEx* owner) {
  if (!m_initialized || m_device != owner)
    return;
  retire();
}

void War3Imgui::shutdown(D3D9DeviceEx* owner) {
  if (!m_initialized || m_device != owner)
    return;
  player::FlushIfDue(true);
  beginDeviceTeardown(owner);
  m_renderStatsRefresh.reset();
  destroyContext();
}

void War3Imgui::shutdownProxy(IDirect3DDevice9* device) {
  if (!m_initialized || !m_proxy || m_target != device)
    return;
  player::FlushIfDue(true);
  retire();
  m_renderStatsRefresh.reset();
  destroyContext();
}

void War3Imgui::invalidateDeviceObjects(D3D9DeviceEx* owner) {
  if (!isOwner(owner))
    return;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(m_context);
  m_renderer->invalidateDeviceObjects();
  ImGui::SetCurrentContext(previous);
}

void War3Imgui::invalidateProxy(IDirect3DDevice9* device) {
  if (!isProxyOwner(device))
    return;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(m_context);
  m_renderer->invalidateDeviceObjects();
  ImGui::SetCurrentContext(previous);
}

void War3Imgui::restoreDeviceObjects(D3D9DeviceEx* owner) {
  if (!isOwner(owner))
    return;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(m_context);
  m_renderer->createDeviceObjects();
  ImGui::SetCurrentContext(previous);
}

War3Imgui::Status War3Imgui::status() const {
  Status result = m_counters;
  result.initialized = m_initialized;
  result.visible = m_visible;
  result.proxy = m_proxy;
  result.wantCaptureMouse = wantsCaptureMouse();
  result.wantCaptureKeyboard = wantsCaptureKeyboard();
  if (m_initialized && m_context) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_context);
    if (const ImTextureData* atlas = ImGui::GetIO().Fonts->TexData) {
      result.atlasWidth = uint32_t(atlas->Width);
      result.atlasHeight = uint32_t(atlas->Height);
    }
    ImGui::SetCurrentContext(previous);
  }
  return result;
}

bool War3Imgui::handleWndProc(HWND hWnd, UINT msg, WPARAM wParam,
                              LPARAM lParam) {
  if (ownsWindow(hWnd) && m_visible) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
      return true;
  }
  return false;
}

void War3Imgui::toggleVisible() {
  m_visible = !m_visible;
  // Closing the panel saves pending edits right away.
  if (!m_visible)
    player::FlushIfDue(true);
}

bool War3Imgui::wantsCaptureMouse() const {
  if (!m_initialized || m_retiring || !m_visible)
    return false;
  return ImGui::GetIO().WantCaptureMouse;
}

bool War3Imgui::wantsCaptureKeyboard() const {
  if (!m_initialized || m_retiring || !m_visible)
    return false;
  return ImGui::GetIO().WantCaptureKeyboard;
}

void War3Imgui::newFrame() {
  if (!m_initialized || m_retiring || !m_visible)
    return;
  if (m_frameStarted)
    return;

  m_frameStarted = true;
  m_hasRendered = false; // Reset frame flag

  m_renderer->newFrame();
  ImGui_ImplWin32_NewFrame();
  if (m_proxy) {
    // A window resize waits for the Present after this callback (the front
    // end takes none inside it): draw for the back buffer the frame has.
    IDirect3DSurface9* backBuffer = nullptr;
    D3DSURFACE_DESC desc = {};
    if (SUCCEEDED(m_target->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer) {
      if (SUCCEEDED(backBuffer->GetDesc(&desc)) && desc.Width && desc.Height)
        ImGui::GetIO().DisplaySize = ImVec2(float(desc.Width), float(desc.Height));
      backBuffer->Release();
    }
  }
  ImGui::NewFrame();

  war3shader::internal::SetImGuiContext(ImGui::GetCurrentContext());
  war3shader::internal::BeginUiFrame();
  if(m_visible)war3shader::internal::DispatchUiCallbacks();
}

void War3Imgui::endFrame() {
  if (!m_initialized)
    return;
  m_frameStarted = false;
}

void War3Imgui::render(bool inScene) {
  if (!m_initialized || m_retiring || m_hasRendered || !m_visible)
    return;
  // Keep console pixels outside the recorder's final-color history capture.
  if (inScene && tools::evidence::Enabled())
    return;
  m_hasRendered = true;

  // Draw internal windows
  drawDebugWindow();
  war3shader::internal::FlushUiCommands();

  // Draw cursor overlay (only when ImGui is actively capturing the mouse)
  // [User Request] If we are injecting mid-frame (inScene=true), we assume:
  // 1. ImGui is drawn before Game UI.
  // 2. Game Cursor is drawn AFTER Game UI (Standard War3 behavior).
  // Therefore, the Game Cursor will naturally cover ImGui.
  // We skip our manual overlay here to avoid "Double Cursor" or "Cursor under
  // UI" issues.
  if (!inScene && ImGui::GetIO().WantCaptureMouse) {
    drawCursorOverlay();
  }

  // End Frame
  ImGui::EndFrame();
  ImGui::Render();

  // DX9 Draw
  ImDrawData* drawData = ImGui::GetDrawData();
  if (m_target && (!m_proxy || uploadTextures(drawData))) {
    if (!inScene)
      m_target->BeginScene();
    m_renderer->renderDrawData(drawData);
    if (!inScene)
      m_target->EndScene();
    ++m_counters.framesDrawn;
    uint32_t commands = 0;
    for (const ImDrawList* list : drawData->CmdLists)
      commands += uint32_t(list->CmdBuffer.Size);
    m_counters.lastCommands = commands;
    m_counters.lastVertices = uint32_t(drawData->TotalVtxCount);
  }

  war3shader::internal::EndUiFrame();
}

// Proxy mode: the texture requests first, then the draw only if every texture
// it may name is uploaded. A texture the front end refused stays WantCreate
// (the generated proxy backend does not assert) and is asked for again next
// frame; drawing now would name an atlas that does not exist.
bool War3Imgui::uploadTextures(ImDrawData* drawData) {
  bool ready = true;
  if (drawData->Textures != nullptr) {
    for (ImTextureData* texture : *drawData->Textures) {
      if (texture->Status != ImTextureStatus_OK)
        m_renderer->updateTexture(texture);
      if (texture->Status != ImTextureStatus_OK &&
          texture->Status != ImTextureStatus_Destroyed) {
        ++m_counters.textureFailures;
        ready = false;
      }
    }
  }
  if (!ready)
    ++m_counters.framesSkipped;
  return ready;
}

namespace {

// Sandbox only (WARVK_D3D9_EVIDENCE_TEST=1): the isolated desktop has no mouse
// for the collapsed sections, and the panel must be seen drawing.
// Sandbox only (WARVK_D3D9_PANEL_TEST_OPEN=sun): the way to the sun dial opens
// instead, and the dial is scrolled to the top of the panel.
bool SunDialTest() {
  static const bool test = [] {
    wchar_t value[8] = {};
    return GetEnvironmentVariableW(L"WARVK_D3D9_PANEL_TEST_OPEN", value, 8) == 3 && wcscmp(value, L"sun") == 0;
  }();
  return test;
}

void OpenSunDialForTest() {
  if (SunDialTest())
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
}

// Sandbox only (WARVK_D3D9_PANEL_TEST_OPEN=water): the water section opens,
// scrolled to the top of the panel.
bool WaterPanelTest() {
  static const bool test = [] {
    wchar_t value[8] = {};
    return GetEnvironmentVariableW(L"WARVK_D3D9_PANEL_TEST_OPEN", value, 8) == 5 && wcscmp(value, L"water") == 0;
  }();
  return test;
}

void OpenWaterForTest() {
  if (WaterPanelTest())
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
}

void OpenSectionForTest() {
  static const bool test = [] {
    wchar_t value[4] = {};
    return GetEnvironmentVariableW(L"WARVK_D3D9_EVIDENCE_TEST", value, 4) == 1 && value[0] == L'1';
  }();
  if (test && !SunDialTest() && !WaterPanelTest())
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
}

// Green when empty, yellow at half, red when full.
ImVec4 FullnessColor(float fraction) {
  const float t = std::clamp(fraction, 0.0f, 1.0f);
  const ImVec4 green(.25f, .72f, .35f, 1.f), yellow(.95f, .78f, .20f, 1.f), red(.90f, .18f, .14f, 1.f);
  const ImVec4& a = t < .5f ? green : yellow;
  const ImVec4& b = t < .5f ? yellow : red;
  const float k = t < .5f ? t * 2.f : (t - .5f) * 2.f;
  return ImVec4(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k, 1.f);
}

// The bar grows with what is used.
void FullnessBar(uint64_t used, uint64_t total, const char* what) {
  const float fraction = ui::BudgetFraction(used, total);
  char text[160];
  std::snprintf(text, sizeof(text), "%s %.0f / %.0f MiB (%.0f%%)", what, ui::BytesToMiB(used),
                ui::BytesToMiB(total), fraction * 100.0f);
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, FullnessColor(fraction));
  ImGui::ProgressBar(fraction, ImVec2(-1, 0), text);
  ImGui::PopStyleColor();
}

// ---- The sun dial -----------------------------------------------------------
//
// The day as a dial: noon at the top, midnight at the bottom, sunrise on the
// left (east), sunset on the right (west). The ring is the light's colour at
// each hour, the sun sits on it at the frame's time and the moon opposite.
// Inside, the sky of that hour over the horizon, and a figure with the shadow
// the light casts at that moment: the length the shadow pass draws. Every
// colour and length comes from War3SunCycle::Evaluate, the pass's own model.

constexpr float kDialPi = 3.14159265f;

struct SunPreset {
  const char* name;
  std::array<War3SunTintKey, War3SunStyleSettings::KeyCount> keys;
};

// The order of War3SunStyleSettings::KeyHours: moon, sunrise, morning, noon,
// afternoon, sunset.
const SunPreset kSunPresets[] = {
    { "自然", War3SunStyleSettings{}.tint },
    // The horizon keys stay dim (sun and moon change places there); the moon
    // is on the scale of the night's own ambient (about 0.46 is the default):
    // brighter than about 0.6 the night's light passes white on the ground
    // and turns grey. By day the lit ground is white whatever the key, so a
    // preset shows in the golden hours, on walls, in the shadow's colour and
    // at night.
    { "暖阳", {{ { 0.66f, 0.76f, 1.00f, 0.48f }, { 1.00f, 0.60f, 0.24f, 0.18f }, { 1.00f, 0.90f, 0.74f, 1.00f },
                 { 1.00f, 0.95f, 0.86f, 1.00f }, { 1.00f, 0.86f, 0.66f, 1.00f }, { 1.00f, 0.54f, 0.20f, 0.18f } }} },
    { "黄金时刻", {{ { 0.56f, 0.70f, 1.00f, 0.44f }, { 1.00f, 0.54f, 0.14f, 0.20f }, { 1.00f, 0.82f, 0.54f, 1.00f },
                   { 1.00f, 0.94f, 0.82f, 1.00f }, { 1.00f, 0.76f, 0.42f, 1.00f }, { 1.00f, 0.40f, 0.10f, 0.20f } }} },
    { "冷暖对比", {{ { 0.42f, 0.60f, 1.00f, 0.50f }, { 1.00f, 0.50f, 0.36f, 0.16f }, { 1.00f, 0.95f, 0.86f, 1.00f },
                   { 0.94f, 0.97f, 1.00f, 1.00f }, { 1.00f, 0.86f, 0.66f, 1.00f }, { 1.00f, 0.36f, 0.22f, 0.16f } }} },
    { "明亮月夜", {{ { 0.58f, 0.74f, 1.00f, 0.60f }, { 1.00f, 0.54f, 0.18f, 0.14f }, { 1.00f, 0.96f, 0.89f, 1.00f },
                   { 1.00f, 0.98f, 0.93f, 1.00f }, { 1.00f, 0.95f, 0.86f, 1.00f }, { 1.00f, 0.50f, 0.16f, 0.14f } }} },
};
const char* const kSunKeyNames[War3SunStyleSettings::KeyCount] = { "月光", "日出", "上午", "正午", "下午", "日落" };

float DialSmooth(float v) {
  v = std::clamp(v, 0.0f, 1.0f);
  return v * v * (3.0f - 2.0f * v);
}

float WrapHours(float hours) {
  if (!std::isfinite(hours))
    return 12.0f;
  hours = std::fmod(hours, 24.0f);
  return hours < 0.0f ? hours + 24.0f : hours;
}

ImVec4 MixColor(const ImVec4& a, const ImVec4& b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImU32 DialColor(const ImVec4& c, float alpha = 1.0f) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(c.x, 0.0f, 1.0f), std::clamp(c.y, 0.0f, 1.0f),
                                               std::clamp(c.z, 0.0f, 1.0f), std::clamp(c.w * alpha, 0.0f, 1.0f)));
}

// A light (colour times brightness, brightness in w) as a swatch: its hue at
// full value, dimmed for a dim light and washed toward white past 1.
ImVec4 LightSwatch(const Vector4& light) {
  const float peak = std::max({ light.x, light.y, light.z, 1e-4f });
  const float bright = std::clamp(std::isfinite(light.w) ? light.w : peak, 0.0f, 3.0f);
  const float value = 0.20f + 0.80f * std::min(1.0f, bright);
  const ImVec4 hue(light.x / peak, light.y / peak, light.z / peak, 1.0f);
  const ImVec4 lit(hue.x * value, hue.y * value, hue.z * value, 1.0f);
  return MixColor(lit, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), std::clamp((bright - 1.0f) * 0.25f, 0.0f, 0.35f));
}

// The ring: an hour at the bearing the light has then (screen, y down): 6
// east (right), 12 on the side the path passes at noon (south, below, unless
// the player moved it north), 18 west. side: +1 south, -1 north.
ImVec2 DialRingDirection(float hours, float side) {
  const float a = (hours - 6.0f) / 12.0f * kDialPi;
  return ImVec2(std::cos(a), side * std::sin(a));
}

float DialRingHours(const ImVec2& d, float side) {
  return WrapHours(6.0f + std::atan2(d.y * side, d.x) / kDialPi * 12.0f);
}

float DialPathSide(const War3SunStyleSettings& style) {
  return style.pathSet && War3SunCycle::PathNorth(style) > 0.0f ? -1.0f : 1.0f;
}

// The wind a light comes from, x east and y north.
const char* DialBearing(float x, float y) {
  if (x * x + y * y < 1e-6f)
    return "头顶";
  static const char* const names[8] = { "东边", "东北边", "北边", "西北边", "西边", "西南边", "南边", "东南边" };
  const int sector = int(std::lround(std::atan2(y, x) / (2.0f * kDialPi) * 8.0f));
  return names[((sector % 8) + 8) % 8];
}

void DialQuad(ImDrawList* dl, const ImVec2& a, const ImVec2& b, const ImVec2& c, const ImVec2& d,
              ImU32 ca, ImU32 cb, ImU32 cc, ImU32 cd) {
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  dl->PrimReserve(6, 4);
  const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
  dl->PrimWriteIdx(base);
  dl->PrimWriteIdx(ImDrawIdx(base + 1));
  dl->PrimWriteIdx(ImDrawIdx(base + 2));
  dl->PrimWriteIdx(base);
  dl->PrimWriteIdx(ImDrawIdx(base + 2));
  dl->PrimWriteIdx(ImDrawIdx(base + 3));
  dl->PrimWriteVtx(a, uv, ca);
  dl->PrimWriteVtx(b, uv, cb);
  dl->PrimWriteVtx(c, uv, cc);
  dl->PrimWriteVtx(d, uv, cd);
}

// A disc shaded from its centre colour to its rim colour.
void DialRadial(ImDrawList* dl, const ImVec2& c, float r, const ImVec4& inner, const ImVec4& outer, int rings,
                int segments) {
  for (int k = 0; k < rings; ++k) {
    const float t0 = float(k) / float(rings), t1 = float(k + 1) / float(rings);
    const ImU32 c0 = DialColor(MixColor(inner, outer, t0)), c1 = DialColor(MixColor(inner, outer, t1));
    for (int i = 0; i < segments; ++i) {
      const float a0 = 2.0f * kDialPi * float(i) / float(segments);
      const float a1 = 2.0f * kDialPi * float(i + 1) / float(segments);
      const ImVec2 u0(std::cos(a0) * r, std::sin(a0) * r), u1(std::cos(a1) * r, std::sin(a1) * r);
      DialQuad(dl, ImVec2(c.x + u0.x * t0, c.y + u0.y * t0), ImVec2(c.x + u0.x * t1, c.y + u0.y * t1),
               ImVec2(c.x + u1.x * t1, c.y + u1.y * t1), ImVec2(c.x + u1.x * t0, c.y + u1.y * t0), c0, c1, c1, c0);
    }
  }
}

void DialText(ImDrawList* dl, float size, const ImVec2& centre, ImU32 col, const char* text) {
  ImFont* font = ImGui::GetFont();
  const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  dl->AddText(font, size, ImVec2(centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f), col, text);
}

void DialHelp(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", text);
}

// The hours of the frame: the locked time, or the time the pass resolved.
float DialCurrentHours(const War3RenderSettings& s) {
  return s.shadows.lockSun ? WrapHours(s.shadows.lockSunTime * 24.0f) : WrapHours(s.dayNight.renderTimeHours);
}

void LockSunAt(War3RenderSettings& s, float hours) {
  s.shadows.lockSun = true;
  s.shadows.lockSunTime = std::clamp(WrapHours(hours) / 24.0f, 0.0f, 0.9999f);
}

// The dial seen from above, as the screen shows the map: east right, north
// up. Inside, the ground with the light's path over its half of the day, the
// sun (or the moon) on it, and a ball on a post in the centre with the shadow
// the frame casts. Around it the ring of the day's light, which sets the time.
void DrawSunDialDisc(War3RenderSettings& s) {
  ImGuiIO& io = ImGui::GetIO();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float size = std::clamp(avail, 190.0f, 300.0f);
  const float label = ImGui::GetFontSize() * 0.85f;
  const float big = ImGui::GetFontSize() * 1.55f;
  const float textHeight = big + label * 2.7f;
  if (avail > size)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size) * 0.5f);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##sundial", ImVec2(size, size + textHeight));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  const bool pressed = ImGui::IsItemActivated();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  const ImVec2 c(origin.x + size * 0.5f, origin.y + size * 0.5f);
  const float rOut = size * 0.5f - label - 4.0f;
  const float ring = std::clamp(size * 0.07f, 12.0f, 20.0f);
  const float rIn = rOut - ring;
  const float rGround = rIn - 3.0f;
  const float rMid = (rIn + rOut) * 0.5f;

  auto& style = s.sunStyle;
  const bool locked = s.shadows.lockSun;
  float hours = DialCurrentHours(s);
  War3SunCycle::Result now = War3SunCycle::Evaluate(s, hours / 24.0f);

  // Where the press began decides: the ring sets the time; the disc moves the
  // light, across for the time (when locked), up and down for the side it
  // comes from.
  enum { kDragNone, kDragRing, kDragDisc };
  static int dragMode = kDragNone;
  static bool dragDay = true;
  const ImVec2 m(io.MousePos.x - c.x, io.MousePos.y - c.y);
  const float mr = std::sqrt(m.x * m.x + m.y * m.y);
  const bool overRing = mr >= rIn - 3.0f && mr <= rOut + 4.0f;
  const bool overDisc = mr < rIn - 3.0f;
  if (pressed) {
    dragMode = overRing ? kDragRing : overDisc ? kDragDisc : kDragNone;
    dragDay = now.day;
  } else if (!active) {
    dragMode = kDragNone;
  }
  if (active && dragMode == kDragRing && locked && mr > 4.0f) {
    hours = DialRingHours(m, DialPathSide(style));
    if (io.KeyCtrl)
      hours = WrapHours(std::round(hours));
    LockSunAt(s, hours);
  } else if (active && dragMode == kDragDisc) {
    const float east = std::clamp(m.x / rGround, -1.0f, 1.0f);
    const float north = -m.y / rGround;
    float phase = 0.0f;
    if (locked) {
      // The half of the day the press began in: the sun by day, the moon by
      // night, east at its start, west at its end.
      const float start = dragDay ? 6.0f : 18.0f;
      float h = start + std::acos(east) / kDialPi * 12.0f;
      if (io.KeyCtrl)
        h = std::round(h);
      h = std::clamp(h, start + 0.05f, start + 11.95f);
      phase = (h - start) / 12.0f * kDialPi;
      hours = WrapHours(h);
      LockSunAt(s, hours);
    } else {
      // The time runs; only the side moves.
      const float start = now.day ? 6.0f : 18.0f;
      phase = std::clamp(WrapHours(hours - start) / 12.0f, 0.0f, 1.0f) * kDialPi;
    }
    const float across = std::sin(phase);
    if (across > 0.2f) {
      style.pathNorth = std::clamp(north / across, -1.0f, 1.0f);
      style.pathSet = true;
    }
  } else if (hovered && locked && io.MouseWheel != 0.0f) {
    hours = WrapHours(hours + io.MouseWheel * 0.25f);
    LockSunAt(s, hours);
  }
  if ((hovered || active) && (overDisc || (overRing && locked) || dragMode != kDragNone))
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (hovered && !active) {
    if (overRing)
      ImGui::SetTooltip(locked ? "拖动外圈改变时间；滚轮每格 15 分钟；按住 Ctrl 对齐整点"
                               : "勾选下面的“锁定时间”后，可以拖动外圈改变时间");
    else if (overDisc)
      ImGui::SetTooltip(locked ? "拖动太阳（夜里是月亮）：左右改变时间，上下改变光从哪边照来\n按住 Ctrl 对齐整点"
                               : "上下拖动，改变光从哪边照来\n勾选“锁定时间”后，左右拖动可以改变时间");
  }

  now = War3SunCycle::Evaluate(s, hours / 24.0f);
  const float side = DialPathSide(style);
  const bool day = now.day;
  const float altitude01 = std::clamp(now.realAltitudeRad / (kDialPi * 0.5f), -1.0f, 1.0f);
  const float dayness = DialSmooth((altitude01 + 0.10f) / 0.45f);
  const ImVec4 lightHue = LightSwatch(Vector4(now.color.x, now.color.y, now.color.z, 1.0f));
  const float strength = std::clamp(now.shadowStrength / 0.6075f, 0.0f, 1.0f);

  // The ground, lit by the frame's light, darker toward the rim.
  {
    const ImVec4 grass(0.34f, 0.37f, 0.22f, 1.0f), night(0.05f, 0.06f, 0.10f, 1.0f);
    const ImVec4 ground = MixColor(MixColor(night, grass, dayness), lightHue, 0.22f * std::max(dayness, 0.3f));
    const ImVec4 centre(std::min(1.0f, ground.x * 1.18f), std::min(1.0f, ground.y * 1.18f),
                        std::min(1.0f, ground.z * 1.18f), 1.0f);
    const ImVec4 rim(ground.x * 0.42f, ground.y * 0.42f, ground.z * 0.48f, 1.0f);
    DialRadial(dl, c, rGround, centre, rim, 8, 64);
  }
  // The compass: the cross, the circle of 60 degrees up, the four winds.
  {
    const ImU32 faint = DialColor(ImVec4(1, 1, 1, 0.10f));
    dl->AddLine(ImVec2(c.x - rGround, c.y), ImVec2(c.x + rGround, c.y), faint, 1.0f);
    dl->AddLine(ImVec2(c.x, c.y - rGround), ImVec2(c.x, c.y + rGround), faint, 1.0f);
    dl->AddCircle(c, rGround * 0.5f, faint, 64, 1.0f);
    const ImU32 wind = DialColor(ImVec4(1, 1, 1, 0.42f));
    const float inset = label * 0.85f;
    DialText(dl, label, ImVec2(c.x, c.y - rGround + inset), wind, "北");
    DialText(dl, label, ImVec2(c.x, c.y + rGround - inset), wind, "南");
    DialText(dl, label, ImVec2(c.x + rGround - inset, c.y), wind, "东");
    DialText(dl, label, ImVec2(c.x - rGround + inset, c.y), wind, "西");
  }
  // The light's path over its half of the day, dashed, a dot every 3 hours.
  {
    const float start = day ? 6.0f : 18.0f;
    const ImU32 pathColor = DialColor(day ? ImVec4(1.0f, 0.86f, 0.45f, 0.60f) : ImVec4(0.72f, 0.82f, 1.0f, 0.55f));
    const auto at = [&](float h) {
      const War3SunCycle::Result r = War3SunCycle::Evaluate(s, WrapHours(h) / 24.0f);
      return ImVec2(c.x + r.dialX * rGround, c.y - r.dialY * rGround);
    };
    constexpr int kSamples = 48;
    ImVec2 prev = at(start + 0.05f);
    for (int i = 1; i <= kSamples; ++i) {
      const ImVec2 p = at(start + 0.05f + 11.9f * float(i) / float(kSamples));
      if (i % 2 == 1)
        dl->AddLine(prev, p, pathColor, 1.5f);
      prev = p;
    }
    for (int k = 1; k <= 3; ++k)
      dl->AddCircleFilled(at(start + 3.0f * float(k)), 2.2f, pathColor, 12);
  }

  const ImVec2 body(c.x + now.dialX * rGround, c.y - now.dialY * rGround);
  // The ray from the light to the model.
  {
    const ImVec2 d(c.x - body.x, c.y - body.y);
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len > 22.0f) {
      const ImU32 ray = DialColor(ImVec4(lightHue.x, lightHue.y, lightHue.z, 0.30f + 0.30f * strength));
      for (float t = 12.0f; t < len - 10.0f; t += 7.0f) {
        const float t1 = std::min(t + 3.5f, len - 10.0f);
        dl->AddLine(ImVec2(body.x + d.x / len * t, body.y + d.y / len * t),
                    ImVec2(body.x + d.x / len * t1, body.y + d.y / len * t1), ray, 1.0f);
      }
    }
  }
  // The model's shadow: the post as a band, the ball at its end; the length
  // is the frame's (height / tan of the cast altitude).
  const float ball = std::max(5.0f, rGround * 0.075f);
  const float castAlt = std::max(now.castAltitudeRad, 0.002f);
  const float lengthFactor = 1.0f / std::tan(castAlt);
  {
    const float fx = now.direction.x, fy = -now.direction.y;
    const float flat = std::sqrt(fx * fx + fy * fy);
    if (strength > 0.01f && flat > 1e-4f) {
      const ImVec2 u(fx / flat, fy / flat), n(-u.y, u.x);
      const float height = rGround * 0.17f;
      const float reach = rGround - 6.0f;
      const float want = height * lengthFactor;
      const bool clipped = want > reach;
      const float drawn = std::min(want, reach);
      const float a0 = 0.22f + 0.55f * strength;
      const ImU32 dark = DialColor(ImVec4(0, 0, 0, a0));
      const ImU32 clear = DialColor(ImVec4(0, 0, 0, 0));
      const float post = ball * 0.38f;
      const float solid = clipped ? 0.75f : 1.0f;
      const ImVec2 mid(c.x + u.x * drawn * solid, c.y + u.y * drawn * solid);
      const ImVec2 tip(c.x + u.x * drawn, c.y + u.y * drawn);
      DialQuad(dl, ImVec2(c.x + n.x * post, c.y + n.y * post), ImVec2(mid.x + n.x * post, mid.y + n.y * post),
               ImVec2(mid.x - n.x * post, mid.y - n.y * post), ImVec2(c.x - n.x * post, c.y - n.y * post),
               dark, dark, dark, dark);
      if (clipped)
        DialQuad(dl, ImVec2(mid.x + n.x * post, mid.y + n.y * post), ImVec2(tip.x + n.x * post, tip.y + n.y * post),
                 ImVec2(tip.x - n.x * post, tip.y - n.y * post), ImVec2(mid.x - n.x * post, mid.y - n.y * post),
                 dark, clear, clear, dark);
      else
        dl->AddCircleFilled(tip, ball * 0.95f, dark, 24);
    }
    dl->AddCircleFilled(c, ball * 1.15f, DialColor(ImVec4(0, 0, 0, 0.30f)), 24);
  }
  // The model: a ball seen from above, lit on the side the light comes from.
  {
    const float lit = 0.35f + 0.65f * std::max(dayness, 0.35f);
    const ImVec4 stone(0.80f * lit, 0.76f * lit, 0.68f * lit, 1.0f);
    const ImVec4 shade(stone.x * 0.42f, stone.y * 0.42f, stone.z * 0.48f, 1.0f);
    const float bx = body.x - c.x, by = body.y - c.y;
    const float bl = std::sqrt(bx * bx + by * by);
    const ImVec2 toward = bl > 1e-3f ? ImVec2(bx / bl, by / bl) : ImVec2(0.0f, 0.0f);
    const ImVec4 litSide = MixColor(stone, ImVec4(std::min(1.0f, lightHue.x * 1.1f), std::min(1.0f, lightHue.y * 1.1f),
                                                  std::min(1.0f, lightHue.z * 1.1f), 1.0f), 0.45f);
    dl->AddCircleFilled(c, ball, DialColor(shade), 32);
    dl->AddCircleFilled(ImVec2(c.x + toward.x * ball * 0.22f, c.y + toward.y * ball * 0.22f), ball * 0.76f,
                        DialColor(MixColor(shade, litSide, 0.35f + 0.65f * strength)), 32);
    dl->AddCircleFilled(ImVec2(c.x + toward.x * ball * 0.45f, c.y + toward.y * ball * 0.45f), ball * 0.26f,
                        DialColor(ImVec4(1, 1, 1, 0.12f + 0.50f * strength)), 16);
    dl->AddCircle(c, ball, DialColor(ImVec4(0, 0, 0, 0.65f)), 32, 1.2f);
  }
  // The light: the sun with a glow in its own colour, or the moon.
  if (day) {
    const float up = 0.55f + 0.45f * dayness;
    for (int k = 5; k >= 1; --k)
      dl->AddCircleFilled(body, 7.0f + float(k) * 3.2f,
                          DialColor(ImVec4(1.0f, 0.80f, 0.40f, 0.07f * up * float(6 - k) / 5.0f)), 28);
    dl->AddCircleFilled(body, 8.0f, DialColor(ImVec4(1.0f, 0.78f, 0.30f, up)), 28);
    dl->AddCircleFilled(body, 4.5f, DialColor(ImVec4(1.0f, 0.97f, 0.85f, up)), 20);
    dl->AddCircle(body, 8.0f, DialColor(ImVec4(0.45f, 0.25f, 0.0f, 0.7f * up)), 28, 1.2f);
  } else {
    dl->AddCircleFilled(body, 12.0f, DialColor(ImVec4(0.66f, 0.74f, 1.0f, 0.16f)), 24);
    dl->AddCircleFilled(body, 7.0f, DialColor(ImVec4(0.90f, 0.93f, 1.0f, 1.0f)), 24);
    dl->AddCircleFilled(ImVec2(body.x + 3.0f, body.y - 1.8f), 5.8f, DialColor(ImVec4(0.10f, 0.13f, 0.26f, 0.85f)), 24);
  }
  if (active && dragMode == kDragDisc)
    dl->AddCircle(body, 12.5f, DialColor(ImVec4(1, 1, 1, 0.8f)), 28, 2.0f);

  // The ring: the light of every quarter hour.
  constexpr int kSegments = 96;
  ImU32 ringColors[kSegments + 1];
  for (int i = 0; i <= kSegments; ++i) {
    const War3SunCycle::Result at = War3SunCycle::Evaluate(s, float(i) / float(kSegments));
    ringColors[i] = DialColor(LightSwatch(at.color));
  }
  for (int i = 0; i < kSegments; ++i) {
    const float h0 = 24.0f * float(i) / float(kSegments), h1 = 24.0f * float(i + 1) / float(kSegments);
    const ImVec2 d0 = DialRingDirection(h0, side), d1 = DialRingDirection(h1, side);
    DialQuad(dl, ImVec2(c.x + d0.x * rIn, c.y + d0.y * rIn), ImVec2(c.x + d0.x * rOut, c.y + d0.y * rOut),
             ImVec2(c.x + d1.x * rOut, c.y + d1.y * rOut), ImVec2(c.x + d1.x * rIn, c.y + d1.y * rIn),
             ringColors[i], ringColors[i], ringColors[i + 1], ringColors[i + 1]);
  }
  dl->AddCircle(c, rOut, DialColor(ImVec4(0, 0, 0, 0.55f)), 128, 1.5f);
  dl->AddCircle(c, rIn, DialColor(ImVec4(0, 0, 0, 0.55f)), 128, 1.5f);
  dl->AddCircle(c, rOut - 1.5f, DialColor(ImVec4(1, 1, 1, 0.16f)), 128, 1.0f);
  dl->AddCircle(c, rGround, DialColor(ImVec4(0, 0, 0, 0.45f)), 128, 2.0f);
  for (int h = 0; h < 24; ++h) {
    const ImVec2 d = DialRingDirection(float(h), side);
    const bool major = h % 6 == 0, mid = h % 3 == 0;
    const float in = major ? 7.0f : mid ? 5.0f : 3.0f;
    dl->AddLine(ImVec2(c.x + d.x * (rOut - in), c.y + d.y * (rOut - in)),
                ImVec2(c.x + d.x * (rOut - 1.0f), c.y + d.y * (rOut - 1.0f)),
                DialColor(ImVec4(0.05f, 0.05f, 0.08f, major ? 0.85f : 0.55f)), major ? 2.0f : 1.0f);
  }
  const char* const hourLabels[4] = { "0", "6", "12", "18" };
  for (int i = 0; i < 4; ++i) {
    const ImVec2 d = DialRingDirection(float(i * 6), side);
    DialText(dl, label, ImVec2(c.x + d.x * (rOut + label * 0.75f), c.y + d.y * (rOut + label * 0.75f)),
             ImGui::GetColorU32(ImGuiCol_TextDisabled), hourLabels[i]);
  }
  if (style.customTint) {
    for (float keyHours : War3SunStyleSettings::KeyHours) {
      const ImVec2 d = DialRingDirection(keyHours, side);
      const ImVec2 p(c.x + d.x * (rIn + 3.5f), c.y + d.y * (rIn + 3.5f));
      dl->AddQuadFilled(ImVec2(p.x, p.y - 3.5f), ImVec2(p.x + 3.5f, p.y), ImVec2(p.x, p.y + 3.5f),
                        ImVec2(p.x - 3.5f, p.y), DialColor(ImVec4(1, 1, 1, 0.9f)));
      dl->AddQuad(ImVec2(p.x, p.y - 3.5f), ImVec2(p.x + 3.5f, p.y), ImVec2(p.x, p.y + 3.5f),
                  ImVec2(p.x - 3.5f, p.y), DialColor(ImVec4(0, 0, 0, 0.6f)), 1.0f);
    }
  }
  // The time on the ring.
  {
    const ImVec2 d = DialRingDirection(hours, side);
    const ImVec2 p(c.x + d.x * rMid, c.y + d.y * rMid);
    const float r = ring * 0.36f;
    dl->AddCircleFilled(p, r, DialColor(day ? ImVec4(1.0f, 0.86f, 0.40f, 1.0f) : ImVec4(0.82f, 0.88f, 1.0f, 1.0f)), 20);
    dl->AddCircle(p, r + 1.0f, DialColor(ImVec4(0, 0, 0, 0.75f)), 20, 1.5f);
    if (active && dragMode == kDragRing)
      dl->AddCircle(p, r + 4.0f, DialColor(ImVec4(1, 1, 1, 0.85f)), 20, 2.0f);
  }

  // Under the dial: the time, where the light comes from, the shadow's length.
  const int minutes = int(std::lround(hours * 60.0f)) % (24 * 60);
  char text[96];
  std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60, minutes % 60);
  const float textTop = origin.y + size;
  DialText(dl, big, ImVec2(c.x, textTop + big * 0.5f), DialColor(ImVec4(1, 1, 1, 0.95f)), text);
  std::snprintf(text, sizeof(text), "%s%s · 光从%s照来", day ? "白天" : "夜晚", locked ? " · 已锁定" : "",
                DialBearing(now.dialX, now.dialY));
  DialText(dl, label, ImVec2(c.x, textTop + big + label * 0.55f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
  if (strength <= 0.01f)
    std::snprintf(text, sizeof(text), "%s", "光线太低，没有阴影");
  else if (lengthFactor >= 19.0f)
    std::snprintf(text, sizeof(text), "%s", "影长 19 倍以上");
  else
    std::snprintf(text, sizeof(text), "影长 %.1f 倍身高", lengthFactor);
  DialText(dl, label, ImVec2(c.x, textTop + big + label * 1.85f), DialColor(ImVec4(1, 1, 1, 0.80f)), text);
}

// The colour curve over the day, with its keys and the frame's time. A click
// on it (with the time locked) moves the time there.
void DrawSunCurveBar(War3RenderSettings& s, float hours) {
  const float width = ImGui::GetContentRegionAvail().x;
  const float height = 18.0f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##suncurve", ImVec2(width, height + ImGui::GetFontSize() + 2.0f));
  if (ImGui::IsItemActive() && s.shadows.lockSun) {
    hours = WrapHours(std::clamp((ImGui::GetIO().MousePos.x - p.x) / width, 0.0f, 0.9999f) * 24.0f);
    LockSunAt(s, hours);
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr int kSamples = 96;
  ImU32 prev = 0;
  for (int i = 0; i <= kSamples; ++i) {
    const ImU32 col = DialColor(LightSwatch(War3SunCycle::Evaluate(s, float(i) / float(kSamples)).color));
    if (i > 0) {
      const float x0 = p.x + width * float(i - 1) / float(kSamples), x1 = p.x + width * float(i) / float(kSamples);
      dl->AddRectFilledMultiColor(ImVec2(x0, p.y), ImVec2(x1 + 0.5f, p.y + height), prev, col, col, prev);
    }
    prev = col;
  }
  dl->AddRect(p, ImVec2(p.x + width, p.y + height), DialColor(ImVec4(0, 0, 0, 0.6f)), 3.0f, 0, 1.0f);
  if (s.sunStyle.customTint) {
    for (float keyHours : War3SunStyleSettings::KeyHours) {
      const float x = p.x + width * keyHours / 24.0f;
      dl->AddTriangleFilled(ImVec2(x, p.y + height - 5.0f), ImVec2(x - 4.0f, p.y + height + 2.0f),
                            ImVec2(x + 4.0f, p.y + height + 2.0f), DialColor(ImVec4(1, 1, 1, 0.9f)));
    }
  }
  const float x = p.x + width * hours / 24.0f;
  dl->AddLine(ImVec2(x, p.y - 2.0f), ImVec2(x, p.y + height + 2.0f), DialColor(ImVec4(1, 1, 1, 0.95f)), 2.0f);
  const float label = ImGui::GetFontSize() * 0.8f;
  for (int h = 0; h <= 24; h += 6) {
    char text[8];
    std::snprintf(text, sizeof(text), "%d", h);
    const float tx = std::clamp(p.x + width * float(h) / 24.0f, p.x + label * 0.5f, p.x + width - label * 0.5f);
    DialText(dl, label, ImVec2(tx, p.y + height + label * 0.75f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
  }
}

// The sun section of the shadow settings: the dial, the time controls, the
// real shadow length and the colour curve.
void DrawSunDial(War3RenderSettings& s) {
  if (SunDialTest())
    ImGui::SetScrollHereY(0.0f);
  ImGui::SeparatorText("太阳与时间");
  DrawSunDialDisc(s);

  ImGui::Checkbox("锁定时间", &s.shadows.lockSun);
  DialHelp("锁定后时间停在圆盘上太阳的位置，白天黑夜都可以。不锁定时圆盘显示游戏当前的时间。");
  {
    const struct { const char* name; float hours; } quick[] = {
        { "日出", 6.5f }, { "上午", 9.0f }, { "正午", 12.0f }, { "黄昏", 17.5f }, { "深夜", 0.0f } };
    for (size_t i = 0; i < std::size(quick); ++i) {
      if (i > 0)
        ImGui::SameLine();
      if (ImGui::SmallButton(quick[i].name))
        LockSunAt(s, quick[i].hours);
    }
  }

  auto& style = s.sunStyle;
  if (style.pathSet) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("光照方向：自定义");
    ImGui::SameLine();
    if (ImGui::SmallButton("恢复默认方向"))
      style.pathSet = false;
  } else {
    ImGui::TextDisabled("光照方向：默认（从南边照来）");
  }
  DialHelp("在圆盘里上下拖动太阳（夜里是月亮），改变它一天里走过的路线：\n"
           "路线偏下，光从南边（屏幕下方）照来，影子朝上；偏上则相反。\n"
           "离圆心越近，太阳越高。没有拖动过时用默认路线。\n"
           "太阳越低，光越暖、越暗，影子越淡。");

  ImGui::Checkbox("真实阴影长度", &style.realShadowLength);
  DialHelp("不再把阴影限制在物体高度的约 0.7 到 1.4 倍之间：影长 = 高度 ÷ tan(太阳高度)。\n"
           "日出、日落时影子会非常长，远处的阴影也会变糊。\n各类物体的“最长投影”仍然有效，可以单独截短。\n"
           "这个开关只改变影子的长度，光的颜色和明暗不变。");
  if (!style.pathSet) {
    ImGui::SliderFloat("正午太阳高度", &style.noonAltitudeDeg, 10.0f, 90.0f, "%.0f°", ImGuiSliderFlags_AlwaysClamp);
    DialHelp("太阳一天中能升到多高（月亮在午夜同样高）。\n"
             "光的颜色和明暗跟着太阳的高度变：太阳低时偏暖、偏暗。\n"
             "勾选“真实阴影长度”后，影子的长度也按这个高度算；90° 时正午正好在头顶，几乎没有影子。");
  } else {
    const float noon = std::acos(std::abs(War3SunCycle::PathNorth(style))) * (180.0f / kDialPi);
    ImGui::TextDisabled("正午太阳高度 %.0f°（由圆盘里的路线决定）", noon);
  }

  OpenSunDialForTest();
  if (ImGui::TreeNode("光色曲线")) {
    ImGui::Checkbox("自定义光色", &style.customTint);
    DialHelp("用下面六个时刻的颜色和亮度代替太阳和月亮自己的光色，时刻之间平滑过渡。\n"
             "“日出”“日落”两项是太阳在地平线上时的光：颜色会在之后一两个小时里慢慢过渡到白天的颜色\n"
             "（清晨和傍晚的金色时段），亮度则很快升上去。日月在这一刻交接，亮度宜低（默认 0.14）。\n"
             "“月光”从日落后半小时保持到日出前半小时。\n"
             "影子的颜色跟着光走：光越暖，影子里留下的天空蓝越明显。\n"
             "圆盘外圈和下面的色条就是这条曲线。");
    if (!s.dayNight.timeColorGradingEnabled)
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "这张地图关闭了随时间变色，曲线不会生效。");
    DrawSunCurveBar(s, DialCurrentHours(s));
    ImGui::BeginDisabled(!style.customTint);
    ImGui::TextUnformatted("预设");
    for (const auto& preset : kSunPresets) {
      ImGui::SameLine();
      if (ImGui::SmallButton(preset.name))
        style.tint = preset.keys;
    }
    const float keyColumn = ImGui::GetCursorPosX() + ImGui::CalcTextSize("下午 15:00").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    for (size_t k = 0; k < War3SunStyleSettings::KeyCount; ++k) {
      auto& key = style.tint[k];
      ImGui::PushID(int(k));
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%s %02d:00", kSunKeyNames[k], int(War3SunStyleSettings::KeyHours[k]));
      ImGui::SameLine(keyColumn);
      ImGui::ColorEdit3("##colour", &key.r, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 2.6f));
      ImGui::SliderFloat("##brightness", &key.intensity, 0.0f, 3.0f, "亮度 %.2f", ImGuiSliderFlags_AlwaysClamp);
      ImGui::SameLine();
      // The horizon keys show half an hour into the day: at the horizon
      // itself the light is nearly out.
      const float lookHours = k == 1u ? 6.5f : k == 5u ? 17.5f : War3SunStyleSettings::KeyHours[k];
      if (ImGui::SmallButton("看"))
        LockSunAt(s, lookHours);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(k == 1u || k == 5u ? "把时间锁定到这个时刻旁边的金色时段（%02d:%02d）" : "把时间锁定到这个时刻（%02d:%02d）",
                          int(lookHours), int((lookHours - float(int(lookHours))) * 60.0f + 0.5f));
      ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::TreePop();
  }
}

void DrawAddressSpace(const proxy::AddressSpaceSample& space) {
  ImGui::SeparatorText("魔兽争霸 III 虚拟地址空间");
  if (!space.valid) {
    ImGui::TextDisabled("查询失败。");
    return;
  }
  FullnessBar(space.lowUsed, space.lowTotal, "低 2 GiB 已用");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("游戏自身的指针必须落在这里；用满时游戏会因内存不足崩溃。");
  ImGui::Text("最大连续空闲 %.1f MiB / 进程私有提交 %.1f MiB",
      ui::BytesToMiB(space.lowLargestFree), ui::BytesToMiB(space.privateBytes));
  if (space.largeAddress) {
    FullnessBar(space.highUsed, space.highTotal, "高位 2 GiB 已用");
    ImGui::TextDisabled("高位只放本插件自己访问的数据，不能替游戏分担低 2 GiB。");
  } else {
    ImGui::TextDisabled("游戏未开启大地址（LAA）：只有低 2 GiB。");
  }
}

void DrawProxyEvidence(const proxy::DiagStatus& d) {
  namespace wd = warvk::diag;
  ImGui::TextWrapped("画面在 x64 宿主里循环保存，按 Ctrl+Shift+C 保存刚才几秒（控制台不必打开）。按下和保存完成时各有一声提示音。");
  if (!d.available) {
    ImGui::TextDisabled("诊断通道未建立。");
    return;
  }
  if (!d.hostAttached) {
    ImGui::TextDisabled("宿主尚未连接诊断通道（第一帧后自动连接）。");
    return;
  }
  switch (d.evidenceState) {
  case wd::EvidenceRecording:
    ImGui::TextColored(d.evidenceFrames ? ImVec4(.4f, 1.f, .4f, 1) : ImVec4(1.f, .8f, .2f, 1),
        d.evidenceFrames ? "采集中：已缓存 %d 帧 / %.1f 秒" : "等待进入地图（已缓存 %d 帧 / %.1f 秒）",
        d.evidenceFrames, d.evidenceMillis / 1000.0f);
    break;
  case wd::EvidenceTriggered:
    ImGui::TextUnformatted("已触发：保留后续几帧");
    break;
  case wd::EvidenceSaving:
    ImGui::Text("正在保存图片 %d / %d，请勿退出", d.evidenceSaved, d.evidenceTotal);
    break;
  case wd::EvidenceSaved:
    ImGui::TextColored(ImVec4(.4f, 1.f, .4f, 1), "已保存 %d 帧；继续采集中（缓存 %d 帧）", d.evidenceSaved, d.evidenceFrames);
    break;
  case wd::EvidenceFailed:
    ImGui::TextColored(ImVec4(1, .3f, .3f, 1), "保存失败：%s", d.error.c_str());
    break;
  default:
    ImGui::TextDisabled("帧取证已关闭%s%s", d.error.empty() ? "" : "：", d.error.c_str());
    break;
  }
  if (d.evidenceState != wd::EvidenceTriggered && d.evidenceState != wd::EvidenceSaving &&
      ImGui::Button("保存最近几秒的取证包"))
    proxy::DiagRequestEvidence();
  if (!d.evidencePath.empty())
    ImGui::TextWrapped("最近的取证包：%s", d.evidencePath.c_str());
  else
    ImGui::TextWrapped("保存位置：%s", d.outputDirectory.c_str());
}

void DrawProxyRenderStats(const proxy::DiagStatus& d, const proxy::FrameStatus& frame,
                          const proxy::AddressSpaceSample& space) {
  const auto mib = [](uint64_t value) { return ui::BytesToMiB(value); };
  const auto u64 = [](uint64_t value) { return static_cast<unsigned long long>(value); };
  const auto& h = d.stats;
  ImGui::SeparatorText("代理模式内存占用");
  if (!d.hostAttached || !d.statsValid) {
    ImGui::TextColored(ImVec4(1.f, .8f, .2f, 1.f), "宿主尚未发布统计（第一帧后自动连接）。");
  } else {
    ImGui::Text("x64 宿主 warvk-d3d9-host.exe（PID %d）", d.hostPid);
    ImGui::Text("宿主进程：私有 %.1f / 峰值 %.1f / 工作集 %.1f MiB",
        mib(h.processPrivate), mib(h.processPeakPrivate), mib(h.processWorkingSet));
    FullnessBar(h.localCommitted, h.localBudget ? h.localBudget : h.localHeap, "宿主显存");
    ImGui::Text("显存分配器：已分配 %.1f / 使用中 %.1f MiB（显存总量 %.0f MiB）",
        mib(h.localAllocated), mib(h.localUsed), mib(h.localHeap));
    ImGui::Text("共享内存：占用 %.1f / 已分配 %.1f / 使用中 %.1f MiB",
        mib(h.sharedCommitted), mib(h.sharedAllocated), mib(h.sharedUsed));
    ImGui::Text("其中 阴影 %.1f / 后处理 %.1f / 体积光 %.1f / 帧取证缓存 %.1f MiB",
        mib(h.shadowBytes), mib(h.effectBytes), mib(h.volumetricBytes), mib(h.evidenceRingBytes));
    ImGui::TextDisabled("这些内存都在 x64 宿主和显卡上，不占魔兽的地址空间。");

    ImGui::SeparatorText("宿主渲染");
    const float averageMs = h.averageIntervalUs / 1000.0f;
    ImGui::Text("%u x %u / %.1f FPS（平均 %.2f ms，最近一帧 %.2f ms）", h.width, h.height,
        averageMs > 0.f ? 1000.0f / averageMs : 0.f, averageMs, h.intervalUs / 1000.0f);
    ImGui::Text("呈现 %llu 帧 / 世界帧 %llu / 本帧 draws %u，批次 %u",
        u64(h.frames), u64(h.worldFrames), h.draws, h.drawBatches);
    ImGui::Text("后处理：SSAO %s / 雪 %s / 抗锯齿 %s / Bloom %s",
        (h.effectsRecorded & 1u) ? "是" : "否", (h.effectsRecorded & 2u) ? "是" : "否",
        (h.effectsRecorded & 4u) ? "是" : "否", (h.effectsRecorded & 8u) ? "是" : "否");

    ImGui::SeparatorText("宿主阴影（最近一个世界帧）");
    ImGui::Text("运行 %u / 图 serial %llu / 完整图 %u / 本帧更新 %u / 接收端绘制 %u",
        h.shadowRan, u64(h.mapRenderSerial), h.completeMap, h.mapExecuted, h.receiverExecuted);
    ImGui::Text("caster 收到 %u / 拒绝 %u / 复制 %u（%.2f MiB）",
        h.casters, h.refused, h.copied, mib(h.copiedBytes));
    ImGui::Text("回放 draws %u / 绘制 caster %u / 级联 %u：%u | %u | %u | %u", h.replayDraws, h.drawnCasters,
        h.cascades, h.cascadeDrawn[0], h.cascadeDrawn[1], h.cascadeDrawn[2], h.cascadeDrawn[3]);
    ImGui::Text("沿用旧图 %u：CSM 无效 %u / 空回放 %u / 身份抖动 %u", h.reuseMap, h.holdInvalidCsm,
        h.holdEmptyReplay, h.holdIdentityChurn);
    ImGui::Text("CSM 回退 %u / 无完整图 %u / 缺资源跳过 %u / 阻止不完整 %u", h.csmFallback,
        h.noCompleteMap, h.skippedNoResources, h.partialPrevented);
    ImGui::Text("早退码 %u / 输入拒绝码 %u / 回放拒绝 %u（原因 %u）", h.earlyReturn, h.inputReject,
        h.replayRejected, h.replayLastReason);
    ImGui::Text("TAA 模式 %u，历史 %u / 点光阴影 %u / 描边 %u / 体积光 %u，合成 %u / Alpha caster %u", h.taaMode,
        h.taaHistory, h.pointShadow, h.outline, h.volumetric, h.volumetricComposite, h.alphaPrepared);
    if (h.auditOdd)
      ImGui::TextColored(ImVec4(1.f, .4f, .2f, 1.f), "caster 几何检查：%u 个中 %u 个异常", h.auditChecked, h.auditOdd);
    else
      ImGui::Text("caster 几何检查：%u 个，无异常", h.auditChecked);
  }

  ImGui::SeparatorText("游戏侧数据层");
  ImGui::Text("帧 %llu / 世界场景 %llu / 插入 %llu（在 Present %llu）", u64(frame.frames),
      u64(frame.worldScenes), u64(frame.insertions), u64(frame.insertionsAtPresent));
  ImGui::Text("无相机帧 %llu / 沿用上帧相机 %llu / 记录 %llu / 被拒 %llu%s", u64(frame.framesWithoutCamera),
      u64(frame.cameraFallbacks), u64(frame.records), u64(frame.refused), frame.sendingStopped ? "（已停止发送）" : "");
  const auto& c = frame.casters;
  ImGui::Text("最近一帧 caster：考察 %u / 发送 %u（地形 %u，装饰 %u，世界物 %u，单位 %u）", c.lastFrame.considered,
      c.lastFrame.sent, c.lastFrame.terrain, c.lastFrame.doodads, c.lastFrame.worldObjects, c.lastFrame.units);
  ImGui::Text("累计发送 %llu / 带省略的帧 %llu", u64(c.sent), u64(c.omittedFrames));
  (void)space;
}

} // namespace

void War3Imgui::drawFrameRecorderPanel() {
  OpenSectionForTest();
  if (!ImGui::CollapsingHeader("帧取证 (Ctrl+Shift+C)"))
    return;
  if (m_proxy) {
    DrawProxyEvidence(proxy::DiagQuery());
    return;
  }

  // Display-only snapshot: capture and resource lifetime remain recorder-owned.
  const auto recorder = tools::QueryFrameHistoryHud();
  ImGui::TextWrapped("Ctrl+F1 或窗口关闭按钮仅隐藏控制台，不停止采集、不释放取证内存。");
  ImGui::TextWrapped("完全停用取证：启动前设置 DXVK_WAR3_FRAME_EVIDENCE=0，并重新启动游戏。");
  if (!recorder.enabled) {
    ImGui::TextDisabled("本进程的帧取证已关闭；修改环境变量需要重新启动。");
    return;
  }
  if (tools::evidence::InternalRecorderBuild())
    ImGui::TextUnformatted("内部诊断构建：默认开启取证（有内存/显存开销）");
  switch (recorder.state) {
  case 0:
    ImGui::TextUnformatted(recorder.selfContained ? "内置采集：等待进入地图" : "外部控制模式：等待采集程序");
    break;
  case 1:
    ImGui::TextUnformatted("正在准备显存缓冲，请稍候");
    break;
  case 2:
    ImGui::TextColored(recorder.bufferedMs >= recorder.requiredMs ? ImVec4(.4f, 1.f, .4f, 1) : ImVec4(1.f, .8f, .2f, 1),
        "采集中：%.2f 秒 / 目标 %.2f 秒", recorder.bufferedMs / 1000.0f, recorder.requiredMs / 1000.0f);
    break;
  case 3:
    ImGui::TextUnformatted("已触发：保留后续帧");
    break;
  case 4: case 5:
    ImGui::Text("正在保存图片 %u / %u，请勿退出", recorder.saved, recorder.total);
    break;
  case 6:
    ImGui::TextUnformatted(recorder.packageReady ? "原始包已保存，可退出；完整性待离线分析" : "图片已保存，正在整理日志，请勿退出");
    break;
  case 7:
    ImGui::TextColored(ImVec4(1, .3f, .3f, 1), "采集失败：%s", recorder.error);
    break;
  default:
    ImGui::TextUnformatted("采集会话正在清理");
    break;
  }
  if (recorder.selfContained)
    ImGui::TextUnformatted("DLL 独立采集，无需后台连接；每进程保留一份事件");
  else if (recorder.state == 2 && !recorder.watcherAlive)
    ImGui::TextColored(ImVec4(1, .4f, .2f, 1), "外部控制已断开；该模式需要 watcher");
  if (recorder.notice == 2)
    ImGui::TextUnformatted("已收到快捷键，但当前未处于采集状态");
}

void War3Imgui::drawDebugWindow() {
  ImGuiIO &io = ImGui::GetIO();

  // Panel state outside War3RenderSettings that the player may save.
  const auto currentExtras = []() {
    player::Extras extras;
    extras.fpsUnlocked = isFpsUnlocked();
    extras.nativeShadowMode = War3RenderState::GetNativeShadowMode();
    return extras;
  };
  const player::Extras extrasBefore = currentExtras();
  bool recordExtras = true;

  // Allow resizing (removed ImGuiWindowFlags_AlwaysAutoResize)
  if (ImGui::Begin("War3VK 调试器", &m_visible, ImGuiWindowFlags_None)) {
    ImGui::Text("帧率 (FPS): %.1f (%.3f ms)", io.Framerate,
                1000.0f / io.Framerate);

    // FPS 解锁控制
    ImGui::SameLine();
    bool fpsUnlocked = isFpsUnlocked();
    // Proxy mode: the present interval is fixed when the device is created
    // (war3_proxy_mode.cpp); switching it while running is not wired.
    if (m_proxy)
      ImGui::BeginDisabled();
    if (ImGui::Checkbox("突破帧率上限", &fpsUnlocked)) {
      setFpsUnlocked(fpsUnlocked);
    }
    if (m_proxy)
      ImGui::EndDisabled();
    if (m_proxy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("代理模式：呈现间隔在创建设备时决定，运行中切换尚未接入");
    } else if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("禁用 VSync 同步，允许帧率超过显示器刷新率");
    }

    // [Perf] Start/Stop Performance Logging
    ImGui::SameLine();
    auto &perf = War3PerfMonitor::instance();
    // Proxy mode: the frame timing lives in the host, which writes the log.
    const proxy::DiagStatus diag = m_proxy ? proxy::DiagQuery() : proxy::DiagStatus();
    const bool recording = m_proxy ? diag.perfWanted : perf.isRecording();
    const char *label = recording ? "结束性能日志记录" : "开始性能日志记录";
    const bool perfUnavailable = m_proxy && !diag.hostAttached;
    if (perfUnavailable)
      ImGui::BeginDisabled();
    const bool perfClicked = ImGui::Button(label);
    if (perfUnavailable)
      ImGui::EndDisabled();
    if (m_proxy) {
      if (perfClicked)
        proxy::DiagSetPerfRecording(!recording);
      if (recording) {
        ImGui::SameLine();
        ImGui::TextDisabled("记录中 %d 帧", diag.perfRows);
      } else if (diag.perfState == warvk::diag::PerfFailed) {
        ImGui::TextColored(ImVec4(1, .3f, .3f, 1), "性能日志写入失败：%s", diag.outputDirectory.c_str());
      } else if (diag.perfState == warvk::diag::PerfWritten && !diag.perfPath.empty()) {
        ImGui::TextWrapped("性能日志已保存：%s", diag.perfPath.c_str());
      }
    } else if (perfClicked) {
      if (!recording) {
        perf.setRecording(true);
        perf.resetHistory();
      } else {
        perf.setRecording(false);
        perf.exportHtmlReport("war3_perf_report.html");
        perf.resetHistory();
        perf.cleanupTempFolder();
      }
    }
    if (recording && !m_proxy) {
      ImGui::SameLine();
      ImGui::TextDisabled("记录中...");
    }
    const auto pendingExports = perf.getPendingExportCount();
    if (pendingExports > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("导出中...");
    }

    ImGui::Separator();

    drawFrameRecorderPanel();

    if (ImGui::CollapsingHeader("着色器系统 (Shader System)")) {
      if (ImGui::Button("重载配置 (Reload Configs)")) {
        ShaderManager::get().reload();
      }

      auto &packs = ShaderManager::get().getPacks();
      if (packs.empty()) {
        ImGui::TextDisabled("未加载着色器包 (No Packs)");
      } else {
        int packIdx = 0;
        for (auto &pack : packs) {
          ImGui::PushID(packIdx++);
          bool enabled = pack.enabled;
          if (ImGui::Checkbox(pack.name.c_str(), &enabled)) {
            pack.enabled = enabled;
            ShaderManager::get().rebuildCache();
          }

          // Show Materials
          if (pack.enabled) {
            ImGui::Indent();
            int matIdx = 0;
            for (const auto &kv : pack.materials) {
              auto stage = kv.first;
              auto mat = kv.second;

              ImGui::PushID(matIdx++);
              ImGui::Text("阶段 %d: %s", (int)stage, mat->getName().c_str());
              ImGui::SameLine();

              // Status Label
              if (mat->isCompiled()) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "[已编译]");
              } else if (mat->hasCompileFailure()) {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "[失败]");
                if (!mat->getLastError().empty() && ImGui::IsItemHovered()) {
                  ImGui::SetTooltip("%s", mat->getLastError().c_str());
                }
              } else {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "[等待]");
              }

              // Compile Button (Always visible; proxy mode has no device to
              // compile on here)
              ImGui::SameLine();
              if (!m_device)
                ImGui::BeginDisabled();
              const bool compileClicked = ImGui::Button("编译");
              if (!m_device)
                ImGui::EndDisabled();
              if (compileClicked) {
                Logger::info(str::format("War3Imgui: Compile clicked for ",
                                         mat->getName()));
                if (m_device) {
                  bool result = mat->compile(m_device, true);
                  Logger::info(str::format("War3Imgui: Result = ", result));
                } else {
                  Logger::err("War3Imgui: Device is NULL");
                }
              }
              ImGui::PopID();
            }
            ImGui::Unindent();
          }
          ImGui::PopID();
        }
      }
    }

    OpenSunDialForTest();
    OpenWaterForTest();
    if (ImGui::CollapsingHeader("内置渲染设置")) {
      // Proxy mode: the settings mailbox is the data layer's own and the host
      // takes it with the frame's records; there is no pipeline here.
      if (!m_proxy && (!m_device || !m_device->GetWar3Pipeline())) {
        ImGui::TextDisabled("渲染管线未就绪");
      } else {
        auto settingsWrite = dxvk::war3::GetMutableSettings();
        if (!settingsWrite)
          ImGui::TextDisabled("渲染设置暂不可写");
        if (settingsWrite) {
        auto &settings = *settingsWrite;
        // The guard is held for the whole header, so the difference between
        // this copy and the final state is exactly what the panel changed this
        // frame. Map/JAPI writes use their own guards and are never recorded.
        const War3RenderSettings settingsBefore = settings;
        bool recordSettings = true;

        if (ImGui::Button("恢复默认设置")) {
          player::Extras extras = currentExtras();
          if (player::ResetToBaseline(settings, extras)) {
            setFpsUnlocked(extras.fpsUnlocked);
            War3RenderState::SetNativeShadowMode(extras.nativeShadowMode);
          }
          recordSettings = false;
          recordExtras = false;
        }
        ImGui::SameLine();
        {
          const auto status = player::GetStoreStatus();
          if (status.lastSaveFailed)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                               "保存失败：WarVK\\settings.ini 无法写入");
          else if (status.savedKeys == 0u)
            ImGui::TextDisabled("修改会自动保存，下次启动沿用");
          else
            ImGui::TextDisabled("已保存 %u 项到 WarVK\\settings.ini%s",
                                unsigned(status.savedKeys),
                                status.pending ? "（待写入）" : "");
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "只记录你在面板里改过的项。地图脚本仍可在游戏中临时修改这些设置，"
              "\n那些修改不会被保存。删除 settings.ini 也能恢复默认。");
        }

        ImGui::Checkbox("光影", &settings.shadows.enabled);

        if (ImGui::TreeNode("Shadow Debug Items")) {
          ImGui::Checkbox("Gr 0: Units",
                          &dxvk::war3::internal::kShadowRenderGroup0);
          ImGui::Checkbox("Gr 1: Buildings (Original)",
                          &dxvk::war3::internal::kShadowRenderGroup1);
          ImGui::Checkbox("Gr 2: Effects",
                          &dxvk::war3::internal::kShadowRenderGroup2);
          ImGui::Checkbox("Gr 3: ShadowCasters (Fix)",
                          &dxvk::war3::internal::kShadowRenderGroup3);
          ImGui::Separator();
          ImGui::Checkbox("S10: 装饰物阴影 (Decorations)",
                          &dxvk::war3::internal::kShadowRenderDecorations);
          ImGui::Separator();
          ImGui::Text("路径阻断器过滤:");
          ImGui::Checkbox("诊断日志##pathdebug",
                          &dxvk::war3::internal::kPathBlockerDebugEnabled);
          ImGui::Checkbox("隐藏路径阻断器##pathhide",
                          &dxvk::war3::internal::kPathBlockerHideEnabled);
          ImGui::TreePop();
        }

        // Stage Debug Tool
        if (ImGui::TreeNode("Stage Debug Tool")) {
          ImGui::Checkbox("启用 Stage 过滤",
                          &dxvk::war3::internal::kStageDebugEnabled);
          ImGui::TextDisabled("勾选要渲染的 Stage:");

          // 分成 5 行，每行 5 个 stage
          for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 5; col++) {
              int idx = row * 5 + col;
              char label[16];
              snprintf(label, sizeof(label), "S%d", idx);
              ImGui::Checkbox(label, &dxvk::war3::internal::kStageDebug[idx]);
              if (col < 4)
                ImGui::SameLine();
            }
          }

          // 快捷按钮
          if (ImGui::Button("全选")) {
            for (int i = 0; i < 25; i++)
              dxvk::war3::internal::kStageDebug[i] = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("全不选")) {
            for (int i = 0; i < 25; i++)
              dxvk::war3::internal::kStageDebug[i] = false;
          }
          ImGui::SameLine();
          if (ImGui::Button("反选")) {
            for (int i = 0; i < 25; i++)
              dxvk::war3::internal::kStageDebug[i] =
                  !dxvk::war3::internal::kStageDebug[i];
          }

          ImGui::TreePop();
        }
        ImGui::SameLine();
        ImGui::Checkbox("点光源##enable", &settings.shadows.pointLightsEnabled);
        ImGui::BeginDisabled(!settings.shadows.pointLightsEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("点光源阴影##enable",
                        &settings.shadows.pointShadowEnabled);
        ImGui::EndDisabled();
        ImGui::Checkbox("被遮挡描边", &settings.occludedOutline.enabled);

        if (ImGui::TreeNode("描边参数")) {
          bool debugAllObjects =
              War3RenderState::IsOutlineDebugAllObjectsEnabled();
          if (ImGui::Checkbox("调试：强制所有对象描边", &debugAllObjects)) {
            // A debug action, not a player choice: do not save its side effects.
            recordSettings = false;
            War3RenderState::SetOutlineDebugAllObjectsEnabled(debugAllObjects);

            // 按你的验证需求：强制所有目标、关闭常驻描边、仅显示被遮挡描边
            if (debugAllObjects) {
              settings.occludedOutline.enabled = true;
              settings.occludedOutline.useScreenSpace = true;
              settings.occludedOutline.showVisible = false;
              settings.occludedOutline.showOccluded = true;
              settings.occludedOutline.mode = War3OutlineMode::Silhouette;
              War3RenderState::SetOutlineForceEnabled(true);
            }
          }
          ImGui::SameLine();
          ImGui::TextDisabled("handles=%u last=0x%08X",
                              War3RenderState::GetOutlineHandleCount(),
                              War3RenderState::GetLastRenderHandle());

          if (ImGui::Button("清空描边句柄")) {
            War3RenderState::ClearOutlineHandles();
          }

          ImGui::BeginDisabled(!settings.occludedOutline.enabled);
          {
            const char *outlineModes[] = {"遮挡填充", "轮廓描边"};
            int outlineMode = static_cast<int>(settings.occludedOutline.mode);
            if (ImGui::Combo("描边模式", &outlineMode, outlineModes,
                             IM_ARRAYSIZE(outlineModes))) {
              outlineMode = std::clamp(outlineMode, 0, 1);
              settings.occludedOutline.mode =
                  static_cast<War3OutlineMode>(outlineMode);
            }

            ImGui::Checkbox("屏幕空间描边",
                            &settings.occludedOutline.useScreenSpace);
            ImGui::SliderFloat("描边宽度(px)",
                               &settings.occludedOutline.widthPx, 1.0f, 10.0f,
                               "%.1f");
            ImGui::Checkbox("常驻显示(可见)",
                            &settings.occludedOutline.showVisible);
            ImGui::Checkbox("被遮挡显示",
                            &settings.occludedOutline.showOccluded);

            float col[4] = {
                settings.occludedOutline.colorR,
                settings.occludedOutline.colorG,
                settings.occludedOutline.colorB,
                settings.occludedOutline.colorA,
            };
            if (ImGui::ColorEdit4("描边颜色", col,
                                  ImGuiColorEditFlags_AlphaBar |
                                      ImGuiColorEditFlags_Float)) {
              settings.occludedOutline.colorR = std::clamp(col[0], 0.0f, 1.0f);
              settings.occludedOutline.colorG = std::clamp(col[1], 0.0f, 1.0f);
              settings.occludedOutline.colorB = std::clamp(col[2], 0.0f, 1.0f);
              settings.occludedOutline.colorA = std::clamp(col[3], 0.0f, 1.0f);
            }
          }
          ImGui::EndDisabled();

          ImGui::TreePop();
        }
        ImGui::Checkbox("后处理总开关", &settings.postFx.enabled);

        if (ImGui::TreeNode("光照")) {
          ImGui::Checkbox("太阳直射光", &settings.sun.enabled);
          ImGui::SliderFloat("太阳强度", &settings.sun.intensity, 0.0f, 3.0f,
                             "%.2f");
          ImGui::TreePop();
        }

        OpenSunDialForTest();
        if (ImGui::TreeNode("光影参数")) {
          ImGui::SliderFloat("强度", &settings.shadows.strength, 0.0f, 1.0f,
                             "%.2f");
          ImGui::SliderFloat("PCF 半径", &settings.shadows.pcfRadius, 0.5f,
                             6.0f, "%.2f");
          const char *pcfKernels[] = {"3x3", "5x5", "Poisson16", "Poisson25"};
          int pcfKernel = static_cast<int>(settings.shadows.pcfKernel);
          if (ImGui::Combo("PCF 核", &pcfKernel, pcfKernels,
                           IM_ARRAYSIZE(pcfKernels))) {
            settings.shadows.pcfKernel =
                static_cast<War3ShadowPcfKernel>(pcfKernel);
          }
          ImGui::BeginDisabled(pcfKernel < 2);
          ImGui::Checkbox("PCF 旋转", &settings.shadows.pcfRotate);
          ImGui::BeginDisabled(!settings.shadows.pcfRotate || pcfKernel < 2);
          const char *rotateModes[] = {"关闭", "屏幕", "世界"};
          int rotateMode =
              settings.shadows.pcfRotate
                  ? static_cast<int>(settings.shadows.pcfRotateMode)
                  : 0;
          if (ImGui::Combo("PCF 旋转模式", &rotateMode, rotateModes,
                           IM_ARRAYSIZE(rotateModes))) {
            rotateMode = std::clamp(rotateMode, 0, 2);
            settings.shadows.pcfRotateMode =
                static_cast<War3ShadowPcfRotateMode>(rotateMode);
            settings.shadows.pcfRotate = (rotateMode != 0);
          }
          ImGui::EndDisabled();
          ImGui::EndDisabled();
          ImGui::SliderFloat("偏移", &settings.shadows.receiverBias, 0.0f,
                             0.02f, "%.4f");
          ImGui::SliderFloat("级联偏移缩放", &settings.shadows.cascadeBiasScale,
                             0.0f, 1.0f, "%.2f");
          ImGui::SliderFloat("PCF 级联锐化",
                             &settings.shadows.pcfCascadeRadiusScale, 0.0f,
                             1.0f, "%.2f");
          ImGui::SliderFloat("投射器偏移", &settings.shadows.casterDepthBias,
                             0.0f, 4.0f, "%.2f");
          ImGui::SliderFloat("投射器斜率偏移",
                             &settings.shadows.casterSlopeBias, 0.0f, 4.0f,
                             "%.2f");
          ImGui::SliderFloat("投射器偏移上限",
                             &settings.shadows.casterBiasClamp, 0.0f, 0.01f,
                             "%.4f");
          int cascadeCount = std::clamp<int>(
              static_cast<int>(settings.shadows.csm.cascadeCount), 1, 4);
          if (ImGui::SliderInt("CSM 级联数", &cascadeCount, 1, 4)) {
            settings.shadows.csm.cascadeCount =
                static_cast<uint32_t>(cascadeCount);
          }
          {
            const uint32_t resValues[] = {1024u, 2048u, 4096u};
            const char *resItems[] = {"1024", "2048", "4096"};
            int resIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(resValues); i++) {
              if (settings.shadows.csm.shadowResolution == resValues[i]) {
                resIndex = i;
                break;
              }
            }
            if (ImGui::Combo("CSM 分辨率", &resIndex, resItems,
                             IM_ARRAYSIZE(resItems))) {
              resIndex = std::clamp(resIndex, 0, IM_ARRAYSIZE(resValues) - 1);
              settings.shadows.csm.shadowResolution = resValues[resIndex];
            }
          }
          ImGui::SliderFloat("CSM 最大距离", &settings.shadows.csm.maxDistance,
                             1000.0f, 8000.0f, "%.0f");
          ImGui::SliderFloat("CSM Split Lambda",
                             &settings.shadows.csm.splitLambda, 0.0f, 1.0f,
                             "%.2f");
          {
            const char *fitModes[] = {"稳定(球体)", "紧凑(AABB)"};
            int fitMode = static_cast<int>(settings.shadows.csm.fitMode);
            if (ImGui::Combo("CSM 拟合", &fitMode, fitModes,
                             IM_ARRAYSIZE(fitModes))) {
              fitMode = std::clamp(fitMode, 0, 1);
              settings.shadows.csm.fitMode =
                  static_cast<War3CsmFitMode>(fitMode);
            }
          }
          ImGui::SliderFloat("CSM 深度外扩",
                             &settings.shadows.csm.depthRangeMargin, 0.0f,
                             500.0f, "%.1f");
          DrawSunDial(settings);
          ImGui::Separator();
          ImGui::Checkbox("太阳移动稳定Snap",
                          &settings.shadows.stableSnapWhenSunMoving);
          const char *nativeShadowModes[] = {"完整", "仅雾/边界", "禁用"};
          int nativeShadowMode =
              static_cast<int>(War3RenderState::GetNativeShadowMode());
          if (ImGui::Combo("原生阴影", &nativeShadowMode, nativeShadowModes,
                           IM_ARRAYSIZE(nativeShadowModes))) {
            War3RenderState::SetNativeShadowMode(
                static_cast<uint32_t>(nativeShadowMode));
          }
          const char *shadowModes[] = {"PCF", "PCSS"};
          int shadowMode = settings.shadows.pcssEnabled ? 1 : 0;
          if (ImGui::Combo("阴影方案", &shadowMode, shadowModes,
                           IM_ARRAYSIZE(shadowModes))) {
            settings.shadows.pcssEnabled = (shadowMode == 1);
          }
          const char *receiverModes[] = {"Legacy", "NormalBias", "Adaptive"};
          int receiverMode = static_cast<int>(settings.shadows.receiverMode);
          if (ImGui::Combo("接收器模式", &receiverMode, receiverModes,
                           IM_ARRAYSIZE(receiverModes))) {
            settings.shadows.receiverMode =
                static_cast<War3ShadowReceiverMode>(receiverMode);
          }
          if (settings.shadows.receiverMode != War3ShadowReceiverMode::Legacy) {
            ImGui::SliderFloat("法线偏移比例",
                               &settings.shadows.normalBiasScale, 0.0f, 0.01f,
                               "%.4f");
          }
          const char *filterModes[] = {"Nearest", "Linear"};
          int filterMode = static_cast<int>(settings.shadows.filterMode);
          if (ImGui::Combo("阴影采样", &filterMode, filterModes,
                           IM_ARRAYSIZE(filterModes))) {
            settings.shadows.filterMode =
                static_cast<War3ShadowFilterMode>(filterMode);
          }
          // The band the shadow altitude is kept in; the real shadow length
          // (the sun dial) has none.
          ImGui::BeginDisabled(settings.sunStyle.realShadowLength);
          const char *altitudeModes[] = {"真实高度", "时间线性"};
          int altitudeMode = static_cast<int>(settings.shadows.altitudeMode);
          if (ImGui::Combo("阴影高度曲线", &altitudeMode, altitudeModes,
                           IM_ARRAYSIZE(altitudeModes))) {
            settings.shadows.altitudeMode =
                static_cast<War3ShadowAltitudeMode>(altitudeMode);
          }
          ImGui::EndDisabled();
          ImGui::TextUnformatted("各类物体的投影");
          ImGui::TextWrapped("勾选后投影。最长投影：阴影最多为物体高度的几倍，0 为不限。"
                             "树木、门、箱子属于可破坏物，地形上的装饰属于装饰物。");
          const char* classNames[] = {"单位", "建筑", "装饰物", "可破坏物", "其他"};
          static_assert(IM_ARRAYSIZE(classNames) == std::tuple_size_v<War3CasterClasses>);
          for (size_t i = 0; i < IM_ARRAYSIZE(classNames); ++i) {
            auto& cls = settings.casterClasses[i];
            ImGui::PushID(int(i));
            ImGui::Checkbox(classNames[i], &cls.castShadow);
            ImGui::SameLine(100.0f);
            ImGui::SliderFloat("最长投影", &cls.maxShadowLength, 0.0f, 8.0f,
                cls.maxShadowLength > 0 ? "%.2f 倍" : "不限", ImGuiSliderFlags_AlwaysClamp);
            ImGui::PopID();
          }
          ImGui::BeginDisabled(settings.sunStyle.realShadowLength);
          ImGui::SliderFloat("阴影长度缩放",
                             &settings.shadows.shadowLengthScale, 0.6f, 1.2f,
                             "%.2f");
          ImGui::SliderFloat("阴影最长缩放",
                             &settings.shadows.shadowMaxLengthScale, 0.5f, 1.2f,
                             "%.2f");
          ImGui::EndDisabled();
          ImGui::SliderFloat("夜间阴影倍率", &settings.shadows.nightShadowScale,
                             0.0f, 1.0f, "%.2f");
          ImGui::SliderFloat("轮廓光强度", &settings.shadows.rimIntensity, 0.0f,
                             1.5f, "%.2f");
          ImGui::BeginDisabled(settings.shadows.rimIntensity <= 0.0f);
          ImGui::SliderFloat("轮廓光指数", &settings.shadows.rimPower, 0.2f,
                             6.0f, "%.2f");
          ImGui::EndDisabled();
          if (settings.shadows.pcssEnabled) {
            const char *pcssSearchKernels[] = {"3x3", "5x5"};
            int pcssSearchKernel =
                static_cast<int>(settings.shadows.pcssSearchKernel);
            if (ImGui::Combo("PCSS 搜索核", &pcssSearchKernel,
                             pcssSearchKernels,
                             IM_ARRAYSIZE(pcssSearchKernels))) {
              settings.shadows.pcssSearchKernel =
                  static_cast<War3ShadowPcssSearchKernel>(pcssSearchKernel);
            }
            ImGui::SliderFloat("PCSS 搜索半径",
                               &settings.shadows.pcssSearchRadius, 1.0f, 12.0f,
                               "%.2f");
            ImGui::SliderFloat("PCSS 最小半径", &settings.shadows.pcssMinRadius,
                               0.5f, 4.0f, "%.2f");
            ImGui::SliderFloat("PCSS 最大半径", &settings.shadows.pcssMaxRadius,
                               1.0f, 8.0f, "%.2f");
            ImGui::SliderFloat("PCSS 深度缩放",
                               &settings.shadows.pcssDepthScale, 10.0f, 80.0f,
                               "%.2f");
          }
          ImGui::Checkbox("Alpha 阴影 Hash",
                          &settings.shadows.alphaShadowHashed);
          ImGui::Checkbox("Alpha 阴影 Mip",
                          &settings.shadows.alphaShadowUseMip);
          ImGui::BeginDisabled(!settings.shadows.alphaShadowUseMip);
          ImGui::SliderFloat("Alpha Mip LOD Bias",
                             &settings.shadows.alphaShadowMipLodBias, -2.0f,
                             4.0f, "%.2f");
          ImGui::EndDisabled();
          ImGui::SliderFloat("Alpha 远级联阈值偏移",
                             &settings.shadows.alphaShadowFarAlphaRefBias, 0.0f,
                             0.35f, "%.3f");
          ImGui::Separator();
          const char* shadowTaaModes[] = {
              "DirectInline（无历史）",
              "PrepassCurrentOnly（同源无历史）",
              "Temporal（TAA v2）",
          };
          int shadowTaaMode =
              static_cast<int>(settings.shadows.shadowTaaMode);
          if (ImGui::Combo("阴影时域模式", &shadowTaaMode,
                           shadowTaaModes, IM_ARRAYSIZE(shadowTaaModes))) {
            shadowTaaMode = std::clamp(shadowTaaMode, 0, 2);
            settings.shadows.shadowTaaMode =
                static_cast<War3ShadowTaaMode>(shadowTaaMode);
            // Keep the old binary setting as a compatibility mirror only.
            settings.shadows.shadowTaaEnabled =
                settings.shadows.shadowTaaMode ==
                War3ShadowTaaMode::Temporal;
            ++settings.shadows.shadowTaaSettingsRevision;
          }
          const auto taaDiagnostics = ProcessServices().QueryShadowTaaDiagnostics();
          ImGui::Text(
              "TAA requested/effective/shader: %u / %u / %u",
              taaDiagnostics.requestedMode, taaDiagnostics.effectiveMode,
              taaDiagnostics.shaderMode);
          ImGui::Text(
              "history valid/readable: %u / %u  gen=%llu  invalidate=0x%X",
              taaDiagnostics.historyValid, taaDiagnostics.historyReadable,
              static_cast<unsigned long long>(
                  taaDiagnostics.historyGeneration),
              taaDiagnostics.lastInvalidationReason);
          ImGui::Text("固定墙旁路帧: %llu  settings rev=%llu",
                      static_cast<unsigned long long>(
                          taaDiagnostics.fixedWallBypassCount),
                      static_cast<unsigned long long>(
                          taaDiagnostics.settingsRevision));
          const auto csmDiagnostics = ProcessServices().QueryCsmResolutionDiagnostics();
          ImGui::Text("CSM requested/effective: %u / %u  generation=%llu",
                      csmDiagnostics.requestedResolution,
                      csmDiagnostics.effectiveResolution,
                      static_cast<unsigned long long>(
                          csmDiagnostics.resourceGeneration));
          ImGui::Text("CSM fallback: %s  reason=%u  rebuilds=%llu",
                      csmDiagnostics.fallbackLatched ? "latched" : "none",
                      csmDiagnostics.fallbackReason,
                      static_cast<unsigned long long>(
                          csmDiagnostics.resourceRebuildCount));
          if (ImGui::Button("保留最近阴影证据"))
            dxvk::war3::tools::RequestShadowEvidenceRetention();
          ImGui::SameLine();
          ImGui::Text("采集器: %s",
                      dxvk::war3::tools::IsShadowEvidenceCollectorAttached()
                          ? "已连接"
                          : "未连接");
          ImGui::BeginDisabled(
              settings.shadows.shadowTaaMode !=
              War3ShadowTaaMode::Temporal);
          ImGui::SliderFloat("TAA 混合因子(新帧权重)",
                             &settings.shadows.shadowTaaBlendFactor, 0.12f,
                             0.30f, "%.3f");
          ImGui::Checkbox("TAA 邻域夹紧",
                          &settings.shadows.shadowTaaNeighborClamp);
          ImGui::EndDisabled();
            const char *debugItems[] = {"关闭", "级联",     "阴影因子",
                                        "深度", "运动向量", "阴影历史",
                                        "点阴影", "本帧阴影",
                                        "本帧阴影覆盖", "CSM失败原因"};
          int debugIndex = static_cast<int>(settings.shadows.debugMode);
          if (ImGui::Combo("调试输出", &debugIndex, debugItems,
                           IM_ARRAYSIZE(debugItems))) {
            settings.shadows.debugMode =
                static_cast<War3ShadowDebugMode>(debugIndex);
          }
          ImGui::TreePop();
        }

        OpenWaterForTest();
        if (ImGui::TreeNode("水面##panel")) {
          if (WaterPanelTest())
            ImGui::SetScrollHereY(0.0f);
          // The water (64-bit rendering only: the host replays the game's
          // water draws for the surface; in process nothing changes).
          ImGui::TextWrapped("只在 64 位渲染下生效。水下的地面阴影随水深淡出："
                             "浅水保留，深水只留水面自己的淡影。"
                             "水深按游戏单位，一层悬崖是 128。");
          ImGui::SliderFloat("水下阴影开始淡出", &settings.water.shadowFadeStart, 0.0f,
                             256.0f, "水深 %.0f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("水下阴影完全消失", &settings.water.shadowFadeEnd, 1.0f,
                             512.0f, "水深 %.0f", ImGuiSliderFlags_AlwaysClamp);
          if (!(settings.water.shadowFadeEnd >= settings.water.shadowFadeStart + 1.0f))
            settings.water.shadowFadeEnd = settings.water.shadowFadeStart + 1.0f;
          ImGui::SliderFloat("水面阴影浓度", &settings.water.surfaceShadow, 0.0f, 1.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::Separator();
          ImGui::Checkbox("水面效果", &settings.water.surfaceEnabled);
          ImGui::BeginDisabled(!settings.water.surfaceEnabled);
          ImGui::SliderInt("原位水面", &settings.water.inPlace, 0, 2,
                           settings.water.inPlace == 0 ? "%d 关闭" : settings.water.inPlace == 1 ? "%d 开启" : "%d 自动（独显）",
                           ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("水面风格", &settings.water.style, 0.0f, 1.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("水深染色", &settings.water.depthColor, 0.0f, 2.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("岸边白沫", &settings.water.foam, 0.0f, 2.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("水下折射", &settings.water.refraction, 0.0f, 2.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("水底焦散##water", &settings.water.caustics, 0.0f, 2.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("水面反射##water", &settings.water.reflection, 0.0f, 1.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("阳光闪烁##water", &settings.water.specular, 0.0f, 2.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("波纹起伏##water", &settings.water.waveScale, 0.0f, 2.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          ImGui::SliderFloat("波纹速度##water", &settings.water.waveSpeed, 0.0f, 3.0f,
                             "%.2f", ImGuiSliderFlags_AlwaysClamp);
          if (ImGui::TreeNode("后合成（原位水面关闭时）")) {
            ImGui::SliderFloat("后合成清澈度##water", &settings.water.clarity, 0.0f, 1.0f,
                               "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::TreePop();
          }
          ImGui::EndDisabled();
          ImGui::TreePop();
        }

        if (ImGui::TreeNode("点光源##panel")) {
          ImGui::Text("当前数量: %u (gen=%llu)",
                      dxvk::War3LightManager::Instance().GetLightCount(),
                      static_cast<unsigned long long>(
                          dxvk::War3LightManager::Instance().GetGeneration()));
          ImGui::BeginDisabled(!settings.shadows.pointLightsEnabled);
          if (ImGui::Button("创建测试点光源##create")) {
            dxvk::War3LightManager::Instance().InitTestLight();
          }
          ImGui::SameLine();
          if (ImGui::Button("清空点光源##clear")) {
            dxvk::War3LightManager::Instance().ClearLights();
          }
          int maxShadowLights =
              static_cast<int>(settings.shadows.pointShadowMaxLights);
          if (ImGui::SliderInt("点阴影最大光源", &maxShadowLights, 0, 4))
            settings.shadows.pointShadowMaxLights =
                static_cast<uint32_t>(maxShadowLights);
          // Resolution changes recreate a cube array, so expose discrete tiers
          // instead of a continuous slider that could allocate every integer
          // size while the user drags it.
          constexpr uint32_t shadowResValues[] = {128u, 256u, 512u, 1024u,
                                                   2048u};
          constexpr const char *shadowResLabels[] = {"128", "256", "512",
                                                      "1024", "2048 (Ultra)"};
          int shadowResIndex = 0;
          uint32_t bestResDelta = ~0u;
          for (int i = 0; i < IM_ARRAYSIZE(shadowResValues); ++i) {
            const uint32_t value = shadowResValues[i];
            const uint32_t current = settings.shadows.pointShadowResolution;
            const uint32_t delta = value > current ? value - current
                                                    : current - value;
            if (delta < bestResDelta) {
              bestResDelta = delta;
              shadowResIndex = i;
            }
          }
          if (ImGui::Combo("点阴影分辨率", &shadowResIndex, shadowResLabels,
                           IM_ARRAYSIZE(shadowResLabels))) {
            settings.shadows.pointShadowResolution =
                shadowResValues[shadowResIndex];
          }
          const uint64_t pointShadowBytesPerLight =
              uint64_t(settings.shadows.pointShadowResolution) *
              uint64_t(settings.shadows.pointShadowResolution) * 4ull * 6ull;
          const uint32_t pointShadowRequested =
              std::min<uint32_t>(settings.shadows.pointShadowMaxLights, 4u);
          const uint32_t pointShadowBudgetCapacity =
              pointShadowRequested == 0u
                  ? 0u
                  : std::min<uint32_t>(
                        pointShadowRequested,
                        static_cast<uint32_t>(std::max<uint64_t>(
                            1ull, (96ull * 1024ull * 1024ull) /
                                      std::max<uint64_t>(
                                          pointShadowBytesPerLight, 1ull))));
          const double pointShadowMiB =
              double(pointShadowBytesPerLight * pointShadowBudgetCapacity) /
              (1024.0 * 1024.0);
          ImGui::TextDisabled("有效阴影灯: %u / %u，D32 预算 %.1f / 96 MiB",
                              pointShadowBudgetCapacity, pointShadowRequested,
                              pointShadowMiB);
          if (pointShadowBudgetCapacity < pointShadowRequested) {
            ImGui::TextDisabled(
                "当前分辨率受 96 MiB 安全门限制；其余灯仍保留直接照明");
          }
          ImGui::Checkbox("点阴影时序复用",
                          &settings.shadows.pointShadowTemporalReuse);
          int updatePeriod =
              static_cast<int>(settings.shadows.pointShadowUpdatePeriod);
          if (ImGui::SliderInt("点阴影更新周期", &updatePeriod, 1, 4))
            settings.shadows.pointShadowUpdatePeriod =
                static_cast<uint32_t>(updatePeriod);
          ImGui::Checkbox("点阴影 Face 剔除",
                          &settings.shadows.pointShadowFaceCulling);
          ImGui::SliderFloat("点阴影 Bias", &settings.shadows.pointShadowBias,
                             0.0f, 0.2f, "%.3f");
          ImGui::SliderFloat("点阴影 PCF 近端",
                             &settings.shadows.pointShadowPcfRadiusNear, 0.0f,
                             3.0f, "%.2f");
          settings.shadows.pointShadowPcfRadiusFar = std::max(
              settings.shadows.pointShadowPcfRadiusFar,
              settings.shadows.pointShadowPcfRadiusNear);
          ImGui::SliderFloat("点阴影 PCF 远端",
                             &settings.shadows.pointShadowPcfRadiusFar,
                             settings.shadows.pointShadowPcfRadiusNear, 4.0f,
                             "%.2f");
          ImGui::SliderFloat("点阴影 Texel Bias",
                             &settings.shadows.pointShadowTexelBiasScale, 0.0f,
                             1.0f, "%.3f");
          ImGui::SliderFloat("点阴影范围淡出起点",
                             &settings.shadows.pointShadowRangeFadeStart, 0.50f,
                             0.98f, "%.2f");
          ImGui::Separator();
          ImGui::TextDisabled("软件射线接触阴影");
          ImGui::Checkbox("启用点光软件射线阴影",
                          &settings.shadows.pointRayShadowEnabled);
          ImGui::Checkbox("Hi-Z 分层遍历",
                          &settings.shadows.pointRayShadowHiZEnabled);
          int pointRayMaxLights =
              static_cast<int>(settings.shadows.pointRayShadowMaxLights);
          if (ImGui::SliderInt("射线光源上限", &pointRayMaxLights, 1, 2))
            settings.shadows.pointRayShadowMaxLights =
                static_cast<uint32_t>(pointRayMaxLights);
          int pointRaySteps =
              static_cast<int>(settings.shadows.pointRayShadowSteps);
          if (ImGui::SliderInt("射线步数", &pointRaySteps, 4, 32))
            settings.shadows.pointRayShadowSteps =
                static_cast<uint32_t>(pointRaySteps);
          int pointRayHiZVisits =
              static_cast<int>(settings.shadows.pointRayShadowHiZMaxVisits);
          if (ImGui::SliderInt("Hi-Z 节点预算", &pointRayHiZVisits, 8, 64))
            settings.shadows.pointRayShadowHiZMaxVisits =
                static_cast<uint32_t>(pointRayHiZVisits);
          ImGui::SliderFloat("射线最大距离",
                             &settings.shadows.pointRayShadowMaxDistance,
                             32.0f, 2400.0f, "%.0f");
          ImGui::SliderFloat("射线厚度",
                             &settings.shadows.pointRayShadowThickness,
                             1.0f, 160.0f, "%.1f");
          ImGui::SliderFloat("射线起点偏移",
                             &settings.shadows.pointRayShadowStartOffset,
                             1.0f, 96.0f, "%.1f");
          ImGui::SliderFloat("射线阴影强度",
                             &settings.shadows.pointRayShadowStrength,
                             0.0f, 1.0f, "%.2f");
          ImGui::TextDisabled(
              "仅补短距离屏幕内接触细节；越界/缺深度时自动退回 cube 阴影");
          ImGui::EndDisabled();
          ImGui::TreePop();
        }

        if (ImGui::TreeNode("模块热插拔")) {
          if (m_devicePanels.passToggles)
            m_devicePanels.passToggles(m_device);
          else
            ImGui::TextDisabled("宿主执行：代理模式下不可切换");
          ImGui::TreePop();
        }

        if (ImGui::TreeNode("AA")) {
          const char *aaModes[] = {"关闭",    "FXAA",    "SMAA 低",
                                   "SMAA 中", "SMAA 高", "SMAA 极高"};
          int aaMode = static_cast<int>(settings.postFx.aa.mode);
          if (ImGui::Combo("模式", &aaMode, aaModes, IM_ARRAYSIZE(aaModes))) {
            settings.postFx.aa.mode = static_cast<War3AAMode>(aaMode);
            // 切换到 SMAA 时写入推荐档位，避免“无效调整”错觉
            switch (settings.postFx.aa.mode) {
            case War3AAMode::SMAA_Low:
              settings.postFx.aa.smaaThreshold = 0.10f;
              settings.postFx.aa.smaaMaxSearchSteps = 8;
              settings.postFx.aa.smaaMaxSearchStepsDiag = 0;
              break;
            case War3AAMode::SMAA_Medium:
              settings.postFx.aa.smaaThreshold = 0.08f;
              settings.postFx.aa.smaaMaxSearchSteps = 12;
              settings.postFx.aa.smaaMaxSearchStepsDiag = 0;
              break;
            case War3AAMode::SMAA_High:
              settings.postFx.aa.smaaThreshold = 0.06f;
              settings.postFx.aa.smaaMaxSearchSteps = 16;
              settings.postFx.aa.smaaMaxSearchStepsDiag = 8;
              break;
            case War3AAMode::SMAA_Ultra:
              settings.postFx.aa.smaaThreshold = 0.05f;
              settings.postFx.aa.smaaMaxSearchSteps = 32;
              settings.postFx.aa.smaaMaxSearchStepsDiag = 16;
              break;
            default:
              break;
            }
          }

          if (settings.postFx.aa.mode == War3AAMode::FXAA) {
            ImGui::SliderFloat("子像素质量",
                               &settings.postFx.aa.fxaaQualitySubpix, 0.0f,
                               1.0f, "%.2f");
            ImGui::SliderFloat("边缘阈值",
                               &settings.postFx.aa.fxaaQualityEdgeThreshold,
                               0.05f, 0.5f, "%.3f");
            ImGui::SliderFloat("最小阈值",
                               &settings.postFx.aa.fxaaQualityEdgeThresholdMin,
                               0.02f, 0.2f, "%.3f");
          } else if (settings.postFx.aa.mode != War3AAMode::None) {
            ImGui::SliderFloat("边缘阈值", &settings.postFx.aa.smaaThreshold,
                               0.05f, 0.2f, "%.3f");
            ImGui::SliderInt("搜索步数", &settings.postFx.aa.smaaMaxSearchSteps,
                             4, 32);
            ImGui::SliderInt("对角搜索",
                             &settings.postFx.aa.smaaMaxSearchStepsDiag, 0, 16);
          }

          ImGui::TreePop();
        }

        if (ImGui::TreeNode("SSAO")) {
          ImGui::Checkbox("启用 SSAO", &settings.postFx.ssao.enabled);
          ImGui::SliderFloat("半径(px)", &settings.postFx.ssao.radiusPx, 1.0f,
                             32.0f, "%.1f");
          ImGui::SliderFloat("强度", &settings.postFx.ssao.strength, 0.0f, 3.0f,
                             "%.2f");
          ImGui::SliderFloat("偏移", &settings.postFx.ssao.bias, 0.0f, 0.2f,
                             "%.3f");
          ImGui::SliderFloat("曲线", &settings.postFx.ssao.power, 0.5f, 4.0f,
                             "%.2f");
          ImGui::SliderFloat("衰减近", &settings.postFx.ssao.fadeNear, 0.0f,
                             0.9f, "%.2f");
          ImGui::SliderFloat("衰减远", &settings.postFx.ssao.fadeFar, 0.1f,
                             1.0f, "%.2f");
          ImGui::TreePop();
        }

        if (ImGui::TreeNode("体积光")) {
          ImGui::Checkbox("启用体积光",
                          &settings.postFx.volumetricLight.enabled);
          int volumetricBackend = static_cast<int>(
              settings.postFx.volumetricLight.quality);
          const char* volumetricBackends[] = {
              "兼容 RayMarch", "Froxel Medium", "Froxel High"};
          if (ImGui::Combo("体积后端", &volumetricBackend,
                           volumetricBackends,
                           IM_ARRAYSIZE(volumetricBackends))) {
            settings.postFx.volumetricLight.quality =
                static_cast<War3VolumetricQuality>(volumetricBackend);
          }
          if (volumetricBackend > 0) {
            ImGui::Checkbox("Froxel 时域重建",
                            &settings.postFx.volumetricLight
                                 .froxelTemporalEnabled);
          }
          ImGui::Checkbox("需要 CSM 快照",
                          &settings.postFx.volumetricLight.requireCsmSnapshot);
          ImGui::Checkbox("叠加点光散射",
                          &settings.postFx.volumetricLight.includePointLights);
          int volumetricPointMaxLights =
              static_cast<int>(settings.postFx.volumetricLight.maxPointLights);
          if (ImGui::SliderInt("体积点光上限", &volumetricPointMaxLights,
                               0, 2)) {
            settings.postFx.volumetricLight.maxPointLights =
                static_cast<uint32_t>(volumetricPointMaxLights);
          }
          ImGui::Checkbox("自适应采样",
                          &settings.postFx.volumetricLight.adaptiveSampleCount);
          ImGui::SliderFloat("最低太阳强度",
                             &settings.postFx.volumetricLight.minSunIntensity,
                             0.0f, 0.5f, "%.3f");
          ImGui::SliderFloat("强度(整体能量)",
                             &settings.postFx.volumetricLight.intensity, 0.0f,
                             4.0f, "%.3f");
          ImGui::SliderInt("采样数(安全上限16)",
                           &settings.postFx.volumetricLight.sampleCount, 4, 16);
          ImGui::SliderFloat("密度(散射厚度)",
                             &settings.postFx.volumetricLight.density, 0.0f,
                             2.0f, "%.3f");
          ImGui::SliderFloat("权重(阴影束对比度)",
                             &settings.postFx.volumetricLight.weight, 0.0f,
                             3.0f, "%.3f");
          ImGui::TextDisabled(
              "提示: 可见度请调强度/密度/权重；采样数不会突破 GPU 安全预算");
          ImGui::SliderFloat("最大距离",
                             &settings.postFx.volumetricLight.sunDistance,
                             100.0f, 6000.0f, "%.0f");
          ImGui::SliderFloat("Froxel统一远端",
                             &settings.postFx.volumetricLight.froxelFar,
                             1000.0f, 10000.0f, "%.0f");
          ImGui::SliderFloat("近处衰减",
                             &settings.postFx.volumetricLight.fadeNear, 0.0f,
                             0.95f, "%.2f");
          ImGui::SliderFloat("远处衰减",
                             &settings.postFx.volumetricLight.fadeFar, 0.1f,
                             1.0f, "%.2f");
          ImGui::SliderFloat("高度雾强度",
                             &settings.postFx.volumetricLight.heightFogStrength,
                             0.0f, 2.0f, "%.2f");
          ImGui::SliderFloat("底图消光强度",
                             &settings.postFx.volumetricLight.extinctionStrength,
                             0.0f, 1.0f, "%.2f");
          ImGui::SliderFloat(
              "无 CSM 散射保底",
              &settings.postFx.volumetricLight.unshadowedScattering, 0.0f,
              1.0f, "%.2f");
          ImGui::TextDisabled(
              "关闭“需要 CSM 快照”时，仅使用有界无阴影介质保底，不生成伪阴影束");
          ImGui::TreePop();
        }

        if (ImGui::TreeNode("Bloom / ACES")) {
          ImGui::Checkbox("启用 Bloom", &settings.postFx.bloom.enabled);
          ImGui::SliderFloat("阈值", &settings.postFx.bloom.threshold, 0.1f,
                             2.5f, "%.2f");
          ImGui::SliderFloat("软阈值", &settings.postFx.bloom.softKnee, 0.05f,
                             1.5f, "%.2f");
          ImGui::SliderFloat("强度", &settings.postFx.bloom.intensity, 0.0f,
                             2.0f, "%.2f");
          bool acesToneMap = settings.postFx.bloom.acesToneMap;
          if (ImGui::Checkbox("启用 ACES 色调映射", &acesToneMap)) {
            settings.postFx.bloom.acesToneMap = acesToneMap;
          }
          if (ImGui::SliderFloat("曝光", &settings.postFx.exposure, 0.1f, 4.0f,
                                 "%.2f")) {
            settings.dayNight.affectExposure = false;
          }
          ImGui::Checkbox("sRGB 采样/输出", &settings.postFx.useSrgb);
          ImGui::TreePop();
        }

        if (ImGui::TreeNode("环境光覆盖")) {
          if (ImGui::Checkbox("启用覆盖", &settings.ambient.overrideEnabled)) {
            settings.ambient.autoOverride = false;
          }
          float amb[3] = {settings.ambient.color.x, settings.ambient.color.y,
                          settings.ambient.color.z};
          if (ImGui::ColorEdit3("颜色", amb, ImGuiColorEditFlags_NoInputs)) {
            settings.ambient.color.x = amb[0];
            settings.ambient.color.y = amb[1];
            settings.ambient.color.z = amb[2];
            settings.ambient.color.w = 1.0f;
            settings.ambient.autoOverride = false;
          }
          ImGui::TreePop();
        }

        if (ImGui::TreeNode("日夜色调")) {
          ImGui::Checkbox("启用修正", &settings.dayNight.enabled);
          ImGui::Checkbox("影响环境光", &settings.dayNight.affectAmbient);
          ImGui::Checkbox("影响曝光", &settings.dayNight.affectExposure);
          ImGui::SliderFloat("日出日落最低亮度",
                             &settings.dayNight.transitionMinFactor, 0.5f, 1.0f,
                             "%.2f");

          float dayAmb[3] = {settings.dayNight.dayAmbient.x,
                             settings.dayNight.dayAmbient.y,
                             settings.dayNight.dayAmbient.z};
          if (ImGui::ColorEdit3("白天环境光", dayAmb,
                                ImGuiColorEditFlags_NoInputs)) {
            settings.dayNight.dayAmbient.x = dayAmb[0];
            settings.dayNight.dayAmbient.y = dayAmb[1];
            settings.dayNight.dayAmbient.z = dayAmb[2];
            settings.dayNight.dayAmbient.w = 1.0f;
          }

          float nightAmb[3] = {settings.dayNight.nightAmbient.x,
                               settings.dayNight.nightAmbient.y,
                               settings.dayNight.nightAmbient.z};
          if (ImGui::ColorEdit3("夜晚环境光", nightAmb,
                                ImGuiColorEditFlags_NoInputs)) {
            settings.dayNight.nightAmbient.x = nightAmb[0];
            settings.dayNight.nightAmbient.y = nightAmb[1];
            settings.dayNight.nightAmbient.z = nightAmb[2];
            settings.dayNight.nightAmbient.w = 1.0f;
          }

          ImGui::SliderFloat("白天曝光", &settings.dayNight.dayExposure, 0.6f,
                             2.0f, "%.2f");
          ImGui::SliderFloat("夜晚曝光", &settings.dayNight.nightExposure, 0.2f,
                             1.2f, "%.2f");
          ImGui::SliderFloat("过渡起点(度)",
                             &settings.dayNight.dayTransitionStartDeg, -6.0f,
                             6.0f, "%.1f");
          ImGui::SliderFloat("过渡终点(度)",
                             &settings.dayNight.dayTransitionEndDeg, 2.0f,
                             16.0f, "%.1f");

          ImGui::TreePop();
        }

        if (recordSettings)
          player::RecordPanelEdit(settingsBefore, settings);
        }
      }
    }

    if (ImGui::CollapsingHeader("Vulkan ShaderPack")) {
      if (m_devicePanels.shaderPack)
        m_devicePanels.shaderPack();
      else
        ImGui::TextDisabled("宿主执行：代理模式下不可用");
    }

    OpenSectionForTest();
    if (ImGui::CollapsingHeader("渲染统计 (Render Stats)")) {
      drawRenderStatsPanel();
    }
  }
  ImGui::End();

  if (recordExtras)
    player::RecordExtrasEdit(extrasBefore, currentExtras());
  // Save once edits have settled; the close button saves right away.
  player::FlushIfDue(!m_visible);
}

void War3Imgui::drawRenderStatsPanel() {
  // Singleton UI; POD copies retain no game objects or GPU resources. The
  // shutdown reset forces a fresh read even if a device address is reused.
  struct Samples {
    render::ShadowProducerRuntimeDiagnostics producer;
    memory::ShadowArenaMemoryStats arena;
    ShadowReplayDiagnostics replay;
    render::ShadowDisplayStats receiver;
    CsmResolutionDiagnostics csm;
    War3WeatherDiagnostics weather;
    War3VolumetricExecutionDiagnostics weatherVolume;
    MEMORYSTATUSEX processMemory{};
    bool processMemoryValid = false;
    uint64_t snapshotCap = 0, sliceReusedBytes = 0, budgetGrowthRejects = 0;
    proxy::AddressSpaceSample addressSpace;
    uint64_t addressSpaceTick = 0;
    proxy::DiagStatus diag;
    proxy::FrameStatus proxyFrame;
  };
  static Samples samples;
  const uint64_t statsNow = GetTickCount64();
  const bool statsDue = m_renderStatsRefresh.due(statsNow);
  // Walking the address space takes a moment: once a second, panel open only.
  if (statsDue && (!samples.addressSpace.valid || statsNow - samples.addressSpaceTick >= 1000u)) {
    samples.addressSpace = proxy::QueryAddressSpace();
    samples.addressSpaceTick = statsNow;
  }
  if (m_proxy) {
    // The render statistics of proxy mode are the host's.
    if (statsDue) {
      samples.diag = proxy::DiagQuery();
      samples.proxyFrame = proxy::QueryFrameStatus();
    }
    ImGui::TextDisabled("4 Hz 刷新；渲染在 x64 宿主进程执行，下列数值来自宿主。");
    DrawAddressSpace(samples.addressSpace);
    DrawProxyRenderStats(samples.diag, samples.proxyFrame, samples.addressSpace);
    return;
  }
  if (statsDue) {
    samples.producer = render::QueryShadowProducerRuntimeDiagnostics();
    samples.arena = memory::ShadowArena_QueryMemoryStats();
    samples.replay = ProcessServices().QueryShadowReplayDiagnostics();
    samples.receiver = render::QueryShadowDisplayStats();
    samples.csm = ProcessServices().QueryCsmResolutionDiagnostics();
    samples.weather = ProcessServices().QueryWar3WeatherDiagnostics();
    samples.weatherVolume = ProcessServices().QueryWar3WeatherVolumeDiagnostics();
    samples.snapshotCap = render::g_stage11AdaptiveTarget.load(std::memory_order_relaxed);
    samples.sliceReusedBytes = render::g_stage11SliceReusedBytes.load(std::memory_order_relaxed);
    samples.budgetGrowthRejects = render::g_stage11BudgetGrowthRejects.load(std::memory_order_relaxed);
    samples.processMemory = {};
    samples.processMemory.dwLength = sizeof(MEMORYSTATUSEX);
    samples.processMemoryValid = GlobalMemoryStatusEx(&samples.processMemory) != FALSE;
  }
  const auto& p = samples.producer;
  const auto& a = samples.arena;
  const auto& r = samples.replay;
  const auto mib = [](uint64_t value) { return ui::BytesToMiB(value); };
  const auto u64 = [](uint64_t value) { return static_cast<unsigned long long>(value); };
  ImGui::TextDisabled("4 Hz 刷新；不需要开启性能录制或帧取证");
  DrawAddressSpace(samples.addressSpace);
  ImGui::Separator();
  const auto& weather = samples.weather;
  ImGui::Text("天气: %s / 演出 %.2f 秒 / 活动雪片估算 %u / %u",
      War3WeatherDrawStageName(weather.stage), weather.timeSeconds,
      weather.activeParticles, weather.capacity);
  ImGui::Text("雪片命令记录: %s / 深度副本 %.2f MiB（体积天气另计）",
      weather.submitted ? "是" : "否", mib(weather.resourceBytes));
  ImGui::Text("雪雾: %s / 合成命令 %s / 当前资源 %.2f MiB",
      samples.weatherVolume.stage, samples.weatherVolume.compositeSubmitted ? "已记录" : "未记录",
      mib(samples.weatherVolume.resourceBytes));
  ImGui::Text("已退休切片累计复用 %.1f MiB / 动态预算增长拒绝 %llu",
      mib(samples.sliceReusedBytes), u64(samples.budgetGrowthRejects));
  ImGui::TextWrapped("各来源独立采样，可能相差数帧；数值用于诊断，不代表像素验收。单位 MiB。");

  if (!p.producerSealFrameSerial) {
    ImGui::TextDisabled("尚无已封口场景数据（主菜单/加载中/阴影未运行）。");
  } else {
    ImGui::Text("生产帧 %llu / 地图 %llu / 设备 %llu",
        u64(p.producerSealFrameSerial), u64(p.producerSealMapEpoch), u64(p.producerSealDeviceEpoch));
    ImGui::Text("生产封口 %s / 必需 caster 缺失 %llu / 原因 0x%llX",
        p.producerCompletenessSealed ? "是" : "否",
        u64(p.producerRequiredCasterOmissionCount), u64(p.producerCompletenessReasonMask));
    if (p.producerRequiredCasterOmissionCount || p.producerCompletenessReasonMask ||
        p.producerCompletenessCounterOverflow)
      ImGui::TextColored(ImVec4(1.f, .4f, .2f, 1.f), "生产不完整：可导致本帧阴影拒绝更新。");
  }

  ImGui::Separator();
  ImGui::Text("快照页池 Stage11：常驻 %.1f / 最近扩页额度 %.1f MiB",
      mib(p.drawTimeSnapshotPageResidentBytes), mib(samples.snapshotCap));
  ImGui::ProgressBar(ui::BudgetFraction(p.drawTimeSnapshotPageResidentBytes, samples.snapshotCap),
      ImVec2(-1, 0), "快照页池常驻 / 预算");
  ImGui::Text("页内已占用 %.1f MiB；回收页 %llu",
      mib(p.drawTimeSnapshotPageUsedBytes), u64(p.drawTimeSnapshotPageReclaimedCount));
  ImGui::Text("发布帧计数：容量拒绝 %llu / 分配失败 %llu / 新页 %llu",
      u64(p.drawTimeSnapshotPageCapacityRejectCount),
      u64(p.drawTimeSnapshotPageAllocationFailureCount), u64(p.drawTimeSnapshotPageCreateCount));
  ImGui::Text("静态缓存逻辑引用 %.1f MiB（受保护 %.1f）",
      mib(p.drawTimeVBCacheStaticLiveBytes), mib(p.drawTimeVBCacheStaticProtectedBytes));
  ImGui::TextWrapped("页内占用包含未单独回收的旧切片，并非当前镜头的几何净量。缓存引用与页常驻有重叠，不可相加；池上限也不是总显存上限。");

  ImGui::Separator();
  ImGui::Text("Arena：上一提交代使用 %.1f MiB / 保留容量 %.1f MiB",
      mib(a.lastSubmittedUsedBytes), mib(a.residentBytes));
  ImGui::TextDisabled("当前代瞬时使用 %.1f MiB（切帧后可为 0；不是上一帧用量）", mib(a.usedBytes));
  {
    ImGui::Text("Arena 常驻上限 %.1f MiB / 活跃代 %u / 代号 %llu",
        mib(a.residentLimitBytes), a.activeGenerationCount, u64(a.generation));
    ImGui::ProgressBar(ui::BudgetFraction(a.residentBytes, a.residentLimitBytes),
        ImVec2(-1, 0), "Arena 保留容量 / 预算（不是帧内用量）");
  }
  ImGui::Text("提交 serial %llu / 完成 serial %llu / 当前不完整 %s",
      u64(a.submittedSerial), u64(a.completedSerial), a.frameIncomplete ? "是" : "否");
  ImGui::Text("Arena 累计：溢出 %llu / 准入拒绝 %llu / 忙拒绝 %llu / 隔离 %llu",
      u64(a.overflowCount), u64(a.admissionRejectedCount), u64(a.busyReuseRejectCount), u64(a.quarantineCount));
  ImGui::TextDisabled("Arena 为独立临时上传池；原子字段非同一事务快照，不能据此授权回收。");
  ImGui::TextWrapped("已完成代际按需求窗口逐页缩减空闲尾页；上一提交代不表示 GPU 已完成。");
  if (a.budgetSupported && a.budgetTrusted && a.budgetFrameSerial) {
    ImGui::Text("Vulkan 分配堆：预算 %.1f / 物理承诺估计 %.1f / 可用估计 %.1f MiB",
        mib(a.budgetBytes), mib(a.allocatedBytes), mib(a.availableBytes));
    ImGui::TextDisabled("堆预算采样帧 %llu；不是整张显卡的实时占用。", u64(a.budgetFrameSerial));
  } else {
    ImGui::TextDisabled("Vulkan 主堆预算：暂无可信采样（不以 0 当作空闲）。");
  }
  if (samples.processMemoryValid) {
    ImGui::Text("本 32 位进程 VA 可用 %.1f / 总量 %.1f MiB",
        mib(samples.processMemory.ullAvailVirtual), mib(samples.processMemory.ullTotalVirtual));
    ImGui::TextDisabled("VA 是虚拟地址空间，不是物理 RAM 或显存。");
  } else {
    ImGui::TextDisabled("进程 VA：查询失败。");
  }

  ImGui::Separator();
  const auto& receiver = samples.receiver;
  ImGui::Text("已发布阴影状态（生产帧 %llu）：图 serial %llu",
      u64(receiver.producerFrame), u64(receiver.mapRenderSerial));
  ImGui::Text("完整图 %u / 本帧更新 %u / 接收端绘制 %u / 早退码 %u",
      receiver.hasCompleteMap, receiver.mapExecuted, receiver.receiverExecuted, receiver.earlyReturnReason);
  ImGui::Text("准备 caster %u（地形装饰 %u）/ 级联 draws %u | %u | %u | %u",
      receiver.prepared, receiver.terrainDoodadPrepared, receiver.cascadeDrawn[0],
      receiver.cascadeDrawn[1], receiver.cascadeDrawn[2], receiver.cascadeDrawn[3]);
  ImGui::Text("阴影回放采样帧 %llu / 地图 %llu / 设备 %llu",
      u64(r.candidateFrameSerial), u64(r.mapEpoch), u64(r.deviceEpoch));
  ImGui::Text("计划 %u / 回放 %u / 验证 %u / drawn %u",
      r.plannedCasterCount, r.replayCasterCount, r.validatedCasterCount, r.drawnCasterCount);
  ImGui::Text("回放累计：验证拒绝 %llu / 阻止不完整 %llu / 旧代拒绝 %llu",
      u64(r.validationRejectCount), u64(r.partialPreventedCount), u64(r.staleEpochConsumerRejectCount));
  ImGui::Text("最近回放拒绝原因码 %u / CSM 请求 %u → 实际 %u",
      r.lastRejectReason, samples.csm.requestedResolution, samples.csm.effectiveResolution);
  ImGui::TextDisabled("CSM 降级原因码 %u；drawn 是提交统计，不是屏幕可见对象数。", samples.csm.fallbackReason);
}

// setCursorBitmap: war3_imgui_inprocess.cpp (the in-process device only).

void War3Imgui::drawCursorOverlay() {
  ImGui::Begin("CursorOverlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImDrawList *drawList =
      ImGui::GetForegroundDrawList(); // Use foreground list to be on top of
                                      // everything
  ImVec2 mousePos = ImGui::GetMousePos();

  if (m_cursorTexture != nullptr) {
    // Draw custom cursor
    ImVec2 tl = ImVec2(mousePos.x - m_cursorHotX, mousePos.y - m_cursorHotY);
    ImVec2 br = ImVec2(tl.x + m_cursorWidth, tl.y + m_cursorHeight);
    drawList->AddImage((ImTextureID)m_cursorTexture.ptr(), tl, br);
  } else {
    // Fallback: Draw a simple arrow (White with Black border)
    ImVec2 p = mousePos;
    const ImU32 col = IM_COL32(255, 255, 255, 255);
    const ImU32 border = IM_COL32(0, 0, 0, 255);

    // Standard Mouse Arrow Vertices (Approx)
    ImVec2 v1 = p;
    ImVec2 v2 = ImVec2(p.x, p.y + 16);
    ImVec2 v3 = ImVec2(p.x + 11, p.y + 11);

    drawList->AddTriangleFilled(v1, v2, v3, col);
    drawList->AddTriangle(v1, v2, v3, border, 2.0f);
  }

  ImGui::End();
} // Helper to avoid stroke issue since ImGui doesn't have AddTriangleStroke

} // namespace dxvk::war3
