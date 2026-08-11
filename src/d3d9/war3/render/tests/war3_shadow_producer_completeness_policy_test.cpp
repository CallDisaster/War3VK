#include "../../../d3d9_war3_scene.h"
#include "../war3_shadow_capture_frontend.h"
#include "../war3_shadow_drawtime_cache_policy.h"
#include "../war3_shadow_generation_backed_stream.h"

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
  constexpr uint64_t frame = 100u;
  constexpr uint64_t inactiveTarget = 64u * mib;
  std::vector<Entry> entries = {
      {3u, frame, 48u * mib}, {2u, frame - 1u, 40u * mib},
      {9u, frame - 10u, 40u * mib}, {5u, frame - 10u, 40u * mib},
  };
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

} // namespace

int main() {
  return testProducerCompletenessStampAndSaturation() &&
      testProtectedWorkingSetLru() &&
      testRequiredCasterHardAdmissionIsOrderIndependent() &&
      testGenerationBackedStreamProof() ? 0 : 1;
}
