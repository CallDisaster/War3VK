// Isolated public-D3D9 fixture. Compile this as a real Win32 executable and run
// each candidate/reference DLL in a SEPARATE process. No proxy/wire/backend
// headers are included: expected pixels come from the scene below, not from the
// implementation under test. This is not a Warcraft or release acceptance test.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

static_assert(sizeof(void*) == 4, "The D3D9 API caller must be an actual x86 process");

namespace {
constexpr UINT Width = 64, Height = 64;
constexpr UINT VertexBufferBytes = 131101;
constexpr UINT StreamOffset = 65560;
constexpr UINT FrontPatchOffset = 65880;
constexpr UINT FrontPatchBytes = 160;
constexpr DWORD Behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
constexpr uint64_t FnvOffset = 14695981039346656037ull;
constexpr uint64_t FnvPrime = 1099511628211ull;
constexpr GUID UnknownIid = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr wchar_t WindowClass[] = L"WarVK_Isolated_D3D9_API_Fixture";
constexpr UINT WsiFrames = 8;
constexpr UINT WindowPulse = WM_APP + 0x341;

void Require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}

void Check(HRESULT hr, const char* operation) {
  if (hr == D3D_OK) return;
  char text[180];
  std::snprintf(text, sizeof(text), "%s: HRESULT 0x%08lx", operation,
                static_cast<unsigned long>(hr));
  throw std::runtime_error(text);
}

const char* JsonBool(bool value) { return value ? "true" : "false"; }

std::string JsonString(const char* value) {
  std::string result = "\"";
  // Failure diagnostics remain bounded even if a compiler returns a large blob.
  for (size_t i = 0; value && value[i] && i < 1600; ++i) {
    const auto ch = static_cast<unsigned char>(value[i]);
    if (ch == '"' || ch == '\\') { result += '\\'; result += char(ch); }
    else if (ch == '\n') result += "\\n";
    else if (ch == '\r') result += "\\r";
    else if (ch == '\t') result += "\\t";
    else if (ch < 32) {
      char escape[7];
      std::snprintf(escape, sizeof(escape), "\\u%04x", unsigned(ch));
      result += escape;
    } else result += char(ch);
  }
  result += '"';
  return result;
}

std::wstring DesktopName(HDESK desktop) {
  Require(desktop != nullptr, "desktop handle unavailable");
  DWORD bytes = 0;
  GetUserObjectInformationW(desktop, UOI_NAME, nullptr, 0, &bytes);
  Require(bytes >= sizeof(wchar_t) && bytes <= 4096 && bytes % sizeof(wchar_t) == 0,
          "desktop name size unavailable/out of bounds");
  std::vector<wchar_t> name(bytes / sizeof(wchar_t), L'\0');
  Require(GetUserObjectInformationW(desktop, UOI_NAME, name.data(), bytes, &bytes) != FALSE &&
      !name.empty() && name.back() == L'\0' && name.front() != L'\0', "desktop name query failed");
  return std::wstring(name.data());
}

struct DesktopWitness { std::wstring thread, input; };

DesktopWitness NonInputDesktop() {
  DesktopWitness witness;
  witness.thread = DesktopName(GetThreadDesktop(GetCurrentThreadId()));
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  Require(input != nullptr, "input desktop query denied; cannot prove WSI isolation");
  try { witness.input = DesktopName(input); }
  catch (...) { CloseDesktop(input); throw; }
  Require(CloseDesktop(input) != FALSE, "input desktop query handle close");
  Require(_wcsicmp(witness.thread.c_str(), witness.input.c_str()) != 0,
          "WSI fixture refused on the input desktop");
  return witness;
}

std::string Utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()),
      nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return "<desktop-name-encoding-failed>";
  std::string result(size_t(bytes), '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()),
      result.data(), bytes, nullptr, nullptr)) return "<desktop-name-encoding-failed>";
  return result;
}

uint64_t Hash(const void* bytes, size_t size, uint64_t value = FnvOffset) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) value = (value ^ data[i]) * FnvPrime;
  return value;
}

template<class T> class Com {
public:
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { reset(); }
  T* get() const { return value; }
  T* operator->() const { return value; }
  T** put() { reset(); return &value; }
  T* detach() { T* result = value; value = nullptr; return result; }
  void reset() { if (value) { T* old = detach(); old->Release(); } }
private:
  T* value = nullptr;
};

struct Options {
  std::wstring dll;
  bool invalid = false, reference = false, present = false, wsi = false, wsiReadback = false;
};

Options ParseOptions(int argc, wchar_t** argv) {
  Options options;
  bool seenScenario = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--dll") {
      Require(options.dll.empty() && i + 1 < argc, "duplicate/missing --dll");
      options.dll = argv[++i];
    } else if (arg == L"--scenario") {
      Require(!seenScenario && i + 1 < argc, "duplicate/missing --scenario");
      const std::wstring scenario = argv[++i];
      Require(scenario == L"normal" || scenario == L"invalid", "unknown scenario");
      options.invalid = scenario == L"invalid";
      seenScenario = true;
    } else if (arg == L"--reference") {
      Require(!options.reference, "duplicate --reference"); options.reference = true;
    } else if (arg == L"--present") {
      Require(!options.present, "duplicate --present"); options.present = true;
    } else if (arg == L"--wsi") {
      Require(!options.wsi, "duplicate --wsi"); options.wsi = true;
    } else if (arg == L"--wsi-readback") {
      Require(!options.wsi, "duplicate/conflicting WSI mode");
      options.wsi = true; options.wsiReadback = true;
    } else throw std::runtime_error("usage: --dll ABS --scenario normal|invalid [--reference] [--present] [--wsi|--wsi-readback]");
  }
  const auto& path = options.dll;
  const bool driveAbsolute = path.size() >= 3 && path[1] == L':' &&
    ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
    (path[2] == L'\\' || path[2] == L'/');
  const bool uncAbsolute = path.size() > 3 && path[0] == L'\\' && path[1] == L'\\';
  Require(seenScenario && (driveAbsolute || uncAbsolute), "an explicit absolute DLL and scenario are required");
  const DWORD attributes = GetFileAttributesW(path.c_str());
  Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY), "DLL file missing");
  Require(!options.wsi || (!options.invalid && !options.reference && !options.present),
          "--wsi requires scenario normal and excludes --reference/--present");
  return options;
}

struct WindowOwner {
  HANDLE ready = nullptr, thread = nullptr;
  std::atomic<HWND> window{nullptr};
  std::atomic<uint32_t> messages{0};
  std::atomic<DWORD> failure{0};
  std::atomic<bool> classUnregistered{false};
  std::atomic<bool> desktopVerified{false};
  std::atomic<uint32_t> pulseAck{0};
  DesktopWitness desktop;
  std::atomic<DWORD> ownerThreadId{0};
  uint32_t pulseSequence = 0;
  bool visibleWsi = false;
  bool wasShown = false;
  bool closed = false;

  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<WindowOwner*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WindowPulse && self) {
      self->pulseAck.store(uint32_t(wp), std::memory_order_release); return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) {
      if (self) self->window.store(nullptr, std::memory_order_release);
      PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }

  static unsigned __stdcall Thread(void* parameter) noexcept {
    auto& self = *static_cast<WindowOwner*>(parameter);
    self.ownerThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    if (self.visibleWsi) {
      try {
        self.desktop = NonInputDesktop();
        self.desktopVerified.store(true, std::memory_order_release);
      } catch (...) {
        self.failure.store(ERROR_ACCESS_DENIED, std::memory_order_release);
        SetEvent(self.ready); return 2;
      }
    }
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = Proc; cls.lpszClassName = WindowClass;
    if (!RegisterClassExW(&cls)) {
      self.failure.store(GetLastError(), std::memory_order_release);
      SetEvent(self.ready); return 2;
    }
    // The old offscreen mode is never shown/activated. WSI may be shown only
    // after proving that its actual HWND owner is on a non-input desktop.
    // Neither mode changes desktops, focus or sends global input.
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WindowClass,
      L"WarVK hidden 64x64 D3D9 fixture", WS_POPUP, 0, 0, Width, Height,
      nullptr, nullptr, instance, &self);
    if (!hwnd) {
      self.failure.store(GetLastError(), std::memory_order_release);
      self.classUnregistered.store(UnregisterClassW(WindowClass, instance) != FALSE, std::memory_order_release);
      SetEvent(self.ready); return 2;
    }
    if (self.visibleWsi) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    self.window.store(hwnd, std::memory_order_release);
    SetEvent(self.ready);
    MSG msg{};
    BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
      TranslateMessage(&msg); DispatchMessageW(&msg);
      self.messages.fetch_add(1, std::memory_order_relaxed);
    }
    if (result == -1) self.failure.store(GetLastError(), std::memory_order_release);
    if (self.window.load(std::memory_order_acquire)) DestroyWindow(hwnd);
    self.classUnregistered.store(UnregisterClassW(WindowClass, instance) != FALSE, std::memory_order_release);
    return result == -1 ? 2u : 0u;
  }

  void start(bool wsi = false) {
    visibleWsi = wsi;
    ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Require(ready != nullptr, "window readiness event");
    const uintptr_t handle = _beginthreadex(nullptr, 0, Thread, this, 0, nullptr);
    Require(handle != 0, "window thread creation");
    thread = reinterpret_cast<HANDLE>(handle);
    Require(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "window readiness timeout");
    HWND hwnd = window.load(std::memory_order_acquire);
    RECT client{};
    Require(hwnd && !failure.load(std::memory_order_acquire), "window creation failure");
    Require(GetClientRect(hwnd, &client) && client.right == LONG(Width) && client.bottom == LONG(Height),
            "window client size");
    if (visibleWsi) {
      Require(desktopVerified.load(std::memory_order_acquire), "window desktop was not verified");
      DWORD pid = 0;
      const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
      Require(tid == ownerThreadId.load(std::memory_order_acquire) && tid != GetCurrentThreadId() && pid == GetCurrentProcessId(),
              "WSI HWND/API owner identity mismatch");
      Require(IsWindowVisible(hwnd) != FALSE, "WSI window was not shown on its non-input desktop");
      wasShown = true;
    } else Require(!IsWindowVisible(hwnd), "fixture unexpectedly visible");
  }

  void pulse() {
    Require(visibleWsi, "window pulse is WSI-only");
    const HWND hwnd = window.load(std::memory_order_acquire);
    const uint32_t before = messages.load(std::memory_order_acquire);
    const uint32_t sequence = ++pulseSequence;
    Require(hwnd && PostMessageW(hwnd, WindowPulse, sequence, 0) != FALSE, "window progress pulse post");
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (pulseAck.load(std::memory_order_acquire) != sequence ||
           messages.load(std::memory_order_acquire) <= before) {
      Require(!failure.load(std::memory_order_acquire) && GetTickCount64() < deadline,
              "window message pump did not make progress");
      Sleep(1);
    }
  }

  void stop() {
    HWND hwnd = window.load(std::memory_order_acquire);
    Require(hwnd && (IsWindowVisible(hwnd) != FALSE) == visibleWsi,
            "window missing or unexpected visibility before shutdown");
    Require(PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE, "window close post");
    Require(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "window shutdown timeout");
    DWORD exit = STILL_ACTIVE;
    Require(GetExitCodeThread(thread, &exit) && exit == 0 && !failure.load(std::memory_order_acquire),
            "window thread failure");
    Require(!window.load(std::memory_order_acquire) && classUnregistered.load(std::memory_order_acquire),
            "window/class not closed");
    CloseHandle(thread); thread = nullptr;
    CloseHandle(ready); ready = nullptr;
    closed = true;
  }
};

struct Vertex {
  float position[4];
  float uv[2];
  float color[4];
};
static_assert(sizeof(Vertex) == 40 && offsetof(Vertex, uv) == 16 && offsetof(Vertex, color) == 24,
              "public vertex declaration layout");
static_assert(FrontPatchOffset == StreamOffset + 8 * sizeof(Vertex), "partial update addresses the front quad");

struct PixelStats {
  uint32_t checked = 0, clear = 0, opaque = 0, blend = 0, depthProtected = 0;
  uint32_t failures = 0, maxError = 0;
  uint32_t firstX = 0, firstY = 0, firstActual = 0, firstExpected = 0;
  uint64_t imageHash = FnvOffset;
};

struct WsiFrameStats {
  uint32_t index = 0, clearColor = 0, drawCalls = 0;
  uint32_t windowMessagesBefore = 0, windowMessagesAfter = 0;
  HRESULT presentResult = E_NOTIMPL;
  HRESULT presentedReadbackResult = E_NOTIMPL;
  UINT64 presentedFrame = 0;
  PixelStats before, after, presented;
  bool complete = false;
};

struct WsiStats {
  std::array<WsiFrameStats, WsiFrames> frames{};
  DesktopWitness apiDesktop;
  std::wstring finalInputDesktop;
  DWORD apiThreadId = 0;
  uint32_t desktopChecks = 0, createRejectionChecks = 0, presentRejectionChecks = 0, invalidPresentCalls = 0;
  uint32_t framesStarted = 0, framesCompleted = 0, presentCalls = 0, successfulPresents = 0;
  uint32_t readbackCalls = 0, successfulReadbacks = 0, prePresentReadbacks = 0, postPresentReadbacks = 0;
  uint32_t oracleChecks = 0, oracleFailures = 0, maxChannelError = 0;
  uint32_t presentedImageBeforeFirstPresentAttempts = 0;
  uint32_t presentedImageReadAttempts = 0, presentedImageReads = 0, presentedImageBufferGuardChecks = 0;
  uint32_t presentedImageOracleChecks = 0, presentedImageOracleFailures = 0, presentedImageMaxError = 0;
  uint32_t windowProgressFrames = 0;
  bool profileVerified = false, presentedImagePixelsVerified = false;
  bool presentedImageBeforeFirstPresentRejected = false;
};

// Development-DLL diagnostic only: not a public D3D9 method or a game JAPI.
// The export must return the actual completed Presenter output, not the source
// color target already covered by GetRenderTargetData.
using ReadPresentedImage = HRESULT (WINAPI*)(IDirect3DDevice9*, void*, UINT, UINT64*);

struct Owner {
  Options options;
  const char* stage = "arguments";
  WindowOwner window;
  HMODULE renderer = nullptr, compiler = nullptr;
  ReadPresentedImage readPresentedImage = nullptr;
  Com<IDirect3D9> factory;
  Com<IDirect3DDevice9> device;
  Com<IDirect3DVertexBuffer9> vb;
  Com<IDirect3DIndexBuffer9> ib;
  Com<IDirect3DTexture9> texture;
  Com<IDirect3DVertexDeclaration9> declaration;
  Com<IDirect3DVertexShader9> vs;
  Com<IDirect3DVertexShader9> missingSemanticVs;
  Com<IDirect3DPixelShader9> ps;
  Com<IDirect3DSurface9> back, target, readback;
  Com<IDirect3DQuery9> query;
  D3DCAPS9 caps{};
  PixelStats pixels;
  WsiStats wsi;
  uint32_t comChecks = 0, invalidChecks = 0, queryPolls = 0, queryPending = 0;
  uint32_t profileRestrictionChecks = 0;
  uint32_t backendRejectionChecks = 0, badVertexShaderBytes = 0;
  uint32_t idleRequestedMs = 0;
  uint64_t idleElapsedMs = 0;
  uint32_t processBoundaryChecks = 0;
  uint32_t draws = 0, primitives = 0, vertexShaderBytes = 0, pixelShaderBytes = 0;
  uint32_t fullVertexUploaded = 0, partialVertexUploaded = 0;
  uint32_t fullIndexUploaded = 0, partialIndexUploaded = 0, textureUploaded = 0;
  uint64_t vertexShaderHash = 0, pixelShaderHash = 0;
  bool parentVulkanAbsent = true, presentAttempted = false, presentSupported = false, normalTeardown = false;
  bool shaderBytesRoundtripped = false, queryCompleted = false, readbackVerified = false;
  bool queryValueGuardChecked = false;
  bool resourceHeldDevice = false;
  HRESULT presentResult = E_NOTIMPL;
  ULONG deviceFinalRefs = ~ULONG(0), factoryFinalRefs = ~ULONG(0);
  ULONG deviceRefsWhileResourceHeld = ~ULONG(0);
};

void VerifyWsiDesktop(Owner& owner) {
  if (!owner.options.wsi) return;
  const auto witness = NonInputDesktop();
  if (!owner.wsi.desktopChecks) {
    owner.wsi.apiDesktop = witness;
    owner.wsi.apiThreadId = GetCurrentThreadId();
  } else {
    Require(GetCurrentThreadId() == owner.wsi.apiThreadId &&
        _wcsicmp(witness.thread.c_str(), owner.wsi.apiDesktop.thread.c_str()) == 0,
        "WSI API owner/desktop changed");
  }
  if (owner.window.window.load(std::memory_order_acquire)) {
    Require(owner.window.desktopVerified.load(std::memory_order_acquire) &&
        owner.window.ownerThreadId.load(std::memory_order_acquire) != owner.wsi.apiThreadId,
        "WSI window does not have its own verified owner thread");
    const auto actual = DesktopName(GetThreadDesktop(owner.window.ownerThreadId.load(std::memory_order_acquire)));
    Require(_wcsicmp(actual.c_str(), owner.window.desktop.thread.c_str()) == 0 &&
        _wcsicmp(actual.c_str(), witness.thread.c_str()) == 0 &&
        _wcsicmp(actual.c_str(), witness.input.c_str()) != 0,
        "WSI HWND desktop drifted or became the input desktop");
  }
  owner.wsi.finalInputDesktop = witness.input;
  ++owner.wsi.desktopChecks;
}

void CheckProcessBoundary(Owner& owner) {
  ++owner.processBoundaryChecks;
  Require(!GetModuleHandleW(L"Game.dll") && !GetModuleHandleW(L"War3.exe"), "game module present in isolated fixture");
  const bool absent = GetModuleHandleW(L"vulkan-1.dll") == nullptr;
  owner.parentVulkanAbsent = owner.parentVulkanAbsent && absent;
  Require(owner.options.reference || absent, "remote API caller loaded Vulkan");
}

void SameIdentity(Owner& owner, IUnknown* first, IUnknown* second) {
  Require(first && second, "missing COM identity input");
  Com<IUnknown> a, b, again;
  Check(first->QueryInterface(UnknownIid, reinterpret_cast<void**>(a.put())), "QI first IUnknown");
  Check(second->QueryInterface(UnknownIid, reinterpret_cast<void**>(b.put())), "QI second IUnknown");
  Check(a->QueryInterface(UnknownIid, reinterpret_cast<void**>(again.put())), "QI repeated IUnknown");
  Require(a.get() && a.get() == b.get() && a.get() == again.get(), "canonical IUnknown identity mismatch");
  ++owner.comChecks;
}

template<class T> void CheckDeviceIdentity(Owner& owner, T* resource) {
  Com<IDirect3DDevice9> parent;
  Check(resource->GetDevice(parent.put()), "resource GetDevice");
  SameIdentity(owner, parent.get(), owner.device.get());
}

void CreateDevice(Owner& owner) {
  owner.stage = "window-and-factory";
  CheckProcessBoundary(owner);
  if (owner.options.wsi) {
    wchar_t profile[64]{};
    const DWORD count = GetEnvironmentVariableW(L"WARVK_D3D9_PROFILE", profile, 64);
    const wchar_t* expectedProfile = owner.options.wsiReadback
        ? L"presenter-readback-isolated-v1" : L"presenter-isolated-v1";
    Require(count && count < 64 && std::wcscmp(profile, expectedProfile) == 0,
            owner.options.wsiReadback
                ? "--wsi-readback requires explicit WARVK_D3D9_PROFILE=presenter-readback-isolated-v1"
                : "--wsi requires explicit WARVK_D3D9_PROFILE=presenter-isolated-v1");
    owner.wsi.profileVerified = true;
    VerifyWsiDesktop(owner);
  }
  owner.window.start(owner.options.wsi);
  VerifyWsiDesktop(owner);
  owner.renderer = LoadLibraryW(owner.options.dll.c_str());
  Require(owner.renderer != nullptr, "explicit DLL LoadLibraryW failed");
  if (owner.options.wsiReadback) {
    owner.readPresentedImage = reinterpret_cast<ReadPresentedImage>(
        GetProcAddress(owner.renderer, "WarVKD3D9ReadPresentedImage"));
    Require(owner.readPresentedImage != nullptr, "actual presented-image diagnostic export unavailable");
  }
  using Create9 = IDirect3D9* (WINAPI*)(UINT);
  const auto create9 = reinterpret_cast<Create9>(GetProcAddress(owner.renderer, "Direct3DCreate9"));
  Require(create9 != nullptr, "Direct3DCreate9 export missing");
  *owner.factory.put() = create9(D3D_SDK_VERSION);
  Require(owner.factory.get() && owner.factory->GetAdapterCount() > 0, "D3D9 factory/adapter unavailable");
  SameIdentity(owner, owner.factory.get(), owner.factory.get());
#ifdef WARVK_FACTORY_PRECREATE_TEST
  FactoryPrecreateTest(owner);
#endif

  owner.stage = "create-device";
  D3DPRESENT_PARAMETERS params{};
  params.BackBufferWidth = Width; params.BackBufferHeight = Height;
  params.BackBufferFormat = D3DFMT_A8R8G8B8; params.BackBufferCount = 1;
  params.MultiSampleType = D3DMULTISAMPLE_NONE; params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = owner.window.window.load(std::memory_order_acquire);
  params.Windowed = TRUE; params.EnableAutoDepthStencil = TRUE;
  // Exact DEV profile: float32 depth, no stencil (not D24 precision or D32 UNORM).
  params.AutoDepthStencilFormat = D3DFMT_D32F_LOCKABLE;
  params.PresentationInterval = owner.options.wsi ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
  if (owner.options.wsi) {
    // These are explicit development-profile restrictions, not universal D3D9
    // errors. They must fail before creating a remote device/window owner.
    auto rejectedCreate = [&](D3DPRESENT_PARAMETERS rejected) {
      IDirect3DDevice9* unexpected = reinterpret_cast<IDirect3DDevice9*>(uintptr_t(1));
      const HRESULT hr = owner.factory->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
          rejected.hDeviceWindow, Behavior, &rejected, &unexpected);
      Require(hr == D3DERR_INVALIDCALL && unexpected == nullptr,
              "unsupported WSI CreateDevice published a device or did not reject");
      ++owner.wsi.createRejectionChecks;
    };
    auto rejected = params;
    // Windowed D3D9 accepts only DEFAULT, ONE and IMMEDIATE.
    rejected.PresentationInterval = D3DPRESENT_INTERVAL_TWO;
    rejectedCreate(rejected);
    rejected = params;
    rejected.BackBufferWidth = Width + 1;
    rejectedCreate(rejected);
  }
  Check(owner.factory->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, params.hDeviceWindow,
      Behavior, &params, owner.device.put()), "CreateDevice");
  Require(owner.device.get() != nullptr, "successful CreateDevice returned null");
  Check(owner.device->GetDeviceCaps(&owner.caps), "GetDeviceCaps");
  Require(owner.caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) &&
    owner.caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) &&
    owner.caps.MaxTextureWidth >= 2 && owner.caps.MaxTextureHeight >= 2, "required caps missing");
  SameIdentity(owner, owner.device.get(), owner.device.get());
  Com<IDirect3D9> parent;
  Check(owner.device->GetDirect3D(parent.put()), "GetDirect3D");
  SameIdentity(owner, parent.get(), owner.factory.get());
  Check(owner.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, owner.back.put()), "GetBackBuffer");
  Check(owner.device->GetRenderTarget(0, owner.target.put()), "GetRenderTarget");
  SameIdentity(owner, owner.back.get(), owner.target.get());
  D3DSURFACE_DESC description{};
  Check(owner.target->GetDesc(&description), "render target GetDesc");
  Require(description.Width == Width && description.Height == Height &&
    description.Format == D3DFMT_A8R8G8B8 && description.MultiSampleType == D3DMULTISAMPLE_NONE,
    "render target profile mismatch");
  CheckProcessBoundary(owner);
}

void InvalidBeforeResources(Owner& owner) {
  if (!owner.options.invalid) return;
  owner.stage = "invalid-before-resources";
  Com<IDirect3DVertexBuffer9> invalid;
  const HRESULT hr = owner.device->CreateVertexBuffer(0, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
      0, D3DPOOL_DEFAULT, invalid.put(), nullptr);
  Require(hr == D3DERR_INVALIDCALL && !invalid.get(), "zero-length VB accepted or published");
  ++owner.invalidChecks;
  Require(owner.device->SetRenderTarget(0, nullptr) == D3DERR_INVALIDCALL,
          "null RT0 did not return INVALIDCALL");
  Com<IDirect3DSurface9> after;
  Check(owner.device->GetRenderTarget(0, after.put()), "GetRenderTarget after rejection");
  SameIdentity(owner, owner.target.get(), after.get());
  ++owner.invalidChecks;
  if (!owner.options.reference) {
    Com<IDirect3DStateBlock9> unsupported;
    const HRESULT result = owner.device->CreateStateBlock(D3DSBT_ALL, unsupported.put());
    Require((result == E_NOTIMPL || result == D3DERR_INVALIDCALL) && !unsupported.get(),
            "unsupported stateblock must fail without publishing an object");
    ++owner.invalidChecks;
  }
}

void FillQuad(Vertex* vertices, float extent, float depth, float color) {
  const float xy[4][2] = {{-extent, extent}, {extent, extent}, {extent, -extent}, {-extent, -extent}};
  const float uv[4][2] = {{-.25f, -.25f}, {1.25f, -.25f}, {1.25f, 1.25f}, {-.25f, 1.25f}};
  for (size_t i = 0; i < 4; ++i) {
    vertices[i] = {{xy[i][0], xy[i][1], depth, 1.f}, {uv[i][0], uv[i][1]}, {color, color, color, 1.f}};
  }
}

void CreateGeometry(Owner& owner) {
  owner.stage = "geometry-and-partial-updates";
  Check(owner.device->CreateVertexBuffer(VertexBufferBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
      0, D3DPOOL_DEFAULT, owner.vb.put(), nullptr), "CreateVertexBuffer");
  Check(owner.device->CreateIndexBuffer(12, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
      D3DFMT_INDEX16, D3DPOOL_DEFAULT, owner.ib.put(), nullptr), "CreateIndexBuffer");
  Require(owner.vb.get() && owner.ib.get(), "successful buffer creation returned null");
  CheckDeviceIdentity(owner, owner.vb.get());
  CheckDeviceIdentity(owner, owner.ib.get());
  if (owner.options.invalid) {
    Require(owner.vb->Lock(0, 16, nullptr, 0) == D3DERR_INVALIDCALL, "null buffer Lock output accepted");
    ++owner.invalidChecks;
    if (!owner.options.reference) {
      // This is a restriction of the negotiated remote DEV profile, NOT a
      // general D3D9 invalid call. A partial first write cannot establish the
      // untouched prefix; a rejected lock must not leave the buffer locked.
      void* rejected = nullptr;
      Require(owner.vb->Lock(16, 16, &rejected, 0) == D3DERR_INVALIDCALL && !rejected,
              "remote partial-first Lock accepted or published storage");
      ++owner.invalidChecks; ++owner.profileRestrictionChecks;
    }
  }
  std::array<Vertex, 12> vertices{};
  FillQuad(vertices.data(), .75f, .6f, 1.f);
  FillQuad(vertices.data() + 4, .625f, .8f, .25f);
  FillQuad(vertices.data() + 8, .375f, .2f, 0.f);
  void* mapped = nullptr;
  Check(owner.vb->Lock(0, VertexBufferBytes, &mapped, 0), "VB full Lock");
  Require(mapped != nullptr, "VB full Lock null storage");
  auto* data = static_cast<uint8_t*>(mapped);
  for (UINT i = 0; i < VertexBufferBytes; ++i) data[i] = uint8_t((i * 37u + 19u) & 255u);
  std::memcpy(data + StreamOffset, vertices.data(), sizeof(vertices));
  Check(owner.vb->Unlock(), "VB full Unlock");
  owner.fullVertexUploaded = VertexBufferBytes;
  if (owner.options.invalid && !owner.options.reference) {
    // Deliberate idle between two legal remote uploads. This exceeds the old
    // five-second packet-read window without leaving a partial packet in flight.
    // It is a connection liveness test, NOT a performance/tail-latency benchmark.
    owner.stage = "remote-clean-packet-idle";
    owner.idleRequestedMs = 6500;
    const ULONGLONG begin = GetTickCount64();
    Sleep(owner.idleRequestedMs);
    while (GetTickCount64() - begin < owner.idleRequestedMs) Sleep(1);
    owner.idleElapsedMs = GetTickCount64() - begin;
    Require(owner.idleElapsedMs >= owner.idleRequestedMs, "idle interval shorter than requested");
    owner.stage = "geometry-and-partial-updates-after-idle";
  }
  for (size_t i = 8; i < vertices.size(); ++i) vertices[i].color[3] = .5f;
  Check(owner.vb->Lock(FrontPatchOffset, FrontPatchBytes, &mapped, 0), "VB partial Lock");
  Require(mapped != nullptr, "VB partial Lock null storage");
  std::memcpy(mapped, vertices.data() + 8, FrontPatchBytes);
  Check(owner.vb->Unlock(), "VB partial Unlock");
  owner.partialVertexUploaded = FrontPatchBytes;

  const uint16_t initialIndices[] = {0, 1, 2, 0, 2, 0};
  Check(owner.ib->Lock(0, sizeof(initialIndices), &mapped, 0), "IB full Lock");
  Require(mapped != nullptr, "IB full Lock null storage");
  std::memcpy(mapped, initialIndices, sizeof(initialIndices));
  Check(owner.ib->Unlock(), "IB full Unlock");
  owner.fullIndexUploaded = sizeof(initialIndices);
  Check(owner.ib->Lock(10, 2, &mapped, 0), "IB two-byte partial Lock");
  Require(mapped != nullptr, "IB partial Lock null storage");
  const uint16_t lastIndex = 3;
  std::memcpy(mapped, &lastIndex, sizeof(lastIndex));
  Check(owner.ib->Unlock(), "IB two-byte partial Unlock");
  owner.partialIndexUploaded = sizeof(lastIndex);
  Check(owner.device->SetStreamSource(0, owner.vb.get(), StreamOffset, sizeof(Vertex)), "SetStreamSource");
  Check(owner.device->SetIndices(owner.ib.get()), "SetIndices");
}

constexpr char VertexSource[] =
  "struct In { float4 position:POSITION0; float2 uv:TEXCOORD0; float4 color:COLOR0; };\n"
  "struct Out { float4 position:POSITION0; float2 uv:TEXCOORD0; float4 color:COLOR0; };\n"
  "Out main(In i) { Out o; o.position=i.position; o.uv=i.uv; o.color=i.color; return o; }\n";
constexpr char PixelSource[] =
  "sampler2D texture0 : register(s0);\n"
  "float4 main(float2 uv:TEXCOORD0, float4 color:COLOR0):COLOR0 {\n"
  "  return tex2D(texture0, uv) * color;\n"
  "}\n";
// This is legal original HLSL3, not a malformed token stream. Only the VS input
// TEXCOORD semantic differs from the fixed public declaration (TEXCOORD0).
// The missing TEXCOORD1 must be rejected by real backend draw preflight, with
// the healthy connection and previously uploaded resources left usable.
constexpr char MissingSemanticVertexSource[] =
  "struct In { float4 position:POSITION0; float2 uv:TEXCOORD1; float4 color:COLOR0; };\n"
  "struct Out { float4 position:POSITION0; float2 uv:TEXCOORD0; float4 color:COLOR0; };\n"
  "Out main(In i) { Out o; o.position=i.position; o.uv=i.uv; o.color=i.color; return o; }\n";

template<class T> void CheckShaderBytes(T* shader, ID3DBlob* original) {
  UINT bytes = 0;
  Check(shader->GetFunction(nullptr, &bytes), "shader GetFunction size");
  Require(bytes == original->GetBufferSize(), "shader function size differs from D3DCompile bytes");
  std::vector<uint8_t> copy(bytes);
  Check(shader->GetFunction(copy.data(), &bytes), "shader GetFunction bytes");
  Require(bytes == copy.size() && !std::memcmp(copy.data(), original->GetBufferPointer(), bytes),
          "shader function bytes differ from original bytecode");
}

void CreateShadersAndTexture(Owner& owner) {
  owner.stage = "original-sm3-shaders";
  owner.compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  Require(owner.compiler != nullptr, "system d3dcompiler_47.dll unavailable");
  const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(owner.compiler, "D3DCompile"));
  Require(compile != nullptr, "system D3DCompile export missing");
  Com<ID3DBlob> vsBytes, psBytes, errors;
  auto compileOne = [&](const char* source, size_t size, const char* profile, Com<ID3DBlob>& output) {
    const HRESULT hr = compile(source, size, "independent-api-fixture.hlsl", nullptr, nullptr,
      // sampler2D/tex2D is original HLSL3 syntax. STRICTNESS rejects it as
      // deprecated (X3086); the test intentionally compiles legacy SM3 bytes.
      "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
      output.put(), errors.put());
    if (FAILED(hr) && errors.get()) {
      const size_t limit = errors->GetBufferSize() < 1200 ? errors->GetBufferSize() : 1200;
      throw std::runtime_error("D3DCompile: " + std::string(static_cast<const char*>(errors->GetBufferPointer()), limit));
    }
    Check(hr, "D3DCompile");
    Require(output.get() && output->GetBufferSize() >= 8 && output->GetBufferSize() % 4 == 0,
            "D3DCompile returned invalid bytecode size");
  };
  compileOne(VertexSource, sizeof(VertexSource) - 1, "vs_3_0", vsBytes);
  compileOne(PixelSource, sizeof(PixelSource) - 1, "ps_3_0", psBytes);
  owner.vertexShaderBytes = UINT(vsBytes->GetBufferSize());
  owner.pixelShaderBytes = UINT(psBytes->GetBufferSize());
  owner.vertexShaderHash = Hash(vsBytes->GetBufferPointer(), vsBytes->GetBufferSize());
  owner.pixelShaderHash = Hash(psBytes->GetBufferPointer(), psBytes->GetBufferSize());
  Check(owner.device->CreateVertexShader(static_cast<const DWORD*>(vsBytes->GetBufferPointer()), owner.vs.put()),
        "CreateVertexShader original SM3");
  Check(owner.device->CreatePixelShader(static_cast<const DWORD*>(psBytes->GetBufferPointer()), owner.ps.put()),
        "CreatePixelShader original SM3");
  Require(owner.vs.get() && owner.ps.get(), "successful shader creation returned null");
  CheckShaderBytes(owner.vs.get(), vsBytes.get());
  CheckShaderBytes(owner.ps.get(), psBytes.get());
  owner.shaderBytesRoundtripped = true;
  if (owner.options.invalid && !owner.options.reference) {
    Com<ID3DBlob> badBytes;
    compileOne(MissingSemanticVertexSource, sizeof(MissingSemanticVertexSource) - 1, "vs_3_0", badBytes);
    Check(owner.device->CreateVertexShader(static_cast<const DWORD*>(badBytes->GetBufferPointer()),
        owner.missingSemanticVs.put()), "CreateVertexShader legal missing-semantic SM3");
    Require(owner.missingSemanticVs.get() != nullptr, "missing-semantic shader creation returned null");
    CheckShaderBytes(owner.missingSemanticVs.get(), badBytes.get());
    owner.badVertexShaderBytes = UINT(badBytes->GetBufferSize());
  }
  // Destroy compiler-owned byte arrays before draw; a shader must own its input.
  vsBytes.reset(); psBytes.reset(); errors.reset();
  const D3DVERTEXELEMENT9 elements[] = {
    {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    {0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    D3DDECL_END()
  };
  Check(owner.device->CreateVertexDeclaration(elements, owner.declaration.put()), "CreateVertexDeclaration");
  Require(owner.declaration.get() != nullptr, "successful declaration creation returned null");
  Check(owner.device->SetVertexDeclaration(owner.declaration.get()), "SetVertexDeclaration");
  Check(owner.device->SetVertexShader(owner.vs.get()), "SetVertexShader");
  Check(owner.device->SetPixelShader(owner.ps.get()), "SetPixelShader");

  owner.stage = "texture-upload";
  Check(owner.device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
      owner.texture.put(), nullptr), "CreateTexture");
  Require(owner.texture.get() != nullptr, "successful texture creation returned null");
  CheckDeviceIdentity(owner, owner.texture.get());
  D3DLOCKED_RECT locked{};
  Check(owner.texture->LockRect(0, &locked, nullptr, 0), "texture LockRect");
  Require(locked.pBits && locked.Pitch >= 8, "texture pitch/storage");
  constexpr uint8_t texels[2][8] = {
    {0, 0, 255, 255, 0, 255, 0, 255},
    {255, 0, 0, 255, 0, 255, 255, 255}
  };
  for (size_t row = 0; row < 2; ++row)
    std::memcpy(static_cast<uint8_t*>(locked.pBits) + row * size_t(locked.Pitch), texels[row], 8);
  Check(owner.texture->UnlockRect(0), "texture UnlockRect");
  owner.textureUploaded = 16;
  Check(owner.device->SetTexture(0, owner.texture.get()), "SetTexture");
  Check(owner.device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT), "sampler min point");
  Check(owner.device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT), "sampler mag point");
  Check(owner.device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE), "sampler mip none");
  Check(owner.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP), "sampler U clamp");
  Check(owner.device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP), "sampler V clamp");
}

void DrawScene(Owner& owner, DWORD clearColor = 0xff101820u) {
  owner.stage = "indexed-depth-and-blend-draws";
  const D3DVIEWPORT9 viewport{0, 0, Width, Height, 0.f, 1.f};
  Check(owner.device->SetViewport(&viewport), "SetViewport");
  auto state = [&](D3DRENDERSTATETYPE name, DWORD value) {
    Check(owner.device->SetRenderState(name, value), "SetRenderState");
  };
  state(D3DRS_ZENABLE, TRUE); state(D3DRS_ZWRITEENABLE, TRUE);
  state(D3DRS_ZFUNC, D3DCMP_LESSEQUAL); state(D3DRS_CULLMODE, D3DCULL_NONE);
  state(D3DRS_ALPHABLENDENABLE, FALSE);
  state(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); state(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  state(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  Com<IDirect3DTexture9> unuploaded;
  if (owner.options.invalid && !owner.options.reference) {
    Check(owner.device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        unuploaded.put(), nullptr), "CreateTexture deliberately unuploaded");
    Require(unuploaded.get() != nullptr, "temporary texture creation returned null");
  }
  Check(owner.device->BeginScene(), "BeginScene");
  Check(owner.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.f, 0), "Clear");
  if (unuploaded.get()) {
    Check(owner.device->SetTexture(0, unuploaded.get()), "bind unuploaded remote texture");
    // Drawing uninitialized texture bytes is rejected by this narrow remote
    // profile. Do not demand this restriction of the normal DXVK reference.
    Require(owner.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2) == D3DERR_INVALIDCALL,
            "remote draw with unuploaded texture accepted");
    ++owner.invalidChecks; ++owner.profileRestrictionChecks;
    Check(owner.device->SetTexture(0, owner.texture.get()), "restore uploaded texture after rejection");
  }
  if (owner.missingSemanticVs.get()) {
    owner.stage = "recoverable-backend-declaration-rejection";
    Check(owner.device->SetVertexShader(owner.missingSemanticVs.get()), "bind legal missing-semantic VS");
    Require(owner.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2) == D3DERR_INVALIDCALL,
            "backend accepted a VS input missing from the declaration");
    ++owner.invalidChecks;
    Check(owner.device->TestCooperativeLevel(), "cooperative level after backend preflight rejection");
    Check(owner.device->SetVertexShader(owner.vs.get()), "restore original VS after backend rejection");
    ++owner.backendRejectionChecks;
    owner.stage = "indexed-depth-and-blend-draws-after-backend-rejection";
  }
  for (INT quad = 0; quad < 3; ++quad) {
    if (quad == 2) state(D3DRS_ALPHABLENDENABLE, TRUE);
    Check(owner.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, quad * 4, 0, 4, 0, 2),
          "DrawIndexedPrimitive");
    ++owner.draws; owner.primitives += 2;
  }
  Check(owner.device->EndScene(), "EndScene");
  // It overlapped the unuploaded texture above, so its original token byte
  // length contributes to the remote-invalid peak logical resource population.
  owner.missingSemanticVs.reset();
  CheckProcessBoundary(owner);
}

void WaitForEvent(Owner& owner) {
  owner.stage = "event-query";
  Check(owner.device->CreateQuery(D3DQUERYTYPE_EVENT, owner.query.put()), "CreateQuery EVENT");
  Require(owner.query.get() && owner.query->GetType() == D3DQUERYTYPE_EVENT &&
    owner.query->GetDataSize() == sizeof(BOOL), "EVENT query profile");
  Check(owner.query->Issue(D3DISSUE_END), "EVENT Issue END");
  const ULONGLONG deadline = GetTickCount64() + 10000;
  for (;;) {
    BOOL completed = FALSE;
    const HRESULT hr = owner.query->GetData(&completed, sizeof(completed),
      owner.queryPolls ? D3DGETDATA_FLUSH : 0);
    ++owner.queryPolls;
    if (hr == S_OK) {
      Require(completed == TRUE, "EVENT S_OK without completion");
      owner.queryCompleted = true; break;
    }
    Require(hr == S_FALSE, "EVENT poll failed");
    ++owner.queryPending;
    Require(GetTickCount64() < deadline, "EVENT query timed out");
    Sleep(1); // Only the isolated caller sleeps; the HWND owner still pumps.
  }
  // Immediate S_OK is legal for a fast/synchronous backend. A pending count is
  // reported, never invented; backend retirement evidence is a separate gate.
  if (owner.options.invalid && !owner.options.reference) {
    struct QueryValue {
      DWORD before = 0x18395a7bu;
      BOOL complete = static_cast<BOOL>(0xa5a5a5a5u);
      DWORD after = 0xc2e40628u;
    } value;
    Check(owner.query->GetData(&value.complete, sizeof(value.complete), 0), "completed EVENT full BOOL write");
    ++owner.queryPolls;
    Require(value.complete == TRUE && value.before == 0x18395a7bu && value.after == 0xc2e40628u,
            "completed EVENT must write one full BOOL without touching guards");
    owner.queryValueGuardChecked = true;
  }
}

PixelStats CheckPixels(const D3DLOCKED_RECT& locked, DWORD clearColor = 0xff101820u) {
  Require(locked.pBits && locked.Pitch >= int(Width * 4), "readback pitch/storage");
  PixelStats stats;
  for (UINT y = 0; y < Height; ++y) {
    const auto* row = static_cast<const uint8_t*>(locked.pBits) + size_t(y) * size_t(locked.Pitch);
    stats.imageHash = Hash(row, Width * 4, stats.imageHash);
    for (UINT x = 0; x < Width; ++x) {
      std::array<uint8_t, 4> expected{}; // BGRA, independently derived from scene.
      UINT tolerance = 0;
      const bool clear = x <= 5 || x >= 58 || y <= 5 || y >= 58;
      const bool base = x >= 10 && x <= 53 && y >= 10 && y <= 53;
      const bool quadrantSafe = (x <= 28 || x >= 35) && (y <= 28 || y >= 35);
      const bool front = x >= 22 && x <= 41 && y >= 22 && y <= 41;
      const bool opaque = x <= 17 || x >= 46 || y <= 17 || y >= 46;
      if (clear) {
        expected = {uint8_t(clearColor), uint8_t(clearColor >> 8),
                    uint8_t(clearColor >> 16), uint8_t(clearColor >> 24)};
        ++stats.clear;
      } else if (base && quadrantSafe && (front || opaque)) {
        const bool right = x >= 35, bottom = y >= 35;
        if (!bottom) expected = right ? std::array<uint8_t, 4>{0, 255, 0, 255}
                                    : std::array<uint8_t, 4>{0, 0, 255, 255};
        else expected = right ? std::array<uint8_t, 4>{0, 255, 255, 255}
                              : std::array<uint8_t, 4>{255, 0, 0, 255};
        if (front) {
          for (size_t channel = 0; channel < 3; ++channel)
            expected[channel] = expected[channel] ? 128 : 0;
          expected[3] = 191; tolerance = 1; ++stats.blend;
        } else {
          ++stats.opaque;
          if (x >= 14 && x <= 49 && y >= 14 && y <= 49) ++stats.depthProtected;
        }
      } else continue; // Skip edge/texture-boundary strips, not scene errors.
      ++stats.checked;
      bool bad = false;
      for (size_t channel = 0; channel < 4; ++channel) {
        const int delta = int(row[x * 4 + channel]) - int(expected[channel]);
        const UINT error = UINT(delta < 0 ? -delta : delta);
        if (error > stats.maxError) stats.maxError = error;
        bad = bad || error > tolerance;
      }
      if (bad) {
        if (!stats.failures) {
          stats.firstX = x; stats.firstY = y;
          std::memcpy(&stats.firstActual, row + x * 4, 4);
          std::memcpy(&stats.firstExpected, expected.data(), 4);
        }
        ++stats.failures;
      }
    }
  }
  Require(stats.clear > 1000 && stats.opaque > 500 && stats.blend > 100 && stats.depthProtected > 100,
          "independent oracle has insufficient coverage");
  return stats;
}

void ReadAndVerify(Owner& owner, DWORD clearColor = 0xff101820u, PixelStats* frameStats = nullptr) {
  owner.stage = "real-render-target-readback";
  Check(owner.device->CreateOffscreenPlainSurface(Width, Height, D3DFMT_A8R8G8B8,
      D3DPOOL_SYSTEMMEM, owner.readback.put(), nullptr), "CreateOffscreenPlainSurface SYSTEMMEM");
  Require(owner.readback.get() != nullptr, "successful readback surface creation returned null");
  if (owner.options.wsi) ++owner.wsi.readbackCalls;
  Check(owner.device->GetRenderTargetData(owner.target.get(), owner.readback.get()), "GetRenderTargetData");
  if (owner.options.wsi) ++owner.wsi.successfulReadbacks;
  D3DLOCKED_RECT locked{};
  Check(owner.readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "SYSTEMMEM LockRect");
  owner.pixels = CheckPixels(locked, clearColor);
  if (frameStats) *frameStats = owner.pixels;
  if (owner.options.wsi) {
    owner.wsi.oracleChecks += owner.pixels.checked;
    owner.wsi.oracleFailures += owner.pixels.failures;
    if (owner.pixels.maxError > owner.wsi.maxChannelError) owner.wsi.maxChannelError = owner.pixels.maxError;
  }
  Check(owner.readback->UnlockRect(), "SYSTEMMEM UnlockRect");
  Require(owner.pixels.failures == 0, "independent clear/coverage/texture/depth/blend pixel oracle failed");
  owner.readbackVerified = true;
  CheckProcessBoundary(owner);
}

void RejectPresentedReadBeforeFirstPresent(Owner& owner) {
  Require(owner.options.wsiReadback && owner.readPresentedImage,
          "presented-image readback was not explicitly enabled");
  owner.stage = "presenter-readback-before-first-present";
  constexpr UINT imageBytes = Width * Height * 4;
  constexpr size_t guardBytes = 32;
  std::array<uint8_t, imageBytes + guardBytes * 2> guarded{};
  guarded.fill(0xa5);
  constexpr UINT64 frameSentinel = ~UINT64(0);
  UINT64 actualFrame = frameSentinel;
  ++owner.wsi.presentedImageBeforeFirstPresentAttempts;
  const HRESULT hr = owner.readPresentedImage(owner.device.get(),
      guarded.data() + guardBytes, imageBytes, &actualFrame);
  Require(hr == D3DERR_INVALIDCALL, "presented-image readback succeeded before any Present");
  Require(actualFrame == frameSentinel, "rejected pre-Present readback changed frame output");
  for (const uint8_t byte : guarded)
    Require(byte == 0xa5, "rejected pre-Present readback changed image output or guards");
  owner.wsi.presentedImageBeforeFirstPresentRejected = true;
}

void ReadPresentedAndVerify(Owner& owner, WsiFrameStats& frame) {
  Require(owner.options.wsiReadback && owner.readPresentedImage,
          "presented-image readback was not explicitly enabled");
  owner.stage = "actual-presenter-output-readback";
  constexpr UINT imageBytes = Width * Height * 4;
  constexpr size_t guardBytes = 32;
  std::array<uint8_t, imageBytes + guardBytes * 2> guarded{};
  guarded.fill(0xa5);
  UINT64 actualFrame = ~UINT64(0);
  ++owner.wsi.presentedImageReadAttempts;
  frame.presentedReadbackResult = owner.readPresentedImage(owner.device.get(),
      guarded.data() + guardBytes, imageBytes, &actualFrame);
  frame.presentedFrame = actualFrame;
  Check(frame.presentedReadbackResult, "WarVKD3D9ReadPresentedImage");
  ++owner.wsi.presentedImageReads;
  Require(actualFrame == UINT64(frame.index) + 1, "presented-image frame is stale or out of sequence");
  for (size_t i = 0; i < guardBytes; ++i) {
    Require(guarded[i] == 0xa5 && guarded[guardBytes + imageBytes + i] == 0xa5,
            "presented-image readback wrote outside its declared output span");
  }
  ++owner.wsi.presentedImageBufferGuardChecks;
  const D3DLOCKED_RECT locked{int(Width * 4), guarded.data() + guardBytes};
  // The production no-HUD/no-cursor blitter preserves alpha: present_common
  // converts color.rgb, and composite_image returns the original value here.
  // OPAQUE WSI composition is not permission to replace the stored alpha with
  // one. Keep the same four-channel oracle, including blend alpha 191 +/- 1.
  frame.presented = CheckPixels(locked, frame.clearColor);
  owner.wsi.presentedImageOracleChecks += frame.presented.checked;
  owner.wsi.presentedImageOracleFailures += frame.presented.failures;
  if (frame.presented.maxError > owner.wsi.presentedImageMaxError)
    owner.wsi.presentedImageMaxError = frame.presented.maxError;
  Require(frame.presented.failures == 0,
          "actual Presenter output failed independent clear/coverage/texture/depth/blend pixel oracle");
  CheckProcessBoundary(owner);
}

void StrictWsiFrames(Owner& owner) {
  Require(owner.options.wsi, "strict WSI mode not selected");
  VerifyWsiDesktop(owner);
  if (owner.options.wsiReadback) RejectPresentedReadBeforeFirstPresent(owner);
  owner.stage = "wsi-invalid-present-parameters";
  const RECT rect{0, 0, LONG(Width), LONG(Height)};
  const HWND hwnd = owner.window.window.load(std::memory_order_acquire);
  auto rejectedPresent = [&](const RECT* src, const RECT* dst, HWND overrideWindow) {
    ++owner.wsi.invalidPresentCalls;
    const HRESULT hr = owner.device->Present(src, dst, overrideWindow, nullptr);
    Require(hr == D3DERR_INVALIDCALL, "unsupported WSI Present parameters did not return INVALIDCALL");
    ++owner.wsi.presentRejectionChecks;
  };
  rejectedPresent(&rect, nullptr, nullptr);
  rejectedPresent(nullptr, &rect, nullptr);
  rejectedPresent(nullptr, nullptr, hwnd);
  Check(owner.device->TestCooperativeLevel(), "WSI cooperative level after parameter rejection");

  constexpr DWORD clearColors[] = {0xff101820u, 0xff203040u};
  for (UINT i = 0; i < WsiFrames; ++i) {
    auto& frame = owner.wsi.frames[i];
    frame.index = i;
    // The readback profile must distinguish every frame, not just parity: a
    // two-frame-old swapchain image could pass an alternating-color oracle.
    frame.clearColor = owner.options.wsiReadback ? 0xff101820u + i * 0x00020304u : clearColors[i % 2];
    ++owner.wsi.framesStarted;
    VerifyWsiDesktop(owner);
    frame.windowMessagesBefore = owner.window.messages.load(std::memory_order_acquire);
    owner.window.pulse();
    const uint32_t drawsBefore = owner.draws;
    DrawScene(owner, frame.clearColor);
    frame.drawCalls = owner.draws - drawsBefore;
    Require(frame.drawCalls == 3, "WSI frame did not issue its three real draws");
    WaitForEvent(owner);
    ReadAndVerify(owner, frame.clearColor, &frame.before);
    ++owner.wsi.prePresentReadbacks;
    owner.stage = "strict-non-input-desktop-present";
    ++owner.wsi.presentCalls;
    frame.presentResult = owner.device->Present(nullptr, nullptr, nullptr, nullptr);
    // Unlike MaybePresent, E_NOTIMPL/INVALIDCALL are failures. The same D3D9
    // color target is checked after the real Presenter consumed it; this is
    // explicitly not a screenshot or proof of the pixels visible in the HWND.
    Check(frame.presentResult, "strict WSI Present");
    ++owner.wsi.successfulPresents;
    ReadAndVerify(owner, frame.clearColor, &frame.after);
    ++owner.wsi.postPresentReadbacks;
    Require(frame.before.checked == frame.after.checked &&
        frame.before.imageHash == frame.after.imageHash,
        "WSI Present changed its source color target");
    if (owner.options.wsiReadback) ReadPresentedAndVerify(owner, frame);
    owner.window.pulse();
    frame.windowMessagesAfter = owner.window.messages.load(std::memory_order_acquire);
    Require(frame.windowMessagesAfter > frame.windowMessagesBefore, "WSI window message progress stopped");
    ++owner.wsi.windowProgressFrames;
    VerifyWsiDesktop(owner);
    frame.complete = true;
    ++owner.wsi.framesCompleted;
  }
  Require(owner.wsi.framesCompleted == WsiFrames && owner.draws == WsiFrames * 3 &&
      owner.wsi.successfulPresents == WsiFrames && owner.wsi.successfulReadbacks == WsiFrames * 2 &&
      owner.wsi.oracleChecks == WsiFrames * 2 * owner.wsi.frames[0].before.checked &&
      owner.wsi.oracleFailures == 0, "incomplete WSI multi-frame acceptance");
  // Analytically checked clear pixels must actually change the images. A hash
  // alone is never the color/geometry correctness gate.
  Require(owner.wsi.frames[0].before.imageHash != owner.wsi.frames[1].before.imageHash,
          "WSI scene changes were not reflected in the real color target");
  if (owner.options.wsiReadback) {
    Require(owner.wsi.presentedImageBeforeFirstPresentAttempts == 1 &&
        owner.wsi.presentedImageBeforeFirstPresentRejected &&
        owner.wsi.presentedImageReadAttempts == WsiFrames && owner.wsi.presentedImageReads == WsiFrames &&
        owner.wsi.presentedImageBufferGuardChecks == WsiFrames &&
        owner.wsi.presentedImageOracleChecks == WsiFrames * owner.wsi.frames[0].before.checked &&
        owner.wsi.presentedImageOracleFailures == 0 &&
        owner.wsi.frames[0].presented.imageHash != owner.wsi.frames[1].presented.imageHash,
        "incomplete actual Presenter output acceptance");
    for (UINT i = 0; i < WsiFrames; ++i) {
      for (UINT j = 0; j < i; ++j) {
        Require(owner.wsi.frames[i].before.imageHash != owner.wsi.frames[j].before.imageHash &&
            owner.wsi.frames[i].presented.imageHash != owner.wsi.frames[j].presented.imageHash,
            "distinct readback frame colors did not produce distinct source and Presenter images");
      }
    }
    owner.wsi.presentedImagePixelsVerified = true;
  }
  Check(owner.device->TestCooperativeLevel(), "WSI cooperative level after eight frames");
}

void MaybePresent(Owner& owner) {
  if (!owner.options.present) return;
  owner.stage = "optional-hidden-present";
  owner.presentAttempted = true;
  owner.presentResult = owner.device->Present(nullptr, nullptr, nullptr, nullptr);
  owner.presentSupported = owner.presentResult == D3D_OK;
  Require(owner.presentSupported || owner.presentResult == E_NOTIMPL || owner.presentResult == D3DERR_INVALIDCALL,
          "Present failed with an unexpected status");
  // An explicit unsupported return remains unsupported, never WSI/visual proof.
}

void Teardown(Owner& owner) {
  owner.stage = "bound-resource-com-teardown";
  CheckProcessBoundary(owner);
  VerifyWsiDesktop(owner);
  // Intentionally do NOT unbind stream/index/texture/shader/declaration first.
  // The device's internal bindings must not create a resource<->device COM cycle.
  owner.query.reset(); owner.readback.reset(); owner.back.reset(); owner.target.reset();
  owner.vb.reset(); owner.ib.reset();
  owner.vs.reset(); owner.ps.reset(); owner.missingSemanticVs.reset(); owner.declaration.reset();
  // Hold exactly the texture's external resource reference while dropping the
  // caller's device reference. The resource must keep its parent alive; recover
  // a device ref through the public API, then verify the same canonical object.
  // No temporary IUnknown ref is retained across the device Release.
  IDirect3DDevice9* original = owner.device.detach();
  owner.deviceRefsWhileResourceHeld = original->Release();
  Require(owner.deviceRefsWhileResourceHeld > 0, "resource failed to retain its device");
  Check(owner.texture->GetDevice(owner.device.put()), "resource-held device GetDevice");
  SameIdentity(owner, original, owner.device.get());
  owner.resourceHeldDevice = true;
  owner.texture.reset();
  owner.deviceFinalRefs = owner.device.detach()->Release();
  Require(owner.deviceFinalRefs == 0, "final device Release retained a bound-resource cycle");
  owner.factoryFinalRefs = owner.factory.detach()->Release();
  Require(owner.factoryFinalRefs == 0, "final factory Release retained a device");
  Require(FreeLibrary(owner.renderer) != FALSE, "renderer FreeLibrary failed"); owner.renderer = nullptr;
  Require(FreeLibrary(owner.compiler) != FALSE, "compiler FreeLibrary failed"); owner.compiler = nullptr;
  owner.window.stop();
  CheckProcessBoundary(owner);
  owner.normalTeardown = true;
  owner.stage = "complete";
}

void PrintPixelOracle(const PixelStats& p) {
  std::printf("{\"checked\":%u,\"clear\":%u,\"opaque\":%u,\"blend\":%u,\"depthProtected\":%u,"
    "\"failures\":%u,\"maxChannelError\":%u,\"imageFnv\":\"%016llx\","
    "\"firstFailure\":{\"x\":%u,\"y\":%u,\"actualBGRA\":%u,\"expectedBGRA\":%u}}",
    p.checked, p.clear, p.opaque, p.blend, p.depthProtected, p.failures, p.maxError,
    static_cast<unsigned long long>(p.imageHash), p.firstX, p.firstY, p.firstActual, p.firstExpected);
}

void PrintWsiResult(const Owner& owner, bool ok, const char* error) {
  const auto& s = owner.wsi;
  const bool windowDesktopVerified = owner.window.desktopVerified.load(std::memory_order_acquire);
  const bool desktopVerified = s.desktopChecks >= 18 && windowDesktopVerified;
  // A setup failure may leave the window thread in its desktop query. Never
  // read its strings until the release/acquire publication is complete.
  const DesktopWitness emptyWitness{};
  const auto& windowDesktop = windowDesktopVerified ? owner.window.desktop : emptyWitness;
  uint32_t observedVariants = 0;
  for (uint32_t i = 0; i < s.framesStarted; ++i) {
    if (!s.frames[i].before.checked || s.frames[i].before.failures) continue;
    bool alreadyObserved = false;
    for (uint32_t j = 0; j < i; ++j) {
      if (s.frames[j].before.checked && !s.frames[j].before.failures &&
          s.frames[j].clearColor == s.frames[i].clearColor) alreadyObserved = true;
    }
    if (!alreadyObserved) ++observedVariants;
  }
  std::printf("{\"schema\":\"warvk-d3d9-wsi-fixture/v1\",\"ok\":%s,\"bits\":32,"
    "\"scenario\":\"normal\",\"reference\":false,\"stage\":%s,\"error\":%s,"
    "\"publicD3D9Api\":true,\"parentVulkanAbsent\":%s,\"processBoundaryChecks\":%u,"
    "\"gameTouched\":false,\"globalInputUsed\":false,\"desktopSwitched\":false,"
    "\"productIntegrated\":false,\"memorySavingsProven\":false,"
    "\"profile\":\"%s\",\"profileVerified\":%s,"
    "\"width\":64,\"height\":64,\"presentationInterval\":1,"
    "\"apiThreadId\":%lu,\"windowThreadId\":%lu,"
    "\"apiDesktopName\":%s,\"windowDesktopName\":%s,"
    "\"inputDesktopNameAtStart\":%s,\"inputDesktopNameAtEnd\":%s,\"windowInitialInputDesktopName\":%s,"
    "\"nonInputDesktopVerified\":%s,\"desktopChecks\":%u,\"windowShownOnNonInputDesktop\":%s,"
    "\"windowPixelReadbackAttempted\":false,\"windowPixelsVerified\":false,\"playerVisualAccepted\":false,"
    "\"oracleScope\":\"D3D9-color-target-before-and-after-Present-not-HWND-pixels\","
    "\"createRejectionChecks\":%u,\"presentRejectionChecks\":%u,\"invalidPresentCalls\":%u,"
    "\"framesExpected\":8,\"framesStarted\":%u,\"framesCompleted\":%u,\"clearColorVariantsExpected\":%u,\"clearColorVariants\":%u,"
    "\"drawCalls\":%u,\"primitiveCount\":%u,"
    "\"presentApiCallsTotal\":%u,\"presentCalls\":%u,\"successfulPresents\":%u,"
    "\"readbackCalls\":%u,\"successfulReadbacks\":%u,\"prePresentReadbacks\":%u,\"postPresentReadbacks\":%u,"
    "\"pixelOracle\":{\"checked\":%u,\"failures\":%u,\"maxChannelError\":%u},"
    "\"queryPolls\":%u,\"queryPending\":%u,\"queryCompleted\":%s,\"readbackVerified\":%s,"
    "\"windowProgressFrames\":%u,\"windowPulseCount\":%u,\"windowMessages\":%u,\"windowThreadFailureCode\":%lu,"
    "\"comIdentityChecks\":%u,\"shaderBytesRoundtripped\":%s,"
    "\"resourceHeldDevice\":%s,\"deviceRefsWhileResourceHeld\":%lu,"
    "\"deviceFinalRefs\":%lu,\"factoryFinalRefs\":%lu,\"windowClosed\":%s,\"normalTeardown\":%s",
    JsonBool(ok), JsonString(owner.stage).c_str(), JsonString(error).c_str(),
    JsonBool(owner.parentVulkanAbsent), owner.processBoundaryChecks,
    owner.options.wsiReadback ? "presenter-readback-isolated-v1" : "presenter-isolated-v1", JsonBool(s.profileVerified),
    static_cast<unsigned long>(s.apiThreadId), static_cast<unsigned long>(owner.window.ownerThreadId.load(std::memory_order_acquire)),
    JsonString(Utf8(s.apiDesktop.thread).c_str()).c_str(), JsonString(Utf8(windowDesktop.thread).c_str()).c_str(),
    JsonString(Utf8(s.apiDesktop.input).c_str()).c_str(), JsonString(Utf8(s.finalInputDesktop).c_str()).c_str(),
    JsonString(Utf8(windowDesktop.input).c_str()).c_str(),
    JsonBool(desktopVerified), s.desktopChecks, JsonBool(owner.window.wasShown),
    s.createRejectionChecks, s.presentRejectionChecks, s.invalidPresentCalls, s.framesStarted, s.framesCompleted,
    owner.options.wsiReadback ? WsiFrames : 2u, observedVariants,
    owner.draws, owner.primitives, s.presentCalls + s.invalidPresentCalls, s.presentCalls, s.successfulPresents,
    s.readbackCalls, s.successfulReadbacks, s.prePresentReadbacks, s.postPresentReadbacks,
    s.oracleChecks, s.oracleFailures, s.maxChannelError, owner.queryPolls, owner.queryPending,
    JsonBool(owner.queryCompleted), JsonBool(owner.readbackVerified),
    s.windowProgressFrames, owner.window.pulseSequence, owner.window.messages.load(std::memory_order_relaxed),
    static_cast<unsigned long>(owner.window.failure.load(std::memory_order_acquire)),
    owner.comChecks, JsonBool(owner.shaderBytesRoundtripped), JsonBool(owner.resourceHeldDevice),
    static_cast<unsigned long>(owner.deviceRefsWhileResourceHeld), static_cast<unsigned long>(owner.deviceFinalRefs),
    static_cast<unsigned long>(owner.factoryFinalRefs), JsonBool(owner.window.closed), JsonBool(owner.normalTeardown));
  if (owner.options.wsiReadback) {
    // These fields do not exist in the legacy --wsi record. Reading the actual
    // Presenter image is a separate gate, not a re-labelled source readback.
    std::printf(",\"presentedImageReadbackEnabled\":%s,\"presentedImageBeforeFirstPresentAttempts\":%u,"
        "\"presentedImageBeforeFirstPresentRejected\":%s,\"presentedImageReadAttempts\":%u,"
        "\"presentedImageReads\":%u,\"presentedImageBufferGuardChecks\":%u,\"presentedImagePixelsVerified\":%s,"
        "\"presentedImageScope\":\"DXVK-Presenter-swapchain-output-not-compositor-or-HWND-screenshot\","
        "\"presentedImageOracle\":{\"checked\":%u,\"failures\":%u,\"maxChannelError\":%u},"
        "\"totalPixelOracleChecks\":%u",
        JsonBool(s.profileVerified && owner.readPresentedImage != nullptr),
        s.presentedImageBeforeFirstPresentAttempts, JsonBool(s.presentedImageBeforeFirstPresentRejected),
        s.presentedImageReadAttempts, s.presentedImageReads, s.presentedImageBufferGuardChecks,
        JsonBool(s.presentedImagePixelsVerified), s.presentedImageOracleChecks,
        s.presentedImageOracleFailures, s.presentedImageMaxError, s.oracleChecks + s.presentedImageOracleChecks);
  }
  std::printf(",\"frames\":[");
  for (uint32_t i = 0; i < s.framesStarted; ++i) {
    const auto& frame = s.frames[i];
    if (i) std::printf(",");
    std::printf("{\"index\":%u,\"clearColorBGRA\":%u,\"drawCalls\":%u,\"presentHresult\":%lu,"
        "\"windowMessagesBefore\":%u,\"windowMessagesAfter\":%u,\"complete\":%s,\"beforeOracle\":",
        frame.index, frame.clearColor, frame.drawCalls, static_cast<unsigned long>(frame.presentResult),
        frame.windowMessagesBefore, frame.windowMessagesAfter, JsonBool(frame.complete));
    PrintPixelOracle(frame.before);
    std::printf(",\"afterOracle\":");
    PrintPixelOracle(frame.after);
    if (owner.options.wsiReadback) {
      std::printf(",\"presentedFrame\":%llu,\"presentedReadbackHresult\":%lu,\"presentedOracle\":",
          static_cast<unsigned long long>(frame.presentedFrame), static_cast<unsigned long>(frame.presentedReadbackResult));
      PrintPixelOracle(frame.presented);
    }
    std::printf("}");
  }
  std::printf("]}\n");
  std::fflush(stdout);
}

void PrintResult(const Owner& owner, bool ok, const char* error) {
  if (owner.options.wsi) { PrintWsiResult(owner, ok, error); return; }
  const auto& p = owner.pixels;
  const char* presentStatus = !owner.options.present ? "not-requested" : !owner.presentAttempted ? "not-attempted" :
    owner.presentSupported ? "succeeded-hidden-only" : "explicitly-unsupported";
  std::printf("{\"schema\":\"warvk-d3d9-api-fixture/v1\",\"ok\":%s,\"bits\":32,"
    "\"scenario\":\"%s\",\"reference\":%s,\"stage\":%s,\"error\":%s,"
    "\"publicD3D9Api\":true,\"parentVulkanAbsent\":%s,\"processBoundaryChecks\":%u,"
    "\"gameTouched\":false,\"globalInputUsed\":false,\"productIntegrated\":false,\"memorySavingsProven\":false,"
    "\"width\":64,\"height\":64,\"drawCalls\":%u,\"primitiveCount\":%u,"
    "\"vertexBufferBytes\":%u,\"streamOffset\":%u,\"vertexStride\":40,"
    "\"fullVertexUploadBytes\":%u,\"partialVertexUploadBytes\":%u,"
    "\"fullIndexUploadBytes\":%u,\"partialIndexUploadBytes\":%u,\"textureUploadBytes\":%u,"
    "\"vertexShaderBytes\":%u,\"pixelShaderBytes\":%u,\"badVertexShaderBytes\":%u,"
    "\"shaderBytesRoundtripped\":%s,"
    "\"vertexShaderFnv\":\"%016llx\",\"pixelShaderFnv\":\"%016llx\","
    "\"pixelOracle\":{\"checked\":%u,\"clear\":%u,\"opaque\":%u,\"blend\":%u,\"depthProtected\":%u,"
    "\"failures\":%u,\"maxChannelError\":%u,\"imageFnv\":\"%016llx\","
    "\"firstFailure\":{\"x\":%u,\"y\":%u,\"actualBGRA\":%u,\"expectedBGRA\":%u}},"
    "\"comIdentityChecks\":%u,\"invalidChecks\":%u,\"profileRestrictionChecks\":%u,"
    "\"backendRejectionChecks\":%u,\"idleRequestedMs\":%u,\"idleElapsedMs\":%llu,"
    "\"queryPolls\":%u,\"queryPending\":%u,"
    "\"queryCompleted\":%s,\"queryValueGuardChecked\":%s,\"readbackVerified\":%s,"
    "\"present\":\"%s\",\"presentHresult\":%lu,"
    "\"resourceHeldDevice\":%s,\"deviceRefsWhileResourceHeld\":%lu,"
    "\"deviceFinalRefs\":%lu,\"factoryFinalRefs\":%lu,\"windowClosed\":%s,"
    "\"windowMessages\":%u,\"normalTeardown\":%s}\n",
    JsonBool(ok), owner.options.invalid ? "invalid" : "normal", JsonBool(owner.options.reference),
    JsonString(owner.stage).c_str(), JsonString(error).c_str(), JsonBool(owner.parentVulkanAbsent),
    owner.processBoundaryChecks, owner.draws, owner.primitives, VertexBufferBytes, StreamOffset,
    owner.fullVertexUploaded, owner.partialVertexUploaded, owner.fullIndexUploaded, owner.partialIndexUploaded,
    owner.textureUploaded, owner.vertexShaderBytes, owner.pixelShaderBytes, owner.badVertexShaderBytes,
    JsonBool(owner.shaderBytesRoundtripped),
    static_cast<unsigned long long>(owner.vertexShaderHash), static_cast<unsigned long long>(owner.pixelShaderHash),
    p.checked, p.clear, p.opaque, p.blend, p.depthProtected, p.failures, p.maxError,
    static_cast<unsigned long long>(p.imageHash), p.firstX, p.firstY, p.firstActual, p.firstExpected,
    owner.comChecks, owner.invalidChecks, owner.profileRestrictionChecks, owner.backendRejectionChecks,
    owner.idleRequestedMs, static_cast<unsigned long long>(owner.idleElapsedMs), owner.queryPolls, owner.queryPending,
    JsonBool(owner.queryCompleted), JsonBool(owner.queryValueGuardChecked), JsonBool(owner.readbackVerified), presentStatus,
    static_cast<unsigned long>(owner.presentResult), JsonBool(owner.resourceHeldDevice),
    static_cast<unsigned long>(owner.deviceRefsWhileResourceHeld), static_cast<unsigned long>(owner.deviceFinalRefs),
    static_cast<unsigned long>(owner.factoryFinalRefs), JsonBool(owner.window.closed),
    owner.window.messages.load(std::memory_order_relaxed), JsonBool(owner.normalTeardown));
  std::fflush(stdout);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  // On failure an API may have partially initialized driver/worker/WSI state.
  // Keep every owner alive until exact-process termination; do not destruct a
  // driver after stopping its HWND pump. The outer runner owns a hard timeout.
  Owner* owner = nullptr;
  try {
    owner = new Owner;
    owner->options = ParseOptions(argc, argv);
    CreateDevice(*owner);
    InvalidBeforeResources(*owner);
    CreateGeometry(*owner);
    CreateShadersAndTexture(*owner);
    if (owner->options.wsi) StrictWsiFrames(*owner);
    else {
      DrawScene(*owner);
      WaitForEvent(*owner);
      ReadAndVerify(*owner);
      MaybePresent(*owner);
    }
    Teardown(*owner);
    PrintResult(*owner, true, "");
    delete owner;
    return 0;
  } catch (const std::exception& error) {
    if (owner) PrintResult(*owner, false, error.what());
    else { std::printf("{\"schema\":\"warvk-d3d9-api-fixture/v1\",\"ok\":false,\"error\":\"owner allocation\"}\n"); std::fflush(stdout); }
  } catch (...) {
    if (owner) PrintResult(*owner, false, "unknown exception");
    else { std::printf("{\"schema\":\"warvk-d3d9-api-fixture/v1\",\"ok\":false,\"error\":\"unknown setup exception\"}\n"); std::fflush(stdout); }
  }
  ExitProcess(2);
}
