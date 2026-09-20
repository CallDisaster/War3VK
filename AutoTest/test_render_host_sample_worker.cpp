#include "../tools/render_host/owned_child.h"
#include "../tools/render_host/shared_slots.h"
#include "../tools/render_host/slot_channel.h"
#include "../tools/render_host/sample_envelope.h"
#include "render_host_handle_probe.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <functional>
#include <process.h>
#include <vector>

// Actual CPU lab worker, not a game-facing service. Test barriers are OUTSIDE
// tryPush; only the I/O worker touches the pipe, mapping, or child process.
using namespace warvk::host;
static_assert(sizeof(void*) == 4, "Real Win32 producer required");
namespace {
void Check(bool value) { if (!value) throw std::runtime_error("sample fixture assertion"); }
// Explicit actual-thread settlement for this Windows laboratory fixture. No
// timeout is permission to free the callback. The outer process watchdog owns
// pathological non-settlement; these joins never run on a game thread.
class Thread final {
public:
  Thread() = default;
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  ~Thread() {
    if (m_handle && WaitForSingleObject(m_handle.get(), INFINITE) != WAIT_OBJECT_0) std::terminate();
  }
  void start(std::function<void()> work) {
    Check(!m_handle); m_work = std::move(work);
    m_handle.reset(reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Entry, this, 0, nullptr)));
    Check(bool(m_handle));
  }
  bool joinable() const noexcept { return bool(m_handle); }
  void join() {
    Check(m_handle && WaitForSingleObject(m_handle.get(), INFINITE) == WAIT_OBJECT_0);
    DWORD code = 0; Check(GetExitCodeThread(m_handle.get(), &code) && code == 0);
    m_handle.reset();
  }
private:
  static unsigned __stdcall Entry(void* opaque) noexcept {
    try { static_cast<Thread*>(opaque)->m_work(); return 0; } catch (...) { return 9; }
  }
  std::function<void()> m_work;
  lab::Handle m_handle;
};
uint64_t Qpc() { LARGE_INTEGER q{}; Check(QueryPerformanceCounter(&q)); return q.QuadPart; }
uint64_t Frequency() { LARGE_INTEGER q{}; Check(QueryPerformanceFrequency(&q)); return q.QuadPart; }
struct Record { uint64_t frame; uint32_t attempt, bytes; };
struct Ack { Record source; slots::Key key; lab::Digest hash; };
void Pattern(uint8_t* out, uint32_t bytes, uint64_t frame, uint32_t attempt) {
  for (uint32_t i = 0; i < bytes; ++i) out[i] = uint8_t(i * 17ull + (i >> 8) * 3ull + frame * 13 + attempt * 19);
}
struct Signals {
  std::atomic<bool> ready{false}, done{false}, abort{false};
  std::atomic<unsigned> go{0}, phase{0}, acknowledged{0};
};
struct WireClient {
  HANDLE pipe, peer; ipc::Nonce nonce; lab::IoStats io; uint64_t sequence = 0;
  std::array<uint8_t, 128> exchange(slotwire::Op op, const uint8_t* data, uint32_t bytes, uint32_t expected) {
    Check(sequence < UINT64_MAX && expected <= slotwire::MaxPayloadBytes);
    slotwire::Header h{op, 0, bytes, nonce, ++sequence};
    slotwire::Packet packet{}; size_t count = 0;
    Check(slotwire::Encode(h, data, bytes, packet.data(), packet.size(), count) == slotwire::Error::None);
    lab::WriteBytes(pipe, peer, packet.data(), count, io);
    Check(slotwire::ReadPacket(pipe, peer, packet, count, io) == slotwire::Error::None);
    slotwire::View v;
    Check(slotwire::Decode(packet.data(), count, v) == slotwire::Error::None);
    Check(v.header.op == op && v.header.flags == 1 && v.header.sequence == sequence &&
      v.header.nonce == nonce && v.header.payloadBytes == expected);
    std::array<uint8_t, 128> result{};
    if (expected) std::memcpy(result.data(), v.payload, expected);
    return result;
  }
  void echo(slotwire::Op op, const uint8_t* data = nullptr, uint32_t bytes = 0) {
    const auto result = exchange(op, data, bytes, bytes);
    Check(!bytes || !std::memcmp(data, result.data(), bytes));
  }
};
struct WorkerResult {
  std::vector<Ack> written;
  std::vector<Ack> acks;
  DWORD pid = 0, exit = STILL_ACTIVE;
  uint64_t creation = 0;
  unsigned failedInFlight = 0;
  bool fault = false, completed = false, settled = false;
  lab::SlotIoStats copies;
  const char* failure = "None";
};
bool Is(const std::wstring& mode, const wchar_t* text) { return mode == text; }
void RunWorker(const std::wstring& executable, const std::wstring& log, const std::wstring& mode,
               inbox::Scope scope, inbox::ProducerInbox& queue, Signals& signal, WorkerResult& result) noexcept {
  bool inFlight = false;
  auto fault = [&](const char* text) {
    result.fault = true; result.failure = text;
    result.failedInFlight = inFlight ? 1 : 0;
    queue.requestStop();
  };
  try {
    lab::SharedSlots mapping(scope.connection, lab::MappingRole::OwnerWriter);
    auto args = lab::NonceText(scope.connection) + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
      std::to_wstring(lab::CreationTime(GetCurrentProcess()));
    if (!Is(mode, L"p3-to-p2")) args += L" " + (Is(mode, L"slow") || Is(mode, L"disconnect") ? mode : L"normal");
    lab::OwnedChild child(executable, args, log);
    result.pid = child.pid(); result.creation = child.creation();
    try {
      auto pipe = lab::Connect(scope.connection, child.process(), child.pid(), lab::Channel::Slots1);
      WireClient wire{pipe.get(), child.process(), scope.connection, {}};
      const auto capability = Is(mode, L"p2-to-p3") ? slotwire::SharedCpuSlots :
        slotwire::SharedCpuSlots | slotwire::EnvelopeSamples;
      const auto hello = slotwire::Hello(32, 64, capability), expected = slotwire::Hello(64, 32, capability);
      auto reply = wire.exchange(slotwire::Op::Hello, hello.data(), hello.size(), expected.size());
      Check(!std::memcmp(reply.data(), expected.data(), expected.size()));
      std::array<uint8_t, 16> begin{};
      ipc::WriteLe(begin.data(), scope.map, 8); ipc::WriteLe(begin.data() + 8, scope.device, 8);
      wire.echo(slotwire::Op::Begin, begin.data(), begin.size());
      signal.ready.store(true, std::memory_order_release);
      inbox::Sample source; lab::Sample bytes{};
      uint64_t transfer = 0, previousAttempt = 0;
      for (;;) {
        if (signal.abort.load(std::memory_order_acquire)) throw std::runtime_error("test coordinator aborted");
        const auto pop = queue.tryPop(source);
        if (pop == inbox::Pop::Finished) break;
        if (pop == inbox::Pop::Empty) { Sleep(1); continue; } // Worker only, never producer hot path.
        inFlight = true;
        Check(source.scope == scope && transfer < UINT64_MAX);
        ++transfer;
        size_t count = 0;
        Check(envelope::Encode({scope, source.frame, source.ordinal, transfer}, source.payload.data(), source.bytes,
          bytes.data(), bytes.size(), count) == envelope::Error::None);
        if (transfer == 3) { // Deliberately keep SHA correct: test the HOST envelope check itself.
          if (Is(mode, L"wrong-scope")) ipc::WriteLe(bytes.data() + 40, scope.map + 1, 8);
          if (Is(mode, L"wrong-transfer")) ipc::WriteLe(bytes.data() + 72, transfer + 1, 8);
          if (Is(mode, L"repeat-attempt")) ipc::WriteLe(bytes.data() + 64, previousAttempt, 8);
          if (Is(mode, L"source-backward")) ipc::WriteLe(bytes.data() + 56, 1, 8);
          if (Is(mode, L"length")) ipc::WriteLe(bytes.data() + 20, source.bytes + 1, 4);
        }
        previousAttempt = source.ordinal;
        std::array<uint8_t, 16> reserve{};
        ipc::WriteLe(reserve.data(), transfer, 8); ipc::WriteLe(reserve.data() + 8, count, 4);
        reply = wire.exchange(slotwire::Op::Reserve, reserve.data(), reserve.size(), slots::DescriptorBytes);
        slots::Key key;
        Check(slots::DecodeKey(reply.data(), slots::DescriptorBytes, key) == slots::Error::None);
        Check(key.connection == scope.connection && key.map == scope.map && key.device == scope.device &&
          key.frame == transfer && key.bytes == count);
        mapping.writeOnce(key, bytes.data(), count, child.process());
        const auto hash = lab::Sha256(bytes.data(), count);
        result.written.push_back({{source.frame, source.ordinal, source.bytes}, key, hash});
        std::array<uint8_t, 96> publish{};
        Check(slots::EncodeKey(key, publish.data(), publish.size()) == slots::Error::None);
        std::memcpy(publish.data() + slots::DescriptorBytes, hash.data(), hash.size());
        wire.echo(slotwire::Op::Publish, publish.data(), publish.size());
        result.acks.push_back({{source.frame, source.ordinal, source.bytes}, key, hash});
        inFlight = false;
        signal.acknowledged.store(unsigned(result.acks.size()), std::memory_order_release);
        if (transfer == 1 && !Is(mode, L"normal") && !Is(mode, L"slow")) {
          // Fixture-only barrier: make the fault population deterministic. The
          // producer fills its three-entry batch before host consumes sample 2.
          const auto until = GetTickCount64() + 10000;
          while (signal.phase.load(std::memory_order_acquire) < 2) {
            Check(!signal.abort.load(std::memory_order_acquire) && GetTickCount64() < until); Sleep(1);
          }
        }
      }
      wire.echo(slotwire::Op::End); wire.echo(slotwire::Op::Close);
      result.completed = true;
    } catch (const lab::Failure& e) { fault(lab::FaultName(e.fault));
    } catch (...) { fault("ProtocolOrFixture"); }
    result.exit = child.wait(7000); result.settled = true; result.copies = mapping.stats();
  } catch (const lab::Failure& e) { fault(lab::FaultName(e.fault));
  } catch (...) { fault("SetupOrSettlement"); }
  signal.done.store(true, std::memory_order_release);
}
bool WaitGo(Signals& s, unsigned n) {
  const auto until = GetTickCount64() + 10000;
  while (s.go.load(std::memory_order_acquire) < n) {
    if (s.done.load(std::memory_order_acquire) || s.abort.load(std::memory_order_acquire)) return false;
    Check(GetTickCount64() < until); Sleep(1);
  }
  return true;
}
bool DelayStarted(const std::wstring& path) {
  lab::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) return false;
  char bytes[16384]{}; DWORD read = 0;
  Check(ReadFile(file.get(), bytes, sizeof(bytes) - 1, &read, nullptr));
  return std::strstr(bytes, "\"delayStart\":") != nullptr;
}
void Gone(ipc::Nonce nonce) {
  lab::Handle m(OpenFileMappingW(FILE_MAP_READ, FALSE, lab::SharedSlots::MappingName(nonce).c_str()));
  Check(!m && GetLastError() == ERROR_FILE_NOT_FOUND);
  for (uint32_t i = 0; i < slots::SlotCount; ++i) {
    lab::Handle h(OpenMutexW(SYNCHRONIZE, FALSE, lab::SharedSlots::MutexName(nonce, i).c_str()));
    Check(!h && GetLastError() == ERROR_FILE_NOT_FOUND);
  }
}
void Run(const std::wstring& exe, const std::wstring& log, const std::wstring& mode, unsigned cycle, DWORD expectedHandles) {
  const inbox::Scope scope{lab::RandomNonce(), 101 + cycle, 201 + cycle};
  auto queue = std::make_unique<inbox::ProducerInbox>(scope, envelope::InboxLimits);
  Signals signal; WorkerResult worker; worker.acks.reserve(32); worker.written.reserve(32);
  std::vector<Record> published; published.reserve(32);
  uint64_t batchStart = 0, batchEnd = 0; bool producerError = false;
  Thread io;
  io.start([&] { RunWorker(exe, log, mode, scope, *queue, signal, worker); });
  Thread producer;
  try {
    producer.start([&] {
      try {
        lab::Sample payload{}; uint32_t ordinal = 0;
        auto push = [&](uint64_t frame, uint32_t bytes) {
          Pattern(payload.data(), bytes, frame, ordinal + 1);
          const auto r = queue->tryPush(frame, payload.data(), bytes);
          if (r.ordinal) Check(r.ordinal == ++ordinal);
          if (r.outcome == inbox::Push::Published) published.push_back({frame, r.ordinal, bytes});
          return r.outcome;
        };
        while (!signal.ready.load(std::memory_order_acquire) && !signal.done.load(std::memory_order_acquire)) Sleep(1);
        if (signal.ready.load(std::memory_order_acquire)) {
          Check(push(7000, 97) == inbox::Push::Published); signal.phase.store(1, std::memory_order_release);
          if (WaitGo(signal, 1)) {
            if (Is(mode, L"slow")) {
              batchStart = Qpc();
              for (unsigned i = 0; i < 1000; ++i) push(7001, 97);
              batchEnd = Qpc(); signal.phase.store(2, std::memory_order_release);
              if (WaitGo(signal, 2)) {
                Check(push(7002, envelope::MaxPayloadBytes) == inbox::Push::Published);
                Check(push(0, 1) == inbox::Push::Invalid);
                Check(push(7002, 1) == inbox::Push::Published);
              }
            } else {
              const unsigned rounds = Is(mode, L"normal") ? 8 : 1;
              for (unsigned r = 0; r < rounds; ++r) {
                if (r && !WaitGo(signal, r + 1)) break;
                Check(push(0, 1) == inbox::Push::Invalid);
                for (auto size : {1u, 65456u, 97u})
                  Check(push(7000 + r, size) == inbox::Push::Published);
                signal.phase.store(r + 2, std::memory_order_release);
              }
            }
          }
        }
      } catch (...) { producerError = true; signal.abort.store(true, std::memory_order_release); }
      queue->closeProducer();
    });
    const auto until = GetTickCount64() + 15000;
    while (!signal.done.load(std::memory_order_acquire)) {
      Check(GetTickCount64() < until);
      auto phase = signal.phase.load(std::memory_order_acquire);
      auto acks = signal.acknowledged.load(std::memory_order_acquire);
      if (Is(mode, L"slow")) {
        if (phase == 1 && DelayStarted(log)) signal.go.store(1, std::memory_order_release);
        if (phase == 2 && acks >= 5) signal.go.store(2, std::memory_order_release);
      } else if (phase && acks >= 1 + (phase - 1) * 3) signal.go.store(phase, std::memory_order_release);
      Sleep(1);
    }
  } catch (...) {
    signal.abort.store(true, std::memory_order_release); queue->requestStop();
    if (producer.joinable()) producer.join();
    io.join();
    throw;
  }
  producer.join(); io.join(); Gone(scope.connection);
  DWORD handles = 0; Check(GetProcessHandleCount(GetCurrentProcess(), &handles));
  ProbeOwnHandles("after-cycle");
  const auto c = queue->countersAfterJoin();
  Check(!producerError && worker.settled && c.closed && published.size() == c.published);
  Check(c.attempted == c.published + c.full + c.invalid && c.published == worker.acks.size() + worker.failedInFlight + c.queued);
  Check(c.popped == worker.acks.size() + worker.failedInFlight);
  for (auto row : published) std::printf("{\"produced\":true,\"sourceFrame\":%llu,\"attempt\":%u,\"payloadBytes\":%u}\n",
    (unsigned long long)row.frame, row.attempt, row.bytes);
  const auto receipt = [](const char* kind, const Ack& row) {
    std::printf("{\"%s\":true,\"sourceFrame\":%llu,\"attempt\":%u,\"payloadBytes\":%u,\"transfer\":%llu,"
      "\"slot\":%u,\"generation\":%llu,\"bytes\":%u,\"sha256\":\"", kind, (unsigned long long)row.source.frame,
      row.source.attempt, row.source.bytes, (unsigned long long)row.key.frame, row.key.slot,
      (unsigned long long)row.key.generation, row.key.bytes);
    for (auto b : row.hash) std::printf("%02x", unsigned(b));
    std::printf("\"}\n");
  };
  for (const auto& row : worker.written) receipt("written", row);
  for (const auto& row : worker.acks) receipt("ack", row);
  std::printf("{\"cycle\":%u,\"mode\":\"%ls\",\"bits\":32,\"nonce\":\"%ls\",\"map\":%llu,\"device\":%llu,"
    "\"attempted\":%u,\"published\":%u,\"full\":%u,\"invalid\":%u,\"popped\":%u,\"queued\":%u,"
    "\"acknowledged\":%u,\"failedInFlight\":%u,\"stopped\":%s,\"completed\":%s,\"fault\":%s,\"failure\":\"%s\","
    "\"childPid\":%lu,\"childCreation\":%llu,\"childExit\":%lu,\"settled\":true,\"containersGone\":true,\"handles\":%lu,"
    "\"batchStart\":%llu,\"batchEnd\":%llu,\"frequency\":%llu,\"writes\":%llu,\"writtenBytes\":%llu}\n",
    cycle, mode.c_str(), lab::NonceText(scope.connection).c_str(), (unsigned long long)scope.map, (unsigned long long)scope.device,
    c.attempted, c.published, c.full, c.invalid, c.popped, c.queued, unsigned(worker.acks.size()), worker.failedInFlight,
    c.stopped ? "true" : "false", worker.completed ? "true" : "false", worker.fault ? "true" : "false", worker.failure,
    (unsigned long)worker.pid, (unsigned long long)worker.creation, (unsigned long)worker.exit, (unsigned long)handles,
    (unsigned long long)batchStart, (unsigned long long)batchEnd, (unsigned long long)Frequency(),
    (unsigned long long)worker.copies.writes, (unsigned long long)worker.copies.writtenBytes);
  std::fflush(stdout);
  Check(handles == expectedHandles);
}
}
int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && !std::wcscmp(argv[1], L"--dependency-init")) return 0;
  try {
    Check(argc == 4);
    const std::wstring mode = argv[2];
    bool known = false;
    for (auto name : {L"normal", L"slow", L"disconnect", L"recovery", L"soak", L"p2-to-p3", L"p3-to-p2",
      L"p3-to-p2-soak", L"p2-to-p3-soak",
      L"wrong-scope", L"wrong-transfer", L"repeat-attempt", L"source-backward", L"length"}) known |= mode == name;
    Check(known);
    // Separate first-use dependency initialization from the resource-leak gate.
    DWORD initBefore = 0, initAfter = 0, initPid = 0; uint64_t initCreation = 0;
    Check(GetProcessHandleCount(GetCurrentProcess(), &initBefore));
    for (unsigned i = 0; i < 8; ++i) { lab::RandomNonce(); lab::Sha256(reinterpret_cast<const uint8_t*>("a"), 1); }
    {
      lab::OwnedChild warm(argv[0], L"--dependency-init", std::wstring(argv[3]) + L".init");
      initPid = warm.pid(); initCreation = warm.creation();
      Check(warm.wait(7000) == 0);
    }
    {
      const auto nonce = lab::RandomNonce();
      auto pipe = lab::CreateServer(nonce, lab::Channel::Slots1);
      lab::SharedSlots owner(nonce, lab::MappingRole::OwnerWriter);
      lab::SharedSlots reader(nonce, lab::MappingRole::Reader);
      Check(owner.protection() == PAGE_READWRITE && reader.protection() == PAGE_READONLY);
    }
    Thread warm;
    warm.start([] { for (unsigned i = 0; i < 72; ++i) {
      try { throw lab::Failure(lab::Fault::SlotLease, lab::Stage::SlotCopy); }
      catch (const lab::Failure& e) { Check(e.fault == lab::Fault::SlotLease); }
    } });
    warm.join();
    // Exact-image launch initialization, separately receipted. The empty-arg
    // child must exit before touching IPC; repeats must not grow handles. This
    // is not a reconnect, renderer restart, or discarded failed protocol run.
    std::array<DWORD, 3> imageHandles{};
    std::array<DWORD, 2> imagePids{}, imageExits{};
    std::array<uint64_t, 2> imageCreations{};
    Check(GetProcessHandleCount(GetCurrentProcess(), &imageHandles[0]));
    for (unsigned i = 0; i < 2; ++i) {
      {
        lab::OwnedChild init(argv[1], L"", std::wstring(argv[3]) + L".host-init-" + std::to_wstring(i));
        imagePids[i] = init.pid(); imageCreations[i] = init.creation(); imageExits[i] = init.wait(7000);
        Check(imageExits[i] == (mode.find(L"p3-to-p2") == 0 ? 2u : 3u));
      }
      Check(GetProcessHandleCount(GetCurrentProcess(), &imageHandles[i + 1]));
    }
    Check(imageHandles[1] == imageHandles[2]);
    Check(GetProcessHandleCount(GetCurrentProcess(), &initAfter));
    DWORD before = 0, after = 0; Check(GetProcessHandleCount(GetCurrentProcess(), &before));
    ProbeOwnHandles("baseline");
    const bool mismatchSoak = Is(mode, L"p3-to-p2-soak") || Is(mode, L"p2-to-p3-soak");
    const unsigned cycles = Is(mode, L"soak") || mismatchSoak ? 16 : Is(mode, L"recovery") ? 2 : 1;
    for (unsigned i = 0; i < cycles; ++i) {
      const auto part = mismatchSoak ? mode.substr(0, 8) : Is(mode, L"soak") ? (i % 2 ? L"disconnect" : L"normal") :
        Is(mode, L"recovery") ? (i ? L"normal" : L"disconnect") : mode;
      const auto log = std::wstring(argv[3]) + (i ? L".cycle-" + std::to_wstring(i) : L"");
      Run(argv[1], log, part, i, before);
    }
    Check(GetProcessHandleCount(GetCurrentProcess(), &after));
    std::printf("{\"summary\":true,\"handlesBefore\":%lu,\"handlesAfter\":%lu,\"initBefore\":%lu,\"initAfter\":%lu,"
      "\"initPid\":%lu,\"initCreation\":%llu,\"imageHandles\":[%lu,%lu,%lu],\"imageInit\":["
      "{\"pid\":%lu,\"creation\":%llu,\"exit\":%lu},{\"pid\":%lu,\"creation\":%llu,\"exit\":%lu}]}\n",
      (unsigned long)before, (unsigned long)after, (unsigned long)initBefore, (unsigned long)initAfter,
      (unsigned long)initPid, (unsigned long long)initCreation,
      (unsigned long)imageHandles[0], (unsigned long)imageHandles[1], (unsigned long)imageHandles[2],
      (unsigned long)imagePids[0], (unsigned long long)imageCreations[0], (unsigned long)imageExits[0],
      (unsigned long)imagePids[1], (unsigned long long)imageCreations[1], (unsigned long)imageExits[1]);
    Check(before == after);
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 2;
  } catch (...) { return 3; }
}
