#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>
#include "../../../util/com/com_include.h"

namespace dxvk::war3::tools::screenshot {

// Only the dedicated screenshot worker enters this phase. Cancellation and
// leaving the phase share a gate, but the worker never holds it during I/O.
// In particular, Logger and evidence publication must remain outside the phase.
class IoCancellationGate {
public:
  class FilePhase {
  public:
    FilePhase() = default;
    FilePhase(const FilePhase&) = delete;
    FilePhase& operator=(const FilePhase&) = delete;
    FilePhase(FilePhase&& other) noexcept
      : m_gate(std::exchange(other.m_gate, nullptr)) { }
    ~FilePhase() { finish(); }

    explicit operator bool() const noexcept { return m_gate != nullptr; }

    void finish() {
      if (!m_gate) return;
      std::lock_guard<std::mutex> lock(m_gate->m_mutex);
      m_gate->m_active = false;
      m_gate = nullptr;
    }

  private:
    friend class IoCancellationGate;
    explicit FilePhase(IoCancellationGate* gate) : m_gate(gate) { }
    IoCancellationGate* m_gate = nullptr;
  };

  FilePhase enter(const std::atomic<bool>& stop) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active || stop.load(std::memory_order_acquire)) return {};
    m_active = true;
    return FilePhase(this);
  }

  template<typename Cancel>
  void cancelIfActive(Cancel&& cancel) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active) cancel();
  }

private:
  std::mutex m_mutex;
  bool m_active = false;
};

enum class WorkerWait { Pending, Complete, Failed };
enum class CancelStatus { Requested, NotFound, Failed };
struct CancelResult {
  CancelStatus status;
  uint32_t error = 0;
};
struct ShutdownReport {
  uint64_t cancelAttempts = 0, cancelRequested = 0, cancelNotFound = 0, cancelFailed = 0;
  uint32_t lastCancelError = 0;
  bool completionPollFailed = false;
};

struct WorkerHandlePublication {
  HANDLE handle = nullptr;
  uint32_t error = 0;
};

struct Win32WorkerHandleOps {
  WorkerHandlePublication duplicateCurrentThread() const noexcept {
    HANDLE handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
        &handle, SYNCHRONIZE | THREAD_TERMINATE, FALSE, 0)) return {handle, 0};
    return {nullptr, GetLastError()};
  }
  WorkerWait wait(HANDLE handle, uint32_t milliseconds) const noexcept {
    const DWORD result = WaitForSingleObject(handle, milliseconds);
    if (result == WAIT_OBJECT_0) return WorkerWait::Complete;
    if (result == WAIT_TIMEOUT) return WorkerWait::Pending;
    return WorkerWait::Failed;
  }
  CancelResult cancel(HANDLE handle) const noexcept {
    if (CancelSynchronousIo(handle)) return {CancelStatus::Requested};
    const DWORD error = GetLastError();
    return {error == ERROR_NOT_FOUND ? CancelStatus::NotFound : CancelStatus::Failed, error};
  }
  void close(HANDLE handle) const noexcept { CloseHandle(handle); }
};

// std::thread's native_handle need not be a HANDLE (e.g. MinGW pthread_t).
// The worker duplicates its own pseudo handle and release-publishes the result.
// The owner waits for publication/exit, and closes the real handle only AFTER
// joining std::thread. Ops injection exercises the same startup/failure policy.
template<typename Ops = Win32WorkerHandleOps>
class WorkerHandleControl {
public:
  explicit WorkerHandleControl(Ops ops = {}) : m_ops(std::move(ops)) { }
  WorkerHandleControl(const WorkerHandleControl&) = delete;
  WorkerHandleControl& operator=(const WorkerHandleControl&) = delete;

  class ExitSignal {
  public:
    explicit ExitSignal(WorkerHandleControl& owner) : m_owner(owner) { }
    ExitSignal(const ExitSignal&) = delete;
    ~ExitSignal() { m_owner.markExited(); }
  private:
    WorkerHandleControl& m_owner;
  };

  bool publishCurrentThread() noexcept {
    const auto result = m_ops.duplicateCurrentThread();
    m_publicationError.store(result.error, std::memory_order_relaxed);
    m_handle.store(result.handle, std::memory_order_relaxed);
    m_published.store(true, std::memory_order_release);
    m_publicationWake.notify_all();
    return result.handle != nullptr;
  }

  void markExited() noexcept {
    m_exited.store(true, std::memory_order_release);
    m_publicationWake.notify_all();
  }

  WorkerWait waitForCompletion(uint32_t milliseconds = 25) {
    if (!m_published.load(std::memory_order_acquire) &&
        !m_exited.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(m_publicationMutex);
      // A publication racing the predicate/wait boundary may cost one interval;
      // it cannot turn startup into an unbounded wait or a busy-spin.
      m_publicationWake.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] {
        return m_published.load(std::memory_order_acquire) ||
            m_exited.load(std::memory_order_acquire);
      });
    }
    // The final std::thread join still confirms actual native thread teardown.
    if (m_exited.load(std::memory_order_acquire)) return WorkerWait::Complete;
    if (!m_published.load(std::memory_order_acquire)) return WorkerWait::Pending;
    const HANDLE handle = m_handle.load(std::memory_order_relaxed);
    if (!handle) return WorkerWait::Failed; // Duplication failed: original join.
    return m_ops.wait(handle, milliseconds);
  }

  CancelResult cancelPending() {
    if (!m_published.load(std::memory_order_acquire))
      return {CancelStatus::NotFound, ERROR_NOT_FOUND};
    const HANDLE handle = m_handle.load(std::memory_order_relaxed);
    return handle ? m_ops.cancel(handle)
                  : CancelResult{CancelStatus::Failed, publicationError()};
  }

  uint32_t publicationError() const noexcept {
    return m_publicationError.load(std::memory_order_acquire);
  }

  template<typename Join>
  void joinAndClose(Join&& join) {
    join();
    const HANDLE handle = m_handle.exchange(nullptr, std::memory_order_acq_rel);
    if (handle) m_ops.close(handle);
  }

private:
  // No destructor close: failed join must not masquerade as completed ownership.
  // The screenshot owner always calls joinAndClose before destroying this object.
  Ops m_ops;
  std::atomic<HANDLE> m_handle{nullptr};
  std::atomic<uint32_t> m_publicationError{0};
  std::atomic<bool> m_published{false}, m_exited{false};
  std::mutex m_publicationMutex;
  std::condition_variable m_publicationWake;
};
using Win32WorkerHandle = WorkerHandleControl<>;

// Shared production control flow, with platform operations injected by callers.
// A pending result NEVER releases ownership. Even a successful cancel request
// or ERROR_NOT_FOUND still needs actual completion. A failed poll falls back to
// the existing join, rather than treating an invalid observation as completion.
// There is deliberately no deadline: drivers may refuse cancellation.
template<typename Wake, typename Wait, typename Cancel, typename Join>
ShutdownReport CancelAndJoin(std::atomic<bool>& stop, IoCancellationGate& gate,
    Wake&& wake, Wait&& wait, Cancel&& cancel, Join&& join) {
  stop.store(true, std::memory_order_release);
  wake();
  ShutdownReport report;
  for (;;) {
    const auto result = wait();
    if (result != WorkerWait::Pending) {
      report.completionPollFailed = result == WorkerWait::Failed;
      break;
    }
    gate.cancelIfActive([&] {
      ++report.cancelAttempts;
      const auto result = cancel();
      report.lastCancelError = result.error;
      switch (result.status) {
        case CancelStatus::Requested: ++report.cancelRequested; break;
        case CancelStatus::NotFound: ++report.cancelNotFound; break;
        case CancelStatus::Failed: ++report.cancelFailed; break;
      }
    });
  }
  join();
  return report;
}

// Teardown only, AFTER worker join. Do not alter GPU slot state or references:
// failure to save does not prove completion of a submitted GPU copy.
template<typename Slots>
void FailPendingHistoryAfterJoin(Slots& slots) {
  for (auto& slot : slots) {
    const auto& history = slot.history;
    if (!history || history->done.load(std::memory_order_acquire)) continue;
    history->success.store(false, std::memory_order_relaxed);
    history->done.store(true, std::memory_order_release);
  }
}

} // namespace dxvk::war3::tools::screenshot
