// war3_proxy_services.cpp - see war3_proxy_services.h

#include "war3_proxy_services.h"

#include "war3_proxy_casters.h"
#include "war3_proxy_frame.h"
#include "war3_proxy_host_lost.h"
#include "war3_proxy_mode.h"
#include "war3_proxy_overlay.h"
#include "../war3.h"
#include "../hooks/war3_hook_lifecycle.h"
#include "../render/war3_lightning_runtime.h"
#include "../render/war3_render_exec_batch.h"
#include "../render/war3_render_state.h"
#include "../render/war3_renderer.h"
#include "../state/war3_render_state.h"
#include "../tools/war3_diagnostics_hub.h"
#include "../tools/war3_frame_capture.h"
// War3ShadowLifecycleDiagnostics only: a value type. No device symbol is used.
#include "../../d3d9_device.h"
#include "../../../util/log/log.h"
#include "../../../util/util_string.h"

#include <atomic>
#include <memory>

namespace dxvk::war3::proxy {

namespace {

// Owner-thread state.
bool g_inWorldScene = false;

// IDeviceServices for a device that lives in the x64 host. Each member's
// proxy-mode meaning is stated beside its declaration
// (war3_execution_services.h); what has no meaning there answers
// "unavailable" in the member's own terms.
class DeviceServices final : public IDeviceServices {
public:
  explicit DeviceServices(IDirect3DDevice9* device) : m_device(device) {}

  IDirect3DDevice9* device() const { return m_device; }

  void War3SetWorldLightingScope(bool inWorld) override {
    g_inWorldScene = inWorld;
    FrameWorldScene(inWorld);
  }

  void War3RequestShadowMapEpochReset() override {
    // The x86 half of a map change is the data layer's own CPU state; the
    // host's half is its session (shadow lifetime, content, effect history).
    ProcessServices().War3ResetCpuSemanticMapSession();
    // The map-wide render registries end with the map, as at the device's
    // own reset (War3ApplyShadowMapEpochResetAtPresent). The lightning
    // runtime's textures are created on the game's device and hold it: a
    // game that quits from inside a map could not destroy its device, and
    // the host's session would not end in order.
    render::War3LightningRuntime::instance().reset();
    const uint64_t generation = FrameSessionReset();
    const HRESULT hr = RequestSessionReset(1u, generation);
    Logger::info(str::format("WarVK proxy mode: map change, session reset generation=", generation,
                             " hr=0x", std::hex, uint32_t(hr), std::dec));
  }

  void War3AttachGpuSkinNativeBridge(uintptr_t) override {}

  uint32_t War3PopulateSemanticShadowSceneForValidation(bool, bool) override { return 0u; }
  bool War3ExecuteSemanticShadowSceneForValidation(bool, bool) override { return false; }

  War3ShadowLifecycleDiagnostics QueryWar3ShadowLifecycleDiagnostics() const override {
    return War3ShadowLifecycleDiagnostics{};
  }

  int64_t QueryWar3DeviceStatus() const override {
    // VK_SUCCESS, or VK_ERROR_DEVICE_LOST once the host session ended.
    return QueryStatus().terminal ? -4 : 0;
  }

  bool War3ResetGpuSkinBridgeForTest(bool) override { return false; }
  bool War3LogGpuSkinDiagnosticsForTest(bool, bool* outQuiescent) override {
    if (outQuiescent != nullptr)
      *outQuiescent = false;
    return false;
  }

  reimpl::War3InstanceBuffer* GetWar3InstanceBuffer() override { return nullptr; }

  bool War3CreateMaterialVertexShader(const uint32_t* tokens, size_t, const std::string& path,
                                      IDirect3DVertexShader9** shader, std::string* error) override {
    const HRESULT hr = m_device->CreateVertexShader(reinterpret_cast<const DWORD*>(tokens), shader);
    if (FAILED(hr) && error != nullptr)
      *error = str::format("CreateVertexShader failed for ", path, " hr=0x", std::hex, uint32_t(hr));
    return SUCCEEDED(hr);
  }

  bool War3CreateMaterialPixelShader(const uint32_t* tokens, size_t, const std::string& path,
                                     IDirect3DPixelShader9** shader, std::string* error) override {
    const HRESULT hr = m_device->CreatePixelShader(reinterpret_cast<const DWORD*>(tokens), shader);
    if (FAILED(hr) && error != nullptr)
      *error = str::format("CreatePixelShader failed for ", path, " hr=0x", std::hex, uint32_t(hr));
    return SUCCEEDED(hr);
  }

private:
  IDirect3DDevice9* m_device;
};

std::unique_ptr<DeviceServices> g_services;

} // namespace

void ConnectDevice(IDirect3DDevice9* device) {
  if (g_services != nullptr)
    DisconnectDevice(g_services->device());
  g_inWorldScene = false;
  g_services = std::make_unique<DeviceServices>(device);
  PublishProxyDeviceServices(g_services.get(), FrameSettingsMailbox());
  FrameConnect();
}

void DisconnectDevice(IDirect3DDevice9* device) {
  if (g_services == nullptr || g_services->device() != device)
    return;
  FrameDisconnect();
  ClearProxyDeviceServices(g_services.get());
  g_services.reset();
  g_inWorldScene = false;
}

void ObserveDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts) {
  FrameDraw(device, facts);
}

void ObservePresent(IDirect3DDevice9* device) {
  // The frame's records first: the world-stage flags they read are reset by
  // the frame boundary below.
  FramePresent(device);
  // Sandbox only (WARVK_D3D9_PANEL_TEST_OPEN set): the capture shows the panel.
  static const bool capturePanel = ::GetEnvironmentVariableW(L"WARVK_D3D9_PANEL_TEST_OPEN", nullptr, 0) > 0;
  const auto capture = [device] {
    if (!tools::HasPendingFrameCaptureRequest())
      return;
    IDirect3DSurface9* backBuffer = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
        backBuffer != nullptr) {
      tools::ProcessPendingFrameCapture(device, backBuffer);
      backBuffer->Release();
    }
  };
  if (!capturePanel)
    capture();
  // The settings panel: after the frame's records (FramePresent placed the
  // BeforeUi marker, so the host's effects never touch it) and after the
  // frame capture (which, as in process, does not show it). Its draws come
  // from inside the callback: the front end does not observe them.
  OverlayPresent(device);
  if (capturePanel)
    capture();

  // The frame boundary the in-process device keeps in Present / PresentEx
  // (d3d9_device.cpp): the data layer's frame index, the per-frame render
  // state, and the tracking the producers of this file read. Without it the
  // presented-frame serial never moves and the map-change gate
  // (ObserveWar3MainRunnerEnter) believes a world scene is still live.
  state::RenderState::instance().beginFrame();
  War3RenderState::OnFrameStart();
  War3RenderState::SetBatchTagTrackingEnabled(true);
  // What a caster's record says about its object comes from the batch
  // dispatcher's semantic context: the scene node and the world object behind
  // a draw. Only that - the shadow runtime registries the in-process semantic
  // scene reads (object identity tracking) stay off: nothing here reads the
  // game's models, the casters are its own draws. A caster's shadow class,
  // when the player sets one apart, is read from the game's own lists
  // (war3_proxy_casters.cpp, OwnerTable): identity tracking costs a dense
  // map ~190 ms a frame (measured 2026-11-19).
  War3RenderState::SetShadowSemanticTrackingEnabled(CastersWanted());
  render::War3Renderer::instance().BeginFrame();
  render::ExecBatchProcessor::ResetFrameCaches();
}

void ObserveResized(IDirect3DDevice9* device, uint32_t previousWidth, uint32_t previousHeight,
                    uint32_t width, uint32_t height) {
  // The front end did the first half of the in-process window hook's WM_SIZE
  // answer (d3d9_window.cpp): the window's WM_SIZE never reached the game, and
  // the swap chain now has the client extent. This is the second half: the
  // game's own resolution fields take the new extent, and its UI is told to
  // lay itself out again. Without it the game keeps setting its old viewport.
  // The game's D3D thread is its window thread, as it is in WM_SIZE.
  D3DDEVICE_CREATION_PARAMETERS creation = {};
  const HWND window = SUCCEEDED(device->GetCreationParameters(&creation)) ? creation.hFocusWindow : nullptr;
  const bool fields = hooks::TryOverrideWindowedClientSize(previousWidth, previousHeight, width, height);
  const char* uiFailure = "";
  const bool ui = hooks::TryNotifyWindowedUiSizeChanged(window, width, height, &uiFailure);
  Logger::info(str::format("WarVK proxy mode: window resized ", previousWidth, "x", previousHeight,
                           " -> ", width, "x", height, " gameFields=", fields ? 1 : 0, " gameUi=", ui ? 1 : 0,
                           ui ? "" : " (", uiFailure, ui ? "" : ")"));
}

void ObserveTerminal(IDirect3DDevice9*, const WarVKD3D9TerminalFacts& facts) {
  const Status status = QueryStatus();
  HostLostContext context;
  context.presents = status.presents;
  context.draws = status.draws;
  context.width = status.width;
  context.height = status.height;
  ReportRenderHostLostAndExit(facts, context);
}

bool InWorldScene() noexcept { return g_inWorldScene; }

} // namespace dxvk::war3::proxy
