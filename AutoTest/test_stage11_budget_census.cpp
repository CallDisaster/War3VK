#include "../src/d3d9/war3/tools/war3_stage11_budget_census.h"
#include "../src/d3d9/war3/render/war3_draw_time_snapshot_lifetime.h"
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <new>

static uint64_t allocations = 0;
void* operator new(std::size_t n) {
  ++allocations;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace dxvk::war3::stage11_census;
using namespace dxvk::war3::render;
struct CensusResource {};
struct CensusEntry {
  std::shared_ptr<CensusResource> positionSnapshotPage;
  std::shared_ptr<CensusResource> uvSnapshotPage;
  uint64_t positionSnapshotOffset = 0u;
  uint64_t uvSnapshotOffset = 0u;
  uint64_t positionCapacity = 0u;
  uint64_t uvCapacity = 0u;
  bool uvSharesPositionBuffer = false;
};
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while (false)

static RangeTags tags(bool isStatic, bool failed, bool touched) {
  return RangeTags::Known(
      isStatic, failed ? TagState::True : TagState::False,
      touched ? TagState::True : TagState::False);
}

static RangeTags unknownAux(bool isStatic) {
  return RangeTags::AcknowledgedUnknownAux(isStatic);
}

int main(int argc, char** argv) { try {
  auto c = std::make_unique<Collector>();
  if (argc > 1 && std::string(argv[1]) == "--json") {
    auto h = std::make_unique<History>(); h->enabled = true;
    auto p = c->page(1, 4096, 2048, true, 3);
    c->range(p, 0, 1024, Owner::ColdCache, tags(true, true, false), 200);
    c->range(p, 512, 1024, Owner::Touched, tags(false, false, true), 0);
    c->finish(4096);
    c->result.deviceIdentity = 0x12345678u;
    c->result.mapEpoch = 7u;
    c->result.deviceEpoch = 9u;
    c->result.frame = 42u;
    c->result.cap = 4096u;
    c->result.stage = 0u;
    h->samples[0] = c->result; WriteJson(std::cout, *h); return 0;
  }

  auto reset = [&] { c.reset(new Collector); };
  auto p = c->page(1, 4096, 2048, true, 4);
  c->range(p, 0, 1024, Owner::ColdCache, tags(true, true, false), 200);
  c->range(p, 512, 1024, Owner::Touched, tags(false, false, true), 0);
  c->range(p, 512, 1024, Owner::Touched, tags(false, false, true), 0); // alias
  c->finish(4096);
  CHECK(c->result.errors == 0);
  CHECK(c->result.pages[0].owned[0] == 1024);
  CHECK(c->result.pages[0].owned[3] == 512);
  CHECK(c->result.pages[0].unreferencedUsed == 512);
  CHECK(c->result.pages[0].tail == 2048);
  CHECK(c->result.pages[0].referenceCount == 3);
  CHECK(c->result.pages[0].staticReferences == 1);
  CHECK(c->result.pages[0].tagUnknownReferences == 0);
  CHECK(c->result.pages[0].staticTagOnly == 512);
  CHECK(c->result.pages[0].notStaticTagOnly == 512);
  CHECK(c->result.pages[0].mixedTagOverlap == 512);
  CHECK(c->result.pages[0].staticTaggedUnion == 1024);
  CHECK(c->result.pages[0].notStaticTaggedUnion == 1024);
  CHECK(c->result.pages[0].failedRetainedUnion == 1024);
  CHECK(c->result.pages[0].touchedUnion == 1024);
  CHECK(c->result.pages[0].touchedAndFailedUnion == 512);
  CHECK(c->result.pages[0].touchedOrFailedUnion == 1536);
  CHECK(c->result.pages[0].tagUnknownUnion == 0);
  CHECK(c->result.pages[0].pageReferences == 4);
  CHECK(c->result.pages[0].maxAccessAge == 200);

  // All 9 aux tri-state combinations.  Known False must not enter the
  // unknown bucket; Unknown must enter it.
  reset(); p = c->page(1, 4096, 9u * 256u, true);
  const TagState failedStates[3] = {
      TagState::False, TagState::True, TagState::Unknown};
  const TagState touchedStates[3] = {
      TagState::False, TagState::True, TagState::Unknown};
  uint64_t expectedFailed = 0u, expectedTouched = 0u;
  uint64_t expectedBoth = 0u, expectedEither = 0u, expectedUnknown = 0u;
  for (unsigned f = 0; f < 3u; ++f) {
    for (unsigned t = 0; t < 3u; ++t) {
      const unsigned ordinal = f * 3u + t;
      const bool isStatic = (ordinal & 1u) != 0u;
      c->range(p, ordinal * 256u, 256u, Owner::ColdCache,
               RangeTags::Known(isStatic, failedStates[f], touchedStates[t]),
               0);
      if (failedStates[f] == TagState::True) expectedFailed += 256u;
      if (touchedStates[t] == TagState::True) expectedTouched += 256u;
      if (failedStates[f] == TagState::True &&
          touchedStates[t] == TagState::True)
        expectedBoth += 256u;
      if (failedStates[f] == TagState::True ||
          touchedStates[t] == TagState::True)
        expectedEither += 256u;
      if (failedStates[f] == TagState::Unknown ||
          touchedStates[t] == TagState::Unknown)
        expectedUnknown += 256u;
    }
  }
  c->finish(4096);
  CHECK(c->result.errors == 0);
  CHECK(c->result.pages[0].owned[3] == 9u * 256u);
  CHECK(c->result.pages[0].failedRetainedUnion == expectedFailed);
  CHECK(c->result.pages[0].touchedUnion == expectedTouched);
  CHECK(c->result.pages[0].touchedAndFailedUnion == expectedBoth);
  CHECK(c->result.pages[0].touchedOrFailedUnion == expectedEither);
  CHECK(c->result.pages[0].tagUnknownUnion == expectedUnknown);
  // Ordinals 1,3,5,7 are static; 0,2,4,6,8 are not. Keep the expected
  // counts independent of the collector and of the loop's tag expression.
  CHECK(c->result.pages[0].staticTagOnly == 4u * 256u);
  CHECK(c->result.pages[0].notStaticTagOnly == 5u * 256u);
  CHECK(c->result.pages[0].mixedTagOverlap == 0u);

  // Known + Unknown overlap and duplicate alias at the same offsets.
  reset(); p = c->page(1, 4096, 512, true);
  c->range(p, 0, 512, Owner::ColdCache, tags(true, true, false), 0);
  c->range(p, 0, 512, Owner::Touched,
           RangeTags::Known(false, TagState::Unknown, TagState::Unknown), 0);
  c->range(p, 0, 512, Owner::Touched,
           RangeTags::Known(false, TagState::Unknown, TagState::Unknown), 0);
  c->finish(4096);
  CHECK(c->result.errors == 0);
  CHECK(c->result.pages[0].owned[0] == 512u);
  CHECK(c->result.pages[0].owned[3] == 0u);
  CHECK(c->result.pages[0].mixedTagOverlap == 512u);
  CHECK(c->result.pages[0].failedRetainedUnion == 512u);
  CHECK(c->result.pages[0].touchedUnion == 0u);
  CHECK(c->result.pages[0].touchedAndFailedUnion == 0u);
  CHECK(c->result.pages[0].touchedOrFailedUnion == 512u);
  CHECK(c->result.pages[0].tagUnknownUnion == 512u);
  CHECK(c->result.pages[0].tagUnknownReferences == 2u);

  reset(); p = c->page(2, 4096, 1024, true);
  c->range(p, 0, 1024, Owner::ColdCache, unknownAux(true), 200);
  c->finish(4096);
  CHECK(c->result.errors == 0);
  CHECK(c->result.pages[0].staticTagOnly == 1024);
  CHECK(c->result.pages[0].tagUnknownUnion == 1024);
  CHECK(c->result.pages[0].tagUnknownReferences == 1);
  CHECK(c->result.pages[0].failedRetainedUnion == 0);
  CHECK(c->result.pages[0].touchedUnion == 0);
  CHECK(c->result.pages[0].touchedOrFailedUnion == 0);
  CHECK(c->result.pages[0].unreferencedUsed == 0);

  reset(); p = c->page(8, 4096, 4096, false);
  c->range(p, 0, 256, Owner::Retired, tags(false, false, false), UINT64_MAX);
  c->finish(0);
  CHECK(c->result.errors == 0 && c->result.activeResident == 0);
  CHECK(c->result.pages[0].owned[4] == 256);
  CHECK(c->result.pages[0].notStaticTagOnly == 256);
  CHECK(c->result.pages[0].knownAgeReferences == 0);

  reset(); CHECK(c->page(0, 4096, 0, true) == -1);
  CHECK(c->page(1, UINT64_MAX, 0, true) == -1);
  CHECK(c->page(1, 256, 512, true) == -1);
  CHECK(c->page(1, 512, 1, true) == -1);
  CHECK(c->result.errors & InvalidRange);

  reset(); p = c->page(1, 4096, 2048, true);
  c->range(p, UINT64_MAX, 256, Owner::Touched, tags(false, false, false), 0);
  c->range(p, 2048, 256, Owner::Touched, tags(false, false, false), 0);
  c->range(p, 1, 256, Owner::Touched, tags(false, false, false), 0);
  c->range(p, 0, 256, Owner::Touched,
           RangeTags{false, TagState::Count, TagState::Unknown}, 0);
  CHECK(c->result.sliceCount == 0 && (c->result.errors & InvalidRange));
  c->page(1, 4096, 2048, false); CHECK(c->result.errors & PageConflict);

  reset(); for (uint32_t i = 0; i <= MaxPages; ++i) c->page(i + 1, 256, 0, true);
  CHECK(c->result.pageCount == MaxPages && (c->result.errors & PageLimit));

  reset(); p = c->page(1, 256, 256, true);
  const auto beforeAlloc = allocations;
  const auto start = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i <= MaxSlices; ++i)
    c->range(p, 0, 256, Owner::ColdCache, tags(true, false, false), 121);
  c->finish(256);
  const auto censusUs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
  CHECK(allocations == beforeAlloc);
  CHECK(c->result.sliceCount == MaxSlices && (c->result.errors & SliceLimit));
  CHECK(c->result.pages[0].owned[3] == 256);
  CHECK(c->result.pages[0].staticTagOnly == 256);

  reset(); c->page(1, 4096, 0, true); c->finish(8192);
  CHECK(c->result.errors & ResidentMismatch);

  Schedule s{}; CHECK(s.take(0, 60) == -1); CHECK(s.take(1, 60) == 0);
  CHECK(s.take(2, 60) == -1); ++s.rejects;
  CHECK(s.take(3, 60) == 1); CHECK(s.take(122, 60) == -1);
  CHECK(s.take(123, 60) == 2); ++s.rejects;
  CHECK(s.take(124, 60) == -1); CHECK(s.take(125, 60) == 3);
  CHECK(s.take(10000, 60) == -1);
  s.noteUnknown(IndexUnknown::NonHostCached, 4096);
  CHECK(s.unknown[1] == 1 && s.bytes[1] == 4096);
  s.reset(9, 3); CHECK(s.mask == 0 && s.rejects == 0 && s.bytes[1] == 0 && s.map == 9);

  // Comprehensive 256-byte bitmap oracle: owner priority, static-tag
  // partition, failed/touched true sets, intersections/unions and unknown
  // aux union, including same-offset alias overlap.
  uint32_t random = 0x20260921u;
  const auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
  for (uint32_t trial = 0; trial < 500; ++trial) {
    reset(); p = c->page(1, 4096, 4096, true);
    std::array<uint8_t, 16> ownerBitmap; ownerBitmap.fill(uint8_t(Owner::Count));
    std::array<uint8_t, 16> staticBitmap{}; staticBitmap.fill(0u);
    std::array<uint8_t, 16> failedTrueBitmap{}; failedTrueBitmap.fill(0u);
    std::array<uint8_t, 16> touchedTrueBitmap{}; touchedTrueBitmap.fill(0u);
    std::array<uint8_t, 16> unknownBitmap{}; unknownBitmap.fill(0u);
    uint32_t expectedUnknownRefs = 0u;
    for (unsigned k = 0; k < 25; ++k) {
      uint32_t first = next() % 16, count = 1 + next() % (16 - first);
      uint32_t owner = next() % OwnerCount;
      bool isStatic = (next() & 1u) != 0u;
      const unsigned failedCode = next() % 3u;
      const unsigned touchedCode = next() % 3u;
      const TagState failed = failedCode == 0u ? TagState::False
          : failedCode == 1u ? TagState::True : TagState::Unknown;
      const TagState touched = touchedCode == 0u ? TagState::False
          : touchedCode == 1u ? TagState::True : TagState::Unknown;
      if (failed == TagState::Unknown || touched == TagState::Unknown)
        ++expectedUnknownRefs;
      c->range(p, first * 256, count * 256, Owner(owner),
               RangeTags::Known(isStatic, failed, touched), 0);
      for (uint32_t j = first; j < first + count; ++j) {
        ownerBitmap[j] = std::min(ownerBitmap[j], uint8_t(owner));
        staticBitmap[j] |= isStatic ? 1u : 2u;
        if (failed == TagState::True) failedTrueBitmap[j] |= 1u;
        if (touched == TagState::True) touchedTrueBitmap[j] |= 1u;
        if (failed == TagState::Unknown || touched == TagState::Unknown)
          unknownBitmap[j] |= 1u;
      }
    }
    c->finish(4096); CHECK(c->result.errors == 0);
    std::array<uint64_t, OwnerCount + 1> expectedOwner{};
    for (auto owner : ownerBitmap) expectedOwner[owner] += 256;
    for (size_t i = 0; i < OwnerCount; ++i)
      CHECK(c->result.pages[0].owned[i] == expectedOwner[i]);
    CHECK(c->result.pages[0].unreferencedUsed == expectedOwner[OwnerCount]);
    uint64_t expectedStaticOnly = 0, expectedNotStaticOnly = 0, expectedMixed = 0;
    uint64_t expectedFailed = 0, expectedTouched = 0;
    uint64_t expectedBoth = 0, expectedEither = 0, expectedUnknown = 0;
    for (uint32_t i = 0; i < 16u; ++i) {
      const uint8_t staticBits = staticBitmap[i];
      if (staticBits == 1u) expectedStaticOnly += 256;
      else if (staticBits == 2u) expectedNotStaticOnly += 256;
      else if (staticBits == 3u) expectedMixed += 256;
      if (failedTrueBitmap[i]) expectedFailed += 256;
      if (touchedTrueBitmap[i]) expectedTouched += 256;
      if (failedTrueBitmap[i] && touchedTrueBitmap[i]) expectedBoth += 256;
      if (failedTrueBitmap[i] || touchedTrueBitmap[i]) expectedEither += 256;
      if (unknownBitmap[i]) expectedUnknown += 256;
    }
    CHECK(c->result.pages[0].staticTagOnly == expectedStaticOnly);
    CHECK(c->result.pages[0].notStaticTagOnly == expectedNotStaticOnly);
    CHECK(c->result.pages[0].mixedTagOverlap == expectedMixed);
    CHECK(c->result.pages[0].failedRetainedUnion == expectedFailed);
    CHECK(c->result.pages[0].touchedUnion == expectedTouched);
    CHECK(c->result.pages[0].touchedAndFailedUnion == expectedBoth);
    CHECK(c->result.pages[0].touchedOrFailedUnion == expectedEither);
    CHECK(c->result.pages[0].tagUnknownUnion == expectedUnknown);
    CHECK(c->result.pages[0].tagUnknownReferences == expectedUnknownRefs);
  }

  // b06 exact UV census resolver integration.  The resolver reports the
  // interval; Collector::range remains the authority for alignment and bounds.
  {
    CensusEntry entry;
    entry.positionSnapshotPage = std::make_shared<CensusResource>();
    entry.uvSnapshotPage = entry.positionSnapshotPage;
    entry.positionSnapshotOffset = 256u;
    entry.uvSnapshotOffset = 256u;
    entry.positionCapacity = 2048u;
    entry.uvCapacity = 0u; // common alias zero-capacity representation
    entry.uvSharesPositionBuffer = true;
    const auto aliasPage = entry.positionSnapshotPage;
    const auto aliasRefs = aliasPage.use_count();

    War3DrawTimeUvCensusSpan uvSpan{};
    CHECK(War3ResolveDrawTimeUvCensusSpan(entry, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::PositionAlias);
    CHECK(uvSpan.offset == 256u && uvSpan.capacity == 2048u);
    CHECK(aliasPage.use_count() == aliasRefs);

    auto c = std::make_unique<Collector>();
    const int p = c->page(1u, 4096u, 4096u, true, uint64_t(aliasPage.use_count()));
    const auto t = tags(true, false, false);
    c->range(p, entry.positionSnapshotOffset, entry.positionCapacity,
             Owner::ColdCache, t, 0u);
    c->range(p, uvSpan.offset, uvSpan.capacity, Owner::ColdCache, t, 0u);
    c->finish(4096u);
    CHECK(c->result.errors == 0);
    CHECK(c->result.pages[0].owned[3] == 2048u);
    CHECK(c->result.pages[0].staticTagOnly == 2048u);
    CHECK(c->result.pages[0].mixedTagOverlap == 0u);
    CHECK(c->result.pages[0].referenceCount == 2u);
    CHECK(c->result.pages[0].staticReferences == 2u);
    CHECK(c->result.sliceCount == 2u);
    CHECK(aliasPage.use_count() == aliasRefs);

    entry.uvCapacity = 8192u; // stale independent-UV capacity is ignored
    uvSpan = {};
    CHECK(War3ResolveDrawTimeUvCensusSpan(entry, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::PositionAlias);
    CHECK(uvSpan.offset == 256u && uvSpan.capacity == 2048u);

    entry.uvSnapshotPage = std::make_shared<CensusResource>();
    CHECK(War3ResolveDrawTimeUvCensusSpan(entry, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);
    entry.uvSnapshotPage = entry.positionSnapshotPage;
    entry.uvSnapshotOffset = 257u;
    CHECK(War3ResolveDrawTimeUvCensusSpan(entry, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);
    entry.uvSnapshotOffset = 256u;
    entry.positionCapacity = 0u;
    CHECK(War3ResolveDrawTimeUvCensusSpan(entry, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);
  }
  {
    CensusEntry external;
    external.uvSharesPositionBuffer = true;
    external.uvCapacity = 4096u; // stale but ignored for both-null alias
    War3DrawTimeUvCensusSpan uvSpan{};
    CHECK(War3ResolveDrawTimeUvCensusSpan(external, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::None);
    external.positionCapacity = 1u;
    CHECK(War3ResolveDrawTimeUvCensusSpan(external, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);
  }
  {
    CensusEntry independent;
    independent.uvSnapshotPage = std::make_shared<CensusResource>();
    independent.uvSnapshotOffset = 256u;
    independent.uvCapacity = 1024u;
    War3DrawTimeUvCensusSpan uvSpan{};
    CHECK(War3ResolveDrawTimeUvCensusSpan(independent, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::Independent);
    CHECK(uvSpan.offset == 256u && uvSpan.capacity == 1024u);

    auto c = std::make_unique<Collector>();
    const int p = c->page(1u, 4096u, 4096u, true);
    c->range(p, uvSpan.offset + 1u, uvSpan.capacity, Owner::ColdCache,
             tags(false, false, false), 0u);
    CHECK(c->result.errors & InvalidRange);
    c->range(p, uvSpan.offset, uvSpan.capacity + 4096u, Owner::ColdCache,
             tags(false, false, false), 0u);
    CHECK(c->result.errors & InvalidRange);
  }
  {
    CensusEntry staleNull;
    staleNull.uvCapacity = 4096u;
    staleNull.uvSnapshotOffset = 256u;
    War3DrawTimeUvCensusSpan uvSpan{};
    CHECK(War3ResolveDrawTimeUvCensusSpan(staleNull, uvSpan) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);
  }
  std::cout << "checks=" << checks << " failures=0 scratchBytes=" << sizeof(Collector)
            << " historyBytes=" << sizeof(History) << " maxAliasesSampleUs=" << censusUs << '\n';
  return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
