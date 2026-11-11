// Standalone CPU/USER32 probe. No D3D/Vulkan initialization, game or global input.
// An external exact-process/Job watchdog is mandatory. Compile as x86 or x64.
#include "../tools/render_host/owned_child.h"
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace warvk::host;

namespace {
constexpr UINT PingMessage = WM_APP + 91;

void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

std::wstring ObjectName(HANDLE object) {
  Require(object != nullptr, "user-object-null");
  DWORD bytes = 0;
  GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &bytes);
  Require(bytes > 0 && bytes <= 4096, "user-object-name-size");
  std::vector<wchar_t> text((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t));
  Require(GetUserObjectInformationW(object, UOI_NAME, text.data(), bytes, &bytes) != FALSE,
          "user-object-name-read");
  Require(!text.empty() && text.back() == L'\0', "user-object-name-termination");
  return text.data();
}

std::wstring InputName() {
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  Require(input != nullptr, "input-desktop-open");
  std::wstring name;
  try { name = ObjectName(input); }
  catch (...) { CloseDesktop(input); throw; }
  Require(CloseDesktop(input) != FALSE, "input-desktop-close");
  return name;
}

std::string Json(const std::wstring& value) {
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  Require(count > 0 || value.empty(), "utf8-size");
  std::string utf8(static_cast<size_t>(count), '\0');
  if (count) Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), utf8.data(), count, nullptr, nullptr) == count, "utf8-read");
  std::ostringstream out;
  out << '"';
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char c : utf8) {
    if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
    else if (c < 0x20) out << "\\u00" << hex[c >> 4] << hex[c & 15];
    else out << static_cast<char>(c);
  }
  out << '"';
  return out.str();
}

const char* Boolean(bool value) { return value ? "true" : "false"; }

struct Expected {
  std::wstring desktop, station, input, child, childLog;
};

struct Witness {
  DWORD thread = 0;
  std::wstring station, desktop, inputBefore, inputAfter;
  void collect(const Expected& expected) {
    thread = GetCurrentThreadId();
    station = ObjectName(GetProcessWindowStation());
    desktop = ObjectName(GetThreadDesktop(thread));
    inputBefore = InputName();
    Require(station == expected.station && station == L"WinSta0", "actual-window-station");
    Require(desktop == expected.desktop && desktop != inputBefore, "actual-thread-desktop");
    Require(inputBefore == expected.input, "input-desktop-before");
  }
  void finish(const Expected& expected) {
    inputAfter = InputName();
    Require(inputAfter == expected.input && inputAfter == inputBefore, "input-desktop-after");
    Require(ObjectName(GetProcessWindowStation()) == station &&
            ObjectName(GetThreadDesktop(GetCurrentThreadId())) == desktop, "thread-desktop-changed");
  }
  std::string fields() const {
    std::ostringstream out;
    out << "\"threadId\":" << thread << ",\"station\":" << Json(station)
        << ",\"desktop\":" << Json(desktop) << ",\"inputBefore\":" << Json(inputBefore)
        << ",\"inputAfter\":" << Json(inputAfter);
    return out.str();
  }
};

struct Pump {
  lab::Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  lab::Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  lab::Handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  Witness witness;
  HWND window = nullptr;
  uint32_t messages = 0, pings = 0, width = 0, height = 0;
  bool created = false, hidden = false, destroyed = false, unregistered = false, ok = false;
  std::string error;
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  if (message == PingMessage) {
    auto* pump = reinterpret_cast<Pump*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (pump) ++pump->pings;
    return 0;
  }
  return DefWindowProcW(window, message, wp, lp);
}

void PumpThread(Pump& pump, const Expected& expected) noexcept {
  const wchar_t* className = L"WarVK_NonInput_Desktop_Probe";
  HINSTANCE instance = GetModuleHandleW(nullptr);
  bool registered = false;
  try {
    pump.witness.collect(expected); // Reject wrong Desktop before creating any HWND.
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = WindowProc; cls.lpszClassName = className;
    Require(RegisterClassExW(&cls) != 0, "window-class");
    registered = true;
    pump.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className,
        L"WarVK non-input CPU probe", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, instance, &pump);
    Require(pump.window != nullptr, "window-create");
    pump.created = true;
    RECT rect{};
    Require(GetClientRect(pump.window, &rect) != FALSE, "window-client-rect");
    pump.width = static_cast<uint32_t>(rect.right - rect.left);
    pump.height = static_cast<uint32_t>(rect.bottom - rect.top);
    Require(pump.width == 64 && pump.height == 64, "window-client-size");
    pump.hidden = IsWindowVisible(pump.window) == FALSE;
    Require(pump.hidden, "window-unexpectedly-visible");
    // Only the HWND owner posts to it. The coordinator never borrows an HWND
    // across timeout/destruction, so a recyclable HWND cannot become authority.
    Require(PostMessageW(pump.window, PingMessage, 0, 0) != FALSE, "owned-ping-post");
    Require(SetEvent(pump.ready.get()) != FALSE, "ready-event");
    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
      HANDLE stop = pump.stop.get();
      const DWORD wait = MsgWaitForMultipleObjects(1, &stop, FALSE, 100, QS_ALLINPUT);
      if (wait == WAIT_OBJECT_0) break;
      Require(wait != WAIT_FAILED && GetTickCount64() <= deadline, "pump-timeout");
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message); ++pump.messages;
      }
      if (pump.pings == 1) break;
    }
    pump.hidden = pump.hidden && IsWindowVisible(pump.window) == FALSE;
    Require(pump.hidden && pump.pings == 1 && pump.messages >= 1, "pump-witness");
    pump.witness.finish(expected);
    pump.ok = true;
  } catch (const std::exception& error) { pump.error = error.what(); }
  catch (...) { pump.error = "pump-native-failure"; }
  if (pump.window) {
    pump.destroyed = DestroyWindow(pump.window) != FALSE;
    pump.window = nullptr;
  }
  if (registered) pump.unregistered = UnregisterClassW(className, instance) != FALSE;
  pump.ok = pump.ok && pump.destroyed && pump.unregistered;
  SetEvent(pump.ready.get()); // Also release the coordinator on setup failure.
  SetEvent(pump.done.get());
}

void RunPump(Pump& pump, const Expected& expected) {
  Require(bool(pump.ready) && bool(pump.stop) && bool(pump.done), "pump-events");
  std::thread thread(PumpThread, std::ref(pump), std::cref(expected));
  bool passed = WaitForSingleObject(pump.ready.get(), 5000) == WAIT_OBJECT_0;
  if (!passed) SetEvent(pump.stop.get());
  if (WaitForSingleObject(pump.done.get(), 7000) != WAIT_OBJECT_0) {
    // Do not hang in std::thread's destructor or declare forced cleanup success.
    std::printf("{\"schema\":1,\"scope\":\"NON_INPUT_DESKTOP_PROBE\",\"ok\":false,"
                "\"reason\":\"owned-pump-unsettled\"}\n");
    std::fflush(stdout);
    ExitProcess(2);
  }
  thread.join();
  Require(passed && pump.ok, pump.error.empty() ? "pump-failed" : pump.error.c_str());
}

Expected Parse(int argc, wchar_t** argv) {
  Expected result;
  for (int i = 1; i < argc; i += 2) {
    Require(i + 1 < argc, "argument-value-missing");
    const std::wstring key = argv[i];
    std::wstring* target = nullptr;
    if (key == L"--expect-desktop") target = &result.desktop;
    else if (key == L"--expect-station") target = &result.station;
    else if (key == L"--input-desktop") target = &result.input;
    else if (key == L"--child") target = &result.child;
    else if (key == L"--child-log") target = &result.childLog;
    Require(target != nullptr && target->empty(), "unknown-or-duplicate-argument");
    *target = argv[i + 1];
    Require(!target->empty() && target->find_first_of(L"\"\r\n") == std::wstring::npos,
            "invalid-argument");
  }
  Require(!result.desktop.empty() && result.station == L"WinSta0" && !result.input.empty(), "desktop-arguments");
  Require(result.desktop.find_first_of(L"\\/") == std::wstring::npos &&
          result.input.find_first_of(L"\\/") == std::wstring::npos &&
          result.desktop != result.input, "desktop-name-arguments");
  Require(result.child.empty() == result.childLog.empty(), "child-log-pair");
  return result;
}

std::wstring Quoted(const std::wstring& value) { return L"\"" + value + L"\""; }
} // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    const Expected expected = Parse(argc, argv);
    Witness main;
    main.collect(expected);
    Pump pump;
    RunPump(pump, expected);
    DWORD childPid = 0, childExit = 0;
    uint64_t childCreation = 0;
    const bool childRequested = !expected.child.empty();
    if (childRequested) {
      const std::wstring arguments = L"--expect-desktop " + Quoted(expected.desktop) +
          L" --expect-station " + Quoted(expected.station) + L" --input-desktop " + Quoted(expected.input);
      lab::OwnedChild child(expected.child, arguments, expected.childLog,
                            expected.station + L"\\" + expected.desktop);
      childPid = child.pid(); childCreation = child.creation();
      childExit = child.wait(10000);
      Require(childExit == 0, "child-exit");
    }
    main.finish(expected);
    const bool gpuAbsent = GetModuleHandleW(L"vulkan-1.dll") == nullptr &&
        GetModuleHandleW(L"d3d9.dll") == nullptr && GetModuleHandleW(L"dxgi.dll") == nullptr;
    Require(gpuAbsent, "unexpected-gpu-runtime-module");
    Require(main.thread != pump.witness.thread, "independent-pump-thread");
    std::ostringstream out;
    out << "{\"schema\":1,\"scope\":\"NON_INPUT_DESKTOP_PROBE\",\"bits\":" << sizeof(void*) * 8
        << ",\"ok\":true,\"pid\":" << GetCurrentProcessId()
        << ",\"creation100ns\":" << lab::CreationTime(GetCurrentProcess())
        << ",\"main\":{" << main.fields() << "},\"pump\":{" << pump.witness.fields()
        << ",\"windowCreated\":" << Boolean(pump.created)
        << ",\"windowHidden\":" << Boolean(pump.hidden)
        << ",\"width\":" << pump.width << ",\"height\":" << pump.height
        << ",\"messagesDispatched\":" << pump.messages << ",\"pings\":" << pump.pings
        << ",\"windowDestroyed\":" << Boolean(pump.destroyed)
        << ",\"classUnregistered\":" << Boolean(pump.unregistered)
        << "},\"childRequested\":" << Boolean(childRequested)
        << ",\"childDesktopExplicit\":" << Boolean(childRequested)
        << ",\"childPid\":" << childPid << ",\"childCreation\":" << childCreation
        << ",\"childExit\":" << childExit
        << ",\"gpuModulesAbsent\":true,\"gameTouched\":false,\"globalInputUsed\":false,"
           "\"desktopSwitchUsed\":false,\"normalTeardown\":true}";
    std::printf("%s\n", out.str().c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "desktop probe: %s\n", error.what());
  } catch (...) {
    std::fprintf(stderr, "desktop probe: native/setup failure\n");
  }
  std::printf("{\"schema\":1,\"scope\":\"NON_INPUT_DESKTOP_PROBE\",\"ok\":false,"
              "\"reason\":\"setup-or-witness-failed\"}\n");
  return 2;
}
