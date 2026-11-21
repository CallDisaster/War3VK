// Portable core of the D9C2 command ring (tools/render_host/d3d9_command_ring.h):
// layout, framing, backpressure and every "the peer's bytes are not trusted"
// rule, each with a counterexample. No process, pipe, event or GPU here; the
// real host and Connection are covered by their own gates.
#include "../tools/render_host/d3d9_command_ring.h"
#include <cstdio>
#include <memory>
#include <new>
#include <random>
#include <thread>
#include <vector>

using namespace warvk::host;
namespace ring = warvk::host::d9::ring;
namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const char* what) {
  ++checks;
  if (!value && ++failures <= 24) std::fprintf(stderr, "FAILED: %s\n", what);
}
constexpr ipc::Nonce Session{0x1122334455667788ull, 0x99aabbccddeeff01ull};

uint8_t Pattern(uint64_t sequence, uint32_t index) {
  return uint8_t(sequence * 131u + index * 7u + (index >> 8));
}
d9::Header MakeHeader(uint64_t sequence, uint32_t payloadBytes, ipc::Nonce nonce = Session) {
  d9::Header h; h.op = d9::Op::Clear; h.nonce = nonce; h.sequence = sequence; h.payloadBytes = payloadBytes;
  h.args[0] = uint32_t(sequence * 2654435761u); h.args[15] = ~uint32_t(sequence);
  return h;
}

// Both regions in this process, as each side would map them.
struct Rig {
  uint32_t capacity;
  std::vector<uint64_t> producerRegion, consumerRegion; // zero filled, 8-byte aligned
  ring::ProducerBlock* pb; uint8_t* data;
  ring::ConsumerBlock* cb; uint8_t* reply;
  std::unique_ptr<ring::Producer> producer;
  std::unique_ptr<ring::Consumer> consumer;
  std::unique_ptr<d9::Packet> packet = std::make_unique<d9::Packet>();
  std::vector<uint8_t> scratch = std::vector<uint8_t>(d9::MaxPayload);

  explicit Rig(uint32_t cap = ring::MinCapacity, uint64_t last = 1)
    : capacity(cap), producerRegion((ring::ControlBytes + cap) / 8),
      consumerRegion((ring::ControlBytes + ring::ReplyCapacity) / 8) {
    pb = new (producerRegion.data()) ring::ProducerBlock{};
    cb = new (consumerRegion.data()) ring::ConsumerBlock{};
    data = reinterpret_cast<uint8_t*>(producerRegion.data()) + ring::ControlBytes;
    reply = reinterpret_cast<uint8_t*>(consumerRegion.data()) + ring::ControlBytes;
    ring::InitializeProducerBlock(*pb, Session, cap);
    ring::InitializeConsumerBlock(*cb, Session);
    producer = std::make_unique<ring::Producer>(pb, data, cb, reply, cap, last);
    consumer = std::make_unique<ring::Consumer>(pb, data, cb, reply, cap, Session, last);
  }
  ring::Producer::Write write(uint64_t sequence, uint32_t bytes, bool sync = false) {
    uint8_t encoded[d9::HeaderBytes];
    check(d9::EncodeHeader(MakeHeader(sequence, bytes), encoded), "header encodes");
    for (uint32_t i = 0; i < bytes; ++i) scratch[i] = Pattern(sequence, i);
    return producer->tryWrite(encoded, bytes ? scratch.data() : nullptr, bytes, sequence, sync);
  }
  // Reads one record and requires exactly this message.
  bool expect(uint64_t sequence, uint32_t bytes, bool sync = false) {
    d9::Header h; bool s = !sync;
    if (consumer->tryRead(*packet, h, s) != ring::Consumer::Read::Item) return false;
    uint8_t encoded[d9::HeaderBytes];
    if (!d9::EncodeHeader(MakeHeader(sequence, bytes), encoded)
        || std::memcmp(encoded, packet->data(), d9::HeaderBytes) || s != sync) return false;
    for (uint32_t i = 0; i < bytes; ++i)
      if ((*packet)[d9::HeaderBytes + i] != Pattern(sequence, i)) return false;
    return true;
  }
  ring::Consumer::Read read() { d9::Header h; bool s = false; return consumer->tryRead(*packet, h, s); }
  // A record written behind the producer's back, as a wild write or a hostile
  // producer would. `fix` recomputes the check so later rules are reached.
  void craft(uint64_t position, uint32_t message, uint32_t flags, uint32_t sequenceLow,
             const uint8_t* header, bool fix = true) {
    uint8_t record[ring::RecordHeaderBytes];
    d9::P32(record, message); d9::P32(record + 4, flags); d9::P32(record + 8, sequenceLow);
    d9::P32(record + 12, fix ? ring::Check(record, header) : 0);
    std::memcpy(data + (position & (capacity - 1)), record, sizeof(record));
    std::memcpy(data + ((position + ring::RecordHeaderBytes) & (capacity - 1)), header, d9::HeaderBytes);
  }
  // The consumer refuses with exactly this code, keeps refusing, and returns nothing.
  void refuses(ring::Fault fault, const char* what) {
    check(read() == ring::Consumer::Read::Faulted && consumer->fault() == fault, what);
    check(read() == ring::Consumer::Read::Faulted && consumer->fault() == fault, "a faulted consumer stays faulted");
  }
};

void Layout() {
  ring::ProducerBlock p{}; ring::ConsumerBlock c{};
  const auto at = [](const void* base, const void* member) {
    return size_t(static_cast<const uint8_t*>(member) - static_cast<const uint8_t*>(base));
  };
  check(sizeof(p) == 128 && sizeof(c) == 192, "block sizes");
  check(at(&p, &p.version) == 8 && at(&p, &p.capacity) == 12 && at(&p, &p.nonceLow) == 16 && at(&p, &p.nonceHigh) == 24,
    "producer constants");
  check(at(&p, &p.tail) == 64 && at(&p, &p.waiting) == 72 && at(&p, &p.closed) == 76, "producer dynamic fields");
  check(at(&c, &c.replyCapacity) == 12 && at(&c, &c.nonceLow) == 16, "consumer constants");
  check(at(&c, &c.head) == 64 && at(&c, &c.applied) == 72 && at(&c, &c.gpuSubmitted) == 80
    && at(&c, &c.gpuCompleted) == 88 && at(&c, &c.gpuRetired) == 96 && at(&c, &c.draws) == 104
    && at(&c, &c.parked) == 112, "consumer per-record fields share one line");
  check(at(&c, &c.state) == 128 && at(&c, &c.faultCode) == 132 && at(&c, &c.faultResult) == 136
    && at(&c, &c.faultSequence) == 144, "consumer state has its own line");
  check(sizeof(std::atomic<uint64_t>) == 8 && sizeof(std::atomic<uint32_t>) == 4, "atomics are the bare integers");
  check(ring::RecordBytes(128) == 144 && ring::RecordBytes(129) == 160 && ring::RecordBytes(d9::MaxBytes) == 65552,
    "record sizes");
  check(!ring::ValidCapacity(ring::MinCapacity - 1) && !ring::ValidCapacity(ring::MinCapacity / 2)
    && ring::ValidCapacity(ring::MinCapacity) && ring::ValidCapacity(ring::DefaultCapacity)
    && ring::ValidCapacity(ring::MaxCapacity) && !ring::ValidCapacity(ring::MaxCapacity * 2u)
    && !ring::ValidCapacity(ring::MinCapacity + 4096) && !ring::ValidCapacity(0), "capacity range");

  Rig rig;
  check(ring::ValidProducerBlock(*rig.pb, Session, rig.capacity) && ring::ValidConsumerBlock(*rig.cb, Session, 1), "fresh blocks");
  check(!ring::ValidProducerBlock(*rig.pb, {Session.low + 1, Session.high}, rig.capacity)
    && !ring::ValidProducerBlock(*rig.pb, Session, rig.capacity * 2) && !ring::ValidProducerBlock(*rig.pb, {}, rig.capacity)
    && !ring::ValidConsumerBlock(*rig.cb, {Session.low, Session.high + 1}, 1)
    && !ring::ValidConsumerBlock(*rig.cb, Session, 2), "foreign scope, capacity or attach sequence");
  { Rig other; other.pb->magic[7] = '2'; check(!ring::ValidProducerBlock(*other.pb, Session, other.capacity), "producer magic"); }
  // Any version but this build's is refused: the next one, and the previous
  // one (version 1 ended a session at a refused scene record or an
  // asynchronous Present the host skipped; version 2 does not).
  { Rig other; other.pb->version = ring::Version + 1; check(!ring::ValidProducerBlock(*other.pb, Session, other.capacity), "producer version"); }
  { Rig other; other.pb->version = ring::Version - 1; check(!ring::ValidProducerBlock(*other.pb, Session, other.capacity), "previous producer version"); }
  check(ring::Version == 2, "ring version");
  { Rig other; other.pb->tail.store(144); check(!ring::ValidProducerBlock(*other.pb, Session, other.capacity), "producer already published"); }
  { Rig other; other.pb->closed.store(1); check(!ring::ValidProducerBlock(*other.pb, Session, other.capacity), "producer already closed"); }
  { Rig other; other.cb->magic[0] = 'X'; check(!ring::ValidConsumerBlock(*other.cb, Session, 1), "consumer magic"); }
  { Rig other; other.cb->replyCapacity = 4096; check(!ring::ValidConsumerBlock(*other.cb, Session, 1), "reply capacity"); }
  { Rig other; other.cb->head.store(16); check(!ring::ValidConsumerBlock(*other.cb, Session, 1), "consumer already consumed"); }
  { Rig other; other.cb->state.store(2); check(!ring::ValidConsumerBlock(*other.cb, Session, 1), "consumer already faulted"); }
}

void RoundTrips() {
  Rig rig;
  uint64_t sequence = 2;
  check(rig.read() == ring::Consumer::Read::Empty, "fresh ring is empty");
  for (const uint32_t bytes : {0u, 1u, 15u, 16u, 17u, 4096u, d9::MaxPayload}) for (const bool sync : {false, true}) {
    check(rig.write(sequence, bytes, sync) == ring::Producer::Write::Written, "write");
    check(rig.expect(sequence, bytes, sync), "the same message, flag and bytes come out");
    check(rig.read() == ring::Consumer::Read::Empty, "nothing else");
    ++sequence;
  }
  check(rig.producer->published() == sequence - 1 && rig.consumer->expected() == sequence, "sequences advanced together");
  // A locally misused producer refuses instead of publishing a gap.
  check(rig.write(sequence + 1, 0) == ring::Producer::Write::Faulted && rig.producer->fault() == ring::Fault::Sequence,
    "a skipped sequence is refused locally");
  check(rig.write(sequence, 0) == ring::Producer::Write::Faulted && rig.read() == ring::Consumer::Read::Empty,
    "a faulted producer publishes nothing");
}

void Backpressure() {
  { // Three largest records fit the smallest ring; the fourth waits.
    Rig rig;
    for (uint64_t s = 2; s < 5; ++s) check(rig.write(s, d9::MaxPayload) == ring::Producer::Write::Written, "largest record");
    const auto tail = rig.producer->tail();
    check(rig.write(5, d9::MaxPayload) == ring::Producer::Write::Full && rig.producer->tail() == tail
      && rig.producer->published() == 4 && rig.pb->tail.load() == tail, "Full changes nothing");
    check(rig.write(5, 0) == ring::Producer::Write::Written, "a smaller record still fits");
    check(rig.write(6, d9::MaxPayload) == ring::Producer::Write::Full, "still full for the large one");
    check(rig.expect(2, d9::MaxPayload), "oldest first");
    // Space returns when the consumer has copied the record out, before any applied.
    check(rig.cb->applied.load() == 1 && rig.write(6, d9::MaxPayload) == ring::Producer::Write::Written, "space returns at copy-out");
    check(rig.expect(3, d9::MaxPayload) && rig.expect(4, d9::MaxPayload) && rig.expect(5, 0) && rig.expect(6, d9::MaxPayload),
      "order kept across backpressure");
  }
  { // Records that end exactly at the ring's end, then start exactly at offset 0.
    Rig rig;
    const uint32_t exact = 65536 - ring::RecordHeaderBytes - d9::HeaderBytes; // record of 65536 bytes
    check(ring::RecordBytes(d9::HeaderBytes + exact) == 65536, "exact record");
    for (uint64_t s = 2; s < 6; ++s) check(rig.write(s, exact) == ring::Producer::Write::Written, "fills to the last byte");
    check(rig.producer->tail() == rig.capacity && rig.write(6, 0) == ring::Producer::Write::Full, "exactly full");
    for (uint64_t s = 2; s < 6; ++s) check(rig.expect(s, exact), "exact records");
    check(rig.write(6, 777) == ring::Producer::Write::Written && rig.expect(6, 777), "restart at offset zero");
  }
  { // A message (and its D9C2 header) split across the ring's end.
    Rig rig;
    const uint32_t exact = 65536 - ring::RecordHeaderBytes - d9::HeaderBytes;
    for (uint64_t s = 2; s < 5; ++s) check(rig.write(s, exact) == ring::Producer::Write::Written, "prefix");
    check(rig.write(5, exact - 64) == ring::Producer::Write::Written && rig.producer->tail() == rig.capacity - 64, "64 bytes left");
    for (uint64_t s = 2; s < 5; ++s) check(rig.expect(s, exact), "prefix out");
    check(rig.expect(5, exact - 64), "short one out");
    check(rig.write(6, 1000, true) == ring::Producer::Write::Written, "write across the end");
    check(rig.producer->tail() > rig.capacity && rig.expect(6, 1000, true), "split header and payload read back whole");
    check(rig.write(7, d9::MaxPayload) == ring::Producer::Write::Written && rig.expect(7, d9::MaxPayload), "and continue");
  }
  { // Deterministic mixed traffic over many wraps.
    Rig rig; std::mt19937 random(20261113u);
    uint64_t written = 1, read = 1; std::vector<uint32_t> sizes; size_t first = 0; unsigned full = 0;
    for (unsigned step = 0; step < 200000; ++step) {
      if (random() % 3) {
        const uint32_t roll = random() % 100;
        const uint32_t bytes = roll < 80 ? random() % 256 : roll < 97 ? random() % 4096 : random() % (d9::MaxPayload + 1);
        const auto result = rig.write(written + 1, bytes);
        if (result == ring::Producer::Write::Written) { ++written; sizes.push_back(bytes); }
        else { check(result == ring::Producer::Write::Full, "only Full refuses"); ++full; }
      } else if (read < written) {
        check(rig.expect(read + 1, sizes[first]), "mixed traffic order and content"); ++read; ++first;
      } else check(rig.read() == ring::Consumer::Read::Empty, "empty when caught up");
    }
    while (read < written) { check(rig.expect(read + 1, sizes[first]), "drain"); ++read; ++first; }
    check(full > 100 && rig.producer->tail() > uint64_t(rig.capacity) * 50, "the traffic wrapped and hit backpressure");
    std::printf("command ring mixed written=%llu full=%u bytes=%llu\n", static_cast<unsigned long long>(written - 1), full,
      static_cast<unsigned long long>(rig.producer->tail()));
  }
  { // Close: the remaining records, then end of stream instead of Empty.
    Rig rig;
    check(rig.write(2, 5) == ring::Producer::Write::Written && rig.write(3, 0) == ring::Producer::Write::Written, "two records");
    rig.producer->close();
    check(rig.expect(2, 5) && rig.expect(3, 0) && rig.read() == ring::Consumer::Read::Closed
      && rig.read() == ring::Consumer::Read::Closed, "closed after the last record");
  }
}

void ConsumerRefusals() {
  uint8_t header[d9::HeaderBytes];
  const auto encode = [&](uint64_t sequence, uint32_t payload, ipc::Nonce nonce = Session) {
    check(d9::EncodeHeader(MakeHeader(sequence, payload, nonce), header), "crafted header encodes");
  };
  { Rig rig; check(rig.write(2, 0) == ring::Producer::Write::Written && rig.write(3, 0) == ring::Producer::Write::Written, "two");
    check(rig.expect(2, 0), "one read"); rig.pb->tail.store(ring::RecordBytes(128));
    rig.refuses(ring::Fault::TailInvalid, "tail went backwards"); }
  { Rig rig; rig.pb->tail.store(uint64_t(rig.capacity) + 16); rig.refuses(ring::Fault::TailInvalid, "tail beyond the ring"); }
  { Rig rig; rig.pb->tail.store(~uint64_t(0)); rig.refuses(ring::Fault::TailInvalid, "wild tail"); }
  { Rig rig; rig.pb->tail.store(8); rig.refuses(ring::Fault::Truncated, "less than a record header published"); }
  { Rig rig; check(rig.write(2, 1000) == ring::Producer::Write::Written, "one record");
    rig.pb->tail.store(rig.producer->tail() - 16); rig.refuses(ring::Fault::Truncated, "tail inside a record"); }
  for (const uint32_t message : {0u, 127u, d9::MaxBytes + 1, 0xffffffffu}) {
    Rig rig; encode(2, 0); rig.craft(0, message, 0, 2, header); rig.pb->tail.store(65552);
    rig.refuses(ring::Fault::RecordHeader, "record length out of range"); }
  for (const uint32_t flags : {2u, 3u, 0x80000000u}) {
    Rig rig; encode(2, 0); rig.craft(0, 128, flags, 2, header); rig.pb->tail.store(144);
    rig.refuses(ring::Fault::RecordHeader, "unknown record flags"); }
  // Every bit of framing is under the check: the 16 record bytes ...
  for (uint32_t byte = 0; byte < ring::RecordHeaderBytes; ++byte) {
    Rig rig; check(rig.write(2, 16) == ring::Producer::Write::Written && rig.write(3, 64) == ring::Producer::Write::Written, "two");
    rig.data[byte] ^= (byte == 4 ? 1 : 0x10);
    // A length or flag that leaves its range is refused as such; one that
    // stays in range (144 -> 128 bytes, async -> sync) is caught by the check.
    const auto fault = byte == 1 ? ring::Fault::Truncated
      : byte == 2 || byte == 3 || (byte > 4 && byte < 8) ? ring::Fault::RecordHeader : ring::Fault::Checksum;
    rig.refuses(fault, "damaged record header"); }
  // ... and the 128 bytes of the message header.
  for (uint32_t byte = 0; byte < d9::HeaderBytes; ++byte) {
    Rig rig; check(rig.write(2, 16) == ring::Producer::Write::Written, "one");
    rig.data[ring::RecordHeaderBytes + byte] ^= 0x04;
    rig.refuses(ring::Fault::Checksum, "damaged message header"); }
  // With a matching check the later rules still hold.
  { Rig rig; encode(2, 0); header[0] = 'X'; rig.craft(0, 128, 0, 2, header); rig.pb->tail.store(144);
    rig.refuses(ring::Fault::MessageHeader, "not a D9C2 header"); }
  { Rig rig; encode(2, 32); rig.craft(0, 128 + 16, 0, 2, header); rig.pb->tail.store(160);
    rig.refuses(ring::Fault::MessageHeader, "header payload size disagrees with the record"); }
  { Rig rig; encode(2, 0, {Session.low, Session.high ^ 1}); rig.craft(0, 128, 0, 2, header); rig.pb->tail.store(144);
    rig.refuses(ring::Fault::Scope, "another session's message"); }
  { Rig rig; encode(4, 0); rig.craft(0, 128, 0, 4, header); rig.pb->tail.store(144);
    rig.refuses(ring::Fault::Sequence, "skipped sequence"); }
  { Rig rig; check(rig.write(2, 0) == ring::Producer::Write::Written && rig.expect(2, 0), "one");
    encode(2, 0); rig.craft(144, 128, 0, 2, header); rig.pb->tail.store(288);
    rig.refuses(ring::Fault::Sequence, "repeated sequence"); }
  { Rig rig; encode(2, 0); rig.craft(0, 128, 0, 3, header); rig.pb->tail.store(144);
    rig.refuses(ring::Fault::Sequence, "sequenceLow disagrees with the header"); }
  { // A refusal never hands out the records behind it.
    Rig rig; check(rig.write(2, 8) == ring::Producer::Write::Written && rig.write(3, 8) == ring::Producer::Write::Written, "two");
    rig.data[ring::RecordHeaderBytes + 40] ^= 1;
    rig.refuses(ring::Fault::Checksum, "first record damaged");
    check(rig.cb->head.load() == 0 && rig.consumer->expected() == 2, "nothing consumed after a refusal"); }
  { // Documented limit: payload bytes are not under the check.
    Rig rig; check(rig.write(2, 64) == ring::Producer::Write::Written, "one");
    rig.data[ring::RecordHeaderBytes + d9::HeaderBytes + 10] ^= 0xff;
    d9::Header h; bool sync = true;
    check(rig.consumer->tryRead(*rig.packet, h, sync) == ring::Consumer::Read::Item
      && (*rig.packet)[d9::HeaderBytes + 10] == uint8_t(Pattern(2, 10) ^ 0xff)
      && (*rig.packet)[d9::HeaderBytes + 11] == Pattern(2, 11), "payload damage passes the ring (host validates payloads)"); }
}

void ProducerRefusals() {
  ring::Producer::Progress progress;
  { Rig rig; for (uint64_t s = 2; s < 5; ++s) check(rig.write(s, 0) == ring::Producer::Write::Written, "three");
    check(rig.expect(2, 0) && rig.expect(3, 0) && rig.producer->observe(progress) && progress.head == 288 && progress.applied == 1,
      "validated head");
    rig.cb->head.store(144);
    check(!rig.producer->observe(progress) && rig.producer->fault() == ring::Fault::HeadInvalid, "head went backwards");
    const auto tail = rig.pb->tail.load();
    check(rig.write(5, 0) == ring::Producer::Write::Faulted && rig.pb->tail.load() == tail, "no publication after a fault"); }
  { Rig rig; check(rig.write(2, 0) == ring::Producer::Write::Written, "one"); rig.cb->head.store(160);
    check(!rig.producer->observe(progress) && rig.producer->fault() == ring::Fault::HeadInvalid, "head beyond tail"); }
  { // Found on the write path too, where a wild head would overwrite unread bytes.
    Rig rig; for (uint64_t s = 2; s < 5; ++s) check(rig.write(s, d9::MaxPayload) == ring::Producer::Write::Written, "fill");
    rig.cb->head.store(~uint64_t(0) - 15);
    check(rig.write(5, d9::MaxPayload) == ring::Producer::Write::Faulted && rig.producer->fault() == ring::Fault::HeadInvalid,
      "wild head refused when space is needed"); }
  { Rig rig; check(rig.write(2, 0) == ring::Producer::Write::Written, "one"); rig.cb->applied.store(3);
    check(!rig.producer->observe(progress) && rig.producer->fault() == ring::Fault::AppliedInvalid, "applied beyond published"); }
  { Rig rig; check(rig.write(2, 0) == ring::Producer::Write::Written && rig.write(3, 0) == ring::Producer::Write::Written, "two");
    check(rig.expect(2, 0) && rig.expect(3, 0), "read"); rig.consumer->publishApplied(3, 9, 8, 7, 6);
    check(rig.producer->observe(progress) && progress.applied == 3 && rig.producer->gpuCompleted() == 8, "applied and GPU watermark");
    rig.cb->applied.store(2);
    check(!rig.producer->observe(progress) && rig.producer->fault() == ring::Fault::AppliedInvalid, "applied went backwards"); }

  // Reply slot.
  const auto h = MakeHeader(7, 0);
  for (const uint32_t bytes : {0u, 1u, 4096u, d9::MaxReplyPayload}) {
    Rig rig; std::vector<uint8_t> payload(bytes), out(d9::MaxReplyPayload, 0xee);
    for (uint32_t i = 0; i < bytes; ++i) payload[i] = Pattern(99, i);
    rig.consumer->writeReply(h, d9::Reply{0x8876086Cu, 5, 4, 3, 77, bytes}, payload.data());
    d9::Reply r;
    check(rig.producer->readReply(h, r, out.data()) && r.result == 0x8876086Cu && r.submitted == 5 && r.completed == 4
      && r.retired == 3 && r.draws == 77 && r.payloadBytes == bytes && (!bytes || !std::memcmp(out.data(), payload.data(), bytes))
      && (bytes == d9::MaxReplyPayload || out[bytes] == 0xee), "reply round trip");
  }
  const auto broken = [&](const char* what, void (*damage)(Rig&)) {
    Rig rig; uint8_t payload[8]{}; std::vector<uint8_t> out(d9::MaxReplyPayload);
    rig.consumer->writeReply(h, d9::Reply{0, 5, 4, 3, 77, 8}, payload);
    damage(rig); d9::Reply r;
    check(!rig.producer->readReply(h, r, out.data()) && rig.producer->fault() == ring::Fault::ReplyInvalid, what);
    check(rig.write(2, 0) == ring::Producer::Write::Faulted, "no publication after a bad reply");
  };
  broken("reply magic", [](Rig& rig) { rig.reply[0] ^= 1; });
  broken("reply for another sequence", [](Rig& rig) { rig.reply[32] ^= 1; });
  broken("reply for another session", [](Rig& rig) { rig.reply[16] ^= 1; });
  broken("reply for another resource", [](Rig& rig) { rig.reply[40] ^= 1; });
  broken("reply length beyond the slot", [](Rig& rig) { d9::P32(rig.reply + 8, d9::MaxReplyPayload + 1); });
  broken("reply reserved bytes", [](Rig& rig) { rig.reply[95] = 1; });
  broken("retired beyond completed", [](Rig& rig) { d9::P64(rig.reply + 64, 5); });
  { Rig rig; uint8_t none = 0; std::vector<uint8_t> out(d9::MaxReplyPayload); d9::Reply r;
    rig.consumer->writeReply(h, d9::Reply{0, 1, 1, 1, 1, 0}, &none);
    check(!rig.producer->readReply(MakeHeader(8, 0), r, out.data()) && rig.producer->fault() == ring::Fault::ReplyInvalid,
      "a stale reply does not answer the next call"); }

  // Host state.
  { Rig rig; auto state = rig.producer->hostState();
    check(state.state == ring::State::Running && state.code == ring::Fault::None, "running");
    rig.consumer->publishFault(ring::Fault::AsyncRefused, 0x8876086Cu, 77); state = rig.producer->hostState();
    check(state.state == ring::State::Faulted && state.code == ring::Fault::AsyncRefused && state.result == 0x8876086Cu
      && state.sequence == 77 && rig.read() == ring::Consumer::Read::Faulted, "published fault");
    rig.cb->state.store(99); check(rig.producer->hostState().state == ring::State::Faulted, "an unknown state is a fault"); }
  { Rig rig; rig.consumer->publishFinished(); check(rig.producer->hostState().state == ring::State::Finished, "finished"); }
}

// One producer thread and one consumer thread, as the two processes run. Every
// 20000 records the consumer stops until the producer has met a full ring, so
// backpressure is exercised under real concurrency; no synchronous record
// falls in the stretch that fills it.
bool IsSync(uint64_t sequence) { return sequence % 37 == 0 && sequence % 20000 >= 2000; }
void TwoThreads() {
  Rig rig; constexpr uint64_t Count = 200000; std::atomic<bool> bad{false}; std::atomic<uint64_t> fullWaits{0};
  std::thread consumer([&] {
    auto packet = std::make_unique<d9::Packet>(); d9::Header h; bool sync = false; uint64_t next = 2;
    while (next < Count + 2 && !bad.load(std::memory_order_relaxed)) {
      if (next % 20000 == 0 && next + 2000 < Count) // the last stretch has too few records left to fill the ring
        for (const auto before = fullWaits.load(); fullWaits.load() == before && !bad.load(std::memory_order_relaxed);)
          std::this_thread::yield();
      const auto result = rig.consumer->tryRead(*packet, h, sync);
      if (result == ring::Consumer::Read::Empty) { std::this_thread::yield(); continue; }
      if (result != ring::Consumer::Read::Item || h.sequence != next || sync != IsSync(next)) { bad = true; break; }
      for (uint32_t i = 0; i < h.payloadBytes; ++i)
        if ((*packet)[d9::HeaderBytes + i] != Pattern(next, i)) { bad = true; break; }
      if (sync) rig.consumer->writeReply(h, d9::Reply{0, next, next, next, next, std::min(h.payloadBytes, 64u)},
        packet->data() + d9::HeaderBytes);
      rig.consumer->publishApplied(next, next, next, next, next);
      ++next;
    }
  });
  std::mt19937 random(7u); std::vector<uint8_t> payload(d9::MaxPayload), out(d9::MaxReplyPayload);
  uint64_t replies = 0, expectedReplies = 0; ring::Producer::Progress progress;
  for (uint64_t sequence = 2; sequence < Count + 2 && !bad.load(std::memory_order_relaxed); ++sequence) {
    const uint32_t roll = random() % 100;
    const uint32_t bytes = roll < 90 ? random() % 256 : roll < 99 ? random() % 4096 : random() % (d9::MaxPayload + 1);
    for (uint32_t i = 0; i < bytes; ++i) payload[i] = Pattern(sequence, i);
    const auto h = MakeHeader(sequence, bytes); uint8_t encoded[d9::HeaderBytes];
    if (!d9::EncodeHeader(h, encoded)) { bad = true; break; }
    const bool sync = IsSync(sequence); expectedReplies += sync;
    for (;;) {
      const auto result = rig.producer->tryWrite(encoded, bytes ? payload.data() : nullptr, bytes, sequence, sync);
      if (result == ring::Producer::Write::Written) break;
      if (result != ring::Producer::Write::Full || bad.load(std::memory_order_relaxed)) { bad = true; break; }
      ++fullWaits; std::this_thread::yield();
    }
    if (sync && !bad.load(std::memory_order_relaxed)) {
      for (;;) {
        if (!rig.producer->observe(progress) || bad.load(std::memory_order_relaxed)) { bad = true; break; }
        if (progress.applied == sequence) break;
        std::this_thread::yield();
      }
      d9::Reply r;
      if (bad.load() || !rig.producer->readReply(h, r, out.data()) || r.draws != sequence
          || r.payloadBytes != std::min(bytes, 64u) || std::memcmp(out.data(), payload.data(), r.payloadBytes)) bad = true;
      else ++replies;
    }
  }
  consumer.join();
  check(!bad.load() && rig.producer->fault() == ring::Fault::None && rig.consumer->fault() == ring::Fault::None,
    "two threads: no fault, order and content kept");
  check(rig.producer->observe(progress) && progress.applied == Count + 1 && progress.head == rig.producer->tail(),
    "two threads: everything consumed and applied");
  check(replies == expectedReplies && replies > 4000 && rig.producer->tail() > uint64_t(rig.capacity) * 50,
    "two threads: replies and wraps");
  check(fullWaits.load() >= 9, "two threads: the producer met a full ring in every stopped stretch");
  std::printf("command ring two-thread records=%llu replies=%llu fullWaits=%llu bytes=%llu\n",
    static_cast<unsigned long long>(Count), static_cast<unsigned long long>(replies),
    static_cast<unsigned long long>(fullWaits.load()), static_cast<unsigned long long>(rig.producer->tail()));
}
}

int main() {
  Layout();
  RoundTrips();
  Backpressure();
  ConsumerRefusals();
  ProducerRefusals();
  TwoThreads();
  std::printf("d3d9 command ring checks=%u failures=%u bits=%zu\n", checks, failures, sizeof(void*) * 8);
  return failures ? 1 : 0;
}
