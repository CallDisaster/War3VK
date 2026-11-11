#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

namespace warvk::host::commands {

inline constexpr uint32_t Capacity = 4;
inline constexpr uint32_t MaxBatchBytes = 65536;
static_assert(std::atomic<uint32_t>::is_always_lock_free,
  "The render producer must not fall back to library locks on this target");

struct Scope {
  uint64_t nonceLow = 0, nonceHigh = 0;
  uint64_t mapEpoch = 0, deviceEpoch = 0;

  bool valid() const noexcept {
    return (nonceLow || nonceHigh) && mapEpoch && deviceEpoch;
  }
  bool operator==(const Scope& other) const noexcept {
    return nonceLow == other.nonceLow && nonceHigh == other.nonceHigh
      && mapEpoch == other.mapEpoch && deviceEpoch == other.deviceEpoch;
  }
};

struct Limits {
  // A construction-time range, not a reset/resume API. A new connection must
  // choose a new nonce; changing this range does not make an old scope fresh.
  uint64_t firstSequence = 1;
  uint64_t lastSequence = std::numeric_limits<uint64_t>::max();
  uint32_t maxBatchBytes = MaxBatchBytes;
};

struct Batch {
  Scope scope;
  uint64_t sequence = 0;
  uint32_t bytes = 0;
  // Only [0, bytes) is valid. The tail is not transferred or cleared on dequeue.
  std::array<uint8_t, MaxBatchBytes> payload{};
};

enum class Publish {
  Published, Full, Invalid, Stopped, Closed, Faulted, Exhausted, Unconfigured,
};

struct [[nodiscard]] PublishResult {
  Publish outcome;
  uint64_t sequence;

  // This authorizes only a caller's matching CPU enqueue transaction. It is
  // neither remote acceptance, command execution nor GPU completion. A caller
  // MUST retain/reject its transaction on every other outcome, including Full.
  bool mayCommitLocalState() const noexcept {
    return outcome == Publish::Published && sequence != 0;
  }
};

enum class Dequeue { Item, Empty, AwaitingCpuAck, Finished, Faulted, Unconfigured };
enum class CpuAck { Acknowledged, Invalid, NoPending, Faulted, Unconfigured };

struct Counters {
  uint64_t published = 0, dequeued = 0, cpuAcknowledged = 0;
  uint64_t lastPublished = 0, lastDequeued = 0, lastCpuAck = 0;
  uint32_t queued = 0;
  bool pendingCpuAck = false;
  bool closed = false, stopped = false, faulted = false, exhausted = false;
};

// Process-private SPSC candidate: exactly one fixed producer and one fixed
// consumer for its lifetime. Construct before starting either thread; destroy
// only after both join. Control threads may request stop/fault but cannot reset
// the scope, close the producer, dequeue or acknowledge for another consumer.
//
// No allocation, lock, OS I/O, internal retry or silent command drop. Each call
// performs a bounded number of atomics and at most one <=64 KiB byte copy. This
// is not an interprocess atomic layout, a transport owner or a concrete D3D9
// command format; it must not be substituted for RH1 IPC. uint64_t sequence
// state is thread-owner-private; only lock-free uint32_t atomics are shared.
//
// Stop/fault are irreversible admission decisions, NOT a join or quiescence
// proof. Operations linearized before a concurrent stop/fault may still return.
// Fault invalidates the owner/session; queued bytes are retained for settlement,
// never replayed into a new scope. CPU acknowledgements never retire GPU leases.
class SpscRenderCommandQueue final {
public:
  explicit SpscRenderCommandQueue(Scope scope, Limits limits = {}) noexcept
    : m_scope(scope), m_limits(limits),
      m_configured(scope.valid() && limits.firstSequence != 0
        && limits.firstSequence <= limits.lastSequence
        && limits.maxBatchBytes != 0 && limits.maxBatchBytes <= MaxBatchBytes),
      m_written(static_cast<uint32_t>(limits.firstSequence - 1)),
      m_read(static_cast<uint32_t>(limits.firstSequence - 1)),
      m_nextSequence(limits.firstSequence),
      m_nextExpectedSequence(limits.firstSequence) { }

  SpscRenderCommandQueue(const SpscRenderCommandQueue&) = delete;
  SpscRenderCommandQueue& operator=(const SpscRenderCommandQueue&) = delete;
  SpscRenderCommandQueue(SpscRenderCommandQueue&&) = delete;
  SpscRenderCommandQueue& operator=(SpscRenderCommandQueue&&) = delete;

  // Producer only. Source remains readable/immutable for the duration of this
  // synchronous copy. Source bytes are not referenced after return. Rejections
  // return sequence=0, leave committed bytes and successful sequence unchanged,
  // and do not authorize the caller to commit local D3D9/resource state.
  [[nodiscard]] PublishResult tryPublish(const Scope& scope,
      const uint8_t* source, uint32_t bytes) noexcept {
    if (!m_configured) return {Publish::Unconfigured, 0};
    const auto initialControl = m_control.load(std::memory_order_acquire);
    if (initialControl) return {controlOutcome(initialControl), 0};
    if (!(scope == m_scope) || !source || !bytes || bytes > m_limits.maxBatchBytes)
      return {Publish::Invalid, 0};
    if (m_sequenceExhausted) return {Publish::Exhausted, 0};

    const auto written = m_written.load(std::memory_order_relaxed);
    const auto read = m_read.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(written - read) == Capacity)
      return {Publish::Full, 0};

    uint32_t expected = 0;
    if (!m_control.compare_exchange_strong(expected, PublishingBit,
        std::memory_order_acq_rel, std::memory_order_acquire))
      return {controlOutcome(expected), 0};

    // This is a free, uncommitted slot. A stop/fault during this bounded copy
    // may discard its scratch bytes, never bytes committed to the consumer.
    auto& slot = m_slots[written % Capacity];
    slot.sequence = m_nextSequence;
    slot.bytes = bytes;
    std::memcpy(slot.payload.data(), source, bytes);

    expected = PublishingBit;
    if (!m_control.compare_exchange_strong(expected, 0,
        std::memory_order_acq_rel, std::memory_order_acquire)) {
      m_control.fetch_and(~PublishingBit, std::memory_order_release);
      return {controlOutcome(expected), 0};
    }
    // Successful CAS is the enqueue commit/stop linearization point. A stop
    // immediately afterwards does not retract this transaction. Visibility is
    // published by written.release. Stop alone must never mean "drained".
    const auto sequence = m_nextSequence;
    if (sequence == m_limits.lastSequence) m_sequenceExhausted = true;
    else ++m_nextSequence; // No UINT64_MAX wrap and no failure consumes a number.
    ++m_published;
    m_lastPublished = sequence;
    m_written.store(written + 1, std::memory_order_release);
    return {Publish::Published, sequence};
  }

  // Consumer only. A successful dequeue releases the ring slot and hands out a
  // private copy. Exact CPU acknowledgement is required before another dequeue.
  // No pointer aliases the reusable slot. Every non-Item leaves output intact.
  [[nodiscard]] Dequeue tryDequeue(Batch& output) noexcept {
    if (!m_configured) return Dequeue::Unconfigured;
    auto control = m_control.load(std::memory_order_acquire);
    if (control & FaultBit) return Dequeue::Faulted;
    if (m_pendingCpuAck) return Dequeue::AwaitingCpuAck;

    const auto read = m_read.load(std::memory_order_relaxed);
    auto written = m_written.load(std::memory_order_acquire);
    if (read == written) {
      control = m_control.load(std::memory_order_acquire);
      if (control & FaultBit) return Dequeue::Faulted;
      if (!(control & ClosedBit)) return Dequeue::Empty;
      // closeProducer.release is sequenced AFTER the final tryPublish returns.
      // Acquiring close then rechecking written prevents an early Finished when
      // stop races between publication's successful CAS and written.release.
      written = m_written.load(std::memory_order_acquire);
      if (read == written) return Dequeue::Finished;
    }

    const auto& slot = m_slots[read % Capacity];
    if (m_consumerExhausted || slot.sequence != m_nextExpectedSequence
        || !slot.bytes || slot.bytes > m_limits.maxBatchBytes) {
      requestFault();
      return Dequeue::Faulted;
    }
    output.scope = m_scope;
    output.sequence = slot.sequence;
    output.bytes = slot.bytes;
    std::memcpy(output.payload.data(), slot.payload.data(), slot.bytes);
    m_pendingSequence = slot.sequence;
    m_pendingCpuAck = true;
    if (slot.sequence == m_limits.lastSequence) m_consumerExhausted = true;
    else ++m_nextExpectedSequence;
    ++m_dequeued;
    m_lastDequeued = slot.sequence;
    m_read.store(read + 1, std::memory_order_release);
    return Dequeue::Item;
  }

  // Consumer only, for the currently held private batch, exactly once. This is
  // CPU bookkeeping, not a remote ACK, semaphore/fence or GPU retirement proof.
  [[nodiscard]] CpuAck acknowledgeCpu(const Scope& scope, uint64_t sequence) noexcept {
    if (!m_configured) return CpuAck::Unconfigured;
    if (m_control.load(std::memory_order_acquire) & FaultBit) return CpuAck::Faulted;
    if (!(scope == m_scope) || !sequence) return CpuAck::Invalid;
    if (!m_pendingCpuAck) return CpuAck::NoPending;
    if (sequence != m_pendingSequence) return CpuAck::Invalid;
    m_lastCpuAck = sequence;
    ++m_cpuAcknowledged;
    m_pendingCpuAck = false;
    m_pendingSequence = 0;
    return CpuAck::Acknowledged;
  }

  // Any control/consumer thread. Existing committed batches can still drain.
  // Stop with an empty queue is Empty, not Finished; producer must close/join.
  void requestStop() noexcept {
    m_control.fetch_or(StopBit, std::memory_order_acq_rel);
  }
  // Any control/consumer thread. Never clears pending/committed bytes, revives
  // the scope or grants destruction permission. No later operation is admitted.
  void requestFault() noexcept {
    m_control.fetch_or(FaultBit | StopBit, std::memory_order_acq_rel);
  }
  // Producer only, AFTER its last tryPublish has returned. Close is irreversible
  // and allows a normal drain (including the final exact CPU acknowledgement).
  void closeProducer() noexcept {
    m_control.fetch_or(ClosedBit, std::memory_order_release);
  }

  // Coordinator only AFTER BOTH THREADS JOIN, or while neither has started.
  // This is deliberately not a live snapshot of private uint64_t metadata.
  Counters countersAfterJoin() const noexcept {
    Counters result;
    result.published = m_published;
    result.dequeued = m_dequeued;
    result.cpuAcknowledged = m_cpuAcknowledged;
    result.lastPublished = m_lastPublished;
    result.lastDequeued = m_lastDequeued;
    result.lastCpuAck = m_lastCpuAck;
    result.queued = m_written.load(std::memory_order_relaxed)
      - m_read.load(std::memory_order_relaxed);
    result.pendingCpuAck = m_pendingCpuAck;
    const auto control = m_control.load(std::memory_order_relaxed);
    result.closed = (control & ClosedBit) != 0;
    result.stopped = (control & StopBit) != 0;
    result.faulted = (control & FaultBit) != 0;
    result.exhausted = m_sequenceExhausted;
    return result;
  }

private:
  static constexpr uint32_t StopBit = 1, ClosedBit = 2, FaultBit = 4, PublishingBit = 8;

  static Publish controlOutcome(uint32_t control) noexcept {
    if (control & FaultBit) return Publish::Faulted;
    if (control & ClosedBit) return Publish::Closed;
    if (control & StopBit) return Publish::Stopped;
    // A second producer violates the fixed-owner contract. This is not a
    // multi-producer synchronization scheme or permission to share owner state.
    return Publish::Invalid;
  }

  struct Slot {
    uint64_t sequence = 0;
    uint32_t bytes = 0;
    std::array<uint8_t, MaxBatchBytes> payload{};
  };

  const Scope m_scope;
  const Limits m_limits;
  const bool m_configured;
  std::array<Slot, Capacity> m_slots{};
  alignas(64) std::atomic<uint32_t> m_written;
  alignas(64) std::atomic<uint32_t> m_read;
  std::atomic<uint32_t> m_control{0};
  alignas(64) uint64_t m_nextSequence;
  uint64_t m_published = 0, m_lastPublished = 0;
  bool m_sequenceExhausted = false;
  alignas(64) uint64_t m_nextExpectedSequence;
  uint64_t m_dequeued = 0, m_lastDequeued = 0;
  uint64_t m_cpuAcknowledged = 0, m_lastCpuAck = 0, m_pendingSequence = 0;
  bool m_pendingCpuAck = false, m_consumerExhausted = false;
};

} // namespace warvk::host::commands
