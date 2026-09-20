#include "../tools/render_host/slot_wire.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
using namespace warvk::host;
namespace {
unsigned checks = 0;
void Check(bool v) { ++checks; if (!v) throw std::runtime_error("wire check=" + std::to_string(checks)); }
slotwire::Packet Golden(size_t& bytes) {
  slotwire::Header h; h.nonce = {0x0807060504030201ull, 0x1817161514131211ull}; h.payloadBytes = 24;
  const auto hello = slotwire::Hello(32, 64); slotwire::Packet p{};
  Check(slotwire::Encode(h, hello.data(), hello.size(), p.data(), p.size(), bytes) == slotwire::Error::None);
  return p;
}
}
int main(int argc, char** argv) {
  try {
    size_t bytes = 0; const auto golden = Golden(bytes);
    if (argc == 2 && !std::strcmp(argv[1], "--golden")) {
      for (size_t i = 0; i < bytes; ++i) std::printf("%02x", unsigned(golden[i]));
      std::printf("\n"); return 0;
    }
    Check(argc == 1 && bytes == 72);
    slotwire::View view;
    Check(slotwire::Decode(golden.data(), bytes, view) == slotwire::Error::None);
    const auto hello = slotwire::Hello(32, 64);
    Check(!std::memcmp(view.payload, hello.data(), hello.size()));
    for (size_t length = 0; length < bytes; ++length) {
      view.payload = golden.data(); view.header.payloadBytes = 99;
      Check(slotwire::Decode(golden.data(), length, view) != slotwire::Error::None);
      Check(view.payload == nullptr && view.header.nonce.empty() && view.header.payloadBytes == 0);
    }
    struct Mutation { size_t offset; uint8_t value; slotwire::Error error; };
    for (const auto m : {Mutation{0, 'X', slotwire::Error::Magic}, {4, 2, slotwire::Error::Version},
      {6, 1, slotwire::Error::Version}, {8, 49, slotwire::Error::HeaderSize}, {10, 9, slotwire::Error::Opcode},
      {12, 2, slotwire::Error::Flags}, {16, 129, slotwire::Error::PayloadLimit}, {20, 1, slotwire::Error::Reserved}}) {
      auto changed = golden; changed[m.offset] = m.value;
      Check(slotwire::Decode(changed.data(), bytes, view) == m.error);
      Check(!view.payload && view.header.nonce.empty());
    }
    auto emptyNonce = golden; std::memset(emptyNonce.data() + 24, 0, 16);
    Check(slotwire::Decode(emptyNonce.data(), bytes, view) == slotwire::Error::EmptyNonce);
    Check(slotwire::Decode(nullptr, bytes, view) == slotwire::Error::NullBuffer);
    Check(slotwire::Decode(golden.data(), bytes + 1, view) == slotwire::Error::PacketSize);
    for (unsigned op = 1; op <= 8; ++op) for (unsigned flag = 0; flag <= 1; ++flag) {
      for (unsigned size = 0; size <= 128; ++size) {
        slotwire::Header h; h.nonce = {1, 2}; h.op = slotwire::Op(op); h.flags = flag; h.payloadBytes = size;
        std::array<uint8_t, 128> payload{}; slotwire::Packet p{};
        size_t count = 0;
        Check(slotwire::Encode(h, payload.data(), size, p.data(), p.size(), count) == slotwire::Error::None);
        Check(count == 48 + size && slotwire::Decode(p.data(), count, view) == slotwire::Error::None);
        Check(view.header.op == h.op && view.header.flags == flag && view.header.payloadBytes == size);
      }
    }
    uint32_t rng = 0x147aa23u;
    for (unsigned i = 0; i < 10000; ++i) {
      rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
      auto mutated = golden; mutated[rng % bytes] ^= uint8_t((rng >> 8) | 1);
      const auto error = slotwire::Decode(mutated.data(), bytes, view);
      if (error == slotwire::Error::None) {
        slotwire::Packet rewritten{}; size_t count = 0;
        Check(slotwire::Encode(view.header, view.payload, view.header.payloadBytes,
          rewritten.data(), rewritten.size(), count) == slotwire::Error::None);
        Check(count == bytes && !std::memcmp(mutated.data(), rewritten.data(), bytes));
      } else Check(!view.payload && view.header.nonce.empty());
    }
    slotwire::Header h; h.nonce = {1, 2}; h.flags = 2;
    auto unchanged = golden; size_t written = 99;
    Check(slotwire::Encode(h, nullptr, 0, unchanged.data(), unchanged.size(), written) == slotwire::Error::Flags);
    Check(!written && unchanged == golden);
    h.flags = 0; h.payloadBytes = 129;
    Check(slotwire::Encode(h, golden.data(), 129, unchanged.data(), unchanged.size(), written) == slotwire::Error::PayloadLimit);
    Check(!written && unchanged == golden);
    std::printf("{\"checks\":%u,\"bits\":%u,\"ok\":true}\n", checks, unsigned(sizeof(void*) * 8));
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
