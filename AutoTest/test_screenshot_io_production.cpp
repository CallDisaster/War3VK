// Production cancellation control + actual Win32 synchronous pipe/file I/O.
// No Vulkan or game. A blocked pipe is NOT physical slow-disk acceptance.
#include "../src/d3d9/war3/tools/war3_async_screenshot_io.h"
#include "../src/d3d9/war3/tools/war3_async_screenshot_core.h"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace dxvk::war3::tools::screenshot;
unsigned checks = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); ++checks; } while (0)

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) { }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { close(); }
  bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
  bool close() {
    if (!valid()) return true;
    const HANDLE h = value; value = INVALID_HANDLE_VALUE;
    return CloseHandle(h) != FALSE;
  }
};
struct JoiningThread {
  std::thread value;
  ~JoiningThread() { if (value.joinable()) value.join(); }
};

ShutdownReport StopWorker(std::atomic<bool>& stop, IoCancellationGate& gate,
    Win32WorkerHandle& handle, std::thread& worker) {
  return CancelAndJoin(stop, gate, [] { },
      [&] { return handle.waitForCompletion(); },
      [&] { return handle.cancelPending(); },
      [&] { handle.joinAndClose([&] { worker.join(); }); });
}

struct FakeHandleState {
  bool failDuplicate = false;
  std::atomic<bool> live{false}, finished{false}, joined{false}, closedBeforeJoin{false};
  std::atomic<unsigned> duplicates{0}, closes{0}, waits{0}, cancels{0};
};
struct FakeHandleOps {
  FakeHandleState* state;
  WorkerHandlePublication duplicateCurrentThread() const noexcept {
    ++state->duplicates;
    if (state->failDuplicate) return {nullptr, ERROR_ACCESS_DENIED};
    state->live.store(true);
    return {reinterpret_cast<HANDLE>(uintptr_t(0x771)), 0};
  }
  WorkerWait wait(HANDLE, uint32_t milliseconds) const noexcept {
    ++state->waits;
    if (state->finished.load()) return WorkerWait::Complete;
    Sleep(milliseconds);
    return WorkerWait::Pending;
  }
  CancelResult cancel(HANDLE) const noexcept {
    ++state->cancels; return {CancelStatus::Requested};
  }
  void close(HANDLE) const noexcept {
    if (!state->joined.load()) state->closedBeforeJoin.store(true);
    state->live.store(false); ++state->closes;
  }
};

void TestHandleStartupAndFailure() {
  using Control = WorkerHandleControl<FakeHandleOps>;
  {
    FakeHandleState state; Control handle(FakeHandleOps{&state});
    const auto start = std::chrono::steady_clock::now();
    CHECK(handle.waitForCompletion() == WorkerWait::Pending);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(20));
    CHECK(state.waits == 0 && state.closes == 0); // No handle polling/busy-spin.
    handle.markExited();
    handle.joinAndClose([&] { state.joined = true; });
    CHECK(state.closes == 0);
  }
  {
    // Destruction begins before the worker has published a handle. Stop must be
    // visible when it finally starts, with no file operation admitted afterward.
    FakeHandleState state; Control handle(FakeHandleOps{&state});
    std::atomic<bool> stop{false}, sawStop{false}; IoCancellationGate gate;
    Handle startWorker(CreateEventW(nullptr, TRUE, FALSE, nullptr)); CHECK(startWorker.valid());
    JoiningThread worker{std::thread([&] {
      Control::ExitSignal exited(handle);
      if (WaitForSingleObject(startWorker.value, 3000) != WAIT_OBJECT_0) return;
      sawStop.store(stop.load(std::memory_order_acquire));
      handle.publishCurrentThread();
      auto phase = gate.enter(stop);
      if (phase) sawStop.store(false);
      state.finished.store(true);
    })};
    const auto report = CancelAndJoin(stop, gate, [&] { SetEvent(startWorker.value); },
      [&] { return handle.waitForCompletion(); }, [&] { return handle.cancelPending(); },
      [&] { handle.joinAndClose([&] {
        CHECK(state.closes == 0);
        worker.value.join(); state.joined.store(true);
      }); });
    CHECK(sawStop && !worker.value.joinable() && report.cancelAttempts == 0);
    CHECK(state.duplicates == 1 && state.closes == 1 && !state.closedBeforeJoin && !state.live);
  }
  {
    // Failed DuplicateHandle is observable and falls back to the original join.
    // Even with a simulated file phase in progress it never cancels a fake/null
    // native handle or releases storage ahead of join.
    FakeHandleState state; state.failDuplicate = true; Control handle(FakeHandleOps{&state});
    CHECK(!handle.publishCurrentThread());
    CHECK(handle.publicationError() == ERROR_ACCESS_DENIED);
    std::atomic<bool> stop{false}; IoCancellationGate gate; auto phase = gate.enter(stop);
    auto storage = std::make_unique<int>(43);
    const auto report = CancelAndJoin(stop, gate, [] { },
      [&] { return handle.waitForCompletion(); }, [&] { return handle.cancelPending(); },
      [&] { handle.joinAndClose([&] {
        CHECK(storage && *storage == 43 && state.closes == 0);
        phase.finish(); handle.markExited(); state.joined.store(true);
      }); });
    CHECK(report.completionPollFailed && report.cancelAttempts == 0);
    CHECK(state.joined && state.closes == 0 && state.cancels == 0);
  }
  {
    FakeHandleState state; Control handle(FakeHandleOps{&state});
    CHECK(handle.publishCurrentThread()); handle.markExited();
    bool threw = false;
    try { handle.joinAndClose([] { throw std::runtime_error("injected join failure"); }); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && state.live && state.closes == 0);
    handle.joinAndClose([&] { state.joined.store(true); });
    CHECK(state.closes == 1 && !state.closedBeforeJoin);
  }
}

void TestScriptedCompletion() {
  // Stop arrives after the writer's precheck but before it enters WriteFile:
  // the first cancel cannot find anything. Later requests do not permit an
  // early return while the simulated operation still owns its buffer.
  for (const auto finalStatus : {CancelStatus::Requested, CancelStatus::Failed}) {
    std::atomic<bool> stop{false}; IoCancellationGate gate;
    auto phase = gate.enter(stop); CHECK(bool(phase));
    auto buffer = std::make_unique<unsigned>(17);
    unsigned waits = 0, cancels = 0, joins = 0;
    bool woken = false, completed = false;
    auto report = CancelAndJoin(stop, gate, [&] {
      CHECK(stop.load()); woken = true;
    }, [&] {
      CHECK(woken && buffer && *buffer == 17);
      if (++waits != 5) return WorkerWait::Pending;
      completed = true; phase.finish(); return WorkerWait::Complete;
    }, [&] {
      CHECK(!completed && buffer && *buffer == 17);
      if (++cancels == 1) return CancelResult{CancelStatus::NotFound, ERROR_NOT_FOUND};
      return CancelResult{finalStatus, finalStatus == CancelStatus::Failed ? ERROR_ACCESS_DENIED : 0u};
    }, [&] { CHECK(completed && buffer); ++joins; });
    CHECK(waits == 5 && cancels == 4 && joins == 1);
    CHECK(report.cancelNotFound == 1 && report.cancelAttempts == 4);
    CHECK(report.cancelRequested == (finalStatus == CancelStatus::Requested ? 3u : 0u));
    CHECK(report.cancelFailed == (finalStatus == CancelStatus::Failed ? 3u : 0u));
    CHECK(!report.completionPollFailed);
    buffer.reset(); // Only after production control returned from join.
    CHECK(!gate.enter(stop)); // A later file operation cannot start after stop.
  }

  // A cancellation request may lose to normal completion. The control layer
  // does not invent ERROR_OPERATION_ABORTED or alter the actual save outcome.
  std::atomic<bool> stop{false}; IoCancellationGate gate;
  auto phase = gate.enter(stop);
  unsigned waits = 0; bool normalCompletion = false, joined = false;
  const auto normal = CancelAndJoin(stop, gate, [] { }, [&] {
    if (++waits == 1) return WorkerWait::Pending;
    normalCompletion = true; phase.finish(); return WorkerWait::Complete;
  }, [] { return CancelResult{CancelStatus::Requested}; }, [&] { joined = true; });
  CHECK(normal.cancelRequested == 1 && normalCompletion && joined);

  // A failed observation must still join; it is not a release permission.
  std::atomic<bool> stop2{false}; IoCancellationGate gate2;
  auto phase2 = gate2.enter(stop2); bool fallbackJoined = false;
  const auto failedPoll = CancelAndJoin(stop2, gate2, [] { },
    [] { return WorkerWait::Failed; },
    [] { return CancelResult{CancelStatus::Requested}; },
    [&] { phase2.finish(); fallbackJoined = true; });
  CHECK(failedPoll.completionPollFailed && fallbackJoined);
}

void TestOutsideFilePhase() {
  std::atomic<bool> stop{false}; IoCancellationGate gate;
  unsigned waits = 0, cancels = 0, joins = 0;
  const auto report = CancelAndJoin(stop, gate, [] { }, [&] {
    return ++waits < 4 ? WorkerWait::Pending : WorkerWait::Complete;
  }, [&] { ++cancels; return CancelResult{CancelStatus::Requested}; }, [&] { ++joins; });
  CHECK(waits == 4 && cancels == 0 && joins == 1 && report.cancelAttempts == 0);
  CHECK(!gate.enter(stop));
}

void TestCancellationCannotReachLoggerPhase() {
  std::atomic<bool> stop{false}; IoCancellationGate gate;
  Handle entered(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  Handle finishNow(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  Handle finishing(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  Handle loggerPhase(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  CHECK(entered.valid() && finishNow.valid() && finishing.valid() && loggerPhase.valid());
  std::atomic<bool> workerOk{false};
  JoiningThread worker{std::thread([&] {
    auto phase = gate.enter(stop);
    if (!phase) { SetEvent(entered.value); return; }
    SetEvent(entered.value);
    if (WaitForSingleObject(finishNow.value, 3000) != WAIT_OBJECT_0) return;
    SetEvent(finishing.value);
    phase.finish();
    // Represents the production Logger/evidence calls, outside the file phase.
    workerOk.store(true);
    SetEvent(loggerPhase.value);
  })};
  CHECK(WaitForSingleObject(entered.value, 3000) == WAIT_OBJECT_0);
  bool cancelCalled = false;
  gate.cancelIfActive([&] {
    cancelCalled = true;
    SetEvent(finishNow.value);
    CHECK(WaitForSingleObject(finishing.value, 3000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(loggerPhase.value, 0) == WAIT_TIMEOUT);
  });
  CHECK(cancelCalled);
  CHECK(WaitForSingleObject(loggerPhase.value, 3000) == WAIT_OBJECT_0);
  worker.value.join();
  CHECK(workerOk.load());
  gate.cancelIfActive([&] { CHECK(false); });
}

void TestActualBlockedPipeCancellation(unsigned ordinal) {
  const std::wstring name = L"\\\\.\\pipe\\WarVK-screenshot-io-test-" +
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
      L"-" + std::to_wstring(ordinal);
  Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr));
  CHECK(pipe.valid());
  Handle client(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  CHECK(client.valid());
  const BOOL connected = ConnectNamedPipe(pipe.value, nullptr);
  CHECK(connected || GetLastError() == ERROR_PIPE_CONNECTED);
  Handle entered(CreateEventW(nullptr, TRUE, FALSE, nullptr)); CHECK(entered.valid());
  std::vector<uint8_t> bytes(1024 * 1024, 0xA5);
  std::atomic<bool> stop{false}; IoCancellationGate gate;
  Win32WorkerHandle workerHandle;
  std::atomic<DWORD> error{ERROR_SUCCESS}; std::atomic<bool> wrote{false};
  JoiningThread worker{std::thread([&] {
    Win32WorkerHandle::ExitSignal exited(workerHandle);
    const bool handleReady = workerHandle.publishCurrentThread();
    auto phase = gate.enter(stop);
    SetEvent(entered.value);
    if (!phase || !handleReady) return;
    DWORD transferred = 0;
    const BOOL ok = WriteFile(pipe.value, bytes.data(), DWORD(bytes.size()), &transferred, nullptr);
    error.store(ok ? ERROR_SUCCESS : GetLastError());
    wrote.store(ok != FALSE);
  })};
  CHECK(WaitForSingleObject(entered.value, 3000) == WAIT_OBJECT_0);
  // The client deliberately never drains the 4 KiB pipe. Prove this is still
  // outstanding before requesting shutdown; no Sleep-only fake slow writer.
  CHECK(workerHandle.waitForCompletion(50) == WorkerWait::Pending);
  const auto report = StopWorker(stop, gate, workerHandle, worker.value);
  CHECK(!worker.value.joinable());
  CHECK(report.cancelRequested >= 1 && !report.completionPollFailed);
  CHECK(!wrote.load() && error.load() == ERROR_OPERATION_ABORTED);
  CHECK(bytes.front() == 0xA5 && bytes.back() == 0xA5);
  std::cout << "actual blocked synchronous pipe cancelled, requests=" << report.cancelRequested << '\n';
}

void TestActualCompleteTga() {
  struct TempFile {
    std::array<wchar_t, MAX_PATH + 1> path{};
    ~TempFile() { if (path[0]) DeleteFileW(path.data()); }
  } temp;
  std::array<wchar_t, MAX_PATH + 1> directory{};
  const DWORD length = GetTempPathW(DWORD(directory.size()), directory.data());
  CHECK(length && length < directory.size());
  CHECK(GetTempFileNameW(directory.data(), L"wsi", 0, temp.path.data()) != 0);
  Handle file(CreateFileW(temp.path.data(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_TEMPORARY, nullptr)); CHECK(file.valid());
  std::atomic<bool> stop{false}; IoCancellationGate gate;
  const std::array<uint8_t, 32> pixels{
      1,2,3,4, 5,6,7,8, 9,10,11,12, 99,99,99,99,
      13,14,15,16, 17,18,19,20, 21,22,23,24, 98,98,98,98};
  auto phase = gate.enter(stop); CHECK(bool(phase));
  CHECK(EncodeTga(3, 2, pixels.data(), 16, [&](const void* p, size_t bytes) {
    DWORD written = 0;
    return WriteFile(file.value, p, DWORD(bytes), &written, nullptr) && written == bytes;
  }, [&] { return stop.load(); }));
  CHECK(file.close()); phase.finish();
  Handle input(CreateFileW(temp.path.data(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  CHECK(input.valid()); LARGE_INTEGER size{};
  CHECK(GetFileSizeEx(input.value, &size) && size.QuadPart == 42);
  std::array<uint8_t, 42> actual{}; DWORD read = 0;
  CHECK(ReadFile(input.value, actual.data(), DWORD(actual.size()), &read, nullptr) && read == actual.size());
  const auto header = TgaHeader(3, 2);
  CHECK(std::memcmp(actual.data(), header.data(), header.size()) == 0);
  for (unsigned y = 0; y != 2; ++y)
    for (unsigned x = 0; x != 3; ++x) {
      const unsigned dst = 18 + y * 12 + x * 4, src = y * 16 + x * 4;
      CHECK(actual[dst] == pixels[src] && actual[dst+1] == pixels[src+1] &&
          actual[dst+2] == pixels[src+2] && actual[dst+3] == 255);
    }
}

void TestEncoderFailureAndStop() {
  std::array<uint8_t, 4> pixel{1, 2, 3, 4};
  std::atomic<bool> stop{false}; IoCancellationGate gate;
  auto phase = gate.enter(stop);
  DWORD writeError = ERROR_SUCCESS;
  CHECK(!EncodeTga(1, 1, pixel.data(), 4, [&](const void* p, size_t size) {
    DWORD written = 0;
    const BOOL ok = WriteFile(INVALID_HANDLE_VALUE, p, DWORD(size), &written, nullptr);
    writeError = ok ? ERROR_SUCCESS : GetLastError();
    return ok && written == size;
  }, [&] { return stop.load(); }));
  CHECK(writeError == ERROR_INVALID_HANDLE);
  unsigned writes = 0;
  CHECK(!EncodeTga(1, 1, pixel.data(), 4, [&](const void*, size_t) {
    ++writes; stop.store(true); return true;
  }, [&] { return stop.load(); }));
  CHECK(writes == 1); // Stop after the header prevents later pixel writes.
  phase.finish();
  CHECK(!gate.enter(stop));
}

void TestPendingHistoryFailure() {
  struct History { std::atomic<bool> done{false}, success{false}; };
  struct Slot { std::shared_ptr<History> history; State state = State::Submitted; std::shared_ptr<int> buffer; };
  std::array<Slot, 4> slots;
  auto storage = std::make_shared<int>(29);
  for (auto& slot : slots) { slot.history = std::make_shared<History>(); slot.buffer = storage; }
  slots[1].history->done = true; slots[1].history->success = true;
  slots[2].history->done = true;
  slots[3].history.reset();
  FailPendingHistoryAfterJoin(slots);
  CHECK(slots[0].history->done && !slots[0].history->success);
  CHECK(slots[1].history->done && slots[1].history->success);
  CHECK(slots[2].history->done && !slots[2].history->success);
  CHECK(!slots[3].history);
  for (auto& slot : slots) CHECK(slot.state == State::Submitted && slot.buffer == storage && *slot.buffer == 29);
}
}

int main() {
  try {
    TestHandleStartupAndFailure();
    TestScriptedCompletion();
    TestOutsideFilePhase();
    TestCancellationCannotReachLoggerPhase();
    for (unsigned n = 0; n != 3; ++n) TestActualBlockedPipeCancellation(n);
    TestActualCompleteTga();
    TestEncoderFailureAndStop();
    TestPendingHistoryFailure();
    std::cout << "screenshot production cancellation checks=" << checks
        << " failures=0 (Win32 CPU I/O; not physical slow disk/GPU/game)\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "screenshot production cancellation failed: " << e.what() << '\n';
    return 1;
  }
}
