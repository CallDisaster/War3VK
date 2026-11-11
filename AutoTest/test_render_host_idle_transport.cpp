#include "../tools/render_host/owned_child.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

// Real CPU-only, independent-process regression for the OPT-IN first-byte
// idle boundary. No window, Vulkan, game, simulated pipe or global input.
// Run under an external owned-process watchdog (at least 30 seconds); actual
// cancellation/child-exit drains deliberately never guess completion.
using namespace warvk::host;
namespace {
constexpr DWORD IdleMs = 6500;
constexpr uint8_t Start = 0xa1, Release = 0xa2;
constexpr size_t HeaderBytes = 32;
constexpr std::array<uint8_t, 8> HelloMagic{{'I','D','L','E','T','E','S','T'}};

void CheckAt(bool value, unsigned line) {
  if (!value)
    throw std::runtime_error("idle transport assertion line=" + std::to_string(line));
}
#define Check(value) CheckAt(bool(value), __LINE__)

void CheckCpuOnly() {
  Check(!GetModuleHandleW(L"Game.dll"));
  Check(!GetModuleHandleW(L"vulkan-1.dll"));
}

uint64_t Number(const wchar_t* text) {
  Check(text && *text);
  for (const wchar_t* p = text; *p; ++p)
    Check(*p >= L'0' && *p <= L'9');
  size_t end = 0;
  const auto value = std::stoull(text, &end, 10);
  Check(end == std::wcslen(text));
  return value;
}

std::array<uint8_t, HeaderBytes> Header() {
  std::array<uint8_t, HeaderBytes> header{};
  for (size_t i = 0; i < header.size(); ++i)
    header[i] = uint8_t(0x43 + 17 * i);
  return header;
}

void SendByte(HANDLE pipe, HANDLE peer, uint8_t value, lab::IoStats& stats) {
  lab::WriteBytes(pipe, peer, &value, 1, stats);
}

void AwaitByte(HANDLE pipe, HANDLE peer, uint8_t expected, lab::IoStats& stats) {
  uint8_t value = 0;
  const lab::ReadWindow window;
  lab::ReadExact(pipe, peer, &value, 1, window, lab::Stage::Header, stats);
  Check(value == expected);
}

void AwaitRelease(HANDLE pipe, HANDLE peer, lab::IoStats& stats) {
  uint8_t value = 0;
  // In the partial-header case the parent is intentionally busy for the
  // complete 5 s framing deadline. Its acknowledgement is a healthy idle.
  lab::ReadFirstByteAfterIdle(pipe, peer, &value, stats);
  Check(value == Release);
}

int Child(const std::wstring& mode, ipc::Nonce nonce, DWORD parentPid, uint64_t creation) {
  CheckCpuOnly();
  auto parent = lab::OpenPeer(parentPid, creation);
  auto pipe = lab::Connect(nonce, parent.get(), parentPid, lab::Channel::D3D9Dev5);
  lab::IoStats stats;
  std::array<uint8_t, 16> hello{};
  std::memcpy(hello.data(), HelloMagic.data(), HelloMagic.size());
  // Test control only, never a rendering/resource protocol: the parent uses
  // its OWNED exact process handle to duplicate this client's endpoint.
  ipc::WriteLe(hello.data() + 8, uint64_t(reinterpret_cast<uintptr_t>(pipe.get())), 8);
  lab::WriteBytes(pipe.get(), parent.get(), hello.data(), hello.size(), stats);
  AwaitByte(pipe.get(), parent.get(), Start, stats);
  const auto header = Header();
  if (mode == L"idle6500") {
    // A single planned wait is exactly 6.5 s, interruptible by parent exit.
    Check(WaitForSingleObject(parent.get(), IdleMs) == WAIT_TIMEOUT);
    lab::WriteBytes(pipe.get(), parent.get(), header.data(), header.size(), stats);
    AwaitRelease(pipe.get(), parent.get(), stats);
  } else if (mode == L"partial-header") {
    SendByte(pipe.get(), parent.get(), header[0], stats);
    // Do not send the remaining header. Stay alive so only the receiver's
    // existing framing deadline can cause the expected timeout.
    AwaitRelease(pipe.get(), parent.get(), stats);
  } else {
    Check(mode == L"peer-exit");
    // Parent already holds a duplicate client endpoint, isolating process
    // death from broken-pipe completion. No arbitrary process is killed.
  }
  CheckCpuOnly();
  std::printf("{\"childOk\":true,\"cpuOnly\":true,\"bits\":%u}\n", unsigned(sizeof(void*) * 8));
  return 0;
}

std::wstring Executable() {
  std::array<wchar_t, 32768> path{};
  const auto count = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  Check(count && count < path.size());
  return std::wstring(path.data(), count);
}

void RunCase(const std::wstring& executable, const std::wstring& artifacts,
    const wchar_t* mode, const char* caseName) {
  const auto nonce = lab::RandomNonce();
  auto pipe = lab::CreateServer(nonce, lab::Channel::D3D9Dev5);
  const auto arguments = L"--child " + std::wstring(mode) + L" " + lab::NonceText(nonce)
    + L" " + std::to_wstring(GetCurrentProcessId()) + L" "
    + std::to_wstring(lab::CreationTime(GetCurrentProcess()));
  const auto log = artifacts + L"\\idle-transport-" + mode + L"-" + lab::NonceText(nonce) + L".stdout";
  lab::OwnedChild child(executable, arguments, log);
  lab::IoStats io;
  lab::Accept(pipe.get(), child.process(), io);
  lab::VerifyPeer(pipe.get(), true, child.pid());
  std::array<uint8_t, 16> hello{};
  const lab::ReadWindow helloWindow;
  lab::ReadExact(pipe.get(), child.process(), hello.data(), hello.size(), helloWindow, lab::Stage::Header, io);
  Check(std::memcmp(hello.data(), HelloMagic.data(), HelloMagic.size()) == 0);

  lab::Handle clientEndpointHeld;
  if (!std::wcscmp(mode, L"peer-exit")) {
    const auto remote = ipc::ReadLe(hello.data() + 8, 8);
    Check(remote != 0 && remote <= std::numeric_limits<uintptr_t>::max());
    HANDLE duplicate = nullptr;
    Check(DuplicateHandle(child.process(), reinterpret_cast<HANDLE>(uintptr_t(remote)),
      GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
    clientEndpointHeld.reset(duplicate);
    Check(GetFileType(clientEndpointHeld.get()) == FILE_TYPE_PIPE);
    lab::VerifyPeer(clientEndpointHeld.get(), false, GetCurrentProcessId());
  }

  SendByte(pipe.get(), child.process(), Start, io);
  lab::IoStats measured;
  std::array<uint8_t, HeaderBytes> actual{};
  ULONGLONG elapsed = 0;
  const auto start = GetTickCount64();
  if (!std::wcscmp(mode, L"idle6500")) {
    lab::ReadFirstByteAfterIdle(pipe.get(), child.process(), actual.data(), measured);
    elapsed = GetTickCount64() - start;
    // Time measurement is evidence, not a replacement for byte equality.
    // A 6.5 s child wait must demonstrably outlive the old 5 s deadline.
    Check(elapsed >= 6000);
    const lab::ReadWindow frame;
    lab::ReadExact(pipe.get(), child.process(), actual.data() + 1, actual.size() - 1,
      frame, lab::Stage::Header, measured);
    Check(actual == Header());
    Check(measured.cancelRequests == 0 && measured.cancelCompletions == 0);
    SendByte(pipe.get(), child.process(), Release, io);
  } else if (!std::wcscmp(mode, L"partial-header")) {
    lab::ReadFirstByteAfterIdle(pipe.get(), child.process(), actual.data(), measured);
    Check(actual[0] == Header()[0]);
    bool timedOut = false;
    const auto frameStart = GetTickCount64();
    const lab::ReadWindow frame;
    try {
      lab::ReadExact(pipe.get(), child.process(), actual.data() + 1, actual.size() - 1,
        frame, lab::Stage::Header, measured);
    } catch (const lab::Failure& failure) {
      Check(failure.fault == lab::Fault::Timeout && failure.stage == lab::Stage::Header);
      timedOut = true;
    }
    elapsed = GetTickCount64() - frameStart;
    Check(timedOut && elapsed >= lab::DeadlineMs - 50);
    Check(measured.cancelRequests == 1 && measured.cancelCompletions == 1);
    SendByte(pipe.get(), child.process(), Release, io);
  } else {
    Check(clientEndpointHeld);
    bool peerExited = false;
    try {
      lab::ReadFirstByteAfterIdle(pipe.get(), child.process(), actual.data(), measured);
    } catch (const lab::Failure& failure) {
      Check(failure.fault == lab::Fault::PeerExited && failure.stage == lab::Stage::Header);
      peerExited = true;
    }
    elapsed = GetTickCount64() - start;
    Check(peerExited && measured.cancelRequests == 1 && measured.cancelCompletions == 1);
    Check(WaitForSingleObject(child.process(), 0) == WAIT_OBJECT_0);
    // Release only AFTER pending I/O has actually drained, as witnessed by
    // the common transport completion path, not merely CancelIoEx success.
    clientEndpointHeld.reset();
  }
  Check(child.wait(5000) == 0);
  CheckCpuOnly();
  std::printf("{\"case\":\"%s\",\"ok\":true,\"cpuOnly\":true,\"bits\":%u,"
    "\"childPid\":%lu,\"childCreation\":%llu,\"childExit\":0,\"peerVerified\":true,"
    "\"elapsedMs\":%llu,\"framingDeadlineMs\":%lu,\"reads\":%llu,"
    "\"cancelRequests\":%llu,\"cancelCompletions\":%llu}\n",
    caseName, unsigned(sizeof(void*) * 8), static_cast<unsigned long>(child.pid()),
    static_cast<unsigned long long>(child.creation()), static_cast<unsigned long long>(elapsed),
    static_cast<unsigned long>(lab::DeadlineMs), static_cast<unsigned long long>(measured.reads),
    static_cast<unsigned long long>(measured.cancelRequests), static_cast<unsigned long long>(measured.cancelCompletions));
  std::fflush(stdout);
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    CheckCpuOnly();
    if (argc == 6 && !std::wcscmp(argv[1], L"--child")) {
      const auto pid = Number(argv[4]);
      const auto creation = Number(argv[5]);
      Check(pid != 0 && pid <= UINT32_MAX && creation != 0);
      return Child(argv[2], lab::ParseNonce(argv[3]), DWORD(pid), creation);
    }
    Check(argc == 3 && !std::wcscmp(argv[1], L"--artifacts"));
    const std::wstring artifacts = argv[2];
    lab::QuoteAbsoluteFile(artifacts);
    const auto attributes = GetFileAttributesW(artifacts.c_str());
    Check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
    const auto executable = Executable();
    RunCase(executable, artifacts, L"idle6500", "idle6500");
    RunCase(executable, artifacts, L"partial-header", "partial-header");
    RunCase(executable, artifacts, L"peer-exit", "peer-exit");
    std::printf("{\"schema\":\"warvk-idle-transport/v1\",\"cases\":3,\"passed\":3,"
      "\"childProcesses\":3,\"cpuOnly\":true,\"gameTouched\":false,\"externalWatchdogRequired\":true}\n");
    return 0;
  } catch (const lab::Failure& failure) {
    std::fprintf(stderr, "idle transport failure: fault=%s stage=%s win32=%lu\n",
      lab::FaultName(failure.fault), lab::StageName(failure.stage), static_cast<unsigned long>(failure.win32));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "idle transport failure: %s\n", error.what());
  }
  return 1;
}
