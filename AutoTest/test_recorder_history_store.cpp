// E1 CPU history-store test. System under test: the real WVE1 Encode/Decode
// (recorder_event_wire.cpp) plus the real HistoryStore; no state machine is
// re-modelled here. All positive flows go through actual Encode; negative
// wire cases only byte-patch actually encoded packets. Build/link is run by
// the main thread: test_recorder_history_store.cpp + recorder_history_store.cpp
// + recorder_event_wire.cpp. No GPU/OS/IPC/disk; small caps run on 32/64 bit,
// the single max-capacity case is expected to run on the 64-bit host.
#include "../tools/render_host/recorder_history_store.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace warvk::host::recorder;
using dxvk::war3::tools::evidence::Kind;

// Exercise the actual new Event[] failure path, without exhausting the OS or
// introducing a production test hook. This executable is single-threaded.
static bool failNextArray = false;
static uint32_t injectedArrayFailures = 0;
[[gnu::noinline]] void* operator new[](std::size_t n) {
  if (failNextArray) {
    failNextArray=false; ++injectedArrayFailures; throw std::bad_alloc();
  }
  if (void* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete[](void* p) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

namespace {
unsigned g_checks = 0;
void check(bool v, const char* what) { ++g_checks; if (!v) throw std::runtime_error(what); }

Event makeEvent(uint64_t seq, uint64_t session) {
  Event e{};
  e.sequence = seq; e.session = session;
  e.parent = seq * 7 + 1; e.qpc = 1000 + seq;
  e.key.owner = 0xAB00 + seq; e.key.frame = seq; e.key.mapEpoch = 5; e.key.deviceEpoch = 6;
  e.thread = uint32_t(seq % 8);
  e.kind = Kind(uint32_t(Kind::PresentBegin) + uint32_t(seq % 18)); // valid kinds 1..18
  const std::string label = "ev" + std::to_string(seq);
  std::copy(label.begin(), label.end(), e.label.begin()); // rest stays NUL
  for (uint32_t j = 0; j < 12; ++j) e.data[j] = seq * 100 + j;
  for (uint32_t j = 0; j < 48; ++j) e.bits[j] = uint32_t(seq * 31 + j);
  return e;
}

std::vector<uint8_t> encodePacket(const Header& h, const Event* events) {
  std::vector<uint8_t> packet(MaxPacketBytes);
  size_t written = 0;
  const WireError w = Encode(h, events, packet.data(), packet.size(), written);
  if (w != WireError::None) throw std::runtime_error(std::string("encode refused: ") + ErrorName(w));
  packet.resize(written);
  return packet;
}
StoreError sendBegin(HistoryStore& s, uint64_t session, uint32_t capacity) {
  Header h{}; h.op = Op::Begin; h.session = session; h.ordinal = 1; h.capacity = capacity;
  const auto p = encodePacket(h, nullptr);
  return s.accept(p.data(), p.size());
}
StoreError sendData(HistoryStore& s, uint64_t session, uint32_t capacity, uint64_t ordinal,
                    uint64_t firstSeq, uint32_t count, uint64_t trigger) {
  std::vector<Event> events(count);
  for (uint32_t i = 0; i < count; ++i) events[i] = makeEvent(firstSeq + i, session);
  Header h{}; h.op = Op::Data; h.count = count; h.session = session; h.ordinal = ordinal;
  h.trigger = trigger; h.capacity = capacity;
  const auto p = encodePacket(h, events.data());
  return s.accept(p.data(), p.size());
}
StoreError sendSeal(HistoryStore& s, uint64_t session, uint32_t capacity, uint64_t ordinal,
                    uint64_t trigger, uint64_t accepted, uint64_t attempted, uint32_t reason) {
  Header h{}; h.op = Op::Seal; h.session = session; h.ordinal = ordinal; h.trigger = trigger;
  h.capacity = capacity; h.reason = reason; h.accepted = accepted; h.attempted = attempted;
  h.lost = attempted - accepted;
  const auto p = encodePacket(h, nullptr);
  return s.accept(p.data(), p.size());
}
void sendRange(HistoryStore& s, uint64_t session, uint32_t capacity, uint64_t& ordinal,
               uint64_t firstSeq, uint64_t total, uint64_t trigger) {
  uint64_t left = total, seq = firstSeq;
  while (left) {
    const uint32_t n = uint32_t(std::min<uint64_t>(left, MaxEvents));
    check(sendData(s, session, capacity, ordinal++, seq, n, trigger) == StoreError::None, "data packet accepted");
    seq += n; left -= n;
  }
}
std::vector<uint64_t> collectSequences(const HistoryStore& s) {
  std::vector<uint64_t> out;
  check(s.visitFrozen([&](const Event& e) { out.push_back(e.sequence); return true; }), "visitFrozen succeeds");
  return out;
}

void testEmptySummary() {
  const HistoryStore s;
  const auto m = s.summary();
  check(m.state == StoreState::Empty && m.session == 0 && m.nextOrdinal == 1, "empty state");
  check(m.lastSequence == 0 && m.trigger == NoTrigger && m.capacity == 0, "empty counters");
  check(m.storageBytes == 0 && m.wireError == WireError::None && m.storeError == StoreError::None, "empty errors");
  check(!s.visitFrozen([](const Event&) { return true; }), "empty store cannot be visited");
}

void testHappyPathWithTrigger() {
  HistoryStore s;
  check(sendBegin(s, 77, 64) == StoreError::None, "begin");
  // cut first appears in Data with cut >= lastSequence(0); no reclassification.
  check(sendData(s, 77, 64, 2, 1, 24, 24) == StoreError::None, "data 1..24 with cut=24");
  check(sendData(s, 77, 64, 3, 25, 16, 24) == StoreError::None, "data 25..40 post region");
  check(sendSeal(s, 77, 64, 4, 24, 40, 42, 1) == StoreError::None, "seal closes");
  const auto m = s.summary();
  check(m.state == StoreState::Frozen && m.session == 77 && m.nextOrdinal == 5, "summary state/session/ordinal");
  check(m.lastSequence == 40 && m.trigger == 24 && m.capacity == 64, "summary sequence/trigger/capacity");
  check(m.accepted == 40 && m.attempted == 42 && m.lost == 2, "summary totals");
  check(m.evicted == 0 && m.retained == 40 && m.reason == 1, "summary retention");
  check(m.storageBytes == uint64_t(64) * sizeof(Event), "summary storageBytes");
  std::cout << "storageBytes cap=64: " << m.storageBytes << "\n";
  check(m.wireError == WireError::None && m.storeError == StoreError::None, "no errors");
  const auto seqs = collectSequences(s);
  check(seqs.size() == 40, "frozen visit count");
  uint64_t expect = 1;
  check(s.visitFrozen([&](const Event& e) {
    if (e.sequence != expect || e.data[0] != expect * 100 || e.qpc != 1000 + expect ||
        e.key.owner != 0xAB00 + expect || e.label[0] != 'e') return false;
    ++expect; return true; }), "ascending sequence and intact content");
  check(expect == 41, "all records visited once");
}

void testPreRingOverwriteEviction() {
  HistoryStore s;
  check(sendBegin(s, 5, 16) == StoreError::None, "begin"); // pre=12 post=4
  uint64_t ordinal = 2;
  sendRange(s, 5, 16, ordinal, 1, 20, NoTrigger);
  check(sendSeal(s, 5, 16, ordinal, NoTrigger, 20, 20, 1) == StoreError::None, "untriggered seal");
  const auto m = s.summary();
  check(m.state == StoreState::Frozen && m.accepted == 20 && m.lastSequence == 20, "overwrite totals");
  check(m.evicted == 8 && m.retained == 12, "pre-ring eviction counted");
  check(m.trigger == NoTrigger && m.storageBytes == uint64_t(16) * sizeof(Event), "untriggered summary");
  const auto seqs = collectSequences(s); // untriggered seal exports the pre region only
  check(seqs.size() == 12 && seqs.front() == 9 && seqs.back() == 20, "retained are the newest pre-ring records");
  check(std::is_sorted(seqs.begin(), seqs.end()), "visit order is ascending");
}

void testVisitorFailureAndNotFrozen() {
  HistoryStore s;
  check(sendBegin(s, 9, 32) == StoreError::None, "begin");
  check(!s.visitFrozen([](const Event&) { return true; }), "recording store cannot be visited");
  uint64_t ordinal = 2;
  sendRange(s, 9, 32, ordinal, 1, 8, NoTrigger);
  check(sendSeal(s, 9, 32, ordinal, NoTrigger, 8, 8, 1) == StoreError::None, "seal");
  uint32_t calls = 0;
  check(!s.visitFrozen([&](const Event&) { return ++calls < 3; }), "visitor failure returns false");
  check(calls == 3, "visitor failure stops the visit early");
  check(s.summary().state == StoreState::Frozen, "failed visit does not damage the store");
}

void testCorruptTailRecordNoPartialCommit() {
  { HistoryStore s;
    check(sendBegin(s, 11, 32) == StoreError::None, "begin");
    std::vector<Event> events(4);
    for (uint32_t i = 0; i < 4; ++i) events[i] = makeEvent(1 + i, 11);
    events[3].sequence = 99; // tail breaks continuity
    Header h{}; h.op = Op::Data; h.count = 4; h.session = 11; h.ordinal = 2; h.capacity = 32;
    const auto p = encodePacket(h, events.data());
    check(s.accept(p.data(), p.size()) == StoreError::SequenceGap, "gap tail rejected");
    const auto m = s.summary();
    check(m.state == StoreState::Fault && m.storeError == StoreError::SequenceGap, "gap fault");
    check(m.lastSequence == 0 && m.accepted == 0 && m.retained == 0, "no partial commit on gap"); }
  { HistoryStore s;
    check(sendBegin(s, 12, 32) == StoreError::None, "begin");
    std::vector<Event> events(4);
    for (uint32_t i = 0; i < 4; ++i) events[i] = makeEvent(1 + i, 12);
    Header h{}; h.op = Op::Data; h.count = 4; h.session = 12; h.ordinal = 2; h.capacity = 32;
    auto p = encodePacket(h, events.data());
    // Byte-patch the last record's kind (event offset 68) to an invalid value.
    const size_t kindOff = HeaderBytes + size_t(3) * EventBytes + 68;
    p[kindOff] = 99; p[kindOff + 1] = 0; p[kindOff + 2] = 0; p[kindOff + 3] = 0;
    check(s.accept(p.data(), p.size()) == StoreError::WireDecode, "corrupt tail rejected by real codec");
    const auto m = s.summary();
    check(m.state == StoreState::Fault && m.wireError != WireError::None, "wire fault recorded");
    check(m.lastSequence == 0 && m.accepted == 0, "no partial commit on corrupt tail"); }
}

void testOrdinalSkipAndReplay() {
  { HistoryStore s;
    check(sendBegin(s, 21, 32) == StoreError::None, "begin");
    check(sendData(s, 21, 32, 2, 1, 4, NoTrigger) == StoreError::None, "first data");
    check(sendData(s, 21, 32, 2, 5, 4, NoTrigger) == StoreError::OrdinalMismatch, "replayed ordinal rejected");
    check(s.summary().state == StoreState::Fault, "replay faults"); }
  { HistoryStore s;
    check(sendBegin(s, 22, 32) == StoreError::None, "begin");
    check(sendData(s, 22, 32, 3, 1, 4, NoTrigger) == StoreError::OrdinalMismatch, "skipped ordinal rejected"); }
}

void testWrongSessionAndCapacityChange() {
  { HistoryStore s;
    check(sendBegin(s, 31, 32) == StoreError::None, "begin");
    check(sendData(s, 32, 32, 2, 1, 4, NoTrigger) == StoreError::SessionMismatch, "wrong session rejected"); }
  { HistoryStore s;
    check(sendBegin(s, 33, 32) == StoreError::None, "begin");
    check(sendData(s, 33, 64, 2, 1, 4, NoTrigger) == StoreError::BadDataFields, "capacity change rejected"); }
}

void testTriggerRegressedAndChanged() {
  { HistoryStore s;
    check(sendBegin(s, 41, 64) == StoreError::None, "begin");
    check(sendData(s, 41, 64, 2, 1, 10, NoTrigger) == StoreError::None, "data without cut");
    check(sendData(s, 41, 64, 3, 11, 10, 5) == StoreError::TriggerRegressed, "cut before stored records rejected");
    check(s.summary().state == StoreState::Fault && s.summary().lastSequence == 10, "regressed cut faults without writes"); }
  { HistoryStore s;
    check(sendBegin(s, 42, 64) == StoreError::None, "begin");
    check(sendData(s, 42, 64, 2, 1, 8, 8) == StoreError::None, "cut fixed");
    check(sendData(s, 42, 64, 3, 9, 8, 9) == StoreError::TriggerConflict, "changed cut rejected"); }
  { HistoryStore s;
    check(sendBegin(s, 43, 64) == StoreError::None, "begin");
    check(sendData(s, 43, 64, 2, 1, 8, 8) == StoreError::None, "cut fixed for seal case");
    check(sendSeal(s, 43, 64, 3, 7, 8, 8, 1) == StoreError::TriggerConflict, "seal with a different cut rejected"); }
}

void testSealCutAndTotalsClosure() {
  { HistoryStore s;
    check(sendBegin(s, 51, 64) == StoreError::None, "begin");
    check(sendData(s, 51, 64, 2, 1, 10, 10) == StoreError::None, "data with cut=10");
    check(sendSeal(s, 51, 64, 3, 10, 10, 12, 1) == StoreError::None, "seal with covered cut closes"); }
  { HistoryStore s;
    check(sendBegin(s, 52, 64) == StoreError::None, "begin");
    check(sendData(s, 52, 64, 2, 1, 4, NoTrigger) == StoreError::None, "data");
    check(sendSeal(s, 52, 64, 3, NoTrigger, 5, 5, 1) == StoreError::TotalsMismatch, "accepted beyond stored rejected"); }
  { HistoryStore s; // cut first fixed at Seal, beyond the stored records: codec-legal, store must reject
    check(sendBegin(s, 53, 64) == StoreError::None, "begin");
    check(sendData(s, 53, 64, 2, 1, 10, NoTrigger) == StoreError::None, "data without cut");
    const StoreError e = sendSeal(s, 53, 64, 3, 12, 12, 12, 1); // claims 12, cut=12, only 10 stored
    check(e == StoreError::TriggerNotCovered, "seal cut beyond stored records rejected");
    check(s.summary().state == StoreState::Fault && s.summary().storeError == StoreError::TriggerNotCovered, "uncovered cut faults"); }
}

void testPostOverflowNoPartialCommit() {
  HistoryStore s;
  check(sendBegin(s, 61, 4) == StoreError::None, "begin"); // pre=3 post=1
  check(sendData(s, 61, 4, 2, 1, 5, 1) == StoreError::PostOverflow, "post region beyond capacity rejected");
  const auto m = s.summary();
  check(m.state == StoreState::Fault && m.storeError == StoreError::PostOverflow, "post overflow fault");
  check(m.accepted == 0 && m.lastSequence == 0, "post overflow never partially commits");
}

void testTerminalStatesAndFaultPersistence() {
  HistoryStore s;
  check(sendBegin(s, 71, 32) == StoreError::None, "begin");
  check(sendData(s, 71, 32, 2, 1, 4, NoTrigger) == StoreError::None, "data");
  check(sendSeal(s, 71, 32, 3, NoTrigger, 4, 4, 1) == StoreError::None, "seal");
  check(s.summary().state == StoreState::Frozen, "frozen after seal");
  check(sendData(s, 71, 32, 4, 5, 4, NoTrigger) == StoreError::BadState, "late data rejected");
  check(s.summary().state == StoreState::Fault, "late data permanently faults");
  check(sendSeal(s, 71, 32, 4, NoTrigger, 4, 4, 1) == StoreError::BadState, "duplicate seal rejected");
  check(sendBegin(s, 72, 32) == StoreError::BadState, "faulted store never reopens");
  check(s.summary().storeError == StoreError::BadState, "first fault reason persists");
}

void testNewObjectRecovery() {
  { HistoryStore broken;
    check(broken.accept(nullptr, 0) == StoreError::WireDecode, "null packet faults");
    check(broken.summary().state == StoreState::Fault, "broken stays faulted"); }
  HistoryStore s; // recovery is a fresh object with a fresh session
  check(sendBegin(s, 81, 16) == StoreError::None, "new object begins");
  check(sendData(s, 81, 16, 2, 1, 6, NoTrigger) == StoreError::None, "new object records");
  check(sendSeal(s, 81, 16, 3, NoTrigger, 6, 6, 1) == StoreError::None, "new object seals");
  check(s.summary().state == StoreState::Frozen && s.summary().retained == 6, "new object recovers fully");
}

void testWireLevelNegatives() {
  { HistoryStore s; // bad magic, patched into a real encoded Begin
    Header h{}; h.op = Op::Begin; h.session = 91; h.ordinal = 1; h.capacity = 16;
    auto p = encodePacket(h, nullptr); p[0] = 'X';
    check(s.accept(p.data(), p.size()) == StoreError::WireDecode, "bad magic rejected"); }
  { HistoryStore s; // truncated real packet
    Header h{}; h.op = Op::Begin; h.session = 92; h.ordinal = 1; h.capacity = 16;
    const auto p = encodePacket(h, nullptr);
    check(s.accept(p.data(), p.size() - 5) == StoreError::WireDecode, "truncated packet rejected"); }
  { HistoryStore s; // capacity below the wire minimum, patched into a real Begin
    Header h{}; h.op = Op::Begin; h.session = 93; h.ordinal = 1; h.capacity = 16;
    auto p = encodePacket(h, nullptr);
    p[64] = 3; p[65] = 0; p[66] = 0; p[67] = 0; // capacity field (offset 64)
    check(s.accept(p.data(), p.size()) == StoreError::WireDecode, "bad capacity rejected"); }
  { HistoryStore s; // record session disagrees with the header session
    check(sendBegin(s, 94, 32) == StoreError::None, "begin");
    std::vector<Event> events(2);
    for (uint32_t i = 0; i < 2; ++i) events[i] = makeEvent(1 + i, 94);
    Header h{}; h.op = Op::Data; h.count = 2; h.session = 94; h.ordinal = 2; h.capacity = 32;
    auto p = encodePacket(h, events.data());
    p[HeaderBytes + 8] ^= 0xFF; // first record session low byte (event offset 8)
    check(s.accept(p.data(), p.size()) == StoreError::WireDecode, "record session mismatch rejected"); }
  { HistoryStore s;
    const auto m0 = s.summary();
    check(m0.state == StoreState::Empty, "pre-data state");
    check(sendData(s, 95, 32, 1, 1, 2, NoTrigger) == StoreError::BadState, "data before begin rejected");
    check(sendSeal(s, 95, 32, 1, NoTrigger, 0, 0, 1) == StoreError::BadState, "seal before begin rejected"); }
}

void testFixedCutMustRepeatExactly() {
  { HistoryStore s; // Data carrying NoTrigger after the cut was fixed is a change
    check(sendBegin(s, 44, 64) == StoreError::None, "begin");
    check(sendData(s, 44, 64, 2, 1, 8, 8) == StoreError::None, "cut fixed");
    check(sendData(s, 44, 64, 3, 9, 8, NoTrigger) == StoreError::TriggerConflict, "data without the fixed cut rejected");
    check(s.summary().state == StoreState::Fault && s.summary().storeError == StoreError::TriggerConflict, "conflict faults permanently");
    check(sendData(s, 44, 64, 3, 9, 8, 8) == StoreError::TriggerConflict, "faulted store keeps the first error"); }
  { HistoryStore s; // Seal carrying NoTrigger after the cut was fixed is a change
    check(sendBegin(s, 45, 64) == StoreError::None, "begin");
    check(sendData(s, 45, 64, 2, 1, 8, 8) == StoreError::None, "cut fixed");
    check(sendSeal(s, 45, 64, 3, NoTrigger, 8, 8, 1) == StoreError::TriggerConflict, "seal without the fixed cut rejected");
    check(s.summary().state == StoreState::Fault, "seal conflict faults"); }
}

void testVisitorReentryFailsVisit() {
  HistoryStore s;
  check(sendBegin(s, 96, 32) == StoreError::None, "begin");
  check(sendData(s, 96, 32, 2, 1, 4, NoTrigger) == StoreError::None, "data");
  check(sendSeal(s, 96, 32, 3, NoTrigger, 4, 4, 1) == StoreError::None, "seal");
  HistoryStore* raw = &s;
  uint32_t calls = 0;
  const bool ok = s.visitFrozen([raw, &calls](const Event&) {
    ++calls;
    if (calls == 2) { // a visitor must not reenter accept(); it faults the store
      Header h{}; h.op = Op::Data; h.count = 1; h.session = 96; h.ordinal = 4; h.capacity = 32;
      Event e = makeEvent(5, 96);
      const auto p = encodePacket(h, &e);
      raw->accept(p.data(), p.size());
    }
    return true;
  });
  check(!ok, "visit fails after a reentrant accept instead of reporting success");
  check(s.summary().state == StoreState::Fault, "reentrant accept faulted the store");
}

void testAllocationFailure() {
  Header h{};h.op=Op::Begin;h.session=900;h.capacity=16;h.ordinal=1;
  const auto packet=encodePacket(h,nullptr); // Prepare outside the injected failure.
  HistoryStore failed;
  failNextArray=true;
  check(failed.accept(packet.data(),packet.size())==StoreError::AllocationFailed,"actual allocation failure rejected");
  check(!failNextArray && injectedArrayFailures==1,"new Event[] failure actually injected");
  const auto s=failed.summary();
  check(s.state==StoreState::Fault && !s.capacity && !s.storageBytes && !s.accepted,"failed Begin has no claimed allocation");
  check(failed.accept(packet.data(),packet.size())==StoreError::AllocationFailed,"allocation fault cannot rearm");
  check(!failed.visitFrozen([](const Event&){return true;}),"allocation failure cannot export");
  HistoryStore recovered;
  check(sendBegin(recovered,901,16)==StoreError::None,"new session can recover after allocation failure");
  check(sendSeal(recovered,901,16,2,NoTrigger,0,0,1)==StoreError::None,"empty new session seals");
  check(recovered.visitFrozen([](const Event&){return false;}),"empty history never calls visitor");
}

void testMaxCapacity() {
  HistoryStore s;
  const StoreError begun = sendBegin(s, 101, MaxHistoryEvents);
  check(begun == StoreError::None, "maxcap begin");
  uint64_t ordinal = 2;
  sendRange(s, 101, MaxHistoryEvents, ordinal, 1, MaxHistoryEvents, NoTrigger);
  check(sendSeal(s, 101, MaxHistoryEvents, ordinal, NoTrigger, MaxHistoryEvents, MaxHistoryEvents, 3) == StoreError::None, "maxcap seal");
  const auto m = s.summary();
  const uint64_t pre = MaxHistoryEvents - MaxHistoryEvents / 4;
  check(m.accepted == MaxHistoryEvents && m.lastSequence == MaxHistoryEvents, "maxcap totals");
  check(m.evicted == MaxHistoryEvents - pre && m.retained == pre, "maxcap retention closes");
  check(m.storageBytes == uint64_t(MaxHistoryEvents) * sizeof(Event), "maxcap storageBytes");
  std::cout << "storageBytes cap=262144: " << m.storageBytes << "\n";
  uint64_t count = 0, prev = 0;
  check(s.visitFrozen([&](const Event& e) {
    if (e.sequence <= prev || e.data[0] != e.sequence * 100) return false;
    prev = e.sequence; ++count; return true; }), "maxcap frozen visit");
  check(count == pre && prev == MaxHistoryEvents, "maxcap visit count and last sequence");
}
} // namespace

int main() {
  try {
    testEmptySummary();
    testHappyPathWithTrigger();
    testPreRingOverwriteEviction();
    testVisitorFailureAndNotFrozen();
    testCorruptTailRecordNoPartialCommit();
    testOrdinalSkipAndReplay();
    testWrongSessionAndCapacityChange();
    testTriggerRegressedAndChanged();
    testSealCutAndTotalsClosure();
    testPostOverflowNoPartialCommit();
    testTerminalStatesAndFaultPersistence();
    testNewObjectRecovery();
    testWireLevelNegatives();
    testFixedCutMustRepeatExactly();
    testVisitorReentryFailsVisit();
    testAllocationFailure();
    testMaxCapacity();
    std::cout << "recorder history store checks=" << g_checks << " PASS\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "recorder history store FAIL: " << e.what() << "\n";
    return 1;
  }
}
