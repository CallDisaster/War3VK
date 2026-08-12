#include "../war3_current_draw_group_slot_summary.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using dxvk::war3::render::CurrentDrawGroupSlotSummary;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_current_draw_group_slot_summary_test: " << message
              << '\n';
  return condition;
}

uint64_t referenceStableHash(const std::array<uint8_t, 8>& slots,
                             uint32_t payloadWord48,
                             uint32_t payloadWord108,
                             uint32_t payloadWord11C,
                             uint32_t layerIndex) {
  uint64_t hash = dxvk::bit::fnv1a_init();
  for (uint8_t slot : slots)
    hash = dxvk::bit::fnv1a_iter(hash, uint32_t(slot));
  hash = dxvk::bit::fnv1a_iter(hash, 1u);
  hash = dxvk::bit::fnv1a_iter(hash, payloadWord48);
  hash = dxvk::bit::fnv1a_iter(hash, payloadWord108);
  hash = dxvk::bit::fnv1a_iter(hash, payloadWord11C);
  return dxvk::bit::fnv1a_iter(hash, layerIndex);
}

bool testSinglePassSummary() {
  constexpr std::array<uint8_t, 8> slots = {0u, 3u, 1u, 7u,
                                             2u, 7u, 4u, 1u};
  constexpr uint32_t payloadWord48 = 1u;
  constexpr uint32_t payloadWord108 = 0x1234u;
  constexpr uint32_t payloadWord11C = 0x5678u;
  constexpr uint32_t layerIndex = 5u;
  CurrentDrawGroupSlotSummary summary = {};
  for (uint8_t slot : slots)
    summary.include(uint32_t(slot));

  const uint64_t stable = summary.stableHash(
      1u, payloadWord48, payloadWord108, payloadWord11C, layerIndex);
  if (!require(stable == referenceStableHash(
                            slots, payloadWord48, payloadWord108,
                            payloadWord11C, layerIndex),
               "single-pass stable hash diverged from the historical oracle"))
    return false;
  if (!require(summary.maxGroupSlot == 7u,
               "single-pass maximum group slot is wrong"))
    return false;

  const uint64_t diagnosticA = summary.diagnosticHash(
      uintptr_t(0x1000u), 1u, payloadWord48, payloadWord108, payloadWord11C,
      layerIndex);
  const uint64_t diagnosticB = summary.diagnosticHash(
      uintptr_t(0x2000u), 1u, payloadWord48, payloadWord108, payloadWord11C,
      layerIndex);
  return require(diagnosticA != diagnosticB,
                 "diagnostic hash no longer distinguishes stream addresses") &&
         require(stable == summary.stableHash(
                               1u, payloadWord48, payloadWord108,
                               payloadWord11C, layerIndex),
                 "stable hash changed with no content change");
}

bool testImmutableHintRequiresExactBytesAndGeneration() {
  constexpr std::array<uint8_t, 6> immutable = {0u, 2u, 1u, 3u, 2u, 1u};
  CurrentDrawGroupSlotSummary summary = {};
  for (uint8_t slot : immutable)
    summary.include(slot);

  dxvk::war3::render::CurrentDrawImmutableGroupSlotHint hint = {
      immutable.data(), immutable.size(), summary.slotHash,
      summary.maxGroupSlot, 17u};
  if (!require(hint.matches(immutable.data(), immutable.size(), 4u),
               "exact immutable bytes were rejected"))
    return false;
  if (!require(!hint.matches(immutable.data(), immutable.size(), 3u),
               "palette upper bound was not enforced"))
    return false;

  auto changed = immutable;
  changed[4] = 1u;
  if (!require(!hint.matches(changed.data(), changed.size(), 4u),
               "different current bytes reused immutable summary"))
    return false;
  hint.immutableGeneration = 0u;
  return require(!hint.matches(immutable.data(), immutable.size(), 4u),
                 "unversioned immutable summary was accepted");
}

} // namespace

int main() {
  return testSinglePassSummary() &&
          testImmutableHintRequiresExactBytesAndGeneration()
      ? 0 : 1;
}
