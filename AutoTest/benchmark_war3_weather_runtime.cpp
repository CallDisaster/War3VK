#include "../src/d3d9/war3/render/war3_weather_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

using dxvk::war3::render::SnowWeatherParams;
using dxvk::war3::render::WeatherFrameSnapshot;
using dxvk::war3::render::WeatherRuntime;

constexpr unsigned kRounds = 5;
constexpr unsigned kWarmupCalls = 10000;
constexpr unsigned kCallsPerRound = 200000;
constexpr unsigned kCallsPerBatch = 1000;
constexpr unsigned kBatchesPerRound = kCallsPerRound / kCallsPerBatch;
constexpr unsigned kTotalBatches = kRounds * kBatchesPerRound;
constexpr double kStepSeconds = 1.0 / 60.0;
constexpr double kKeyInterval = 53.0;
constexpr uint64_t kMapEpoch = 31;
constexpr uint64_t kDeviceEpoch = 47;
constexpr uint32_t kSeed = 0x5ca7;
constexpr double kPi = 3.14159265358979323846;

static_assert(kCallsPerRound % kCallsPerBatch == 0,
  "Batch timers must cover exactly the advertised call count");
static_assert(double(kCallsPerRound) * kStepSeconds < 86400.0,
  "Measured weather time must never clamp at the runtime limit");
static_assert(double(kCallsPerRound) * kStepSeconds < 63.0 * kKeyInterval
  && double(kCallsPerRound) * kStepSeconds > 62.0 * kKeyInterval,
  "64-key timeline should reach its last interpolated interval, not hold");

double g_fakeClock = 0.0;
bool g_measureAllocations = false;
unsigned g_allocationCount = 0;
volatile double g_snapshotChecksumSink = 0.0;
unsigned g_checks = 0;
unsigned g_failures = 0;

double fakeClock() noexcept { return g_fakeClock; }

enum class Case {
  ChecksumHarness,
  DisabledProbe,
  ManualSameClock,
  ManualAdvanceWind,
  Timeline64AdvanceWind,
  PausedAdvanceWall,
};

constexpr std::array<const char*, 6> kNames = {{
  "checksum-harness-no-runtime-call",
  "disabled-hasEnabled",
  "manual-snapshot-same-clock",
  "manual-snapshot-advance-wind",
  "timeline64-snapshot-advance-wind",
  "paused-snapshot-advance-wall",
}};

struct Result {
  std::array<double, kTotalBatches> batchNsPerCall = {};
  std::array<double, kRounds> roundNsPerCall = {};
  std::array<double, kRounds> checksums = {};
  std::array<unsigned, kRounds> allocations = {};
};

void check(bool passed, const char* label) {
  ++g_checks;
  if (!passed) {
    ++g_failures;
    std::printf("FAIL: %s\n", label);
  }
}

bool near(double actual, double expected, double absoluteTolerance = 1.0e-6) {
  return std::isfinite(actual) && std::abs(actual - expected) <= absoluteTolerance;
}

bool advances(Case which) {
  return which == Case::ManualAdvanceWind
      || which == Case::Timeline64AdvanceWind
      || which == Case::PausedAdvanceWall;
}

SnowWeatherParams manualParams() {
  return {0.6f, 35.0f, 100.0f, 0.3f, 0.5f, 0.4f};
}

SnowWeatherParams keyParams(unsigned index) {
  const float f = float(index) / 63.0f;
  return {0.05f + 0.9f * f, 350.0f + 7.0f * float(index),
    100.0f + 3.0f * float(index), 0.1f + 0.8f * f,
    0.05f + 0.7f * f, 0.15f + 0.75f * f};
}

bool setup(WeatherRuntime& runtime, Case which, int32_t& id) {
  if (which == Case::ChecksumHarness || which == Case::DisabledProbe)
    return !runtime.hasEnabled();
  id = runtime.createSnow(kSeed);
  if (id <= 0)
    return false;
  if (which == Case::Timeline64AdvanceWind) {
    for (unsigned i = 0; i < WeatherRuntime::kMaxKeyframes; ++i) {
      if (!runtime.addKeyframe(id, float(double(i) * kKeyInterval), keyParams(i)))
        return false;
    }
    return runtime.playTimeline(id, 0.0f);
  }
  if (!runtime.setTarget(id, manualParams(), 0.0f)
      || !runtime.setEnabled(id, true))
    return false;
  return which != Case::PausedAdvanceWall || runtime.setPaused(id, true);
}

double consume(const WeatherFrameSnapshot& snapshot) {
  // Consume real returned fields. This fixed checksum is benchmark overhead,
  // not production work; do not silently subtract it from the result.
  return snapshot.timeSeconds + snapshot.windDisplacementX * 0.001
      + snapshot.windDisplacementY * 0.001 + snapshot.params.snowfall
      + double(snapshot.frameSerial) * 0.000001 + double(snapshot.id)
      + double(snapshot.seed) * 0.000001 + double(snapshot.historyRevision);
}

void verifyFinal(WeatherRuntime& runtime, Case which, int32_t id) {
  if (which == Case::ChecksumHarness || which == Case::DisabledProbe) {
    check(!runtime.hasEnabled(), "disabled probe never creates or enables weather");
    return;
  }
  const auto last = runtime.snapshot(kCallsPerRound, kMapEpoch, kDeviceEpoch);
  check(last.id == id && last.seed == kSeed && last.enabled,
    "measured real snapshots retain active author identity");
  check(last.frameSerial == kCallsPerRound && last.mapEpoch == kMapEpoch
      && last.deviceEpoch == kDeviceEpoch,
    "measured real snapshots retain independent frame domains");
  const double expectedTime = which == Case::ManualAdvanceWind
      || which == Case::Timeline64AdvanceWind
    ? double(kCallsPerRound) * kStepSeconds : 0.0;
  check(near(last.timeSeconds, expectedTime),
    "fake clock actually advances or freezes weather as advertised");
  check(last.paused == (which == Case::PausedAdvanceWall),
    "paused benchmark measures paused production state");
  check(std::isfinite(last.windDisplacementX) && std::isfinite(last.windDisplacementY),
    "measured wind integration remains finite");

  if (which == Case::ManualAdvanceWind) {
    const double angle = 35.0 * kPi / 180.0;
    check(near(last.windDisplacementX, 100.0 * expectedTime * std::cos(angle), 1.0e-4)
        && near(last.windDisplacementY, 100.0 * expectedTime * std::sin(angle), 1.0e-4),
      "advanced manual case really performs consistent wind integration");
  } else if (which == Case::Timeline64AdvanceWind) {
    const double keyPosition = expectedTime / kKeyInterval;
    const double expectedAngle = std::fmod(350.0 + 7.0 * keyPosition, 360.0);
    check(near(last.params.snowfall, 0.05 + 0.9 * keyPosition / 63.0, 1.0e-5)
        && near(last.params.windSpeed, 100.0 + 3.0 * keyPosition, 1.0e-4)
        && near(last.params.windAngle, expectedAngle, 1.0e-4),
      "full 64-key timeline reaches last changing interval without clamping");
    check(std::abs(last.windDisplacementX) + std::abs(last.windDisplacementY) > 1.0,
      "timeline benchmark actually integrates nonzero wind history");
  } else {
    check(near(last.windDisplacementX, 0) && near(last.windDisplacementY, 0),
      "same-clock and paused cases do not gain wind displacement");
  }
}

void measureRound(Case which, unsigned round, Result& result) {
  g_fakeClock = 0.0;
  WeatherRuntime runtime(fakeClock);
  int32_t id = 0;
  if (!setup(runtime, which, id)) {
    check(false, "benchmark setup uses valid production calls");
    return;
  }

  // Warm up the actual selected branch, with setup and seek excluded from the
  // allocation/timing window. Do not roll the injected wall clock backward.
  double warmupChecksum = 0.0;
  for (unsigned i = 0; i < kWarmupCalls; ++i) {
    if (which == Case::ChecksumHarness) {
      warmupChecksum += 1.0;
    } else if (which == Case::DisabledProbe) {
      warmupChecksum += runtime.hasEnabled() ? 3.0 : 1.0;
    } else {
      if (advances(which))
        g_fakeClock += kStepSeconds;
      warmupChecksum += consume(runtime.snapshot(i + 1, kMapEpoch, kDeviceEpoch));
    }
  }
  g_snapshotChecksumSink = warmupChecksum;
  if (id > 0 && !runtime.setTime(id, 0.0f)) {
    check(false, "outside-window seek resets measured weather time and wind");
    return;
  }
  const auto initial = runtime.snapshot(0, kMapEpoch, kDeviceEpoch);
  if (id > 0 && (!near(initial.timeSeconds, 0)
      || !near(initial.windDisplacementX, 0)
      || !near(initial.windDisplacementY, 0))) {
    check(false, "warmup reset has no leftover weather time or wind");
    return;
  }
  const double fakeClockAtStart = g_fakeClock;
  // Touch the wall timer before enabling the measurement allocation counter.
  const auto timerWarmup = std::chrono::steady_clock::now();
  (void)timerWarmup;
  volatile uint64_t probeChecksum = 0;
  double snapshotChecksum = 0.0;
  g_allocationCount = 0;
  g_measureAllocations = true;
  for (unsigned batch = 0; batch < kBatchesPerRound; ++batch) {
    const auto start = std::chrono::steady_clock::now();
    for (unsigned call = 0; call < kCallsPerBatch; ++call) {
      if (which == Case::ChecksumHarness) {
        probeChecksum += 1;
      } else if (which == Case::DisabledProbe) {
        // Volatile accumulation prevents an apparently all-false run from
        // becoming an unused result. Atomic hasEnabled is the real method.
        probeChecksum += runtime.hasEnabled() ? 3 : 1;
      } else {
        if (advances(which))
          g_fakeClock += kStepSeconds;
        const uint64_t frame = uint64_t(batch) * kCallsPerBatch + call + 1;
        snapshotChecksum += consume(runtime.snapshot(frame, kMapEpoch, kDeviceEpoch));
      }
    }
    const auto finish = std::chrono::steady_clock::now();
    const double nanoseconds = std::chrono::duration<double, std::nano>(finish - start).count();
    result.batchNsPerCall[round * kBatchesPerRound + batch]
      = nanoseconds / double(kCallsPerBatch);
  }
  g_measureAllocations = false;
  result.allocations[round] = g_allocationCount;
  result.checksums[round] = snapshotChecksum + double(probeChecksum);
  g_snapshotChecksumSink = result.checksums[round];

  double roundTotal = 0.0;
  for (unsigned batch = 0; batch < kBatchesPerRound; ++batch)
    roundTotal += result.batchNsPerCall[round * kBatchesPerRound + batch];
  result.roundNsPerCall[round] = roundTotal / double(kBatchesPerRound);
  check(result.allocations[round] == 0, "entire timed component window has zero heap allocations");
  check(std::isfinite(result.checksums[round]) && result.checksums[round] > 0.0,
    "measured production calls feed a nonzero finite checksum");
  if (which == Case::ChecksumHarness || which == Case::DisabledProbe)
    check(probeChecksum == kCallsPerRound, "volatile checksum retains every disabled probe call");
  const double expectedWallAdvance = advances(which)
    ? double(kCallsPerRound) * kStepSeconds : 0.0;
  check(near(g_fakeClock - fakeClockAtStart, expectedWallAdvance),
    "timed loop advances fake wall clock exactly as advertised");
  verifyFinal(runtime, which, id);
}

void report(Case which, const Result& result) {
  auto sortedBatches = result.batchNsPerCall;
  auto sortedRounds = result.roundNsPerCall;
  std::sort(sortedBatches.begin(), sortedBatches.end());
  std::sort(sortedRounds.begin(), sortedRounds.end());
  const double median = 0.5 * (sortedBatches[kTotalBatches / 2 - 1]
    + sortedBatches[kTotalBatches / 2]);
  // Nearest-rank P95 of 1000 batch means, NOT P95 of individual calls.
  constexpr unsigned p95Index = (kTotalBatches * 95 + 99) / 100 - 1;
  std::printf("CASE %s\n", kNames[unsigned(which)]);
  std::printf("  rounds=%u callsPerRound=%u totalCalls=%u warmupPerRound=%u batchCalls=%u\n",
    kRounds, kCallsPerRound, kRounds * kCallsPerRound, kWarmupCalls, kCallsPerBatch);
  std::printf("  batchMeanNsPerCall median=%.3f p95=%.3f min=%.3f max=%.3f roundMeanMedian=%.3f\n",
    median, sortedBatches[p95Index], sortedBatches.front(), sortedBatches.back(),
    sortedRounds[kRounds / 2]);
  for (unsigned round = 0; round < kRounds; ++round) {
    std::printf("  round=%u meanNsPerCall=%.3f checksum=%.12g heapAllocations=%u\n",
      round + 1, result.roundNsPerCall[round], result.checksums[round], result.allocations[round]);
  }
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
  std::printf("WeatherRuntime production CPU microbenchmark\n");
  std::printf("Timer=steady_clock; weatherClock=injected monotonic fake; no GPU/game/frame-FPS proof\n");
  std::printf("P95 is the distribution of 1000-call batch means, not individual-call tail latency.\n");
  std::printf("Reported costs include loop/checksum/timer and fake-clock overhead; no baseline subtraction.\n");
  for (unsigned caseIndex = 0; caseIndex < kNames.size(); ++caseIndex) {
    const auto which = Case(caseIndex);
    Result result;
    for (unsigned round = 0; round < kRounds; ++round)
      measureRound(which, round, result);
    report(which, result);
  }
  std::printf("weather runtime benchmark: %u checks, %u failures, checksumSink=%.12g\n",
    g_checks, g_failures, double(g_snapshotChecksumSink));
  return g_failures ? 1 : 0;
}
