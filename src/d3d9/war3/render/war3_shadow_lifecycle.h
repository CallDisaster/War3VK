#pragma once

#include <cstdint>
#include <vector>

namespace dxvk::war3::render {

enum class ShadowCasterTombstoneReason : uint8_t {
  Hidden = 0u,
  Removed,
  StageDisabled,
  Replaced,
  Count,
};

struct ShadowCasterIdentity {
  void* unitPtr = nullptr;
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* renderablePart = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  int16_t producerStage = -1;

  bool hasStrongIdentity() const {
    return unitPtr != nullptr || worldObjectEntry != nullptr ||
           sceneNode != nullptr || renderablePart != nullptr ||
           jHandle != 0u;
  }
};

struct ShadowCasterTombstone {
  ShadowCasterIdentity identity = {};
  ShadowCasterTombstoneReason reason =
      ShadowCasterTombstoneReason::Removed;
  // Identity tombstones are valid only inside the map epoch in which the
  // event was published. Stage policy tombstones are process-scoped and use
  // zero so an explicit disabled stage remains disabled after a map change.
  uint64_t mapEpoch = 0u;
  uint64_t eventSerial = 0u;
  uint64_t stagePolicyRevision = 0u;
  uint64_t visibleFrameSerial = 0u;
  uintptr_t callerRva = 0u;
};

struct ShadowCasterLifecycleDiagnostics {
  uint64_t publishedCount = 0u;
  uint64_t acknowledgedFreshCount = 0u;
  uint64_t activeCount = 0u;
  uint64_t queueOverflowCount = 0u;
  uint64_t lastEventSerial = 0u;
  uint64_t stagePolicyRevision = 0u;
  uint64_t hiddenCount = 0u;
  uint64_t removedCount = 0u;
  uint64_t stageDisabledCount = 0u;
  uint64_t replacedCount = 0u;
};

bool ShadowCasterIdentityMatches(
    const ShadowCasterIdentity& lhs,
    const ShadowCasterIdentity& rhs);

uint64_t PublishShadowCasterTombstone(
    ShadowCasterTombstone tombstone);

void AcknowledgeFreshShadowCaster(
    const ShadowCasterIdentity& identity,
    uint64_t visibleFrameSerial,
    uint64_t stagePolicyRevision);

bool IsShadowCasterTombstoned(
    const ShadowCasterIdentity& identity);

uint64_t CurrentShadowCasterTombstoneSerial();

// Starts a new identity domain and returns the event serial at the boundary.
// Existing stage-policy state is preserved, while raw pointer/handle identity
// tombstones from the previous map become ineligible immediately.
uint64_t ResetShadowCasterLifecycleMapEpoch(uint64_t mapEpoch);

bool ShadowCasterTombstoneBelongsToMap(
    const ShadowCasterTombstone& tombstone, uint64_t mapEpoch);

uint64_t CurrentShadowStagePolicyRevision();

uint64_t AdvanceShadowStagePolicyRevision();

uint64_t PublishShadowStagePolicyTransition(
    int16_t producerStage,
    bool enabled,
    uint64_t visibleFrameSerial = 0u);

void SnapshotShadowCasterTombstonesAfter(
    uint64_t afterSerial,
    std::vector<ShadowCasterTombstone>& out,
    bool& historyOverflowed);

ShadowCasterLifecycleDiagnostics
QueryShadowCasterLifecycleDiagnostics();

const char* DescribeShadowCasterTombstoneReason(
    ShadowCasterTombstoneReason reason);

} // namespace dxvk::war3::render
