#include "war3_weather_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace dxvk::war3::render {
namespace {

constexpr double kDegreesToRadians = 0.017453292519943295769;

double normalizedAngle(double degrees) noexcept {
  degrees = std::fmod(degrees, 360.0);
  return degrees < 0.0 ? degrees + 360.0 : degrees;
}

double angleDelta(double from, double to) noexcept {
  double delta = normalizedAngle(to - from);
  return delta > 180.0 ? delta - 360.0 : delta;
}

bool unitRange(float value) noexcept {
  return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

float mix(float from, float to, double fraction) noexcept {
  return float(double(from) + (double(to) - double(from)) * fraction);
}

} // namespace

WeatherRuntime::WeatherRuntime(Clock clock) noexcept
: m_clock(clock ? clock : steadySeconds) {
}

WeatherRuntime& WeatherRuntime::instance() {
  static WeatherRuntime runtime;
  return runtime;
}

bool WeatherRuntime::hasEnabled() const noexcept {
  return m_enabledProbe.load(std::memory_order_acquire);
}

double WeatherRuntime::steadySeconds() noexcept {
  return std::chrono::duration<double>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool WeatherRuntime::validParams(const SnowWeatherParams& p) noexcept {
  return unitRange(p.snowfall) && unitRange(p.gust)
      && unitRange(p.haze) && unitRange(p.lowSnow)
      && std::isfinite(p.windAngle) && std::isfinite(p.windSpeed)
      && p.windSpeed >= 0.0f && p.windSpeed <= kMaxWindSpeed;
}

bool WeatherRuntime::validRegion(const WeatherRegion& r) noexcept {
  const float coordinates[] = {
    r.minX, r.minY, r.maxX, r.maxY, r.bottomZ, r.topZ,
  };
  for (float coordinate : coordinates) {
    if (!std::isfinite(coordinate) || std::abs(coordinate) > 1.0e7f)
      return false;
  }
  return r.maxX > r.minX && r.maxY > r.minY && r.topZ > r.bottomZ
      && std::isfinite(r.edgeFeather)
      && r.edgeFeather >= 0.0f && r.edgeFeather <= 0.5f;
}

bool WeatherRuntime::validTime(float seconds) noexcept {
  return std::isfinite(seconds) && seconds >= 0.0f
      && seconds <= kMaxTimeSeconds;
}

SnowWeatherParams WeatherRuntime::normalized(SnowWeatherParams p) noexcept {
  p.windAngle = float(normalizedAngle(double(p.windAngle)));
  // Casting a double below 360 to float can round to 360.
  if (p.windAngle >= 360.0f)
    p.windAngle = 0.0f;
  return p;
}

SnowWeatherParams WeatherRuntime::interpolate(
    const SnowWeatherParams& from, const SnowWeatherParams& to,
    double fraction) noexcept {
  fraction = std::clamp(fraction, 0.0, 1.0);
  SnowWeatherParams out;
  out.snowfall = mix(from.snowfall, to.snowfall, fraction);
  out.windAngle = float(normalizedAngle(double(from.windAngle)
    + angleDelta(from.windAngle, to.windAngle) * fraction));
  if (out.windAngle >= 360.0f)
    out.windAngle = 0.0f;
  out.windSpeed = mix(from.windSpeed, to.windSpeed, fraction);
  out.gust = mix(from.gust, to.gust, fraction);
  out.haze = mix(from.haze, to.haze, fraction);
  out.lowSnow = mix(from.lowSnow, to.lowSnow, fraction);
  return out;
}

void WeatherRuntime::integrateWind(const SnowWeatherParams& from,
    const SnowWeatherParams& to, double duration,
    double& x, double& y) noexcept {
  // Exact integral for linearly varying speed and shortest-arc angle. This
  // does not depend on render sampling frequency, unlike Euler integration.
  const double delta = angleDelta(from.windAngle, to.windAngle)
    * kDegreesToRadians;
  const double halfAngle = 0.5 * delta;
  const double middleAngle = double(from.windAngle) * kDegreesToRadians
    + halfAngle;
  double sinc;
  double firstMoment;
  if (std::abs(halfAngle) < 1.0e-3) {
    const double h2 = halfAngle * halfAngle;
    sinc = 1.0 - h2 / 6.0 + h2 * h2 / 120.0;
    firstMoment = halfAngle * (1.0 / 6.0 - h2 / 60.0
      + h2 * h2 / 1680.0);
  } else {
    sinc = std::sin(halfAngle) / halfAngle;
    firstMoment = (std::sin(halfAngle) - halfAngle * std::cos(halfAngle))
      / (2.0 * halfAngle * halfAngle);
  }
  const double along = 0.5 * (double(from.windSpeed) + double(to.windSpeed))
    * sinc;
  const double across = (double(to.windSpeed) - double(from.windSpeed))
    * firstMoment;
  const double c = std::cos(middleAngle);
  const double s = std::sin(middleAngle);
  x += duration * (along * c - across * s);
  y += duration * (along * s + across * c);
}

bool WeatherRuntime::matchesLocked(int32_t id) const noexcept {
  return id > 0 && id == m_id;
}

void WeatherRuntime::changedLocked() noexcept {
  // Revision is diagnostic/config identity, not GPU completion or object life.
  ++m_revision;
  if (m_revision == 0)
    ++m_revision;
}

bool WeatherRuntime::changeHistoryLocked() noexcept {
  if (m_historyRevision == std::numeric_limits<uint64_t>::max())
    return false;
  ++m_historyRevision;
  return true;
}

void WeatherRuntime::stopTimelineForEditingLocked() noexcept {
  if (!m_timeline)
    return;
  m_timeline = false;
  m_transitionFrom = m_params;
  m_transitionTo = m_params;
  m_transitionDuration = 0.0;
}

SnowWeatherParams WeatherRuntime::evaluateLocked(double time) const noexcept {
  if (m_timeline && m_keyframeCount) {
    for (uint32_t i = 1; i < m_keyframeCount; ++i) {
      if (time < m_keyframes[i].time) {
        const Keyframe& from = m_keyframes[i - 1];
        const Keyframe& to = m_keyframes[i];
        return interpolate(from.params, to.params,
          (time - from.time) / (to.time - from.time));
      }
    }
    return m_keyframes[m_keyframeCount - 1].params;
  }
  if (m_transitionDuration > 0.0) {
    return interpolate(m_transitionFrom, m_transitionTo,
      (time - m_transitionStart) / m_transitionDuration);
  }
  return m_transitionTo;
}

void WeatherRuntime::integrateLocked(double from, double to) noexcept {
  // At most 64 timeline boundaries (or one manual transition) are visited.
  double cursor = from;
  while (cursor < to) {
    double next = to;
    if (m_timeline) {
      for (uint32_t i = 1; i < m_keyframeCount; ++i) {
        if (m_keyframes[i].time > cursor) {
          next = std::min(next, m_keyframes[i].time);
          break;
        }
      }
    } else {
      const double finish = m_transitionStart + m_transitionDuration;
      if (finish > cursor)
        next = std::min(next, finish);
    }
    integrateWind(evaluateLocked(cursor), evaluateLocked(next),
      next - cursor, m_windX, m_windY);
    cursor = next;
  }
}

void WeatherRuntime::advanceLocked() noexcept {
  const double now = m_clock();
  // A faulty/backward injected clock neither poisons state nor adds time twice.
  if (!std::isfinite(now) || now < m_lastClock)
    return;
  const double elapsed = now - m_lastClock;
  m_lastClock = now;
  if (!m_id || !m_enabled || m_paused || elapsed <= 0.0)
    return;
  const double next = std::min(double(kMaxTimeSeconds), m_timeSeconds + elapsed);
  integrateLocked(m_timeSeconds, next);
  m_timeSeconds = next;
  m_params = evaluateLocked(next);
}

void WeatherRuntime::seekLocked(double seconds) noexcept {
  m_timeSeconds = seconds;
  m_windX = 0.0;
  m_windY = 0.0;
  m_params = evaluateLocked(seconds);
  changedLocked();
}

void WeatherRuntime::resetRecordLocked() noexcept {
  m_keyframeCount = 0;
  m_params = {};
  m_transitionFrom = {};
  m_transitionTo = {};
  m_region = {};
  m_timeSeconds = 0.0;
  m_transitionStart = 0.0;
  m_transitionDuration = 0.0;
  m_windX = 0.0;
  m_windY = 0.0;
  m_seed = 0;
  m_id = 0;
  m_enabled = false;
  m_enabledProbe.store(false, std::memory_order_release);
  m_paused = false;
  m_timeline = false;
}

int32_t WeatherRuntime::createSnow(uint32_t seed) {
  std::lock_guard lock(m_mutex);
  if (m_id || m_nextId > uint32_t(std::numeric_limits<int32_t>::max()))
    return 0;
  const double now = m_clock();
  if (!std::isfinite(now))
    return 0;
  if (!changeHistoryLocked())
    return 0;
  resetRecordLocked();
  m_lastClock = now;
  m_seed = seed;
  m_id = int32_t(m_nextId++);
  changedLocked();
  return m_id;
}

bool WeatherRuntime::destroy(int32_t id) {
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id))
    return false;
  // Exhaustion still invalidates the instance; future create/play/seek fail.
  changeHistoryLocked();
  resetRecordLocked();
  changedLocked();
  return true;
}

bool WeatherRuntime::isAlive(int32_t id) {
  std::lock_guard lock(m_mutex);
  return matchesLocked(id);
}

bool WeatherRuntime::setRegion(int32_t id, const WeatherRegion& region) {
  if (!validRegion(region))
    return false;
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id))
    return false;
  m_region = region;
  changedLocked();
  return true;
}

bool WeatherRuntime::setEnabled(int32_t id, bool enabled) {
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id))
    return false;
  advanceLocked();
  if (m_enabled != enabled) {
    m_enabled = enabled;
    m_enabledProbe.store(enabled, std::memory_order_release);
    changedLocked();
  }
  return true;
}

bool WeatherRuntime::setTarget(int32_t id, const SnowWeatherParams& params,
    float transitionSeconds) {
  if (!validParams(params) || !std::isfinite(transitionSeconds)
      || transitionSeconds < 0.0f || transitionSeconds > 3600.0f)
    return false;
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id))
    return false;
  advanceLocked();
  m_timeline = false;
  m_transitionFrom = m_params;
  m_transitionTo = normalized(params);
  m_transitionStart = m_timeSeconds;
  m_transitionDuration = transitionSeconds;
  m_params = evaluateLocked(m_timeSeconds);
  changedLocked();
  return true;
}

bool WeatherRuntime::clearKeyframes(int32_t id) {
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id) || (m_timeline && (m_enabled || m_paused)))
    return false;
  stopTimelineForEditingLocked();
  m_keyframeCount = 0;
  changedLocked();
  return true;
}

bool WeatherRuntime::addKeyframe(int32_t id, float timeSeconds,
    const SnowWeatherParams& params) {
  if (!validTime(timeSeconds) || !validParams(params))
    return false;
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id) || (m_timeline && (m_enabled || m_paused))
      || m_keyframeCount == kMaxKeyframes
      || (!m_keyframeCount && timeSeconds != 0.0f)
      || (m_keyframeCount
        && double(timeSeconds) <= m_keyframes[m_keyframeCount - 1].time))
    return false;
  stopTimelineForEditingLocked();
  m_keyframes[m_keyframeCount++] = {double(timeSeconds), normalized(params)};
  changedLocked();
  return true;
}

bool WeatherRuntime::playTimeline(int32_t id, float startSeconds) {
  if (!validTime(startSeconds))
    return false;
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id) || !m_keyframeCount)
    return false;
  if (!changeHistoryLocked())
    return false;
  advanceLocked();
  m_timeline = true;
  m_enabled = true;
  m_enabledProbe.store(true, std::memory_order_release);
  m_paused = false;
  seekLocked(startSeconds);
  return true;
}

bool WeatherRuntime::setPaused(int32_t id, bool paused) {
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id))
    return false;
  advanceLocked();
  if (m_paused != paused) {
    m_paused = paused;
    changedLocked();
  }
  return true;
}

bool WeatherRuntime::setTime(int32_t id, float seconds) {
  if (!validTime(seconds))
    return false;
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id))
    return false;
  if (!changeHistoryLocked())
    return false;
  advanceLocked();
  if (!m_timeline) {
    // Manual seek holds the current appearance instead of rewinding a former
    // target transition. A timeline seek evaluates its authored keyframes.
    m_transitionFrom = m_params;
    m_transitionTo = m_params;
    m_transitionDuration = 0.0;
  }
  seekLocked(seconds);
  return true;
}

bool WeatherRuntime::getTime(int32_t id, float& outSeconds) {
  std::lock_guard lock(m_mutex);
  if (!matchesLocked(id))
    return false;
  advanceLocked();
  outSeconds = float(m_timeSeconds);
  return true;
}

WeatherFrameSnapshot WeatherRuntime::snapshot(uint64_t frameSerial,
    uint64_t mapEpoch, uint64_t deviceEpoch) {
  std::lock_guard lock(m_mutex);
  advanceLocked();
  WeatherFrameSnapshot out;
  out.params = m_params;
  out.region = m_region;
  out.frameSerial = frameSerial;
  out.mapEpoch = mapEpoch;
  out.deviceEpoch = deviceEpoch;
  out.revision = m_revision;
  out.historyRevision = m_historyRevision;
  out.timeSeconds = m_timeSeconds;
  out.windDisplacementX = m_windX;
  out.windDisplacementY = m_windY;
  out.seed = m_seed;
  out.id = m_id;
  out.enabled = m_enabled;
  out.paused = m_paused;
  return out;
}

void WeatherRuntime::resetAuthorState() {
  std::lock_guard lock(m_mutex);
  // At exhaustion the empty record remains fail-closed and create is refused.
  changeHistoryLocked();
  resetRecordLocked();
  changedLocked();
}

} // namespace dxvk::war3::render
