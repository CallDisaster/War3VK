#include "../src/d3d9/war3/render/war3_weather_runtime.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>

namespace {

std::atomic<double> g_clock{0.0};
std::atomic<bool> g_measureAllocations{false};
std::atomic<unsigned> g_allocationCount{0};
unsigned g_checks = 0;
unsigned g_failures = 0;

double fakeClock() noexcept { return g_clock.load(std::memory_order_relaxed); }

void check(bool passed, const char* label) {
  ++g_checks;
  if (!passed) {
    ++g_failures;
    std::printf("FAIL: %s\n", label);
  }
}

bool near(double actual, double expected, double tolerance = 1.0e-5) {
  return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}

using dxvk::war3::render::SnowWeatherParams;
using dxvk::war3::render::WeatherFrameSnapshot;
using dxvk::war3::render::WeatherRegion;
using dxvk::war3::render::WeatherRuntime;

SnowWeatherParams params(float strength, float angle = 0.0f,
                         float speed = 100.0f) {
  return {strength, angle, speed, strength, strength, strength};
}

WeatherFrameSnapshot snap(WeatherRuntime& runtime) {
  return runtime.snapshot(19, 29, 39);
}

void identityAndLifetime() {
  g_clock = 100.0;
  WeatherRuntime runtime(fakeClock);
  check(!runtime.hasEnabled(), "uncreated runtime is disabled");
  const auto empty = snap(runtime);
  check(empty.id == 0 && !empty.enabled, "uncreated snapshot is empty");
  check(empty.frameSerial == 19 && empty.mapEpoch == 29
    && empty.deviceEpoch == 39, "frame domains copied independently");
  const int32_t a = runtime.createSnow(0xffffffffu);
  check(a > 0 && runtime.isAlive(a), "create live positive id");
  check(runtime.createSnow(1) == 0, "single instance capacity");
  check(snap(runtime).seed == 0xffffffffu, "full signed JASS seed bits preserved");
  check(!snap(runtime).enabled && !runtime.hasEnabled(), "create remains disabled");
  check(!runtime.destroy(0) && !runtime.isAlive(-1), "invalid identities refused");
  check(runtime.setTarget(a, params(0.5f), 0), "set manual target");
  check(runtime.setEnabled(a, true) && runtime.hasEnabled(), "enable scheduling hint");
  const auto owned = snap(runtime);
  runtime.resetAuthorState();
  check(!runtime.hasEnabled() && !runtime.isAlive(a), "reset invalidates old instance");
  const int32_t b = runtime.createSnow(2);
  check(b > a, "reset never reuses old id");
  check(!runtime.setEnabled(a, true) && !runtime.setTarget(a, params(1), 0)
    && !runtime.destroy(a), "late old-map commands cannot mutate new instance");
  check(snap(runtime).id == b && !snap(runtime).enabled,
    "late old-map command leaves new state unchanged");
  check(owned.id == a && owned.enabled && owned.params.snowfall == 0.5f,
    "already queued value survives reset without aliasing");
  check(runtime.destroy(b), "destroy current id");
  const int32_t c = runtime.createSnow(3);
  check(c > b && !runtime.isAlive(b), "destroy never reuses old id");
  float unchanged = 123;
  check(!runtime.getTime(b, unchanged) && unchanged == 123,
    "failed query does not overwrite output");
}

void validation() {
  g_clock = 0.0;
  WeatherRuntime runtime(fakeClock);
  const int32_t id = runtime.createSnow(1);
  const auto before = snap(runtime);
  auto p = params(0.5f);
  p.snowfall = std::numeric_limits<float>::quiet_NaN();
  check(!runtime.setTarget(id, p, 0), "NaN snowfall refused");
  p = params(0.5f);
  p.windAngle = std::numeric_limits<float>::infinity();
  check(!runtime.setTarget(id, p, 0), "infinite angle refused");
  p = params(0.5f);
  p.windSpeed = 2048.01f;
  check(!runtime.setTarget(id, p, 0), "speed upper limit refused");
  p.windSpeed = -1;
  check(!runtime.setTarget(id, p, 0), "negative speed refused");
  p = params(1.01f);
  check(!runtime.setTarget(id, p, 0), "intensity above one refused");
  p = params(-0.01f);
  check(!runtime.setTarget(id, p, 0), "negative intensity refused");
  check(!runtime.setTarget(id, params(0), -1)
    && !runtime.setTarget(id, params(0), 3600.01f), "transition limits refused");
  check(!runtime.setTime(id, -1) && !runtime.setTime(id, 86401)
    && !runtime.setTime(id, std::numeric_limits<float>::quiet_NaN()),
    "seek range and finite checks");
  WeatherRegion region;
  region.maxX = region.minX;
  check(!runtime.setRegion(id, region), "empty X range refused");
  region = {};
  region.maxY = region.minY - 1;
  check(!runtime.setRegion(id, region), "reverse Y range refused");
  region = {};
  region.topZ = region.bottomZ;
  check(!runtime.setRegion(id, region), "empty Z range refused");
  region = {};
  region.minX = -10000001.0f;
  check(!runtime.setRegion(id, region), "unbounded coordinate refused");
  region = {};
  region.edgeFeather = 0.5001f;
  check(!runtime.setRegion(id, region), "feather limit refused");
  region = {};
  region.topZ = std::numeric_limits<float>::infinity();
  check(!runtime.setRegion(id, region), "infinite coordinate refused");
  const auto after = snap(runtime);
  check(after.revision == before.revision && after.timeSeconds == 0
    && after.params.snowfall == 0, "invalid requests have no state mutation");
  check(runtime.setTarget(id, params(1, -450, 2048), 3600),
    "inclusive upper bounds and finite negative angle accepted");
  check(runtime.setTarget(id, params(1, -450, 2048), 0), "immediate valid target");
  check(near(snap(runtime).params.windAngle, 270), "angle normalized into 0..360");
  region = {-1.0e7f, -1.0e7f, 1.0e7f, 1.0e7f, -1.0e7f, 1.0e7f, 0.5f};
  check(runtime.setRegion(id, region), "inclusive region limits accepted");
}

void clockPauseAndDisable() {
  g_clock = 10.0;
  WeatherRuntime runtime(fakeClock);
  const int32_t id = runtime.createSnow(1);
  runtime.setTarget(id, params(0.2f, 0, 10), 0);
  g_clock = 1000.0;
  check(near(snap(runtime).timeSeconds, 0), "unenabled wall time excluded");
  runtime.setEnabled(id, true);
  g_clock = 1002.0;
  auto a = snap(runtime);
  check(near(a.timeSeconds, 2) && near(a.windDisplacementX, 20),
    "enabled time and steady wind advance together");
  runtime.setPaused(id, true);
  g_clock = 2000.0;
  auto b = snap(runtime);
  check(b.paused && near(b.timeSeconds, 2) && near(b.windDisplacementX, 20),
    "paused time and wind remain frozen");
  runtime.setPaused(id, false);
  g_clock = 2001.0;
  auto c = snap(runtime);
  check(near(c.timeSeconds, 3) && near(c.windDisplacementX, 30),
    "resume excludes paused interval");
  runtime.setEnabled(id, false);
  check(!runtime.hasEnabled(), "disable changes atomic scheduling hint");
  g_clock = 3000.0;
  check(near(snap(runtime).timeSeconds, 3), "disabled time frozen");
  runtime.setEnabled(id, true);
  g_clock = 3002.0;
  auto d = snap(runtime);
  check(near(d.timeSeconds, 5) && near(d.windDisplacementX, 50),
    "reenable excludes disabled interval");
  const uint64_t revision = d.revision;
  g_clock = 3003.0;
  check(snap(runtime).revision == revision, "frame progression does not rewrite revision");
  g_clock = 2500.0;
  check(near(snap(runtime).timeSeconds, 6), "backward clock does not subtract time");
  g_clock = std::numeric_limits<double>::quiet_NaN();
  check(near(snap(runtime).timeSeconds, 6), "NaN clock does not poison state");
  g_clock = 3004.0;
  check(near(snap(runtime).timeSeconds, 7), "clock recovery does not integrate twice");
  runtime.setTime(id, 86400);
  g_clock = 1.0e12;
  a = snap(runtime);
  check(near(a.timeSeconds, 86400) && std::isfinite(a.windDisplacementX),
    "time domain remains bounded after long absence");
}

void manualTransitionAndWind() {
  g_clock = 0;
  WeatherRuntime runtime(fakeClock);
  const int32_t id = runtime.createSnow(1);
  runtime.setTarget(id, params(0, 350, 10), 0);
  runtime.setEnabled(id, true);
  runtime.setTarget(id, params(1, 10, 30), 10);
  g_clock = 5;
  auto mid = snap(runtime);
  check(near(mid.params.snowfall, 0.5) && near(mid.params.windSpeed, 20),
    "manual transition interpolates correlated fields");
  check(near(mid.params.windAngle, 0), "angle crosses 360 by shortest path");
  const double x = mid.windDisplacementX;
  const double y = mid.windDisplacementY;
  runtime.setTarget(id, params(0.25f, 90, 40), 0);
  const auto switched = snap(runtime);
  check(near(switched.windDisplacementX, x)
    && near(switched.windDisplacementY, y), "wind target switch preserves historical position");
  g_clock = 7;
  auto next = snap(runtime);
  check(near(next.windDisplacementX, x) && near(next.windDisplacementY, y + 80),
    "new wind continues from old displacement");
  check(runtime.setTime(id, 1), "manual seek accepted");
  auto seek = snap(runtime);
  check(near(seek.timeSeconds, 1) && near(seek.windDisplacementX, 0)
    && near(seek.windDisplacementY, 0), "explicit seek resets particle wind history");
  check(near(seek.params.snowfall, 0.25), "manual seek holds current appearance");

  // Independent closed-form oracle: speed=100, angle goes 0->90 over 10s.
  g_clock = 0;
  WeatherRuntime curve(fakeClock);
  const int32_t curveId = curve.createSnow(2);
  curve.setTarget(curveId, params(0, 0, 100), 0);
  curve.setEnabled(curveId, true);
  curve.setTarget(curveId, params(1, 90, 100), 10);
  g_clock = 10;
  const auto arc = snap(curve);
  const double expected = 2000.0 / 3.14159265358979323846;
  check(near(arc.windDisplacementX, expected)
    && near(arc.windDisplacementY, expected), "analytic wind matches circular arc oracle");
  g_clock = 15;
  const auto held = snap(curve);
  check(near(held.windDisplacementX, expected)
    && near(held.windDisplacementY, expected + 500), "finished manual transition holds target wind");
}

void timelineAndCapacity() {
  g_clock = 0;
  WeatherRuntime runtime(fakeClock);
  const int32_t id = runtime.createSnow(1);
  check(!runtime.playTimeline(id, 0), "empty timeline cannot play");
  check(!runtime.addKeyframe(id, 1, params(0)), "first keyframe must be zero");
  check(runtime.addKeyframe(id, 0, params(0, 350, 10)), "zero keyframe accepted");
  check(!runtime.addKeyframe(id, 0, params(1)), "duplicate keyframe time refused");
  check(runtime.addKeyframe(id, 10, params(1, 10, 30)), "second keyframe accepted");
  check(!runtime.addKeyframe(id, 9, params(1)), "descending keyframe refused");
  check(runtime.playTimeline(id, 5), "timeline seek start accepted");
  auto start = snap(runtime);
  check(start.enabled && !start.paused && runtime.hasEnabled(), "play enables and unpauses");
  check(near(start.params.snowfall, 0.5) && near(start.params.windAngle, 0)
    && near(start.windDisplacementX, 0), "play evaluates selected time and rebuilds history");
  check(!runtime.clearKeyframes(id) && !runtime.addKeyframe(id, 20, params(0)),
    "active timeline cannot be modified");
  runtime.setPaused(id, true);
  g_clock = 30;
  check(!runtime.clearKeyframes(id), "paused timeline still owns immutable keyframes");
  runtime.setTime(id, 10);
  check(near(snap(runtime).params.snowfall, 1), "paused seek evaluates exact keyframe");
  runtime.setPaused(id, false);
  g_clock = 35;
  auto end = snap(runtime);
  check(near(end.timeSeconds, 15) && near(end.params.snowfall, 1),
    "end of timeline holds final state");
  runtime.setTarget(id, params(0.3f), 2);
  check(runtime.clearKeyframes(id), "manual target releases timeline edit ownership");
  for (uint32_t i = 0; i < WeatherRuntime::kMaxKeyframes; ++i)
    check(runtime.addKeyframe(id, float(i), params(float(i) / 64)),
      "fixed capacity keyframe accepted");
  const auto full = snap(runtime);
  check(!runtime.addKeyframe(id, 64, params(1)), "65th keyframe refused");
  check(snap(runtime).revision == full.revision, "capacity refusal leaves config unchanged");
}

void samplingIndependentIntegration() {
  g_clock = 0;
  WeatherRuntime coarse(fakeClock);
  WeatherRuntime fine(fakeClock);
  const int32_t a = coarse.createSnow(1);
  const int32_t b = fine.createSnow(1);
  const SnowWeatherParams path[] = {
    params(0.1f, 350, 0), params(0.5f, 10, 100),
    params(0.8f, 170, 500), params(1, 270, 30),
  };
  for (unsigned i = 0; i < 4; ++i) {
    coarse.addKeyframe(a, float(i) * 10, path[i]);
    fine.addKeyframe(b, float(i) * 10, path[i]);
  }
  coarse.playTimeline(a, 0);
  fine.playTimeline(b, 0);
  for (unsigned i = 1; i <= 400; ++i) {
    g_clock = double(i) / 10.0;
    snap(fine);
  }
  const auto fc = snap(coarse);
  const auto ff = snap(fine);
  check(near(fc.windDisplacementX, ff.windDisplacementX, 0.01)
    && near(fc.windDisplacementY, ff.windDisplacementY, 0.01),
    "wind integral independent of one vs 400 snapshot cadence");
  check(near(fc.params.snowfall, ff.params.snowfall)
    && near(fc.timeSeconds, ff.timeSeconds), "timeline state independent of sample cadence");
}

void disabledTimelineEditing() {
  g_clock = 0;
  WeatherRuntime runtime(fakeClock);
  const int32_t id = runtime.createSnow(20);
  runtime.addKeyframe(id, 0, params(0, 0, 10));
  runtime.addKeyframe(id, 10, params(1, 90, 100));
  runtime.playTimeline(id, 0);
  g_clock = 2;
  runtime.setEnabled(id, false);
  const auto frozen = snap(runtime);
  auto invalid = params(1);
  invalid.gust = std::numeric_limits<float>::quiet_NaN();
  check(!runtime.addKeyframe(id, 15, invalid)
    && !runtime.addKeyframe(id, 1, params(1)),
    "invalid disabled timeline edit is refused before changing control mode");
  check(snap(runtime).revision == frozen.revision,
    "failed disabled timeline edit leaves revision unchanged");
  g_clock = 20;
  runtime.setEnabled(id, true);
  g_clock = 21;
  const auto resumed = snap(runtime);
  check(near(resumed.timeSeconds, 3) && near(resumed.params.snowfall, 0.3),
    "disable without successful edit resumes old timeline in place");
  runtime.setEnabled(id, false);
  check(runtime.clearKeyframes(id), "disabled timeline permits clear");
  check(runtime.addKeyframe(id, 0, params(0.8f, 0, 80))
    && runtime.addKeyframe(id, 10, params(0.9f, 90, 90)),
    "disabled timeline permits replacement keyframes");
  check(near(snap(runtime).params.snowfall, 0.3),
    "editing disabled timeline retains frozen current appearance");
  runtime.setEnabled(id, true);
  g_clock = 22;
  check(near(snap(runtime).params.snowfall, 0.3),
    "enable after editing holds appearance until explicit Play");
  check(runtime.playTimeline(id, 0), "replacement timeline explicitly starts");
  check(near(snap(runtime).params.snowfall, 0.8),
    "explicit Play evaluates replacement first keyframe");
  runtime.setPaused(id, true);
  runtime.setEnabled(id, false);
  check(!runtime.clearKeyframes(id)
    && !runtime.addKeyframe(id, 20, params(1)),
    "paused timeline refuses edits even if also disabled");
  runtime.setPaused(id, false);
  check(runtime.addKeyframe(id, 20, params(1)),
    "disabled unpaused timeline permits append");
}

void historyIdentity() {
  g_clock = 0;
  WeatherRuntime runtime(fakeClock);
  check(snap(runtime).historyRevision == 0, "uncreated history starts at zero");
  const int32_t id = runtime.createSnow(1);
  const uint64_t created = snap(runtime).historyRevision;
  check(created > 0, "creation establishes history identity");
  runtime.setTarget(id, params(0.5f), 0);
  runtime.setRegion(id, {});
  runtime.setEnabled(id, true);
  runtime.setPaused(id, true);
  runtime.setPaused(id, false);
  runtime.setEnabled(id, false);
  runtime.addKeyframe(id, 0, params(0));
  runtime.addKeyframe(id, 10, params(1));
  check(snap(runtime).historyRevision == created,
    "continuous author controls retain history identity");
  runtime.playTimeline(id, 0);
  const uint64_t played = snap(runtime).historyRevision;
  check(played == created + 1, "Play increments discontinuous history identity");
  g_clock = 2;
  const auto queued = snap(runtime);
  runtime.setTarget(id, params(0.8f), 5);
  check(snap(runtime).historyRevision == played,
    "continuous departure from timeline preserves history");
  runtime.setTime(id, 1);
  const uint64_t sought = snap(runtime).historyRevision;
  check(sought == played + 1, "Seek increments history identity");
  check(!runtime.setTime(id, -1) && !runtime.playTimeline(0, 0)
    && snap(runtime).historyRevision == sought,
    "invalid discontinuous operations preserve history identity");
  runtime.destroy(id);
  const uint64_t destroyed = snap(runtime).historyRevision;
  check(destroyed == sought + 1, "destroy increments history identity");
  runtime.resetAuthorState();
  const uint64_t reset = snap(runtime).historyRevision;
  check(reset == destroyed + 1, "reset increments without recycling history identity");
  const int32_t nextId = runtime.createSnow(2);
  check(nextId > id && snap(runtime).historyRevision == reset + 1,
    "new instance cannot reuse prior history identity");
  check(queued.id == id && queued.historyRevision == played,
    "already queued snapshot owns old history identity by value");
}

void fixedStorageAllocation() {
  g_clock = 0;
  g_allocationCount = 0;
  g_measureAllocations = true;
  {
    WeatherRuntime runtime(fakeClock);
    const int32_t id = runtime.createSnow(42);
    for (unsigned i = 0; i < 64; ++i)
      runtime.addKeyframe(id, float(i), params(float(i) / 64, float(i) * 7));
    runtime.playTimeline(id, 0);
    for (unsigned i = 0; i < 1000; ++i) {
      g_clock = double(i) / 20;
      snap(runtime);
    }
    runtime.setPaused(id, true);
    runtime.setTime(id, 12);
    runtime.setEnabled(id, false);
    runtime.resetAuthorState();
  }
  g_measureAllocations = false;
  check(g_allocationCount == 0, "production runtime operation window has zero heap allocations");
}

void concurrentValuePublication() {
  g_clock = 0;
  WeatherRuntime runtime(fakeClock);
  const int32_t id = runtime.createSnow(12);
  runtime.setTarget(id, params(0.25f, 25, 25), 0);
  runtime.setEnabled(id, true);
  std::atomic<unsigned> ready{0};
  std::atomic<bool> go{false};
  std::atomic<unsigned> inconsistent{0};
  std::atomic<unsigned> reads{0};
  std::thread writer([&] {
    ++ready;
    while (!go.load(std::memory_order_acquire))
      std::this_thread::yield();
    for (unsigned i = 0; i < 20000; ++i) {
      const float value = (i & 1) ? 0.25f : 0.75f;
      if (!runtime.setTarget(id, params(value, value * 100, value * 100), 0))
        ++inconsistent;
    }
  });
  std::thread reader([&] {
    ++ready;
    while (!go.load(std::memory_order_acquire))
      std::this_thread::yield();
    for (unsigned i = 0; i < 20000; ++i) {
      const auto value = snap(runtime);
      const auto& p = value.params;
      if (value.id != id || !value.enabled || value.seed != 12
          || p.snowfall != p.gust || p.gust != p.haze || p.haze != p.lowSnow
          || p.windSpeed != p.snowfall * 100 || p.windAngle != p.windSpeed
          || value.frameSerial != 19 || value.mapEpoch != 29 || value.deviceEpoch != 39)
        ++inconsistent;
      ++reads;
    }
  });
  while (ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  writer.join();
  reader.join();
  check(reads == 20000 && inconsistent == 0,
    "concurrent production setTarget and snapshots retain complete correlated values");
}

} // namespace

void* operator new(std::size_t size) {
  if (g_measureAllocations.load(std::memory_order_relaxed))
    g_allocationCount.fetch_add(1, std::memory_order_relaxed);
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
  identityAndLifetime();
  validation();
  clockPauseAndDisable();
  manualTransitionAndWind();
  timelineAndCapacity();
  samplingIndependentIntegration();
  disabledTimelineEditing();
  historyIdentity();
  fixedStorageAllocation();
  concurrentValuePublication();
  std::printf("weather runtime: %u checks, %u failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
