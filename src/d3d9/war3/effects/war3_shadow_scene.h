#pragma once

// Scene packet records of the shadow pass.
//
// The shadow pass (d3d9_war3_shadow.cpp) is one source built into the x86
// device and into the x64 host. Besides the frame's targets and the world
// camera (war3_effect_scene.h) it reads three kinds of x86 data-layer facts,
// and these are their layouts:
//
//   Lighting      FrameScene record. The authored sun, shadow, day/night and
//                 ambient settings: War3RenderSettings::{sun, shadows,
//                 dayNight, ambient}, every field.
//   ShadowFrame   FrameScene record. What the data layer knows about this
//                 frame's casters as a whole, and its lifecycle serials.
//   ShadowCaster  draw record (AnnotateDraws). Marks the draw it is bound to
//                 as a shadow caster and says what the draw message does not:
//                 its world transform, bounds, class and identity. The
//                 geometry is the draw's own vertex and index buffers.
//   ShadowCasterAlpha
//                 draw record after the draw's ShadowCaster record: the caster
//                 is alpha-tested, with the threshold, texture stage and
//                 texture coordinates it names. The texture is the draw's own.
//   PointLights   FrameScene record. The data layer's active point lights of
//                 the frame (War3LightManager): where, how far, what colour,
//                 and whether they cast shadows.
//   Outline       FrameScene record. The unit outline's settings
//                 (War3RenderSettings::occludedOutline) and the handles of the
//                 objects to outline (War3RenderState's outline handles).
//   VolumetricLight
//                 FrameScene record. The volumetric light's settings
//                 (War3RenderSettings::postFx.volumetricLight), every field.
//                 The volumetric light pass (d3d9_war3_volumetric_light.cpp)
//                 runs after the shadow pass, from the shadow pass's maps.
//   FogVolumes    FrameScene record. The map's fog volumes
//                 (War3FogVolumeManager), as authored; the volumetric light
//                 scatters in them.
//
// The rules of war3_effect_scene.h hold: values only, complete, per frame,
// little-endian words. Without a Lighting record the pass does not run.
//
// This header has no dependency beyond war3_effect_scene.h.

#include "war3_effect_scene.h"
#include <algorithm>
#include <limits>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace dxvk::war3::effects::scene {

constexpr uint32_t LightingSchema = 9;
constexpr uint32_t ShadowFrameSchema = 10;
constexpr uint32_t ShadowCasterSchema = 11;
constexpr uint32_t ShadowCasterAlphaSchema = 12;
constexpr uint32_t PointLightsSchema = 13;
constexpr uint32_t OutlineSchema = 14;
constexpr uint32_t VolumetricLightSchema = 15;
constexpr uint32_t FogVolumesSchema = 16;

// ---- Lighting ---------------------------------------------------------------
//
// One row per field, in wire order: name of the record member, the path of
// the field in War3RenderSettings, its default there. Kinds:
//   B  bool, one word, 0 or 1          U  uint32_t, one word
//   Q  uint64_t, two words             F  float, one word, finite
//   E  enum, one word, 0..max          V  Vector4, four words, finite
// The table is the layout. war3_shadow_scene_input.h checks that it names
// every field of the three settings structs and that the defaults agree.
#define WAR3_SCENE_LIGHTING_FIELDS(B, U, Q, F, E, V) \
  B(sunEnabled, sun.enabled, 1) \
  V(sunDirection, sun.direction, -0.3f, -1.0f, -0.2f, 0.0f) \
  V(sunColor, sun.color, 1.0f, 1.0f, 1.0f, 0.0f) \
  F(sunIntensity, sun.intensity, 1.0f) \
  B(shadowsEnabled, shadows.enabled, 1) \
  U(csmCascadeCount, shadows.csm.cascadeCount, 4) \
  U(csmShadowResolution, shadows.csm.shadowResolution, 4096) \
  F(csmSplitLambda, shadows.csm.splitLambda, 0.5f) \
  F(csmMaxDistance, shadows.csm.maxDistance, 4000.0f) \
  F(csmStableSnap, shadows.csm.stableSnap, 1.0f) \
  F(csmDepthRangeMargin, shadows.csm.depthRangeMargin, 50.0f) \
  F(csmFarCasterDepthExtension, shadows.csm.farCasterDepthExtension, 0.0f) \
  E(csmFitMode, shadows.csm.fitMode, 0, 2) \
  F(csmRtsReceiverPlaneHeight, shadows.csm.rtsReceiverPlaneHeight, 0.0f) \
  F(csmRtsReceiverBandHalfHeight, shadows.csm.rtsReceiverBandHalfHeight, 512.0f) \
  F(csmRtsReceiverPadding, shadows.csm.rtsReceiverPadding, 64.0f) \
  F(csmRtsBaseWorldTexelSize, shadows.csm.rtsBaseWorldTexelSize, 0.25f) \
  F(shadowStrength, shadows.strength, 0.75f) \
  F(pcfRadius, shadows.pcfRadius, 0.70f) \
  F(receiverBias, shadows.receiverBias, 0.0004f) \
  F(cascadeBlendRange, shadows.cascadeBlendRange, 120.0f) \
  E(pcfKernel, shadows.pcfKernel, 2, 3) \
  B(pcfRotate, shadows.pcfRotate, 0) \
  E(pcfRotateMode, shadows.pcfRotateMode, 0, 2) \
  F(cascadeBiasScale, shadows.cascadeBiasScale, 0.0f) \
  F(pcfCascadeRadiusScale, shadows.pcfCascadeRadiusScale, 0.5f) \
  F(casterDepthBias, shadows.casterDepthBias, 1.0f) \
  F(casterSlopeBias, shadows.casterSlopeBias, 1.5f) \
  F(casterBiasClamp, shadows.casterBiasClamp, 0.001f) \
  B(pcssEnabled, shadows.pcssEnabled, 0) \
  F(pcssSearchRadius, shadows.pcssSearchRadius, 6.0f) \
  F(pcssMinRadius, shadows.pcssMinRadius, 1.0f) \
  F(pcssMaxRadius, shadows.pcssMaxRadius, 3.0f) \
  F(pcssDepthScale, shadows.pcssDepthScale, 30.0f) \
  E(pcssSearchKernel, shadows.pcssSearchKernel, 0, 1) \
  B(alphaShadowHashed, shadows.alphaShadowHashed, 0) \
  B(alphaShadowUseMip, shadows.alphaShadowUseMip, 0) \
  F(alphaShadowMipLodBias, shadows.alphaShadowMipLodBias, 0.0f) \
  F(alphaShadowFarAlphaRefBias, shadows.alphaShadowFarAlphaRefBias, 0.0f) \
  E(shadowTaaMode, shadows.shadowTaaMode, 0, 2) \
  B(shadowTaaEnabled, shadows.shadowTaaEnabled, 0) \
  Q(shadowTaaSettingsRevision, shadows.shadowTaaSettingsRevision, 0) \
  F(shadowTaaBlendFactor, shadows.shadowTaaBlendFactor, 0.20f) \
  B(shadowTaaNeighborClamp, shadows.shadowTaaNeighborClamp, 1) \
  B(pointLightsEnabled, shadows.pointLightsEnabled, 0) \
  U(pointShadowMaxLights, shadows.pointShadowMaxLights, 1) \
  U(pointShadowResolution, shadows.pointShadowResolution, 1024) \
  U(pointShadowDebugLightIndex, shadows.pointShadowDebugLightIndex, 0) \
  F(pointShadowBias, shadows.pointShadowBias, 0.05f) \
  F(pointShadowPcfRadiusNear, shadows.pointShadowPcfRadiusNear, 0.65f) \
  F(pointShadowPcfRadiusFar, shadows.pointShadowPcfRadiusFar, 1.15f) \
  F(pointShadowTexelBiasScale, shadows.pointShadowTexelBiasScale, 0.50f) \
  F(pointShadowRangeFadeStart, shadows.pointShadowRangeFadeStart, 0.78f) \
  B(pointShadowEnabled, shadows.pointShadowEnabled, 0) \
  B(pointRayShadowEnabled, shadows.pointRayShadowEnabled, 0) \
  B(pointRayShadowHiZEnabled, shadows.pointRayShadowHiZEnabled, 1) \
  U(pointRayShadowMaxLights, shadows.pointRayShadowMaxLights, 1) \
  U(pointRayShadowSteps, shadows.pointRayShadowSteps, 12) \
  U(pointRayShadowHiZMaxVisits, shadows.pointRayShadowHiZMaxVisits, 24) \
  F(pointRayShadowMaxDistance, shadows.pointRayShadowMaxDistance, 480.0f) \
  F(pointRayShadowThickness, shadows.pointRayShadowThickness, 24.0f) \
  F(pointRayShadowStartOffset, shadows.pointRayShadowStartOffset, 10.0f) \
  F(pointRayShadowStrength, shadows.pointRayShadowStrength, 0.85f) \
  B(pointShadowTemporalReuse, shadows.pointShadowTemporalReuse, 0) \
  U(pointShadowUpdatePeriod, shadows.pointShadowUpdatePeriod, 1) \
  F(pointShadowCasterCullPadding, shadows.pointShadowCasterCullPadding, 1.20f) \
  U(pointShadowMaxCastersPerFace, shadows.pointShadowMaxCastersPerFace, 0) \
  B(pointShadowFaceCulling, shadows.pointShadowFaceCulling, 1) \
  U(pointShadowMaxFacesPerFrame, shadows.pointShadowMaxFacesPerFrame, 6) \
  E(receiverMode, shadows.receiverMode, 1, 2) \
  F(normalBiasScale, shadows.normalBiasScale, 0.0005f) \
  F(rimIntensity, shadows.rimIntensity, 0.0f) \
  F(rimPower, shadows.rimPower, 2.0f) \
  E(altitudeMode, shadows.altitudeMode, 0, 1) \
  F(shadowLengthScale, shadows.shadowLengthScale, 0.9f) \
  F(shadowMaxLengthScale, shadows.shadowMaxLengthScale, 0.8f) \
  E(filterMode, shadows.filterMode, 1, 1) \
  F(nightShadowScale, shadows.nightShadowScale, 1.0f) \
  B(lockSun, shadows.lockSun, 0) \
  F(lockSunTime, shadows.lockSunTime, 0.5f) \
  B(stableSnapWhenSunMoving, shadows.stableSnapWhenSunMoving, 1) \
  E(shadowDebugMode, shadows.debugMode, 0, 9) \
  B(dayNightEnabled, dayNight.enabled, 1) \
  B(celestialMotionEnabled, dayNight.celestialMotionEnabled, 1) \
  B(timeColorGradingEnabled, dayNight.timeColorGradingEnabled, 1) \
  E(clockMode, dayNight.clockMode, 0, 2) \
  F(renderTimeHours, dayNight.renderTimeHours, 12.0f) \
  F(independentDayLengthSeconds, dayNight.independentDayLengthSeconds, 480.0f) \
  Q(clockRevision, dayNight.clockRevision, 1) \
  B(customColorTemperatureProfile, dayNight.customColorTemperatureProfile, 0) \
  F(midnightKelvin, dayNight.midnightKelvin, 9000.0f) \
  F(dawnKelvin, dayNight.dawnKelvin, 2500.0f) \
  F(noonKelvin, dayNight.noonKelvin, 6500.0f) \
  F(duskKelvin, dayNight.duskKelvin, 2500.0f) \
  B(affectAmbient, dayNight.affectAmbient, 1) \
  B(affectExposure, dayNight.affectExposure, 0) \
  F(transitionMinFactor, dayNight.transitionMinFactor, 0.85f) \
  V(dayAmbient, dayNight.dayAmbient, 0.28f, 0.28f, 0.28f, 1.0f) \
  V(nightAmbient, dayNight.nightAmbient, 0.08f, 0.10f, 0.16f, 1.0f) \
  F(dayExposure, dayNight.dayExposure, 1.0f) \
  F(nightExposure, dayNight.nightExposure, 1.0f) \
  F(dayTransitionStartDeg, dayNight.dayTransitionStartDeg, -2.0f) \
  F(dayTransitionEndDeg, dayNight.dayTransitionEndDeg, 8.0f) \
  B(ambientOverrideEnabled, ambient.overrideEnabled, 0) \
  B(ambientAutoOverride, ambient.autoOverride, 0) \
  V(ambientColor, ambient.color, 0.0f, 0.0f, 0.0f, 0.0f)

#define WAR3_SL_WORDS_1(...) + 1
#define WAR3_SL_WORDS_2(...) + 2
#define WAR3_SL_WORDS_4(...) + 4
constexpr uint32_t LightingWords = 0 WAR3_SCENE_LIGHTING_FIELDS(
    WAR3_SL_WORDS_1, WAR3_SL_WORDS_1, WAR3_SL_WORDS_2, WAR3_SL_WORDS_1, WAR3_SL_WORDS_1, WAR3_SL_WORDS_4);
#undef WAR3_SL_WORDS_1
#undef WAR3_SL_WORDS_2
#undef WAR3_SL_WORDS_4
constexpr uint32_t LightingBytes = LightingWords * 4;

struct Lighting {
#define WAR3_SL_WORD(name, path, value, ...) uint32_t name = value;
#define WAR3_SL_QUAD(name, path, value) uint64_t name = value;
#define WAR3_SL_FLOAT(name, path, value) float name = value;
#define WAR3_SL_VECTOR(name, path, x, y, z, w) std::array<float, 4> name = { x, y, z, w };
  WAR3_SCENE_LIGHTING_FIELDS(WAR3_SL_WORD, WAR3_SL_WORD, WAR3_SL_QUAD, WAR3_SL_FLOAT, WAR3_SL_WORD, WAR3_SL_VECTOR)
#undef WAR3_SL_WORD
#undef WAR3_SL_QUAD
#undef WAR3_SL_FLOAT
#undef WAR3_SL_VECTOR
};

inline std::array<uint8_t, LightingBytes> EncodeLighting(const Lighting& l) {
  std::array<uint8_t, LightingBytes> p{}; uint32_t at = 0;
  const auto word = [&](uint32_t v) { detail::Put32(p.data() + at, v); at += 4; };
  const auto real = [&](float v) { detail::PutFloat(p.data() + at, v); at += 4; };
#define WAR3_SL_WORD(name, ...) word(l.name);
#define WAR3_SL_QUAD(name, ...) word(uint32_t(l.name)); word(uint32_t(l.name >> 32));
#define WAR3_SL_FLOAT(name, ...) real(l.name);
#define WAR3_SL_VECTOR(name, ...) for (float v : l.name) real(v);
  WAR3_SCENE_LIGHTING_FIELDS(WAR3_SL_WORD, WAR3_SL_WORD, WAR3_SL_QUAD, WAR3_SL_FLOAT, WAR3_SL_WORD, WAR3_SL_VECTOR)
#undef WAR3_SL_WORD
#undef WAR3_SL_QUAD
#undef WAR3_SL_FLOAT
#undef WAR3_SL_VECTOR
  return p;
}

// False for a switch that is not 0 or 1, an enum outside its range or a
// number that is not finite. The pass clamps the numbers it uses.
inline bool DecodeLighting(const uint8_t* p, uint32_t bytes, Lighting& out) {
  if (!p || bytes != LightingBytes) return false;
  Lighting l; uint32_t at = 0; bool valid = true;
  const auto word = [&]() { const uint32_t v = detail::Get32(p + at); at += 4; return v; };
  const auto real = [&]() { const float v = detail::GetFloat(p + at); at += 4; valid = valid && std::isfinite(v); return v; };
#define WAR3_SL_BOOL(name, ...) l.name = word(); valid = valid && l.name <= 1;
#define WAR3_SL_WORD(name, ...) l.name = word();
#define WAR3_SL_QUAD(name, ...) l.name = word(); l.name |= uint64_t(word()) << 32;
#define WAR3_SL_FLOAT(name, ...) l.name = real();
#define WAR3_SL_ENUM(name, path, value, max) l.name = word(); valid = valid && l.name <= max;
#define WAR3_SL_VECTOR(name, ...) for (float& v : l.name) v = real();
  WAR3_SCENE_LIGHTING_FIELDS(WAR3_SL_BOOL, WAR3_SL_WORD, WAR3_SL_QUAD, WAR3_SL_FLOAT, WAR3_SL_ENUM, WAR3_SL_VECTOR)
#undef WAR3_SL_BOOL
#undef WAR3_SL_WORD
#undef WAR3_SL_QUAD
#undef WAR3_SL_FLOAT
#undef WAR3_SL_ENUM
#undef WAR3_SL_VECTOR
  if (!valid) return false;
  out = l; return true;
}

// ---- ShadowFrame ------------------------------------------------------------

constexpr uint32_t ShadowFrameBytes = 14 * 4;
// War3RequiredCasterOmissionReason: the bits a producer may report.
constexpr uint32_t ShadowOmissionReasons = 0xff;

struct ShadowFrame {
  // The game's TIME_OF_DAY in hours, 0..24; negative when it cannot be read
  // yet. Only the GameTime lighting clock uses it.
  float timeOfDay = -1.0f;
  // One bit per War3RuntimeModule the runtime profile turned off.
  uint32_t runtimeDisabledModules = 0;
  // A change of either one invalidates what the pass kept of earlier frames:
  // a caster was removed for good, or the stage policy changed.
  uint64_t tombstoneSerial = 0, stagePolicyRevision = 0;
  // ShadowCaster records sent for this frame. The executor compares it with
  // what it received and could use; a difference is an incomplete frame.
  uint32_t casters = 0;
  // Required casters the data layer itself could not send, and why.
  uint32_t omittedCasters = 0, omissionReasons = 0;
  // Animated casters of the frame: how many poses, how many skinned outputs,
  // and a signature that changes when any pose does.
  uint32_t dynamicPoseCount = 0, dynamicSkinnedOutputCount = 0;
  uint64_t dynamicPoseSignature = 0;
};

inline std::array<uint8_t, ShadowFrameBytes> EncodeShadowFrame(const ShadowFrame& f) {
  std::array<uint8_t, ShadowFrameBytes> p{};
  detail::PutFloat(p.data(), f.timeOfDay);
  detail::Put32(p.data() + 4, f.runtimeDisabledModules);
  detail::Put32(p.data() + 8, uint32_t(f.tombstoneSerial)); detail::Put32(p.data() + 12, uint32_t(f.tombstoneSerial >> 32));
  detail::Put32(p.data() + 16, uint32_t(f.stagePolicyRevision)); detail::Put32(p.data() + 20, uint32_t(f.stagePolicyRevision >> 32));
  detail::Put32(p.data() + 24, f.casters);
  detail::Put32(p.data() + 28, f.omittedCasters);
  detail::Put32(p.data() + 32, f.omissionReasons);
  detail::Put32(p.data() + 36, f.dynamicPoseCount);
  detail::Put32(p.data() + 40, f.dynamicSkinnedOutputCount);
  // word 11 is reserved and zero
  detail::Put32(p.data() + 48, uint32_t(f.dynamicPoseSignature)); detail::Put32(p.data() + 52, uint32_t(f.dynamicPoseSignature >> 32));
  return p;
}

inline bool DecodeShadowFrame(const uint8_t* p, uint32_t bytes, ShadowFrame& out) {
  if (!p || bytes != ShadowFrameBytes) return false;
  ShadowFrame f;
  f.timeOfDay = detail::GetFloat(p);
  f.runtimeDisabledModules = detail::Get32(p + 4);
  f.tombstoneSerial = uint64_t(detail::Get32(p + 8)) | (uint64_t(detail::Get32(p + 12)) << 32);
  f.stagePolicyRevision = uint64_t(detail::Get32(p + 16)) | (uint64_t(detail::Get32(p + 20)) << 32);
  f.casters = detail::Get32(p + 24);
  f.omittedCasters = detail::Get32(p + 28);
  f.omissionReasons = detail::Get32(p + 32);
  f.dynamicPoseCount = detail::Get32(p + 36);
  f.dynamicSkinnedOutputCount = detail::Get32(p + 40);
  f.dynamicPoseSignature = uint64_t(detail::Get32(p + 48)) | (uint64_t(detail::Get32(p + 52)) << 32);
  // Omissions and their reasons come together.
  if (!std::isfinite(f.timeOfDay) || detail::Get32(p + 44) || (f.omissionReasons & ~ShadowOmissionReasons)
      || (f.omittedCasters != 0) != (f.omissionReasons != 0)) return false;
  out = f; return true;
}

// ---- ShadowCaster -----------------------------------------------------------

constexpr uint32_t ShadowCasterBytes = (16 + 4) * 4 + 4 + 3 * 4 + 4 + 4 * 4 + 2 * 8 + 4;

// How the caster's vertices reach world space (War3ShadowReplayMode). Only
// FixedWorld has a record so far: object-space positions and one matrix.
constexpr uint32_t ShadowModeFixedWorld = 1;
// War3RenderState::StageCategory, War3BatchTag (-1 is Unknown) and
// war3::render::ObjectKind value ranges.
constexpr uint32_t ShadowCategoryLast = 6, ShadowObjectKindLast = 5;
constexpr int32_t ShadowBatchTagFirst = -1, ShadowBatchTagLast = 12;
// War3ShadowPartLifecycleState and War3ShadowBoundsProvenance.
constexpr uint32_t ShadowLifecycleLast = 3, ShadowBoundsProvenanceLast = 4;
// boundsAge of bounds whose frame is not known.
constexpr uint32_t ShadowBoundsAgeUnknown = 0xffffffffu;

// Facts of the draw's render state and of the data layer's knowledge of it.
namespace ShadowCasterFlag {
constexpr uint32_t AlphaBlend = 1u << 0;        // D3DRS_ALPHABLENDENABLE
constexpr uint32_t DepthWrite = 1u << 1;        // D3DRS_ZWRITEENABLE
constexpr uint32_t DepthTest = 1u << 2;         // D3DRS_ZENABLE
constexpr uint32_t Additive = 1u << 3;          // an additive blend: glow, not a surface
constexpr uint32_t PathBlocker = 1u << 4;       // a pathing / line-of-sight blocker: casts nothing
constexpr uint32_t PathBlockerMarker = 1u << 5; // its hidden marker geometry
constexpr uint32_t UnitIdentityProven = 1u << 6;
constexpr uint32_t BoundsIdentityProven = 1u << 7;
constexpr uint32_t BoundsSourceWasSkinned = 1u << 8;
constexpr uint32_t BoundsFrameLocalDynamic = 1u << 9;
constexpr uint32_t BoundsAnimatedAttachment = 1u << 10;
// No further bit: the frozen f8 front decodes every record with the mask it
// was built with and refuses a record with any other bit set.
constexpr uint32_t All = (1u << 11) - 1;
} // namespace ShadowCasterFlag

// The game's water draw as a ShadowCaster record: it casts nothing; the host
// replays it for the depth of the water's surface (War3FrameScene::
// waterSurfaces). Marked by identity words the front accepts unchanged, not
// by a flag (see All): a rawcode no object of the editor can have ('~' is not
// in its alphabet), every bit of the part, the unknown stage and category.
constexpr uint32_t WaterSurfaceRawcode = 0x7E574154u; // "~WAT"
constexpr uint32_t WaterSurfacePart = 0xFFFFFFFFu;

// What an executor may copy of one frame's casters out of dynamic buffers, and
// the chunks it copies into. Only the host knows which slices are reused.
// The producer counts declared demand and caps the number of records.
constexpr uint64_t CasterCopyFrameBytes = uint64_t(64) << 20;
constexpr uint64_t CasterCopyChunkBytes = uint64_t(1) << 20;
constexpr uint32_t CasterFrameRecordLimit = 8192;

inline constexpr uint32_t CasterVertexStride(bool alpha) { return alpha ? 20u : 12u; }

enum class CasterClass : uint32_t { Unit, Building, Doodad, Destructible, Other, Terrain };

// Values of StageCategory and ObjectKind in ShadowCaster. Terrain tiles take
// precedence over any stale batch identity. Stage 10 also contains real objects.
inline CasterClass ClassifyCaster(uint32_t category, int stage, uint32_t kind, bool unitProven) {
  if (category == 2 && stage != 10) return CasterClass::Terrain;
  switch (kind) {
    case 1: return CasterClass::Unit;
    case 2: return CasterClass::Building;
    case 3: return CasterClass::Destructible;
    case 4: case 5: return CasterClass::Other;
  }
  if (stage == 10 && (category == 2 || category == 3)) return CasterClass::Doodad;
  return unitProven ? CasterClass::Unit : CasterClass::Other;
}

inline float CasterHeightScale(float limit, float x, float y, float z) {
  if (!(limit > 0)) return 1;
  const float xy = std::hypot(x, y);
  return xy > 0 ? std::min(1.0f, limit * std::abs(z) / xy) : 1.0f;
}

struct CasterPartBase {
  uint32_t handle;
  float origin, minimum;
  static constexpr float unknown = std::numeric_limits<float>::infinity();
  float own(bool preferOrigin = true) const { return Pick(origin, minimum, preferOrigin); }
  static float Pick(float origin, float minimum, bool preferOrigin) {
    if (preferOrigin) return std::isfinite(origin) ? origin : minimum;
    return std::isfinite(minimum) ? minimum : origin;
  }
};

// Where an object stands. A building, a tree or a doodad is placed at its base,
// and its model often reaches below the ground (foundations, roots): its placed
// origin is the base. A unit is drawn in world space, skinned, with no placed
// origin, and its lowest vertex is its feet (measured in game, 2026-11-19).
inline bool CasterBasePrefersOrigin(CasterClass cls) {
  return cls == CasterClass::Building || cls == CasterClass::Doodad || cls == CasterClass::Destructible;
}

// Clear only occupied slots. Size the table once for all parts of this frame
// so next frame's admission can also look up a height in constant expected time.
// Per object, the lowest placed origin and the lowest exact bound of its parts.
class CasterBases {
  struct Entry { uint32_t handle = 0; float origin = 0, minimum = 0; };
  std::vector<Entry> table;
  std::vector<size_t> occupied;
  size_t slot(uint32_t handle) const {
    const uint32_t key = (handle ^ (handle >> 16)) * 0x9e3779b9u;
    size_t i = (key ^ (key >> 16)) & (table.size() - 1);
    while (table[i].handle && table[i].handle != handle)
      i = (i + 1) & (table.size() - 1);
    return i;
  }
public:
  void reset(size_t parts) {
    for (size_t i : occupied) table[i].handle = 0;
    occupied.clear();
    size_t size = 2;
    while (size / 2 < parts) size *= 2;
    if (size > table.size()) table.resize(size);
  }
  void add(CasterPartBase part) {
    if (!part.handle || !(std::isfinite(part.origin) || std::isfinite(part.minimum))) return;
    const size_t i = slot(part.handle);
    if (table[i].handle) {
      table[i].origin = std::min(table[i].origin, part.origin);
      table[i].minimum = std::min(table[i].minimum, part.minimum);
    } else {
      table[i] = {part.handle, part.origin, part.minimum};
      occupied.push_back(i);
    }
  }
  float resolve(CasterPartBase part, bool preferOrigin) const {
    if (part.handle && !table.empty()) {
      const auto& e = table[slot(part.handle)];
      if (e.handle) return CasterPartBase::Pick(e.origin, e.minimum, preferOrigin);
    }
    return part.own(preferOrigin);
  }
};

inline CasterPartBase CasterBase(uint32_t handle, float x, float y, float z, float minimum) {
  const bool placed = (x || y || z) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
  return {handle, placed ? z : CasterPartBase::unknown, minimum};
}

template<class Matrix>
inline Matrix CasterLightMatrix(const Matrix& light, float k, float z0) {
  if (k == 1 || !std::isfinite(z0)) return light;
  // Columns 2 and 3 of light * S. Leave X and Y byte-identical.
  Matrix result = light;
  for (unsigned j = 0; j < 4; ++j) {
    result[2][j] = light[2][j] * k;
    result[3][j] += light[2][j] * ((1 - k) * z0);
  }
  return result;
}

inline std::array<float, 6> ExtendCasterBox(std::array<float, 6> box, float z0) {
  if (std::isfinite(z0)) {
    box[2] = std::min(box[2], z0);
    box[5] = std::max(box[5], z0);
  }
  return box;
}

struct CasterCopySizes {
  uint64_t vertex, index;
  static CasterCopySizes forDraw(uint32_t dynamicVertices, bool alpha, uint32_t dynamicIndices, uint32_t indexSize) {
    return {uint64_t(dynamicVertices) * CasterVertexStride(alpha), uint64_t(dynamicIndices) * indexSize};
  }
};

// A nonzero newChunk starts a buffer. Otherwise offset refers to the current
// buffer. Empty streams do not advance the cursor.
struct CasterPlacement { uint64_t newChunk = 0, offset = 0; };
struct CasterCopyPlan { CasterPlacement vertex, index; };

struct CasterCopyAccount {
  uint64_t frameBytes = 0, chunkBytes = 0, used = 0;

  bool append(CasterCopySizes sizes, uint64_t limit, CasterCopyPlan& plan) {
    if (frameBytes > limit)
      return false;
    CasterCopyAccount next = *this;
    CasterCopyPlan placements;
    if (!next.place(sizes.vertex, limit, placements.vertex) || !next.place(sizes.index, limit, placements.index))
      return false;
    *this = next;
    plan = placements;
    return true;
  }

private:
  bool place(uint64_t size, uint64_t limit, CasterPlacement& place) {
    if (size > UINT64_MAX - 15u)
      return false;
    const uint64_t aligned = (size + 15u) & ~uint64_t(15u);
    if (aligned > chunkBytes - used) {
      chunkBytes = aligned > CasterCopyChunkBytes ? aligned : CasterCopyChunkBytes;
      if (chunkBytes > limit - frameBytes)
        return false;
      frameBytes += chunkBytes;
      used = 0;
      place.newChunk = chunkBytes;
    }
    place.offset = used;
    used += aligned;
    return true;
  }
};

// Keep counting declared demand beyond the record count cap.
struct CasterPoolMirror {
  CasterCopyAccount demand;
  uint32_t casters = 0, omitted = 0;

  bool wants(CasterCopySizes sizes, uint32_t countLimit) {
    CasterCopyPlan plan;
    ++casters;
    if (!demand.append(sizes, UINT64_MAX, plan) || casters > countLimit) {
      ++omitted;
      return false;
    }
    return true;
  }
};

// The caller checked that both attributes fit in the source stride. Copy
// directly into the mapped slice. Keep layout decisions outside the loop.
inline void CopyCasterVertices(uint8_t* dst, const uint8_t* src, uint32_t count, uint32_t stride,
    uint32_t position, bool alpha, uint32_t uv) {
  const uint32_t packed = CasterVertexStride(alpha);
  if (stride == packed && !position && (!alpha || uv == 12u)) {
    std::memcpy(dst, src, size_t(count) * packed);
  } else if (alpha) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* vertex = src + size_t(i) * stride;
      std::memcpy(dst + size_t(i) * 20u, vertex + position, 12);
      std::memcpy(dst + size_t(i) * 20u + 12u, vertex + uv, 8);
    }
  } else {
    for (uint32_t i = 0; i < count; ++i)
      std::memcpy(dst + size_t(i) * 12u, src + size_t(i) * stride + position, 12);
  }
}

struct CasterIndexSpan {
  uint32_t min = UINT16_MAX, max = 0;

  void include(uint16_t index) {
    if (index < min) min = index;
    if (index > max) max = index;
  }
  uint32_t count() const { return max >= min ? max - min + 1u : 0u; }
};

// With the source IB, index i addresses i - minimum in the copied VB.
// With a normalized IB, add the minimum back to the replay base.
struct CasterIndexedAddress {
  int64_t firstVertex;
  int32_t replayBase;
  int32_t rebased(uint32_t indexMinimum) const { return int32_t(int64_t(replayBase) + indexMinimum); }
  static CasterIndexedAddress from(int32_t base, uint32_t minimum, bool copied) {
    return {int64_t(base) + minimum, copied ? int32_t(-int64_t(minimum)) : base};
  }
};

inline bool CasterVerticesReadable(uint64_t bytes, uint64_t offset, int64_t first, uint32_t count,
    uint32_t stride, uint32_t extent) {
  return first >= 0 && count && extent && extent <= stride && offset <= bytes && extent <= bytes - offset
      && uint64_t(first) + count - 1u <= (bytes - offset - extent) / stride;
}

// Normalize before hashing so copies with different source base indices share.
// A zero minimum already has the required byte representation.
inline const uint8_t* RebaseCasterIndices(std::vector<uint16_t>& scratch, const uint8_t* src,
    uint32_t count, uint32_t minimum) {
  if (!minimum) return src;
  scratch.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t value;
    std::memcpy(&value, src + size_t(i) * 2, 2);
    scratch[i] = uint16_t(value - minimum);
  }
  return reinterpret_cast<const uint8_t*>(scratch.data());
}

inline uint64_t CasterIndexHash(const uint8_t* data, size_t bytes) {
  uint64_t hash = 0xcbf29ce484222325ull ^ bytes;
  while (bytes >= 8) {
    uint64_t word;
    std::memcpy(&word, data, 8);
    hash = (hash ^ word) * 0x100000001b3ull;
    data += 8;
    bytes -= 8;
  }
  uint64_t tail = 0;
  std::memcpy(&tail, data, bytes);
  hash = (hash ^ tail) * 0x9e3779b97f4a7c15ull;
  return hash ^ (hash >> 32);
}

// The bytes a hit is compared with are this table's own copy in system memory.
// The slice itself lives in host-visible GPU memory, which is write-combined:
// reading it back costs about a microsecond per hundred bytes (measured in game,
// 2026-11-19: 3 ms per frame for 460 KiB of hits). The copy keeps its capacity.
template<class Slice>
class CasterIndexTable {
  static constexpr unsigned Buckets = CasterFrameRecordLimit * 2;
  struct Entry { uint64_t hash; size_t offset, size; Slice slice; unsigned bucket; };
  std::vector<uint16_t> buckets;
  std::vector<Entry> entries;
  std::vector<uint8_t> bytesCopy;
public:
  unsigned find(uint64_t hash, const uint8_t* bytes, size_t size, Slice& hit) {
    if (buckets.empty()) buckets.resize(Buckets);
    unsigned slot = unsigned(hash) & (Buckets - 1);
    while (buckets[slot]) {
      const auto& e = entries[buckets[slot] - 1];
      if (e.hash == hash && e.size == size && !std::memcmp(bytesCopy.data() + e.offset, bytes, size)) {
        hit = e.slice;
        break;
      }
      slot = (slot + 1) & (Buckets - 1);
    }
    return slot;
  }
  // bytes: the content as written to the slice, in system memory.
  void insert(unsigned bucket, uint64_t hash, const uint8_t* bytes, size_t size, const Slice& slice) {
    if (entries.size() == CasterFrameRecordLimit || buckets[bucket]) return;
    const size_t offset = bytesCopy.size();
    bytesCopy.insert(bytesCopy.end(), bytes, bytes + size);
    entries.push_back({hash, offset, size, slice, bucket});
    buckets[bucket] = uint16_t(entries.size());
  }
  void clear() {
    for (const auto& e : entries) buckets[e.bucket] = 0;
    entries.clear();
    bytesCopy.clear();
  }
};

// One most recent slice per stream. A new slot incarnation must be uploaded
// before its first draw. The owner numbers uploads across all slots.
struct CasterSliceKey {
  uint64_t generation = 0, offset = 0;
  uint32_t slot = 0, count = 0, stride = 0, position = 0, uv = UINT32_MAX;

  bool matches(const CasterSliceKey& other) const {
    return generation && generation == other.generation && slot == other.slot && offset == other.offset
        && count == other.count && stride == other.stride && position == other.position && uv == other.uv;
  }
};

template<typename Slice>
struct CasterRecentSlice {
  CasterSliceKey key;
  Slice slice;

  bool find(const CasterSliceKey& request, Slice& result) const {
    if (!key.matches(request))
      return false;
    result = slice;
    return true;
  }
};

// Host replay facts, never serialized. Low XYZ followed by high XYZ.
using CasterBox = std::array<float, 6>;
struct CasterLocalBounds {
  CasterBox box{};
  bool known = false;
};

// src starts at the copied VB span (including the signed base vertex).
// Holes inside a frozen index span are not vertices of this draw. Without
// immutable indices, include every position that the replay may address.
inline CasterLocalBounds ReadCasterBounds(const uint8_t* src, uint32_t stride, uint32_t count,
    const uint8_t* indices, uint32_t elements, uint32_t minimum) {
  CasterLocalBounds result;
  if (!src || !count) return result;
#if defined(__x86_64__) || defined(_M_X64)
  const __m128 infinity = _mm_set1_ps(std::numeric_limits<float>::infinity());
  __m128 low = infinity, high = _mm_sub_ps(_mm_setzero_ps(), infinity);
  __m128 invalid = _mm_setzero_ps();
#endif
  for (uint32_t i = 0; i < (indices ? elements : count); ++i) {
    uint32_t vertex = i;
    if (indices) {
      uint16_t index;
      std::memcpy(&index, indices + size_t(i) * 2, 2);
      vertex = uint32_t(index) - minimum;
      if (vertex >= count) return {};
    }
    const auto* position = src + size_t(vertex) * stride;
#if defined(__x86_64__) || defined(_M_X64)
    // The last float3 may end at the allocation boundary.
    __m128 p = _mm_setzero_ps();
    std::memcpy(&p, position, 12);
    low = _mm_min_ps(low, p);
    high = _mm_max_ps(high, p);
    invalid = _mm_or_ps(invalid, _mm_cmpnlt_ps(_mm_andnot_ps(_mm_set1_ps(-0.f), p), infinity));
#else
    float p[3];
    std::memcpy(p, position, sizeof(p));
    for (unsigned j = 0; j < 3; ++j) {
      if (!std::isfinite(p[j])) return {};
      result.box[j] = i ? std::min(result.box[j], p[j]) : p[j];
      result.box[3+j] = i ? std::max(result.box[3+j], p[j]) : p[j];
    }
#endif
  }
#if defined(__x86_64__) || defined(_M_X64)
  if (_mm_movemask_ps(invalid)) return {};
  std::memcpy(result.box.data(), &low, 12);
  std::memcpy(result.box.data() + 3, &high, 12);
#endif
  result.known = indices ? elements != 0 : count != 0;
  return result;
}

// Bounds depend on which indices were read, even if two draws use the
// same compact vertex slice. Each instance transforms the local result anew.
struct CasterBoundsCache {
  CasterSliceKey vertexKey, indexKey;
  uint32_t minimum = 0;
  CasterLocalBounds local;
  CasterLocalBounds get(const CasterSliceKey& vertex, const CasterSliceKey& index,
      const uint8_t* src, const uint8_t* indices, uint32_t first) {
    if (vertexKey.matches(vertex) && minimum == first &&
        (indices ? indexKey.matches(index) : !indexKey.count))
      return local;
    vertexKey = vertex;
    indexKey = indices ? index : CasterSliceKey{};
    minimum = first;
    local = ReadCasterBounds(src, vertex.stride, vertex.count, indices, index.count, first);
    return local;
  }
};

inline bool CasterAffineMatrix(const std::array<float, 16>& m) {
  for (float v : m)
    if (!std::isfinite(v) || (v != 0 && std::abs(v) < std::numeric_limits<float>::min())) return false;
  return m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1;
}

// Bound a shader dot product, including rounding in separate FP32
// multiplies/adds, storage as float and flush-to-zero on the GPU.
inline bool CasterAxisInterval(const CasterBox& box, const std::array<float, 16>& m,
    unsigned axis, double& low, double& high) {
  low = high = m[12+axis];
  double magnitude = std::abs(low), flush = 1;
  for (unsigned j = 0; j < 3; ++j) {
    const double k = m[4*j+axis], a = k * box[j], b = k * box[3+j];
    low += std::min(a, b);
    high += std::max(a, b);
    magnitude += std::max(std::abs(a), std::abs(b));
    flush += std::abs(k);
  }
  if (!(magnitude < std::numeric_limits<float>::max() / 2)) return false;
  const double error = 8 * (std::numeric_limits<float>::epsilon() * magnitude +
                            std::numeric_limits<float>::min() * flush);
  low -= error;
  high += error;
  return true;
}

inline bool CasterWorldBounds(const CasterLocalBounds& local, const std::array<float, 16>& world,
    CasterBox& result) {
  if (!local.known || !CasterAffineMatrix(world)) return false;
  for (unsigned j = 0; j < 3; ++j) {
    double low, high;
    if (!CasterAxisInterval(local.box, world, j, low, high)) return false;
    result[j] = float(low);
    result[3+j] = float(high);
  }
  return true;
}

struct CasterQuantization {
  CasterBox box{};
  std::array<float, 3> reciprocal{};
  std::array<float, 16> world{};

  bool prepare(const CasterLocalBounds& local, const std::array<float, 16>& original) {
    if (!local.known || !CasterAffineMatrix(original)) return false;
    box = local.box;
    std::array<double, 3> span;
    for (unsigned i = 0; i < 3; ++i) {
      span[i] = double(box[i+3]) - box[i];
      if (!(span[i] > 0 && span[i] <= 65535.0 / 64)) return false;
      reciprocal[i] = float(65535.0 / span[i]);
      if (!std::isfinite(reciprocal[i])) return false;
    }
    world = original;
    for (unsigned j = 0; j < 3; ++j) {
      double origin = original[12+j], width = 0, sourceMagnitude = std::abs(origin);
      for (unsigned i = 0; i < 3; ++i) {
        origin += double(original[4*i+j]) * box[i];
        sourceMagnitude += std::abs(double(original[4*i+j])) * std::max(std::abs(box[i]), std::abs(box[i+3]));
        const double axis = double(original[4*i+j]) * span[i];
        width += std::abs(axis);
        world[4*i+j] = float(axis);
      }
      world[12+j] = float(origin);
      // Include both float shader dot products. A small folded origin can
      // hide cancellation between a large local position and its placement.
      const double eps = std::numeric_limits<float>::epsilon();
      const double error = 0.502 * width / 65535.0 +
          eps * (4 * (std::abs(origin) + width + 1) + 2 * sourceMagnitude);
      if (!(width <= 65535.0 / 64 && error <= 1.0 / 128)) return false;
    }
    return true;
  }

  void write(uint8_t* dst, const uint8_t* src, uint32_t stride, uint32_t count) const {
    for (uint32_t i = 0; i < count; ++i) {
      float p[3];
      std::memcpy(p, src + size_t(i)*stride, 12);
      uint16_t packed[4];
      for (unsigned j = 0; j < 3; ++j) {
        // Unreferenced holes in a frozen index span may contain any bits.
        const float q = (p[j] - box[j]) * reciprocal[j] + 0.5f;
        packed[j] = uint16_t(std::min(65535.f, std::max(0.f, q)));
      }
      packed[3] = UINT16_MAX;
      std::memcpy(dst + size_t(i)*8, packed, 8);
    }
  }
};

// 5x5 PCF and PCSS blocker search reach two radii along either axis.
// Add one texel for bilinear taps and one for stable snapping. The
// receiver can blend different layers, but neither can sample a caster
// which rasterized no texel in that layer.
inline double CasterClipLimit(uint32_t resolution, float pcf, float search, float pcssMin, float pcssMax) {
  if (!resolution || !std::isfinite(pcf) || !std::isfinite(search) ||
      !std::isfinite(pcssMin) || !std::isfinite(pcssMax)) return std::numeric_limits<double>::infinity();
  const double radius = std::max({1.5f, pcf, search, pcssMin, pcssMax});
  return 1.0 + 2.0 * (2.0 * radius + 2.0) / resolution;
}

// light is the final orthographic caster push constant, checked once per
// cascade. Depth clamp lets an upstream caster write even outside [0,1].
inline bool CasterOutsideCascade(const CasterBox& world, const std::array<float, 16>& light, double limit) {
  bool outside = false;
  for (unsigned j = 0; j < 2; ++j) {
    double low, high;
    if (!CasterAxisInterval(world, light, j, low, high)) return false;
    outside |= low > limit || high < -limit;
  }
  return outside;
}

struct ShadowCaster {
  // Object to world, D3DMATRIX memory order like Camera.
  std::array<float, 16> world{};
  // World-space bounding sphere; radius 0 means no bounds (never culled).
  std::array<float, 3> boundsCenter{};
  float boundsRadius = 0.0f;
  uint8_t mode = ShadowModeFixedWorld, objectKind = 0, lifecycle = 0, boundsProvenance = 0;
  uint32_t category = 0;
  int32_t batchTag = -1, stage = -1;
  uint32_t flags = 0;
  // Data-layer identities, opaque here: the batch's handle, the object's type
  // code and handle, and the renderable part (0: not known). They are only
  // compared and logged.
  uint32_t batchHandle = 0, rawcode = 0, objectHandle = 0, part = 0;
  // Identity of the exact geometry, and the generation its bounds came from.
  uint64_t geometryKey = 0, boundsGeneration = 0;
  // Frames since the bounds were computed: 0 is this frame.
  uint32_t boundsAge = ShadowBoundsAgeUnknown;
};

// Only the record executor supplies these matrices. A bad transform cannot
// exclude a caster. Depth clamp makes both X and Y, but not Z, separating axes.
struct CasterCascadeFootprints {
  std::array<std::array<float, 16>, 4> light{};
  uint32_t count = 0;
  double limit = 0;
  uint32_t touches(const CasterBox& box) const {
    uint32_t mask = 0;
    for (uint32_t c = 0; c < count; ++c)
      if (!CasterAffineMatrix(light[c]) || !CasterOutsideCascade(box, light[c], limit))
        mask |= 1u << c;
    return mask;
  }
  uint32_t priority(bool known, const CasterBox& box) const {
    if (!known || !count) return 0;
    for (uint32_t c = 0; c < count; ++c)
      if (!CasterAffineMatrix(light[c]) || !CasterOutsideCascade(box, light[c], limit)) return c;
    return 4;
  }
};

// The host ranks only cache misses. Defer ranking until BeforeUi on the
// first overflow. During sustained pressure, charge each tier at the draw.
class CasterAdmission {
public:
  bool allows(CasterCopySizes writes, const CasterLocalBounds& world, uint64_t limit,
      bool fits, const CasterCopyAccount& next, unsigned& tier) {
    const uint64_t bytes = writes.vertex + writes.index;
    if (!bytes) return true;
    if (!reserving) {
      if (count < requests.size()) requests[count++] = {world, bytes};
      else tiers[0] += bytes; // Beyond the producer's cap, reserve as unknown.
      overflow |= !fits;
      return fits;
    }
    // Would the same misses fit without reservations? A soft refusal alone
    // must not keep reservations alive after demand falls.
    if (!overflow) {
      CasterCopyPlan unused;
      overflow = !demand.append(writes, limit, unused);
    }
    tier = footprints.priority(world.known, world.box);
    tiers[tier] += bytes;
    if (!fits) return false;
    // Closed chunk tails cannot be used. The open chunk's free tail can.
    const uint64_t cursor = next.frameBytes - next.chunkBytes + next.used;
    return reserved[tier] <= limit - cursor;
  }

  void accepted(unsigned tier, uint64_t bytes) {
    if (!reserving || !bytes) return;
    for (unsigned t = tier + 1; t < reserved.size(); ++t)
      reserved[t] -= std::min(reserved[t], bytes);
  }

  void finish(const CasterCascadeFootprints& final) {
    reserved = {};
    if (overflow && final.count) {
      // A cold start can use its first final matrices to seed the next frame.
      const auto& hint = footprints.count ? footprints : final;
      for (size_t i = 0; i < count; ++i) {
        const auto& r = requests[i];
        tiers[hint.priority(r.world.known, r.world.box)] += r.bytes;
      }
      uint64_t nearer = 0;
      for (unsigned t = 1; t < reserved.size(); ++t) {
        nearer += tiers[t - 1];
        reserved[t] = nearer + (nearer + 31) / 32; // 3.125% for changes and padding
      }
    }
    reserving = overflow && final.count;
    footprints = final;
    count = 0;
    tiers = {};
    demand = {};
    overflow = false;
  }

private:
  struct Request { CasterLocalBounds world; uint64_t bytes; };
  std::array<Request, CasterFrameRecordLimit> requests;
  size_t count = 0;
  bool overflow = false, reserving = false;
  CasterCopyAccount demand;
  CasterCascadeFootprints footprints;
  std::array<uint64_t, 5> tiers{}, reserved{};
};

// Frame, producer and binding identities have already been checked by the
// caller. Any difference in the omission census revokes every layer.
inline uint32_t CasterCompleteLayers(const CasterCascadeFootprints& f,
    const CasterBox* boxes, size_t exact, uint64_t omissions, bool attributable) {
  if (!attributable || exact != omissions) return 0;
  uint32_t mask = (1u << f.count) - 1u;
  for (size_t i = 0; i < exact && mask; ++i) mask &= ~f.touches(boxes[i]);
  return mask;
}

// Map, device, image generation, tombstone and stage policy, respectively.
using CasterLayerIdentity = std::array<uint64_t, 5>;
struct CasterLayerPublication {
  CasterLayerIdentity identity{};
  std::array<uint64_t, 4> lastComplete{};
  uint32_t settle(uint64_t serial, uint32_t layers, uint32_t written, bool previousValid,
                  const CasterLayerIdentity& current) {
    if (!previousValid || identity != current) lastComplete = {};
    identity = current;
    uint32_t held = 0;
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t bit = 1u << c;
      if (written & bit) lastComplete[c] = serial;
      else if ((layers & bit) && lastComplete[c] && serial >= lastComplete[c] &&
               serial - lastComplete[c] <= 8) held |= bit;
      else lastComplete[c] = 0;
    }
    return held;
  }
};

inline std::array<uint8_t, ShadowCasterBytes> EncodeShadowCaster(const ShadowCaster& c) {
  std::array<uint8_t, ShadowCasterBytes> p{}; uint32_t at = 0;
  const auto word = [&](uint32_t v) { detail::Put32(p.data() + at, v); at += 4; };
  const auto real = [&](float v) { detail::PutFloat(p.data() + at, v); at += 4; };
  for (float v : c.world) real(v);
  for (float v : c.boundsCenter) real(v);
  real(c.boundsRadius);
  word(uint32_t(c.mode) | (uint32_t(c.objectKind) << 8) | (uint32_t(c.lifecycle) << 16)
       | (uint32_t(c.boundsProvenance) << 24));
  word(c.category); word(uint32_t(c.batchTag)); word(uint32_t(c.stage));
  word(c.flags);
  word(c.batchHandle); word(c.rawcode); word(c.objectHandle); word(c.part);
  word(uint32_t(c.geometryKey)); word(uint32_t(c.geometryKey >> 32));
  word(uint32_t(c.boundsGeneration)); word(uint32_t(c.boundsGeneration >> 32));
  word(c.boundsAge);
  return p;
}

inline bool IsWaterSurfaceRecord(const ShadowCaster& c) {
  return c.rawcode == WaterSurfaceRawcode && c.part == WaterSurfacePart && c.category == 0u &&
         c.stage == -1 && c.batchTag == -1;
}

inline bool DecodeShadowCaster(const uint8_t* p, uint32_t bytes, ShadowCaster& out) {
  if (!p || bytes != ShadowCasterBytes) return false;
  ShadowCaster c; uint32_t at = 0; bool valid = true;
  const auto word = [&]() { const uint32_t v = detail::Get32(p + at); at += 4; return v; };
  const auto real = [&]() { const float v = detail::GetFloat(p + at); at += 4; valid = valid && std::isfinite(v); return v; };
  for (float& v : c.world) v = real();
  for (float& v : c.boundsCenter) v = real();
  c.boundsRadius = real();
  const uint32_t kinds = word();
  c.mode = uint8_t(kinds); c.objectKind = uint8_t(kinds >> 8); c.lifecycle = uint8_t(kinds >> 16);
  c.boundsProvenance = uint8_t(kinds >> 24);
  c.category = word(); c.batchTag = int32_t(word()); c.stage = int32_t(word());
  c.flags = word();
  c.batchHandle = word(); c.rawcode = word(); c.objectHandle = word(); c.part = word();
  c.geometryKey = word(); c.geometryKey |= uint64_t(word()) << 32;
  c.boundsGeneration = word(); c.boundsGeneration |= uint64_t(word()) << 32;
  c.boundsAge = word();
  // The game's stages are a small table; -1 is "not known".
  if (!valid || c.mode != ShadowModeFixedWorld || c.objectKind > ShadowObjectKindLast
      || c.lifecycle > ShadowLifecycleLast || c.boundsProvenance > ShadowBoundsProvenanceLast
      || c.category > ShadowCategoryLast
      || c.batchTag < ShadowBatchTagFirst || c.batchTag > ShadowBatchTagLast || c.stage < -1 || c.stage > 255
      || (c.flags & ~ShadowCasterFlag::All) || !(c.boundsRadius >= 0.0f)) return false;
  out = c; return true;
}

// The source placement and identity survive a host budget refusal. boxIndex
// names an exact omitted box; an unknown box never proves a layer complete.
struct OmittedCasterPart {
  CasterPartBase part;
  CasterClass cls;
  uint32_t boxIndex;
};

// ---- ShadowCasterAlpha ------------------------------------------------------
//
// The draw is alpha-tested, and so is its shadow: texels whose alpha is below
// the threshold cast nothing. The texture is the one the draw bound at the
// record's stage, its coordinates the draw's TEXCOORD element the record
// names. It follows the ShadowCaster record of the same draw; alone it means
// nothing and the transport refuses it.

constexpr uint32_t ShadowCasterAlphaBytes = 3 * 4;
// D3D9's pixel texture stages and TEXCOORD usage indices.
constexpr uint32_t ShadowTextureStageLast = 7, ShadowTexcoordLast = 7;

struct ShadowCasterAlpha {
  // The stage whose texture the alpha comes from.
  uint32_t stage = 0;
  // D3DRS_ALPHAREF as a fraction of 255, 0..1.
  float alphaRef = 0.5f;
  // The usage index of the stream-0 TEXCOORD element the stage samples with.
  uint32_t texcoord = 0;
};

inline std::array<uint8_t, ShadowCasterAlphaBytes> EncodeShadowCasterAlpha(const ShadowCasterAlpha& a) {
  std::array<uint8_t, ShadowCasterAlphaBytes> p{};
  detail::Put32(p.data(), a.stage);
  detail::PutFloat(p.data() + 4, a.alphaRef);
  detail::Put32(p.data() + 8, a.texcoord);
  return p;
}

inline bool DecodeShadowCasterAlpha(const uint8_t* p, uint32_t bytes, ShadowCasterAlpha& out) {
  if (!p || bytes != ShadowCasterAlphaBytes) return false;
  ShadowCasterAlpha a;
  a.stage = detail::Get32(p);
  a.alphaRef = detail::GetFloat(p + 4);
  a.texcoord = detail::Get32(p + 8);
  if (a.stage > ShadowTextureStageLast || a.texcoord > ShadowTexcoordLast
      || !(a.alphaRef >= 0.0f && a.alphaRef <= 1.0f)) return false;
  out = a; return true;
}

// ---- PointLights ------------------------------------------------------------
//
// The active point lights of the data layer's light manager for this frame,
// in its order, at most 16 (War3PointLightFrameSnapshot::kMaxLights). A word
// with their number, then nine words each: position, range, colour,
// intensity, shadow intensity (0 casts no shadow). The pass ranks them by
// itself. A frame without the record has no point lights.

constexpr uint32_t PointLightsMax = 16, PointLightWords = 9;
constexpr uint32_t PointLightsBytes(uint32_t count) { return (1 + count * PointLightWords) * 4; }
constexpr uint32_t PointLightsMaxBytes = PointLightsBytes(PointLightsMax);

struct PointLight {
  std::array<float, 3> position{};
  // War3LightManager keeps ranges of at least 1.
  float range = 1.0f;
  std::array<float, 3> color{};
  float intensity = 0.0f;
  // 0..1.
  float shadowIntensity = 0.0f;
};

struct PointLights {
  uint32_t count = 0;
  std::array<PointLight, PointLightsMax> lights{};
};

// The record, and how many of its bytes are used.
inline uint32_t EncodePointLights(const PointLights& l, std::array<uint8_t, PointLightsMaxBytes>& p) {
  p = {};
  const uint32_t count = l.count < PointLightsMax ? l.count : PointLightsMax;
  detail::Put32(p.data(), count);
  uint32_t at = 4;
  const auto real = [&](float v) { detail::PutFloat(p.data() + at, v); at += 4; };
  for (uint32_t i = 0; i < count; ++i) {
    const PointLight& light = l.lights[i];
    for (float v : light.position) real(v);
    real(light.range);
    for (float v : light.color) real(v);
    real(light.intensity); real(light.shadowIntensity);
  }
  return at;
}

inline bool DecodePointLights(const uint8_t* p, uint32_t bytes, PointLights& out) {
  if (!p || bytes < 4) return false;
  PointLights l;
  l.count = detail::Get32(p);
  if (l.count > PointLightsMax || bytes != PointLightsBytes(l.count)) return false;
  uint32_t at = 4; bool valid = true;
  const auto real = [&]() { const float v = detail::GetFloat(p + at); at += 4; valid = valid && std::isfinite(v); return v; };
  for (uint32_t i = 0; i < l.count; ++i) {
    PointLight& light = l.lights[i];
    for (float& v : light.position) v = real();
    light.range = real();
    for (float& v : light.color) v = real();
    light.intensity = real(); light.shadowIntensity = real();
    valid = valid && light.range >= 1.0f && light.color[0] >= 0.0f && light.color[1] >= 0.0f
        && light.color[2] >= 0.0f && light.intensity >= 0.0f
        && light.shadowIntensity >= 0.0f && light.shadowIntensity <= 1.0f;
  }
  if (!valid) return false;
  out = l; return true;
}

// ---- Outline ------------------------------------------------------------------
//
// The unit outline the shadow pass draws after the receiver: its settings, word
// for word War3OccludedOutlineSettings, then the handles of the objects to
// outline, at most 1024, as the data layer holds them (War3RenderState's
// outline handles; a caster is outlined when its ShadowCaster batchHandle is
// one of them). A frame without the record draws no outline.
//
//   word 0  enabled         0/1
//   word 1  mode            War3OutlineMode: 0 occluded fill, 1 silhouette
//   word 2  useScreenSpace  0/1: the screen-space edge path, else geometry
//   word 3  widthPx         finite, 0 < width <= 64
//   word 4  showVisible     0/1
//   word 5  showOccluded    0/1
//   words 6-9  colour RGBA  finite, 0..1
//   word 10 handles         n, at most OutlineHandlesMax
//   words 11..  the n handles, none 0

constexpr uint32_t OutlineHandlesMax = 1024, OutlineHeaderWords = 11;
constexpr uint32_t OutlineBytes(uint32_t handles) { return (OutlineHeaderWords + handles) * 4; }
constexpr uint32_t OutlineMaxBytes = OutlineBytes(OutlineHandlesMax);
constexpr float OutlineWidthMax = 64.0f;

struct Outline {
  // War3OccludedOutlineSettings' defaults.
  uint32_t enabled = 1, mode = 1, useScreenSpace = 1;
  float widthPx = 3.0f;
  uint32_t showVisible = 0, showOccluded = 1;
  std::array<float, 4> color{ 1.0f, 1.0f, 0.0f, 0.8f };
  uint32_t handleCount = 0;
  std::array<uint32_t, OutlineHandlesMax> handles{};
};

// The record, and how many of its bytes are used.
inline uint32_t EncodeOutline(const Outline& o, std::array<uint8_t, OutlineMaxBytes>& p) {
  p = {};
  const uint32_t count = o.handleCount < OutlineHandlesMax ? o.handleCount : OutlineHandlesMax;
  uint32_t at = 0;
  const auto word = [&](uint32_t v) { detail::Put32(p.data() + at, v); at += 4; };
  const auto real = [&](float v) { detail::PutFloat(p.data() + at, v); at += 4; };
  word(o.enabled); word(o.mode); word(o.useScreenSpace); real(o.widthPx);
  word(o.showVisible); word(o.showOccluded);
  for (float v : o.color) real(v);
  word(count);
  for (uint32_t i = 0; i < count; ++i) word(o.handles[i]);
  return at;
}

inline bool DecodeOutline(const uint8_t* p, uint32_t bytes, Outline& out) {
  if (!p || bytes < OutlineBytes(0)) return false;
  Outline o;
  uint32_t at = 0;
  const auto word = [&]() { const uint32_t v = detail::Get32(p + at); at += 4; return v; };
  const auto real = [&]() { const float v = detail::GetFloat(p + at); at += 4; return v; };
  o.enabled = word(); o.mode = word(); o.useScreenSpace = word(); o.widthPx = real();
  o.showVisible = word(); o.showOccluded = word();
  for (float& v : o.color) v = real();
  o.handleCount = word();
  if (o.handleCount > OutlineHandlesMax || bytes != OutlineBytes(o.handleCount)) return false;
  bool valid = o.enabled <= 1 && o.mode <= 1 && o.useScreenSpace <= 1 && o.showVisible <= 1 && o.showOccluded <= 1
      && std::isfinite(o.widthPx) && o.widthPx > 0.0f && o.widthPx <= OutlineWidthMax;
  for (float v : o.color) valid = valid && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
  for (uint32_t i = 0; i < o.handleCount; ++i) { o.handles[i] = word(); valid = valid && o.handles[i] != 0; }
  if (!valid) return false;
  out = o; return true;
}

// ---- VolumetricLight ----------------------------------------------------------
//
// War3RenderSettings::postFx.volumetricLight, one row per field in wire order,
// as the Lighting table: name, path in War3VolumetricLightSettings, default.
// Kinds: B bool (0 or 1), U uint32_t, I int32_t, F finite float, E enum 0..max.
// A frame without the record has no volumetric light. The record's presence is
// the producer's decision that the light runs (post-processing on and the
// effect enabled); `enabled` still travels, as the pass reads it.
#define WAR3_SCENE_VOLUMETRIC_FIELDS(B, U, I, F, E) \
  B(enabled, enabled, 0) \
  E(quality, quality, 2, 2) \
  B(froxelTemporalEnabled, froxelTemporalEnabled, 1) \
  F(froxelHistoryWeight, froxelHistoryWeight, 0.88f) \
  F(froxelVarianceGamma, froxelVarianceGamma, 1.50f) \
  F(froxelReactiveScale, froxelReactiveScale, 1.25f) \
  F(intensity, intensity, 1.50f) \
  F(decay, decay, 0.95f) \
  F(density, density, 0.22f) \
  F(weight, weight, 2.50f) \
  F(skyThreshold, skyThreshold, 0.72f) \
  F(fadeNear, fadeNear, 0.0f) \
  F(fadeFar, fadeFar, 0.45f) \
  F(maxRayDistance, maxRayDistance, 1.00f) \
  I(sampleCount, sampleCount, 16) \
  U(resolutionDivisor, resolutionDivisor, 4) \
  F(sunDistance, sunDistance, 1400.0f) \
  F(froxelNear, froxelNear, 20.0f) \
  F(froxelFar, froxelFar, 10000.0f) \
  B(globalMediumEnabled, globalMediumEnabled, 1) \
  B(heightFogEnabled, heightFogEnabled, 0) \
  F(heightFogBase, heightFogBase, 0.0f) \
  F(heightFogFalloff, heightFogFalloff, 0.0012f) \
  F(heightFogStrength, heightFogStrength, 0.35f) \
  F(extinctionStrength, extinctionStrength, 0.05f) \
  F(unshadowedScattering, unshadowedScattering, 0.03f) \
  F(minSunIntensity, minSunIntensity, 0.08f) \
  B(requireCsmSnapshot, requireCsmSnapshot, 1) \
  B(includePointLights, includePointLights, 1) \
  U(maxPointLights, maxPointLights, 2) \
  B(adaptiveSampleCount, adaptiveSampleCount, 1) \
  B(volumeSunShadowEnabled, volumeSunShadowEnabled, 1) \
  B(volumeSunShadowFallbackToCsm, volumeSunShadowFallbackToCsm, 1) \
  F(volumeSunOrthoRadius, volumeSunOrthoRadius, 10000.0f) \
  F(volumeSunNearRadiusScale, volumeSunNearRadiusScale, 0.30f) \
  F(volumeSunDepthExtension, volumeSunDepthExtension, 640.0f) \
  F(volumeSunDepthMargin, volumeSunDepthMargin, 96.0f) \
  U(volumeSunResolution, volumeSunResolution, 2048) \
  F(volumeSunSoftRadius, volumeSunSoftRadius, 1.85f) \
  F(volumeSunReceiverBias, volumeSunReceiverBias, 2.0f) \
  B(volumeSunDualCascade, volumeSunDualCascade, 1)

#define WAR3_SV_WORD(...) + 1
constexpr uint32_t VolumetricLightWords = 0 WAR3_SCENE_VOLUMETRIC_FIELDS(
    WAR3_SV_WORD, WAR3_SV_WORD, WAR3_SV_WORD, WAR3_SV_WORD, WAR3_SV_WORD);
#undef WAR3_SV_WORD
constexpr uint32_t VolumetricLightBytes = VolumetricLightWords * 4;

struct VolumetricLight {
#define WAR3_SV_WORD(name, path, value, ...) uint32_t name = value;
#define WAR3_SV_INT(name, path, value) int32_t name = value;
#define WAR3_SV_FLOAT(name, path, value) float name = value;
  WAR3_SCENE_VOLUMETRIC_FIELDS(WAR3_SV_WORD, WAR3_SV_WORD, WAR3_SV_INT, WAR3_SV_FLOAT, WAR3_SV_WORD)
#undef WAR3_SV_WORD
#undef WAR3_SV_INT
#undef WAR3_SV_FLOAT
};

inline std::array<uint8_t, VolumetricLightBytes> EncodeVolumetricLight(const VolumetricLight& v) {
  std::array<uint8_t, VolumetricLightBytes> p{}; uint32_t at = 0;
  const auto word = [&](uint32_t x) { detail::Put32(p.data() + at, x); at += 4; };
  const auto real = [&](float x) { detail::PutFloat(p.data() + at, x); at += 4; };
#define WAR3_SV_WORD(name, ...) word(v.name);
#define WAR3_SV_INT(name, ...) word(uint32_t(v.name));
#define WAR3_SV_FLOAT(name, ...) real(v.name);
  WAR3_SCENE_VOLUMETRIC_FIELDS(WAR3_SV_WORD, WAR3_SV_WORD, WAR3_SV_INT, WAR3_SV_FLOAT, WAR3_SV_WORD)
#undef WAR3_SV_WORD
#undef WAR3_SV_INT
#undef WAR3_SV_FLOAT
  return p;
}

// False for a switch that is not 0 or 1, a quality outside its range or a
// number that is not finite. The pass clamps the numbers it uses.
inline bool DecodeVolumetricLight(const uint8_t* p, uint32_t bytes, VolumetricLight& out) {
  if (!p || bytes != VolumetricLightBytes) return false;
  VolumetricLight v; uint32_t at = 0; bool valid = true;
  const auto word = [&]() { const uint32_t x = detail::Get32(p + at); at += 4; return x; };
  const auto real = [&]() { const float x = detail::GetFloat(p + at); at += 4; valid = valid && std::isfinite(x); return x; };
#define WAR3_SV_BOOL(name, ...) v.name = word(); valid = valid && v.name <= 1;
#define WAR3_SV_WORD(name, ...) v.name = word();
#define WAR3_SV_INT(name, ...) v.name = int32_t(word());
#define WAR3_SV_FLOAT(name, ...) v.name = real();
#define WAR3_SV_ENUM(name, path, value, max) v.name = word(); valid = valid && v.name <= max;
  WAR3_SCENE_VOLUMETRIC_FIELDS(WAR3_SV_BOOL, WAR3_SV_WORD, WAR3_SV_INT, WAR3_SV_FLOAT, WAR3_SV_ENUM)
#undef WAR3_SV_BOOL
#undef WAR3_SV_WORD
#undef WAR3_SV_INT
#undef WAR3_SV_FLOAT
#undef WAR3_SV_ENUM
  if (!valid) return false;
  out = v; return true;
}

// ---- FogVolumes ---------------------------------------------------------------
//
// The map's fog volumes as the data layer holds them (War3FogVolumeManager),
// in its order, at most 8 (War3FogVolumeFrameSnapshot::kMaxVolumes): a word
// with their number, then thirteen words each, the values the map authored -
// shape, whether it is on, centre, half extents, rotation in degrees, density,
// edge feather. The executor builds the volumes from them through the
// manager's own interface, so both ends derive what the pass reads with the
// same code. A sphere's half extents are its radius three times, a
// cylinder's its radius twice and half its height. The ranges are the
// manager's. A frame without the record has no fog volumes; they light only
// where the frame's VolumetricLight runs.

constexpr uint32_t FogVolumesMax = 8, FogVolumeWords = 13;
constexpr uint32_t FogVolumesBytes(uint32_t count) { return (1 + count * FogVolumeWords) * 4; }
constexpr uint32_t FogVolumesMaxBytes = FogVolumesBytes(FogVolumesMax);

struct FogVolume {
  // War3FogVolumeShape: 0 sphere, 1 box, 2 cylinder.
  uint32_t shape = 0;
  uint32_t active = 1;
  std::array<float, 3> position{};
  std::array<float, 3> halfSize{1.0f, 1.0f, 1.0f};
  std::array<float, 3> rotationDegrees{};
  float density = 0.0f;
  float edgeFeather = 0.0f;
};

struct FogVolumes {
  uint32_t count = 0;
  std::array<FogVolume, FogVolumesMax> volumes{};
};

// The record, and how many of its bytes are used.
inline uint32_t EncodeFogVolumes(const FogVolumes& f, std::array<uint8_t, FogVolumesMaxBytes>& p) {
  p = {};
  const uint32_t count = f.count < FogVolumesMax ? f.count : FogVolumesMax;
  detail::Put32(p.data(), count);
  uint32_t at = 4;
  const auto word = [&](uint32_t v) { detail::Put32(p.data() + at, v); at += 4; };
  const auto real = [&](float v) { detail::PutFloat(p.data() + at, v); at += 4; };
  for (uint32_t i = 0; i < count; ++i) {
    const FogVolume& v = f.volumes[i];
    word(v.shape); word(v.active);
    for (float x : v.position) real(x);
    for (float x : v.halfSize) real(x);
    for (float x : v.rotationDegrees) real(x);
    real(v.density); real(v.edgeFeather);
  }
  return at;
}

// False for a count above 8 or bytes that do not match it, a shape or switch
// out of range, a value outside the manager's range, or half extents the
// shape cannot have.
inline bool DecodeFogVolumes(const uint8_t* p, uint32_t bytes, FogVolumes& out) {
  if (!p || bytes < 4) return false;
  FogVolumes f;
  f.count = detail::Get32(p);
  if (f.count > FogVolumesMax || bytes != FogVolumesBytes(f.count)) return false;
  uint32_t at = 4; bool valid = true;
  const auto word = [&]() { const uint32_t v = detail::Get32(p + at); at += 4; return v; };
  const auto real = [&](float lo, float hi) {
    const float v = detail::GetFloat(p + at); at += 4;
    valid = valid && std::isfinite(v) && v >= lo && v <= hi; return v; };
  for (uint32_t i = 0; i < f.count; ++i) {
    FogVolume& v = f.volumes[i];
    v.shape = word(); v.active = word();
    for (float& x : v.position) x = real(-1000000.0f, 1000000.0f);
    for (float& x : v.halfSize) x = real(0.5f, 100000.0f);
    for (float& x : v.rotationDegrees) x = real(-360000.0f, 360000.0f);
    v.density = real(0.0f, 2.0f); v.edgeFeather = real(0.0f, 1.0f);
    valid = valid && v.shape <= 2 && v.active <= 1;
    if (v.shape == 0) valid = valid && v.halfSize[1] == v.halfSize[0] && v.halfSize[2] == v.halfSize[0];
    if (v.shape == 2) valid = valid && v.halfSize[1] == v.halfSize[0];
  }
  if (!valid) return false;
  out = f; return true;
}

// A shadow record a FrameScene message may carry.
inline bool ValidShadowRecord(uint32_t schema, const uint8_t* p, uint32_t bytes) {
  if (schema == LightingSchema) { Lighting ignored; return DecodeLighting(p, bytes, ignored); }
  if (schema == ShadowFrameSchema) { ShadowFrame ignored; return DecodeShadowFrame(p, bytes, ignored); }
  if (schema == PointLightsSchema) { PointLights ignored; return DecodePointLights(p, bytes, ignored); }
  if (schema == OutlineSchema) { Outline ignored; return DecodeOutline(p, bytes, ignored); }
  if (schema == VolumetricLightSchema) { VolumetricLight ignored; return DecodeVolumetricLight(p, bytes, ignored); }
  if (schema == FogVolumesSchema) { FogVolumes ignored; return DecodeFogVolumes(p, bytes, ignored); }
  return false;
}

// The shadow records of one frame, beside FramePacket and with its rules: a
// later record replaces the earlier one, and the owner clears it when the
// frame ends or the session changes.
struct ShadowPacket {
  bool hasLighting = false, hasFrame = false, hasPointLights = false, hasOutline = false;
  bool hasVolumetric = false, hasFogVolumes = false;
  Lighting lighting;
  ShadowFrame frame;
  PointLights pointLights;
  Outline outline;
  VolumetricLight volumetric;
  FogVolumes fogVolumes;

  void clear() { *this = ShadowPacket(); }
  // True when the shadow pass has something to do at BeforeUi.
  bool work() const { return hasLighting; }
  bool accept(uint32_t schema, const uint8_t* p, uint32_t bytes) {
    if (schema == LightingSchema) { Lighting v; if (!DecodeLighting(p, bytes, v)) return false; lighting = v; return hasLighting = true; }
    if (schema == ShadowFrameSchema) { ShadowFrame v; if (!DecodeShadowFrame(p, bytes, v)) return false; frame = v; return hasFrame = true; }
    if (schema == PointLightsSchema) { PointLights v; if (!DecodePointLights(p, bytes, v)) return false; pointLights = v; return hasPointLights = true; }
    if (schema == OutlineSchema) { Outline v; if (!DecodeOutline(p, bytes, v)) return false; outline = v; return hasOutline = true; }
    if (schema == VolumetricLightSchema) {
      VolumetricLight v; if (!DecodeVolumetricLight(p, bytes, v)) return false; volumetric = v; return hasVolumetric = true;
    }
    if (schema == FogVolumesSchema) {
      FogVolumes v; if (!DecodeFogVolumes(p, bytes, v)) return false; fogVolumes = v; return hasFogVolumes = true;
    }
    return false;
  }
};

} // namespace dxvk::war3::effects::scene
