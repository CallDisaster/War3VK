#pragma once
#include "recorder_event_wire.h"
#include <array>
#include <atomic>
#include <type_traits>

namespace warvk::host::recorder {
enum class Push { Published, Full, Contended, Closed, Stale, Exhausted };
enum class Pop { Item, Empty, Finished };
struct PushResult { Push outcome; uint64_t sequence = 0; };
struct IngressCounters {
  uint64_t attempted = 0, accepted = 0, lost = 0, popped = 0, queued = 0;
  bool closed = false;
};

// Process-private bounded MPSC ingress. The same immutable session lives for
// this object's entire lifetime. One consumer; many producers. Stop is not a
// join or destruction permit. The owner must join ALL callers before deletion.
// Cell stamps publish/release ownership, never a pointer into reusable storage.
template<uint32_t Capacity = 8192>
class EventIngress final {
  static_assert(Capacity >= 2 && Capacity <= 8192);
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
    "Do not silently use library locks in a render producer");
  static_assert(std::is_trivially_copyable_v<Event>);
public:
  // UINT32_MAX bounds attempts; 64-bit counters leave headroom for concurrent
  // callers already admitted when the attempt limit closes the gate.
  explicit EventIngress(uint64_t session, uint32_t attempts = UINT32_MAX) noexcept
    : m_session(session), m_attemptLimit(attempts) {
    for (uint32_t i = 0; i < Capacity; ++i) m_slots[i].stamp.store(i);
    if (!session || !attempts) m_closed.store(true);
  }
  EventIngress(const EventIngress&) = delete;
  EventIngress& operator=(const EventIngress&) = delete;

  PushResult tryPush(uint64_t session, Event event) noexcept {
    m_writers.fetch_add(1); // seq_cst with close and final writer drain
    struct Exit { std::atomic<uint64_t>& writers; ~Exit() { writers.fetch_sub(1); } } exit{m_writers};
    if (!session || session != m_session) return {Push::Stale};
    if (m_closed.load()) return {Push::Closed};
    const auto attempt = m_attempted.fetch_add(1, std::memory_order_relaxed);
    if (attempt >= m_attemptLimit) {
      m_closed.store(true);
      return lose(Push::Exhausted);
    }
    uint64_t position = m_claimed.load(std::memory_order_relaxed);
    for (uint32_t retry = 0; retry < 8; ++retry) {
      auto& slot = m_slots[position % Capacity];
      const auto stamp = slot.stamp.load(std::memory_order_acquire);
      if (stamp == position) {
        if (m_claimed.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
          event.sequence = position + 1;
          event.session = session;
          slot.event = event;
          slot.stamp.store(position + 1, std::memory_order_release);
          return {Push::Published, position + 1};
        }
      } else if (stamp < position) {
        return lose(Push::Full);
      } else {
        position = m_claimed.load(std::memory_order_relaxed);
      }
    }
    return lose(Push::Contended); // Never wait for another producer/consumer.
  }

  // Consumer only; output unchanged unless Item. A preempted producer may
  // leave the next cell unpublished: Empty, not overwrite/skip that record.
  Pop tryPop(Event& output) noexcept {
    auto& slot = m_slots[m_read % Capacity];
    if (slot.stamp.load(std::memory_order_acquire) != m_read + 1) {
      if (m_closed.load() && m_writers.load() == 0 &&
          m_claimed.load(std::memory_order_acquire) == m_read)
        return Pop::Finished;
      return Pop::Empty;
    }
    output = slot.event;
    slot.stamp.store(m_read + Capacity, std::memory_order_release);
    ++m_read;
    return Pop::Item;
  }
  void close() noexcept { m_closed.store(true); }
  // Includes claimed-but-unpublished entries. Control must retain this cut and
  // await actual writer/queue drain before sealing or destroying the session.
  uint64_t publicationCut() const noexcept { return m_claimed.load(); }
  // Coordinator only after every producer and the single consumer have joined.
  IngressCounters countersAfterJoin() const noexcept {
    const auto accepted = m_claimed.load(std::memory_order_relaxed);
    return {m_attempted.load(std::memory_order_relaxed), accepted,
      m_lost.load(std::memory_order_relaxed), m_read, accepted - m_read, m_closed.load()};
  }
private:
  PushResult lose(Push reason) noexcept {
    m_lost.fetch_add(1, std::memory_order_relaxed);
    return {reason};
  }
  struct alignas(64) Cell { std::atomic<uint64_t> stamp{0}; Event event{}; };
  const uint64_t m_session;
  const uint32_t m_attemptLimit;
  std::array<Cell, Capacity> m_slots{};
  alignas(64) std::atomic<uint64_t> m_claimed{0};
  alignas(64) uint64_t m_read = 0;
  alignas(64) std::atomic<uint64_t> m_writers{0}, m_attempted{0}, m_lost{0};
  std::atomic<bool> m_closed{false};
};
}
