#define WAR3_SHADER_API_INTERNAL

#include "war3_imgui.h"
#include "../../d3d9_device.h"
#include "../../d3d9_war3_light.h"
#include "../../war3_shader_api.h"
#include "../../war3_shaderpack.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "../core/war3_internal_test_config.h"
#include "../debug/war3_debug.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_render_state.h"
#include "../shader/war3_shader_manager.h"
#include "../tools/war3_perf_monitor.h"

#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace dxvk::war3 {

// 静态成员初始化 - 默认启用 FPS 解锁
std::atomic<bool> War3Imgui::s_fpsUnlocked{true};

War3Imgui &War3Imgui::get() {
  static War3Imgui instance;
  return instance;
}

void War3Imgui::initialize(HWND hwnd, D3D9DeviceEx *device) {
  if (m_initialized)
    return;

  m_hwnd = hwnd;
  m_device = device;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigFlags |=
      ImGuiConfigFlags_NoMouseCursorChange; // Prevent ImGui from messing with
                                            // OS cursor
  io.MouseDrawCursor =
      false; // Disable ImGui software cursor, let game handle it

  // Style
  ImGui::StyleColorsDark();

  // Load Chinese Font (Microsoft YaHei) for Chinese characters
  // If failed, it acts as default.
  ImFontConfig font_config;
  io.Fonts->AddFontFromFileTTF("c:\\windows\\fonts\\msyh.ttc", 18.0f,
                               &font_config,
                               io.Fonts->GetGlyphRangesChineseFull());

  // Platform/Renderer backends
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX9_Init(device);

  m_initialized = true;
  Logger::info("War3Imgui initialized.");
}

void War3Imgui::shutdown() {
  if (!m_initialized)
    return;

  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  m_initialized = false;
}

bool War3Imgui::handleWndProc(HWND hWnd, UINT msg, WPARAM wParam,
                              LPARAM lParam) {
  if (m_initialized && m_visible) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
      return true;
  }
  return false;
}

void War3Imgui::toggleVisible() { m_visible = !m_visible; }

bool War3Imgui::wantsCaptureMouse() const {
  if (!m_initialized || !m_visible)
    return false;
  return ImGui::GetIO().WantCaptureMouse;
}

bool War3Imgui::wantsCaptureKeyboard() const {
  if (!m_initialized || !m_visible)
    return false;
  return ImGui::GetIO().WantCaptureKeyboard;
}

void War3Imgui::newFrame() {
  if (!m_initialized || !m_visible)
    return;
  if (m_frameStarted)
    return;

  m_frameStarted = true;
  m_hasRendered = false; // Reset frame flag

  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  war3shader::internal::SetImGuiContext(ImGui::GetCurrentContext());
  war3shader::internal::BeginUiFrame();
  war3shader::internal::DispatchUiCallbacks();
}

void War3Imgui::endFrame() {
  if (!m_initialized)
    return;
  m_frameStarted = false;
}

void War3Imgui::render(bool inScene) {
  if (!m_initialized || m_hasRendered || !m_visible)
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
  if (m_device) {
    if (!inScene)
      m_device->BeginScene();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    if (!inScene)
      m_device->EndScene();
  }

  war3shader::internal::EndUiFrame();
}

void War3Imgui::drawDebugWindow() {
  ImGuiIO &io = ImGui::GetIO();

  // Allow resizing (removed ImGuiWindowFlags_AlwaysAutoResize)
  if (ImGui::Begin("War3VK 调试器", nullptr, ImGuiWindowFlags_None)) {
    ImGui::Text("帧率 (FPS): %.1f (%.3f ms)", io.Framerate,
                1000.0f / io.Framerate);

    // FPS 解锁控制
    ImGui::SameLine();
    bool fpsUnlocked = isFpsUnlocked();
    if (ImGui::Checkbox("突破帧率上限", &fpsUnlocked)) {
      setFpsUnlocked(fpsUnlocked);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("禁用 VSync 同步，允许帧率超过显示器刷新率");
    }

    // [Perf] Start/Stop Performance Logging
    ImGui::SameLine();
    auto &perf = War3PerfMonitor::instance();
    const bool recording = perf.isRecording();
    const char *label = recording ? "结束性能日志记录" : "开始性能日志记录";
    if (ImGui::Button(label)) {
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
    if (recording) {
      ImGui::SameLine();
      ImGui::TextDisabled("记录中...");
    }
    const auto pendingExports = perf.getPendingExportCount();
    if (pendingExports > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("导出中...");
    }

    ImGui::Separator();

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

              // Compile Button (Always visible)
              ImGui::SameLine();
              if (ImGui::Button("编译")) {
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

    if (ImGui::CollapsingHeader("内置渲染设置")) {
      if (!m_device || !m_device->GetWar3Pipeline()) {
        ImGui::TextDisabled("渲染管线未就绪");
      } else {
        auto *pipeline = m_device->GetWar3Pipeline();
        auto &settings = pipeline->MutableSettings();
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
          ImGui::Checkbox("太阳光覆盖", &settings.sun.enabled);
          ImGui::SliderFloat("太阳强度", &settings.sun.intensity, 0.0f, 3.0f,
                             "%.2f");
          ImGui::TreePop();
        }

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
                             &settings.shadows.casterBiasClamp, 0.0f, 5.0f,
                             "%.2f");
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
          ImGui::Checkbox("锁定太阳", &settings.shadows.lockSun);
          ImGui::BeginDisabled(!settings.shadows.lockSun);
          ImGui::SliderFloat("锁定时间", &settings.shadows.lockSunTime, 0.0f,
                             1.0f, "%.3f");
          ImGui::EndDisabled();
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
          const char *altitudeModes[] = {"真实高度", "时间线性"};
          int altitudeMode = static_cast<int>(settings.shadows.altitudeMode);
          if (ImGui::Combo("阴影高度曲线", &altitudeMode, altitudeModes,
                           IM_ARRAYSIZE(altitudeModes))) {
            settings.shadows.altitudeMode =
                static_cast<War3ShadowAltitudeMode>(altitudeMode);
          }
          ImGui::SliderFloat("阴影长度缩放",
                             &settings.shadows.shadowLengthScale, 0.6f, 1.2f,
                             "%.2f");
          ImGui::SliderFloat("阴影最长缩放",
                             &settings.shadows.shadowMaxLengthScale, 0.5f, 1.2f,
                             "%.2f");
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
          }
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
      }
    }

    if (ImGui::CollapsingHeader("Vulkan ShaderPack")) {
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
    }

    if (ImGui::CollapsingHeader("渲染统计 (Render Stats)")) {
      // Query Draw Calls
      ImGui::Text("渲染队列: 激活");
    }
  }
  ImGui::End();
}

void War3Imgui::setCursorBitmap(int width, int height, const void *bgraData,
                                int hotX, int hotY) {
  if (!m_device || width <= 0 || height <= 0 || !bgraData)
    return;

  // Release old texture if size changed (or just release to assume simplicity)
  // Optimization: Keep if size matches.
  if (m_cursorTexture) {
    D3DSURFACE_DESC desc;
    m_cursorTexture->GetLevelDesc(0, &desc);
    if (desc.Width != (UINT)width || desc.Height != (UINT)height) {
      m_cursorTexture->Release();
      m_cursorTexture = nullptr;
    }
  }

  if (!m_cursorTexture) {
    if (FAILED(m_device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED, &m_cursorTexture,
                                       NULL))) {
      Logger::err("War3Imgui: Failed to create cursor texture");
      return;
    }
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

  if (m_cursorTexture) {
    // Draw custom cursor
    ImVec2 tl = ImVec2(mousePos.x - m_cursorHotX, mousePos.y - m_cursorHotY);
    ImVec2 br = ImVec2(tl.x + m_cursorWidth, tl.y + m_cursorHeight);
    drawList->AddImage((ImTextureID)m_cursorTexture, tl, br);
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
