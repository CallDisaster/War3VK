#pragma once

// Pure lifetime-grouped page selection policy for Stage11 exact snapshots.
//
// This header deliberately does not allocate, free, copy, overwrite or retain
// any Vulkan/DXVK resource.  It only decides whether a new slice should use
// an existing page tail, create a new 16 MiB page, or report that no safe
// choice exists under the caller's current shared cap and create gate.
//
// The page owner remains responsible for the whole page and for proving all
// CPU, command-stream and GPU uses have finished before retirement.  A page
// in RetirePending state is never selected, even when it still has unused
// tail bytes.  The policy never treats cache refcount, use_count, a page
// hole, or cache-unreferenced bytes as an overwrite/free lease.

#include "war3_stage11_snapshot_page_policy.h"

#include <cstdint>
#include <limits>

namespace dxvk::war3::render {

// The policy intentionally reuses the existing shared cap and 16 MiB page
// granularity.  These assertions catch an accidental split into separate
// lifetime quotas at compile time.
static_assert(kWar3Stage11SnapshotPageBytes == (16u << 20u),
              "Stage11 lifetime policy must keep the existing 16 MiB page");
static_assert(kWar3Stage11SnapshotResidentCapBytes == (384u << 20u),
              "Stage11 lifetime policy must keep the shared 384 MiB cap");

// Lifetime evidence is intentionally coarse.  Unknown is the conservative
// class for cases that are not proven static or proven dynamic; it must not
// cause a legal caster allocation to be omitted.
enum class War3Stage11PageLifetime : uint8_t {
  Unknown = 0u,
  ShortLived,
  LongLived,
  Count,
};

enum class War3Stage11PageOwnerState : uint8_t {
  Active = 0u,
  // CPU cache references are gone, but CS/GPU/backing retirement is not
  // proven complete.  Tail bytes in this page are not reusable.
  RetirePending,
  // Removed from the owner vector; kept only by accounting/retire records.
  Retired,
  Count,
};

enum class War3Stage11LifetimePagePlanKind : uint8_t {
  ExistingPage = 0u,
  NewPage,
  NoSafeSelection,
};

enum class War3Stage11LifetimePagePlanReason : uint8_t {
  None = 0u,
  SameLifetime,
  ConservativeUnknownPage,
  MixedLifetimeBorrow,
  NewPageForLifetime,
  InvalidRequest,
  InvalidEpoch,
  InvalidPageMetadata,
  PageIdentityConflict,
  ResidentMismatch,
  SharedCapOutOfRange,
  SharedCapReached,
  PageVectorLimit,
  CreateGateClosed,
  NoActiveTail,
  Count,
};

struct War3Stage11LifetimePageView {
  uint64_t id = 0u;
  uint64_t capacity = 0u;
  uint64_t used = 0u;
  War3Stage11PageLifetime lifetime = War3Stage11PageLifetime::Unknown;
  // Active by default is deliberately not chosen: a forgotten field must not
  // make an old/retiring page selectable.
  War3Stage11PageOwnerState ownerState = War3Stage11PageOwnerState::Retired;
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
};

struct War3Stage11LifetimePageRequest {
  uint64_t requiredBytes = 0u;
  // Capacity of active pages already counted in this shared pool.  Retired
  // and RetirePending backing might still exist physically, but the caller
  // must not add it here because this policy only governs active page tails.
  uint64_t residentBytes = 0u;
  uint64_t capBytes = kWar3Stage11SnapshotResidentCapBytes;
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uint32_t maxPages = 32u;
  // Existing pages are still usable when this is false.  The caller must
  // keep the existing per-frame create gate in the same place as today.
  bool pageCreateGateOpen = false;
  // Strict retention-intent grouping: when true, only the exact
  // requested class may use the sameLifetime/conservative-compatible
  // selection; every other class (including Unknown) is only an
  // explicit mixed borrow.  Default false preserves the existing
  // Unknown-compatible behavior and all current callers.
  bool sameRetentionIntentOnly = false;
  War3Stage11PageLifetime lifetime = War3Stage11PageLifetime::Unknown;
};

struct War3Stage11LifetimePagePlan {
  War3Stage11LifetimePagePlanKind kind =
      War3Stage11LifetimePagePlanKind::NoSafeSelection;
  War3Stage11LifetimePagePlanReason reason =
      War3Stage11LifetimePagePlanReason::InvalidRequest;
  uint64_t pageId = 0u;
  uint64_t offset = 0u;
  uint64_t sliceBytes = 0u;       // aligned bytes the caller must reserve
  uint64_t nextUsed = 0u;         // new tail cursor for an existing page
  uint64_t pageBytes = 0u;        // non-zero only for NewPage
  uint64_t residentBytesAfter = 0u;
  War3Stage11PageLifetime pageLifetime = War3Stage11PageLifetime::Unknown;
  // True when the chosen page had no proof-compatible lifetime class and the
  // caller used a proven different class only to avoid omitting a legal draw.
  bool mixedLifetimeBorrow = false;
  // True when the page class is Unknown while the allocation has a known
  // lifetime, or the allocation is Unknown and shares a known page.
  bool conservativeUnknownPage = false;
  bool createsPage() const noexcept {
    return kind == War3Stage11LifetimePagePlanKind::NewPage;
  }
  bool usesExistingPage() const noexcept {
    return kind == War3Stage11LifetimePagePlanKind::ExistingPage;
  }
  bool safeToUse() const noexcept {
    return kind != War3Stage11LifetimePagePlanKind::NoSafeSelection;
  }
};

inline constexpr const char* War3Stage11PageLifetimeName(
    War3Stage11PageLifetime lifetime) noexcept {
  switch (lifetime) {
  case War3Stage11PageLifetime::Unknown: return "unknown";
  case War3Stage11PageLifetime::ShortLived: return "shortLived";
  case War3Stage11PageLifetime::LongLived: return "longLived";
  default: return "invalid";
  }
}

inline constexpr War3Stage11PageLifetime War3Stage11NormalizePageLifetime(
    War3Stage11PageLifetime lifetime) noexcept {
  return lifetime < War3Stage11PageLifetime::Count
      ? lifetime
      : War3Stage11PageLifetime::Unknown;
}

// Maps existing producer evidence to the page policy class.  This is the
// pure counterpart of d3d9_device.cpp's generationBackedStaticCandidate /
// isStaticGeometry decision.  A failed or unproven capture stays Unknown;
// do not pretend a retained failed slice is a proven lifetime.
inline constexpr War3Stage11PageLifetime War3Stage11ClassifyPageLifetime(
    bool hasDynamicPoseOrUnitIdentity,
    bool generationBackedStaticCandidate,
    bool captureFailedOrUnproven = false) noexcept {
  if (captureFailedOrUnproven)
    return War3Stage11PageLifetime::Unknown;
  if (hasDynamicPoseOrUnitIdentity)
    return War3Stage11PageLifetime::ShortLived;
  if (generationBackedStaticCandidate)
    return War3Stage11PageLifetime::LongLived;
  return War3Stage11PageLifetime::Unknown;
}

// Unknown is conservative-compatible with every page; proven ShortLived and
// proven LongLived must not share a page except in the explicit borrow
// fallback below.  Callers must record mixedLifetimeBorrow when this helper
// is overridden to avoid omitting a legal draw.
inline constexpr bool War3Stage11LifetimeClassesMaySharePage(
    War3Stage11PageLifetime pageLifetime,
    War3Stage11PageLifetime requestedLifetime) noexcept {
  pageLifetime = War3Stage11NormalizePageLifetime(pageLifetime);
  requestedLifetime = War3Stage11NormalizePageLifetime(requestedLifetime);
  return pageLifetime == requestedLifetime ||
      pageLifetime == War3Stage11PageLifetime::Unknown ||
      requestedLifetime == War3Stage11PageLifetime::Unknown;
}

namespace detail {

struct War3Stage11LifetimePageCandidate {
  const War3Stage11LifetimePageView* page = nullptr;
  uint64_t offset = 0u;
  uint64_t nextUsed = 0u;
  uint64_t slack = 0u;
  bool valid = false;
};

inline constexpr bool War3Stage11BetterPageCandidate(
    const War3Stage11LifetimePageCandidate& candidate,
    const War3Stage11LifetimePageCandidate& current) noexcept {
  if (!current.valid)
    return true;
  if (candidate.slack != current.slack)
    return candidate.slack < current.slack;
  if (candidate.nextUsed != current.nextUsed)
    return candidate.nextUsed > current.nextUsed;
  return candidate.page->id < current.page->id;
}

} // namespace detail

// Pure, append-only selection decision.
//
// Priority:
//   1. existing Active page with the same proven lifetime;
//   2. existing Active Unknown page (conservative, no omitted caster);
//   3. create a new page for the requested lifetime / Unknown;
//   4. if creation is blocked by the shared cap, page-vector limit or the
//      caller's existing create gate, use any Active page tail as a
//      correctness-preserving borrow and mark mixedLifetimeBorrow.
//
// A NoSafeSelection result means no legal active tail and no legal create
// under this request.  It is never permission to free a live page.
inline constexpr War3Stage11LifetimePagePlan War3PlanStage11LifetimePage(
    const War3Stage11LifetimePageRequest& request,
    const War3Stage11LifetimePageView* pages,
    uint32_t pageCount) noexcept {
  using Kind = War3Stage11LifetimePagePlanKind;
  using Reason = War3Stage11LifetimePagePlanReason;

  War3Stage11LifetimePagePlan plan = {};
  plan.kind = Kind::NoSafeSelection;
  plan.reason = Reason::InvalidRequest;

  const War3Stage11PageLifetime requested =
      War3Stage11NormalizePageLifetime(request.lifetime);
  plan.pageLifetime = requested;

  uint64_t alignedBytes = 0u;
  if (!War3TryAlignStage11SnapshotBytes(request.requiredBytes, alignedBytes) ||
      alignedBytes == 0u ||
      (pageCount != 0u && pages == nullptr)) {
    return plan;
  }
  if (request.mapEpoch == 0u || request.deviceEpoch == 0u) {
    plan.reason = Reason::InvalidEpoch;
    return plan;
  }
  if (request.capBytes < kWar3Stage11SnapshotResidentCapMinBytes ||
      request.capBytes > kWar3Stage11SnapshotResidentCapMaxBytes) {
    plan.reason = Reason::SharedCapOutOfRange;
    return plan;
  }
  if (request.residentBytes > request.capBytes) {
    plan.reason = Reason::ResidentMismatch;
    return plan;
  }
  constexpr uint32_t kHardMaxPages = uint32_t(
      kWar3Stage11SnapshotResidentCapMaxBytes /
      kWar3Stage11SnapshotPageBytes);
  if (request.maxPages == 0u || request.maxPages > kHardMaxPages ||
      pageCount > request.maxPages) {
    plan.reason = Reason::PageVectorLimit;
    return plan;
  }

  constexpr uint64_t kValidateMask = kWar3Stage11SnapshotAlignment - 1u;
  uint64_t activeViewCapacity = 0u;
  for (uint32_t i = 0u; i < pageCount; ++i) {
    const auto& page = pages[i];
    if (page.id == 0u || page.capacity == 0u ||
        page.capacity > kWar3Stage11SnapshotResidentCapMaxBytes ||
        page.used > page.capacity ||
        (page.capacity & kValidateMask) != 0u ||
        (page.used & kValidateMask) != 0u ||
        page.lifetime >= War3Stage11PageLifetime::Count ||
        page.ownerState >= War3Stage11PageOwnerState::Count) {
      plan.reason = Reason::InvalidPageMetadata;
      return plan;
    }
    for (uint32_t j = 0u; j < i; ++j) {
      if (pages[j].id == page.id) {
        plan.reason = Reason::PageIdentityConflict;
        return plan;
      }
    }
    if (page.ownerState == War3Stage11PageOwnerState::Active)
      activeViewCapacity += page.capacity;
  }
  if (activeViewCapacity != request.residentBytes) {
    plan.reason = Reason::ResidentMismatch;
    return plan;
  }
  plan.sliceBytes = alignedBytes;
  plan.residentBytesAfter = request.residentBytes;

  detail::War3Stage11LifetimePageCandidate sameLifetime = {};
  detail::War3Stage11LifetimePageCandidate conservative = {};
  detail::War3Stage11LifetimePageCandidate mixedBorrow = {};

  const auto consider = [&](detail::War3Stage11LifetimePageCandidate& target,
                            const detail::War3Stage11LifetimePageCandidate& candidate) {
    if (detail::War3Stage11BetterPageCandidate(candidate, target))
      target = candidate;
  };

  for (uint32_t i = 0u; i < pageCount; ++i) {
    const auto& page = pages[i];
    if (page.id == 0u || page.ownerState != War3Stage11PageOwnerState::Active)
      continue;
    if (page.mapEpoch != request.mapEpoch ||
        page.deviceEpoch != request.deviceEpoch)
      continue;
    if (page.capacity == 0u || page.used > page.capacity)
      continue;
    constexpr uint64_t kAlignMask = kWar3Stage11SnapshotAlignment - 1u;
    if ((page.capacity & kAlignMask) != 0u || (page.used & kAlignMask) != 0u)
      continue;

    const auto sub = War3PlanStage11SnapshotSuballocation(
        page.used, page.capacity, alignedBytes);
    if (!sub.valid)
      continue;

    detail::War3Stage11LifetimePageCandidate candidate = {};
    candidate.page = &page;
    candidate.offset = sub.offset;
    candidate.nextUsed = sub.nextUsed;
    candidate.slack = page.capacity - sub.nextUsed;
    candidate.valid = true;

    const auto pageLifetime =
        War3Stage11NormalizePageLifetime(page.lifetime);
    if (pageLifetime == requested) {
      consider(sameLifetime, candidate);
    } else if (!request.sameRetentionIntentOnly &&
               War3Stage11LifetimeClassesMaySharePage(
                   pageLifetime, requested)) {
      consider(conservative, candidate);
    } else {
      consider(mixedBorrow, candidate);
    }
  }

  const auto fillExisting = [&](const detail::War3Stage11LifetimePageCandidate& candidate,
                                Reason reason, bool mixed, bool conservativePage) {
    plan.kind = Kind::ExistingPage;
    plan.reason = reason;
    plan.pageId = candidate.page->id;
    plan.offset = candidate.offset;
    plan.nextUsed = candidate.nextUsed;
    plan.pageBytes = 0u;
    plan.residentBytesAfter = request.residentBytes;
    plan.pageLifetime = War3Stage11NormalizePageLifetime(candidate.page->lifetime);
    plan.mixedLifetimeBorrow = mixed;
    plan.conservativeUnknownPage = conservativePage;
    return plan;
  };

  if (sameLifetime.valid)
    return fillExisting(sameLifetime, Reason::SameLifetime, false, false);

  if (conservative.valid)
    return fillExisting(
        conservative, Reason::ConservativeUnknownPage, false, true);

  const auto createPage = [&]() {
    if (!request.pageCreateGateOpen) {
      plan.reason = Reason::CreateGateClosed;
      return false;
    }
    if (pageCount >= request.maxPages) {
      plan.reason = Reason::PageVectorLimit;
      return false;
    }
    const uint64_t pageBytes =
        War3Stage11SnapshotPageCapacity(alignedBytes, request.capBytes);
    if (pageBytes == 0u || !War3Stage11SnapshotCanAddPage(
                               request.residentBytes, pageBytes,
                               request.capBytes)) {
      plan.reason = Reason::SharedCapReached;
      return false;
    }
    plan.kind = Kind::NewPage;
    plan.reason = Reason::NewPageForLifetime;
    plan.pageId = 0u;
    plan.offset = 0u;
    plan.nextUsed = alignedBytes;
    plan.pageBytes = pageBytes;
    plan.residentBytesAfter = request.residentBytes + pageBytes;
    plan.pageLifetime = requested;
    plan.mixedLifetimeBorrow = false;
    plan.conservativeUnknownPage = false;
    return true;
  };

  if (createPage())
    return plan;

  if (mixedBorrow.valid) {
    return fillExisting(
        mixedBorrow, Reason::MixedLifetimeBorrow, true, false);
  }

  // No page tail exists.  createPage() already preserved the specific
  // blocker reason (gate, vector limit, or shared cap); the caller maps it
  // to the existing ResidentCapacity / PageCreateBudget result and evidence
  // record.
  return plan;
}

} // namespace dxvk::war3::render
