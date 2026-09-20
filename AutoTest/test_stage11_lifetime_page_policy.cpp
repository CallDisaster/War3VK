#include "../src/d3d9/war3/render/war3_stage11_lifetime_page_policy.h"
#include "../src/d3d9/war3/render/war3_draw_time_snapshot_lifetime.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dxvk::war3::render;

namespace {

unsigned checks = 0u;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while (false)

constexpr uint64_t kCap = kWar3Stage11SnapshotResidentCapBytes;
constexpr uint64_t kPage = kWar3Stage11SnapshotPageBytes;
constexpr uint64_t kMap = 7u;
constexpr uint64_t kDevice = 9u;

War3Stage11LifetimePageView makeView(
    uint64_t id, uint64_t used, uint64_t capacity,
    War3Stage11PageLifetime lifetime,
    War3Stage11PageOwnerState ownerState = War3Stage11PageOwnerState::Active,
    uint64_t mapEpoch = kMap, uint64_t deviceEpoch = kDevice) {
  War3Stage11LifetimePageView view = {};
  view.id = id;
  view.used = used;
  view.capacity = capacity;
  view.lifetime = lifetime;
  view.ownerState = ownerState;
  view.mapEpoch = mapEpoch;
  view.deviceEpoch = deviceEpoch;
  return view;
}

War3Stage11LifetimePageRequest makeRequest(
    uint64_t bytes, War3Stage11PageLifetime lifetime,
    uint64_t residentBytes = 0u, bool createGateOpen = true,
    uint32_t maxPages = 32u, uint64_t capBytes = kCap) {
  War3Stage11LifetimePageRequest request = {};
  request.requiredBytes = bytes;
  request.residentBytes = residentBytes;
  request.capBytes = capBytes;
  request.mapEpoch = kMap;
  request.deviceEpoch = kDevice;
  request.maxPages = maxPages;
  request.pageCreateGateOpen = createGateOpen;
  request.lifetime = lifetime;
  return request;
}

struct FakeOwnedEntry {
  uint64_t positionCapacity = 0u;
  uint64_t indexCapacity = 0u;
  uint64_t uvCapacity = 0u;
  bool uvSharesPositionBuffer = false;
};

struct FakeCaptureEntry {
  bool captureComplete = false;
  bool complete = false;
  uint64_t frameSerial = 0u;
  uint64_t lastAttemptFrameSerial = 0u;
  uint64_t lastAccessFrameSerial = 0u;
  uint64_t ownedGpuBytes = 0u;
  uint64_t positionCapacity = 0u;
  uint64_t indexCapacity = 0u;
  uint64_t uvCapacity = 0u;
  bool uvSharesPositionBuffer = false;
  bool HasCompleteBacking() const noexcept { return complete; }
};

void TestAliasAndSharedPageTail() {
  War3Stage11LifetimePageView pages[1] = {
      makeView(1u, 0u, kPage, War3Stage11PageLifetime::ShortLived)};

  auto first = War3PlanStage11LifetimePage(
      makeRequest(4096u, War3Stage11PageLifetime::ShortLived, kPage),
      pages, 1u);
  CHECK(first.usesExistingPage());
  CHECK(first.reason == War3Stage11LifetimePagePlanReason::SameLifetime);
  CHECK(first.pageId == 1u);
  CHECK(first.offset == 0u);
  CHECK(first.sliceBytes == 4096u);
  CHECK(first.nextUsed == 4096u);
  pages[0].used = first.nextUsed;

  auto alignedSmall = War3PlanStage11LifetimePage(
      makeRequest(1u, War3Stage11PageLifetime::ShortLived, kPage), pages, 1u);
  CHECK(alignedSmall.usesExistingPage());
  CHECK(alignedSmall.offset == 4096u);
  CHECK(alignedSmall.sliceBytes == 256u);
  CHECK(alignedSmall.nextUsed == 4352u);
  pages[0].used = alignedSmall.nextUsed;

  auto second = War3PlanStage11LifetimePage(
      makeRequest(512u, War3Stage11PageLifetime::ShortLived, kPage), pages, 1u);
  CHECK(second.usesExistingPage());
  CHECK(second.offset == 4352u);
  CHECK(second.nextUsed == 4864u);

  FakeOwnedEntry snapshot = {};
  snapshot.positionCapacity = 4096u;
  snapshot.indexCapacity = 512u;
  snapshot.uvCapacity = 256u;
  CHECK(War3DrawTimeOwnedSnapshotBytes(snapshot) == 4864u);
  snapshot.uvSharesPositionBuffer = true;
  CHECK(War3DrawTimeOwnedSnapshotBytes(snapshot) == 4608u);
}

void TestStaticAnchorAndDynamicRotation() {
  War3Stage11LifetimePageView pages[2] = {
      makeView(1u, 256u, kPage, War3Stage11PageLifetime::LongLived),
      makeView(2u, 0u, kPage, War3Stage11PageLifetime::ShortLived)};

  auto dynamicSlice = War3PlanStage11LifetimePage(
      makeRequest(4096u, War3Stage11PageLifetime::ShortLived, 2u * kPage),
      pages, 2u);
  CHECK(dynamicSlice.usesExistingPage());
  CHECK(dynamicSlice.pageId == 2u);
  CHECK(dynamicSlice.reason == War3Stage11LifetimePagePlanReason::SameLifetime);
  pages[1].used = dynamicSlice.nextUsed;

  auto longSlice = War3PlanStage11LifetimePage(
      makeRequest(4096u, War3Stage11PageLifetime::LongLived, 2u * kPage),
      pages, 2u);
  CHECK(longSlice.usesExistingPage());
  CHECK(longSlice.pageId == 1u);
  pages[0].used = longSlice.nextUsed;

  pages[1].ownerState = War3Stage11PageOwnerState::RetirePending;
  auto rotated = War3PlanStage11LifetimePage(
      makeRequest(4096u, War3Stage11PageLifetime::ShortLived, kPage),
      pages, 2u);
  CHECK(rotated.createsPage());
  CHECK(rotated.reason ==
        War3Stage11LifetimePagePlanReason::NewPageForLifetime);
  CHECK(rotated.pageBytes == kPage);
  CHECK(!rotated.mixedLifetimeBorrow);

  auto stillLong = War3PlanStage11LifetimePage(
      makeRequest(4096u, War3Stage11PageLifetime::LongLived, kPage),
      pages, 2u);
  CHECK(stillLong.usesExistingPage());
  CHECK(stillLong.pageId == 1u);
}

void TestFailedCaptureRollback() {
  FakeCaptureEntry entry = {};
  entry.positionCapacity = 256u;
  entry.indexCapacity = 128u;
  entry.uvCapacity = 64u;
  const uint64_t retained = War3DrawTimeOwnedSnapshotBytes(entry);
  CHECK(retained == 448u);

  {
    War3DrawTimeSnapshotCaptureAttempt attempt(entry, 77u);
    CHECK(!entry.captureComplete);
    CHECK(entry.lastAttemptFrameSerial == 77u);
  }
  CHECK(!entry.captureComplete);
  CHECK(entry.frameSerial == 0u);
  CHECK(entry.lastAccessFrameSerial == 0u);
  CHECK(entry.ownedGpuBytes == retained);

  entry.complete = true;
  {
    War3DrawTimeSnapshotCaptureAttempt attempt(entry, 78u);
    entry.captureComplete = true;
    CHECK(attempt.commit());
  }
  CHECK(entry.captureComplete);
  CHECK(entry.frameSerial == 78u);
  CHECK(entry.lastAccessFrameSerial == 78u);

  CHECK(War3Stage11ClassifyPageLifetime(true, false, false) ==
        War3Stage11PageLifetime::ShortLived);
  CHECK(War3Stage11ClassifyPageLifetime(false, true, false) ==
        War3Stage11PageLifetime::LongLived);
  CHECK(War3Stage11ClassifyPageLifetime(false, false, false) ==
        War3Stage11PageLifetime::Unknown);
  CHECK(War3Stage11ClassifyPageLifetime(true, true, true) ==
        War3Stage11PageLifetime::Unknown);
}

void TestEpochSwitchAndRetirementRefs() {
  War3Stage11LifetimePageView pages[3] = {
      makeView(1u, 0u, kPage, War3Stage11PageLifetime::ShortLived,
               War3Stage11PageOwnerState::Active, 7u, 9u),
      makeView(2u, 0u, kPage, War3Stage11PageLifetime::ShortLived,
               War3Stage11PageOwnerState::Active, 8u, 9u),
      makeView(3u, 0u, kPage, War3Stage11PageLifetime::ShortLived,
               War3Stage11PageOwnerState::RetirePending, 8u, 9u)};

  auto request = makeRequest(
      512u, War3Stage11PageLifetime::ShortLived, 2u * kPage);
  request.mapEpoch = 8u;
  request.deviceEpoch = 9u;
  auto current = War3PlanStage11LifetimePage(request, pages, 3u);
  CHECK(current.usesExistingPage());
  CHECK(current.pageId == 2u);

  pages[1].ownerState = War3Stage11PageOwnerState::RetirePending;
  request.residentBytes = kPage;
  auto freshEpoch = War3PlanStage11LifetimePage(request, pages, 3u);
  CHECK(freshEpoch.createsPage());
  CHECK(freshEpoch.reason ==
        War3Stage11LifetimePagePlanReason::NewPageForLifetime);

  request.pageCreateGateOpen = false;
  auto blocked = War3PlanStage11LifetimePage(request, pages, 3u);
  CHECK(!blocked.safeToUse());
  CHECK(blocked.reason ==
        War3Stage11LifetimePagePlanReason::CreateGateClosed);
}

void TestAlignmentOverflowFullAndUnknown() {
  auto tiny = makeRequest(1u, War3Stage11PageLifetime::ShortLived);
  auto newTiny = War3PlanStage11LifetimePage(tiny, nullptr, 0u);
  CHECK(newTiny.createsPage());
  CHECK(newTiny.sliceBytes == 256u);
  CHECK(newTiny.nextUsed == 256u);
  CHECK(newTiny.pageBytes == kPage);
  CHECK(newTiny.residentBytesAfter == kPage);

  auto overflow = makeRequest(std::numeric_limits<uint64_t>::max(),
                              War3Stage11PageLifetime::ShortLived);
  auto noOverflow = War3PlanStage11LifetimePage(overflow, nullptr, 0u);
  CHECK(!noOverflow.safeToUse());
  CHECK(noOverflow.reason ==
        War3Stage11LifetimePagePlanReason::InvalidRequest);

  War3Stage11LifetimePageView full[1] = {
      makeView(1u, kCap, kCap, War3Stage11PageLifetime::ShortLived)};
  auto capFull = makeRequest(
      256u, War3Stage11PageLifetime::ShortLived, kCap, true, 32u, kCap);
  auto noCap = War3PlanStage11LifetimePage(capFull, full, 1u);
  CHECK(!noCap.safeToUse());
  CHECK(noCap.reason ==
        War3Stage11LifetimePagePlanReason::SharedCapReached);

  War3Stage11LifetimePageView unknown[1] = {
      makeView(9u, 0u, kPage, War3Stage11PageLifetime::Unknown)};
  auto knownOnUnknown = makeRequest(
      256u, War3Stage11PageLifetime::ShortLived, kPage);
  auto conservative = War3PlanStage11LifetimePage(knownOnUnknown, unknown, 1u);
  CHECK(conservative.usesExistingPage());
  CHECK(conservative.pageId == 9u);
  CHECK(conservative.reason ==
        War3Stage11LifetimePagePlanReason::ConservativeUnknownPage);
  CHECK(conservative.conservativeUnknownPage);
  CHECK(!conservative.mixedLifetimeBorrow);

  War3Stage11LifetimePageView shortPage[1] = {
      makeView(5u, 0u, kPage, War3Stage11PageLifetime::ShortLived)};
  auto unknownOnKnown = makeRequest(
      256u, War3Stage11PageLifetime::Unknown, kPage, false);
  auto conservative2 = War3PlanStage11LifetimePage(
      unknownOnKnown, shortPage, 1u);
  CHECK(conservative2.usesExistingPage());
  CHECK(conservative2.pageId == 5u);
  CHECK(conservative2.conservativeUnknownPage);
  CHECK(!conservative2.mixedLifetimeBorrow);

  War3Stage11LifetimePageView longPage[1] = {
      makeView(6u, 0u, kPage, War3Stage11PageLifetime::LongLived)};
  auto shortOnLong = makeRequest(
      256u, War3Stage11PageLifetime::ShortLived, kPage, false);
  auto borrowed = War3PlanStage11LifetimePage(shortOnLong, longPage, 1u);
  CHECK(borrowed.usesExistingPage());
  CHECK(borrowed.pageId == 6u);
  CHECK(borrowed.mixedLifetimeBorrow);
  CHECK(borrowed.reason ==
        War3Stage11LifetimePagePlanReason::MixedLifetimeBorrow);

  auto pageLimit = makeRequest(
      256u, War3Stage11PageLifetime::ShortLived, kCap, true, 1u, kCap);
  auto noVector = War3PlanStage11LifetimePage(pageLimit, full, 1u);
  CHECK(!noVector.safeToUse());
  CHECK(noVector.reason ==
        War3Stage11LifetimePagePlanReason::PageVectorLimit);
}

void TestDoubleCountAccounting() {
  FakeOwnedEntry entry = {};
  entry.positionCapacity = 1024u;
  entry.indexCapacity = 256u;
  entry.uvCapacity = 128u;
  CHECK(War3DrawTimeOwnedSnapshotBytes(entry) == 1408u);
  entry.uvSharesPositionBuffer = true;
  CHECK(War3DrawTimeOwnedSnapshotBytes(entry) == 1280u);

  War3Stage11LifetimePageView pages[1] = {
      makeView(11u, 256u, kPage, War3Stage11PageLifetime::LongLived)};
  auto existing = War3PlanStage11LifetimePage(
      makeRequest(256u, War3Stage11PageLifetime::LongLived, kPage),
      pages, 1u);
  CHECK(existing.usesExistingPage());
  CHECK(existing.residentBytesAfter == kPage);
  CHECK(existing.pageBytes == 0u);

  War3Stage11LifetimePageView full[1] = {
      makeView(12u, kPage, kPage, War3Stage11PageLifetime::LongLived)};
  auto created = War3PlanStage11LifetimePage(
      makeRequest(256u, War3Stage11PageLifetime::LongLived, kPage),
      full, 1u);
  CHECK(created.createsPage());
  CHECK(created.pageBytes == kPage);
  CHECK(created.residentBytesAfter == 2u * kPage);

  CHECK(War3Stage11LifetimeClassesMaySharePage(
      War3Stage11PageLifetime::ShortLived,
      War3Stage11PageLifetime::LongLived) == false);
  CHECK(War3Stage11LifetimeClassesMaySharePage(
      War3Stage11PageLifetime::ShortLived,
      War3Stage11PageLifetime::ShortLived) == true);
  CHECK(War3Stage11LifetimeClassesMaySharePage(
      War3Stage11PageLifetime::Unknown,
      War3Stage11PageLifetime::LongLived) == true);
}

void TestPolicyEdgeRejections() {
  // Unknown/zero epochs must not authorize selection or creation.
  auto epochZero = makeRequest(256u, War3Stage11PageLifetime::Unknown);
  epochZero.mapEpoch = 0u;
  auto noEpoch = War3PlanStage11LifetimePage(epochZero, nullptr, 0u);
  CHECK(noEpoch.reason ==
        War3Stage11LifetimePagePlanReason::InvalidEpoch);

  // Cap ranges are validated against the existing configured max, never
  // expanded by an oversized request.
  auto capBig = makeRequest(256u, War3Stage11PageLifetime::Unknown, 0u,
                            true, 32u,
                            kWar3Stage11SnapshotResidentCapMaxBytes + kPage);
  auto noBigCap = War3PlanStage11LifetimePage(capBig, nullptr, 0u);
  CHECK(noBigCap.reason ==
        War3Stage11LifetimePagePlanReason::SharedCapOutOfRange);
  auto capSmall = makeRequest(
      256u, War3Stage11PageLifetime::Unknown, 0u, true, 32u,
      kWar3Stage11SnapshotResidentCapMinBytes - kPage);
  auto noSmallCap = War3PlanStage11LifetimePage(capSmall, nullptr, 0u);
  CHECK(noSmallCap.reason ==
        War3Stage11LifetimePagePlanReason::SharedCapOutOfRange);

  // Duplicate page identities and conflicting metadata are caller errors,
  // not a license to pick one of the two views.
  War3Stage11LifetimePageView duplicate[2] = {
      makeView(1u, 0u, kPage, War3Stage11PageLifetime::ShortLived),
      makeView(1u, 0u, kPage, War3Stage11PageLifetime::ShortLived)};
  auto duplicated = War3PlanStage11LifetimePage(
      makeRequest(256u, War3Stage11PageLifetime::ShortLived, 2u * kPage),
      duplicate, 2u);
  CHECK(duplicated.reason ==
        War3Stage11LifetimePagePlanReason::PageIdentityConflict);
  War3Stage11LifetimePageView conflict[2] = {
      makeView(1u, 0u, kPage, War3Stage11PageLifetime::ShortLived),
      makeView(1u, 0u, kPage, War3Stage11PageLifetime::LongLived)};
  auto conflicting = War3PlanStage11LifetimePage(
      makeRequest(256u, War3Stage11PageLifetime::ShortLived, 2u * kPage),
      conflict, 2u);
  CHECK(conflicting.reason ==
        War3Stage11LifetimePagePlanReason::PageIdentityConflict);

  // Resident accounting must match the active view capacities the caller
  // supplied.  Under-accounting cannot create a page past the shared cap.
  War3Stage11LifetimePageView onePage[1] = {
      makeView(2u, 0u, kPage, War3Stage11PageLifetime::ShortLived)};
  auto wrongResident = War3PlanStage11LifetimePage(
      makeRequest(256u, War3Stage11PageLifetime::ShortLived, 0u),
      onePage, 1u);
  CHECK(wrongResident.reason ==
        War3Stage11LifetimePagePlanReason::ResidentMismatch);
  auto overResident = War3PlanStage11LifetimePage(
      makeRequest(256u, War3Stage11PageLifetime::ShortLived, 2u * kPage),
      onePage, 1u);
  CHECK(overResident.reason ==
        War3Stage11LifetimePagePlanReason::ResidentMismatch);

  // pageCount/maxPages are bounded; an empty view is valid only with zero
  // resident bytes and can use the normal new-page path.
  auto tooMany = makeRequest(
      256u, War3Stage11PageLifetime::ShortLived, 2u * kPage, true, 1u);
  auto noTooMany = War3PlanStage11LifetimePage(tooMany, duplicate, 2u);
  CHECK(noTooMany.reason ==
        War3Stage11LifetimePagePlanReason::PageVectorLimit);
  auto zeroMax = makeRequest(
      256u, War3Stage11PageLifetime::ShortLived, 0u, true, 0u);
  auto noZeroMax = War3PlanStage11LifetimePage(zeroMax, nullptr, 0u);
  CHECK(noZeroMax.reason ==
        War3Stage11LifetimePagePlanReason::PageVectorLimit);
  auto emptyValid = makeRequest(256u, War3Stage11PageLifetime::Unknown);
  auto emptyCreated = War3PlanStage11LifetimePage(emptyValid, nullptr, 0u);
  CHECK(emptyCreated.createsPage());

  // Invalid page metadata is rejected before any selection.
  War3Stage11LifetimePageView invalid[1] = {
      makeView(0u, 0u, kPage, War3Stage11PageLifetime::ShortLived)};
  auto invalidPage = War3PlanStage11LifetimePage(
      makeRequest(256u, War3Stage11PageLifetime::ShortLived, 0u),
      invalid, 1u);
  CHECK(invalidPage.reason ==
        War3Stage11LifetimePagePlanReason::InvalidPageMetadata);
}

void TestStrictRetentionIntentSeparation() {
  War3Stage11LifetimePageView unknown[1] = {
      makeView(9u, 0u, kPage, War3Stage11PageLifetime::Unknown)};

  // Default false remains Unknown-compatible.
  auto compat = War3PlanStage11LifetimePage(
      makeRequest(256u, War3Stage11PageLifetime::LongLived, kPage),
      unknown, 1u);
  CHECK(compat.usesExistingPage());
  CHECK(compat.conservativeUnknownPage);
  CHECK(!compat.mixedLifetimeBorrow);

  // Strict mode: same class only; Unknown is not conservative.
  auto strict = makeRequest(256u, War3Stage11PageLifetime::LongLived, kPage);
  strict.sameRetentionIntentOnly = true;
  auto createOverUnknown = War3PlanStage11LifetimePage(strict, unknown, 1u);
  CHECK(createOverUnknown.createsPage());
  CHECK(createOverUnknown.reason ==
        War3Stage11LifetimePagePlanReason::NewPageForLifetime);
  CHECK(!createOverUnknown.conservativeUnknownPage);
  CHECK(!createOverUnknown.mixedLifetimeBorrow);

  // Strict mode: create requested class before any different borrow.
  War3Stage11LifetimePageView shortPage[1] = {
      makeView(5u, 0u, kPage, War3Stage11PageLifetime::ShortLived)};
  auto createOverShort = War3PlanStage11LifetimePage(strict, shortPage, 1u);
  CHECK(createOverShort.createsPage());
  CHECK(!createOverShort.mixedLifetimeBorrow);

  // Strict mode with create blocked: borrow any legal tail explicitly.
  auto strictBlocked = strict;
  strictBlocked.pageCreateGateOpen = false;
  auto borrowUnknown = War3PlanStage11LifetimePage(strictBlocked, unknown, 1u);
  CHECK(borrowUnknown.usesExistingPage());
  CHECK(borrowUnknown.pageId == 9u);
  CHECK(borrowUnknown.mixedLifetimeBorrow);
  CHECK(!borrowUnknown.conservativeUnknownPage);
  CHECK(borrowUnknown.reason ==
        War3Stage11LifetimePagePlanReason::MixedLifetimeBorrow);

  // Mixed cap full: no create room, old opposite tail still legal.
  War3Stage11LifetimePageView fullOpposite[1] = {
      makeView(6u, 0u, kCap, War3Stage11PageLifetime::ShortLived)};
  auto strictCapFull = makeRequest(
      256u, War3Stage11PageLifetime::LongLived, kCap, true, 32u, kCap);
  strictCapFull.sameRetentionIntentOnly = true;
  auto borrowCapFull = War3PlanStage11LifetimePage(
      strictCapFull, fullOpposite, 1u);
  CHECK(borrowCapFull.usesExistingPage());
  CHECK(borrowCapFull.pageId == 6u);
  CHECK(borrowCapFull.mixedLifetimeBorrow);
  CHECK(borrowCapFull.reason ==
        War3Stage11LifetimePagePlanReason::MixedLifetimeBorrow);

  // Large page under strict mode still creates the requested-size page.
  const uint64_t largeBytes = 20u << 20u;
  auto largeRequest = makeRequest(
      largeBytes, War3Stage11PageLifetime::LongLived);
  largeRequest.sameRetentionIntentOnly = true;
  auto largePlan = War3PlanStage11LifetimePage(largeRequest, nullptr, 0u);
  CHECK(largePlan.createsPage());
  CHECK(largePlan.nextUsed == largeBytes);
  CHECK(largePlan.pageBytes >= largeBytes);
  CHECK(largePlan.reason ==
        War3Stage11LifetimePagePlanReason::NewPageForLifetime);

  // Invalid epoch remains fail-closed in strict mode.
  auto badEpoch = makeRequest(256u, War3Stage11PageLifetime::LongLived);
  badEpoch.sameRetentionIntentOnly = true;
  badEpoch.mapEpoch = 0u;
  auto noEpoch = War3PlanStage11LifetimePage(badEpoch, nullptr, 0u);
  CHECK(!noEpoch.safeToUse());
  CHECK(noEpoch.reason == War3Stage11LifetimePagePlanReason::InvalidEpoch);

  // A legal old tail must not be silently omitted.
  auto strictOldTail = makeRequest(
      256u, War3Stage11PageLifetime::LongLived, kPage, false);
  strictOldTail.sameRetentionIntentOnly = true;
  auto oldTail = War3PlanStage11LifetimePage(strictOldTail, unknown, 1u);
  CHECK(oldTail.usesExistingPage());
  CHECK(oldTail.pageId == 9u);
  CHECK(oldTail.mixedLifetimeBorrow);
}

void TestPolicyCostFixture() {
  War3Stage11LifetimePageView pages[32] = {};
  for (uint32_t i = 0u; i < 32u; ++i) {
    pages[i] = makeView(
        uint64_t(i + 1u), 0u, kPage,
        (i & 1u) ? War3Stage11PageLifetime::ShortLived
                 : War3Stage11PageLifetime::LongLived);
  }
  auto request = makeRequest(
      256u, War3Stage11PageLifetime::ShortLived, 32u * kPage, false, 32u,
      kWar3Stage11SnapshotResidentCapMaxBytes);
  const auto start = std::chrono::steady_clock::now();
  uint64_t selected = 0u;
  for (uint32_t i = 0u; i < 10000u; ++i) {
    const auto plan = War3PlanStage11LifetimePage(request, pages, 32u);
    selected += plan.usesExistingPage() ? 1u : 0u;
  }
  const auto elapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start).count();
  std::cout << "policyPlanLoopUs=" << elapsedUs
            << " policySelected=" << selected
            << " maxIdentityPairChecks=496\n";
}

} // namespace

int main() {
  try {
    TestAliasAndSharedPageTail();
    TestStaticAnchorAndDynamicRotation();
    TestFailedCaptureRollback();
    TestEpochSwitchAndRetirementRefs();
    TestAlignmentOverflowFullAndUnknown();
    TestDoubleCountAccounting();
    TestPolicyEdgeRejections();
    TestStrictRetentionIntentSeparation();
    TestPolicyCostFixture();
    std::cout << "Stage11 lifetime page policy checks=" << checks
              << " PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
