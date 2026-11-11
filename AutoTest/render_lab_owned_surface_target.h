#pragma once
// COPY-TU DIAGNOSTIC ONLY. Never linked by the product/host Meson targets.
// Both arms create the same independent window. Only the VkSurface target
// differs. No reparenting, input injection, system-watcher messages or GL API.
#include "../tools/render_host/d3d9_window_identity.h"
#include <process.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace warvk::host::diagnostic {
class OwnedSurfaceTarget {
  static constexpr UINT Pulse = WM_APP + 0x357;
  static constexpr const wchar_t* Class = L"WarVK.Diagnostic.OwnedSurface.v1";
  HANDLE ready = nullptr, thread = nullptr, pulseAck = nullptr;
  std::atomic<HWND> live{nullptr};
  std::atomic<DWORD> failure{0}, ownerTid{0};
  std::atomic<uint32_t> messages{0}, destroyed{0}, ncDestroyed{0};
  std::atomic<bool> unregistered{false};
  HWND peer = nullptr, historicalWindow = nullptr;
  DWORD peerPid = 0, peerTid = 0, mainTid = 0;
  uint64_t creation = 0, threadCreation = 0;
  uint64_t actualSurfaceHinstance = 0;
  std::wstring desktop, input, marker, peerMarker;
  bool hostTarget = false;
  uint32_t sequence = 0, surfaceCalls = 0, pulses = 0;
  static void need(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
  }
  static uint64_t ft(const FILETIME& value) {
    return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
  }
  static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<OwnedSurfaceTarget*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == Pulse && self) { SetEvent(self->pulseAck); return 0; }
    if (message == WM_CLOSE && self) {
      if (!DestroyWindow(hwnd)) self->failure.store(GetLastError());
      return 0;
    }
    if (message == WM_DESTROY && self) {
      ++self->destroyed;
      PostQuitMessage(0);
      return 0;
    }
    if (message == WM_NCDESTROY && self) {
      if (RemovePropW(hwnd, self->marker.c_str()) != reinterpret_cast<HANDLE>(uintptr_t(1)))
        self->failure.store(ERROR_INVALID_DATA);
      ++self->ncDestroyed;
      self->live.store(nullptr, std::memory_order_release);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }
  static unsigned __stdcall pump(void* data) noexcept {
    auto& self = *static_cast<OwnedSurfaceTarget*>(data);
    self.ownerTid.store(GetCurrentThreadId());
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    bool registered = false;
    try {
      need(d9::RequireIsolatedDesktop() == L"WinSta0\\" + self.desktop
        && d9::InputDesktop() == self.input, "diagnostic target thread Desktop mismatch");
      WNDCLASSEXW cls{};
      cls.cbSize = sizeof(cls); cls.hInstance = instance;
      cls.lpfnWndProc = proc; cls.lpszClassName = Class;
      need(RegisterClassExW(&cls) != 0, "diagnostic target register class");
      registered = true;
      const HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, Class,
        L"WarVK diagnostic surface owner", WS_POPUP, 80, 80, 64, 64,
        nullptr, nullptr, instance, &self);
      need(hwnd != nullptr, "diagnostic target create window");
      self.live.store(hwnd, std::memory_order_release);
      need(SetPropW(hwnd, self.marker.c_str(), reinterpret_cast<HANDLE>(uintptr_t(1))) != FALSE,
        "diagnostic target session property");
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      need(SetEvent(self.ready) != FALSE, "diagnostic target ready event");
      MSG message{};
      BOOL result;
      while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        self.messages.fetch_add(1);
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      need(result == 0 && message.wParam == 0, "diagnostic target message pump failure");
      need(!self.live.load() && self.ncDestroyed.load() == 1,
        "diagnostic target premature pump exit");
    } catch (...) {
      self.failure.store(ERROR_INVALID_DATA);
      if (const HWND hwnd = self.live.load()) DestroyWindow(hwnd); // exact creating thread only
    }
    if (registered) self.unregistered.store(UnregisterClassW(Class, instance) != FALSE);
    SetEvent(self.ready); // failed startup is signalled, never promoted to ready
    return self.failure.load() ? 2u : 0u;
  }
  void pulse() {
    need(ResetEvent(pulseAck) != FALSE && PostMessageW(live.load(), Pulse, 0, 0) != FALSE,
      "diagnostic target pulse post");
    need(WaitForSingleObject(pulseAck, 1000) == WAIT_OBJECT_0 && !failure.load(),
      "diagnostic target pump stalled");
    ++pulses;
  }
  void emit(const char* event, uint64_t closeMs = 0) {
    const HWND target = hostTarget ? historicalWindow : peer;
    std::fprintf(stderr, "WARVK_OWNED_TARGET_V1 {\"event\":\"%s\",\"sequence\":%u,"
      "\"mode\":\"%s\",\"pid\":%lu,\"creation100ns\":%llu,\"mainTid\":%lu,"
      "\"peerHwnd\":%llu,\"peerPid\":%lu,\"peerTid\":%lu,\"peerMarker\":%s,"
      "\"ownedHwnd\":%llu,\"ownedTid\":%lu,\"ownedThreadCreation100ns\":%llu,"
      "\"ownedMarker\":%s,\"surfaceHwnd\":%llu,\"surfacePid\":%lu,\"surfaceTid\":%lu,\"surfaceHinstance\":%llu,"
      "\"desktop\":%s,\"inputDesktop\":%s,\"surfaceCalls\":%u,\"pulses\":%u,"
      "\"messages\":%u,\"destroyed\":%u,\"ncDestroyed\":%u,\"classUnregistered\":%s,"
      "\"failure\":%lu,\"closeMs\":%llu}\n",
      event, ++sequence, hostTarget ? "host" : "peer", GetCurrentProcessId(),
      static_cast<unsigned long long>(creation), mainTid,
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(peer)), peerPid, peerTid,
      d9::NameJson(peerMarker).c_str(),
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(historicalWindow)), ownerTid.load(),
      static_cast<unsigned long long>(threadCreation), d9::NameJson(marker).c_str(),
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)),
      hostTarget ? GetCurrentProcessId() : peerPid, hostTarget ? ownerTid.load() : peerTid,
      static_cast<unsigned long long>(actualSurfaceHinstance),
      d9::NameJson(desktop).c_str(), d9::NameJson(input).c_str(), surfaceCalls, pulses, messages.load(),
      destroyed.load(), ncDestroyed.load(), unregistered.load() ? "true" : "false", failure.load(),
      static_cast<unsigned long long>(closeMs));
    std::fflush(stderr);
  }
public:
  OwnedSurfaceTarget() = default;
  OwnedSurfaceTarget(const OwnedSurfaceTarget&) = delete;
  OwnedSurfaceTarget& operator=(const OwnedSurfaceTarget&) = delete;
  // On terminal backend failure the whole Owner is quarantined by production;
  // do not attempt unproven WSI/GL cleanup from an exception destructor.
  void start(HWND authenticatedPeer, DWORD pid, DWORD tid, const std::wstring& sessionMarker) {
    need(!thread && authenticatedPeer && pid && tid && !sessionMarker.empty(),
      "diagnostic target requires validated peer");
    wchar_t mode[8]{};
    const DWORD n = GetEnvironmentVariableW(L"WARVK_DIAGNOSTIC_SURFACE_OWNER", mode, 8);
    need(n && n < 8 && (std::wstring(mode) == L"peer" || std::wstring(mode) == L"host"),
      "explicit diagnostic surface owner required");
    hostTarget = std::wstring(mode) == L"host";
    peer = authenticatedPeer; peerPid = pid; peerTid = tid; mainTid = GetCurrentThreadId();
    peerMarker = sessionMarker; marker = sessionMarker + L"-diagnostic-owned";
    desktop = d9::CurrentDesktop(); input = d9::InputDesktop();
    need(d9::RequireIsolatedDesktop() == L"WinSta0\\" + desktop, "diagnostic non-input Desktop");
    FILETIME c{}, e{}, k{}, u{};
    need(GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) != FALSE, "diagnostic process creation");
    creation = ft(c);
    ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pulseAck = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    need(ready && pulseAck, "diagnostic target events");
    thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, pump, this, 0, nullptr));
    need(thread && WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0 && !failure.load(),
      "diagnostic target startup failed");
    need(GetThreadTimes(thread, &c, &e, &k, &u) != FALSE, "diagnostic target thread creation");
    threadCreation = ft(c); historicalWindow = live.load();
    validate(); pulse(); emit("ready");
  }
  void validate() const {
    need(thread && GetCurrentThreadId() == mainTid && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT
      && !failure.load(), "diagnostic target owner/thread no longer live");
    const HWND hwnd = live.load(); DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    need(hwnd && hwnd == historicalWindow && pid == GetCurrentProcessId()
      && tid == ownerTid.load() && tid != mainTid && IsWindowVisible(hwnd), "diagnostic target HWND identity");
    need(hwnd != peer && tid != peerTid && !GetParent(hwnd) && !GetWindow(hwnd, GW_OWNER)
      && (GetWindowLongPtrW(hwnd, GWL_STYLE) & (WS_POPUP | WS_CHILD)) == WS_POPUP,
      "diagnostic target must be a distinct unparented top-level window");
    need(GetPropW(hwnd, marker.c_str()) == reinterpret_cast<HANDLE>(uintptr_t(1))
      && GetWindowLongPtrW(hwnd, GWLP_HINSTANCE) == reinterpret_cast<LONG_PTR>(GetModuleHandleW(nullptr)),
      "diagnostic target marker/module identity");
    need(d9::ObjectName(GetThreadDesktop(tid)) == desktop
      && d9::RequireIsolatedDesktop() == L"WinSta0\\" + desktop && d9::InputDesktop() == input,
      "diagnostic target Desktop drift");
    RECT extent{};
    need(GetClientRect(hwnd, &extent) && extent.left == 0 && extent.top == 0
      && extent.right == 64 && extent.bottom == 64, "diagnostic target extent");
  }
  HWND select(HWND originalPeer) const {
    need(originalPeer == peer, "diagnostic target peer changed");
    validate();
    return hostTarget ? historicalWindow : peer;
  }
  void surfaceCreated(HWND actualTarget, int result) {
    need(actualTarget == select(peer) && result == 0 && ++surfaceCalls == 1,
      "diagnostic requires one successful surface creation on the selected target");
    actualSurfaceHinstance = static_cast<uint64_t>(GetWindowLongPtrW(actualTarget, GWLP_HINSTANCE));
    need(actualSurfaceHinstance != 0, "diagnostic surface HINSTANCE missing");
    emit("surface-created");
  }
  void closeAfterSurfaceRetired() {
    if (!thread) return;
    validate(); pulse(); emit("surface-retired");
    const auto start = GetTickCount64();
    need(PostMessageW(live.load(), WM_CLOSE, 0, 0) != FALSE, "diagnostic owned close post");
    // Inside original Finish's 5000ms RPC window, not an added grace period.
    need(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0, "diagnostic owned close timeout");
    DWORD exit = STILL_ACTIVE;
    need(GetExitCodeThread(thread, &exit) && exit == 0 && !failure.load() && !live.load()
      && destroyed.load() == 1 && ncDestroyed.load() == 1 && unregistered.load(),
      "diagnostic owned window did not close normally");
    emit("closed", GetTickCount64() - start);
    need(CloseHandle(thread) != FALSE, "diagnostic close thread handle"); thread = nullptr;
    need(CloseHandle(ready) != FALSE, "diagnostic close ready handle"); ready = nullptr;
    need(CloseHandle(pulseAck) != FALSE, "diagnostic close pulse handle"); pulseAck = nullptr;
  }
};
} // namespace warvk::host::diagnostic
