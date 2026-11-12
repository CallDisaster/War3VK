#include "war3_player_settings.h"

#include "../../d3d9_war3_settings.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace dxvk::war3::player {

namespace {

enum class Kind : uint8_t { Bool, Float, Int, UInt32, Enum8, Enum32 };

struct Field {
  const char* key;
  Kind kind;
  uint8_t size;
  double lo;
  double hi;
  bool pow2;
  void* (*ref)(War3RenderSettings&) noexcept;
};

template <typename T> struct AlwaysFalse : std::false_type {};

template <typename T> constexpr Kind KindOf() {
  if constexpr (std::is_same_v<T, bool>)
    return Kind::Bool;
  else if constexpr (std::is_same_v<T, float>)
    return Kind::Float;
  else if constexpr (std::is_same_v<T, int>)
    return Kind::Int;
  else if constexpr (std::is_same_v<T, uint32_t>)
    return Kind::UInt32;
  else if constexpr (std::is_enum_v<T> && sizeof(T) == 1)
    return Kind::Enum8;
  else if constexpr (std::is_enum_v<T> && sizeof(T) == 4)
    return Kind::Enum32;
  else
    static_assert(AlwaysFalse<T>::value, "unsupported player setting type");
}

template <typename T>
using Bare = std::remove_cv_t<std::remove_reference_t<T>>;

#define WARVK_PLAYER_FIELD_IMPL(key, member, lo, hi, pow2)                     \
  Field{key,                                                                   \
        KindOf<Bare<decltype(std::declval<War3RenderSettings&>().member)>>(), \
        uint8_t(sizeof(std::declval<War3RenderSettings&>().member)),          \
        double(lo), double(hi), pow2,                                          \
        [](War3RenderSettings& s) noexcept -> void* { return &s.member; }}
#define F(key, member, lo, hi) WARVK_PLAYER_FIELD_IMPL(key, member, lo, hi, false)
#define F_POW2(key, member, lo, hi) WARVK_PLAYER_FIELD_IMPL(key, member, lo, hi, true)
#define F_BOOL(key, member) WARVK_PLAYER_FIELD_IMPL(key, member, 0, 1, false)

// Every value the Ctrl+F1 "内置渲染设置" header can change, with the panel's own
// ranges. Debug-only controls (debug output, stage/group filters, pass
// hot-plugging, test lights) are deliberately absent. Key names are stable:
// renaming one silently drops a player's saved value.
const Field kFields[] = {
    // 总开关
    F_BOOL("Shadows.Enabled", shadows.enabled),
    F_BOOL("Shadows.PointLights", shadows.pointLightsEnabled),
    F_BOOL("Shadows.PointShadows", shadows.pointShadowEnabled),
    F_BOOL("PostFx.Enabled", postFx.enabled),

    // 被遮挡描边
    F_BOOL("Outline.Enabled", occludedOutline.enabled),
    F("Outline.Mode", occludedOutline.mode, 0, 1),
    F_BOOL("Outline.ScreenSpace", occludedOutline.useScreenSpace),
    F("Outline.WidthPx", occludedOutline.widthPx, 1, 10),
    F_BOOL("Outline.ShowVisible", occludedOutline.showVisible),
    F_BOOL("Outline.ShowOccluded", occludedOutline.showOccluded),
    F("Outline.ColorR", occludedOutline.colorR, 0, 1),
    F("Outline.ColorG", occludedOutline.colorG, 0, 1),
    F("Outline.ColorB", occludedOutline.colorB, 0, 1),
    F("Outline.ColorA", occludedOutline.colorA, 0, 1),

    // 光照
    F_BOOL("Sun.Enabled", sun.enabled),
    F("Sun.Intensity", sun.intensity, 0, 3),

    // 光影参数
    F("Shadows.Strength", shadows.strength, 0, 1),
    F("Shadows.PcfRadius", shadows.pcfRadius, 0.5, 6),
    F("Shadows.PcfKernel", shadows.pcfKernel, 0, 3),
    F_BOOL("Shadows.PcfRotate", shadows.pcfRotate),
    F("Shadows.PcfRotateMode", shadows.pcfRotateMode, 0, 2),
    F("Shadows.ReceiverBias", shadows.receiverBias, 0, 0.02),
    F("Shadows.CascadeBiasScale", shadows.cascadeBiasScale, 0, 1),
    F("Shadows.PcfCascadeRadiusScale", shadows.pcfCascadeRadiusScale, 0, 1),
    F("Shadows.CasterDepthBias", shadows.casterDepthBias, 0, 4),
    F("Shadows.CasterSlopeBias", shadows.casterSlopeBias, 0, 4),
    F("Shadows.CasterBiasClamp", shadows.casterBiasClamp, 0, 5),
    F("Shadows.CsmCascades", shadows.csm.cascadeCount, 1, 4),
    F_POW2("Shadows.CsmResolution", shadows.csm.shadowResolution, 1024, 4096),
    F("Shadows.CsmMaxDistance", shadows.csm.maxDistance, 1000, 8000),
    F("Shadows.CsmSplitLambda", shadows.csm.splitLambda, 0, 1),
    // 0..1 only: the development RTS receiver band (2) is never a player value.
    F("Shadows.CsmFitMode", shadows.csm.fitMode, 0, 1),
    F("Shadows.CsmDepthMargin", shadows.csm.depthRangeMargin, 0, 500),
    F_BOOL("Shadows.LockSun", shadows.lockSun),
    F("Shadows.LockSunTime", shadows.lockSunTime, 0, 1),
    F_BOOL("Shadows.StableSnap", shadows.stableSnapWhenSunMoving),
    F_BOOL("Shadows.Pcss", shadows.pcssEnabled),
    F("Shadows.ReceiverMode", shadows.receiverMode, 0, 2),
    F("Shadows.NormalBiasScale", shadows.normalBiasScale, 0, 0.01),
    F("Shadows.FilterMode", shadows.filterMode, 0, 1),
    F("Shadows.AltitudeMode", shadows.altitudeMode, 0, 1),
    F("Shadows.LengthScale", shadows.shadowLengthScale, 0.6, 1.2),
    F("Shadows.MaxLengthScale", shadows.shadowMaxLengthScale, 0.5, 1.2),
    F("Shadows.NightScale", shadows.nightShadowScale, 0, 1),
    F("Shadows.RimIntensity", shadows.rimIntensity, 0, 1.5),
    F("Shadows.RimPower", shadows.rimPower, 0.2, 6),
    F("Shadows.PcssSearchKernel", shadows.pcssSearchKernel, 0, 1),
    F("Shadows.PcssSearchRadius", shadows.pcssSearchRadius, 1, 12),
    F("Shadows.PcssMinRadius", shadows.pcssMinRadius, 0.5, 4),
    F("Shadows.PcssMaxRadius", shadows.pcssMaxRadius, 1, 8),
    F("Shadows.PcssDepthScale", shadows.pcssDepthScale, 10, 80),
    F_BOOL("Shadows.AlphaHashed", shadows.alphaShadowHashed),
    F_BOOL("Shadows.AlphaUseMip", shadows.alphaShadowUseMip),
    F("Shadows.AlphaMipLodBias", shadows.alphaShadowMipLodBias, -2, 4),
    F("Shadows.AlphaFarRefBias", shadows.alphaShadowFarAlphaRefBias, 0, 0.35),
    F("Shadows.TaaMode", shadows.shadowTaaMode, 0, 2),
    F("Shadows.TaaBlend", shadows.shadowTaaBlendFactor, 0.12, 0.30),
    F_BOOL("Shadows.TaaNeighborClamp", shadows.shadowTaaNeighborClamp),

    // 点光源
    F("PointShadow.MaxLights", shadows.pointShadowMaxLights, 0, 4),
    F_POW2("PointShadow.Resolution", shadows.pointShadowResolution, 128, 2048),
    F_BOOL("PointShadow.TemporalReuse", shadows.pointShadowTemporalReuse),
    F("PointShadow.UpdatePeriod", shadows.pointShadowUpdatePeriod, 1, 4),
    F_BOOL("PointShadow.FaceCulling", shadows.pointShadowFaceCulling),
    F("PointShadow.Bias", shadows.pointShadowBias, 0, 0.2),
    F("PointShadow.PcfNear", shadows.pointShadowPcfRadiusNear, 0, 3),
    F("PointShadow.PcfFar", shadows.pointShadowPcfRadiusFar, 0, 4),
    F("PointShadow.TexelBias", shadows.pointShadowTexelBiasScale, 0, 1),
    F("PointShadow.FadeStart", shadows.pointShadowRangeFadeStart, 0.5, 0.98),
    F_BOOL("PointRay.Enabled", shadows.pointRayShadowEnabled),
    F_BOOL("PointRay.HiZ", shadows.pointRayShadowHiZEnabled),
    F("PointRay.MaxLights", shadows.pointRayShadowMaxLights, 1, 2),
    F("PointRay.Steps", shadows.pointRayShadowSteps, 4, 32),
    F("PointRay.HiZVisits", shadows.pointRayShadowHiZMaxVisits, 8, 64),
    F("PointRay.MaxDistance", shadows.pointRayShadowMaxDistance, 32, 2400),
    F("PointRay.Thickness", shadows.pointRayShadowThickness, 1, 160),
    F("PointRay.StartOffset", shadows.pointRayShadowStartOffset, 1, 96),
    F("PointRay.Strength", shadows.pointRayShadowStrength, 0, 1),

    // AA
    F("AA.Mode", postFx.aa.mode, 0, 5),
    F("AA.FxaaSubpix", postFx.aa.fxaaQualitySubpix, 0, 1),
    F("AA.FxaaEdge", postFx.aa.fxaaQualityEdgeThreshold, 0.05, 0.5),
    F("AA.FxaaEdgeMin", postFx.aa.fxaaQualityEdgeThresholdMin, 0.02, 0.2),
    F("AA.SmaaThreshold", postFx.aa.smaaThreshold, 0.05, 0.2),
    F("AA.SmaaSteps", postFx.aa.smaaMaxSearchSteps, 4, 32),
    F("AA.SmaaDiagSteps", postFx.aa.smaaMaxSearchStepsDiag, 0, 16),

    // SSAO
    F_BOOL("SSAO.Enabled", postFx.ssao.enabled),
    F("SSAO.RadiusPx", postFx.ssao.radiusPx, 1, 32),
    F("SSAO.Strength", postFx.ssao.strength, 0, 3),
    F("SSAO.Bias", postFx.ssao.bias, 0, 0.2),
    F("SSAO.Power", postFx.ssao.power, 0.5, 4),
    F("SSAO.FadeNear", postFx.ssao.fadeNear, 0, 0.9),
    F("SSAO.FadeFar", postFx.ssao.fadeFar, 0.1, 1),

    // 体积光
    F_BOOL("Volumetric.Enabled", postFx.volumetricLight.enabled),
    F("Volumetric.Quality", postFx.volumetricLight.quality, 0, 2),
    F_BOOL("Volumetric.FroxelTemporal", postFx.volumetricLight.froxelTemporalEnabled),
    F_BOOL("Volumetric.RequireCsm", postFx.volumetricLight.requireCsmSnapshot),
    F_BOOL("Volumetric.PointLights", postFx.volumetricLight.includePointLights),
    F("Volumetric.MaxPointLights", postFx.volumetricLight.maxPointLights, 0, 2),
    F_BOOL("Volumetric.AdaptiveSamples", postFx.volumetricLight.adaptiveSampleCount),
    F("Volumetric.MinSunIntensity", postFx.volumetricLight.minSunIntensity, 0, 0.5),
    F("Volumetric.Intensity", postFx.volumetricLight.intensity, 0, 4),
    F("Volumetric.Samples", postFx.volumetricLight.sampleCount, 4, 16),
    F("Volumetric.Density", postFx.volumetricLight.density, 0, 2),
    F("Volumetric.Weight", postFx.volumetricLight.weight, 0, 3),
    F("Volumetric.SunDistance", postFx.volumetricLight.sunDistance, 100, 6000),
    F("Volumetric.FroxelFar", postFx.volumetricLight.froxelFar, 1000, 10000),
    F("Volumetric.FadeNear", postFx.volumetricLight.fadeNear, 0, 0.95),
    F("Volumetric.FadeFar", postFx.volumetricLight.fadeFar, 0.1, 1),
    F("Volumetric.HeightFog", postFx.volumetricLight.heightFogStrength, 0, 2),
    F("Volumetric.Extinction", postFx.volumetricLight.extinctionStrength, 0, 1),
    F("Volumetric.Unshadowed", postFx.volumetricLight.unshadowedScattering, 0, 1),

    // Bloom / ACES
    F_BOOL("Bloom.Enabled", postFx.bloom.enabled),
    F("Bloom.Threshold", postFx.bloom.threshold, 0.1, 2.5),
    F("Bloom.SoftKnee", postFx.bloom.softKnee, 0.05, 1.5),
    F("Bloom.Intensity", postFx.bloom.intensity, 0, 2),
    F_BOOL("Bloom.Aces", postFx.bloom.acesToneMap),
    F("PostFx.Exposure", postFx.exposure, 0.1, 4),
    F_BOOL("PostFx.Srgb", postFx.useSrgb),

    // 环境光覆盖
    F_BOOL("Ambient.Override", ambient.overrideEnabled),
    F_BOOL("Ambient.AutoOverride", ambient.autoOverride),
    F("Ambient.ColorR", ambient.color.x, 0, 1),
    F("Ambient.ColorG", ambient.color.y, 0, 1),
    F("Ambient.ColorB", ambient.color.z, 0, 1),
    F("Ambient.ColorA", ambient.color.w, 0, 1),

    // 日夜色调
    F_BOOL("DayNight.Enabled", dayNight.enabled),
    F_BOOL("DayNight.AffectAmbient", dayNight.affectAmbient),
    F_BOOL("DayNight.AffectExposure", dayNight.affectExposure),
    F("DayNight.TransitionMin", dayNight.transitionMinFactor, 0.5, 1),
    F("DayNight.DayAmbientR", dayNight.dayAmbient.x, 0, 1),
    F("DayNight.DayAmbientG", dayNight.dayAmbient.y, 0, 1),
    F("DayNight.DayAmbientB", dayNight.dayAmbient.z, 0, 1),
    F("DayNight.DayAmbientA", dayNight.dayAmbient.w, 0, 1),
    F("DayNight.NightAmbientR", dayNight.nightAmbient.x, 0, 1),
    F("DayNight.NightAmbientG", dayNight.nightAmbient.y, 0, 1),
    F("DayNight.NightAmbientB", dayNight.nightAmbient.z, 0, 1),
    F("DayNight.NightAmbientA", dayNight.nightAmbient.w, 0, 1),
    F("DayNight.DayExposure", dayNight.dayExposure, 0.6, 2),
    F("DayNight.NightExposure", dayNight.nightExposure, 0.2, 1.2),
    F("DayNight.TransitionStartDeg", dayNight.dayTransitionStartDeg, -6, 6),
    F("DayNight.TransitionEndDeg", dayNight.dayTransitionEndDeg, 2, 16),
};

#undef F
#undef F_POW2
#undef F_BOOL
#undef WARVK_PLAYER_FIELD_IMPL

constexpr const char* kFpsUnlockedKey = "General.FpsUnlocked";
constexpr const char* kNativeShadowModeKey = "Shadows.NativeMode";
constexpr const char* kTaaModeKey = "Shadows.TaaMode";
constexpr const char* kSectionName = "PlayerSettings";

constexpr size_t kMaxEntries = 512u;
constexpr size_t kMaxKeyLength = 64u;
constexpr size_t kMaxValueLength = 64u;
constexpr DWORD kMaxFileBytes = 64u * 1024u;

const Field* FindField(const std::string& key) {
  for (const Field& field : kFields) {
    if (key == field.key)
      return &field;
  }
  return nullptr;
}

bool IsExtraKey(const std::string& key) {
  return key == kFpsUnlockedKey || key == kNativeShadowModeKey;
}

double ReadNumber(const Field& field, const War3RenderSettings& settings) {
  const void* source = field.ref(const_cast<War3RenderSettings&>(settings));
  switch (field.kind) {
    case Kind::Bool: {
      bool value = false;
      std::memcpy(&value, source, sizeof(value));
      return value ? 1.0 : 0.0;
    }
    case Kind::Float: {
      float value = 0.0f;
      std::memcpy(&value, source, sizeof(value));
      return double(value);
    }
    case Kind::Int: {
      int32_t value = 0;
      std::memcpy(&value, source, sizeof(value));
      return double(value);
    }
    case Kind::UInt32:
    case Kind::Enum32: {
      uint32_t value = 0;
      std::memcpy(&value, source, sizeof(value));
      return double(value);
    }
    case Kind::Enum8: {
      uint8_t value = 0;
      std::memcpy(&value, source, sizeof(value));
      return double(value);
    }
  }
  return 0.0;
}

void WriteNumber(const Field& field, War3RenderSettings& settings, double value) {
  void* target = field.ref(settings);
  switch (field.kind) {
    case Kind::Bool: {
      const bool v = value != 0.0;
      std::memcpy(target, &v, sizeof(v));
      break;
    }
    case Kind::Float: {
      const float v = float(value);
      std::memcpy(target, &v, sizeof(v));
      break;
    }
    case Kind::Int: {
      const int32_t v = int32_t(std::llround(value));
      std::memcpy(target, &v, sizeof(v));
      break;
    }
    case Kind::UInt32:
    case Kind::Enum32: {
      const uint32_t v = uint32_t(std::llround(value));
      std::memcpy(target, &v, sizeof(v));
      break;
    }
    case Kind::Enum8: {
      const uint8_t v = uint8_t(std::llround(value));
      std::memcpy(target, &v, sizeof(v));
      break;
    }
  }
}

bool SameValue(const Field& field, const War3RenderSettings& a,
               const War3RenderSettings& b) {
  const void* pa = field.ref(const_cast<War3RenderSettings&>(a));
  const void* pb = field.ref(const_cast<War3RenderSettings&>(b));
  return std::memcmp(pa, pb, field.size) == 0;
}

std::string Trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
    ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
    --end;
  return text.substr(begin, end - begin);
}

std::string Lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
  }
  return text;
}

bool ParseBool(const std::string& text, bool& value) {
  const std::string lower = Lower(Trim(text));
  if (lower == "1" || lower == "true" || lower == "on" || lower == "yes") {
    value = true;
    return true;
  }
  if (lower == "0" || lower == "false" || lower == "off" || lower == "no") {
    value = false;
    return true;
  }
  return false;
}

bool ParseInteger(const std::string& raw, long long& value) {
  const std::string text = Trim(raw);
  size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  if (i >= text.size() || text.size() - i > 18u)
    return false;
  long long result = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9')
      return false;
    result = result * 10 + (text[i] - '0');
  }
  value = negative ? -result : result;
  return true;
}

double SnapPow2(double value, double lo, double hi) {
  double best = lo;
  for (double candidate = lo; candidate <= hi; candidate *= 2.0) {
    if (std::fabs(candidate - value) < std::fabs(best - value) ||
        (std::fabs(candidate - value) == std::fabs(best - value) &&
         candidate > best))
      best = candidate;
  }
  return best;
}

bool ParseFieldValue(const Field& field, const std::string& text, double& out) {
  double value = 0.0;
  switch (field.kind) {
    case Kind::Bool: {
      bool b = false;
      if (!ParseBool(text, b))
        return false;
      out = b ? 1.0 : 0.0;
      return true;
    }
    case Kind::Float:
      if (!ParseDecimal(text, value))
        return false;
      break;
    default: {
      long long integer = 0;
      if (!ParseInteger(text, integer))
        return false;
      value = double(integer);
      break;
    }
  }
  value = std::clamp(value, field.lo, field.hi);
  if (field.pow2)
    value = SnapPow2(value, field.lo, field.hi);
  out = value;
  return true;
}

std::string FormatFieldValue(const Field& field, const War3RenderSettings& settings) {
  const double value = ReadNumber(field, settings);
  switch (field.kind) {
    case Kind::Bool:
      return value != 0.0 ? "1" : "0";
    case Kind::Float:
      return FormatDecimal(value);
    default:
      return std::to_string(std::llround(value));
  }
}

bool ParseExtra(const std::string& key, const std::string& text, Extras& extras) {
  if (key == kFpsUnlockedKey) {
    bool value = false;
    if (!ParseBool(text, value))
      return false;
    extras.fpsUnlocked = value;
    return true;
  }
  long long mode = 0;
  if (!ParseInteger(text, mode))
    return false;
  extras.nativeShadowMode = uint32_t(std::clamp<long long>(mode, 0, 2));
  return true;
}

// Keeps values the panel derives from another control consistent after a
// load or a reset touched the source control.
void FixDerived(War3RenderSettings& settings, bool taaModeTouched,
                bool pointPcfTouched) {
  if (taaModeTouched) {
    settings.shadows.shadowTaaEnabled =
        settings.shadows.shadowTaaMode == War3ShadowTaaMode::Temporal;
    ++settings.shadows.shadowTaaSettingsRevision;
  }
  if (pointPcfTouched) {
    settings.shadows.pointShadowPcfRadiusFar =
        (std::max)(settings.shadows.pointShadowPcfRadiusFar,
                   settings.shadows.pointShadowPcfRadiusNear);
  }
}

bool IsPointPcfKey(const std::string& key) {
  return key == "PointShadow.PcfNear" || key == "PointShadow.PcfFar";
}

bool ValidKey(const std::string& key) {
  if (key.empty() || key.size() > kMaxKeyLength)
    return false;
  for (const char c : key) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_';
    if (!ok)
      return false;
  }
  return true;
}

bool ValidValue(const std::string& value) {
  if (value.empty() || value.size() > kMaxValueLength)
    return false;
  for (const char c : value) {
    if (c < 0x20 || c > 0x7e)
      return false;
  }
  return true;
}

std::wstring ParentDirectory(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

uint64_t NowMs() { return GetTickCount64(); }

struct StoreState {
  std::mutex mutex;
  bool loaded = false;
  std::wstring path;
  Overrides overrides;
  bool dirty = false;
  bool lastSaveFailed = false;
  uint64_t lastChangeMs = 0;
  std::unique_ptr<War3RenderSettings> baselineSettings;
  Extras baselineExtras;
};

StoreState& State() {
  static StoreState state;
  return state;
}

void LoadLocked(StoreState& state) {
  if (state.loaded)
    return;
  state.loaded = true;
  if (state.path.empty())
    state.path = DefaultSettingsPath();
  std::string text;
  if (ReadTextFile(state.path, text))
    state.overrides = ParseFile(text);
}

} // namespace

ApplyReport ApplyOverrides(Overrides& overrides, War3RenderSettings& settings,
                           Extras& extras) {
  ApplyReport report;
  bool taaModeTouched = false;
  bool pointPcfTouched = false;
  for (auto it = overrides.begin(); it != overrides.end();) {
    const std::string& key = it->first;
    bool ok = false;
    bool known = true;
    if (const Field* field = FindField(key)) {
      double value = 0.0;
      ok = ParseFieldValue(*field, it->second, value);
      if (ok) {
        WriteNumber(*field, settings, value);
        taaModeTouched = taaModeTouched || key == kTaaModeKey;
        pointPcfTouched = pointPcfTouched || IsPointPcfKey(key);
      }
    } else if (IsExtraKey(key)) {
      ok = ParseExtra(key, it->second, extras);
    } else {
      known = false;
    }

    if (!known) {
      ++report.unknown;
      ++it;
    } else if (ok) {
      ++report.applied;
      ++it;
    } else {
      report.rejected.push_back(key);
      it = overrides.erase(it);
    }
  }
  FixDerived(settings, taaModeTouched, pointPcfTouched);
  return report;
}

size_t RecordChanges(const War3RenderSettings& before,
                     const War3RenderSettings& after, Overrides& overrides) {
  size_t written = 0;
  for (const Field& field : kFields) {
    if (SameValue(field, before, after))
      continue;
    overrides[field.key] = FormatFieldValue(field, after);
    ++written;
  }
  return written;
}

size_t RecordExtraChanges(const Extras& before, const Extras& after,
                          Overrides& overrides) {
  size_t written = 0;
  if (before.fpsUnlocked != after.fpsUnlocked) {
    overrides[kFpsUnlockedKey] = after.fpsUnlocked ? "1" : "0";
    ++written;
  }
  if (before.nativeShadowMode != after.nativeShadowMode) {
    overrides[kNativeShadowModeKey] =
        std::to_string((std::min)(after.nativeShadowMode, 2u));
    ++written;
  }
  return written;
}

void RestoreBaseline(Overrides& overrides,
                     const War3RenderSettings& baselineSettings,
                     const Extras& baselineExtras,
                     War3RenderSettings& settings, Extras& extras) {
  bool taaModeTouched = false;
  bool pointPcfTouched = false;
  for (auto it = overrides.begin(); it != overrides.end();) {
    const std::string& key = it->first;
    if (const Field* field = FindField(key)) {
      if (key == kTaaModeKey && !SameValue(*field, settings, baselineSettings))
        taaModeTouched = true;
      pointPcfTouched = pointPcfTouched || IsPointPcfKey(key);
      WriteNumber(*field, settings, ReadNumber(*field, baselineSettings));
      it = overrides.erase(it);
    } else if (key == kFpsUnlockedKey) {
      extras.fpsUnlocked = baselineExtras.fpsUnlocked;
      it = overrides.erase(it);
    } else if (key == kNativeShadowModeKey) {
      extras.nativeShadowMode = baselineExtras.nativeShadowMode;
      it = overrides.erase(it);
    } else {
      ++it;
    }
  }
  FixDerived(settings, taaModeTouched, pointPcfTouched);
}

bool IsKnownKey(const std::string& key) {
  return FindField(key) != nullptr || IsExtraKey(key);
}

size_t KnownKeyCount() { return std::size(kFields) + 2u; }

std::vector<std::string> KnownKeys() {
  std::vector<std::string> keys;
  keys.reserve(KnownKeyCount());
  for (const Field& field : kFields)
    keys.emplace_back(field.key);
  keys.emplace_back(kFpsUnlockedKey);
  keys.emplace_back(kNativeShadowModeKey);
  return keys;
}

std::string SerializeFile(const Overrides& overrides) {
  std::string text = "\xEF\xBB\xBF";
  text += "; WarVK 设置：在游戏里按 Ctrl+F1 修改后自动保存，下次启动沿用。\r\n";
  text += "; 只记录你改过的项。删除本文件或在面板里点“恢复默认设置”即可回到默认值。\r\n";
  text += "; 地图脚本仍可以在游戏中临时修改这些设置。\r\n";
  text += "[";
  text += kSectionName;
  text += "]\r\n";
  for (const auto& entry : overrides) {
    text += entry.first;
    text += '=';
    text += entry.second;
    text += "\r\n";
  }
  return text;
}

Overrides ParseFile(const std::string& raw, size_t* ignoredLines) {
  Overrides result;
  size_t ignored = 0;
  std::string text = raw;
  if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    text.erase(0, 3);
  bool inSection = false;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    line = Trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#')
      continue;
    if (line.front() == '[') {
      inSection = line == std::string("[") + kSectionName + "]";
      continue;
    }
    const size_t equals = line.find('=');
    if (!inSection || equals == std::string::npos) {
      ++ignored;
      continue;
    }
    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    if (!ValidKey(key) || !ValidValue(value) ||
        (result.size() >= kMaxEntries && result.find(key) == result.end())) {
      ++ignored;
      continue;
    }
    result[key] = value;
  }
  if (ignoredLines)
    *ignoredLines = ignored;
  return result;
}

std::string FormatDecimal(double value) {
  if (!std::isfinite(value))
    return "0";
  const bool negative = value < 0.0;
  double magnitude = negative ? -value : value;
  magnitude = (std::min)(magnitude, 1.0e12);
  const auto scaled =
      static_cast<unsigned long long>(std::llround(magnitude * 1000000.0));
  const unsigned long long whole = scaled / 1000000ull;
  unsigned long long fraction = scaled % 1000000ull;
  std::string out = (negative && scaled != 0ull) ? "-" : "";
  out += std::to_string(whole);
  if (fraction != 0ull) {
    std::string digits(6, '0');
    for (int i = 5; i >= 0; --i) {
      digits[size_t(i)] = char('0' + fraction % 10ull);
      fraction /= 10ull;
    }
    while (!digits.empty() && digits.back() == '0')
      digits.pop_back();
    out += '.';
    out += digits;
  }
  return out;
}

bool ParseDecimal(const std::string& raw, double& value) {
  const std::string text = Trim(raw);
  size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  double mantissa = 0.0;
  int digits = 0;
  int fractionDigits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    mantissa = mantissa * 10.0 + double(text[i] - '0');
    ++digits;
    ++i;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      mantissa = mantissa * 10.0 + double(text[i] - '0');
      ++digits;
      ++fractionDigits;
      ++i;
    }
  }
  if (digits == 0 || digits > 30)
    return false;
  int exponent = 0;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    bool expNegative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
      expNegative = text[i] == '-';
      ++i;
    }
    int expDigits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      exponent = exponent * 10 + (text[i] - '0');
      if (++expDigits > 3)
        return false;
      ++i;
    }
    if (expDigits == 0)
      return false;
    if (expNegative)
      exponent = -exponent;
  }
  if (i != text.size())
    return false;
  const double result =
      mantissa * std::pow(10.0, double(exponent - fractionDigits));
  if (!std::isfinite(result))
    return false;
  value = negative ? -result : result;
  return true;
}

bool ReadTextFile(const std::wstring& path, std::string& text) {
  text.clear();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
  }
  LARGE_INTEGER size = {};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
            size.QuadPart <= LONGLONG(kMaxFileBytes);
  if (ok && size.QuadPart > 0) {
    text.resize(size_t(size.QuadPart));
    DWORD read = 0;
    ok = ReadFile(file, text.data(), DWORD(text.size()), &read, nullptr) &&
         read == DWORD(text.size());
  }
  CloseHandle(file);
  if (!ok)
    text.clear();
  return ok;
}

bool WriteTextFileAtomically(const std::wstring& path, const std::string& text) {
  const std::wstring directory = ParentDirectory(path);
  if (!directory.empty())
    CreateDirectoryW(directory.c_str(), nullptr);
  const std::wstring temp = path + L".tmp";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  bool ok = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) &&
            written == DWORD(text.size());
  ok = CloseHandle(file) && ok;
  if (ok) {
    ok = MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  }
  if (!ok)
    DeleteFileW(temp.c_str());
  return ok;
}

bool DeleteFileIfPresent(const std::wstring& path) {
  if (DeleteFileW(path.c_str()))
    return true;
  const DWORD error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

std::wstring DefaultSettingsPath() {
  std::array<wchar_t, MAX_PATH * 2> buffer = {};
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
  std::wstring directory = L".";
  if (length != 0 && length < buffer.size()) {
    std::wstring exe(buffer.data(), length);
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
      directory = exe.substr(0, slash);
  }
  return directory + L"\\WarVK\\settings.ini";
}

ApplyReport ApplyAtStartup(War3RenderSettings& settings, Extras& extras) {
  StoreState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  LoadLocked(state);
  if (!state.baselineSettings) {
    state.baselineSettings = std::make_unique<War3RenderSettings>(settings);
    state.baselineExtras = extras;
  }
  return ApplyOverrides(state.overrides, settings, extras);
}

void RecordPanelEdit(const War3RenderSettings& before,
                     const War3RenderSettings& after) {
  StoreState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  LoadLocked(state);
  if (RecordChanges(before, after, state.overrides) != 0u) {
    state.dirty = true;
    state.lastChangeMs = NowMs();
  }
}

void RecordExtrasEdit(const Extras& before, const Extras& after) {
  StoreState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  LoadLocked(state);
  if (RecordExtraChanges(before, after, state.overrides) != 0u) {
    state.dirty = true;
    state.lastChangeMs = NowMs();
  }
}

bool ResetToBaseline(War3RenderSettings& settings, Extras& extras) {
  StoreState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  LoadLocked(state);
  if (!state.baselineSettings)
    return false;
  const bool anyKnown = std::any_of(
      state.overrides.begin(), state.overrides.end(),
      [](const auto& entry) { return IsKnownKey(entry.first); });
  if (!anyKnown)
    return false;
  RestoreBaseline(state.overrides, *state.baselineSettings,
                  state.baselineExtras, settings, extras);
  state.dirty = true;
  state.lastChangeMs = 0u;  // due on the next flush
  return true;
}

void FlushIfDue(bool force) {
  StoreState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.dirty)
    return;
  if (!force && NowMs() - state.lastChangeMs < kSaveDelayMs)
    return;
  // A failed save is not retried every frame; the next edit tries again.
  state.dirty = false;
  const bool ok = state.overrides.empty()
                      ? DeleteFileIfPresent(state.path)
                      : WriteTextFileAtomically(state.path,
                                                SerializeFile(state.overrides));
  state.lastSaveFailed = !ok;
}

StoreStatus GetStoreStatus() {
  StoreState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  StoreStatus status;
  status.savedKeys = size_t(std::count_if(
      state.overrides.begin(), state.overrides.end(),
      [](const auto& entry) { return IsKnownKey(entry.first); }));
  status.pending = state.dirty;
  status.lastSaveFailed = state.lastSaveFailed;
  return status;
}

void ResetStoreForTest(const std::wstring& path) {
  StoreState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.loaded = false;
  state.path = path;
  state.overrides.clear();
  state.dirty = false;
  state.lastSaveFailed = false;
  state.lastChangeMs = 0u;
  state.baselineSettings.reset();
  state.baselineExtras = Extras();
}

} // namespace dxvk::war3::player
