#pragma once
#include "d3d9_wire.h"
#include <atomic>

// D9C2 command ring: the shared-memory transport that replaces one pipe round
// trip per message (docs/plan/2026-11-13-x64-phase1-transport-design.md).
//
// Two regions, each written by exactly one process and mapped read-only by the
// other:
//   producer region  x86 writes   ProducerBlock, then the byte ring
//   consumer region  host writes  ConsumerBlock, then the reply slot
//
// Nothing read from the peer's region is trusted. Every peer field is checked
// when read, each shared byte is fetched once into private memory before it is
// validated, and an owner never reads its own shared fields back. Any broken
// rule is terminal for the session: no retry, resynchronisation or skipping.
//
// This header is the portable core (layout, framing, validation). Mapping the
// regions and waiting on events live in d3d9_ring_channel.h.
namespace warvk::host::d9::ring {
// 2 (2026-11-18): `waiting` says what the producer waits for (2: space), the
// consumer block counts refused scene records at offset 140, and an
// asynchronous Present may be skipped without ending the session. The two
// ends are built together; the number makes a mismatched pair fail at attach
// instead of at the first record the older end reads differently.
constexpr uint32_t Version = 2;
constexpr uint32_t RecordHeaderBytes = 16, RecordAlign = 16, FlagSync = 1;
// Region layout: one control page, then the ring bytes / the reply slot.
constexpr uint32_t ControlBytes = 4096, ReplyCapacity = MaxBytes;
constexpr uint32_t MinCapacity = 262144, MaxCapacity = 67108864, DefaultCapacity = 4194304;
constexpr uint32_t MaxRecordBytes = RecordHeaderBytes + MaxBytes;
static_assert(MaxRecordBytes * 2 <= MinCapacity, "the smallest ring holds two largest records");
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free,
  "shared atomics must be address-free in both processes");

inline bool ValidCapacity(uint32_t capacity) {
  return capacity >= MinCapacity && capacity <= MaxCapacity && !(capacity & (capacity - 1));
}
inline uint32_t RecordBytes(uint32_t message) {
  return RecordHeaderBytes + ((message + RecordAlign - 1) & ~(RecordAlign - 1));
}

// Explicit padding: both ABIs place every member at the same offset.
struct ProducerBlock {
  uint8_t magic[8];                  //   0  "WVKRNGP1"
  uint32_t version, capacity;        //   8
  uint64_t nonceLow, nonceHigh;      //  16
  uint8_t reserved0[32];             //  32
  std::atomic<uint64_t> tail;        //  64  bytes published; whole records only
  std::atomic<uint32_t> waiting;     //  72  producer is about to sleep on `progress`
  std::atomic<uint32_t> closed;      //  76  producer publishes nothing more
  uint8_t reserved1[48];             //  80
};
struct ConsumerBlock {
  uint8_t magic[8];                  //   0  "WVKRNGC1"
  uint32_t version, replyCapacity;   //   8
  uint64_t nonceLow, nonceHigh;      //  16
  uint8_t reserved0[32];             //  32
  // Written for every record: one cache line.
  std::atomic<uint64_t> head;        //  64  bytes copied out of the ring
  std::atomic<uint64_t> applied;     //  72  sequence of the last executed record
  std::atomic<uint64_t> gpuSubmitted, gpuCompleted, gpuRetired, draws; // 80
  std::atomic<uint32_t> parked;      // 112  consumer is about to sleep on `work`
  uint8_t reserved1[12];             // 116
  // Written once, read by x86 on every call: its own line.
  std::atomic<uint32_t> state;       // 128  State
  std::atomic<uint32_t> faultCode;   // 132  Fault
  std::atomic<uint32_t> faultResult; // 136  HRESULT of a refused record
  // Not a fault: asynchronous scene records (FrameScene / StageMarker) the host
  // refused. From the first one on it ignores every later scene record.
  std::atomic<uint32_t> sceneRefused; // 140
  std::atomic<uint64_t> faultSequence; // 144
  uint8_t reserved3[40];             // 152
};
static_assert(sizeof(ProducerBlock) == 128 && sizeof(ConsumerBlock) == 192, "ring control layout");
static_assert(sizeof(ProducerBlock) <= ControlBytes && sizeof(ConsumerBlock) <= ControlBytes, "control page");

enum class State : uint32_t { Running = 0, Finished = 1, Faulted = 2 };
// What broke. Host-detected codes are published in ConsumerBlock::faultCode;
// the x86 side raises the last three itself.
enum class Fault : uint32_t {
  None = 0,
  Layout,          // region header: magic, version, nonce or capacity
  TailInvalid,     // tail went backwards or beyond head + capacity
  Truncated,       // published bytes do not hold one whole record
  RecordHeader,    // record length or flags out of range
  Checksum,        // record header / message header check mismatch
  MessageHeader,   // D9C2 header refused by DecodeHeader
  Scope,           // message nonce is not this session's
  Sequence,        // not exactly the next sequence, or sequenceLow disagrees
  AsyncRefused,    // an asynchronous record did not return D3D_OK
  DeviceLost,      // a record returned D3DERR_DEVICELOST; the host is leaving
  HeadInvalid,     // x86: head went backwards or beyond tail
  AppliedInvalid,  // x86: applied went backwards or beyond the published sequence
  ReplyInvalid,    // x86: reply header identity or length
};
inline const char* FaultName(Fault fault) {
  switch (fault) {
    case Fault::None: return "none";
    case Fault::Layout: return "layout";
    case Fault::TailInvalid: return "tail-invalid";
    case Fault::Truncated: return "truncated";
    case Fault::RecordHeader: return "record-header";
    case Fault::Checksum: return "checksum";
    case Fault::MessageHeader: return "message-header";
    case Fault::Scope: return "scope";
    case Fault::Sequence: return "sequence";
    case Fault::AsyncRefused: return "async-refused";
    case Fault::DeviceLost: return "device-lost";
    case Fault::HeadInvalid: return "head-invalid";
    case Fault::AppliedInvalid: return "applied-invalid";
    case Fault::ReplyInvalid: return "reply-invalid";
  }
  return "unknown";
}

// Word-wise mix over the framing bytes. It finds a misplaced or damaged record
// header; it is not a payload checksum and not a security boundary.
inline uint32_t Mix(uint32_t state, const uint8_t* bytes, uint32_t count) {
  for (uint32_t i = 0; i + 4 <= count; i += 4) {
    state ^= U32(bytes + i);
    state *= 0x85EBCA6Bu;
    state = (state << 13) | (state >> 19);
  }
  return state;
}
inline uint32_t Check(const uint8_t* recordHeader12, const uint8_t* messageHeader) {
  uint32_t state = Mix(0x9E3779B9u, recordHeader12, 12);
  state = Mix(state, messageHeader, HeaderBytes);
  state ^= state >> 16; state *= 0xC2B2AE35u; state ^= state >> 13;
  return state;
}

inline void InitializeProducerBlock(ProducerBlock& block, ipc::Nonce nonce, uint32_t capacity) {
  std::memcpy(block.magic, "WVKRNGP1", 8);
  block.version = Version; block.capacity = capacity;
  block.nonceLow = nonce.low; block.nonceHigh = nonce.high;
}
inline void InitializeConsumerBlock(ConsumerBlock& block, ipc::Nonce nonce) {
  std::memcpy(block.magic, "WVKRNGC1", 8);
  block.version = Version; block.replyCapacity = ReplyCapacity;
  block.nonceLow = nonce.low; block.nonceHigh = nonce.high;
}
// One-time attach checks. The 32 constant bytes are fetched once into private
// memory; afterwards each side uses only its own copies of these values.
struct Constants { uint8_t magic[8]; uint32_t version, size; uint64_t nonceLow, nonceHigh; };
static_assert(sizeof(Constants) == 32, "constant prefix of both blocks");
inline bool ValidConstants(const void* block, const char* magic, uint32_t size, ipc::Nonce nonce) {
  Constants c; std::memcpy(&c, block, sizeof(c));
  return !std::memcmp(c.magic, magic, 8) && c.version == Version && c.size == size
    && c.nonceLow == nonce.low && c.nonceHigh == nonce.high && !nonce.empty();
}
// Host, at attach: the x86 region is this session's and has published nothing.
inline bool ValidProducerBlock(const ProducerBlock& block, ipc::Nonce nonce, uint32_t capacity) {
  return ValidCapacity(capacity) && ValidConstants(&block, "WVKRNGP1", capacity, nonce)
    && !block.tail.load(std::memory_order_seq_cst) && !block.closed.load(std::memory_order_seq_cst);
}
// x86, after the attach reply: the host's region is this session's, running,
// has consumed nothing and reports the attach message as executed.
inline bool ValidConsumerBlock(const ConsumerBlock& block, ipc::Nonce nonce, uint64_t attachSequence) {
  return ValidConstants(&block, "WVKRNGC1", ReplyCapacity, nonce)
    && !block.head.load(std::memory_order_seq_cst) && block.applied.load(std::memory_order_seq_cst) == attachSequence
    && block.state.load(std::memory_order_seq_cst) == uint32_t(State::Running);
}

// x86 side. One thread; `data` is this process's writable ring, `peer`/`reply`
// are the host's region mapped read-only.
class Producer {
public:
  enum class Write { Written, Full, Faulted };
  struct Progress { uint64_t head = 0, applied = 0; };

  Producer(ProducerBlock* own, uint8_t* data, const ConsumerBlock* peer, const uint8_t* reply,
           uint32_t capacity, uint64_t lastSequence) noexcept
    : m_own(own), m_data(data), m_peer(peer), m_reply(reply), m_capacity(capacity),
      m_published(lastSequence), m_applied(lastSequence) { }

  // `header` is the encoded 128-byte D9C2 header for `sequence`, which must be
  // the next one. Full leaves everything unchanged; the caller waits and
  // retries the same bytes.
  Write tryWrite(const uint8_t* header, const void* payload, uint32_t payloadBytes,
                 uint64_t sequence, bool sync) noexcept {
    if (m_fault != Fault::None) return Write::Faulted;
    if (payloadBytes > MaxPayload || (payloadBytes && !payload) || sequence != m_published + 1)
      return fail(Fault::Sequence);
    const uint32_t message = HeaderBytes + payloadBytes, total = RecordBytes(message);
    if (m_capacity - (m_tail - m_head) < total) {
      if (!refreshHead()) return Write::Faulted;
      if (m_capacity - (m_tail - m_head) < total) return Write::Full;
    }
    uint8_t record[RecordHeaderBytes];
    P32(record, message); P32(record + 4, sync ? FlagSync : 0); P32(record + 8, uint32_t(sequence));
    P32(record + 12, Check(record, header));
    put(m_tail, record, RecordHeaderBytes);
    put(m_tail + RecordHeaderBytes, header, HeaderBytes);
    if (payloadBytes) put(m_tail + RecordHeaderBytes + HeaderBytes, payload, payloadBytes);
    m_tail += total; m_published = sequence;
    // Sequentially consistent with the consumer's parked store: it either sees
    // this tail or we see its parked flag.
    m_own->tail.store(m_tail, std::memory_order_seq_cst);
    return Write::Written;
  }
  bool consumerParked() const noexcept { return m_peer->parked.load(std::memory_order_seq_cst) != 0; }
  // 1: waiting for a record to be applied. 2: waiting for ring space, which the
  // host returns when it reads a record, before it applies it.
  void setWaiting(bool waiting, bool space = false) noexcept {
    m_own->waiting.store(waiting ? (space ? 2u : 1u) : 0u, std::memory_order_seq_cst);
  }
  void close() noexcept { m_own->closed.store(1, std::memory_order_seq_cst); }

  // The host's validated progress. False once anything it published is invalid.
  bool observe(Progress& progress) noexcept {
    if (m_fault != Fault::None || !refreshHead()) return false;
    const auto applied = m_peer->applied.load(std::memory_order_seq_cst);
    if (applied < m_applied || applied > m_published) { fail(Fault::AppliedInvalid); return false; }
    m_applied = applied;
    progress = {m_head, m_applied};
    return true;
  }
  // After observe() reported applied == h.sequence for a synchronous record.
  // `payload` receives r.payloadBytes bytes and must hold MaxReplyPayload.
  bool readReply(const Header& h, Reply& r, uint8_t* payload) noexcept {
    if (m_fault != Fault::None) return false;
    uint8_t header[ReplyBytes];
    std::memcpy(header, m_reply, ReplyBytes);
    if (!DecodeReply(h, header, r) || r.payloadBytes > MaxReplyPayload) { fail(Fault::ReplyInvalid); return false; }
    if (r.payloadBytes) std::memcpy(payload, m_reply + ReplyBytes, r.payloadBytes);
    return true;
  }
  struct HostFault { State state = State::Running; Fault code = Fault::None; uint32_t result = 0; uint64_t sequence = 0; };
  HostFault hostState() const noexcept {
    HostFault result;
    const auto state = m_peer->state.load(std::memory_order_acquire);
    result.state = state <= uint32_t(State::Faulted) ? State(state) : State::Faulted;
    result.code = Fault(m_peer->faultCode.load(std::memory_order_acquire));
    result.result = m_peer->faultResult.load(std::memory_order_acquire);
    result.sequence = m_peer->faultSequence.load(std::memory_order_acquire);
    return result;
  }
  uint64_t gpuCompleted() const noexcept { return m_peer->gpuCompleted.load(std::memory_order_acquire); }
  bool sceneRefused() const noexcept { return m_peer->sceneRefused.load(std::memory_order_acquire) != 0; }
  // Measurement only (lost-wakeup profile): unvalidated peeks that change no
  // state; observe() stays the only accepted reading of the host's progress.
  uint64_t peekApplied() const noexcept { return m_peer->applied.load(std::memory_order_seq_cst); }
  uint64_t peekHead() const noexcept { return m_peer->head.load(std::memory_order_seq_cst); }
  Fault fault() const noexcept { return m_fault; }
  uint64_t published() const noexcept { return m_published; }
  uint64_t tail() const noexcept { return m_tail; }

private:
  Write fail(Fault fault) noexcept { if (m_fault == Fault::None) m_fault = fault; return Write::Faulted; }
  bool refreshHead() noexcept {
    const auto head = m_peer->head.load(std::memory_order_seq_cst);
    if (head < m_head || head > m_tail) { fail(Fault::HeadInvalid); return false; }
    m_head = head;
    return true;
  }
  void put(uint64_t position, const void* source, uint32_t bytes) noexcept {
    const uint32_t offset = uint32_t(position) & (m_capacity - 1), first = std::min(bytes, m_capacity - offset);
    std::memcpy(m_data + offset, source, first);
    if (first != bytes) std::memcpy(m_data, static_cast<const uint8_t*>(source) + first, bytes - first);
  }
  ProducerBlock* m_own; uint8_t* m_data; const ConsumerBlock* m_peer; const uint8_t* m_reply;
  uint32_t m_capacity;
  uint64_t m_tail = 0, m_head = 0, m_published, m_applied;
  Fault m_fault = Fault::None;
};

// Host side. One thread; `data`/`peer` are the x86 region mapped read-only.
class Consumer {
public:
  enum class Read { Item, Empty, Closed, Faulted };

  Consumer(const ProducerBlock* peer, const uint8_t* data, ConsumerBlock* own, uint8_t* reply,
           uint32_t capacity, ipc::Nonce nonce, uint64_t lastSequence) noexcept
    : m_peer(peer), m_data(data), m_own(own), m_reply(reply), m_capacity(capacity), m_nonce(nonce),
      m_expected(lastSequence + 1) {
    // Everything up to the attach message is executed; the producer checks
    // that applied never falls below it.
    m_own->applied.store(lastSequence, std::memory_order_seq_cst);
  }

  // Item: `packet` holds a private copy of one fully validated message, `h` is
  // its header, and the ring space is already returned to the producer.
  Read tryRead(Packet& packet, Header& h, bool& sync) noexcept {
    if (m_fault != Fault::None) return Read::Faulted;
    // Read `closed` first: a close published after this load is seen next time.
    const bool closed = m_peer->closed.load(std::memory_order_seq_cst) != 0;
    const auto tail = m_peer->tail.load(std::memory_order_seq_cst);
    if (tail < m_tail || tail - m_head > m_capacity) return fail(Fault::TailInvalid);
    m_tail = tail;
    const uint64_t available = m_tail - m_head;
    if (!available) return closed ? Read::Closed : Read::Empty;
    if (available < RecordHeaderBytes) return fail(Fault::Truncated);
    uint8_t record[RecordHeaderBytes];
    get(m_head, record, RecordHeaderBytes);
    const uint32_t message = U32(record), flags = U32(record + 4);
    if (message < HeaderBytes || message > MaxBytes || (flags & ~FlagSync)) return fail(Fault::RecordHeader);
    const uint32_t total = RecordBytes(message);
    if (total > available) return fail(Fault::Truncated);
    get(m_head + RecordHeaderBytes, packet.data(), message);
    if (Check(record, packet.data()) != U32(record + 12)) return fail(Fault::Checksum);
    if (!DecodeHeader(packet.data(), h) || HeaderBytes + h.payloadBytes != message) return fail(Fault::MessageHeader);
    if (!(h.nonce == m_nonce)) return fail(Fault::Scope);
    if (h.sequence != m_expected || uint32_t(h.sequence) != U32(record + 8) || m_expected == UINT64_MAX)
      return fail(Fault::Sequence);
    ++m_expected; m_head += total; sync = flags == FlagSync;
    m_own->head.store(m_head, std::memory_order_seq_cst);
    return Read::Item;
  }
  bool producerWaiting() const noexcept { return m_peer->waiting.load(std::memory_order_seq_cst) != 0; }
  // After tryRead published `head` (sequentially consistent with the producer's
  // store, as above): the producer sleeps for the space that read returned.
  bool producerWaitingSpace() const noexcept { return m_peer->waiting.load(std::memory_order_seq_cst) == 2; }
  void setParked(bool parked) noexcept { m_own->parked.store(parked ? 1 : 0, std::memory_order_seq_cst); }

  // Before publishApplied of the same synchronous record.
  void writeReply(const Header& h, const Reply& r, const uint8_t* payload) noexcept {
    Packet& packet = *reinterpret_cast<Packet*>(m_reply);
    if (r.payloadBytes) std::memmove(m_reply + ReplyBytes, payload, r.payloadBytes);
    EncodeReply(h, r, packet);
  }
  void publishApplied(uint64_t sequence, uint64_t submitted, uint64_t completed, uint64_t retired, uint64_t draws) noexcept {
    m_own->gpuSubmitted.store(submitted, std::memory_order_release);
    m_own->gpuCompleted.store(completed, std::memory_order_release);
    m_own->gpuRetired.store(retired, std::memory_order_release);
    m_own->draws.store(draws, std::memory_order_release);
    m_own->applied.store(sequence, std::memory_order_seq_cst);
  }
  // Terminal. The fields are published before the state that makes them valid.
  void publishFault(Fault code, uint32_t result, uint64_t sequence) noexcept {
    if (m_fault == Fault::None) m_fault = code;
    m_own->faultCode.store(uint32_t(code), std::memory_order_release);
    m_own->faultResult.store(result, std::memory_order_release);
    m_own->faultSequence.store(sequence, std::memory_order_release);
    m_own->state.store(uint32_t(State::Faulted), std::memory_order_seq_cst);
  }
  void publishFinished() noexcept { m_own->state.store(uint32_t(State::Finished), std::memory_order_seq_cst); }
  // Before publishApplied of the refused record: x86 reads it ahead of its next scene record.
  void publishSceneRefused() noexcept { m_own->sceneRefused.fetch_add(1, std::memory_order_release); }
  Fault fault() const noexcept { return m_fault; }
  uint64_t expected() const noexcept { return m_expected; }
  // Measurement only (lost-wakeup profile): an unvalidated peek at the
  // producer's tail, and this side's own consumed position. No state changes.
  uint64_t peekTail() const noexcept { return m_peer->tail.load(std::memory_order_seq_cst); }
  uint64_t consumed() const noexcept { return m_head; }

private:
  Read fail(Fault fault) noexcept { if (m_fault == Fault::None) m_fault = fault; return Read::Faulted; }
  void get(uint64_t position, uint8_t* target, uint32_t bytes) const noexcept {
    const uint32_t offset = uint32_t(position) & (m_capacity - 1), first = std::min(bytes, m_capacity - offset);
    std::memcpy(target, m_data + offset, first);
    if (first != bytes) std::memcpy(target + first, m_data, bytes - first);
  }
  const ProducerBlock* m_peer; const uint8_t* m_data; ConsumerBlock* m_own; uint8_t* m_reply;
  uint32_t m_capacity; ipc::Nonce m_nonce;
  uint64_t m_tail = 0, m_head = 0, m_expected;
  Fault m_fault = Fault::None;
};
} // namespace warvk::host::d9::ring
