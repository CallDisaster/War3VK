#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace dxvk::war3::render {

struct SnowWeatherParams {
  float snowfall = 0.0f;
  float windAngle = 0.0f; // Degrees: +X=0, +Y=90, Z-up.
  float windSpeed = 0.0f; // Warcraft world units per second.
  float gust = 0.0f;
  float haze = 0.0f;
  float lowSnow = 0.0f;
};

struct WeatherRegion {
  float minX = -4096.0f;
  float minY = -4096.0f;
  float maxX = 4096.0f;
  float maxY = 4096.0f;
  float bottomZ = 0.0f;
  float topZ = 2048.0f;
  float edgeFeather = 0.1f;
};

// CPU-owned, fixed-size input for one render frame. A queued GPU command owns
// its copy; it must never re-read the live author state while executing.
struct WeatherFrameSnapshot {
  SnowWeatherParams params = {};
  WeatherRegion region = {};
  uint64_t frameSerial = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint64_t revision = 0;
  uint64_t historyRevision = 0; // Discontinuous weather history only.
  double timeSeconds = 0.0;
  double windDisplacementX = 0.0;
  double windDisplacementY = 0.0;
  uint32_t seed = 0;
  int32_t id = 0;
  bool enabled = false;
  bool paused = false;
};

class WeatherRuntime {
public:
  using Clock = double (*)() noexcept;
  static constexpr uint32_t kMaxKeyframes = 64;
  static constexpr float kMaxTimeSeconds = 86400.0f;
  static constexpr float kMaxWindSpeed = 2048.0f;

  // Null selects steady_clock. The injected clock uses monotonic seconds and
  // is sampled under this runtime's mutex; it must not call back into runtime.
  explicit WeatherRuntime(Clock clock = nullptr) noexcept;
  static WeatherRuntime& instance();
  // Scheduling hint only; snapshot remains the authoritative locked state.
  bool hasEnabled() const noexcept;

  int32_t createSnow(uint32_t seed);
  bool destroy(int32_t id);
  bool isAlive(int32_t id);
  bool setRegion(int32_t id, const WeatherRegion& region);
  bool setEnabled(int32_t id, bool enabled);
  bool setTarget(int32_t id, const SnowWeatherParams& params,
                 float transitionSeconds);
  bool clearKeyframes(int32_t id);
  bool addKeyframe(int32_t id, float timeSeconds,
                   const SnowWeatherParams& params);
  bool playTimeline(int32_t id, float startSeconds);
  bool setPaused(int32_t id, bool paused);
  bool setTime(int32_t id, float seconds);
  bool getTime(int32_t id, float& outSeconds);
  WeatherFrameSnapshot snapshot(uint64_t frameSerial,
                                 uint64_t mapEpoch,
                                 uint64_t deviceEpoch);
  void resetAuthorState();

private:
  struct Keyframe {
    double time = 0.0;
    SnowWeatherParams params = {};
  };

  static double steadySeconds() noexcept;
  static bool validParams(const SnowWeatherParams& params) noexcept;
  static bool validRegion(const WeatherRegion& region) noexcept;
  static bool validTime(float seconds) noexcept;
  static SnowWeatherParams normalized(SnowWeatherParams params) noexcept;
  static SnowWeatherParams interpolate(const SnowWeatherParams& from,
                                        const SnowWeatherParams& to,
                                        double fraction) noexcept;
  static void integrateWind(const SnowWeatherParams& from,
                             const SnowWeatherParams& to,
                             double duration, double& x, double& y) noexcept;

  bool matchesLocked(int32_t id) const noexcept;
  void advanceLocked() noexcept;
  SnowWeatherParams evaluateLocked(double time) const noexcept;
  void integrateLocked(double from, double to) noexcept;
  void seekLocked(double seconds) noexcept;
  void resetRecordLocked() noexcept;
  void changedLocked() noexcept;
  bool changeHistoryLocked() noexcept;
  void stopTimelineForEditingLocked() noexcept;

  mutable std::mutex m_mutex;
  std::atomic<bool> m_enabledProbe{false};
  Clock m_clock;
  std::array<Keyframe, kMaxKeyframes> m_keyframes = {};
  uint32_t m_keyframeCount = 0;
  SnowWeatherParams m_params = {};
  SnowWeatherParams m_transitionFrom = {};
  SnowWeatherParams m_transitionTo = {};
  WeatherRegion m_region = {};
  double m_timeSeconds = 0.0;
  double m_lastClock = 0.0;
  double m_transitionStart = 0.0;
  double m_transitionDuration = 0.0;
  double m_windX = 0.0;
  double m_windY = 0.0;
  uint64_t m_revision = 0;
  uint64_t m_historyRevision = 0; // Never cleared or wrapped by map reset.
  uint32_t m_nextId = 1; // Not reset: old-map commands cannot address a new id.
  uint32_t m_seed = 0;
  int32_t m_id = 0;
  bool m_enabled = false;
  bool m_paused = false;
  bool m_timeline = false;
};

} // namespace dxvk::war3::render
