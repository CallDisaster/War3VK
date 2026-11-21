// Where the command ring's x86 views are mapped (tools/render_host/d3d9_high_va.h and
// d3d9_ring_channel.h), in real processes of each kind. Build this file three times and run each
// with the kind it is expected to be:
//
//   i686, plain link                      test.exe low    a stock game: not large-address-aware
//   i686, -Wl,--large-address-aware       test.exe high   a patched game
//   x86_64                                test.exe wide   the host's side of the same headers
//
//   <clang++> -std=c++17 -O2 -Wall -Wextra -Werror -static -pthread -municode -DNOMINMAX
//     AutoTest/test_render_host_high_va.cpp tools/render_host/protocol.cpp
//     tools/render_host/win32_security.cpp -ladvapi32 [-Wl,--large-address-aware]
//
// "low" must behave exactly as before the feature existed: every view below 2 GiB, no placement
// attempt made. "high" must put the views wholly above 2 GiB by either method, fall back to the
// plain call when no method is allowed, and carry real ring traffic between a producer on the
// high views and a consumer on its own (peer) views of the same sections.
#include "../tools/render_host/d3d9_ring_channel.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <random>
#include <vector>

using namespace warvk::host;
namespace ring = warvk::host::d9::ring;
namespace highva = warvk::host::d9::highva;
namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const char* what) {
  ++checks;
  if (!value && ++failures <= 24) std::fprintf(stderr, "FAILED: %s\n", what);
}
uint64_t Address(const void* p) { return uint64_t(reinterpret_cast<uintptr_t>(p)); }
uint8_t Pattern(uint64_t sequence, uint32_t index) { return uint8_t(sequence * 131u + index * 7u + (index >> 8)); }

ipc::Nonce NextNonce() {
  static uint64_t counter = 0;
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return {uint64_t(GetCurrentProcessId()) << 32 | ++counter, uint64_t(now.QuadPart) | 1};
}

// One whole view: committed, mapped, one allocation that starts at the view.
bool MappedWhole(const ring::Region& region) {
  MEMORY_BASIC_INFORMATION info{};
  return VirtualQuery(region.base(), &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT
    && info.Type == MEM_MAPPED && info.AllocationBase == region.base() && info.RegionSize >= region.bytes();
}

// A producer on the owner's views and a consumer on the peer's views of the same two sections.
struct Session {
  ipc::Nonce nonce = NextNonce();
  ring::OwnerChannel owner;
  ring::PeerChannel peer;
  ring::Producer producer;
  ring::Consumer consumer;
  std::unique_ptr<d9::Packet> packet = std::make_unique<d9::Packet>();
  std::vector<uint8_t> scratch = std::vector<uint8_t>(d9::MaxPayload);

  Session(uint32_t capacity, const highva::Process* high, unsigned methods)
    : owner(nonce, capacity, high, methods), peer(nonce, capacity),
      producer(owner.producerBlock(), owner.data(), owner.consumerBlock(), owner.reply(), capacity, 1),
      consumer(peer.producerBlock(), peer.data(), peer.consumerBlock(), peer.reply(), capacity, nonce, 1) {}
  d9::Header header(uint64_t sequence, uint32_t bytes) const {
    d9::Header h; h.op = d9::Op::Clear; h.nonce = nonce; h.sequence = sequence; h.payloadBytes = bytes;
    h.args[0] = uint32_t(sequence * 2654435761u); h.args[15] = ~uint32_t(sequence);
    return h;
  }
  ring::Producer::Write write(uint64_t sequence, uint32_t bytes) {
    uint8_t encoded[d9::HeaderBytes];
    check(d9::EncodeHeader(header(sequence, bytes), encoded), "header encodes");
    for (uint32_t i = 0; i < bytes; ++i) scratch[i] = Pattern(sequence, i);
    return producer.tryWrite(encoded, bytes ? scratch.data() : nullptr, bytes, sequence, false);
  }
  bool expect(uint64_t sequence, uint32_t bytes) {
    d9::Header h; bool sync = true;
    if (consumer.tryRead(*packet, h, sync) != ring::Consumer::Read::Item || sync) return false;
    uint8_t encoded[d9::HeaderBytes];
    if (!d9::EncodeHeader(header(sequence, bytes), encoded) || std::memcmp(encoded, packet->data(), d9::HeaderBytes)) return false;
    for (uint32_t i = 0; i < bytes; ++i)
      if ((*packet)[d9::HeaderBytes + i] != Pattern(sequence, i)) return false;
    return true;
  }
  // Mixed traffic over several wraps of the ring, the producer always ahead by what fits.
  void traffic(unsigned steps, unsigned seed) {
    std::mt19937 random(seed);
    uint64_t written = 1, read = 1; std::vector<uint32_t> sizes; size_t first = 0;
    for (unsigned step = 0; step < steps; ++step) {
      if (random() % 3) {
        const uint32_t roll = random() % 100;
        const uint32_t bytes = roll < 60 ? random() % 256 : roll < 90 ? random() % 4096 : random() % (d9::MaxPayload + 1);
        const auto result = write(written + 1, bytes);
        if (result == ring::Producer::Write::Written) { ++written; sizes.push_back(bytes); }
        else check(result == ring::Producer::Write::Full, "only Full refuses");
      } else if (read < written) {
        check(expect(read + 1, sizes[first]), "order and content across the two processes' views"); ++read; ++first;
      }
    }
    while (read < written) { check(expect(read + 1, sizes[first]), "drain"); ++read; ++first; }
    if (owner.capacity() == ring::MinCapacity)
      check(producer.tail() > uint64_t(owner.capacity()) * 4, "the traffic wrapped the ring several times");
  }
};

void Arithmetic() {
  const auto at = [](uint64_t value) { return reinterpret_cast<const void*>(uintptr_t(value)); };
  const uint64_t top = 0xFFFEFFFFull; // a large-address-aware WOW64 process
  check(highva::WhollyHigh(at(0x80000000ull), 1, top) && highva::WhollyHigh(at(0x80000000ull), 0x7FFF0000ull, top), "from the boundary");
  check(!highva::WhollyHigh(at(0x7FFFF000ull), 0x2000, top) && !highva::WhollyHigh(at(0x7FFFFFFFull), 1, top), "below or across the boundary");
  check(highva::WhollyHigh(at(0xFFFE0000ull), 0x10000, top) && !highva::WhollyHigh(at(0xFFFE0000ull), 0x10001, top), "the last page");
  check(!highva::WhollyHigh(at(0x80000000ull), 0, top), "an empty range is not a placement");
  check(!highva::WhollyHigh(at(0x80000000ull), 1, 0x7FFEFFFFull), "a process without such addresses");
  highva::Process none;
  check(!none.eligible(), "a default process is not eligible");
  highva::Process flagOnly; flagOnly.exeFlag = true; flagOnly.maxApp = 0x7FFEFFFFull;
  highva::Process rangeOnly; rangeOnly.maxApp = top;
  check(!flagOnly.eligible() && !rangeOnly.eligible(), "the header and the range must agree");
}

enum class Kind { Low, High, Wide };

void Placement(Kind kind, const highva::Process& process) {
  struct Case { const char* name; bool request; unsigned methods; highva::Method expect; };
  const Case cases[] = {
    {"not requested", false, highva::AllMethods, highva::Method::None},
    {"automatic", true, highva::AllMethods, highva::Method::MapViewOfFile3},
    {"scan only", true, unsigned(highva::Method::Scan), highva::Method::Scan},
    {"no method allowed", true, 0, highva::Method::None},
  };
  for (const auto& c : cases) {
    for (const uint32_t capacity : {ring::MinCapacity, ring::DefaultCapacity, 16u << 20, ring::MaxCapacity}) {
      Session session(capacity, c.request ? &process : nullptr, c.methods);
      const auto& producer = session.owner.producerRegion();
      const auto& consumer = session.owner.consumerRegion();
      check(MappedWhole(producer) && MappedWhole(consumer), "both views are whole mappings");
      check(producer.bytes() == ring::ControlBytes + capacity && consumer.bytes() == ring::ControlBytes + ring::ReplyCapacity, "view sizes");
      const bool high = kind == Kind::High && c.expect != highva::Method::None;
      if (kind != Kind::Wide) {
        check(highva::WhollyHigh(producer.base(), producer.bytes(), process.maxApp) == high
          && highva::WhollyHigh(consumer.base(), consumer.bytes(), process.maxApp) == high, c.name);
        if (!high)
          check(Address(producer.base()) + producer.bytes() <= highva::Boundary
            && Address(consumer.base()) + consumer.bytes() <= highva::Boundary, "a plain view lies wholly below 2 GiB");
      }
      // Windows 10 1803 and later have MapViewOfFile3; the scan is the answer anywhere else.
      const auto method = producer.placement().method;
      check(high ? (method == c.expect || (c.expect == highva::Method::MapViewOfFile3 && method == highva::Method::Scan))
                 : method == highva::Method::None, "placement method");
      check(consumer.placement().method == method, "both views by the same method");
      if (kind != Kind::High || !c.request || !c.methods)
        check(!producer.placement().attempts && !consumer.placement().attempts, "no placement was attempted");
      // The first and last byte of both views, seen from the other side's mapping.
      session.owner.data()[0] = 0x5a; session.owner.data()[capacity - 1] = 0xa5;
      check(session.peer.data()[0] == 0x5a && session.peer.data()[capacity - 1] == 0xa5, "producer view is shared end to end");
      session.peer.reply()[0] = 0x3c; session.peer.reply()[ring::ReplyCapacity - 1] = 0xc3;
      check(session.owner.reply()[0] == 0x3c && session.owner.reply()[ring::ReplyCapacity - 1] == 0xc3, "consumer view is shared end to end");
      session.owner.data()[0] = 0; session.owner.data()[capacity - 1] = 0;
      session.traffic(capacity == ring::MinCapacity ? 60000 : 6000, 20261118u + capacity);
      std::printf("high-va %-18s capacity=%8u producer=0x%09llx consumer=0x%09llx method=%s attempts=%u\n", c.name, capacity,
        static_cast<unsigned long long>(Address(producer.base())), static_cast<unsigned long long>(Address(consumer.base())),
        highva::MethodName(method), producer.placement().attempts);
    }
  }
  // Views are returned: the address space does not fill up over many sessions.
  uint64_t firstBase = 0; bool stable = true;
  for (unsigned i = 0; i < 200; ++i) {
    Session session(16u << 20, &process, highva::AllMethods);
    const auto base = Address(session.owner.producerRegion().base());
    if (!i) firstBase = base;
    if (kind == Kind::High) stable = stable && highva::WhollyHigh(session.owner.producerRegion().base(), session.owner.producerRegion().bytes(), process.maxApp);
    else if (kind == Kind::Low) stable = stable && base + session.owner.producerRegion().bytes() <= highva::Boundary;
  }
  check(stable && firstBase, "200 sessions in a row keep their placement");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  const Kind kind = argc > 1 && !std::wcscmp(argv[1], L"high") ? Kind::High
    : argc > 1 && !std::wcscmp(argv[1], L"wide") ? Kind::Wide : Kind::Low;
  const auto process = highva::Probe();
  std::printf("high-va process exeLargeAddress=%d maxApp=0x%llx eligible=%d pointerBytes=%u\n", int(process.exeFlag),
    static_cast<unsigned long long>(process.maxApp), int(process.eligible()), unsigned(sizeof(void*)));
  Arithmetic();
  if (kind == Kind::Low)
    check(sizeof(void*) == 4 && !process.exeFlag && process.maxApp < highva::Boundary && !process.eligible(), "a stock 32-bit process");
  else if (kind == Kind::High)
    check(sizeof(void*) == 4 && process.exeFlag && process.maxApp >= highva::Boundary && process.eligible(), "a large-address-aware 32-bit process");
  else
    check(sizeof(void*) == 8 && !process.eligible(), "a 64-bit process is never eligible");
  try {
    Placement(kind, process);
  } catch (const std::exception& e) {
    check(false, e.what());
  }
  std::printf("render host high-va %s: %u checks, %u failures\n", kind == Kind::High ? "high" : kind == Kind::Wide ? "wide" : "low",
    checks, failures);
  return failures ? 1 : 0;
}
