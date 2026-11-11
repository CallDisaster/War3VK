// Actual x86 Connection -> authenticated x64 D3D9 host preflight/close fixture.
// No successful Initialize is sent. This is CPU/IPC evidence, not GPU evidence.
// Run each scenario in its own process with a fresh WARVK_D3D9_HOST_LOG. The
// window-reject scenario additionally requires an already isolated desktop;
// this fixture never switches desktops, shows windows or sends global input.
//
// The runner MUST combine this result with the independent proxy/host JSON:
// childExit=0, transportFailed=false, host normalTeardown=true,
// gpuInitialized=false, backendCalls=0 and all GPU/resource counters zero.
// Connection's destruction emits the actual child-exit receipt; an API reply
// or this executable's exit code by itself is not a host teardown certificate.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../tools/render_host/d3d9_connection.h"
#include <process.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

using namespace warvk::host;
using warvk::host::d3d9proxy::Connection;
static_assert(sizeof(void*) == 4, "initialization producer must be a real x86 process");

namespace {
constexpr wchar_t WindowClass[] = L"WarVK_D3D9_Init_Refusal_Only";
constexpr UINT ResizeMessage = WM_APP + 0x371;
constexpr UINT PulseMessage = WM_APP + 0x372;

void Require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}

const char* Bool(bool value) { return value ? "true" : "false"; }

std::string JsonString(const char* value) {
  std::string result = "\"";
  for (size_t i = 0; value && value[i] && i < 768; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
    else if (c >= 32 && c < 127) result += char(c);
    else result += '?';
  }
  return result + '"';
}

struct WindowOwner {
  lab::Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  lab::Handle thread;
  std::atomic<HWND> window{nullptr};
  std::atomic<DWORD> failure{0};
  std::atomic<uint32_t> pulse{0};
  DWORD threadId = 0;
  bool closed = false;

  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<WindowOwner*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == PulseMessage && self) {
      self->pulse.store(uint32_t(wp), std::memory_order_release); return 1;
    }
    if (message == ResizeMessage) {
      return SetWindowPos(hwnd, nullptr, 0, 0, int(wp), int(lp),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) {
      if (self) self->window.store(nullptr, std::memory_order_release);
      PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }

  static unsigned __stdcall Run(void* parameter) noexcept {
    auto& self = *static_cast<WindowOwner*>(parameter);
    try {
      d9::RequireIsolatedDesktop();
      self.threadId = GetCurrentThreadId();
      WNDCLASSEXW cls{};
      cls.cbSize = sizeof(cls); cls.hInstance = GetModuleHandleW(nullptr);
      cls.lpfnWndProc = Proc; cls.lpszClassName = WindowClass;
      Require(RegisterClassExW(&cls) != 0, "window class creation");
      const HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        WindowClass, L"WarVK CPU-only init refusal", WS_POPUP, 0, 0, 64, 64,
        nullptr, nullptr, cls.hInstance, &self);
      Require(hwnd != nullptr, "hidden refusal window creation");
      self.window.store(hwnd, std::memory_order_release);
      SetEvent(self.ready.get());
      MSG msg{};
      BOOL status;
      while ((status = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
      }
      Require(status == 0, "refusal window message loop");
      Require(UnregisterClassW(WindowClass, cls.hInstance) != FALSE, "window class teardown");
      return 0;
    } catch (...) {
      self.failure.store(ERROR_INVALID_DATA, std::memory_order_release);
      SetEvent(self.ready.get());
      return 2;
    }
  }

  void start() {
    Require(bool(ready), "window readiness event");
    const uintptr_t handle = _beginthreadex(nullptr, 0, Run, this, 0, nullptr);
    Require(handle != 0, "window owner thread creation");
    thread.reset(reinterpret_cast<HANDLE>(handle));
    Require(WaitForSingleObject(ready.get(), 5000) == WAIT_OBJECT_0,
      "window owner readiness timeout");
    const HWND hwnd = window.load(std::memory_order_acquire);
    Require(hwnd && !failure.load(std::memory_order_acquire) && !IsWindowVisible(hwnd),
      "refusal window creation/visibility");
    DWORD pid = 0;
    Require(GetWindowThreadProcessId(hwnd, &pid) == threadId
      && threadId != GetCurrentThreadId() && pid == GetCurrentProcessId(),
      "window is not owned by the dedicated fixture thread");
    DWORD_PTR result = 0;
    Require(SendMessageTimeoutW(hwnd, PulseMessage, 1, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
      2000, &result) != 0 && result == 1 && pulse.load(std::memory_order_acquire) == 1,
      "window owner did not pump its progress probe");
  }

  void resize(LONG width, LONG height) {
    const HWND hwnd = window.load(std::memory_order_acquire);
    DWORD_PTR result = 0;
    Require(SendMessageTimeoutW(hwnd, ResizeMessage, WPARAM(width), LPARAM(height),
      SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0 && result == 1,
      "owner-thread extent change failed");
    RECT client{};
    Require(GetClientRect(hwnd, &client) && client.left == 0 && client.top == 0
      && client.right == width && client.bottom == height, "actual refusal client extent");
  }

  void close() {
    const HWND hwnd = window.load(std::memory_order_acquire);
    Require(hwnd && PostMessageW(hwnd, WM_CLOSE, 0, 0), "window close post");
    Require(WaitForSingleObject(thread.get(), 5000) == WAIT_OBJECT_0,
      "window close timeout");
    DWORD code = STILL_ACTIVE;
    Require(GetExitCodeThread(thread.get(), &code) && code == 0
      && !failure.load(std::memory_order_acquire) && !window.load(std::memory_order_acquire),
      "window owner did not close normally");
    thread.reset(); closed = true;
  }
};

struct Receipt {
  const char* scenario = "unknown";
  uint32_t invalidCallChecks = 0, windowChecks = 0;
  DWORD ownWindowPid = 0, ownWindowThreadId = 0;
  DWORD foreignWindowPid = 0, foreignWindowThreadId = 0;
  bool foreignWindowOwnerTested = false, windowClosed = false;
  bool finishSucceeded = false, connectionDestroyed = false;
};

d9::Header Initialize() {
  d9::Header h; h.op = d9::Op::Initialize; h.args[0] = 64; h.args[1] = 64; return h;
}

void Reject(Connection& connection, Receipt& receipt, const d9::Header& h,
    const char* name, const void* payload = nullptr) {
  const HRESULT result = connection.call(h, payload);
  Require(result == D3DERR_INVALIDCALL, name);
  Require(connection.healthy(), "preflight refusal poisoned the live connection");
  ++receipt.invalidCallChecks;
  std::printf("{\"schema\":\"warvk-d3d9-init-case/v1\",\"case\":%s,\"result\":%lu,"
    "\"index\":%u,\"transportHealthy\":true}\n", JsonString(name).c_str(),
    static_cast<unsigned long>(result), receipt.invalidCallChecks);
}

void InvalidInitialization(Connection& connection, Receipt& receipt) {
  for (size_t axis : {size_t(0), size_t(1)}) {
    // Any 1..4096 per axis is a valid target extent.
    for (uint32_t value : {0u, 4097u, 65536u, UINT32_MAX}) {
      auto h = Initialize(); h.args[axis] = value;
      Reject(connection, receipt, h, axis ? "invalid-height" : "invalid-width");
    }
  }
  for (uint32_t value : {3u, UINT32_MAX}) {
    auto h = Initialize(); h.args[2] = value;
    Reject(connection, receipt, h, "unknown-presentation-profile");
  }
  for (size_t field = 3; field < 16; ++field) {
    // args[6] bits 0 and 1 are negotiated features; 4 is the lowest reserved one.
    auto h = Initialize(); h.args[field] = field == 6 ? 4u : 1u;
    Reject(connection, receipt, h, field < 6 ? "offscreen-has-wsi-identity" : "reserved-initialize-argument");
  }
  {
    auto h = Initialize(); h.id = 1; h.generation = 1;
    Reject(connection, receipt, h, "initialize-has-resource-id");
  }
  {
    const uint8_t payload = 0x5a;
    auto h = Initialize(); h.payloadBytes = 1;
    Reject(connection, receipt, h, "initialize-has-payload", &payload);
  }
  auto wsi = Initialize();
  wsi.args[2] = uint32_t(d9::PresentationProfile::IsolatedPresenter);
  wsi.args[3] = 1; wsi.args[4] = 1; wsi.args[5] = 1;
  for (size_t field : {size_t(3), size_t(4)}) {
    auto h = wsi; h.args[field] = 0;
    Reject(connection, receipt, h, field == 3 ? "wsi-missing-window" : "wsi-missing-thread");
  }
  // ONE and IMMEDIATE are the two windowed intervals; FOUR is not windowed.
  for (uint32_t value : {0u, 2u, 8u, UINT32_MAX}) {
    auto h = wsi; h.args[5] = value;
    Reject(connection, receipt, h, "wsi-unsupported-interval");
  }
  // Clear is always asynchronous: its refusal is terminal by contract, so the
  // recoverable pre-initialize probe uses the synchronous ClearRegions op.
  for (d9::Op op : {d9::Op::ClearRegions, d9::Op::Present, d9::Op::Readback}) {
    d9::Header h; h.op = op;
    Reject(connection, receipt, h, "operation-before-initialize");
  }
}

void WindowRefusals(Connection& connection, WindowOwner& owner, Receipt& receipt) {
  owner.start();
  const HWND hwnd = owner.window.load(std::memory_order_acquire);
  const std::wstring marker = connection.windowMarker();
  Require(GetWindowThreadProcessId(hwnd, &receipt.ownWindowPid) == owner.threadId,
    "window owner identity query");
  receipt.ownWindowThreadId = owner.threadId;
  auto h = Initialize();
  h.args[2] = uint32_t(d9::PresentationProfile::IsolatedPresenter);
  h.args[3] = uint32_t(reinterpret_cast<uintptr_t>(hwnd));
  h.args[4] = owner.threadId; h.args[5] = 1;
  auto rejectWindow = [&](const d9::Header& value, const char* name) {
    Reject(connection, receipt, value, name); ++receipt.windowChecks;
  };

  Require(!GetPropW(hwnd, marker.c_str()) && SetPropW(hwnd, marker.c_str(),
    reinterpret_cast<HANDLE>(uintptr_t(1))), "set first owned marker");
  auto wrongThread = h;
  wrongThread.args[4] = GetCurrentThreadId();
  rejectWindow(wrongThread, "actual-window-wrong-thread");
  Require(RemovePropW(hwnd, marker.c_str()) == reinterpret_cast<HANDLE>(uintptr_t(1)),
    "remove first owned marker");
  rejectWindow(h, "actual-window-missing-marker");

  Require(SetPropW(hwnd, marker.c_str(), reinterpret_cast<HANDLE>(uintptr_t(2))),
    "set incorrect marker value");
  rejectWindow(h, "actual-window-wrong-marker-value");
  Require(RemovePropW(hwnd, marker.c_str()) == reinterpret_cast<HANDLE>(uintptr_t(2)),
    "remove incorrect marker value");
  Require(SetPropW(hwnd, marker.c_str(), reinterpret_cast<HANDLE>(uintptr_t(1))),
    "restore owned marker");
  owner.resize(63, 64); rejectWindow(h, "actual-client-width-outside-profile");
  owner.resize(64, 65); rejectWindow(h, "actual-client-height-outside-profile");
  owner.resize(64, 64);
  Require(RemovePropW(hwnd, marker.c_str()) == reinterpret_cast<HANDLE>(uintptr_t(1)),
    "final owned marker cleanup");

  // Read-only system HWND witness. Do not attach our marker to a foreign window,
  // modify it or send it any message. Some Windows configurations do not expose
  // a process-owned desktop HWND; report that branch as untested, not passed.
  const HWND foreign = GetDesktopWindow();
  const DWORD foreignTid = GetWindowThreadProcessId(foreign, &receipt.foreignWindowPid);
  receipt.foreignWindowThreadId = foreignTid;
  if (IsWindow(foreign) && foreignTid && receipt.foreignWindowPid
      && receipt.foreignWindowPid != GetCurrentProcessId()) {
    auto foreignHeader = h;
    foreignHeader.args[3] = uint32_t(reinterpret_cast<uintptr_t>(foreign));
    foreignHeader.args[4] = foreignTid;
    rejectWindow(foreignHeader, "actual-foreign-window-owner-not-authenticated-peer");
    receipt.foreignWindowOwnerTested = true;
  }

  owner.close();
  receipt.windowClosed = owner.closed;
  Require(!IsWindow(hwnd), "destroyed window unexpectedly still exists");
  rejectWindow(h, "actual-window-destroyed");
}

void Print(const Receipt& r, bool ok, const char* error) {
  std::printf("{\"schema\":\"warvk-d3d9-init/v1\",\"ok\":%s,\"bits\":32,"
    "\"scenario\":%s,\"invalidCallChecks\":%u,\"windowChecks\":%u,"
    "\"expectedExchanges\":%u,\"finishSucceeded\":%s,\"connectionDestroyed\":%s,"
    "\"ownWindowPid\":%lu,\"ownWindowThreadId\":%lu,\"foreignWindowPid\":%lu,"
    "\"foreignWindowThreadId\":%lu,\"foreignWindowOwnerTested\":%s,\"windowClosed\":%s,"
    "\"x86VulkanAbsent\":%s,\"hostReceiptRequired\":true,\"proxyReceiptRequired\":true,"
    "\"gpuAcceptance\":false,\"displayAcceptance\":false,\"error\":%s}\n",
    Bool(ok), JsonString(r.scenario).c_str(), r.invalidCallChecks, r.windowChecks,
    r.invalidCallChecks + uint32_t(r.finishSucceeded), Bool(r.finishSucceeded), Bool(r.connectionDestroyed),
    r.ownWindowPid, r.ownWindowThreadId, r.foreignWindowPid, r.foreignWindowThreadId,
    Bool(r.foreignWindowOwnerTested), Bool(r.windowClosed), Bool(!GetModuleHandleW(L"vulkan-1.dll")),
    JsonString(error).c_str());
  std::fflush(stdout);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  Receipt receipt;
  // On an exceptional path keep the HWND worker's shared state alive until
  // process exit. No thread may observe a destructed WindowOwner container.
  std::unique_ptr<WindowOwner> window;
  try {
    Require(argc == 3 && std::wstring(argv[1]) == L"--scenario",
      "usage: --scenario no-init|invalid-init|window-reject");
    const std::wstring scenario = argv[2];
    if (scenario == L"no-init") receipt.scenario = "no-init";
    else if (scenario == L"invalid-init") receipt.scenario = "invalid-init";
    else if (scenario == L"window-reject") receipt.scenario = "window-reject";
    else throw std::runtime_error("unknown initialization scenario");
    Require(!GetModuleHandleW(L"Game.dll") && !GetModuleHandleW(L"War3.exe")
      && !GetModuleHandleW(L"vulkan-1.dll"), "game/GPU module forbidden in preflight producer");
    const bool windows = scenario == L"window-reject";
    if (windows) {
      d9::RequireIsolatedDesktop();
      window = std::make_unique<WindowOwner>();
    }
    {
      Connection connection(windows);
      if (scenario == L"invalid-init") InvalidInitialization(connection, receipt);
      if (windows) WindowRefusals(connection, *window, receipt);
      d9::Header finish; finish.op = d9::Op::Finish;
      Require(connection.call(finish) == D3D_OK, "CPU-only Finish was not successful");
      Require(connection.healthy(), "CPU-only Finish poisoned the transport");
      receipt.finishSucceeded = true;
    }
    receipt.connectionDestroyed = true;
    Require(!GetModuleHandleW(L"vulkan-1.dll"), "preflight producer loaded Vulkan");
    Print(receipt, true, "");
    return 0;
  } catch (const std::exception& e) {
    (void)window.release();
    Print(receipt, false, e.what());
    ExitProcess(2);
  } catch (...) {
    (void)window.release();
    Print(receipt, false, "non-standard exception");
    ExitProcess(3);
  }
}
