#include "../war3_shadow_arena_lifecycle.h"

#include <cassert>

int main() {
  using namespace dxvk::war3::memory;

  ShadowArenaGenerationLease lease = {};
  assert(ShadowArenaTryAcquireGeneration(lease, 10u, 0u));
  assert(lease.allocatable);
  assert(ShadowArenaQuarantineGeneration(lease, 10u));
  assert(!lease.allocatable);
  assert(!ShadowArenaTryAcquireGeneration(lease, 11u, 9u));
  assert(!ShadowArenaTryAcquireGeneration(lease, 11u, 0u));
  assert(ShadowArenaTryAcquireGeneration(lease, 11u, 10u));
  assert(!ShadowArenaQuarantineGeneration(lease, 0u));

  ShadowArenaGenerationLease second = {};
  assert(ShadowArenaTryAcquireGeneration(second, 20u, 0u));
  assert(ShadowArenaQuarantineGeneration(second, 22u));
  assert(!ShadowArenaGenerationCanBeReused(second.retireSerial, 21u));
  assert(ShadowArenaGenerationCanBeReused(second.retireSerial, 22u));
  return 0;
}
