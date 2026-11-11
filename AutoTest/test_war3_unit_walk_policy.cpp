#include "../src/d3d9/war3/game/war3_unit_walk_policy.h"

#include <cassert>
#include <cstdint>
#include <thread>

using dxvk::war3::game::IsAutomaticWalkRequest;
using dxvk::war3::game::IsDirectCUnitHandleProof;
using dxvk::war3::game::UnitWalkPolicy;

int main() {
  assert(IsAutomaticWalkRequest(0x64C754u, 5, 0x40, 1));
  assert(IsAutomaticWalkRequest(0x64C893u, 5, 0x40, 1));
  assert(!IsAutomaticWalkRequest(0x64C754u, 4, 0x40, 1));
  assert(!IsAutomaticWalkRequest(0x64C754u, 5, 0, 1));
  assert(!IsAutomaticWalkRequest(0x64C754u, 5, 0x40, 0));
  assert(!IsAutomaticWalkRequest(0x1F7383u, 5, 0x40, 1));
  constexpr uintptr_t kGameBase = 0x6F000000u;
  assert(IsDirectCUnitHandleProof(0x48616D67u, kGameBase + 0x67BB90u,
                                  kGameBase)); // Hamg direct CUnit fixture
  assert(!IsDirectCUnitHandleProof(0u, kGameBase + 0x67BB90u, kGameBase));
  assert(!IsDirectCUnitHandleProof(0x48616D67u, kGameBase + 0x67BB91u,
                                   kGameBase));
  assert(!IsDirectCUnitHandleProof(0x48616D67u, kGameBase + 0x67BB90u,
                                   0u));

  UnitWalkPolicy policy;
  assert(policy.count() == 0u);
  assert(policy.disabledIdentity(0x12340u).jHandle == 0u);
  assert(!policy.set(0u, {0x12340u, 0x100123u}, false));
  assert(!policy.set(0x12340u, {}, false));
  assert(policy.set(0x12340u, {0x12340u, 0x100123u}, false));
  assert(policy.count() == 1u);
  assert(policy.disabledIdentity(0x12340u).jHandle == 0x100123u);
  assert(policy.set(0x12340u, {0x12340u, 0x100124u}, false));
  assert(policy.count() == 1u);
  assert(policy.disabledIdentity(0x12340u).jHandle == 0x100124u);
  assert(policy.set(0x12340u, {0x12340u, 0x100124u}, true));
  assert(policy.count() == 0u);
  assert(policy.disabledIdentity(0x12340u).jHandle == 0u);

  // A deleted entry must not hide a later entry on the same probe chain.
  for (uintptr_t i = 1u; i <= UnitWalkPolicy::kCapacity; ++i)
    assert(policy.set(i << 4u,
                      {i << 4u, 0x100000u | static_cast<uint32_t>(i)}, false));
  assert(policy.count() == UnitWalkPolicy::kCapacity);
  assert(!policy.set(0x888880u, {0x888880u, 0x100777u}, false));
  for (uintptr_t i = 1u; i <= UnitWalkPolicy::kCapacity; i += 2u)
    policy.erase(i << 4u);
  for (uintptr_t i = 2u; i <= UnitWalkPolicy::kCapacity; i += 2u)
    assert(policy.disabledIdentity(i << 4u).jHandle ==
           (0x100000u | static_cast<uint32_t>(i)));
  assert(policy.count() == UnitWalkPolicy::kCapacity / 2u);
  for (uintptr_t i = 1u; i <= UnitWalkPolicy::kCapacity; i += 2u)
    assert(policy.set((i + 1024u) << 4u,
                      {(i + 1024u) << 4u,
                       0x100000u | static_cast<uint32_t>(i)}, false));
  assert(policy.count() == UnitWalkPolicy::kCapacity);
  policy.reset();
  assert(policy.count() == 0u);
  assert(policy.disabledIdentity(2u << 4u).jHandle == 0u);

  // The JASS setter and animation callback may arrive on different threads;
  // publish/lookup/clear must remain data-race-free even if that assumption
  // changes. This is a host concurrency check, not game-runtime proof.
  std::thread writer([&] {
    for (uint32_t i = 0u; i < 10000u; ++i)
      assert(policy.set(0x34560u, {0x34560u, 0x100345u},
                        (i & 1u) != 0u));
  });
  for (uint32_t i = 0u; i < 10000u; ++i) {
    const uint32_t value = policy.disabledIdentity(0x34560u).jHandle;
    assert(value == 0u || value == 0x100345u);
  }
  writer.join();
  policy.reset();
  return 0;
}
