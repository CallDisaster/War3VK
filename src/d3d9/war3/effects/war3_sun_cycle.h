#pragma once

// The day/night sun of a frame: from the author's settings and the game's
// time of day to the light the frame is lit with.
//
// The shadow pass (d3d9_war3_shadow.cpp) resolves it for its cascades and
// feeds the direction and colour back into the settings, where the device
// reads them for the fixed-function world lighting (light 0). In proxy mode
// the shadow pass runs in the x64 host, the fixed-function state is composed
// in the x86 process, and nothing comes back from the host: the data layer
// resolves the same sun with this class. The code is the shadow pass's own,
// moved here without change (phase 5.1-c).
//
// Stateful: TIME_OF_DAY arrives in 100 ms steps and is smoothed across
// frames. One instance per consumer, on its render thread. Evaluate is the
// stateless rest - the light at a given time of day - which the sun dial of
// the Ctrl+F1 panel draws the whole day with.

#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_settings.h"
#include "../core/war3_internal_test_config.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace dxvk {

class War3SunCycle {
public:
  struct Result {
    // The smoothed time of day, 0..1, and the same in hours.
    float time01 = 0.0f;
    float renderTimeHours = 0.0f;
    float realAltitudeRad = 0.0f;
    float shadowAltRad = 0.0f;
    float transitionRad = 0.0f;
    // The light of the frame: the sun by day, the moon by night; the
    // author's own values where a cycle is switched off.
    Vector4 direction = Vector4(0.0f);
    Vector4 color = Vector4(0.0f);
    float shadowStrength = 0.0f;
    // Each channel's share of that strength (1 1 1: a grey shadow). The
    // shadow takes the beam and leaves the sky's light, so a warm low sun
    // casts a shadow with the sky's blue in it (ShadowLook).
    Vector4 shadowTint = Vector4(1.0f, 1.0f, 1.0f, 0.0f);
    // Whether the sun (not the moon) lights the frame, and the altitude its
    // shadow is cast from: tan of it is the height a shadow's length divides.
    bool day = true;
    float castAltitudeRad = 0.0f;
    // The direction the world is shaded from (light 0): the light's bearing
    // at the elevation of the look (ShadeAltitudeRad), the same whatever
    // length the shadow is drawn with.
    Vector4 lightDirection = Vector4(0.0f);
    // The elevation of the light of the frame (the sun's or the moon's own,
    // not the shadow's), which its colour and brightness follow.
    float lightElevationRad = 0.0f;
    // Where the dial draws the light (sun or moon) seen from above, in
    // radii of the dial, x east and y north: the point of the player's path,
    // or the light's bearing (on the rim, or inside at the cos of the cast
    // altitude with the real shadow length).
    float dialX = 0.0f;
    float dialY = 0.0f;
  };

  // realGameTime: War3RenderState::GetGameTime(), hours, outside 0..24 when
  // the game's clock cannot be read yet.
  Result Resolve(const War3RenderSettings* settings, float realGameTime,
                 std::chrono::steady_clock::time_point now) {
    if (!m_timeInitialized) {
      m_timeInitialized = true;
      m_timeStart = now;
      m_timeSmoothingInitialized = false;
      m_time01LastUpdate = now;
      m_time01LastRawSample = now;
      m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);
    }

    // 1. 计算当前时间（0..1）
    auto wrap01 = [](float v) {
      v = std::fmod(v, 1.0f);
      if (v < 0.0f)
        v += 1.0f;
      return v;
    };
    auto diffSigned01 = [&](float a, float b) {
      // 返回 a-b 的最短差值，范围约 [-0.5, 0.5)
      float d = a - b;
      d -= std::floor(d + 0.5f);
      return d;
    };
    auto diffForward01 = [&](float a, float b) {
      // 返回从 b 走到 a 的“正向”差值，范围 [0, 1)
      float d = a - b;
      d -= std::floor(d);
      return d;
    };

    float rawTime01 = 0.0f;
    const War3DayNightSettings &dayNightSettings = settings->dayNight;

    static bool s_hasValidGameTime = false;
    const bool hasRealGameTime =
        realGameTime >= 0.0f && realGameTime <= 24.0f;
    const War3LightingClockMode clockMode = dayNightSettings.clockMode;
    if (clockMode != War3LightingClockMode::GameTime) {
      rawTime01 = wrap01(dayNightSettings.renderTimeHours / 24.0f);
      m_timeSmoothingInitialized = false;
    } else if (hasRealGameTime) {
      // Game Time 0=Midnight, 6=Sunrise, 18=Sunset, 24=Midnight.
      if (!s_hasValidGameTime) {
        s_hasValidGameTime = true;
        WAR3_RENDER_LOG("DXVK War3Shadow: Switched to Real Game Time! t=%f\n",
                        realGameTime);
      }
      rawTime01 = wrap01(realGameTime / 24.0f);
    } else {
      // Follow-game mode retains the historical startup fallback until the
      // native TIME_OF_DAY source becomes readable.
      static int s_failLog = 0;
      if (s_failLog++ < 10) {
        WAR3_RENDER_LOG("DXVK War3Shadow: Fallback used. RealGameTime=%.4f "
                        "(Valid Range: 0-24)\n",
                        realGameTime);
      }
      const float elapsed =
          std::chrono::duration<float>(now - m_timeStart).count();
      rawTime01 = wrap01(elapsed / m_dayLengthSeconds + m_startTime01);
    }

    const War3ShadowSettings &shadowSettings = settings->shadows;
    if (shadowSettings.lockSun) {
      rawTime01 = std::clamp(shadowSettings.lockSunTime, 0.0f, 1.0f);
      // 锁太阳时强制重置平滑状态，避免解锁后“追赶”造成突跳
      m_timeSmoothingInitialized = false;
    }

    // 说明：TIME_OF_DAY 通常是 100ms 台阶更新；这里把它平滑成逐帧连续值，
    // 以消除日夜切换/太阳移动时的高频跳变（尤其在 CSM 与阴影接收器上很明显）。
    float time01 = rawTime01;
    if (clockMode == War3LightingClockMode::GameTime && hasRealGameTime &&
        !shadowSettings.lockSun) {
      if (!m_timeSmoothingInitialized) {
        m_timeSmoothingInitialized = true;
        m_time01Smoothed = rawTime01;
        m_time01LastRaw = rawTime01;
        m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);
        m_time01LastUpdate = now;
        m_time01LastRawSample = now;
      } else {
        const float dt =
            std::chrono::duration<float>(now - m_time01LastUpdate).count();
        m_time01LastUpdate = now;
        if (dt > 0.0f && dt < 0.25f) {
          m_time01Smoothed = wrap01(m_time01Smoothed + m_time01Speed * dt);
        }

        const float rawDelta = diffForward01(rawTime01, m_time01LastRaw);
        if (rawDelta > 1e-5f) {
          const float rawDt =
              std::chrono::duration<float>(now - m_time01LastRawSample).count();
          m_time01LastRawSample = now;
          m_time01LastRaw = rawTime01;

          // 时间被脚本/触发器重置时可能会出现大跳变；此时直接对齐，避免“极速追赶”
          if constexpr (dxvk::war3::internal::kShadowTimeAdaptiveSpeedEnabled) {
            if (rawDt > 0.0f && rawDt < 2.0f && rawDelta < 0.25f) {
              const float newSpeed = rawDelta / rawDt;
              // 合理上限：允许加速（比如加速昼夜），但避免异常值把系统带飞
              const float maxSpeed = 1.0f / 5.0f; // 5 秒跑完一天已经非常夸张
              m_time01Speed = std::clamp(newSpeed, 0.0f, maxSpeed);
            } else {
              m_time01Smoothed = rawTime01;
              m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);
            }
          } else {
            if (rawDelta >= 0.25f || rawDt >= 2.0f)
              m_time01Smoothed = rawTime01;
            m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);
          }
        }

        const float err = diffSigned01(rawTime01, m_time01Smoothed);
        const float absErr = std::abs(err);
        if (absErr > 0.25f) {
          // 误差过大（例如瞬移时间/重置时间），直接对齐
          m_time01Smoothed = rawTime01;
        } else {
          // 小误差回正：限制每帧回正幅度，避免可见跳动
          constexpr float kPull = 0.05f;
          constexpr float kMaxPullPerFrame = 0.02f;
          const float pull =
              std::clamp(err * kPull, -kMaxPullPerFrame, kMaxPullPerFrame);
          m_time01Smoothed = wrap01(m_time01Smoothed + pull);
        }
      }
      time01 = m_time01Smoothed;
    }

    return Evaluate(*settings, time01);
  }

  // The light at a time of day (0..1), as Resolve gives it once the time is
  // known.
  static Result Evaluate(const War3RenderSettings& settingsRef, float time01) {
    const War3RenderSettings* settings = &settingsRef;
    const War3DayNightSettings &dayNightSettings = settings->dayNight;
    const War3ShadowSettings &shadowSettings = settings->shadows;
    const War3SunStyleSettings &style = settings->sunStyle;
    auto wrap01 = [](float v) {
      v = std::fmod(v, 1.0f);
      if (v < 0.0f)
        v += 1.0f;
      return v;
    };

    // 2. 计算太阳的“真实”轨迹 (Real Trajectory)
    // 假设：X=东, -X=西, Y=北, -Y=南, Z=上
    // 方位角(Azimuth): 从东(0) -> 南(PI/2) -> 西(PI)
    // 让太阳稍微偏南一点，保证阴影更有立体感
    // Time 0.25(Sunrise) -> Azimuth 0 (East)
    // Time 0.50(Noon)    -> Azimuth PI/2 (South)
    // Time 0.75(Sunset)  -> Azimuth PI (West)

    // 真实高度角 (Real Altitude): 正弦波模拟
    // T=0.25(Sunrise) -> Alt=0, T=0.5(Noon) -> Alt=90
    // sin argument: (time01 - 0.25) * 2 * PI -> 0 ~ PI (for 0.25~0.75 range)
    // (time01 - 0.25) * 2PI 涵盖整个 0..1 周期 (对应 0..2PI)
    float sunAnglePhase = (time01 - 0.25f) * 2.0f * 3.14159265f;
    float realAltitudeRad = std::sin(sunAnglePhase) * (3.14159265f / 2.0f);

    // 3. 计算受控的“投影”高度角（全时段平滑变化）
    float minAltRad = kMinSunAltitudeDeg * (3.14159265f / 180.0f);
    float maxAltRad = kMaxSunAltitudeDeg * (3.14159265f / 180.0f);
    const float halfPi = 3.14159265f / 2.0f;

    auto smoothstep01 = [](float v) {
      v = std::min(1.0f, std::max(0.0f, v));
      return v * v * (3.0f - 2.0f * v);
    };

    auto calcLinearBell01 = [&](float phase01) {
      float p = std::min(1.0f, std::max(0.0f, phase01));
      float tri = 1.0f - std::abs(p * 2.0f - 1.0f);
      return std::min(1.0f, std::max(0.0f, tri));
    };

    float lengthScale = std::clamp(shadowSettings.shadowLengthScale, 0.1f, 2.0f);
    float maxLenScale =
        std::clamp(shadowSettings.shadowMaxLengthScale, 0.1f, 2.0f);

    auto calcShadowAltitude01 = [&](float altitude01) {
      float t = smoothstep01(altitude01);
      float minAltForLength = minAltRad;
      if (maxLenScale > 0.0f) {
        float baseTan = std::tan(std::max(0.001f, minAltRad));
        float scaledMinAlt = std::atan(baseTan * (lengthScale / maxLenScale));
        minAltForLength = std::min(scaledMinAlt, maxAltRad - 0.01f);
      }
      float baseAlt = minAltForLength + (maxAltRad - minAltForLength) * t;
      float tanBase = std::tan(std::max(0.001f, baseAlt));
      return std::atan(tanBase / lengthScale);
    };

    float dayPhase = (time01 - 0.25f) / 0.5f;
    float dayAlt01 = 0.0f;
    if (shadowSettings.altitudeMode == War3ShadowAltitudeMode::TimeLinear) {
      dayAlt01 = calcLinearBell01(dayPhase);
    } else {
      dayAlt01 = std::clamp(realAltitudeRad / halfPi, 0.0f, 1.0f);
    }
    // The real shadow length (the player's choice): the shadow is cast from
    // the light's own altitude on a path that peaks at the noon altitude. A
    // floor of a few degrees keeps the cascades finite; the shadow has faded
    // out by then (ShadowLook).
    const bool realLength = style.realShadowLength;
    const float noonAltRad = std::clamp(std::isfinite(style.noonAltitudeDeg)
        ? style.noonAltitudeDeg : 60.0f, 10.0f, 90.0f) * (3.14159265f / 180.0f);
    const float realFloorRad = kRealShadowFloorDeg * (3.14159265f / 180.0f);
    auto realShadowAltitude = [&](float pathAltitudeRad) {
      return std::max(realFloorRad, pathAltitudeRad * (noonAltRad / halfPi));
    };
    float shadowAltRad = realLength ? realShadowAltitude(std::max(0.0f, realAltitudeRad))
                                    : calcShadowAltitude01(dayAlt01);

    // 5. 计算光源方向向量 (Z-Up)
    // 修正：War3 中 +X=East, +Y=North.
    // 想要太阳从东(X) -> 南(-Y) -> 西(-X)
    // Angle 0 = East (+X).
    // Angle -90 (PI/2) = South (-Y).
    // Angle -180 (PI) = West (-X).
    // 所以我们让 azimuth 从 0 变到 -PI

    // [Fix] Optional time quantization for diagnosis. Default stays smooth:
    // quantized sun steps can look like periodic shadow pops when visual QA is
    // focused on flicker.
    const float kSunQuantStep = 0.0002f;
    float quantizedTime01 = time01;
    if constexpr (dxvk::war3::internal::kShadowSunTimeQuantizationEnabled)
      quantizedTime01 = std::round(time01 / kSunQuantStep) * kSunQuantStep;

    float rawDayPhase = (quantizedTime01 - 0.25f) / 0.5f;
    float sunYaw = -rawDayPhase * 3.14159265f;

    // The player's path (the dial): the bearing follows the path point;
    // with the real shadow length the altitude is the path's own (a point
    // r from the centre is acos(r) high). Without it only the bearing
    // changes. pathNorth -1 is the default bearing.
    const bool customPath = style.pathSet;
    const float pathNorth = PathNorth(style);
    struct PathPoint { float x, y, cosYaw, sinYaw, altitudeRad; };
    auto pathPoint = [&](float phaseRad) {
      PathPoint p;
      p.x = std::cos(phaseRad);
      p.y = pathNorth * std::sin(phaseRad);
      const float r = std::sqrt(p.x * p.x + p.y * p.y);
      p.cosYaw = r > 1e-6f ? p.x / r : 0.0f;
      p.sinYaw = r > 1e-6f ? p.y / r : (pathNorth >= 0.0f ? 1.0f : -1.0f);
      p.altitudeRad = std::acos(std::min(1.0f, r));
      return p;
    };
    const PathPoint sunPath = pathPoint(rawDayPhase * 3.14159265f);
    if (customPath && realLength && realAltitudeRad > 0.0f)
      shadowAltRad = std::max(realFloorRad, sunPath.altitudeRad);

    // 球坐标转笛卡尔 (Z-Up)
    // X = cos(Alt) * cos(Yaw)
    // Y = cos(Alt) * sin(Yaw)
    // Z = sin(Alt)
    float cosAlt = std::cos(shadowAltRad);
    float sinAlt = std::sin(shadowAltRad);
    float cosYaw = customPath ? sunPath.cosYaw : std::cos(sunYaw);
    float sinYaw = customPath ? sunPath.sinYaw : std::sin(sunYaw);

    Vector4 sunPos;
    sunPos.x = cosAlt * cosYaw;
    sunPos.y = cosAlt * sinYaw;
    sunPos.z = sinAlt;

    // LightDir = -SunPos
    const Vector4 sunDirection = Vector4(-sunPos.x, -sunPos.y, -sunPos.z, 0.0f);

    // 6. 光源逻辑 (Sun vs Moon)
    Vector4 finalLightDir;
    Vector4 finalLightColor;
    float finalShadowStrength = 0.0f;

    // The band of the day/night events (UpdatePhase), on the day's arc.
    const float kTransitionDeg = 24.0f;
    const float kTransitionRad = kTransitionDeg * (3.14159265f / 180.0f);
    const float kDegrees = 180.0f / 3.14159265f;

    // ======== 白天 (Sun) 结果 ========
    // The sun's own elevation (the path's, or the noon altitude's share of
    // the day's arc) gives its colour and brightness: the look of the hour is
    // the same whatever length the shadow is drawn with.
    Vector4 dayLightDir = sunDirection;
    const float sunLookRad = realAltitudeRad > 0.0f ? LightElevationRad(style, dayPhase) : 0.0f;
    const float hoursOfDay = wrap01(time01) * 24.0f;
    const Vector4 dayLight = SunLight(sunLookRad * kDegrees, smoothstep01((hoursOfDay - 10.5f) / 3.0f));
    const float dayLightFactor = dayLight.w;
    Vector4 dayLightColor = Vector4(dayLight.x, dayLight.y, dayLight.z, 1.0f);

    // ======== 黑夜 (Moon) 结果 ========
    float moonRealAlt = std::max(0.0f, -realAltitudeRad);
    float nightPhase = 0.0f;
    if (time01 < 0.25f) {
      nightPhase = (time01 + 0.25f) / 0.5f;
    } else {
      nightPhase = (time01 - 0.75f) / 0.5f;
    }
    float moonAlt01 = 0.0f;
    if (shadowSettings.altitudeMode == War3ShadowAltitudeMode::TimeLinear) {
      moonAlt01 = calcLinearBell01(nightPhase);
    } else {
      moonAlt01 = std::clamp(moonRealAlt / halfPi, 0.0f, 1.0f);
    }
    float moonShadowAlt = realLength ? realShadowAltitude(moonRealAlt) : calcShadowAltitude01(moonAlt01);
    // The moon runs the player's path by night: east at 18:00, the path's
    // noon point at midnight, west at 6:00.
    const PathPoint moonPath = pathPoint(nightPhase * 3.14159265f);
    if (customPath && realLength && moonRealAlt > 0.0f)
      moonShadowAlt = std::max(realFloorRad, moonPath.altitudeRad);
    float moonYaw = sunYaw + 3.14159265f;
    float cAlt = std::cos(moonShadowAlt);
    float sAlt = std::sin(moonShadowAlt);
    float cYaw = customPath ? moonPath.cosYaw : std::cos(moonYaw);
    float sYaw = customPath ? moonPath.sinYaw : std::sin(moonYaw);
    Vector4 moonPos;
    moonPos.x = cAlt * cYaw;
    moonPos.y = cAlt * sYaw;
    moonPos.z = sAlt;
    Vector4 nightLightDir = Vector4(-moonPos.x, -moonPos.y, -moonPos.z, 0.0f);

    // The moon's own elevation, as the sun's by day.
    const float moonLookRad = moonRealAlt > 0.0f ? LightElevationRad(style, nightPhase) : 0.0f;
    const Vector4 nightLight = MoonLight(moonLookRad * kDegrees);
    const float nightLightFactor = nightLight.w;
    Vector4 nightLightColor = Vector4(nightLight.x, nightLight.y, nightLight.z, 1.0f);
    const float nightShadowScale =
        std::clamp(shadowSettings.nightShadowScale, 0.0f, 1.0f);

    // ======== 主光源选择（解决“日夜切换阴影方向瞬移”） ========
    // 说明：
    // -
    // 日落/日出时，太阳与月亮方向接近相反，直接对方向做线性混合会出现“向量相互抵消”，
    //   进而产生极端角速度：看起来像太阳/月亮在一秒内快速乱跑。
    // - 我们渲染端只有一套方向光阴影（CSM），因此在过渡带内应当“选一个主光源”，
    //   并依赖地平线附近的阴影强度淡出使切换不可见。
    const bool useDayLight = realAltitudeRad >= 0.0f;
    finalLightDir = useDayLight ? dayLightDir : nightLightDir;
    finalLightColor = useDayLight ? dayLightColor : nightLightColor;

    if (dayNightSettings.timeColorGradingEnabled &&
        dayNightSettings.customColorTemperatureProfile) {
      const auto validKelvin = [](float value, float fallback) {
        return std::isfinite(value)
            ? std::clamp(value, 1000.0f, 20000.0f)
            : fallback;
      };
      const float keyKelvin[4] = {
          validKelvin(dayNightSettings.midnightKelvin, 9000.0f),
          validKelvin(dayNightSettings.dawnKelvin, 2500.0f),
          validKelvin(dayNightSettings.noonKelvin, 6500.0f),
          validKelvin(dayNightSettings.duskKelvin, 2500.0f),
      };
      const float hours = wrap01(time01) * 24.0f;
      const uint32_t segment =
          std::min(3u, static_cast<uint32_t>(hours / 6.0f));
      const uint32_t next = (segment + 1u) & 3u;
      const float segmentT = smoothstep01(
          (hours - static_cast<float>(segment) * 6.0f) / 6.0f);
      const float kelvin = keyKelvin[segment] +
          (keyKelvin[next] - keyKelvin[segment]) * segmentT;
      const float brightness =
          useDayLight ? dayLightFactor : nightLightFactor;
      finalLightColor = kelvinToRgb(kelvin) * brightness;
    }
    // The player's colour curve: the colour and the brightness both.
    if (dayNightSettings.timeColorGradingEnabled && style.customTint)
      finalLightColor = TintAt(style, wrap01(time01) * 24.0f);

    // Manual ownership is final. When a cycle is disabled the pass neither
    // rewrites the corresponding global sun value nor substitutes a local CSM
    // value, so a JASS setting remains stable on every subsequent frame.
    if (!dayNightSettings.timeColorGradingEnabled)
      finalLightColor = settings->sun.color;

    // The shadow is as dark as the light of the frame makes it, and the world
    // is shaded from the light's bearing at the elevation of the look.
    const float lookRad = useDayLight ? sunLookRad : moonLookRad;
    const Vector4 shadowLook = ShadowLook(finalLightColor, lookRad,
        (useDayLight ? sunLookRad : -moonLookRad) * kDegrees);
    finalShadowStrength = shadowLook.w * (useDayLight ? 1.0f : nightShadowScale);
    Vector4 shadowTint = Vector4(shadowLook.x, shadowLook.y, shadowLook.z, 0.0f);
    const float shadeAltRad = ShadeAltitudeRad(lookRad);
    const float shadeFlat = std::cos(shadeAltRad);
    Vector4 shadeLightDir = useDayLight
        ? Vector4(-shadeFlat * cosYaw, -shadeFlat * sinYaw, -std::sin(shadeAltRad), 0.0f)
        : Vector4(-shadeFlat * cYaw, -shadeFlat * sYaw, -std::sin(shadeAltRad), 0.0f);
    if (!dayNightSettings.celestialMotionEnabled) {
      finalLightDir = settings->sun.direction;
      shadeLightDir = settings->sun.direction;
      finalShadowStrength = settings->shadows.strength;
      shadowTint = Vector4(1.0f, 1.0f, 1.0f, 0.0f);
    }

    Result result;
    result.shadowTint = shadowTint;
    result.lightDirection = shadeLightDir;
    result.lightElevationRad = lookRad;
    result.time01 = time01;
    result.renderTimeHours = wrap01(time01) * 24.0f;
    result.realAltitudeRad = realAltitudeRad;
    result.shadowAltRad = shadowAltRad;
    result.transitionRad = kTransitionRad;
    result.direction = finalLightDir;
    result.color = finalLightColor;
    result.shadowStrength = finalShadowStrength;
    result.day = useDayLight;
    const float dirLength = std::sqrt(finalLightDir.x * finalLightDir.x +
        finalLightDir.y * finalLightDir.y + finalLightDir.z * finalLightDir.z);
    result.castAltitudeRad = dirLength > 0.0f
        ? std::asin(std::clamp(-finalLightDir.z / dirLength, -1.0f, 1.0f)) : 0.0f;
    if (customPath && dayNightSettings.celestialMotionEnabled) {
      result.dialX = useDayLight ? sunPath.x : moonPath.x;
      result.dialY = useDayLight ? sunPath.y : moonPath.y;
    } else {
      const float flat = std::sqrt(finalLightDir.x * finalLightDir.x + finalLightDir.y * finalLightDir.y);
      const float reach = realLength ? std::cos(result.castAltitudeRad) : 1.0f;
      result.dialX = flat > 1e-6f ? -finalLightDir.x / flat * reach : 0.0f;
      result.dialY = flat > 1e-6f ? -finalLightDir.y / flat * reach : 0.0f;
    }
    return result;
  }

  // The path's noon point, -1..1, kept off the centre: a path through the
  // zenith would turn the light half round in an instant at noon.
  static float PathNorth(const War3SunStyleSettings& style) {
    const float v = std::isfinite(style.pathNorth) ? std::clamp(style.pathNorth, -1.0f, 1.0f) : -0.5f;
    if (std::abs(v) >= kMinPathNorth)
      return v;
    return v > 0.0f ? kMinPathNorth : -kMinPathNorth;
  }

  // ---- The look of the hour -------------------------------------------------
  //
  // What the light looks like, apart from where its shadow falls: its colour
  // and brightness, the elevation the world is shaded from, how dark the
  // shadow is and the tint of the ambient, all against the light's own
  // elevation. Display values: the game lights in gamma space.

  // The elevation of the sun (by day) or the moon (by night) at `phase01` of
  // its half day, 0 at its rise and 1 at its set: the path's, or the noon
  // altitude's share of the arc.
  static float LightElevationRad(const War3SunStyleSettings& style, float phase01) {
    const float phase = std::clamp(phase01, 0.0f, 1.0f) * 3.14159265f;
    if (style.pathSet) {
      const float x = std::cos(phase), y = PathNorth(style) * std::sin(phase);
      return std::acos(std::min(1.0f, std::sqrt(x * x + y * y)));
    }
    const float noonDeg = std::isfinite(style.noonAltitudeDeg) ? style.noonAltitudeDeg : 60.0f;
    return std::clamp(noonDeg, 10.0f, 90.0f) * (3.14159265f / 180.0f) * std::sin(phase);
  }

  // The sun's elevation at a time of day in degrees, negative by the moon's
  // elevation while the moon lights the frame.
  static float SunElevationDeg(const War3SunStyleSettings& style, float time01) {
    time01 -= std::floor(time01);
    const float dayPhase = (time01 - 0.25f) / 0.5f;
    if (dayPhase >= 0.0f && dayPhase <= 1.0f)
      return LightElevationRad(style, dayPhase) * (180.0f / 3.14159265f);
    const float nightPhase = time01 < 0.25f ? (time01 + 0.25f) / 0.5f : (time01 - 0.75f) / 0.5f;
    return -LightElevationRad(style, nightPhase) * (180.0f / 3.14159265f);
  }

  // The share of the beam that flat ground takes: more than the sine of the
  // elevation, as a picture exposed for the hour shows it (a low sun still
  // lights the ground), and never nothing. The world is shaded from the
  // elevation with that sine.
  static float GroundShare(float elevationRad) {
    const float sine = std::sin(std::clamp(elevationRad, 0.0f, 3.14159265f * 0.5f));
    return std::max(kGroundShareFloor, std::pow(sine, kGroundShareGamma));
  }
  static float ShadeAltitudeRad(float elevationRad) {
    return std::asin(std::min(1.0f, GroundShare(elevationRad)));
  }

  struct LightKey { float deg, r, g, b, brightness; };

  // The light of a table at an elevation: colour times brightness, the
  // brightness in w.
  template<size_t N>
  static Vector4 LightAt(const LightKey (&keys)[N], float deg) {
    size_t i = 0;
    while (i + 2 < N && deg > keys[i + 1].deg)
      ++i;
    const LightKey& a = keys[i];
    const LightKey& b = keys[i + 1];
    const float t = std::clamp((deg - a.deg) / (b.deg - a.deg), 0.0f, 1.0f);
    const float k = a.brightness + (b.brightness - a.brightness) * t;
    return Vector4((a.r + (b.r - a.r) * t) * k, (a.g + (b.g - a.g) * t) * k, (a.b + (b.b - a.b) * t) * k, k);
  }

  // The sun at an elevation, from the morning's table (evening 0) to the
  // evening's (1).
  static Vector4 SunLight(float deg, float evening) {
    const Vector4 a = LightAt(kSunMorning, deg);
    const Vector4 b = LightAt(kSunEvening, deg);
    return a + (b - a) * std::clamp(evening, 0.0f, 1.0f);
  }
  static Vector4 MoonLight(float deg) { return LightAt(kMoon, deg); }

  // The shadow of a light at elevationRad, the sun at sunElevationDeg (under
  // the horizon for the moon's). It takes the beam off flat ground and
  // leaves the ambient, so its strength is the beam's share of that ground's
  // light: a low or dim light casts a faint shadow, the noon sun the 0.61
  // the pass was tuned to. Channel by channel the shares differ - the low
  // sun's red is nearly all beam, its blue nearly all sky - and that is the
  // shadow's colour: xyz is each channel's share over the strength in w.
  static Vector4 ShadowLook(const Vector4& light, float elevationRad, float sunElevationDeg) {
    const float share = GroundShare(elevationRad);
    const Vector4 sky = LightAt(kAmbient, sunElevationDeg) * kAmbientOnGround;
    const auto luminance = [](const Vector4& c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; };
    const auto taken = [share](float beam, float ambient) {
      beam = std::max(0.0f, beam) * share;
      return kShadowDepth * beam / (beam + std::max(ambient, 1e-3f));
    };
    const float strength = taken(luminance(light), luminance(sky));
    if (!(strength > 1e-5f))
      return Vector4(1.0f, 1.0f, 1.0f, 0.0f);
    return Vector4(taken(light.x, sky.x) / strength, taken(light.y, sky.y) / strength,
                   taken(light.z, sky.z) / strength, strength);
  }
  static float ShadowStrength(const Vector4& light, float elevationRad, float sunElevationDeg) {
    return ShadowLook(light, elevationRad, sunElevationDeg).w;
  }

  // The day/night blend of two ambient colours at dayFactor (0 night, 1 day),
  // kept above 85% of the day's luminance (War3SettingsFrameResolver).
  static Vector4 AmbientBlend(const Vector4& night, const Vector4& day, float dayFactor) {
    const auto luminance = [](const Vector4& c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; };
    Vector4 ambient = night * (1.0f - dayFactor) + day * dayFactor;
    const float floor = luminance(day) * 0.85f;
    const float now = luminance(ambient);
    if (now > 1e-4f && now < floor)
      ambient = ambient * (floor / now);
    ambient.w = 1.0f;
    return ambient;
  }

  // The ambient's tint at a time of day, a factor on the blend of the
  // author's two ambient colours: the sky's blue by day, the purple light
  // around the horizon, the blue hour after it. With the default colours the
  // product is the table's own colour.
  static Vector4 AmbientTint(const War3SunStyleSettings& style, float time01, float dayFactor) {
    const War3DayNightSettings defaults{};
    const Vector4 base = AmbientBlend(defaults.nightAmbient, defaults.dayAmbient, dayFactor);
    const Vector4 want = LightAt(kAmbient, SunElevationDeg(style, time01));
    return Vector4(want.x / base.x, want.y / base.y, want.z / base.z, 1.0f);
  }

  // The colour curve at an hour (0..24): the colour and the brightness of
  // the two keys around it. Between two keys of the day both ease from one
  // to the other. Beside a horizon key (sunrise, sunset) the brightness
  // changes quickly and the colour slowly: the light is already bright while
  // it is still the horizon's colour, the golden hour. The moon keeps its
  // brightness from half an hour after sunset until half an hour before
  // sunrise and its colour a quarter of an hour longer on either side, so
  // the light dims before it changes colour (no grey between the moon's blue
  // and the horizon's orange).
  static Vector4 TintAt(const War3SunStyleSettings& style, float hours) {
    constexpr size_t n = War3SunStyleSettings::KeyCount;
    if (!std::isfinite(hours))
      hours = 12.0f;
    hours = std::fmod(hours, 24.0f);
    if (hours < 0.0f)
      hours += 24.0f;
    size_t i = n - 1u;
    for (size_t k = 0; k < n; ++k)
      if (hours >= War3SunStyleSettings::KeyHours[k])
        i = k;
    const size_t j = (i + 1u) % n;
    const float start = War3SunStyleSettings::KeyHours[i];
    const float end = j == 0u ? 24.0f : War3SunStyleSettings::KeyHours[j];
    float t = end > start ? (hours - start) / (end - start) : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const auto ease = [](float v) {
      v = std::clamp(v, 0.0f, 1.0f);
      return v * v * (3.0f - 2.0f * v);
    };
    float colourT = ease(t), brightT = colourT;
    switch (i) {
      case 0u:  // the moon, then (from 05:30) to sunrise
        brightT = ease((t - 11.0f / 12.0f) * 12.0f);
        colourT = ease((t - 23.0f / 24.0f) * 24.0f);
        break;
      case 1u:  // sunrise to the morning
        colourT = 1.0f - std::pow(1.0f - t, 3.0f);
        brightT = 1.0f - std::pow(1.0f - t, 10.0f);
        break;
      case 4u:  // the afternoon to sunset
        colourT = std::pow(t, 3.0f);
        brightT = std::pow(t, 10.0f);
        break;
      case 5u:  // sunset, then (by 18:30) the moon
        brightT = ease(t * 12.0f);
        colourT = ease(t * 24.0f);
        break;
      default:
        break;
    }
    // The colour in xyz (not yet times the brightness), the brightness in w.
    const auto light = [](const War3SunTintKey& key) {
      const auto channel = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f; };
      return Vector4(channel(key.r), channel(key.g), channel(key.b),
                     std::isfinite(key.intensity) ? std::clamp(key.intensity, 0.0f, 3.0f) : 1.0f);
    };
    const Vector4 a = light(style.tint[i]);
    const Vector4 b = light(style.tint[j]);
    const float k = a.w + (b.w - a.w) * brightT;
    return Vector4((a.x + (b.x - a.x) * colourT) * k, (a.y + (b.y - a.y) * colourT) * k,
                   (a.z + (b.z - a.z) * colourT) * k, k);
  }

  // Tanner Helland's algorithm (simplified)
  static Vector4 kelvinToRgb(float k) {
    float temp = k / 100.0f;
    float r, g, b;

    // Red
    if (temp <= 66.0f) {
      r = 255.0f;
    } else {
      r = temp - 60.0f;
      r = 329.698727446f * std::pow(r, -0.1332047592f);
      r = std::min(255.0f, std::max(0.0f, r));
    }

    // Green
    if (temp <= 66.0f) {
      g = temp;
      g = 99.4708025861f * std::log(g) - 161.1195681661f;
      g = std::min(255.0f, std::max(0.0f, g));
    } else {
      g = temp - 60.0f;
      g = 288.1221695283f * std::pow(g, -0.0755148492f);
      g = std::min(255.0f, std::max(0.0f, g));
    }

    // Blue
    if (temp >= 66.0f) {
      b = 255.0f;
    } else {
      if (temp <= 19.0f) {
        b = 0.0f;
      } else {
        b = temp - 10.0f;
        b = 138.5177312231f * std::log(b) - 305.0447927307f;
        b = std::min(255.0f, std::max(0.0f, b));
      }
    }

    return Vector4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
  }

private:
  bool m_timeInitialized = false;
  std::chrono::steady_clock::time_point m_timeStart;

  // 时间平滑：将 JASS/Hook 侧“台阶式”的 TIME_OF_DAY 变为逐帧连续值
  // 说明：Hook 侧通常 100ms 更新一次 TIME_OF_DAY，会导致太阳方向/CSM 以 10Hz 抖动。
  // 这里保持 Hook 采样频率不变，但渲染侧做一个轻量 PLL 平滑（积分 + 误差回正）。
  bool m_timeSmoothingInitialized = false;
  float m_time01Smoothed = 0.0f;
  float m_time01LastRaw = 0.0f;
  float m_time01Speed = 0.0f;  // time01 每秒变化量（约等于 1 / dayLengthSeconds）
  std::chrono::steady_clock::time_point m_time01LastUpdate;
  std::chrono::steady_clock::time_point m_time01LastRawSample;

  // 配置参数 (可根据需求调整或暴露给 Config)
  float m_dayLengthSeconds = 480.0f;       // 8分钟 = 24小时
  float m_startTime01 = 0.22f;             // 进游戏起始时间 (0.25=日出, 0.5=正午)

  // 阴影限制参数
  static constexpr float kMinSunAltitudeDeg = 35.0f; // 最小高度角 (保证阴影不拉太长, ~1.43x)
  static constexpr float kMaxSunAltitudeDeg = 55.0f; // 最大高度角 (防止自阴影过多 0.7x)
  // The real shadow length's floor: ~19 times the caster's height.
  static constexpr float kRealShadowFloorDeg = 3.0f;
  static constexpr float kMinPathNorth = 0.08f;

  // The look of the hour (docs/plan/2026-11-17-x64-proxy-performance-plan.md,
  // 12.19, has the sources and the numbers they gave). The ground's share of
  // the beam is sin(elevation) ^ gamma: more than the cosine law gives a low
  // sun, so the golden hour shows on the ground and not only on walls.
  static constexpr float kGroundShareGamma = 0.42f;
  static constexpr float kGroundShareFloor = 0.12f;
  // The ground gets the ambient twice (as D3DRS_AMBIENT and again as light
  // 0's ambient); the depth makes the noon sun's shadow 0.61.
  static constexpr float kAmbientOnGround = 2.0f;
  static constexpr float kShadowDepth = 0.985f;

  // The sun against its elevation in degrees: the colour (largest channel 1)
  // and the brightness. Direct sunlight through a clear atmosphere (Preetham,
  // Shirley, Smits, SIGGRAPH 99, appendix A.1) as sRGB, balanced so that the
  // noon sun is a warm white, and read at a lower elevation than the sun's:
  // elevation / 1.30 with turbidity 2.2 in the morning, / 1.42 with turbidity
  // 2.8 in the evening. The golden hour reaches higher than in nature and the
  // evening is redder than the morning. At the horizon the beam is nearly
  // out: there the moon takes over (kMoon starts on the same key).
  static constexpr LightKey kSunMorning[] = {
      {  0.00f, 1.000f, 0.460f, 0.220f, 0.120f },
      {  1.00f, 1.000f, 0.460f, 0.220f, 0.320f },
      {  2.00f, 1.000f, 0.513f, 0.220f, 0.493f },
      {  3.00f, 1.000f, 0.592f, 0.220f, 0.640f },
      {  4.50f, 1.000f, 0.677f, 0.236f, 0.735f },
      {  6.00f, 1.000f, 0.736f, 0.351f, 0.830f },
      {  8.00f, 1.000f, 0.791f, 0.461f, 0.880f },
      { 10.00f, 1.000f, 0.828f, 0.542f, 0.930f },
      { 13.00f, 1.000f, 0.867f, 0.630f, 0.957f },
      { 16.00f, 1.000f, 0.892f, 0.693f, 0.978f },
      { 20.00f, 1.000f, 0.915f, 0.753f, 0.988f },
      { 25.00f, 1.000f, 0.933f, 0.805f, 1.000f },
      { 32.00f, 1.000f, 0.950f, 0.852f, 1.000f },
      { 40.00f, 1.000f, 0.962f, 0.888f, 1.000f },
      { 50.00f, 1.000f, 0.971f, 0.916f, 1.000f },
      { 65.00f, 1.000f, 0.979f, 0.942f, 1.000f },
      { 90.00f, 1.000f, 0.985f, 0.962f, 1.000f },
  };
  static constexpr LightKey kSunEvening[] = {
      {  0.00f, 1.000f, 0.460f, 0.220f, 0.120f },
      {  1.00f, 1.000f, 0.460f, 0.220f, 0.320f },
      {  2.00f, 1.000f, 0.460f, 0.220f, 0.493f },
      {  3.00f, 1.000f, 0.507f, 0.220f, 0.640f },
      {  4.50f, 1.000f, 0.602f, 0.220f, 0.735f },
      {  6.00f, 1.000f, 0.670f, 0.220f, 0.830f },
      {  8.00f, 1.000f, 0.734f, 0.346f, 0.880f },
      { 10.00f, 1.000f, 0.781f, 0.440f, 0.930f },
      { 13.00f, 1.000f, 0.828f, 0.545f, 0.957f },
      { 16.00f, 1.000f, 0.861f, 0.621f, 0.978f },
      { 20.00f, 1.000f, 0.891f, 0.696f, 0.988f },
      { 25.00f, 1.000f, 0.916f, 0.761f, 1.000f },
      { 32.00f, 1.000f, 0.939f, 0.823f, 1.000f },
      { 40.00f, 1.000f, 0.955f, 0.869f, 1.000f },
      { 50.00f, 1.000f, 0.967f, 0.907f, 1.000f },
      { 65.00f, 1.000f, 0.979f, 0.942f, 1.000f },
      { 90.00f, 1.000f, 0.988f, 0.972f, 1.000f },
  };
  // The moon against its elevation: the sun's own last light at the horizon
  // (no jump where they change places), then the blue the eye gives a moonlit
  // scene (towards the hue of Jensen et al., "A Physically-Based Night Sky
  // Model", SIGGRAPH 2001). Its brightness is on the scale of the night's
  // ambient: together they stay under white, so the night is blue, not grey.
  static constexpr LightKey kMoon[] = {
      {  0.00f, 1.000f, 0.460f, 0.220f, 0.120f },
      {  1.50f, 0.820f, 0.560f, 0.560f, 0.220f },
      {  3.00f, 0.720f, 0.640f, 0.800f, 0.300f },
      {  8.00f, 0.620f, 0.720f, 0.960f, 0.400f },
      { 20.00f, 0.600f, 0.740f, 1.000f, 0.460f },
      { 90.00f, 0.600f, 0.740f, 1.000f, 0.460f },
  };
  // The ambient with the default day and night colours, against the sun's
  // elevation (negative: the moon's): night, the blue hour, the purple light
  // (strongest with the sun 3 to 4 degrees under the horizon), a mauve with
  // little blue through the golden hour (the low sun's light then shows warm
  // on the ground and leaves a cool shadow), the cool sky of the day.
  static constexpr LightKey kAmbient[] = {
      { -90.00f, 0.150f, 0.190f, 0.320f, 1.000f },
      { -14.00f, 0.152f, 0.192f, 0.330f, 1.000f },
      { -7.00f, 0.200f, 0.208f, 0.370f, 1.000f },
      { -3.00f, 0.245f, 0.235f, 0.380f, 1.000f },
      {  0.00f, 0.250f, 0.228f, 0.330f, 1.000f },
      {  1.50f, 0.250f, 0.228f, 0.300f, 1.000f },
      {  4.00f, 0.252f, 0.234f, 0.280f, 1.000f },
      {  8.00f, 0.258f, 0.248f, 0.262f, 1.000f },
      { 12.00f, 0.262f, 0.262f, 0.280f, 1.000f },
      { 30.00f, 0.262f, 0.277f, 0.315f, 1.000f },
      { 90.00f, 0.262f, 0.278f, 0.312f, 1.000f },
  };
};

} // namespace dxvk
