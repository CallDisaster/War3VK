// Standalone codec and wire acceptance tests for WVE1 recorder event wire.
// Directly tests tools/render_host/recorder_event_wire.h / .cpp.
// Pure CPU: no heap/OS calls in codec, no game, no GPU, no native Record.
#include "../tools/render_host/recorder_event_wire.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace warvk::host::recorder;

static unsigned s_checks = 0;
#define CHECK(x) do { \
  ++s_checks; \
  if (!(x)) { \
    std::fprintf(stderr, "CHECK FAILED line %d: %s\n", __LINE__, #x); \
    std::exit(1); \
  } \
} while (false)

static const char kGoldenHex[] =
  "575645310100000002000000500000008801000001000000efcdab8967452301"
  "2a00000000000000640000000000000000000000000000000000000000000000"
  "002000000000000000000000000000007b00000000000000efcdab8967452301"
  "1032547698badcfe07060504030201008877665544332211f401000000000000"
  "030000000000000002000000000000005713000012000000776172766b2e676f"
  "6c64656e2e6576656e742e7631000000000000000000000004a003a002a001a0"
  "04b003b002b001b004c003c002c001c004d003d002d001d004e003e002e001e0"
  "04f003f002f001f0efcdab896745230167452301efcdab89bebafecaefbeadde"
  "ffffffff0000000000000000fffffffff0debc9a785634120000803f00000040"
  "000000c00000803e000080bf0000000000000080ffff7f7f0000800000004000"
  "0000c8420000c8c20000003fabaaaa3edb0f494054f82d40efbeaddebebafeca"
  "0df0adbacefaedfe0403020100ff00ffff00ff0055555555aaaaaaaa78563412"
  "f0debc9a0100000000000080ffffffffffffff7fffff00002000001021000010"
  "2200001023000010240000102500001026000010270000102800001029000010"
  "2a0000102b0000102c0000102d0000102e0000102f000010";

static std::string ToHex(const uint8_t* data, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string s;
  s.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    s.push_back(kDigits[(data[i] >> 4) & 0xf]);
    s.push_back(kDigits[data[i] & 0xf]);
  }
  return s;
}

static Header MakeGoldenHeader() {
  Header h{};
  h.op = Op::Data;
  h.count = 1;
  h.session = 0x0123456789ABCDEFULL;
  h.ordinal = 42;
  h.trigger = 100;
  h.attempted = 0;
  h.lost = 0;
  h.capacity = 8192;
  h.reason = 0;
  h.accepted = 0;
  return h;
}

static Event MakeGoldenEvent() {
  Event ev{};
  ev.sequence = 123;
  ev.session = 0x0123456789ABCDEFULL;
  ev.parent = 0xFEDCBA9876543210ULL;
  ev.qpc = 0x0001020304050607ULL;
  ev.key.owner = 0x1122334455667788ULL;
  ev.key.frame = 500;
  ev.key.mapEpoch = 3;
  ev.key.deviceEpoch = 2;
  ev.thread = 0x00001357;
  ev.kind = dxvk::war3::tools::evidence::Kind::DirectionalDraw; // 18
  std::memcpy(ev.label.data(), "warvk.golden.event.v1", 21);

  const uint64_t dataItems[12] = {
    0xA001A002A003A004ULL, 0xB001B002B003B004ULL, 0xC001C002C003C004ULL, 0xD001D002D003D004ULL,
    0xE001E002E003E004ULL, 0xF001F002F003F004ULL, 0x0123456789ABCDEFULL, 0x89ABCDEF01234567ULL,
    0xDEADBEEFCAFEBABEULL, 0x00000000FFFFFFFFULL, 0xFFFFFFFF00000000ULL, 0x123456789ABCDEF0ULL
  };
  for (size_t i = 0; i < 12; ++i) ev.data[i] = dataItems[i];

  const uint32_t bitsItems[48] = {
    0x3F800000, 0x40000000, 0xC0000000, 0x3E800000,
    0xBF800000, 0x00000000, 0x80000000, 0x7F7FFFFF,
    0x00800000, 0x00400000, 0x42C80000, 0xC2C80000,
    0x3F000000, 0x3EAAAAAB, 0x40490FDB, 0x402DF854,
    0xDEADBEEF, 0xCAFEBABE, 0xBAADF00D, 0xFEEDFACE,
    0x01020304, 0xFF00FF00, 0x00FF00FF, 0x55555555,
    0xAAAAAAAA, 0x12345678, 0x9ABCDEF0, 0x00000001,
    0x80000000, 0xFFFFFFFF, 0x7FFFFFFF, 0x0000FFFF,
    0x10000020, 0x10000021, 0x10000022, 0x10000023,
    0x10000024, 0x10000025, 0x10000026, 0x10000027,
    0x10000028, 0x10000029, 0x1000002A, 0x1000002B,
    0x1000002C, 0x1000002D, 0x1000002E, 0x1000002F
  };
  for (size_t i = 0; i < 48; ++i) ev.bits[i] = bitsItems[i];
  return ev;
}

static void TestGoldenCodec() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();

  std::vector<uint8_t> buffer(MaxPacketBytes, 0);
  size_t written = 0;
  WireError err = Encode(h, &ev, buffer.data(), buffer.size(), written);
  CHECK(err == WireError::None);
  CHECK(written == 80 + 392);

  std::string hexStr = ToHex(buffer.data(), written);
  CHECK(hexStr == kGoldenHex);
  std::printf("GOLDEN=%s\n", hexStr.c_str());

  View v{};
  err = Decode(buffer.data(), written, v);
  CHECK(err == WireError::None);
  CHECK(v.header.op == Op::Data);
  CHECK(v.header.count == 1);
  CHECK(v.header.session == h.session);
  CHECK(v.header.ordinal == h.ordinal);
  CHECK(v.header.trigger == h.trigger);
  CHECK(v.header.attempted == 0);
  CHECK(v.header.lost == 0);
  CHECK(v.header.capacity == 8192);
  CHECK(v.header.reason == 0);
  CHECK(v.header.accepted == 0);
  CHECK(v.events != nullptr);

  Event decEv{};
  err = DecodeEvent(v.events, EventBytes, decEv);
  CHECK(err == WireError::None);
  CHECK(decEv.sequence == ev.sequence);
  CHECK(decEv.session == ev.session);
  CHECK(decEv.parent == ev.parent);
  CHECK(decEv.qpc == ev.qpc);
  CHECK(decEv.key.owner == ev.key.owner);
  CHECK(decEv.key.frame == ev.key.frame);
  CHECK(decEv.key.mapEpoch == ev.key.mapEpoch);
  CHECK(decEv.key.deviceEpoch == ev.key.deviceEpoch);
  CHECK(decEv.thread == ev.thread);
  CHECK(decEv.kind == ev.kind);
  CHECK(std::memcmp(decEv.label.data(), ev.label.data(), 32) == 0);
  for (size_t i = 0; i < 12; ++i) {
    CHECK(decEv.data[i] == ev.data[i]);
  }
  for (size_t i = 0; i < 48; ++i) {
    CHECK(decEv.bits[i] == ev.bits[i]);
  }
}

static void TestRoundtripBegin() {
  Header h{};
  h.op = Op::Begin;
  h.count = 0;
  h.session = 0x9988776655443322ULL;
  h.ordinal = 1;
  h.trigger = NoTrigger;
  h.attempted = 0;
  h.lost = 0;
  h.capacity = 16384;
  h.reason = 0;
  h.accepted = 0;

  std::vector<uint8_t> buf(HeaderBytes, 0);
  size_t written = 0;
  CHECK(Encode(h, nullptr, buf.data(), buf.size(), written) == WireError::None);
  CHECK(written == HeaderBytes);

  View v{};
  CHECK(Decode(buf.data(), written, v) == WireError::None);
  CHECK(v.header.op == Op::Begin);
  CHECK(v.header.count == 0);
  CHECK(v.header.session == h.session);
  CHECK(v.header.ordinal == 1);
  CHECK(v.header.trigger == NoTrigger);
  CHECK(v.header.capacity == 16384);
  CHECK(v.events == nullptr);
}

static void TestRoundtripSeal() {
  for (uint32_t reason = 1; reason <= 4; ++reason) {
    Header h{};
    h.op = Op::Seal;
    h.count = 0;
    h.session = 0xFEEDFACE12345678ULL;
    h.ordinal = 77;
    h.trigger = 50;
    h.attempted = 100;
    h.lost = 15;
    h.capacity = 8192;
    h.reason = reason;
    h.accepted = 85;

    std::vector<uint8_t> buf(HeaderBytes, 0);
    size_t written = 0;
    CHECK(Encode(h, nullptr, buf.data(), buf.size(), written) == WireError::None);
    CHECK(written == HeaderBytes);

    View v{};
    CHECK(Decode(buf.data(), written, v) == WireError::None);
    CHECK(v.header.op == Op::Seal);
    CHECK(v.header.count == 0);
    CHECK(v.header.session == h.session);
    CHECK(v.header.ordinal == 77);
    CHECK(v.header.trigger == 50);
    CHECK(v.header.attempted == 100);
    CHECK(v.header.lost == 15);
    CHECK(v.header.capacity == 8192);
    CHECK(v.header.reason == reason);
    CHECK(v.header.accepted == 85);
    CHECK(v.events == nullptr);
  }

  // Seal with NoTrigger is also valid
  Header hNoTrig{};
  hNoTrig.op = Op::Seal;
  hNoTrig.session = 0x1111;
  hNoTrig.ordinal = 2;
  hNoTrig.trigger = NoTrigger;
  hNoTrig.attempted = 50;
  hNoTrig.lost = 0;
  hNoTrig.capacity = 4;
  hNoTrig.reason = 1;
  hNoTrig.accepted = 50;
  std::vector<uint8_t> buf(HeaderBytes, 0);
  size_t written = 0;
  CHECK(Encode(hNoTrig, nullptr, buf.data(), buf.size(), written) == WireError::None);
  View v{};
  CHECK(Decode(buf.data(), written, v) == WireError::None);
  CHECK(v.header.trigger == NoTrigger);
}

static void TestRoundtripData160() {
  Header h{};
  h.op = Op::Data;
  h.count = MaxEvents;
  h.session = 0xAABBCCDDEEFF0011ULL;
  h.ordinal = 5;
  h.trigger = NoTrigger;
  h.attempted = 0;
  h.lost = 0;
  h.capacity = MaxHistoryEvents;
  h.reason = 0;
  h.accepted = 0;

  std::vector<Event> events(MaxEvents);
  for (uint32_t i = 0; i < MaxEvents; ++i) {
    auto& ev = events[i];
    ev.sequence = i + 1;
    ev.session = h.session;
    ev.parent = i * 10;
    ev.qpc = 1000000ULL + i;
    ev.key.owner = 100 + i;
    ev.key.frame = 200 + i;
    ev.key.mapEpoch = 1;
    ev.key.deviceEpoch = 1;
    ev.thread = i;
    ev.kind = static_cast<dxvk::war3::tools::evidence::Kind>(1 + (i % 18));
    std::snprintf(ev.label.data(), 32, "event_%u", i);
    for (size_t j = 0; j < 12; ++j) {
      ev.data[j] = (static_cast<uint64_t>(i) << 32) | static_cast<uint64_t>(j);
    }
    for (size_t j = 0; j < 48; ++j) {
      ev.bits[j] = (i * 48 + static_cast<uint32_t>(j)) ^ 0xA5A5A5A5;
    }
  }

  std::vector<uint8_t> buf(MaxPacketBytes, 0);
  size_t written = 0;
  CHECK(Encode(h, events.data(), buf.data(), buf.size(), written) == WireError::None);
  CHECK(written == HeaderBytes + MaxEvents * EventBytes);

  View v{};
  CHECK(Decode(buf.data(), written, v) == WireError::None);
  CHECK(v.header.count == MaxEvents);
  CHECK(v.events != nullptr);

  for (uint32_t i = 0; i < MaxEvents; ++i) {
    Event dec{};
    CHECK(DecodeEvent(v.events + i * EventBytes, EventBytes, dec) == WireError::None);
    CHECK(dec.sequence == events[i].sequence);
    CHECK(dec.session == events[i].session);
    CHECK(dec.kind == events[i].kind);
    CHECK(std::strcmp(dec.label.data(), events[i].label.data()) == 0);
    CHECK(dec.bits[0] == events[i].bits[0]);
    CHECK(dec.data[11] == events[i].data[11]);
  }
}

static void TestFloatBitsPreservation() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();

  // Test special float bit patterns in bits (separate focused preservation test)
  const uint32_t floatBits[] = {
    0x00000000, // +0.0f
    0x80000000, // -0.0f
    0x3F800000, // +1.0f
    0xBF800000, // -1.0f
    0x7F800000, // +inf (0x7f800000)
    0xFF800000, // -inf
    0x7FC00000, // quiet NaN
    0x7FA5A5A5, // custom NaN payload
    0x00400000, // subnormal
    0x00000001, // smallest subnormal
    0x7F7FFFFF, // max normal
    0x00800000  // min normal
  };
  for (size_t i = 0; i < sizeof(floatBits) / sizeof(floatBits[0]); ++i) {
    ev.bits[i] = floatBits[i];
  }

  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;
  CHECK(Encode(h, &ev, buf.data(), buf.size(), written) == WireError::None);

  View v{};
  CHECK(Decode(buf.data(), written, v) == WireError::None);
  Event dec{};
  CHECK(DecodeEvent(v.events, EventBytes, dec) == WireError::None);
  for (size_t i = 0; i < sizeof(floatBits) / sizeof(floatBits[0]); ++i) {
    CHECK(dec.bits[i] == floatBits[i]);
  }
}

static void TestNullBufferRejection() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  size_t written = 123;
  CHECK(Encode(h, &ev, nullptr, 1000, written) == WireError::Null);
  CHECK(written == 0);

  written = 123;
  // Use genuine independent buffer to ensure no false alias detection with written
  std::array<uint8_t, HeaderBytes + EventBytes> independentBuffer{};
  CHECK(Encode(h, nullptr, independentBuffer.data(), independentBuffer.size(), written) == WireError::Null);
  CHECK(written == 0);

  View v{};
  v.header.count = 99;
  CHECK(Decode(nullptr, 100, v) == WireError::Null);
  CHECK(v.header.count == 0);
  CHECK(v.events == nullptr);

  Event dec{};
  dec.sequence = 99;
  CHECK(DecodeEvent(nullptr, EventBytes, dec) == WireError::Null);
  CHECK(dec.sequence == 0);
}

static void TestSizeAndTruncationRejection() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  size_t written = 123;

  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  // Encode buffer too small
  CHECK(Encode(h, &ev, buf.data(), HeaderBytes + EventBytes - 1, written) == WireError::Size);
  CHECK(written == 0);

  // Encode valid packet first
  CHECK(Encode(h, &ev, buf.data(), buf.size(), written) == WireError::None);

  // Decode too small (< 80)
  View v{};
  CHECK(Decode(buf.data(), 79, v) == WireError::Size);
  CHECK(v.events == nullptr);

  // Decode truncated (471 < 472)
  CHECK(Decode(buf.data(), 471, v) == WireError::Size);

  // Decode with trailing bytes (473 > 472)
  std::vector<uint8_t> trailingBuf(buf);
  trailingBuf.push_back(0);
  CHECK(Decode(trailingBuf.data(), trailingBuf.size(), v) == WireError::Size);

  // DecodeEvent size != 392
  Event dec{};
  CHECK(DecodeEvent(buf.data() + HeaderBytes, 391, dec) == WireError::Size);
  CHECK(dec.sequence == 0);
  CHECK(DecodeEvent(buf.data() + HeaderBytes, 393, dec) == WireError::Size);
  CHECK(DecodeEvent(buf.data() + HeaderBytes, 0, dec) == WireError::Size);
}

static void TestEncodeWrittenAliasing() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes, 0x55);

  // 1. &written aliases header.session:
  // Must return Overlap and header.session must NOT be modified (not zeroed).
  const uint64_t origSession = h.session;
  size_t* aliasedSession = reinterpret_cast<size_t*>(&h.session);
  WireError err = Encode(h, &ev, buf.data(), buf.size(), *aliasedSession);
  CHECK(err == WireError::Overlap);
  CHECK(h.session == origSession);

  // 2. &written aliases ev.sequence:
  // Must return Overlap and ev.sequence must NOT be modified.
  const uint64_t origSeq = ev.sequence;
  size_t* aliasedSeq = reinterpret_cast<size_t*>(&ev.sequence);
  err = Encode(h, &ev, buf.data(), buf.size(), *aliasedSeq);
  CHECK(err == WireError::Overlap);
  CHECK(ev.sequence == origSeq);

  // 3. &written aliases dst[0]:
  // Must return Overlap and dst must NOT be modified.
  buf[0] = 0x77; buf[1] = 0x88;
  size_t* aliasedDst = reinterpret_cast<size_t*>(buf.data());
  err = Encode(h, &ev, buf.data(), buf.size(), *aliasedDst);
  CHECK(err == WireError::Overlap);
  CHECK(buf[0] == 0x77);
  CHECK(buf[1] == 0x88);
}

static void TestDecodeViewAliasing() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;
  CHECK(Encode(h, &ev, buf.data(), buf.size(), written) == WireError::None);

  // Save copy of the buffer bytes where View would sit
  std::vector<uint8_t> backup(buf.begin(), buf.begin() + sizeof(View));

  // If &out aliases src, Decode must return Overlap and NOT zero src!
  View* aliasedView = reinterpret_cast<View*>(buf.data());
  WireError err = Decode(buf.data(), written, *aliasedView);
  CHECK(err == WireError::Overlap);
  CHECK(std::memcmp(buf.data(), backup.data(), sizeof(View)) == 0);
}

static void TestDecodeEventAliasing() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;
  CHECK(Encode(h, &ev, buf.data(), buf.size(), written) == WireError::None);

  uint8_t* evSrc = buf.data() + HeaderBytes;
  std::vector<uint8_t> backup(evSrc, evSrc + sizeof(Event));

  // If &out aliases src, DecodeEvent must return Overlap and NOT zero src!
  Event* aliasedEv = reinterpret_cast<Event*>(evSrc);
  WireError err = DecodeEvent(evSrc, EventBytes, *aliasedEv);
  CHECK(err == WireError::Overlap);
  CHECK(std::memcmp(evSrc, backup.data(), sizeof(Event)) == 0);
}

static void TestDestinationOverlapWithInputs() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  size_t written = 999;

  // When &written is independent, dst overlapping &header should return Overlap and written == 0
  uint8_t* dstHeader = reinterpret_cast<uint8_t*>(&h);
  CHECK(Encode(h, &ev, dstHeader, 1000, written) == WireError::Overlap);
  CHECK(written == 0);

  // dst overlapping events should return Overlap and written == 0
  written = 999;
  uint8_t* dstEv = reinterpret_cast<uint8_t*>(&ev);
  CHECK(Encode(h, &ev, dstEv, 1000, written) == WireError::Overlap);
  CHECK(written == 0);
}

static void TestRangeOverflowAsOverlap() {
  View v{};
  size_t hugeSize = SIZE_MAX;
  uint8_t dummy = 0;
  CHECK(Decode(&dummy, hugeSize, v) == WireError::Overlap);
  CHECK(v.events == nullptr);

  Event dec{};
  CHECK(DecodeEvent(&dummy, hugeSize, dec) == WireError::Overlap);
  CHECK(dec.sequence == 0);
}

static void TestMagicVersionHeaderFlagsOpcode() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;
  CHECK(Encode(h, &ev, buf.data(), buf.size(), written) == WireError::None);

  View v{};
  // Bad magic
  buf[0] = 'X';
  CHECK(Decode(buf.data(), written, v) == WireError::Magic);
  buf[0] = 'W';

  // Bad major version
  buf[4] = 2;
  CHECK(Decode(buf.data(), written, v) == WireError::Version);
  buf[4] = 1;

  // Bad minor version
  buf[6] = 1;
  CHECK(Decode(buf.data(), written, v) == WireError::Version);
  buf[6] = 0;

  // Bad headerBytes
  buf[12] = 84;
  CHECK(Decode(buf.data(), written, v) == WireError::Header);
  buf[12] = 80;

  // Bad flags
  buf[10] = 1;
  CHECK(Decode(buf.data(), written, v) == WireError::Flags);
  buf[10] = 0;

  // Bad opcode in Decode
  buf[8] = 0;
  CHECK(Decode(buf.data(), written, v) == WireError::Opcode);
  buf[8] = 4;
  CHECK(Decode(buf.data(), written, v) == WireError::Opcode);
  buf[8] = 2;

  // Bad opcode in Encode
  Header badOp = h;
  badOp.op = static_cast<Op>(0);
  CHECK(Encode(badOp, &ev, buf.data(), buf.size(), written) == WireError::Opcode);
  badOp.op = static_cast<Op>(4);
  CHECK(Encode(badOp, &ev, buf.data(), buf.size(), written) == WireError::Opcode);
}

static void TestShapeRejection() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(MaxPacketBytes + 1000);
  size_t written = 0;

  // count > MaxEvents in Encode: allocate actual 161 events so events array is genuinely bounded in heap
  Header badH = h;
  badH.count = MaxEvents + 1;
  std::vector<Event> events161(MaxEvents + 1, ev);
  CHECK(Encode(badH, events161.data(), buf.data(), buf.size(), written) == WireError::Shape);

  // Begin count != 0
  badH = h;
  badH.op = Op::Begin;
  badH.ordinal = 1;
  badH.trigger = NoTrigger;
  badH.attempted = 0;
  badH.lost = 0;
  badH.accepted = 0;
  badH.reason = 0;
  badH.count = 1;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Shape);

  // Begin reason != 0
  badH.count = 0;
  badH.reason = 1;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Shape);

  // Data count == 0
  badH = h;
  badH.op = Op::Data;
  badH.count = 0;
  badH.reason = 0;
  badH.attempted = 0;
  badH.lost = 0;
  badH.accepted = 0;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Shape);

  // Data reason != 0
  badH.count = 1;
  badH.reason = 1;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Shape);

  // Seal count != 0 (ensure all other Seal fields valid: trigger=NoTrigger, totals 0, reason 1)
  badH = h;
  badH.op = Op::Seal;
  badH.ordinal = 2;
  badH.trigger = NoTrigger;
  badH.attempted = 50;
  badH.accepted = 50;
  badH.lost = 0;
  badH.reason = 1;
  badH.count = 1;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Shape);

  // Seal reason < 1 or > 4 (with valid Seal fields)
  badH.count = 0;
  badH.reason = 0;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Shape);
  badH.reason = 5;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Shape);

  // payloadBytes != count * EventBytes in Decode
  CHECK(Encode(h, &ev, buf.data(), buf.size(), written) == WireError::None);
  buf[16] = 0; buf[17] = 0; // payloadBytes = 0 with count = 1
  View v{};
  CHECK(Decode(buf.data(), written, v) == WireError::Shape);
}

static void TestCapacityRejection() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;

  // Capacity boundaries: valid 4..262144
  Header badH = h;
  badH.capacity = 3;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Capacity);
  badH.capacity = MaxHistoryEvents + 1;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Capacity);

  badH.capacity = 4;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::None);
  badH.capacity = MaxHistoryEvents;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::None);
}

static void TestSessionRejection() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;

  // Header session == 0
  Header badH = h;
  badH.session = 0;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Session);

  // Event session != Header session
  Event badEv = ev;
  badEv.session = h.session + 1;
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::Session);

  // Event session == 0 in DecodeEvent
  badEv.session = 0;
  CHECK(Encode(h, &ev, buf.data(), buf.size(), written) == WireError::None);
  // Corrupt event session in buffer to 0
  buf[80 + 8] = 0; buf[80 + 9] = 0; buf[80 + 10] = 0; buf[80 + 11] = 0;
  buf[80 + 12] = 0; buf[80 + 13] = 0; buf[80 + 14] = 0; buf[80 + 15] = 0;
  Event dec{};
  CHECK(DecodeEvent(buf.data() + 80, EventBytes, dec) == WireError::Session);
}

static void TestOrdinalRejection() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;

  // ordinal == 0
  Header badH = h;
  badH.ordinal = 0;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Ordinal);

  // Begin with ordinal != 1 (all other Begin fields valid)
  badH.op = Op::Begin;
  badH.count = 0;
  badH.trigger = NoTrigger;
  badH.attempted = 0;
  badH.lost = 0;
  badH.accepted = 0;
  badH.reason = 0;
  badH.ordinal = 2;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Ordinal);
}

static void TestTriggerRejection() {
  Header h = MakeGoldenHeader();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;

  // Begin with trigger != NoTrigger (all other Begin fields valid)
  Header badH = h;
  badH.op = Op::Begin;
  badH.ordinal = 1;
  badH.count = 0;
  badH.attempted = 0;
  badH.lost = 0;
  badH.accepted = 0;
  badH.reason = 0;
  badH.trigger = 1;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Trigger);

  // Seal with trigger > accepted (all other Seal fields valid: count=0, valid totals, valid reason)
  badH.op = Op::Seal;
  badH.count = 0;
  badH.ordinal = 5;
  badH.reason = 1;
  badH.attempted = 100;
  badH.accepted = 100;
  badH.lost = 0;
  badH.trigger = 101;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Trigger);
}

static void TestTotalsRejection() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;

  // Begin with non-zero totals (all other Begin fields valid)
  Header badH = h;
  badH.op = Op::Begin;
  badH.ordinal = 1;
  badH.count = 0;
  badH.trigger = NoTrigger;
  badH.reason = 0;
  badH.attempted = 1;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Totals);
  badH.attempted = 0; badH.lost = 1;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Totals);
  badH.lost = 0; badH.accepted = 1;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Totals);

  // Data with non-zero totals (all other Data fields valid)
  badH = h;
  badH.attempted = 1;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Totals);
  badH.attempted = 0; badH.lost = 1;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Totals);
  badH.lost = 0; badH.accepted = 1;
  CHECK(Encode(badH, &ev, buf.data(), buf.size(), written) == WireError::Totals);

  // Seal: accepted > attempted (all other Seal fields valid: count=0, reason=1, trigger=NoTrigger)
  badH.op = Op::Seal;
  badH.count = 0;
  badH.reason = 1;
  badH.ordinal = 2;
  badH.trigger = NoTrigger;
  badH.attempted = 10;
  badH.accepted = 11;
  badH.lost = 0;
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Totals);

  // Seal: lost != attempted - accepted (all other Seal fields valid)
  badH.trigger = NoTrigger;
  badH.attempted = 100;
  badH.accepted = 80;
  badH.lost = 19; // 100 - 80 = 20 != 19
  CHECK(Encode(badH, nullptr, buf.data(), buf.size(), written) == WireError::Totals);
}

static void TestEventValueRejection() {
  Header h = MakeGoldenHeader();
  Event ev = MakeGoldenEvent();
  std::vector<uint8_t> buf(HeaderBytes + EventBytes);
  size_t written = 0;

  // sequence == 0
  Event badEv = ev;
  badEv.sequence = 0;
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::EventValue);

  // sequence == UINT64_MAX
  badEv.sequence = UINT64_MAX;
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::EventValue);

  // kind == 0
  badEv = ev;
  badEv.kind = static_cast<dxvk::war3::tools::evidence::Kind>(0);
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::EventValue);

  // kind == 19
  badEv.kind = static_cast<dxvk::war3::tools::evidence::Kind>(19);
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::EventValue);

  // kind == 1 and kind == 18 are valid
  badEv.kind = static_cast<dxvk::war3::tools::evidence::Kind>(1);
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::None);
  badEv.kind = static_cast<dxvk::war3::tools::evidence::Kind>(18);
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::None);

  // label without NUL terminator (all 32 bytes non-zero)
  badEv = ev;
  badEv.label.fill('X');
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::EventValue);

  // label with NUL at index 0 or index 31
  badEv.label[0] = '\0';
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::None);
  badEv.label.fill('A');
  badEv.label[31] = '\0';
  CHECK(Encode(h, &badEv, buf.data(), buf.size(), written) == WireError::None);
}

static void TestErrorNames() {
  CHECK(std::strcmp(ErrorName(WireError::None), "None") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Null), "Null") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Size), "Size") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Capacity), "Capacity") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Overlap), "Overlap") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Magic), "Magic") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Version), "Version") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Header), "Header") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Opcode), "Opcode") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Flags), "Flags") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Shape), "Shape") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Session), "Session") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Ordinal), "Ordinal") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Trigger), "Trigger") == 0);
  CHECK(std::strcmp(ErrorName(WireError::Totals), "Totals") == 0);
  CHECK(std::strcmp(ErrorName(WireError::EventValue), "EventValue") == 0);
  CHECK(std::strcmp(ErrorName(static_cast<WireError>(999)), "Unknown") == 0);
}

int main() {
  TestGoldenCodec();
  TestRoundtripBegin();
  TestRoundtripSeal();
  TestRoundtripData160();
  TestFloatBitsPreservation();
  TestNullBufferRejection();
  TestSizeAndTruncationRejection();
  TestEncodeWrittenAliasing();
  TestDecodeViewAliasing();
  TestDecodeEventAliasing();
  TestDestinationOverlapWithInputs();
  TestRangeOverflowAsOverlap();
  TestMagicVersionHeaderFlagsOpcode();
  TestShapeRejection();
  TestCapacityRejection();
  TestSessionRejection();
  TestOrdinalRejection();
  TestTriggerRejection();
  TestTotalsRejection();
  TestEventValueRejection();
  TestErrorNames();

  std::printf("checks=%u PASS\n", s_checks);
  return 0;
}