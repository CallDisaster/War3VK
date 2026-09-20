#pragma once
#include <atomic>
#include <cstdint>

namespace dxvk::war3::hooks {
// A successful Present, not the profiler's recording flag, establishes owner.
// A second swapchain/thread or destruction permanently revokes this lease.
// Reset/failed Present temporarily revoke it until the SAME owner succeeds.
class NativeFrameSyncOwner {
  std::atomic<uintptr_t> chain{0};
  std::atomic<uint32_t> thread{0};
  std::atomic<bool> ready{false}, fault{false};
public:
  void begin(uintptr_t p, uint32_t tid) noexcept {
    ready.store(false, std::memory_order_release);
    uintptr_t expected = 0;
    chain.compare_exchange_strong(expected, p);
    uint32_t expectedThread = 0;
    thread.compare_exchange_strong(expectedThread, tid);
    if (!p || !tid || chain.load() != p || thread.load() != tid)
      fault.store(true, std::memory_order_release);
  }
  void complete(uintptr_t p, uint32_t tid) noexcept {
    if (!fault.load(std::memory_order_acquire) && p && tid &&
        chain.load() == p && thread.load() == tid)
      ready.store(true, std::memory_order_release);
  }
  void revoke(bool permanent) noexcept {
    ready.store(false, std::memory_order_release);
    if (permanent) fault.store(true, std::memory_order_release);
  }
  bool owns(uint32_t tid, uint32_t worldThread) const noexcept {
    return tid && tid == worldThread && ready.load(std::memory_order_acquire) &&
        !fault.load(std::memory_order_acquire) && thread.load() == tid;
  }
};
}
