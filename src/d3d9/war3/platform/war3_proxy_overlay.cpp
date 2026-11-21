// war3_proxy_overlay.cpp - see war3_proxy_overlay.h

#include "war3_proxy_overlay.h"

#include "war3_proxy_diag.h"
#include "../core/war3_runtime_profile.h"
#include "../ui/war3_imgui.h"
#include "../../../util/log/log.h"
#include "../../../util/util_string.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace dxvk::war3::proxy {

namespace {

// ui.panel_open / ui.panel_close, honoured only while RoutePanelRequest sends
// it (as the in-process frame-history test message, d3d9_window.cpp).
constexpr UINT kPanelTestMessage = WM_APP + 0x6f4;

// Owner-thread state.
IDirect3DDevice9* g_device = nullptr;
HWND g_window = nullptr;
bool g_routing = false;

std::atomic<bool> g_available{false};
std::atomic<bool> g_wantOpen{false};
std::atomic<uint64_t> g_routed{0}, g_toggles{0}, g_swallowed{0}, g_opens{0}, g_closes{0}, g_drawn{0};
std::atomic<uint64_t> g_resources{0}, g_resourceFailures{0};
std::atomic<int32_t> g_lastResource{0};

// The subclass of each window the overlay was attached to. It stays in the
// chain, forwarding only, when something subclassed the window after it and it
// cannot be taken out; a later device on the same window attaches it again.
struct WindowHook {
  WNDPROC next = nullptr;
  bool unicode = false;
  bool attached = false;
};
std::mutex g_hooksMutex;
std::unordered_map<HWND, WindowHook> g_hooks;

bool IsImeMessage(UINT message) {
  switch (message) {
  case WM_IME_SETCONTEXT:
  case WM_IME_NOTIFY:
  case WM_IME_CONTROL:
  case WM_IME_COMPOSITIONFULL:
  case WM_IME_SELECT:
  case WM_IME_CHAR:
  case WM_IME_REQUEST:
  case WM_IME_KEYDOWN:
  case WM_IME_KEYUP:
  case WM_IME_STARTCOMPOSITION:
  case WM_IME_ENDCOMPOSITION:
  case WM_IME_COMPOSITION:
  case WM_INPUTLANGCHANGE:
  case WM_INPUTLANGCHANGEREQUEST:
    return true;
  default:
    return false;
  }
}

bool IsMouseMessage(UINT message) {
  return message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN ||
      message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK ||
      message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
      message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDOWN ||
      message == WM_MBUTTONUP || message == WM_MBUTTONDBLCLK ||
      message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL ||
      message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
      message == WM_XBUTTONDBLCLK;
}

bool IsKeyMessage(UINT message) {
  return message == WM_KEYDOWN || message == WM_KEYUP ||
      message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
      message == WM_CHAR || message == WM_SYSCHAR;
}

struct MemorySample {
  uint64_t privateBytes = 0;
  uint64_t virtualUsed = 0;
};

MemorySample SampleMemory() {
  MemorySample sample;
  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  if (::GlobalMemoryStatusEx(&status))
    sample.virtualUsed = status.ullTotalVirtual - status.ullAvailVirtual;
  // PROCESS_MEMORY_COUNTERS_EX without psapi: kernel32's K32 entry.
  struct Counters {
    DWORD cb; DWORD pageFaultCount; SIZE_T peakWorkingSetSize; SIZE_T workingSetSize;
    SIZE_T quotaPeakPagedPoolUsage; SIZE_T quotaPagedPoolUsage; SIZE_T quotaPeakNonPagedPoolUsage;
    SIZE_T quotaNonPagedPoolUsage; SIZE_T pagefileUsage; SIZE_T peakPagefileUsage; SIZE_T privateUsage;
  };
  using Query = BOOL (WINAPI*)(HANDLE, Counters*, DWORD);
  static const Query query = reinterpret_cast<Query>(reinterpret_cast<void*>(
      ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
  Counters counters = {};
  counters.cb = sizeof(counters);
  if (query != nullptr && query(::GetCurrentProcess(), &counters, sizeof(counters)))
    sample.privateBytes = counters.privateUsage;
  return sample;
}

void LogMemory(const char* what, const MemorySample& sample) {
  Logger::info(str::format("WarVK proxy overlay: ", what, " privateBytes=", sample.privateBytes,
                           " vaUsed=", sample.virtualUsed));
}

void Toggle() {
  auto& ui = War3Imgui::get();
  g_toggles.fetch_add(1, std::memory_order_relaxed);
  if (ui.isProxyOwner(g_device))
    ui.toggleVisible();  // a closed panel is released at the next Present
  else
    g_wantOpen.store(!g_wantOpen.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void Request(bool open) {
  auto& ui = War3Imgui::get();
  const bool shown = ui.isProxyOwner(g_device) ? ui.isVisible() : g_wantOpen.load(std::memory_order_relaxed);
  if (shown != open)
    Toggle();
}

LRESULT Forward(const WindowHook& hook, HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (hook.next == nullptr)
    return hook.unicode ? ::DefWindowProcW(window, message, wp, lp) : ::DefWindowProcA(window, message, wp, lp);
  return hook.unicode ? ::CallWindowProcW(hook.next, window, message, wp, lp)
                      : ::CallWindowProcA(hook.next, window, message, wp, lp);
}

// The in-process window procedure's panel part (d3d9_window.cpp, D3D9WindowProc)
// in front of the front end's (d3d9_proxy.cpp, DeviceWindowProc), which keeps
// its own work: the WM_SIZE answer and everything else for the game.
LRESULT CALLBACK PanelWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  WindowHook hook;
  {
    std::lock_guard<std::mutex> lock(g_hooksMutex);
    const auto it = g_hooks.find(window);
    if (it != g_hooks.end()) {
      hook = it->second;
      if (message == WM_NCDESTROY)
        g_hooks.erase(it);
    } else {
      hook.unicode = ::IsWindowUnicode(window) != FALSE;
    }
  }
  if (!hook.attached || window != g_window)
    return Forward(hook, window, message, wp, lp);

  if (message == kPanelTestMessage && g_routing) {
    g_routed.fetch_add(1, std::memory_order_relaxed);
    Request(wp != 0);
    return 1;
  }
  if (message == WM_KEYDOWN && wp == VK_F1 && (::GetKeyState(VK_CONTROL) & 0x8000)) {
    if ((lp & 0x40000000) == 0)
      Toggle();
    return 1;
  }
  // Frame evidence, as in process (d3d9_window.cpp). The console need not be open.
  if (message == WM_KEYDOWN && wp == 'C' && (::GetKeyState(VK_CONTROL) & 0x8000) &&
      (::GetKeyState(VK_SHIFT) & 0x8000)) {
    if ((lp & 0x40000000) == 0)
      DiagRequestEvidence();
    return 1;
  }
  if (IsImeMessage(message) || (message == WM_KEYDOWN && wp == VK_PROCESSKEY))
    return Forward(hook, window, message, wp, lp);

  auto& ui = War3Imgui::get();
  const bool handled = ui.handleWndProc(window, message, wp, lp);
  const bool ownsUi = ui.ownsWindow(window);
  const bool wantMouse = ownsUi && ui.wantsCaptureMouse();
  const bool wantKeyboard = ownsUi && ui.wantsCaptureKeyboard();
  if ((IsMouseMessage(message) && wantMouse) || (IsKeyMessage(message) && wantKeyboard) || handled) {
    g_swallowed.fetch_add(1, std::memory_order_relaxed);
    return 1;
  }
  return Forward(hook, window, message, wp, lp);
}

bool AttachWindow(HWND window) {
  std::lock_guard<std::mutex> lock(g_hooksMutex);
  const auto it = g_hooks.find(window);
  if (it != g_hooks.end()) {
    it->second.attached = true;
    return true;
  }
  WindowHook hook;
  hook.unicode = ::IsWindowUnicode(window) != FALSE;
  const auto proc = reinterpret_cast<LONG_PTR>(&PanelWindowProc);
  // The window thread waits on the lock, so no message misses the entry.
  auto& entry = g_hooks[window];
  entry = hook;
  entry.next = reinterpret_cast<WNDPROC>(hook.unicode ? ::SetWindowLongPtrW(window, GWLP_WNDPROC, proc)
                                                      : ::SetWindowLongPtrA(window, GWLP_WNDPROC, proc));
  if (entry.next == nullptr) {
    g_hooks.erase(window);
    return false;
  }
  entry.attached = true;
  return true;
}

void DetachWindow(HWND window) {
  std::lock_guard<std::mutex> lock(g_hooksMutex);
  const auto it = g_hooks.find(window);
  if (it == g_hooks.end())
    return;
  const auto proc = reinterpret_cast<LONG_PTR>(&PanelWindowProc);
  const LONG_PTR current = it->second.unicode ? ::GetWindowLongPtrW(window, GWLP_WNDPROC)
                                              : ::GetWindowLongPtrA(window, GWLP_WNDPROC);
  if (current != proc || !::IsWindow(window)) {
    // A later subclass still calls through this one: it keeps forwarding.
    it->second.attached = false;
    return;
  }
  const auto next = reinterpret_cast<LONG_PTR>(it->second.next);
  if (it->second.unicode)
    ::SetWindowLongPtrW(window, GWLP_WNDPROC, next);
  else
    ::SetWindowLongPtrA(window, GWLP_WNDPROC, next);
  g_hooks.erase(it);
}

void ClosePanel(IDirect3DDevice9* device, const char* why) {
  auto& ui = War3Imgui::get();
  if (!ui.isInitialized())
    return;
  ui.shutdownProxy(device);
  if (ui.isInitialized())
    return;
  g_closes.fetch_add(1, std::memory_order_relaxed);
  LogMemory(why, SampleMemory());
}

} // namespace

void OverlayConnect(IDirect3DDevice9* device, bool supported) {
  g_wantOpen.store(false, std::memory_order_relaxed);
  g_available.store(false, std::memory_order_release);
  g_device = nullptr;
  g_window = nullptr;
  // The same switch as in process (d3d9_swapchain.cpp): the UI hook module,
  // not the frame-rate-only compatibility mode.
  if (runtime::UseDxvkOnlyCompatibilityMode() ||
      !runtime::IsWar3RuntimeModuleEnabled(runtime::War3RuntimeModule::HookUi)) {
    Logger::info("WarVK proxy overlay: off (UI module disabled)");
    return;
  }
  if (!supported) {
    Logger::warn("WarVK proxy overlay: off (the front end lacks the overlay entries)");
    return;
  }
  D3DDEVICE_CREATION_PARAMETERS creation = {};
  if (FAILED(device->GetCreationParameters(&creation)) || creation.hFocusWindow == nullptr ||
      !AttachWindow(creation.hFocusWindow)) {
    Logger::warn("WarVK proxy overlay: off (device window not subclassed)");
    return;
  }
  g_device = device;
  g_window = creation.hFocusWindow;
  g_available.store(true, std::memory_order_release);
  Logger::info("WarVK proxy overlay: attached (Ctrl+F1)");
}

void OverlayPresent(IDirect3DDevice9* device) {
  if (device == nullptr || device != g_device)
    return;
  auto& ui = War3Imgui::get();
  // Closed by Ctrl+F1, the window's close button or ui.panel_close: release
  // everything (context, font, atlas, buffers) now.
  if (ui.isProxyOwner(device) && !ui.isVisible()) {
    ClosePanel(device, "panel closed");
    return;
  }
  if (!ui.isInitialized() && g_wantOpen.load(std::memory_order_relaxed)) {
    if (device->TestCooperativeLevel() != D3D_OK)
      return;  // asked again at the next Present
    g_wantOpen.store(false, std::memory_order_relaxed);
    LogMemory("panel opening", SampleMemory());
    bool opened = false;
    try {
      opened = ui.initializeProxy(g_window, device);
    } catch (...) {
      opened = false;
    }
    if (!opened) {
      Logger::err("WarVK proxy overlay: panel initialization failed");
      return;
    }
    g_opens.fetch_add(1, std::memory_order_relaxed);
    LogMemory("panel opened", SampleMemory());
  }
  if (!ui.isProxyOwner(device) || !ui.isVisible() || device->TestCooperativeLevel() != D3D_OK)
    return;
  ui.newFrame();
  if (!ui.hasRenderedThisFrame())
    ui.render(false);
  ui.endFrame();
  g_drawn.fetch_add(1, std::memory_order_relaxed);
}

void OverlayReset(IDirect3DDevice9* device) {
  if (device != nullptr && device == g_device)
    War3Imgui::get().invalidateProxy(device);
}

void OverlayReleased(IDirect3DDevice9* device) {
  if (device == nullptr || device != g_device)
    return;
  // The panel's resources hold only private references: release them while
  // the device still exists (this is the first thing its destructor does).
  ClosePanel(device, "panel released with the device");
  DetachWindow(g_window);
  g_available.store(false, std::memory_order_release);
  g_wantOpen.store(false, std::memory_order_relaxed);
  g_device = nullptr;
  g_window = nullptr;
  Logger::info("WarVK proxy overlay: detached");
}

void NoteOverlayResource(HRESULT result) noexcept {
  g_resources.fetch_add(1, std::memory_order_relaxed);
  if (FAILED(result))
    g_resourceFailures.fetch_add(1, std::memory_order_relaxed);
  g_lastResource.store(int32_t(result), std::memory_order_relaxed);
}

bool RoutePanelRequest(bool open) noexcept {
  if (!g_available.load(std::memory_order_acquire) || g_window == nullptr || g_routing)
    return false;
  // The test runs on the game's window thread: SendMessage calls the
  // subclassed window procedure directly, through the whole chain.
  g_routing = true;
  DWORD_PTR answer = 0;
  const LRESULT sent = ::SendMessageTimeoutW(g_window, kPanelTestMessage, open ? 1 : 0, 0,
                                             SMTO_BLOCK | SMTO_ABORTIFHUNG, 2000, &answer);
  g_routing = false;
  return sent != 0 && answer == 1;
}

OverlayStatus QueryOverlayStatus() noexcept {
  OverlayStatus status;
  status.available = g_available.load(std::memory_order_acquire);
  status.wantOpen = g_wantOpen.load(std::memory_order_relaxed);
  status.routedRequests = g_routed.load(std::memory_order_relaxed);
  status.toggles = g_toggles.load(std::memory_order_relaxed);
  status.swallowedInput = g_swallowed.load(std::memory_order_relaxed);
  status.opens = g_opens.load(std::memory_order_relaxed);
  status.closes = g_closes.load(std::memory_order_relaxed);
  status.presentsDrawn = g_drawn.load(std::memory_order_relaxed);
  status.resourcesCreated = g_resources.load(std::memory_order_relaxed);
  status.resourceFailures = g_resourceFailures.load(std::memory_order_relaxed);
  status.lastResourceResult = g_lastResource.load(std::memory_order_relaxed);
  const MemorySample memory = SampleMemory();
  status.privateBytes = memory.privateBytes;
  status.virtualUsed = memory.virtualUsed;
  return status;
}

} // namespace dxvk::war3::proxy
