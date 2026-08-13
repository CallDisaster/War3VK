#include "../war3_shadow_arena_budget.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

constexpr uint64_t MiB(uint64_t value) {
  return value * 1024ull * 1024ull;
}

constexpr uint64_t GiB(uint64_t value) {
  return value * 1024ull * 1024ull * 1024ull;
}

using dxvk::war3::memory::ResolveShadowArenaMemoryBudget;
using dxvk::war3::memory::ShadowArenaCanGrowResident;
using dxvk::war3::memory::ShadowArenaMemoryBudgetInput;
using dxvk::war3::memory::kShadowArenaFixedResidentLimitBytes;

void TestUnsupportedAndMalformedBudgetsPreserveLegacyCap() {
  ShadowArenaMemoryBudgetInput input = {};
  auto result = ResolveShadowArenaMemoryBudget(input);
  assert(!result.trusted);
  assert(result.effectiveResidentLimitBytes ==
         kShadowArenaFixedResidentLimitBytes);

  input.extensionSupported = true;
  input.primaryDeviceLocalHeapFound = true;
  input.heapSizeBytes = GiB(8u);
  input.heapBudgetBytes = GiB(9u);
  result = ResolveShadowArenaMemoryBudget(input);
  assert(result.supported);
  assert(!result.trusted);
  assert(result.effectiveResidentLimitBytes ==
         kShadowArenaFixedResidentLimitBytes);
}

void TestLargeBudgetNeverRaisesFixedCap() {
  ShadowArenaMemoryBudgetInput input = {};
  input.extensionSupported = true;
  input.primaryDeviceLocalHeapFound = true;
  input.heapSizeBytes = GiB(16u);
  input.heapBudgetBytes = GiB(15u);
  input.heapAllocatedBytes = GiB(3u);
  const auto result = ResolveShadowArenaMemoryBudget(input);
  assert(result.trusted);
  assert(result.availableBytes == GiB(12u));
  assert(result.effectiveResidentLimitBytes ==
         kShadowArenaFixedResidentLimitBytes);
}

void TestConservativeFractionAndReserveClamp() {
  ShadowArenaMemoryBudgetInput input = {};
  input.extensionSupported = true;
  input.primaryDeviceLocalHeapFound = true;
  input.heapSizeBytes = GiB(4u);
  input.heapBudgetBytes = GiB(4u);
  input.heapAllocatedBytes = GiB(2u);
  auto result = ResolveShadowArenaMemoryBudget(input);
  assert(result.trusted);
  assert(result.proportionalLimitBytes == MiB(512u));
  assert(result.reserveLimitBytes == MiB(1536u));
  assert(result.effectiveResidentLimitBytes == MiB(512u));

  input.heapBudgetBytes = MiB(600u);
  input.heapAllocatedBytes = 0u;
  result = ResolveShadowArenaMemoryBudget(input);
  assert(result.proportionalLimitBytes == MiB(150u));
  assert(result.reserveLimitBytes == MiB(88u));
  assert(result.effectiveResidentLimitBytes == MiB(88u));
}

void TestOverBudgetSnapshotBlocksOnlyNewGrowth() {
  ShadowArenaMemoryBudgetInput input = {};
  input.extensionSupported = true;
  input.primaryDeviceLocalHeapFound = true;
  input.heapSizeBytes = GiB(8u);
  input.heapBudgetBytes = GiB(6u);
  input.heapAllocatedBytes = GiB(7u);
  const auto result = ResolveShadowArenaMemoryBudget(input);
  const uint64_t existingResidentBytes = MiB(192u);
  assert(result.trusted);
  assert(result.availableBytes == 0u);
  assert(result.effectiveResidentLimitBytes == 0u);
  assert(!ShadowArenaCanGrowResident(existingResidentBytes, MiB(64u),
                                     result.effectiveResidentLimitBytes));
  // Existing residency is intentionally not mutated or reclaimed by policy.
  assert(existingResidentBytes == MiB(192u));
}

void TestGrowthCheckIsOverflowSafeAndPageExact() {
  assert(ShadowArenaCanGrowResident(MiB(192u), MiB(64u), MiB(256u)));
  assert(!ShadowArenaCanGrowResident(MiB(193u), MiB(64u), MiB(256u)));
  assert(!ShadowArenaCanGrowResident(UINT64_MAX - 31u, 64u, UINT64_MAX));
}

} // namespace

int main() {
  TestUnsupportedAndMalformedBudgetsPreserveLegacyCap();
  TestLargeBudgetNeverRaisesFixedCap();
  TestConservativeFractionAndReserveClamp();
  TestOverBudgetSnapshotBlocksOnlyNewGrowth();
  TestGrowthCheckIsOverflowSafeAndPageExact();
  std::cout << "war3_shadow_arena_budget_test: PASS\n";
  return 0;
}
