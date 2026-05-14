#pragma once

#include "d3d9_war3_csm.h"

namespace dxvk {

/**
 * @brief War3 渲染增强全局设置（DXVK 内部）
 */
struct War3DirectionalLightSettings {
  bool enabled = true;
  Vector4 direction = Vector4(-0.3f, -1.0f, -0.2f, 0.0f);
  Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 0.0f);
  float intensity = 1.0f;
};

enum class War3ShadowDebugMode : uint8_t {
  None = 0,
  Cascades = 1,
  ShadowFactor = 2,
  Depth = 3,
  MotionVector = 4,
  ShadowHistory = 5,
};

enum class War3ShadowReceiverMode : uint8_t {
  Legacy = 0,
  NormalBias = 1,
  AdaptiveBias = 2,
};

enum class War3ShadowFilterMode : uint8_t {
  Nearest = 0,
  Linear = 1,
};

enum class War3ShadowPcfKernel : uint8_t {
  Grid3x3 = 0,
  Grid5x5 = 1,
  Poisson16 = 2,
  Poisson25 = 3,
};

enum class War3ShadowPcfRotateMode : uint8_t {
  Off = 0,
  Screen = 1,
  World = 2,
};

enum class War3ShadowPcssSearchKernel : uint8_t {
  Grid3x3 = 0,
  Grid5x5 = 1,
};

enum class War3ShadowAltitudeMode : uint8_t {
  SineAltitude = 0,
  TimeLinear = 1,
};

struct War3ShadowSettings {
  bool enabled = true;
  War3CsmConfig csm = {};
  float strength = 0.75f;
  // Phase 7.31 Iteration D：从 0.95 降到 0.70，让 Poisson16 的采样落在
  // 更小的 texel 半径内，shadow edge 更锐。配合 CSM maxDistance 4000 + 
  // 自适应 min resolution 3072 给用户"清晰 caster"观感。
  float pcfRadius = 0.70f;
  float receiverBias = 0.004f;
  float cascadeBlendRange = 120.0f;
  War3ShadowPcfKernel pcfKernel = War3ShadowPcfKernel::Poisson16;
  bool pcfRotate = true;
  War3ShadowPcfRotateMode pcfRotateMode = War3ShadowPcfRotateMode::World;
  float cascadeBiasScale = 0.5f;
  float pcfCascadeRadiusScale = 0.5f;

  float casterDepthBias = 1.0f;
  float casterSlopeBias = 1.5f;
  float casterBiasClamp = 0.0f;

  bool pcssEnabled = false;
  float pcssSearchRadius = 6.0f;
  float pcssMinRadius = 1.0f;
  float pcssMaxRadius = 3.0f;
  float pcssDepthScale = 30.0f;
  War3ShadowPcssSearchKernel pcssSearchKernel =
      War3ShadowPcssSearchKernel::Grid3x3;

  bool alphaShadowHashed = true;
  bool alphaShadowUseMip = true;
  float alphaShadowMipLodBias = 0.5f;
  float alphaShadowFarAlphaRefBias = 0.05f;

  bool shadowTaaEnabled = true;
  float shadowTaaBlendFactor = 0.05f;
  bool shadowTaaNeighborClamp = true;

  bool pointLightsEnabled = false;
  uint32_t pointShadowMaxLights = 1;
  uint32_t pointShadowResolution = 512;
  float pointShadowBias = 0.05f;
  bool pointShadowEnabled = false;

  War3ShadowReceiverMode receiverMode = War3ShadowReceiverMode::NormalBias;
  float normalBiasScale = 0.005f;
  float rimIntensity = 0.0f;
  float rimPower = 2.0f;
  War3ShadowAltitudeMode altitudeMode = War3ShadowAltitudeMode::SineAltitude;
  float shadowLengthScale = 0.9f;
  float shadowMaxLengthScale = 0.8f;
  War3ShadowFilterMode filterMode = War3ShadowFilterMode::Linear;
  float nightShadowScale = 1.0f;

  bool lockSun = false;
  float lockSunTime = 0.5f;
  bool stableSnapWhenSunMoving = true;

  War3ShadowDebugMode debugMode = War3ShadowDebugMode::None;
};

enum class War3AAMode : uint8_t {
  None = 0,
  FXAA = 1,
  SMAA_Low = 2,
  SMAA_Medium = 3,
  SMAA_High = 4,
  SMAA_Ultra = 5,
};

struct War3AASettings {
  // FXAA 保留作回退手段，但默认档位改为最高 SMAA。
  War3AAMode mode = War3AAMode::SMAA_Ultra;
  float fxaaQualitySubpix = 0.75f;
  float fxaaQualityEdgeThreshold = 0.166f;
  float fxaaQualityEdgeThresholdMin = 0.0833f;
  float smaaThreshold = 0.05f;
  int smaaMaxSearchSteps = 32;
  int smaaMaxSearchStepsDiag = 16;
};

struct War3SsaoSettings {
  bool enabled = false;
  float radiusPx = 6.0f;
  float strength = 1.0f;
  float bias = 0.02f;
  float power = 1.5f;
  float fadeNear = 0.15f;
  float fadeFar = 0.75f;
};

struct War3BloomSettings {
  bool enabled = true;
  float threshold = 1.18f; // 用户要求默认 1.18
  float softKnee = 0.5f;
  float intensity = 0.4f; // 用户要求默认 0.4
  bool acesToneMap = false;
};

struct War3PostFxSettings {
  bool enabled = true;
  War3AASettings aa = {};
  War3SsaoSettings ssao = {};
  War3BloomSettings bloom = {};
  float exposure = 1.0f; // 用户要求默认 1.0
  bool useSrgb = false;
};

struct War3DayNightSettings {
  bool enabled = true;
  bool affectAmbient = true;
  bool affectExposure = false;
  float transitionMinFactor = 0.85f;

  Vector4 dayAmbient = Vector4(0.28f, 0.28f, 0.28f, 1.0f);
  Vector4 nightAmbient = Vector4(0.08f, 0.10f, 0.16f, 1.0f);

  float dayExposure = 1.0f;
  float nightExposure = 1.0f;

  float dayTransitionStartDeg = -2.0f;
  float dayTransitionEndDeg = 8.0f;
};

enum class War3OutlineMode : uint8_t {
  OccludedFill = 0,
  Silhouette = 1,
};

struct War3OccludedOutlineSettings {
  bool enabled = true;
  War3OutlineMode mode = War3OutlineMode::Silhouette;
  bool useScreenSpace = true;
  float widthPx = 3.0f;
  bool showVisible = false;
  bool showOccluded = true;
  float colorR = 1.0f;
  float colorG = 1.0f;
  float colorB = 0.0f;
  float colorA = 0.8f;
};

struct War3RenderSettings {
  War3DirectionalLightSettings sun = {};
  War3ShadowSettings shadows = {};
  War3PostFxSettings postFx = {};
  War3OccludedOutlineSettings occludedOutline = {};
  War3DayNightSettings dayNight = {};

  struct {
    bool overrideEnabled = false;
    bool autoOverride = false;
    Vector4 color = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  } ambient;
};

} // namespace dxvk
