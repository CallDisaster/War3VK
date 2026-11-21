#pragma once

// Scene packet records of the effect executors.
//
// An effect that runs at an insertion point (BeforeUi, AfterUi, BeforePresent)
// reads three things: the frame's colour and depth targets, the world camera,
// and one record of its own. The targets belong to whoever executes; the
// camera and the effect records are x86 data-layer facts. In process the
// device hands them to the pass in War3PipelineInput. Across the process
// boundary each one is a FrameScene message (d3d9_annotation_wire.h) whose
// payload is defined here, so both sides and the fixtures share one layout.
//
// Rules for a record:
// - values only: no pointers, handles or indices into x86 state;
// - complete: everything the executor reads besides camera and targets;
// - per frame: a record applies to the frame it was sent in and to nothing
//   later. An effect whose record is absent does not run that frame, which
//   is also how the data layer turns it off;
// - little-endian words; floats and doubles travel as their IEEE bits.
//
// This header has no dependency: the x86 proxy, the x64 host, the in-process
// device and test fixtures all include it.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dxvk::war3::effects::scene {

// FrameScene schema numbers. 1..4 belong to d3d9_annotation_wire.h.
constexpr uint32_t CameraSchema = 2;
constexpr uint32_t SsaoSchema = 5;
constexpr uint32_t SnowSchema = 6;
constexpr uint32_t AaSchema = 7;
constexpr uint32_t BloomSchema = 8;

constexpr uint32_t CameraBytes = (16 + 16 + 4 + 2) * 4 + 8;
constexpr uint32_t SsaoBytes = 6 * 4;
constexpr uint32_t SnowBytes = 11 * 4 + 3 * 8 + 8;
constexpr uint32_t AaBytes = 7 * 4;
constexpr uint32_t BloomBytes = 6 * 4;
constexpr uint32_t MaxRecordBytes = CameraBytes;

// The largest target edge a viewport may name; far above any real extent.
constexpr uint32_t MaxViewportEdge = 16384;

// World camera of the frame. Matrices are in D3DMATRIX memory order: element
// (row, column) is word row * 4 + column.
struct Camera {
  std::array<float, 16> view{}, projection{};
  uint32_t x = 0, y = 0, width = 0, height = 0;
  float minZ = 0.0f, maxZ = 1.0f;
  uint32_t frame = 0;
  float gameTime = 0.0f;
};

// war3_ssao_executor.h: War3RenderSettings::postFx.ssao without the switch.
struct Ssao {
  float radiusPx = 0.0f, strength = 0.0f, bias = 0.0f, power = 0.0f;
  float fadeNear = 0.0f, fadeFar = 0.0f;
};

// war3_snow_executor.h: the part of WeatherFrameSnapshot the draw reads. Wind
// is already integrated into the displacement by the weather runtime.
struct Snow {
  float snowfall = 0.0f, gust = 0.0f, haze = 0.0f, lowSnow = 0.0f;
  float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
  float bottomZ = 0.0f, topZ = 0.0f, edgeFeather = 0.0f;
  double timeSeconds = 0.0, windDisplacementX = 0.0, windDisplacementY = 0.0;
  uint32_t seed = 0;
  int32_t id = 0;
};

// war3_aa_executor.h: War3RenderSettings::postFx.aa. Mode 1 is FXAA, 2..5 are
// the SMAA presets (one algorithm; the parameters set its quality). There is
// no record for mode 0: without a record the effect does not run.
constexpr uint32_t AaModeFxaa = 1, AaModeSmaaLast = 5;
struct Aa {
  uint32_t mode = 0;
  float fxaaQualitySubpix = 0.0f, fxaaQualityEdgeThreshold = 0.0f, fxaaQualityEdgeThresholdMin = 0.0f;
  float smaaThreshold = 0.0f;
  int32_t smaaMaxSearchSteps = 0, smaaMaxSearchStepsDiag = 0;
};

// war3_bloom_executor.h: the post-processing chain (bloom, exposure, tone map).
// intensity is 0 when bloom itself is off; the composite still applies the
// exposure and tone map, as the D3D9 implementation did.
struct Bloom {
  float threshold = 0.0f, softKnee = 0.0f, intensity = 0.0f, exposure = 0.0f;
  uint32_t acesToneMap = 0, useSrgb = 0;
};

namespace detail {
inline void Put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline uint32_t Get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void PutFloat(uint8_t* p, float v) { uint32_t bits; std::memcpy(&bits, &v, 4); Put32(p, bits); }
inline float GetFloat(const uint8_t* p) { const uint32_t bits = Get32(p); float v; std::memcpy(&v, &bits, 4); return v; }
inline void PutDouble(uint8_t* p, double v) {
  uint64_t bits; std::memcpy(&bits, &v, 8); Put32(p, uint32_t(bits)); Put32(p + 4, uint32_t(bits >> 32));
}
inline double GetDouble(const uint8_t* p) {
  const uint64_t bits = uint64_t(Get32(p)) | (uint64_t(Get32(p + 4)) << 32); double v; std::memcpy(&v, &bits, 8); return v;
}
// A viewport edge or origin: a whole number of pixels within the bound.
inline bool Pixels(float v, uint32_t& out) {
  if (!(v >= 0.0f) || v > float(MaxViewportEdge) || v != std::floor(v)) return false;
  out = uint32_t(v); return true;
}
} // namespace detail

inline std::array<uint8_t, CameraBytes> EncodeCamera(const Camera& c) {
  std::array<uint8_t, CameraBytes> p{}; uint32_t at = 0;
  const auto f = [&](float v) { detail::PutFloat(p.data() + at, v); at += 4; };
  for (float v : c.view) f(v);
  for (float v : c.projection) f(v);
  f(float(c.x)); f(float(c.y)); f(float(c.width)); f(float(c.height));
  f(c.minZ); f(c.maxZ);
  detail::Put32(p.data() + at, c.frame); at += 4;
  f(c.gameTime);
  return p;
}

// False for a record no executor can use. The transport accepts any finite
// camera; an executor then finds this one unusable and does not run.
inline bool DecodeCamera(const uint8_t* p, uint32_t bytes, Camera& out) {
  if (!p || bytes != CameraBytes) return false;
  Camera c; uint32_t at = 0;
  const auto f = [&]() { const float v = detail::GetFloat(p + at); at += 4; return v; };
  for (float& v : c.view) v = f();
  for (float& v : c.projection) v = f();
  const float x = f(), y = f(), width = f(), height = f();
  c.minZ = f(); c.maxZ = f();
  c.frame = detail::Get32(p + at); at += 4;
  c.gameTime = f();
  for (float v : c.view) if (!std::isfinite(v)) return false;
  for (float v : c.projection) if (!std::isfinite(v)) return false;
  if (!detail::Pixels(x, c.x) || !detail::Pixels(y, c.y) || !detail::Pixels(width, c.width)
      || !detail::Pixels(height, c.height) || !c.width || !c.height
      || !std::isfinite(c.minZ) || !std::isfinite(c.maxZ) || !std::isfinite(c.gameTime)) return false;
  out = c; return true;
}

inline std::array<uint8_t, SsaoBytes> EncodeSsao(const Ssao& s) {
  std::array<uint8_t, SsaoBytes> p{};
  const float values[6] = { s.radiusPx, s.strength, s.bias, s.power, s.fadeNear, s.fadeFar };
  for (uint32_t i = 0; i < 6; ++i) detail::PutFloat(p.data() + 4 * i, values[i]);
  return p;
}

inline bool DecodeSsao(const uint8_t* p, uint32_t bytes, Ssao& out) {
  if (!p || bytes != SsaoBytes) return false;
  float values[6];
  for (uint32_t i = 0; i < 6; ++i) {
    values[i] = detail::GetFloat(p + 4 * i);
    if (!std::isfinite(values[i])) return false;
  }
  out = { values[0], values[1], values[2], values[3], values[4], values[5] };
  return true;
}

inline std::array<uint8_t, SnowBytes> EncodeSnow(const Snow& s) {
  std::array<uint8_t, SnowBytes> p{};
  const float floats[11] = { s.snowfall, s.gust, s.haze, s.lowSnow, s.minX, s.minY, s.maxX, s.maxY,
                             s.bottomZ, s.topZ, s.edgeFeather };
  for (uint32_t i = 0; i < 11; ++i) detail::PutFloat(p.data() + 4 * i, floats[i]);
  detail::PutDouble(p.data() + 44, s.timeSeconds);
  detail::PutDouble(p.data() + 52, s.windDisplacementX);
  detail::PutDouble(p.data() + 60, s.windDisplacementY);
  detail::Put32(p.data() + 68, s.seed);
  detail::Put32(p.data() + 72, uint32_t(s.id));
  return p;
}

inline bool DecodeSnow(const uint8_t* p, uint32_t bytes, Snow& out) {
  if (!p || bytes != SnowBytes) return false;
  float floats[11];
  for (uint32_t i = 0; i < 11; ++i) {
    floats[i] = detail::GetFloat(p + 4 * i);
    if (!std::isfinite(floats[i])) return false;
  }
  Snow s;
  s.snowfall = floats[0]; s.gust = floats[1]; s.haze = floats[2]; s.lowSnow = floats[3];
  s.minX = floats[4]; s.minY = floats[5]; s.maxX = floats[6]; s.maxY = floats[7];
  s.bottomZ = floats[8]; s.topZ = floats[9]; s.edgeFeather = floats[10];
  s.timeSeconds = detail::GetDouble(p + 44);
  s.windDisplacementX = detail::GetDouble(p + 52);
  s.windDisplacementY = detail::GetDouble(p + 60);
  s.seed = detail::Get32(p + 68);
  s.id = int32_t(detail::Get32(p + 72));
  // A weather record names a live weather object; id 0 is "none".
  if (!std::isfinite(s.timeSeconds) || !std::isfinite(s.windDisplacementX)
      || !std::isfinite(s.windDisplacementY) || s.id == 0) return false;
  out = s; return true;
}

inline std::array<uint8_t, AaBytes> EncodeAa(const Aa& a) {
  std::array<uint8_t, AaBytes> p{};
  detail::Put32(p.data(), a.mode);
  detail::PutFloat(p.data() + 4, a.fxaaQualitySubpix);
  detail::PutFloat(p.data() + 8, a.fxaaQualityEdgeThreshold);
  detail::PutFloat(p.data() + 12, a.fxaaQualityEdgeThresholdMin);
  detail::PutFloat(p.data() + 16, a.smaaThreshold);
  detail::Put32(p.data() + 20, uint32_t(a.smaaMaxSearchSteps));
  detail::Put32(p.data() + 24, uint32_t(a.smaaMaxSearchStepsDiag));
  return p;
}

// The executor clamps the parameters; the record only has to be a mode and
// finite numbers.
inline bool DecodeAa(const uint8_t* p, uint32_t bytes, Aa& out) {
  if (!p || bytes != AaBytes) return false;
  Aa a;
  a.mode = detail::Get32(p);
  a.fxaaQualitySubpix = detail::GetFloat(p + 4);
  a.fxaaQualityEdgeThreshold = detail::GetFloat(p + 8);
  a.fxaaQualityEdgeThresholdMin = detail::GetFloat(p + 12);
  a.smaaThreshold = detail::GetFloat(p + 16);
  a.smaaMaxSearchSteps = int32_t(detail::Get32(p + 20));
  a.smaaMaxSearchStepsDiag = int32_t(detail::Get32(p + 24));
  if (a.mode < AaModeFxaa || a.mode > AaModeSmaaLast || !std::isfinite(a.fxaaQualitySubpix)
      || !std::isfinite(a.fxaaQualityEdgeThreshold) || !std::isfinite(a.fxaaQualityEdgeThresholdMin)
      || !std::isfinite(a.smaaThreshold)) return false;
  out = a; return true;
}

inline std::array<uint8_t, BloomBytes> EncodeBloom(const Bloom& b) {
  std::array<uint8_t, BloomBytes> p{};
  detail::PutFloat(p.data(), b.threshold);
  detail::PutFloat(p.data() + 4, b.softKnee);
  detail::PutFloat(p.data() + 8, b.intensity);
  detail::PutFloat(p.data() + 12, b.exposure);
  detail::Put32(p.data() + 16, b.acesToneMap);
  detail::Put32(p.data() + 20, b.useSrgb);
  return p;
}

inline bool DecodeBloom(const uint8_t* p, uint32_t bytes, Bloom& out) {
  if (!p || bytes != BloomBytes) return false;
  Bloom b;
  b.threshold = detail::GetFloat(p);
  b.softKnee = detail::GetFloat(p + 4);
  b.intensity = detail::GetFloat(p + 8);
  b.exposure = detail::GetFloat(p + 12);
  b.acesToneMap = detail::Get32(p + 16);
  b.useSrgb = detail::Get32(p + 20);
  if (!std::isfinite(b.threshold) || !std::isfinite(b.softKnee) || !std::isfinite(b.intensity)
      || !std::isfinite(b.exposure) || b.acesToneMap > 1 || b.useSrgb > 1) return false;
  out = b; return true;
}

// An effect record the transport may carry (the camera has its own rule).
inline bool ValidEffectRecord(uint32_t schema, const uint8_t* p, uint32_t bytes) {
  if (schema == SsaoSchema) { Ssao ignored; return DecodeSsao(p, bytes, ignored); }
  if (schema == SnowSchema) { Snow ignored; return DecodeSnow(p, bytes, ignored); }
  if (schema == AaSchema) { Aa ignored; return DecodeAa(p, bytes, ignored); }
  if (schema == BloomSchema) { Bloom ignored; return DecodeBloom(p, bytes, ignored); }
  return false;
}

// Stage markers: ordered points between the draws of a frame.
//   WorldSceneEnter / WorldSceneExit  bracket the game's world RenderScene;
//   BeforeUi   the BeforeUi effects run here: after the world scene, before
//              anything else is drawn, once;
//   FrameEnd   the frame's scene packet ends (Present ends it as well).
// WorldBegin and UiBegin carry no rule yet.
enum class Stage : uint32_t { WorldBegin = 1, BeforeUi, UiBegin, FrameEnd, WorldSceneEnter, WorldSceneExit };

inline bool ValidStage(uint32_t stage) {
  return stage >= uint32_t(Stage::WorldBegin) && stage <= uint32_t(Stage::WorldSceneExit);
}

// The executing side's view of the game's world RenderScene. The in-process
// device reads x86 statics for this at every draw and Present; an executor
// that only receives markers has the two scene markers, the session reset
// and its own draw count, and they are enough for the BeforeUi boundary and
// the frame-end fallback.
struct SceneTracker {
  bool active = false, completedThisFrame = false;
  uint64_t enters = 0, exits = 0, drawsAtExit = 0, generation = 0, resets = 0;
  // BeforeUi effects run once per completed world scene.
  bool effectsInserted = false;
  uint64_t insertions = 0;
  // A scene marker out of order is refused and changes nothing.
  bool mark(uint32_t stage, uint64_t draws) {
    if (stage == uint32_t(Stage::WorldSceneEnter)) {
      if (active) return false;
      active = true; completedThisFrame = false; effectsInserted = false; ++enters;
    } else if (stage == uint32_t(Stage::WorldSceneExit)) {
      if (!active) return false;
      active = false; completedThisFrame = true; drawsAtExit = draws; ++exits;
    }
    return true;
  }
  bool canInsertBeforeUi() const { return completedThisFrame && !active && !effectsInserted; }
  void insertedBeforeUi() { effectsInserted = true; ++insertions; }
  // Draws since this frame's last completed world scene.
  uint64_t drawsAfterScene(uint64_t draws) const { return completedThisFrame && !active ? draws - drawsAtExit : 0; }
  void frameEnd() { completedThisFrame = false; effectsInserted = false; }
  // Generations only ascend, and a session never changes inside a world scene.
  bool reset(uint64_t next) {
    if (active || next <= generation) return false;
    generation = next; ++resets; return true;
  }
};

// The records of one frame. A later record of a schema replaces the earlier
// one. The owner clears the packet when the frame ends (the FrameEnd marker or
// Present) and when the session changes, so nothing carries over.
struct FramePacket {
  bool hasCamera = false, hasSsao = false, hasSnow = false, hasAa = false, hasBloom = false;
  Camera camera;
  Ssao ssao;
  Snow snow;
  Aa aa;
  Bloom bloom;

  void clear() { *this = FramePacket(); }
  // True when an executor has something to do at BeforeUi.
  bool effects() const { return hasSsao || hasSnow || hasAa || hasBloom; }
  // False for a schema or payload no executor reads. A camera that cannot be
  // used also drops the previous one: effects then report it unavailable.
  bool accept(uint32_t schema, const uint8_t* p, uint32_t bytes) {
    if (schema == CameraSchema) return hasCamera = DecodeCamera(p, bytes, camera);
    if (schema == SsaoSchema) { Ssao v; if (!DecodeSsao(p, bytes, v)) return false; ssao = v; return hasSsao = true; }
    if (schema == SnowSchema) { Snow v; if (!DecodeSnow(p, bytes, v)) return false; snow = v; return hasSnow = true; }
    if (schema == AaSchema) { Aa v; if (!DecodeAa(p, bytes, v)) return false; aa = v; return hasAa = true; }
    if (schema == BloomSchema) { Bloom v; if (!DecodeBloom(p, bytes, v)) return false; bloom = v; return hasBloom = true; }
    return false;
  }
};

} // namespace dxvk::war3::effects::scene
