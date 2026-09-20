#include "../war3_shadow_arena_budget.h"
#include <cassert>
#include <cstdint>
#include <iostream>

using namespace dxvk::war3::memory;
constexpr uint64_t MiB(uint64_t n) { return n * MemoryMiB; }
static AdaptiveBudgetInput Input() {
  AdaptiveBudgetInput in{};
  in.vaValid = true; in.availableVA = MiB(512);
  in.hardCap = in.fallbackCap = kShadowArenaFixedResidentLimitBytes;
  in.pageBytes = MiB(64); in.quotaEighths = 3;
  return in;
}
int main() {
  auto in = Input();
  auto out = ResolveShadowArenaMemoryBudget(in);
  assert(!out.trusted && out.effectiveResidentLimitBytes == MiB(1152));
  in.supported = in.heapValid = true;
  in.heapSize = MiB(8192); in.budget = MiB(9216);
  out = ResolveShadowArenaMemoryBudget(in);
  assert(!out.trusted && out.effectiveResidentLimitBytes == MiB(1152));
  in.budget = MiB(8192); in.committed = MiB(2048);
  out = ResolveShadowArenaMemoryBudget(in);
  assert(out.trusted && out.availableBytes == MiB(6144));
  assert(out.effectiveResidentLimitBytes == MiB(1152));

  // Quota and backing headroom differ. Existing 384 MiB is ALREADY in the
  // physical usage, so 128 MiB headroom permits one more 64 MiB page.
  in.budget = MiB(2048); in.committed = MiB(1408); in.resident = MiB(384);
  out = ResolveShadowArenaMemoryBudget(in);
  assert(out.proportionalLimitBytes == MiB(768));
  assert(out.reserveLimitBytes == MiB(128));
  assert(out.effectiveResidentLimitBytes == MiB(512));
  assert(ShadowArenaCanGrowResident(in.resident, MiB(64), out.effectiveResidentLimitBytes));
  in.budget = MiB(600); in.committed = in.resident = 0;
  out = ResolveShadowArenaMemoryBudget(in);
  assert(out.proportionalLimitBytes == MiB(225));
  assert(out.reserveLimitBytes == MiB(88));
  assert(out.effectiveResidentLimitBytes == MiB(88));

  in.budget = MiB(6144); in.committed = MiB(7168); in.resident = MiB(192);
  out = ResolveShadowArenaMemoryBudget(in);
  assert(out.trusted && out.availableBytes == 0 && out.effectiveResidentLimitBytes == 0);
  assert(in.resident == MiB(192)); // pure decision never revokes live storage
  in.committed = 0; in.vaValid = false;
  assert(ResolveShadowArenaMemoryBudget(in).effectiveResidentLimitBytes == 0);
  in.vaValid = true; in.availableVA = MiB(255);
  assert(ResolveShadowArenaMemoryBudget(in).effectiveResidentLimitBytes == 0);
  in.availableVA = MiB(512); in.resident = UINT64_MAX - 1;
  assert(ResolveShadowArenaMemoryBudget(in).effectiveResidentLimitBytes == MiB(1152));
  assert(ShadowArenaCanGrowResident(MiB(192), MiB(64), MiB(256)));
  assert(!ShadowArenaCanGrowResident(MiB(193), MiB(64), MiB(256)));
  assert(!ShadowArenaCanGrowResident(UINT64_MAX - 31, 64, UINT64_MAX));
  std::cout << "war3_shadow_arena_budget_test: shared production policy PASS\n";
}
