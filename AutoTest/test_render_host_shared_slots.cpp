#include "../tools/render_host/owned_child.h"
#include "../tools/render_host/shared_slots.h"
#include "../tools/render_host/slot_channel.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

using namespace warvk::host;
static_assert(sizeof(void*) == 4, "RH1 requires an actual Win32 producer");
namespace {
void CheckAt(bool valid, unsigned line) {
  if (!valid) throw std::runtime_error("RH1 client assertion line=" + std::to_string(line));
}
#define Check(x) CheckAt(bool(x), __LINE__)
lab::Sample Pattern(const slots::Key& key) {
  lab::Sample result{};
  for (uint32_t i = 0; i < key.bytes; ++i)
    result[i] = uint8_t(i * 17ull + (i >> 8) * 3ull + key.frame * 13 + key.map * 7 + key.device * 19);
  return result;
}
struct Client {
  HANDLE pipe, peer;
  ipc::Nonce nonce;
  lab::IoStats io;
  uint64_t sequence = 0, map = 0, device = 0, frame = 0;
  unsigned exchanges = 0;
  size_t fragment = slotwire::MaxMessageBytes;
  slotwire::Header header(slotwire::Op op, size_t bytes) {
    Check(sequence < UINT64_MAX);
    slotwire::Header h;
    h.op = op; h.payloadBytes = uint32_t(bytes); h.sequence = ++sequence; h.nonce = nonce;
    return h;
  }
  slotwire::Packet encode(const slotwire::Header& h, const uint8_t* data, size_t& size) {
    slotwire::Packet packet{};
    Check(slotwire::Encode(h, data, h.payloadBytes, packet.data(), packet.size(), size) == slotwire::Error::None);
    return packet;
  }
  void send(const slotwire::Header& h, const uint8_t* data) {
    size_t size = 0; auto packet = encode(h, data, size);
    lab::WriteBytes(pipe, peer, packet.data(), size, io, fragment);
  }
  std::array<uint8_t, 128> exchange(slotwire::Op op, const uint8_t* data, size_t bytes, size_t replyBytes) {
    const auto h = header(op, bytes); send(h, data);
    slotwire::Packet reply{}; size_t size = 0;
    Check(slotwire::ReadPacket(pipe, peer, reply, size, io) == slotwire::Error::None);
    slotwire::View view;
    Check(slotwire::Decode(reply.data(), size, view) == slotwire::Error::None);
    Check(view.header.op == op && view.header.flags == 1 && view.header.sequence == h.sequence &&
      view.header.nonce == nonce && view.header.payloadBytes == replyBytes);
    std::array<uint8_t, 128> result{};
    if (replyBytes) std::memcpy(result.data(), view.payload, replyBytes);
    ++exchanges; return result;
  }
  void hello() {
    const auto data = slotwire::Hello(32, 64), expected = slotwire::Hello(64, 32);
    const auto reply = exchange(slotwire::Op::Hello, data.data(), data.size(), expected.size());
    Check(!std::memcmp(reply.data(), expected.data(), expected.size()));
  }
  void begin(uint64_t m, uint64_t d) {
    std::array<uint8_t, 16> data{};
    ipc::WriteLe(data.data(), m, 8); ipc::WriteLe(data.data() + 8, d, 8);
    auto reply = exchange(slotwire::Op::Begin, data.data(), data.size(), data.size());
    Check(!std::memcmp(reply.data(), data.data(), data.size())); map = m; device = d; frame = 0;
  }
  slots::Key reserve(uint32_t bytes) {
    std::array<uint8_t, 16> data{}; ++frame;
    ipc::WriteLe(data.data(), frame, 8); ipc::WriteLe(data.data() + 8, bytes, 4);
    auto reply = exchange(slotwire::Op::Reserve, data.data(), data.size(), slots::DescriptorBytes);
    slots::Key key;
    Check(slots::DecodeKey(reply.data(), slots::DescriptorBytes, key) == slots::Error::None);
    Check(key.connection == nonce && key.map == map && key.device == device &&
      key.frame == frame && key.bytes == bytes);
    return key;
  }
  std::array<uint8_t, 96> publishBody(const slots::Key& key, const lab::Digest& hash) {
    std::array<uint8_t, 96> data{};
    Check(slots::EncodeKey(key, data.data(), data.size()) == slots::Error::None);
    std::memcpy(data.data() + slots::DescriptorBytes, hash.data(), hash.size()); return data;
  }
  void publish(const slots::Key& key, const lab::Digest& hash, bool read = true) {
    auto data = publishBody(key, hash);
    if (!read) { send(header(slotwire::Op::Publish, data.size()), data.data()); return; }
    auto reply = exchange(slotwire::Op::Publish, data.data(), data.size(), data.size());
    Check(!std::memcmp(reply.data(), data.data(), data.size()));
    std::printf("{\"sample\":true,\"map\":%llu,\"device\":%llu,\"frame\":%llu,"
      "\"generation\":%llu,\"slot\":%u,\"bytes\":%u,\"sha256\":\"",
      (unsigned long long)key.map, (unsigned long long)key.device, (unsigned long long)key.frame,
      (unsigned long long)key.generation, key.slot, key.bytes);
    for (auto b : hash) std::printf("%02x", unsigned(b));
    std::printf("\"}\n");
  }
  void cancel(const slots::Key& key) {
    std::array<uint8_t, 64> data{};
    Check(slots::EncodeKey(key, data.data(), data.size()) == slots::Error::None);
    auto reply = exchange(slotwire::Op::Cancel, data.data(), data.size(), data.size());
    Check(!std::memcmp(reply.data(), data.data(), data.size()));
  }
  void empty(slotwire::Op op) { exchange(op, nullptr, 0, 0); }
};
struct AbandonInput { slots::Key key; lab::Sample bytes; };
DWORD PartialWriter(const AbandonInput& input, bool release) {
  // Fault injection writes ONLY half the bytes in the real shared mapping.
  // It is not a full-size copy of a buffer whose tail was merely set to zero.
  lab::Handle mutex(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
    lab::SharedSlots::MutexName(input.key.connection, input.key.slot).c_str()));
  lab::Handle mapping(OpenFileMappingW(FILE_MAP_WRITE, FALSE,
    lab::SharedSlots::MappingName(input.key.connection).c_str()));
  if (!mutex || !mapping) return 4;
  lab::MappedView view;
  view.set(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, slots::PoolBytes));
  if (!view.get() || WaitForSingleObject(mutex.get(), 1000) != WAIT_OBJECT_0) return 5;
  std::memcpy(static_cast<uint8_t*>(view.get()) + input.key.slot * slots::SlotBytes,
    input.bytes.data(), input.key.bytes / 2);
  if (release && !ReleaseMutex(mutex.get())) return 6;
  return 0;
}
DWORD WINAPI AbandonWriter(void* opaque) {
  // Real thread death without ReleaseMutex; process and other handles survive.
  // No corruption entry point is added to SharedSlots or shipping code.
  return PartialWriter(*static_cast<AbandonInput*>(opaque), false);
}
void AssertContainerGone(ipc::Nonce nonce) {
  lab::Handle m(OpenFileMappingW(FILE_MAP_READ, FALSE, lab::SharedSlots::MappingName(nonce).c_str()));
  Check(!m && GetLastError() == ERROR_FILE_NOT_FOUND);
  for (uint32_t i = 0; i < slots::SlotCount; ++i) {
    lab::Handle h(OpenMutexW(SYNCHRONIZE, FALSE, lab::SharedSlots::MutexName(nonce, i).c_str()));
    Check(!h && GetLastError() == ERROR_FILE_NOT_FOUND);
  }
}
void FillAndPublish(Client& c, lab::SharedSlots& owner, const slots::Key& key) {
  const auto bytes = Pattern(key);
  owner.writeOnce(key, bytes.data(), key.bytes, c.peer);
  c.publish(key, lab::Sha256(bytes.data(), key.bytes));
}
void LateReused(Client& c, lab::SharedSlots& owner) {
  slots::Key first;
  for (unsigned i = 0; i < 4; ++i) {
    auto key = c.reserve(1024); if (!i) first = key;
    FillAndPublish(c, owner, key);
  }
  const auto current = c.reserve(1024);
  Check(current.slot == first.slot && current.generation > first.generation && current.frame > first.frame);
  const auto bytes = Pattern(current);
  owner.writeOnce(current, bytes.data(), current.bytes, c.peer);
  const auto old = Pattern(first);
  c.publish(first, lab::Sha256(old.data(), first.bytes), false);
}
struct Cycle {
  ipc::Nonce nonce;
  DWORD pid = 0, exit = STILL_ACTIVE, handles = 0;
  uint64_t creation = 0, wireWrites = 0;
  unsigned exchanges = 0;
  lab::SlotIoStats stats;
};
Cycle RunCycle(const wchar_t* executable, const std::wstring& log, unsigned index) {
  Cycle proof; proof.nonce = lab::RandomNonce();
  {
    lab::SharedSlots owner(proof.nonce, lab::MappingRole::OwnerWriter);
    const auto args = lab::NonceText(proof.nonce) + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
      std::to_wstring(lab::CreationTime(GetCurrentProcess()));
    lab::OwnedChild child(executable, args, log); proof.pid = child.pid(); proof.creation = child.creation();
    auto pipe = lab::Connect(proof.nonce, child.process(), child.pid(), lab::Channel::Slots1);
    Client c{pipe.get(), child.process(), proof.nonce, {}};
    c.hello(); c.begin(1, 1);
    switch (index % 4) {
      case 0:
        for (uint64_t epoch = 1; epoch <= 2; ++epoch) {
          if (epoch != 1) c.begin(epoch, epoch);
          for (unsigned i = 0; i < 4; ++i) FillAndPublish(c, owner, c.reserve(i ? 65536 : 1));
          c.empty(slotwire::Op::End);
        }
        c.empty(slotwire::Op::Close); break;
      case 1: {
        auto cancelled = c.reserve(1024); c.cancel(cancelled);
        for (unsigned i = 0; i < 3; ++i) {
          const auto key = c.reserve(1024); Check(key.slot != cancelled.slot);
          FillAndPublish(c, owner, key);
        }
        c.empty(slotwire::Op::Retire);
      } break;
      case 2: LateReused(c, owner); break;
      case 3: {
        const auto key = c.reserve(1024); const auto bytes = Pattern(key);
        owner.writeOnce(key, bytes.data(), key.bytes, c.peer);
        auto wrong = lab::Sha256(bytes.data(), key.bytes); wrong[0] ^= 1;
        c.publish(key, wrong, false);
      } break;
    }
    proof.exit = child.wait(7000);
    Check(proof.exit == (index % 4 < 2 ? 0u : 2u));
    proof.exchanges = c.exchanges; proof.stats = owner.stats(); proof.wireWrites = c.io.writes;
  }
  AssertContainerGone(proof.nonce);
  Check(GetProcessHandleCount(GetCurrentProcess(), &proof.handles));
  return proof;
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 2 && !std::wcscmp(argv[1], L"--dependency-init")) {
      std::printf("{\"dependencyInit\":true,\"bits\":32}\n"); return 0;
    }
    Check(argc == 4);
    const std::wstring mode = argv[2];
    const char* name = nullptr;
    for (const char* candidate : {"normal", "fragment", "cancel", "duplicate-write", "old-nonce",
      "old-map", "old-device", "old-frame", "old-generation", "old-size", "old-slot", "duplicate-publish",
      "digest", "partial-write", "mutex-timeout", "mutex-abandoned", "bad-version", "bad-bits", "bad-capability",
      "bad-flags", "bad-sequence", "bad-nonce", "oversize", "disconnect", "parent-exit",
      "existing-mapping", "existing-mutex", "lifecycle-soak", "parent-exit-writing", "late-reused",
      "hello-required", "repeated-hello", "begin-active", "reserve-before-begin", "end-before-begin",
      "end-writing", "close-active", "capacity", "reserve-zero", "cancelled-publish", "publish-shape"})
      if (mode == std::wstring(candidate, candidate + std::strlen(candidate))) name = candidate;
    Check(name);
    DWORD initBefore = 0, initAfter = 0, before = 0, after = 0, initPid = 0;
    DWORD faultBefore = 0, faultFirst = 0, faultEighth = 0, faultLast = 0;
    uint64_t initCreation = 0;
    Check(GetProcessHandleCount(GetCurrentProcess(), &initBefore));
    for (unsigned i = 0; i < 8; ++i) lab::RandomNonce();
    lab::Sample zero{};
    for (unsigned i = 0; i < 8; ++i) lab::Sha256(zero.data(), zero.size());
    {
      lab::OwnedChild warm(argv[0], L"--dependency-init", std::wstring(argv[3]) + L".dependency-init");
      initPid = warm.pid(); initCreation = warm.creation(); Check(warm.wait(7000) == 0);
    }
    {
      const auto nonce = lab::RandomNonce();
      auto pipe = lab::CreateServer(nonce, lab::Channel::Slots1);
      lab::SharedSlots owner(nonce, lab::MappingRole::OwnerWriter);
      lab::SharedSlots reader(nonce, lab::MappingRole::Reader);
      Check(owner.protection() == PAGE_READWRITE && reader.protection() == PAGE_READONLY);
      // Separately measured exception-runtime initialization. No valid lease or
      // shared data is touched. The next 64 throws MUST show no handle growth.
      Check(GetProcessHandleCount(GetCurrentProcess(), &faultBefore));
      for (unsigned i = 0; i < 72; ++i) {
        bool rejected = false;
        try { owner.writeOnce({}, zero.data(), 1, GetCurrentProcess()); }
        catch (const lab::Failure& e) { rejected = e.fault == lab::Fault::SlotLease; }
        Check(rejected);
        if (i == 0) Check(GetProcessHandleCount(GetCurrentProcess(), &faultFirst));
        if (i == 7) Check(GetProcessHandleCount(GetCurrentProcess(), &faultEighth));
      }
      Check(GetProcessHandleCount(GetCurrentProcess(), &faultLast));
      Check(faultEighth == faultLast);
    }
    Check(GetProcessHandleCount(GetCurrentProcess(), &initAfter)); before = initAfter;
    auto nonce = lab::RandomNonce();
    DWORD pid = 0, exit = STILL_ACTIVE; uint64_t creation = 0;
    unsigned exchanges = 0, duplicateRejected = 0; lab::SlotIoStats stats;
    uint64_t wireWrites = 0;
    bool abrupt = false, containerGone = false;
    if (mode == L"lifecycle-soak") {
      for (unsigned index = 0; index < 48; ++index) {
        const auto log = std::wstring(argv[3]) + (index ? L".cycle-" + std::to_wstring(index) : L"");
        const auto cycle = RunCycle(argv[1], log, index);
        Check(cycle.handles == before);
        nonce = cycle.nonce; pid = cycle.pid; creation = cycle.creation; exit = cycle.exit;
        exchanges += cycle.exchanges; wireWrites += cycle.wireWrites;
        stats.writes += cycle.stats.writes; stats.writtenBytes += cycle.stats.writtenBytes;
        std::printf("{\"cycle\":true,\"index\":%u,\"nonce\":\"%ls\",\"childPid\":%lu,\"childCreation\":%llu,"
          "\"childExit\":%lu,\"exchanges\":%u,\"writes\":%llu,\"writtenBytes\":%llu,\"handles\":%lu}\n",
          index, lab::NonceText(cycle.nonce).c_str(), (unsigned long)cycle.pid, (unsigned long long)cycle.creation,
          (unsigned long)cycle.exit, cycle.exchanges, (unsigned long long)cycle.stats.writes,
          (unsigned long long)cycle.stats.writtenBytes, (unsigned long)cycle.handles);
      }
    } else if (mode == L"existing-mapping" || mode == L"existing-mutex") {
      {
        const bool isMapping = mode == L"existing-mapping";
        lab::Handle existing(isMapping ?
          CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, slots::PoolBytes,
            lab::SharedSlots::MappingName(nonce).c_str()) :
          CreateMutexExW(nullptr, lab::SharedSlots::MutexName(nonce, 2).c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
        Check(existing && GetLastError() != ERROR_ALREADY_EXISTS);
        bool rejected = false;
        try { lab::SharedSlots wrong(nonce, lab::MappingRole::OwnerWriter); }
        catch (const lab::Failure& e) { rejected = e.stage == lab::Stage::Setup && e.win32 == ERROR_ALREADY_EXISTS; }
        Check(rejected);
        if (isMapping) {
          lab::MappedView view; view.set(MapViewOfFile(existing.get(), FILE_MAP_WRITE, 0, 0, slots::PoolBytes));
          Check(view.get());
        } else {
          Check(WaitForSingleObject(existing.get(), 1000) == WAIT_OBJECT_0); Check(ReleaseMutex(existing.get()));
        }
      }
      exit = 0;
    } else {
      { // Container is constructed BEFORE child and destroyed AFTER it settles.
        lab::SharedSlots owner(nonce, lab::MappingRole::OwnerWriter);
        const auto args = lab::NonceText(nonce) + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
          std::to_wstring(lab::CreationTime(GetCurrentProcess()));
        lab::OwnedChild child(argv[1], args, argv[3]); pid = child.pid(); creation = child.creation();
        auto pipe = lab::Connect(nonce, child.process(), child.pid(), lab::Channel::Slots1);
        Client c{pipe.get(), child.process(), nonce, {}};
        if (mode == L"fragment") c.fragment = 3;
        if (mode == L"bad-version" || mode == L"oversize" || mode == L"bad-flags" ||
            mode == L"bad-sequence" || mode == L"bad-nonce" || mode == L"bad-bits" || mode == L"bad-capability") {
          auto hello = slotwire::Hello(32, 64);
          if (mode == L"bad-bits") hello[0] = 64;
          if (mode == L"bad-capability") hello[8] = 1;
          auto h = c.header(slotwire::Op::Hello, hello.size());
          if (mode == L"bad-sequence") h.sequence = 2;
          if (mode == L"bad-nonce") h.nonce.low ^= 1;
          size_t size = 0; auto packet = c.encode(h, hello.data(), size);
          if (mode == L"bad-version") packet[4] = 2;
          if (mode == L"bad-flags") packet[12] = 1; // Valid reply bit, invalid request role.
          if (mode == L"oversize") { ipc::WriteLe(packet.data() + 16, 129, 4); size = slotwire::HeaderBytes; }
          lab::WriteBytes(pipe.get(), child.process(), packet.data(), size, c.io);
        } else if (mode == L"hello-required") {
          std::array<uint8_t, 16> begin{}; ipc::WriteLe(begin.data(), 1, 8); ipc::WriteLe(begin.data() + 8, 1, 8);
          c.send(c.header(slotwire::Op::Begin, begin.size()), begin.data());
        } else {
          c.hello();
          if (mode == L"parent-exit" || mode == L"parent-exit-writing") {
            if (mode == L"parent-exit-writing") {
              c.begin(1, 1); const auto key = c.reserve(65536); const auto bytes = Pattern(key);
              owner.writeOnce(key, bytes.data(), key.bytes, c.peer);
            }
            std::printf("{\"summary\":true,\"case\":\"%s\",\"bits\":32,\"childPid\":%lu,\"childCreation\":%llu,"
              "\"abruptParentExit\":true,\"exchanges\":%u,\"nonce\":\"%ls\",\"initPid\":%lu,\"initCreation\":%llu,"
              "\"writes\":%llu,\"writtenBytes\":%llu}\n",
              name, (unsigned long)pid, (unsigned long long)creation, c.exchanges, lab::NonceText(nonce).c_str(),
              (unsigned long)initPid, (unsigned long long)initCreation,
              (unsigned long long)owner.stats().writes, (unsigned long long)owner.stats().writtenBytes);
            std::fflush(stdout); ExitProcess(0); // Atomic Job owns exact host settlement.
          } else if (mode == L"disconnect") {
            pipe.reset();
          } else if (mode == L"repeated-hello") {
            const auto hello = slotwire::Hello(32, 64);
            c.send(c.header(slotwire::Op::Hello, hello.size()), hello.data());
          } else if (mode == L"end-before-begin") {
            c.send(c.header(slotwire::Op::End, 0), nullptr);
          } else if (mode == L"reserve-before-begin") {
            std::array<uint8_t, 16> reserve{};
            ipc::WriteLe(reserve.data(), 1, 8); ipc::WriteLe(reserve.data() + 8, 1, 4);
            c.send(c.header(slotwire::Op::Reserve, reserve.size()), reserve.data());
          } else {
            c.begin(1, 1);
            if (mode == L"begin-active") {
              std::array<uint8_t, 16> begin{}; ipc::WriteLe(begin.data(), 2, 8); ipc::WriteLe(begin.data() + 8, 2, 8);
              c.send(c.header(slotwire::Op::Begin, begin.size()), begin.data());
            } else if (mode == L"close-active") {
              c.send(c.header(slotwire::Op::Close, 0), nullptr);
            } else if (mode == L"reserve-zero" || mode == L"capacity") {
              if (mode == L"capacity") for (unsigned i = 0; i < 4; ++i) c.reserve(1);
              std::array<uint8_t, 16> reserve{}; ipc::WriteLe(reserve.data(), c.frame + 1, 8);
              ipc::WriteLe(reserve.data() + 8, mode == L"capacity" ? 1 : 0, 4);
              c.send(c.header(slotwire::Op::Reserve, reserve.size()), reserve.data());
            } else if (mode == L"end-writing") {
              c.reserve(1); c.send(c.header(slotwire::Op::End, 0), nullptr);
            } else if (mode == L"late-reused") {
              LateReused(c, owner);
            } else if (mode == L"normal" || mode == L"fragment") {
              for (uint64_t epoch = 1; epoch <= 3; ++epoch) {
                if (epoch != 1) c.begin(epoch, epoch);
                for (unsigned batch = 0; batch < 3; ++batch) {
                  std::array<slots::Key, 4> keys{};
                  const uint32_t sizes[] = {1, 97, 4096, 65536};
                  for (unsigned i = 0; i < 4; ++i) {
                    keys[i] = c.reserve(sizes[i]); auto data = Pattern(keys[i]);
                    owner.writeOnce(keys[i], data.data(), keys[i].bytes, child.process());
                  }
                  for (unsigned i = 4; i-- > 0;) {
                    auto data = Pattern(keys[i]); c.publish(keys[i], lab::Sha256(data.data(), keys[i].bytes));
                  }
                }
                c.empty(slotwire::Op::End);
              }
              c.empty(slotwire::Op::Close);
            } else if (mode == L"cancel") {
              auto cancelled = c.reserve(128); c.cancel(cancelled);
              for (unsigned i = 0; i < 18; ++i) {
                auto key = c.reserve(257); Check(key.slot != cancelled.slot);
                auto data = Pattern(key); owner.writeOnce(key, data.data(), key.bytes, child.process());
                c.publish(key, lab::Sha256(data.data(), key.bytes));
              }
              c.empty(slotwire::Op::Retire);
            } else {
              auto key = c.reserve(65536); auto data = Pattern(key); const auto hash = lab::Sha256(data.data(), key.bytes);
              if (mode == L"cancelled-publish") {
                c.cancel(key); c.publish(key, hash, false);
              } else if (mode == L"publish-shape") {
                const auto body = c.publishBody(key, hash);
                c.send(c.header(slotwire::Op::Publish, 95), body.data());
              } else if (mode == L"mutex-abandoned") {
                AbandonInput input{key, data};
                lab::Handle thread(CreateThread(nullptr, 0, AbandonWriter, &input, 0, nullptr));
                Check(thread && WaitForSingleObject(thread.get(), 3000) == WAIT_OBJECT_0);
                DWORD code = 0; Check(GetExitCodeThread(thread.get(), &code) && code == 0);
                c.publish(key, hash, false);
              } else if (mode == L"partial-write") {
                AbandonInput input{key, data}; Check(PartialWriter(input, true) == 0);
                c.publish(key, hash, false);
              } else {
                auto actual = data;
                owner.writeOnce(key, actual.data(), key.bytes, child.process());
                if (mode == L"duplicate-write") {
                  actual[0] ^= 255;
                  try { owner.writeOnce(key, actual.data(), key.bytes, child.process()); }
                  catch (const lab::Failure& e) { Check(e.fault == lab::Fault::SlotLease); ++duplicateRejected; }
                  Check(duplicateRejected == 1); c.publish(key, hash);
                  c.empty(slotwire::Op::End); c.empty(slotwire::Op::Close);
                } else if (mode == L"mutex-timeout") {
                  lab::Handle lock(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                    lab::SharedSlots::MutexName(nonce, key.slot).c_str()));
                  Check(lock && WaitForSingleObject(lock.get(), 1000) == WAIT_OBJECT_0);
                  c.publish(key, hash, false); exit = child.wait(7000); Check(ReleaseMutex(lock.get()));
                } else if (mode == L"duplicate-publish") {
                  c.publish(key, hash); c.publish(key, hash, false);
                } else {
                  if (mode == L"old-nonce") key.connection.low ^= 1;
                  if (mode == L"old-map") ++key.map;
                  if (mode == L"old-device") ++key.device;
                  if (mode == L"old-frame") ++key.frame;
                  if (mode == L"old-generation") ++key.generation;
                  if (mode == L"old-size") --key.bytes;
                  if (mode == L"old-slot") key.slot = (key.slot + 1) % slots::SlotCount;
                  auto wrongHash = hash; if (mode == L"digest") wrongHash[0] ^= 1;
                  c.publish(key, wrongHash, false);
                }
              }
            }
          }
        }
        exit = child.wait(7000);
        const bool good = mode == L"normal" || mode == L"fragment" || mode == L"cancel" || mode == L"duplicate-write";
        Check(exit == (good ? 0u : 2u)); exchanges = c.exchanges; stats = owner.stats(); wireWrites = c.io.writes;
      }
    }
    AssertContainerGone(nonce); containerGone = true;
    Check(GetProcessHandleCount(GetCurrentProcess(), &after));
    std::printf("{\"summary\":true,\"case\":\"%s\",\"bits\":32,\"childPid\":%lu,\"childCreation\":%llu,"
      "\"childExit\":%lu,\"exchanges\":%u,\"abruptParentExit\":%s,\"containerGone\":%s,"
      "\"nonce\":\"%ls\",\"handlesBefore\":%lu,\"handlesAfter\":%lu,\"initHandlesBefore\":%lu,\"initHandlesAfter\":%lu,"
      "\"initPid\":%lu,\"initCreation\":%llu,\"writes\":%llu,\"writtenBytes\":%llu,\"duplicateRejected\":%u,"
      "\"exceptionInit\":{\"before\":%lu,\"first\":%lu,\"eighth\":%lu,\"last\":%lu,\"steadySamples\":64},"
      "\"wireWrites\":%llu}\n",
      name, (unsigned long)pid, (unsigned long long)creation, (unsigned long)exit, exchanges,
      abrupt ? "true" : "false", containerGone ? "true" : "false", lab::NonceText(nonce).c_str(),
      (unsigned long)before, (unsigned long)after, (unsigned long)initBefore, (unsigned long)initAfter,
      (unsigned long)initPid, (unsigned long long)initCreation,
      (unsigned long long)stats.writes, (unsigned long long)stats.writtenBytes, duplicateRejected,
      (unsigned long)faultBefore, (unsigned long)faultFirst, (unsigned long)faultEighth, (unsigned long)faultLast,
      (unsigned long long)wireWrites);
    std::fflush(stdout); Check(before == after); return 0;
  } catch (const lab::Failure& e) {
    std::fprintf(stderr, "RH1 failure %s/%s code=%lu\n", lab::FaultName(e.fault), lab::StageName(e.stage), (unsigned long)e.win32);
  } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); }
  return 1;
}
