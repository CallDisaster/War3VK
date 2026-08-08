#include "../war3_shadow_lifecycle.h"

#include <cassert>
#include <cstdint>

namespace dxvk::war3::render {

// The lifecycle unit only reports optional producer telemetry through this
// hook. Keep the runnable value-only and independent from the live renderer.
void NoteShadowStageRetired(int, uint32_t) {
}

} // namespace dxvk::war3::render

int main() {
  using namespace dxvk::war3::render;

  ShadowCasterIdentity identity = {};
  identity.sceneNode = reinterpret_cast<void*>(uintptr_t{0x1000u});
  identity.producerStage = 11;

  ResetShadowCasterLifecycleMapEpoch(100u);
  ShadowCasterTombstone removed = {};
  removed.identity = identity;
  removed.reason = ShadowCasterTombstoneReason::Removed;
  const uint64_t firstSerial = PublishShadowCasterTombstone(removed);
  assert(firstSerial != 0u);
  assert(IsShadowCasterTombstoned(identity));

  const uint64_t boundary = ResetShadowCasterLifecycleMapEpoch(200u);
  assert(boundary >= firstSerial);
  assert(!IsShadowCasterTombstoned(identity));

  ShadowCasterTombstone currentRemoved = {};
  currentRemoved.identity = identity;
  currentRemoved.reason = ShadowCasterTombstoneReason::Removed;
  assert(PublishShadowCasterTombstone(currentRemoved) > boundary);
  assert(IsShadowCasterTombstoned(identity));
  AcknowledgeFreshShadowCaster(
      identity, 2u, CurrentShadowStagePolicyRevision());
  assert(!IsShadowCasterTombstoned(identity));

  PublishShadowStagePolicyTransition(11, false, 3u);
  assert(IsShadowCasterTombstoned(identity));
  ResetShadowCasterLifecycleMapEpoch(300u);
  assert(IsShadowCasterTombstoned(identity));
  PublishShadowStagePolicyTransition(11, true, 4u);
  assert(!IsShadowCasterTombstoned(identity));

  ShadowCasterTombstone oldMap = {};
  oldMap.identity = identity;
  oldMap.reason = ShadowCasterTombstoneReason::Hidden;
  oldMap.mapEpoch = 200u;
  assert(!ShadowCasterTombstoneBelongsToMap(oldMap, 300u));
  oldMap.mapEpoch = 300u;
  assert(ShadowCasterTombstoneBelongsToMap(oldMap, 300u));

  return 0;
}
