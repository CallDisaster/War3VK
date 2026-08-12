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
  PointShadow = 6,
  ShadowCurrent = 7,
  ShadowCurrentOverlay = 8,
  ShadowCsmDiagnosis = 9,
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

// Shadow receiver source policy. Keep this separate from the shader's
// four-state execution contract (Temporal expands to current-only/history):
// - DirectInline: receiver evaluates the current shadow map directly.
// - PrepassCurrentOnly: visibility prepass is sampled, history is not reused.
// - Temporal: visibility + motion + history resolve are allowed.
enum class War3ShadowTaaMode : uint8_t {
  DirectInline = 0,
  PrepassCurrentOnly = 1,
  Temporal = 2,
};

struct War3ShadowSettings {
  bool enabled = true;
  War3CsmConfig csm = {};
  float strength = 0.75f;
  // Release hardening：0.70 让 Poisson16 的采样落在更小的 texel 半径内；
  // 配合固定 4096 CSM 保持清晰边缘，不用扩大滤波掩盖几何撕裂。
  float pcfRadius = 0.70f;
  float receiverBias = 0.004f;
  float cascadeBlendRange = 120.0f;
  War3ShadowPcfKernel pcfKernel = War3ShadowPcfKernel::Poisson16;
  // DirectInline has no temporal integration. Rotating a PCF kernel with a
  // periodic world-position seed makes moving sun shadows crawl through a
  // visible stripe field, so the release baseline is deterministic.
  bool pcfRotate = false;
  War3ShadowPcfRotateMode pcfRotateMode = War3ShadowPcfRotateMode::Off;
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

  // Alpha foliage shadows must be deterministic by default. Hashed coverage
  // helps fractional transparency, but on tree cutouts it shows up as edge
  // shimmer after CSM/TAA. Keep the hard cutoff path as the visual baseline.
  bool alphaShadowHashed = false;
  bool alphaShadowUseMip = false;
  float alphaShadowMipLodBias = 0.0f;
  // Release keeps the same cutout threshold in every directional cascade so
  // a cascade blend or transition cannot change a foliage silhouette. A
  // non-zero value is only an explicit debug or author opt-in.
  float alphaShadowFarAlphaRefBias = 0.0f;

  // The bool remains as a compatibility surface for the existing UI and
  // DXVK_WAR3_SHADOW_TAA switch. A true legacy value promotes the default
  // DirectInline mode to Temporal unless DXVK_WAR3_SHADOW_TAA_MODE explicitly
  // selects a mode.
  War3ShadowTaaMode shadowTaaMode = War3ShadowTaaMode::DirectInline;
  bool shadowTaaEnabled = false;
  // UI-owned revision. Environment variables initialize the setting once in
  // War3RenderPipeline; the render pass must never re-parse them per frame.
  uint64_t shadowTaaSettingsRevision = 0u;
  // TAA v2 clarity-first default. Candidate sweeps use 0.12/0.20/0.30;
  // Temporal is still disabled by default, so this only takes effect when the
  // user explicitly selects it.
  float shadowTaaBlendFactor = 0.20f;
  bool shadowTaaNeighborClamp = true;

  // 点光源 / 点光阴影：默认关闭，关闭时 ShadowReceiver 只做 atomic 计数检查，
  // 不构建灯光快照、不渲染 cube shadow，避免拖累 CSM 主路径。
  bool pointLightsEnabled = false;
  uint32_t pointShadowMaxLights = 1;
  // 512 在默认 2000 world-unit 范围下仍约为 7.8 units/texel。资源现在按
  // configured light capacity 分配，因此默认单灯 1024 仍只占约 24 MiB D32；
  // 高级单灯可选 2048（约 96 MiB），多灯应按显存预算降档。
  uint32_t pointShadowResolution = 1024;
  uint32_t pointShadowDebugLightIndex = 0;
  float pointShadowBias = 0.05f;
  // Manual cube PCF controls. The cube is sampled with a dedicated nearest
  // sampler; these values are therefore the only intended filter footprint.
  float pointShadowPcfRadiusNear = 0.65f;
  float pointShadowPcfRadiusFar = 1.15f;
  // World-space receiver bias contribution = cube texel footprint * this
  // scale. Point cubes store radial distance, so a fixed world-space bias is
  // not enough: the quantization footprint grows with receiver distance.
  float pointShadowTexelBiasScale = 0.50f;
  // Fade optional shadowing before the finite-radius direct light reaches zero.
  float pointShadowRangeFadeStart = 0.78f;
  bool pointShadowEnabled = false;
  // Optional software ray-marched contact shadow. This traces the current
  // receiver depth toward each point light in view space, so it can recover
  // short-range contact detail that a finite-resolution cube map misses. It is
  // deliberately opt-in: the disabled path adds no depth taps, while enabled
  // cost scales with lit pixels * lights * steps.
  bool pointRayShadowEnabled = false;
  // A1 hierarchical path. The overall feature remains opt-in, but once the
  // user enables software rays this is the preferred implementation: it builds
  // one half-resolution min/max depth pyramid and traces with a strict node
  // visit budget. Disable only for A/B against the A0 linear fallback.
  bool pointRayShadowHiZEnabled = true;
  // Bound the full-screen ray cost independently from the direct-light count.
  // A0 traces only the canonical most-important prefix; 1 is the safe default.
  uint32_t pointRayShadowMaxLights = 1;
  uint32_t pointRayShadowSteps = 12;
  uint32_t pointRayShadowHiZMaxVisits = 24;
  float pointRayShadowMaxDistance = 480.0f;
  float pointRayShadowThickness = 24.0f;
  float pointRayShadowStartOffset = 10.0f;
  float pointRayShadowStrength = 0.85f;
  // 默认逐帧更新：当前内容签名不包含逐骨骼 pose，隔帧复用会让动态单位
  // 阴影落后。高级用户仍可显式开启复用以换取性能。
  bool pointShadowTemporalReuse = false;
  uint32_t pointShadowUpdatePeriod = 1;
  // 光源范围外 caster 剔除的 padding（>1 留安全边）。
  float pointShadowCasterCullPadding = 1.20f;
  // 每个 cube face 最多绘制的 caster 数；0 = 不限制。质量默认不能静默
  // 丢弃密集地图中的远端/大型 caster；需要性能档时再显式设置上限。
  uint32_t pointShadowMaxCastersPerFace = 0;
  // 按 cube face 方向做半球剔除，减少 6 面重复提交。
  bool pointShadowFaceCulling = true;
  // 显式开启点阴影后默认保持六面同帧一致；1..5 可作为性能档轮转更新，
  // 但动态 caster 会出现各面 pose 年龄不一致。0 或 >=6 = 每帧全更。
  uint32_t pointShadowMaxFacesPerFrame = 6;

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

struct War3VolumetricLightSettings {
  // CSM 对齐的体积光。默认关闭；开启后以受光雾/缺光雾的真实差值为主。
  // 关闭时 Run() 在入口即返回，不申请资源、不拷贝 depth/color。
  // intensity：整体加亮能量；weight：仅放大真实 CSM/cube 遮挡形成的
  // 受光雾/缺光雾对比，不再把整个世界统一加雾；
  // density：世界空间散射密度；sampleCount 受执行层预算硬限制，避免
  // 低机位/高分辨率时的全屏 ray-march 触发 GPU TDR。
  bool enabled = false;
  // soft-clip + composite headroom 防冲白。weight<=1 只通过少加散射形成
  // 物理阴影；weight>1 启用温和柱可读性（地表端积分后不再依赖激进 peak）。
  // 默认可读档：先保证区间正确，再谈强度；勿再靠拉满 intensity 补柱。
  // Clear-air 默认档：空气本身不应先把全屏染灰；强光与真实阴影差值负责
  // 暴露介质。局部 volume 可在 global density=0 时独立存在。
  float intensity = 1.20f;
  float decay = 0.95f;
  float density = 0.22f;
  float weight = 2.10f;
  float skyThreshold = 0.72f;
  float fadeNear = 0.0f;
  float fadeFar = 0.45f;
  // 与 sunDistance 相乘为世界积分预算；1.0 时 UI「最大距离」与实际预算一致。
  float maxRayDistance = 1.00f;
  // 4x + 16 steps keeps the default workload below the pass hard budget at
  // common 1440p extents. Visibility must be tuned through energy/density,
  // never by restoring an unbounded fragment loop.
  int sampleCount = 16;
  // TDR 合同硬下限 divisor=4；俯视靠地表端区间 + 温和可读性，不靠降 divisor。
  uint32_t resolutionDivisor = 4;
  // 地表端 [L-D,L] 下 D=sunDistance*maxRayDistance；1400 覆盖常见 RTS 射线尾。
  float sunDistance = 1400.0f;
  float froxelNear = 20.0f;
  // Independent gate for the homogeneous/global medium. Turning this off
  // preserves authored local fog volumes and their sun/point-light scattering.
  // Keep density as an authored value so toggling the gate does not destroy it.
  bool globalMediumEnabled = true;
  // Global height fog is an independent author control. Disabling it keeps
  // directional and point-light scattering active while removing the height
  // density profile from the medium.
  bool heightFogEnabled = false;
  float heightFogBase = 0.0f;
  float heightFogFalloff = 0.0012f;
  float heightFogStrength = 0.35f;
  // Stylized shaft composite: retain only a controlled part of physical
  // extinction so added scattering is not cancelled by LDR base darkening.
  float extinctionStrength = 0.05f;
  // When a ray falls outside CSM coverage (or CSM is explicitly optional),
  // permit a low-energy unshadowed medium term rather than an all-or-nothing cut.
  float unshadowedScattering = 0.03f;
  // 夜间/太阳过弱时跳过体积光，避免无可见收益仍付 full-screen 成本。
  float minSunIntensity = 0.08f;
  // 要求有效 CSM 快照；没有完整 shadow map 时直接跳过（不回退假散射）。
  bool requireCsmSnapshot = true;
  // 体积光中是否叠加点光源散射；关闭可省一次灯光快照构建。
  bool includePointLights = true;
  // Full-screen raymarch cost is samples * point lights. The volume consumer
  // selects a relevance-ranked local top-K without changing the canonical
  // shadow-first snapshot order; 0 disables point-volume while retaining sun.
  uint32_t maxPointLights = 2;
  // 按强度自适应 sampleCount：只在极弱时略降，避免默认把 24 砍到碎点区。
  bool adaptiveSampleCount = true;

  // 长期线：体积太阳柱遮挡与相机相对 CSM 解耦。
  // 开启时 ShadowReceiver 额外渲染一张太阳对齐、固定世界半径的 ortho 深度图，
  // 体积 pass 优先采样它；无效时才 fail-soft 回退相机 CSM。
  // 体积 enabled=false 时不分配、不绘制（零成本合同）。
  bool volumeSunShadowEnabled = true;
  // snapshot 无效时是否回退到相机 CSM（过渡期默认 true）。
  bool volumeSunShadowFallbackToCsm = true;
  // 远级 volume cascade 水平半径（War3 世界单位）。
  float volumeSunOrthoRadius = 3400.0f;
  // 近级半径 = far * volumeSunNearRadiusScale（固定比例，仍不随 pitch 变）。
  float volumeSunNearRadiusScale = 0.42f;
  // 向太阳侧 caster 深度余量；大于历史 C2/C3 的 384 以覆盖高空单位柱体。
  float volumeSunDepthExtension = 640.0f;
  float volumeSunDepthMargin = 96.0f;
  // 双层 depth 分辨率；1536 在近级约 4.2 units/texel。
  uint32_t volumeSunResolution = 1536;
  // 体积专用 soft compare 半径（shader 内 3x3 footprint）。
  float volumeSunSoftRadius = 1.85f;
  float volumeSunReceiverBias = 0.0075f;
  // 双级 cascade（近锐/远盖）；false 时退回单层远级。
  bool volumeSunDualCascade = true;
};

struct War3PostFxSettings {
  bool enabled = true;
  War3AASettings aa = {};
  War3SsaoSettings ssao = {};
  War3BloomSettings bloom = {};
  War3VolumetricLightSettings volumetricLight = {};
  float exposure = 1.0f; // 用户要求默认 1.0
  bool useSrgb = false;
};

enum class War3LightingClockMode : uint8_t {
  GameTime = 0,
  Held = 1,
  Independent = 2,
};

struct War3DayNightSettings {
  bool enabled = true;
  // Author-facing ownership is deliberately split. Celestial motion owns the
  // directional-light vector and shadow strength; time color grading owns the
  // sun color plus the ambient/exposure cycle. Disabling both leaves manual
  // sun direction/color writes untouched across subsequent render frames.
  bool celestialMotionEnabled = true;
  bool timeColorGradingEnabled = true;

  // GameTime follows Warcraft TIME_OF_DAY. Held uses renderTimeHours without
  // advancing. Independent advances from renderTimeHours using a render-only
  // day duration and never mutates Warcraft gameplay time.
  War3LightingClockMode clockMode = War3LightingClockMode::GameTime;
  float renderTimeHours = 12.0f;
  float independentDayLengthSeconds = 480.0f;
  uint64_t clockRevision = 1u;

  // The legacy curve remains the default until an author submits a profile.
  // Custom values are sampled at 00:00/06:00/12:00/18:00 and interpolated
  // cyclically. Kelvin values are bounded by the public JAPI to 1000..20000.
  bool customColorTemperatureProfile = false;
  float midnightKelvin = 9000.0f;
  float dawnKelvin = 2500.0f;
  float noonKelvin = 6500.0f;
  float duskKelvin = 2500.0f;

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
