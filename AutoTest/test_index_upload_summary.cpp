#include "../src/d3d9/war3/render/war3_index_upload_summary.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace dxvk::war3::render;
static unsigned checks = 0;
static volatile uint64_t benchmarkSink = 0;
#define CHECK(x) do {++checks; if (!(x)) throw std::runtime_error(#x);} while(false)
int main(){try{
  std::vector<uint8_t> source(20000), target(20000);
  uint32_t rng = 0x20260919;
  const auto random = [&]{rng = rng * 1664525u + 1013904223u; return rng;};
  IndexUploadBudget budget;
  for (uint32_t trial = 0; trial < 10000; ++trial) {
    const uint32_t width = trial % 2 ? 2 : 4, bytes = (1 + random() % 4096) * width;
    for (auto& b : source) b = uint8_t(random() >> 16);
    const auto serial = budget.next(trial);
    auto proof = IndexUploadSummary::Copy(target.data(), source.data(), bytes, width, 123, 1, serial, budget, true);
    const auto p = proof.query(target.data(), bytes, width, 123, 1, serial);
    CHECK(p.valid && std::memcmp(source.data(), target.data(), bytes) == 0);
    uint32_t lo = UINT32_MAX, hi = 0; uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < bytes; i += width) {
      uint32_t value = 0; std::memcpy(&value, source.data() + i, width);
      lo = std::min(lo, value); hi = std::max(hi, value); hash = (hash ^ value) * 0x100000001b3ull;
    }
    CHECK(p.min == lo && p.max == hi && p.hash == hash);
    CHECK(!proof.query(target.data(), bytes, width, 123, 2, serial).valid);
    CHECK(!proof.query(target.data(), bytes, width, 123, 1, serial + 1).valid);
    CHECK(!proof.query(target.data() + 1, bytes, width, 123, 1, serial).valid);
    CHECK(!proof.query(target.data(), bytes - 1, width, 123, 1, serial).valid);
    CHECK(!proof.query(target.data(), bytes, width == 2 ? 4 : 2, 123, 1, serial).valid);
    CHECK(!proof.query(target.data(), bytes, width, 124, 1, serial).valid);
  }
  for (bool enabled : {false, true}) for (uint32_t bytes : {0u, 3u, 16388u}) {
    budget.next(20000 + bytes);
    auto proof = IndexUploadSummary::Copy(target.data(), source.data(), bytes, 2, 123, 1, budget.serial, budget, enabled);
    CHECK(!proof.query(target.data(), bytes, 2, 123, 1, budget.serial).valid);
    CHECK(std::memcmp(source.data(), target.data(), bytes) == 0);
  }
  budget = {}; budget.next(1);
  for (unsigned i = 0; i < 17; ++i) {
    const auto serial = budget.next(1);
    auto proof = IndexUploadSummary::Copy(target.data(), source.data(), 16384, 2, 123, 1, serial, budget, true);
    CHECK(proof.query(target.data(), 16384, 2, 123, 1, serial).valid == (i < 16));
  }
  CHECK(budget.bytes == 256 * 1024 && budget.bypassed == 1);
  budget.next(2); CHECK(budget.bytes == 0);
  budget.serial = UINT64_MAX; CHECK(budget.next(3) == 0 && budget.next(4) == 0);

  // B07 targeted bypass and offset boundaries. These are intentionally small
  // and cover identities not exercised by the 10k randomized valid-size loop.
  const auto fillBytes = [&](uint32_t bytes, uint8_t seed) {
    for (uint32_t i = 0; i < bytes; ++i)
      source[i] = uint8_t(seed + (i * 29u));
  };
  const auto targetMatches = [&](uint32_t bytes) {
    return std::memcmp(source.data(), target.data(), bytes) == 0;
  };

  budget = {};
  fillBytes(16384u, 0x11u);
  std::memset(target.data(), 0, 16384);
  uint64_t b07Serial = budget.next(7u);
  auto b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 16384u, 2u, 123u, 1u, b07Serial, budget, false);
  CHECK(!b07Proof.query(target.data(), 16384u, 2u, 123u, 1u, b07Serial).valid);
  CHECK(targetMatches(16384u));
  CHECK(budget.bytes == 0u && budget.copiedWithProof == 0u && budget.bypassed == 0u);

  budget = {};
  fillBytes(4096u, 0x21u);
  std::memset(target.data(), 0, 4096);
  b07Serial = budget.next(7u);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 4096u, 4u, 0u, 1u, b07Serial, budget, true);
  CHECK(!b07Proof.query(target.data(), 4096u, 4u, 0u, 1u, b07Serial).valid);
  CHECK(targetMatches(4096u));
  CHECK(budget.bytes == 0u && budget.copiedWithProof == 0u && budget.bypassed == 1u);

  budget = {};
  fillBytes(4096u, 0x31u);
  std::memset(target.data(), 0, 4096);
  b07Serial = 0u;
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 4096u, 4u, 123u, 1u, b07Serial, budget, true);
  CHECK(!b07Proof.query(target.data(), 4096u, 4u, 123u, 1u, b07Serial).valid);
  CHECK(targetMatches(4096u));
  CHECK(budget.bytes == 0u && budget.copiedWithProof == 0u && budget.bypassed == 1u);

  budget = {};
  budget.serial = UINT64_MAX;
  CHECK(budget.next(7u) == 0u);
  fillBytes(1024u, 0x41u);
  std::memset(target.data(), 0, 1024);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 1024u, 2u, 123u, 1u, 0u, budget, true);
  CHECK(!b07Proof.query(target.data(), 1024u, 2u, 123u, 1u, 0u).valid);
  CHECK(targetMatches(1024u));
  CHECK(budget.bypassed == 1u);
  CHECK(budget.next(8u) == 0u);

  budget = {};
  fillBytes(4096u, 0x51u);
  std::memset(target.data(), 0, 4096);
  b07Serial = budget.next(7u);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 4096u, 1u, 123u, 1u, b07Serial, budget, true);
  CHECK(!b07Proof.query(target.data(), 4096u, 1u, 123u, 1u, b07Serial).valid);
  CHECK(targetMatches(4096u));
  CHECK(budget.bypassed == 1u);

  budget = {};
  fillBytes(4098u, 0x61u);
  std::memset(target.data(), 0, 4098);
  b07Serial = budget.next(7u);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 4098u, 4u, 123u, 1u, b07Serial, budget, true);
  CHECK(!b07Proof.query(target.data(), 4098u, 4u, 123u, 1u, b07Serial).valid);
  CHECK(targetMatches(4098u));
  CHECK(budget.bypassed == 1u);

  budget = {};
  fillBytes(16384u, 0x71u);
  std::memset(target.data(), 0, 16384);
  b07Serial = budget.next(7u);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 16384u, 4u, 123u, 1u, b07Serial, budget, true);
  CHECK(b07Proof.query(target.data(), 16384u, 4u, 123u, 1u, b07Serial).valid);
  CHECK(targetMatches(16384u));
  CHECK(budget.bytes == 16384u && budget.copiedWithProof == 1u && budget.bypassed == 0u);

  // Exact 256 KiB mixed-size boundary, then one aligned UINT16 over budget.
  budget = {};
  fillBytes(16384u, 0x81u);
  std::memset(target.data(), 0, 16384);
  for (unsigned i = 0; i < 15u; ++i) {
    b07Serial = budget.next(9u);
    b07Proof = IndexUploadSummary::Copy(
        target.data(), source.data(), 16384u, 2u, 123u, 1u, b07Serial, budget, true);
    CHECK(b07Proof.query(target.data(), 16384u, 2u, 123u, 1u, b07Serial).valid);
    CHECK(targetMatches(16384u));
  }
  for (unsigned i = 0; i < 2u; ++i) {
    fillBytes(8192u, uint8_t(0x91u + i));
    std::memset(target.data(), 0, 8192);
    b07Serial = budget.next(9u);
    b07Proof = IndexUploadSummary::Copy(
        target.data(), source.data(), 8192u, 2u, 123u, 1u, b07Serial, budget, true);
    CHECK(b07Proof.query(target.data(), 8192u, 2u, 123u, 1u, b07Serial).valid);
    CHECK(targetMatches(8192u));
  }
  CHECK(budget.bytes == 256u * 1024u && budget.copiedWithProof == 17u && budget.bypassed == 0u);
  fillBytes(2u, 0xA1u);
  std::memset(target.data(), 0, 2);
  b07Serial = budget.next(9u);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 2u, 2u, 123u, 1u, b07Serial, budget, true);
  CHECK(!b07Proof.query(target.data(), 2u, 2u, 123u, 1u, b07Serial).valid);
  CHECK(targetMatches(2u));
  CHECK(budget.bytes == 256u * 1024u && budget.bypassed == 1u);

  // A per-draw bypass must not consume the per-frame scan budget.
  budget = {};
  fillBytes(16388u, 0xB1u);
  std::memset(target.data(), 0, 16388);
  b07Serial = budget.next(10u);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 16388u, 2u, 123u, 1u, b07Serial, budget, true);
  CHECK(!b07Proof.query(target.data(), 16388u, 2u, 123u, 1u, b07Serial).valid);
  CHECK(targetMatches(16388u));
  CHECK(budget.bytes == 0u && budget.bypassed == 1u);
  fillBytes(16384u, 0xC1u);
  std::memset(target.data(), 0, 16384);
  b07Serial = budget.next(10u);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 16384u, 2u, 123u, 1u, b07Serial, budget, true);
  CHECK(b07Proof.query(target.data(), 16384u, 2u, 123u, 1u, b07Serial).valid);
  CHECK(targetMatches(16384u));
  CHECK(budget.bytes == 16384u && budget.copiedWithProof == 1u && budget.bypassed == 1u);

  budget.next(11u);
  CHECK(budget.bytes == 0u && budget.copiedWithProof == 1u && budget.bypassed == 1u);

  // Nonzero source byte offset; UINT16 and UINT32 index reads.
  for (uint32_t width : {2u, 4u}) {
    budget = {};
    const uint32_t sourceOffset = width == 2u ? 34u : 52u;
    const uint32_t copyBytes = 256u * width;
    fillBytes(20000u, width == 2u ? 0xD1u : 0xD9u);
    std::memset(target.data(), 0, target.size());
    b07Serial = budget.next(12u);
    b07Proof = IndexUploadSummary::Copy(
        target.data(), source.data() + sourceOffset, copyBytes, width,
        123u, 1u, b07Serial, budget, true);
    const auto range = b07Proof.query(target.data(), copyBytes, width, 123u, 1u, b07Serial);
    CHECK(range.valid);
    CHECK(std::memcmp(source.data() + sourceOffset, target.data(), copyBytes) == 0);
    uint32_t lo = UINT32_MAX, hi = 0u;
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < copyBytes; i += width) {
      uint32_t value = 0u;
      std::memcpy(&value, source.data() + sourceOffset + i, width);
      lo = std::min(lo, value);
      hi = std::max(hi, value);
      hash = (hash ^ value) * 0x100000001b3ull;
    }
    CHECK(range.min == lo && range.max == hi && range.hash == hash);
  }

  // Exact identity rejection table after one valid proof.
  budget = {};
  fillBytes(2048u, 0xE1u);
  std::memset(target.data(), 0, 2048);
  b07Serial = budget.next(13u);
  b07Proof = IndexUploadSummary::Copy(
      target.data(), source.data(), 2048u, 4u, 0x1234u, 0x5678u, b07Serial, budget, true);
  CHECK(b07Proof.query(target.data(), 2048u, 4u, 0x1234u, 0x5678u, b07Serial).valid);
  CHECK(!b07Proof.query(target.data(), 2048u, 4u, 0x1235u, 0x5678u, b07Serial).valid);
  CHECK(!b07Proof.query(target.data(), 2048u, 4u, 0x1234u, 0x5679u, b07Serial).valid);
  CHECK(!b07Proof.query(target.data(), 2048u, 4u, 0x1234u, 0x5678u, b07Serial + 1u).valid);
  CHECK(!b07Proof.query(target.data() + 1, 2048u, 4u, 0x1234u, 0x5678u, b07Serial).valid);
  CHECK(!b07Proof.query(target.data(), 2047u, 4u, 0x1234u, 0x5678u, b07Serial).valid);
  CHECK(!b07Proof.query(target.data(), 2048u, 2u, 0x1234u, 0x5678u, b07Serial).valid);

  // Signed base/stride checks remain in the existing consumer, not this proof.
  // Approximate helper cost only: not GPU or main-thread p95/p99.
  auto volatile memcpyFunction = &std::memcpy;
  const auto plainBegin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < 10000; ++i) {
    memcpyFunction(target.data(), source.data(), 16384);
    benchmarkSink = benchmarkSink ^ target[i % 16384];
  }
  const auto plainUs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - plainBegin).count();
  budget = {}; const auto begin = std::chrono::steady_clock::now();
  auto volatile copyFunction = &IndexUploadSummary::Copy;
  for (uint32_t i = 0; i < 10000; ++i) {
    const auto serial = budget.next(i);
    const auto proof = copyFunction(target.data(), source.data(), 16384, 2, 123, 1, serial, budget, true);
    const auto range = proof.query(target.data(), 16384, 2, 123, 1, serial);
    CHECK(range.valid);
    benchmarkSink = benchmarkSink ^ range.hash ^ range.min ^ range.max;
  }
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
  std::cout << "checks=" << checks << " failures=0 boundedCopy16KiB10000_us=" << us
            << " plainMemcpy16KiB10000_us=" << plainUs << '\n';
  return 0;
}catch(const std::exception& e){std::cerr << e.what() << '\n';return 1;}}
