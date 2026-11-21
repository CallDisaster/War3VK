// What the x86 front end pays per fixed-function draw record, by path:
//   full      - the complete record copied into the batch, the batch copied into the ring;
//   plain     - the delta encoder in its first form (a compare and a branch per word), kept here;
//   tree      - EncodeFixedDeltaRecord from tools/render_host/d3d9_ff_delta_wire.h.
// It first checks that plain and tree write the same bytes. Message lengths follow the batch histogram
// of a 生与死 run; record contents are synthetic, sized so that the bytes sent per record are near the
// game's (about 270 B). Not a gate: timing only, the best of seven passes.
//
// Build (32-bit, the front end's flags), from the repository root:
//   i686-w64-mingw32-clang++ -std=c++17 -O2 -static -DNOMINMAX -Itools/render_host
//     AutoTest/bench_render_host_fixed_delta.cpp -o bench_fixed_delta.exe
//
// 2026-11-18, ns per record on top of producing the record: full 101, plain 980, tree 258 (sparser
// shape); 90, 1109, 305 (denser shape). In the game (生与死, 1,325 fixed draws per frame) the plain form
// cost the x86 thread 1.3 ms per frame against product-async; the tree form about 0.1 ms.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "d3d9_ff_delta_wire.h"

using namespace warvk::host;

static uint32_t seed = 0x12345u;
static uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

static uint32_t EncodePlain(const d9::FixedDrawPacket& full, d9::FixedDrawPacket& previous, uint8_t* out) {
  uint32_t blocks = 0, at = 4;
  for (uint32_t b = 0; b < d9::FixedDeltaBlocks; ++b) {
    const auto words = std::min(32u, d9::FixedDrawWords - b * 32);
    const auto* src = full.data() + b * 128; auto* base = previous.data() + b * 128;
    if (!std::memcmp(src, base, words * 4)) continue;
    blocks |= 1u << b; const auto maskAt = at; at += 4; uint32_t mask = 0;
    for (uint32_t w = 0; w < words; ++w) {
      const auto value = d9::U32(src + w * 4);
      if (value == d9::U32(base + w * 4)) continue;
      mask |= 1u << w; d9::P32(out + at, value); at += 4; d9::P32(base + w * 4, value);
    }
    d9::P32(out + maskAt, mask);
  }
  d9::P32(out, blocks);
  return at;
}

static const uint32_t Histogram[20] = {769050, 161558, 46612, 76727, 54178, 16927, 1615, 9082, 6145, 48258,
                                       3563, 3632, 5642, 3858, 9533, 3394, 203, 7466, 213, 99372};

struct Shape { uint32_t firstWords, firstBlocks, nextWords, nextBlocks; };

// One message's records, produced in place: the first is rebuilt from zero, the rest change a few words.
struct Source {
  Shape shape{}; d9::FixedDrawPacket packet{};
  uint32_t blocks[25]{};
  void pick(uint32_t count) {
    for (uint32_t i = 0; i < 25; ++i) blocks[i] = i;
    for (uint32_t i = 0; i < count; ++i) std::swap(blocks[i], blocks[i + rnd() % (25 - i)]);
  }
  void put(uint32_t block, uint32_t value) {
    const uint32_t words = std::min(32u, d9::FixedDrawWords - block * 32);
    d9::P32(packet.data() + (block * 32 + rnd() % words) * 4, value);
  }
  void first() {
    packet.fill(0); pick(shape.firstBlocks);
    for (uint32_t i = 0; i < shape.firstWords; ++i) put(blocks[i % shape.firstBlocks], rnd() | 1);
  }
  void next() {
    pick(shape.nextBlocks);
    for (uint32_t i = 0; i < shape.nextWords; ++i) put(blocks[i % shape.nextBlocks], rnd() | 1);
  }
};

static std::vector<uint8_t> lengths;  // message lengths, shuffled
static std::vector<uint8_t> ring(4u << 20);
static std::array<uint8_t, d9::MaxPayload> batch;
static d9::FixedDrawPacket previous;

enum class Mode { Generate, Full, Plain, Tree };

static double pass(Mode mode, const Shape& shape, uint64_t& records, uint64_t& sent, uint64_t& checksum) {
  seed = 0x12345u; Source source; source.shape = shape;
  uint32_t ringAt = 0; records = sent = checksum = 0;
  LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
  for (const uint8_t length : lengths) {
    uint32_t used = 0;
    if (mode == Mode::Plain || mode == Mode::Tree) previous.fill(0);
    for (uint32_t i = 0; i < length; ++i) {
      if (i) source.next(); else source.first();
      switch (mode) {
        case Mode::Generate: checksum += source.packet[rnd() % d9::FixedDrawBytes]; break;
        case Mode::Full: std::memcpy(batch.data() + used, source.packet.data(), d9::FixedDrawBytes); used += d9::FixedDrawBytes; break;
        case Mode::Plain: used += EncodePlain(source.packet, previous, batch.data() + used); break;
        case Mode::Tree: used += d9::EncodeFixedDeltaRecord(source.packet, previous, batch.data() + used, uint32_t(batch.size()) - used, false); break;
      }
    }
    if (used) {
      if (ringAt + used > ring.size()) ringAt = 0;
      std::memcpy(ring.data() + ringAt, batch.data(), used); ringAt += used;
      checksum += ring[ringAt - 1] + (uint64_t(used) << 8);
    }
    records += length; sent += used;
  }
  QueryPerformanceCounter(&b);
  return double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
}

int main() {
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  // 1/40 of the histogram: about 33,000 messages, 127,000 records.
  for (uint32_t n = 0; n < 20; ++n) for (uint32_t i = 0; i < Histogram[n] / 40; ++i) lengths.push_back(uint8_t(n + 1));
  for (size_t i = lengths.size(); i > 1; --i) std::swap(lengths[i - 1], lengths[rnd() % i]);
  {
    static std::array<uint8_t, d9::MaxPayload> other; static d9::FixedDrawPacket previousOther;
    uint64_t compared = 0;
    for (const Shape shape : {Shape{160, 12, 22, 7}, Shape{320, 16, 40, 10}, Shape{795, 25, 795, 25}, Shape{1, 1, 0, 1}}) {
      seed = 0x777u; Source source; source.shape = shape;
      for (const uint8_t length : lengths) {
        previous.fill(0); previousOther.fill(0);
        for (uint32_t i = 0; i < length; ++i) {
          if (i) source.next(); else source.first();
          const uint32_t x = EncodePlain(source.packet, previous, batch.data());
          const uint32_t y = d9::EncodeFixedDeltaRecord(source.packet, previousOther, other.data(), uint32_t(other.size()), false);
          if (!x || x != y || std::memcmp(batch.data(), other.data(), x) || previous != previousOther || previous != source.packet) {
            std::printf("MISMATCH at record %llu\n", static_cast<unsigned long long>(compared)); return 1;
          }
          ++compared;
        }
      }
    }
    std::printf("tree encoder equals the plain form over %llu records\n", static_cast<unsigned long long>(compared));
  }
  for (const Shape shape : {Shape{160, 12, 22, 7}, Shape{320, 16, 40, 10}}) {
    double best[4] = {1e9, 1e9, 1e9, 1e9}; uint64_t records = 0, sent[4] = {}, checksum = 0;
    for (int round = 0; round < 7; ++round)
      for (int mode = 0; mode < 4; ++mode) {
        uint64_t s = 0; const double t = pass(Mode(mode), shape, records, s, checksum);
        best[mode] = std::min(best[mode], t); sent[mode] = s;
      }
    const double per = 1e9 / double(records);
    std::printf("first record %u words in %u blocks, later records %u in %u: %llu records, %.0f delta bytes per record\n",
                shape.firstWords, shape.firstBlocks, shape.nextWords, shape.nextBlocks,
                static_cast<unsigned long long>(records), double(sent[3]) / double(records));
    std::printf("   producing the record %.0f ns; on top of it: full %.0f ns, plain %.0f ns, tree %.0f ns (bytes %s)\n",
                best[0] * per, (best[1] - best[0]) * per, (best[2] - best[0]) * per, (best[3] - best[0]) * per,
                sent[2] == sent[3] ? "equal" : "DIFFERENT");
  }
  return 0;
}
