#include "host_surface_target.h"
#include "../d3d9_window_identity.h"
#include <process.h>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>

namespace warvk::host::surface {
namespace {
enum class Fault : uint32_t {
  None, Mode, Caller, Desktop, PeerProcess, PeerThread, PeerWindow, PeerMarker,
  DpiApi, DpiSet, DpiChanged, DpiRestore, Extent, Class, Create, TargetIdentity,
  TargetMarker, ParentChanged, UnexpectedClose, UnexpectedDestroy, Pump,
  StartTimeout, CloseTimeout, SurfaceState, SurfaceCreate, OwnerQuarantined,
};
const char* Name(Fault f) noexcept {
  switch (f) {
#define LABEL(x) case Fault::x: return #x
    LABEL(None); LABEL(Mode); LABEL(Caller); LABEL(Desktop); LABEL(PeerProcess);
    LABEL(PeerThread); LABEL(PeerWindow); LABEL(PeerMarker); LABEL(DpiApi);
    LABEL(DpiSet); LABEL(DpiChanged); LABEL(DpiRestore); LABEL(Extent);
    LABEL(Class); LABEL(Create); LABEL(TargetIdentity); LABEL(TargetMarker);
    LABEL(ParentChanged); LABEL(UnexpectedClose); LABEL(UnexpectedDestroy);
    LABEL(Pump); LABEL(StartTimeout); LABEL(CloseTimeout); LABEL(SurfaceState);
    LABEL(SurfaceCreate); LABEL(OwnerQuarantined);
#undef LABEL
  }
  return "Unknown";
}
struct Error final : std::exception {
  Fault fault;
  DWORD code;
  explicit Error(Fault reason, DWORD error = ERROR_INVALID_DATA) noexcept : fault(reason), code(error) { }
  const char* what() const noexcept override { return Name(fault); }
};
void Need(bool okay, Fault fault, DWORD error = ERROR_INVALID_DATA) {
  if (!okay) throw Error(fault, error);
}
void Win32Need(bool okay, Fault fault) {
  // Evaluate the API before reading its last error (function-argument order
  // would otherwise permit GetLastError() to run before the API).
  if (!okay) throw Error(fault, GetLastError());
}
bool ObjectName(HANDLE object, wchar_t (&name)[256]) noexcept {
  DWORD bytes = 0;
  return object && GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name), &bytes)
    && bytes >= sizeof(wchar_t) && bytes <= sizeof(name) && name[bytes / sizeof(wchar_t) - 1] == 0;
}
bool NameEquals(HANDLE object, const wchar_t* expected) noexcept {
  wchar_t actual[256]{};
  return ObjectName(object, actual) && !std::wcscmp(actual, expected);
}
uint64_t Created(HANDLE object, bool process) {
  FILETIME created{}, ended{}, kernel{}, user{};
  Win32Need((process ? GetProcessTimes(object, &created, &ended, &kernel, &user)
                : GetThreadTimes(object, &created, &ended, &kernel, &user)) != FALSE,
    process ? Fault::PeerProcess : Fault::PeerThread);
  return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
struct DpiApi {
  using WindowContext = DPI_AWARENESS_CONTEXT (WINAPI*)(HWND);
  using ThreadContext = DPI_AWARENESS_CONTEXT (WINAPI*)();
  using SetContext = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
  using Equal = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
  using WindowDpi = UINT (WINAPI*)(HWND);
  WindowContext window = nullptr;
  ThreadContext current = nullptr;
  SetContext set = nullptr;
  Equal equal = nullptr;
  WindowDpi dpi = nullptr;
  void load() {
    const auto user = GetModuleHandleW(L"user32.dll");
    Need(user != nullptr, Fault::DpiApi);
    window = reinterpret_cast<WindowContext>(GetProcAddress(user, "GetWindowDpiAwarenessContext"));
    current = reinterpret_cast<ThreadContext>(GetProcAddress(user, "GetThreadDpiAwarenessContext"));
    set = reinterpret_cast<SetContext>(GetProcAddress(user, "SetThreadDpiAwarenessContext"));
    equal = reinterpret_cast<Equal>(GetProcAddress(user, "AreDpiAwarenessContextsEqual"));
    dpi = reinterpret_cast<WindowDpi>(GetProcAddress(user, "GetDpiForWindow"));
    Need(window && current && set && equal && dpi, Fault::DpiApi, ERROR_CALL_NOT_IMPLEMENTED);
  }
};
class DpiScope final {
  const DpiApi& api;
  DPI_AWARENESS_CONTEXT previous = nullptr;
public:
  DpiScope(const DpiApi& functions, DPI_AWARENESS_CONTEXT expected) : api(functions) {
    Need(expected != nullptr, Fault::DpiSet);
    previous = api.set(expected);
    Need(previous != nullptr, Fault::DpiSet, GetLastError());
    if (!api.current() || !api.equal(api.current(), expected)) {
      (void)restore();
      throw Error(Fault::DpiSet);
    }
  }
  ~DpiScope() { (void)restore(); }
  bool restore() noexcept {
    if (!previous) return true;
    const auto old = previous; previous = nullptr;
    return api.set(old) != nullptr && api.current() && api.equal(api.current(), old);
  }
};
constexpr UINT ReadyPulse = WM_APP + 0x358;
constexpr uint32_t StartMs = 2000, CloseMs = 1000, HeartbeatMs = 1000;
static_assert(sizeof(void*) == 8, "HostSurfaceTarget is x64 dev-host only");
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
} // namespace

Mode ReadStartupMode() {
  wchar_t value[32]{};
  SetLastError(ERROR_SUCCESS);
  const DWORD count = GetEnvironmentVariableW(L"WARVK_D3D9_SURFACE_OWNER", value, 32);
  if (!count && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return Mode::Peer;
  Mode result = Mode::Peer;
  Need(count != 0 && count < 32 && ParseMode(value, result), Fault::Mode);
  return result;
}

struct HostSurfaceTarget::State {
  // Written before thread creation and immutable thereafter.
  Mode mode = Mode::Peer;
  HWND parent = nullptr;
  DWORD peerPid = 0, peerTid = 0, hostPid = 0, backendTid = 0;
  uint64_t peerCreation = 0, peerThreadCreation = 0;
  lab::Handle peerProcess, peerThread, ready, wake;
  std::wstring peerMarker, ownMarker, className;
  wchar_t desktop[256]{}, inputDesktop[256]{};
  DpiApi api;
  DPI_AWARENESS_CONTEXT expectedContext = nullptr;
  UINT expectedDpi = 0;
  HINSTANCE instance = nullptr;
  Lifetime lifetime;

  std::atomic<HWND> live{nullptr}, historical{nullptr};
  std::atomic<DWORD> ownerTid{0};
  std::atomic<uint64_t> heartbeat{0}, firstFault{0};
  std::atomic<uint32_t> generation{1}, messages{0}, pulses{0}, created{0}, destroyed{0}, ncDestroyed{0}, inputMessages{0};
  // Independent of firstFault: an earlier size/quarantine reason must not hide
  // a later OS-cascaded child destruction before the backend's retirement.
  std::atomic<uint32_t> preRetirementDestroyed{0};
  std::atomic<bool> unregistered{false}, exited{false}, terminalPump{false};
  // Owner-UI-thread private; never read by backend diagnostics.
  bool registered = false, markerInstalled = false, readyPermitted = false;

  void fail(Fault reason, DWORD error = ERROR_INVALID_DATA) noexcept {
    uint64_t empty = 0;
    const auto packed = (uint64_t(uint32_t(reason)) << 32) | error;
    if (firstFault.compare_exchange_strong(empty, packed, std::memory_order_acq_rel))
      generation.fetch_add(1, std::memory_order_release);
    lifetime.invalidate();
    if (wake) SetEvent(wake.get());
  }
  Fault fault() const noexcept { return Fault(firstFault.load(std::memory_order_acquire) >> 32); }
  void desktops() const {
    Need(NameEquals(GetProcessWindowStation(), L"WinSta0")
      && NameEquals(GetThreadDesktop(GetCurrentThreadId()), desktop)
      && NameEquals(GetThreadDesktop(peerTid), desktop), Fault::Desktop);
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    wchar_t actual[256]{};
    const bool okay = input && ObjectName(input, actual);
    if (input) CloseDesktop(input);
    Need(okay && !std::wcscmp(actual, inputDesktop)
      && _wcsicmp(actual, desktop) && _wcsicmp(desktop, L"Default"), Fault::Desktop);
  }
  void validateNative(bool ownWindow) const {
    desktops();
    Need(WaitForSingleObject(peerProcess.get(), 0) == WAIT_TIMEOUT
      && GetProcessId(peerProcess.get()) == peerPid, Fault::PeerProcess);
    Need(WaitForSingleObject(peerThread.get(), 0) == WAIT_TIMEOUT
      && GetProcessIdOfThread(peerThread.get()) == peerPid
      && GetThreadId(peerThread.get()) == peerTid, Fault::PeerThread);
    DWORD pid = 0;
    const auto tid = GetWindowThreadProcessId(parent, &pid);
    Need(IsWindow(parent) && pid == peerPid && tid == peerTid, Fault::PeerWindow);
    Need(GetPropW(parent, peerMarker.c_str()) == reinterpret_cast<HANDLE>(uintptr_t(1)), Fault::PeerMarker);
    const auto parentContext = api.window(parent);
    Need(parentContext && api.current() && api.equal(parentContext, expectedContext)
      && api.equal(api.current(), expectedContext) && api.dpi(parent) == expectedDpi, Fault::DpiChanged);
    RECT client{};
    Need(GetClientRect(parent, &client) && !client.left && !client.top
      && client.right == 64 && client.bottom == 64 && !IsIconic(parent), Fault::Extent);
    if (!ownWindow) return;
    const auto hwnd = live.load(std::memory_order_acquire);
    const auto ownedTid = GetWindowThreadProcessId(hwnd, &pid);
    Need(hwnd && hwnd == historical.load(std::memory_order_acquire) && hwnd != parent
      && pid == hostPid && ownedTid == ownerTid.load(std::memory_order_acquire)
      && ownedTid != backendTid && ownedTid != peerTid
      && NameEquals(GetThreadDesktop(ownedTid), desktop), Fault::TargetIdentity);
    Need(GetPropW(hwnd, ownMarker.c_str()) == reinterpret_cast<HANDLE>(uintptr_t(1))
      && GetWindowLongPtrW(hwnd, GWLP_HINSTANCE) == reinterpret_cast<LONG_PTR>(instance), Fault::TargetMarker);
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (mode == Mode::OwnedChild) {
      Need(GetParent(hwnd) == parent && !GetWindow(hwnd, GW_OWNER)
        && (style & (WS_CHILD | WS_POPUP)) == WS_CHILD && (style & WS_DISABLED), Fault::ParentChanged);
      POINT point{}; SetLastError(ERROR_SUCCESS);
      const auto mapped = MapWindowPoints(hwnd, parent, &point, 1);
      Need((mapped || GetLastError() == ERROR_SUCCESS) && !point.x && !point.y, Fault::Extent);
    } else {
      Need(!GetParent(hwnd) && !GetWindow(hwnd, GW_OWNER)
        && (style & (WS_CHILD | WS_POPUP)) == WS_POPUP, Fault::ParentChanged);
    }
    const auto context = api.window(hwnd);
    Need(context && api.equal(context, expectedContext) && api.dpi(hwnd) == expectedDpi, Fault::DpiChanged);
    Need(IsWindowVisible(hwnd) && GetClientRect(hwnd, &client) && !client.left && !client.top
      && client.right == 64 && client.bottom == 64, Fault::Extent);
  }
  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) noexcept {
    State* self = nullptr;
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
      self = static_cast<State*>(create->lpCreateParams);
      if (!self || GetCurrentThreadId() != self->ownerTid.load(std::memory_order_acquire)) return FALSE;
      SetLastError(ERROR_SUCCESS);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      if (GetLastError() != ERROR_SUCCESS) { self->fail(Fault::Create, GetLastError()); return FALSE; }
      self->historical.store(hwnd, std::memory_order_release);
      self->live.store(hwnd, std::memory_order_release);
      self->created.fetch_add(1, std::memory_order_relaxed);
    } else {
      self = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, message, wp, lp);
    // No Vulkan, IPC, log stream, wait, thread join or DestroyWindow here.
    if (message == ReadyPulse) {
      if (self->readyPermitted && self->lifetime.publishReady()) {
        self->pulses.fetch_add(1, std::memory_order_relaxed);
        self->heartbeat.store(GetTickCount64(), std::memory_order_release);
      }
      SetEvent(self->ready.get()); return 0;
    }
    if (message == WM_CLOSE) { self->fail(Fault::UnexpectedClose); return 0; }
    if (message == WM_DPICHANGED) { self->fail(Fault::DpiChanged); return 0; }
    if (message == WM_SIZE && (wp == SIZE_MINIMIZED || LOWORD(lp) != 64 || HIWORD(lp) != 64))
      self->fail(Fault::Extent);
    if ((message >= WM_KEYFIRST && message <= WM_KEYLAST)
      || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST))
      self->inputMessages.fetch_add(1, std::memory_order_relaxed);
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_PAINT) { PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint); EndPaint(hwnd, &paint); return 0; }
    if (message == WM_DESTROY) {
      if (self->lifetime.mustKeepOwnerAlive())
        self->preRetirementDestroyed.fetch_add(1, std::memory_order_relaxed);
      if (!self->lifetime.mayDestroy()) self->fail(Fault::UnexpectedDestroy);
      self->destroyed.fetch_add(1, std::memory_order_relaxed);
    }
    if (message == WM_NCDESTROY) {
      if (self->markerInstalled) {
        if (RemovePropW(hwnd, self->ownMarker.c_str()) != reinterpret_cast<HANDLE>(uintptr_t(1)))
          self->fail(Fault::TargetMarker);
        self->markerInstalled = false;
      }
      if (!self->lifetime.markWindowGone()) self->fail(Fault::UnexpectedDestroy);
      self->live.store(nullptr, std::memory_order_release);
      self->ncDestroyed.fetch_add(1, std::memory_order_relaxed);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      SetEvent(self->wake.get());
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }
  void ownerMain() {
    ownerTid.store(GetCurrentThreadId(), std::memory_order_release);
    validateNative(false);
    Need(!lifetime.invalid() && !lifetime.mayDestroy(), Fault::StartTimeout);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = windowProc; cls.lpszClassName = className.c_str();
    Win32Need(RegisterClassExW(&cls) != 0, Fault::Class); registered = true;
    const bool child = mode == Mode::OwnedChild;
    POINT origin{};
    if (!child) {
      SetLastError(ERROR_SUCCESS);
      const auto mapped = MapWindowPoints(parent, nullptr, &origin, 1);
      Need(mapped || GetLastError() == ERROR_SUCCESS, Fault::Extent);
    }
    // Direct WS_CHILD creation with an authenticated parent. Never SetParent.
    const DWORD ex = child ? WS_EX_NOPARENTNOTIFY : WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    const DWORD style = child ? WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED : WS_POPUP | WS_DISABLED;
    validateNative(false); // Last preflight before the cross-process CreateWindow.
    const auto hwnd = CreateWindowExW(ex, className.c_str(), L"WarVK dev host surface",
      style, origin.x, origin.y, 64, 64, child ? parent : nullptr, nullptr, instance, this);
    Need(hwnd && hwnd == live.load(std::memory_order_acquire), Fault::Create, GetLastError());
    Win32Need(SetPropW(hwnd, ownMarker.c_str(), reinterpret_cast<HANDLE>(uintptr_t(1))) != FALSE,
      Fault::TargetMarker); markerInstalled = true;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    validateNative(true);
    Need(!lifetime.invalid() && !lifetime.mayDestroy(), Fault::StartTimeout);
    readyPermitted = true;
    Win32Need(PostMessageW(hwnd, ReadyPulse, 0, 0) != FALSE, Fault::Pump);
    bool peerExited = false;
    uint64_t nextHealth = 0;
    for (;;) {
      if (lifetime.invalid() && !lifetime.hasSurface()) (void)lifetime.abortBeforeSurface();
      if (lifetime.mayDestroy()) {
        if (const auto owned = live.load(std::memory_order_acquire))
          Win32Need(DestroyWindow(owned) != FALSE, Fault::UnexpectedDestroy);
        break;
      }
      if (!live.load(std::memory_order_acquire)) break;
      MSG message{}; unsigned drained = 0;
      while (drained < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Need(message.message != WM_QUIT, Fault::Pump);
        messages.fetch_add(1, std::memory_order_relaxed);
        TranslateMessage(&message); DispatchMessageW(&message); ++drained;
      }
      const auto now = GetTickCount64();
      heartbeat.store(now, std::memory_order_release);
      if (now >= nextHealth && !lifetime.invalid()) {
        try { validateNative(true); }
        catch (const Error& e) { fail(e.fault, e.code); }
        nextHealth = now + 100;
      }
      // Drain all queued messages before sleeping. A continuous message stream
      // still returns to close/health checks after each bounded 64-message batch.
      if (drained == 64) continue;
      HANDLE waits[] = {wake.get(), peerProcess.get()};
      const DWORD count = peerExited ? 1u : 2u;
      const auto waited = MsgWaitForMultipleObjects(count, waits, FALSE, 100, QS_ALLINPUT);
      if (!peerExited && waited == WAIT_OBJECT_0 + 1) {
        peerExited = true; fail(Fault::PeerProcess, ERROR_PROCESS_ABORTED);
      } else {
        Need(waited == WAIT_OBJECT_0 || waited == WAIT_OBJECT_0 + count || waited == WAIT_TIMEOUT,
          Fault::Pump, GetLastError());
      }
    }
  }
  void preserveFaultedWindow() noexcept {
    // Keeping userdata alive is insufficient: returning from the UI thread
    // itself destroys its HWNDs. An unexpected WM_QUIT/pump exception therefore
    // enters this terminal pump, never a thread-exit/self-cycle workaround.
    // No health recovery or rendering is authorized; the fault stays latched.
    terminalPump.store(true, std::memory_order_release);
    while (live.load(std::memory_order_acquire)) {
      (void)lifetime.abortBeforeSurface();
      if (lifetime.mayDestroy() && !lifetime.mustKeepOwnerAlive()) {
        if (!DestroyWindow(live.load(std::memory_order_acquire)))
          fail(Fault::UnexpectedDestroy, GetLastError());
        if (!live.load(std::memory_order_acquire)) break;
      }
      MSG message{}; unsigned drained = 0;
      while (drained < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        ++drained;
        messages.fetch_add(1, std::memory_order_relaxed);
        if (message.message == WM_QUIT) { fail(Fault::Pump); continue; }
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      heartbeat.store(GetTickCount64(), std::memory_order_release);
      if (drained == 64) continue;
      // Do not wait on a signalled dead-peer HANDLE, which would spin forever.
      const HANDLE wait = wake.get();
      const auto result = MsgWaitForMultipleObjects(1, &wait, FALSE, 100, QS_ALLINPUT);
      if (result != WAIT_OBJECT_0 && result != WAIT_OBJECT_0 + 1 && result != WAIT_TIMEOUT) {
        fail(Fault::Pump, GetLastError());
        Sleep(50); // Terminal degraded park; still polls explicit retirement.
      }
    }
  }
  static unsigned __stdcall run(void* payload) noexcept {
    // The HWND never points at a stack object or at HostSurfaceTarget itself.
    // This reference survives caller startup/close timeout and destruction.
    std::unique_ptr<std::shared_ptr<State>> baton(static_cast<std::shared_ptr<State>*>(payload));
    auto keep = std::move(*baton); baton.reset();
    auto& self = *keep;
    std::unique_ptr<DpiScope> dpi;
    try {
      dpi = std::make_unique<DpiScope>(self.api, self.expectedContext);
      self.ownerMain();
    } catch (const Error& e) { self.fail(e.fault, e.code); }
      catch (...) { self.fail(Fault::Pump); }
    SetEvent(self.ready.get()); // Wake failed startup before a terminal park.
    if (self.live.load(std::memory_order_acquire)) {
      self.preserveFaultedWindow(); // Cannot return while a live HWND remains.
    }
    if (self.registered && !self.live.load(std::memory_order_acquire))
      self.unregistered.store(UnregisterClassW(self.className.c_str(), self.instance) != FALSE, std::memory_order_release);
    if (self.registered && !self.unregistered.load(std::memory_order_acquire))
      self.fail(Fault::Class, GetLastError());
    if (dpi && !dpi->restore()) self.fail(Fault::DpiRestore);
    self.exited.store(true, std::memory_order_release);
    SetEvent(self.ready.get()); // Failed startup wakes caller but is never Ready.
    return self.fault() == Fault::None ? 0u : 2u;
  }
};

HostSurfaceTarget::HostSurfaceTarget() : callerThread(GetCurrentThreadId()) { }
HostSurfaceTarget::~HostSurfaceTarget() noexcept {
  if (state && state->lifetime.abortBeforeSurface() && state->wake) SetEvent(state->wake.get());
  // Closing a HANDLE does not stop a thread. Its own shared State keeps all
  // userdata and events alive. Unknown WSI is not destroyed from this destructor.
  if (thread) CloseHandle(thread);
}
void HostSurfaceTarget::requireCaller() const { Need(GetCurrentThreadId() == callerThread, Fault::Caller); }
void HostSurfaceTarget::start(Mode mode, HWND parent, DWORD pid, DWORD tid,
    HANDLE authenticatedPeerProcess, const wchar_t* marker, uint64_t operationDeadline) {
  requireCaller();
  const auto until = CappedDeadline(GetTickCount64(), StartMs, operationDeadline);
  Need(!state && !thread && (mode == Mode::OwnedTopLevel || mode == Mode::OwnedChild), Fault::Mode);
  auto s = std::make_shared<State>(); state = s;
  try {
    Need(parent && pid && tid && authenticatedPeerProcess && marker, Fault::PeerWindow);
    size_t length = 0; while (length < 128 && marker[length]) ++length;
    Need(length && length < 128, Fault::PeerMarker);
    s->mode = mode; s->parent = parent; s->peerPid = pid; s->peerTid = tid;
    s->hostPid = GetCurrentProcessId(); s->backendTid = callerThread;
    s->peerMarker.assign(marker, length); s->ownMarker = s->peerMarker + L"-host-surface";
    s->className = L"WarVK.HostSurface." + std::to_wstring(s->hostPid) + L"." + s->peerMarker;
    s->instance = GetModuleHandleW(nullptr);
    HANDLE duplicate = nullptr;
    Win32Need(DuplicateHandle(GetCurrentProcess(), authenticatedPeerProcess, GetCurrentProcess(),
      &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE, Fault::PeerProcess);
    s->peerProcess.reset(duplicate);
    Need(GetProcessId(duplicate) == pid && WaitForSingleObject(duplicate, 0) == WAIT_TIMEOUT, Fault::PeerProcess);
    s->peerCreation = Created(duplicate, true);
    s->peerThread.reset(OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid));
    Need(bool(s->peerThread), Fault::PeerThread, GetLastError()); s->peerThreadCreation = Created(s->peerThread.get(), false);
    Need(ObjectName(GetThreadDesktop(callerThread), s->desktop), Fault::Desktop);
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    const bool named = input && ObjectName(input, s->inputDesktop);
    if (input) CloseDesktop(input);
    Need(named, Fault::Desktop);
    s->api.load(); s->expectedContext = s->api.window(parent); s->expectedDpi = s->api.dpi(parent);
    Need(s->expectedContext && s->expectedDpi, Fault::DpiChanged);
    {
      DpiScope dpi(s->api, s->expectedContext);
      s->validateNative(false);
      Need(dpi.restore(), Fault::DpiRestore);
    }
    s->ready.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Win32Need(bool(s->ready), Fault::Pump);
    s->wake.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    Win32Need(bool(s->wake), Fault::Pump);
    Need(Remaining(GetTickCount64(), until) != 0, Fault::StartTimeout, WAIT_TIMEOUT);
    auto baton = std::make_unique<std::shared_ptr<State>>(s);
    thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, State::run, baton.get(), 0, nullptr));
    Need(thread != nullptr, Fault::Pump, GetLastError()); (void)baton.release();
    HANDLE waits[] = {s->ready.get(), thread};
    const auto waited = WaitForMultipleObjects(2, waits, FALSE, Remaining(GetTickCount64(), until));
    Need(waited == WAIT_OBJECT_0, Fault::StartTimeout, waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_INVALID_DATA);
    Need(s->fault() == Fault::None && s->lifetime.usable() && s->pulses.load(std::memory_order_acquire) == 1,
      s->fault() == Fault::None ? Fault::Pump : s->fault());
    threadCreation = Created(thread, false);
    validate(); report("ready");
  } catch (const Error& e) {
    s->fail(e.fault, e.code); (void)s->lifetime.abortBeforeSurface(); if (s->wake) SetEvent(s->wake.get());
    report("startup-failed"); throw;
  } catch (...) {
    s->fail(Fault::Pump); (void)s->lifetime.abortBeforeSurface(); if (s->wake) SetEvent(s->wake.get());
    report("startup-failed"); throw;
  }
}
void HostSurfaceTarget::validate() const {
  requireCaller(); Need(state && thread, Fault::TargetIdentity);
  auto& s = *state;
  try {
    Need(s.fault() == Fault::None && s.lifetime.usable(), s.fault() == Fault::None ? Fault::SurfaceState : s.fault());
    Need(WaitForSingleObject(thread, 0) == WAIT_TIMEOUT && !s.exited.load(std::memory_order_acquire), Fault::Pump);
    const auto now = GetTickCount64(), last = s.heartbeat.load(std::memory_order_acquire);
    Need(last && now >= last && now - last <= HeartbeatMs, Fault::Pump);
    DpiScope dpi(s.api, s.expectedContext);
    s.validateNative(true);
    Need(dpi.restore(), Fault::DpiRestore);
  } catch (const Error& e) { s.fail(e.fault, e.code); throw; }
}
HWND HostSurfaceTarget::validatedWindow() const { validate(); return state->historical.load(std::memory_order_acquire); }
void HostSurfaceTarget::claimSurface() {
  validate(); Need(state->lifetime.claimSurface(), Fault::SurfaceState); report("surface-lifetime-claimed");
}
void HostSurfaceTarget::noteSurfaceResult(HWND actual, int32_t result) {
  requireCaller(); Need(state && actual == state->historical.load(std::memory_order_acquire), Fault::TargetIdentity);
  ++surfaceCalls;
  if (result != 0) state->fail(Fault::SurfaceCreate, DWORD(result));
  report(result == 0 ? "surface-created" : "surface-create-failed");
}
void HostSurfaceTarget::closeAfterSurfaceRetired(uint64_t operationDeadline, bool allowWindowFault) {
  requireCaller(); Need(state && thread, Fault::SurfaceState);
  auto& s = *state;
  const auto until = CappedDeadline(GetTickCount64(), CloseMs, operationDeadline);
  Need(s.lifetime.markSurfaceRetired() && s.lifetime.requestCloseAfterRetirement(), Fault::SurfaceState);
  Win32Need(SetEvent(s.wake.get()) != FALSE, Fault::Pump);
  const auto waited = WaitForSingleObject(thread, Remaining(GetTickCount64(), until));
  if (waited != WAIT_OBJECT_0) {
    s.fail(Fault::CloseTimeout, waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError());
    report("close-failed"); throw Error(Fault::CloseTimeout);
  }
  DWORD code = STILL_ACTIVE;
  Need(GetExitCodeThread(thread, &code) && s.exited.load(std::memory_order_acquire)
    && !s.live.load(std::memory_order_acquire) && s.created.load() == 1
    && s.destroyed.load() == 1 && s.ncDestroyed.load() == 1 && s.unregistered.load(), Fault::Pump);
  Need(code == 0 || (allowWindowFault && code == 2), Fault::Pump);
  Need(allowWindowFault || s.fault() == Fault::None, s.fault());
  report("closed-after-surface-retired");
  Win32Need(CloseHandle(thread) != FALSE, Fault::Pump); thread = nullptr;
}
void HostSurfaceTarget::onOwnerQuarantined() noexcept {
  if (!state) return;
  state->fail(Fault::OwnerQuarantined);
  (void)state->lifetime.abortBeforeSurface();
  if (state->wake) SetEvent(state->wake.get());
}
void HostSurfaceTarget::report(const char* event) const noexcept {
  if (!state) return;
  const auto& s = *state;
  const auto fault = s.firstFault.load(std::memory_order_acquire);
  std::fprintf(stderr, "WARVK_HOST_SURFACE_V1 {\"event\":\"%s\",\"mode\":\"%s\","
    "\"hostPid\":%lu,\"backendTid\":%lu,\"ownerTid\":%lu,\"ownerThreadCreation\":%llu,"
    "\"peerPid\":%lu,\"peerTid\":%lu,\"peerCreation\":%llu,\"peerThreadCreation\":%llu,"
    "\"peerHwnd\":%llu,\"ownedHwnd\":%llu,\"hinstance\":%llu,\"generation\":%u,"
    "\"dpi\":%u,\"width\":64,\"height\":64,\"stateBits\":%u,\"surfaceCalls\":%u,"
    "\"pulses\":%u,\"messages\":%u,\"inputMessages\":%u,\"created\":%u,\"destroyed\":%u,"
    "\"ncDestroyed\":%u,\"preRetirementDestroyed\":%u,\"classUnregistered\":%s,\"terminalPump\":%s,\"fault\":\"%s\",\"faultCode\":%lu,"
    "\"productIntegrated\":false,\"physicalPixelsProven\":false}\n",
    event, ModeName(s.mode), s.hostPid, s.backendTid, s.ownerTid.load(),
    static_cast<unsigned long long>(threadCreation), s.peerPid, s.peerTid,
    static_cast<unsigned long long>(s.peerCreation), static_cast<unsigned long long>(s.peerThreadCreation),
    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.parent)),
    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.historical.load())),
    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.instance)), s.generation.load(),
    s.expectedDpi, s.lifetime.snapshot(), surfaceCalls, s.pulses.load(), s.messages.load(), s.inputMessages.load(),
    s.created.load(), s.destroyed.load(), s.ncDestroyed.load(), s.preRetirementDestroyed.load(),
    s.unregistered.load() ? "true" : "false",
    s.terminalPump.load() ? "true" : "false", Name(Fault(fault >> 32)), DWORD(fault));
  std::fflush(stderr);
}
} // namespace warvk::host::surface
