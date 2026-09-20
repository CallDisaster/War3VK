#pragma once

// CPU-only accounting, never a resource lifetime or eviction authority.
// Producer: D3D9 Present owner. Consumer: immutable perf export snapshot.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <ostream>

namespace dxvk::war3::stage11_census {

inline bool Enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXVK_WAR3_STAGE11_BUDGET_CENSUS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

constexpr uint32_t MaxPages = 128, MaxSlices = 32768, MaxEntries = 16384;
constexpr uint32_t SampleCount = 4;
enum class Owner : uint8_t { Touched, Failed, RecentCache, ColdCache, Retired, Count };
enum class IndexUnknown : uint8_t {
  GpuAuthored, NonHostCached, MissingIdentity, MissingGeneration, MissingSpan, Count
};
// Independent reference tags.  staticTag is always supplied as a bool:
// false means "not static tagged / unknown", never "proven dynamic".
// failedRetained and touchedThisFrame are tri-state so an old caller cannot
// silently turn an unavailable failure/attempt label into false.
enum class TagState : uint8_t { Unknown = 0, False, True, Count };
struct RangeTags {
  bool staticTag = false;
  TagState failedRetained = TagState::Unknown;
  TagState touchedThisFrame = TagState::Unknown;

  static constexpr RangeTags Known(
      bool isStatic, TagState failed, TagState touched) noexcept {
    return RangeTags{isStatic, failed, touched};
  }
  static constexpr RangeTags AcknowledgedUnknownAux(
      bool isStatic) noexcept {
    return RangeTags{isStatic, TagState::Unknown, TagState::Unknown};
  }
};
constexpr size_t OwnerCount = size_t(Owner::Count);
constexpr size_t ReasonCount = size_t(IndexUnknown::Count);
enum Error : uint32_t {
  None = 0, PageLimit = 1, SliceLimit = 2, EntryLimit = 4,
  InvalidRange = 8, PageConflict = 16, ResidentMismatch = 32,
  ScratchAllocation = 64, WrongOwner = 128
};

struct Page {
  uint64_t id = 0, capacity = 0, used = 0;
  // Exact union of retained slice capacities, NOT bytes of useful vertices.
  std::array<uint64_t, OwnerCount> owned{};
  // Disjoint static-tag partition.  staticTaggedUnion / notStaticTaggedUnion
  // include mixed overlap; staticOnly + notStaticOnly + mixedTagOverlap is
  // exactly the old owned total.  false is only a non-static/unknown label.
  uint64_t staticTaggedUnion = 0, notStaticTaggedUnion = 0;
  uint64_t staticTagOnly = 0, notStaticTagOnly = 0, mixedTagOverlap = 0;
  // Cross-statistics over the same intervals.  touchedAndFailed is the
  // intersection; touchedOrFailed is the union; neither can exceed the
  // owner-covered total and touchedAndFailed cannot exceed either input set.
  uint64_t failedRetainedUnion = 0, touchedUnion = 0;
  uint64_t touchedAndFailedUnion = 0, touchedOrFailedUnion = 0;
  uint64_t tagUnknownUnion = 0;
  uint64_t unreferencedUsed = 0, tail = 0;
  uint64_t minAccessAge = UINT64_MAX, maxAccessAge = 0;
  uint32_t referenceCount = 0, staticReferences = 0, knownAgeReferences = 0;
  uint32_t tagUnknownReferences = 0;
  uint64_t pageReferences = 0;
  bool active = false;
};
struct Sample {
  uint64_t deviceIdentity = 0, mapEpoch = 0, deviceEpoch = 0, frame = 0;
  uint64_t cap = 0, activeResident = 0, capacityRejects = 0;
  uint64_t uploadRangeHits = 0;
  std::array<uint64_t, ReasonCount> unknownCounts{}, unknownPositionBytes{};
  uint64_t sampleCpuUs = 0;
  uint32_t stage = 0, errors = 0, pageCount = 0, sliceCount = 0, entryCount = 0;
  bool sampled = false;
  std::array<Page, MaxPages> pages{};
};
struct History {
  bool enabled = false;
  std::array<Sample, SampleCount> samples{};
};

// One map/device epoch per schedule. No allocation, lock, record or IO on draw.
struct Schedule {
  uint64_t map = 0, device = 0, rejects = 0, previousRejects = 0, pressureFrame = 0;
  uint32_t mask = 0;
  uint64_t uploadRangeHits = 0;
  std::array<uint64_t, ReasonCount> unknown{}, bytes{};
  void reset(uint64_t m, uint64_t d) { *this = {}; map = m; device = d; }
  void noteUnknown(IndexUnknown reason, uint64_t positionBytes) {
    const auto i = size_t(reason);
    if (i >= ReasonCount) return;
    ++unknown[i]; bytes[i] += positionBytes;
  }
  // 0 normal, 1 first pressure, 2 >= two existing 60-frame GC intervals,
  // 3 first no-reject frame AFTER sample 2. This is not a shadow recovery test.
  int take(uint64_t frame, uint64_t gcInterval) {
    int stage = -1;
    if (!(mask & 1u) && frame) stage = 0;
    else if (!(mask & 2u) && rejects) { stage = 1; pressureFrame = frame; }
    else if ((mask & 2u) && !(mask & 4u) && frame >= pressureFrame &&
             frame - pressureFrame >= 2u * gcInterval) stage = 2;
    else if ((mask & 4u) && !(mask & 8u) && rejects == previousRejects) stage = 3;
    previousRejects = rejects;
    if (stage >= 0) mask |= 1u << stage;
    return stage;
  }
};

// Fixed scratch: two endpoints per retained range. Sorting + sweep deduplicate
// aliases and overlaps; no per-entry allocation and no retained resource refs.
class Collector {
  static constexpr uint8_t kStaticTagBit = 1u;
  static constexpr uint8_t kFailedKnownBit = 2u;
  static constexpr uint8_t kFailedTrueBit = 4u;
  static constexpr uint8_t kTouchedKnownBit = 8u;
  static constexpr uint8_t kTouchedTrueBit = 16u;
  struct Endpoint {
    uint64_t offset;
    uint32_t page;
    uint8_t owner;
    int8_t delta;
    uint8_t tagFlags;
  };
  std::array<Endpoint, 2 * MaxSlices> m_points{};
  uint32_t m_count = 0;
public:
  Sample result{};
  int page(uint64_t id, uint64_t capacity, uint64_t used, bool active, uint64_t refs = 0) {
    if (!id || !capacity || capacity > (512ull << 20u) || used > capacity ||
        (capacity & 255u) || (used & 255u)) { result.errors |= InvalidRange; return -1; }
    for (uint32_t i = 0; i < result.pageCount; ++i) {
      auto& p = result.pages[i];
      if (p.id != id) continue;
      if (p.capacity != capacity || p.used != used || p.active != active)
        result.errors |= PageConflict;
      return int(i);
    }
    if (result.pageCount == MaxPages) { result.errors |= PageLimit; return -1; }
    auto& p = result.pages[result.pageCount];
    p.id = id; p.capacity = capacity; p.used = used; p.active = active; p.pageReferences = refs;
    return int(result.pageCount++);
  }
  void range(int pageIndex, uint64_t offset, uint64_t capacity, Owner owner,
             const RangeTags& tags, uint64_t accessAge) {
    if (pageIndex < 0) return;
    if (uint32_t(pageIndex) >= result.pageCount || size_t(owner) >= OwnerCount ||
        size_t(tags.failedRetained) >= size_t(TagState::Count) ||
        size_t(tags.touchedThisFrame) >= size_t(TagState::Count)) {
      result.errors |= InvalidRange; return;
    }
    auto& p = result.pages[pageIndex];
    if (!capacity || offset > p.used || capacity > p.used - offset ||
        (offset & 255u) || (capacity & 255u)) {
      result.errors |= InvalidRange; return;
    }
    if (m_count == m_points.size()) { result.errors |= SliceLimit; return; }
    uint8_t tagFlags = tags.staticTag ? kStaticTagBit : 0u;
    if (tags.failedRetained != TagState::Unknown) tagFlags |= kFailedKnownBit;
    if (tags.failedRetained == TagState::True) tagFlags |= kFailedTrueBit;
    if (tags.touchedThisFrame != TagState::Unknown) tagFlags |= kTouchedKnownBit;
    if (tags.touchedThisFrame == TagState::True) tagFlags |= kTouchedTrueBit;
    m_points[m_count++] = {offset, uint32_t(pageIndex), uint8_t(owner), 1, tagFlags};
    m_points[m_count++] = {offset + capacity, uint32_t(pageIndex), uint8_t(owner), -1, tagFlags};
    ++result.sliceCount; ++p.referenceCount;
    p.staticReferences += tags.staticTag ? 1u : 0u;
    if (tags.failedRetained == TagState::Unknown ||
        tags.touchedThisFrame == TagState::Unknown)
      ++p.tagUnknownReferences;
    if (accessAge != UINT64_MAX) {
      ++p.knownAgeReferences;
      p.minAccessAge = std::min(p.minAccessAge, accessAge);
      p.maxAccessAge = std::max(p.maxAccessAge, accessAge);
    }
  }
  void finish(uint64_t expectedActiveResident) {
    std::sort(m_points.begin(), m_points.begin() + m_count,
      [](const Endpoint& a, const Endpoint& b) {
        return a.page < b.page || (a.page == b.page && a.offset < b.offset);
      });
    uint32_t cursor = 0;
    uint64_t resident = 0;
    for (uint32_t pi = 0; pi < result.pageCount; ++pi) {
      auto& p = result.pages[pi];
      std::array<int32_t, OwnerCount> ownerCounts{};
      int32_t staticTrue = 0, staticFalse = 0;
      int32_t failedTrue = 0, failedUnknown = 0;
      int32_t touchedTrue = 0, touchedUnknown = 0;
      uint64_t previous = 0;
      while (cursor < m_count && m_points[cursor].page == pi) {
        const uint64_t offset = m_points[cursor].offset;
        bool anyOwner = false;
        for (const auto count : ownerCounts) if (count > 0) anyOwner = true;
        if (offset > previous && anyOwner) {
          const uint64_t length = offset - previous;
          for (size_t c = 0; c < OwnerCount; ++c) {
            if (ownerCounts[c] > 0) { p.owned[c] += length; break; }
          }
          if (staticTrue > 0 && staticFalse > 0) p.mixedTagOverlap += length;
          else if (staticTrue > 0) p.staticTagOnly += length;
          else if (staticFalse > 0) p.notStaticTagOnly += length;
          else result.errors |= InvalidRange;
          if (failedTrue > 0) p.failedRetainedUnion += length;
          if (touchedTrue > 0) p.touchedUnion += length;
          if (failedTrue > 0 && touchedTrue > 0)
            p.touchedAndFailedUnion += length;
          if (failedTrue > 0 || touchedTrue > 0)
            p.touchedOrFailedUnion += length;
          if (failedUnknown > 0 || touchedUnknown > 0)
            p.tagUnknownUnion += length;
        }
        do {
          const auto& e = m_points[cursor++];
          const int32_t delta = e.delta;
          ownerCounts[size_t(e.owner)] += delta;
          if (e.tagFlags & kStaticTagBit) staticTrue += delta;
          else staticFalse += delta;
          if (e.tagFlags & kFailedKnownBit) {
            if (e.tagFlags & kFailedTrueBit) failedTrue += delta;
          } else {
            failedUnknown += delta;
          }
          if (e.tagFlags & kTouchedKnownBit) {
            if (e.tagFlags & kTouchedTrueBit) touchedTrue += delta;
          } else {
            touchedUnknown += delta;
          }
        } while (cursor < m_count && m_points[cursor].page == pi &&
                 m_points[cursor].offset == offset);
        for (const auto count : ownerCounts)
          if (count < 0) result.errors |= InvalidRange;
        if (staticTrue < 0 || staticFalse < 0 || failedTrue < 0 ||
            failedUnknown < 0 || touchedTrue < 0 || touchedUnknown < 0)
          result.errors |= InvalidRange;
        previous = offset;
      }
      uint64_t total = 0;
      for (auto b : p.owned) total += b;
      for (auto c : ownerCounts) if (c != 0) result.errors |= InvalidRange;
      if (staticTrue != 0 || staticFalse != 0 || failedTrue != 0 ||
          failedUnknown != 0 || touchedTrue != 0 || touchedUnknown != 0)
        result.errors |= InvalidRange;
      p.staticTaggedUnion = p.staticTagOnly + p.mixedTagOverlap;
      p.notStaticTaggedUnion = p.notStaticTagOnly + p.mixedTagOverlap;
      if (p.staticTagOnly + p.notStaticTagOnly + p.mixedTagOverlap != total)
        result.errors |= InvalidRange;
      if (p.failedRetainedUnion > total || p.touchedUnion > total ||
          p.touchedAndFailedUnion > p.failedRetainedUnion ||
          p.touchedAndFailedUnion > p.touchedUnion ||
          p.touchedOrFailedUnion > total ||
          p.touchedOrFailedUnion + p.touchedAndFailedUnion !=
              p.failedRetainedUnion + p.touchedUnion ||
          p.tagUnknownUnion > total ||
          (p.touchedOrFailedUnion != 0u &&
           p.touchedOrFailedUnion <
               (std::max)(p.failedRetainedUnion, p.touchedUnion)))
        result.errors |= InvalidRange;
      if (p.tagUnknownReferences > p.referenceCount)
        result.errors |= InvalidRange;
      if (total > p.used) result.errors |= InvalidRange;
      else p.unreferencedUsed = p.used - total;
      p.tail = p.capacity - p.used;
      if (p.active) resident += p.capacity;
    }
    if (resident != expectedActiveResident) result.errors |= ResidentMismatch;
    result.activeResident = resident;
    result.sampled = true;
  }
};
// Includes owner scratch and several queued/exporting report copies, not the
// pre-existing report/iostream buffers. No GPU bytes are copied by this census.
static_assert(sizeof(Collector) + 6 * sizeof(History) < 4u * 1024u * 1024u);

inline void WriteJson(std::ostream& out, const History& history) {
  out << "{\"schema\":2,\"enabled\":" << (history.enabled ? "true" : "false")
      << ",\"scope\":\"retained-page-and-cache-slices\",\"physicalBackingComplete\":false,"
         "\"gpuCompletionKnown\":false,\"ownerCategories\":[\"touched\",\"failed\","
         "\"recentCache\",\"coldCache\",\"retired\"],\"unknownReasons\":[\"gpuAuthored\","
         "\"nonHostCached\",\"missingIdentity\",\"missingGeneration\",\"missingSpan\"],"
         "\"tagDimensions\":[\"staticTagOnly\",\"notStaticTagOnly\",\"mixedTagOverlap\","
         "\"failedRetainedUnion\",\"touchedUnion\",\"touchedAndFailedUnion\","
         "\"touchedOrFailedUnion\",\"tagUnknownUnion\"],\"samples\":[";
  bool first = true;
  for (const auto& s : history.samples) {
    if (!s.sampled) continue;
    if (!first) out << ',';
    first = false;
    out << "{\"deviceIdentity\":" << s.deviceIdentity << ",\"mapEpoch\":" << s.mapEpoch
        << ",\"deviceEpoch\":" << s.deviceEpoch << ",\"frame\":" << s.frame
        << ",\"stage\":" << s.stage << ",\"errors\":" << s.errors
        << ",\"complete\":" << (s.errors ? "false" : "true")
        << ",\"cap\":" << s.cap << ",\"activeResident\":" << s.activeResident
        << ",\"capacityRejects\":" << s.capacityRejects << ",\"uploadRangeHits\":" << s.uploadRangeHits
        << ",\"sampleCpuUs\":" << s.sampleCpuUs
        << ",\"entryCount\":" << s.entryCount << ",\"sliceCount\":" << s.sliceCount;
    const auto writeArray = [&](const char* name, const auto& values) {
      out << ",\"" << name << "\":[";
      for (size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << values[i]; }
      out << ']';
    };
    writeArray("unknownCounts", s.unknownCounts);
    writeArray("unknownPositionBytes", s.unknownPositionBytes);
    out << ",\"pages\":[";
    for (uint32_t i = 0; i < s.pageCount; ++i) {
      const auto& p = s.pages[i];
      if (i) out << ',';
      out << "{\"id\":" << p.id << ",\"active\":" << (p.active ? "true" : "false")
          << ",\"capacity\":" << p.capacity << ",\"used\":" << p.used
          << ",\"notReferencedByCacheBytes\":" << p.unreferencedUsed << ",\"tail\":" << p.tail
          << ",\"references\":" << p.referenceCount << ",\"staticReferences\":" << p.staticReferences
          << ",\"tagUnknownReferences\":" << p.tagUnknownReferences
          << ",\"staticTaggedUnionBytes\":" << p.staticTaggedUnion
          << ",\"notStaticTaggedUnionBytes\":" << p.notStaticTaggedUnion
          << ",\"staticTagOnlyBytes\":" << p.staticTagOnly
          << ",\"notStaticTagOnlyBytes\":" << p.notStaticTagOnly
          << ",\"mixedTagOverlapBytes\":" << p.mixedTagOverlap
          << ",\"failedRetainedUnionBytes\":" << p.failedRetainedUnion
          << ",\"touchedUnionBytes\":" << p.touchedUnion
          << ",\"touchedAndFailedUnionBytes\":" << p.touchedAndFailedUnion
          << ",\"touchedOrFailedUnionBytes\":" << p.touchedOrFailedUnion
          << ",\"tagUnknownUnionBytes\":" << p.tagUnknownUnion
          << ",\"pageReferences\":" << p.pageReferences << ",\"knownAgeReferences\":" << p.knownAgeReferences
          << ",\"minAccessAge\":" << (p.knownAgeReferences ? p.minAccessAge : 0)
          << ",\"maxAccessAge\":" << p.maxAccessAge;
      writeArray("owned", p.owned);
      out << '}';
    }
    out << "]}";
  }
  out << "]}";
}
} // namespace dxvk::war3::stage11_census
