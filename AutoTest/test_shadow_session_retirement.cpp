// Production CPU transition/ledger regression, not a GPU-fence or pixel proof.
// The production commit preconditions must remain active even in release tests.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/d3d9/war3/shadow/war3_shadow_session_retirement.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <vector>

using dxvk::war3::shadow::CollectRetiredShadowSessions;
using dxvk::war3::shadow::PersistentGeometryCharges;
using dxvk::war3::shadow::PreparedShadowSessionRetirement;

namespace {
unsigned checks = 0, failures = 0;
const char* context = "initialization";
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { ++failures; std::printf("FAIL [%s]: %s\n", context, message); }
}

struct AllocationState {
  unsigned attempts = 0, allocations = 0, frees = 0, failNext = 0;
};
template<typename T>
struct FailingAllocator {
  using value_type = T;
  AllocationState* state = nullptr;
  FailingAllocator() noexcept = default;
  explicit FailingAllocator(AllocationState* value) noexcept : state(value) {}
  template<typename U>
  FailingAllocator(const FailingAllocator<U>& other) noexcept : state(other.state) {}
  T* allocate(size_t n) {
    if (state) {
      ++state->attempts;
      if (state->failNext) { --state->failNext; throw std::bad_alloc(); }
    }
    T* result = std::allocator<T>{}.allocate(n);
    if (state) ++state->allocations;
    return result;
  }
  void deallocate(T* value, size_t n) noexcept {
    if (state) ++state->frees;
    std::allocator<T>{}.deallocate(value, n);
  }
  template<typename U>
  bool operator==(const FailingAllocator<U>& other) const noexcept { return state == other.state; }
  template<typename U>
  bool operator!=(const FailingAllocator<U>& other) const noexcept { return !(*this == other); }
};

struct Life { unsigned live = 0, created = 0, destroyed = 0; };
struct Resource {
  Life* life;
  uint64_t identity;
  Resource(Life& value, uint64_t id) noexcept : life(&value), identity(id) {
    ++life->live; ++life->created;
  }
  ~Resource() { --life->live; ++life->destroyed; }
};
struct Traffic { unsigned copies = 0, moves = 0; } traffic;
struct Session {
  uint64_t mapEpoch = 0, retireSerial = 0, persistentChargeBytes = 0;
  std::shared_ptr<Resource> geometry, retainedSource;
  Session() noexcept = default;
  Session(const Session& other)
      : mapEpoch(other.mapEpoch), retireSerial(other.retireSerial),
        persistentChargeBytes(other.persistentChargeBytes),
        geometry(other.geometry), retainedSource(other.retainedSource) { ++traffic.copies; }
  Session(Session&& other) noexcept
      : mapEpoch(other.mapEpoch), retireSerial(other.retireSerial),
        persistentChargeBytes(other.persistentChargeBytes), geometry(std::move(other.geometry)),
        retainedSource(std::move(other.retainedSource)) { ++traffic.moves; }
  Session& operator=(const Session& other) {
    ++traffic.copies;
    mapEpoch = other.mapEpoch; retireSerial = other.retireSerial;
    persistentChargeBytes = other.persistentChargeBytes;
    geometry = other.geometry; retainedSource = other.retainedSource;
    return *this;
  }
  Session& operator=(Session&& other) noexcept {
    ++traffic.moves;
    mapEpoch = other.mapEpoch; retireSerial = other.retireSerial;
    persistentChargeBytes = other.persistentChargeBytes;
    geometry = std::move(other.geometry); retainedSource = std::move(other.retainedSource);
    return *this;
  }
};
using Prepared = PreparedShadowSessionRetirement<Session, FailingAllocator<Session>>;
using Queue = Prepared::Queue;
struct LiveOwners {
  uint64_t mapEpoch = 0;
  std::shared_ptr<Resource> geometry, retainedSource;
};
struct Fixture {
  AllocationState allocations;
  Life life;
  Queue queue{FailingAllocator<Session>(&allocations)};
  PersistentGeometryCharges charges;
  LiveOwners live;
};

void OpenLive(Fixture& f, uint64_t epoch, uint64_t bytes, uint64_t geometryId, uint64_t sourceId = 0) {
  Check(f.charges.live == 0 && !f.live.geometry && !f.live.retainedSource,
        "a new live session starts only after previous owners and charge transfer");
  f.live.mapEpoch = epoch;
  f.charges.live = bytes;
  if (geometryId) f.live.geometry = std::make_shared<Resource>(f.life, geometryId);
  if (sourceId) f.live.retainedSource = std::make_shared<Resource>(f.life, sourceId);
}
Session* Retire(Fixture& f, uint64_t serial) {
  Prepared prepared(f.queue);
  Check(bool(prepared), "real list node preparation succeeds");
  if (!prepared) return nullptr;
  return &prepared.commit(f.charges, f.live.mapEpoch, serial, [&](Session& session) noexcept {
    session.geometry = std::move(f.live.geometry);
    session.retainedSource = std::move(f.live.retainedSource);
  });
}
struct NodeView {
  const Session* address;
  uint64_t epoch, serial, bytes;
  const Resource* geometry;
  const Resource* source;
};
std::vector<NodeView> View(const Queue& queue) {
  std::vector<NodeView> result;
  for (const auto& session : queue)
    result.push_back({&session, session.mapEpoch, session.retireSerial, session.persistentChargeBytes,
                      session.geometry.get(), session.retainedSource.get()});
  return result;
}
void CheckQueue(const Queue& queue, const std::vector<NodeView>& expected) {
  const auto actual = View(queue);
  bool same = actual.size() == expected.size();
  if (same) for (size_t i = 0; i < actual.size(); ++i) {
    const auto& a = actual[i]; const auto& b = expected[i];
    same &= a.address == b.address && a.epoch == b.epoch && a.serial == b.serial &&
            a.bytes == b.bytes && a.geometry == b.geometry && a.source == b.source;
  }
  Check(same, "old list nodes keep their addresses, fields, owners and order");
}
void CheckNoRelocation(const Traffic& before) {
  Check(traffic.copies == before.copies && traffic.moves == before.moves,
        "preparation/commit/collection never copies or moves old Session objects");
}

void MapABAAndOutOfOrderCollection() {
  Fixture f;
  const Traffic before = traffic;
  OpenLive(f, 1, 400, 101);
  std::weak_ptr<Resource> oldA = f.live.geometry;
  Retire(f, 30);
  Check(f.charges.live == 0 && f.charges.retired == 400 && f.charges.total() == 400,
        "A retirement transfers, rather than releases, all live charges");
  OpenLive(f, 2, 350, 202);
  std::weak_ptr<Resource> oldB = f.live.geometry;
  Retire(f, 10); // Deliberately earlier completion than the list's first node.
  OpenLive(f, 3, 200, 303); // Same map name A, distinct map-session epoch and owner.
  const Resource* newA = f.live.geometry.get();
  Check(f.charges.live == 200 && f.charges.retired == 750 && f.charges.total() == 950,
        "A-to-B-to-A conservation includes both in-flight sessions and the new live session");
  Check(f.charges.canAdmit(50, 1000) && !f.charges.canAdmit(51, 1000),
        "retired charges enforce the exact capacity boundary before completion");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 9) == 0 &&
        !oldA.expired() && !oldB.expired() && f.life.live == 3,
        "all in-flight owners survive before either serial completes");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 10) == 1 &&
        !oldA.expired() && oldB.expired() && f.queue.size() == 1,
        "collection visits the whole list, releasing a completed non-front session");
  Check(f.charges.live == 200 && f.charges.retired == 400 && f.charges.total() == 600,
        "partial collection removes exactly B's retired charge");
  Check(f.live.mapEpoch == 3 && f.live.geometry.get() == newA && newA->identity == 303,
        "old map collection cannot replace or clear the new A owners");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 10) == 0 &&
        CollectRetiredShadowSessions(f.queue, f.charges, 5) == 0 &&
        CollectRetiredShadowSessions(f.queue, f.charges, 29) == 0,
        "repeated and older completion observations do not release the remaining session");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 30) == 1 && oldA.expired(),
        "the final completed batch releases A exactly once");
  Check(f.queue.empty() && f.charges.live == 200 && f.charges.retired == 0 && f.charges.total() == 200 &&
        f.live.mapEpoch == 3 && f.live.geometry.get() == newA,
        "all retired charges disappear without touching the new live charge");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, UINT64_MAX) == 0 && f.life.live == 1,
        "repeated final collection is idempotent");
  CheckNoRelocation(before);
}

void UnknownSerialAndZeroChargeAlias() {
  Fixture f;
  OpenLive(f, 4, 96, 404);
  std::weak_ptr<Resource> unknown = f.live.geometry;
  Retire(f, 0);
  Check(CollectRetiredShadowSessions(f.queue, f.charges, UINT64_MAX) == 0 && !unknown.expired(),
        "serial zero is unknown and cannot be collected even at maximum completion");
  Check(f.charges.retired == 96 && !f.charges.canAdmit(5, 100),
        "unknown-serial ownership retains its full capacity charge");
  OpenLive(f, 5, 0, 0, 505);
  std::weak_ptr<Resource> alias = f.live.retainedSource;
  Retire(f, 7);
  Check(f.queue.size() == 2 && f.charges.total() == 96 && !alias.expired(),
        "retained-source alias owns a retirement node even with zero additional bytes");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 6) == 0 && !alias.expired(),
        "zero-charge retained source stays alive until its own completion serial");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 7) == 1 && alias.expired() &&
        !unknown.expired() && f.charges.retired == 96 && f.queue.size() == 1,
        "zero-charge collection releases only the completed alias, not the unknown session");
}

void LiveGcDoesNotConsumeRetiredCharges() {
  Fixture f;
  OpenLive(f, 6, 240, 606);
  std::weak_ptr<Resource> retired = f.live.geometry;
  Retire(f, 40);
  OpenLive(f, 7, 120, 707);
  const auto retiredNodes = View(f.queue);
  const Resource* currentOwner = f.live.geometry.get();
  f.charges.releaseLive(50);
  Check(f.charges.live == 70 && f.charges.retired == 240 && f.charges.total() == 310,
        "live GC reduces only the live ledger");
  f.charges.releaseLive(999);
  Check(f.charges.live == 0 && f.charges.retired == 240 && !f.charges.canAdmit(61, 300),
        "saturating live GC cannot consume retired bytes or unlock their capacity");
  CheckQueue(f.queue, retiredNodes);
  Check(!retired.expired() && f.live.geometry.get() == currentOwner && f.live.mapEpoch == 7,
        "ledger-only live GC does not mutate either resource owner");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 39) == 0 && !retired.expired(),
        "live GC does not accelerate retired completion");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 40) == 1 && retired.expired() &&
        f.charges.total() == 0 && f.live.geometry.get() == currentOwner,
        "retired completion remains independent of new-session live ownership");
}

void PrepareFailuresAndRecovery() {
  Fixture f;
  OpenLive(f, 8, 128, 808);
  Retire(f, 80);
  OpenLive(f, 9, 64, 909, 910);
  const auto oldNodes = View(f.queue);
  const Traffic before = traffic;
  const Resource* geometry = f.live.geometry.get();
  const Resource* source = f.live.retainedSource.get();
  const unsigned initialAttempts = f.allocations.attempts;
  const unsigned initialAllocations = f.allocations.allocations;
  f.allocations.failNext = 4;
  for (unsigned n = 0; n < 4; ++n) {
    {
      Prepared prepared(f.queue);
      Check(!prepared, "bad_alloc during preparation returns an unprepared guard");
    }
    CheckQueue(f.queue, oldNodes);
    Check(f.charges.live == 64 && f.charges.retired == 128 && f.charges.total() == 192,
          "repeated preparation failure preserves both ledgers without accumulation");
    Check(f.live.geometry.get() == geometry && f.live.retainedSource.get() == source && f.life.live == 3,
          "failed preparation never moves or releases live owners");
    CheckNoRelocation(before);
  }
  Check(f.allocations.attempts == initialAttempts + 4 && f.allocations.allocations == initialAllocations,
        "four bad_alloc injections reached the real list allocator without retaining nodes");
  Session* added = Retire(f, 90);
  Check(added && added->geometry.get() == geometry && added->retainedSource.get() == source &&
        added->mapEpoch == 9 && added->retireSerial == 90 && added->persistentChargeBytes == 64,
        "the next successful prepare transfers exactly the original live payload");
  Check(f.queue.size() == 2 && f.charges.live == 0 && f.charges.retired == 192 &&
        !f.live.geometry && !f.live.retainedSource,
        "recovery commits exactly one retirement node and one charge transfer");
  const auto after = View(f.queue);
  Check(after.size() == 2 && after.front().address == oldNodes.front().address &&
        after.front().epoch == oldNodes.front().epoch && after.front().serial == oldNodes.front().serial &&
        after.front().geometry == oldNodes.front().geometry && after.front().source == oldNodes.front().source &&
        after.front().bytes == oldNodes.front().bytes,
        "successful list growth also preserves the old node and its ownership");
  CheckNoRelocation(before);
}

struct CleanupFailure final : std::exception {};
void ExceptionBeforeCommitRollsBackPreparedSlot() {
  Fixture f;
  OpenLive(f, 10, 80, 1010);
  Retire(f, 100);
  OpenLive(f, 11, 48, 1111);
  const auto oldNodes = View(f.queue);
  const Traffic before = traffic;
  const Resource* current = f.live.geometry.get();
  const unsigned frees = f.allocations.frees;
  bool caught = false;
  try {
    Prepared prepared(f.queue);
    Check(bool(prepared) && f.queue.size() == oldNodes.size() + 1,
          "guard owns a new placeholder before any live mutation");
    throw CleanupFailure();
  } catch (const CleanupFailure&) { caught = true; }
  Check(caught, "pre-commit exception propagates to the caller");
  CheckQueue(f.queue, oldNodes);
  Check(f.allocations.frees == frees + 1, "guard destroys exactly its uncommitted placeholder node");
  Check(f.charges.live == 48 && f.charges.retired == 80 && f.live.geometry.get() == current && f.life.live == 2,
        "pre-commit exception preserves all charges and resource owners");
  CheckNoRelocation(before);
  Check(Retire(f, 110) != nullptr && f.charges.live == 0 && f.charges.retired == 128,
        "an aborted preparation does not prevent the next transition");
}

void ExceptionAfterCommitKeepsRetiredResources() {
  Fixture f;
  OpenLive(f, 12, 72, 1212, 1213);
  std::weak_ptr<Resource> geometry = f.live.geometry, source = f.live.retainedSource;
  bool caught = false;
  try {
    Prepared prepared(f.queue);
    Check(bool(prepared), "post-commit exception fixture prepares a real node");
    if (!prepared) return;
    prepared.commit(f.charges, f.live.mapEpoch, 120, [&](Session& session) noexcept {
      session.geometry = std::move(f.live.geometry);
      session.retainedSource = std::move(f.live.retainedSource);
    });
    throw CleanupFailure(); // Represents unrelated cleanup outside the transaction.
  } catch (const CleanupFailure&) { caught = true; }
  Check(caught && f.queue.size() == 1 && !geometry.expired() && !source.expired(),
        "committed guard does not undo retirement when later cleanup throws");
  Check(f.charges.live == 0 && f.charges.retired == 72 && !f.live.geometry && !f.live.retainedSource,
        "post-commit exception leaves charge and ownership together in retirement");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 119) == 0 && f.life.live == 2,
        "both committed resource owners survive before completion");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 120) == 1 && geometry.expired() && source.expired() &&
        f.charges.total() == 0 && f.life.created == f.life.destroyed,
        "completion releases committed resources exactly once after the unrelated exception");
}

void ExtremeCapacityArithmetic() {
  constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
  PersistentGeometryCharges almost{maximum - 8, 5};
  Check(almost.total() == maximum - 3 && almost.canAdmit(3, maximum) && !almost.canAdmit(4, maximum),
        "near-UINT64_MAX capacity uses subtraction without wrapped admission");
  Check(!almost.canAdmit(0, maximum - 4) && !almost.canAdmit(maximum, maximum - 1),
        "existing over-cap charge and oversized requests remain rejected");
  PersistentGeometryCharges overflow{maximum, 1};
  Check(overflow.total() == maximum && !overflow.canAdmit(0, maximum) && !overflow.canAdmit(1, maximum),
        "inconsistent overflowing totals saturate for display and fail closed for admission");
  // Never call commit with an overflowing sum: its enabled assertion is the
  // production precondition. Exercise the largest valid transfer instead.
  Fixture f;
  OpenLive(f, 13, maximum - 10, 1313);
  Retire(f, 130);
  OpenLive(f, 14, 10, 1414);
  Check(f.charges.canAdmit(0, maximum) && !f.charges.canAdmit(1, maximum),
        "an exact full ledger allows zero-byte aliases, not one extra byte");
  Retire(f, 140);
  Check(f.charges.live == 0 && f.charges.retired == maximum && f.charges.total() == maximum,
        "largest valid retirement transfer conserves the exact uint64 charge");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 130) == 1 && f.charges.retired == 10 &&
        f.charges.canAdmit(maximum - 10, maximum),
        "partial near-limit collection subtracts exactly without underflow");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 140) == 1 && f.charges.total() == 0 && f.life.live == 0,
        "the second near-limit batch returns the ledger to zero");
}

void InconsistentChargeCannotReleaseOwners() {
  Fixture f;
  OpenLive(f, 15, 80, 1515, 1516);
  std::weak_ptr<Resource> geometry = f.live.geometry, source = f.live.retainedSource;
  Retire(f, 150);
  const auto oldNodes = View(f.queue);
  f.charges.retired = 40; // Explicitly inject a broken accounting witness.
  for (unsigned n = 0; n < 3; ++n) {
    Check(CollectRetiredShadowSessions(f.queue, f.charges, UINT64_MAX) == 0,
          "completed serial cannot authorize a charge larger than the retired ledger");
    CheckQueue(f.queue, oldNodes);
    Check(f.charges.retired == 40 && !geometry.expired() && !source.expired() && f.life.live == 2,
          "inconsistent accounting neither underflows nor prematurely releases resource owners");
  }
  f.charges.retired = 80; // Restore the original witness, not an implicit test-side release.
  Check(CollectRetiredShadowSessions(f.queue, f.charges, 150) == 1 && geometry.expired() && source.expired() &&
        f.charges.total() == 0 && f.queue.empty(),
        "collection resumes only after the charge witness is consistent");
  Check(CollectRetiredShadowSessions(f.queue, f.charges, UINT64_MAX) == 0 &&
        f.life.created == f.life.destroyed, "repeated collection cannot double release corrected ownership");
}
} // namespace

int main() {
  bool assertRan = false;
  assert((assertRan = true));
  Check(assertRan, "assertions are enabled independently of release build flags");
  struct Case { const char* name; void (*run)(); };
  const std::array<Case, 8> cases{{
      {"map-ABA/out-of-order-collection", MapABAAndOutOfOrderCollection},
      {"serial-zero/zero-charge-alias", UnknownSerialAndZeroChargeAlias},
      {"live-GC-isolation", LiveGcDoesNotConsumeRetiredCharges},
      {"prepare-failure/recovery/stable-nodes", PrepareFailuresAndRecovery},
      {"exception-before-commit", ExceptionBeforeCommitRollsBackPreparedSlot},
      {"exception-after-commit", ExceptionAfterCommitKeepsRetiredResources},
      {"uint64-capacity-boundary", ExtremeCapacityArithmetic},
      {"inconsistent-charge-fail-closed", InconsistentChargeCannotReleaseOwners},
  }};
  for (const auto& test : cases) {
    context = test.name;
    std::printf("CASE %s\n", context);
    try { test.run(); }
    catch (const std::exception& error) {
      Check(false, "unexpected exception escaped the CPU test case");
      std::printf("EXCEPTION [%s]: %s\n", context, error.what());
    } catch (...) { Check(false, "unknown exception escaped the CPU test case"); }
  }
  std::printf("SHADOW_SESSION_RETIREMENT checks=%u failures=%u CPU_ONLY\n", checks, failures);
  return failures ? 1 : 0;
}
