// Real registry detachment plus the shipping CPU JAPI reset. Build with
// WARVK_JAPI_PROTOCOL_TEST=1 and link war3_japi_v1.cpp/weather_runtime.cpp.
// Only GPU/native backends are excluded by that existing protocol-test mode.
#include "../src/d3d9/war3/japi/war3_managed_object_reset.h"
#include "../src/d3d9/war3/japi/war3_japi_v1.h"
#include "../src/d3d9/war3/render/war3_weather_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <unordered_map>

namespace {
bool denyAllocation = false;
size_t deniedAllocationAttempts = 0;
unsigned checks = 0;
unsigned failures = 0;

void check(bool value, const char* label) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", label);
  }
}

struct NoAllocation {
  size_t before = deniedAllocationAttempts;
  NoAllocation() { denyAllocation = true; }
  ~NoAllocation() { denyAllocation = false; }
  bool untouched() const { return deniedAllocationAttempts == before; }
};

// Test mutex avoids trying to recursively lock a std::mutex (undefined
// behavior). It exercises the production helper's exact lock_guard boundary.
struct AuditMutex {
  bool held = false;
  bool invalid = false;
  unsigned locks = 0;
  unsigned unlocks = 0;
  void lock() noexcept {
    invalid |= held;
    held = true;
    ++locks;
  }
  void unlock() noexcept {
    invalid |= !held;
    held = false;
    ++unlocks;
  }
};

struct Object {
  uint32_t type = 0;
  uint32_t id = 0;
};
using Registry = std::unordered_map<int32_t, Object>;
using dxvk::war3::japi::DetachAndRetireManagedObjects;

void allocationFailureControl() {
  const size_t before = deniedAllocationAttempts;
  bool caught = false;
  {
    NoAllocation guard;
    try {
      void* value = ::operator new(16);
      ::operator delete(value);
    } catch (const std::bad_alloc&) {
      caught = true;
    }
  }
  check(caught && deniedAllocationAttempts == before + 1,
        "fail-allocation injection rejects a real allocation");
}

void emptyReset() {
  Registry live;
  AuditMutex mutex;
  bool clean = false;
  size_t count = 999;
  unsigned called = 0;
  {
    NoAllocation guard;
    count = DetachAndRetireManagedObjects(live, mutex,
        [&](const Object&) noexcept { ++called; });
    clean = guard.untouched();
  }
  check(clean, "empty reset does not request heap allocation");
  check(count == 0 && called == 0 && live.empty(), "empty reset is idempotent");
  check(!mutex.invalid && !mutex.held && mutex.locks == 1 && mutex.unlocks == 1,
        "empty reset has one balanced registry critical section");
}

void populatedReset() {
  Registry live;
  AuditMutex mutex;
  for (int32_t id = 1; id <= 1024; ++id)
    live.emplace(id, Object{uint32_t(id % 4), uint32_t(id)});
  bool clean = false;
  bool allUnlocked = true;
  bool allDetached = true;
  size_t count = 0;
  uint64_t sum = 0;
  uint32_t types[4] = {};
  {
    NoAllocation guard;
    count = DetachAndRetireManagedObjects(live, mutex,
        [&](const Object& object) noexcept {
          allUnlocked &= !mutex.held;
          allDetached &= live.empty();
          sum += object.id;
          ++types[object.type];
        });
    clean = guard.untouched();
  }
  check(clean, "populated reset does not request heap allocation");
  check(count == 1024 && sum == uint64_t(1024) * 1025 / 2,
        "each detached object is retired exactly once");
  check(types[0] == 256 && types[1] == 256 && types[2] == 256 && types[3] == 256,
        "all managed object types remain in detached cleanup");
  check(allDetached && live.empty(), "all old ids are invalid before first callback");
  check(allUnlocked && !mutex.held && !mutex.invalid,
        "every backend callback runs outside registry lock");
}

void reentrantReset() {
  Registry live;
  AuditMutex mutex;
  live.emplace(1, Object{1, 101});
  live.emplace(2, Object{2, 102});
  unsigned calls = 0;
  unsigned nestedCalls = 0;
  size_t nestedCount = 999;
  bool outside = true;
  const size_t count = DetachAndRetireManagedObjects(live, mutex,
      [&](const Object&) noexcept {
        outside &= !mutex.held;
        if (++calls == 1) {
          nestedCount = DetachAndRetireManagedObjects(live, mutex,
              [&](const Object&) noexcept { ++nestedCalls; });
          // This allocation is intentionally outside the fail-allocation
          // window: it represents later authoring, not reset overhead.
          live.emplace(3, Object{3, 103});
        }
      });
  check(count == 2 && calls == 2, "outer detached generation completes once");
  check(nestedCount == 0 && nestedCalls == 0, "reentry cannot see detached old ids");
  check(live.size() == 1 && live.at(3).id == 103,
        "later generation survives detached old-generation destruction");
  check(outside && !mutex.invalid && !mutex.held && mutex.locks == 2,
        "reentrant cleanup never nests the registry lock");
}

void shippingReset() {
  using namespace dxvk::war3::japi;
  using dxvk::war3::render::WeatherRuntime;
  ResetAuthorState();
  Reply created = Dispatch(Carrier::Hotkey, "warvk:v1;weather.createSnow;i:19");
  check(created.ok() && created.integer > 0, "shipping dispatcher creates weather");
  const std::string id = ";d:" + std::to_string(created.integer);
  Reply enabled = Dispatch(Carrier::Preloader,
      "warvk:v1;weather.setEnabled" + id + ";b:1");
  check(enabled.ok() && WeatherRuntime::instance().hasEnabled(),
        "shipping weather is enabled before reset");
  constexpr uint32_t table = 0x12345678u;
  int32_t probe = 0;
  check(!TryTypedSaveInteger(table, kTypedRegisterParent,
        kTypedRegisterChildA, kTypedRegisterCookieA)
      && !TryTypedSaveInteger(table, kTypedRegisterParent,
        kTypedRegisterChildB, kTypedRegisterCookieB),
      "typed registration preserves native forwarding");
  check(TryTypedLoadInteger(table, kTypedRegisterParent, kTypedProbeChild, probe)
      && probe == kTypedProbeAck, "typed table is registered before reset");
  check(TryTypedSaveInteger(table, 7, kTypedBeginChild,
      static_cast<int32_t>(TypedOpcode::MathEvaluateReal)),
      "pending typed transaction exists before reset");
  const auto before = WeatherRuntime::instance().snapshot(1, 10, 20);
  bool clean = false;
  {
    NoAllocation guard;
    // Regression for the old reserve(g_objects.size()) inside noexcept. The
    // old code terminates here rather than being rescued by a test-only catch.
    ResetAuthorState();
    clean = guard.untouched();
  }
  const auto after = WeatherRuntime::instance().snapshot(2, 11, 20);
  check(clean, "actual shipping CPU ResetAuthorState succeeds with new denied");
  check(!WeatherRuntime::instance().hasEnabled() && after.id == 0 && !after.enabled,
        "weather backend reset is reached under allocation pressure");
  check(after.historyRevision > before.historyRevision,
        "weather history identity advances during pressure reset");
  check(!TryTypedLoadInteger(table, kTypedRegisterParent, kTypedProbeChild, probe),
        "pressure reset invalidates previous typed transport table");
  check(Dispatch(Carrier::Hotkey, "warvk:v1;weather.isAlive" + id).integer == 0,
        "old public weather handle is invalid after actual reset");
  check(Dispatch(Carrier::Hotkey, "warvk:v1;managedObject.count").integer == 0,
        "actual public registry is empty after reset");
  Reply next = Dispatch(Carrier::Hotkey, "warvk:v1;weather.createSnow;i:20");
  check(next.ok() && next.integer > created.integer,
        "legal authoring recovers without reusing old public ids");
  ResetAuthorState();
  {
    NoAllocation guard;
    ResetAuthorState();
    ResetAuthorState();
    clean = guard.untouched();
  }
  check(clean, "repeated empty shipping resets also allocate nothing");
}
} // namespace

void* operator new(size_t bytes) {
  if (denyAllocation) {
    ++deniedAllocationAttempts;
    throw std::bad_alloc();
  }
  if (void* value = std::malloc(bytes ? bytes : 1))
    return value;
  throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }

int main() {
  allocationFailureControl();
  emptyReset();
  populatedReset();
  reentrantReset();
  shippingReset();
  std::printf("managed reset checks=%u failures=%u deniedAllocationAttempts=%zu\n",
      checks, failures, deniedAllocationAttempts);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
