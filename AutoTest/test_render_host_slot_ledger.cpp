#include "../tools/render_host/slot_ledger.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

using namespace warvk::host;
using namespace warvk::host::slots;
static unsigned checks = 0;
static void CheckAt(bool condition, unsigned line) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "RH1 check failed line=%u checks=%u\n", line, checks);
    std::exit(1);
  }
}
#define CHECK(x) CheckAt(bool(x), __LINE__)
static_assert(!std::is_copy_constructible_v<SlotLedger> && !std::is_move_constructible_v<SlotLedger>);
static_assert(!std::is_copy_constructible_v<ReadPermit> && !std::is_move_assignable_v<ReadPermit>);
static_assert(std::is_nothrow_move_constructible_v<ReadPermit>);
static_assert(!std::is_reference_v<decltype(std::declval<const ReadPermit&>().key())>);
static constexpr ipc::Nonce Connection{0x0807060504030201ull, 0x1817161514131211ull};

static void Complete(SlotLedger& ledger, const Key& key) {
  CHECK(ledger.publish(key) == Error::None);
  ReadPermit permit;
  CHECK(ledger.beginRead(key, permit) == Error::None);
  CHECK(permit.active() && permit.key() == key);
  CHECK(ledger.finishRead(permit) == Error::None);
  CHECK(!permit.active());
}

static void Invariant(const SlotLedger& ledger) {
  auto snapshot = ledger.snapshot();
  CHECK(snapshot.cursor < SlotCount);
  unsigned readers = 0;
  for (uint32_t i = 0; i < SlotCount; ++i) {
    if (snapshot.generations[i]) {
      CHECK(snapshot.keys[i].slot == i);
      CHECK(snapshot.keys[i].generation == snapshot.generations[i]);
      CHECK(snapshot.keys[i].bytes && snapshot.keys[i].bytes <= SlotBytes);
    }
    if (snapshot.states[i] == SlotState::Reading)
      ++readers;
  }
  CHECK(ledger.localReadersDone() == (readers == 0));
}

static Key GoldenKey() { return {Connection, 3, 7, 9, 15, 2, SlotBytes}; }

static void Codec() {
  auto key = GoldenKey();
  std::array<uint8_t, DescriptorBytes + 1> buffer{};
  CHECK(EncodeKey(key, buffer.data(), DescriptorBytes) == Error::None);
  CHECK(!std::memcmp(buffer.data(), "WVL1", 4));
  CHECK(ipc::ReadLe(buffer.data() + 56, 4) == 2);
  Key output{};
  CHECK(DecodeKey(buffer.data(), DescriptorBytes, output) == Error::None && output == key);
  for (size_t n = 0; n < DescriptorBytes; ++n) {
    output = key;
    CHECK(DecodeKey(buffer.data(), n, output) == Error::DescriptorSize);
    CHECK(output == Key{});
  }
  CHECK(DecodeKey(buffer.data(), buffer.size(), output) == Error::DescriptorSize);
  CHECK(DecodeKey(nullptr, DescriptorBytes, output) == Error::NullBuffer);
  CHECK(EncodeKey(key, nullptr, DescriptorBytes) == Error::NullBuffer);
  CHECK(EncodeKey(key, buffer.data(), DescriptorBytes - 1) == Error::DescriptorSize);
  auto original = buffer;
  for (size_t offset : {0u, 4u, 6u}) {
    output = key;
    buffer = original;
    buffer[offset] ^= 0x80;
    CHECK(DecodeKey(buffer.data(), DescriptorBytes, output) != Error::None);
    CHECK(output == Key{});
  }
  for (unsigned field = 0; field < 8; ++field) {
    auto bad = key;
    switch (field) {
      case 0: bad.connection = {}; break;
      case 1: bad.map = 0; break;
      case 2: bad.device = 0; break;
      case 3: bad.frame = 0; break;
      case 4: bad.generation = 0; break;
      case 5: bad.slot = SlotCount; break;
      case 6: bad.bytes = 0; break;
      case 7: bad.bytes = SlotBytes + 1; break;
    }
    buffer = original;
    CHECK(EncodeKey(bad, buffer.data(), DescriptorBytes) != Error::None);
    CHECK(buffer == original);
  }
  uint32_t random = 0x76a2931;
  for (unsigned i = 0; i < 10000; ++i) {
    random = random * 1664525u + 1013904223u;
    buffer = original;
    buffer[random % DescriptorBytes] ^= uint8_t(1u << ((random >> 16) & 7));
    Key decoded{};
    auto result = DecodeKey(buffer.data(), DescriptorBytes, decoded);
    if (result == Error::None) {
      std::array<uint8_t, DescriptorBytes> reencoded{};
      CHECK(EncodeKey(decoded, reencoded.data(), reencoded.size()) == Error::None);
      CHECK(!std::memcmp(buffer.data(), reencoded.data(), reencoded.size()));
    } else {
      CHECK(decoded == Key{});
    }
  }
}

static void CapacityAndFields() {
  SlotLedger ledger(Connection);
  CHECK(ledger.snapshot().phase == Phase::Dormant);
  CHECK(ledger.beginEpoch(3, 7) == Error::None);
  std::array<Key, SlotCount> keys;
  for (uint32_t i = 0; i < SlotCount; ++i) {
    CHECK(ledger.reserve(i + 1, SlotBytes - i, keys[i]) == Error::None);
    CHECK(keys[i].slot == i && keys[i].generation == 1);
  }
  auto before = ledger.snapshot();
  Key sentinel = GoldenKey();
  CHECK(ledger.reserve(5, 1, sentinel) == Error::Capacity);
  CHECK(ledger.snapshot() == before && sentinel == GoldenKey());
  CHECK(ledger.endEpoch() == Error::SlotState && ledger.snapshot() == before);
  for (const auto& key : keys) {
    for (unsigned field = 0; field < 8; ++field) {
      auto bad = key;
      switch (field) {
        case 0: bad.connection.low ^= 1; break;
        case 1: bad.connection.high ^= 1; break;
        case 2: ++bad.map; break;
        case 3: ++bad.device; break;
        case 4: ++bad.frame; break;
        case 5: ++bad.generation; break;
        case 6: bad.slot = (bad.slot + 1) % SlotCount; break;
        case 7: --bad.bytes; break;
      }
      auto snapshot = ledger.snapshot();
      CHECK(ledger.publish(bad) != Error::None && ledger.snapshot() == snapshot);
      CHECK(ledger.cancelWrite(bad) != Error::None && ledger.snapshot() == snapshot);
      ReadPermit rejected;
      CHECK(ledger.beginRead(bad, rejected) != Error::None && !rejected.active());
      CHECK(ledger.snapshot() == snapshot);
    }
  }
  Complete(ledger, keys[1]);
  Key replacement;
  CHECK(ledger.reserve(5, 27, replacement) == Error::None);
  CHECK(replacement.slot == keys[1].slot && replacement.generation == 2);
  before = ledger.snapshot();
  CHECK(ledger.publish(keys[1]) == Error::Lease && ledger.snapshot() == before);
  Complete(ledger, replacement);
  for (uint32_t i : {0u, 2u, 3u})
    Complete(ledger, keys[i]);
  CHECK(ledger.endEpoch() == Error::None);
  before = ledger.snapshot();
  for (auto epochs : {std::pair{3ull, 7ull}, std::pair{2ull, 8ull}, std::pair{4ull, 6ull}, std::pair{0ull, 8ull}})
    CHECK(ledger.beginEpoch(epochs.first, epochs.second) == Error::Epoch && ledger.snapshot() == before);
  CHECK(ledger.beginEpoch(3, 8) == Error::None);
  Key newMap;
  CHECK(ledger.reserve(1, 17, newMap) == Error::None);
  before = ledger.snapshot();
  CHECK(ledger.publish(keys[newMap.slot]) == Error::Lease && ledger.snapshot() == before);
  Complete(ledger, newMap);
  CHECK(ledger.endEpoch() == Error::None);
  Invariant(ledger);
  SlotLedger dropped(Connection);
  CHECK(dropped.beginEpoch(1, 1) == Error::None);
  Key dropKey;
  CHECK(dropped.reserve(1, 1, dropKey) == Error::None);
  CHECK(dropped.publish(dropKey) == Error::None);
  {
    ReadPermit lost;
    CHECK(dropped.beginRead(dropKey, lost) == Error::None);
  }
  CHECK(!dropped.localReadersDone());
  CHECK(dropped.endEpoch() == Error::SlotState);
  dropped.retire();
  CHECK(!dropped.localReadersDone());
}

static void PermitsAndRetire() {
  SlotLedger ledger(Connection), foreign(Connection);
  CHECK(ledger.beginEpoch(1, 1) == Error::None);
  CHECK(foreign.beginEpoch(1, 1) == Error::None);
  Key first, second, foreignKey;
  CHECK(ledger.reserve(1, 24, first) == Error::None);
  CHECK(ledger.reserve(2, 24, second) == Error::None);
  CHECK(foreign.reserve(1, 24, foreignKey) == Error::None && first == foreignKey);
  CHECK(ledger.publish(first) == Error::None);
  CHECK(ledger.publish(second) == Error::None);
  auto before = ledger.snapshot();
  CHECK(ledger.publish(first) == Error::SlotState && ledger.snapshot() == before);
  ReadPermit permit;
  CHECK(ledger.beginRead(first, permit) == Error::None);
  auto observed = permit.key();
  observed.generation = UINT64_MAX;
  CHECK(permit.key() == first && !(observed == permit.key()));
  before = ledger.snapshot();
  CHECK(ledger.beginRead(second, permit) == Error::OutputBusy && ledger.snapshot() == before);
  auto foreignBefore = foreign.snapshot();
  CHECK(foreign.finishRead(permit) == Error::Permit && permit.active());
  CHECK(foreign.snapshot() == foreignBefore && ledger.snapshot() == before);
  ReadPermit moved(std::move(permit));
  CHECK(moved.active() && !permit.active());
  CHECK(ledger.finishRead(permit) == Error::Permit && ledger.snapshot() == before);
  CHECK(ledger.beginRead(second, permit) == Error::OutputUsed && ledger.snapshot() == before);
  CHECK(ledger.finishRead(moved) == Error::None);
  before = ledger.snapshot();
  CHECK(ledger.finishRead(moved) == Error::Permit && ledger.snapshot() == before);
  CHECK(ledger.beginRead(second, moved) == Error::OutputUsed && ledger.snapshot() == before);
  ReadPermit secondPermit;
  CHECK(ledger.beginRead(second, secondPermit) == Error::None);
  CHECK(!ledger.localReadersDone());
  ledger.retire();
  CHECK(ledger.snapshot().phase == Phase::Retired && !ledger.localReadersDone());
  before = ledger.snapshot();
  Key out;
  CHECK(ledger.reserve(3, 3, out) == Error::Phase && ledger.snapshot() == before);
  CHECK(ledger.publish(second) == Error::Phase && ledger.snapshot() == before);
  CHECK(ledger.beginEpoch(2, 2) == Error::Phase && ledger.snapshot() == before);
  CHECK(ledger.endEpoch() == Error::Phase && ledger.snapshot() == before);
  CHECK(ledger.finishRead(secondPermit) == Error::None);
  CHECK(ledger.localReadersDone());
  for (auto state : ledger.snapshot().states)
    CHECK(state == SlotState::Retired);
  before = ledger.snapshot();
  ledger.retire();
  CHECK(ledger.snapshot() == before);
  Invariant(ledger);
}

static void CancelAndNewConnection() {
  SlotLedger old(Connection);
  CHECK(old.beginEpoch(1, 1) == Error::None);
  Key cancelled;
  CHECK(old.reserve(1, 20, cancelled) == Error::None);
  CHECK(old.cancelWrite(cancelled) == Error::None);
  auto before = old.snapshot();
  CHECK(old.publish(cancelled) == Error::SlotState && old.snapshot() == before);
  CHECK(old.cancelWrite(cancelled) == Error::SlotState && old.snapshot() == before);
  ReadPermit denied;
  CHECK(old.beginRead(cancelled, denied) == Error::SlotState && !denied.active());
  for (uint64_t frame = 2; frame < 35; ++frame) {
    Key key;
    CHECK(old.reserve(frame, 1, key) == Error::None && key.slot != cancelled.slot);
    Complete(old, key);
  }
  CHECK(old.snapshot().states[cancelled.slot] == SlotState::Quarantined);
  CHECK(old.endEpoch() == Error::SlotState);
  old.retire();
  SlotLedger next({Connection.low + 1, Connection.high});
  CHECK(next.beginEpoch(1, 1) == Error::None);
  Key key;
  CHECK(next.reserve(1, 20, key) == Error::None);
  before = next.snapshot();
  CHECK(next.publish(cancelled) == Error::Lease && next.snapshot() == before);
  Complete(next, key);
  CHECK(next.endEpoch() == Error::None);
  SlotLedger full(Connection);
  CHECK(full.beginEpoch(1, 1) == Error::None);
  for (uint64_t frame = 1; frame <= SlotCount; ++frame) {
    CHECK(full.reserve(frame, 2, key) == Error::None);
    CHECK(full.cancelWrite(key) == Error::None);
  }
  before = full.snapshot();
  CHECK(full.reserve(5, 2, key) == Error::Capacity && full.snapshot() == before);
  CHECK(full.endEpoch() == Error::SlotState && full.snapshot() == before);
  full.retire();
  for (auto state : full.snapshot().states)
    CHECK(state == SlotState::Retired);
}

static void LimitsAndInvalid() {
  for (auto limits : {Limits{0, 10}, Limits{10, 0}}) {
    SlotLedger bad(Connection, limits);
    CHECK(bad.snapshot().phase == Phase::Invalid);
    CHECK(bad.beginEpoch(1, 1) == Error::Phase);
  }
  SlotLedger empty({});
  CHECK(empty.snapshot().phase == Phase::Invalid);
  SlotLedger ledger(Connection, {2, UINT64_MAX});
  CHECK(ledger.beginEpoch(1, 1) == Error::None);
  Key key;
  auto before = ledger.snapshot();
  for (uint64_t frame : {0ull, 2ull, UINT64_MAX})
    CHECK(ledger.reserve(frame, 1, key) == Error::Frame && ledger.snapshot() == before);
  for (uint32_t bytes : {0u, SlotBytes + 1, UINT32_MAX})
    CHECK(ledger.reserve(1, bytes, key) == Error::Size && ledger.snapshot() == before);
  for (uint64_t frame = 1; frame <= 8; ++frame) {
    CHECK(ledger.reserve(frame, 1, key) == Error::None);
    Complete(ledger, key);
  }
  before = ledger.snapshot();
  CHECK(ledger.reserve(9, 1, key) == Error::GenerationExhausted && ledger.snapshot() == before);
  CHECK(ledger.endEpoch() == Error::None);
  CHECK(ledger.beginEpoch(1, 2) == Error::None);
  before = ledger.snapshot();
  CHECK(ledger.reserve(1, 1, key) == Error::GenerationExhausted && ledger.snapshot() == before);
  SlotLedger bigEpoch(Connection);
  CHECK(bigEpoch.beginEpoch(UINT64_MAX, UINT64_MAX) == Error::None);
  CHECK(bigEpoch.reserve(1, 1, key) == Error::None);
  Complete(bigEpoch, key);
  CHECK(bigEpoch.endEpoch() == Error::None);
  before = bigEpoch.snapshot();
  CHECK(bigEpoch.beginEpoch(1, 1) == Error::Epoch && bigEpoch.snapshot() == before);
  SlotLedger frameLimit(Connection, {UINT64_MAX, 3});
  CHECK(frameLimit.beginEpoch(1, 1) == Error::None);
  for (uint64_t frame = 1; frame <= 3; ++frame) {
    CHECK(frameLimit.reserve(frame, 1, key) == Error::None);
    Complete(frameLimit, key);
  }
  before = frameLimit.snapshot();
  CHECK(frameLimit.reserve(4, 1, key) == Error::FrameExhausted && frameLimit.snapshot() == before);
  CHECK(frameLimit.endEpoch() == Error::None);
  CHECK(frameLimit.beginEpoch(1, 2) == Error::None);
  CHECK(frameLimit.reserve(1, 1, key) == Error::None);
  Complete(frameLimit, key);
}

static void Cycles() {
  SlotLedger ledger(Connection);
  uint64_t successful = 0;
  for (uint64_t epoch = 1; epoch <= 250; ++epoch) {
    CHECK(ledger.beginEpoch(epoch, 1) == Error::None);
    for (uint64_t group = 0; group < 10; ++group) {
      std::array<Key, SlotCount> keys;
      std::array<ReadPermit, SlotCount> permits;
      for (uint32_t i = 0; i < SlotCount; ++i) {
        CHECK(ledger.reserve(group * 4 + i + 1, 1 + uint32_t((epoch * 17 + i) % SlotBytes), keys[i]) == Error::None);
        CHECK(ledger.publish(keys[i]) == Error::None);
        CHECK(ledger.beginRead(keys[i], permits[i]) == Error::None);
        ++successful;
      }
      auto before = ledger.snapshot();
      Key denied;
      CHECK(ledger.reserve(group * 4 + 5, 1, denied) == Error::Capacity && ledger.snapshot() == before);
      for (uint32_t i : {2u, 0u, 3u, 1u}) {
        auto old = keys[i];
        old.generation = old.generation == UINT64_MAX ? 1 : old.generation + 1;
        CHECK(ledger.publish(old) == Error::Lease);
        CHECK(ledger.finishRead(permits[i]) == Error::None);
        before = ledger.snapshot();
        CHECK(ledger.finishRead(permits[i]) == Error::Permit && ledger.snapshot() == before);
      }
      Invariant(ledger);
    }
    CHECK(ledger.endEpoch() == Error::None);
  }
  CHECK(successful == 10000);
}

int main(int argc, char** argv) {
  if (argc == 2 && !std::strcmp(argv[1], "--golden")) {
    std::array<uint8_t, DescriptorBytes> bytes{};
    if (EncodeKey(GoldenKey(), bytes.data(), bytes.size()) != Error::None)
      return 2;
    for (auto byte : bytes)
      std::printf("%02x", byte);
    std::printf("\n");
    return 0;
  }
  Codec(); CapacityAndFields(); PermitsAndRetire(); CancelAndNewConnection(); LimitsAndInvalid(); Cycles();
  std::printf("RH1 slot/core checks=%u bits=%u positiveRecords=10000 PASS\n", checks, unsigned(sizeof(void*) * 8));
}
