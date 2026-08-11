#include "../../../d3d9_war3_scene.h"
#include "../war3_shadow_capture_frontend.h"
#include "../war3_shadow_drawtime_cache_policy.h"
#include "../war3_shadow_generation_backed_stream.h"
#include "../war3_shadow_pinned_upload_policy.h"
#include "../war3_shadow_stage11_allocation_observer.h"
#include "../war3_stage11_snapshot_page_policy.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

namespace policy = dxvk::war3::render;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_shadow_producer_completeness_policy_test: "
              << message << '\n';
  return condition;
}

bool testProducerCompletenessStampAndSaturation() {
  dxvk::War3ShadowProducerCompleteness completeness = {};
  if (!require(!completeness.accepts(41u, 7u, 9u),
               "unsealed zero contract was accepted"))
    return false;
  completeness.seal(41u, 7u, 9u);
  if (!require(completeness.accepts(41u, 7u, 9u),
               "matching sealed empty contract was rejected") ||
      !require(!completeness.accepts(42u, 7u, 9u),
               "frame stamp mismatch was accepted") ||
      !require(!completeness.accepts(41u, 8u, 9u),
               "map stamp mismatch was accepted"))
    return false;
  completeness.note(dxvk::War3RequiredCasterOmissionReason::IndexAllocBudget,
                    true);
  if (!require(!completeness.accepts(41u, 7u, 9u) &&
                   completeness.requiredCasterOmissionCount == 1u &&
                   completeness.indexAllocBudgetCount == 1u,
               "required caster omission did not reject publication"))
    return false;
  bool overflow = false;
  if (!require(dxvk::War3SaturatingAddU64(
                       std::numeric_limits<uint64_t>::max(), 1u, overflow) ==
                   std::numeric_limits<uint64_t>::max() && overflow,
               "counter saturation did not fail closed"))
    return false;
  return true;
}

bool testProtectedWorkingSetLru() {
  struct Entry { uint64_t key; uint64_t last; uint64_t bytes; };
  constexpr uint64_t mib = 1024u * 1024u;
  constexpr uint64_t protectFrames =
      policy::kWar3ShadowDrawTimeStaticRecentProtectFrames;
  constexpr uint64_t frame = protectFrames + 100u;
  constexpr uint64_t inactiveTarget = 64u * mib;
  std::vector<Entry> entries = {
      {3u, frame, 48u * mib},
      {2u, frame - protectFrames, 40u * mib},
      {9u, frame - protectFrames - 1u, 40u * mib},
      {5u, frame - protectFrames - 1u, 40u * mib},
  };
  if (!require(protectFrames >=
                   policy::kWar3ShadowDrawTimeVBCacheGcIntervalFrames,
               "static working-set protection is shorter than GC cadence"))
    return false;
  uint64_t protectedBytes = 0u;
  uint64_t inactiveBytes = 0u;
  for (const auto& entry : entries) {
    if (policy::IsWar3ShadowDrawTimeStaticWorkingSetProtected(entry.last,
                                                               frame))
      protectedBytes += entry.bytes;
    else
      inactiveBytes += entry.bytes;
  }
  if (!require(protectedBytes == 88u * mib && inactiveBytes == 80u * mib &&
                   policy::War3ShadowDrawTimeStaticOverCapBytes(
                       protectedBytes + inactiveBytes, inactiveTarget) ==
                       104u * mib,
               "working-set accounting changed"))
    return false;
  std::vector<uint64_t> evicted;
  while (inactiveBytes > inactiveTarget) {
    auto victim = std::min_element(entries.begin(), entries.end(),
        [frame](const Entry& a, const Entry& b) {
          const bool aProtected =
              policy::IsWar3ShadowDrawTimeStaticWorkingSetProtected(a.last, frame);
          const bool bProtected =
              policy::IsWar3ShadowDrawTimeStaticWorkingSetProtected(b.last, frame);
          if (aProtected != bProtected)
            return !aProtected;
          if (aProtected)
            return false;
          return a.last != b.last ? a.last < b.last : a.key < b.key;
        });
    if (victim == entries.end() ||
        policy::IsWar3ShadowDrawTimeStaticWorkingSetProtected(victim->last,
                                                               frame))
      return false;
    inactiveBytes -= victim->bytes;
    evicted.push_back(victim->key);
    entries.erase(victim);
  }
  return require(evicted.size() == 1u && evicted[0] == 5u &&
                     entries.size() == 3u,
                 "inactive LRU tie-break was not deterministic/protected");
}

bool testGenerationObservationClock() {
  policy::War3ShadowGenerationObservationClock clock = {};
  return require(
      policy::AdvanceWar3ShadowGenerationObservationClock(clock, 0u) == 0u &&
          policy::AdvanceWar3ShadowGenerationObservationClock(clock, 100u) ==
              1u &&
          policy::AdvanceWar3ShadowGenerationObservationClock(clock, 100u) ==
              1u &&
          policy::AdvanceWar3ShadowGenerationObservationClock(clock, 104u) ==
              2u &&
          policy::AdvanceWar3ShadowGenerationObservationClock(clock, 110u) ==
              3u,
      "sparse Present serials did not form adjacent producer observations");
}

bool testStage11AllocationClassification() {
  using Class = policy::War3Stage11PositionAllocationClass;
  using policy::ClassifyWar3Stage11PositionAllocation;
  return require(
      ClassifyWar3Stage11PositionAllocation(
          false, false, 0u, 4096u, false) == Class::NewEntry &&
      ClassifyWar3Stage11PositionAllocation(
          true, false, 0u, 4096u, false) ==
          Class::ExistingMissingBacking &&
      ClassifyWar3Stage11PositionAllocation(
          true, true, 2048u, 4096u, false) == Class::CapacityGrowth &&
      ClassifyWar3Stage11PositionAllocation(
          true, true, 8192u, 4096u, true) == Class::GpuSkinLeaseDetach,
      "Stage11 position-allocation observer classification changed");
}

bool testStage11SnapshotPagePolicy() {
  constexpr uint64_t mib = 1024u * 1024u;
  uint64_t aligned = 0u;
  if (!require(!policy::War3TryAlignStage11SnapshotBytes(0u, aligned) &&
                   aligned == 0u &&
                   policy::War3TryAlignStage11SnapshotBytes(257u, aligned) &&
                   aligned == 512u,
               "snapshot alignment accepted an empty range or misaligned"))
    return false;
  if (!require(policy::War3Stage11SnapshotPageCapacity(1u) == 4u * mib &&
                   policy::War3Stage11SnapshotPageCapacity(5u * mib) ==
                       8u * mib &&
                   policy::War3Stage11SnapshotPageCapacity(385u * mib) == 0u,
               "snapshot page sizing escaped its bounded policy"))
    return false;

  const auto first = policy::War3PlanStage11SnapshotSuballocation(
      0u, 4u * mib, 1000u);
  const auto second = policy::War3PlanStage11SnapshotSuballocation(
      first.nextUsed, 4u * mib, 257u);
  const auto overflow = policy::War3PlanStage11SnapshotSuballocation(
      4u * mib - 128u, 4u * mib, 256u);
  return require(first.valid && first.offset == 0u &&
                     first.capacity == 1024u && second.valid &&
                     second.offset == 1024u && second.capacity == 512u &&
                     !overflow.valid &&
                     policy::War3Stage11SnapshotCanAddPage(
                         380u * mib, 4u * mib) &&
                     !policy::War3Stage11SnapshotCanAddPage(
                         381u * mib, 4u * mib),
                 "snapshot page cursor reused or exceeded a physical range");
}

bool testPinnedUploadRange() {
  const auto valid = policy::MakeWar3PinnedUploadRange(4096u, 512u, 1024u);
  const auto zero = policy::MakeWar3PinnedUploadRange(4096u, 0u, 0u);
  const auto offsetPastEnd =
      policy::MakeWar3PinnedUploadRange(4096u, 4097u, 1u);
  const auto lengthPastEnd =
      policy::MakeWar3PinnedUploadRange(4096u, 3584u, 513u);
  const auto exactEnd =
      policy::MakeWar3PinnedUploadRange(4096u, 3584u, 512u);
  return require(valid.valid && valid.offset == 512u &&
                     valid.length == 1024u && !zero.valid &&
                     !offsetPastEnd.valid && !lengthPastEnd.valid &&
                     exactEnd.valid && exactEnd.offset == 3584u &&
                     exactEnd.length == 512u,
                 "pinned upload range did not fail closed");
}

bool testRequiredCasterHardAdmissionIsOrderIndependent() {
  constexpr uint64_t mib = 1024u * 1024u;
  constexpr uint64_t hardBudget = 384u * mib;
  const std::array<uint64_t, 4u> bundles = {
      48u * mib, 120u * mib, 40u * mib, 80u * mib};
  std::array<uint32_t, 4u> order = {0u, 1u, 2u, 3u};
  do {
    uint64_t used = 0u;
    for (const uint32_t index : order) {
      policy::ShadowCaptureBudgetPolicy budget = {};
      budget.hardBudgetBytes = hardBudget;
      budget.usedBudgetBytes = used;
      budget.posBytes = bundles[index];
      budget.requiredDirectionalCaster = true;
      if (!require(policy::RequiredShadowCasterFitsHardBudget(budget),
                   "required caster below hard capacity was rejected"))
        return false;
      used += bundles[index];
    }
    if (!require(used == 288u * mib,
                 "permutation changed required caster byte total"))
      return false;
  } while (std::next_permutation(order.begin(), order.end()));

  policy::ShadowCaptureBudgetPolicy overHard = {};
  overHard.hardBudgetBytes = hardBudget;
  overHard.usedBudgetBytes = 380u * mib;
  overHard.posBytes = 8u * mib;
  overHard.requiredDirectionalCaster = true;
  if (!require(!policy::RequiredShadowCasterFitsHardBudget(overHard),
               "required caster beyond hard capacity was admitted"))
    return false;

  overHard.usedBudgetBytes = std::numeric_limits<uint64_t>::max() - 1u;
  overHard.hardBudgetBytes = std::numeric_limits<uint64_t>::max();
  overHard.posBytes = 1u;
  overHard.indexBytes = 1u;
  return require(!policy::RequiredShadowCasterFitsHardBudget(overHard),
                 "overflowing bundle byte sum was admitted");
}

bool testGenerationBackedStreamProof() {
  using Proof = policy::War3ShadowGenerationBackedStreamProof;
  using Kind = policy::War3ShadowStreamKind;
  Proof proof = {};
  if (!require(!proof.valid() && !proof.matches(proof),
               "empty stream proof was accepted"))
    return false;

  proof.ownerIdentity = 0x1234u;
  proof.identityGeneration = 3u;
  proof.allocationGeneration = 5u;
  proof.contentGeneration = 7u;
  proof.sourceOffset = 64u;
  proof.sourceLength = 384u;
  proof.elementStride = 12u;
  proof.elementSize = 12u;
  proof.mapEpoch = 11u;
  proof.deviceEpoch = 13u;
  proof.streamKind = Kind::Position;
  if (!require(proof.valid() && proof.matches(proof),
               "exact generation-backed proof was rejected"))
    return false;

  const auto requireMismatch = [&](auto mutate, const char* message) {
    Proof changed = proof;
    mutate(changed);
    return require(!proof.matches(changed), message);
  };
  return requireMismatch([](Proof& p) { ++p.ownerIdentity; },
                         "owner alias was accepted") &&
      requireMismatch([](Proof& p) { ++p.identityGeneration; },
                      "identity generation mismatch was accepted") &&
      requireMismatch([](Proof& p) { ++p.allocationGeneration; },
                      "allocation generation mismatch was accepted") &&
      requireMismatch([](Proof& p) { ++p.contentGeneration; },
                      "content generation mismatch was accepted") &&
      requireMismatch([](Proof& p) { ++p.sourceOffset; },
                      "source range mismatch was accepted") &&
      requireMismatch([](Proof& p) { ++p.sourceLength; },
                      "source length mismatch was accepted") &&
      requireMismatch([](Proof& p) { ++p.mapEpoch; },
                      "map epoch mismatch was accepted") &&
      requireMismatch([](Proof& p) { ++p.deviceEpoch; },
                      "device epoch mismatch was accepted") &&
      requireMismatch([](Proof& p) { p.streamKind = Kind::Index; },
                      "stream-kind mismatch was accepted") &&
      requireMismatch([](Proof& p) { p.elementSize = p.elementStride + 1u; },
                      "invalid element coverage was accepted");
}

bool testGenerationBackedStabilityProbation() {
  using GeometryProof = policy::War3ShadowGenerationBackedGeometryProof;
  using Observation = policy::War3ShadowGenerationObservation;
  using Proof = policy::War3ShadowGenerationBackedStreamProof;
  using State = policy::War3ShadowGenerationStabilityState;
  using Kind = policy::War3ShadowStreamKind;

  auto makeProof = [](uintptr_t owner, Kind kind) {
    Proof proof = {};
    proof.ownerIdentity = owner;
    proof.identityGeneration = 3u;
    proof.allocationGeneration = 5u;
    proof.contentGeneration = 7u;
    proof.sourceOffset = kind == Kind::Position ? 64u : 128u;
    proof.sourceLength = kind == Kind::Position ? 384u : 96u;
    proof.elementStride = kind == Kind::Position ? 12u : 2u;
    proof.elementSize = proof.elementStride;
    proof.mapEpoch = 11u;
    proof.deviceEpoch = 13u;
    proof.streamKind = kind;
    return proof;
  };

  GeometryProof proof = {};
  proof.position = makeProof(0x1234u, Kind::Position);
  proof.index = makeProof(0x5678u, Kind::Index);
  proof.indexed = true;
  State state = {};

  auto first = policy::ObserveWar3ShadowGenerationStability(
      state, proof, 100u);
  if (!require(first.observation == Observation::First &&
                   !first.promotionReady && state.distinctStableFrames == 1u,
               "first proof observation incorrectly authorized promotion"))
    return false;
  auto sameFrame = policy::ObserveWar3ShadowGenerationStability(
      state, proof, 100u);
  if (!require(sameFrame.observation == Observation::SameFrame &&
                   !sameFrame.promotionReady &&
                   state.distinctStableFrames == 1u,
               "same-frame draws manufactured generation stability"))
    return false;
  auto advanced = policy::ObserveWar3ShadowGenerationStability(
      state, proof, 101u);
  if (!require(advanced.observation == Observation::Advanced &&
                   advanced.promotionReady &&
                   state.distinctStableFrames == 2u,
               "adjacent exact proof did not become promotion-ready"))
    return false;

  auto changedProof = proof;
  ++changedProof.position.contentGeneration;
  auto changed = policy::ObserveWar3ShadowGenerationStability(
      state, changedProof, 102u);
  if (!require(changed.observation == Observation::Changed &&
                   !changed.promotionReady &&
                   state.distinctStableFrames == 1u,
               "content generation change did not restart probation"))
    return false;

  auto stale = policy::ObserveWar3ShadowGenerationStability(
      state, changedProof, 110u);
  if (!require(stale.observation == Observation::StaleRestart &&
                   !stale.promotionReady &&
                   state.distinctStableFrames == 1u,
               "non-adjacent observation retained stale stability"))
    return false;

  GeometryProof invalid = proof;
  invalid.index.contentGeneration = 0u;
  const auto before = state;
  auto rejected = policy::ObserveWar3ShadowGenerationStability(
      state, invalid, 111u);
  return require(rejected.observation == Observation::Invalid &&
                     !rejected.promotionReady &&
                     state.lastObservedFrame == before.lastObservedFrame &&
                     state.distinctStableFrames == before.distinctStableFrames,
                 "invalid proof mutated probation state");
}

} // namespace

int main() {
  return testProducerCompletenessStampAndSaturation() &&
      testProtectedWorkingSetLru() &&
      testGenerationObservationClock() &&
      testStage11AllocationClassification() &&
      testStage11SnapshotPagePolicy() &&
      testPinnedUploadRange() &&
      testRequiredCasterHardAdmissionIsOrderIndependent() &&
      testGenerationBackedStreamProof() &&
      testGenerationBackedStabilityProbation() ? 0 : 1;
}
