#pragma once

#include "war3_frame_history_core.h"
#include <atomic>
#include <cstdint>

namespace dxvk::war3::tools::history {
// Shared by the production worker and the native policy test. No GPU refs, I/O,
// callbacks or implicit retries: one owner, one arm, one retained incident.
class RecorderSession {
public:
  enum class Action { None, Arm, Export, Stop };
  Action next(bool ready, CaptureLifecycle::State imageState, bool cpuFrozen, bool stopping) noexcept {
    if (stopping || failed || exported) return Action::Stop;
    if (!attempted) {
      if (!ready) return Action::None;
      attempted = true;
      return Action::Arm;
    }
    if (!token || imageState == CaptureLifecycle::Fault) { failed = true; return Action::Stop; }
    if (imageState == CaptureLifecycle::Complete && cpuFrozen && !exportAttempted) {
      exportAttempted = true;
      return Action::Export;
    }
    return Action::None;
  }
  void armed(uint64_t value) noexcept { token = value; failed = !value; }
  void finished(bool ok) noexcept { exported = ok; failed = !ok; }
  uint64_t session() const noexcept { return token; }
  bool resetEndsSession(bool reset) const noexcept { return reset && token!=0; }
private:
  uint64_t token = 0;
  bool attempted = false, exportAttempted = false, exported = false, failed = false;
};

// A hotkey never takes a render/control mutex. Requests carry the arm generation
// and are consumed only by that generation, never by a later recording.
class TriggerMailbox {
public:
  void arm(uint64_t session) noexcept { pending.store(0); active.store(session, std::memory_order_release); }
  void disarm() noexcept { active.store(0, std::memory_order_release); }
  bool request() noexcept {
    auto session = active.load(std::memory_order_acquire);
    if (!session) return false;
    auto previous=pending.load(std::memory_order_acquire);
    // Generations are monotonic. A preempted old hotkey must not replace a
    // request already published for a newer arm.
    while(previous<session&&!pending.compare_exchange_weak(previous,session,std::memory_order_acq_rel)){}
    return active.load(std::memory_order_acquire)==session;
  }
  bool consume(uint64_t session) noexcept {
    // A delayed old owner must not clear another generation's pending hotkey.
    // Failure leaves the mailbox intact, including a newer request.
    return session && pending.compare_exchange_strong(session,0,std::memory_order_acq_rel);
  }
private:
  std::atomic<uint64_t> active{0}, pending{0};
};
}
