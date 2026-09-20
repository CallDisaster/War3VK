// E2 recorder host helper: the 64-bit CPU history consumer for the laboratory
// 32->64 shared-slot flow. argv[1..3] follow RH1 (nonce, parent pid, parent
// creation); argv[4] is the helper mode: normal | slow | disconnect.
// It owns the only HistoryStore, validates every WVE1 packet against the slot
// lease (key.frame == packet ordinal) and the deterministic lab fixture, and
// after a successful runtime prints exactly one recorderHost JSON receipt.
// Never a game capture path; no GPU, no product linkage, no second big history.
#include "slot_host_runtime.h"
#include "process_memory_probe.h"
#include "recorder_history_store.h"
#include "recorder_lab_fixture.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>

static_assert(sizeof(void*) == 8, "E2 recorder host must be an actual 64-bit executable");

namespace {
using namespace warvk::host;
using warvk::host::recorder::Event;
using warvk::host::recorder::HistoryStore;
using warvk::host::recorder::StoreError;
using warvk::host::recorder::StoreState;

enum class Mode { Normal, Slow, Disconnect, Invalid };
Mode ParseMode(const wchar_t* text) {
  if (!text) return Mode::Invalid;
  if (!std::wcscmp(text, L"normal")) return Mode::Normal;
  if (!std::wcscmp(text, L"slow")) return Mode::Slow;
  if (!std::wcscmp(text, L"disconnect")) return Mode::Disconnect;
  return Mode::Invalid;
}
const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::Normal: return "normal";
    case Mode::Slow: return "slow";
    case Mode::Disconnect: return "disconnect";
    default: return "invalid";
  }
}
uint64_t QpcNow() { LARGE_INTEGER t{}; return QueryPerformanceCounter(&t) ? uint64_t(t.QuadPart) : 0; }

// Streaming SHA-256 (BCrypt) over bounded chunks; never holds a second copy
// of the history.
class Sha256Stream {
public:
  Sha256Stream() {
    if (BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
    if (BCryptCreateHash(m_algorithm, &m_hash, nullptr, 0, nullptr, 0, 0) != 0) {
      BCryptCloseAlgorithmProvider(m_algorithm, 0);
      m_algorithm = nullptr;
      return;
    }
    m_ok = true;
  }
  Sha256Stream(const Sha256Stream&) = delete;
  Sha256Stream& operator=(const Sha256Stream&) = delete;
  ~Sha256Stream() {
    if (m_hash) BCryptDestroyHash(m_hash);
    if (m_algorithm) BCryptCloseAlgorithmProvider(m_algorithm, 0);
  }
  bool ok() const { return m_ok; }
  bool add(const uint8_t* bytes, size_t count) {
    if (!m_ok) return false;
    if (BCryptHashData(m_hash, const_cast<uint8_t*>(bytes), static_cast<ULONG>(count), 0) != 0) {
      m_ok = false;
      return false;
    }
    return true;
  }
  bool finish(std::array<uint8_t, 32>& out) {
    if (!m_ok) return false;
    if (BCryptFinishHash(m_hash, out.data(), static_cast<ULONG>(out.size()), 0) != 0) {
      m_ok = false;
      return false;
    }
    return true;
  }
private:
  BCRYPT_ALG_HANDLE m_algorithm = nullptr;
  BCRYPT_HASH_HANDLE m_hash = nullptr;
  bool m_ok = false;
};

class RecorderConsumer final : public lab::SlotPayloadConsumer {
public:
  explicit RecorderConsumer(Mode mode) : m_mode(mode) {
    LARGE_INTEGER f{};
    if (QueryPerformanceFrequency(&f)) m_qpcFrequency = uint64_t(f.QuadPart);
  }
  uint64_t capabilities() const noexcept override {
    return slotwire::SharedCpuSlots | slotwire::RecorderEvents; // profile 10, exact both directions
  }
  const char* begin(ipc::Nonce nonce, uint64_t map, uint64_t device) override {
    if (map != recorder::fixture::Map || device != recorder::fixture::Device) return "RecorderIdentity";
    if (m_beginSeen) return "RecorderStore"; // one epoch per connection
    m_nonce = nonce;
    m_beginSeen = true;
    return nullptr;
  }
  const char* consume(const slots::ReadPermit& permit, const uint8_t* bytes, size_t count) override {
    // The slot payload is one WVE1 packet; key.frame is its packet ordinal.
    if (!m_beginSeen || !permit.active() || !(permit.key().connection == m_nonce) ||
        permit.key().map != recorder::fixture::Map || permit.key().device != recorder::fixture::Device ||
        permit.key().bytes != count) return "RecorderLease";
    recorder::View view;
    m_outerWireError = recorder::Decode(bytes, count, view);
    if (m_outerWireError != recorder::WireError::None) return "RecorderWire";
    if (view.header.ordinal != permit.key().frame) return "RecorderLease";
    if (view.header.session != recorder::fixture::Session) return "RecorderIdentity";
    // Every record must match the deterministic lab fixture field by field.
    for (uint32_t i = 0; i < view.header.count; ++i) {
      Event e;
      if (recorder::DecodeEvent(view.events + size_t(i) * recorder::EventBytes,
                                recorder::EventBytes, e) != recorder::WireError::None)
        return "RecorderWire";
      if (!recorder::fixture::ContentMatches(e)) return "RecorderIdentity";
    }
    if (view.header.op == recorder::Op::Data) {
      ++m_dataPackets;
      if (m_mode == Mode::Disconnect && m_dataPackets == 3) return "InjectedRecorderDisconnect";
      if (m_mode == Mode::Slow && !m_delayDone) {
        // The producer watches these two lines for the actual blocked window.
        m_delayDone = true;
        m_delayStart = QpcNow();
        if (!m_delayStart || !m_qpcFrequency) return "RecorderQpc";
        std::printf("{\"delayStart\":%llu,\"frequency\":%llu}\n",
          (unsigned long long)m_delayStart, (unsigned long long)m_qpcFrequency);
        std::fflush(stdout);
        Sleep(1200);
        m_delayEnd = QpcNow();
        if (!m_delayEnd) return "RecorderQpc";
        std::printf("{\"delayEnd\":%llu,\"frequency\":%llu}\n",
          (unsigned long long)m_delayEnd, (unsigned long long)m_qpcFrequency);
        std::fflush(stdout);
      }
    }
    const StoreError error = m_store.accept(bytes, count); // the store self-decodes
    if (error == StoreError::None) {
      if (view.header.op == recorder::Op::Begin) {
        m_allocated = lab::ProbeOwnProcessMemory(); // the real history allocation happened here
        m_allocatedValid = true;
      }
      return nullptr;
    }
    if (error == StoreError::WireDecode) return "RecorderWire";
    if (error == StoreError::SessionMismatch) return "RecorderIdentity";
    return "RecorderStore";
  }
  const char* end() override {
    // End requires a legal Seal (store Frozen); anything else is explicit.
    if (m_store.summary().state != StoreState::Frozen) return "RecorderMissingSeal";
    m_endSeen = true;
    return nullptr;
  }
  const char* close() override {
    // Close additionally requires a successful End.
    if (!m_endSeen || m_store.summary().state != StoreState::Frozen) return "RecorderMissingSeal";
    m_closeSeen = true;
    return nullptr;
  }
  const HistoryStore& store() const { return m_store; }
  bool beginSeen() const { return m_beginSeen; }
  bool endSeen() const { return m_endSeen; }
  bool closeSeen() const { return m_closeSeen; }
  bool allocatedValid() const { return m_allocatedValid; }
  const lab::ProcessMemory& allocated() const { return m_allocated; }
  uint64_t delayStart() const { return m_delayStart; }
  uint64_t delayEnd() const { return m_delayEnd; }
  uint64_t qpcFrequency() const { return m_qpcFrequency; }
  recorder::WireError outerWireError() const { return m_outerWireError; }
private:
  ipc::Nonce m_nonce{};
  recorder::WireError m_outerWireError = recorder::WireError::None;
  Mode m_mode;
  HistoryStore m_store;
  bool m_beginSeen = false, m_endSeen = false, m_closeSeen = false;
  uint32_t m_dataPackets = 0;
  bool m_delayDone = false;
  uint64_t m_delayStart = 0, m_delayEnd = 0, m_qpcFrequency = 0;
  bool m_allocatedValid = false;
  lab::ProcessMemory m_allocated{};
};
} // namespace

int wmain(int argc, wchar_t** argv) {
  // Match the existing laboratory helper's pre-IPC invalid-argv convention.
  if (argc != 5 || ParseMode(argv[4]) == Mode::Invalid) return 3;
  const lab::ProcessMemory memoryBefore = lab::ProbeOwnProcessMemory(); // cold checkpoint
  Mode mode = Mode::Invalid;
  if (argc == 5) mode = ParseMode(argv[4]);
  RecorderConsumer consumer(mode);
  int runtimeExit = 2;
  if (mode != Mode::Invalid)
    runtimeExit = lab::RunSlotHost(4, argv, &consumer); // argv[1..3] follow RH1
  const HistoryStore& store = consumer.store();
  const auto summary = store.summary();

  // Only a successful runtime with a legally Frozen store may be visited; a
  // failed runtime never yields a usable digest.
  uint64_t visited = 0, firstSequence = 0, lastVisited = 0;
  bool contentValid = false, digestOk = false;
  std::array<uint8_t, 32> digest{};
  if (runtimeExit == 0 && consumer.endSeen() && consumer.closeSeen() &&
      summary.state == StoreState::Frozen) {
    Sha256Stream sha;
    if (sha.ok()) {
      bool allMatch = true;
      const bool visitOk = store.visitFrozen([&](const Event& e) {
        if (!recorder::fixture::ContentMatches(e)) { allMatch = false; return false; }
        // Re-encode each record with the real codec and hash only the 392-byte
        // WVE1 event region (no 80-byte header, no C++ padding), bounded chunks.
        recorder::Header single{};
        single.op = recorder::Op::Data;
        single.count = 1;
        single.session = e.session;
        single.ordinal = 1;
        single.capacity = 4;
        uint8_t packet[recorder::HeaderBytes + recorder::EventBytes];
        size_t written = 0;
        if (recorder::Encode(single, &e, packet, sizeof packet, written) != recorder::WireError::None ||
            written != sizeof packet) return false;
        if (!sha.add(packet + recorder::HeaderBytes, recorder::EventBytes)) return false;
        if (visited == 0) firstSequence = e.sequence;
        lastVisited = e.sequence;
        ++visited;
        return true;
      });
      if (visitOk && allMatch && visited > 0 && visited == summary.retained &&
          lastVisited - firstSequence + 1 == visited && sha.finish(digest)) {
        contentValid = true;
        digestOk = true;
      }
    }
  }
  if (!digestOk) { visited = 0; firstSequence = 0; lastVisited = 0; }
  const bool cpuComplete = runtimeExit == 0 && consumer.beginSeen() && consumer.endSeen() &&
    consumer.closeSeen() && summary.state == StoreState::Frozen && contentValid &&
    summary.lost == 0;
  const lab::ProcessMemory memoryAllocated =
    consumer.allocatedValid() ? consumer.allocated() : memoryBefore; // never a fake allocation
  const lab::ProcessMemory memoryActive = lab::ProbeOwnProcessMemory();

  std::printf("{\"recorderHost\":true,\"bits\":64,\"mode\":\"%s\",\"session\":%llu,"
    "\"state\":%u,\"nextOrdinal\":%llu,\"lastSequence\":%llu,\"trigger\":%llu,"
    "\"capacity\":%u,\"accepted\":%llu,\"attempted\":%llu,\"lost\":%llu,"
    "\"evicted\":%llu,\"retained\":%llu,\"reason\":%u,\"storageBytes\":%llu,"
    "\"wireError\":%u,\"storeError\":%u,\"runtimeExit\":%d,"
    "\"beginSeen\":%s,\"endSeen\":%s,\"closeSeen\":%s,"
    "\"visited\":%llu,\"firstSequence\":%llu,\"lastVisited\":%llu,\"digest\":\"",
    ModeName(mode),
    (unsigned long long)summary.session, unsigned(summary.state),
    (unsigned long long)summary.nextOrdinal, (unsigned long long)summary.lastSequence,
    (unsigned long long)summary.trigger, unsigned(summary.capacity),
    (unsigned long long)summary.accepted, (unsigned long long)summary.attempted,
    (unsigned long long)summary.lost, (unsigned long long)summary.evicted,
    (unsigned long long)summary.retained, unsigned(summary.reason),
    (unsigned long long)summary.storageBytes, unsigned(consumer.outerWireError() != recorder::WireError::None ?
      consumer.outerWireError() : summary.wireError),
    unsigned(summary.storeError), runtimeExit,
    consumer.beginSeen() ? "true" : "false", consumer.endSeen() ? "true" : "false",
    consumer.closeSeen() ? "true" : "false",
    (unsigned long long)visited, (unsigned long long)firstSequence,
    (unsigned long long)lastVisited);
  if (digestOk) for (auto byte : digest) std::printf("%02x", unsigned(byte));
  std::printf("\",\"contentValid\":%s,\"cpuComplete\":%s,"
    "\"delayStartQpc\":%llu,\"delayEndQpc\":%llu,\"qpcFrequency\":%llu,"
    "\"memoryBefore\":", contentValid ? "true" : "false", cpuComplete ? "true" : "false",
    (unsigned long long)consumer.delayStart(), (unsigned long long)consumer.delayEnd(),
    (unsigned long long)consumer.qpcFrequency());
  lab::PrintProcessMemory(memoryBefore);
  std::printf(",\"memoryAllocated\":");
  lab::PrintProcessMemory(memoryAllocated);
  std::printf(",\"memoryActive\":");
  lab::PrintProcessMemory(memoryActive);
  std::printf("}\n");
  std::fflush(stdout);
  return runtimeExit;
}
