// war3_proxy_mode.cpp - see war3_proxy_mode.h

#include "war3_proxy_mode.h"

#include "war3_proxy_auto.h"
#include "war3_proxy_overlay.h"
#include "war3_proxy_services.h"
#include "../core/war3_d3d9_observer.h"
#include "../core/war3_internal_test_config.h"
#include "../model/war3_model_hook.h"
#include "../../d3d9_war3_hook.h"
#include "../../../util/config/config.h"
#include "../../../util/log/log.h"
#include "../../../util/util_env.h"
#include "../../../util/util_string.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

namespace dxvk::war3::proxy {

namespace {

// The front end's entry points. Loaded once and never unloaded: the game
// makes and drops several Direct3D9 objects before it creates its device.
struct FrontEnd {
  HMODULE module = nullptr;
  IDirect3D9* (WINAPI* create)(UINT) = nullptr;
  long (WINAPI* setObserver)(IDirect3DDevice9*, const WarVKD3D9Observer*) = nullptr;
  HRESULT (WINAPI* setFrameScene)(IDirect3DDevice9*, UINT, const void*, UINT) = nullptr;
  HRESULT (WINAPI* annotateNextDraw)(IDirect3DDevice9*, UINT, const void*, UINT) = nullptr;
  HRESULT (WINAPI* markStage)(IDirect3DDevice9*, UINT) = nullptr;
  HRESULT (WINAPI* requestSessionReset)(IDirect3DDevice9*, UINT, UINT, UINT) = nullptr;
  long (WINAPI* setWorldLighting)(IDirect3DDevice9*, const WarVKD3D9WorldLighting*) = nullptr;
  // The settings panel's (war3_proxy_overlay.h). Optional: a front end
  // without them runs without the panel.
  long (WINAPI* retainPrivate)(IDirect3DDevice9*, IUnknown*) = nullptr;
  long (WINAPI* releasePrivate)(IDirect3DDevice9*, IUnknown*) = nullptr;
};

std::once_flag g_frontOnce;
FrontEnd g_front;

// Owner-thread state of the connected device. The counters are read by the
// control plane's thread, so they are atomics; the pointer is owner-only.
IDirect3DDevice9* g_device = nullptr;
std::atomic<bool> g_deviceConnected{false};
std::atomic<bool> g_terminal{false};
std::atomic<uint32_t> g_factories{0};
std::atomic<uint32_t> g_devices{0};
std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_presents{0};
std::atomic<uint32_t> g_width{0};
std::atomic<uint32_t> g_height{0};
std::mutex g_terminalMutex;
char g_terminalKind[24] = {};
char g_terminalDetail[48] = {};

// ---- buffers of a device that follows another ---------------------------------

// The game keeps its place in its dynamic vertex and index buffers when the
// device goes away (the campaign closes it for a movie) and goes on appending
// from there in the buffers it creates on the next device. D3D9 allows that.
// The front end lets a lock start only inside what has been written, so every
// lock of such a buffer is refused, its draws with it, and the game's place
// never moves: a black loading screen, then a mission without its interface.
// Buffers created once a device has ended are defined in full at creation.
std::atomic<bool> g_deviceEnded{false};
std::atomic<uint32_t> g_definedBuffers{0};

using CreateVertexBufferFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
                                                          IDirect3DVertexBuffer9**, HANDLE*);
using CreateIndexBufferFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                         IDirect3DIndexBuffer9**, HANDLE*);
CreateVertexBufferFn g_createVertexBuffer = nullptr;
CreateIndexBufferFn g_createIndexBuffer = nullptr;
constexpr size_t kCreateVertexBufferSlot = 26u;
constexpr size_t kCreateIndexBufferSlot = 27u;

template <class Buffer> bool ZeroBufferRange(Buffer* buffer, UINT size) {
  void* bytes = nullptr;
  if (FAILED(buffer->Lock(0u, size, &bytes, D3DLOCK_DISCARD)) || bytes == nullptr)
    return false;
  std::memset(bytes, 0, size);
  return SUCCEEDED(buffer->Unlock());
}

template <class Buffer> void DefineWholeBuffer(Buffer* buffer, UINT length, DWORD usage) {
  if (!g_deviceEnded.load(std::memory_order_acquire) || !(usage & D3DUSAGE_DYNAMIC) || length == 0u)
    return;
  // The second DISCARD leaves all of it initialized and almost none of it
  // current, so that appending past it needs no copy of the old contents.
  const bool defined = ZeroBufferRange(buffer, length) && ZeroBufferRange(buffer, (std::min)(length, UINT(16u)));
  const uint32_t count = g_definedBuffers.fetch_add(1u, std::memory_order_relaxed);
  if (!defined)
    Logger::warn(str::format("WarVK proxy mode: dynamic buffer of ", length, " bytes not defined"));
  else if (count < 8u)
    Logger::info(str::format("WarVK proxy mode: dynamic buffer of ", length, " bytes defined for the new device"));
}

HRESULT STDMETHODCALLTYPE CreateVertexBufferDefined(IDirect3DDevice9* device, UINT length, DWORD usage, DWORD fvf,
                                                    D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared) {
  const HRESULT hr = g_createVertexBuffer(device, length, usage, fvf, pool, out, shared);
  if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    DefineWholeBuffer(*out, length, usage);
  return hr;
}

HRESULT STDMETHODCALLTYPE CreateIndexBufferDefined(IDirect3DDevice9* device, UINT length, DWORD usage, D3DFORMAT format,
                                                   D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared) {
  const HRESULT hr = g_createIndexBuffer(device, length, usage, format, pool, out, shared);
  if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    DefineWholeBuffer(*out, length, usage);
  return hr;
}

template <class Fn> void ReplaceDeviceSlot(void** table, size_t slot, Fn detour, Fn& original) {
  void* const replacement = reinterpret_cast<void*>(detour);
  if (table[slot] == replacement)
    return;
  DWORD protection = 0;
  if (!::VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &protection)) {
    Logger::warn(str::format("WarVK proxy mode: device slot ", slot, " not replaced, error ", ::GetLastError()));
    return;
  }
  original = reinterpret_cast<Fn>(table[slot]);
  table[slot] = replacement;
  ::VirtualProtect(&table[slot], sizeof(void*), protection, &protection);
}

std::wstring FrontPath() {
  wchar_t value[1024] = {};
  const DWORD size = ::GetEnvironmentVariableW(L"WARVK_D3D9_FRONT", value, 1024);
  return size && size < 1024 ? std::wstring(value, size) : std::wstring();
}

template <class T> bool Resolve(HMODULE module, const char* name, T& out) {
  out = reinterpret_cast<T>(::GetProcAddress(module, name));
  return out != nullptr;
}

void LoadFrontEnd() {
  const std::wstring path = FrontPath();
  if (path.empty())
    return;
  // The front end is named by its full path: no search order is involved.
  HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr) {
    Logger::err(str::format("WarVK proxy mode: front end not loaded, error ", ::GetLastError()));
    return;
  }
  FrontEnd front;
  front.module = module;
  const bool complete =
      Resolve(module, "Direct3DCreate9", front.create) &&
      Resolve(module, "WarVKD3D9SetObserver", front.setObserver) &&
      Resolve(module, "WarVKD3D9SetFrameScene", front.setFrameScene) &&
      Resolve(module, "WarVKD3D9AnnotateNextDraw", front.annotateNextDraw) &&
      Resolve(module, "WarVKD3D9MarkStage", front.markStage) &&
      Resolve(module, "WarVKD3D9RequestSessionReset", front.requestSessionReset) &&
      Resolve(module, "WarVKD3D9SetWorldLighting", front.setWorldLighting);
  if (!complete) {
    Logger::err("WarVK proxy mode: front end lacks a required export");
    return;
  }
  if (!Resolve(module, "WarVKD3D9RetainPrivate", front.retainPrivate) ||
      !Resolve(module, "WarVKD3D9ReleasePrivate", front.releasePrivate)) {
    front.retainPrivate = nullptr;
    front.releasePrivate = nullptr;
  }
  g_front = front;
  Logger::info("WarVK proxy mode: front end loaded");
}

const FrontEnd* Front() {
  std::call_once(g_frontOnce, LoadFrontEnd);
  return g_front.create != nullptr ? &g_front : nullptr;
}

// ---- observer: the four moments the in-process device used to provide ----

void __stdcall OnDraw(void*, IDirect3DDevice9* device, const WarVKD3D9DrawFacts* facts) {
  g_draws.fetch_add(1, std::memory_order_relaxed);
  ObserveDraw(device, *facts);
}

void __stdcall OnPresent(void*, IDirect3DDevice9* device) {
  g_presents.fetch_add(1, std::memory_order_relaxed);
  ObservePresent(device);
}

void __stdcall OnResized(void*, IDirect3DDevice9* device, uint32_t previousWidth,
                         uint32_t previousHeight, uint32_t width, uint32_t height) {
  g_width.store(width, std::memory_order_relaxed);
  g_height.store(height, std::memory_order_relaxed);
  ObserveResized(device, previousWidth, previousHeight, width, height);
}

void __stdcall OnTerminal(void*, IDirect3DDevice9* device, const WarVKD3D9TerminalFacts* facts) {
  if (facts == nullptr || facts->size != sizeof(WarVKD3D9TerminalFacts))
    return;
  const char* kind = facts->kind ? facts->kind : "";
  const char* detail = facts->detail ? facts->detail : "";
  {
    std::lock_guard<std::mutex> lock(g_terminalMutex);
    std::snprintf(g_terminalKind, sizeof(g_terminalKind), "%s", kind);
    std::snprintf(g_terminalDetail, sizeof(g_terminalDetail), "%s", detail);
  }
  g_terminal.store(true, std::memory_order_release);
  Logger::err(str::format("WarVK proxy mode: render host session ended: ",
                          kind, " (", detail, ") result=", facts->result));
  ObserveTerminal(device, *facts);
}

void __stdcall OnReset(void*, IDirect3DDevice9* device) {
  // Default-pool buffers are made again after a Reset, as after a new device.
  g_deviceEnded.store(true, std::memory_order_release);
  OverlayReset(device);
}

void __stdcall OnReleased(void*, IDirect3DDevice9* device) {
  if (g_device != device)
    return;
  g_deviceEnded.store(true, std::memory_order_release);
  // The panel first: its resources are released while the device lives.
  OverlayReleased(device);
  DisconnectDevice(device);
  g_deviceConnected.store(false, std::memory_order_release);
  g_device = nullptr;
  Logger::info("WarVK proxy mode: device released");
}

void ConnectCreatedDevice(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& parameters) {
  const FrontEnd* front = Front();
  void** const table = *reinterpret_cast<void***>(device);
  ReplaceDeviceSlot(table, kCreateVertexBufferSlot, &CreateVertexBufferDefined, g_createVertexBuffer);
  ReplaceDeviceSlot(table, kCreateIndexBufferSlot, &CreateIndexBufferDefined, g_createIndexBuffer);
  WarVKD3D9Observer observer = {};
  observer.size = sizeof(observer);
  observer.draw = &OnDraw;
  observer.present = &OnPresent;
  observer.resized = &OnResized;
  observer.terminal = &OnTerminal;
  observer.released = &OnReleased;
  observer.reset = &OnReset;
  long observed = front->setObserver(device, &observer);
  // A front end of the earlier observer layout (no `reset`): everything but
  // the settings panel, which could not let a Reset through.
  bool overlay = observed == 0 && front->retainPrivate != nullptr;
  if (observed != 0) {
    observer.size = offsetof(WarVKD3D9Observer, reset);
    observed = front->setObserver(device, &observer);
    overlay = false;
  }
  g_device = device;
  g_width.store(parameters.BackBufferWidth, std::memory_order_relaxed);
  g_height.store(parameters.BackBufferHeight, std::memory_order_relaxed);
  g_devices.fetch_add(1, std::memory_order_relaxed);
  Logger::info(str::format("WarVK proxy mode: device connected ", parameters.BackBufferWidth, "x",
                           parameters.BackBufferHeight, " observer=", observed));
  // The same order as the in-process device: services published, then the
  // Game.dll hooks installed on the device (D3D9InterfaceEx::CreateDeviceEx).
  ConnectDevice(device);
  g_deviceConnected.store(true, std::memory_order_release);
  War3Hook::InstallHooks(device);
  // After the front end's own window subclass (made by CreateDevice).
  OverlayConnect(device, overlay);
}

// ---- the factory the game sees ----

class Factory final : public IDirect3D9 {
public:
  explicit Factory(IDirect3D9* inner) : m_inner(inner) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9)) {
      *object = static_cast<IDirect3D9*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return m_references.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = m_references.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
      m_inner->Release();
      delete this;
    }
    return left;
  }
  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override {
    return m_inner->RegisterSoftwareDevice(init);
  }
  UINT STDMETHODCALLTYPE GetAdapterCount() override { return m_inner->GetAdapterCount(); }
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags,
                                                 D3DADAPTER_IDENTIFIER9* identifier) override {
    return m_inner->GetAdapterIdentifier(adapter, flags, identifier);
  }
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT format) override {
    return m_inner->GetAdapterModeCount(adapter, format);
  }
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode,
                                             D3DDISPLAYMODE* out) override {
    return m_inner->EnumAdapterModes(adapter, format, mode, out);
  }
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) override {
    return m_inner->GetAdapterDisplayMode(adapter, mode);
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display,
                                            D3DFORMAT back, BOOL windowed) override {
    return m_inner->CheckDeviceType(adapter, type, display, back, windowed);
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                              DWORD usage, D3DRESOURCETYPE resource,
                                              D3DFORMAT format) override {
    return m_inner->CheckDeviceFormat(adapter, type, adapterFormat, usage, resource, format);
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format,
                                                       BOOL windowed, D3DMULTISAMPLE_TYPE samples,
                                                       DWORD* quality) override {
    return m_inner->CheckDeviceMultiSampleType(adapter, type, format, windowed, samples, quality);
  }
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                                   D3DFORMAT target, D3DFORMAT depth) override {
    return m_inner->CheckDepthStencilMatch(adapter, type, adapterFormat, target, depth);
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT source,
                                                        D3DFORMAT target) override {
    return m_inner->CheckDeviceFormatConversion(adapter, type, source, target);
  }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override {
    return m_inner->GetDeviceCaps(adapter, type, caps);
  }
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override {
    return m_inner->GetAdapterMonitor(adapter);
  }
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                         D3DPRESENT_PARAMETERS* parameters,
                                         IDirect3DDevice9** device) override {
    // The present interval the in-process device gets. Every later Reset reads
    // the game's parameters through the lifecycle hook, which forces an
    // immediate present (Hook_GetD3d9Parameters); the device itself is created
    // before that hook is installed, so in process InitialReset applies the
    // same rule (War3ForceImmediatePresent, frame-rate cap lifted by default).
    // Without it the game's DEFAULT reaches the host as FIFO.
    if (parameters != nullptr && internal::kWar3ForceImmediatePresentEnabled)
      parameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    const HRESULT hr = m_inner->CreateDevice(adapter, type, window, flags, parameters, device);
    Logger::info(str::format("WarVK proxy mode: CreateDevice hr=0x", std::hex, uint32_t(hr), std::dec));
    if (SUCCEEDED(hr) && device != nullptr && *device != nullptr && parameters != nullptr)
      ConnectCreatedDevice(*device, *parameters);
    return hr;
  }

private:
  ~Factory() = default;
  IDirect3D9* m_inner;
  std::atomic<ULONG> m_references{1};
};

} // namespace

bool Requested() noexcept {
  return autostart::CurrentMode() != autostart::Mode::Off;
}

// The in-process interface makes the process DPI aware when the game creates
// its Direct3D9 object (D3D9InterfaceEx, d3d9.dpiAware): the game's window and
// its back buffer are then in physical pixels. The same here, by the same
// option; without it the host draws a smaller picture and the system enlarges it.
static void ApplyDpiAwareness() {
  static std::once_flag once;
  std::call_once(once, [] {
    Config config = Config::getUserConfig();
    config.merge(Config::getAppConfig(env::getExePath()));
    if (config.getOption<bool>("d3d9.dpiAware", true)) {
      Logger::info("WarVK proxy mode: process set as DPI aware");
      SetProcessDPIAware();
    }
  });
}

// Runs before any device exists, so no hook reads the tables yet.
static IDirect3D9* FallBackToInProcess(const char* reason) noexcept {
  try {
    Logger::warn(str::format("WarVK proxy mode: continuing in process: ", reason));
    model::AllocateProducerPaletteTables();
  } catch (...) {
  }
  autostart::FallBack(reason);
  return nullptr;
}

// The automatic start covers the game version the proxy was tested with, 1.27a.
static bool IsSupportedGame() noexcept {
  const auto* base = reinterpret_cast<const uint8_t*>(::GetModuleHandleW(L"Game.dll"));
  if (base == nullptr)
    return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  return nt->FileHeader.TimeDateStamp == 0x56BD0E1Cu && nt->OptionalHeader.SizeOfImage == 0x00CD8000u;
}

IDirect3D9* CreateFactory(UINT sdkVersion) noexcept {
  static std::atomic<bool> hostAnswered{false};
  const bool automatic = autostart::CurrentMode() == autostart::Mode::Automatic;
  try {
    if (automatic) {
      autostart::PrepareFirstFactory();
      if (!IsSupportedGame())
        return FallBackToInProcess("Game.dll is not 1.27a");
    }
    const FrontEnd* front = Front();
    if (front == nullptr)
      return automatic ? FallBackToInProcess("front end not loaded") : nullptr;
    ApplyDpiAwareness();
    IDirect3D9* inner = front->create(sdkVersion);
    if (inner == nullptr) {
      Logger::err("WarVK proxy mode: the front end refused Direct3DCreate9");
      return automatic ? FallBackToInProcess("front end refused") : nullptr;
    }
    if (automatic && !hostAnswered.load(std::memory_order_acquire)) {
      D3DCAPS9 caps{};
      if (FAILED(inner->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps))) {
        inner->Release();
        return FallBackToInProcess("x64 host did not answer");
      }
      hostAnswered.store(true, std::memory_order_release);
    }
    g_factories.fetch_add(1, std::memory_order_relaxed);
    return new Factory(inner);
  } catch (...) {
    return automatic ? FallBackToInProcess("factory creation failed") : nullptr;
  }
}

IDirect3DDevice9* ConnectedDevice() noexcept { return g_device; }

HRESULT SetWorldLighting(const WarVKD3D9WorldLighting* lighting) noexcept {
  return g_device != nullptr ? HRESULT(g_front.setWorldLighting(g_device, lighting)) : D3DERR_INVALIDCALL;
}

HRESULT SetFrameScene(uint32_t schema, const void* data, uint32_t bytes) noexcept {
  return g_device != nullptr ? g_front.setFrameScene(g_device, schema, data, bytes) : D3DERR_INVALIDCALL;
}

HRESULT AnnotateNextDraw(uint32_t schema, const void* data, uint32_t bytes) noexcept {
  return g_device != nullptr ? g_front.annotateNextDraw(g_device, schema, data, bytes) : D3DERR_INVALIDCALL;
}

HRESULT MarkStage(uint32_t stage) noexcept {
  return g_device != nullptr ? g_front.markStage(g_device, stage) : D3DERR_INVALIDCALL;
}

HRESULT RetainPrivate(IUnknown* resource) noexcept {
  return g_device != nullptr && g_front.retainPrivate != nullptr
      ? HRESULT(g_front.retainPrivate(g_device, resource)) : D3DERR_INVALIDCALL;
}

HRESULT ReleasePrivate(IUnknown* resource) noexcept {
  const HRESULT hr = g_device != nullptr && g_front.releasePrivate != nullptr
      ? HRESULT(g_front.releasePrivate(g_device, resource)) : D3DERR_INVALIDCALL;
  static std::atomic<bool> logged{false};
  if (FAILED(hr) && !logged.exchange(true))
    Logger::err(str::format("WarVK proxy mode: a panel resource was not released, hr=0x", std::hex, uint32_t(hr)));
  return hr;
}

HRESULT RequestSessionReset(uint32_t reason, uint64_t generation) noexcept {
  return g_device != nullptr
      ? g_front.requestSessionReset(g_device, reason, uint32_t(generation), uint32_t(generation >> 32))
      : D3DERR_INVALIDCALL;
}

Status QueryStatus() noexcept {
  Status status;
  status.requested = Requested();
  status.frontLoaded = g_front.create != nullptr;
  status.deviceConnected = g_deviceConnected.load(std::memory_order_acquire);
  status.terminal = g_terminal.load(std::memory_order_acquire);
  status.factories = g_factories.load(std::memory_order_relaxed);
  status.devices = g_devices.load(std::memory_order_relaxed);
  status.draws = g_draws.load(std::memory_order_relaxed);
  status.presents = g_presents.load(std::memory_order_relaxed);
  status.width = g_width.load(std::memory_order_relaxed);
  status.height = g_height.load(std::memory_order_relaxed);
  if (status.terminal) {
    std::lock_guard<std::mutex> lock(g_terminalMutex);
    std::memcpy(status.terminalKind, g_terminalKind, sizeof(status.terminalKind));
    std::memcpy(status.terminalDetail, g_terminalDetail, sizeof(status.terminalDetail));
  }
  return status;
}

} // namespace dxvk::war3::proxy
