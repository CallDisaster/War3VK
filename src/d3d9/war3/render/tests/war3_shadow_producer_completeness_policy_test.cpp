#include "../../../d3d9_war3_scene.h"
#include "../war3_shadow_drawtime_cache_policy.h"

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

} // namespace

int main() {
  return testProducerCompletenessStampAndSaturation() &&
      testProtectedWorkingSetLru() ? 0 : 1;
}
