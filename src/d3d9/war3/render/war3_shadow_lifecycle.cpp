#include "war3_shadow_lifecycle.h"
#include "war3_shadow_producer_policy.h"

#include "../../../util/util_bit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace dxvk::war3::render {

namespace {

constexpr size_t kMaxTombstoneHistory = 4096u;

struct LifecycleState {
  std::mutex mutex;
  std::deque<ShadowCasterTombstone> history;
  std::unordered_map<uint64_t, ShadowCasterTombstone> activeByIdentity;
  std::unordered_map<int16_t, ShadowCasterTombstone> activeDisabledStages;
  uint64_t firstRetainedSerial = 1u;
};

LifecycleState& State() {
  static LifecycleState state;
  return state;
}

std::atomic<uint64_t> g_eventSerial{0u};
std::atomic<uint64_t> g_mapEpoch{0u};
std::atomic<uint64_t> g_stagePolicyRevision{1u};
std::atomic<uint64_t> g_publishedCount{0u};
std::atomic<uint64_t> g_acknowledgedFreshCount{0u};
std::atomic<uint64_t> g_queueOverflowCount{0u};
std::array<std::atomic<uint64_t>,
           static_cast<size_t>(ShadowCasterTombstoneReason::Count)>
    g_reasonCounts = {};

uint64_t TaggedPointerKey(uint32_t tag, const void* value) {
  if (value == nullptr)
    return 0u;
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, tag);
  hash = bit::fnv1a_iter(
      hash, uint64_t(reinterpret_cast<uintptr_t>(value)));
  return hash;
}

uint64_t TaggedU32Key(uint32_t tag, uint32_t value) {
  if (value == 0u)
    return 0u;
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, tag);
  hash = bit::fnv1a_iter(hash, value);
  return hash;
}

size_t BuildIdentityKeys(
    const ShadowCasterIdentity& identity,
    std::array<uint64_t, 5u>& keys) {
  size_t count = 0u;
  const auto append = [&](uint64_t key) {
    if (key == 0u)
      return;
    for (size_t i = 0u; i < count; ++i) {
      if (keys[i] == key)
        return;
    }
    keys[count++] = key;
  };
  append(TaggedPointerKey(1u, identity.unitPtr));
  append(TaggedU32Key(2u, identity.jHandle));
  append(TaggedPointerKey(4u, identity.worldObjectEntry));
  append(TaggedPointerKey(5u, identity.sceneNode));
  append(TaggedPointerKey(7u, identity.renderablePart));
  return count;
}

bool SameStageDomain(
    const ShadowCasterTombstone& tombstone,
    const ShadowCasterIdentity& identity) {
  return tombstone.identity.producerStage < 0 ||
      identity.producerStage < 0 ||
      tombstone.identity.producerStage == identity.producerStage;
}

} // namespace

bool ShadowCasterIdentityMatches(
    const ShadowCasterIdentity& lhs,
    const ShadowCasterIdentity& rhs) {
  if (lhs.producerStage >= 0 && rhs.producerStage >= 0 &&
      lhs.producerStage != rhs.producerStage) {
    return false;
  }
  return (lhs.unitPtr != nullptr && lhs.unitPtr == rhs.unitPtr) ||
      (lhs.worldObjectEntry != nullptr &&
       lhs.worldObjectEntry == rhs.worldObjectEntry) ||
      (lhs.sceneNode != nullptr && lhs.sceneNode == rhs.sceneNode) ||
      (lhs.renderablePart != nullptr &&
       lhs.renderablePart == rhs.renderablePart) ||
      (lhs.jHandle != 0u && lhs.jHandle == rhs.jHandle);
}

uint64_t PublishShadowCasterTombstone(
    ShadowCasterTombstone tombstone) {
  if (!tombstone.identity.hasStrongIdentity() &&
      tombstone.reason != ShadowCasterTombstoneReason::StageDisabled) {
    return 0u;
  }

  tombstone.mapEpoch =
      tombstone.reason == ShadowCasterTombstoneReason::StageDisabled
      ? 0u
      : g_mapEpoch.load(std::memory_order_acquire);
  tombstone.eventSerial =
      g_eventSerial.fetch_add(1u, std::memory_order_acq_rel) + 1u;
  if (tombstone.stagePolicyRevision == 0u) {
    tombstone.stagePolicyRevision =
        g_stagePolicyRevision.load(std::memory_order_acquire);
  }

  std::array<uint64_t, 5u> keys = {};
  const size_t keyCount = BuildIdentityKeys(tombstone.identity, keys);
  {
    std::lock_guard<std::mutex> lock(State().mutex);
    if (tombstone.reason == ShadowCasterTombstoneReason::StageDisabled &&
        tombstone.identity.producerStage >= 0) {
      State().activeDisabledStages[tombstone.identity.producerStage] =
          tombstone;
    }
    for (size_t i = 0u; i < keyCount; ++i)
      State().activeByIdentity[keys[i]] = tombstone;

    State().history.push_back(tombstone);
    while (State().history.size() > kMaxTombstoneHistory) {
      State().history.pop_front();
      State().firstRetainedSerial++;
      g_queueOverflowCount.fetch_add(1u, std::memory_order_relaxed);
    }
  }

  g_publishedCount.fetch_add(1u, std::memory_order_relaxed);
  const size_t reasonIndex = static_cast<size_t>(tombstone.reason);
  if (reasonIndex < g_reasonCounts.size()) {
    g_reasonCounts[reasonIndex].fetch_add(1u, std::memory_order_relaxed);
    NoteShadowStageRetired(
        tombstone.identity.producerStage,
        static_cast<uint32_t>(reasonIndex));
  }
  return tombstone.eventSerial;
}

void AcknowledgeFreshShadowCaster(
    const ShadowCasterIdentity& identity,
    uint64_t visibleFrameSerial,
    uint64_t stagePolicyRevision) {
  if (!identity.hasStrongIdentity())
    return;

  std::array<uint64_t, 5u> keys = {};
  const size_t keyCount = BuildIdentityKeys(identity, keys);
  const uint64_t currentMapEpoch =
      g_mapEpoch.load(std::memory_order_acquire);
  bool acknowledged = false;
  std::lock_guard<std::mutex> lock(State().mutex);
  if (identity.producerStage >= 0) {
    auto stageIt =
        State().activeDisabledStages.find(identity.producerStage);
    if (stageIt != State().activeDisabledStages.end() &&
        stagePolicyRevision > stageIt->second.stagePolicyRevision) {
      State().activeDisabledStages.erase(stageIt);
      acknowledged = true;
    }
  }
  for (size_t i = 0u; i < keyCount; ++i) {
    auto it = State().activeByIdentity.find(keys[i]);
    if (it == State().activeByIdentity.end())
      continue;
    const ShadowCasterTombstone& tombstone = it->second;
    if (!ShadowCasterTombstoneBelongsToMap(
            tombstone, currentMapEpoch)) {
      State().activeByIdentity.erase(it);
      continue;
    }
    if (!SameStageDomain(tombstone, identity))
      continue;
    if (tombstone.reason == ShadowCasterTombstoneReason::StageDisabled &&
        stagePolicyRevision <= tombstone.stagePolicyRevision) {
      continue;
    }
    if (tombstone.visibleFrameSerial != 0u &&
        visibleFrameSerial != 0u &&
        visibleFrameSerial <= tombstone.visibleFrameSerial) {
      continue;
    }
    State().activeByIdentity.erase(it);
    acknowledged = true;
  }
  if (acknowledged)
    g_acknowledgedFreshCount.fetch_add(1u, std::memory_order_relaxed);
}

bool IsShadowCasterTombstoned(
    const ShadowCasterIdentity& identity) {
  if (!identity.hasStrongIdentity())
    return false;
  std::array<uint64_t, 5u> keys = {};
  const size_t keyCount = BuildIdentityKeys(identity, keys);
  const uint64_t currentMapEpoch =
      g_mapEpoch.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> lock(State().mutex);
  if (identity.producerStage >= 0 &&
      State().activeDisabledStages.find(identity.producerStage) !=
          State().activeDisabledStages.end()) {
    return true;
  }
  for (size_t i = 0u; i < keyCount; ++i) {
    const auto it = State().activeByIdentity.find(keys[i]);
    if (it != State().activeByIdentity.end() &&
        ShadowCasterTombstoneBelongsToMap(
            it->second, currentMapEpoch) &&
        SameStageDomain(it->second, identity)) {
      return true;
    }
  }
  return false;
}

uint64_t CurrentShadowCasterTombstoneSerial() {
  return g_eventSerial.load(std::memory_order_acquire);
}

uint64_t ResetShadowCasterLifecycleMapEpoch(uint64_t mapEpoch) {
  if (mapEpoch == 0u)
    return CurrentShadowCasterTombstoneSerial();

  std::lock_guard<std::mutex> lock(State().mutex);
  g_mapEpoch.store(mapEpoch, std::memory_order_release);
  State().activeByIdentity.clear();
  return g_eventSerial.load(std::memory_order_acquire);
}

bool ShadowCasterTombstoneBelongsToMap(
    const ShadowCasterTombstone& tombstone, uint64_t mapEpoch) {
  return tombstone.reason == ShadowCasterTombstoneReason::StageDisabled ||
      (mapEpoch != 0u && tombstone.mapEpoch == mapEpoch);
}

uint64_t CurrentShadowStagePolicyRevision() {
  return g_stagePolicyRevision.load(std::memory_order_acquire);
}

uint64_t AdvanceShadowStagePolicyRevision() {
  return g_stagePolicyRevision.fetch_add(
             1u, std::memory_order_acq_rel) +
      1u;
}

uint64_t PublishShadowStagePolicyTransition(
    int16_t producerStage,
    bool enabled,
    uint64_t visibleFrameSerial) {
  if (producerStage < 0)
    return CurrentShadowStagePolicyRevision();
  const uint64_t revision = AdvanceShadowStagePolicyRevision();
  if (!enabled) {
    ShadowCasterTombstone tombstone = {};
    tombstone.identity.producerStage = producerStage;
    tombstone.reason = ShadowCasterTombstoneReason::StageDisabled;
    tombstone.stagePolicyRevision = revision;
    tombstone.visibleFrameSerial = visibleFrameSerial;
    PublishShadowCasterTombstone(tombstone);
  } else {
    bool acknowledged = false;
    {
      std::lock_guard<std::mutex> lock(State().mutex);
      acknowledged =
          State().activeDisabledStages.erase(producerStage) != 0u;
    }
    if (acknowledged) {
      g_acknowledgedFreshCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
  }
  return revision;
}

void SnapshotShadowCasterTombstonesAfter(
    uint64_t afterSerial,
    std::vector<ShadowCasterTombstone>& out,
    bool& historyOverflowed) {
  out.clear();
  historyOverflowed = false;
  std::lock_guard<std::mutex> lock(State().mutex);
  if (!State().history.empty() &&
      afterSerial + 1u < State().firstRetainedSerial) {
    historyOverflowed = true;
  }
  for (const ShadowCasterTombstone& tombstone : State().history) {
    if (tombstone.eventSerial > afterSerial)
      out.push_back(tombstone);
  }
}

ShadowCasterLifecycleDiagnostics
QueryShadowCasterLifecycleDiagnostics() {
  ShadowCasterLifecycleDiagnostics result = {};
  result.publishedCount =
      g_publishedCount.load(std::memory_order_relaxed);
  result.acknowledgedFreshCount =
      g_acknowledgedFreshCount.load(std::memory_order_relaxed);
  result.queueOverflowCount =
      g_queueOverflowCount.load(std::memory_order_relaxed);
  result.lastEventSerial =
      g_eventSerial.load(std::memory_order_relaxed);
  result.stagePolicyRevision =
      g_stagePolicyRevision.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(State().mutex);
    result.activeCount = State().activeByIdentity.size() +
        State().activeDisabledStages.size();
  }
  result.hiddenCount =
      g_reasonCounts[static_cast<size_t>(
          ShadowCasterTombstoneReason::Hidden)]
          .load(std::memory_order_relaxed);
  result.removedCount =
      g_reasonCounts[static_cast<size_t>(
          ShadowCasterTombstoneReason::Removed)]
          .load(std::memory_order_relaxed);
  result.stageDisabledCount =
      g_reasonCounts[static_cast<size_t>(
          ShadowCasterTombstoneReason::StageDisabled)]
          .load(std::memory_order_relaxed);
  result.replacedCount =
      g_reasonCounts[static_cast<size_t>(
          ShadowCasterTombstoneReason::Replaced)]
          .load(std::memory_order_relaxed);
  return result;
}

const char* DescribeShadowCasterTombstoneReason(
    ShadowCasterTombstoneReason reason) {
  switch (reason) {
  case ShadowCasterTombstoneReason::Hidden:
    return "Hidden";
  case ShadowCasterTombstoneReason::Removed:
    return "Removed";
  case ShadowCasterTombstoneReason::StageDisabled:
    return "StageDisabled";
  case ShadowCasterTombstoneReason::Replaced:
    return "Replaced";
  default:
    return "Unknown";
  }
}

} // namespace dxvk::war3::render
