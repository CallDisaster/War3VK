#pragma once

#include <atomic>
#include <cstdint>

namespace dxvk {
namespace war3dbg {

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "first-chance counters must remain signal-safe scalar atomics");
static_assert(std::atomic<uintptr_t>::is_always_lock_free,
              "first-chance addresses must remain signal-safe scalar atomics");
static_assert(std::atomic<bool>::is_always_lock_free,
              "fatal capture gate must remain a lock-free scalar atomic");

struct War3FirstChanceSnapshot {
  uint32_t count = 0;
  uint32_t exceptionCode = 0;
  uintptr_t exceptionAddress = 0;
  uint32_t threadId = 0;
};

// Keeps first-chance observation independent from the one-shot fatal dump gate.
// The first-chance path performs only lock-free scalar stores: no allocation,
// logging, file I/O, module enumeration or DbgHelp calls are permitted here.
class War3CrashCaptureCoordinator {
public:
  void recordFirstChance(uint32_t exceptionCode,
                         uintptr_t exceptionAddress,
                         uint32_t threadId) noexcept {
    m_lastFirstChanceCode.store(exceptionCode, std::memory_order_relaxed);
    m_lastFirstChanceAddress.store(exceptionAddress, std::memory_order_relaxed);
    m_lastFirstChanceThreadId.store(threadId, std::memory_order_relaxed);
    m_firstChanceCount.fetch_add(1u, std::memory_order_release);
  }

  War3FirstChanceSnapshot firstChanceSnapshot() const noexcept {
    War3FirstChanceSnapshot result;
    result.count = m_firstChanceCount.load(std::memory_order_acquire);
    result.exceptionCode = m_lastFirstChanceCode.load(std::memory_order_relaxed);
    result.exceptionAddress = m_lastFirstChanceAddress.load(std::memory_order_relaxed);
    result.threadId = m_lastFirstChanceThreadId.load(std::memory_order_relaxed);
    return result;
  }

  bool tryBeginFatalCapture() noexcept {
    bool expected = false;
    return m_fatalCaptureStarted.compare_exchange_strong(
      expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
  }

private:
  std::atomic<uint32_t> m_firstChanceCount { 0u };
  std::atomic<uint32_t> m_lastFirstChanceCode { 0u };
  std::atomic<uintptr_t> m_lastFirstChanceAddress { 0u };
  std::atomic<uint32_t> m_lastFirstChanceThreadId { 0u };
  std::atomic<bool> m_fatalCaptureStarted { false };
};

} // namespace war3dbg
} // namespace dxvk
