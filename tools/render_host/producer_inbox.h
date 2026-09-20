#pragma once
#include "protocol.h"
#include <atomic>
#include <cstring>

namespace warvk::host::inbox {
inline constexpr uint32_t Capacity = 4;
inline constexpr uint32_t MaxBytes = 65536;
static_assert(std::atomic<uint32_t>::is_always_lock_free,
  "The producer must not fall back to library locks on this target");

struct Scope {
  ipc::Nonce connection;
  uint64_t map = 0, device = 0;
  bool valid() const noexcept { return !connection.empty() && map && device; }
  bool operator==(const Scope& other) const noexcept {
    return connection == other.connection && map == other.map && device == other.device;
  }
};
struct Sample {
  Scope scope;
  uint64_t frame = 0;
  uint32_t ordinal = 0, bytes = 0;
  std::array<uint8_t, MaxBytes> payload{};
};
enum class Push { Published, Full, Invalid, Stopped, Closed, Exhausted, Unconfigured };
struct Result { Push outcome; uint32_t ordinal; };
enum class Pop { Item, Empty, Finished };
struct Counters {
  uint32_t attempted = 0, published = 0, full = 0, invalid = 0;
  uint32_t popped = 0, queued = 0;
  bool closed = false, stopped = false, exhausted = false;
};
struct Limits {
  uint32_t attempts = UINT32_MAX;
  uint32_t sampleBytes = MaxBytes;
};

// Process-private SPSC: one fixed producer and one fixed consumer for its entire
// lifetime. Construct before starting threads; destroy only after both join.
// This is NOT a cross-process atomic layout, a GPU lease, or a realtime promise.
class ProducerInbox final {
public:
  explicit ProducerInbox(Scope scope, Limits limits = {}) noexcept
    : m_scope(scope), m_limits(limits) { }
  ProducerInbox(const ProducerInbox&) = delete;
  ProducerInbox& operator=(const ProducerInbox&) = delete;
  ProducerInbox(ProducerInbox&&) = delete;
  ProducerInbox& operator=(ProducerInbox&&) = delete;

  // Producer only. No waits, allocation, retry loop, hashing or OS calls.
  // Source must be readable/immutable throughout this bounded synchronous copy.
  Result tryPush(uint64_t frame, const uint8_t* source, uint32_t bytes) noexcept {
    if (!m_scope.valid() || !m_limits.sampleBytes || m_limits.sampleBytes > MaxBytes)
      return {Push::Unconfigured, 0};
    if (m_closed.load(std::memory_order_relaxed)) return {Push::Closed, 0};
    if (m_counters.exhausted) return {Push::Exhausted, 0};
    if (m_stop.load(std::memory_order_acquire)) return {Push::Stopped, 0};
    if (m_counters.attempted == m_limits.attempts) {
      m_counters.exhausted = true;
      requestStop();
      return {Push::Exhausted, 0};
    }
    const auto ordinal = ++m_counters.attempted;
    if (!frame || frame < m_lastFrame || !source || !bytes || bytes > m_limits.sampleBytes) {
      ++m_counters.invalid;
      return {Push::Invalid, ordinal};
    }
    m_lastFrame = frame;
    const auto written = m_written.load(std::memory_order_relaxed);
    const auto read = m_read.load(std::memory_order_acquire);
    if (written - read == Capacity) {
      ++m_counters.full;
      return {Push::Full, ordinal};
    }
    auto& slot = m_slots[written % Capacity];
    slot.frame = frame; slot.ordinal = ordinal; slot.bytes = bytes;
    std::memcpy(slot.payload.data(), source, bytes);
    // No wrap: published <= attempted <= m_limits.attempts <= UINT32_MAX.
    m_written.store(written + 1, std::memory_order_release);
    ++m_counters.published;
    return {Push::Published, ordinal};
  }

  // Consumer only. Returns a private copy, never an alias into a reusable slot.
  // Empty/Finished leave output unchanged. A pop is not a host ACK.
  Pop tryPop(Sample& output) noexcept {
    const auto read = m_read.load(std::memory_order_relaxed);
    auto written = m_written.load(std::memory_order_acquire);
    if (read == written) {
      if (!m_closed.load(std::memory_order_acquire)) return Pop::Empty;
      // The close release follows the producer's last publication. Recheck it
      // after acquiring close before claiming the queue has actually drained.
      written = m_written.load(std::memory_order_acquire);
      if (read == written) return Pop::Finished;
    }
    const auto& slot = m_slots[read % Capacity];
    output.scope = m_scope;
    output.frame = slot.frame; output.ordinal = slot.ordinal; output.bytes = slot.bytes;
    std::memcpy(output.payload.data(), slot.payload.data(), slot.bytes);
    m_read.store(read + 1, std::memory_order_release);
    return Pop::Item;
  }

  // Any control/consumer thread. An already admitted push may still publish;
  // this is neither a join nor a destruction permission.
  void requestStop() noexcept { m_stop.store(1, std::memory_order_release); }
  // Producer only, after its last push. Coordinator still has to join it.
  void closeProducer() noexcept { m_closed.store(1, std::memory_order_release); }
  // Coordinator only AFTER BOTH THREADS JOIN. Not a live concurrent snapshot.
  Counters countersAfterJoin() const noexcept {
    auto result = m_counters;
    result.popped = m_read.load(std::memory_order_relaxed);
    result.queued = m_written.load(std::memory_order_relaxed) - result.popped;
    result.closed = m_closed.load(std::memory_order_relaxed) != 0;
    result.stopped = m_stop.load(std::memory_order_relaxed) != 0;
    return result;
  }
private:
  struct Slot {
    uint64_t frame = 0;
    uint32_t ordinal = 0, bytes = 0;
    std::array<uint8_t, MaxBytes> payload{};
  };
  const Scope m_scope;
  const Limits m_limits;
  std::array<Slot, Capacity> m_slots{};
  alignas(64) std::atomic<uint32_t> m_written{0};
  alignas(64) std::atomic<uint32_t> m_read{0};
  std::atomic<uint32_t> m_stop{0}, m_closed{0};
  // Keep frequently-written producer metadata away from the consumer index.
  alignas(64) uint64_t m_lastFrame = 0;
  Counters m_counters; // producer-private until both threads join
};
}
