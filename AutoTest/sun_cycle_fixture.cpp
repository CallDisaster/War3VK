// The light of the day as numbers: War3SunCycle::Evaluate over whole days.
//
// Standalone (no device, no game):
//   clang++ -std=c++17 -O1 -static -Isrc/d3d9 -Isrc -Iinclude -Iinclude/vulkan/include
//           -Iinclude/spirv/include -Iinclude/native AutoTest/sun_cycle_fixture.cpp -o sun-cycle.exe
// Exit code 0 when every check holds; one line per check.

#include "war3/effects/war3_sun_cycle.h"

#include <cmath>
#include <cstdio>

using namespace dxvk;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what, const char* detail = "") {
  std::printf("%s %s %s\n", ok ? "ok  " : "FAIL", what, detail);
  if (!ok)
    ++g_failed;
}

float Angle(const Vector4& a, const Vector4& b) {
  const float la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z), lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
  const float c = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
  return std::acos(std::fmin(1.0f, std::fmax(-1.0f, c))) * 57.29578f;
}

float MaxDiff(const Vector4& a, const Vector4& b) {
  return std::fmax(std::fabs(a.x - b.x), std::fmax(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
}

struct Path { bool set; float north; float noon; };
constexpr Path kPaths[] = {
    { false, -0.5f, 60.0f }, { false, -0.5f, 25.0f }, { false, -0.5f, 90.0f },
    { true, -0.545f, 60.0f }, { true, 0.3f, 60.0f }, { true, -0.95f, 60.0f },
};

War3RenderSettings Settings(const Path& path, bool realLength) {
  War3RenderSettings s{};
  s.sunStyle.pathSet = path.set;
  s.sunStyle.pathNorth = path.north;
  s.sunStyle.noonAltitudeDeg = path.noon;
  s.sunStyle.realShadowLength = realLength;
  return s;
}

}  // namespace

int main() {
  char detail[160];
  constexpr int kSteps = 2880;  // half a game minute

  // 1. The real shadow length changes the shadow and nothing of the look.
  {
    float worstColor = 0.0f, worstAngle = 0.0f, worstStrength = 0.0f;
    bool lengthDiffers = false;
    for (const Path& path : kPaths) {
      const War3RenderSettings off = Settings(path, false), on = Settings(path, true);
      for (int i = 0; i < kSteps; ++i) {
        const float t = float(i) / float(kSteps);
        const auto a = War3SunCycle::Evaluate(off, t), b = War3SunCycle::Evaluate(on, t);
        worstColor = std::fmax(worstColor, MaxDiff(a.color, b.color));
        worstAngle = std::fmax(worstAngle, Angle(a.lightDirection, b.lightDirection));
        worstStrength = std::fmax(worstStrength, std::fabs(a.shadowStrength - b.shadowStrength));
        lengthDiffers = lengthDiffers || std::fabs(a.castAltitudeRad - b.castAltitudeRad) > 0.05f;
      }
    }
    std::snprintf(detail, sizeof(detail), "colour %.6f, shading %.4f deg, strength %.6f", worstColor, worstAngle,
                  worstStrength);
    Check(worstColor == 0.0f && worstAngle < 1e-3f && worstStrength == 0.0f,
          "the look is the same with and without the real shadow length:", detail);
    Check(lengthDiffers, "the shadow's own altitude does differ");
  }

  // 2. No jump over the day (a jump keeps its size however fine the step; a slope shrinks with it);
  //    at the handover between sun and moon the light is dim and shadowless.
  constexpr int kFine = kSteps * 8;
  for (int real = 0; real < 2; ++real) {
    float colorStep = 0.0f, strengthStep = 0.0f, turn = 0.0f, handoverLight = 0.0f, handoverShadow = 0.0f;
    for (const Path& path : kPaths) {
      const War3RenderSettings s = Settings(path, real != 0);
      auto before = War3SunCycle::Evaluate(s, 0.0f);
      for (int i = 1; i <= kFine; ++i) {
        const auto now = War3SunCycle::Evaluate(s, float(i % kFine) / float(kFine));
        colorStep = std::fmax(colorStep, MaxDiff(now.color, before.color));
        strengthStep = std::fmax(strengthStep, std::fabs(now.shadowStrength - before.shadowStrength));
        if (now.day == before.day) {
          turn = std::fmax(turn, Angle(now.lightDirection, before.lightDirection));
        } else {
          handoverLight = std::fmax(handoverLight, std::fmax(now.color.x, std::fmax(now.color.y, now.color.z)));
          handoverShadow = std::fmax(handoverShadow, std::fmax(now.shadowStrength, before.shadowStrength));
        }
        before = now;
      }
    }
    std::snprintf(detail, sizeof(detail), "(%s) colour %.4f, strength %.4f, shading %.2f deg per 1/16 minute",
                  real ? "real length" : "short shadows", colorStep, strengthStep, turn);
    Check(colorStep < 0.008f && strengthStep < 0.004f && turn < 0.5f, "the day runs without a jump", detail);
    std::snprintf(detail, sizeof(detail), "light %.3f, shadow %.4f", handoverLight, handoverShadow);
    Check(handoverLight < 0.30f && handoverShadow < 0.03f, "sun and moon change places in a dim, shadowless light:",
          detail);
  }

  // 3. The shading direction: a unit vector from above, as high as the ground's share of the beam.
  {
    bool ok = true;
    for (const Path& path : kPaths) {
      const War3RenderSettings s = Settings(path, false);
      for (int i = 0; i < kSteps; ++i) {
        const auto r = War3SunCycle::Evaluate(s, float(i) / float(kSteps));
        const Vector4& d = r.lightDirection;
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        ok = ok && std::fabs(length - 1.0f) < 1e-4f && d.z < 0.0f &&
             std::fabs(-d.z - War3SunCycle::GroundShare(r.lightElevationRad)) < 1e-4f;
      }
    }
    Check(ok, "the world is shaded from the ground's share of the beam");
  }

  // 4. The numbers the look was tuned to (default path).
  {
    const War3RenderSettings s = Settings(kPaths[0], false);
    const auto noon = War3SunCycle::Evaluate(s, 0.5f);
    const auto morning = War3SunCycle::Evaluate(s, 6.6f / 24.0f), evening = War3SunCycle::Evaluate(s, 17.4f / 24.0f);
    const auto night = War3SunCycle::Evaluate(s, 0.0f);
    std::snprintf(detail, sizeof(detail), "%.3f %.3f %.3f, shadow %.3f", noon.color.x, noon.color.y, noon.color.z,
                  noon.shadowStrength);
    Check(noon.color.x == 1.0f && noon.color.y > 0.96f && noon.color.z > 0.90f && noon.color.z < 0.96f &&
          std::fabs(noon.shadowStrength - 0.6075f) < 0.02f, "noon: a warm white, the shadow the pass was tuned to:", detail);
    std::snprintf(detail, sizeof(detail), "morning b/r %.3f, evening b/r %.3f, shadows %.3f %.3f",
                  morning.color.z / morning.color.x, evening.color.z / evening.color.x, morning.shadowStrength,
                  evening.shadowStrength);
    Check(evening.color.z / evening.color.x < morning.color.z / morning.color.x &&
          morning.color.z / morning.color.x < 0.75f && morning.shadowStrength < noon.shadowStrength * 0.8f,
          "the low sun is warm, the evening warmer, its shadow fainter:", detail);
    Check(night.color.z > night.color.x * 1.5f && night.shadowStrength > 0.3f, "the midnight moon is blue and casts");
  }

  // 5. The colour curve's default keys follow the sun's own colours by day.
  {
    War3RenderSettings model = Settings(kPaths[0], false), keys = model;
    keys.sunStyle.customTint = true;
    float worstDay = 0.0f, worstAll = 0.0f;
    for (int i = 0; i < kSteps; ++i) {
      const float t = float(i) / float(kSteps);
      const float d = MaxDiff(War3SunCycle::Evaluate(model, t).color, War3SunCycle::Evaluate(keys, t).color);
      worstAll = std::fmax(worstAll, d);
      if (t > 7.5f / 24.0f && t < 16.5f / 24.0f)
        worstDay = std::fmax(worstDay, d);
    }
    std::snprintf(detail, sizeof(detail), "by day %.3f, at most %.3f", worstDay, worstAll);
    Check(worstDay < 0.05f && worstAll < 0.20f, "the default keys are the sun's own colours:", detail);
  }

  // 6. The ambient: finite, never dark, the same hue family as the table at the default colours.
  {
    const War3RenderSettings s = Settings(kPaths[0], false);
    const War3DayNightSettings dn{};
    bool ok = true;
    float step = 0.0f;
    Vector4 before(0.0f);
    for (int i = 0; i <= kSteps; ++i) {
      const float t = float(i % kSteps) / float(kSteps);
      const float arc = std::sin((t - 0.25f) * 6.2831853f) * 90.0f;
      float f = std::fmin(1.0f, std::fmax(0.0f, (arc - dn.dayTransitionStartDeg) /
                                                    (dn.dayTransitionEndDeg - dn.dayTransitionStartDeg)));
      f = f * f * (3.0f - 2.0f * f);
      const Vector4 a = War3SunCycle::AmbientBlend(dn.nightAmbient, dn.dayAmbient, f) *
                        War3SunCycle::AmbientTint(s.sunStyle, t, f);
      ok = ok && std::isfinite(a.x) && a.x > 0.15f && a.x < 0.40f && a.y > 0.15f && a.y < 0.40f && a.z > a.y &&
           a.z < 0.45f;
      if (i > 0)
        step = std::fmax(step, MaxDiff(a, before));
      before = a;
    }
    std::snprintf(detail, sizeof(detail), "largest step %.4f", step);
    Check(ok && step < 0.01f, "the ambient stays in range, bluer than green, without a jump:", detail);
  }

  std::printf("%s\n", g_failed == 0 ? "sun cycle fixture: all checks hold" : "sun cycle fixture: FAILED");
  return g_failed == 0 ? 0 : 1;
}
