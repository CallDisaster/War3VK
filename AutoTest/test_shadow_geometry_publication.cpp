// CPU exception-safety test of the shared production publication algorithm.
// This does not model GPU retirement or prove rendered output.
#include "../src/d3d9/war3/shadow/war3_shadow_geometry_publication.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <memory>
#include <new>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {
unsigned checks = 0, failures = 0;
char context[128] = "initialization";

void SetContext(const char* value) { std::snprintf(context, sizeof(context), "%s", value); }

void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL [%s]: %s\n", context, message);
  }
}

enum class Domain { Geometry, Registry, Expiry };
enum class AllocationKind { Node, Buckets, Buffer };
enum class Fault { None, BadAlloc, Unexpected };
struct UnexpectedAllocation final : std::exception {
  const char* what() const noexcept override { return "injected non-bad_alloc"; }
};
struct AllocationEvent {
  Domain domain;
  AllocationKind kind;
  size_t count;
};

const char* Name(Domain domain) {
  return domain == Domain::Geometry ? "geometry" :
         domain == Domain::Registry ? "registry" : "expiry";
}
const char* Name(AllocationKind kind) {
  return kind == AllocationKind::Node ? "node" :
         kind == AllocationKind::Buckets ? "buckets" : "buffer";
}

// Shared by allocator rebinds: unordered_map nodes and bucket arrays must be
// independently injectable. Fixed diagnostic storage does not itself allocate.
struct AllocationControl {
  std::array<AllocationEvent, 64> events{};
  size_t calls = 0;
  size_t failAt = 0;
  bool matchingSite = false;
  Domain failDomain = Domain::Geometry;
  AllocationKind failKind = AllocationKind::Node;
  Fault fault = Fault::None;
  unsigned injected = 0;
  unsigned registryNodeFrees = 0;

  void Begin(Fault next = Fault::None, size_t ordinal = 0) {
    calls = 0;
    failAt = ordinal;
    matchingSite = false;
    fault = next;
    injected = 0;
    registryNodeFrees = 0;
  }
  void BeginSite(Fault next, Domain domain, AllocationKind kind) {
    Begin(next);
    matchingSite = true;
    failDomain = domain;
    failKind = kind;
  }
  void BeforeAllocate(Domain domain, AllocationKind kind, size_t count) {
    if (calls < events.size()) events[calls] = {domain, kind, count};
    ++calls;
    const bool hit = matchingSite ? domain == failDomain && kind == failKind
                                  : calls == failAt;
    if (fault == Fault::None || !hit) return;
    ++injected;
    const Fault selected = fault;
    fault = Fault::None; // one-shot: rollback itself is not a second injection
    if (selected == Fault::BadAlloc) throw std::bad_alloc();
    throw UnexpectedAllocation();
  }
};

template<typename T>
struct ThrowingAllocator {
  using value_type = T;
  using propagate_on_container_move_assignment = std::true_type;
  AllocationControl* control = nullptr;
  Domain domain = Domain::Geometry;

  ThrowingAllocator() noexcept = default;
  ThrowingAllocator(AllocationControl* state, Domain owner) noexcept
      : control(state), domain(owner) {}
  template<typename U>
  ThrowingAllocator(const ThrowingAllocator<U>& other) noexcept
      : control(other.control), domain(other.domain) {}
  AllocationKind kind() const noexcept {
    return domain == Domain::Expiry ? AllocationKind::Buffer :
           std::is_pointer<T>::value ? AllocationKind::Buckets : AllocationKind::Node;
  }
  T* allocate(size_t count) {
    if (control) control->BeforeAllocate(domain, kind(), count);
    return std::allocator<T>{}.allocate(count);
  }
  void deallocate(T* pointer, size_t count) noexcept {
    if (control && domain == Domain::Registry && kind() == AllocationKind::Node)
      ++control->registryNodeFrees;
    std::allocator<T>{}.deallocate(pointer, count);
  }
  template<typename U>
  bool operator==(const ThrowingAllocator<U>& other) const noexcept {
    return control == other.control && domain == other.domain;
  }
  template<typename U>
  bool operator!=(const ThrowingAllocator<U>& other) const noexcept { return !(*this == other); }
};

struct LifeCount { unsigned live = 0, created = 0, destroyed = 0; };
struct Payload {
  LifeCount* life;
  uint64_t tag;
  Payload(LifeCount& count, uint64_t value) noexcept : life(&count), tag(value) {
    ++life->live; ++life->created;
  }
  ~Payload() { --life->live; ++life->destroyed; }
};
struct GeometryEntry {
  std::shared_ptr<Payload> payload;
  uint64_t mapEpoch, immutableGeneration, layout, vertexOffset, indexOffset, bytes, lastSeen;
};
struct Slot {
  uint64_t geometryId, mapEpoch, immutableGeneration, layout, lastSeen;
};
struct Token { uint64_t lastSeen, geometryId, mapEpoch; };
struct TokenCompare {
  bool operator()(const Token& a, const Token& b) const noexcept {
    if (a.lastSeen != b.lastSeen) return a.lastSeen > b.lastSeen;
    if (a.geometryId != b.geometryId) return a.geometryId > b.geometryId;
    return a.mapEpoch > b.mapEpoch;
  }
};
struct KeyHash {
  size_t operator()(uint64_t key) const noexcept { return size_t(key ^ (key >> 32)); }
};
struct KeyEqual {
  bool operator()(uint64_t a, uint64_t b) const noexcept { return a == b; }
};
static_assert(std::is_trivial<Slot>::value && std::is_trivial<Token>::value,
              "publication slots and expiry tokens must remain POD in this test");
static_assert(noexcept(TokenCompare{}(Token{}, Token{})), "heap comparator must not throw");

using GeometryMap = std::unordered_map<uint64_t, GeometryEntry, KeyHash, KeyEqual,
    ThrowingAllocator<std::pair<const uint64_t, GeometryEntry>>>;
using RegistryMap = std::unordered_map<uint64_t, Slot, KeyHash, KeyEqual,
    ThrowingAllocator<std::pair<const uint64_t, Slot>>>;
using TokenVector = std::vector<Token, ThrowingAllocator<Token>>;
using QueueBase = std::priority_queue<Token, TokenVector, TokenCompare>;
struct ExpiryQueue : QueueBase {
  explicit ExpiryQueue(AllocationControl& control)
      : QueueBase(TokenCompare{}, TokenVector(ThrowingAllocator<Token>(&control, Domain::Expiry))) {}
  void FillInitialCapacity() {
    c.reserve(4);
    // Do not assume reserve(n) means capacity()==n; fill the actual capacity.
    while (size() < c.capacity()) {
      const uint64_t n = size();
      push(n == 0 ? Token{21u, 100u, 9u} : Token{30u + n, 80u + n, 9u});
    }
  }
  std::vector<Token> PopSequence() const {
    std::priority_queue<Token, std::vector<Token>, TokenCompare> copy(
        TokenCompare{}, std::vector<Token>(c.begin(), c.end()));
    std::vector<Token> result;
    while (!copy.empty()) { result.push_back(copy.top()); copy.pop(); }
    return result;
  }
};

bool Equal(const Slot& a, const Slot& b) {
  return a.geometryId == b.geometryId && a.mapEpoch == b.mapEpoch &&
         a.immutableGeneration == b.immutableGeneration && a.layout == b.layout &&
         a.lastSeen == b.lastSeen;
}
bool operator==(const Token& a, const Token& b) {
  return a.lastSeen == b.lastSeen && a.geometryId == b.geometryId && a.mapEpoch == b.mapEpoch;
}
struct GeometryView {
  uint64_t id, payloadTag;
  const Payload* payload;
  uint64_t mapEpoch, immutableGeneration, layout, vertexOffset, indexOffset, bytes, lastSeen;
};
GeometryView View(uint64_t id, const GeometryEntry& entry) {
  return {id, entry.payload ? entry.payload->tag : 0u, entry.payload.get(), entry.mapEpoch,
          entry.immutableGeneration, entry.layout, entry.vertexOffset, entry.indexOffset,
          entry.bytes, entry.lastSeen};
}
bool operator==(const GeometryView& a, const GeometryView& b) {
  return a.id == b.id && a.payloadTag == b.payloadTag && a.payload == b.payload &&
         a.mapEpoch == b.mapEpoch && a.immutableGeneration == b.immutableGeneration &&
         a.layout == b.layout && a.vertexOffset == b.vertexOffset &&
         a.indexOffset == b.indexOffset && a.bytes == b.bytes && a.lastSeen == b.lastSeen;
}
using RegistryView = std::vector<std::pair<uint64_t, Slot>>;
struct Snapshot {
  std::vector<GeometryView> geometries;
  RegistryView registry;
  std::vector<Token> tokens;
  uint64_t bytes;
};

struct Fixture {
  static constexpr uint64_t oldId = 100, oldKey = 700, newKey = 701, needed = 256;
  AllocationControl allocations;
  LifeCount life;
  GeometryMap geometries;
  RegistryMap registry;
  ExpiryQueue expiry;
  uint64_t bytesUsed = 128, nextId = 101;

  Fixture()
      : geometries(0, KeyHash{}, KeyEqual{}, GeometryMap::allocator_type(&allocations, Domain::Geometry)),
        registry(0, KeyHash{}, KeyEqual{}, RegistryMap::allocator_type(&allocations, Domain::Registry)),
        expiry(allocations) {
    geometries.emplace(oldId, GeometryEntry{std::make_shared<Payload>(life, oldId), 9, 17, 3, 11, 13, 128, 21});
    registry.emplace(oldKey, Slot{oldId, 9, 17, 3, 21});
    expiry.FillInitialCapacity();
    // The next new key must request both a node and a rehash. Rollback is allowed
    // to retain any larger bucket array; only logical state is compared below.
    geometries.max_load_factor(float(geometries.size() + 0.25) / float(geometries.bucket_count()));
    registry.max_load_factor(float(registry.size() + 0.25) / float(registry.bucket_count()));
    allocations.Begin();
  }
  Snapshot Take() const {
    Snapshot result{{}, {}, expiry.PopSequence(), bytesUsed};
    for (const auto& item : geometries) result.geometries.push_back(View(item.first, item.second));
    std::sort(result.geometries.begin(), result.geometries.end(),
              [](const GeometryView& a, const GeometryView& b) { return a.id < b.id; });
    for (const auto& item : registry) result.registry.push_back(item);
    std::sort(result.registry.begin(), result.registry.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return result;
  }
};

struct Candidate {
  uint64_t id;
  std::shared_ptr<Payload> owner;
  std::weak_ptr<Payload> weak;
  GeometryEntry entry;
  Slot slot;
  Token token;
  explicit Candidate(Fixture& fixture, bool duplicate = false)
      : id(duplicate ? Fixture::oldId : fixture.nextId++),
        owner(std::make_shared<Payload>(fixture.life, id)), weak(owner),
        entry{owner, 9, 19, 7, 31, 37, Fixture::needed, 41},
        slot{id, 9, 19, 7, 41}, token{41, id, 9} {}
  void ReleaseInput() { entry.payload.reset(); owner.reset(); }
};
enum class ExceptionKind { None, BadAlloc, Unexpected, Other };
struct Result { bool end = true; uint64_t id = 0; ExceptionKind exception = ExceptionKind::None; };
Result Publish(Fixture& fixture, Candidate& candidate, bool replace) {
  Result result;
  try {
    const auto it = dxvk::war3::shadow::PublishPersistentGeometry(
        fixture.geometries, fixture.registry, fixture.expiry, fixture.bytesUsed,
        Fixture::needed, candidate.id, replace ? Fixture::oldKey : Fixture::newKey,
        std::move(candidate.entry), candidate.slot, candidate.token);
    result.end = it == fixture.geometries.end();
    if (!result.end) result.id = it->first;
  } catch (const std::bad_alloc&) { result.exception = ExceptionKind::BadAlloc;
  } catch (const UnexpectedAllocation&) { result.exception = ExceptionKind::Unexpected;
  } catch (...) { result.exception = ExceptionKind::Other; }
  fixture.allocations.fault = Fault::None;
  return result;
}

void CheckSnapshot(const Fixture& fixture, const Snapshot& expected) {
  const Snapshot actual = fixture.Take();
  Check(actual.geometries == expected.geometries, "geometry map and every old entry field are unchanged");
  bool sameRegistry = actual.registry.size() == expected.registry.size();
  if (sameRegistry) for (size_t i = 0; i < actual.registry.size(); ++i)
    sameRegistry &= actual.registry[i].first == expected.registry[i].first &&
                    Equal(actual.registry[i].second, expected.registry[i].second);
  Check(sameRegistry, "registry keys and every old slot field are unchanged");
  Check(actual.bytes == expected.bytes, "bytesUsed is unchanged");
  Check(actual.tokens == expected.tokens, "expiry heap preserves the exact original pop sequence");
}

void CheckOldSlotNode(const Fixture& fixture, const Slot* oldAddress, bool replace) {
  const auto found = fixture.registry.find(Fixture::oldKey);
  Check(found != fixture.registry.end() && &found->second == oldAddress,
        "old registry slot node is not erased and rebuilt");
  if (replace) Check(fixture.allocations.registryNodeFrees == 0,
                     "replacement never deallocates an existing registry node");
}

void CheckSuccess(const Fixture& fixture, const Snapshot& before, const Candidate& candidate,
                  const GeometryView& expectedEntry, const Result& result, bool replace) {
  Check(result.exception == ExceptionKind::None && !result.end && result.id == candidate.id,
        "successful publication returns the inserted geometry iterator");
  Snapshot expected = before;
  expected.geometries.push_back(expectedEntry);
  std::sort(expected.geometries.begin(), expected.geometries.end(),
            [](const GeometryView& a, const GeometryView& b) { return a.id < b.id; });
  if (replace) {
    for (auto& item : expected.registry) if (item.first == Fixture::oldKey) item.second = candidate.slot;
  } else {
    expected.registry.push_back({Fixture::newKey, candidate.slot});
    std::sort(expected.registry.begin(), expected.registry.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  expected.bytes += Fixture::needed;
  expected.tokens.push_back(candidate.token);
  std::sort(expected.tokens.begin(), expected.tokens.end(),
            [](const Token& a, const Token& b) { return TokenCompare{}(b, a); });
  CheckSnapshot(fixture, expected);
}

std::vector<AllocationEvent> SuccessBaseline(bool replace) {
  SetContext(replace ? "success/replace" : "success/new-key");
  Fixture fixture;
  const Snapshot before = fixture.Take();
  const Slot* oldAddress = &fixture.registry.at(Fixture::oldKey);
  Candidate candidate(fixture);
  const GeometryView expectedEntry = View(candidate.id, candidate.entry);
  const Result result = Publish(fixture, candidate, replace);
  CheckSuccess(fixture, before, candidate, expectedEntry, result, replace);
  CheckOldSlotNode(fixture, oldAddress, replace);
  Check(fixture.nextId == candidate.id + 1, "caller-owned ID advances on success");
  Check(fixture.allocations.calls <= fixture.allocations.events.size(), "allocation trace is not truncated");
  const size_t count = std::min(fixture.allocations.calls, fixture.allocations.events.size());
  std::vector<AllocationEvent> trace(fixture.allocations.events.begin(), fixture.allocations.events.begin() + count);
  std::printf("ALLOCATIONS [%s] count=%zu", context, count);
  for (const auto& event : trace) std::printf(" %s/%s(%zu)", Name(event.domain), Name(event.kind), event.count);
  std::printf("\n");
  const auto has = [&](Domain domain, AllocationKind kind) {
    return std::any_of(trace.begin(), trace.end(), [&](const AllocationEvent& e) { return e.domain == domain && e.kind == kind; });
  };
  Check(has(Domain::Geometry, AllocationKind::Node) && has(Domain::Geometry, AllocationKind::Buckets),
        "success exercises separate geometry-node and rehash allocations");
  Check(has(Domain::Expiry, AllocationKind::Buffer), "success exercises expiry vector growth");
  if (!replace) Check(has(Domain::Registry, AllocationKind::Node) && has(Domain::Registry, AllocationKind::Buckets),
                      "new registry key exercises separate node and rehash allocations");
  else Check(!has(Domain::Registry, AllocationKind::Node), "existing slot replacement does not allocate a registry node");
  candidate.ReleaseInput();
  Check(fixture.life.live == 2 && !candidate.weak.expired(), "successful map owns the candidate RAII payload");
  return trace;
}

void Recover(Fixture& fixture, const Snapshot& original, const Slot* oldAddress, bool replace) {
  Candidate retry(fixture);
  const GeometryView expectedEntry = View(retry.id, retry.entry);
  fixture.allocations.Begin();
  const Result result = Publish(fixture, retry, replace);
  CheckSuccess(fixture, original, retry, expectedEntry, result, replace);
  CheckOldSlotNode(fixture, oldAddress, replace);
  Check(fixture.nextId == retry.id + 1, "recovery uses a fresh caller ID without rewinding");
  retry.ReleaseInput();
  Check(fixture.life.live == 2, "recovery retains only the original and successful payloads");
}

void AllocationFailureMatrix(bool replace, const std::vector<AllocationEvent>& trace) {
  for (size_t ordinal = 1; ordinal <= trace.size(); ++ordinal) {
    char label[128];
    const auto& site = trace[ordinal - 1];
    std::snprintf(label, sizeof(label), "bad_alloc/%s/#%zu/%s/%s", replace ? "replace" : "new-key",
                  ordinal, Name(site.domain), Name(site.kind));
    SetContext(label);
    Fixture fixture;
    const Snapshot before = fixture.Take();
    const Slot* oldAddress = &fixture.registry.at(Fixture::oldKey);
    Candidate candidate(fixture);
    fixture.allocations.Begin(Fault::BadAlloc, ordinal);
    const Result result = Publish(fixture, candidate, replace);
    Check(fixture.allocations.injected == 1, "the selected allocation failure was reached");
    Check(result.exception == ExceptionKind::None && result.end, "bad_alloc is converted to end, not propagated");
    candidate.ReleaseInput();
    CheckSnapshot(fixture, before);
    CheckOldSlotNode(fixture, oldAddress, replace);
    Check(candidate.weak.expired() && fixture.life.live == 1, "rollback releases the failed candidate and preserves old payload");
    Check(fixture.nextId == candidate.id + 1, "failed publication cannot rewind caller-owned geometry ID");
    Recover(fixture, before, oldAddress, replace);
  }
}

void RepeatedFailure(bool replace) {
  SetContext(replace ? "repeated-queue-failure/replace" : "repeated-queue-failure/new-key");
  Fixture fixture;
  const Snapshot before = fixture.Take();
  const Slot* oldAddress = &fixture.registry.at(Fixture::oldKey);
  // Select by allocation kind, not ordinal: successful rehash capacity may be
  // retained by rollback, so later attempts legitimately allocate less.
  for (unsigned n = 0; n < 4; ++n) {
    Candidate candidate(fixture);
    fixture.allocations.BeginSite(Fault::BadAlloc, Domain::Expiry, AllocationKind::Buffer);
    const Result result = Publish(fixture, candidate, replace);
    Check(fixture.allocations.injected == 1 && result.exception == ExceptionKind::None && result.end,
          "each repeated expiry allocation failure returns end");
    candidate.ReleaseInput();
    CheckSnapshot(fixture, before);
    CheckOldSlotNode(fixture, oldAddress, replace);
    Check(candidate.weak.expired() && fixture.life.live == 1, "repeated failures do not accumulate candidate ownership");
    Check(fixture.nextId == 102u + n, "each failed attempt consumes exactly one caller ID");
  }
  Recover(fixture, before, oldAddress, replace);
}

void UnknownException(bool replace) {
  SetContext(replace ? "unknown-exception/replace" : "unknown-exception/new-key");
  Fixture fixture;
  const Snapshot before = fixture.Take();
  const Slot* oldAddress = &fixture.registry.at(Fixture::oldKey);
  Candidate candidate(fixture);
  fixture.allocations.BeginSite(Fault::Unexpected, Domain::Expiry, AllocationKind::Buffer);
  const Result result = Publish(fixture, candidate, replace);
  Check(fixture.allocations.injected == 1 && result.exception == ExceptionKind::Unexpected,
        "unknown exception is rethrown with its original type after rollback");
  candidate.ReleaseInput();
  CheckSnapshot(fixture, before);
  CheckOldSlotNode(fixture, oldAddress, replace);
  Check(candidate.weak.expired() && fixture.life.live == 1, "unknown exception rollback releases candidate ownership");
  Check(fixture.nextId == candidate.id + 1, "unknown exception cannot rewind caller ID");
  Recover(fixture, before, oldAddress, replace);
}

void DuplicateId(bool replace) {
  SetContext(replace ? "duplicate-id/replace" : "duplicate-id/new-key");
  Fixture fixture;
  const Snapshot before = fixture.Take();
  const Slot* oldAddress = &fixture.registry.at(Fixture::oldKey);
  Candidate candidate(fixture, true);
  const uint64_t nextBefore = fixture.nextId;
  const Result result = Publish(fixture, candidate, replace);
  Check(result.exception == ExceptionKind::None && result.end, "duplicate geometry ID returns end");
  candidate.ReleaseInput();
  CheckSnapshot(fixture, before);
  CheckOldSlotNode(fixture, oldAddress, replace);
  Check(candidate.weak.expired() && fixture.life.live == 1, "duplicate insertion does not replace original payload");
  Check(fixture.nextId == nextBefore, "callee never changes caller ID state for duplicate input");
}

void ExternalCandidateQueuedCopy(bool replace) {
  SetContext(replace ? "queued-candidate-rollback/replace" : "queued-candidate-rollback/new-key");
  Fixture fixture;
  const Snapshot before = fixture.Take();
  const Slot* oldAddress = &fixture.registry.at(Fixture::oldKey);
  Candidate candidate(fixture);
  std::shared_ptr<Payload> queuedCopy = candidate.owner;
  fixture.allocations.BeginSite(Fault::BadAlloc, Domain::Expiry, AllocationKind::Buffer);
  const Result result = Publish(fixture, candidate, replace);
  Check(fixture.allocations.injected == 1 && result.exception == ExceptionKind::None && result.end,
        "queued candidate publication still fails cleanly at expiry allocation");
  candidate.ReleaseInput();
  CheckSnapshot(fixture, before);
  CheckOldSlotNode(fixture, oldAddress, replace);
  Check(!candidate.weak.expired() && fixture.life.live == 2 && queuedCopy->tag == candidate.id,
        "rollback cannot destroy a candidate retained by an external queuedCopy");
  queuedCopy.reset();
  Check(candidate.weak.expired() && fixture.life.live == 1,
        "failed candidate is destroyed when its last queuedCopy owner releases it");
  Recover(fixture, before, oldAddress, replace);
}

void ExternalOldQueuedCopy() {
  SetContext("external-old-queued-copy");
  Fixture fixture;
  std::shared_ptr<Payload> queuedCopy = fixture.geometries.at(Fixture::oldId).payload;
  std::weak_ptr<Payload> oldWeak = queuedCopy;
  const Snapshot before = fixture.Take();
  Candidate candidate(fixture);
  const GeometryView expectedEntry = View(candidate.id, candidate.entry);
  const Result result = Publish(fixture, candidate, true);
  CheckSuccess(fixture, before, candidate, expectedEntry, result, true);
  candidate.ReleaseInput();
  // Simulate only CPU container ownership removal, not a GPU retirement fence.
  fixture.geometries.erase(Fixture::oldId);
  Check(!oldWeak.expired() && fixture.life.live == 2 && queuedCopy->tag == Fixture::oldId,
        "external queuedCopy keeps the old payload alive after map ownership ends");
  queuedCopy.reset();
  Check(oldWeak.expired() && fixture.life.live == 1, "last external owner releases exactly the old payload");
  fixture.geometries.clear();
  Check(fixture.life.live == 0 && fixture.life.created == fixture.life.destroyed,
        "all RAII payloads are destroyed exactly once when their owners end");
}

} // namespace

int main() {
  try {
    for (const bool replace : {false, true}) {
      const auto trace = SuccessBaseline(replace);
      AllocationFailureMatrix(replace, trace);
      RepeatedFailure(replace);
      UnknownException(replace);
      DuplicateId(replace);
      ExternalCandidateQueuedCopy(replace);
    }
    ExternalOldQueuedCopy();
  } catch (const std::exception& error) {
    Check(false, "unexpected exception escaped the per-publication capture");
    std::printf("EXCEPTION [%s]: %s\n", context, error.what());
  } catch (...) {
    Check(false, "unknown exception escaped the per-publication capture");
  }
  std::printf("SHADOW_GEOMETRY_PUBLICATION checks=%u failures=%u CPU_ONLY\n", checks, failures);
  return failures ? 1 : 0;
}
