// Real public x86 D3D9 proxy -> x64 DXVK Presenter control-failure experiment.
// Requires the external non-input Desktop launcher and exact Job watchdog.
// One executable run is one case: control | marker | extent | destroy.
//
// "ok" below is only the fixture's expected API/owned cleanup result. A fault
// MUST also have the independent host-fault receipt, proxy remoteTerminal=true/
// deviceLost=true and the expected exact child exit: 2 for a quiescent pre-acquire
// window fault, 4 for the opt-in live-resource quarantine, 0 for healthy control.
// Clear/submission counts must stay unchanged after the rejecting Present.
// Neither a kill-on-close Job nor a timeout is accepted as expected rejection.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <process.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

static_assert(sizeof(void*) == 4, "presenter fault caller must be x86");

namespace {
void Require(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
const char* Bool(bool value) { return value ? "true" : "false"; }
uint32_t Hr(HRESULT value) { return static_cast<uint32_t>(value); }

std::string Json(const std::wstring& value) {
  std::ostringstream out;
  out << '"';
  for (wchar_t c : value) {
    char escaped[7];
    std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(uint16_t(c)));
    out << escaped;
  }
  out << '"';
  return out.str();
}

std::wstring ObjectName(HANDLE object) {
  wchar_t text[256]{};
  DWORD needed = 0;
  Require(object && GetUserObjectInformationW(object, UOI_NAME, text, sizeof(text), &needed)
          && needed >= sizeof(wchar_t) && needed <= sizeof(text), "user object identity");
  return text;
}

std::wstring InputDesktop() {
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  Require(input != nullptr, "input Desktop open");
  std::wstring name;
  try { name = ObjectName(input); }
  catch (...) { CloseDesktop(input); throw; }
  Require(CloseDesktop(input) != FALSE, "input Desktop close");
  return name;
}

struct Options {
  std::wstring dll, desktop, input;
  std::string scenario;
  bool diagnosticReadback = false, liveResource = false;
  bool fault() const { return scenario != "control" && scenario != "clear-soak"; }
};

Options Parse(int argc, wchar_t** argv) {
  Options result;
  for (int i = 1; i < argc; i += 2) {
    Require(i + 1 < argc, "missing argument value");
    const std::wstring key = argv[i], value = argv[i + 1];
    Require(!value.empty(), "empty argument");
    if (key == L"--dll") { Require(result.dll.empty(), "duplicate DLL"); result.dll = value; }
    else if (key == L"--expect-desktop") { Require(result.desktop.empty(), "duplicate Desktop"); result.desktop = value; }
    else if (key == L"--input-desktop") { Require(result.input.empty(), "duplicate input"); result.input = value; }
    else if (key == L"--scenario") {
      Require(result.scenario.empty(), "duplicate scenario");
      if (value == L"control") result.scenario = "control";
      else if (value == L"clear-soak") result.scenario = "clear-soak";
      else if (value == L"marker") result.scenario = "marker";
      else if (value == L"extent") result.scenario = "extent";
      else if (value == L"destroy") result.scenario = "destroy";
      else throw std::runtime_error("unknown scenario");
    } else if (key == L"--diagnostic-readback") {
      Require(!result.diagnosticReadback && value == L"1", "diagnostic readback requires one explicit value 1");
      result.diagnosticReadback = true;
    } else if (key == L"--live-resource") {
      Require(!result.liveResource && value == L"1", "live resource requires one explicit value 1");
      result.liveResource = true;
    } else throw std::runtime_error("unknown argument");
  }
  Require(result.dll.size() > 3 && result.dll[1] == L':' &&
          (result.dll[2] == L'\\' || result.dll[2] == L'/'), "absolute explicit DLL required");
  Require(!result.scenario.empty() && !result.desktop.empty() && !result.input.empty()
          && result.desktop != result.input, "scenario and non-input Desktop required");
  Require(!result.diagnosticReadback || result.scenario == "marker" || result.scenario == "control" || result.scenario == "clear-soak",
          "diagnostic readback requires marker or control scenario");
  Require(!result.liveResource || result.scenario == "marker", "live resource requires marker scenario");
  return result;
}

void DesktopWitness(const Options& options) {
  Require(ObjectName(GetProcessWindowStation()) == L"WinSta0", "window station mismatch");
  Require(ObjectName(GetThreadDesktop(GetCurrentThreadId())) == options.desktop,
          "actual thread Desktop mismatch");
  Require(InputDesktop() == options.input && options.input != options.desktop,
          "input Desktop mismatch");
}

struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE h = nullptr) : value(h) {}
  ~Handle() { if (value) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  void close() {
    if (!value) return;
    Require(CloseHandle(value) != FALSE, "owned handle close");
    value = nullptr;
  }
};

enum class WindowCommand : uint32_t { Pulse, RemoveMarker, Resize, Destroy };

struct WindowOwner {
  explicit WindowOwner(const Options& value) : options(value) {}
  const Options& options;
  Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  Handle command{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  Handle acknowledged{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  Handle thread;
  std::atomic<HWND> window{nullptr};
  std::atomic<bool> prepared{false}, commandOk{false};
  std::atomic<uint32_t> request{0}, completed{0}, failure{0};
  WindowCommand pending = WindowCommand::Pulse;
  DWORD threadId = 0;
  uint32_t pulses = 0, messages = 0, markerMatches = 0, markersRemoved = 0;
  uint32_t widthAfter = 64, heightAfter = 64;
  bool destroyed = false, unregistered = false, closed = false, initiallyHidden = false;
  bool markerEnumerationInvalid = false;
  std::wstring actualDesktop, marker;

  ~WindowOwner() {
    if (thread.value) {
      SetEvent(stop.value);
      if (WaitForSingleObject(thread.value, 5000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "fault fixture: owned window thread did not settle\n");
        std::fflush(stderr);
        ExitProcess(3); // Explicit failed fixture, never a passing cleanup.
      }
    }
  }

  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_CLOSE) return 0; // Only the exact owner command destroys it.
    return DefWindowProcW(hwnd, message, wp, lp);
  }

  static BOOL CALLBACK Property(HWND, LPWSTR name, HANDLE value, ULONG_PTR context) {
    auto& self = *reinterpret_cast<WindowOwner*>(context);
    try {
      if (!IS_INTRESOURCE(name) && std::wstring(name).rfind(L"WarVK-D9C1-", 0) == 0) {
        ++self.markerMatches;
        if (value != reinterpret_cast<HANDLE>(uintptr_t(1))) {
          self.markerEnumerationInvalid = true; return FALSE;
        }
        self.marker = name;
      }
      return TRUE;
    } catch (...) { self.markerEnumerationInvalid = true; return FALSE; }
  }

  void apply() {
    DesktopWitness(options);
    const HWND hwnd = window.load(std::memory_order_acquire);
    Require(hwnd && IsWindow(hwnd), "own HWND no longer exists before mutation");
    DWORD pid = 0;
    Require(GetWindowThreadProcessId(hwnd, &pid) == GetCurrentThreadId()
            && pid == GetCurrentProcessId(), "mutation target is not this owner HWND");
    if (pending == WindowCommand::Pulse) {
      ++pulses;
    } else if (pending == WindowCommand::RemoveMarker) {
      markerMatches = markersRemoved = 0; marker.clear(); markerEnumerationInvalid = false;
      Require(EnumPropsExW(hwnd, Property, reinterpret_cast<LPARAM>(this)) != -1,
              "own HWND property enumeration");
      Require(!markerEnumerationInvalid && markerMatches == 1 && !marker.empty(),
              "expected exactly one owned proxy marker");
      Require(RemovePropW(hwnd, marker.c_str()) == reinterpret_cast<HANDLE>(uintptr_t(1))
              && GetPropW(hwnd, marker.c_str()) == nullptr, "own marker removal failed");
      markersRemoved = 1;
    } else if (pending == WindowCommand::Resize) {
      Require(SetWindowPos(hwnd, nullptr, 0, 0, 65, 64,
              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "owner extent change");
      RECT client{};
      Require(GetClientRect(hwnd, &client) && client.left == 0 && client.top == 0
              && client.right == 65 && client.bottom == 64, "actual changed extent");
      widthAfter = 65;
    } else if (pending == WindowCommand::Destroy) {
      Require(DestroyWindow(hwnd) != FALSE && !IsWindow(hwnd), "owner window destruction");
      window.store(nullptr, std::memory_order_release);
      destroyed = true; widthAfter = heightAfter = 0;
    }
  }

  static unsigned __stdcall Run(void* argument) noexcept {
    auto& self = *static_cast<WindowOwner*>(argument);
    constexpr wchar_t Name[] = L"WarVK_D3D9_Presenter_Fault_Owned";
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    bool registered = false;
    try {
      DesktopWitness(self.options);
      self.actualDesktop = ObjectName(GetThreadDesktop(GetCurrentThreadId()));
      self.threadId = GetCurrentThreadId();
      WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance;
      cls.lpfnWndProc = Proc; cls.lpszClassName = Name;
      Require(RegisterClassExW(&cls) != 0, "owned window class"); registered = true;
      const HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, Name,
          L"WarVK controlled Presenter fault", WS_POPUP, 0, 0, 64, 64,
          nullptr, nullptr, instance, nullptr);
      Require(hwnd != nullptr, "owned window creation");
      self.window.store(hwnd, std::memory_order_release);
      self.initiallyHidden = IsWindowVisible(hwnd) == FALSE;
      Require(self.initiallyHidden, "window not initially hidden");
      // Existing DXVK Presenter WSI proof uses a window shown ONLY on this
      // already verified non-input Desktop. Never activate or switch Desktop.
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      Require(IsWindowVisible(hwnd) != FALSE, "non-input WSI window visibility");
      self.prepared.store(true, std::memory_order_release);
      SetEvent(self.ready.value);
      for (;;) {
        HANDLE events[] = {self.stop.value, self.command.value};
        const DWORD wait = MsgWaitForMultipleObjects(2, events, FALSE, 100, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        Require(wait != WAIT_FAILED, "window pump wait");
        if (wait == WAIT_OBJECT_0 + 1) {
          const uint32_t serial = self.request.load(std::memory_order_acquire);
          self.apply();
          self.commandOk.store(true, std::memory_order_relaxed);
          self.completed.store(serial, std::memory_order_release);
          SetEvent(self.acknowledged.value);
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&message); DispatchMessageW(&message); ++self.messages;
        }
      }
      DesktopWitness(self.options);
    } catch (...) {
      self.failure.store(ERROR_INVALID_DATA, std::memory_order_release);
      SetEvent(self.ready.value); SetEvent(self.acknowledged.value);
    }
    const HWND remaining = self.window.load(std::memory_order_acquire);
    if (remaining) {
      self.destroyed = DestroyWindow(remaining) != FALSE;
      self.window.store(nullptr, std::memory_order_release);
    }
    if (registered) self.unregistered = UnregisterClassW(Name, instance) != FALSE;
    return self.failure.load(std::memory_order_acquire) || !self.destroyed || !self.unregistered ? 2 : 0;
  }

  void start() {
    Require(ready.value && command.value && acknowledged.value && stop.value, "window events");
    const uintptr_t native = _beginthreadex(nullptr, 0, Run, this, 0, nullptr);
    Require(native != 0, "window thread creation"); thread.value = reinterpret_cast<HANDLE>(native);
    Require(WaitForSingleObject(ready.value, 5000) == WAIT_OBJECT_0
            && prepared.load(std::memory_order_acquire) && !failure.load(std::memory_order_acquire),
            "window thread readiness");
    Require(threadId != GetCurrentThreadId() && window.load(std::memory_order_acquire), "independent window owner");
    execute(WindowCommand::Pulse);
  }

  void execute(WindowCommand value) {
    Require(!failure.load(std::memory_order_acquire), "window owner failed");
    pending = value;
    commandOk.store(false, std::memory_order_relaxed);
    const uint32_t serial = request.load(std::memory_order_relaxed) + 1;
    request.store(serial, std::memory_order_release);
    Require(SetEvent(command.value) && WaitForSingleObject(acknowledged.value, 5000) == WAIT_OBJECT_0
            && completed.load(std::memory_order_acquire) == serial
            && commandOk.load(std::memory_order_relaxed) && !failure.load(std::memory_order_acquire),
            "owned window command failed");
  }

  void close() {
    Require(SetEvent(stop.value) && WaitForSingleObject(thread.value, 5000) == WAIT_OBJECT_0,
            "window thread close timeout");
    DWORD exit = STILL_ACTIVE;
    Require(GetExitCodeThread(thread.value, &exit) && exit == 0 && destroyed && unregistered
            && !window.load(std::memory_order_acquire), "normal window thread close");
    thread.close(); ready.close(); command.close(); acknowledged.close(); stop.close(); closed = true;
  }
};

struct Owner {
  HMODULE module = nullptr;
  IDirect3D9* factory = nullptr;
  IDirect3DDevice9* device = nullptr;
  ULONG deviceRefs = std::numeric_limits<ULONG>::max(), factoryRefs = std::numeric_limits<ULONG>::max();
  bool unloaded = false;
  ~Owner() {
    if (device) device->Release();
    if (factory) factory->Release();
    if (module) FreeLibrary(module);
  }
  void close(const std::wstring& dll) {
    if (device) { deviceRefs = device->Release(); device = nullptr; }
    Require(deviceRefs == 0, "public device ownership leaked");
    if (factory) { factoryRefs = factory->Release(); factory = nullptr; }
    Require(factoryRefs == 0, "public factory ownership leaked");
    Require(module && FreeLibrary(module), "proxy DLL unload"); module = nullptr;
    unloaded = GetModuleHandleW(dll.c_str()) == nullptr;
    Require(unloaded, "proxy DLL remains loaded");
  }
};

struct Results {
  HRESULT warmupClear = E_FAIL, warmupPresent = E_FAIL, cooperativeBefore = E_FAIL;
  HRESULT nextPresent = E_FAIL, cooperativeAfter = E_FAIL, nextClear = E_FAIL;
  HRESULT repeatedPresent = E_FAIL, cooperativeFinal = E_FAIL;
  uint32_t clearCalls = 0, presentCalls = 0, cooperativeCalls = 0, successes = 0, rejectionChecks = 0;
};

struct LiveResource {
  static constexpr UINT Bytes = 64;
  IDirect3DVertexBuffer9* buffer = nullptr;
  HRESULT create = E_FAIL, lock = E_FAIL, unlock = E_FAIL;
  ULONG finalRefs = std::numeric_limits<ULONG>::max();
  ~LiveResource() { if (buffer) buffer->Release(); }

  void initialize(IDirect3DDevice9* device) {
    create = device->CreateVertexBuffer(Bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &buffer, nullptr);
    Require(create == D3D_OK && buffer != nullptr, "live public vertex buffer creation");
    void* mapped = nullptr;
    lock = buffer->Lock(0, Bytes, &mapped, 0);
    Require(lock == D3D_OK && mapped != nullptr, "live public vertex buffer lock");
    auto* bytes = static_cast<uint8_t*>(mapped);
    for (UINT i = 0; i < Bytes; ++i) bytes[i] = static_cast<uint8_t>((i * 29u + 7u) & 255u);
    unlock = buffer->Unlock();
    Require(unlock == D3D_OK, "live public vertex buffer upload");
  }

  void close() {
    Require(buffer != nullptr, "live resource missing at terminal release");
    finalRefs = buffer->Release(); buffer = nullptr;
    Require(finalRefs == 0, "live public vertex buffer ownership leaked");
  }

  void append(std::ostringstream& out) const {
    out << ",\"liveResource\":{\"enabled\":true,\"bytes\":" << Bytes
        << ",\"createHresult\":" << Hr(create) << ",\"lockHresult\":" << Hr(lock)
        << ",\"unlockHresult\":" << Hr(unlock) << ",\"finalRefs\":" << finalRefs << '}';
  }
};

using ReadPresentedImage = HRESULT (WINAPI*)(IDirect3DDevice9*, void*, UINT, UINT64*);

// These diagnostic calls are deliberately separate from the original public
// API counters. They inspect the acquired present-image copy, NOT HWND pixels.
struct DiagnosticReadback {
  static constexpr UINT ImageBytes = 64u * 64u * 4u;
  static constexpr UINT64 FrameSentinel = 0x6c5a39f12e7084bdull;
  HRESULT imageResult = E_FAIL, postFault = E_FAIL;
  UINT64 imageFrame = FrameSentinel;
  uint32_t calls = 0, pixelsChecked = 0, pixelFailures = 0, maxRgbError = 0, alphaFailures = 0;
  uint32_t postFaultBytesChecked = 0, postFaultBytesChanged = 0;
  bool postFaultFrameUnchanged = false;

  void checkClear(ReadPresentedImage read, IDirect3DDevice9* device, UINT64 expectedFrame,
                  uint32_t expectedBgra, const char* phase) {
    std::array<uint8_t, ImageBytes> image;
    image.fill(0xee);
    imageResult = read(device, image.data(), ImageBytes, &imageFrame); ++calls;
    Require(imageResult == D3D_OK, "clear presented image readback failed");
    Require(imageFrame == expectedFrame, "clear presented image frame mismatch");
    const uint8_t expectedBgr[] = {uint8_t(expectedBgra), uint8_t(expectedBgra >> 8), uint8_t(expectedBgra >> 16)};
    UINT firstMismatchPixel = ImageBytes / 4u;
    uint32_t firstActualBgra = 0;
    for (UINT pixel = 0; pixel < ImageBytes / 4u; ++pixel) {
      bool failed = false;
      for (UINT channel = 0; channel < 3; ++channel) {
        const uint32_t actual = image[pixel * 4u + channel], expected = expectedBgr[channel];
        const uint32_t error = actual > expected ? actual - expected : expected - actual;
        if (error > maxRgbError) maxRgbError = error;
        failed = failed || error > 1;
      }
      if (image[pixel * 4u + 3u] != 255) { ++alphaFailures; failed = true; }
      ++pixelsChecked;
      if (failed) {
        ++pixelFailures;
        if (firstMismatchPixel == ImageBytes / 4u) {
          firstMismatchPixel = pixel;
          firstActualBgra = uint32_t(image[pixel * 4u])
            | (uint32_t(image[pixel * 4u + 1u]) << 8)
            | (uint32_t(image[pixel * 4u + 2u]) << 16)
            | (uint32_t(image[pixel * 4u + 3u]) << 24);
        }
      }
    }
    if (pixelFailures != 0) {
      // One bounded, typed failure record after the full scan, never a pixel
      // dump. Keep the original strict predicate below unchanged.
      std::fprintf(stderr,
          "{\"schema\":\"warvk-d3d9-presenter-readback-mismatch/v1\",\"phase\":\"%s\","
          "\"firstMismatchX\":%u,\"firstMismatchY\":%u,\"actualBGRA\":%u,"
          "\"actualBGRAHex\":\"0x%08x\",\"expectedBGRA\":%u,\"expectedBGRAHex\":\"0x%08x\","
          "\"pixelsChecked\":%u,\"pixelFailures\":%u,\"maxRgbError\":%u,\"alphaFailures\":%u}\n",
          phase, unsigned(firstMismatchPixel % 64u), unsigned(firstMismatchPixel / 64u),
          unsigned(firstActualBgra), unsigned(firstActualBgra), unsigned(expectedBgra), unsigned(expectedBgra),
          unsigned(pixelsChecked), unsigned(pixelFailures), unsigned(maxRgbError), unsigned(alphaFailures));
      std::fflush(stderr);
    }
    Require(pixelsChecked == 4096 && pixelFailures == 0 && alphaFailures == 0 && maxRgbError <= 1,
            "clear presented image pixels differ from expected BGRA clear");
  }

  void checkTerminal(ReadPresentedImage read, IDirect3DDevice9* device) {
    std::array<uint8_t, ImageBytes> image;
    for (UINT i = 0; i < ImageBytes; ++i) image[i] = static_cast<uint8_t>((i * 73u + 19u) & 255u);
    const auto original = image;
    UINT64 frame = FrameSentinel;
    postFault = read(device, image.data(), ImageBytes, &frame); ++calls;
    for (UINT i = 0; i < ImageBytes; ++i) {
      ++postFaultBytesChecked;
      if (image[i] != original[i]) ++postFaultBytesChanged;
    }
    postFaultFrameUnchanged = frame == FrameSentinel;
    Require(postFault == D3DERR_DEVICELOST, "terminal presented image readback must return DEVICELOST");
    Require(postFaultBytesChecked == ImageBytes && postFaultBytesChanged == 0 && postFaultFrameUnchanged,
            "terminal presented image readback modified caller outputs");
  }

  void append(std::ostringstream& out, const DiagnosticReadback* controlSecond = nullptr) const {
    out << ",\"diagnosticReadback\":{\"schema\":\"warvk-d3d9-presenter-readback-check/v1\","
           "\"enabled\":true,\"profile\":\"presenter-readback-isolated-v1\","
           "\"scope\":\"acquired-present-image-not-window-pixels\",\"format\":\"BGRA8\","
           "\"imageBytes\":" << ImageBytes << ",\"readCalls\":" << (calls + (controlSecond ? controlSecond->calls : 0))
        << ",\"warmupHresult\":" << Hr(imageResult) << ",\"warmupFrame\":" << imageFrame
        << ",\"expectedBgra\":" << uint32_t(0xff204060)
        << ",\"rgbTolerance\":1,\"expectedAlpha\":255,\"pixelsChecked\":" << pixelsChecked
        << ",\"pixelFailures\":" << pixelFailures << ",\"maxRgbError\":" << maxRgbError
        << ",\"alphaFailures\":" << alphaFailures;
    if (controlSecond) {
      out << ",\"controlSecond\":{\"hresult\":" << Hr(controlSecond->imageResult)
          << ",\"frame\":" << controlSecond->imageFrame << ",\"pixelsChecked\":" << controlSecond->pixelsChecked
          << ",\"pixelFailures\":" << controlSecond->pixelFailures << ",\"maxRgbError\":" << controlSecond->maxRgbError
          << ",\"alphaFailures\":" << controlSecond->alphaFailures << ",\"expectedBgra\":" << uint32_t(0xff806040) << '}';
    } else {
      out << ",\"postFaultHresult\":" << Hr(postFault)
          << ",\"postFaultBufferBytesChecked\":" << postFaultBytesChecked
          << ",\"postFaultBufferBytesChanged\":" << postFaultBytesChanged
          << ",\"postFaultFrameUnchanged\":" << Bool(postFaultFrameUnchanged);
    }
    out << '}';
  }
};

void Must(HRESULT actual, HRESULT expected, const char* reason) { Require(actual == expected, reason); }
} // namespace

int wmain(int argc, wchar_t** argv) {
  std::string scenario = "unparsed";
  try {
    const Options options = Parse(argc, argv); scenario = options.scenario;
    Require(!GetModuleHandleW(L"Game.dll") && !GetModuleHandleW(L"vulkan-1.dll"), "game/Vulkan in fixture");
    DesktopWitness(options);
    const std::wstring actualApiDesktop = ObjectName(GetThreadDesktop(GetCurrentThreadId()));
    const std::wstring initialInput = InputDesktop();
    wchar_t profile[64]{};
    const std::wstring expectedProfile = options.diagnosticReadback
        ? L"presenter-readback-isolated-v1" : L"presenter-isolated-v1";
    Require(GetEnvironmentVariableW(L"WARVK_D3D9_PROFILE", profile, 64) == expectedProfile.size()
            && std::wstring(profile) == expectedProfile, "explicit Presenter profile required");
    WindowOwner window(options); window.start();
    Owner owner; Results result; DiagnosticReadback diagnostic, controlSecond; LiveResource liveResource;
    owner.module = LoadLibraryW(options.dll.c_str()); Require(owner.module != nullptr, "proxy DLL load");
    using Create = IDirect3D9* (WINAPI*)(UINT);
    const auto create = reinterpret_cast<Create>(GetProcAddress(owner.module, "Direct3DCreate9"));
    Require(create != nullptr, "public factory export");
    ReadPresentedImage readPresentedImage = nullptr;
    if (options.diagnosticReadback) {
      readPresentedImage = reinterpret_cast<ReadPresentedImage>(
          GetProcAddress(owner.module, "WarVKD3D9ReadPresentedImage"));
      Require(readPresentedImage != nullptr, "diagnostic presented image export");
    }
    owner.factory = create(D3D_SDK_VERSION); Require(owner.factory != nullptr, "public factory");
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window.window.load(std::memory_order_acquire); pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D32F_LOCKABLE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    Must(owner.factory->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &owner.device), D3D_OK,
        "public device initialization");
    Require(owner.device != nullptr, "device output");
    if(options.scenario=="clear-soak") {
      Require(options.diagnosticReadback&&!options.liveResource,"soak requires diagnostic only");
      uint32_t frames=0,clears=0,pixels=0,maxError=0;uint64_t frameSum=0;
      const auto started=GetTickCount64();
      for(uint32_t cycle=0;cycle<64;++cycle) {
        uint32_t color=0;
        for(uint32_t stage=0;stage<4;++stage) {
          if(stage<3)color=0xff000000u|((cycle*3u+stage+17u)<<16)
            |(((cycle*11u+stage*19u+29u)&255u)<<8)|((cycle*23u+stage*31u+47u)&255u);
          if(stage==1) {
            Must(owner.device->Clear(0,nullptr,D3DCLEAR_TARGET,color^0x00808080u,1,0),D3D_OK,"soak decoy clear");++clears;
          }
          if(stage<3) {
            Must(owner.device->Clear(0,nullptr,D3DCLEAR_TARGET|(stage==2?D3DCLEAR_ZBUFFER:0),color,1,0),D3D_OK,"soak clear");++clears;
          }
          Must(owner.device->Present(nullptr,nullptr,nullptr,nullptr),D3D_OK,"soak Present");++frames;
          DiagnosticReadback observed;
          observed.checkClear(readPresentedImage,owner.device,frames,color,"clear-soak");
          pixels+=observed.pixelsChecked;frameSum+=observed.imageFrame;
          if(observed.maxRgbError>maxError)maxError=observed.maxRgbError;
          window.execute(WindowCommand::Pulse);
        }
      }
      const auto elapsed=GetTickCount64()-started;
      Require(frames==256&&clears==256&&pixels==1048576&&frameSum==32896,"soak complete population");
      Must(owner.device->TestCooperativeLevel(),D3D_OK,"soak healthy at end");
      owner.close(options.dll);window.close();DesktopWitness(options);
      Require(!GetModuleHandleW(L"vulkan-1.dll"),"soak x86 Vulkan absent");
      std::ostringstream out;
      out<<"{\"schema\":\"warvk-d3d9-presenter-clear-soak/v1\",\"ok\":true,\"bits\":32,"
        "\"scope\":\"clear-handoff-acquired-image-not-window-pixels\",\"frames\":"<<frames
        <<",\"clears\":"<<clears<<",\"pixelsChecked\":"<<pixels<<",\"frameSum\":"<<frameSum
        <<",\"pixelFailures\":0,\"alphaFailures\":0,\"maxRgbError\":"<<maxError
        <<",\"sourceReadbacks\":0,\"queryCalls\":0,\"authorResources\":0,\"elapsedMs\":"<<elapsed
        <<",\"deviceFinalRefs\":"<<owner.deviceRefs<<",\"factoryFinalRefs\":"<<owner.factoryRefs
        <<",\"moduleUnloaded\":"<<Bool(owner.unloaded)<<",\"windowClosed\":"<<Bool(window.closed)
        <<",\"windowDestroyed\":"<<Bool(window.destroyed)<<",\"windowClassUnregistered\":"<<Bool(window.unregistered)
        <<",\"windowPulses\":"<<window.pulses<<",\"windowMessages\":"<<window.messages
        <<",\"apiThreadId\":"<<GetCurrentThreadId()<<",\"windowThreadId\":"<<window.threadId
        <<",\"apiDesktop\":"<<Json(actualApiDesktop)<<",\"windowDesktop\":"<<Json(window.actualDesktop)
        <<",\"inputBefore\":"<<Json(initialInput)<<",\"inputAfter\":"<<Json(InputDesktop())
        <<",\"publicD3D9Api\":true,\"parentVulkanAbsent\":true,\"gameTouched\":false,"
          "\"globalInputUsed\":false,\"desktopSwitched\":false,\"productIntegrated\":false,"
          "\"memorySavingsProven\":false,\"windowPixelsVerified\":false,\"playerVisualAccepted\":false}";
      std::printf("%s\n",out.str().c_str());return 0;
    }
    result.cooperativeBefore = owner.device->TestCooperativeLevel(); ++result.cooperativeCalls;
    Must(result.cooperativeBefore, D3D_OK, "initial cooperative state");
    if (options.liveResource) liveResource.initialize(owner.device);
    result.warmupClear = owner.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff204060, 1, 0); ++result.clearCalls;
    Must(result.warmupClear, D3D_OK, "first real clear");
    result.warmupPresent = owner.device->Present(nullptr, nullptr, nullptr, nullptr); ++result.presentCalls;
    Must(result.warmupPresent, D3D_OK, "first real Present"); ++result.successes;
    if (options.diagnosticReadback) diagnostic.checkClear(readPresentedImage, owner.device, 1, 0xff204060, "warmup");
    window.execute(WindowCommand::Pulse);
    bool mutationApplied = false;
    if (options.fault()) {
      window.execute(options.scenario == "marker" ? WindowCommand::RemoveMarker :
                     options.scenario == "extent" ? WindowCommand::Resize : WindowCommand::Destroy);
      mutationApplied = true;
      result.nextPresent = owner.device->Present(nullptr, nullptr, nullptr, nullptr); ++result.presentCalls;
      Must(result.nextPresent, D3DERR_DEVICELOST, "mutation must fail Present terminally"); ++result.rejectionChecks;
      result.cooperativeAfter = owner.device->TestCooperativeLevel(); ++result.cooperativeCalls;
      Must(result.cooperativeAfter, D3DERR_DEVICELOST, "cooperative state must remain lost"); ++result.rejectionChecks;
      result.nextClear = owner.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff806040, 1, 0); ++result.clearCalls;
      Must(result.nextClear, D3DERR_DEVICELOST, "later Clear must reject before IPC/GPU"); ++result.rejectionChecks;
      result.repeatedPresent = owner.device->Present(nullptr, nullptr, nullptr, nullptr); ++result.presentCalls;
      Must(result.repeatedPresent, D3DERR_DEVICELOST, "later Present must stay terminal"); ++result.rejectionChecks;
      result.cooperativeFinal = owner.device->TestCooperativeLevel(); ++result.cooperativeCalls;
      Must(result.cooperativeFinal, D3DERR_DEVICELOST, "cooperative state cannot become healthy"); ++result.rejectionChecks;
      if (options.diagnosticReadback) diagnostic.checkTerminal(readPresentedImage, owner.device);
    } else {
      result.nextClear = owner.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff806040, 1, 0); ++result.clearCalls;
      Must(result.nextClear, D3D_OK, "control second clear");
      result.nextPresent = owner.device->Present(nullptr, nullptr, nullptr, nullptr); ++result.presentCalls;
      Must(result.nextPresent, D3D_OK, "control second Present"); ++result.successes;
      if (options.diagnosticReadback) controlSecond.checkClear(readPresentedImage, owner.device, 2, 0xff806040, "control-second");
      result.cooperativeAfter = owner.device->TestCooperativeLevel(); ++result.cooperativeCalls;
      Must(result.cooperativeAfter, D3D_OK, "control remains healthy");
      result.repeatedPresent = S_FALSE; result.cooperativeFinal = S_FALSE; // Explicitly not called in control.
    }
    if (options.liveResource) liveResource.close(); // After terminal checks, before device/Connection destruction.
    owner.close(options.dll); // Emits independent proxy child-exit diagnostics before fixture JSON.
    window.close();
    DesktopWitness(options);
    Require(!GetModuleHandleW(L"vulkan-1.dll"), "x86 loaded Vulkan");
    std::ostringstream out;
    out << "{\"schema\":\"warvk-d3d9-presenter-fault-fixture/v1\",\"ok\":true,"
           "\"scope\":\"controlled-window-failure-not-driver-loss\",\"bits\":32,\"scenario\":\""
        << options.scenario << "\",\"observedExpectedRejection\":" << Bool(options.fault())
        << ",\"mutationApplied\":" << Bool(mutationApplied)
        << ",\"expectedHostExit\":" << (options.liveResource ? 4 : options.fault() ? 2 : 0)
        << ",\"publicD3D9Api\":true,\"fixtureCleanupComplete\":true,\"moduleUnloaded\":" << Bool(owner.unloaded)
        << ",\"deviceFinalRefs\":" << owner.deviceRefs << ",\"factoryFinalRefs\":" << owner.factoryRefs
        << ",\"apiThreadId\":" << GetCurrentThreadId() << ",\"windowThreadId\":" << window.threadId
        << ",\"apiDesktop\":" << Json(actualApiDesktop) << ",\"windowDesktop\":" << Json(window.actualDesktop)
        << ",\"inputBefore\":" << Json(initialInput) << ",\"inputAfter\":" << Json(InputDesktop())
        << ",\"windowClosed\":" << Bool(window.closed) << ",\"windowDestroyed\":" << Bool(window.destroyed)
        << ",\"windowClassUnregistered\":" << Bool(window.unregistered)
        << ",\"windowShownOnNonInputDesktop\":true"
        << ",\"windowPulses\":" << window.pulses << ",\"windowMessages\":" << window.messages
        << ",\"markerMatches\":" << window.markerMatches << ",\"markersRemoved\":" << window.markersRemoved
        << ",\"widthAfterMutation\":" << window.widthAfter << ",\"heightAfterMutation\":" << window.heightAfter
        << ",\"clearCalls\":" << result.clearCalls << ",\"presentCalls\":" << result.presentCalls
        << ",\"cooperativeCalls\":" << result.cooperativeCalls << ",\"successfulPresents\":" << result.successes
        << ",\"terminalRejectionChecks\":" << result.rejectionChecks
        << ",\"warmupClearHresult\":" << Hr(result.warmupClear)
        << ",\"warmupPresentHresult\":" << Hr(result.warmupPresent)
        << ",\"cooperativeBeforeHresult\":" << Hr(result.cooperativeBefore)
        << ",\"nextPresentHresult\":" << Hr(result.nextPresent)
        << ",\"cooperativeAfterHresult\":" << Hr(result.cooperativeAfter)
        << ",\"nextClearHresult\":" << Hr(result.nextClear)
        << ",\"repeatedPresentHresult\":" << Hr(result.repeatedPresent)
        << ",\"cooperativeFinalHresult\":" << Hr(result.cooperativeFinal)
        << ",\"repeatedPresentCalled\":" << Bool(options.fault())
        << ",\"cooperativeFinalCalled\":" << Bool(options.fault())
        << ",\"parentVulkanAbsent\":true,\"postFaultNoGpuLocallyCertified\":false,"
           "\"gpuNormalTeardownClaimed\":false,\"globalInputUsed\":false,\"desktopSwitched\":false,"
           "\"gameTouched\":false,\"productIntegrated\":false,\"memorySavingsProven\":false,"
           "\"windowPixelsVerified\":false,\"playerVisualAccepted\":false";
    if (options.diagnosticReadback) diagnostic.append(out, options.fault() ? nullptr : &controlSecond);
    if (options.liveResource) liveResource.append(out);
    out << '}';
    std::printf("%s\n", out.str().c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Presenter fault fixture [%s]: %s\n", scenario.c_str(), error.what());
  } catch (...) {
    std::fprintf(stderr, "Presenter fault fixture: native failure\n");
  }
  std::printf("{\"schema\":\"warvk-d3d9-presenter-fault-fixture/v1\",\"ok\":false,"
              "\"scope\":\"controlled-window-failure-not-driver-loss\"}\n");
  return 2;
}
