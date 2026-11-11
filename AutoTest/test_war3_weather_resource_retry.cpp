#include "../src/d3d9/war3/render/war3_weather_resource_retry.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

namespace {

using dxvk::war3::render::WeatherResourceRetry;
using dxvk::war3::render::WeatherResourceRetryKey;

static_assert(std::is_trivially_copyable<WeatherResourceRetry>::value,
  "Retry state must stay entirely by value");
static_assert(std::is_standard_layout<WeatherResourceRetry>::value,
  "Retry state must not hide a polymorphic owner");
static_assert(sizeof(WeatherResourceRetry) <= 64,
  "Single retry slot must remain small and bounded");
static_assert(noexcept(WeatherResourceRetry{}.ready({}, 0))
  && noexcept(WeatherResourceRetry{}.failed({}, 0))
  && noexcept(WeatherResourceRetry{}.clear()),
  "The hot-path gate must not throw");

bool g_measureAllocations = false;
unsigned g_allocationCount = 0;
unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool passed, const char* label) {
  ++g_checks;
  if (!passed) {
    ++g_failures;
    std::printf("FAIL: %s\n", label);
  }
}

WeatherResourceRetryKey key() {
  return {7, 11, 2560, 1440, 37, 129};
}

void boundariesAndRepeatedFrames() {
  WeatherResourceRetry retry;
  const auto k = key();
  check(retry.ready(k, 0), "no failure permits business frame zero");
  check(retry.ready(k, 55), "no failure permits an arbitrary frame");
  check(retry.ready(k, std::numeric_limits<uint64_t>::max()),
    "no failure permits maximum frame");
  retry.failed(k, 0);
  check(!retry.ready(k, 0), "failure at zero blocks delta zero");
  check(!retry.ready(k, 119), "failure blocks delta 119");
  check(retry.ready(k, 120), "failure permits exact delta 120");
  check(retry.ready(k, 121), "failure permits delta 121");
  retry.failed(k, 120);
  check(!retry.ready(k, 120), "new failure restarts cooldown");
  check(!retry.ready(k, 239), "new failure blocks its delta 119");
  check(retry.ready(k, 240), "new failure permits its delta 120");

  retry.clear();
  retry.failed(k, 600);
  for (unsigned i = 0; i < 10; ++i) {
    retry.failed(k, 600);
    check(!retry.ready(k, 600), "repeated same-frame failure remains blocked");
  }
  for (unsigned i = 0; i < 10; ++i)
    check(!retry.ready(k, 719), "repeated ready check does not shorten cooldown");
  check(retry.ready(k, 720), "same-frame reports do not extend cooldown");
}

void allResourceKeyChanges() {
  const auto original = key();
  for (unsigned field = 0; field < 6; ++field) {
    WeatherResourceRetry retry;
    retry.failed(original, 500);
    auto different = original;
    switch (field) {
      case 0: ++different.mapEpoch; break;
      case 1: ++different.deviceEpoch; break;
      case 2: ++different.width; break;
      case 3: ++different.height; break;
      case 4: ++different.colorFormat; break;
      case 5: ++different.depthFormat; break;
    }
    check(retry.ready(different, 500), "each independent key change bypasses old failure");
    check(retry.ready(original, 500), "single slot discards failure on key switch");
    retry.failed(different, 500);
    check(!retry.ready(different, 619), "new-key failure has its own cooldown");
    check(retry.ready(different, 620), "new-key failure permits delta 120");
  }

  // Map and device identities must retain their high 32 bits.
  WeatherResourceRetry retry;
  retry.failed(original, 200);
  auto highEpoch = original;
  highEpoch.mapEpoch += uint64_t{1} << 32;
  check(retry.ready(highEpoch, 200), "map epoch comparison is full uint64");
  retry.failed(original, 200);
  highEpoch = original;
  highEpoch.deviceEpoch += uint64_t{1} << 32;
  check(retry.ready(highEpoch, 200), "device epoch comparison is full uint64");

  retry.failed(original, 800);
  auto different = original;
  ++different.width;
  retry.failed(different, 1);
  check(!retry.ready(different, 120), "failed accepts changed key with its own lower frame");
  check(retry.ready(different, 121), "changed-key failure not tied to old frame");

  retry.clear();
  const WeatherResourceRetryKey zero = {};
  retry.failed(zero, 0);
  check(!retry.ready(zero, 119) && retry.ready(zero, 120),
    "all-zero key is not an uninitialized-failure sentinel");
}

void rollbackAndOverflow() {
  const auto k = key();
  WeatherResourceRetry retry;
  retry.failed(k, 1000);
  check(!retry.ready(k, 0), "rollback to zero does not bypass cooldown");
  check(!retry.ready(k, 999), "rollback one frame does not underflow elapsed delta");
  retry.failed(k, 1);
  check(!retry.ready(k, 121), "backward failure cannot overwrite later failure stamp");
  check(!retry.ready(k, 1119), "backward failure retains original delta 119 bound");
  check(retry.ready(k, 1120), "original cooldown still ends at its exact bound");

  const auto maximum = std::numeric_limits<uint64_t>::max();
  retry.clear();
  retry.failed(k, maximum - 120);
  check(!retry.ready(k, maximum - 1), "maximum-frame boundary blocks delta 119");
  check(retry.ready(k, maximum), "maximum-frame boundary permits exact delta 120");
  check(!retry.ready(k, 0), "wrap to zero does not permit a prior failed key");
  retry.failed(k, 0);
  check(!retry.ready(k, 120), "wrapped failure cannot shorten near-maximum stamp");

  retry.clear();
  retry.failed(k, maximum - 119);
  check(!retry.ready(k, maximum), "insufficient remaining integer domain fails closed");
  check(!retry.ready(k, 1), "near-overflow cooldown never wraps deadline");
  retry.clear();
  retry.failed(k, maximum);
  check(!retry.ready(k, maximum), "failure at maximum blocks same frame");
  check(!retry.ready(k, 119) && !retry.ready(k, 120),
    "failure at maximum is not treated as 120-frame wrapped deadline");
  auto newDevice = k;
  ++newDevice.deviceEpoch;
  check(retry.ready(newDevice, 0), "new device is immediately eligible after max-frame failure");
}

void explicitClearAndIndependentCopies() {
  const auto k = key();
  WeatherResourceRetry retry;
  retry.failed(k, 900);
  auto copy = retry;
  retry.clear();
  check(retry.ready(k, 0), "successful creation clear removes cooldown");
  check(!copy.ready(k, 0), "by-value copy has independent failure history");
  copy.clear();
  copy.clear();
  check(copy.ready(k, 900), "clear is idempotent");
  retry.failed(k, 0);
  check(!retry.ready(k, 0), "clear does not disable later failure recording");
}

void zeroAllocationWindow() {
  // No check()/printf while the allocation measurement window is open.
  g_allocationCount = 0;
  g_measureAllocations = true;
  WeatherResourceRetry retry;
  bool correct = true;
  auto current = key();
  for (uint64_t i = 0; i < 10000; ++i) {
    retry.failed(current, i * 1000);
    correct &= !retry.ready(current, i * 1000 + 119);
    correct &= retry.ready(current, i * 1000 + 120);
    auto changed = current;
    ++changed.width;
    correct &= retry.ready(changed, 0);
    retry.failed(changed, 0);
    correct &= !retry.ready(changed, 119);
    retry.clear();
    correct &= retry.ready(current, 0);
  }
  g_measureAllocations = false;
  check(correct, "10000 real component cycles keep resource retry semantics");
  check(g_allocationCount == 0, "construction, ready, failed and clear make zero heap allocations");
}

} // namespace

void* operator new(std::size_t size) {
  if (g_measureAllocations)
    ++g_allocationCount;
  if (void* allocation = std::malloc(size ? size : 1))
    return allocation;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }

int main() {
  boundariesAndRepeatedFrames();
  allResourceKeyChanges();
  rollbackAndOverflow();
  explicitClearAndIndependentCopies();
  zeroAllocationWindow();
  std::printf("weather resource retry: %u checks, %u failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
