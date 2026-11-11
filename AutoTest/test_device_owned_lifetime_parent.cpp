// Actual x86 public-D3D9 lifecycle caller. Uses only the isolated companion DLL's
// documented scalar probe; no production/private headers or copied COM model.
// Run under the parent's owned-process watchdog: --dll ABS --cycles 3.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#include <d3d9.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

static_assert(sizeof(void*) == 4, "Run this production API fixture as x86");

namespace {
constexpr UINT Width = 160, Height = 128;
constexpr uint32_t FieldCount = 39;
using Probe = HRESULT (__cdecl*)(IDirect3DDevice9*, uint32_t, uint64_t*, uint32_t);
using Values = std::array<uint64_t, FieldCount>;
enum Operation : uint32_t {
  Snapshot = 0, UiDraw = 1, CursorSmall = 2, CursorLarge = 3,
  PostFx = 4, WrongOwner = 5, PostFxPartialRetry = 6,
  CursorSmallUpdate = 7, InstanceBuffer = 8, InstanceBufferArm = 9,
};
enum Field : uint32_t {
  Version = 0, Op = 1, RefsBefore = 2, RefsAfter = 3, UiInitialized = 4,
  UiOwner = 5, ContextPresent = 6, Vertices = 7, Indices = 8, Commands = 9,
  AtlasLive = 10, AtlasPixels = 11, GpuDraws = 12, GpuCompleted = 13,
  PostFxReady = 14, PostFxApplied = 15, PartialRejected = 16,
  WrongOwnerPreserved = 17, LosableCheckEnabled = 18,
  CursorWidth = 19, CursorHeight = 20, PixelsChecked = 21, PixelFailures = 22,
  CursorCommands = 23, CursorPixelsChecked = 24, CursorPixelFailures = 25,
  CursorColor = 26, InstanceBufferReady = 27, InstanceBytesWritten = 28,
  InstanceStride = 29, InstanceBound = 30, InstanceTextureRetained = 31,
  InstanceTexturePixels = 32, InstanceTextureFailures = 33,
  InstanceOtherOwner = 34, InstanceTlsIsolation = 35, InstanceActive = 36,
  InstanceStableOwner = 37, Reserved = 38,
};
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void Check(HRESULT hr, const char* operation) {
  if (hr == D3D_OK) return;
  char message[180];
  std::snprintf(message, sizeof(message), "%s: HRESULT=0x%08lx", operation, static_cast<unsigned long>(hr));
  throw std::runtime_error(message);
}
const char* Bool(bool value) { return value ? "true" : "false"; }
std::string Quote(const char* text) {
  std::string result = "\"";
  for (size_t i = 0; text && text[i] && i < 600; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch == '\\' || ch == '"') { result += '\\'; result += char(ch); }
    else if (ch >= 32 && ch < 127) result += char(ch);
    else result += '?';
  }
  return result + '"';
}
template<class T> struct Com {
  T* ptr = nullptr;
  ~Com() { reset(); }
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  T* operator->() const { return ptr; }
  T** put() { reset(); return &ptr; }
  T* detach() { T* result = ptr; ptr = nullptr; return result; }
  void reset() { if (ptr) detach()->Release(); }
};

struct WindowOwner {
  HANDLE ready = nullptr, thread = nullptr;
  std::atomic<HWND> window{nullptr};
  std::atomic<DWORD> failure{0};
  std::atomic<uint32_t> messages{0};
  std::atomic<bool> unregistered{false};
  std::wstring className;
  bool closed = false;

  static LRESULT CALLBACK Procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<WindowOwner*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (msg == WM_DESTROY) {
      if (self) self->window.store(nullptr, std::memory_order_release);
      PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
  }
  static unsigned __stdcall Run(void* argument) noexcept {
    auto& self = *static_cast<WindowOwner*>(argument);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = Procedure; cls.lpszClassName = self.className.c_str();
    if (!RegisterClassExW(&cls)) {
      self.failure.store(GetLastError(), std::memory_order_release);
      SetEvent(self.ready); return 2;
    }
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, self.className.c_str(),
        L"WarVK hidden ownership fixture", WS_POPUP, 0, 0, Width, Height,
        nullptr, nullptr, instance, &self);
    if (!window) {
      self.failure.store(GetLastError(), std::memory_order_release);
      self.unregistered.store(UnregisterClassW(self.className.c_str(), instance) != FALSE);
      SetEvent(self.ready); return 2;
    }
    self.window.store(window, std::memory_order_release);
    SetEvent(self.ready);
    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
      TranslateMessage(&message); DispatchMessageW(&message);
      self.messages.fetch_add(1, std::memory_order_relaxed);
    }
    if (result == -1) self.failure.store(GetLastError(), std::memory_order_release);
    if (self.window.load(std::memory_order_acquire)) DestroyWindow(window);
    self.unregistered.store(UnregisterClassW(self.className.c_str(), instance) != FALSE);
    return result == -1 ? 2u : 0u;
  }
  void start(unsigned slot) {
    className = L"WarVK_DeviceOwner_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(slot);
    ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Require(ready, "Create window readiness event");
    thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Run, this, 0, nullptr));
    Require(thread, "Start HWND owner thread");
    Require(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "Window ready timeout");
    HWND handle = window.load(std::memory_order_acquire);
    RECT size{};
    Require(handle && !failure.load() && !IsWindowVisible(handle), "Window unavailable or visible");
    Require(GetClientRect(handle, &size) && size.right == LONG(Width) && size.bottom == LONG(Height), "Wrong client extent");
  }
  void close() {
    HWND handle = window.load(std::memory_order_acquire);
    Require(handle && !IsWindowVisible(handle), "Window disappeared or became visible");
    Require(PostMessageW(handle, WM_CLOSE, 0, 0) != FALSE, "Post owned WM_CLOSE");
    Require(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "Window join timeout");
    DWORD exit = STILL_ACTIVE;
    Require(GetExitCodeThread(thread, &exit) && exit == 0 && !failure.load() && unregistered.load(), "Window normal shutdown failed");
    Require(!window.load(), "Owned HWND remains");
    CloseHandle(thread); thread = nullptr;
    CloseHandle(ready); ready = nullptr;
    closed = true;
  }
};

struct State {
  const char* stage = "arguments";
  std::wstring dll;
  uint32_t requestedCycles = 3, cycles = 0, created = 0, destroyed = 0, probes = 0;
  uint32_t uiDraws = 0, postFxRuns = 0, partialRetries = 0, cursorUpdates = 0;
  uint32_t resets = 0, resetRejectChecks = 0, resetRejectNotEnabled = 0, ownerChecks = 0, heldDeviceChecks = 0;
  uint32_t instanceBufferRuns = 0, instanceArms = 0, instanceOtherOwnerChecks = 0, instanceTlsIsolationChecks = 0;
  uint64_t actualGpuDraws = 0, postFxPixels = 0, pixelFailures = 0, cursorPixels = 0, cursorPixelFailures = 0;
  uint64_t instanceBytesWritten = 0, instanceTexturePixels = 0, instanceTextureFailures = 0;
  bool moduleUnloaded = false, normalTeardown = false;
  ULONG factoryFinalRefs = ~ULONG(0), deviceFinalRefs = ~ULONG(0);
  HMODULE module = nullptr;
  Probe probe = nullptr;
  Com<IDirect3D9> factory;
  Com<IDirect3DDevice9> primary, secondary;
  WindowOwner windows[2];
};

void Stage(State& state, const char* stage) {
  state.stage = stage;
  std::printf("DEVICE_OWNER_STAGE stage=%s\n", stage); std::fflush(stdout);
}
void NoGame() {
  Require(!GetModuleHandleW(L"Game.dll") && !GetModuleHandleW(L"War3.exe") &&
      !GetModuleHandleW(L"Warcraft III.exe"), "Game module forbidden in fixture");
}
ULONG PublicRefs(IDirect3DDevice9* device) {
  device->AddRef(); return device->Release();
}
D3DPRESENT_PARAMETERS Parameters(HWND window) {
  D3DPRESENT_PARAMETERS params{};
  params.BackBufferWidth = Width; params.BackBufferHeight = Height;
  params.BackBufferFormat = D3DFMT_A8R8G8B8; params.BackBufferCount = 1;
  params.MultiSampleType = D3DMULTISAMPLE_NONE;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD; params.Windowed = TRUE;
  params.hDeviceWindow = window; params.EnableAutoDepthStencil = FALSE;
  params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  return params;
}
void Create(State& state, Com<IDirect3DDevice9>& device, unsigned slot) {
  D3DPRESENT_PARAMETERS params = Parameters(state.windows[slot].window.load());
  Check(state.factory->CreateDevice(0, D3DDEVTYPE_HAL, params.hDeviceWindow,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &params, device.put()), "CreateDevice");
  Require(device.ptr, "CreateDevice returned null");
  ++state.created;
  Require(PublicRefs(device.ptr) == 1, "New device has unexpected internal public references");
}
Values Call(State& state, IDirect3DDevice9* device, uint32_t operation) {
  Values values{};
  const HRESULT result = state.probe(device, operation, values.data(), uint32_t(values.size()));
  ++state.probes;
  std::printf("DEVICE_OWNER_PROBE {\"op\":%u,\"hr\":%lu,\"values\":[", operation, static_cast<unsigned long>(result));
  for (size_t i = 0; i < values.size(); ++i)
    std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(values[i]));
  std::printf("]}\n"); std::fflush(stdout);
  Check(result, "Companion probe");
  Require(values[Version] == 1 && values[Op] == operation && values[Reserved] == 0,
      "Probe returned wrong ABI");
  Require(values[RefsBefore] == values[RefsAfter], "Probe leaked public references");
  state.actualGpuDraws += values[GpuDraws];
  state.postFxPixels += values[PixelsChecked];
  state.pixelFailures += values[PixelFailures];
  state.cursorPixels += values[CursorPixelsChecked];
  state.cursorPixelFailures += values[CursorPixelFailures];
  state.instanceBytesWritten += values[InstanceBytesWritten];
  state.instanceTexturePixels += values[InstanceTexturePixels];
  state.instanceTextureFailures += values[InstanceTextureFailures];
  Require(values[InstanceActive] == (operation == InstanceBufferArm ? 1u : 0u),
      "instance TLS active owner was not isolated/cleared");
  return values;
}
void DrawUi(State& state, uint32_t operation = UiDraw) {
  const Values values = Call(state, state.primary.ptr, operation);
  Require(values[UiOwner] == 1 && values[ContextPresent] == 1 && values[Vertices] &&
      values[Indices] && values[Commands] && values[AtlasLive] && values[AtlasPixels] &&
      values[GpuDraws] && values[GpuCompleted], "UI resource/draw evidence missing");
  Require(PublicRefs(state.primary.ptr) == 1, "UI or cursor retained public device refs");
  ++state.uiDraws;
  if (operation == CursorSmall || operation == CursorSmallUpdate || operation == CursorLarge) {
    const uint32_t expectedExtent = operation == CursorLarge ? 17u : 8u;
    const uint32_t expectedColor = operation == CursorLarge ? 0xff40a0e0u :
        (operation == CursorSmallUpdate ? 0xffe05060u : 0xff80c040u);
    Require(values[CursorWidth] == expectedExtent && values[CursorHeight] == expectedExtent &&
        values[CursorCommands] == 1 && values[CursorPixelsChecked] == expectedExtent * expectedExtent &&
        !values[CursorPixelFailures] && values[CursorColor] == expectedColor, "Cursor texture/draw receipt missing");
    ++state.cursorUpdates;
  }
}
void Reset(State& state, IDirect3DDevice9* device, unsigned slot) {
  D3DPRESENT_PARAMETERS params = Parameters(state.windows[slot].window.load());
  Check(device->Reset(&params), "Legal Reset");
  ++state.resets;
  Require(PublicRefs(device) == 1, "Reset retained internal public device refs");
}
void Release(State& state, Com<IDirect3DDevice9>& device) {
  state.deviceFinalRefs = device.detach()->Release();
  Require(state.deviceFinalRefs == 0, "Final device Release is not zero");
  ++state.destroyed;
}
void CheckInstanceBuffer(State& state, IDirect3DDevice9* device, bool secondary) {
  const Values values = Call(state, device, InstanceBuffer);
  Require(values[InstanceBufferReady] && values[InstanceStableOwner] &&
      values[InstanceBytesWritten] == 288 && values[InstanceStride] == 96 && values[InstanceBound] &&
      values[InstanceTextureRetained] && values[InstanceTexturePixels] == 16 && !values[InstanceTextureFailures],
      "real instance storage/captured texture evidence missing");
  Require(values[InstanceOtherOwner] == uint64_t(secondary) && values[InstanceTlsIsolation] == uint64_t(secondary),
      "instance cross-device isolation evidence mismatch");
  Require(PublicRefs(device) == 1, "instance resources retained public device refs");
  ++state.instanceBufferRuns;
  if (secondary) { ++state.instanceOtherOwnerChecks; ++state.instanceTlsIsolationChecks; }
}
void ArmInstanceBuffer(State& state, IDirect3DDevice9* device) {
  Require(Call(state, device, InstanceBufferArm)[InstanceActive] == 1, "instance owner not armed for destruction");
  ++state.instanceArms;
}

void RunCycle(State& state) {
  Stage(state, "create-primary");
  Create(state, state.primary, 0);
  const Values initial = Call(state, state.primary.ptr, Snapshot);
  Require(initial[UiInitialized] && initial[UiOwner] && initial[ContextPresent], "Primary UI did not initialize");
  Stage(state, "ui-and-cursor-resources");
  DrawUi(state);
  DrawUi(state, CursorSmall);
  DrawUi(state, CursorSmallUpdate); // Same extent, changed payload, no additional owner.
  DrawUi(state, CursorLarge);
  Stage(state, "postfx-and-partial-retry");
  for (uint32_t operation : {uint32_t(PostFx), uint32_t(PostFxPartialRetry)}) {
    const Values values = Call(state, state.primary.ptr, operation);
    Require(values[PostFxReady] && values[PostFxApplied] && values[GpuCompleted] &&
        values[GpuDraws] >= 12 && values[PixelsChecked] == 1024 && !values[PixelFailures], "PostFX render evidence missing");
    ++state.postFxRuns;
    if (operation == PostFxPartialRetry) {
      Require(values[PartialRejected], "Partial Init rejection was not exercised");
      ++state.partialRetries;
    }
    Require(PublicRefs(state.primary.ptr) == 1, "PostFX retained device refs");
  }
  Stage(state, "instance-storage-before-reset");
  CheckInstanceBuffer(state, state.primary.ptr, false);
  Stage(state, "reset-resource-contract");
  {
    Com<IDirect3DVertexBuffer9> held;
    Check(state.primary->CreateVertexBuffer(256, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
        held.put(), nullptr), "Create external DEFAULT buffer");
    D3DPRESENT_PARAMETERS params = Parameters(state.windows[0].window.load());
    const HRESULT resetResult = state.primary->Reset(&params);
    std::printf("DEVICE_OWNER_RESET {\"enabled\":%s,\"hr\":%lu}\n",
        Bool(initial[LosableCheckEnabled] != 0), static_cast<unsigned long>(resetResult));
    if (initial[LosableCheckEnabled]) {
      Require(resetResult == D3DERR_INVALIDCALL, "Reset accepted a live DEFAULT resource");
      ++state.resetRejectChecks;
    } else {
      Check(resetResult, "Reset with configured losable check disabled");
      ++state.resetRejectNotEnabled; // Not counted as rejection coverage.
    }
  }
  Reset(state, state.primary.ptr, 0);
  Stage(state, "instance-storage-after-reset");
  CheckInstanceBuffer(state, state.primary.ptr, false);
  DrawUi(state);

  Stage(state, "second-device-owner-isolation");
  Create(state, state.secondary, 1);
  Require(Call(state, state.secondary.ptr, Snapshot)[UiOwner] == 0, "Second device stole UI");
  Require(Call(state, state.secondary.ptr, WrongOwner)[WrongOwnerPreserved] == 1, "Wrong-owner cleanup changed UI");
  CheckInstanceBuffer(state, state.secondary.ptr, true);
  Reset(state, state.secondary.ptr, 1);
  Require(Call(state, state.primary.ptr, Snapshot)[UiOwner] == 1, "Second device Reset detached primary UI");
  DrawUi(state);
  ArmInstanceBuffer(state, state.secondary.ptr);
  Release(state, state.secondary);
  Require(Call(state, state.primary.ptr, Snapshot)[UiOwner] == 1, "Second device destruction detached primary UI");
  DrawUi(state);
  ++state.ownerChecks;

  Stage(state, "external-resource-retains-device");
  {
    Com<IDirect3DTexture9> held;
    Check(state.primary->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        held.put(), nullptr), "Create external texture");
    IDirect3DDevice9* original = state.primary.detach();
    Require(original->Release() == 1, "External texture did not retain exactly one device reference");
    Check(held->GetDevice(state.primary.put()), "Texture GetDevice after original owner Release");
    Require(state.primary.ptr == original, "GetDevice identity mismatch");
    held.reset();
    Require(PublicRefs(state.primary.ptr) == 1, "External texture did not release parent reference");
    Check(state.primary->TestCooperativeLevel(), "Retained device cooperative level");
    ++state.heldDeviceChecks;
  }
  Stage(state, "normal-device-destruction");
  ArmInstanceBuffer(state, state.primary.ptr);
  Release(state, state.primary);
  const Values ended = Call(state, nullptr, Snapshot);
  Require(!ended[UiInitialized] && !ended[UiOwner] && !ended[ContextPresent] && !ended[AtlasLive],
      "UI context/resources remained after device destruction");
  ++state.cycles;
  NoGame();
}

void Report(const State& state, bool ok, const char* error) {
  std::printf("DEVICE_OWNER_RESULT {\"schema\":1,\"ok\":%s,\"stage\":%s,\"error\":%s,"
      "\"requestedCycles\":%u,\"cycles\":%u,\"devicesCreated\":%u,\"devicesDestroyed\":%u,"
      "\"probes\":%u,\"uiDraws\":%u,\"cursorUpdates\":%u,\"postFxRuns\":%u,\"partialRetries\":%u,"
      "\"resets\":%u,\"resetRejectChecks\":%u,\"resetRejectNotEnabled\":%u,\"ownerChecks\":%u,"
      "\"heldDeviceChecks\":%u,\"actualGpuDraws\":%llu,\"postFxPixels\":%llu,\"pixelFailures\":%llu,"
      "\"cursorTexturePixels\":%llu,\"cursorPixelFailures\":%llu,"
      "\"instanceBufferRuns\":%u,\"instanceArms\":%u,\"instanceOtherOwnerChecks\":%u,\"instanceTlsIsolationChecks\":%u,"
      "\"instanceBytesWritten\":%llu,\"instanceTexturePixels\":%llu,\"instanceTextureFailures\":%llu,"
      "\"deviceFinalRefs\":%lu,\"factoryFinalRefs\":%lu,\"moduleUnloaded\":%s,"
      "\"windowsClosed\":%s,\"normalTeardown\":%s,\"gameLaunched\":false,\"globalInput\":false,"
      "\"allocatorFailureInjected\":false,\"visualGameAcceptance\":false,\"instanceDrawAcceptance\":false}\n",
      Bool(ok), Quote(state.stage).c_str(), Quote(error).c_str(), state.requestedCycles, state.cycles,
      state.created, state.destroyed, state.probes, state.uiDraws, state.cursorUpdates,
      state.postFxRuns, state.partialRetries, state.resets, state.resetRejectChecks,
      state.resetRejectNotEnabled, state.ownerChecks, state.heldDeviceChecks,
      static_cast<unsigned long long>(state.actualGpuDraws), static_cast<unsigned long long>(state.postFxPixels),
      static_cast<unsigned long long>(state.pixelFailures), static_cast<unsigned long long>(state.cursorPixels),
      static_cast<unsigned long long>(state.cursorPixelFailures), state.instanceBufferRuns, state.instanceArms,
      state.instanceOtherOwnerChecks, state.instanceTlsIsolationChecks,
      static_cast<unsigned long long>(state.instanceBytesWritten), static_cast<unsigned long long>(state.instanceTexturePixels),
      static_cast<unsigned long long>(state.instanceTextureFailures), static_cast<unsigned long>(state.deviceFinalRefs),
      static_cast<unsigned long>(state.factoryFinalRefs), Bool(state.moduleUnloaded),
      Bool(state.windows[0].closed && state.windows[1].closed), Bool(state.normalTeardown));
  std::fflush(stdout); std::fflush(stderr);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  // Deliberately do not run best-effort teardown after a failed acceptance step:
  // report the exact reached stage, exit 2, let the external watchdog settle us.
  State* state = new State;
  try {
    bool cyclesSeen = false;
    for (int i = 1; i < argc; ++i) {
      const std::wstring option = argv[i];
      if (option == L"--dll" && state->dll.empty() && i + 1 < argc) state->dll = argv[++i];
      else if (option == L"--cycles" && !cyclesSeen && i + 1 < argc) {
        const std::wstring number = argv[++i];
        Require(number.size() == 1 && number[0] >= L'1' && number[0] <= L'8', "cycles must be 1..8");
        state->requestedCycles = uint32_t(number[0] - L'0'); cyclesSeen = true;
      } else throw std::runtime_error("usage: --dll ABS [--cycles 1..8]");
    }
    const std::wstring& path = state->dll;
    const bool drive = path.size() >= 3 && path[1] == L':' && (path[2] == L'/' || path[2] == L'\\');
    const bool unc = path.size() > 3 && path[0] == L'\\' && path[1] == L'\\';
    Require(drive || unc, "DLL path must be absolute");
    const DWORD attributes = GetFileAttributesW(path.c_str());
    Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY), "DLL file missing");
    NoGame();
    Stage(*state, "hidden-windows");
    state->windows[0].start(0); state->windows[1].start(1);
    Stage(*state, "load-companion-dll");
    state->module = LoadLibraryW(path.c_str());
    Require(state->module, "Load companion DLL failed");
    using Create9 = IDirect3D9* (WINAPI*)(UINT);
    const auto create9 = reinterpret_cast<Create9>(GetProcAddress(state->module, "Direct3DCreate9"));
    state->probe = reinterpret_cast<Probe>(GetProcAddress(state->module, "WarVKTestDeviceOwned"));
    Require(create9 && state->probe, "Companion export missing");
    state->factory.ptr = create9(D3D_SDK_VERSION);
    Require(state->factory.ptr && state->factory->GetAdapterCount(), "Factory or adapter unavailable");
    for (uint32_t i = 0; i < state->requestedCycles; ++i) RunCycle(*state);
    Stage(*state, "normal-factory-destruction");
    state->factoryFinalRefs = state->factory.detach()->Release();
    Require(state->factoryFinalRefs == 0, "Final factory Release is not zero");
    Stage(*state, "normal-dll-unload");
    Require(FreeLibrary(state->module) != FALSE, "FreeLibrary failed");
    state->module = nullptr; state->probe = nullptr;
    state->moduleUnloaded = GetModuleHandleW(path.c_str()) == nullptr;
    Require(state->moduleUnloaded, "Companion DLL remains loaded");
    Stage(*state, "normal-window-close");
    state->windows[0].close(); state->windows[1].close();
    state->normalTeardown = true;
    Stage(*state, "complete");
    Report(*state, true, "");
    delete state;
    return 0;
  } catch (const std::exception& error) {
    Report(*state, false, error.what());
  } catch (...) {
    Report(*state, false, "unknown exception");
  }
  ExitProcess(2);
}
