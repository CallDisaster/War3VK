#include "../tools/render_host/buffer_commands.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

using namespace warvk::host::buffers;
using warvk::host::commands::Scope;

static unsigned checks = 0;
static void Check(bool condition, unsigned line) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "render buffer commands check failed line=%u\n", line);
    std::exit(1);
  }
}
#define CHECK(x) Check(bool(x), __LINE__)
static constexpr Scope ScopeA{0x1122334455667788ull, 0x8877665544332211ull, 3, 7};
static constexpr Scope ScopeB{0x88776655, 0x12345678, 4, 8};
static_assert(!std::is_copy_constructible_v<Session> && !std::is_move_constructible_v<Session>);

static Header Command(Op op, uint64_t sequence, uint64_t id = 1, uint64_t generation = 1,
    uint64_t argument = 0, uint32_t bytes = 0) {
  Header result;
  result.op = op; result.payloadBytes = bytes; result.scope = ScopeA;
  result.sequence = sequence; result.resourceId = id;
  result.generation = generation; result.argument = argument;
  return result;
}
static void SameMetadata(const Snapshot& before, const Snapshot& after) {
  CHECK(before.accepted == after.accepted);
  CHECK(before.lastSequence == after.lastSequence);
  CHECK(before.sequenceExhausted == after.sequenceExhausted);
  for (size_t i = 0; i < ResourceSlots; ++i) CHECK(before.slots[i] == after.slots[i]);
}
static uint64_t Sum(const uint8_t* bytes, uint32_t count) {
  uint64_t sum = 0;
  for (uint32_t i = 0; i < count; ++i) sum += uint64_t(bytes[i]) * (i + 1);
  return sum;
}
struct TestBackend final : Backend {
  enum class Mode { Pass, Reject, Throw, FaultOwner, Reenter };
  Mode mode = Mode::Pass;
  unsigned calls = 0;
  uint64_t payloadSum = 0;
  Session* session = nullptr;
  const uint8_t* packet = nullptr;
  size_t count = 0;
  Snapshot expected;
  bool inspectBeforeCommit = false;
  Error nested = Error::None;
  bool apply(const View& view) override {
    ++calls;
    if (inspectBeforeCommit) {
      CHECK(session != nullptr);
      SameMetadata(expected, session->snapshot());
    }
    payloadSum = Sum(view.payload, view.header.payloadBytes);
    if (mode == Mode::Reject) return false;
    if (mode == Mode::Throw) throw 43;
    if (mode == Mode::FaultOwner) session->fault();
    if (mode == Mode::Reenter) nested = session->execute(packet, count, *this);
    return true;
  }
};
static size_t Pack(const Header& h, Packet& packet, const uint8_t* payload = nullptr) {
  size_t size = 0;
  CHECK(Encode(h, payload, packet, size) == Error::None);
  CHECK(size == HeaderBytes + h.payloadBytes);
  return size;
}
static void Accept(Session& session, TestBackend& backend, const Header& header,
    const uint8_t* payload = nullptr) {
  Packet packet;
  const auto size = Pack(header, packet, payload);
  backend.session = &session; backend.expected = session.snapshot(); backend.inspectBeforeCommit = true;
  const auto priorCalls = backend.calls;
  CHECK(session.execute(packet.data(), size, backend) == Error::None);
  CHECK(backend.calls == priorCalls + 1);
  CHECK(session.snapshot().accepted == backend.expected.accepted + 1);
  CHECK(session.snapshot().lastSequence == header.sequence);
}
static void Refuse(Session& session, TestBackend& backend, const Header& header,
    Error error, const uint8_t* payload = nullptr) {
  Packet packet;
  const auto size = Pack(header, packet, payload);
  const auto before = session.snapshot(); const auto calls = backend.calls;
  CHECK(session.execute(packet.data(), size, backend) == error);
  const auto after = session.snapshot();
  CHECK(after.state == State::Faulted); SameMetadata(before, after);
  CHECK(backend.calls == calls); // Invalid input never reaches the backend.
  CHECK(session.execute(packet.data(), size, backend) == Error::Terminal);
  SameMetadata(after, session.snapshot()); CHECK(backend.calls == calls);
}
static void WriteLeForMutation(uint8_t* output, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) { output[i] = uint8_t(value & 255); value >>= 8; }
}
static void DecodeFailure(const Packet& packet, size_t bytes, Error expected) {
  View view; view.header.scope = ScopeA; view.header.resourceId = 4; view.payload = packet.data();
  CHECK(Decode(packet.data(), bytes, view) == expected);
  CHECK(!view.header.scope.valid() && view.header.resourceId == 0 && view.payload == nullptr);
}

static void CodecGoldenAndBounds() {
  constexpr std::array<uint8_t, HeaderBytes> golden = {
    0x57,0x52,0x43,0x31,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
    0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  };
  Packet packet{}, mutated{};
  const auto create = Command(Op::Create, 1, 2, 1, 4096);
  CHECK(Pack(create, packet) == HeaderBytes);
  CHECK(std::memcmp(packet.data(), golden.data(), HeaderBytes) == 0);
  View view;
  CHECK(Decode(golden.data(), golden.size(), view) == Error::None);
  CHECK(view.header.op == Op::Create && view.header.scope == ScopeA && view.header.sequence == 1);
  CHECK(view.header.resourceId == 2 && view.header.generation == 1 && view.header.argument == 4096);
  CHECK(view.payload == nullptr);

  for (size_t size = 0; size < HeaderBytes; ++size) {
    Header header; header.scope = ScopeA;
    CHECK(DecodeHeader(packet.data(), size, header) == Error::HeaderSize);
    CHECK(!header.scope.valid()); DecodeFailure(packet, size, Error::PacketSize);
  }
  Header header;
  CHECK(DecodeHeader(packet.data(), HeaderBytes + 1, header) == Error::HeaderSize);
  CHECK(DecodeHeader(nullptr, HeaderBytes, header) == Error::NullBuffer);
  CHECK(Decode(nullptr, HeaderBytes, view) == Error::NullBuffer && view.payload == nullptr);
  DecodeFailure(packet, HeaderBytes + 1, Error::PacketSize);
  DecodeFailure(packet, MaxPacketBytes + 1, Error::PacketSize);
  for (size_t i = 0; i < 4; ++i) {
    mutated = packet; mutated[i] ^= 0x40; DecodeFailure(mutated, HeaderBytes, Error::Magic);
  }
  for (const unsigned version : {0u, 2u, 65535u}) {
    mutated = packet; WriteLeForMutation(mutated.data() + 4, version, 2);
    DecodeFailure(mutated, HeaderBytes, Error::Version);
  }
  for (const unsigned op : {0u, 6u, 65535u}) {
    mutated = packet; WriteLeForMutation(mutated.data() + 6, op, 2);
    DecodeFailure(mutated, HeaderBytes, Error::Opcode);
  }
  for (size_t i = 12; i < HeaderBytes; ++i) {
    if (i >= 16 && i < 80) continue;
    mutated = packet; mutated[i] = 1; DecodeFailure(mutated, HeaderBytes, Error::Reserved);
  }
  for (const size_t offset : {size_t(32), size_t(40), size_t(48), size_t(56), size_t(64)}) {
    mutated = packet; std::memset(mutated.data() + offset, 0, 8);
    const auto error = offset < 48 ? Error::Scope : offset == 48 ? Error::Sequence
      : offset == 56 ? Error::ResourceId : Error::Generation;
    DecodeFailure(mutated, HeaderBytes, error);
  }
  mutated = packet; std::memset(mutated.data() + 16, 0, 16);
  DecodeFailure(mutated, HeaderBytes, Error::Scope);
  for (const auto size : {uint64_t(0), MaxBufferBytes + 1, UINT64_MAX}) {
    mutated = packet; WriteLeForMutation(mutated.data() + 72, size, 8);
    DecodeFailure(mutated, HeaderBytes, Error::Argument);
  }
  for (const uint64_t id : {uint64_t(0), uint64_t(5), UINT64_MAX}) {
    mutated = packet; WriteLeForMutation(mutated.data() + 56, id, 8);
    DecodeFailure(mutated, HeaderBytes, Error::ResourceId);
  }
  for (const uint32_t body : {1u, MaxUploadBytes, MaxUploadBytes + 1, UINT32_MAX}) {
    mutated = packet; WriteLeForMutation(mutated.data() + 8, body, 4);
    DecodeFailure(mutated, HeaderBytes, Error::PayloadSize);
  }

  std::array<uint8_t, MaxUploadBytes> data{};
  for (uint32_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 31 + (i >> 8));
  auto upload = Command(Op::Upload, 2, 2, 1, 8, 4);
  CHECK(Pack(upload, packet, data.data()) == HeaderBytes + 4);
  auto uploadGolden = golden;
  uploadGolden[6] = 2; uploadGolden[8] = 4; uploadGolden[48] = 2;
  uploadGolden[72] = 8; uploadGolden[73] = 0;
  CHECK(std::memcmp(packet.data(), uploadGolden.data(), HeaderBytes) == 0);
  CHECK(std::memcmp(packet.data() + HeaderBytes, data.data(), 4) == 0);
  CHECK(DecodeHeader(packet.data(), HeaderBytes, header) == Error::None && header.payloadBytes == 4);
  for (const size_t count : {HeaderBytes, HeaderBytes + 1, HeaderBytes + 3, HeaderBytes + 5})
    DecodeFailure(packet, count, Error::PacketSize);
  upload.payloadBytes = MaxUploadBytes;
  CHECK(Pack(upload, packet, data.data()) == MaxPacketBytes);
  CHECK(Decode(packet.data(), packet.size(), view) == Error::None);
  CHECK(view.header.payloadBytes == MaxUploadBytes && Sum(view.payload, MaxUploadBytes) == Sum(data.data(), MaxUploadBytes));

  // Every encode failure leaves the destination unchanged and written zero.
  mutated.fill(0xCE); size_t written = 111;
  auto invalid = upload; invalid.payloadBytes = MaxUploadBytes + 1;
  CHECK(Encode(invalid, data.data(), mutated, written) == Error::PayloadSize && written == 0);
  CHECK(Encode(upload, nullptr, mutated, written) == Error::NullBuffer && written == 0);
  CHECK(Encode(upload, data.data(), mutated.data(), MaxPacketBytes - 1, written) == Error::PacketSize && written == 0);
  CHECK(Encode(upload, data.data(), nullptr, MaxPacketBytes, written) == Error::NullBuffer && written == 0);
  bool unchanged = true; for (auto value : mutated) unchanged &= value == 0xCE; CHECK(unchanged);
  invalid = upload; invalid.payloadBytes = 0;
  CHECK(Encode(invalid, data.data(), mutated, written) == Error::PayloadSize);
  invalid = upload; invalid.argument = UINT64_MAX - MaxUploadBytes + 1;
  CHECK(Encode(invalid, data.data(), mutated, written) == Error::Range);
  // Full 64-bit endpoint may encode without wrap; resource bounds reject it in
  // the ledger. It is not narrowed to native size_t in the header decoder.
  invalid.argument = UINT64_MAX - MaxUploadBytes;
  CHECK(Encode(invalid, data.data(), mutated, written) == Error::None);
  CHECK(Decode(mutated.data(), written, view) == Error::None && view.header.argument == invalid.argument);
  upload.payloadBytes = 17; upload.argument = 0;
  std::memcpy(mutated.data(), data.data(), 17);
  CHECK(Encode(upload, mutated.data(), mutated, written) == Error::None);
  CHECK(std::memcmp(mutated.data() + HeaderBytes, data.data(), 17) == 0);

  for (const auto op : {Op::Verify, Op::Destroy, Op::Finish}) {
    auto h = Command(op, 1, op == Op::Finish ? 0 : 1, op == Op::Finish ? 0 : 1);
    CHECK(Pack(h, packet) == HeaderBytes);
    CHECK(Decode(packet.data(), HeaderBytes, view) == Error::None && view.header.op == op);
    h.argument = 1; CHECK(Encode(h, nullptr, packet, written) == Error::Argument);
    h.argument = 0; h.payloadBytes = 1;
    CHECK(Encode(h, data.data(), packet, written) == Error::PayloadSize);
  }
  auto finish = Command(Op::Finish, 1, 0, 0);
  finish.resourceId = 1; CHECK(Encode(finish, nullptr, packet, written) == Error::ResourceId);
  finish.resourceId = 0; finish.generation = 1;
  CHECK(Encode(finish, nullptr, packet, written) == Error::Generation);
}

static void LifecycleAndRange() {
  std::array<uint8_t, MaxUploadBytes> bytes{}; bytes.fill(137);
  for (const uint64_t capacity : {uint64_t(1), MaxBufferBytes}) {
    Session session(ScopeA); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, capacity));
    CHECK(session.snapshot().slots[0].live && session.snapshot().slots[0].bytes == capacity);
    Accept(session, backend, Command(Op::Upload, 2, 1, 1, capacity - 1, 1), bytes.data());
    CHECK(backend.payloadSum == 137); // One-byte/unaligned upload is a valid wire operation.
    Accept(session, backend, Command(Op::Verify, 3));
    Accept(session, backend, Command(Op::Destroy, 4));
    const auto afterDestroy = session.snapshot();
    CHECK(!afterDestroy.slots[0].live && afterDestroy.slots[0].bytes == 0 && afterDestroy.slots[0].lastGeneration == 1);
    Accept(session, backend, Command(Op::Finish, 5, 0, 0));
    CHECK(session.snapshot().state == State::Finished);
    Refuse(session, backend, Command(Op::Finish, 6, 0, 0), Error::Terminal);
  }
  {
    Session session(ScopeA); TestBackend backend;
    for (uint64_t id = 1; id <= 4; ++id)
      Accept(session, backend, Command(Op::Create, id, id, 1, MaxBufferBytes));
    for (const auto& slot : session.snapshot().slots) CHECK(slot.live && slot.bytes == MaxBufferBytes);
    Accept(session, backend, Command(Op::Upload, 5, 4, 1, MaxBufferBytes - MaxUploadBytes, MaxUploadBytes), bytes.data());
    CHECK(backend.payloadSum == Sum(bytes.data(), MaxUploadBytes));
    Refuse(session, backend, Command(Op::Finish, 6, 0, 0), Error::LiveResources);
  }
  for (const uint64_t offset : {uint64_t(17), uint64_t(16), UINT64_MAX - 1}) {
    Session session(ScopeA); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, 16));
    Refuse(session, backend, Command(Op::Upload, 2, 1, 1, offset, 1), Error::Range, bytes.data());
  }
  {
    Session session(ScopeA); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, 16));
    Refuse(session, backend, Command(Op::Upload, 2, 1, 1, 15, 2), Error::Range, bytes.data());
  }
}

static void IdentitySequenceAndAba() {
  const uint8_t byte = 97;
  for (unsigned field = 0; field < 4; ++field) {
    Session session(ScopeA); TestBackend backend;
    auto header = Command(Op::Create, 1, 1, 1, 1);
    if (field == 0) ++header.scope.nonceLow;
    if (field == 1) ++header.scope.nonceHigh;
    if (field == 2) ++header.scope.mapEpoch;
    if (field == 3) ++header.scope.deviceEpoch;
    Refuse(session, backend, header, Error::Scope);
  }
  for (const uint64_t sequence : {uint64_t(1), uint64_t(3), UINT64_MAX}) {
    Session session(ScopeA); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, 1));
    Refuse(session, backend, Command(Op::Destroy, sequence), Error::Sequence);
  }
  for (const auto op : {Op::Upload, Op::Verify, Op::Destroy}) {
    Session session(ScopeA); TestBackend backend;
    Refuse(session, backend, Command(op, 1, 1, 1, 0, op == Op::Upload ? 1 : 0),
      Error::ResourceInactive, op == Op::Upload ? &byte : nullptr);
  }
  {
    Session session(ScopeA); TestBackend backend;
    Refuse(session, backend, Command(Op::Create, 1, 1, 2, 1), Error::Generation);
  }
  {
    Session session(ScopeA); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, 1));
    Refuse(session, backend, Command(Op::Create, 2, 1, 2, 1), Error::ResourceLive);
  }
  for (const bool stale : {false, true}) {
    Session session(ScopeA); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, 1));
    Accept(session, backend, Command(Op::Destroy, 2));
    Accept(session, backend, Command(Op::Create, 3, 1, 2, 8));
    if (stale) {
      Refuse(session, backend, Command(Op::Upload, 4, 1, 1, 0, 1), Error::Generation, &byte);
    } else {
      Accept(session, backend, Command(Op::Upload, 4, 1, 2, 7, 1), &byte);
      Accept(session, backend, Command(Op::Verify, 5, 1, 2));
      Accept(session, backend, Command(Op::Destroy, 6, 1, 2));
      CHECK(session.snapshot().slots[0].lastGeneration == 2);
      Accept(session, backend, Command(Op::Finish, 7, 0, 0));
    }
  }
  {
    Session session(ScopeA, Limits{UINT64_MAX, 2}); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, 1));
    Accept(session, backend, Command(Op::Destroy, 2));
    Accept(session, backend, Command(Op::Create, 3, 1, 2, 1));
    Accept(session, backend, Command(Op::Destroy, 4, 1, 2));
    Refuse(session, backend, Command(Op::Create, 5, 1, 3, 1), Error::GenerationExhausted);
  }
  {
    Session session(ScopeA, Limits{2, UINT64_MAX}); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, 1));
    Accept(session, backend, Command(Op::Verify, 2));
    CHECK(session.snapshot().sequenceExhausted);
    Refuse(session, backend, Command(Op::Destroy, 3), Error::SequenceExhausted);
  }
  {
    Session session(ScopeA, Limits{3, UINT64_MAX}); TestBackend backend;
    Accept(session, backend, Command(Op::Create, 1, 1, 1, 1));
    Accept(session, backend, Command(Op::Destroy, 2));
    Accept(session, backend, Command(Op::Finish, 3, 0, 0));
    CHECK(session.snapshot().sequenceExhausted && session.snapshot().state == State::Finished);
  }
  {
    Session fresh(ScopeB); TestBackend backend;
    Refuse(fresh, backend, Command(Op::Create, 1, 1, 1, 1), Error::Scope);
    Session newer(ScopeB); auto header = Command(Op::Create, 1, 1, 1, 1); header.scope = ScopeB;
    Accept(newer, backend, header);
  }
  for (const auto limits : {Limits{0, 1}, Limits{1, 0}}) {
    Session session(ScopeA, limits); TestBackend backend; Packet packet;
    const auto count = Pack(Command(Op::Create, 1, 1, 1, 1), packet);
    CHECK(session.execute(packet.data(), count, backend) == Error::Unconfigured);
    CHECK(backend.calls == 0 && session.snapshot().accepted == 0);
  }
  Session badScope(Scope{}); TestBackend backend; Packet packet;
  const auto count = Pack(Command(Op::Create, 1, 1, 1, 1), packet);
  CHECK(badScope.execute(packet.data(), count, backend) == Error::Unconfigured && backend.calls == 0);
}

static void FailedBackendNeverCommits() {
  const uint8_t byte = 4;
  for (const auto mode : {TestBackend::Mode::Reject, TestBackend::Mode::Throw,
      TestBackend::Mode::FaultOwner, TestBackend::Mode::Reenter}) {
    for (const auto op : {Op::Create, Op::Upload, Op::Verify, Op::Destroy, Op::Finish}) {
      Session session(ScopeA); TestBackend backend;
      uint64_t sequence = 1;
      if (op != Op::Create && op != Op::Finish) {
        Accept(session, backend, Command(Op::Create, sequence++, 1, 1, 4));
      }
      const auto header = Command(op, sequence, op == Op::Finish ? 0 : 1,
        op == Op::Finish ? 0 : 1, op == Op::Create ? 4 : 0, op == Op::Upload ? 1 : 0);
      Packet packet; const auto count = Pack(header, packet, op == Op::Upload ? &byte : nullptr);
      const auto before = session.snapshot(); const auto calls = backend.calls;
      backend.session = &session; backend.packet = packet.data(); backend.count = count;
      backend.expected = before; backend.inspectBeforeCommit = true; backend.mode = mode;
      const auto expected = mode == TestBackend::Mode::Reject || mode == TestBackend::Mode::Throw
        ? Error::BackendFailure : Error::Terminal;
      CHECK(session.execute(packet.data(), count, backend) == expected);
      CHECK(backend.calls == calls + 1); CHECK(session.snapshot().state == State::Faulted);
      SameMetadata(before, session.snapshot());
      if (mode == TestBackend::Mode::Reenter) CHECK(backend.nested == Error::Reentrant);
      backend.mode = TestBackend::Mode::Pass;
      CHECK(session.execute(packet.data(), count, backend) == Error::Terminal);
      CHECK(backend.calls == calls + 1); SameMetadata(before, session.snapshot());
    }
  }
  // Invalid raw bytes are rejected before backend ownership can change, even
  // after a live object exists. Do not rely solely on Encode to reject input.
  Session session(ScopeA); TestBackend backend;
  Accept(session, backend, Command(Op::Create, 1, 1, 1, 4));
  Packet malformed; const auto count = Pack(Command(Op::Destroy, 2), malformed);
  malformed[80] = 1; const auto before = session.snapshot(); const auto calls = backend.calls;
  CHECK(session.execute(malformed.data(), count, backend) == Error::Reserved);
  CHECK(backend.calls == calls); SameMetadata(before, session.snapshot());
  CHECK(session.snapshot().state == State::Faulted);
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--golden") == 0) {
    auto header = Command(Op::Upload, 9, 2, 5, 11, 3);
    header.scope = {0x0807060504030201ull, 0x1817161514131211ull, 3, 7};
    constexpr uint8_t payload[] = {1, 2, 254};
    Packet packet; size_t count = 0;
    if (Encode(header, payload, packet, count) != Error::None) return 2;
    for (size_t i = 0; i < count; ++i) std::printf("%02x", unsigned(packet[i]));
    std::printf("\n");
    return 0;
  }
  if (argc != 1) {
    std::fprintf(stderr, "buffer commands usage: [--golden]\n");
    return 2;
  }
  CodecGoldenAndBounds(); LifecycleAndRange(); IdentitySequenceAndAba(); FailedBackendNeverCommits();
  std::printf("buffer commands checks=%u failures=0 bits=%zu headerBytes=%zu maxPacketBytes=%zu "
    "resourceSlots=%u strictScope=1 privateCommit=1 abaRejected=1 backendFailureTerminal=1 "
    "cpuOnly=1 gpuTested=0 ipcTested=0\n",
    checks, sizeof(void*) * 8, HeaderBytes, MaxPacketBytes, ResourceSlots);
}
