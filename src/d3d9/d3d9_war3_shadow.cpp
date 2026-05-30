#include "d3d9_war3_shadow.h"
#include "d3d9_shader.h"
#include "d3d9_war3_debug.h"
#include "war3/core/war3_internal_test_config.h"
#include "war3/hooks/war3_hook_widget_identity.h"
#include "war3/render/war3_render_objects.h"
#include "war3/render/war3_shadow_runtime_bridge.h"
#include "war3/shader/war3_shader_manager.h"
#include "war3/tools/war3_diagnostics_hub.h"
#include "war3/tools/war3_perf_monitor.h"
#include "war3_shader_api.h"

#include <sstream>

#include "../dxvk/dxvk_access.h"
#include "../dxvk/dxvk_buffer.h"
#include "../dxvk/dxvk_context.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_util.h"
#include "d3d9_war3_light.h"

#include <war3_fullscreen_vert.h>
#include <war3_motion_vector.h>
#include <war3_outline_edge.h>
#include <war3_outline_expand_vert.h>
#include <war3_outline_mask.h>
#include <war3_shadow_caster_frag.h>
#include <war3_shadow_caster_vert.h>
#include <war3_shadow_receiver.h>
#include <war3_shadow_visibility.h>
#include <war3_unit_outline.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "d3d9_device.h"
#include "d3d9_texture.h"
#include "d3d9_war3_hook.h"
#include "../util/util_env.h"

namespace dxvk {

namespace {
struct ReceiverPushConstants {
  uint32_t colorSampler;
  uint32_t shadowSampler;
};

// ===== 阴影投射器推送常量 =====
// 必须与 shaders/war3_shadow_caster_vert.vert 和 frag.frag 中的 push_block
// 保持一致
struct ShadowCasterPushConstants {
  Matrix4 mvp;            // 模型视图投影矩阵
  uint32_t paletteOffset; // 骨骼调色板偏移
  uint32_t blendCount;    // 混合权重数量 (0-3)
  uint32_t flags;         // bit0=useBlend, bit1=indexed, bit2=alphaTest
  float alphaRef;         // Alpha测试阈值 (0.0-1.0)
  uint32_t samplerIndex;  // [NEW] Bindless Sampler Index
  uint32_t padding[3];    // Padding to 96 bytes (16-byte alignment)
  float outlineColor[4];  // [NEW] Outline Color (RGBA) -> Total 112 bytes
};

enum class ReceiverInputRejectReason : uint32_t {
  None = 0,
  MissingColor = 1,
  MissingDepth = 2,
  MissingCamera = 3,
  EmptyExtent = 4,
  ExtentMismatch = 5,
  BadViewport = 6,
  SmallViewport = 7,
  BadDepthRange = 8,
  BadMatrix = 9,
  StaleCamera = 10,
};

const char* ReceiverInputRejectReasonName(ReceiverInputRejectReason reason) {
  switch (reason) {
  case ReceiverInputRejectReason::None:
    return "none";
  case ReceiverInputRejectReason::MissingColor:
    return "missing-color";
  case ReceiverInputRejectReason::MissingDepth:
    return "missing-depth";
  case ReceiverInputRejectReason::MissingCamera:
    return "missing-camera";
  case ReceiverInputRejectReason::EmptyExtent:
    return "empty-extent";
  case ReceiverInputRejectReason::ExtentMismatch:
    return "extent-mismatch";
  case ReceiverInputRejectReason::BadViewport:
    return "bad-viewport";
  case ReceiverInputRejectReason::SmallViewport:
    return "small-viewport";
  case ReceiverInputRejectReason::BadDepthRange:
    return "bad-depth-range";
  case ReceiverInputRejectReason::BadMatrix:
    return "bad-matrix";
  case ReceiverInputRejectReason::StaleCamera:
    return "stale-camera";
  }
  return "unknown";
}

bool IsFiniteMatrix(const Matrix4& matrix) {
  for (uint32_t r = 0; r < 4; r++) {
    for (uint32_t c = 0; c < 4; c++) {
      if (!std::isfinite(matrix[r][c]))
        return false;
    }
  }
  return true;
}

bool EnvFlagDefault(const char* name, bool fallback) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return fallback;
  if (value == "0" || value == "false" || value == "FALSE" ||
      value == "off" || value == "OFF")
    return false;
  if (value == "1" || value == "true" || value == "TRUE" ||
      value == "on" || value == "ON")
    return true;
  return fallback;
}

template <typename T>
uint64_t War3RcObjectId(const Rc<T>& object) {
  return uint64_t(reinterpret_cast<uintptr_t>(object.ptr()));
}

float EnvFloatDefault(const char* name, float fallback) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return fallback;
  char* end = nullptr;
  const float parsed = std::strtof(value.c_str(), &end);
  return end != value.c_str() && std::isfinite(parsed) ? parsed : fallback;
}

uint32_t EnvU32Default(const char* name, uint32_t fallback) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return fallback;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == value.c_str())
    return fallback;
  return static_cast<uint32_t>(
      std::min<unsigned long>(parsed, std::numeric_limits<uint32_t>::max()));
}

bool SemanticReceiverStabilityModeEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_MODE", false);
  return s_enabled;
}

bool SemanticReceiverFreezeLastGoodLightingEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_FREEZE_LAST_GOOD_LIGHTING", true);
  return s_enabled;
}

enum class ReceiverRunEarlyReturnReason : uint32_t {
  None = 0u,
  InvalidInput = 1u,
  ShadowsAndOutlineDisabled = 2u,
  NoWorkNeeded = 3u,
  CsmComputeFailed = 4u,
};

enum ReceiverRunEntryFlag : uint32_t {
  ReceiverRunEntryInputValid = 1u << 0,
  ReceiverRunEntryShadowsEnabled = 1u << 1,
  ReceiverRunEntryOutlineEnabled = 1u << 2,
  ReceiverRunEntryHasSunShadow = 1u << 3,
  ReceiverRunEntryHasPointShadow = 1u << 4,
  ReceiverRunEntryHasPointLights = 1u << 5,
  ReceiverRunEntryNeedOutlinePass = 1u << 6,
  ReceiverRunEntryNeedsShadowMap = 1u << 7,
  ReceiverRunEntryNeedsReceiverPass = 1u << 8,
  ReceiverRunEntryHasReplayDraws = 1u << 9,
  ReceiverRunEntryShadowMapExecuted = 1u << 10,
  ReceiverRunEntryDebugShadow = 1u << 11,
};

float SemanticReceiverStableStrengthClamp() {
  static const float s_clamp = EnvFloatDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_STRENGTH_CLAMP", 0.55f);
  return s_clamp;
}

float SemanticReceiverStablePcfRadiusOverride() {
  static const float s_radius = EnvFloatDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_PCF_RADIUS", -1.0f);
  return s_radius;
}

bool SemanticReceiverHoldUntilStableIdentityEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_HOLD_SHADOWMAP_UNTIL_STABLE_IDENTITY", true);
  return s_enabled;
}

uint32_t SemanticReceiverStableIdentityFramesBeforeRedraw() {
  static const uint32_t s_frames = EnvU32Default(
      "DXVK_WAR3_SEMANTIC_IDENTITY_STABLE_FRAMES_BEFORE_REDRAW", 2u);
  return std::max<uint32_t>(1u, s_frames);
}

bool SemanticReceiverHoldOnCoverageDropEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_HOLD_SHADOWMAP_ON_COVERAGE_DROP", true);
  return s_enabled;
}

uint32_t SemanticReceiverCoverageDropTolerance() {
  static const uint32_t s_tolerance = EnvU32Default(
      "DXVK_WAR3_SEMANTIC_COVERAGE_DROP_TOLERANCE", 0u);
  return s_tolerance;
}

uint32_t SemanticReceiverCoverageDropMaxHoldFrames() {
  static const uint32_t s_frames = EnvU32Default(
      "DXVK_WAR3_SEMANTIC_COVERAGE_DROP_MAX_HOLD_FRAMES", 30u);
  return s_frames;
}

bool SemanticReceiverDisablePointLightsEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_DISABLE_POINT_LIGHTS", true);
  return s_enabled;
}

ReceiverInputRejectReason ValidateMainWorldReceiverInput(
    const War3PipelineInput& input, VkExtent3D* colorExtentOut = nullptr,
    VkExtent3D* depthExtentOut = nullptr) {
  if (!input.colorView)
    return ReceiverInputRejectReason::MissingColor;
  if (!input.depthView)
    return ReceiverInputRejectReason::MissingDepth;
  if (!input.scene.worldCamera.valid)
    return ReceiverInputRejectReason::MissingCamera;

  const VkExtent3D colorExtent = input.colorView->mipLevelExtent(0u);
  const VkExtent3D depthExtent = input.depthView->mipLevelExtent(0u);
  if (colorExtentOut)
    *colorExtentOut = colorExtent;
  if (depthExtentOut)
    *depthExtentOut = depthExtent;

  if (colorExtent.width == 0u || colorExtent.height == 0u ||
      depthExtent.width == 0u || depthExtent.height == 0u)
    return ReceiverInputRejectReason::EmptyExtent;

  if (colorExtent.width != depthExtent.width ||
      colorExtent.height != depthExtent.height)
    return ReceiverInputRejectReason::ExtentMismatch;

  const auto& cam = input.scene.worldCamera;
  if (cam.frameIndex != 0u && input.frameIndex != 0u &&
      cam.frameIndex != input.frameIndex)
    return ReceiverInputRejectReason::StaleCamera;

  const auto& vp = cam.viewport;
  if (vp.Width == 0u || vp.Height == 0u || vp.X >= colorExtent.width ||
      vp.Y >= colorExtent.height)
    return ReceiverInputRejectReason::BadViewport;

  const uint64_t vpRight = uint64_t(vp.X) + uint64_t(vp.Width);
  const uint64_t vpBottom = uint64_t(vp.Y) + uint64_t(vp.Height);
  if (vpRight > colorExtent.width || vpBottom > colorExtent.height)
    return ReceiverInputRejectReason::BadViewport;

  const uint64_t vpArea = uint64_t(vp.Width) * uint64_t(vp.Height);
  const uint64_t colorArea =
      uint64_t(colorExtent.width) * uint64_t(colorExtent.height);
  if (colorArea == 0u || vpArea * 4u < colorArea)
    return ReceiverInputRejectReason::SmallViewport;

  if (!std::isfinite(vp.MinZ) || !std::isfinite(vp.MaxZ) ||
      vp.MinZ < 0.0f || vp.MaxZ > 1.0f || vp.MaxZ <= vp.MinZ)
    return ReceiverInputRejectReason::BadDepthRange;

  if (!IsFiniteMatrix(cam.view) || !IsFiniteMatrix(cam.proj) ||
      !IsFiniteMatrix(cam.viewProj) || !IsFiniteMatrix(cam.invViewProj))
    return ReceiverInputRejectReason::BadMatrix;

  return ReceiverInputRejectReason::None;
}

// 同一帧内 BuildShadowReplayDraws 会被多个调用点重复调（executePassImpl 入口、
// renderPointShadow 等）。每次调用都做 heap allocation + N 次 push_back + 可能
// 还会做 stable_sort，是干净的合批/缓存机会。
//
// 缓存采用 thread_local + scene 三元组（地址 + size）作为命中 key：
//   - scene 的 shadowCasters / shadowInstances / shadowFallbacks 是
//     `War3FrameScene` 的成员 vector，scene 在每帧初被 reset({})，向量地址保持
//     稳定但内容可能改变；
//   - 同一帧内 vector 不会再被 push（这点由 BeforeUi 的 publish 流程保证），
//     所以 (address, size, capacity) 三元组对"同帧同 scene"是稳定的标识。
//   - 不同帧 scene 重新填充会改 size，cache 自动失效。
//   - 跨线程调用走各自 thread_local 副本，无锁竞争。
struct ReplayDrawsCacheKey {
  const void* castersAddr = nullptr;
  const void* instancesAddr = nullptr;
  const void* fallbacksAddr = nullptr;
  size_t castersSize = 0u;
  size_t instancesSize = 0u;
  size_t fallbacksSize = 0u;

  bool operator==(const ReplayDrawsCacheKey& rhs) const {
    return castersAddr == rhs.castersAddr &&
           instancesAddr == rhs.instancesAddr &&
           fallbacksAddr == rhs.fallbacksAddr &&
           castersSize == rhs.castersSize &&
           instancesSize == rhs.instancesSize &&
           fallbacksSize == rhs.fallbacksSize;
  }
};

struct ReplayDrawsCache {
  ReplayDrawsCacheKey key{};
  bool valid = false;
  std::vector<const War3ShadowCasterDraw*> draws;
};

// =====================================================================
// 2026-05-31: Path blocker 最终防线（draw-time chokepoint）
// =====================================================================
// BuildShadowReplayDraws 是所有 shadow caster（来自 6+ 个 append 站点）汇聚到
// GPU shadow map 渲染前的**唯一收集点**，每个 War3ShadowCasterDraw 都带
// rawcode / jHandle / batchHandle。在这里做最终 path blocker 过滤，覆盖任何
// 上游漏判（包括 rawcode 在上游为 0、到这里才被 widget cache 解析出来的情况）。
//
// 用户实测：上游 EntryGate/Producer 日志命中 YTfb/YTpb/YTab，但视觉仍可见。
// 说明存在某条路径，caster 在上游 rawcode=0 未被识别就 append 了。这里用
// rawcode + jHandle 双通道兜底，是 D3D9 CSM 侧最后一道闸。
//
// 诊断：env DXVK_WAR3_SHADOW_DRAW_SURVEY=1 时，前 40 个 unique rawcode 各写
// 1 行到 war3_d3d9.log，直接告诉我们 shadow map 实际画了哪些对象。

std::atomic<uint64_t> g_shadowReplayPathBlockerRejectCount{0};

inline uint32_t War3ReplayNormalizeFourCc(uint32_t rawcode) {
  if (rawcode == 0u)
    return 0u;
  // 第二字符大小写归一化（兼容 YTlc/Ytlc 编辑器输出差异）。
  auto normChar2 = [](uint32_t code) -> uint32_t {
    const uint8_t c1 = uint8_t((code >> 16) & 0xFFu);
    if (c1 >= 'A' && c1 <= 'Z')
      return (code & 0xFF00FFFFu) | (uint32_t(c1 - 'A' + 'a') << 16);
    return code;
  };
  return normChar2(rawcode);
}

inline bool War3ReplayIsPathBlockerRawcode(uint32_t rawcode) {
  if (rawcode == 0u)
    return false;
  const uint32_t norm = War3ReplayNormalizeFourCc(rawcode);
  // 同时尝试字节翻转（内存序 vs 编辑器序）。
  const uint32_t swapped = War3ReplayNormalizeFourCc(
      ((rawcode & 0xFFu) << 24) | ((rawcode & 0xFF00u) << 8) |
      ((rawcode >> 8) & 0xFF00u) | ((rawcode >> 24) & 0xFFu));
  for (uint32_t i = 0u; i < dxvk::war3::internal::kPathBlockerFourCCsCount;
       ++i) {
    const uint32_t b = dxvk::war3::internal::kPathBlockerFourCCs[i];
    if (norm == b || swapped == b || rawcode == b)
      return true;
  }
  return false;
}

inline bool War3ReplayDrawIsPathBlocker(const War3ShadowCasterDraw& draw) {
  if (!dxvk::war3::internal::kPathBlockerHideEnabled)
    return false;
  // 2026-05-31 回退：默认关闭 draw-time sweep（避免 rawcode=0 时 batchHandle
  // 误判真实单位为 path blocker，引发逐帧阴影闪烁）。上游 capture/eligibility
  // 拦截仍然生效；这里仅在显式开启时作为兜底。
  if (!dxvk::war3::internal::kPathBlockerDrawTimeSweepEnabled)
    return false;
  // 2026-05-31：caster 现在自带 rawcode（各 append 站点填充）。优先用它，
  // 其次用 batchHandle → widget cache 兜底。
  if (War3ReplayIsPathBlockerRawcode(draw.rawcode))
    return true;
  const uint32_t handle = draw.jHandle != 0u ? draw.jHandle : draw.batchHandle;
  if (handle != 0u) {
    const uint32_t cached =
        dxvk::war3::hooks::QueryWidgetRawcodeByHandle(handle);
    if (cached != 0u && War3ReplayIsPathBlockerRawcode(cached))
      return true;
  }
  return false;
}

inline void War3ReplayDrawSurvey(const War3ShadowCasterDraw& draw) {
  // 2026-05-31：survey 默认开启（一次性 40 条，节流），让下一次实机测试
  // 自动把 shadow map 实际画的对象身份写进 war3_d3d9.log，不再依赖用户设 env。
  // env DXVK_WAR3_SHADOW_DRAW_SURVEY=0 可显式关闭。
  static const bool enabled = []() {
    const char* env = std::getenv("DXVK_WAR3_SHADOW_DRAW_SURVEY");
    return env == nullptr || env[0] == '\0' || env[0] != '0';
  }();
  if (!enabled)
    return;
  // 优先用 caster 自带 rawcode；为 0 时用 batchHandle 查 widget cache。
  uint32_t rawcode = draw.rawcode;
  const uint32_t handle = draw.jHandle != 0u ? draw.jHandle : draw.batchHandle;
  if (rawcode == 0u && handle != 0u)
    rawcode = dxvk::war3::hooks::QueryWidgetRawcodeByHandle(handle);
  static std::atomic<uint32_t> s_logCount{0};
  static std::array<std::atomic<uint32_t>, 40> s_logged{};
  if (s_logCount.load(std::memory_order_relaxed) >= 40u)
    return;
  // 2026-05-31：rawcode/handle 都为 0 的"匿名 caster"才是 path blocker 漏网
  // 的嫌疑对象（上游没识别到身份）。用 (category, vertexCount) 合成一个 dedup
  // key，确保这类匿名几何也能被记录一次（否则旧逻辑 logKey=0 直接 return，
  // 恰好把最需要看的对象漏掉）。
  uint32_t logKey;
  if (rawcode != 0u)
    logKey = rawcode;
  else if (handle != 0u)
    logKey = 0x80000000u | handle;
  else
    logKey = 0x40000000u | ((uint32_t(draw.category) & 0xFFu) << 20) |
             (draw.numVertices & 0xFFFFFu);
  if (logKey == 0u)
    return;
  uint32_t freeSlot = 40u;
  for (uint32_t i = 0u; i < 40u; ++i) {
    const uint32_t cur = s_logged[i].load(std::memory_order_relaxed);
    if (cur == logKey)
      return;
    if (cur == 0u && freeSlot == 40u)
      freeSlot = i;
  }
  if (freeSlot >= 40u)
    return;
  uint32_t expected = 0u;
  if (!s_logged[freeSlot].compare_exchange_strong(expected, logKey,
                                                  std::memory_order_relaxed))
    return;
  const uint32_t idx = s_logCount.fetch_add(1, std::memory_order_relaxed);
  char fc[5] = {char((rawcode >> 24) & 0xFF), char((rawcode >> 16) & 0xFF),
                char((rawcode >> 8) & 0xFF), char(rawcode & 0xFF), 0};
  // 2026-05-31：补充几何签名 + 世界坐标。path blocker 漏网时 rawcode/handle
  // 多半为 0，靠"顶点数 + 世界位置（在悬崖上）+ objectKind"才能确认它就是
  // 那个看不见的 marker。worldMatrix 第 4 行是平移（DXVK 列存：m[3] = 平移）。
  const float wx = draw.worldMatrix[3].x;
  const float wy = draw.worldMatrix[3].y;
  const float wz = draw.worldMatrix[3].z;
  char buf[320];
  snprintf(buf, sizeof(buf),
           "DXVK War3Shadow: SHADOW DRAW SURVEY #%u rawcode=0x%08X (%s) "
           "batchHandle=0x%X kind=%u cat=%d vtx=%u idx=%u blend=%d "
           "pos=(%.0f,%.0f,%.0f)",
           idx + 1, rawcode, (rawcode != 0u ? fc : "----"), unsigned(handle),
           unsigned(draw.objectKind), int(draw.category),
           unsigned(draw.numVertices), unsigned(draw.indexCount),
           int(draw.vertexBlendEnabled), double(wx), double(wy), double(wz));
  ::dxvk::Logger::info(buf);
}

std::vector<const War3ShadowCasterDraw*> BuildShadowReplayDraws(
    const War3FrameScene& scene) {
  // thread_local 缓存：同一帧内重复调用直接复用上次的 draws 向量。
  thread_local ReplayDrawsCache cache;

  ReplayDrawsCacheKey key;
  key.castersAddr = scene.shadowCasters.data();
  key.instancesAddr = scene.shadowInstances.data();
  key.fallbacksAddr = scene.shadowFallbacks.data();
  key.castersSize = scene.shadowCasters.size();
  key.instancesSize = scene.shadowInstances.size();
  key.fallbacksSize = scene.shadowFallbacks.size();

  if (cache.valid && cache.key == key) {
    return cache.draws;
  }

  std::vector<const War3ShadowCasterDraw*>& draws = cache.draws;
  draws.clear();
  draws.reserve(scene.shadowInstances.size() + scene.shadowFallbacks.size());

  for (const auto& instance : scene.shadowInstances) {
    if (instance.replayDrawIndex >= scene.shadowCasters.size())
      continue;
    const auto& caster = scene.shadowCasters[instance.replayDrawIndex];
    // 2026-05-31：draw-time 最终 path blocker 防线（覆盖所有上游漏判）。
    if (War3ReplayDrawIsPathBlocker(caster)) {
      g_shadowReplayPathBlockerRejectCount.fetch_add(
          1, std::memory_order_relaxed);
      continue;
    }
    War3ReplayDrawSurvey(caster);
    draws.push_back(&caster);
  }

  for (const auto& fallback : scene.shadowFallbacks) {
    if (War3ReplayDrawIsPathBlocker(fallback.snapshot)) {
      g_shadowReplayPathBlockerRejectCount.fetch_add(
          1, std::memory_order_relaxed);
      continue;
    }
    War3ReplayDrawSurvey(fallback.snapshot);
    draws.push_back(&fallback.snapshot);
  }

  if (!war3::internal::kShadowReplayCasterCapEnabled) {
    cache.key = key;
    cache.valid = true;
    return draws;
  }

  const size_t cap =
      std::max<size_t>(war3::internal::kShadowReplayCasterCap, 1u);
  if (draws.size() <= cap) {
    cache.key = key;
    cache.valid = true;
    return draws;
  }

  auto computeTier = [](const War3ShadowCasterDraw& draw) {
    if (draw.category == War3RenderState::StageCategory::Terrain)
      return 0u;

    switch (static_cast<war3::render::ObjectKind>(draw.objectKind)) {
    case war3::render::ObjectKind::Building:
      return 1u;
    case war3::render::ObjectKind::Unit:
      return 2u;
    case war3::render::ObjectKind::Destructible:
    case war3::render::ObjectKind::Item:
      return 3u;
    case war3::render::ObjectKind::Effect:
      return 5u;
    default:
      break;
    }

    if (draw.alphaBlendEnabled && !draw.alphaTestEnabled)
      return 5u;

    return 4u;
  };

  auto computeDepth = [&](const War3ShadowCasterDraw& draw) {
    if (!scene.worldCamera.valid || !(draw.boundsRadius > 0.0f))
      return 0.0f;
    const Vector4 viewPos = scene.worldCamera.view * draw.boundsCenter;
    return std::abs(viewPos.z);
  };

  struct RankedDraw {
    const War3ShadowCasterDraw* draw = nullptr;
    uint32_t tier = 0;
    float depth = 0.0f;
    uint32_t order = 0;
  };

  std::vector<RankedDraw> ranked;
  ranked.reserve(draws.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(draws.size()); i++) {
    ranked.push_back(
        RankedDraw{draws[i], computeTier(*draws[i]), computeDepth(*draws[i]), i});
  }

  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const RankedDraw& a, const RankedDraw& b) {
                     if (a.tier != b.tier)
                       return a.tier < b.tier;
                     if (a.depth != b.depth)
                       return a.depth < b.depth;
                     return a.order < b.order;
                   });

  std::vector<const War3ShadowCasterDraw*> limited;
  limited.reserve(cap);
  for (size_t i = 0; i < cap; i++)
    limited.push_back(ranked[i].draw);

  static uint32_t s_capLogCounter = 0;
  if ((s_capLogCounter++ % 300u) == 0u) {
    WAR3_RENDER_LOG(
        "DXVK War3Shadow: replay caster cap active total=%u kept=%u dropped=%u\n",
        static_cast<unsigned>(draws.size()), static_cast<unsigned>(limited.size()),
        static_cast<unsigned>(draws.size() - limited.size()));
  }

  // 把裁剪后的 limited 写回 cache.draws（覆盖 unfiltered draws），后续同 scene
  // 调用直接命中。
  draws = std::move(limited);
  cache.key = key;
  cache.valid = true;
  return draws;
}

uint32_t ComputeAdaptiveShadowMapPeriod(size_t replayCasterCount) {
  uint32_t period =
      std::max<uint32_t>(war3::internal::kShadowAdaptiveMapUpdatePeriod, 1u);

  if (replayCasterCount >=
      war3::internal::kShadowAdaptiveMapUpdateHugeCasterThreshold) {
    period = std::max<uint32_t>(
        period, war3::internal::kShadowAdaptiveMapUpdateHugeCasterPeriod);
  } else if (replayCasterCount >=
             war3::internal::kShadowAdaptiveMapUpdateHighCasterThreshold) {
    period = std::max<uint32_t>(
        period, war3::internal::kShadowAdaptiveMapUpdateHighCasterPeriod);
  }

  return period;
}

uint64_t EstimateShadowReplayGeometryWork(
    const std::vector<const War3ShadowCasterDraw*>& replayDraws) {
  uint64_t work = 0u;
  for (const auto* draw : replayDraws) {
    if (draw == nullptr)
      continue;

    uint32_t primitiveWork = draw->indexed ? draw->indexCount
                                           : draw->vertexCount;
    if (primitiveWork == 0u)
      primitiveWork = draw->numVertices;
    work += primitiveWork;
  }
  return work;
}

uint32_t ResolveAdaptiveShadowResolution(uint32_t requestedResolution,
                                         uint64_t geometryWork) {
  if constexpr (!war3::internal::kShadowAdaptiveResolutionEnabled)
    return requestedResolution;

  if (requestedResolution <= war3::internal::kShadowAdaptiveResolutionMin)
    return requestedResolution;

  uint32_t targetResolution = requestedResolution;
  if (geometryWork >= war3::internal::kShadowAdaptiveResolutionHugeWork) {
    targetResolution = std::min<uint32_t>(
        targetResolution, war3::internal::kShadowAdaptiveResolutionHuge);
  } else if (geometryWork >=
             war3::internal::kShadowAdaptiveResolutionHighWork) {
    targetResolution = std::min<uint32_t>(
        targetResolution, war3::internal::kShadowAdaptiveResolutionHigh);
  }

  targetResolution = std::max<uint32_t>(
      targetResolution, war3::internal::kShadowAdaptiveResolutionMin);
  return targetResolution;
}

float MaxMatrixAbsDelta(const Matrix4& a, const Matrix4& b) {
  float delta = 0.0f;
  for (uint32_t i = 0; i < 4; i++) {
    delta = std::max(delta, std::abs(a[i].x - b[i].x));
    delta = std::max(delta, std::abs(a[i].y - b[i].y));
    delta = std::max(delta, std::abs(a[i].z - b[i].z));
    delta = std::max(delta, std::abs(a[i].w - b[i].w));
  }
  return delta;
}

float MaxCsmAbsDelta(const War3CsmData& a, const War3CsmData& b) {
  if (a.cascadeCount != b.cascadeCount)
    return std::numeric_limits<float>::infinity();

  float delta = 0.0f;
  for (uint32_t i = 0; i < a.cascadeCount; i++) {
    delta = std::max(delta, MaxMatrixAbsDelta(a.cascades[i].lightViewProj,
                                              b.cascades[i].lightViewProj));
    delta = std::max(delta,
                     std::abs(a.cascades[i].splitFar - b.cascades[i].splitFar));
  }
  return delta;
}

struct OutlineEdgePushConstants {
  uint32_t visibleSampler;
  uint32_t allSampler;
  float invWidth;
  float invHeight;
  float widthPx;
  uint32_t showVisible;  // 0/1
  uint32_t showOccluded; // 0/1
  float color[4];        // RGBA
};

// Must match `shaders/war3_shadow_receiver.frag` UBO layout (scalar +
// row_major). Shader order:
//   mat4 u_view; mat4 u_invViewProj; mat4 u_lightViewProj[4];
//   vec4 u_splitFar; vec4 u_params; vec4 u_params2; vec4 u_params3; vec4
//   u_params4; vec4 u_sunDir; vec4 u_params5; vec4 u_params6; vec4 u_viewport;
//   vec4 u_viewportZ; mat4 u_prevViewProj; vec4 u_taaParams;
struct ShadowReceiverUniform {
  Matrix4 view;
  Matrix4 invViewProj;
  Matrix4 lightViewProj[4];
  Vector4 splitFar;
  Vector4
      params; // x=shadowStrength, y=pcfRadius, z=invShadowRes, w=cascadeCount
  Vector4 params2; // x=receiverBias, y=cascadeBlendRange, z=debugMode,
                   // w=pointLightsEnabled
  Vector4 params3; // x=invViewportW, y=invViewportH, z=pcssEnable,
                   // w=pcssSearchRadius(texel)
  Vector4 params4; // x=pcssMinRadius(texel), y=pcssMaxRadius(texel),
                   // z=pcssDepthScale, w=cascadeBiasScale
  Vector4 sunDir;  // xyz=direction, w=unused
  Vector4
      params5; // x=normalBiasScale, y=rimIntensity, z=rimPower, w=receiverMode
  Vector4 params6;   // x=pcfKernel, y=pcfRotateMode, z=pcssSearchKernel,
                     // w=pcfCascadeRadiusScale
  Vector4 viewport;  // x=vpX, y=vpY, z=vpW, w=vpH
  Vector4 viewportZ; // x=minZ, y=maxZ, z/w unused
  Matrix4 prevViewProj;
  Vector4
      taaParams; // x=taaEnabled, y=blendFactor, z=neighborClamp, w=hasHistory
};

Vector4 Cross3(const Vector4 &a, const Vector4 &b) {
  return Vector4(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x, 0.0f);
}

Vector4 Normalize3(Vector4 v) {
  const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
  if (len2 <= std::numeric_limits<float>::min())
    return v;
  const float inv = 1.0f / std::sqrt(len2);
  v.x *= inv;
  v.y *= inv;
  v.z *= inv;
  v.w = 0.0f;
  return v;
}

Matrix4 MakeLookAtLH(const Vector4 &eye, const Vector4 &target,
                     const Vector4 &up) {
  // Row-vector convention: p' = p * M
  const Vector4 f = Normalize3(
      Vector4(target.x - eye.x, target.y - eye.y, target.z - eye.z, 0.0f));
  Vector4 r = Normalize3(Cross3(up, f));
  const Vector4 u = Cross3(f, r);

  Matrix4 m;
  m[0] = Vector4(r.x, u.x, f.x, 0.0f);
  m[1] = Vector4(r.y, u.y, f.y, 0.0f);
  m[2] = Vector4(r.z, u.z, f.z, 0.0f);
  m[3] = Vector4(-(eye.x * r.x + eye.y * r.y + eye.z * r.z),
                 -(eye.x * u.x + eye.y * u.y + eye.z * u.z),
                 -(eye.x * f.x + eye.y * f.y + eye.z * f.z), 1.0f);
  return m;
}
} // namespace

War3ShadowReceiverPass::War3ShadowReceiverPass(D3D9DeviceEx *device)
    : m_parent(device), m_device(device->GetDXVKDevice()),
      m_layout(createPipelineLayout()),
      m_shadowCasterLayout(createShadowCasterPipelineLayout()) {
  // Cannot init MRT layout here until we know we need it?
  // Better to defer init.

  DxvkSamplerKey samplerInfo = {};
  samplerInfo.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_MIPMAP_MODE_NEAREST);
  samplerInfo.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  samplerInfo.setUsePixelCoordinates(false);

  m_samplerLinear = m_device->createSampler(samplerInfo);

  DxvkSamplerKey shadowSampler = {};
  shadowSampler.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                          VK_SAMPLER_MIPMAP_MODE_NEAREST);
  shadowSampler.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  shadowSampler.setUsePixelCoordinates(false);
  m_shadowSampler = m_device->createSampler(shadowSampler);

  DxvkSamplerKey shadowSamplerLinear = {};
  shadowSamplerLinear.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                VK_SAMPLER_MIPMAP_MODE_NEAREST);
  shadowSamplerLinear.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  shadowSamplerLinear.setUsePixelCoordinates(false);
  m_shadowSamplerLinear = m_device->createSampler(shadowSamplerLinear);
  m_shadowSamplerActive = m_shadowSampler;

  // 初始化性能监控设备句柄（用于 GPU 时间戳）
  war3::War3PerfMonitor::instance().setDevice(m_device.ptr());

  // 初始化 AlphaTest 采样器（稳定树影轮廓，避免 mip 闪烁）
  DxvkSamplerKey fallbackKey = {};
  fallbackKey.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_MIPMAP_MODE_NEAREST);
  fallbackKey.setAddressModes(VK_SAMPLER_ADDRESS_MODE_REPEAT,
                              VK_SAMPLER_ADDRESS_MODE_REPEAT,
                              VK_SAMPLER_ADDRESS_MODE_REPEAT);
  fallbackKey.setLodRange(0.0f, 0.0f, 0.0f);
  m_fallbackSampler = m_device->createSampler(fallbackKey);

  // Alpha 阴影允许 Mip 的采样器（远景更稳定）
  DxvkSamplerKey fallbackMipKey = {};
  fallbackMipKey.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                           VK_SAMPLER_MIPMAP_MODE_LINEAR);
  fallbackMipKey.setAddressModes(VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                 VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                 VK_SAMPLER_ADDRESS_MODE_REPEAT);
  fallbackMipKey.setLodRange(0.0f, 12.0f, 0.0f);
  m_fallbackSamplerMip = m_device->createSampler(fallbackMipKey);

  DxvkBufferCreateInfo uboInfo = {};
  uboInfo.size = sizeof(ShadowReceiverUniform);
  uboInfo.usage =
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  uboInfo.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  uboInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
  m_shadowUniformBuffer =
      m_device->createBuffer(uboInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // Create Light Buffer
  DxvkBufferCreateInfo lightInfo = {};
  lightInfo.size = sizeof(LightUniform);
  lightInfo.usage =
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  lightInfo.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  lightInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
  m_lightBuffer =
      m_device->createBuffer(lightInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

War3ShadowReceiverPass::~War3ShadowReceiverPass() {
  war3::War3PerfMonitor::instance().shutdown();
  auto vk = m_device->vkd();
  for (auto &kv : m_pipelines) {
    if (kv.second.pipeline != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second.pipeline, nullptr);
  }
  for (auto &kv : m_shadowCasterPipelines) {
    if (kv.second.pipeline != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second.pipeline, nullptr);
  }
  if (m_motionVectorPipeline != VK_NULL_HANDLE)
    vk->vkDestroyPipeline(vk->device(), m_motionVectorPipeline, nullptr);
  if (m_shadowVisibilityPipeline != VK_NULL_HANDLE)
    vk->vkDestroyPipeline(vk->device(), m_shadowVisibilityPipeline, nullptr);
  // m_outlineMaskLayouts loop removed (not tracked in list)
  for (auto &kv : m_outlineMaskVisiblePipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
  for (auto &kv : m_outlineMaskAllPipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
  for (auto &kv : m_outlineEdgePipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
  for (auto &kv : m_outlineMaskMRTPipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
}

Rc<DxvkSampler> War3ShadowReceiverPass::getFallbackSampler(bool useMip,
                                                           float mipLodBias) {
  if (!useMip)
    return m_fallbackSampler;

  // 量化 LOD bias，避免每次微调都创建新的 VkSampler
  // 经验值：0.25 步进足以满足调参，同时不会引入明显“卡档”感。
  const float clampedBias = std::clamp(mipLodBias, -4.0f, 4.0f);
  constexpr float kStep = 0.25f;
  const int32_t q = int32_t(std::lround(clampedBias / kStep));
  if (q == 0)
    return m_fallbackSamplerMip;

  auto it = m_fallbackSamplerMipBias.find(q);
  if (it != m_fallbackSamplerMipBias.end())
    return it->second;

  const float biasQ = float(q) * kStep;

  DxvkSamplerKey key = {};
  key.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                VK_SAMPLER_MIPMAP_MODE_LINEAR);
  key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_REPEAT,
                      VK_SAMPLER_ADDRESS_MODE_REPEAT,
                      VK_SAMPLER_ADDRESS_MODE_REPEAT);
  key.setLodRange(0.0f, 12.0f, biasQ);

  Rc<DxvkSampler> sampler = m_device->createSampler(key);
  m_fallbackSamplerMipBias.emplace(q, sampler);
  return sampler;
}

const DxvkPipelineLayout *War3ShadowReceiverPass::createPipelineLayout() const {
  std::array<DxvkDescriptorSetLayoutBinding, 11> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT), // 0: color
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT), // 1: depth
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 2: shadow map (CSM)
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 3: CSM uniforms
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT), // 4: Lights
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 5: [NEW] Point Shadow Cube
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 6: [NEW] PointShadow UBO
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 7: [ShadowTAA] ShadowCurrent
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 8: [ShadowTAA] MotionVector
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 9: [ShadowTAA] ShadowHistory(Read)
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 10: [ShadowTAA] ShadowHistory(Write)
  };
  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(ReceiverPushConstants), bindings.size(), bindings.data());
}

const DxvkPipelineLayout *
War3ShadowReceiverPass::createOutlineMaskPipelineLayout() const {
  // Set 0: Implicit Sampler Heap
  // Set 0: Matrix Palette (SSBO) - Wait, we need to match shader bindings.
  // Actually, let's reuse the ShadowCaster layout logic but add the depth
  // texture.

  // C++ bindings array definition creates the layout.
  // The helper `createBuiltInPipelineLayout` assigns sets/bindings
  // automatically if we pass a simple list? No,
  // `DxvkDescriptorSetLayoutBinding` has implicit assumption. If we want to
  // support `set=1, binding=2` for depth, we might need a custom layout or rely
  // on how `createBuiltInPipelineLayout` packs it. However, looking at
  // `createShadowCasterPipelineLayout`, it defines 2 bindings. Let's assume we
  // can just append the new binding.

  std::array<DxvkDescriptorSetLayoutBinding, 3> bindings = {
      // set=0? binding=0: 骨骼矩阵SSBO
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT),
      // set=1? binding=1: Alpha测试纹理
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      // set=1? binding=2: Scene Depth for Manual Compare
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
  };

  // Note: DXVK built-in layout creation might not strictly respect "set=1" if
  // it packs everything into set 0, unless shader uses distinct sets and the
  // layout helper manages that. Given existing code works with `set=1` in
  // shader, we follow the pattern.

  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap,
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(ShadowCasterPushConstants), bindings.size(), bindings.data());
}

War3ShadowReceiverPass::Pipeline
War3ShadowReceiverPass::getPipeline(VkFormat format,
                                    VkSampleCountFlagBits samples) {
  PipelineKey key;
  key.format = format;
  key.samples = samples;

  auto it = m_pipelines.find(key);
  if (it != m_pipelines.end())
    return it->second;

  Pipeline pipeline = createPipeline(key);
  m_pipelines.insert({key, pipeline});
  return pipeline;
}

War3ShadowReceiverPass::Pipeline
War3ShadowReceiverPass::createPipeline(const PipelineKey &key) const {
  util::DxvkBuiltInGraphicsState state = {};

  state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);

  // Check for override
  auto *mat =
      war3::ShaderManager::get().getMaterial(war3shader::RenderStageId::Shadow);
  IDirect3DPixelShader9 *overridePs =
      (mat && mat->isCompiled()) ? mat->getPixelShader() : nullptr;

  std::vector<uint32_t> customSpirv;

  if (overridePs) {
    auto *d3dPs = static_cast<D3D9PixelShader *>(overridePs);

    std::stringstream ss;
    d3dPs->GetCommonShader()->GetShader()->dump(ss);
    std::string sData = ss.str();

    customSpirv.resize(sData.size() / 4);
    std::memcpy(customSpirv.data(), sData.data(), sData.size());

    state.fs.code = customSpirv.data();
    state.fs.size = customSpirv.size() * sizeof(uint32_t);
    state.fs.spec = nullptr;
  } else {
    state.fs = util::DxvkBuiltInShaderStage(war3_shadow_receiver, nullptr);
  }

  state.colorFormat = key.format;
  state.sampleCount = key.samples;

  Pipeline p;
  p.layout = m_layout;
  p.pipeline = m_device->createBuiltInGraphicsPipeline(m_layout, state);
  return p;
}

const DxvkPipelineLayout *
War3ShadowReceiverPass::createShadowCasterPipelineLayout() const {
  // Set 0: 矩阵调色板(SSBO) + Bindless Samplers (Global)
  // Set 1: Alpha测试纹理资源 (Texture View)
  std::array<DxvkDescriptorSetLayoutBinding, 2> bindings = {
      // set=0, binding=0: 骨骼矩阵SSBO
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT),
      // set=1, binding=0: Alpha测试纹理 (SAMPLED_IMAGE)
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
  };

  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, // [Fix] 启用采样器堆
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(ShadowCasterPushConstants), bindings.size(), bindings.data());
}

War3ShadowReceiverPass::ShadowCasterPipeline
War3ShadowReceiverPass::getShadowCasterPipeline(
    const ShadowCasterPipelineKey &key) {
  auto it = m_shadowCasterPipelines.find(key);
  if (it != m_shadowCasterPipelines.end())
    return it->second;

  ShadowCasterPipeline pipeline = createShadowCasterPipeline(key);
  m_shadowCasterPipelines.insert({key, pipeline});
  return pipeline;
}

War3ShadowReceiverPass::ShadowCasterPipeline
War3ShadowReceiverPass::createShadowCasterPipeline(
    const ShadowCasterPipelineKey &key) const {
  VkVertexInputBindingDescription bindings[2] = {};
  uint32_t bindingCount = 1;

  bindings[0].binding = 0;
  bindings[0].stride = key.positionStride;
  bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  if (key.blendBinding == 1) {
    bindingCount = 2;
    bindings[1].binding = 1;
    bindings[1].stride = key.blendStride;
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  }

  // ===== 顶点属性：位置、混合权重、混合索引、UV =====
  std::array<VkVertexInputAttributeDescription, 4> attributes = {};
  uint32_t attributeCount = 0;

  // 位置 (location = 0)
  attributes[attributeCount].location = 0;
  attributes[attributeCount].binding = 0;
  attributes[attributeCount].format = key.positionFormat;
  attributes[attributeCount].offset = key.positionOffset;
  attributeCount++;

  // 混合权重 (location = 1)
  if (key.blendWeightFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 1;
    attributes[attributeCount].binding = key.blendBinding;
    attributes[attributeCount].format = key.blendWeightFormat;
    attributes[attributeCount].offset = key.blendWeightOffset;
    attributeCount++;
  }

  // 混合索引 (location = 2)
  if (key.blendIndexFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 2;
    attributes[attributeCount].binding = key.blendBinding;
    attributes[attributeCount].format = key.blendIndexFormat;
    attributes[attributeCount].offset = key.blendIndexOffset;
    attributeCount++;
  }

  // UV纹理坐标 (location = 3) - Alpha测试用
  if (key.alphaTestEnabled && key.uvFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 3;
    attributes[attributeCount].binding = 0; // UV通常与Position同一缓冲区
    attributes[attributeCount].format = key.uvFormat;
    attributes[attributeCount].offset = key.uvOffset;
    attributeCount++;
  }

  VkPipelineVertexInputStateCreateInfo viState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  viState.vertexBindingDescriptionCount = bindingCount;
  viState.pVertexBindingDescriptions = bindings;
  viState.vertexAttributeDescriptionCount = attributeCount;
  viState.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo iaState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  iaState.topology = key.topology;
  iaState.primitiveRestartEnable = VK_FALSE;

  VkPipelineRasterizationStateCreateInfo rsState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rsState.depthClampEnable = VK_TRUE;
  rsState.rasterizerDiscardEnable = VK_FALSE;
  rsState.polygonMode = VK_POLYGON_MODE_FILL;
  rsState.cullMode = VK_CULL_MODE_NONE;
  rsState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  const bool depthBiasEnabled =
      (m_shadowCasterBiasConstant != 0.0f) || (m_shadowCasterBiasSlope != 0.0f);
  rsState.depthBiasEnable = depthBiasEnabled ? VK_TRUE : VK_FALSE;
  rsState.depthBiasConstantFactor = m_shadowCasterBiasConstant;
  rsState.depthBiasSlopeFactor = m_shadowCasterBiasSlope;
  rsState.depthBiasClamp = m_shadowCasterBiasClamp;
  rsState.lineWidth = 1.0f;

  VkPipelineDepthStencilStateCreateInfo dsState = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dsState.depthTestEnable = VK_TRUE;
  dsState.depthWriteEnable = VK_TRUE;
  dsState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  dsState.depthBoundsTestEnable = VK_FALSE;
  dsState.stencilTestEnable = VK_FALSE;

  util::DxvkBuiltInGraphicsState state = {};
  state.vs = util::DxvkBuiltInShaderStage(war3_shadow_caster_vert, nullptr);
  state.fs = util::DxvkBuiltInShaderStage(war3_shadow_caster_frag, nullptr);
  state.depthFormat = VK_FORMAT_D32_SFLOAT;
  state.viState = &viState;
  state.iaState = &iaState;
  state.rsState = &rsState;
  state.dsState = &dsState;

  ShadowCasterPipeline p;
  p.layout = m_shadowCasterLayout;
  p.pipeline =
      m_device->createBuiltInGraphicsPipeline(m_shadowCasterLayout, state);
  return p;
}

// [NEW] Helper to create MRT pipeline
War3ShadowReceiverPass::ShadowCasterPipeline
War3ShadowReceiverPass::createOutlineMaskPipeline(
    const ShadowCasterPipelineKey &key) const {
  VkVertexInputBindingDescription bindings[2] = {};
  uint32_t bindingCount = 1;

  bindings[0].binding = 0;
  bindings[0].stride = key.positionStride;
  bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  if (key.blendBinding == 1) {
    bindingCount = 2;
    bindings[1].binding = 1;
    bindings[1].stride = key.blendStride;
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  }

  std::array<VkVertexInputAttributeDescription, 4> attributes = {};
  uint32_t attributeCount = 0;

  attributes[attributeCount].location = 0;
  attributes[attributeCount].binding = 0;
  attributes[attributeCount].format = key.positionFormat;
  attributes[attributeCount].offset = key.positionOffset;
  attributeCount++;

  if (key.blendWeightFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 1;
    attributes[attributeCount].binding = key.blendBinding;
    attributes[attributeCount].format = key.blendWeightFormat;
    attributes[attributeCount].offset = key.blendWeightOffset;
    attributeCount++;
  }

  if (key.blendIndexFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 2;
    attributes[attributeCount].binding = key.blendBinding;
    attributes[attributeCount].format = key.blendIndexFormat;
    attributes[attributeCount].offset = key.blendIndexOffset;
    attributeCount++;
  }

  if (key.alphaTestEnabled && key.uvFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 3;
    attributes[attributeCount].binding = 0;
    attributes[attributeCount].format = key.uvFormat;
    attributes[attributeCount].offset = key.uvOffset;
    attributeCount++;
  }

  VkPipelineVertexInputStateCreateInfo viState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  viState.vertexBindingDescriptionCount = bindingCount;
  viState.pVertexBindingDescriptions = bindings;
  viState.vertexAttributeDescriptionCount = attributeCount;
  viState.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo iaState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  iaState.topology = key.topology;
  iaState.primitiveRestartEnable = VK_FALSE;

  VkPipelineRasterizationStateCreateInfo rsState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rsState.depthClampEnable = VK_FALSE;
  rsState.rasterizerDiscardEnable = VK_FALSE;
  rsState.polygonMode = VK_POLYGON_MODE_FILL;
  rsState.cullMode =
      VK_CULL_MODE_BACK_BIT; // [Fix] Cull Back faces (Draw Front only)
  rsState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // [Fix] Disable Hardware Bias (Rely on Shader Manual Bias)
  rsState.depthBiasEnable = VK_FALSE;
  rsState.depthBiasConstantFactor = 0.0f;
  rsState.depthBiasSlopeFactor = 0.0f;
  rsState.lineWidth = 1.0f;

  // Manual Depth Compare: Disable Hardware Depth Test
  VkPipelineDepthStencilStateCreateInfo dsState = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dsState.depthTestEnable = VK_FALSE;
  dsState.depthWriteEnable = VK_FALSE;
  dsState.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  dsState.stencilTestEnable = VK_FALSE;

  // MRT: 2 Color Attachments
  VkPipelineColorBlendAttachmentState blendAtt[2];
  for (int i = 0; i < 2; ++i) {
    blendAtt[i] = {};
    blendAtt[i].blendEnable = VK_FALSE;
    blendAtt[i].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT; // Only R channel needed
  }

  VkPipelineColorBlendStateCreateInfo cbInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbInfo.attachmentCount = 2;
  cbInfo.pAttachments = blendAtt;
  cbInfo.logicOpEnable = VK_FALSE;

  // Dynamic State
  std::vector<VkDynamicState> dynamicStates;
  dynamicStates.push_back(VK_DYNAMIC_STATE_VIEWPORT);
  dynamicStates.push_back(VK_DYNAMIC_STATE_SCISSOR);

  VkPipelineDynamicStateCreateInfo dyInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyInfo.dynamicStateCount = uint32_t(dynamicStates.size());
  dyInfo.pDynamicStates = dynamicStates.data();

  // Shader Stages
  auto vkd = m_device->vkd();
  VkShaderModuleCreateInfo vsModuleInfo = {
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  vsModuleInfo.codeSize = sizeof(war3_shadow_caster_vert);
  vsModuleInfo.pCode = war3_shadow_caster_vert;
  VkShaderModule vsModule;
  if (vkd->vkCreateShaderModule(vkd->device(), &vsModuleInfo, nullptr,
                                &vsModule) != VK_SUCCESS)
    return {};

  VkShaderModuleCreateInfo fsModuleInfo = {
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  fsModuleInfo.codeSize = sizeof(war3_outline_mask);
  fsModuleInfo.pCode = war3_outline_mask;
  VkShaderModule fsModule;
  if (vkd->vkCreateShaderModule(vkd->device(), &fsModuleInfo, nullptr,
                                &fsModule) != VK_SUCCESS) {
    vkd->vkDestroyShaderModule(vkd->device(), vsModule, nullptr);
    return {};
  }

  VkPipelineShaderStageCreateInfo stages[2];
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vsModule;
  stages[0].pName = "main";
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fsModule;
  stages[1].pName = "main";

  // Graphics Pipeline
  VkGraphicsPipelineCreateInfo pipeInfo = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeInfo.stageCount = 2;
  pipeInfo.pStages = stages;
  pipeInfo.pVertexInputState = &viState;
  pipeInfo.pInputAssemblyState = &iaState;
  pipeInfo.pViewportState =
      nullptr; // Ignored if dynamic, but usually needs non-null ptr with
               // counts? Spec says pViewportState must be valid if
               // rasterization is not disabled. But if dynamic, the counts in
               // pViewportState matter.
  VkPipelineViewportStateCreateInfo vpInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vpInfo.viewportCount = 1;
  vpInfo.scissorCount = 1;
  pipeInfo.pViewportState = &vpInfo;

  pipeInfo.pRasterizationState = &rsState;
  pipeInfo.pMultisampleState = nullptr; // Must be valid!
  VkPipelineMultisampleStateCreateInfo msInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  msInfo.minSampleShading = 1.0f;
  pipeInfo.pMultisampleState = &msInfo;

  pipeInfo.pDepthStencilState = &dsState;
  pipeInfo.pColorBlendState = &cbInfo;
  pipeInfo.pDynamicState = &dyInfo;
  pipeInfo.layout = m_outlineMaskLayout->getPipelineLayout();
  pipeInfo.renderPass = VK_NULL_HANDLE;
  pipeInfo.subpass = 0;

  // Dynamic Rendering
  // If SDK is old, we might need extension struct.
  // Assuming user has VK_KHR_dynamic_rendering or 1.3
  // DXVK D3D9 uses it logic internally.
  // If we pass VK_NULL_HANDLE, we MUST chain VkPipelineRenderingCreateInfo.
  VkPipelineRenderingCreateInfo renderingInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkFormat colorFormats[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM};
  renderingInfo.colorAttachmentCount = 2;
  renderingInfo.pColorAttachmentFormats = colorFormats;
  pipeInfo.pNext = &renderingInfo;

  VkPipeline pipeline;
  if (vkd->vkCreateGraphicsPipelines(vkd->device(), VK_NULL_HANDLE, 1,
                                     &pipeInfo, nullptr,
                                     &pipeline) != VK_SUCCESS) {
    vkd->vkDestroyShaderModule(vkd->device(), vsModule, nullptr);
    vkd->vkDestroyShaderModule(vkd->device(), fsModule, nullptr);
    return {};
  }

  vkd->vkDestroyShaderModule(vkd->device(), vsModule, nullptr);
  vkd->vkDestroyShaderModule(vkd->device(), fsModule, nullptr);

  ShadowCasterPipeline p;
  p.layout = m_outlineMaskLayout;
  p.pipeline = pipeline;
  return p;
}

void War3ShadowReceiverPass::renderMotionVectors(
    const Rc<DxvkCommandList> &ctx, const War3PipelineInput &input) {
  if (!m_motionVectorView || !m_depthCopyView || !m_shadowUniformBuffer)
    return;

  if (m_motionVectorPipeline == VK_NULL_HANDLE) {
    util::DxvkBuiltInGraphicsState state = {};
    state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
    state.fs = util::DxvkBuiltInShaderStage(war3_motion_vector, nullptr);
    state.colorFormat = VK_FORMAT_R16G16_SFLOAT;
    state.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    m_motionVectorPipeline =
        m_device->createBuiltInGraphicsPipeline(m_layout, state);
  }

  if (m_motionVectorPipeline == VK_NULL_HANDLE)
    return;

  VkExtent3D extent = m_motionVectorView->mipLevelExtent(0u);

  VkClearValue clear = {};
  clear.color.float32[0] = 0.0f;
  clear.color.float32[1] = 0.0f;
  clear.color.float32[2] = 0.0f;
  clear.color.float32[3] = 0.0f;

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = m_motionVectorView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.clearValue = clear;

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea.offset = {0u, 0u};
  renderInfo.renderArea.extent = {extent.width, extent.height};
  renderInfo.layerCount = 1u;
  renderInfo.colorAttachmentCount = 1u;
  renderInfo.pColorAttachments = &attachment;

  // Transition to COLOR_ATTACHMENT_OPTIMAL for rendering
  {
    VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = m_motionVectorView->getLayout();
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.image = m_motionVectorView->image()->handle();
    barrier.subresourceRange = m_motionVectorView->imageSubresources();

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  ctx->cmdBeginRendering(&renderInfo);

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = float(extent.width);
  viewport.height = float(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor = {};
  scissor.offset = {0, 0};
  scissor.extent = {extent.width, extent.height};

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);

  std::array<DxvkDescriptorWrite, 11> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor =
      m_colorCopyView ? m_colorCopyView->getDescriptor() : nullptr;

  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_depthCopyView->getDescriptor();

  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor =
      m_shadowMapSampleView ? m_shadowMapSampleView->getDescriptor() : nullptr;

  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[3].descriptor = nullptr;
  descriptors[3].buffer =
      m_shadowUniformBuffer->getSliceInfo(0, sizeof(ShadowReceiverUniform));

  descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[4].descriptor = nullptr;
  descriptors[4].buffer =
      m_lightBuffer ? m_lightBuffer->getSliceInfo(0, sizeof(LightUniform))
                    : DxvkResourceBufferInfo{};

  descriptors[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[5].descriptor = nullptr;

  descriptors[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[6].descriptor = nullptr;

  descriptors[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[7].descriptor = nullptr;

  descriptors[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[8].descriptor = nullptr;

  descriptors[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  const uint32_t readIndex = m_shadowHistoryIndex;
  if (m_shadowHistoryView[readIndex])
    descriptors[9].descriptor = m_shadowHistoryView[readIndex]->getDescriptor();

  descriptors[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptors[10].descriptor = nullptr;

  ReceiverPushConstants pc = {};
  pc.colorSampler = m_samplerLinear->getDescriptor().samplerIndex;
  pc.shadowSampler = m_shadowSamplerActive->getDescriptor().samplerIndex;

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_GRAPHICS, m_motionVectorPipeline);
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout, descriptors.size(),
                     descriptors.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  // Transition to read-only for sampling
  VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = m_motionVectorView->getLayout();
  barrier.image = m_motionVectorView->image()->handle();
  barrier.subresourceRange = m_motionVectorView->imageSubresources();

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &barrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  ctx->track(m_motionVectorView->image(), DxvkAccess::Write);
  ctx->track(m_depthCopyView->image(), DxvkAccess::Read);
  ctx->track(m_shadowUniformBuffer, DxvkAccess::Read);
  if (m_shadowHistory[readIndex])
    ctx->track(m_shadowHistory[readIndex], DxvkAccess::Read);
  ctx->track(m_samplerLinear);
  ctx->track(m_shadowSamplerActive);
}

void War3ShadowReceiverPass::renderShadowVisibility(
    const Rc<DxvkCommandList> &ctx, const War3PipelineInput &input) {
  if (!m_shadowCurrentView || !m_depthCopyView || !m_shadowMapSampleView ||
      !m_shadowUniformBuffer)
    return;

  if (m_shadowVisibilityPipeline == VK_NULL_HANDLE) {
    util::DxvkBuiltInGraphicsState state = {};
    state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
    state.fs = util::DxvkBuiltInShaderStage(war3_shadow_visibility, nullptr);
    state.colorFormat = VK_FORMAT_R8_UNORM;
    state.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    m_shadowVisibilityPipeline =
        m_device->createBuiltInGraphicsPipeline(m_layout, state);
  }

  if (m_shadowVisibilityPipeline == VK_NULL_HANDLE)
    return;

  VkExtent3D extent = m_shadowCurrentView->mipLevelExtent(0u);

  VkClearValue clear = {};
  clear.color.float32[0] = 1.0f; // 默认无阴影
  clear.color.float32[1] = 1.0f;
  clear.color.float32[2] = 1.0f;
  clear.color.float32[3] = 1.0f;

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = m_shadowCurrentView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.clearValue = clear;

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea.offset = {0u, 0u};
  renderInfo.renderArea.extent = {extent.width, extent.height};
  renderInfo.layerCount = 1u;
  renderInfo.colorAttachmentCount = 1u;
  renderInfo.pColorAttachments = &attachment;

  // Transition to COLOR_ATTACHMENT_OPTIMAL for rendering
  {
    VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = m_shadowCurrentView->getLayout();
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.image = m_shadowCurrentView->image()->handle();
    barrier.subresourceRange = m_shadowCurrentView->imageSubresources();

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  ctx->cmdBeginRendering(&renderInfo);

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = float(extent.width);
  viewport.height = float(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor = {};
  scissor.offset = {0, 0};
  scissor.extent = {extent.width, extent.height};

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);

  std::array<DxvkDescriptorWrite, 11> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor =
      m_colorCopyView ? m_colorCopyView->getDescriptor() : nullptr;

  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_depthCopyView->getDescriptor();

  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor = m_shadowMapSampleView->getDescriptor();

  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[3].descriptor = nullptr;
  descriptors[3].buffer =
      m_shadowUniformBuffer->getSliceInfo(0, sizeof(ShadowReceiverUniform));

  descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[4].descriptor = nullptr;
  descriptors[4].buffer =
      m_lightBuffer ? m_lightBuffer->getSliceInfo(0, sizeof(LightUniform))
                    : DxvkResourceBufferInfo{};

  descriptors[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[5].descriptor = nullptr;
  descriptors[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[6].descriptor = nullptr;
  descriptors[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[7].descriptor = nullptr;
  descriptors[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[8].descriptor = nullptr;
  descriptors[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[9].descriptor = nullptr;
  descriptors[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptors[10].descriptor = nullptr;

  ReceiverPushConstants pc = {};
  pc.colorSampler = m_samplerLinear->getDescriptor().samplerIndex;
  pc.shadowSampler = m_shadowSamplerActive->getDescriptor().samplerIndex;

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_GRAPHICS,
                       m_shadowVisibilityPipeline);
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout, descriptors.size(),
                     descriptors.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  // Transition to read-only for sampling
  VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = m_shadowCurrentView->getLayout();
  barrier.image = m_shadowCurrentView->image()->handle();
  barrier.subresourceRange = m_shadowCurrentView->imageSubresources();

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &barrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  ctx->track(m_shadowCurrentView->image(), DxvkAccess::Write);
  ctx->track(m_depthCopyView->image(), DxvkAccess::Read);
  ctx->track(m_shadowMapSampleView->image(), DxvkAccess::Read);
  ctx->track(m_shadowUniformBuffer, DxvkAccess::Read);
  ctx->track(m_samplerLinear);
  ctx->track(m_shadowSamplerActive);
  reconciliation.shadowVisibilityExecutedThisFrame = 1u;
}

bool War3ShadowReceiverPass::renderShadowMap(const Rc<DxvkCommandList> &ctx,
                                             const War3PipelineInput &input,
                                             const std::vector<
                                                 const War3ShadowCasterDraw*>*
                                                 replayDrawOverride) {
  if (!m_shadowMap || !m_shadowMapSampleView)
  {
    reconciliation.shadowMapRenderSkippedNoResourcesCount = 1u;
    return false;
  }

  reconciliation.shadowMapDrawnCasters = 0u;
  reconciliation.cascadeCulledCount = 0u;
  reconciliation.shadowMapPreparedDrawCount = 0u;
  reconciliation.shadowMapAlphaTestPreparedCount = 0u;
  reconciliation.shadowMapAlphaPromotedPreparedCount = 0u;
  reconciliation.shadowMapDynamicPreparedCount = 0u;
  reconciliation.shadowMapStaticPreparedCount = 0u;
  reconciliation.shadowMapOtherPreparedCount = 0u;
  reconciliation.shadowMapCascade0DrawnCount = 0u;
  reconciliation.shadowMapCascade1DrawnCount = 0u;
  reconciliation.shadowMapCascade2DrawnCount = 0u;
  reconciliation.shadowMapCascade3DrawnCount = 0u;
  reconciliation.shadowMapCascade0CulledCount = 0u;
  reconciliation.shadowMapCascade1CulledCount = 0u;
  reconciliation.shadowMapCascade2CulledCount = 0u;
  reconciliation.shadowMapCascade3CulledCount = 0u;
  reconciliation.skinnedCasterCount = 0u;
  reconciliation.skinnedPreparedCount = 0u;
  reconciliation.skinnedDrawnCount = 0u;

  std::vector<const War3ShadowCasterDraw*> localReplayDraws;
  const std::vector<const War3ShadowCasterDraw*>* replayDrawsPtr =
      replayDrawOverride;
  if (replayDrawsPtr == nullptr) {
    localReplayDraws = BuildShadowReplayDraws(input.scene);
    replayDrawsPtr = &localReplayDraws;
  }
  const auto& replayDraws = *replayDrawsPtr;

  War3RenderSettings defaultSettings = {};
  const War3RenderSettings *settings =
      input.settings ? input.settings : &defaultSettings;
  const bool alphaShadowHashed = settings->shadows.alphaShadowHashed;
  const float alphaShadowFarAlphaRefBias =
      std::max(settings->shadows.alphaShadowFarAlphaRefBias, 0.0f);

  const uint32_t cascadeCount =
      std::min<uint32_t>(std::max<uint32_t>(m_csmData.cascadeCount, 1u), 4u);
  if (cascadeCount == 0)
    return false;

  // 首帧诊断：输出 replay draws 总数，定位渲染端输入量
  {
    static uint32_t s_diagLogCounter = 0;
    if (s_diagLogCounter++ < 5u) {
      WAR3_RENDER_LOG("DXVK ShadowMap[%u]: replayDraws=%u\n",
                      s_diagLogCounter - 1u,
                      static_cast<unsigned>(replayDraws.size()));
    }
  }

  // 1) 上传矩阵 SSBO：骨骼调色板 + 每个 draw 的 worldMatrix（用于静态物体 GPU
  // 端 MVP 计算）
  DxvkDescriptorWrite paletteDesc = {};
  paletteDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paletteDesc.buffer = ensureShadowMatrixBuffer(ctx, input, &replayDraws);
  if (paletteDesc.buffer.buffer == VK_NULL_HANDLE) {
    reconciliation.shadowMapRenderSkippedNoMatrixBufferCount = 1u;
    return false;
  }
  reconciliation.shadowMatrixSceneKey = m_shadowMatrixSceneKey;
  reconciliation.shadowMatrixUploadSerial = m_shadowMatrixUploadSerial;
  reconciliation.shadowMatrixBufferObjectPtr =
      War3RcObjectId(m_vertexBlendPaletteBuffer);
  reconciliation.shadowMatrixBufferOffset =
      uint64_t(paletteDesc.buffer.offset);
  reconciliation.shadowMatrixBufferSize = uint64_t(paletteDesc.buffer.size);
  reconciliation.shadowMatrixBufferGpuAddress =
      uint64_t(paletteDesc.buffer.gpuAddress);

  const uint32_t objectBase = m_shadowMatrixObjectBase;

  // 捕获阶段可能对“冻结 VB/IB”执行了 TRANSFER_DST 写入（RenderEdge 风格
  // snapshot）。 Shadow caster pass 将这些 buffer 作为 VERTEX/INDEX
  // 输入读取，需要显式的 transfer->vertex-input 可见性同步。
  {
    VkMemoryBarrier2 memBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    memBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    memBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    memBarrier.dstAccessMask =
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers = &memBarrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  // 2) Transition shadow map to depth attachment layout for rendering
  {
    VkImageMemoryBarrier2 toDepth = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toDepth.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toDepth.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toDepth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toDepth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toDepth.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    toDepth.image = m_shadowMap->handle();
    toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0,
                                cascadeCount};

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toDepth;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  const VkExtent3D extent = {m_shadowMapResolution, m_shadowMapResolution, 1u};
  uint32_t drawnCasters = 0;

  const uint32_t casterCount =
      static_cast<uint32_t>(replayDraws.size());
  m_shadowPreparedScratch.clear();
  m_shadowPreparedScratch.resize(casterCount);
  auto& prepared = m_shadowPreparedScratch;
  uint32_t skinnedCasterCount = 0;
  uint32_t skinnedPreparedCount = 0;
  uint32_t skinnedInvalidBufferCount = 0;
  uint32_t skinnedInvalidPipelineCount = 0;
  uint32_t preparedDrawCount = 0;
  uint32_t alphaTestPreparedCount = 0;
  uint32_t alphaPromotedPreparedCount = 0;
  uint32_t dynamicPreparedCount = 0;
  uint32_t staticPreparedCount = 0;
  uint32_t otherPreparedCount = 0;

  auto len3 = [](float x, float y, float z) {
    return std::sqrt(x * x + y * y + z * z);
  };

  // 预先准备 Pipeline 与排序 key（与级联无关）
  for (uint32_t i = 0; i < casterCount; i++) {
    const auto &draw = *replayDraws[i];
    const bool skinnedDraw = draw.vertexBlendEnabled;
    if (skinnedDraw)
      skinnedCasterCount++;
    if (draw.positionInfo.buffer == VK_NULL_HANDLE ||
        draw.positionInfo.size == 0) {
      if (skinnedDraw)
        skinnedInvalidBufferCount++;
      continue;
    }
    if (draw.indexed &&
        (draw.indexInfo.buffer == VK_NULL_HANDLE || draw.indexInfo.size == 0)) {
      if (skinnedDraw)
        skinnedInvalidBufferCount++;
      continue;
    }

    ShadowCasterPipelineKey key = {};
    key.positionFormat = draw.positionFormat;
    key.positionStride = draw.positionStride;
    key.positionOffset = draw.positionOffset;
    key.topology = draw.topology;

    if (draw.vertexBlendEnabled && draw.vertexBlendCount > 0) {
      key.blendWeightFormat = draw.blendWeightFormat;
      key.blendWeightOffset = draw.blendWeightOffset;
    } else {
      key.blendWeightFormat = VK_FORMAT_UNDEFINED;
      key.blendWeightOffset = 0;
    }

    if (draw.vertexBlendEnabled && draw.vertexBlendIndexed) {
      key.blendIndexFormat = draw.blendIndexFormat;
      key.blendIndexOffset = draw.blendIndexOffset;
    } else {
      key.blendIndexFormat = VK_FORMAT_UNDEFINED;
      key.blendIndexOffset = 0;
    }

    key.blendBinding = draw.blendBinding;
    key.blendStride = draw.blendStride;

    // Phase 7.52 AlphaTest 修复：把 alpha-blend only 物体（有 UV + diffuseTexture，
    // 但 classification 认为是 AlphaBlend 不是 Cutout）也 promote 成 alpha-test
    // caster。原因：War3 很多透明贴图（树叶、栅栏、半透明特效等）走的是
    // D3DRS_ALPHABLENDENABLE 但没设 ALPHATESTENABLE。如果 shadow caster 不做
    // alpha discard，整个贴图投出实心方块阴影，这正是用户反馈的
    // "带 AlphaTest 的贴图光影射过去依然视作不透明"。
    // 约束：必须同时有 UV 数据（uvFormat 有效 + uvStride > 0）和 diffuseTexture，
    // 否则 shader 取不到真实 alpha。
    const bool effectiveAlphaTestShadow =
        draw.alphaTestEnabled ||
        (draw.alphaBlendEnabled && draw.diffuseTexture &&
         draw.uvFormat != VK_FORMAT_UNDEFINED && draw.uvStride > 0u);
    const bool alphaPromotedShadow =
        effectiveAlphaTestShadow && !draw.alphaTestEnabled &&
        draw.alphaBlendEnabled;

    key.alphaTestEnabled = effectiveAlphaTestShadow;
    if (effectiveAlphaTestShadow) {
      key.uvFormat = draw.uvFormat;
      key.uvOffset = draw.uvOffset;
      key.uvStride = draw.uvStride;
    }

    PreparedShadowCaster out = {};
    out.pipeline = getShadowCasterPipeline(key);
    if (out.pipeline.pipeline == VK_NULL_HANDLE) {
      if (skinnedDraw)
        skinnedInvalidPipelineCount++;
      continue;
    }

    out.valid = true;
    out.pipelineHash = key.hash();
    out.positionBuffer = draw.positionInfo.buffer;
    out.indexBuffer = draw.indexed ? draw.indexInfo.buffer : VK_NULL_HANDLE;
    // Phase 7.52 AlphaTest 修复：保留 prepare 阶段的 effective alpha-test 决定，
    // 让 pc.flags 写入循环直接复用，不再仅看 draw.alphaTestEnabled。
    out.effectiveAlphaTest = effectiveAlphaTestShadow;
    out.alphaImageView = (effectiveAlphaTestShadow && draw.diffuseTexture)
                             ? draw.textureDescriptor.legacy.image.imageView
                             : VK_NULL_HANDLE;
    preparedDrawCount++;
    if (effectiveAlphaTestShadow)
      alphaTestPreparedCount++;
    if (alphaPromotedShadow)
      alphaPromotedPreparedCount++;
    const auto preparedKind =
        static_cast<war3::render::ObjectKind>(draw.objectKind);
    if (skinnedDraw || preparedKind == war3::render::ObjectKind::Unit ||
        preparedKind == war3::render::ObjectKind::Effect) {
      dynamicPreparedCount++;
    } else if (draw.category == War3RenderState::StageCategory::Terrain ||
               preparedKind == war3::render::ObjectKind::Building ||
               preparedKind == war3::render::ObjectKind::Destructible) {
      staticPreparedCount++;
    } else {
      otherPreparedCount++;
    }
    if (skinnedDraw)
      skinnedPreparedCount++;

    prepared[i] = out;
  }

  // 每个级联预先计算“世界半径 -> NDC 半径”的缩放（row length）
  struct CascadeCullParams {
    float row0Len = 0.0f;
    float row1Len = 0.0f;
    float row2Len = 0.0f;
  };
  std::array<CascadeCullParams, 4> cullParams = {};
  for (uint32_t c = 0; c < cascadeCount; c++) {
    const Matrix4 &m = m_csmData.cascades[c].lightViewProj;
    cullParams[c].row0Len = len3(m[0].x, m[1].x, m[2].x);
    cullParams[c].row1Len = len3(m[0].y, m[1].y, m[2].y);
    cullParams[c].row2Len = len3(m[0].z, m[1].z, m[2].z);
  }

  auto intersectsCascade = [&](const War3ShadowCasterDraw &draw,
                               uint32_t cascadeIdx) -> bool {
    using war3::render::ObjectKind;

    if (draw.category == War3RenderState::StageCategory::Terrain)
      return true;
    if (!(draw.boundsRadius > 0.0f))
      return true;
    // Phase 7.92：适配 War3 RTS 俯视镜头。cascade 0/1 覆盖玩家视野核心区域，
    // 不做 cull 保证近处阴影完整；cascade 2/3 覆盖远处，做 cull 省 draw。
    if (cascadeIdx < 2u)
      return true;
    const auto objectKind = static_cast<ObjectKind>(draw.objectKind);
    if constexpr (war3::internal::kShadowCascadeCullDisableForUnits) {
      if (objectKind == ObjectKind::Unit)
        return true;
    }

    const Matrix4 &m = m_csmData.cascades[cascadeIdx].lightViewProj;
    const Vector4 clip = m * draw.boundsCenter;
    const float absW = std::abs(clip.w);
    if (!(absW > 1e-6f))
      return true;

    const float invW = 1.0f / absW;
    const float ndcX = clip.x * invW;
    const float ndcY = clip.y * invW;
    const float ndcZ = clip.z * invW;

    const auto &p = cullParams[cascadeIdx];
    float r = draw.boundsRadius;
    float guard = war3::internal::kShadowCascadeCullGuardBandNdc;
    float zGuard = guard;
    if (draw.vertexBlendEnabled) {
      r = r * war3::internal::kShadowCascadeCullSkinnedRadiusScale +
          war3::internal::kShadowCascadeCullSkinnedExtraRadius;
      guard =
          std::max(guard,
                   war3::internal::kShadowCascadeCullSkinnedExtraGuardNdc);
      zGuard =
          std::max(zGuard,
                   war3::internal::kShadowCascadeCullSkinnedZExtraGuardNdc);
    } else if (objectKind == ObjectKind::Building) {
      r *= war3::internal::kShadowCascadeCullBuildingRadiusScale;
      guard =
          std::max(guard,
                   war3::internal::kShadowCascadeCullBuildingExtraGuardNdc);
      zGuard = std::max(zGuard, guard);
    } else if (draw.indexCount >= 8192u || draw.vertexCount >= 8192u ||
               draw.numVertices >= 8192u) {
      r *= war3::internal::kShadowCascadeCullLargeDrawRadiusScale;
      guard =
          std::max(guard,
                   war3::internal::kShadowCascadeCullLargeDrawExtraGuardNdc);
      zGuard = std::max(zGuard, guard);
    }
    const float rX = r * p.row0Len * invW;
    const float rY = r * p.row1Len * invW;
    const float rZ = r * p.row2Len * invW;

    if (ndcX + rX < -1.0f - guard || ndcX - rX > 1.0f + guard)
      return false;
    if (ndcY + rY < -1.0f - guard || ndcY - rY > 1.0f + guard)
      return false;
    // Vulkan NDC: z ∈ [0, 1]
    if (ndcZ + rZ < -zGuard || ndcZ - rZ > 1.0f + zGuard)
      return false;

    return true;
  };

  m_shadowSortedDrawIndicesScratch.clear();
  m_shadowSortedDrawIndicesScratch.reserve(casterCount);
  auto& sortedDrawIndices = m_shadowSortedDrawIndicesScratch;
  for (uint32_t i = 0; i < casterCount; i++) {
    if (prepared[i].valid)
      sortedDrawIndices.push_back(i);
  }
  if (sortedDrawIndices.size() > 1) {
    std::sort(sortedDrawIndices.begin(), sortedDrawIndices.end(),
              [&](uint32_t a, uint32_t b) {
                const auto &ka = prepared[a];
                const auto &kb = prepared[b];
                if (ka.pipelineHash != kb.pipelineHash)
                  return ka.pipelineHash < kb.pipelineHash;
                if (ka.alphaImageView != kb.alphaImageView)
                  return ka.alphaImageView < kb.alphaImageView;
                if (ka.positionBuffer != kb.positionBuffer)
                  return ka.positionBuffer < kb.positionBuffer;
                if (ka.indexBuffer != kb.indexBuffer)
                  return ka.indexBuffer < kb.indexBuffer;
                return a < b;
              });
  }

  m_shadowDrawIndicesScratch.clear();
  m_shadowDrawIndicesScratch.reserve(sortedDrawIndices.size());
  auto& drawIndices = m_shadowDrawIndicesScratch;
  std::array<uint32_t, 4> culledPerCascade = {};
  std::array<uint32_t, 4> drawnPerCascade = {};
  std::array<uint32_t, 4> skinnedCulledPerCascade = {};
  std::array<uint32_t, 4> skinnedDrawnPerCascade = {};

  for (uint32_t c = 0; c < cascadeCount; c++) {
    if (!m_shadowMapLayerViews[c])
      continue;

    drawIndices.clear();
    uint32_t culled = 0;
    for (uint32_t i : sortedDrawIndices) {
      const auto &draw = *replayDraws[i];
      if (!intersectsCascade(draw, c)) {
        culled++;
        if (draw.vertexBlendEnabled)
          skinnedCulledPerCascade[c]++;
        continue;
      }
      drawIndices.push_back(i);
    }

    culledPerCascade[c] = culled;

    const float alphaRefBiasCascade =
        (cascadeCount > 1)
            ? alphaShadowFarAlphaRefBias * (float(c) / float(cascadeCount - 1))
            : 0.0f;

    VkClearValue clearValue = {};
    clearValue.depthStencil.depth = 1.0f;
    clearValue.depthStencil.stencil = 0;

    VkRenderingAttachmentInfo depthAttachment = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = m_shadowMapLayerViews[c]->handle();
    depthAttachment.imageLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue = clearValue;

    VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea.offset = {0u, 0u};
    renderInfo.renderArea.extent = {extent.width, extent.height};
    renderInfo.layerCount = 1u;
    renderInfo.colorAttachmentCount = 0u;
    renderInfo.pDepthAttachment = &depthAttachment;

    ctx->cmdBeginRendering(&renderInfo);

    // D3D-style NDC needs a negative viewport height (receiver shader assumes
    // this).
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = float(extent.height);
    viewport.width = float(extent.width);
    viewport.height = -float(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {extent.width, extent.height};

    ctx->cmdSetViewport(1, &viewport);
    ctx->cmdSetScissor(1, &scissor);

    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkBuffer boundVb0 = VK_NULL_HANDLE;
    VkDeviceSize boundVb0Offset = 0;
    VkDeviceSize boundVb0Size = 0;
    VkDeviceSize boundVb0Stride = 0;
    VkBuffer boundVb1 = VK_NULL_HANDLE;
    VkDeviceSize boundVb1Offset = 0;
    VkDeviceSize boundVb1Size = 0;
    VkDeviceSize boundVb1Stride = 0;
    uint32_t boundVbCount = 0;

    VkBuffer boundIb = VK_NULL_HANDLE;
    VkDeviceSize boundIbOffset = 0;
    VkDeviceSize boundIbSize = 0;
    VkIndexType boundIbType = VK_INDEX_TYPE_UINT16;

    uint32_t cascadeDrawn = 0;

    for (uint32_t idx : drawIndices) {
      const auto &draw = *replayDraws[idx];
      const auto &prep = prepared[idx];

      ShadowCasterPushConstants pc = {};
      pc.blendCount = draw.vertexBlendCount;
      pc.flags = 0u;
      pc.mvp = m_csmData.cascades[c].lightViewProj;

      if (draw.vertexBlendEnabled) {
        pc.flags |= 0x1u;
        if (draw.vertexBlendIndexed)
          pc.flags |= 0x2u;
        pc.paletteOffset = draw.paletteIndex * 256u;
      } else {
        // 非混合物体：worldMatrix 放在矩阵 SSBO 末尾，按 drawIndex 取用
        pc.paletteOffset = objectBase + idx;
      }

      if (prep.effectiveAlphaTest && draw.diffuseTexture) {
        pc.flags |= 0x4u; // bit2 = alphaTest启用
        if (alphaShadowHashed)
          pc.flags |= 0x8u; // bit3 = Hash Alpha
        // Phase 7.52 AlphaTest 修复：alpha-blend only 物体被 promote 成 alpha-test 时，
        // 使用 0.5 作为默认 alphaRef（半透明判断的合理阈值）；
        // classification 为 Cutout 时使用 draw.alphaRef 真实值。
        const float effectiveAlphaRef =
            draw.alphaTestEnabled ? draw.alphaRef : 0.5f;
        pc.alphaRef =
            std::clamp(effectiveAlphaRef + alphaRefBiasCascade, 0.0f, 1.0f);
        pc.samplerIndex = draw.diffuseSamplerIndex;
      }

      if (prep.pipeline.pipeline != boundPipeline) {
        ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                             VK_PIPELINE_BIND_POINT_GRAPHICS,
                             prep.pipeline.pipeline);
        boundPipeline = prep.pipeline.pipeline;
      }

      std::array<DxvkDescriptorWrite, 2> descriptors = {};
      descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptors[0].buffer = paletteDesc.buffer;

      if (prep.effectiveAlphaTest && draw.diffuseTexture) {
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = &draw.textureDescriptor;
        ctx->track(draw.diffuseTexture->image(), DxvkAccess::Read);
      } else {
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = nullptr;
      }

      ctx->bindResources(DxvkCmdBuffer::ExecBuffer, prep.pipeline.layout,
                         descriptors.size(), descriptors.data(), sizeof(pc),
                         &pc);

      if (draw.positionStorage.ptr() != nullptr)
        ctx->track(draw.positionStorage);
      if (draw.indexStorage.ptr() != nullptr &&
          draw.indexStorage.ptr() != draw.positionStorage.ptr())
        ctx->track(draw.indexStorage);
      if (draw.blendStorage.ptr() != nullptr)
        ctx->track(draw.blendStorage);

      // Bind vertex buffers（尽量去重）
      VkBuffer vb0 = draw.positionInfo.buffer;
      VkDeviceSize vb0Off = draw.positionInfo.offset;
      VkDeviceSize vb0Size = draw.positionInfo.size;
      VkDeviceSize vb0Stride = draw.positionStride;

      uint32_t vbCount = 1;
      VkBuffer vb1 = VK_NULL_HANDLE;
      VkDeviceSize vb1Off = 0;
      VkDeviceSize vb1Size = 0;
      VkDeviceSize vb1Stride = 0;

      if (draw.blendBinding == 1) {
        vbCount = 2;
        vb1 = draw.blendInfo.buffer;
        vb1Off = draw.blendInfo.offset;
        vb1Size = draw.blendInfo.size;
        vb1Stride = draw.blendStride;
      }

      const bool vbDirty =
          boundVbCount != vbCount || boundVb0 != vb0 ||
          boundVb0Offset != vb0Off || boundVb0Size != vb0Size ||
          boundVb0Stride != vb0Stride || boundVb1 != vb1 ||
          boundVb1Offset != vb1Off || boundVb1Size != vb1Size ||
          boundVb1Stride != vb1Stride;

      if (vbDirty) {
        VkBuffer vbs[2] = {vb0, vb1};
        VkDeviceSize offsets[2] = {vb0Off, vb1Off};
        VkDeviceSize sizes[2] = {vb0Size, vb1Size};
        VkDeviceSize strides[2] = {vb0Stride, vb1Stride};
        ctx->cmdBindVertexBuffers(0, vbCount, vbs, offsets, sizes, strides);

        boundVbCount = vbCount;
        boundVb0 = vb0;
        boundVb0Offset = vb0Off;
        boundVb0Size = vb0Size;
        boundVb0Stride = vb0Stride;
        boundVb1 = vb1;
        boundVb1Offset = vb1Off;
        boundVb1Size = vb1Size;
        boundVb1Stride = vb1Stride;
      }

      if (draw.indexed) {
        const bool ibDirty = boundIb != draw.indexInfo.buffer ||
                             boundIbOffset != draw.indexInfo.offset ||
                             boundIbSize != draw.indexInfo.size ||
                             boundIbType != draw.indexType;

        if (ibDirty) {
          ctx->cmdBindIndexBuffer2(draw.indexInfo.buffer, draw.indexInfo.offset,
                                   draw.indexInfo.size, draw.indexType);
          boundIb = draw.indexInfo.buffer;
          boundIbOffset = draw.indexInfo.offset;
          boundIbSize = draw.indexInfo.size;
          boundIbType = draw.indexType;
        }

        // [FIX] Restore batchHandle to TLS so D3D9Device::OnDraw can identify
        // the object for filtering
        War3RenderState::SetTlsBatchHandle(draw.batchHandle);

        ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
                            draw.vertexOffset, 0);

        // Reset handle to avoid leaking to other draws
        War3RenderState::SetTlsBatchHandle(0);
      } else {
        // [FIX] Restore batchHandle to TLS
        War3RenderState::SetTlsBatchHandle(draw.batchHandle);

        ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);

        // Reset handle
        War3RenderState::SetTlsBatchHandle(0);
      }

      drawnCasters++;
      cascadeDrawn++;
      if (draw.vertexBlendEnabled)
        skinnedDrawnPerCascade[c]++;
    }

    drawnPerCascade[c] = cascadeDrawn;
    ctx->cmdEndRendering();
  }

  if (casterCount > 0) {
    static uint32_t s_logDrawn = 0;
    const bool hasSkinnedCasters = skinnedCasterCount > 0u;
    const uint32_t logIndex = s_logDrawn++;
    // Phase 7.2: 对账计数器（每帧填充）
    reconciliation.shadowMapDrawnCasters = drawnCasters;
    reconciliation.skinnedCasterCount = skinnedCasterCount;
    reconciliation.skinnedPreparedCount = skinnedPreparedCount;
    reconciliation.skinnedInvalidBufferCount = skinnedInvalidBufferCount;
    reconciliation.skinnedInvalidPipelineCount = skinnedInvalidPipelineCount;
    reconciliation.shadowMapPreparedDrawCount = preparedDrawCount;
    reconciliation.shadowMapAlphaTestPreparedCount = alphaTestPreparedCount;
    reconciliation.shadowMapAlphaPromotedPreparedCount =
        alphaPromotedPreparedCount;
    reconciliation.shadowMapDynamicPreparedCount = dynamicPreparedCount;
    reconciliation.shadowMapStaticPreparedCount = staticPreparedCount;
    reconciliation.shadowMapOtherPreparedCount = otherPreparedCount;
    uint32_t totalCulled = 0u;
    uint32_t totalSkinnedDrawn = 0u;
    for (uint32_t c = 0; c < cascadeCount; ++c) {
      totalCulled += culledPerCascade[c];
      totalSkinnedDrawn += skinnedDrawnPerCascade[c];
    }
    reconciliation.cascadeCulledCount = totalCulled;
    reconciliation.skinnedDrawnCount = totalSkinnedDrawn;
    reconciliation.shadowMapCascade0DrawnCount = drawnPerCascade[0];
    reconciliation.shadowMapCascade1DrawnCount = drawnPerCascade[1];
    reconciliation.shadowMapCascade2DrawnCount = drawnPerCascade[2];
    reconciliation.shadowMapCascade3DrawnCount = drawnPerCascade[3];
    reconciliation.shadowMapCascade0CulledCount = culledPerCascade[0];
    reconciliation.shadowMapCascade1CulledCount = culledPerCascade[1];
    reconciliation.shadowMapCascade2CulledCount = culledPerCascade[2];
    reconciliation.shadowMapCascade3CulledCount = culledPerCascade[3];
    // 前5帧强制日志，验证 renderShadowMap 实际画了多少 caster
    if (logIndex < 5u ||
        (hasSkinnedCasters &&
         (logIndex < 12u || (logIndex % 120u) == 0u)) ||
        (!hasSkinnedCasters && (logIndex % 300u) == 0u)) {
      WAR3_RENDER_LOG(
          "DXVK ShadowMap: DrawCalls=%u Casters=%u | C0=%u(-%u) C1=%u(-%u) "
          "C2=%u(-%u) C3=%u(-%u) | Skinned=%u prep=%u badBuf=%u badPipe=%u "
          "draw=%u/%u/%u/%u cull=%u/%u/%u/%u\n",
          drawnCasters, casterCount, drawnPerCascade[0], culledPerCascade[0],
          drawnPerCascade[1], culledPerCascade[1], drawnPerCascade[2],
          culledPerCascade[2], drawnPerCascade[3], culledPerCascade[3],
          skinnedCasterCount, skinnedPreparedCount, skinnedInvalidBufferCount,
          skinnedInvalidPipelineCount, skinnedDrawnPerCascade[0],
          skinnedDrawnPerCascade[1], skinnedDrawnPerCascade[2],
          skinnedDrawnPerCascade[3], skinnedCulledPerCascade[0],
          skinnedCulledPerCascade[1], skinnedCulledPerCascade[2],
          skinnedCulledPerCascade[3]);
    }
  }

  // 3) Transition shadow map back to read-only for sampling in receiver shader
  {
    VkImageMemoryBarrier2 toRead = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toRead.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toRead.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    toRead.image = m_shadowMap->handle();
    toRead.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0,
                               cascadeCount};

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toRead;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  ctx->track(m_shadowMap, DxvkAccess::Write);
  reconciliation.shadowMapExecutedThisFrame = 1u;
  reconciliation.shadowMapRenderSerial = ++m_shadowMapRenderSerial;
  return true;
}

// [NEW] Point Light Cube Shadow Rendering
// 性能优化：只为第一个光源渲染阴影
// TODO: 完整实现需要共享 caster draw list 或重构架构
void War3ShadowReceiverPass::renderPointShadow(const Rc<DxvkCommandList> &ctx,
                                               const War3PipelineInput &input) {
  auto perfScope = war3::War3PerfMonitor::instance().scope("PointShadow", ctx);
  const auto replayDraws = BuildShadowReplayDraws(input.scene);

  War3RenderSettings defaultSettings = {};
  const War3RenderSettings *settings =
      input.settings ? input.settings : &defaultSettings;
  const bool alphaShadowHashed = settings->shadows.alphaShadowHashed;

  // 获取活跃的点光源
  auto lights = War3LightManager::Instance().GetActiveLights();
  if (lights.empty()) {
    m_pointShadowReady = false;
    return;
  }

  // 性能优化：只为第一个光源渲染阴影
  const War3PointLight &light = lights[0];

  // 确保资源已创建
  ensurePointShadowResources();
  if (!m_pointShadowCube)
    return;

  // 存储光源信息
  float range = std::max(light.position.w, 1.0f);
  m_pointShadowData.lightPos =
      Vector4(light.position.x, light.position.y, light.position.z, range);

  // 计算 6 个面的 View/Projection 矩阵
  Vector4 lightPos = m_pointShadowData.lightPos;
  float nearZ = 1.0f;
  float farZ = range;

  // Perspective projection (90° FOV, aspect 1:1)
  float fov = 1.5707963f; // PI/2 = 90 degrees
  float tanHalf = std::tan(fov * 0.5f);

  // Build perspective projection matrix (Vulkan depth [0,1])
  Matrix4 proj;
  proj[0] = Vector4(1.0f / tanHalf, 0.0f, 0.0f, 0.0f);
  proj[1] = Vector4(0.0f, 1.0f / tanHalf, 0.0f, 0.0f);
  proj[2] = Vector4(0.0f, 0.0f, farZ / (farZ - nearZ), 1.0f);
  proj[3] = Vector4(0.0f, 0.0f, -(nearZ * farZ) / (farZ - nearZ), 0.0f);

  // 6 faces: 使用 LH lookAt，保持与 Cubemap 采样约定一致
  Vector4 eye = Vector4(lightPos.x, lightPos.y, lightPos.z, 1.0f);
  // Vulkan/OpenGL Cubemap 约定（避免采样方向镜像）
  static const struct {
    Vector4 dir;
    Vector4 up;
  } faceParams[6] = {
      {{1, 0, 0, 0}, {0, -1, 0, 0}},  // +X
      {{-1, 0, 0, 0}, {0, -1, 0, 0}}, // -X
      {{0, 1, 0, 0}, {0, 0, 1, 0}},   // +Y
      {{0, -1, 0, 0}, {0, 0, -1, 0}}, // -Y
      {{0, 0, 1, 0}, {0, -1, 0, 0}},  // +Z
      {{0, 0, -1, 0}, {0, -1, 0, 0}}, // -Z
  };

  for (int face = 0; face < 6; face++) {
    Vector4 target =
        Vector4(eye.x + faceParams[face].dir.x, eye.y + faceParams[face].dir.y,
                eye.z + faceParams[face].dir.z, 1.0f);
    Matrix4 view = MakeLookAtLH(eye, target, faceParams[face].up);
    // Row-Major: p' = p * World * View * Proj, so ViewProj = view * proj
    m_pointShadowData.faceViewProj[face] = view * proj;
  }

  // 1) 上传矩阵 SSBO（骨骼调色板 + 每个 draw 的 worldMatrix）
  DxvkDescriptorWrite paletteDesc = {};
  paletteDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paletteDesc.buffer = ensureShadowMatrixBuffer(ctx, input, &replayDraws);
  if (paletteDesc.buffer.buffer == VK_NULL_HANDLE)
    return;

  const uint32_t objectBase = m_shadowMatrixObjectBase;

  // === 完整渲染：绘制 caster 场景到 6 个面 ===
  constexpr uint32_t resolution = kPointShadowResolution;

  // 1) Transition to write-optimal
  {
    VkImageMemoryBarrier2 toWrite = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toWrite.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toWrite.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toWrite.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    toWrite.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toWrite.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    toWrite.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    toWrite.image = m_pointShadowCube->handle();
    toWrite.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 6};

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toWrite;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  // 2) 渲染每个面
  for (uint32_t face = 0; face < 6; face++) {
    VkRenderingAttachmentInfo depthAtt = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = m_pointShadowFaceViews[face]->handle();
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = {{0, 0}, {resolution, resolution}};
    renderInfo.layerCount = 1;
    renderInfo.pDepthAttachment = &depthAtt;

    ctx->cmdBeginRendering(&renderInfo);

    // 设置 viewport 和 scissor
    VkViewport vp = {0.0f, 0.0f, float(resolution), float(resolution),
                     0.0f, 1.0f};
    ctx->cmdSetViewport(1, &vp);
    VkRect2D sc = {{0, 0}, {resolution, resolution}};
    ctx->cmdSetScissor(1, &sc);

    const uint32_t casterCount =
        static_cast<uint32_t>(replayDraws.size());

    // 遍历所有 casters 并绘制
    for (uint32_t drawIdx = 0; drawIdx < casterCount; drawIdx++) {
      const auto &draw = *replayDraws[drawIdx];
      if (draw.positionInfo.buffer == VK_NULL_HANDLE ||
          draw.positionInfo.size == 0)
        continue;
      if (draw.indexed &&
          (draw.indexInfo.buffer == VK_NULL_HANDLE || draw.indexInfo.size == 0))
        continue;

      // 构建 pipeline key（支持 alpha test + vertex blend）
      ShadowCasterPipelineKey key = {};
      key.positionFormat = draw.positionFormat;
      key.positionStride = draw.positionStride;
      key.positionOffset = draw.positionOffset;
      key.topology = draw.topology;

      if (draw.vertexBlendEnabled && draw.vertexBlendCount > 0) {
        key.blendWeightFormat = draw.blendWeightFormat;
        key.blendWeightOffset = draw.blendWeightOffset;
      } else {
        key.blendWeightFormat = VK_FORMAT_UNDEFINED;
        key.blendWeightOffset = 0;
      }

      if (draw.vertexBlendEnabled && draw.vertexBlendIndexed) {
        key.blendIndexFormat = draw.blendIndexFormat;
        key.blendIndexOffset = draw.blendIndexOffset;
      } else {
        key.blendIndexFormat = VK_FORMAT_UNDEFINED;
        key.blendIndexOffset = 0;
      }

      key.blendBinding = draw.blendBinding;
      key.blendStride = draw.blendStride;

      // Phase 7.52 AlphaTest 修复（点光源阴影路径）：同主 CSM 路径。
      const bool effectiveAlphaTestShadowPoint =
          draw.alphaTestEnabled ||
          (draw.alphaBlendEnabled && draw.diffuseTexture &&
           draw.uvFormat != VK_FORMAT_UNDEFINED && draw.uvStride > 0u);

      key.alphaTestEnabled = effectiveAlphaTestShadowPoint;
      if (effectiveAlphaTestShadowPoint) {
        key.uvFormat = draw.uvFormat;
        key.uvOffset = draw.uvOffset;
        key.uvStride = draw.uvStride;
      }

      ShadowCasterPipeline pipeline = getShadowCasterPipeline(key);
      if (pipeline.pipeline == VK_NULL_HANDLE)
        continue;

      ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                           VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

      // 点光源阴影：统一使用 faceViewProj；非混合 worldMatrix 从矩阵 SSBO 读取
      ShadowCasterPushConstants pc = {};
      pc.blendCount = draw.vertexBlendCount;
      pc.flags = 0u;
      pc.mvp = m_pointShadowData.faceViewProj[face];

      if (draw.vertexBlendEnabled) {
        pc.flags |= 0x1u;
        if (draw.vertexBlendIndexed)
          pc.flags |= 0x2u;
        pc.paletteOffset = draw.paletteIndex * 256u;
      } else {
        pc.paletteOffset = objectBase + drawIdx;
      }

      if (effectiveAlphaTestShadowPoint && draw.diffuseTexture) {
        pc.flags |= 0x4u;
        if (alphaShadowHashed)
          pc.flags |= 0x8u;
        // Phase 7.52 AlphaTest 修复：alpha-blend only promote 时使用 0.5 默认 alphaRef。
        const float effectiveAlphaRef =
            draw.alphaTestEnabled ? draw.alphaRef : 0.5f;
        pc.alphaRef = effectiveAlphaRef;
        pc.samplerIndex = draw.diffuseSamplerIndex;
      }

      // 使用 bindResources 绑定资源
      std::array<DxvkDescriptorWrite, 2> descriptors = {};
      descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptors[0].buffer = paletteDesc.buffer;
      if (effectiveAlphaTestShadowPoint && draw.diffuseTexture) {
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = &draw.textureDescriptor;
        ctx->track(draw.diffuseTexture->image(), DxvkAccess::Read);
      } else {
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = nullptr;
      }

      ctx->bindResources(DxvkCmdBuffer::ExecBuffer, pipeline.layout,
                         descriptors.size(), descriptors.data(), sizeof(pc),
                         &pc);

      if (draw.positionStorage.ptr() != nullptr)
        ctx->track(draw.positionStorage);
      if (draw.indexStorage.ptr() != nullptr &&
          draw.indexStorage.ptr() != draw.positionStorage.ptr())
        ctx->track(draw.indexStorage);
      if (draw.blendStorage.ptr() != nullptr)
        ctx->track(draw.blendStorage);

      // 绑定顶点缓冲
      VkBuffer vbs[2];
      VkDeviceSize offsets[2];
      VkDeviceSize sizes[2];
      VkDeviceSize strides[2];
      uint32_t vbCount = 1;

      vbs[0] = draw.positionInfo.buffer;
      offsets[0] = draw.positionInfo.offset;
      sizes[0] = draw.positionInfo.size;
      strides[0] = draw.positionStride;

      if (draw.blendBinding == 1) {
        vbCount = 2;
        vbs[1] = draw.blendInfo.buffer;
        offsets[1] = draw.blendInfo.offset;
        sizes[1] = draw.blendInfo.size;
        strides[1] = draw.blendStride;
      }

      ctx->cmdBindVertexBuffers(0, vbCount, vbs, offsets, sizes, strides);

      // 绘制
      if (draw.indexed) {
        ctx->cmdBindIndexBuffer2(draw.indexInfo.buffer, draw.indexInfo.offset,
                                 draw.indexInfo.size, draw.indexType);
        ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
                            draw.vertexOffset, 0);
      } else {
        ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
      }
    }

    ctx->cmdEndRendering();
  }

  // 3) Transition back to read-only
  {
    VkImageMemoryBarrier2 toRead = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toRead.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toRead.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    toRead.image = m_pointShadowCube->handle();
    toRead.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 6};

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toRead;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  ctx->track(m_pointShadowCube, DxvkAccess::Write);
  m_pointShadowReady = true;

  // 调试日志（首次渲染时打印）
  static bool s_loggedPointShadow = false;
  if (!s_loggedPointShadow) {
    s_loggedPointShadow = true;
    WAR3_RENDER_LOG("DXVK PointShadow: Rendered! lightPos=(%.1f,%.1f,%.1f) "
                    "range=%.1f casters=%zu\n",
                    m_pointShadowData.lightPos.x, m_pointShadowData.lightPos.y,
                    m_pointShadowData.lightPos.z, m_pointShadowData.lightPos.w,
                    replayDraws.size());
  }
}

void War3ShadowReceiverPass::drawReceiver(const Rc<DxvkCommandList> &ctx,
                                          const Rc<DxvkImageView> &dstView) {
  if (!m_colorCopyView || !m_depthCopyView || !m_shadowMapSampleView ||
      !m_shadowUniformBuffer)
    return;

  Pipeline pipeline = getPipeline(dstView->image()->info().format,
                                  dstView->image()->info().sampleCount);

  VkExtent3D extent = dstView->mipLevelExtent(0u);

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = dstView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea.offset = {0u, 0u};
  renderInfo.renderArea.extent = {extent.width, extent.height};
  renderInfo.layerCount = 1u;
  renderInfo.colorAttachmentCount = 1u;
  renderInfo.pColorAttachments = &attachment;

  ctx->cmdBeginRendering(&renderInfo);

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = float(extent.width);
  viewport.height = float(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor = {};
  scissor.offset = {0, 0};
  scissor.extent = {extent.width, extent.height};

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);

  std::array<DxvkDescriptorWrite, 11> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor = m_colorCopyView->getDescriptor();

  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_depthCopyView->getDescriptor();

  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor = m_shadowMapSampleView->getDescriptor();

  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[3].descriptor = nullptr;
  descriptors[3].buffer =
      m_shadowUniformBuffer->getSliceInfo(0, sizeof(ShadowReceiverUniform));

  // [New] Lights
  // Upload light data
  {
    LightUniform lUbo = {};
    size_t activeLightCount = 0;
    if (m_pointLightsEnabled) {
      auto activeLights = War3LightManager::Instance().GetActiveLights();
      activeLightCount = activeLights.size();
      lUbo.count = std::min<uint32_t>((uint32_t)activeLights.size(), 16u);
      for (uint32_t i = 0; i < lUbo.count; i++) {
        lUbo.lights[i].pos = activeLights[i].position;
        lUbo.lights[i].color = activeLights[i].color;
      }
      if (activeLights.empty())
        lUbo.count = 0;
    } else {
      lUbo.count = 0;
    }

    // Debug: Log light info once (unconditionally)
    static bool s_loggedLightCount = false;
    if (!s_loggedLightCount) {
      s_loggedLightCount = true;
      WAR3_RENDER_LOG("DXVK Shadow: Light system check - "
                      "activeLights.size()=%zu, lUbo.count=%u\n",
                      activeLightCount, lUbo.count);
      if (lUbo.count > 0) {
        WAR3_RENDER_LOG("DXVK Shadow: light[0] pos=(%.1f,%.1f,%.1f,%.1f) "
                        "color=(%.1f,%.1f,%.1f,%.1f)\n",
                        lUbo.lights[0].pos.x, lUbo.lights[0].pos.y,
                        lUbo.lights[0].pos.z, lUbo.lights[0].pos.w,
                        lUbo.lights[0].color.x, lUbo.lights[0].color.y,
                        lUbo.lights[0].color.z, lUbo.lights[0].color.w);
      }
    }

    auto lightInfo = m_lightBuffer->getSliceInfo(0u, sizeof(LightUniform));
    ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, lightInfo.buffer,
                         lightInfo.offset, sizeof(lUbo), &lUbo);
  }

  descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[4].descriptor = nullptr;
  descriptors[4].buffer = m_lightBuffer->getSliceInfo(0, sizeof(LightUniform));

  // [NEW] binding 5: Point Shadow Cube Map
  descriptors[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (m_pointShadowCubeView && m_pointShadowReady) {
    descriptors[5].descriptor = m_pointShadowCubeView->getDescriptor();
  } else {
    // Fallback to CSM shadow map (will be ignored in shader)
    descriptors[5].descriptor = m_shadowMapSampleView->getDescriptor();
  }

  // [NEW] binding 6: Point Shadow UBO
  // Create PointShadow UBO buffer if needed
  if (!m_pointShadowUniformBuffer) {
    DxvkBufferCreateInfo bufInfo = {};
    bufInfo.size = 256; // Aligned to 256
    bufInfo.usage =
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.stages =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    bufInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    bufInfo.debugName = "War3PointShadowUBO";
    m_pointShadowUniformBuffer =
        m_device->createBuffer(bufInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }

  // Upload PointShadow UBO
  {
    PointShadowUniform psUbo = {};
    psUbo.lightPos = m_pointShadowData.lightPos;
    psUbo.bias = m_pointShadowBias;
    psUbo.enabled =
        (m_pointShadowEnabled && m_pointShadowReady && m_pointShadowCubeView)
            ? 1.0f
            : 0.0f;

    auto psInfo = m_pointShadowUniformBuffer->getSliceInfo(
        0u, sizeof(PointShadowUniform));
    ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, psInfo.buffer,
                         psInfo.offset, sizeof(psUbo), &psUbo);
  }

  descriptors[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[6].descriptor = nullptr;
  descriptors[6].buffer =
      m_pointShadowUniformBuffer->getSliceInfo(0, sizeof(PointShadowUniform));

  // [ShadowTAA] binding 7: ShadowCurrent（R8）
  descriptors[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[7].descriptor =
      m_shadowCurrentView ? m_shadowCurrentView->getDescriptor() : nullptr;

  // [ShadowTAA] binding 8: MotionVector（RG16F）
  descriptors[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[8].descriptor =
      m_motionVectorView ? m_motionVectorView->getDescriptor() : nullptr;

  // [ShadowTAA] binding 9/10: History Read/Write（Ping-Pong）
  const uint32_t readIndex = m_shadowHistoryIndex & 1u;
  const uint32_t writeIndex = readIndex ^ 1u;
  descriptors[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[9].descriptor =
      m_shadowHistoryView[readIndex]
          ? m_shadowHistoryView[readIndex]->getDescriptor()
          : nullptr;
  descriptors[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptors[10].descriptor =
      m_shadowHistoryStorageView[writeIndex]
          ? m_shadowHistoryStorageView[writeIndex]->getDescriptor()
          : nullptr;

  ReceiverPushConstants pc = {};
  pc.colorSampler = m_samplerLinear->getDescriptor().samplerIndex;
  pc.shadowSampler = m_shadowSamplerActive->getDescriptor().samplerIndex;

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, pipeline.layout,
                     descriptors.size(), descriptors.data(), sizeof(pc), &pc);

  ctx->cmdDraw(3, 1, 0, 0);

  ctx->cmdEndRendering();

  ctx->track(dstView->image(), DxvkAccess::Write);
  ctx->track(m_colorCopy, DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Read);
  ctx->track(m_shadowMap, DxvkAccess::Read);
  ctx->track(m_shadowUniformBuffer, DxvkAccess::Read);
  ctx->track(m_lightBuffer, DxvkAccess::Read);
  if (m_pointShadowCube && m_pointShadowReady)
    ctx->track(m_pointShadowCube, DxvkAccess::Read);
  ctx->track(m_pointShadowUniformBuffer, DxvkAccess::Read);
  if (m_shadowCurrent)
    ctx->track(m_shadowCurrent, DxvkAccess::Read);
  if (m_motionVectorImage)
    ctx->track(m_motionVectorImage, DxvkAccess::Read);
  if (m_shadowHistory[readIndex])
    ctx->track(m_shadowHistory[readIndex], DxvkAccess::Read);
  if (m_shadowHistory[writeIndex])
    ctx->track(m_shadowHistory[writeIndex], DxvkAccess::Write);
  ctx->track(m_samplerLinear);
  ctx->track(m_shadowSamplerActive);
  reconciliation.receiverDrawExecutedThisFrame = 1u;
}

void War3ShadowReceiverPass::Run(const Rc<DxvkCommandList> &ctx,
                                 const War3PipelineInput &input) {
  // [Perf] Add timing scope for Shadow/Outline pass
  auto perfScope = war3::War3PerfMonitor::instance().scope("Shadow/Main", ctx);

  // Phase 7.2: 每帧重置对账计数器
  reconciliation = {};
  reconciliation.shadowMapRenderSerial = m_shadowMapRenderSerial;
  const auto strengthToMilli = [](float value) -> uint32_t {
    return uint32_t(std::clamp(value, 0.0f, 1.0f) * 1000.0f + 0.5f);
  };
  const auto recordShadowResourceFingerprint =
      [&](uint32_t historyReadIndex, uint32_t historyWriteIndex) {
        reconciliation.shadowMapImagePtr = War3RcObjectId(m_shadowMap);
        reconciliation.shadowMapSampleViewPtr =
            War3RcObjectId(m_shadowMapSampleView);
        reconciliation.shadowCurrentImagePtr =
            War3RcObjectId(m_shadowCurrent);
        reconciliation.shadowCurrentViewPtr =
            War3RcObjectId(m_shadowCurrentView);
        reconciliation.shadowHistoryReadImagePtr =
            historyReadIndex < m_shadowHistory.size()
                ? War3RcObjectId(m_shadowHistory[historyReadIndex])
                : 0u;
        reconciliation.shadowHistoryReadViewPtr =
            historyReadIndex < m_shadowHistoryView.size()
                ? War3RcObjectId(m_shadowHistoryView[historyReadIndex])
                : 0u;
        reconciliation.shadowHistoryWriteImagePtr =
            historyWriteIndex < m_shadowHistory.size()
                ? War3RcObjectId(m_shadowHistory[historyWriteIndex])
                : 0u;
        reconciliation.shadowHistoryWriteViewPtr =
            historyWriteIndex < m_shadowHistoryStorageView.size()
                ? War3RcObjectId(m_shadowHistoryStorageView[historyWriteIndex])
                : 0u;
      };
  const auto setReceiverRunEntryFlags =
      [&](bool inputValid, bool shadowsEnabled, bool outlineEnabled,
          bool hasSunShadow, bool hasPointShadow, bool hasPointLights,
          bool needOutlinePass, bool receiverNeedsShadowMap,
          bool needReceiverPass, bool hasReplayDraws,
          bool shadowMapExecuted, bool debugShadow) {
        uint32_t flags = 0u;
        if (inputValid)
          flags |= ReceiverRunEntryInputValid;
        if (shadowsEnabled)
          flags |= ReceiverRunEntryShadowsEnabled;
        if (outlineEnabled)
          flags |= ReceiverRunEntryOutlineEnabled;
        if (hasSunShadow)
          flags |= ReceiverRunEntryHasSunShadow;
        if (hasPointShadow)
          flags |= ReceiverRunEntryHasPointShadow;
        if (hasPointLights)
          flags |= ReceiverRunEntryHasPointLights;
        if (needOutlinePass)
          flags |= ReceiverRunEntryNeedOutlinePass;
        if (receiverNeedsShadowMap)
          flags |= ReceiverRunEntryNeedsShadowMap;
        if (needReceiverPass)
          flags |= ReceiverRunEntryNeedsReceiverPass;
        if (hasReplayDraws)
          flags |= ReceiverRunEntryHasReplayDraws;
        if (shadowMapExecuted)
          flags |= ReceiverRunEntryShadowMapExecuted;
        if (debugShadow)
          flags |= ReceiverRunEntryDebugShadow;
        reconciliation.receiverRunEntryFlags = flags;
      };
  const auto publishReconciliationStats = [&]() {
    auto stats = input.scene.shadowStats;
    stats.semanticSceneShadowCastersCount = reconciliation.shadowCastersCount;
    stats.semanticSceneReplayDrawsCount = reconciliation.replayDrawsCount;
    stats.semanticSceneShadowMapDrawnCasters =
        reconciliation.shadowMapDrawnCasters;
    stats.semanticSceneShadowMapCascadeCulledCount =
        reconciliation.cascadeCulledCount;
    stats.semanticSceneShadowMapSkinnedCasterCount =
        reconciliation.skinnedCasterCount;
    stats.semanticSceneShadowMapSkinnedPreparedCount =
        reconciliation.skinnedPreparedCount;
    stats.semanticSceneShadowMapSkinnedInvalidBufferCount =
        reconciliation.skinnedInvalidBufferCount;
    stats.semanticSceneShadowMapSkinnedInvalidPipelineCount =
        reconciliation.skinnedInvalidPipelineCount;
    stats.semanticSceneShadowMapSkinnedDrawnCount =
        reconciliation.skinnedDrawnCount;
    stats.semanticSceneShadowTaaActive = reconciliation.shadowTaaActive;
    stats.semanticSceneReceiverReuseShadowMap =
        reconciliation.receiverReuseShadowMap;
    stats.semanticSceneReceiverInputValid =
        reconciliation.receiverInputValid;
    stats.semanticSceneReceiverInputRejectReason =
        reconciliation.receiverInputRejectReason;
    stats.semanticSceneReceiverNeedPass = reconciliation.receiverNeedPass;
    stats.semanticSceneReceiverNeedShadowMap =
        reconciliation.receiverNeedShadowMap;
    stats.semanticSceneReceiverHasCompleteShadowMap =
        reconciliation.receiverHasCompleteShadowMap;
    stats.semanticSceneReceiverHasUsableDirectionalShadow =
        reconciliation.receiverHasUsableDirectionalShadow;
    stats.semanticSceneReceiverActiveStrengthMilli =
        reconciliation.receiverActiveStrengthMilli;
    stats.semanticSceneReceiverUboStrengthMilli =
        reconciliation.receiverUboStrengthMilli;
    stats.semanticSceneReceiverDebugMode = reconciliation.receiverDebugMode;
    stats.semanticSceneReceiverCsmCascadeCount =
        reconciliation.receiverCsmCascadeCount;
    stats.semanticSceneReceiverRunEntryFlags =
        reconciliation.receiverRunEntryFlags;
    stats.semanticSceneReceiverRunEarlyReturnReason =
        reconciliation.receiverRunEarlyReturnReason;
    stats.semanticSceneShadowMapExecutedThisFrame =
        reconciliation.shadowMapExecutedThisFrame;
    stats.semanticSceneReceiverSettingsShadowsEnabled =
        reconciliation.receiverSettingsShadowsEnabled;
    stats.semanticSceneReceiverSettingsOutlineEnabled =
        reconciliation.receiverSettingsOutlineEnabled;
    stats.semanticSceneReceiverSettingsRawStrengthMilli =
        reconciliation.receiverSettingsRawStrengthMilli;
    stats.semanticSceneReceiverComputedShadowStrengthMilli =
        reconciliation.receiverComputedShadowStrengthMilli;
    stats.semanticSceneReceiverHasSunShadow =
        reconciliation.receiverHasSunShadow;
    stats.semanticSceneReceiverHasPointShadow =
        reconciliation.receiverHasPointShadow;
    stats.semanticSceneReceiverNeedOutlinePass =
        reconciliation.receiverNeedOutlinePass;
    stats.semanticSceneReceiverZeroStrengthFrameCount =
        reconciliation.receiverZeroStrengthFrameCount;
    stats.semanticSceneReceiverDrawnWithZeroStrengthCount =
        reconciliation.receiverDrawnWithZeroStrengthCount;
    stats.semanticSceneReceiverNoCompleteShadowMapCount =
        reconciliation.receiverNoCompleteShadowMapCount;
    stats.semanticSceneReceiverNoShadowMapImageCount =
        reconciliation.receiverNoShadowMapImageCount;
    stats.semanticSceneReceiverNoShadowMapSampleViewCount =
        reconciliation.receiverNoShadowMapSampleViewCount;
    stats.semanticSceneReceiverNoCandidateCsmCount =
        reconciliation.receiverNoCandidateCsmCount;
    stats.semanticSceneReceiverCsmFallbackToLastGoodCount =
        reconciliation.receiverCsmFallbackToLastGoodCount;
    stats.semanticSceneReceiverHoldInvalidCsmCount =
        reconciliation.receiverHoldInvalidCsmCount;
    stats.semanticSceneReceiverHoldEmptyReplayCount =
        reconciliation.receiverHoldEmptyReplayCount;
    stats.semanticSceneReceiverHoldIdentityChurnCount =
        reconciliation.receiverHoldIdentityChurnCount;
    stats.semanticSceneReceiverReuseInvalidatedAfterEnsureCount =
        reconciliation.receiverReuseInvalidatedAfterEnsureCount;
    stats.semanticSceneShadowMapRenderSkippedNoResourcesCount =
        reconciliation.shadowMapRenderSkippedNoResourcesCount;
    stats.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount =
        reconciliation.shadowMapRenderSkippedNoMatrixBufferCount;
    stats.semanticSceneReceiverViewportX = reconciliation.receiverViewportX;
    stats.semanticSceneReceiverViewportY = reconciliation.receiverViewportY;
    stats.semanticSceneReceiverViewportWidth =
        reconciliation.receiverViewportWidth;
    stats.semanticSceneReceiverViewportHeight =
        reconciliation.receiverViewportHeight;
    stats.semanticSceneShadowMatrixSceneKey =
        reconciliation.shadowMatrixSceneKey;
    stats.semanticSceneShadowMatrixUploadSerial =
        reconciliation.shadowMatrixUploadSerial;
    stats.semanticSceneShadowMatrixBufferObjectPtr =
        reconciliation.shadowMatrixBufferObjectPtr;
    stats.semanticSceneShadowMatrixBufferOffset =
        reconciliation.shadowMatrixBufferOffset;
    stats.semanticSceneShadowMatrixBufferSize =
        reconciliation.shadowMatrixBufferSize;
    stats.semanticSceneShadowMatrixBufferGpuAddress =
        reconciliation.shadowMatrixBufferGpuAddress;
    stats.semanticSceneShadowMapRenderSerial =
        reconciliation.shadowMapRenderSerial;
    stats.semanticSceneShadowMapImagePtr = reconciliation.shadowMapImagePtr;
    stats.semanticSceneShadowMapSampleViewPtr =
        reconciliation.shadowMapSampleViewPtr;
    stats.semanticSceneShadowCurrentImagePtr =
        reconciliation.shadowCurrentImagePtr;
    stats.semanticSceneShadowCurrentViewPtr =
        reconciliation.shadowCurrentViewPtr;
    stats.semanticSceneShadowHistoryReadImagePtr =
        reconciliation.shadowHistoryReadImagePtr;
    stats.semanticSceneShadowHistoryReadViewPtr =
        reconciliation.shadowHistoryReadViewPtr;
    stats.semanticSceneShadowHistoryWriteImagePtr =
        reconciliation.shadowHistoryWriteImagePtr;
    stats.semanticSceneShadowHistoryWriteViewPtr =
        reconciliation.shadowHistoryWriteViewPtr;
    stats.semanticSceneShadowVisibilityExecutedThisFrame =
        reconciliation.shadowVisibilityExecutedThisFrame;
    stats.semanticSceneReceiverDrawExecutedThisFrame =
        reconciliation.receiverDrawExecutedThisFrame;
    stats.semanticSceneShadowTaaMode = reconciliation.shadowTaaMode;
    stats.semanticSceneShadowHistoryValidBefore =
        reconciliation.shadowHistoryValidBefore;
    stats.semanticSceneShadowHistoryValidAfter =
        reconciliation.shadowHistoryValidAfter;
    stats.semanticSceneShadowHistoryReadIndex =
        reconciliation.shadowHistoryReadIndex;
    stats.semanticSceneShadowHistoryWriteIndex =
        reconciliation.shadowHistoryWriteIndex;
    stats.semanticSceneShadowReceiverSampleSource =
        reconciliation.shadowReceiverSampleSource;
    dxvk::war3::render::NoteShadowSceneStats(stats);
    war3::War3PerfMonitor::instance().noteShadowBudgetFrame(stats);
  };

  static bool s_first = true;
  if (s_first) {
    s_first = false;
    WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: FIRST_CALL (BeforeUi)\n");
  }

  VkExtent3D receiverColorExtent = {};
  VkExtent3D receiverDepthExtent = {};
  const ReceiverInputRejectReason receiverInputRejectReason =
      ValidateMainWorldReceiverInput(input, &receiverColorExtent,
                                     &receiverDepthExtent);
  reconciliation.receiverInputRejectReason =
      static_cast<uint32_t>(receiverInputRejectReason);
  reconciliation.receiverInputValid =
      receiverInputRejectReason == ReceiverInputRejectReason::None ? 1u : 0u;
  if (receiverInputRejectReason != ReceiverInputRejectReason::None) {
    reconciliation.receiverRunEarlyReturnReason =
        static_cast<uint32_t>(ReceiverRunEarlyReturnReason::InvalidInput);
    setReceiverRunEntryFlags(false, false, false, false, false, false, false,
                             false, false, false, false, false);
    static uint32_t s_rejectLogs[16] = {};
    const uint32_t reasonIndex =
        std::min<uint32_t>(static_cast<uint32_t>(receiverInputRejectReason),
                           15u);
    if (s_rejectLogs[reasonIndex]++ < 8u) {
      const auto& vp = input.scene.worldCamera.viewport;
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReceiverPass: skip invalid receiver input "
          "reason=%s frame=%u cameraFrame=%u color=%ux%u depth=%ux%u "
          "vp=%ux%u@(%u,%u) z=(%.3f,%.3f)\n",
          ReceiverInputRejectReasonName(receiverInputRejectReason),
          static_cast<unsigned>(input.frameIndex),
          static_cast<unsigned>(input.scene.worldCamera.frameIndex),
          static_cast<unsigned>(receiverColorExtent.width),
          static_cast<unsigned>(receiverColorExtent.height),
          static_cast<unsigned>(receiverDepthExtent.width),
          static_cast<unsigned>(receiverDepthExtent.height),
          static_cast<unsigned>(vp.Width), static_cast<unsigned>(vp.Height),
          static_cast<unsigned>(vp.X), static_cast<unsigned>(vp.Y),
          static_cast<double>(vp.MinZ), static_cast<double>(vp.MaxZ));
    }
    if (War3RenderState::HasOutlineHandles() && !war3dbg::RenderLogEnabled()) {
      static uint32_t s_outlineSkipLogs = 0;
      if (s_outlineSkipLogs++ < 3) {
        war3dbg::Print("DXVK_Outline: skip (invalid receiver input) reason=%s "
                       "handles=%u\n",
                       ReceiverInputRejectReasonName(receiverInputRejectReason),
                       War3RenderState::GetOutlineHandleCount());
      }
    }
    publishReconciliationStats();
    return;
  }
  {
    const auto& vp = input.scene.worldCamera.viewport;
    reconciliation.receiverViewportX = vp.X;
    reconciliation.receiverViewportY = vp.Y;
    reconciliation.receiverViewportWidth = vp.Width;
    reconciliation.receiverViewportHeight = vp.Height;
    static uint32_t s_validLogs = 0;
    if (s_validLogs++ < 3u) {
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReceiverPass: receiver input valid frame=%u "
          "cameraFrame=%u color=%ux%u depth=%ux%u vp=%ux%u@(%u,%u) "
          "z=(%.3f,%.3f)\n",
          static_cast<unsigned>(input.frameIndex),
          static_cast<unsigned>(input.scene.worldCamera.frameIndex),
          static_cast<unsigned>(receiverColorExtent.width),
          static_cast<unsigned>(receiverColorExtent.height),
          static_cast<unsigned>(receiverDepthExtent.width),
          static_cast<unsigned>(receiverDepthExtent.height),
          static_cast<unsigned>(vp.Width), static_cast<unsigned>(vp.Height),
          static_cast<unsigned>(vp.X), static_cast<unsigned>(vp.Y),
          static_cast<double>(vp.MinZ), static_cast<double>(vp.MaxZ));
    }
  }

  War3RenderSettings defaultSettings = {};
  const War3RenderSettings *settings =
      input.settings ? input.settings : &defaultSettings;
  m_shadowSamplerActive =
      (settings->shadows.filterMode == War3ShadowFilterMode::Linear)
          ? m_shadowSamplerLinear
          : m_shadowSampler;
  const float casterBias = std::max(settings->shadows.casterDepthBias, 0.0f);
  const float casterSlope = std::max(settings->shadows.casterSlopeBias, 0.0f);
  const float casterClamp = std::max(settings->shadows.casterBiasClamp, 0.0f);
  if (casterBias != m_shadowCasterBiasConstant ||
      casterSlope != m_shadowCasterBiasSlope ||
      casterClamp != m_shadowCasterBiasClamp) {
    // 参数变化时重建 caster 管线，确保偏移生效
    m_shadowCasterBiasConstant = casterBias;
    m_shadowCasterBiasSlope = casterSlope;
    m_shadowCasterBiasClamp = casterClamp;

    auto vk = m_device->vkd();
    for (auto &kv : m_shadowCasterPipelines) {
      if (kv.second.pipeline != VK_NULL_HANDLE)
        vk->vkDestroyPipeline(vk->device(), kv.second.pipeline, nullptr);
    }
    m_shadowCasterPipelines.clear();
  }
  const bool shadowsEnabled = settings->shadows.enabled;
  const bool outlineEnabled = settings->occludedOutline.enabled;
  reconciliation.receiverSettingsShadowsEnabled = shadowsEnabled ? 1u : 0u;
  reconciliation.receiverSettingsOutlineEnabled = outlineEnabled ? 1u : 0u;
  reconciliation.receiverSettingsRawStrengthMilli =
      strengthToMilli(settings->shadows.strength);
  setReceiverRunEntryFlags(true, shadowsEnabled, outlineEnabled, false, false,
                           false, false, false, false, false, false, false);
  if (!shadowsEnabled && !outlineEnabled) {
    reconciliation.receiverRunEarlyReturnReason =
        static_cast<uint32_t>(
            ReceiverRunEarlyReturnReason::ShadowsAndOutlineDisabled);
    if (War3RenderState::HasOutlineHandles()) {
      static bool s_loggedOutlineDisabled = false;
      if (!s_loggedOutlineDisabled) {
        s_loggedOutlineDisabled = true;
        WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: outline disabled in "
                        "settings (handles=%u)\n",
                        War3RenderState::GetOutlineHandleCount());
      }
      if (!war3dbg::RenderLogEnabled()) {
        static uint32_t s_outlineDisabledLogs = 0;
        if (s_outlineDisabledLogs++ < 3) {
          war3dbg::Print("DXVK_Outline: disabled in settings (handles=%u)\n",
                         War3RenderState::GetOutlineHandleCount());
        }
      }
    }
    publishReconciliationStats();
    return;
  }

  const bool hasListeners = war3shader::internal::HasAnyRenderListeners();
  if (hasListeners) {
    war3shader::internal::DispatchRenderEvent(
        war3shader::RenderEventID::SHADOW_PASS_BEGIN);
  }

  // ========== 日夜循环逻辑 ==========
  const auto now = std::chrono::steady_clock::now();
  if (!m_timeInitialized) {
    m_timeInitialized = true;
    m_timeStart = now;
    m_timeSmoothingInitialized = false;
    m_time01LastUpdate = now;
    m_time01LastRawSample = now;
    m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);

    // 推断 worldUp（保持原有逻辑）
    const Matrix4 invView = inverse(input.scene.worldCamera.view);
    Vector4 camUpWorld = invView * Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    float len =
        std::sqrt(camUpWorld.x * camUpWorld.x + camUpWorld.y * camUpWorld.y +
                  camUpWorld.z * camUpWorld.z);
    if (len > 1e-6f) {
      camUpWorld.x /= len;
      camUpWorld.y /= len;
      camUpWorld.z /= len;
    }

    const Vector4 yUp(0.0f, 1.0f, 0.0f, 0.0f);
    const Vector4 zUp(0.0f, 0.0f, 1.0f, 0.0f);
    float yScore = std::abs(camUpWorld.x * yUp.x + camUpWorld.y * yUp.y +
                            camUpWorld.z * yUp.z);
    float zScore = std::abs(camUpWorld.x * zUp.x + camUpWorld.y * zUp.y +
                            camUpWorld.z * zUp.z);

    m_cachedWorldUp = (yScore >= zScore) ? yUp : zUp;

    WAR3_RENDER_LOG("DXVK War3Shadow: DayNight system started. worldUp=%s\n",
                    (yScore >= zScore) ? "Y" : "Z");
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
  const float realGameTime = War3RenderState::GetGameTime();

  static bool s_hasValidGameTime = false;
  const bool hasRealGameTime = (realGameTime >= 0.0f && realGameTime <= 24.0f);
  if (hasRealGameTime) {
    // 使用真实游戏时间
    if (!s_hasValidGameTime) {
      s_hasValidGameTime = true;
      WAR3_RENDER_LOG("DXVK War3Shadow: Switched to Real Game Time! t=%f\n",
                      realGameTime);
    }
    // Game Time 0=Midnight, 6=Sunrise, 18=Sunset, 24=Midnight
    // Logic expects: 0.0=Midnight, 0.25=Sunrise, 0.5=Noon, 0.75=Sunset
    // Conversion: time01 = time / 24.0
    rawTime01 = wrap01(realGameTime / 24.0f);
  } else {
    // Fallback to simulated time if Jass not ready yet

    // Diagnostic: Why was realGameTime rejected?
    static int s_failLog = 0;
    if (s_failLog++ < 10) {
      WAR3_RENDER_LOG("DXVK War3Shadow: Fallback used. RealGameTime=%.4f "
                      "(Valid Range: 0-24)\n",
                      realGameTime);
    }

    float elapsed = std::chrono::duration<float>(now - m_timeStart).count();
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
  if (hasRealGameTime && !shadowSettings.lockSun) {
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
  float minAltRad = m_minSunAltitudeDeg * (3.14159265f / 180.0f);
  float maxAltRad = m_maxSunAltitudeDeg * (3.14159265f / 180.0f);
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
  float shadowAltRad = calcShadowAltitude01(dayAlt01);

  const float minLightFactor =
      std::clamp(settings->dayNight.transitionMinFactor, 0.0f, 1.0f);

  War3RenderSettings mutableSettings = *settings;

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

  // 球坐标转笛卡尔 (Z-Up)
  // X = cos(Alt) * cos(Yaw)
  // Y = cos(Alt) * sin(Yaw)
  // Z = sin(Alt)
  float cosAlt = std::cos(shadowAltRad);
  float sinAlt = std::sin(shadowAltRad);
  float cosYaw = std::cos(sunYaw);
  float sinYaw = std::sin(sunYaw);

  Vector4 sunPos;
  sunPos.x = cosAlt * cosYaw;
  sunPos.y = cosAlt * sinYaw;
  sunPos.z = sinAlt;

  // LightDir = -SunPos
  mutableSettings.sun.direction =
      Vector4(-sunPos.x, -sunPos.y, -sunPos.z, 0.0f);

  // 6. 光源逻辑 (Sun vs Moon)
  Vector4 finalLightDir;
  Vector4 finalLightColor;
  float finalShadowStrength = 0.0f;

  // 关键视觉参数
  // 地平线颜色：改为【暖灰/米色】，消除过于鲜艳的红色，提供更自然的晨昏过渡
  // 原值(0.5, 0.35, 0.35)太红 -> 新值(0.7, 0.65, 0.6)更亮且低饱和
  const Vector4 kColorHorizon = Vector4(0.7f, 0.65f, 0.60f, 1.0f);
  // 月光颜色：更偏冷蓝以增强夜间氛围
  const Vector4 kColorMoon = Vector4(0.4f, 0.55f, 0.95f, 1.0f);

  // 混合过渡带宽度 (度)：加大到 24 度，让变化更从容
  const float kTransitionDeg = 24.0f;
  const float kTransitionRad = kTransitionDeg * (3.14159265f / 180.0f);

  // 阴影衰减地平线 (Decoupled Fade Horizon)
  const float kFadeHorizonDeg = 10.0f;
  const float kFadeHorizonRad = kFadeHorizonDeg * (3.14159265f / 180.0f);

  // ======== 白天 (Sun) 结果 ========
  Vector4 dayLightDir = mutableSettings.sun.direction;
  float dayAlt = std::max(0.0f, realAltitudeRad);
  Vector4 dayColor;
  if (dayAlt > kTransitionRad) {
    float range = (3.14159265f / 2.0f) - kTransitionRad;
    float tPrime = std::max(0.0f, (dayAlt - kTransitionRad) / range);
    float kelvin = 2500.0f + tPrime * 4000.0f;
    dayColor = kelvinToRgb(kelvin);
  } else {
    float linearBlend =
        (kTransitionRad > 0.0f) ? (dayAlt / kTransitionRad) : 0.0f;
    float blend = linearBlend * linearBlend * (3.0f - 2.0f * linearBlend);
    Vector4 sunLow = kelvinToRgb(2500.0f);
    dayColor = kColorHorizon + (sunLow - kColorHorizon) * blend;
  }
  float dayFadeRatio = (kFadeHorizonRad > 0.0f)
                           ? std::min(1.0f, dayAlt / kFadeHorizonRad)
                           : 0.0f;

  // 【修复】太阳光强度应该与阴影强度绑定
  // 原代码使用 minLightFactor (0.85) 作为最低值，导致太阳落下后依然很亮
  // 修改为：太阳升起时 100%，太阳落下时接近 0%（只保留月光水平）
  const float nightLightLevel = 0.15f; // 夜间光照水平（月光）
  const float dayLightFactor =
      nightLightLevel + (1.0f - nightLightLevel) * dayFadeRatio;

  Vector4 dayLightColor = dayColor * dayLightFactor;
  float dayShadowStrength = dayFadeRatio * 0.6075f;

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
  float moonShadowAlt = calcShadowAltitude01(moonAlt01);
  float moonYaw = sunYaw + 3.14159265f;
  float cAlt = std::cos(moonShadowAlt);
  float sAlt = std::sin(moonShadowAlt);
  float cYaw = std::cos(moonYaw);
  float sYaw = std::sin(moonYaw);
  Vector4 moonPos;
  moonPos.x = cAlt * cYaw;
  moonPos.y = cAlt * sYaw;
  moonPos.z = sAlt;
  Vector4 nightLightDir = Vector4(-moonPos.x, -moonPos.y, -moonPos.z, 0.0f);

  Vector4 nightColor;
  if (moonRealAlt < kTransitionRad) {
    float linearBlend =
        (kTransitionRad > 0.0f) ? (moonRealAlt / kTransitionRad) : 0.0f;
    float blend = linearBlend * linearBlend * (3.0f - 2.0f * linearBlend);
    nightColor = kColorHorizon + (kColorMoon - kColorHorizon) * blend;
  } else {
    nightColor = kColorMoon;
  }
  float moonFadeRatio = (kFadeHorizonRad > 0.0f)
                            ? std::min(1.0f, moonRealAlt / kFadeHorizonRad)
                            : 0.0f;

  // 【修复】月光强度使用夜间基础水平，月亮落下后保持最低亮度
  // 复用上面定义的 nightLightLevel
  const float nightLightFactor =
      nightLightLevel + (1.0f - nightLightLevel) * moonFadeRatio;
  Vector4 nightLightColor = nightColor * nightLightFactor;
  const float nightShadowScale =
      std::clamp(shadowSettings.nightShadowScale, 0.0f, 1.0f);
  float nightShadowStrength = moonFadeRatio * 0.5508f * nightShadowScale;

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
  finalShadowStrength = useDayLight ? dayShadowStrength : nightShadowStrength;

  // [Event System] Update Phase
  bool isRising = std::cos(time01 * (2.0f * 3.14159265f)) >= 0.0f;
  UpdatePhase(realAltitudeRad, isRising, kTransitionRad);

  // Feed the computed day/night light back into the shared sun settings so the
  // fixed-function main light and volumetric pass can see the same cold/warm
  // cycle. Only direction/color are synchronized here; shadow strength stays
  // local so shadow-specific tuning is not hard-overwritten every frame.
  auto *globalSettings = const_cast<War3RenderSettings *>(settings);
  if (globalSettings) {
    globalSettings->sun.direction = finalLightDir;
    globalSettings->sun.color = finalLightColor;
  }

  // Shadow-specific day-night strength remains local to this pass.
  mutableSettings.sun.direction = finalLightDir;
  mutableSettings.sun.color = finalLightColor;
  mutableSettings.shadows.strength = finalShadowStrength;

  // 调试输出 (每 60 帧或满足条件时)
  /*         static int s_logTimer = 0;
          if (s_logTimer++ > 120) {
              s_logTimer = 0;
              WAR3_RENDER_LOG("DXVK War3Shadow: T=%.2f RealAlt=%.1f°
     Clamped=%.1f° Str=%.2f Color=(%.2f,%.2f,%.2f)\n", time01, realAltitudeRad *
     180.0f / 3.14159265f, shadowAltRad * 180.0f / 3.14159265f,
                  mutableSettings.shadows.strength,
                  lightColor.x, lightColor.y, lightColor.z);
          } */

  static uint32_t s_logCount = 0;
  if (s_logCount < 10) {
    s_logCount++;
    WAR3_RENDER_LOG("DXVK War3Shadow: t=%.3f shadowAlt=%.2f str=%.2f "
                    "dir=(%.2f,%.2f,%.2f)\n",
                    static_cast<double>(time01),
                    static_cast<double>(shadowAltRad * 180.0f / 3.14159265f),
                    static_cast<double>(mutableSettings.shadows.strength),
                    static_cast<double>(mutableSettings.sun.direction.x),
                    static_cast<double>(mutableSettings.sun.direction.y),
                    static_cast<double>(mutableSettings.sun.direction.z));
  }

  m_csmConfig = mutableSettings.shadows.csm;
  if (!shadowSettings.lockSun && !shadowSettings.stableSnapWhenSunMoving) {
    m_csmConfig.stableSnap = 0.0f;
  }
  m_pointShadowBias = std::max(0.0f, mutableSettings.shadows.pointShadowBias);

  // 如果阴影强度太低，跳过阴影渲染
  // [Opt P0-4] Hard Gate: Skip entire receiver if no shadows needed
  const bool debugShadow =
      shadowSettings.debugMode != War3ShadowDebugMode::None;
  bool hasSunShadow =
      (mutableSettings.shadows.strength > 0.001f) || debugShadow;
  m_pointLightsEnabled = settings->shadows.pointLightsEnabled;
  m_hasPointLights =
      m_pointLightsEnabled && War3LightManager::Instance().HasActiveLights();
  bool hasPointShadow =
      m_pointLightsEnabled && mutableSettings.shadows.pointShadowEnabled &&
      mutableSettings.shadows.pointShadowMaxLights > 0 && m_hasPointLights;
  m_pointShadowEnabled = hasPointShadow;
  if (!m_pointShadowEnabled) {
    m_pointShadowReady = false;
  }

  const bool needOutlinePass =
      outlineEnabled && War3RenderState::HasOutlineHandles();
  const float activeShadowStrengthForGates =
      shadowsEnabled ? mutableSettings.shadows.strength : 0.0f;
  reconciliation.receiverComputedShadowStrengthMilli =
      strengthToMilli(mutableSettings.shadows.strength);
  reconciliation.receiverActiveStrengthMilli =
      strengthToMilli(activeShadowStrengthForGates);
  reconciliation.receiverDebugMode =
      uint32_t(static_cast<int>(shadowSettings.debugMode));
  reconciliation.receiverHasSunShadow = hasSunShadow ? 1u : 0u;
  reconciliation.receiverHasPointShadow = hasPointShadow ? 1u : 0u;
  reconciliation.receiverNeedOutlinePass = needOutlinePass ? 1u : 0u;
  setReceiverRunEntryFlags(true, shadowsEnabled, outlineEnabled, hasSunShadow,
                           hasPointShadow, m_hasPointLights, needOutlinePass,
                           false, false, false, false, debugShadow);
  if (!hasSunShadow && !hasPointShadow && !m_hasPointLights &&
      !needOutlinePass) {
    reconciliation.receiverRunEarlyReturnReason =
        static_cast<uint32_t>(ReceiverRunEarlyReturnReason::NoWorkNeeded);
    if (hasListeners) {
      war3shader::internal::DispatchRenderEvent(
          war3shader::RenderEventID::SHADOW_PASS_END);
    }
    publishReconciliationStats();
    return;
  }
  {
    static bool s_loggedParams = false;
    if (!s_loggedParams) {
      s_loggedParams = true;
      const auto &vp = input.scene.worldCamera.viewport;
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReceiverPass: params cascades=%u res=%u snap=%.0f "
          "maxDist=%.1f lambda=%.2f sunDir=(%.3f,%.3f,%.3f) vp=%ux%u@(%u,%u) "
          "z=(%.3f,%.3f)\n",
          static_cast<unsigned>(std::min<uint32_t>(
              std::max<uint32_t>(m_csmConfig.cascadeCount, 1u), 4u)),
          static_cast<unsigned>(
              std::max<uint32_t>(m_csmConfig.shadowResolution, 1u)),
          static_cast<double>(m_csmConfig.stableSnap),
          static_cast<double>(m_csmConfig.maxDistance),
          static_cast<double>(m_csmConfig.splitLambda),
          static_cast<double>(mutableSettings.sun.direction.x),
          static_cast<double>(mutableSettings.sun.direction.y),
          static_cast<double>(mutableSettings.sun.direction.z),
          static_cast<unsigned>(vp.Width), static_cast<unsigned>(vp.Height),
          static_cast<unsigned>(vp.X), static_cast<unsigned>(vp.Y),
          static_cast<double>(vp.MinZ), static_cast<double>(vp.MaxZ));
    }
  }

  const auto replayDraws = BuildShadowReplayDraws(input.scene);
  const size_t replayCasterCount = replayDraws.size();
  const uint64_t replayGeometryWork =
      EstimateShadowReplayGeometryWork(replayDraws);
  const uint32_t requestedShadowResolution = m_csmConfig.shadowResolution;
  const uint32_t adaptiveShadowResolution =
      ResolveAdaptiveShadowResolution(requestedShadowResolution,
                                      replayGeometryWork);
  if (adaptiveShadowResolution != requestedShadowResolution) {
    static uint32_t s_adaptiveResolutionLogs = 0;
    if (s_adaptiveResolutionLogs++ < 12u ||
        (s_adaptiveResolutionLogs % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3Shadow: adaptive shadow resolution %u -> %u "
          "(casters=%u geometryWork=%llu)\n",
          static_cast<unsigned>(requestedShadowResolution),
          static_cast<unsigned>(adaptiveShadowResolution),
          static_cast<unsigned>(replayCasterCount),
          static_cast<unsigned long long>(replayGeometryWork));
    }
    m_csmConfig.shadowResolution = adaptiveShadowResolution;
  }
  war3::War3PerfMonitor::instance().noteShadowReceiverFrame(
      static_cast<uint32_t>(std::min<size_t>(
          replayCasterCount, std::numeric_limits<uint32_t>::max())),
      replayGeometryWork, requestedShadowResolution, m_csmConfig.shadowResolution);

  // 计算本帧级联数据（需要外部捕获世界相机矩阵后才会有效）
  War3WorldCameraState effectiveWorldCamera = input.scene.worldCamera;
  War3CsmData newCsm = m_csm.Compute(
      effectiveWorldCamera, mutableSettings.sun.direction, m_csmConfig);
  const bool hasCandidateCsm = newCsm.cascadeCount != 0;
  if (!hasCandidateCsm)
    reconciliation.receiverNoCandidateCsmCount = 1u;
  if (hasCandidateCsm) {
    m_lastGoodReceiverCamera = effectiveWorldCamera;
    m_hasLastGoodReceiverCamera = true;
  }
  const bool csmFallbackToLastGood =
      !hasCandidateCsm && m_csmData.cascadeCount != 0;
  if (csmFallbackToLastGood)
    reconciliation.receiverCsmFallbackToLastGoodCount = 1u;
  if (csmFallbackToLastGood && m_hasLastGoodReceiverCamera)
    effectiveWorldCamera = m_lastGoodReceiverCamera;

  if (!hasCandidateCsm) {
    // 容错：偶发相机矩阵被 overlay/正交污染时，CSM 计算可能失败。
    // 若上一帧已有有效 CSM，则复用以避免“阴影整帧消失→下一帧恢复”的闪烁。
    if (m_csmData.cascadeCount != 0) {
      static bool s_loggedFallback = false;
      if (!s_loggedFallback) {
        s_loggedFallback = true;
        WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: CSM compute failed, "
                        "fallback to last-good CSM/camera\n");
      }
    } else {
      static bool s_warned = false;
      if (!s_warned) {
        s_warned = true;
        const auto &p = input.scene.worldCamera.proj;
        WAR3_RENDER_LOG(
            "DXVK War3ShadowReceiverPass: CSM compute failed, skip (proj "
            "m22=%.6f m23=%.6f m32=%.6f m33=%.6f)\n",
            static_cast<double>(p[2][2]), static_cast<double>(p[2][3]),
            static_cast<double>(p[3][2]), static_cast<double>(p[3][3]));
      }
      if (hasListeners) {
        war3shader::internal::DispatchRenderEvent(
            war3shader::RenderEventID::SHADOW_PASS_END);
      }
      reconciliation.receiverRunEarlyReturnReason =
          static_cast<uint32_t>(ReceiverRunEarlyReturnReason::CsmComputeFailed);
      publishReconciliationStats();
      return;
    }
  }

  const auto debugModeEnum = settings->shadows.debugMode;
  const float activeShadowStrength =
      shadowsEnabled ? mutableSettings.shadows.strength : 0.0f;
  const bool receiverNeedsShadowMap =
      shadowsEnabled && ((debugModeEnum == War3ShadowDebugMode::ShadowFactor) ||
                         (debugModeEnum == War3ShadowDebugMode::None &&
                          activeShadowStrength > 1e-3f));
  const bool needOutlineDepth = settings->occludedOutline.enabled &&
                                settings->occludedOutline.useScreenSpace &&
                                War3RenderState::HasOutlineHandles();
  const bool needReceiverPass =
      (shadowsEnabled && ((debugModeEnum != War3ShadowDebugMode::None) ||
                          (activeShadowStrength > 1e-3f))) ||
      m_hasPointLights;
  reconciliation.receiverNeedShadowMap = receiverNeedsShadowMap ? 1u : 0u;
  reconciliation.receiverNeedPass = needReceiverPass ? 1u : 0u;
  reconciliation.receiverActiveStrengthMilli =
      strengthToMilli(activeShadowStrength);
  reconciliation.receiverDebugMode =
      uint32_t(static_cast<int>(debugModeEnum));
  setReceiverRunEntryFlags(true, shadowsEnabled, outlineEnabled, hasSunShadow,
                           hasPointShadow, m_hasPointLights, needOutlinePass,
                           receiverNeedsShadowMap, needReceiverPass,
                           replayCasterCount != 0u, false, debugShadow);

  // Phase 4：shadow map 优先由 instances + fallbacks 双通道重放生成。
  // shadowCasters 仍保留为兼容容器，但不再作为 directional shadow 的主输入。
  // 即便本帧 caster 为空，也会清屏 shadow map（全亮）以避免复用旧 shadow
  // map 导致拖影。

  // Phase 7.2: 对账计数器
  reconciliation.shadowCastersCount =
      static_cast<uint32_t>(input.scene.shadowCasters.size());
  reconciliation.replayDrawsCount = static_cast<uint32_t>(replayCasterCount);

  const auto& st = input.scene.shadowStats;
  const bool semanticDynamicCastersActive =
      st.semanticSceneSubmittedSkinned != 0u ||
      st.dynamicSkinnedOutputCount != 0u ||
      st.capturedUnitVertexBlend != 0u;
  const bool semanticReceiverStabilityActive =
      SemanticReceiverStabilityModeEnabled() && semanticDynamicCastersActive &&
      debugModeEnum == War3ShadowDebugMode::None;
  const uint64_t semanticIdentityHash =
      st.semanticSceneDirectLastSubmittedIdentityHash;
  const bool semanticIdentityKnown =
      semanticDynamicCastersActive && semanticIdentityHash != 0u;
  if (semanticIdentityKnown) {
    if (semanticIdentityHash == m_pendingShadowMapSemanticIdentityHash) {
      m_pendingShadowMapSemanticIdentityStableFrames =
          std::min<uint32_t>(m_pendingShadowMapSemanticIdentityStableFrames + 1u,
                             0x7fffffffu);
    } else {
      m_pendingShadowMapSemanticIdentityHash = semanticIdentityHash;
      m_pendingShadowMapSemanticIdentityStableFrames = 1u;
    }
  } else {
    m_pendingShadowMapSemanticIdentityHash = 0u;
    m_pendingShadowMapSemanticIdentityStableFrames = 0u;
  }
  const bool semanticIdentityChanged =
      semanticIdentityKnown && m_lastShadowMapSemanticIdentityHash != 0u &&
      semanticIdentityHash != m_lastShadowMapSemanticIdentityHash;
  const uint32_t requiredStableIdentityFrames =
      SemanticReceiverStableIdentityFramesBeforeRedraw();
  const bool semanticIdentityStableForRedraw =
      !semanticIdentityChanged ||
      !SemanticReceiverHoldUntilStableIdentityEnabled() ||
      m_pendingShadowMapSemanticIdentityStableFrames >=
          requiredStableIdentityFrames;
  if (semanticDynamicCastersActive && replayCasterCount != 0u) {
    m_recentSemanticDynamicHoldFramesRemaining =
        dxvk::war3::internal::kShadowSemanticDynamicEmptyReplayHoldFrames;
  } else if (m_recentSemanticDynamicHoldFramesRemaining != 0u) {
    m_recentSemanticDynamicHoldFramesRemaining--;
  }
  const bool canHoldCompleteShadowMap =
      receiverNeedsShadowMap && m_hasCompleteShadowMap && m_shadowMap &&
      m_shadowMapSampleView;
  const bool holdForInvalidCsm =
      dxvk::war3::internal::kShadowHoldLastGoodMapOnInvalidCsm &&
      canHoldCompleteShadowMap && csmFallbackToLastGood;
  const bool holdForTransientEmptyReplay =
      dxvk::war3::internal::kShadowHoldLastGoodMapOnTransientEmptyReplay &&
      canHoldCompleteShadowMap && replayCasterCount == 0u &&
      m_lastShadowMapCasterCount != 0u &&
      m_transientEmptyReplayHoldFramesRemaining != 0u;
  const bool holdForSemanticDynamicEmptyReplay =
      dxvk::war3::internal::kShadowHoldLastGoodMapOnTransientEmptyReplay &&
      canHoldCompleteShadowMap && replayCasterCount == 0u &&
      m_lastShadowMapCasterCount != 0u &&
      m_recentSemanticDynamicHoldFramesRemaining != 0u;
  const bool holdForSemanticIdentityChurn =
      dxvk::war3::internal::kShadowHoldLastGoodMapOnSemanticIdentityChurn &&
      canHoldCompleteShadowMap && replayCasterCount != 0u &&
      semanticIdentityChanged &&
      (!SemanticReceiverHoldUntilStableIdentityEnabled()
           ? m_semanticIdentityChurnHoldFramesRemaining != 0u
           : !semanticIdentityStableForRedraw);
  const bool semanticSelectionUnderCapPressure =
      st.semanticSceneDirectScanCapHitCount != 0u ||
      st.semanticSceneDirectRecordCapHitCount != 0u;
  const uint32_t coverageDropMaxHoldFrames =
      SemanticReceiverCoverageDropMaxHoldFrames();
  const bool semanticCoverageDropCandidate =
      SemanticReceiverHoldOnCoverageDropEnabled() &&
      dxvk::war3::internal::kShadowHoldLastGoodMapOnSemanticIdentityChurn &&
      canHoldCompleteShadowMap && replayCasterCount != 0u &&
      m_lastShadowMapCasterCount != 0u && semanticDynamicCastersActive &&
      semanticSelectionUnderCapPressure &&
      uint32_t(replayCasterCount) + SemanticReceiverCoverageDropTolerance() <
          m_lastShadowMapCasterCount;
  const bool holdForSemanticCoverageDrop =
      semanticCoverageDropCandidate &&
      (coverageDropMaxHoldFrames == 0u ||
       m_semanticCoverageDropHoldStreak < coverageDropMaxHoldFrames);
  if (holdForInvalidCsm)
    reconciliation.receiverHoldInvalidCsmCount = 1u;
  if (holdForTransientEmptyReplay || holdForSemanticDynamicEmptyReplay)
    reconciliation.receiverHoldEmptyReplayCount = 1u;
  if (holdForSemanticIdentityChurn || holdForSemanticCoverageDrop)
    reconciliation.receiverHoldIdentityChurnCount = 1u;

  // 诊断：若 caster
  // 数量在相邻帧间频繁变化，通常意味着“阶段/批次识别不稳定”或“插入点时机漂移”，
  // 这会表现为阴影时有时无/抽搐。默认仅在变化时打点少量日志，避免刷屏。
  /*         {
              static uint32_t s_lastCasterCount = 0;
              static uint32_t s_lastPaletteCount = 0;
              static uint32_t s_loggedChanges = 0;

              const uint32_t casterCount =
     static_cast<uint32_t>(input.scene.shadowCasters.size()); const uint32_t
     paletteCount = static_cast<uint32_t>(input.scene.shadowPalettes.size());

              if ((casterCount != s_lastCasterCount || paletteCount !=
     s_lastPaletteCount) && s_loggedChanges < 24) { s_loggedChanges++; const
     auto& vp = input.scene.worldCamera.viewport; WAR3_RENDER_LOG( "DXVK
     War3ShadowTelemetry: casters %u->%u palettes %u->%u cam=%d
     vp=%ux%u@(%u,%u)\n", static_cast<unsigned>(s_lastCasterCount),
                      static_cast<unsigned>(casterCount),
                      static_cast<unsigned>(s_lastPaletteCount),
                      static_cast<unsigned>(paletteCount),
                      input.scene.worldCamera.valid ? 1 : 0,
                      static_cast<unsigned>(vp.Width),
                      static_cast<unsigned>(vp.Height),
                      static_cast<unsigned>(vp.X),
                      static_cast<unsigned>(vp.Y));
              }

              s_lastCasterCount = casterCount;
              s_lastPaletteCount = paletteCount;
          } */

  bool reuseLastShadowMap = false;
  if (receiverNeedsShadowMap && dxvk::war3::internal::kShadowAdaptiveMapUpdateEnabled &&
      m_hasCompleteShadowMap && hasCandidateCsm &&
      m_csmData.cascadeCount != 0 &&
      replayCasterCount >=
          dxvk::war3::internal::kShadowAdaptiveMapUpdateMinCasters) {
    const uint32_t period = ComputeAdaptiveShadowMapPeriod(replayCasterCount);
    const bool cadenceAllowsReuse =
        period > 1 && ((m_shadowAdaptiveFrameIndex + 1u) % period) != 0u;
    const uint32_t casterDelta =
        static_cast<uint32_t>(std::abs(
            int32_t(replayCasterCount) - int32_t(m_lastShadowMapCasterCount)));
    const float csmDelta = MaxCsmAbsDelta(newCsm, m_csmData);
    const bool dynamicPoseStable =
        input.scene.shadowStats.dynamicPoseSignature ==
        m_lastDynamicPoseSignature;
    const auto& st = input.scene.shadowStats;
    const bool hasDynamicSkinnedCasters =
        st.dynamicSkinnedOutputCount != 0u ||
        st.semanticSceneSubmittedSkinned != 0u ||
        st.capturedUnitVertexBlend != 0u;
    const bool dynamicContentStable =
        !hasDynamicSkinnedCasters ||
        (st.dynamicPoseSignature != 0u && dynamicPoseStable);
    const bool noSemanticOrCaptureInstability =
        st.semanticBridgeMiss == 0 &&
        st.skippedVertexShader == 0 &&
        st.skippedVertexBlend == 0 &&
        st.skippedFreezeBudget == 0 &&
        st.skippedPriorityBudget == 0 &&
        st.skippedMissingPerDrawUpload == 0;

    reuseLastShadowMap =
        cadenceAllowsReuse &&
        casterDelta <= dxvk::war3::internal::kShadowAdaptiveMapUpdateCasterDelta &&
        csmDelta <=
            dxvk::war3::internal::kShadowAdaptiveMapUpdateCameraMaxDelta &&
        noSemanticOrCaptureInstability &&
        dynamicContentStable;
  }

  if (holdForInvalidCsm || holdForTransientEmptyReplay ||
      holdForSemanticDynamicEmptyReplay || holdForSemanticIdentityChurn ||
      holdForSemanticCoverageDrop) {
    reuseLastShadowMap = true;
    if (holdForTransientEmptyReplay &&
        m_transientEmptyReplayHoldFramesRemaining != 0u) {
      m_transientEmptyReplayHoldFramesRemaining--;
    }
    if (holdForSemanticIdentityChurn &&
        !SemanticReceiverHoldUntilStableIdentityEnabled() &&
        m_semanticIdentityChurnHoldFramesRemaining != 0u) {
      m_semanticIdentityChurnHoldFramesRemaining--;
    }
    if (holdForSemanticCoverageDrop)
      m_semanticCoverageDropHoldStreak =
          std::min<uint32_t>(m_semanticCoverageDropHoldStreak + 1u,
                             0x7fffffffu);

    static uint32_t s_loggedHold = 0;
    if (s_loggedHold < 16u || (s_loggedHold % 240u) == 0u) {
      s_loggedHold++;
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReceiverPass: hold last-good shadow map reason=%s "
          "replay=%u lastReplay=%u csmCandidate=%d semanticDynamic=%d "
          "holdFrames=%u semanticHoldFrames=%u identityHoldFrames=%u "
          "identityHash=%llu lastIdentityHash=%llu\n",
          holdForInvalidCsm
              ? "invalid-csm"
              : (holdForSemanticCoverageDrop
                     ? "semantic-coverage-drop"
                     : (holdForSemanticIdentityChurn
                            ? "semantic-identity-churn"
                            : (holdForSemanticDynamicEmptyReplay
                                   ? "semantic-empty-replay"
                                   : "empty-replay"))),
          static_cast<unsigned>(replayCasterCount),
          static_cast<unsigned>(m_lastShadowMapCasterCount),
          hasCandidateCsm ? 1 : 0, semanticDynamicCastersActive ? 1 : 0,
          static_cast<unsigned>(m_transientEmptyReplayHoldFramesRemaining),
          static_cast<unsigned>(
              m_recentSemanticDynamicHoldFramesRemaining),
          static_cast<unsigned>(
              m_semanticIdentityChurnHoldFramesRemaining),
          static_cast<unsigned long long>(semanticIdentityHash),
          static_cast<unsigned long long>(
              m_lastShadowMapSemanticIdentityHash));
    }
  } else if (!semanticCoverageDropCandidate) {
    m_semanticCoverageDropHoldStreak = 0u;
  }

  if (!reuseLastShadowMap && hasCandidateCsm) {
    m_csmData = newCsm;
  }

  if (receiverNeedsShadowMap) {
    // When the frame chooses to reuse the last complete map, keep the old
    // image dimensions alive. Recreating here clears m_hasCompleteShadowMap and
    // turns a stability hold into a forced redraw, which presents as rhythmic
    // whole-scene shadow pulsing under semantic identity churn.
    const bool shadowMapResourcesUsable =
        m_hasCompleteShadowMap && m_shadowMap && m_shadowMapSampleView;
    if (!reuseLastShadowMap || !shadowMapResourcesUsable) {
      ensureShadowResources(m_csmData.cascadeCount, m_csmConfig.shadowResolution);
    }
    if (reuseLastShadowMap &&
        (!m_hasCompleteShadowMap || !m_shadowMap || !m_shadowMapSampleView)) {
      reconciliation.receiverReuseInvalidatedAfterEnsureCount = 1u;
      static uint32_t s_loggedInvalidReuse = 0;
      if (s_loggedInvalidReuse++ < 16u ||
          (s_loggedInvalidReuse % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3ShadowReceiverPass: cancel shadow map reuse after "
            "resource ensure invalidated completeness (replay=%u "
            "layers=%u res=%u)\n",
            static_cast<unsigned>(replayCasterCount),
            static_cast<unsigned>(m_csmData.cascadeCount),
            static_cast<unsigned>(m_csmConfig.shadowResolution));
      }
      reuseLastShadowMap = false;
    }
    if (reuseLastShadowMap) {
      war3::War3PerfMonitor::instance().noteShadowMapFallback(true, false);
      reconciliation.receiverReuseShadowMap = 1u;
    } else {
      auto perfScope = war3::War3PerfMonitor::instance().scope("ShadowMap", ctx);
      const bool renderedShadowMap = renderShadowMap(ctx, input, &replayDraws);
      if (renderedShadowMap) {
        m_semanticCoverageDropHoldStreak = 0u;
        m_hasCompleteShadowMap = true;
        m_lastShadowMapCasterCount = static_cast<uint32_t>(replayCasterCount);
        m_lastDynamicPoseSignature =
            input.scene.shadowStats.dynamicPoseSignature;
        m_lastShadowMapSunDir = mutableSettings.sun.direction;
        m_lastShadowMapStrength = activeShadowStrength;
        m_hasLastShadowMapLighting = true;
        if (semanticIdentityHash != 0u) {
          m_lastShadowMapSemanticIdentityHash = semanticIdentityHash;
          m_semanticIdentityChurnHoldFramesRemaining =
              dxvk::war3::internal::kShadowSemanticIdentityChurnHoldFrames;
        } else if (!semanticDynamicCastersActive) {
          m_lastShadowMapSemanticIdentityHash = 0u;
          m_pendingShadowMapSemanticIdentityHash = 0u;
          m_pendingShadowMapSemanticIdentityStableFrames = 0u;
          m_semanticIdentityChurnHoldFramesRemaining = 0u;
        }
        m_transientEmptyReplayHoldFramesRemaining =
            replayCasterCount != 0u
                ? dxvk::war3::internal::kShadowTransientEmptyReplayHoldFrames
                : 0u;
      } else if (!m_hasCompleteShadowMap) {
        m_lastShadowMapCasterCount = 0u;
        m_lastDynamicPoseSignature = 0u;
        m_lastShadowMapSemanticIdentityHash = 0u;
        m_pendingShadowMapSemanticIdentityHash = 0u;
        m_pendingShadowMapSemanticIdentityStableFrames = 0u;
        m_semanticIdentityChurnHoldFramesRemaining = 0u;
        m_transientEmptyReplayHoldFramesRemaining = 0u;
        m_hasLastShadowMapLighting = false;
      }
    }
  }
  m_shadowAdaptiveFrameIndex++;
  reconciliation.receiverHasCompleteShadowMap =
      (m_hasCompleteShadowMap && m_shadowMap && m_shadowMapSampleView) ? 1u
                                                                       : 0u;
  reconciliation.receiverCsmCascadeCount = m_csmData.cascadeCount;
  recordShadowResourceFingerprint(m_shadowHistoryIndex & 1u,
                                  (m_shadowHistoryIndex & 1u) ^ 1u);
  if (receiverNeedsShadowMap) {
    if (!m_hasCompleteShadowMap)
      reconciliation.receiverNoCompleteShadowMapCount = 1u;
    if (!m_shadowMap)
      reconciliation.receiverNoShadowMapImageCount = 1u;
    if (!m_shadowMapSampleView)
      reconciliation.receiverNoShadowMapSampleViewCount = 1u;
  }

  float receiverShadowStrength = activeShadowStrength;
  Vector4 receiverSunDirSource = mutableSettings.sun.direction;
  if (semanticReceiverStabilityActive &&
      SemanticReceiverFreezeLastGoodLightingEnabled() && reuseLastShadowMap &&
      m_hasLastShadowMapLighting) {
    receiverShadowStrength = m_lastShadowMapStrength;
    receiverSunDirSource = m_lastShadowMapSunDir;
  }
  if (semanticReceiverStabilityActive) {
    const float strengthClamp = SemanticReceiverStableStrengthClamp();
    if (strengthClamp >= 0.0f)
      receiverShadowStrength =
          std::min(receiverShadowStrength, std::clamp(strengthClamp, 0.0f, 1.0f));
  }

  // [NEW] Point Light Cube Shadow（如果启用）
  const bool semanticReceiverPointLightsAllowed =
      !semanticReceiverStabilityActive ||
      !SemanticReceiverDisablePointLightsEnabled();

  if (semanticReceiverPointLightsAllowed && m_pointLightsEnabled &&
      mutableSettings.shadows.pointShadowEnabled &&
      mutableSettings.shadows.pointShadowMaxLights > 0 && m_hasPointLights) {
    renderPointShadow(ctx, input);
  }

  VkExtent3D extent = input.colorView->mipLevelExtent(0u);
  VkExtent3D depthExtent = input.depthView->mipLevelExtent(0u);
  const bool needCopyColor = needReceiverPass;
  const bool needCopyDepth = needReceiverPass || needOutlineDepth;

  if (needCopyColor) {
    ensureCopyResources(extent, input.colorView->image()->info().format);
  }
  if (needCopyDepth) {
    ensureDepthCopyResources(depthExtent,
                             input.depthView->image()->info().format);
  }

  if (needCopyColor || needCopyDepth) {
    auto perfScope = war3::War3PerfMonitor::instance().scope("ShadowCopy", ctx);
    if (needCopyColor) {
      copyColor(ctx, input.colorView);
    }
    if (needCopyDepth) {
      copyDepth(ctx, input.depthView);
    }
  }

  if (replayCasterCount > 0) {
    dxvk::war3::tools::MarkInGameRenderReady("War3Shadow/Replay",
                                             input.frameIndex);
  }

  static uint32_t s_logTimer = 0;
  if ((s_logTimer++ % 300) == 0 && replayCasterCount > 0) {
    const auto &st = input.scene.shadowStats;
    WAR3_RENDER_LOG("DXVK War3Shadow: Run frame=%u casters=%u instances=%u "
                    "fallbacks=%u liveGeom=%u pool=%llu/%llu "
                    "captured=%u/%u/%u/%u unit=%u unitVB=%u "
                    "semScene=%u/%u/%u "
                    "forceFreeze=%u/%u dynPose=%u dynSkin=%u "
                    "rej(noId/mode/dyn/alpha/miss/create)=%u/%u/%u/%u/%u/%u "
                    "skip(cap/vs/vb)=%u/%u/%u cascades=%u res=%u\n",
                    s_logTimer,
                    static_cast<unsigned>(replayCasterCount),
                    static_cast<unsigned>(input.scene.shadowInstances.size()),
                    static_cast<unsigned>(input.scene.shadowFallbacks.size()),
                    static_cast<unsigned>(
                        input.scene.shadowPersistentPool.liveGeometryCount),
                    static_cast<unsigned long long>(
                        input.scene.shadowPersistentPool.bytesUsed),
                    static_cast<unsigned long long>(
                        input.scene.shadowPersistentPool.bytesCap),
                    static_cast<unsigned>(st.capturedIndexed),
                    static_cast<unsigned>(st.capturedNonIndexed),
                    static_cast<unsigned>(st.capturedTerrain),
                    static_cast<unsigned>(st.capturedWorldObject),
                    static_cast<unsigned>(st.capturedUnitObject),
                    static_cast<unsigned>(st.capturedUnitVertexBlend),
                    static_cast<unsigned>(st.semanticSceneSubmitted),
                    static_cast<unsigned>(st.semanticSceneSubmittedUnit),
                    static_cast<unsigned>(st.semanticSceneSubmittedSkinned),
                    static_cast<unsigned>(st.forcedFallbackWorldFreezeCount),
                    static_cast<unsigned>(st.forcedFallbackUnitFreezeCount),
                    static_cast<unsigned>(st.dynamicPoseCount),
                    static_cast<unsigned>(st.dynamicSkinnedOutputCount),
                    static_cast<unsigned>(st.persistentRejectNoIdentity),
                    static_cast<unsigned>(st.persistentRejectUnsupportedMode),
                    static_cast<unsigned>(st.persistentRejectDynamicSource),
                    static_cast<unsigned>(st.persistentRejectAlphaBlend),
                    static_cast<unsigned>(st.persistentRejectMissingStorage),
                    static_cast<unsigned>(st.persistentRejectCreateOrBudget),
                    static_cast<unsigned>(st.skippedCasterCap),
                    static_cast<unsigned>(st.skippedVertexShader),
                    static_cast<unsigned>(st.skippedVertexBlend),
                    static_cast<unsigned>(m_csmData.cascadeCount),
                    static_cast<unsigned>(m_shadowMapResolution));

    // Log Light Matrices
    for (uint32_t i = 0; i < std::min(m_csmData.cascadeCount, 4u); i++) {
      auto &m = m_csmData.cascades[i].lightViewProj;
      WAR3_RENDER_LOG("  Cascade[%u] Proj[3]: %.2f %.2f %.2f %.2f Split=%.2f\n",
                      i, m[3].x, m[3].y, m[3].z, m[3].w,
                      m_csmData.cascades[i].splitFar);
    }

    const War3ShadowSettings &ss = settings->shadows;
    WAR3_RENDER_LOG(
        "  Budget: fallback=%llu/%llu arena=%llu skipFreeze=%u skipPriority=%u "
        "skipCap=%u dropAlpha=%u\n",
        static_cast<unsigned long long>(st.fallbackBudgetUsedBytes),
        static_cast<unsigned long long>(st.fallbackBudgetBytes),
        static_cast<unsigned long long>(st.fallbackArenaBytes),
        static_cast<unsigned>(st.skippedFreezeBudget),
        static_cast<unsigned>(st.skippedPriorityBudget),
        static_cast<unsigned>(st.skippedCasterCap),
        static_cast<unsigned>(st.degradedAlphaBudget));
    WAR3_RENDER_LOG("  Settings: Enabled=%d Strength=%.2f Bias=%.4f PCF=%.4f\n",
                    ss.enabled, activeShadowStrength, ss.receiverBias,
                    ss.pcfRadius);
  }

  if (needReceiverPass || needOutlineDepth) {
    static const bool s_disableTaaForSemanticDynamic = []() {
      const char* env = std::getenv(
          "DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC");
      return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();
    const bool shadowTaaBlockedForSemanticDynamic =
        s_disableTaaForSemanticDynamic &&
        semanticDynamicCastersActive;
    const bool shadowTaaActive =
        settings->shadows.shadowTaaEnabled && receiverNeedsShadowMap &&
        !shadowTaaBlockedForSemanticDynamic && !csmFallbackToLastGood;
    reconciliation.shadowTaaActive = shadowTaaActive ? 1u : 0u;
    if (shadowTaaBlockedForSemanticDynamic)
      m_shadowHistoryValid = false;
    const bool debugMotionVector =
        (debugModeEnum == War3ShadowDebugMode::MotionVector);
    const bool debugShadowHistory =
        (debugModeEnum == War3ShadowDebugMode::ShadowHistory);

    const bool needMotionVectors = shadowTaaActive || debugMotionVector;
    const bool needShadowVisibility = shadowTaaActive;

    // 先确保资源存在：避免 Resize/重建导致 ubo 中的 hasHistory/prev 状态不同步
    if (needMotionVectors) {
      ensureMotionVectorResources(extent);
    }
    if (shadowTaaActive || debugShadowHistory) {
      ensureShadowTaaResources(extent);
    }
    const bool shadowHistoryValidBefore = m_shadowHistoryValid;
    const uint32_t historyReadIndex = m_shadowHistoryIndex & 1u;
    const uint32_t historyWriteIndex = historyReadIndex ^ 1u;
    reconciliation.shadowHistoryValidBefore =
        shadowHistoryValidBefore ? 1u : 0u;
    reconciliation.shadowHistoryReadIndex = historyReadIndex;
    reconciliation.shadowHistoryWriteIndex = historyWriteIndex;
    recordShadowResourceFingerprint(historyReadIndex, historyWriteIndex);

    // Update receiver uniforms (device-local UBO via cmdUpdateBuffer)
    ShadowReceiverUniform ubo = {};
    ubo.view = effectiveWorldCamera.view;
    ubo.invViewProj = effectiveWorldCamera.invViewProj;

    for (uint32_t i = 0; i < 4; i++) {
      ubo.lightViewProj[i] =
          (i < m_csmData.cascadeCount)
              ? m_csmData.cascades[i].lightViewProj
              : m_csmData.cascades[m_csmData.cascadeCount - 1].lightViewProj;
    }

    float split0 = m_csmData.cascades[0].splitFar;
    float split1 =
        (m_csmData.cascadeCount > 1) ? m_csmData.cascades[1].splitFar : split0;
    float split2 =
        (m_csmData.cascadeCount > 2) ? m_csmData.cascades[2].splitFar : split1;
    float split3 =
        (m_csmData.cascadeCount > 3) ? m_csmData.cascades[3].splitFar : split2;
    ubo.splitFar = Vector4(split0, split1, split2, split3);

    const bool receiverHasUsableDirectionalShadow =
        receiverNeedsShadowMap && m_hasCompleteShadowMap && m_shadowMap &&
        m_shadowMapSampleView;
    const float uboShadowStrength = receiverShadowStrength;
    reconciliation.receiverHasUsableDirectionalShadow =
        receiverHasUsableDirectionalShadow ? 1u : 0u;
    reconciliation.receiverUboStrengthMilli =
        strengthToMilli(uboShadowStrength);
    if (needReceiverPass && receiverNeedsShadowMap &&
        uboShadowStrength <= 1e-4f) {
      reconciliation.receiverZeroStrengthFrameCount = 1u;
      reconciliation.receiverDrawnWithZeroStrengthCount = 1u;
    }
    float pcfRadius = settings->shadows.pcfRadius;
    if (semanticReceiverStabilityActive) {
      const float pcfOverride = SemanticReceiverStablePcfRadiusOverride();
      if (pcfOverride >= 0.0f)
        pcfRadius = pcfOverride;
    }
    const float invShadowRes =
        1.0f / float(std::max<uint32_t>(m_shadowMapResolution, 1u));
    ubo.params = Vector4(uboShadowStrength, pcfRadius, invShadowRes,
                         float(m_csmData.cascadeCount));

    const float receiverBias = settings->shadows.receiverBias;
    const float cascadeBlendRange = settings->shadows.cascadeBlendRange;
    const float debugModeF =
        float(static_cast<int>(settings->shadows.debugMode));
    const float pointLightsEnabled =
        (semanticReceiverPointLightsAllowed && m_pointLightsEnabled &&
         m_hasPointLights)
            ? 1.0f
            : 0.0f;
    ubo.params2 = Vector4(receiverBias, cascadeBlendRange, debugModeF,
                          pointLightsEnabled);

    const auto &vp = effectiveWorldCamera.viewport;
    const float invViewportW = 1.0f / float(std::max<DWORD>(vp.Width, 1u));
    const float invViewportH = 1.0f / float(std::max<DWORD>(vp.Height, 1u));
    const float pcssEnable =
        (!semanticReceiverStabilityActive && settings->shadows.pcssEnabled)
            ? 1.0f
            : 0.0f;
    const float pcssSearchRadius =
        std::max(settings->shadows.pcssSearchRadius, 0.0f);
    ubo.params3 =
        Vector4(invViewportW, invViewportH, pcssEnable, pcssSearchRadius);

    const float pcssMinRadius = std::max(settings->shadows.pcssMinRadius, 0.0f);
    const float pcssMaxRadius =
        std::max(settings->shadows.pcssMaxRadius, pcssMinRadius);
    const float pcssDepthScale =
        std::max(settings->shadows.pcssDepthScale, 0.0f);
    const float cascadeBiasScale =
        std::clamp(settings->shadows.cascadeBiasScale, 0.0f, 1.0f);
    ubo.params4 =
        Vector4(pcssMinRadius, pcssMaxRadius, pcssDepthScale, cascadeBiasScale);

    Vector4 sunDir = receiverSunDirSource;
    const float sunLen2 =
        sunDir.x * sunDir.x + sunDir.y * sunDir.y + sunDir.z * sunDir.z;
    if (sunLen2 > 1e-6f) {
      const float invLen = 1.0f / std::sqrt(sunLen2);
      sunDir.x *= invLen;
      sunDir.y *= invLen;
      sunDir.z *= invLen;
    }
    ubo.sunDir = sunDir;

    const float normalBiasScale =
        semanticReceiverStabilityActive
            ? 0.0f
            : std::max(settings->shadows.normalBiasScale, 0.0f);
    const float receiverMode =
        semanticReceiverStabilityActive
            ? float(static_cast<uint32_t>(War3ShadowReceiverMode::Legacy))
            : float(static_cast<uint32_t>(settings->shadows.receiverMode));
    const float rimIntensity =
        semanticReceiverStabilityActive
            ? 0.0f
            : std::max(settings->shadows.rimIntensity, 0.0f);
    const float rimPower = std::max(settings->shadows.rimPower, 0.1f);
    ubo.params5 =
        Vector4(normalBiasScale, rimIntensity, rimPower, receiverMode);
    const float pcfKernel =
        float(static_cast<uint32_t>(settings->shadows.pcfKernel));
    const float pcfRotateMode =
        (!semanticReceiverStabilityActive && settings->shadows.pcfRotate)
            ? float(static_cast<uint32_t>(settings->shadows.pcfRotateMode))
            : 0.0f;
    const float pcssSearchKernel =
        float(static_cast<uint32_t>(settings->shadows.pcssSearchKernel));
    const float pcfCascadeRadiusScale =
        std::clamp(settings->shadows.pcfCascadeRadiusScale, 0.0f, 1.0f);
    ubo.params6 = Vector4(pcfKernel, pcfRotateMode, pcssSearchKernel,
                          pcfCascadeRadiusScale);

    if (semanticReceiverStabilityActive) {
      static uint32_t s_loggedSemanticReceiverStability = 0;
      if (s_loggedSemanticReceiverStability++ < 8u ||
          (s_loggedSemanticReceiverStability % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3ShadowReceiverPass: semantic receiver stability active "
            "reuse=%d strength=%.3f->%.3f normalBias=0 pcfRotate=0 pcss=0 "
            "lastLighting=%d\n",
            reuseLastShadowMap ? 1 : 0,
            static_cast<double>(activeShadowStrength),
            static_cast<double>(uboShadowStrength),
            m_hasLastShadowMapLighting ? 1 : 0);
      }
    }

    ubo.viewport =
        Vector4(float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height));
    ubo.viewportZ = Vector4(vp.MinZ, vp.MaxZ, 0.0f, 0.0f);

    // Shadow TAA / Motion Vector：上一帧 ViewProj（无上一帧时用当前帧填充，保证
    // mv=0）
    const Matrix4 currentViewProj = effectiveWorldCamera.viewProj;
    ubo.prevViewProj = m_hasPrevFrameData ? m_prevViewProj : currentViewProj;

    // Shadow TAA 参数：
    // x：0=关闭，1=启用但无可用历史，2=启用且可用历史（用于 shader
    // 内区分是否参与混合） y：blendFactor（新帧权重） z：neighborClamp
    // w：hasHistory（用于 debug 可视化历史是否存在）
    const float taaMode =
        shadowTaaActive ? ((m_shadowHistoryValid &&
                            m_shadowTaaWasActiveLastFrame && m_hasPrevFrameData)
                               ? 2.0f
                               : 1.0f)
                        : 0.0f;
    const float taaBlend =
        std::clamp(settings->shadows.shadowTaaBlendFactor, 0.0f, 1.0f);
    const float taaClamp =
        settings->shadows.shadowTaaNeighborClamp ? 1.0f : 0.0f;
    const float taaHasHistory = m_shadowHistoryValid ? 1.0f : 0.0f;
    ubo.taaParams = Vector4(taaMode, taaBlend, taaClamp, taaHasHistory);
    reconciliation.shadowTaaMode = uint32_t(taaMode + 0.5f);
    reconciliation.shadowReceiverSampleSource =
        receiverHasUsableDirectionalShadow
            ? (shadowTaaActive
                   ? (reconciliation.shadowTaaMode >= 2u ? 3u : 2u)
                   : 1u)
            : 0u;

    auto uboInfo =
        m_shadowUniformBuffer->getSliceInfo(0u, sizeof(ShadowReceiverUniform));
    ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, uboInfo.buffer,
                         uboInfo.offset, sizeof(ShadowReceiverUniform), &ubo);

    VkBufferMemoryBarrier2 bufBarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bufBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bufBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bufBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    bufBarrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    bufBarrier.buffer = uboInfo.buffer;
    bufBarrier.offset = uboInfo.offset;
    bufBarrier.size = sizeof(ShadowReceiverUniform);

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.bufferMemoryBarrierCount = 1;
    depInfo.pBufferMemoryBarriers = &bufBarrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

    ctx->track(m_shadowUniformBuffer, DxvkAccess::Write);

    // 先生成 Motion Vector / 当前帧阴影可见性（供 receiver 采样）
    if (needMotionVectors) {
      auto perfScope =
          war3::War3PerfMonitor::instance().scope("Shadow/MotionVector", ctx);
      renderMotionVectors(ctx, input);
    }

    if (needShadowVisibility) {
      auto perfScope =
          war3::War3PerfMonitor::instance().scope("Shadow/Visibility", ctx);
      renderShadowVisibility(ctx, input);
    }

    // History 读写同步：
    // - 读：上一帧 storage 写入 -> 本帧 fragment 采样
    // - 写：上一帧 fragment 采样/写入 -> 本帧 fragment storage 写入
    if ((shadowTaaActive || debugShadowHistory) && m_shadowHistory[0] &&
        m_shadowHistory[1]) {
      std::array<VkImageMemoryBarrier2, 2> imgBarriers = {};
      uint32_t barrierCount = 0;

      if (m_shadowHistoryValid) {
        VkImageMemoryBarrier2 &b = imgBarriers[barrierCount++];
        b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.srcAccessMask =
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = m_shadowHistory[historyReadIndex]->handle();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      }

      if (shadowTaaActive) {
        VkImageMemoryBarrier2 &b = imgBarriers[barrierCount++];
        b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.srcAccessMask =
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = m_shadowHistory[historyWriteIndex]->handle();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      }

      if (barrierCount > 0) {
        VkDependencyInfo depInfo2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo2.imageMemoryBarrierCount = barrierCount;
        depInfo2.pImageMemoryBarriers = imgBarriers.data();
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo2);
      }
    }

    if (needReceiverPass) {
      auto perfScope =
          war3::War3PerfMonitor::instance().scope("ShadowReceiver", ctx);
      drawReceiver(ctx, input.colorView);
    }

    // Receiver 内部会对 HistoryWrite 执行 imageStore（仅在 taaMode>0 时触发）。
    // 这里做一次 write->read 可见性同步，并推进 ping-pong 索引。
    if (shadowTaaActive && m_shadowHistory[historyWriteIndex]) {
      VkImageMemoryBarrier2 barrier = {
          VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.image = m_shadowHistory[historyWriteIndex]->handle();
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      VkDependencyInfo depInfo2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      depInfo2.imageMemoryBarrierCount = 1;
      depInfo2.pImageMemoryBarriers = &barrier;
      ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo2);

      m_shadowHistoryIndex = historyWriteIndex;
      m_shadowHistoryValid = true;
    }
    reconciliation.shadowHistoryValidAfter =
        m_shadowHistoryValid ? 1u : 0u;

    // 记录本帧是否执行过 TAA（用于下一帧决定是否允许混合历史）
    m_shadowTaaWasActiveLastFrame = shadowTaaActive;
  } else {
    // 本帧未执行 receiver/ubo 更新，视为 TAA 断档
    m_shadowTaaWasActiveLastFrame = false;
  }

  // 单位被遮挡描边
  // 此时场景已完全渲染，深度缓冲完整，可以正确判断遮挡
  if (settings->occludedOutline.enabled) {
    auto perfScope = war3::War3PerfMonitor::instance().scope("Outline", ctx);
    renderUnitOutline(ctx, input);
  }

  if (hasListeners) {
    war3shader::internal::DispatchRenderEvent(
        war3shader::RenderEventID::SHADOW_PASS_END);
  }

  setReceiverRunEntryFlags(true, shadowsEnabled, outlineEnabled, hasSunShadow,
                           hasPointShadow, m_hasPointLights, needOutlinePass,
                           receiverNeedsShadowMap, needReceiverPass,
                           replayCasterCount != 0u,
                           reconciliation.shadowMapExecutedThisFrame != 0u,
                           debugShadow);
  publishReconciliationStats();

  // 保存上一帧相机矩阵（用于 Motion Vector / ShadowTAA 重投影）
  m_prevViewMatrix = effectiveWorldCamera.view;
  m_prevProjMatrix = effectiveWorldCamera.proj;
  m_prevViewProj = effectiveWorldCamera.viewProj;
  m_hasPrevFrameData = true;
}

// ----------------------------------------------------------------------------
// War3ShadowDataPool Implementation

/*
    void War3ShadowDataPool::init(DxvkDevice* device) {
        if (m_device != nullptr)
            return; // Already inited

        m_device = device;
        m_frameIndex = 0;
        m_cursor = 0;

        DxvkBufferCreateInfo info = { };
        info.size = kBufferSize;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
VK_BUFFER_USAGE_INDEX_BUFFER_BIT; info.stages =
VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        info.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
VK_ACCESS_INDEX_READ_BIT; info.debugName = "War3ShadowStagingPool";

        // Host visible and coherent heavily reduces mapping overhead
        VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (size_t i = 0; i < kRingSize; i++) {
            m_buffers[i] = m_device->createBuffer(info, flags);
            if (m_buffers[i] != nullptr)
                m_mapPtrs[i] = m_buffers[i]->mapPtr(0);
        }
    }

    void War3ShadowDataPool::newFrame() {
        if (m_device == nullptr) return;

        m_frameIndex = (m_frameIndex + 1) % kRingSize;
        m_cursor = 0; // Reset cursor for new frame
    }

    DxvkResourceBufferInfo War3ShadowDataPool::upload(const void* data,
VkDeviceSize size, VkBufferUsageFlags usage) { DxvkResourceBufferInfo result = {
};

        if (m_device == nullptr || data == nullptr || size == 0)
            return result;

        size_t align = 256; // Standard alignment
        VkDeviceSize alignedCursor = (m_cursor + align - 1) & ~(align - 1);

        if (alignedCursor + size > kBufferSize) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                WAR3_RENDER_LOG("DXVK War3ShadowDataPool: EXHAUSTED (size >
8MB)\n");
            }
            return result;
        }

        void* dst = static_cast<uint8_t*>(m_mapPtrs[m_frameIndex]) +
alignedCursor; std::memcpy(dst, data, size);

        // Get info directly from buffer slice
        DxvkResourceBufferInfo baseInfo =
m_buffers[m_frameIndex]->getSliceInfo(0, kBufferSize); result.buffer =
baseInfo.buffer; result.offset = baseInfo.offset + alignedCursor; result.size =
size;

        m_cursor = alignedCursor + size;
        return result;
    }

    Rc<DxvkBuffer> War3ShadowDataPool::getCurrentBuffer() {
        if (m_device == nullptr) return nullptr;
        return m_buffers[m_frameIndex];
    }

} // namespace dxvk */

// Tanner Helland's algorithm (simplified)
Vector4 War3ShadowReceiverPass::kelvinToRgb(float k) {
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

void War3ShadowReceiverPass::UpdatePhase(float altitudeRad, bool isRising,
                                         float transRad) {
  War3DayNightPhase nextPhase = m_currentPhase;
  float progress = 0.0f;

  if (altitudeRad > transRad) {
    nextPhase = War3DayNightPhase::Day;
    // Progress: 0 at Transition, 1 at Zenith (PI/2)
    float range = (3.14159265f / 2.0f) - transRad;
    progress = (std::max)(0.0f, (altitudeRad - transRad) / range);
  } else if (altitudeRad < -transRad) {
    nextPhase = War3DayNightPhase::Night;
    // Progress: 0 at Transition, 1 at Nadir (-PI/2)
    float range = (3.14159265f / 2.0f) - transRad;
    progress = (std::max)(0.0f, (-altitudeRad - transRad) / range);
  } else {
    // Transition Zone
    if (isRising) {
      nextPhase = War3DayNightPhase::Dawn;
      // Progress: 0 at -Trans, 1 at +Trans
      progress = (altitudeRad + transRad) / (2.0f * transRad);
    } else {
      nextPhase = War3DayNightPhase::Dusk;
      // Progress: 0 at +Trans, 1 at -Trans
      progress = (transRad - altitudeRad) / (2.0f * transRad);
    }
  }

  // State change check
  if (nextPhase != m_currentPhase) {
    // Can fire "OnEnter" event here if needed
    m_currentPhase = nextPhase;
  }

  // Fire continuous event
  if (m_eventCb) {
    m_eventCb(m_currentPhase, (std::min)(1.0f, (std::max)(0.0f, progress)));
  }
}

} // namespace dxvk
