#include "d3d9_device.h"
#include "d3d9_war3_debug.h"
#include "d3d9_war3_hook.h"
#include "d3d9_war3_pathtrace.h"
#include "d3d9_war3_pipeline.h"
#include "war3/core/war3_internal_test_config.h"
#include "war3/core/war3_game_structs.h"
#include "war3/core/war3_memory.h"
#include "war3/core/war3_net_event_hook.h"
#include "war3/core/war3_runtime_profile.h"
#include "war3/core/war3_semantic_shadow_gate.h"
#include "war3/reimpl/war3_render_types.h"
#include "war3/render/war3_native_renderer_probe.h"
#include "war3/render/war3_post_process.h"
#include "war3/render/war3_render_exec_batch.h"
#include "war3/render/war3_render_objects.h"
#include "war3/render/war3_renderer.h"
#include "war3/render/war3_shadow_capture_frontend.h"
#include "war3/render/war3_shadow_object_registry.h"
#include "war3/render/war3_shadow_runtime_bridge.h"
#include "war3/render/war3_upper_layer_shadow.h"
#include "war3/render/war3_visible_renderables.h"
#include "war3/memory/war3_shadow_arena.h"
#include "war3/memory/war3_storm_hook.h"
#include "war3/model/war3_model_hook.h"
#include "war3/model/war3_model_resource_cache.h"
#include "war3/model/war3_model_registry.h"
#include "war3/native/war3_native_shadow_hint.h"
#include "war3/platform/war3_runtime_bootstrap.h"
#include "war3/shadow/war3_shadow_renderer_core.h"
#include "war3/shader/war3_shader_manager.h"
#include "war3/state/war3_render_state.h"
#include "war3/tools/war3_diagnostics_hub.h"
#include "war3/tools/war3_perf_monitor.h"
#include "war3_shader_api.h"
#include "war3_shaderpack_internal.h"

#include "d3d9_annotation.h"
#include "d3d9_buffer.h"
#include "d3d9_caps.h"
#include "d3d9_common_texture.h"
#include "d3d9_format_helpers.h"
#include "d3d9_interface.h"
#include "d3d9_monitor.h"
#include "d3d9_names.h"
#include "d3d9_query.h"
#include "d3d9_shader.h"
#include "d3d9_spec_constants.h"
#include "d3d9_stateblock.h"
#include "d3d9_swapchain.h"
#include "d3d9_texture.h"
#include "d3d9_util.h"
#include "d3d9_vertex_declaration.h"
#include "d3d9_volume.h" // Added by user instruction
#include "d3d9_war3_hook.h"
#include "imgui_impl_dx9.h" // Added by user instruction
#include "war3/reimpl/war3_instance_buffer.h"
#include "war3/reimpl/war3_shader_patcher.h"
#include "war3/ui/war3_imgui.h"
#include "war3/war3.h"

#include "../dxvk/dxvk_adapter.h"

#include <chrono>
#include "../dxvk/dxvk_instance.h"

#include "../util/util_bit.h"
#include "../util/util_math.h"

#include "d3d9_initializer.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>
#include <windows.h>

#ifdef MSC_VER
#pragma fenv_access(on)
#endif

#include "war3/reimpl/war3_render_queue.h"

namespace dxvk {

namespace {
void War3ForceImmediatePresent(D3DPRESENT_PARAMETERS *params) {
  if (!params)
    return;
  if (war3::War3Imgui::isFpsUnlocked())
    params->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

uint32_t War3GetEnvU32(const char *name, uint32_t fallback) {
  const std::string v = env::getEnvVar(name);
  if (v.empty())
    return fallback;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(v.c_str(), &end, 0);
  if (end == v.c_str())
    return fallback;
  return static_cast<uint32_t>(parsed);
}

uint64_t War3GetShadowPersistentPoolCapBytes() {
  static const uint64_t s_capBytes =
      uint64_t(War3GetEnvU32("DXVK_WAR3_SHADOW_PERSISTENT_MB", 512u)) *
      1024ull * 1024ull;
  return s_capBytes;
}

uint32_t War3GetShadowPersistentMaxAgeFrames() {
  static const uint32_t s_age =
      (std::max)(30u, War3GetEnvU32("DXVK_WAR3_SHADOW_PERSISTENT_MAX_AGE",
                                    240u));
  return s_age;
}

bool War3SemanticRequireVisibleIndexSliceForSkinnedRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_REQUIRE_VISIBLE_INDEX_SLICE", 1u) != 0u;
  return s_enabled;
}

bool War3SemanticAllowCanonicalSinglePrimitiveFullIndexRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_ALLOW_SINGLE_PRIM_FULL_INDEX", 0u) != 0u;
  return s_enabled;
}

bool War3SemanticValidateUnitCoreRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_VALIDATE_UNIT_CORE", 1u) != 0u;
  return s_enabled;
}

bool War3ShadowPassTraceEnabled() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SHADOW_PASS_TRACE", 0u) != 0u;
  return s_enabled;
}

bool War3SemanticPublishRegistriesBeforeSceneRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE",
                    0u) != 0u;
  return s_enabled;
}

uint64_t War3SemanticContractCapturePeriodRuntime() {
  static const uint64_t s_period =
      std::max<uint32_t>(
          1u, War3GetEnvU32("DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD",
                            240u));
  return s_period;
}

bool War3SemanticSubmitBreakdownRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_SUBMIT_BREAKDOWN", 0u) != 0u;
  return s_enabled;
}

bool War3SemanticLivePaletteRefreshRuntime() {
  // Diagnostic only. 2026-05-01 motion probes showed submit-time live CModel
  // refresh can keep the visible caster on a stable bind/initial pose. The
  // production path rebuilds ShadowRendererCore packets from the pose-only
  // contract instead.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH", 0u) != 0u;
  return s_enabled;
}

bool War3SemanticLivePaletteAllowCModelFallbackRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_ALLOW_CMODEL_FALLBACK",
                    0u) != 0u;
  return s_enabled;
}

bool War3SemanticDrawTimePoseRuntime() {
  // Diagnostic only. 2026-05-01 testing showed War3's fixed-function draw path
  // reaches this hook with vertex blend disabled, so it cannot publish the
  // animated skeletal palette. The production dirty signal comes from the
  // final CModel matrix publisher hooks instead.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE", 0u) != 0u;
  return s_enabled;
}

uint32_t War3SemanticSubmitDrawCapRuntime() {
  static const uint32_t s_cap = War3GetEnvU32(
      "DXVK_WAR3_SEMANTIC_SUBMIT_DRAW_CAP",
      dxvk::war3::internal::kShadowSemanticCoreSceneSubmitDrawCap);
  return s_cap;
}

war3::War3PerfMonitor::ScopedCpuScope War3SemanticSubmitScope(
    const char* name) {
  if (!War3SemanticSubmitBreakdownRuntime())
    return {};
  return war3::War3PerfMonitor::instance().cpuScope(name);
}

VkDeviceSize War3AlignPersistentBytes(VkDeviceSize size) {
  constexpr VkDeviceSize kAlign = 256u;
  return (size + (kAlign - 1u)) & ~(kAlign - 1u);
}

uint32_t War3NormalizeShadowHandle(uint32_t handle) {
  if (handle == 0u)
    return 0u;
  const uint32_t handleId = handle & 0x0FFFFFu;
  return handleId != 0u ? (0x100000u | handleId) : 0u;
}

void War3RecomputeFallbackBreakdown(War3FrameScene& scene) {
  scene.shadowStats.fallbackDrawCount =
      static_cast<uint32_t>(scene.shadowFallbacks.size());
  scene.shadowStats.fallbackDrawCountTerrain = 0u;
  scene.shadowStats.fallbackDrawCountWorldObject = 0u;
  scene.shadowStats.fallbackDrawCountUnitObject = 0u;
  for (const auto& fallback : scene.shadowFallbacks) {
    if (fallback.snapshot.category == War3RenderState::StageCategory::Terrain)
      scene.shadowStats.fallbackDrawCountTerrain++;
    if (fallback.snapshot.category == War3RenderState::StageCategory::WorldObject ||
        fallback.snapshot.category == War3RenderState::StageCategory::Effect) {
      scene.shadowStats.fallbackDrawCountWorldObject++;
    }
    if (fallback.snapshot.objectKind ==
        static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Unit)) {
      scene.shadowStats.fallbackDrawCountUnitObject++;
    }
  }
}

uint64_t War3GetShadowFallbackBudgetCapBytes() {
  const uint32_t fallbackMb = War3GetEnvU32(
      "DXVK_WAR3_SHADOW_FALLBACK_BUDGET_MB",
      dxvk::war3::render::IsShadowArenaCaptureEnabled() ? 1024u : 256u);
  return uint64_t(fallbackMb) * 1024ull * 1024ull;
}

bool War3IsSemanticUnitObject(
    dxvk::war3::render::ObjectKind objectKind) {
  return objectKind == dxvk::war3::render::ObjectKind::Unit;
}

uint32_t War3SemanticUnitFlags5C(
    const dxvk::war3::shadow::ShadowRenderableRecord& renderable) {
  if (renderable.unitFlags5C != 0u)
    return renderable.unitFlags5C;

  uint32_t flags5C = 0u;
  if (renderable.unitPtr != nullptr &&
      dxvk::war3::SafeReadU32Fast(renderable.unitPtr,
                                  dxvk::war3::CUnitOffsets::Flags5C,
                                  flags5C)) {
    return flags5C;
  }

  return 0u;
}

bool War3SemanticRenderableHasBuildingFlags(
    const dxvk::war3::shadow::ShadowRenderableRecord& renderable) {
  const uint32_t flags5C = War3SemanticUnitFlags5C(renderable);
  return (flags5C & dxvk::war3::UnitFlags5C::Building) != 0u;
}

constexpr uint32_t War3SemanticByteSwapU32(uint32_t v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

bool War3SemanticFourCcHasPrefix(uint32_t rawcode, char c0, char c1) {
  const auto matches = [=](uint32_t value) {
    return ((value >> 24) & 0xFFu) == static_cast<uint8_t>(c0) &&
           ((value >> 16) & 0xFFu) == static_cast<uint8_t>(c1);
  };
  return matches(rawcode) || matches(War3SemanticByteSwapU32(rawcode));
}

bool War3SemanticFourCcEqualEitherOrder(uint32_t a, uint32_t b) {
  if (a == b)
    return true;
  return a != 0u && b != 0u && War3SemanticByteSwapU32(a) == b;
}

bool War3SemanticRawcodeLooksStaticWorldCaster(uint32_t rawcode) {
  if (rawcode == 0u)
    return false;

  // Trees/pathing doodads such as LTbr/YTxx can share CWidget-like offsets
  // with CUnit and were observed entering the skinned unit path as obj=Unit.
  // Keep this as a surgical reject list instead of broad rawcode class guesses.
  return War3SemanticFourCcHasPrefix(rawcode, 'L', 'T') ||
         War3SemanticFourCcHasPrefix(rawcode, 'Y', 'T');
}

bool War3SemanticReadUnitCore(
    const dxvk::war3::shadow::ShadowRenderableRecord& renderable,
    uint32_t& outRawcode, uint32_t& outFlags5C, void*& outSpritePtr) {
  outRawcode = 0u;
  outFlags5C = 0u;
  outSpritePtr = nullptr;
  if (renderable.unitPtr == nullptr ||
      !dxvk::war3::IsReadableRangeFast(renderable.unitPtr, 0x64u)) {
    return false;
  }

  if (!dxvk::war3::SafeReadU32Fast(renderable.unitPtr,
                                   dxvk::war3::CUnitOffsets::Rawcode,
                                   outRawcode)) {
    return false;
  }

  dxvk::war3::SafeReadU32Fast(renderable.unitPtr,
                              dxvk::war3::CUnitOffsets::Flags5C,
                              outFlags5C);
  if (!dxvk::war3::SafeReadPtrFast(renderable.unitPtr,
                                   dxvk::war3::CUnitOffsets::Sprite,
                                   outSpritePtr)) {
    outSpritePtr = nullptr;
  }

  return true;
}

bool War3SemanticPacketHasStableUnitResource(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  const auto& renderable = packet.renderable;
  return renderable.runtimeModelPtr != nullptr &&
         (renderable.modelResourcePtr != nullptr || renderable.modelKey != 0u ||
          packet.resource.modelResourcePtr != nullptr ||
          packet.resource.modelKey != 0u);
}

struct War3SemanticUnitValidationCacheEntry {
  void* unitPtr = nullptr;
  void* runtimeModelPtr = nullptr;
  uint32_t rawcode = 0u;
  uint64_t frameSerial = 0u;
  bool valid = false;
  bool populated = false;
};

War3SemanticUnitValidationCacheEntry&
War3SemanticUnitValidationCacheSlot(
    const dxvk::war3::shadow::ShadowRenderableRecord& renderable) {
  thread_local std::array<War3SemanticUnitValidationCacheEntry, 2048u> s_cache;
  uintptr_t hash = reinterpret_cast<uintptr_t>(renderable.unitPtr);
  hash ^= reinterpret_cast<uintptr_t>(renderable.runtimeModelPtr) >> 4u;
  hash ^= uintptr_t(renderable.rawcode) * uintptr_t(0x9E3779B1u);
  hash ^= uintptr_t(renderable.frameSerial) * uintptr_t(0x85EBCA6Bu);
  return s_cache[hash & (s_cache.size() - 1u)];
}

bool War3SemanticPacketHasConsistentUnitCore(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  const auto& renderable = packet.renderable;
  auto& cacheEntry = War3SemanticUnitValidationCacheSlot(renderable);
  if (cacheEntry.populated && cacheEntry.unitPtr == renderable.unitPtr &&
      cacheEntry.runtimeModelPtr == renderable.runtimeModelPtr &&
      cacheEntry.rawcode == renderable.rawcode &&
      cacheEntry.frameSerial == renderable.frameSerial) {
    return cacheEntry.valid;
  }

  uint32_t unitRawcode = 0u;
  uint32_t unitFlags5C = 0u;
  void* unitSpritePtr = nullptr;
  bool valid = War3SemanticReadUnitCore(renderable, unitRawcode, unitFlags5C,
                                        unitSpritePtr);
  if (valid)
    valid = unitSpritePtr != nullptr;
  if (valid)
    valid = (unitFlags5C & dxvk::war3::UnitFlags5C::Building) == 0u;
  const uint32_t semanticRawcode =
      renderable.rawcode != 0u ? renderable.rawcode : unitRawcode;
  if (valid) {
    valid = !War3SemanticRawcodeLooksStaticWorldCaster(semanticRawcode) &&
            !War3SemanticRawcodeLooksStaticWorldCaster(unitRawcode);
  }
  if (valid && renderable.rawcode != 0u && unitRawcode != 0u) {
    valid =
        War3SemanticFourCcEqualEitherOrder(renderable.rawcode, unitRawcode);
  }

  cacheEntry.unitPtr = renderable.unitPtr;
  cacheEntry.runtimeModelPtr = renderable.runtimeModelPtr;
  cacheEntry.rawcode = renderable.rawcode;
  cacheEntry.frameSerial = renderable.frameSerial;
  cacheEntry.valid = valid;
  cacheEntry.populated = true;
  return valid;
}

bool War3HasSemanticDynamicUnitEvidence(
    const dxvk::war3::shadow::ShadowDrawPacket& packet);

dxvk::war3::render::ObjectKind War3ResolveSemanticPacketObjectKind(
    const dxvk::war3::shadow::ShadowRenderableRecord& renderable);

bool War3ShouldSubmitSemanticPacket(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind, bool unitsOnly);

bool War3IsEligibleSemanticDynamicUnit(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind) {
  if (!War3IsSemanticUnitObject(resolvedObjectKind))
    return false;

  return War3HasSemanticDynamicUnitEvidence(packet);
}

bool War3HasSemanticDynamicUnitEvidence(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  const auto& renderable = packet.renderable;
  if (renderable.queueKind ==
      dxvk::war3::render::VisibleRenderableQueueKind::Transparent) {
    return false;
  }

  // WorldObjects group 0 is the live unit group. Groups 1/2 carry buildings,
  // selection/building subparts, doodads and effects; accepting them as
  // "skinned units" is what produced the flickering construction/scaffold
  // caster silhouettes and the grey full-scene veil.
  if (renderable.groupIdx > 0)
    return false;

  if ((renderable.unitFlags5C & dxvk::war3::UnitFlags5C::Building) != 0u)
    return false;

  if (!War3SemanticPacketHasStableUnitResource(packet))
    return false;

  if (renderable.rawcode == 0u && renderable.jHandle == 0u)
    return false;

  if (renderable.unitPtr == nullptr)
    return false;

  if (packet.path != dxvk::war3::shadow::ShadowDrawPath::Skinned)
    return false;

  if (War3SemanticRawcodeLooksStaticWorldCaster(renderable.rawcode))
    return false;

  if (War3SemanticValidateUnitCoreRuntime())
    return War3SemanticPacketHasConsistentUnitCore(packet);

  return true;
}

bool War3SemanticPacketUsesDirectGeosetData(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  return packet.renderable.meshData != nullptr &&
         packet.renderable.runtimeGeosetDataPtr != nullptr &&
         packet.renderable.meshData == packet.renderable.runtimeGeosetDataPtr;
}

dxvk::war3::render::ObjectKind War3ResolveSemanticPacketObjectKindFast(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  if (packet.renderable.objectKind !=
      dxvk::war3::render::ObjectKind::Unknown) {
    return packet.renderable.objectKind;
  }

  if (War3HasSemanticDynamicUnitEvidence(packet))
    return dxvk::war3::render::ObjectKind::Unit;

  return War3ResolveSemanticPacketObjectKind(packet.renderable);
}

bool War3ShouldSubmitSemanticPacketFast(
    const dxvk::war3::shadow::ShadowDrawPacket& packet, bool unitsOnly) {
  if (unitsOnly) {
    if (packet.renderable.objectKind !=
        dxvk::war3::render::ObjectKind::Unknown) {
      return War3IsEligibleSemanticDynamicUnit(packet,
                                               packet.renderable.objectKind);
    }
    return War3HasSemanticDynamicUnitEvidence(packet);
  }

  const auto resolvedObjectKind =
      War3ResolveSemanticPacketObjectKindFast(packet);
  return War3ShouldSubmitSemanticPacket(packet, resolvedObjectKind, false);
}

bool War3LooksSubmitEligibleForScoringFast(
    const dxvk::war3::shadow::ShadowDrawPacket& packet, bool unitsOnly) {
  if (!unitsOnly)
    return War3ShouldSubmitSemanticPacketFast(packet, false);

  const auto& renderable = packet.renderable;
  if (packet.path != dxvk::war3::shadow::ShadowDrawPath::Skinned)
    return false;
  if (renderable.queueKind ==
      dxvk::war3::render::VisibleRenderableQueueKind::Transparent)
    return false;
  if (renderable.groupIdx > 0)
    return false;
  if (renderable.unitPtr == nullptr)
    return false;
  if ((renderable.unitFlags5C & dxvk::war3::UnitFlags5C::Building) != 0u)
    return false;
  if (renderable.rawcode == 0u && renderable.jHandle == 0u)
    return false;
  if (renderable.objectKind != dxvk::war3::render::ObjectKind::Unknown &&
      !War3IsSemanticUnitObject(renderable.objectKind))
    return false;
  if (War3SemanticRawcodeLooksStaticWorldCaster(renderable.rawcode))
    return false;

  return War3SemanticPacketHasStableUnitResource(packet);
}

bool War3IsEligibleSemanticStaticWorldCaster(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind,
    bool hasRenderableGeoset, bool hasPacketGeometry) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate) {
    return false;
  }

  if (resolvedObjectKind != dxvk::war3::render::ObjectKind::Building &&
      resolvedObjectKind != dxvk::war3::render::ObjectKind::Destructible) {
    return false;
  }

  const auto& renderable = packet.renderable;
  if (renderable.queueKind ==
      dxvk::war3::render::VisibleRenderableQueueKind::Transparent) {
    return false;
  }

  if (packet.path != dxvk::war3::shadow::ShadowDrawPath::Rigid)
    return false;

  if (renderable.worldObjectEntry == nullptr ||
      renderable.sceneNode == nullptr) {
    return false;
  }

  if (!packet.pose.hasWorldTransform || !hasRenderableGeoset ||
      !hasPacketGeometry) {
    return false;
  }

  // Selection/decoration/effect groups are not stable static-world casters.
  // Let full static-object support opt in later through a canonical manifest
  // contract instead of submitting hidden build/effect meshes here.
  if (renderable.groupIdx > 0)
    return false;

  return true;
}

dxvk::war3::render::ObjectKind War3ResolveSemanticPacketObjectKind(
    const dxvk::war3::shadow::ShadowRenderableRecord& renderable) {
  using dxvk::war3::render::ObjectKind;

  if (renderable.objectKind != ObjectKind::Unknown)
    return renderable.objectKind;

  auto& renderRegistry = dxvk::war3::render::RenderObjectRegistry::instance();
  if (renderable.sceneNode != nullptr) {
    if (const auto* object = renderRegistry.findBySceneNode(renderable.sceneNode))
      return object->kind;
  }
  if (renderable.worldObjectEntry != nullptr) {
    if (const auto* object =
            renderRegistry.findByEntry(renderable.worldObjectEntry)) {
      return object->kind;
    }
  }
  if (renderable.jHandle != 0u) {
    if (const auto* object = renderRegistry.findByHandle(renderable.jHandle))
      return object->kind;
  }

  dxvk::war3::render::ShadowObjectRecord shadowRecord = {};
  auto& shadowRegistry = dxvk::war3::render::ShadowObjectRegistry::instance();
  if (renderable.sceneNode != nullptr &&
      shadowRegistry.findBySceneNode(renderable.sceneNode, shadowRecord)) {
    return shadowRecord.kind;
  }
  if (renderable.worldObjectEntry != nullptr &&
      shadowRegistry.findByWorldObjectEntry(renderable.worldObjectEntry,
                                            shadowRecord)) {
    return shadowRecord.kind;
  }
  if (renderable.jHandle != 0u &&
      shadowRegistry.findByHandle(renderable.jHandle, shadowRecord)) {
    return shadowRecord.kind;
  }
  if (renderable.runtimeModelPtr != nullptr &&
      shadowRegistry.findByRuntimeModel(renderable.runtimeModelPtr,
                                        shadowRecord)) {
    return shadowRecord.kind;
  }

  return ObjectKind::Unknown;
}

bool War3ShouldSubmitSemanticPacket(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind, bool unitsOnly) {
  const bool hasRenderableGeoset =
      packet.renderable.runtimeGeosetPtr != nullptr ||
      packet.renderable.runtimeGeosetDataPtr != nullptr ||
      packet.renderable.geosetIndex !=
          dxvk::war3::shadow::kInvalidShadowContractGeosetIndex ||
      packet.resource.geosetIndex !=
          dxvk::war3::shadow::kInvalidShadowContractGeosetIndex;
  const bool hasPacketGeometry =
      packet.resource.vertexCount != 0u ||
      (packet.usesDynamicMeshPositions &&
       packet.resource.dynamicPositionStream != nullptr &&
       packet.resource.dynamicPositionStride >= 12u);
  const bool explicitUnknownRigid =
      resolvedObjectKind == dxvk::war3::render::ObjectKind::Unknown &&
      packet.path == dxvk::war3::shadow::ShadowDrawPath::Rigid &&
      packet.renderable.worldObjectEntry != nullptr &&
      packet.renderable.sceneNode != nullptr &&
      packet.pose.hasWorldTransform && hasRenderableGeoset &&
      hasPacketGeometry;

  if (!unitsOnly) {
    if (War3IsEligibleSemanticDynamicUnit(packet, resolvedObjectKind) ||
        War3IsEligibleSemanticStaticWorldCaster(
            packet, resolvedObjectKind, hasRenderableGeoset,
            hasPacketGeometry))
      return true;
    // Keep the explicit resource-owner rigid escape hatch, but do not submit
    // generic effects/unknown translucent payloads; those were the source of
    // the dark full-screen overlay in the previous full-scene experiment.
    return explicitUnknownRigid;
  }

  return War3IsEligibleSemanticDynamicUnit(packet, resolvedObjectKind);
}

struct War3SemanticSceneFrameScore {
  uint32_t inputDrawCount = 0u;
  uint32_t eligibleDrawCount = 0u;
  uint32_t skinnedDrawCount = 0u;
};

War3SemanticSceneFrameScore War3ScoreSemanticSceneFrame(
    const dxvk::war3::shadow::ShadowSubmissionFrame* frame,
    bool unitsOnly) {
  War3SemanticSceneFrameScore score = {};
  if (frame == nullptr || frame->frameSerial == 0u)
    return score;

  score.inputDrawCount = static_cast<uint32_t>(
      std::min<size_t>(frame->draws.size(), size_t(0xFFFFFFFFu)));
  for (const auto& draw : frame->draws) {
    const bool eligible = War3LooksSubmitEligibleForScoringFast(draw, unitsOnly);
    if (eligible)
      ++score.eligibleDrawCount;
    if (eligible && draw.path == dxvk::war3::shadow::ShadowDrawPath::Skinned)
      ++score.skinnedDrawCount;
  }
  return score;
}

bool War3ShouldPreferSemanticSceneFrame(
    const std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame>&
        candidate,
    const std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame>&
        current,
    bool unitsOnly) {
  if (candidate == nullptr || candidate->frameSerial == 0u ||
      candidate->draws.empty())
    return false;
  if (current == nullptr || current->frameSerial == 0u ||
      current->draws.empty())
    return true;
  if (candidate.get() == current.get())
    return false;

  const auto candidateScore =
      War3ScoreSemanticSceneFrame(candidate.get(), unitsOnly);
  const auto currentScore = War3ScoreSemanticSceneFrame(current.get(), unitsOnly);

  if (candidateScore.eligibleDrawCount == 0u &&
      currentScore.eligibleDrawCount != 0u)
    return false;
  if (candidateScore.skinnedDrawCount != currentScore.skinnedDrawCount)
    return candidateScore.skinnedDrawCount > currentScore.skinnedDrawCount;
  if (candidateScore.eligibleDrawCount != currentScore.eligibleDrawCount)
    return candidateScore.eligibleDrawCount > currentScore.eligibleDrawCount;
  if (candidateScore.inputDrawCount != currentScore.inputDrawCount)
    return candidateScore.inputDrawCount > currentScore.inputDrawCount;

  if (candidate->sourcePublishRevision != current->sourcePublishRevision)
    return candidate->sourcePublishRevision > current->sourcePublishRevision;

  return candidate->frameSerial > current->frameSerial;
}

bool War3SemanticDataModuleEnabled() {
  return war3::runtime::IsWar3RuntimeModuleEnabled(
      war3::runtime::War3RuntimeModule::SemanticData);
}

bool War3SemanticModelProducerEnabled() {
  return War3SemanticDataModuleEnabled() &&
         dxvk::war3::internal::
             kWar3RuntimeConfigSemanticModelProducerEffective;
}

bool War3SemanticFrameRegistriesEnabled() {
  return War3SemanticModelProducerEnabled() &&
         dxvk::war3::internal::
             kWar3RuntimeConfigSemanticFrameRegistriesEffective;
}

bool War3SemanticContractCaptureEnabled() {
  return War3SemanticModelProducerEnabled() &&
         dxvk::war3::internal::
             kWar3RuntimeConfigSemanticContractCaptureEffective;
}

bool War3SemanticConsumerEnabled() {
  return War3SemanticModelProducerEnabled() &&
         dxvk::war3::internal::
             kWar3RuntimeConfigSemanticConsumerEffective;
}

void War3PublishSemanticRegistriesForScene() {
  // BeforeUi is the first point where the DXVK shadow scene can be submitted,
  // so publish the write-side semantic registries here instead of waiting for
  // FlushAndReset/EndFrame. The individual endFrame calls are idempotent
  // publish/freshness operations; they do not clear the current frame data.
  dxvk::war3::render::War3Renderer::instance()
      .PublishSemanticRegistriesForScene();
}

class War3DxvkSemanticShadowHost final
    : public dxvk::war3::shadow::IDxvkValidationHost {
public:
  explicit War3DxvkSemanticShadowHost(D3D9DeviceEx& device)
      : m_device(device) {
  }

  bool shouldSubmitDraw(const dxvk::war3::shadow::ShadowDrawPacket& packet,
                        bool unitsOnly) const override {
    return War3ShouldSubmitSemanticPacketFast(packet, unitsOnly);
  }

  bool submitDrawPacket(
      const dxvk::war3::shadow::ShadowDrawPacket& packet) override {
    auto appendScope = []() -> war3::War3PerfMonitor::ScopedCpuScope {
      if constexpr (dxvk::war3::internal::
                        kNativeOptimizationPerfTrackingEnabled) {
        return war3::War3PerfMonitor::instance().cpuScope(
            "War3SemanticScene/SubmitFrame/AppendPacket");
      }
      return {};
    }();
    return m_device.War3SubmitSemanticShadowPacketForBackend(packet);
  }

  uint32_t normalizeHandle(uint32_t handle) const override {
    return War3NormalizeShadowHandle(handle);
  }

private:
  D3D9DeviceEx& m_device;
};

float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind) {
  using dxvk::war3::render::ObjectKind;
  switch (static_cast<ObjectKind>(objectKind)) {
  case ObjectKind::Unit:
    return 260.0f;
  case ObjectKind::Building:
    return 900.0f;
  case ObjectKind::Destructible:
    return 750.0f;
  case ObjectKind::Item:
    return 220.0f;
  case ObjectKind::Effect:
    return 900.0f;
  default:
    return 0.0f;
  }
}

float War3SemanticBoundsMaxScale(const Matrix4& m) {
  auto axisLen3 = [](const Vector4& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  };

  const float sx = axisLen3(m[0]);
  const float sy = axisLen3(m[1]);
  const float sz = axisLen3(m[2]);
  return (std::max)(1.0f, (std::max)(sx, (std::max)(sy, sz)));
}

Vector4 War3SemanticBoundsTranslation(const Matrix4& m) {
  return Vector4(m[3].x, m[3].y, m[3].z, 1.0f);
}

float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b) {
  const float dx = a[3].x - b[3].x;
  const float dy = a[3].y - b[3].y;
  const float dz = a[3].z - b[3].z;
  return dx * dx + dy * dy + dz * dz;
}

bool War3SemanticTranslationFinite(const Matrix4& m) {
  return std::isfinite(m[3].x) && std::isfinite(m[3].y) &&
         std::isfinite(m[3].z);
}

bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette) {
  if (palette.empty())
    return false;
  if (palette.size() > 256u)
    return false;
  return dxvk::war3::IsReadableRange(
      palette.data(), palette.size() * sizeof(Matrix4));
}

template <typename T>
bool War3SemanticVectorStorageReadable(const std::vector<T>& values,
                                       size_t requiredCount = 0u) {
  if (values.size() < requiredCount)
    return false;
  if (values.empty())
    return requiredCount == 0u;
  return dxvk::war3::IsReadableRange(
      values.data(), values.size() * sizeof(T));
}

bool War3SemanticPaletteLooksModelLocal(
    const Matrix4* palette,
    uint32_t paletteCount,
    const Matrix4& worldTransform,
    uint8_t objectKind,
    bool checkReadable = true) {
  if (palette == nullptr || paletteCount == 0u ||
      (checkReadable &&
       !dxvk::war3::IsReadableRange(palette,
                                    size_t(paletteCount) * sizeof(Matrix4))) ||
      !War3SemanticTranslationFinite(worldTransform))
    return false;

  const float worldMagSq = worldTransform[3].x * worldTransform[3].x +
                           worldTransform[3].y * worldTransform[3].y +
                           worldTransform[3].z * worldTransform[3].z;
  if (!(worldMagSq > 16.0f))
    return false;

  float guardRadius = War3SemanticBoundsRadiusForObjectKind(objectKind);
  if (!(guardRadius > 0.0f))
    guardRadius = 260.0f;
  guardRadius = std::max(384.0f, guardRadius * 1.5f);
  const float thresholdSq = guardRadius * guardRadius;

  float closestSq = std::numeric_limits<float>::max();
  float closestPaletteMagSq = std::numeric_limits<float>::max();
  const uint32_t sampleCount =
      std::min<uint32_t>(paletteCount, 4u);
  for (uint32_t i = 0u; i < sampleCount; ++i) {
    if (!War3SemanticTranslationFinite(palette[i]))
      return false;
    const float px = palette[i][3].x;
    const float py = palette[i][3].y;
    const float pz = palette[i][3].z;
    closestPaletteMagSq =
        std::min(closestPaletteMagSq, px * px + py * py + pz * pz);
    closestSq = std::min(
        closestSq,
        War3SemanticTranslationDistanceSq(palette[i], worldTransform));
  }

  // The shadow caster shader expects world-space fixed-function matrices
  // because it evaluates `in_pos * paletteMatrix` directly. CModel's live
  // final-pose array can be model-local on the semantic direct-read path, while
  // CModel+0x64 carries the runtime world transform. If sampled palette
  // translations are far from the runtime world origin, treat the palette as
  // model-local and compose it to the same world-space contract the old D3D
  // fixed-function path provided.
  const float localMagLimit = std::max(1024.0f, guardRadius * 2.0f);
  if (closestPaletteMagSq > localMagLimit * localMagLimit)
    return false;

  return closestSq > thresholdSq;
}

bool War3SemanticPaletteLooksModelLocal(
    const std::vector<Matrix4>& palette,
    const Matrix4& worldTransform,
    uint8_t objectKind) {
  if (!War3SemanticPaletteStorageReadable(palette))
    return false;
  return War3SemanticPaletteLooksModelLocal(
      palette.data(), uint32_t(palette.size()), worldTransform, objectKind,
      false);
}

void War3SemanticBuildWorldPaletteIfNeeded(
    const std::vector<Matrix4>& sourcePalette,
    const Matrix4& worldTransform,
    uint8_t objectKind,
    std::vector<Matrix4>& outPalette) {
  outPalette.clear();
  if (!War3SemanticPaletteLooksModelLocal(sourcePalette, worldTransform,
                                          objectKind)) {
    return;
  }

  outPalette.reserve(sourcePalette.size());
  for (const Matrix4& localMatrix : sourcePalette)
    outPalette.push_back(worldTransform * localMatrix);
}

uint64_t War3SemanticHashMatrix4(const Matrix4& matrix) {
  uint64_t hash = bit::fnv1a_init();
  for (uint32_t r = 0u; r < 4u; ++r) {
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].x));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].y));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].z));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].w));
  }
  return hash;
}

uint64_t War3SemanticHashMatrixPalette(const Matrix4* matrices,
                                        uint32_t matrixCount) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, matrixCount);
  if (matrices == nullptr || matrixCount == 0u)
    return hash;

  for (uint32_t i = 0u; i < matrixCount; ++i) {
    const Matrix4& matrix = matrices[i];
    for (uint32_t r = 0u; r < 4u; ++r) {
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].x));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].y));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].z));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].w));
    }
  }
  return hash;
}

Matrix4 War3DecodeRuntimePoseMatrix48(const uint8_t* poseBytes) {
  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseBytes, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

bool War3TryReadRuntimePoseArray(void* runtimeModelPtr,
                                 uint32_t& outPoseCount,
                                 void*& outPoseArrayPtr) {
  outPoseCount = 0u;
  outPoseArrayPtr = nullptr;
  if (runtimeModelPtr == nullptr)
    return false;

  if (!dxvk::war3::SafeReadU32Fast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          outPoseCount) ||
      outPoseCount == 0u || outPoseCount > 1024u ||
      !dxvk::war3::SafeReadPtrFast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          outPoseArrayPtr) ||
      outPoseArrayPtr == nullptr ||
      !dxvk::war3::IsReadableRange(
          outPoseArrayPtr, size_t(outPoseCount) * sizeof(float) * 12u)) {
    outPoseCount = 0u;
    outPoseArrayPtr = nullptr;
    return false;
  }
  return true;
}

void* War3ResolveLivePoseRuntimeAlias(void* runtimeModelPtr,
                                      uint32_t& outPoseCount,
                                      void*& outPoseArrayPtr) {
  outPoseCount = 0u;
  outPoseArrayPtr = nullptr;
  if (runtimeModelPtr == nullptr)
    return nullptr;

  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  const uintptr_t value = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (value < 0x10000u)
    return nullptr;

  std::array<void*, 3> candidates = {
      reinterpret_cast<void*>(value + kCModelComplexExtensionOffset),
      runtimeModelPtr,
      value > kCModelComplexExtensionOffset
          ? reinterpret_cast<void*>(value - kCModelComplexExtensionOffset)
          : nullptr};

  for (void* candidate : candidates) {
    uint32_t poseCount = 0u;
    void* poseArrayPtr = nullptr;
    if (!War3TryReadRuntimePoseArray(candidate, poseCount, poseArrayPtr))
      continue;
    outPoseCount = poseCount;
    outPoseArrayPtr = poseArrayPtr;
    return candidate;
  }

  return nullptr;
}

bool War3TryBuildLiveRuntimeGroupPalette(
    const dxvk::war3::shadow::ShadowPacketResource& resource,
    void* runtimeModelPtr,
    uint64_t frameSerial,
    std::vector<Matrix4>& outPalette,
    uint32_t& outMaxVertexGroupSlot,
    uint64_t& outHash,
    uint64_t* outRawPoseHash = nullptr,
    void** outPoseRuntimeModelPtr = nullptr) {
  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outHash = 0u;
  if (outRawPoseHash != nullptr)
    *outRawPoseHash = 0u;
  if (outPoseRuntimeModelPtr != nullptr)
    *outPoseRuntimeModelPtr = nullptr;
  if (runtimeModelPtr == nullptr)
    return false;
  const auto& vertexGroups = resource.vertexGroupIndexVec();
  const auto& matrixGroupSizes = resource.matrixGroupSizeVec();
  const auto& matrixIndices = resource.matrixIndexVec();
  if (vertexGroups.empty())
    return false;

  uint32_t poseCount = 0u;
  void* poseArrayPtr = nullptr;
  void* poseRuntimeModelPtr = nullptr;
  const Matrix4* publishedPoseMatrices = nullptr;
  uint64_t publishedPoseHash = 0u;
  dxvk::war3::model::PoseRecord publishedPose = {};
  auto tryUsePublishedPose = [&](void* candidateRuntimeModelPtr) -> bool {
    if (candidateRuntimeModelPtr == nullptr)
      return false;
    dxvk::war3::model::PoseRecord candidate = {};
    if (!dxvk::war3::model::PoseRegistry::instance().findByRuntimeModel(
            candidateRuntimeModelPtr, candidate) ||
        candidate.matrixCount == 0u || candidate.matrixPalette.empty()) {
      return false;
    }

    publishedPose = std::move(candidate);
    poseRuntimeModelPtr = publishedPose.runtimeModelPtr != nullptr
                              ? publishedPose.runtimeModelPtr
                              : candidateRuntimeModelPtr;
    poseCount = std::min<uint32_t>(
        publishedPose.matrixCount,
        uint32_t(std::min<size_t>(publishedPose.matrixPalette.size(),
                                  size_t(1024u))));
    if (poseCount == 0u)
      return false;
    publishedPoseMatrices = publishedPose.matrixPalette.data();
    publishedPoseHash =
        publishedPose.matrixHash != 0u
            ? publishedPose.matrixHash
            : War3SemanticHashMatrixPalette(publishedPoseMatrices, poseCount);
    return true;
  };

  bool usingPublishedPose = tryUsePublishedPose(runtimeModelPtr);
  if (!usingPublishedPose) {
    constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
    const uintptr_t runtimeValue = reinterpret_cast<uintptr_t>(runtimeModelPtr);
    if (runtimeValue >= 0x10000u) {
      if (runtimeValue <= (~uintptr_t(0u)) - kCModelComplexExtensionOffset)
        usingPublishedPose = tryUsePublishedPose(
            reinterpret_cast<void*>(runtimeValue + kCModelComplexExtensionOffset));
      if (!usingPublishedPose && runtimeValue > kCModelComplexExtensionOffset)
        usingPublishedPose = tryUsePublishedPose(
            reinterpret_cast<void*>(runtimeValue - kCModelComplexExtensionOffset));
    }
  }
  if (!usingPublishedPose) {
    poseRuntimeModelPtr = War3ResolveLivePoseRuntimeAlias(
        runtimeModelPtr, poseCount, poseArrayPtr);
    if (poseRuntimeModelPtr == nullptr) {
      return false;
    }
    usingPublishedPose = tryUsePublishedPose(poseRuntimeModelPtr);
    if (!usingPublishedPose) {
      constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
      const uintptr_t poseRuntimeValue =
          reinterpret_cast<uintptr_t>(poseRuntimeModelPtr);
      if (poseRuntimeValue >= 0x10000u) {
        if (poseRuntimeValue <=
            (~uintptr_t(0u)) - kCModelComplexExtensionOffset)
          usingPublishedPose = tryUsePublishedPose(reinterpret_cast<void*>(
              poseRuntimeValue + kCModelComplexExtensionOffset));
        if (!usingPublishedPose &&
            poseRuntimeValue > kCModelComplexExtensionOffset)
          usingPublishedPose = tryUsePublishedPose(reinterpret_cast<void*>(
              poseRuntimeValue - kCModelComplexExtensionOffset));
      }
    }
  }
  if (poseRuntimeModelPtr == nullptr)
    return false;
  if (!usingPublishedPose && !War3SemanticLivePaletteAllowCModelFallbackRuntime())
    return false;
  if (outPoseRuntimeModelPtr != nullptr)
    *outPoseRuntimeModelPtr = poseRuntimeModelPtr;

  const auto* poseBytes = reinterpret_cast<const uint8_t*>(poseArrayPtr);
  // Most Warcraft III runtime models expose a modest final-pose array here.
  // Decoding it once per visible packet is cheaper and more deterministic than
  // repeated matrix-index probes through an on-demand cache.
  (void)frameSerial;
  thread_local std::array<Matrix4, 1024> s_posePalette = {};
  if (usingPublishedPose && publishedPoseMatrices != nullptr) {
    for (uint32_t i = 0u; i < poseCount; ++i)
      s_posePalette[i] = publishedPoseMatrices[i];
  } else {
    if (poseBytes == nullptr)
      return false;
    poseCount = std::min<uint32_t>(poseCount, uint32_t(s_posePalette.size()));
    for (uint32_t i = 0u; i < poseCount; ++i) {
      s_posePalette[i] = War3DecodeRuntimePoseMatrix48(
          poseBytes + size_t(i) * sizeof(float) * 12u);
    }
  }
  if (outRawPoseHash != nullptr) {
    *outRawPoseHash = usingPublishedPose && publishedPoseHash != 0u
                          ? publishedPoseHash
                          : War3SemanticHashMatrixPalette(s_posePalette.data(),
                                                          poseCount);
  }
  auto decodePoseMatrix = [&](uint32_t index, Matrix4& outMatrix) -> bool {
    if (index >= poseCount)
      return false;
    outMatrix = s_posePalette[index];
    return true;
  };

  std::array<uint16_t, 256> uniqueGroupSlots = {};
  uint32_t uniqueGroupSlotCount = 0u;
  std::array<bool, 256> seenGroupSlots = {};
  for (const uint8_t groupSlot : vertexGroups) {
    outMaxVertexGroupSlot =
        std::max(outMaxVertexGroupSlot, uint32_t(groupSlot));
    if (!seenGroupSlots[groupSlot]) {
      seenGroupSlots[groupSlot] = true;
      uniqueGroupSlots[uniqueGroupSlotCount++] = groupSlot;
    }
  }

  auto buildDirectMatrixRemap = [&]() -> bool {
    if (matrixIndices.empty() ||
        outMaxVertexGroupSlot >= matrixIndices.size())
      return false;
    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
      const uint32_t matrixIndex = matrixIndices[group];
      if (!decodePoseMatrix(matrixIndex, outPalette[group]))
        return false;
    }
    return true;
  };

  auto buildSparseMatrixRemap = [&]() -> bool {
    if (matrixIndices.empty() || uniqueGroupSlotCount == 0u ||
        uniqueGroupSlotCount > matrixIndices.size())
      return false;
    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (uint32_t i = 0u; i < uniqueGroupSlotCount; ++i) {
      const uint32_t matrixIndex = matrixIndices[i];
      const uint32_t groupSlot = uniqueGroupSlots[i];
      if (!decodePoseMatrix(matrixIndex, outPalette[groupSlot]))
        return false;
    }
    return true;
  };

  auto buildDirectPosePalette = [&]() -> bool {
    if (outMaxVertexGroupSlot >= poseCount)
      return false;
    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
      if (!decodePoseMatrix(group, outPalette[group]))
        return false;
    }
    return true;
  };

  auto buildSparsePosePalette = [&]() -> bool {
    if (uniqueGroupSlotCount == 0u || uniqueGroupSlotCount > poseCount)
      return false;
    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (uint32_t i = 0u; i < uniqueGroupSlotCount; ++i) {
      if (!decodePoseMatrix(i, outPalette[uniqueGroupSlots[i]]))
        return false;
    }
    return true;
  };

  auto buildUniformPosePalette = [&]() -> bool {
    Matrix4 firstPose;
    if (!decodePoseMatrix(0u, firstPose))
      return false;
    const uint32_t paletteCount =
        std::max(outMaxVertexGroupSlot + 1u,
                 uint32_t(matrixGroupSizes.size()));
    if (paletteCount == 0u)
      return false;
    outPalette.assign(paletteCount, firstPose);
    return true;
  };

  const uint32_t groupCount = uint32_t(matrixGroupSizes.size());
  if (groupCount != 0u) {
    std::array<uint32_t, 256> prefix = {};
    if (groupCount <= prefix.size()) {
    uint32_t running = 0u;
    for (uint32_t i = 0u; i < groupCount; ++i) {
      prefix[i] = running;
      running += matrixGroupSizes[i];
    }
    if (running <= matrixIndices.size()) {
      outPalette.resize(groupCount);
      bool valid = true;
      for (uint32_t group = 0u; group < groupCount && valid; ++group) {
        const uint32_t groupSize = matrixGroupSizes[group];
        const uint32_t groupBase = prefix[group];
        if (groupSize == 0u ||
            (groupBase + groupSize) > matrixIndices.size()) {
          valid = false;
          break;
        }
        Matrix4 accum(0.0f);
        for (uint32_t i = 0u; i < groupSize; ++i) {
          const uint32_t matrixIndex = matrixIndices[groupBase + i];
          Matrix4 poseMatrix;
          if (!decodePoseMatrix(matrixIndex, poseMatrix)) {
            valid = false;
            break;
          }
          accum += poseMatrix;
        }
        if (valid)
          outPalette[group] =
              groupSize == 1u ? accum : (accum / float(groupSize));
      }
      if (valid) {
        for (const uint8_t groupSlot : vertexGroups) {
          if (uint32_t(groupSlot) >= groupCount) {
            valid = false;
            break;
          }
        }
      }
      if (valid && !outPalette.empty()) {
        outHash = War3SemanticHashMatrixPalette(outPalette.data(),
                                                uint32_t(outPalette.size()));
        return true;
      }
    }
    }
  }

  const bool fallbackOk = buildDirectMatrixRemap() || buildSparseMatrixRemap() ||
                          buildDirectPosePalette() || buildSparsePosePalette() ||
                          buildUniformPosePalette();
  if (!fallbackOk || outPalette.empty())
    return false;
  outHash = War3SemanticHashMatrixPalette(outPalette.data(),
                                          uint32_t(outPalette.size()));
  return true;
}

struct War3SemanticPaletteMotionEntry {
  void* runtimeModelPtr = nullptr;
  uint64_t rawHash = 0u;
  uint64_t groupHash = 0u;
  uint64_t frameSerial = 0u;
};

void War3NoteLivePaletteMotion(War3ShadowCaptureStats& stats,
                               void* runtimeModelPtr,
                               uint64_t frameSerial,
                               uint64_t rawHash,
                               uint64_t groupHash) {
  if (runtimeModelPtr == nullptr || rawHash == 0u || groupHash == 0u)
    return;

  stats.semanticSceneLivePaletteMotionSampleCount++;
  static std::array<War3SemanticPaletteMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticPaletteMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }

  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }

  if (entry == nullptr) {
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];
  }

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneLivePaletteMotionLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneLivePaletteMotionLastPrevRawHash =
      isNewRuntime ? 0u : entry->rawHash;
  stats.semanticSceneLivePaletteMotionLastRawHash = rawHash;
  stats.semanticSceneLivePaletteMotionLastPrevGroupHash =
      isNewRuntime ? 0u : entry->groupHash;
  stats.semanticSceneLivePaletteMotionLastGroupHash = groupHash;

  if (isNewRuntime) {
    stats.semanticSceneLivePaletteMotionNewRuntimeCount++;
  } else {
    if (entry->rawHash != rawHash)
      stats.semanticSceneLivePaletteMotionRawChangedCount++;
    else
      stats.semanticSceneLivePaletteMotionRawStableCount++;

    if (entry->groupHash != groupHash)
      stats.semanticSceneLivePaletteMotionGroupChangedCount++;
    else
      stats.semanticSceneLivePaletteMotionGroupStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->rawHash = rawHash;
  entry->groupHash = groupHash;
  entry->frameSerial = frameSerial;
}

struct War3SemanticHashMotionEntry {
  void* runtimeModelPtr = nullptr;
  uint64_t hash = 0u;
  uint64_t frameSerial = 0u;
};

void War3NoteDrawTimePoseMotion(War3ShadowCaptureStats& stats,
                                void* runtimeModelPtr,
                                uint64_t frameSerial,
                                uint64_t hash) {
  if (runtimeModelPtr == nullptr || hash == 0u)
    return;

  static std::array<War3SemanticHashMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticHashMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }
  if (entry == nullptr)
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneDrawTimePoseLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneDrawTimePoseLastPrevHash =
      isNewRuntime ? 0u : entry->hash;
  stats.semanticSceneDrawTimePoseLastHash = hash;

  if (!isNewRuntime) {
    if (entry->hash != hash)
      stats.semanticSceneDrawTimePoseChangedCount++;
    else
      stats.semanticSceneDrawTimePoseStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->hash = hash;
  entry->frameSerial = frameSerial;
}

void War3NoteSubmittedPaletteMotion(War3ShadowCaptureStats& stats,
                                    void* runtimeModelPtr,
                                    uint64_t frameSerial,
                                    uint64_t hash) {
  if (runtimeModelPtr == nullptr || hash == 0u)
    return;

  stats.semanticSceneSubmittedPaletteMotionSampleCount++;
  static std::array<War3SemanticHashMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticHashMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }
  if (entry == nullptr)
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneSubmittedPaletteMotionLastPrevHash =
      isNewRuntime ? 0u : entry->hash;
  stats.semanticSceneSubmittedPaletteMotionLastHash = hash;

  if (isNewRuntime) {
    stats.semanticSceneSubmittedPaletteMotionNewRuntimeCount++;
  } else if (entry->hash != hash) {
    stats.semanticSceneSubmittedPaletteMotionChangedCount++;
  } else {
    stats.semanticSceneSubmittedPaletteMotionStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->hash = hash;
  entry->frameSerial = frameSerial;
}

void War3ApplySemanticBoundsFromMatrix(War3ShadowCasterDraw& draw,
                                       const Matrix4& basisMatrix,
                                       const Vector4& localCenter,
                                       float localRadius) {
  Vector4 worldCenter = basisMatrix * localCenter;
  if (!(worldCenter.w != 0.0f))
    worldCenter = War3SemanticBoundsTranslation(basisMatrix);
  else
    worldCenter /= worldCenter.w;
  worldCenter.w = 1.0f;
  draw.boundsCenter = worldCenter;
  if (localRadius > 0.0f)
    draw.boundsRadius = localRadius * War3SemanticBoundsMaxScale(basisMatrix);
  else
    draw.boundsRadius = 0.0f;
}

bool War3TryResolveNativeShadowHint(
    const dxvk::War3ShadowSemanticContext& semantic,
    const dxvk::war3::render::RenderObjectInfo* currentObj,
    dxvk::war3::native::War3NativeShadowHint& outHint) {
  auto& registry = dxvk::war3::native::War3NativeShadowHintRegistry::instance();

  auto tryByObject = [&](void* objectPtr) {
    return objectPtr != nullptr && registry.findByObjectPtr(objectPtr, outHint);
  };

  if (semantic.jHandle != 0u && registry.findByHandle(semantic.jHandle, outHint))
    return true;

  if (semantic.object != nullptr) {
    if (tryByObject(semantic.object->unitPtr) ||
        tryByObject(semantic.object->agentPtr) ||
        tryByObject(semantic.object->worldObjectEntry) ||
        tryByObject(semantic.object->sceneNode)) {
      return true;
    }
  }

  if (currentObj != nullptr && currentObj != semantic.object) {
    if (tryByObject(currentObj->unitPtr) || tryByObject(currentObj->agentPtr) ||
        tryByObject(currentObj->worldObjectEntry) ||
        tryByObject(currentObj->sceneNode)) {
      return true;
    }
    if (currentObj->jHandle != 0u &&
        registry.findByHandle(currentObj->jHandle, outHint)) {
      return true;
    }
  }

  return tryByObject(semantic.renderablePart) ||
         tryByObject(semantic.worldObjectEntry) ||
         tryByObject(semantic.sceneNode);
}

const dxvk::war3::render::RenderObjectInfo* War3FindRenderObjectForSemantic(
    const dxvk::War3ShadowSemanticContext& semantic,
    const dxvk::war3::render::VisibleRenderableRecord& record) {
  auto& renderRegistry = dxvk::war3::render::RenderObjectRegistry::instance();
  if (record.identity.worldObjectEntry != nullptr) {
    if (const auto* object =
            renderRegistry.findByEntry(record.identity.worldObjectEntry)) {
      return object;
    }
  }
  if (record.sceneNode != nullptr) {
    if (const auto* object = renderRegistry.findBySceneNode(record.sceneNode)) {
      return object;
    }
  }
  if (semantic.jHandle != 0u) {
    if (const auto* object = renderRegistry.findByHandle(semantic.jHandle)) {
      return object;
    }
  }
  if (record.identity.jHandle != 0u) {
    if (const auto* object = renderRegistry.findByHandle(record.identity.jHandle)) {
      return object;
    }
  }
  return nullptr;
}

void War3MergeVisibleRenderableSemantic(
    dxvk::War3ShadowSemanticContext& semantic,
    const dxvk::war3::render::VisibleRenderableRecord& record) {
  if (semantic.renderablePart == nullptr)
    semantic.renderablePart = record.renderablePart;
  if (semantic.sceneNode == nullptr)
    semantic.sceneNode = record.sceneNode;
  if (semantic.worldObjectEntry == nullptr)
    semantic.worldObjectEntry = record.identity.worldObjectEntry;
  if (semantic.runtimeModelPtr == nullptr)
    semantic.runtimeModelPtr = record.runtimeModelPtr;
  if (semantic.modelResourcePtr == nullptr)
    semantic.modelResourcePtr = record.modelResourcePtr;
  if (semantic.modelKey == 0u)
    semantic.modelKey = record.modelKey;
  if (semantic.jHandle == 0u)
    semantic.jHandle = record.identity.jHandle;
  if (semantic.rawcode == 0u)
    semantic.rawcode = record.identity.rawcode;
  if (semantic.objectKind == dxvk::war3::render::ObjectKind::Unknown)
    semantic.objectKind = record.identity.kind;

  if (semantic.object == nullptr)
    semantic.object = War3FindRenderObjectForSemantic(semantic, record);
}

void War3AugmentShadowSemanticFromVisibleManifest(
    dxvk::War3ShadowSemanticContext& semantic) {
  dxvk::war3::render::VisibleRenderableRecord record = {};
  auto& registry = dxvk::war3::render::VisibleRenderableRegistry::instance();

  if (semantic.renderablePart != nullptr &&
      registry.queryByPayload(semantic.renderablePart, record)) {
    War3MergeVisibleRenderableSemantic(semantic, record);
    return;
  }

  if (semantic.renderablePart != nullptr &&
      registry.queryByRenderablePart(semantic.renderablePart, record)) {
    War3MergeVisibleRenderableSemantic(semantic, record);
    return;
  }

  if (semantic.worldObjectEntry != nullptr &&
      registry.queryByWorldObjectEntry(semantic.worldObjectEntry, record)) {
    War3MergeVisibleRenderableSemantic(semantic, record);
    return;
  }

  if (semantic.jHandle != 0u && registry.queryByHandle(semantic.jHandle, record)) {
    War3MergeVisibleRenderableSemantic(semantic, record);
    return;
  }

  if (semantic.sceneNode != nullptr &&
      registry.queryBySceneNode(semantic.sceneNode, record)) {
    War3MergeVisibleRenderableSemantic(semantic, record);
    return;
  }

  if (semantic.runtimeModelPtr != nullptr &&
      registry.queryByRuntimeModel(semantic.runtimeModelPtr, record)) {
    War3MergeVisibleRenderableSemantic(semantic, record);
  }
}

bool War3PublishSemanticSceneBypassCandidate(
    const dxvk::War3ShadowSemanticContext& semantic,
    const dxvk::war3::render::RenderObjectInfo* currentObj) {
  if (!War3SemanticDataModuleEnabled())
    return false;
  if (!War3SemanticFrameRegistriesEnabled())
    return false;

  dxvk::war3::render::VisibleRenderableRecord record = {};
  record.queueKind = dxvk::war3::render::VisibleRenderableQueueKind::MainQueue;
  record.payload = semantic.renderablePart;
  record.renderablePart = semantic.renderablePart;
  record.sceneNode = semantic.sceneNode;
  record.runtimeModelPtr = semantic.runtimeModelPtr;
  record.modelResourcePtr = semantic.modelResourcePtr;
  record.modelKey = semantic.modelKey;

  record.identity.worldObjectEntry = semantic.worldObjectEntry;
  record.identity.sceneNode = semantic.sceneNode;
  record.identity.jHandle = semantic.jHandle;
  record.identity.rawcode = semantic.rawcode;
  record.identity.kind = semantic.objectKind;

  const auto* object = semantic.object != nullptr ? semantic.object : currentObj;
  if (object != nullptr) {
    if (record.identity.worldObjectEntry == nullptr)
      record.identity.worldObjectEntry = object->worldObjectEntry;
    if (record.identity.sceneNode == nullptr)
      record.identity.sceneNode = object->sceneNode;
    if (record.sceneNode == nullptr)
      record.sceneNode = object->sceneNode;
    if (record.identity.unitPtr == nullptr)
      record.identity.unitPtr = object->unitPtr;
    if (record.identity.agentPtr == nullptr)
      record.identity.agentPtr = object->agentPtr;
    if (record.identity.handleId == 0u)
      record.identity.handleId = object->handleId;
    if (record.identity.jHandle == 0u)
      record.identity.jHandle = object->jHandle;
    if (record.identity.rawcode == 0u)
      record.identity.rawcode = object->rawcode;
    if (record.identity.agentType == 0u)
      record.identity.agentType = object->agentType;
    if (record.identity.flags5C == 0u)
      record.identity.flags5C = object->flags5C;
    if (record.identity.kind == dxvk::war3::render::ObjectKind::Unknown)
      record.identity.kind = object->kind;
    if (record.identity.groupIdx < 0)
      record.identity.groupIdx = static_cast<int8_t>(object->groupIdx);
  }

  // This publishes only semantic identity/resource context. It deliberately
  // avoids reading host VB/IB slices or resurrecting the old capture fallback.
  auto& visibleRegistry =
      dxvk::war3::render::VisibleRenderableRegistry::instance();
  const bool registered = visibleRegistry.registerSemanticCandidate(record);
  if (registered &&
      War3SemanticDataModuleEnabled() &&
      War3SemanticContractCaptureEnabled() &&
      !dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled()) {
    const uint64_t frameNumber = visibleRegistry.getFrameNumber();
    const uint64_t visibleCount =
        static_cast<uint64_t>(visibleRegistry.getVisibleCount());
    const bool captureMilestone =
        visibleCount <= 4u ||
        (visibleCount <= 64u && (visibleCount & (visibleCount - 1u)) == 0u);
    if (captureMilestone) {
      static std::atomic<uint64_t> s_lastBypassCaptureKey{0u};
      const uint64_t key =
          (frameNumber << 32) |
          (visibleCount & 0xFFFFFFFFull);
      uint64_t expected = s_lastBypassCaptureKey.load(std::memory_order_relaxed);
      if (expected != key &&
          s_lastBypassCaptureKey.compare_exchange_strong(
              expected, key, std::memory_order_relaxed)) {
        dxvk::war3::shadow::ShadowRuntimeContractCache::instance()
            .captureLiveState();
      }
    }
  }
  return registered;
}

uint64_t War3BuildShadowSemanticIdentityHash(
    const dxvk::War3ShadowSemanticContext& semantic) {
  uint64_t hash = bit::fnv1a_init();
  if (semantic.modelKey != 0u) {
    hash = bit::fnv1a_iter(hash, semantic.modelKey);
    hash = bit::fnv1a_iter(hash, uint32_t(semantic.objectKind));
    hash = bit::fnv1a_iter(hash, uint32_t(semantic.tag));
    return hash;
  }
  // Persistent geometry 的主目标是“相同模型/相同子网格跨单位复用”，
  // 而不是“按对象实例一份”。优先使用 rawcode + objectKind 建模，
  // 只有拿不到模型身份时才退回对象级指针链，避免高压图里重复单位把
  // persistent pool 按实例灌满、导致永远 hit 不到。
  if (semantic.rawcode != 0u) {
    hash = bit::fnv1a_iter(hash, uint64_t(semantic.rawcode));
    hash = bit::fnv1a_iter(hash, uint32_t(semantic.objectKind));
    hash = bit::fnv1a_iter(hash, uint32_t(semantic.tag));
    return hash;
  }

  hash = bit::fnv1a_iter(hash, uint64_t(semantic.jHandle));
  hash = bit::fnv1a_iter(hash, uint32_t(semantic.objectKind));
  hash = bit::fnv1a_iter(hash, uint32_t(semantic.tag));
  hash = bit::fnv1a_iter(hash,
                         reinterpret_cast<uintptr_t>(semantic.worldObjectEntry));
  hash = bit::fnv1a_iter(hash,
                         reinterpret_cast<uintptr_t>(semantic.sceneNode));
  hash = bit::fnv1a_iter(hash,
                         reinterpret_cast<uintptr_t>(semantic.renderablePart));
  return hash;
}

// FourCC 统一到“编辑器显示顺序”（高字节在前），并兼容当前运行时读到的两种字节序。
constexpr uint32_t PackFourCcEditor(char c0, char c1, char c2, char c3) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(c0)) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c1)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c2)) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(c3));
}

constexpr uint32_t ByteSwapU32(uint32_t v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

inline uint32_t NormalizeFourCcEditorOrder(uint32_t rawcode) {
  if (rawcode == 0u)
    return 0u;

  const uint32_t direct = rawcode;
  const uint32_t swapped = ByteSwapU32(rawcode);

  auto looksLikeEditorOrder = [](uint32_t v) -> bool {
    const uint32_t c0 = (v >> 24) & 0xFFu;
    const uint32_t c1 = (v >> 16) & 0xFFu;
    return c0 == static_cast<uint32_t>('Y') &&
           (c1 == static_cast<uint32_t>('T') ||
            c1 == static_cast<uint32_t>('t'));
  };

  uint32_t normalized = looksLikeEditorOrder(direct) ? direct : swapped;

  // 兼容 YTlc / Ytlc 的第二字符大小写差异。
  const uint32_t c1 = (normalized >> 16) & 0xFFu;
  if (c1 >= static_cast<uint32_t>('a') &&
      c1 <= static_cast<uint32_t>('z')) {
    normalized = (normalized & 0xFF00FFFFu) | ((c1 - 0x20u) << 16);
  }

  return normalized;
}

inline bool IsLosBlockerFourCc(uint32_t rawcode) {
  const uint32_t fourcc = NormalizeFourCcEditorOrder(rawcode);
  switch (fourcc) {
  case PackFourCcEditor('Y', 'T', 'a', 'b'):
  case PackFourCcEditor('Y', 'T', 'a', 'c'):
  case PackFourCcEditor('Y', 'T', 'p', 'b'):
  case PackFourCcEditor('Y', 'T', 'p', 'c'):
  case PackFourCcEditor('Y', 'T', 'f', 'b'):
  case PackFourCcEditor('Y', 'T', 'f', 'c'):
  case PackFourCcEditor('Y', 'T', 'l', 'b'):
  case PackFourCcEditor('Y', 'T', 'l', 'c'):
    return true;
  default:
    return false;
  }
}

inline void FormatFourCcEditorString(uint32_t rawcode, char out[5]) {
  const uint32_t fourcc = NormalizeFourCcEditorOrder(rawcode);
  out[0] = static_cast<char>((fourcc >> 24) & 0xFFu);
  out[1] = static_cast<char>((fourcc >> 16) & 0xFFu);
  out[2] = static_cast<char>((fourcc >> 8) & 0xFFu);
  out[3] = static_cast<char>(fourcc & 0xFFu);
  out[4] = '\0';

  for (int i = 0; i < 4; ++i) {
    const uint8_t ch = static_cast<uint8_t>(out[i]);
    if (ch < 0x20u || ch > 0x7Eu)
      out[i] = '.';
  }
}

void War3DebugRunIndexOverflowTest(D3D9DeviceEx *device) {
  static bool s_checked = false;
  static bool s_enabled = false;
  static bool s_done = false;
  if (!s_checked) {
    s_checked = true;
    const std::string envV = env::getEnvVar("DXVK_WAR3_TEST_INDEX_OVERFLOW");
    s_enabled = !envV.empty() && envV != "0";
    if (s_enabled) {
      WAR3_RENDER_LOG("DXVK War3: IndexOverflow test enabled\n");
    }
  }
  if (!s_enabled || s_done || !device)
    return;
  s_done = true;

  const uint32_t primCount =
      War3GetEnvU32("DXVK_WAR3_TEST_INDEX_OVERFLOW_PRIMS", 30000);
  const uint32_t startIndex =
      War3GetEnvU32("DXVK_WAR3_TEST_INDEX_OVERFLOW_START", 60000);
  const uint32_t vertCount = std::max<uint32_t>(
      3u, War3GetEnvU32("DXVK_WAR3_TEST_INDEX_OVERFLOW_VERTS", 65536));
  const uint32_t indexCount = GetVertexCount(D3DPT_TRIANGLELIST, primCount);
  if (!indexCount) {
    WAR3_RENDER_LOG("DXVK War3: IndexOverflow test skipped (indexCount=0)\n");
    return;
  }

  IDirect3DStateBlock9 *stateBlock = nullptr;
  if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock))) {
    stateBlock->Capture();
  }

  device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

  struct War3OverflowVertex {
    float x;
    float y;
    float z;
    float rhw;
    uint32_t color;
  };

  IDirect3DVertexBuffer9 *vb = nullptr;
  IDirect3DIndexBuffer9 *ib = nullptr;
  const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
  const UINT vbSize = vertCount * sizeof(War3OverflowVertex);
  const UINT ibSize = indexCount * sizeof(uint16_t);

  if (FAILED(device->CreateVertexBuffer(vbSize, D3DUSAGE_WRITEONLY, fvf,
                                        D3DPOOL_DEFAULT, &vb, nullptr)) ||
      FAILED(device->CreateIndexBuffer(ibSize, D3DUSAGE_WRITEONLY,
                                       D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib,
                                       nullptr))) {
    if (vb)
      vb->Release();
    if (ib)
      ib->Release();
    if (stateBlock) {
      stateBlock->Apply();
      stateBlock->Release();
    }
    WAR3_RENDER_LOG("DXVK War3: IndexOverflow test failed (alloc)\n");
    return;
  }

  D3DVIEWPORT9 vp = {};
  device->GetViewport(&vp);
  const float width = vp.Width > 0 ? float(vp.Width) : 1024.0f;
  const float height = vp.Height > 0 ? float(vp.Height) : 768.0f;
  const uint32_t grid =
      static_cast<uint32_t>(std::ceil(std::sqrt(double(vertCount))));
  const float stepX = width / std::max<uint32_t>(1, grid - 1);
  const float stepY = height / std::max<uint32_t>(1, grid - 1);

  War3OverflowVertex *vbData = nullptr;
  if (SUCCEEDED(vb->Lock(0, vbSize, reinterpret_cast<void **>(&vbData), 0)) &&
      vbData) {
    for (uint32_t i = 0; i < vertCount; ++i) {
      const uint32_t gx = i % grid;
      const uint32_t gy = i / grid;
      vbData[i].x = gx * stepX;
      vbData[i].y = gy * stepY;
      vbData[i].z = 0.5f;
      vbData[i].rhw = 1.0f;
      vbData[i].color = 0x80FF0000u;
    }
    vb->Unlock();
  }

  uint16_t *ibData = nullptr;
  if (SUCCEEDED(ib->Lock(0, ibSize, reinterpret_cast<void **>(&ibData), 0)) &&
      ibData) {
    for (uint32_t i = 0; i < indexCount; ++i) {
      ibData[i] = static_cast<uint16_t>(i % vertCount);
    }
    ib->Unlock();
  }

  device->SetStreamSource(0, vb, 0, sizeof(War3OverflowVertex));
  device->SetIndices(ib);
  WAR3_RENDER_LOG(
      "DXVK War3: IndexOverflow test draw start prim=%u start=%u verts=%u\n",
      primCount, startIndex, vertCount);
  device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, vertCount, startIndex,
                               primCount);
  WAR3_RENDER_LOG("DXVK War3: IndexOverflow test draw done\n");

  device->SetStreamSource(0, nullptr, 0, 0);
  device->SetIndices(nullptr);

  ib->Release();
  vb->Release();

  if (stateBlock) {
    stateBlock->Apply();
    stateBlock->Release();
  }
}
} // namespace

D3D9DeviceEx::D3D9DeviceEx(D3D9InterfaceEx *pParent, D3D9Adapter *pAdapter,
                           D3DDEVTYPE DeviceType, HWND hFocusWindow,
                           DWORD BehaviorFlags, Rc<DxvkDevice> dxvkDevice)
    : m_parent(pParent), m_deviceType(DeviceType), m_window(hFocusWindow),
      m_behaviorFlags(BehaviorFlags), m_adapter(pAdapter),
      m_dxvkDevice(dxvkDevice), m_memoryAllocator(), m_shaderAllocator(),
      m_ffModules(this), m_shaderModules(new D3D9ShaderModuleSet),
      m_stagingBuffer(dxvkDevice, StagingBufferSize),
      m_stagingBufferFence(new sync::Fence()),
      m_d3d9Options(dxvkDevice, pParent->GetInstance()->config()),
      m_multithread(BehaviorFlags & D3DCREATE_MULTITHREADED),
      m_isSWVP((BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0),
      m_isD3D8Compatible(pParent->IsD3D8Compatible()),
      m_csThread(dxvkDevice, dxvkDevice->createContext()),
      m_csChunk(AllocCsChunk()), m_submissionFence(new sync::Fence()),
      m_flushTracker(GetMaxFlushType()), m_d3d9Interop(this),
      m_d3d9On12Args(pAdapter->Get9On12Args()), m_d3d9On12(this),
      m_d3d8Bridge(this) {

  // If we can SWVP, then we use an extended constant set
  // as SWVP has many more slots available than HWVP.
  bool canSWVP = CanSWVP();
  DetermineConstantLayouts(canSWVP);

  if (canSWVP)
    Logger::info("D3D9DeviceEx: Using extended constant set for software "
                 "vertex processing.");

  if (m_dxvkDevice->debugFlags().test(DxvkDebugFlag::Markers))
    m_annotation = new D3D9UserDefinedAnnotation(this);

  m_initializer = new D3D9Initializer(this);
  m_converter = new D3D9FormatHelper(m_dxvkDevice);
  war3dbg::InstallCrashHandlerOnce();
  m_war3Pipeline = new War3RenderPipeline(m_dxvkDevice);
  m_war3PostProcess = new War3PostProcess(this);
  war3::SetActiveDevice(this);
  war3shader::internal::InitShaderPackRuntime(m_dxvkDevice);
  war3shader::internal::SetVulkanHandles(
      reinterpret_cast<void *>(m_dxvkDevice->instance()->handle()),
      reinterpret_cast<void *>(m_dxvkDevice->adapter()->handle()),
      reinterpret_cast<void *>(m_dxvkDevice->handle()));

  {
    auto shadowPass = std::make_unique<War3ShadowReceiverPass>(this);
    m_shadowReceiverPass = shadowPass.get();
    m_war3Pipeline->RegisterPass(
        "ShadowReceiver", std::move(shadowPass),
        war3::runtime::IsWar3RuntimeModuleEnabled(
            war3::runtime::War3RuntimeModule::ShadowReceiver));
  }
  {
    auto ssaoPass = std::make_unique<War3SsaoPass>(this);
    m_ssaoPass = ssaoPass.get();
    m_war3Pipeline->RegisterPass(
        "SSAO", std::move(ssaoPass),
        war3::runtime::IsWar3RuntimeModuleEnabled(
            war3::runtime::War3RuntimeModule::Ssao));
  }
  {
    auto aaPass = std::make_unique<War3AAPass>(this);
    m_aaPass = aaPass.get();
    m_war3Pipeline->RegisterPass(
        "AA", std::move(aaPass),
        war3::runtime::IsWar3RuntimeModuleEnabled(
            war3::runtime::War3RuntimeModule::Aa));
  }

  // War3 初始化已迁移到首次 JASS 执行时触发

  EmitCs([cDevice = m_dxvkDevice](DxvkContext *ctx) {
    ctx->beginRecording(cDevice->createCommandList());

    // Disable logic op once and for all.
    DxvkLogicOpState loState = {};
    ctx->setLogicOpState(loState);
  });

  SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

  if (!(BehaviorFlags & D3DCREATE_FPU_PRESERVE))
    SetupFPU();

  m_dxsoOptions = DxsoOptions(this, m_d3d9Options);

  // Check if VK_EXT_robustness2 is supported, so we can optimize the number of
  // constants we need to copy. Also check the required alignments.
  const bool supportsRobustness2 =
      m_dxvkDevice->features().extRobustness2.robustBufferAccess2;
  bool useRobustConstantAccess = supportsRobustness2;
  D3D9ConstantSets &vsConstSet = m_consts[DxsoProgramType::VertexShader];
  D3D9ConstantSets &psConstSet = m_consts[DxsoProgramType::PixelShader];
  if (useRobustConstantAccess) {
    m_robustSSBOAlignment =
        m_dxvkDevice->properties()
            .extRobustness2.robustStorageBufferAccessSizeAlignment;
    m_robustUBOAlignment =
        m_dxvkDevice->properties()
            .extRobustness2.robustUniformBufferAccessSizeAlignment;
    if (canSWVP) {
      const uint32_t floatBufferAlignment =
          m_dxsoOptions.vertexFloatConstantBufferAsSSBO ? m_robustSSBOAlignment
                                                        : m_robustUBOAlignment;

      useRobustConstantAccess &=
          vsConstSet.layout.floatSize() % floatBufferAlignment == 0;
      useRobustConstantAccess &=
          vsConstSet.layout.intSize() % m_robustUBOAlignment == 0;
      useRobustConstantAccess &=
          vsConstSet.layout.bitmaskSize() % m_robustUBOAlignment == 0;
    } else {
      useRobustConstantAccess &=
          vsConstSet.layout.totalSize() % m_robustUBOAlignment == 0;
    }
    useRobustConstantAccess &=
        psConstSet.layout.totalSize() % m_robustUBOAlignment == 0;
  }

  if (!useRobustConstantAccess) {
    // Disable optimized constant copies, we always have to copy all constants.
    vsConstSet.maxChangedConstF = vsConstSet.layout.floatCount;
    vsConstSet.maxChangedConstI = vsConstSet.layout.intCount;
    vsConstSet.maxChangedConstB = vsConstSet.layout.boolCount;
    psConstSet.maxChangedConstF = psConstSet.layout.floatCount;

    if (supportsRobustness2) {
      Logger::warn(
          "Disabling robust constant buffer access because of alignment.");
    }
  }

  // Check for VK_EXT_graphics_pipeline_libraries
  m_usingGraphicsPipelines =
      dxvkDevice->features().extGraphicsPipelineLibrary.graphicsPipelineLibrary;

  // Check for VK_EXT_depth_bias_control and set up initial state
  m_depthBiasRepresentation = {
      VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT, false};
  if (dxvkDevice->features().extDepthBiasControl.depthBiasControl) {
    if (dxvkDevice->features().extDepthBiasControl.depthBiasExact)
      m_depthBiasRepresentation.depthBiasExact = true;

    if (dxvkDevice->features().extDepthBiasControl.floatRepresentation) {
      m_depthBiasRepresentation.depthBiasRepresentation =
          VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT;
      m_depthBiasScale = 1.0f;
    } else if (dxvkDevice->features()
                   .extDepthBiasControl
                   .leastRepresentableValueForceUnormRepresentation)
      m_depthBiasRepresentation.depthBiasRepresentation =
          VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
  }

  EmitCs([cRepresentation = m_depthBiasRepresentation](DxvkContext *ctx) {
    ctx->setDepthBiasRepresentation(cRepresentation);
  });

  CreateConstantBuffers();

  m_availableMemory = DetermineInitialTextureMemory();

  m_hazardLayout =
      dxvkDevice->features()
              .extAttachmentFeedbackLoopLayout.attachmentFeedbackLoopLayout
          ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
          : VK_IMAGE_LAYOUT_GENERAL;

  // Initially set all the dirty flags so we
  // always end up giving the backend *something* to work with.
  m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  m_dirty.set(D3D9DeviceDirtyFlag::ClipPlanes);
  m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
  m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
  m_dirty.set(D3D9DeviceDirtyFlag::RasterizerState);
  m_dirty.set(D3D9DeviceDirtyFlag::DepthBias);
  m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
  m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);
  m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
  m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);

  m_dirty.set(D3D9DeviceDirtyFlag::FogState);
  m_dirty.set(D3D9DeviceDirtyFlag::FogColor);
  m_dirty.set(D3D9DeviceDirtyFlag::FogDensity);
  m_dirty.set(D3D9DeviceDirtyFlag::FogScale);
  m_dirty.set(D3D9DeviceDirtyFlag::FogEnd);

  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
  m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
  m_dirty.set(D3D9DeviceDirtyFlag::FFViewport);
  m_dirty.set(D3D9DeviceDirtyFlag::FFPixelData);
  m_dirty.set(D3D9DeviceDirtyFlag::SharedPixelShaderData);
  m_dirty.set(D3D9DeviceDirtyFlag::DepthBounds);
  m_dirty.set(D3D9DeviceDirtyFlag::PointScale);

  m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);

  m_specInfo.set<SpecDrefScaling, uint32_t>(m_d3d9Options.drefScaling);

  BindFFUbershader<DxsoProgramType::VertexShader>();
  BindFFUbershader<DxsoProgramType::PixelShader>();

  war3::ShaderManager::get().initialize(this);
  m_unlockAdditionalFormats = m_parent->HasFormatsUnlocked();
}

D3D9DeviceEx::~D3D9DeviceEx() {
  // [War3 Perf] Export HTML report before shutdown
  war3::War3PerfMonitor::instance().exportHtmlReport("war3_perf_report.html");
  war3::War3PerfMonitor::instance().shutdown();
  // Avoids hanging when in this state, see comment
  // in DxvkDevice::~DxvkDevice.
  if (this_thread::isInModuleDetachment())
    return;

  Flush();
  SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

  if (m_annotation)
    delete m_annotation;

  delete m_initializer;
  delete m_converter;
  delete m_war3Pipeline;
  delete m_war3PostProcess;
  m_shadowReceiverPass = nullptr;
  m_ssaoPass = nullptr;
  m_aaPass = nullptr;
  m_war3PostProcess = nullptr;
  if (war3::GetActiveDevice() == this)
    war3::SetActiveDevice(nullptr);

  m_dxvkDevice->waitForIdle(); // Sync Device
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::QueryInterface(REFIID riid,
                                                       void **ppvObject) {
  if (ppvObject == nullptr)
    return E_POINTER;

  *ppvObject = nullptr;

  bool extended =
      m_parent->IsExtended() && riid == __uuidof(IDirect3DDevice9Ex);

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9) ||
      extended) {
    *ppvObject = ref(this);
    return S_OK;
  }

  if (riid == __uuidof(IDxvkD3D8Bridge)) {
    *ppvObject = ref(&m_d3d8Bridge);
    return S_OK;
  }

  if (riid == __uuidof(ID3D9VkInteropDevice)) {
    *ppvObject = ref(&m_d3d9Interop);
    return S_OK;
  }

  if (riid == __uuidof(IDirect3DDevice9On12)) {
    if (m_d3d9On12Args.Enable9On12) {
      *ppvObject = ref(&m_d3d9On12);
      return S_OK;
    } else if (logQueryInterfaceError(__uuidof(IDirect3DDevice9), riid)) {
      Logger::warn("D3D9DeviceEx::QueryInterface: IDirect3DDevice9On12 "
                   "queried, but 9On12 not enabled for device");
      return E_NOINTERFACE;
    }
  }

  // We want to ignore this if the extended device is queried and we weren't
  // made extended.
  if (riid == __uuidof(IDirect3DDevice9Ex))
    return E_NOINTERFACE;

  if (logQueryInterfaceError(__uuidof(IDirect3DDevice9), riid)) {
    Logger::warn("D3D9DeviceEx::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
  }

  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::TestCooperativeLevel() {
  D3D9DeviceLock lock = LockDevice();

  // Equivelant of D3D11/DXGI present tests. We can always present.
  if (likely(m_deviceLostState == D3D9DeviceLostState::Ok)) {
    return D3D_OK;
  } else if (m_deviceLostState == D3D9DeviceLostState::NotReset) {
    return D3DERR_DEVICENOTRESET;
  } else {
    return D3DERR_DEVICELOST;
  }
}

UINT STDMETHODCALLTYPE D3D9DeviceEx::GetAvailableTextureMem() {
  // This is not meant to be accurate.
  // The values are also wildly incorrect in d3d9... But some games rely
  // on this inaccurate value...

  // Clamp to megabyte range, as per spec.
  constexpr UINT range = 0xfff00000;

  // Can't have negative memory!
  // Ensure the maximum is returned if available memory overflows the u32
  int64_t memory = std::min(std::max<int64_t>(m_availableMemory.load(), 0),
                            static_cast<int64_t>(range));

  return UINT(memory) & range;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::EvictManagedResources() {
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDirect3D(IDirect3D9 **ppD3D9) {
  if (ppD3D9 == nullptr)
    return D3DERR_INVALIDCALL;

  *ppD3D9 = m_parent.ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDeviceCaps(D3DCAPS9 *pCaps) {
  if (pCaps == nullptr)
    return D3DERR_INVALIDCALL;

  m_adapter->GetDeviceCaps(m_deviceType, pCaps);

  // When in SWVP mode, 256 matrices can be used for indexed vertex blending
  pCaps->MaxVertexBlendMatrixIndex = m_isSWVP ? 255 : 8;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDisplayMode(UINT iSwapChain,
                                                       D3DDISPLAYMODE *pMode) {
  if (unlikely(iSwapChain != 0))
    return D3DERR_INVALIDCALL;

  return m_implicitSwapchain->GetDisplayMode(pMode);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetCreationParameters(
    D3DDEVICE_CREATION_PARAMETERS *pParameters) {
  if (pParameters == nullptr)
    return D3DERR_INVALIDCALL;

  pParameters->AdapterOrdinal = m_adapter->GetOrdinal();
  pParameters->BehaviorFlags = m_behaviorFlags;
  pParameters->DeviceType = m_deviceType;
  pParameters->hFocusWindow = m_window;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetCursorProperties(
    UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pCursorBitmap == nullptr))
    return D3DERR_INVALIDCALL;

  Logger::info(
      str::format("SetCursorProperties called: ", XHotSpot, "x", YHotSpot));

  auto *cursorTex = GetCommonTexture(pCursorBitmap);
  if (unlikely(cursorTex->Desc()->Format != D3D9Format::A8R8G8B8))
    return D3DERR_INVALIDCALL;

  const uint32_t inputWidth = cursorTex->Desc()->Width;
  const uint32_t inputHeight = cursorTex->Desc()->Height;

  // Check if surface dimensions are powers of two.
  if (unlikely((inputWidth && (inputWidth & (inputWidth - 1))) ||
               (inputHeight && (inputHeight & (inputHeight - 1)))))
    return D3DERR_INVALIDCALL;

  // It makes no sense to have a hotspot outside of the bitmap.
  if (unlikely((inputWidth && (XHotSpot > inputWidth - 1)) ||
               (inputHeight && (YHotSpot > inputHeight - 1))))
    return D3DERR_INVALIDCALL;

  // For some reason the cursor bitmap size validation is done
  // against the display mode dimensions (which makes for an
  // interesting situation on windowed swapchains...)
  D3DDISPLAYMODEEX mode = {};
  mode.Size = sizeof(D3DDISPLAYMODEEX);
  m_adapter->GetAdapterDisplayModeEx(&mode, nullptr);

  if (unlikely(inputWidth > mode.Width || inputHeight > mode.Height))
    return D3DERR_INVALIDCALL;

  D3DPRESENT_PARAMETERS params;
  m_implicitSwapchain->GetPresentParameters(&params);

  // Use a hardware cursor if w/h == 32 px or when windowed.
  const bool hwCursor = (inputWidth == HardwareCursorWidth &&
                         inputHeight == HardwareCursorHeight) ||
                        params.Windowed;

  D3DLOCKED_BOX lockedBox;
  HRESULT hr =
      LockImage(cursorTex, 0, 0, &lockedBox, nullptr, D3DLOCK_READONLY);
  if (unlikely(FAILED(hr)))
    return hr;

  const uint8_t *data = reinterpret_cast<const uint8_t *>(lockedBox.pBits);

  // [War3] Capture cursor for ImGui overlay (preserve high resolution)
  if (war3::War3Imgui::get().isInitialized()) {
    std::vector<uint8_t> packed(inputWidth * inputHeight * 4);
    for (uint32_t h = 0; h < inputHeight; h++) {
      std::memcpy(&packed[h * inputWidth * 4], &data[h * lockedBox.RowPitch],
                  inputWidth * 4);
    }
    war3::War3Imgui::get().setCursorBitmap(inputWidth, inputHeight,
                                           packed.data(), XHotSpot, YHotSpot);
  }

  if (hwCursor) {
    CursorBitmap bitmap = {0};
    // We need to consider applications that might misbehave in
    // windowed mode, setting a cursor smaller or larger than 32 x 32 px.
    const size_t copyPitch = std::min<size_t>(
        HardwareCursorPitch, inputWidth * HardwareCursorFormatSize);
    const size_t copyHeight =
        std::min<size_t>(HardwareCursorHeight, inputHeight);

    // Windows works with a stride of 128, let's respect that.
    for (uint32_t h = 0; h < copyHeight; h++)
      std::memcpy(&bitmap[h * HardwareCursorPitch],
                  &data[h * lockedBox.RowPitch], copyPitch);

    hr = UnlockImage(cursorTex, 0, 0);
    if (unlikely(FAILED(hr)))
      return hr;

    m_cursor.SetHardwareCursor(XHotSpot, YHotSpot, bitmap);
  } else {
    const size_t copyPitch = inputWidth * HardwareCursorFormatSize;
    std::vector<uint8_t> bitmap(inputHeight * copyPitch, 0);

    for (uint32_t h = 0; h < inputHeight; h++)
      std::memcpy(&bitmap[h * copyPitch], &data[h * lockedBox.RowPitch],
                  copyPitch);

    hr = UnlockImage(cursorTex, 0, 0);
    if (unlikely(FAILED(hr)))
      return hr;

    m_implicitSwapchain->SetCursorTexture(inputWidth, inputHeight, &bitmap[0]);

    m_cursor.SetSoftwareCursor(XHotSpot, YHotSpot, inputWidth, inputHeight);
  }

  return D3D_OK;
}

void STDMETHODCALLTYPE D3D9DeviceEx::SetCursorPosition(int X, int Y,
                                                       DWORD Flags) {
  D3D9DeviceLock lock = LockDevice();

  // I was not able to find an instance
  // where the cursor update was not immediate.

  // Fullscreen + Windowed seem to have the same
  // behaviour here.

  // Hence we ignore the flag D3DCURSOR_IMMEDIATE_UPDATE.

  m_cursor.UpdateCursor(X, Y);
}

BOOL STDMETHODCALLTYPE D3D9DeviceEx::ShowCursor(BOOL bShow) {
  D3D9DeviceLock lock = LockDevice();

  return m_cursor.ShowCursor(bShow);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateAdditionalSwapChain(
    D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DSwapChain9 **ppSwapChain) {
  return CreateAdditionalSwapChainEx(pPresentationParameters, nullptr,
                                     ppSwapChain);
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) {
  D3D9DeviceLock lock = LockDevice();

  InitReturnPtr(pSwapChain);

  if (unlikely(pSwapChain == nullptr))
    return D3DERR_INVALIDCALL;

  // This only returns the implicit swapchain...

  if (unlikely(iSwapChain != 0))
    return D3DERR_INVALIDCALL;

  *pSwapChain = static_cast<IDirect3DSwapChain9 *>(m_implicitSwapchain.ref());

  return D3D_OK;
}

UINT STDMETHODCALLTYPE D3D9DeviceEx::GetNumberOfSwapChains() {
  // This only counts the implicit swapchain...

  return 1;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  D3D9DeviceLock lock = LockDevice();

  // [War3] FPS 解锁时强制禁用 VSync
  War3ForceImmediatePresent(pPresentationParameters);

  // ImGui Invalidate
  ImGui_ImplDX9_InvalidateDeviceObjects();

  // [War3] Release PostFX resources (Default Pool) to allow Reset to succeed
  if (m_war3PostProcess)
    m_war3PostProcess->Shutdown();

  // [War3] Release Instance Buffer resources
  dxvk::war3::reimpl::War3InstanceBuffer::Get(this)->OnLostDevice();

  Logger::info("Device reset");
  m_deviceLostState = D3D9DeviceLostState::Ok;

  HRESULT hr;
  // Black Desert creates a D3DDEVTYPE_NULLREF device and
  // expects reset to work despite passing invalid parameters.
  if (likely(m_deviceType != D3DDEVTYPE_NULLREF)) {
    hr = m_parent->ValidatePresentationParameters(pPresentationParameters);

    if (unlikely(FAILED(hr)))
      return hr;
  }

  if (!IsExtended()) {
    // The internal references are always cleared, regardless of whether the
    // Reset call succeeds.
    ResetState(pPresentationParameters);
    m_implicitSwapchain->DestroyBackBuffers();
    m_autoDepthStencil = nullptr;

    // Unbind all buffers that were still bound to the backend to avoid leaks.
    EmitCs([](DxvkContext *ctx) {
      ctx->bindIndexBuffer(DxvkBufferSlice(), VK_INDEX_TYPE_UINT32);
      for (uint32_t i = 0; i < caps::MaxStreams; i++) {
        ctx->bindVertexBuffer(i, DxvkBufferSlice(), 0);
      }
    });

    // Tests show that regular D3D9 ends the scene in Reset
    // while D3D9Ex doesn't.
    // Observed in Empires: Dawn of the Modern World (D3D8)
    // and the OSU compatibility mode (D3D9Ex).
    m_inScene = false;
  } else {
    // Extended devices will not reset the MinZ/MaxZ viewport values
    const float MinZ = m_state.viewport.MinZ;
    const float MaxZ = m_state.viewport.MaxZ;

    // Extended devices only reset the bound render targets
    for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++) {
      SetRenderTargetInternal(i, nullptr);
    }

    // Previous MinZ/MaxZ values (saved above) need to be restored
    m_state.viewport.MinZ = MinZ;
    m_state.viewport.MaxZ = MaxZ;

    SetDepthStencilSurface(nullptr);
  }

  m_cursor.ResetCursor();

  /*
   * Before calling the IDirect3DDevice9::Reset method for a device,
   * an application should release any explicit render targets,
   * depth stencil surfaces, additional swap chains, state blocks,
   * and D3DPOOL_DEFAULT resources associated with the device.
   *
   * We have to check after ResetState clears the references held by SetTexture,
   * etc. This matches what Windows D3D9 does.
   */
  if (unlikely(m_losableResourceCounter.load() != 0 && !IsExtended() &&
               m_d3d9Options.countLosableResources)) {
    Logger::warn(str::format(
        "Device reset failed because device still has alive losable resources: "
        "Device not reset. Remaining resources: ",
        m_losableResourceCounter.load()));
    m_deviceLostState = D3D9DeviceLostState::NotReset;
    // D3D8 returns D3DERR_DEVICELOST here, whereas D3D9 returns
    // D3DERR_INVALIDCALL.
    return m_isD3D8Compatible ? D3DERR_DEVICELOST : D3DERR_INVALIDCALL;
  }

  hr = ResetSwapChain(pPresentationParameters, nullptr);
  if (unlikely(FAILED(hr))) {
    if (!IsExtended()) {
      Logger::warn("Device reset failed: Device not reset");
      m_deviceLostState = D3D9DeviceLostState::NotReset;
    }
    return hr;
  }

  Flush();
  SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

  if (m_d3d9Options.deferSurfaceCreation)
    m_resetCtr++;

  // ImGui Restore
  ImGui_ImplDX9_CreateDeviceObjects();

  // [War3] Restore Instance Buffer
  dxvk::war3::reimpl::War3InstanceBuffer::Get(this)->OnResetDevice();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Present(const RECT *pSourceRect,
                                                const RECT *pDestRect,
                                                HWND hDestWindowOverride,
                                                const RGNDATA *pDirtyRegion) {
  War3ResetShadowAllocator();

  // War3: 增加帧计数
  dxvk::war3::state::RenderState::instance().beginFrame();

  return PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion,
                   0);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetBackBuffer(
    UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
    IDirect3DSurface9 **ppBackBuffer) {
  InitReturnPtr(ppBackBuffer);

  if (unlikely(iSwapChain != 0))
    return D3DERR_INVALIDCALL;

  return m_implicitSwapchain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRasterStatus(
    UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) {
  if (unlikely(iSwapChain != 0))
    return D3DERR_INVALIDCALL;

  return m_implicitSwapchain->GetRasterStatus(pRasterStatus);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetDialogBoxMode(BOOL bEnableDialogs) {
  return m_implicitSwapchain->SetDialogBoxMode(bEnableDialogs);
}

void STDMETHODCALLTYPE D3D9DeviceEx::SetGammaRamp(UINT iSwapChain, DWORD Flags,
                                                  const D3DGAMMARAMP *pRamp) {
  if (unlikely(iSwapChain != 0))
    return;

  m_implicitSwapchain->SetGammaRamp(Flags, pRamp);
}

void STDMETHODCALLTYPE D3D9DeviceEx::GetGammaRamp(UINT iSwapChain,
                                                  D3DGAMMARAMP *pRamp) {
  if (unlikely(iSwapChain != 0))
    return;

  m_implicitSwapchain->GetGammaRamp(pRamp);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
    D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) {
  InitReturnPtr(ppTexture);

  if (unlikely(ppTexture == nullptr))
    return D3DERR_INVALIDCALL;

  D3D9_COMMON_TEXTURE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Depth = 1;
  desc.ArraySize = 1;
  desc.MipLevels = Levels;
  desc.Usage = Usage;
  desc.Format = EnumerateFormat(Format);
  desc.Pool = Pool;
  desc.Discard = FALSE;
  desc.MultiSample = D3DMULTISAMPLE_NONE;
  desc.MultisampleQuality = 0;
  desc.IsBackBuffer = FALSE;
  desc.IsAttachmentOnly = FALSE;
  // Docs:
  // Textures placed in the D3DPOOL_DEFAULT pool cannot be locked
  // unless they are dynamic textures or they are private, FOURCC, driver
  // formats.
  desc.IsLockable = Pool != D3DPOOL_DEFAULT || (Usage & D3DUSAGE_DYNAMIC) ||
                    IsVendorFormat(EnumerateFormat(Format));

  if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(
          this, D3DRTYPE_TEXTURE, &desc)))
    return D3DERR_INVALIDCALL;

  try {
    void *initialData = nullptr;

    // On Windows Vista (so most likely D3D9Ex), pSharedHandle can be used to
    // pass initial data for a texture, but only for a very specific type of
    // texture.
    if (unlikely(pSharedHandle != nullptr && Pool == D3DPOOL_SYSTEMMEM &&
                 Levels == 1)) {
      initialData = *(reinterpret_cast<void **>(pSharedHandle));
      pSharedHandle = nullptr;
    }

    // Shared textures have to be in POOL_DEFAULT
    if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    // Shared resource handle has to be a D3DKMT global handle */
    if (unlikely(
            pSharedHandle != nullptr && *pSharedHandle != nullptr &&
            !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_TEXTURE, desc)))
      return E_INVALIDARG;

    const Com<D3D9Texture2D> texture =
        new D3D9Texture2D(this, &desc, IsExtended(), pSharedHandle);

    m_initializer->InitTexture(texture->GetCommonTexture(), initialData);
    *ppTexture = texture.ref();

    if (desc.Pool == D3DPOOL_DEFAULT)
      m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_OUTOFVIDEOMEMORY;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVolumeTexture(
    UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture,
    HANDLE *pSharedHandle) {
  InitReturnPtr(ppVolumeTexture);

  if (unlikely(ppVolumeTexture == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle))
    Logger::err("CreateVolumeTexture: Shared volume textures not supported");

  D3D9_COMMON_TEXTURE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Depth = Depth;
  desc.ArraySize = 1;
  desc.MipLevels = Levels;
  desc.Usage = Usage;
  desc.Format = EnumerateFormat(Format);
  desc.Pool = Pool;
  desc.Discard = FALSE;
  desc.MultiSample = D3DMULTISAMPLE_NONE;
  desc.MultisampleQuality = 0;
  desc.IsBackBuffer = FALSE;
  desc.IsAttachmentOnly = FALSE;
  // Docs:
  // Textures placed in the D3DPOOL_DEFAULT pool cannot be locked
  // unless they are dynamic textures. Volume textures do not
  // exempt private, FOURCC, driver formats from these checks.
  desc.IsLockable = Pool != D3DPOOL_DEFAULT || (Usage & D3DUSAGE_DYNAMIC);

  if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(
          this, D3DRTYPE_VOLUMETEXTURE, &desc)))
    return D3DERR_INVALIDCALL;

  if (unlikely(
          pSharedHandle != nullptr && *pSharedHandle != nullptr &&
          !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_VOLUMETEXTURE, desc)))
    return E_INVALIDARG;

  try {
    const Com<D3D9Texture3D> texture =
        new D3D9Texture3D(this, &desc, IsExtended());
    m_initializer->InitTexture(texture->GetCommonTexture());
    *ppVolumeTexture = texture.ref();

    // The device cannot be reset if there's any remaining default resources
    if (desc.Pool == D3DPOOL_DEFAULT)
      m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_OUTOFVIDEOMEMORY;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) {
  InitReturnPtr(ppCubeTexture);

  if (unlikely(ppCubeTexture == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle))
    Logger::err("CreateCubeTexture: Shared cube textures not supported");

  D3D9_COMMON_TEXTURE_DESC desc;
  desc.Width = EdgeLength;
  desc.Height = EdgeLength;
  desc.Depth = 1;
  desc.ArraySize = 6; // A cube has 6 faces, wowwie!
  desc.MipLevels = Levels;
  desc.Usage = Usage;
  desc.Format = EnumerateFormat(Format);
  desc.Pool = Pool;
  desc.Discard = FALSE;
  desc.MultiSample = D3DMULTISAMPLE_NONE;
  desc.MultisampleQuality = 0;
  desc.IsBackBuffer = FALSE;
  desc.IsAttachmentOnly = FALSE;
  // Docs:
  // Textures placed in the D3DPOOL_DEFAULT pool cannot be locked
  // unless they are dynamic textures or they are private, FOURCC, driver
  // formats.
  desc.IsLockable = Pool != D3DPOOL_DEFAULT || (Usage & D3DUSAGE_DYNAMIC) ||
                    IsVendorFormat(EnumerateFormat(Format));

  if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(
          this, D3DRTYPE_CUBETEXTURE, &desc)))
    return D3DERR_INVALIDCALL;

  if (unlikely(
          pSharedHandle != nullptr && *pSharedHandle != nullptr &&
          !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_CUBETEXTURE, desc)))
    return E_INVALIDARG;

  try {
    const Com<D3D9TextureCube> texture =
        new D3D9TextureCube(this, &desc, IsExtended());
    m_initializer->InitTexture(texture->GetCommonTexture());
    *ppCubeTexture = texture.ref();

    // The device cannot be reset if there's any remaining default resources
    if (desc.Pool == D3DPOOL_DEFAULT)
      m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_OUTOFVIDEOMEMORY;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) {
  InitReturnPtr(ppVertexBuffer);

  if (unlikely(ppVertexBuffer == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
    return D3DERR_NOTAVAILABLE;

  if (unlikely(pSharedHandle))
    Logger::err("CreateVertexBuffer: Shared vertex buffers not supported");

  D3D9_BUFFER_DESC desc;
  desc.Format = D3D9Format::VERTEXDATA;
  desc.FVF = FVF;
  desc.Pool = Pool;
  desc.Size = Length;
  desc.Type = D3DRTYPE_VERTEXBUFFER;
  desc.Usage = Usage;

  if (FAILED(D3D9CommonBuffer::ValidateBufferProperties(&desc, IsExtended())))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
               !ValidateSharedBuffer(*pSharedHandle, desc)))
    return E_INVALIDARG;

  try {
    const Com<D3D9VertexBuffer> buffer =
        new D3D9VertexBuffer(this, &desc, IsExtended());
    m_initializer->InitBuffer(buffer->GetCommonBuffer());
    *ppVertexBuffer = buffer.ref();

    // The device cannot be reset if there's any remaining default resources
    if (desc.Pool == D3DPOOL_DEFAULT)
      m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_INVALIDCALL;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) {
  InitReturnPtr(ppIndexBuffer);

  if (unlikely(ppIndexBuffer == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
    return D3DERR_NOTAVAILABLE;

  if (unlikely(pSharedHandle))
    Logger::err("CreateIndexBuffer: Shared index buffers not supported");

  D3D9_BUFFER_DESC desc;
  desc.Format = EnumerateFormat(Format);
  desc.Pool = Pool;
  desc.Size = Length;
  desc.Type = D3DRTYPE_INDEXBUFFER;
  desc.Usage = Usage;

  if (FAILED(D3D9CommonBuffer::ValidateBufferProperties(&desc, IsExtended())))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
               !ValidateSharedBuffer(*pSharedHandle, desc)))
    return E_INVALIDARG;

  try {
    const Com<D3D9IndexBuffer> buffer =
        new D3D9IndexBuffer(this, &desc, IsExtended());
    m_initializer->InitBuffer(buffer->GetCommonBuffer());
    *ppIndexBuffer = buffer.ref();

    // The device cannot be reset if there's any remaining default resources
    if (desc.Pool == D3DPOOL_DEFAULT)
      m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_INVALIDCALL;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface,
    HANDLE *pSharedHandle) {
  return CreateRenderTargetEx(Width, Height, Format, MultiSample,
                              MultisampleQuality, Lockable, ppSurface,
                              pSharedHandle, 0);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface,
    HANDLE *pSharedHandle) {
  return CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample,
                                     MultisampleQuality, Discard, ppSurface,
                                     pSharedHandle, 0);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::UpdateSurface(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
    IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint) {
  D3D9DeviceLock lock = LockDevice();

  D3D9Surface *src = static_cast<D3D9Surface *>(pSourceSurface);
  D3D9Surface *dst = static_cast<D3D9Surface *>(pDestinationSurface);

  if (unlikely(src == nullptr || dst == nullptr))
    return D3DERR_INVALIDCALL;

  D3D9CommonTexture *srcTextureInfo = src->GetCommonTexture();
  D3D9CommonTexture *dstTextureInfo = dst->GetCommonTexture();

  if (unlikely(srcTextureInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM ||
               dstTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT))
    return D3DERR_INVALIDCALL;

  if (unlikely(srcTextureInfo->Desc()->Format !=
               dstTextureInfo->Desc()->Format))
    return D3DERR_INVALIDCALL;

  if (unlikely(srcTextureInfo->Desc()->MultiSample != D3DMULTISAMPLE_NONE))
    return D3DERR_INVALIDCALL;

  if (unlikely(dstTextureInfo->Desc()->MultiSample != D3DMULTISAMPLE_NONE))
    return D3DERR_INVALIDCALL;

  const DxvkFormatInfo *formatInfo =
      lookupFormatInfo(dstTextureInfo->GetFormatMapping().FormatColor);

  VkOffset3D srcOffset = {0u, 0u, 0u};
  VkOffset3D dstOffset = {0u, 0u, 0u};
  VkExtent3D texLevelExtent =
      srcTextureInfo->GetExtentMip(src->GetSubresource());
  VkExtent3D extent = texLevelExtent;

  if (pSourceRect != nullptr) {
    srcOffset = {pSourceRect->left, pSourceRect->top, 0u};

    extent = {uint32_t(pSourceRect->right - pSourceRect->left),
              uint32_t(pSourceRect->bottom - pSourceRect->top), 1};

    const bool extentAligned =
        extent.width % formatInfo->blockSize.width == 0 &&
        extent.height % formatInfo->blockSize.height == 0;

    if (pSourceRect->left < 0 || pSourceRect->top < 0 ||
        pSourceRect->right <= pSourceRect->left ||
        pSourceRect->bottom <= pSourceRect->top ||
        pSourceRect->left % formatInfo->blockSize.width != 0 ||
        pSourceRect->top % formatInfo->blockSize.height != 0 ||
        (extent != texLevelExtent && !extentAligned))
      return D3DERR_INVALIDCALL;
  }

  if (pDestPoint != nullptr) {
    if (pDestPoint->x % formatInfo->blockSize.width != 0 ||
        pDestPoint->y % formatInfo->blockSize.height != 0 ||
        pDestPoint->x < 0 || pDestPoint->y < 0)
      return D3DERR_INVALIDCALL;

    dstOffset = {pDestPoint->x, pDestPoint->y, 0u};
  }

  // The source surface must be in D3DPOOL_SYSTEMMEM so we just treat it as just
  // another texture upload except with a different source.
  UpdateTextureFromBuffer(dstTextureInfo, srcTextureInfo, dst->GetSubresource(),
                          src->GetSubresource(), srcOffset, extent, dstOffset);

  // The contents of the mapping no longer match the image.
  dstTextureInfo->SetNeedsReadback(dst->GetSubresource(), true);

  if (dstTextureInfo->IsAutomaticMip())
    MarkTextureMipsDirty(dstTextureInfo);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture,
                            IDirect3DBaseTexture9 *pDestinationTexture) {
  D3D9DeviceLock lock = LockDevice();

  if (!pDestinationTexture || !pSourceTexture)
    return D3DERR_INVALIDCALL;

  D3D9CommonTexture *dstTexInfo = GetCommonTexture(pDestinationTexture);
  D3D9CommonTexture *srcTexInfo = GetCommonTexture(pSourceTexture);

  if (unlikely(srcTexInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM ||
               dstTexInfo->Desc()->Pool != D3DPOOL_DEFAULT))
    return D3DERR_INVALIDCALL;

  if (unlikely(srcTexInfo->Desc()->MipLevels < dstTexInfo->Desc()->MipLevels &&
               !dstTexInfo->IsAutomaticMip()))
    return D3DERR_INVALIDCALL;

  if (unlikely(dstTexInfo->Desc()->Format != srcTexInfo->Desc()->Format))
    return D3DERR_INVALIDCALL;

  if (unlikely(srcTexInfo->IsAutomaticMip() && !dstTexInfo->IsAutomaticMip()))
    return D3DERR_INVALIDCALL;

  const Rc<DxvkImage> dstImage = dstTexInfo->GetImage();
  uint32_t srcMipLevels =
      srcTexInfo->IsAutomaticMip() ? 1 : srcTexInfo->Desc()->MipLevels;
  uint32_t dstMipLevels =
      dstTexInfo->IsAutomaticMip() ? 1 : dstTexInfo->Desc()->MipLevels;
  uint32_t arraySlices =
      std::min(srcTexInfo->Desc()->ArraySize, dstTexInfo->Desc()->ArraySize);

  uint32_t srcMipOffset = 0;
  VkExtent3D srcFirstMipExtent = srcTexInfo->GetExtent();
  VkExtent3D dstFirstMipExtent = dstTexInfo->GetExtent();

  if (srcMipLevels > 1 || dstMipLevels > 1) {
    // UpdateTexture does not validate dimensions for textures with only one mip
    if (srcFirstMipExtent != dstFirstMipExtent) {
      // UpdateTexture can be used with textures that have different mip
      // lengths. It will either match the the top mips or the bottom ones. If
      // the largest mip maps don't match in size, we try to take the smallest
      // ones of the source.

      srcMipOffset = srcTexInfo->Desc()->MipLevels - dstMipLevels;
      srcFirstMipExtent =
          util::computeMipLevelExtent(srcTexInfo->GetExtent(), srcMipOffset);
      dstFirstMipExtent = dstTexInfo->GetExtent();
    }

    if (srcFirstMipExtent != dstFirstMipExtent)
      return D3DERR_INVALIDCALL;
  } else {
    if (unlikely(srcFirstMipExtent.width > dstFirstMipExtent.width ||
                 srcFirstMipExtent.height > dstFirstMipExtent.height ||
                 srcFirstMipExtent.depth > dstFirstMipExtent.depth))
      Logger::warn("D3D9DeviceEx::UpdateTexture: Source dimensions exceed the "
                   "destination");
  }

  for (uint32_t a = 0; a < arraySlices; a++) {
    // The docs claim that the dirty box is just a performance optimization,
    // however in practice games rely on it.
    const D3DBOX &box = srcTexInfo->GetDirtyBox(a);
    if (box.Left >= box.Right || box.Top >= box.Bottom || box.Front >= box.Back)
      continue;

    // The dirty box is only tracked for mip level 0
    VkExtent3D mip0Extent = {uint32_t(box.Right - box.Left),
                             uint32_t(box.Bottom - box.Top),
                             uint32_t(box.Back - box.Front)};
    VkOffset3D mip0Offset = {int32_t(box.Left), int32_t(box.Top),
                             int32_t(box.Front)};

    for (uint32_t dstMip = 0; dstMip < dstMipLevels; dstMip++) {
      // Scale the dirty box for the respective mip level
      uint32_t srcMip = dstMip + srcMipOffset;
      uint32_t srcSubresource = srcTexInfo->CalcSubresource(a, srcMip);
      uint32_t dstSubresource = dstTexInfo->CalcSubresource(a, dstMip);
      VkExtent3D extent = util::computeMipLevelExtent(mip0Extent, srcMip);
      VkOffset3D offset = util::computeMipLevelOffset(mip0Offset, srcMip);

      // The source surface must be in D3DPOOL_SYSTEMMEM so we just treat it as
      // just another texture upload except with a different source.
      UpdateTextureFromBuffer(dstTexInfo, srcTexInfo, dstSubresource,
                              srcSubresource, offset, extent, offset);

      // The contents of the mapping no longer match the image.
      dstTexInfo->SetNeedsReadback(dstSubresource, true);
    }
  }

  srcTexInfo->ClearDirtyBoxes();
  if (dstTexInfo->IsAutomaticMip() &&
      dstMipLevels != dstTexInfo->Desc()->MipLevels)
    MarkTextureMipsDirty(dstTexInfo);

  ConsiderFlush(GpuFlushType::ImplicitWeakHint);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderTargetData(
    IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(IsDeviceLost())) {
    return D3DERR_DEVICELOST;
  }

  D3D9Surface *src = static_cast<D3D9Surface *>(pRenderTarget);
  D3D9Surface *dst = static_cast<D3D9Surface *>(pDestSurface);

  if (unlikely(src == nullptr || dst == nullptr))
    return D3DERR_INVALIDCALL;

  if (pRenderTarget == pDestSurface)
    return D3D_OK;

  D3D9CommonTexture *dstTexInfo = GetCommonTexture(dst);
  D3D9CommonTexture *srcTexInfo = GetCommonTexture(src);

  if (srcTexInfo->Desc()->Format != dstTexInfo->Desc()->Format)
    return D3DERR_INVALIDCALL;

  if (src->GetSurfaceExtent() != dst->GetSurfaceExtent())
    return D3DERR_INVALIDCALL;

  if (dstTexInfo->Desc()->Pool == D3DPOOL_DEFAULT)
    return this->StretchRect(pRenderTarget, nullptr, pDestSurface, nullptr,
                             D3DTEXF_NONE);

  VkExtent3D dstTexExtent = dstTexInfo->GetExtentMip(dst->GetMipLevel());
  VkExtent3D srcTexExtent = srcTexInfo->GetExtentMip(src->GetMipLevel());

  const bool clearDst = dstTexInfo->Desc()->MipLevels > 1 ||
                        dstTexExtent.width > srcTexExtent.width ||
                        dstTexExtent.height > srcTexExtent.height;

  dstTexInfo->CreateBuffer(clearDst);
  DxvkBufferSlice dstBufferSlice =
      dstTexInfo->GetBufferSlice(dst->GetSubresource());
  Rc<DxvkImage> srcImage = srcTexInfo->GetImage();
  const DxvkFormatInfo *srcFormatInfo =
      lookupFormatInfo(srcImage->info().format);

  const VkImageSubresource srcSubresource = srcTexInfo->GetSubresourceFromIndex(
      srcFormatInfo->aspectMask, src->GetSubresource());
  VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask, srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1};

  EmitCs([cBufferSlice = std::move(dstBufferSlice), cImage = srcImage,
          cSubresources = srcSubresourceLayers,
          cLevelExtent = srcTexExtent](DxvkContext *ctx) {
    ctx->copyImageToBuffer(cBufferSlice.buffer(), cBufferSlice.offset(), 4, 0,
                           VK_FORMAT_UNDEFINED, cImage, cSubresources,
                           VkOffset3D{0, 0, 0}, cLevelExtent);
  });

  dstTexInfo->SetNeedsReadback(dst->GetSubresource(), true);
  TrackTextureMappingBufferSequenceNumber(dstTexInfo, dst->GetSubresource());

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetFrontBufferData(
    UINT iSwapChain, IDirect3DSurface9 *pDestSurface) {
  if (unlikely(iSwapChain != 0))
    return D3DERR_INVALIDCALL;

  D3D9DeviceLock lock = LockDevice();

  // In windowed mode, GetFrontBufferData takes a screenshot of the entire
  // screen. We use the last used swapchain as a workaround. Total War: Medieval
  // 2 relies on this.
  return m_mostRecentlyUsedSwapchain->GetFrontBufferData(pDestSurface);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::StretchRect(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
    IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
    D3DTEXTUREFILTERTYPE Filter) {
  D3D9DeviceLock lock = LockDevice();

  D3D9Surface *dst = static_cast<D3D9Surface *>(pDestSurface);
  D3D9Surface *src = static_cast<D3D9Surface *>(pSourceSurface);

  if (unlikely(src == nullptr || dst == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(src == dst))
    return D3DERR_INVALIDCALL;

  bool fastPath = true;

  D3D9CommonTexture *dstTextureInfo = dst->GetCommonTexture();
  D3D9CommonTexture *srcTextureInfo = src->GetCommonTexture();

  if (unlikely(dstTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT ||
               srcTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT))
    return D3DERR_INVALIDCALL;

  Rc<DxvkImage> dstImage = dstTextureInfo->GetImage();
  Rc<DxvkImage> srcImage = srcTextureInfo->GetImage();

  if (dstImage == nullptr || srcImage == nullptr)
    return D3DERR_INVALIDCALL;

  const DxvkFormatInfo *dstFormatInfo =
      lookupFormatInfo(dstImage->info().format);
  const DxvkFormatInfo *srcFormatInfo =
      lookupFormatInfo(srcImage->info().format);

  const VkImageSubresource dstSubresource =
      dstTextureInfo->GetSubresourceFromIndex(dstFormatInfo->aspectMask,
                                              dst->GetSubresource());
  const VkImageSubresource srcSubresource =
      srcTextureInfo->GetSubresourceFromIndex(srcFormatInfo->aspectMask,
                                              src->GetSubresource());

  if (unlikely(Filter != D3DTEXF_NONE && Filter != D3DTEXF_LINEAR &&
               Filter != D3DTEXF_POINT))
    return D3DERR_INVALIDCALL;

  VkExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);
  VkExtent3D dstExtent = dstImage->mipLevelExtent(dstSubresource.mipLevel);

  D3D9Format srcFormat = srcTextureInfo->Desc()->Format;
  D3D9Format dstFormat = dstTextureInfo->Desc()->Format;

  // We may only fast path copy non identicals one way!
  // We don't know what garbage could be in the X8 data.
  bool similar = AreFormatsSimilar(srcFormat, dstFormat);

  // Copies are only supported on similar formats.
  fastPath &= similar;

  // Copies are only supported if the sample count matches,
  // otherwise we need to resolve.
  auto needsResolve = false;
  if (srcImage->info().sampleCount != dstImage->info().sampleCount) {
    needsResolve = srcImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT;
    auto fbBlit = dstImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT;
    fastPath &= !fbBlit;
  }

  // Copies would only work if we are block aligned.
  if (pSourceRect != nullptr) {
    fastPath &= (pSourceRect->left % srcFormatInfo->blockSize.width == 0);
    fastPath &= (pSourceRect->right % srcFormatInfo->blockSize.width == 0);
    fastPath &= (pSourceRect->top % srcFormatInfo->blockSize.height == 0);
    fastPath &= (pSourceRect->bottom % srcFormatInfo->blockSize.height == 0);
  }

  if (pDestRect != nullptr) {
    fastPath &= (pDestRect->left % dstFormatInfo->blockSize.width == 0);
    fastPath &= (pDestRect->top % dstFormatInfo->blockSize.height == 0);
  }

  VkImageSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspectMask, dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1};

  VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask, srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1};

  VkImageBlit blitInfo;
  blitInfo.dstSubresource = dstSubresourceLayers;
  blitInfo.srcSubresource = srcSubresourceLayers;

  blitInfo.dstOffsets[0] =
      pDestRect != nullptr
          ? VkOffset3D{int32_t(pDestRect->left), int32_t(pDestRect->top), 0}
          : VkOffset3D{0, 0, 0};

  blitInfo.dstOffsets[1] =
      pDestRect != nullptr
          ? VkOffset3D{int32_t(pDestRect->right), int32_t(pDestRect->bottom), 1}
          : VkOffset3D{int32_t(dstExtent.width), int32_t(dstExtent.height), 1};

  blitInfo.srcOffsets[0] =
      pSourceRect != nullptr
          ? VkOffset3D{int32_t(pSourceRect->left), int32_t(pSourceRect->top), 0}
          : VkOffset3D{0, 0, 0};

  blitInfo.srcOffsets[1] =
      pSourceRect != nullptr
          ? VkOffset3D{int32_t(pSourceRect->right),
                       int32_t(pSourceRect->bottom), 1}
          : VkOffset3D{int32_t(srcExtent.width), int32_t(srcExtent.height), 1};

  if (unlikely(IsBlitRegionInvalid(blitInfo.srcOffsets, srcExtent)))
    return D3DERR_INVALIDCALL;

  if (unlikely(IsBlitRegionInvalid(blitInfo.dstOffsets, dstExtent)))
    return D3DERR_INVALIDCALL;

  VkExtent3D srcCopyExtent = {
      uint32_t(blitInfo.srcOffsets[1].x - blitInfo.srcOffsets[0].x),
      uint32_t(blitInfo.srcOffsets[1].y - blitInfo.srcOffsets[0].y),
      uint32_t(blitInfo.srcOffsets[1].z - blitInfo.srcOffsets[0].z)};

  VkExtent3D dstCopyExtent = {
      uint32_t(blitInfo.dstOffsets[1].x - blitInfo.dstOffsets[0].x),
      uint32_t(blitInfo.dstOffsets[1].y - blitInfo.dstOffsets[0].y),
      uint32_t(blitInfo.dstOffsets[1].z - blitInfo.dstOffsets[0].z)};

  bool srcIsDS = IsDepthStencilFormat(srcFormat);
  bool dstIsDS = IsDepthStencilFormat(dstFormat);
  if (unlikely(srcIsDS || dstIsDS)) {
    if (unlikely(!srcIsDS || !dstIsDS))
      return D3DERR_INVALIDCALL;

    if (unlikely(srcTextureInfo->Desc()->Discard ||
                 dstTextureInfo->Desc()->Discard))
      return D3DERR_INVALIDCALL;

    if (unlikely(srcCopyExtent.width != srcExtent.width ||
                 srcCopyExtent.height != srcExtent.height))
      return D3DERR_INVALIDCALL;

    if (unlikely(m_inScene))
      return D3DERR_INVALIDCALL;
  }

  // Copies would only work if the extents match. (ie. no stretching)
  bool stretch = srcCopyExtent != dstCopyExtent;

  bool dstHasAttachmentUsage =
      (dstTextureInfo->Desc()->Usage &
       (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
  bool dstIsSurface = dstTextureInfo->GetType() == D3DRTYPE_SURFACE;
  if (stretch) {
    if (unlikely(pSourceSurface == pDestSurface))
      return D3DERR_INVALIDCALL;

    if (unlikely(dstIsDS))
      return D3DERR_INVALIDCALL;

    // The docs say that stretching is only allowed if the destination is either
    // a render target surface or a render target texture. However in practice,
    // using an offscreen plain surface in D3DPOOL_DEFAULT as the destination
    // works fine. Using a texture without USAGE_RENDERTARGET as destination
    // however does not.
    if (unlikely(!dstIsSurface && !dstHasAttachmentUsage))
      return D3DERR_INVALIDCALL;
  } else {
    bool srcIsSurface = srcTextureInfo->GetType() == D3DRTYPE_SURFACE;
    bool srcHasAttachmentUsage =
        (srcTextureInfo->Desc()->Usage &
         (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;

    // D3D9Ex allows StretchRect to regular (non-RT) textures if it is a simple
    // copy.
    bool isCopy =
        IsExtended() && pSourceRect == nullptr &&
        pDestRect ==
            nullptr // Yes, the rects have to be null. Even passing a rect that
                    // is the same size as the texture is invalid.
        && srcTextureInfo->Desc()->Pool == D3DPOOL_DEFAULT &&
        dstTextureInfo->Desc()->Pool == D3DPOOL_DEFAULT &&
        srcTextureInfo->Desc()->Format == dstTextureInfo->Desc()->Format;

    // Non-stretching copies are only allowed if:
    // - the destination is either a render target surface or a render target
    // texture
    // - both destination and source are depth stencil surfaces
    // - both destination and source are offscreen plain surfaces.
    // The only way to get a surface with resource type D3DRTYPE_SURFACE without
    // USAGE_RT or USAGE_DS is CreateOffscreenPlainSurface.
    if (unlikely((!dstHasAttachmentUsage &&
                  (!dstIsSurface || !srcIsSurface || srcHasAttachmentUsage)) &&
                 !m_isD3D8Compatible && !isCopy))
      return D3DERR_INVALIDCALL;
  }

  fastPath &= !stretch;

  if (!fastPath || needsResolve) {
    // Compressed destination formats are forbidden for blits.
    if (dstFormatInfo->flags.test(DxvkFormatFlag::BlockCompressed))
      return D3DERR_INVALIDCALL;
  }

  if (fastPath) {
    if (needsResolve) {
      VkImageResolve region;
      region.srcSubresource = blitInfo.srcSubresource;
      region.srcOffset = blitInfo.srcOffsets[0];
      region.dstSubresource = blitInfo.dstSubresource;
      region.dstOffset = blitInfo.dstOffsets[0];
      region.extent = srcCopyExtent;

      EmitCs([cDstImage = dstImage, cSrcImage = srcImage,
              cRegion = region](DxvkContext *ctx) {
        // Deliberately use AVERAGE even for depth resolves here
        ctx->resolveImage(cDstImage, cSrcImage, cRegion,
                          cSrcImage->info().format, VK_RESOLVE_MODE_AVERAGE_BIT,
                          VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
      });
    } else {
      EmitCs([cDstImage = dstImage, cSrcImage = srcImage,
              cDstLayers = blitInfo.dstSubresource,
              cSrcLayers = blitInfo.srcSubresource,
              cDstOffset = blitInfo.dstOffsets[0],
              cSrcOffset = blitInfo.srcOffsets[0],
              cExtent = srcCopyExtent](DxvkContext *ctx) {
        ctx->copyImage(cDstImage, cDstLayers, cDstOffset, cSrcImage, cSrcLayers,
                       cSrcOffset, cExtent);
      });
    }
  } else {
    DxvkImageViewKey dstViewInfo;
    dstViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dstViewInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dstViewInfo.format = dstImage->info().format;
    dstViewInfo.aspects = blitInfo.dstSubresource.aspectMask;
    dstViewInfo.mipIndex = blitInfo.dstSubresource.mipLevel;
    dstViewInfo.mipCount = 1;
    dstViewInfo.layerIndex = blitInfo.dstSubresource.baseArrayLayer;
    dstViewInfo.layerCount = blitInfo.dstSubresource.layerCount;
    dstViewInfo.packedSwizzle =
        DxvkImageViewKey::packSwizzle(dstTextureInfo->GetMapping().Swizzle);

    DxvkImageViewKey srcViewInfo;
    srcViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    srcViewInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    srcViewInfo.format = srcImage->info().format;
    srcViewInfo.aspects = blitInfo.srcSubresource.aspectMask;
    srcViewInfo.mipIndex = blitInfo.srcSubresource.mipLevel;
    srcViewInfo.mipCount = 1;
    srcViewInfo.layerIndex = blitInfo.srcSubresource.baseArrayLayer;
    srcViewInfo.layerCount = blitInfo.srcSubresource.layerCount;
    srcViewInfo.packedSwizzle =
        DxvkImageViewKey::packSwizzle(srcTextureInfo->GetMapping().Swizzle);

    EmitCs([cDstView = dstImage->createView(dstViewInfo),
            cSrcView = srcImage->createView(srcViewInfo), cBlitInfo = blitInfo,
            cFilter = stretch ? DecodeFilter(Filter)
                              : VK_FILTER_NEAREST](DxvkContext *ctx) {
      ctx->blitImageView(cDstView, cBlitInfo.dstOffsets, cSrcView,
                         cBlitInfo.srcOffsets, cFilter);
    });
  }

  dstTextureInfo->SetNeedsReadback(dst->GetSubresource(), true);

  if (dstTextureInfo->IsAutomaticMip())
    MarkTextureMipsDirty(dstTextureInfo);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ColorFill(IDirect3DSurface9 *pSurface,
                                                  const RECT *pRect,
                                                  D3DCOLOR Color) {
  D3D9DeviceLock lock = LockDevice();

  D3D9Surface *dst = static_cast<D3D9Surface *>(pSurface);

  if (unlikely(dst == nullptr))
    return D3DERR_INVALIDCALL;

  D3D9CommonTexture *dstTextureInfo = dst->GetCommonTexture();

  if (dstTextureInfo->IsNull())
    return D3D_OK;

  if (unlikely(dstTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT))
    return D3DERR_INVALIDCALL;

  VkExtent3D mipExtent = dstTextureInfo->GetExtentMip(dst->GetSubresource());

  VkOffset3D offset = VkOffset3D{0u, 0u, 0u};
  VkExtent3D extent = mipExtent;

  if (pRect != nullptr)
    ConvertRect(*pRect, offset, extent);

  VkClearValue clearValue = {};
  DecodeD3DCOLOR(Color, clearValue.color.float32);

  Rc<DxvkImage> image = dstTextureInfo->GetImage();

  if (image->formatInfo()->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
    return D3DERR_INVALIDCALL;

  VkImageSubresourceLayers subresource = {};
  subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  subresource.mipLevel = dst->GetMipLevel();

  if (dst->GetFace() == D3D9CommonTexture::AllLayers) {
    subresource.baseArrayLayer = 0u;
    subresource.layerCount = image->info().numLayers;
  } else {
    subresource.baseArrayLayer = dst->GetFace();
    subresource.layerCount = 1u;
  }

  if (image->formatInfo()->flags.test(DxvkFormatFlag::BlockCompressed)) {
    EmitCs([cImage = std::move(image), cSubresource = subresource,
            cOffset = offset, cExtent = extent,
            cClearValue = clearValue](DxvkContext *ctx) {
      auto formatInfo = cImage->formatInfo();

      VkFormat blockFormat = formatInfo->elementSize == 16u
                                 ? VK_FORMAT_R32G32B32A32_UINT
                                 : VK_FORMAT_R32G32_UINT;

      DxvkImageUsageInfo usage = {};
      usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
      usage.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                    VK_IMAGE_CREATE_EXTENDED_USAGE_BIT |
                    VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
      usage.viewFormatCount = 1;
      usage.viewFormats = &blockFormat;
      usage.layout = VK_IMAGE_LAYOUT_GENERAL;

      ctx->ensureImageCompatibility(cImage, usage);

      DxvkImageViewKey viewKey = {};
      viewKey.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      viewKey.format = blockFormat;
      viewKey.usage = VK_IMAGE_USAGE_STORAGE_BIT;
      viewKey.aspects = cSubresource.aspectMask;
      viewKey.mipIndex = cSubresource.mipLevel;
      viewKey.mipCount = 1u;
      viewKey.layerIndex = cSubresource.baseArrayLayer;
      viewKey.layerCount = cSubresource.layerCount;

      Rc<DxvkImageView> view = cImage->createView(viewKey);

      VkClearValue clearBlock = {};
      clearBlock.color =
          util::encodeClearBlockValue(cImage->info().format, cClearValue.color);

      VkOffset3D offset =
          util::computeBlockOffset(cOffset, formatInfo->blockSize);
      VkExtent3D extent =
          util::computeBlockExtent(cExtent, formatInfo->blockSize);

      ctx->clearImageView(view, offset, extent, VK_IMAGE_ASPECT_COLOR_BIT,
                          clearBlock);
    });
  } else {
    EmitCs([cImage = std::move(image), cSubresource = subresource,
            cOffset = offset, cExtent = extent,
            cClearValue = clearValue](DxvkContext *ctx) {
      DxvkImageUsageInfo usage = {};
      usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      usage.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      usage.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

      ctx->ensureImageCompatibility(cImage, usage);

      DxvkImageViewKey viewKey = {};
      viewKey.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      viewKey.format = cImage->info().format;
      viewKey.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      viewKey.aspects = cSubresource.aspectMask;
      viewKey.mipIndex = cSubresource.mipLevel;
      viewKey.mipCount = 1u;
      viewKey.layerIndex = cSubresource.baseArrayLayer;
      viewKey.layerCount = cSubresource.layerCount;

      Rc<DxvkImageView> view = cImage->createView(viewKey);

      if (cOffset == VkOffset3D() &&
          cExtent == cImage->mipLevelExtent(viewKey.mipIndex)) {
        ctx->clearRenderTarget(view, cSubresource.aspectMask, cClearValue, 0u);
      } else {
        ctx->clearImageView(view, cOffset, cExtent, cSubresource.aspectMask,
                            cClearValue);
      }
    });
  }

  dstTextureInfo->SetNeedsReadback(dst->GetSubresource(), true);

  if (dstTextureInfo->IsAutomaticMip())
    MarkTextureMipsDirty(dstTextureInfo);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
  return CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface,
                                       pSharedHandle, 0);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderTarget(
    DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pRenderTarget == nullptr && RenderTargetIndex == 0))
    return D3DERR_INVALIDCALL;

  // We need to make sure the render target was created using this device.
  D3D9Surface *rt = static_cast<D3D9Surface *>(pRenderTarget);
  if (unlikely(rt != nullptr && rt->GetDevice() != this))
    return D3DERR_INVALIDCALL;

  return SetRenderTargetInternal(RenderTargetIndex, pRenderTarget);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderTargetInternal(
    DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  if (unlikely(RenderTargetIndex >= caps::MaxSimultaneousRenderTargets))
    return D3DERR_INVALIDCALL;

  D3D9Surface *rt = static_cast<D3D9Surface *>(pRenderTarget);
  D3D9CommonTexture *texInfo = rt != nullptr ? rt->GetCommonTexture() : nullptr;

  if (unlikely(rt != nullptr &&
               !(texInfo->Desc()->Usage & D3DUSAGE_RENDERTARGET)))
    return D3DERR_INVALIDCALL;

  if (RenderTargetIndex == 0) {
    // Setting Render target 0 changes viewport & scissor
    // even if it gets changed to the one that's already bound.
    D3DVIEWPORT9 viewport;
    viewport.X = 0;
    viewport.Y = 0;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    RECT scissorRect;
    scissorRect.left = 0;
    scissorRect.top = 0;

    if (likely(rt != nullptr)) {
      auto rtSize = rt->GetSurfaceExtent();
      viewport.Width = rtSize.width;
      viewport.Height = rtSize.height;
      scissorRect.right = rtSize.width;
      scissorRect.bottom = rtSize.height;
    } else {
      viewport.Width = 0;
      viewport.Height = 0;
      scissorRect.right = 0;
      scissorRect.bottom = 0;
    }

    if (m_state.viewport != viewport) {
      m_dirty.set(D3D9DeviceDirtyFlag::FFViewport);
      m_dirty.set(D3D9DeviceDirtyFlag::PointScale);
      m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
      m_state.viewport = viewport;
    }

    if (m_state.scissorRect != scissorRect) {
      m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
      m_state.scissorRect = scissorRect;
    }
  }

  if (m_state.renderTargets[RenderTargetIndex] == rt)
    return D3D_OK;

  m_state.renderTargets[RenderTargetIndex] = rt;

  // Do a strong flush if the first render target is changed.
  ConsiderFlush(RenderTargetIndex == 0 ? GpuFlushType::ImplicitStrongHint
                                       : GpuFlushType::ImplicitWeakHint);

  m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);

  // Update tracking bitmasks
  uint32_t oldAlphaSwizzleRTs = m_rtSlotTracking.hasAlphaSwizzle;
  const uint32_t bit = 1u << RenderTargetIndex;
  m_rtSlotTracking.canBeSampled &= ~bit;
  m_rtSlotTracking.hasAlphaSwizzle &= ~bit;

  if (texInfo != nullptr) {
    // Update render target sampling usage bitmask for hazard tracking
    m_rtSlotTracking.canBeSampled |=
        uint8_t(HasRenderTargetBound(RenderTargetIndex) &&
                rt->GetBaseTexture() != nullptr)
        << RenderTargetIndex;

    // Update render target alpha swizzle bitmask if we need to fix up the alpha
    // channel for XRGB formats
    m_rtSlotTracking.hasAlphaSwizzle |=
        uint8_t(texInfo->GetMapping().Swizzle.a == VK_COMPONENT_SWIZZLE_ONE)
        << RenderTargetIndex;

    if (texInfo->IsAutomaticMip())
      texInfo->SetNeedsMipGen(true);
  }

  // Update hazards now that the RT has changed
  UpdateActiveHazardsRT(std::numeric_limits<uint32_t>::max());

  // The blend factors need to get adjusted to the swizzle.
  if (oldAlphaSwizzleRTs != m_rtSlotTracking.hasAlphaSwizzle)
    m_dirty.set(D3D9DeviceDirtyFlag::BlendState);

  if (RenderTargetIndex == 0) {
    // Changing RT0 can disable ATOC and
    // potentially enable alpha test, so we
    // need to keep track of the state.
    UpdateAlphaToCoverangeAndAlphaTest();

    if (likely(texInfo != nullptr)) {
      // We need to recalculate the alpha test precision for the potentially
      // changed RT format. Updating the precision is cheap, so there's no need
      // to compare the previous format to the new one.
      if (m_alphaTestEnabled)
        m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);

      bool oldValidSampleMask = m_validSampleMask;
      m_validSampleMask =
          texInfo->Desc()->MultiSample > D3DMULTISAMPLE_NONMASKABLE;
      // We need to update the multisample state to account for the changed
      // sample mask.
      if (m_validSampleMask != oldValidSampleMask)
        m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);
    } else {
      m_validSampleMask = false;
      m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);
      m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
    }
  }

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderTarget(
    DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) {
  D3D9DeviceLock lock = LockDevice();

  InitReturnPtr(ppRenderTarget);

  if (unlikely(ppRenderTarget == nullptr ||
               RenderTargetIndex > caps::MaxSimultaneousRenderTargets))
    return D3DERR_INVALIDCALL;

  if (m_state.renderTargets[RenderTargetIndex] == nullptr)
    return D3DERR_NOTFOUND;

  *ppRenderTarget = m_state.renderTargets[RenderTargetIndex].ref();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
  D3D9DeviceLock lock = LockDevice();

  D3D9Surface *ds = static_cast<D3D9Surface *>(pNewZStencil);

  if (unlikely(ds && !(ds->GetCommonTexture()->Desc()->Usage &
                       D3DUSAGE_DEPTHSTENCIL)))
    return D3DERR_INVALIDCALL;

  if (m_state.depthStencil == ds)
    return D3D_OK;

  ConsiderFlush(GpuFlushType::ImplicitWeakHint);
  m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);

  // Update depth bias if necessary
  if (ds != nullptr && m_depthBiasRepresentation.depthBiasRepresentation !=
                           VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT) {
    const int32_t vendorId =
        m_dxvkDevice->adapter()->deviceProperties().core.properties.vendorID;
    const bool exact = m_depthBiasRepresentation.depthBiasExact;
    const bool forceUnorm =
        m_depthBiasRepresentation.depthBiasRepresentation ==
        VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
    const float rValue = GetDepthBufferRValue(
        ds->GetCommonTexture()->GetFormatMapping().FormatColor, vendorId, exact,
        forceUnorm);
    if (m_depthBiasScale != rValue) {
      m_depthBiasScale = rValue;
      m_dirty.set(D3D9DeviceDirtyFlag::DepthBias);
    }
  }

  m_state.depthStencil = ds;

  UpdateActiveHazardsDS(std::numeric_limits<uint32_t>::max());

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) {
  D3D9DeviceLock lock = LockDevice();

  InitReturnPtr(ppZStencilSurface);

  if (unlikely(ppZStencilSurface == nullptr))
    return D3DERR_INVALIDCALL;

  if (m_state.depthStencil == nullptr)
    return D3DERR_NOTFOUND;

  *ppZStencilSurface = m_state.depthStencil.ref();

  return D3D_OK;
}

// The Begin/EndScene functions actually do nothing.
// Some games don't even call them.

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::BeginScene() {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(m_inScene))
    return D3DERR_INVALIDCALL;

  m_inScene = true;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::EndScene() {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(!m_inScene))
    return D3DERR_INVALIDCALL;

  ConsiderFlush(GpuFlushType::ImplicitStrongHint);

  m_inScene = false;

  // D3D9 resets the internally bound vertex buffers and index buffer in
  // EndScene if they were unbound in the meantime. We have to ignore unbinding
  // those buffers because of Operation Flashpoint Red River, so we should also
  // clear the bindings here, to avoid leaks.
  if (m_state.indices == nullptr) {
    EmitCs([](DxvkContext *ctx) {
      ctx->bindIndexBuffer(DxvkBufferSlice(), VK_INDEX_TYPE_UINT32);
    });
  }

  for (uint32_t i : bit::BitMask(
           ~static_cast<uint32_t>(m_vbSlotTracking.bound) & ((1 << 16) - 1))) {
    if (m_state.vertexBuffers[i].vertexBuffer == nullptr) {
      EmitCs([cIndex = i](DxvkContext *ctx) {
        ctx->bindVertexBuffer(cIndex, DxvkBufferSlice(), 0);
      });
    }
  }

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Clear(DWORD Count,
                                              const D3DRECT *pRects,
                                              DWORD Flags, D3DCOLOR Color,
                                              float Z, DWORD Stencil) {
  if (unlikely(!Count && pRects))
    return D3D_OK;

  D3D9DeviceLock lock = LockDevice();

  // D3DCLEAR_ZBUFFER and D3DCLEAR_STENCIL are invalid flags
  // if there is no currently bound DS (which can be the autoDS)
  if (unlikely(m_state.depthStencil == nullptr &&
               (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))))
    return D3DERR_INVALIDCALL;

  const auto &vp = m_state.viewport;
  const auto &sc = m_state.scissorRect;

  bool srgb = m_state.renderStates[D3DRS_SRGBWRITEENABLE];
  bool scissor = m_state.renderStates[D3DRS_SCISSORTESTENABLE];

  VkOffset3D offset = {int32_t(vp.X), int32_t(vp.Y), 0};
  VkExtent3D extent = {vp.Width, vp.Height, 1u};

  if (scissor) {
    offset.x = std::max<int32_t>(offset.x, sc.left);
    offset.y = std::max<int32_t>(offset.y, sc.top);

    extent.width = std::min<uint32_t>(extent.width, sc.right - offset.x);
    extent.height = std::min<uint32_t>(extent.height, sc.bottom - offset.y);
  }

  // This becomes pretty unreadable in one singular if statement...
  if (Count) {
    // If pRects is null, or our first rect encompasses the viewport:
    if (!pRects)
      Count = 0;
    else if (pRects[0].x1 <= offset.x && pRects[0].y1 <= offset.y &&
             pRects[0].x2 >= offset.x + int32_t(extent.width) &&
             pRects[0].y2 >= offset.y + int32_t(extent.height))
      Count = 0;
  }

  // Here, Count of 0 will denote whether or not to care about user rects.
  VkClearValue clearValueDepth;
  clearValueDepth.depthStencil.depth = Z;
  clearValueDepth.depthStencil.stencil = Stencil;

  VkClearValue clearValueColor;
  DecodeD3DCOLOR(Color, clearValueColor.color.float32);

  VkImageAspectFlags depthAspectMask = 0;
  if (m_state.depthStencil != nullptr) {
    if (Flags & D3DCLEAR_ZBUFFER)
      depthAspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;

    if (Flags & D3DCLEAR_STENCIL)
      depthAspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

    depthAspectMask &= lookupFormatInfo(m_state.depthStencil->GetCommonTexture()
                                            ->GetFormatMapping()
                                            .FormatColor)
                           ->aspectMask;
  }

  auto ClearImageView =
      [this](uint32_t alignment, VkOffset3D offset, VkExtent3D extent,
             const Rc<DxvkImageView> &imageView, VkImageAspectFlags aspectMask,
             VkClearValue clearValue) {
        VkExtent3D imageExtent = imageView->mipLevelExtent(0);
        extent.width = std::min(imageExtent.width, extent.width);
        extent.height = std::min(imageExtent.height, extent.height);

        if (unlikely(uint32_t(offset.x) >= imageExtent.width ||
                     uint32_t(offset.y) >= imageExtent.height))
          return;

        const bool fullClear = align(extent.width, alignment) ==
                                   align(imageExtent.width, alignment) &&
                               align(extent.height, alignment) ==
                                   align(imageExtent.height, alignment) &&
                               offset.x == 0 && offset.y == 0;

        if (fullClear) {
          EmitCs([cClearValue = clearValue, cAspectMask = aspectMask,
                  cImageView = imageView](DxvkContext *ctx) {
            ctx->clearRenderTarget(cImageView, cAspectMask, cClearValue, 0u);
          });
        } else {
          EmitCs([cClearValue = clearValue, cAspectMask = aspectMask,
                  cImageView = imageView, cOffset = offset,
                  cExtent = extent](DxvkContext *ctx) {
            ctx->clearImageView(cImageView, cOffset, cExtent, cAspectMask,
                                cClearValue);
          });
        }
      };

  auto ClearViewRect = [&](uint32_t alignment, VkOffset3D offset,
                           VkExtent3D extent) {
    // Clear depth if we need to.
    if (depthAspectMask != 0)
      ClearImageView(alignment, offset, extent,
                     m_state.depthStencil->GetDepthStencilView(true),
                     depthAspectMask, clearValueDepth);

    // Clear render targets if we need to.
    if (Flags & D3DCLEAR_TARGET) {
      for (uint32_t rt = 0u; rt < m_state.renderTargets.size(); rt++) {
        if (!HasRenderTargetBound(rt))
          continue;
        const auto &rts = m_state.renderTargets[rt];
        const auto &rtv = rts->GetRenderTargetView(srgb);

        if (likely(rtv != nullptr)) {
          ClearImageView(alignment, offset, extent, rtv,
                         VK_IMAGE_ASPECT_COLOR_BIT, clearValueColor);

          D3D9CommonTexture *dstTexture = rts->GetCommonTexture();

          if (dstTexture->IsAutomaticMip())
            MarkTextureMipsDirty(dstTexture);
        }
      }
    }
  };

  // A Hat in Time and other UE3 games only gets partial clears here
  // because of an oversized rt height due to their weird alignment...
  // This works around that.
  uint32_t alignment = m_d3d9Options.lenientClear ? 8 : 1;

  if (extent.width == 0 || extent.height == 0) {
    return D3D_OK;
  }

  if (!Count) {
    // Clear our viewport & scissor minified region in this rendertarget.
    ClearViewRect(alignment, offset, extent);
  } else {
    // Clear the application provided rects.
    for (uint32_t i = 0; i < Count; i++) {
      VkOffset3D rectOffset = {std::max<int32_t>(pRects[i].x1, offset.x),
                               std::max<int32_t>(pRects[i].y1, offset.y), 0};

      if (std::min<int32_t>(pRects[i].x2, offset.x + extent.width) <=
              rectOffset.x ||
          std::min<int32_t>(pRects[i].y2, offset.y + extent.height) <=
              rectOffset.y) {
        continue;
      }

      VkExtent3D rectExtent = {
          std::min<uint32_t>(pRects[i].x2, offset.x + extent.width) -
              rectOffset.x,
          std::min<uint32_t>(pRects[i].y2, offset.y + extent.height) -
              rectOffset.y,
          1u};

      ClearViewRect(alignment, rectOffset, rectExtent);
    }
  }

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetTransform(
    D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  return SetStateTransform(GetTransformIndex(State), pMatrix);
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pMatrix == nullptr))
    return D3DERR_INVALIDCALL;

  *pMatrix = bit::cast<D3DMATRIX>(m_state.transforms[GetTransformIndex(State)]);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::MultiplyTransform(
    D3DTRANSFORMSTATETYPE TransformState, const D3DMATRIX *pMatrix) {
  D3D9DeviceLock lock = LockDevice();

  const uint32_t idx = GetTransformIndex(TransformState);

  m_state.transforms[idx] = m_state.transforms[idx] * ConvertMatrix(pMatrix);

  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

  if (idx == GetTransformIndex(D3DTS_VIEW) ||
      idx >= GetTransformIndex(D3DTS_WORLD))
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetViewport(const D3DVIEWPORT9 *pViewport) {
  D3D9DeviceLock lock = LockDevice();

  // Outright crashes on native, but let's be
  // somewhat more elegant about it.
  if (unlikely(pViewport == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(ShouldRecord()))
    return m_recorder->SetViewport(pViewport);

  if (m_state.viewport == *pViewport)
    return D3D_OK;

  m_state.viewport.X = pViewport->X;
  m_state.viewport.Y = pViewport->Y;
  m_state.viewport.Width = pViewport->Width;
  m_state.viewport.Height = pViewport->Height;
  m_state.viewport.MinZ = pViewport->MinZ;
  m_state.viewport.MaxZ = pViewport->MinZ < pViewport->MaxZ
                              ? pViewport->MaxZ
                              : pViewport->MinZ + 0.001f;

  m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
  m_dirty.set(D3D9DeviceDirtyFlag::FFViewport);
  m_dirty.set(D3D9DeviceDirtyFlag::PointScale);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetViewport(D3DVIEWPORT9 *pViewport) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pViewport == nullptr))
    return D3DERR_INVALIDCALL;

  *pViewport = m_state.viewport;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetMaterial(const D3DMATERIAL9 *pMaterial) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pMaterial == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(ShouldRecord()))
    return m_recorder->SetMaterial(pMaterial);

  D3DMATERIAL9 finalMat = *pMaterial;

  // [War3 Shadow] Fix Ambient Lighting
  // Force materials to accept ambient light. War3 often sets this to black,
  // which causes D3DRS_AMBIENT to be ignored.
  // By setting this to white, we allow D3DRS_AMBIENT to control the scene
  // darkness. We only apply this if we have a pipeline (to avoid breaking other
  // things potentially)
  if (m_war3Pipeline) {
    finalMat.Ambient.r = 1.0f;
    finalMat.Ambient.g = 1.0f;
    finalMat.Ambient.b = 1.0f;
    finalMat.Ambient.a = 1.0f;
  }

  m_state.material = finalMat;
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetMaterial(D3DMATERIAL9 *pMaterial) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pMaterial == nullptr))
    return D3DERR_INVALIDCALL;

  *pMaterial = m_state.material;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetLight(DWORD Index,
                                                 const D3DLIGHT9 *pLight) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pLight == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(ShouldRecord())) {
    m_recorder->SetLight(Index, pLight);
    return D3D_OK;
  }

  if (Index >= m_state.lights.size())
    m_state.lights.resize(Index + 1);

  D3DLIGHT9 finalLight = *pLight;

  // [War3 Enhanced] Inject Dynamic Day-Night Cycle Lighting
  // Only apply to Light 0 (Main Directional) and if pipeline settings are
  // valid/initialized
  if (Index == 0 && m_war3Pipeline) {
    const auto &sun = m_war3Pipeline->GetSettings().sun;
    if (!sun.enabled) {
      m_state.lights[Index] = finalLight;
      if (m_state.IsLightEnabled(Index))
        m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
      return D3D_OK;
    }
    // Check if settings are initialized (direction not zero)
    if (sun.direction.x != 0.0f || sun.direction.y != 0.0f ||
        sun.direction.z != 0.0f) {
      // Force Type to Directional
      finalLight.Type = D3DLIGHT_DIRECTIONAL;

      // Override Direction (D3D9 expects direction vector)
      finalLight.Direction = {sun.direction.x, sun.direction.y,
                              sun.direction.z};

      // Override Colors (Apply Intensity)
      float r = sun.color.x * sun.intensity;
      float g = sun.color.y * sun.intensity;
      float b = sun.color.z * sun.intensity;

      finalLight.Diffuse = {r, g, b, 1.0f};
      finalLight.Specular = {r, g, b, 1.0f};

      // [War3 Shadow] Ambient Injection
      // Since D3DRS_AMBIENT is ignored by the game engine/shaders,
      // we MUST inject our calculated ambient into the Light's Ambient term.
      // This effectively achieves the "Separation" the user wants:
      // - Diffuse/Specular = Controlled by Sun Logic (Direct Light)
      // - Ambient = Controlled by our Ambient Logic (Fill Light)
      // They just travel in the same D3DLIGHT9 struct.

      // Start with a base ambient (e.g. 20% of diffuse) to emulate skylight
      Vector4 ambColor = {r * 0.2f, g * 0.2f, b * 0.2f, 1.0f};

      // Apply our Override Ambient (calculated from Night/Transition logic)
      const auto &ambSettings = m_war3Pipeline->GetSettings().ambient;
      if (ambSettings.overrideEnabled) {
        // We use MAX blending to ensure we don't darken the scene below our
        // floor
        ambColor.x = std::max(ambColor.x, ambSettings.color.x);
        ambColor.y = std::max(ambColor.y, ambSettings.color.y);
        ambColor.z = std::max(ambColor.z, ambSettings.color.z);
      }

      finalLight.Ambient = {ambColor.x, ambColor.y, ambColor.z, 1.0f};

      // Ensure Range is sufficient
      if (finalLight.Range < 1000.0f)
        finalLight.Range = 10000.0f;
    }
  }

  m_state.lights[Index] = finalLight;

  if (m_state.IsLightEnabled(Index))
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetLight(DWORD Index,
                                                 D3DLIGHT9 *pLight) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pLight == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(Index >= m_state.lights.size() || !m_state.lights[Index]))
    return D3DERR_INVALIDCALL;

  *pLight = m_state.lights[Index].value();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::LightEnable(DWORD Index, BOOL Enable) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(ShouldRecord())) {
    m_recorder->LightEnable(Index, Enable);
    return D3D_OK;
  }

  if (unlikely(Index >= m_state.lights.size()))
    m_state.lights.resize(Index + 1);

  if (unlikely(!m_state.lights[Index]))
    m_state.lights[Index] = DefaultLight;

  if (m_state.IsLightEnabled(Index) == !!Enable)
    return D3D_OK;

  uint32_t searchIndex = std::numeric_limits<uint32_t>::max();
  uint32_t setIndex = Index;

  if (!Enable)
    std::swap(searchIndex, setIndex);

  for (auto &idx : m_state.enabledLightIndices) {
    if (idx == searchIndex) {
      idx = setIndex;
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      break;
    }
  }

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetLightEnable(DWORD Index,
                                                       BOOL *pEnable) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pEnable == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(Index >= m_state.lights.size() || !m_state.lights[Index]))
    return D3DERR_INVALIDCALL;

  *pEnable = m_state.IsLightEnabled(Index) ? 128 : 0; // Weird quirk but OK.

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetClipPlane(DWORD Index,
                                                     const float *pPlane) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(!pPlane))
    return D3DERR_INVALIDCALL;

  // Higher indexes will be capped to the last valid index
  if (unlikely(Index >= caps::MaxClipPlanes))
    Index = caps::MaxClipPlanes - 1;

  if (unlikely(ShouldRecord()))
    return m_recorder->SetClipPlane(Index, pPlane);

  bool dirty = false;

  for (uint32_t i = 0; i < 4; i++) {
    dirty |= m_state.clipPlanes[Index].coeff[i] != pPlane[i];
    m_state.clipPlanes[Index].coeff[i] = pPlane[i];
  }

  bool enabled = m_state.renderStates[D3DRS_CLIPPLANEENABLE] & (1u << Index);
  dirty &= enabled;

  if (dirty)
    m_dirty.set(D3D9DeviceDirtyFlag::ClipPlanes);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetClipPlane(DWORD Index,
                                                     float *pPlane) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(!pPlane))
    return D3DERR_INVALIDCALL;

  // Higher indexes will be capped to the last valid index
  if (unlikely(Index >= caps::MaxClipPlanes))
    Index = caps::MaxClipPlanes - 1;

  for (uint32_t i = 0; i < 4; i++)
    pPlane[i] = m_state.clipPlanes[Index].coeff[i];

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderState(D3DRENDERSTATETYPE State,
                                                       DWORD Value) {
  // [War3] State-Aware Batching - DISABLED: Causes rendering issues (red cubes,
  // wrong positions) if (auto &cb =
  // dxvk::war3::reimpl::GetBatchFlushCallback())
  //   cb(this);

  D3D9DeviceLock lock = LockDevice();

  // [War3 Shadow] Shadow Pass:
  // Allow most states (e.g. VertexBlend, AlphaTest) to be set by game for
  // correct FF execution. Only block critical states: ColorWrite (Must be 0),
  // ZWrite (Must be 1), ZFunc.
  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    if (War3ShadowPassTraceEnabled()) {
      WAR3_RENDER_LOG("ShadowPass: SetRenderState %d -> %u\n",
                      static_cast<int>(State), static_cast<unsigned>(Value));
    }
    if (State == D3DRS_COLORWRITEENABLE || State == D3DRS_COLORWRITEENABLE1 ||
        State == D3DRS_COLORWRITEENABLE2 || State == D3DRS_COLORWRITEENABLE3 ||
        State == D3DRS_ZWRITEENABLE || State == D3DRS_ZFUNC) {
      return D3D_OK;
    }
  }

  // D3D9 only allows reading for values 0 and 7-255 so we don't need to do
  // anything but return OK
  if (unlikely(State > 255 || (State < D3DRS_ZENABLE && State != 0))) {
    return D3D_OK;
  }

  if (unlikely(ShouldRecord()))
    return m_recorder->SetRenderState(State, Value);

  auto &states = m_state.renderStates;
  const DWORD oldValue = states[State];

  if (likely(Value != oldValue)) {
    constexpr uint32_t nvidiaVendorId = uint32_t(DxvkGpuVendor::Nvidia);
    constexpr uint32_t amdVendorId = uint32_t(DxvkGpuVendor::Amd);
    constexpr uint32_t intelVendorId = uint32_t(DxvkGpuVendor::Intel);

    // [War3 Shadow] Ambient Override
    if (unlikely(State == D3DRS_AMBIENT)) {
      m_pureGameAmbient = Value;
      bool overrideActive = false;
      DWORD finalValue = Value;

      if (m_war3Pipeline &&
          m_war3Pipeline->GetSettings().ambient.overrideEnabled) {
        const auto &c = m_war3Pipeline->GetSettings().ambient.color;
        finalValue = D3DCOLOR_ARGB(
            0xFF,
            static_cast<int>(std::max(0.0f, std::min(1.0f, c.x)) * 255.0f),
            static_cast<int>(std::max(0.0f, std::min(1.0f, c.y)) * 255.0f),
            static_cast<int>(std::max(0.0f, std::min(1.0f, c.z)) * 255.0f));
        overrideActive = true;
      }

      Value = finalValue;

      // Debug Log (Limited frequency)
      static int s_ambLogTimer = 0;
      if (s_ambLogTimer++ > 300) { // Every ~5 seconds
        s_ambLogTimer = 0;
        WAR3_RENDER_LOG("D3D9 SetRenderState(AMBIENT): Game=0x%08X Override=%d "
                        "Final=0x%08X\n",
                        m_pureGameAmbient, overrideActive, Value);
      }

      // [Fix] Force Ambient Material Source to MATERIAL (ignore Vertex Color)
      // If we override Ambient, we must ensure the material actually USES it.
      // War3 Terrain often uses Vertex Color for ambient which might be black.
      // We force it to use the Material's Ambient (which we forced to White in
      // SetMaterial).
      if (overrideActive) {
        m_state.renderStates[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
        m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData); // Trigger update
      }
    }

    states[State] = Value;

    switch (State) {
    case D3DRS_SEPARATEALPHABLENDENABLE:
    case D3DRS_ALPHABLENDENABLE:
    case D3DRS_BLENDOP:
    case D3DRS_BLENDOPALPHA:
    case D3DRS_DESTBLEND:
    case D3DRS_DESTBLENDALPHA:
    case D3DRS_SRCBLEND:
    case D3DRS_SRCBLENDALPHA:
      m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
      break;

    case D3DRS_COLORWRITEENABLE:
      if (likely(!Value != !oldValue))
        UpdateAnyColorWrites<0>();
      m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
      break;
    case D3DRS_COLORWRITEENABLE1:
      if (likely(!Value != !oldValue))
        UpdateAnyColorWrites<1>();
      m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
      break;
    case D3DRS_COLORWRITEENABLE2:
      if (likely(!Value != !oldValue))
        UpdateAnyColorWrites<2>();
      m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
      break;
    case D3DRS_COLORWRITEENABLE3:
      if (likely(!Value != !oldValue))
        UpdateAnyColorWrites<3>();
      m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
      break;

    case D3DRS_ALPHATESTENABLE: {
      UpdateAlphaToCoverangeAndAlphaTest();
      break;
    }

    case D3DRS_ALPHAFUNC:
      m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
      break;

    case D3DRS_BLENDFACTOR:
      BindBlendFactor();
      break;

    case D3DRS_MULTISAMPLEMASK:
      if (m_validSampleMask)
        m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);
      break;

    case D3DRS_ZWRITEENABLE:
      if (likely(!Value != !oldValue)) {
        if (likely(m_state.depthStencil != nullptr &&
                   m_state.renderStates[D3DRS_ZENABLE])) {
          // Whether we write the depth has been changed => check for hazards
          UpdateActiveHazardsDS(std::numeric_limits<uint32_t>::max());
        }

        m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
      }
      break;

    case D3DRS_STENCILENABLE:
      if (likely(!Value != !oldValue)) {
        m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
      }
      break;

    case D3DRS_ZENABLE:
      if (likely(!Value != !oldValue)) {
        if (likely(m_state.depthStencil != nullptr)) {
          // The depth test has been enabled or disabled => check for hazards
          UpdateActiveHazardsDS(std::numeric_limits<uint32_t>::max());
        }

        m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
      }
      break;

    case D3DRS_ZFUNC:
    case D3DRS_TWOSIDEDSTENCILMODE:
    case D3DRS_STENCILFAIL:
    case D3DRS_STENCILZFAIL:
    case D3DRS_STENCILPASS:
    case D3DRS_STENCILFUNC:
    case D3DRS_CCW_STENCILFAIL:
    case D3DRS_CCW_STENCILZFAIL:
    case D3DRS_CCW_STENCILPASS:
    case D3DRS_CCW_STENCILFUNC:
    case D3DRS_STENCILMASK:
    case D3DRS_STENCILWRITEMASK:
      m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
      break;

    case D3DRS_STENCILREF:
      BindDepthStencilReference();
      break;

    case D3DRS_SCISSORTESTENABLE:
      m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
      break;

    case D3DRS_SRGBWRITEENABLE:
      m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
      break;

    case D3DRS_DEPTHBIAS:
    case D3DRS_SLOPESCALEDEPTHBIAS:
      m_dirty.set(D3D9DeviceDirtyFlag::DepthBias);
      break;

    case D3DRS_CULLMODE:
    case D3DRS_FILLMODE:
    case D3DRS_MULTISAMPLEANTIALIAS:
      m_dirty.set(D3D9DeviceDirtyFlag::RasterizerState);
      break;

    case D3DRS_CLIPPLANEENABLE:
      if (!Value != !oldValue)
        m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);

      m_dirty.set(D3D9DeviceDirtyFlag::ClipPlanes);
      break;

    case D3DRS_ALPHAREF:
      UpdatePushConstant<D3D9RenderStateItem::AlphaRef>();
      break;

    case D3DRS_TEXTUREFACTOR:
      m_dirty.set(D3D9DeviceDirtyFlag::FFPixelData);
      break;

    case D3DRS_DIFFUSEMATERIALSOURCE:
    case D3DRS_AMBIENTMATERIALSOURCE:
    case D3DRS_SPECULARMATERIALSOURCE:
    case D3DRS_EMISSIVEMATERIALSOURCE:
    case D3DRS_COLORVERTEX:
    case D3DRS_LIGHTING:
    case D3DRS_NORMALIZENORMALS:
    case D3DRS_LOCALVIEWER:
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      break;

    case D3DRS_AMBIENT:
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
      break;

    case D3DRS_SPECULARENABLE:
      m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      break;

    case D3DRS_FOGENABLE:
    case D3DRS_FOGVERTEXMODE:
    case D3DRS_FOGTABLEMODE:
      m_dirty.set(D3D9DeviceDirtyFlag::FogState);
      break;

    case D3DRS_RANGEFOGENABLE:
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      break;

    case D3DRS_FOGCOLOR:
      m_dirty.set(D3D9DeviceDirtyFlag::FogColor);
      break;

    case D3DRS_FOGSTART:
      m_dirty.set(D3D9DeviceDirtyFlag::FogScale);
      break;

    case D3DRS_FOGEND:
      m_dirty.set(D3D9DeviceDirtyFlag::FogScale);
      m_dirty.set(D3D9DeviceDirtyFlag::FogEnd);
      break;

    case D3DRS_FOGDENSITY:
      m_dirty.set(D3D9DeviceDirtyFlag::FogDensity);
      break;

    case D3DRS_POINTSIZE: {
      const uint32_t vendorId = m_adapter->GetVendorId();

      // AMD's driver hack for ATOC, RESZ, INST and CENT (also supported on
      // Nvidia)
      if (likely(vendorId != intelVendorId)) {
        // ATOC (AMD specific)
        constexpr uint32_t AlphaToCoverageEnable = uint32_t(D3D9Format::A2M1);
        constexpr uint32_t AlphaToCoverageDisable = uint32_t(D3D9Format::A2M0);

        if ((Value == AlphaToCoverageEnable ||
             Value == AlphaToCoverageDisable) &&
            vendorId == amdVendorId && !m_isD3D8Compatible) {
          UpdateAlphaToCoverangeAndAlphaTest();
          break;
        }

        // RESZ (AMD specific, also advertised and exposed
        // in D3D8 - once supported by Intel as well,
        // however modern drivers do not expose it)
        constexpr uint32_t RESZ = 0x7fa05000;
        if (unlikely(Value == RESZ && vendorId == amdVendorId)) {
          ResolveZ();
          break;
        }

        // INST (AMD specific)
        if (unlikely(Value == uint32_t(D3D9Format::INST) &&
                     vendorId == amdVendorId && !m_isD3D8Compatible)) {
          // Geometry instancing is supported by SM3, but ATI/AMD
          // exposed this hack to retroactively enable it on their
          // SM2-capable hardware. It's esentially a no-op.
          break;
        }

        // CENT (AMD & Nvidia)
        if (unlikely(Value == uint32_t(D3D9Format::CENT) &&
                     !m_isD3D8Compatible)) {
          // Centroid (alternate pixel center) hack.
          // Taken into account anyway, so yet another no-op.
          break;
        }
      }

      UpdatePushConstant<D3D9RenderStateItem::PointSize>();
      break;
    }

    case D3DRS_POINTSIZE_MIN:
      UpdatePushConstant<D3D9RenderStateItem::PointSizeMin>();
      break;

    case D3DRS_POINTSIZE_MAX:
      UpdatePushConstant<D3D9RenderStateItem::PointSizeMax>();
      break;

    case D3DRS_POINTSCALE_A:
    case D3DRS_POINTSCALE_B:
    case D3DRS_POINTSCALE_C:
      m_dirty.set(D3D9DeviceDirtyFlag::PointScale);
      break;

    case D3DRS_POINTSCALEENABLE:
    case D3DRS_POINTSPRITEENABLE:
      // Nothing to do here!
      // This is handled in UpdatePointMode.
      break;

    case D3DRS_SHADEMODE:
      m_dirty.set(D3D9DeviceDirtyFlag::RasterizerState);
      break;

    case D3DRS_TWEENFACTOR:
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
      break;

    case D3DRS_VERTEXBLEND:
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      break;

    case D3DRS_INDEXEDVERTEXBLENDENABLE:
      if (CanSWVP() && Value)
        m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);

      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      break;

    case D3DRS_ADAPTIVETESS_Y: {
      const uint32_t vendorId = m_adapter->GetVendorId();

      // Nvidia's driver hack for ATOC (also supported on Intel), COPM and SSAA
      if (likely(vendorId != amdVendorId && !m_isD3D8Compatible)) {
        // ATOC (Nvidia & Intel)
        constexpr uint32_t AlphaToCoverageEnable = uint32_t(D3D9Format::ATOC);
        // Disabling both ATOC and SSAA is done using D3DFMT_UNKNOWN (0)
        constexpr uint32_t AlphaToCoverageDisable = 0;

        if (Value == AlphaToCoverageEnable || Value == AlphaToCoverageDisable) {
          UpdateAlphaToCoverangeAndAlphaTest();
          break;
        }

        // COPM (Nvidia specific)
        // UE3 calls this MinimalNVIDIADriverShaderOptimization
        if (unlikely(Value == uint32_t(D3D9Format::COPM) &&
                     vendorId == nvidiaVendorId)) {
          static bool s_copmErrorShown;

          if (!std::exchange(s_copmErrorShown, true))
            Logger::info(
                "D3D9DeviceEx::SetRenderState: "
                "MinimalNVIDIADriverShaderOptimization is unsupported");

          break;
        }

        // SSAA (Nvidia specific)
        if (unlikely(Value == uint32_t(D3D9Format::SSAA) &&
                     vendorId == nvidiaVendorId)) {
          static bool s_ssaaErrorShown;

          if (!std::exchange(s_ssaaErrorShown, true))
            Logger::warn("D3D9DeviceEx::SetRenderState: Transparency "
                         "supersampling (SSAA) is unsupported");

          break;
        }
      }
      break;
    }

    case D3DRS_ADAPTIVETESS_X:
    case D3DRS_ADAPTIVETESS_Z:
    case D3DRS_ADAPTIVETESS_W: {
      const uint32_t vendorId = m_adapter->GetVendorId();

      // Nvidia specific depth bounds test hack
      const bool nvdbEnabled =
          vendorId == nvidiaVendorId && IsNVDepthBoundsTestEnabled();

      if (nvdbEnabled || m_nvdbEnabled) {
        m_dirty.set(D3D9DeviceDirtyFlag::DepthBounds);

        if (likely(IsZTestEnabled()))
          m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);

        // NVDB state changes happen on D3DRS_ADAPTIVETESS_X,
        // whereas D3DRS_ADAPTIVETESS_Z and D3DRS_ADAPTIVETESS_W
        // are used to set the values for the depth bounds test
        if (State == D3DRS_ADAPTIVETESS_X && nvdbEnabled != m_nvdbEnabled)
          m_nvdbEnabled = nvdbEnabled;
      }
      break;
    }

    default:
      static bool s_errorShown[256];

      if (!std::exchange(s_errorShown[State], true))
        Logger::warn(str::format(
            "D3D9DeviceEx::SetRenderState: Unhandled render state ", State));
      break;
    }
  }

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderState(D3DRENDERSTATETYPE State,
                                                       DWORD *pValue) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pValue == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(State > 255 || (State < D3DRS_ZENABLE && State != 0))) {
    return D3DERR_INVALIDCALL;
  }

  if (State < D3DRS_ZENABLE || State > D3DRS_BLENDOPALPHA)
    *pValue = 0;
  else
    *pValue = m_state.renderStates[State];

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateStateBlock(
    D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) {
  D3D9DeviceLock lock = LockDevice();

  // A state block can not be created while another is being recorded.
  if (unlikely(ShouldRecord()))
    return D3DERR_INVALIDCALL;

  InitReturnPtr(ppSB);

  if (unlikely(ppSB == nullptr))
    return D3DERR_INVALIDCALL;

  D3D9StateBlockType stateBlockType = ConvertStateBlockType(Type);

  if (unlikely(stateBlockType == D3D9StateBlockType::Unknown)) {
    Logger::warn(str::format(
        "D3D9DeviceEx::CreateStateBlock: Invalid state block type: ", Type));
    return D3DERR_INVALIDCALL;
  }

  try {
    const Com<D3D9StateBlock> sb = new D3D9StateBlock(this, stateBlockType);
    *ppSB = sb.ref();
    if (!m_isD3D8Compatible)
      m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_INVALIDCALL;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::BeginStateBlock() {
  D3D9DeviceLock lock = LockDevice();

  // Only one state block can be recorded at a given time.
  if (unlikely(ShouldRecord()))
    return D3DERR_INVALIDCALL;

  m_recorder = new D3D9StateBlock(this, D3D9StateBlockType::None);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::EndStateBlock(IDirect3DStateBlock9 **ppSB) {
  D3D9DeviceLock lock = LockDevice();

  // Recording a state block can't end if recording hasn't been started.
  if (unlikely(ppSB == nullptr || !ShouldRecord()))
    return D3DERR_INVALIDCALL;

  InitReturnPtr(ppSB);

  *ppSB = m_recorder.ref();
  if (!m_isD3D8Compatible)
    m_losableResourceCounter++;
  m_recorder = nullptr;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pClipStatus == nullptr))
    return D3DERR_INVALIDCALL;

  m_state.clipStatus = *pClipStatus;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pClipStatus == nullptr))
    return D3DERR_INVALIDCALL;

  *pClipStatus = m_state.clipStatus;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
  D3D9DeviceLock lock = LockDevice();

  if (ppTexture == nullptr)
    return D3DERR_INVALIDCALL;

  *ppTexture = nullptr;

  if (unlikely(InvalidSampler(Stage)))
    return D3D_OK;

  DWORD stateSampler = RemapSamplerState(Stage);

  *ppTexture = ref(m_state.textures[stateSampler]);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
  // [War3] State-Aware Batching - DISABLED: Causes rendering issues
  // if (auto &cb = dxvk::war3::reimpl::GetBatchFlushCallback())
  //   cb(this);

  // [War3] State-Aware Batching
  if (dxvk::war3::reimpl::War3InstanceBuffer *buf =
          dxvk::war3::reimpl::War3InstanceBuffer::GetActive()) {
    buf->OnSetTexture(Stage, pTexture);
  }

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    if (War3ShadowPassTraceEnabled()) {
      WAR3_RENDER_LOG("ShadowPass: SetTexture stage=%u ptr=%p\n",
                      static_cast<unsigned>(Stage), pTexture);
    }
  }
  // [War3 Shadow] Shadow Pass: 允许设置纹理（用于 Alpha Test）
  // if (unlikely(dxvk::War3Hook::IsInShadowPass()))
  //   return D3D_OK;

  if (unlikely(InvalidSampler(Stage)))
    return D3D_OK;

  DWORD stateSampler = RemapSamplerState(Stage);

  return SetStateTexture(stateSampler, pTexture);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
  auto dxvkType = RemapTextureStageStateType(Type);

  if (unlikely(pValue == nullptr))
    return D3DERR_INVALIDCALL;

  Stage = std::min(Stage, DWORD(caps::TextureStageCount - 1));
  dxvkType = std::min(dxvkType, D3D9TextureStageStateTypes(DXVK_TSS_COUNT - 1));

  *pValue = m_state.textureStages[Stage][dxvkType];

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  // [War3] State-Aware Batching - DISABLED: Causes rendering issues
  // if (auto &cb = dxvk::war3::reimpl::GetBatchFlushCallback())
  //   cb(this);

  return SetStateTextureStageState(Stage, RemapTextureStageStateType(Type),
                                   Value);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetSamplerState(
    DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pValue == nullptr))
    return D3DERR_INVALIDCALL;

  *pValue = 0;

  if (unlikely(InvalidSampler(Sampler)))
    return D3D_OK;

  Sampler = RemapSamplerState(Sampler);

  *pValue = m_state.samplerStates[Sampler][Type];

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetSamplerState(
    DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  if (unlikely(InvalidSampler(Sampler)))
    return D3D_OK;

  uint32_t stateSampler = RemapSamplerState(Sampler);

  return SetStateSamplerState(stateSampler, Type, Value);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ValidateDevice(DWORD *pNumPasses) {
  D3D9DeviceLock lock = LockDevice();

  if (pNumPasses != nullptr)
    *pNumPasses = 1;

  return IsDeviceLost() ? D3DERR_DEVICELOST : D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPaletteEntries(
    UINT PaletteNumber, const PALETTEENTRY *pEntries) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pEntries == nullptr))
    return D3DERR_INVALIDCALL;

  auto texturePalettesIter = m_state.texturePalettes.find(PaletteNumber);

  // if the palette doesn't already exist (likely), create a new element
  if (likely(texturePalettesIter == m_state.texturePalettes.end())) {
    // D3D9 documentation: "IDirect3DDevice9::SetPaletteEntries updates all of a
    // palette's 256 entries. Each entry is a PALETTEENTRY structure of the
    // format D3DFMT_A8R8G8B8."
    std::array<PALETTEENTRY, PaletteEntryCount> paletteEntry;
    memcpy(&paletteEntry[0], pEntries,
           sizeof(PALETTEENTRY) * PaletteEntryCount);

    m_state.texturePalettes.emplace(std::piecewise_construct,
                                    std::forward_as_tuple(PaletteNumber),
                                    std::forward_as_tuple(paletteEntry));
    // if the pallete already exists, update the palette entry array
  } else {
    memcpy(&texturePalettesIter->second[0], pEntries,
           sizeof(PALETTEENTRY) * PaletteEntryCount);
  }

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pEntries == nullptr))
    return D3DERR_INVALIDCALL;

  auto texturePalettesIter = m_state.texturePalettes.find(PaletteNumber);

  if (unlikely(texturePalettesIter == m_state.texturePalettes.end())) {
    Logger::warn(
        str::format("D3D9DeviceEx::GetPaletteEntries: Invalid PaletteNumber: ",
                    PaletteNumber));
    return D3DERR_INVALIDCALL;
  }

  memcpy(pEntries, &texturePalettesIter->second[0],
         sizeof(PALETTEENTRY) * PaletteEntryCount);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetCurrentTexturePalette(UINT PaletteNumber) {
  D3D9DeviceLock lock = LockDevice();

  // TODO: Use the PaletteNumber and coresponding texture palette entries
  // to translate all paletted textures for all active texture stages.
  // See:
  // https://learn.microsoft.com/en-us/windows/win32/direct3d9/texture-palettes
  m_state.texturePaletteNumber = PaletteNumber;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetCurrentTexturePalette(UINT *PaletteNumber) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(PaletteNumber == nullptr))
    return D3DERR_INVALIDCALL;

  *PaletteNumber = m_state.texturePaletteNumber;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetScissorRect(const RECT *pRect) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pRect == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(ShouldRecord()))
    return m_recorder->SetScissorRect(pRect);

  if (m_state.scissorRect == *pRect)
    return D3D_OK;

  m_state.scissorRect = *pRect;

  m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetScissorRect(RECT *pRect) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pRect == nullptr))
    return D3DERR_INVALIDCALL;

  *pRect = m_state.scissorRect;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetSoftwareVertexProcessing(BOOL bSoftware) {
  D3D9DeviceLock lock = LockDevice();

  if (bSoftware && !CanSWVP())
    return D3DERR_INVALIDCALL;

  if (!bSoftware && (m_behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING))
    return D3DERR_INVALIDCALL;

  m_isSWVP = bSoftware;

  return D3D_OK;
}

BOOL STDMETHODCALLTYPE D3D9DeviceEx::GetSoftwareVertexProcessing() {
  D3D9DeviceLock lock = LockDevice();

  return m_isSWVP;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetNPatchMode(float nSegments) {
  D3D9DeviceLock lock = LockDevice();

  m_state.nPatchSegments = nSegments;

  return D3D_OK;
}

float STDMETHODCALLTYPE D3D9DeviceEx::GetNPatchMode() {
  D3D9DeviceLock lock = LockDevice();

  return m_state.nPatchSegments;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(m_state.vertexDecl == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(!PrimitiveCount))
    return D3D_OK;

  // 仅用于管线插入点检测（UI 多使用非索引绘制）
  War3MaybeInsertBeforeUi();

  bool dynamicSysmemVBOs;
  uint32_t firstIndex = 0;
  int32_t baseVertexIndex = 0;
  uint32_t vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);
  UploadPerDrawData(StartVertex, vertexCount, firstIndex, 0, baseVertexIndex,
                    &dynamicSysmemVBOs, nullptr);

  War3TryCaptureShadowCasterDrawNonIndexed(PrimitiveType, StartVertex,
                                           vertexCount, dynamicSysmemVBOs);

  War3MaterialOverrideBackup worldBackup = {};
  bool worldOverrideActive = false;
  if (War3ShouldOverrideWorldMaterial()) {
    auto *worldMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::World);
    if (worldMat != nullptr) {
      worldOverrideActive = War3ApplyMaterialOverride(
          worldMat, War3MaterialKind::World, worldBackup);
    }
  }
  War3MaterialOverrideBackup postBackup = {};
  bool postOverrideActive = false;
  if (War3ShouldOverridePostProcessMaterial()) {
    auto *postMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::PostProcess);
    if (postMat != nullptr) {
      postOverrideActive = War3ApplyMaterialOverride(
          postMat, War3MaterialKind::PostProcess, postBackup);
    }
  }

  PrepareDraw(PrimitiveType, !dynamicSysmemVBOs, false);

  EmitCs([this, cPrimType = PrimitiveType, cPrimCount = PrimitiveCount,
          cStartVertex = StartVertex](DxvkContext *ctx) {
    uint32_t vertexCount = GetVertexCount(cPrimType, cPrimCount);

    ApplyPrimitiveType(ctx, cPrimType);

    // Tests on Windows show that D3D9 does not do non-indexed instanced draws.

    VkDrawIndirectCommand draw = {};
    draw.vertexCount = vertexCount;
    draw.instanceCount = 1u;
    draw.firstVertex = cStartVertex;

    ctx->draw(1u, &draw);
  });

  if (postOverrideActive)
    War3RestoreMaterialOverride(postBackup);
  if (worldOverrideActive)
    War3RestoreMaterialOverride(worldBackup);

  if (War3ShouldDrawOutline()) {
    auto *outlineMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::Outline);
    if (outlineMat != nullptr) {
      War3MaterialOverrideBackup outlineBackup = {};
      if (War3ApplyMaterialOverride(outlineMat, War3MaterialKind::Outline,
                                    outlineBackup)) {
        PrepareDraw(PrimitiveType, !dynamicSysmemVBOs, false);
        EmitCs([this, cPrimType = PrimitiveType, cPrimCount = PrimitiveCount,
                cStartVertex = StartVertex](DxvkContext *ctx) {
          uint32_t vertexCount = GetVertexCount(cPrimType, cPrimCount);

          ApplyPrimitiveType(ctx, cPrimType);

          VkDrawIndirectCommand draw = {};
          draw.vertexCount = vertexCount;
          draw.instanceCount = 1u;
          draw.firstVertex = cStartVertex;

          ctx->draw(1u, &draw);
        });
        War3RestoreMaterialOverride(outlineBackup);
      }
    }
  }

  return D3D_OK;
}

void D3D9DeviceEx::War3MaybeInsertBeforeUi() {
  if (!m_war3Pipeline)
    return;

  // [性能] 若本帧不需要插入 BeforeUi，则跳过所有分界检测与外部渲染插入。
  // 目的：在“光影/后处理/ShaderPack 全部关闭”时，避免每帧
  // beginExternalRendering 导致的巨大 CPU/GPU 开销，尽可能贴近原生渲染性能。
  if (!m_war3Pipeline->WantsBeforeUiInsertion())
    return;

  // 默认在 Present 阶段绘制 ImGui，确保 UI 不被后处理影响。
  // 仅在显式启用开关时，才在 UI 渲染前插入 ImGui。
  static const bool s_imguiBeforeUi =
      env::getEnvVar("DXVK_WAR3_IMGUI_BEFORE_UI") == "1";
  if (s_imguiBeforeUi && War3Hook::IsUiRendering() &&
      !war3::War3Imgui::get().hasRenderedThisFrame()) {
    IDirect3DStateBlock9 *sb = nullptr;
    if (SUCCEEDED(this->CreateStateBlock(D3DSBT_ALL, &sb))) {
      sb->Capture();

      war3::War3Imgui::get().newFrame();
      war3::War3Imgui::get().render(true);

      sb->Apply();
      sb->Release();
    } else {
      Logger::err("War3MaybeInsertBeforeUi: Failed to create state block for "
                  "ImGui injection");
    }
  }

  const auto layer = War3RenderState::CurrentLayer();
  const auto cat = War3RenderState::GetStageCategory();
  const auto tag = War3RenderState::GetCurrentBatchTag();
  if (layer == War3RenderLayer::UI ||
      cat == War3RenderState::StageCategory::UI) {
    m_war3UiDrawSeenThisFrame = true;
  }

  // War3 阶段签名遥测（限频）：
  // 目的：辅助整理 “stage →
  // 实际渲染内容/状态特征”，用于后续树木/水面/贴花等分类收敛与文档同步。
  // 说明：默认只打印每个 stage
  // 的前少量“新签名”，避免刷屏；如需更细粒度，再按需扩展。
  {
    const int stage = War3RenderState::GetStage();
    const uint32_t stageIdx =
        (stage >= 0 && stage < 32) ? uint32_t(stage) : 31u;

    const bool terrainActive = War3RenderState::IsTerrainRendering();
    const bool zEn = (m_state.renderStates[D3DRS_ZENABLE] != FALSE) &&
                     (m_state.depthStencil != nullptr);
    const bool zWrite = (m_state.renderStates[D3DRS_ZWRITEENABLE] != FALSE);
    const bool aBlend = (m_state.renderStates[D3DRS_ALPHABLENDENABLE] != FALSE);
    const bool aTest = (m_state.renderStates[D3DRS_ALPHATESTENABLE] != FALSE) &&
                       (m_state.renderStates[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS);
    const bool posT =
        (m_state.vertexDecl != nullptr) &&
        m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
    const bool hasVS = (m_state.vertexShader != nullptr);
    const bool hasPS = (m_state.pixelShader != nullptr);
    const DWORD zFunc = m_state.renderStates[D3DRS_ZFUNC];
    const DWORD cull = m_state.renderStates[D3DRS_CULLMODE];

    const uint64_t sig =
        (uint64_t(stage & 0x3F)) |
        (uint64_t(static_cast<uint32_t>(cat) & 0x0F) << 6) |
        (uint64_t(static_cast<uint32_t>(tag) & 0x0F) << 10) |
        (uint64_t(static_cast<uint32_t>(layer) & 0x03) << 14) |
        (uint64_t(terrainActive ? 1u : 0u) << 16) |
        (uint64_t(zEn ? 1u : 0u) << 17) | (uint64_t(zWrite ? 1u : 0u) << 18) |
        (uint64_t(aBlend ? 1u : 0u) << 19) | (uint64_t(aTest ? 1u : 0u) << 20) |
        (uint64_t(posT ? 1u : 0u) << 21) | (uint64_t(hasVS ? 1u : 0u) << 22) |
        (uint64_t(hasPS ? 1u : 0u) << 23) | (uint64_t(zFunc & 0x0Fu) << 24) |
        (uint64_t(cull & 0x03u) << 28);

    static std::array<uint64_t, 32> s_lastSig = {};
    static std::array<uint8_t, 32> s_sigCount = {};
    if (s_sigCount[stageIdx] < 3 && s_lastSig[stageIdx] != sig) {
      s_lastSig[stageIdx] = sig;
      s_sigCount[stageIdx]++;
      const int dispStage = War3RenderState::GetDispatcherStage();
      WAR3_RENDER_LOG("DXVK War3StageSig: stage=%d disp=%d cat=%d tag=%d "
                      "layer=%d terr=%d posT=%d zEn=%d zFunc=%u zWrite=%d "
                      "aBlend=%d aTest=%d vs=%d ps=%d cull=%u\n",
                      stage, dispStage, static_cast<int>(cat),
                      static_cast<int>(tag), static_cast<int>(layer),
                      terrainActive ? 1 : 0, posT ? 1 : 0, zEn ? 1 : 0,
                      static_cast<unsigned>(zFunc), zWrite ? 1 : 0,
                      aBlend ? 1 : 0, aTest ? 1 : 0, hasVS ? 1 : 0,
                      hasPS ? 1 : 0, static_cast<unsigned>(cull));
    }

    if (stage == 19 && terrainActive) {
      dxvk::war3::tools::MarkInGameRenderReady("War3StageSig/19",
                                               uint64_t(m_war3FrameIndex));
    }
  }

  // Shadow/PostFx 都依赖 world camera（view/proj + invVP）进行深度重建。
  // 必须确保捕获的是“世界相机”，而不是 UI/HUD 的正交相机。
  const bool inWorldThisFrame = War3RenderState::HasWorldStageThisFrame();
  const bool inUiLayer = (layer == War3RenderLayer::UI);
  const bool uiDispatchSeen = War3RenderState::HasUiDispatchThisFrame();
  const int dispStage = War3RenderState::GetDispatcherStage();
  const bool inUiBatchStage = (dispStage == 67);
  const bool uiBatchSeenThisFrame = War3RenderState::HasUiBatchStageThisFrame();
  // “强 UI 语义标记”：来自 Game.dll hook
  // 的明确路径，而不是仅靠正交/viewport/zWrite 等弱信号猜测。 目标：避免把
  // world overlay / post-process 的正交 draw 误当成 UI 开始，从而过早插入导致
  // caster 丢失与阴影闪烁。
  const bool uiStrongMarkerThisFrame =
      inUiLayer || inUiBatchStage || uiBatchSeenThisFrame;
  // Scene stage 白名单：这些值在 1.27a 的 SceneDispatcher/RenderDispatcher
  // 中通常属于“世界批次”。
  // 参考：Core/Base/Graphics/SceneStageRegistry.h（SceneWorld(1) + 38/65/68
  // 白名单经验）。
  const bool dispIsSceneWorld = (dispStage == 1) || // SceneWorld
                                (dispStage == 38) || (dispStage == 65) ||
                                (dispStage == 68);

  // 1) 捕获“世界 viewport”对应的一致输入：相机矩阵 + RT0 + DS
  //
  // 关键：
  // - ShadowReceiver 需要用深度重建 worldPos，因此相机/RT/DS
  // 必须来自同一套世界渲染目标。
  // - 不能用“最后一次看到的 DS/RT”做缓存：后续 overlay/post-process 可能会切换
  // DS/RT，
  //   一旦缓存被覆盖，就会出现“阴影乱飞/随镜头变化/拉丝”等典型症状。
  //
  // 策略：
  // - 只在 Terrain/WorldObject 阶段尝试捕获（避免 Skybox/PostProcess
  // 的特殊矩阵污染）。
  // - 不再依赖“相机 up 向量/视口占比”去猜测主视口：RTS 相机会有固定 pitch，up
  // 向量包含水平分量，
  //   且 UI
  //   可能占据很大屏幕面积，容易导致“主视口判错→相机捕获不稳定→阴影乱飞/随镜头变化”。
  // - 改为：在本帧 Terrain/WorldObject 阶段中，选择“面积最大且投影为透视”的
  // viewport 作为世界相机输入。
  if (inWorldThisFrame && !inUiLayer &&
      (cat == War3RenderState::StageCategory::Terrain ||
       cat == War3RenderState::StageCategory::WorldObject)) {
    const auto &vp = m_state.viewport;
    const uint64_t vpArea = uint64_t(vp.Width) * uint64_t(vp.Height);

    // 仅捕获透视投影（避免 UI/HUD 的正交矩阵污染 last-good camera）
    const Matrix4 &proj =
        m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];
    const float m23 = proj[2][3];
    const float m33 = proj[3][3];
    const bool projIsPerspective =
        (std::abs(std::abs(m23) - 1.0f) <= 1e-3f) && (std::abs(m33) <= 1e-3f);

    // 世界相机捕获优先级（稳定优先）：
    // - 仅信任 stage1（地形主几何）、stage11-13（单位/建筑/可破坏物）与
    // stage10（doodads/trees）；
    // - 其它 Terrain overlay（例如水面/贴花/原版阴影贴花）可能使用不同
    // view/proj，
    //   若误捕获会导致 CSM 随镜头 roll/乱飞、阴影角度异常等问题。
    //
    // 注意：这里“宁可缺一帧相机捕获（用 last-good 兜底）”，也不要频繁被 overlay
    // 覆盖。
    const int stage = War3RenderState::GetStage();
    uint32_t tier = 0u;
    if (cat == War3RenderState::StageCategory::WorldObject) {
      // 11/12/13：WorldObjects_RenderGroup
      tier = (stage >= 11 && stage <= 13) ? 3u : 0u;
    } else if (cat == War3RenderState::StageCategory::Terrain) {
      if (stage == 1)
        tier = 3u; // 主地形
      else if (stage == 10)
        tier = 2u; // doodads/trees（仍应使用主相机）
      else
        tier = 0u;
    }

    const bool shouldCapture = projIsPerspective && (tier > 0u) &&
                               ((tier > m_war3BestWorldCameraTier) ||
                                (tier == m_war3BestWorldCameraTier &&
                                 vpArea >= m_war3BestWorldViewportArea));

    if (shouldCapture) {
      m_war3BestWorldCameraTier = tier;
      m_war3BestWorldViewportArea =
          std::max(m_war3BestWorldViewportArea, vpArea);

      if (m_state.renderTargets[0] != nullptr)
        m_war3LastWorldRt0 = m_state.renderTargets[0];
      if (m_state.depthStencil != nullptr)
        m_war3LastWorldDs = m_state.depthStencil;

      War3RecordWorldCamera();
      if (!dxvk::war3::tools::IsInGameRenderReady()) {
        dxvk::war3::tools::MarkInGameRenderReady("War3Camera/WorldStage",
                                                 uint64_t(m_war3FrameIndex));
      }
      static bool s_loggedCam = false;
      if (!s_loggedCam) {
        s_loggedCam = true;
        WAR3_RENDER_LOG("DXVK War3Camera: captured from stage=%d cat=%d "
                        "tier=%u vp=%ux%u@(%u,%u)\n",
                        stage, static_cast<int>(cat),
                        static_cast<unsigned>(tier), vp.Width, vp.Height, vp.X,
                        vp.Y);
      }
    }
  }

  // 1b) 缓存“当前帧最终世界输出”的 RT0（允许随 post-process 更新）
  // - 仅在“接近本帧最大 world viewport”的范围内更新，避免被小 viewport/overlay
  // 覆盖
  // - 深度缓冲优先在世界透视相机捕获时锁定（见 1），此处仅做兜底
  const uint64_t curVpArea =
      uint64_t(m_state.viewport.Width) * uint64_t(m_state.viewport.Height);
  const bool vpLikelyWorld =
      (m_war3BestWorldViewportArea == 0u) ||
      (curVpArea >= (m_war3BestWorldViewportArea * 8u) / 10u);

  if (inWorldThisFrame && !inUiLayer && vpLikelyWorld) {
    if (m_state.renderTargets[0] != nullptr &&
        (cat == War3RenderState::StageCategory::Skybox ||
         cat == War3RenderState::StageCategory::Terrain ||
         cat == War3RenderState::StageCategory::WorldObject ||
         cat == War3RenderState::StageCategory::Effect ||
         cat == War3RenderState::StageCategory::PostProcess)) {
      bool extentOk = true;
      if (m_war3LastWorldDs != nullptr) {
        auto rtView = m_state.renderTargets[0]->GetRenderTargetView(false);
        auto dsView = m_war3LastWorldDs->GetDepthStencilView(true);
        if (rtView != nullptr && dsView != nullptr) {
          VkExtent3D rtExtent = rtView->mipLevelExtent(0u);
          VkExtent3D dsExtent = dsView->mipLevelExtent(0u);
          extentOk = (rtExtent.width == dsExtent.width) &&
                     (rtExtent.height == dsExtent.height);
        }
      }
      if (extentOk) {
        m_war3LastWorldRt0 = m_state.renderTargets[0];
      }
    }

    if (m_war3LastWorldDs == nullptr && m_state.depthStencil != nullptr) {
      const bool zTestEnabled = (m_state.renderStates[D3DRS_ZENABLE] != FALSE);
      if (zTestEnabled) {
        m_war3LastWorldDs = m_state.depthStencil;
      }
    }
  }

  bool isUiBoundaryDraw = false;

  bool uiHasPositionT = false;
  bool uiZWriteEnabled = true;
  bool uiZTestEnabled = true;
  DWORD uiZFunc = D3DCMP_LESSEQUAL;
  bool uiZFuncAlways = false;
  bool uiProjIsPerspective = true;
  bool uiViewportDiffersFromWorld = false;

  if (m_state.vertexDecl != nullptr) {
    uiHasPositionT =
        m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
    uiZWriteEnabled = m_state.renderStates[D3DRS_ZWRITEENABLE] != FALSE;

    uiZTestEnabled = (m_state.renderStates[D3DRS_ZENABLE] != FALSE) &&
                     (m_state.depthStencil != nullptr);
    uiZFunc = m_state.renderStates[D3DRS_ZFUNC];
    uiZFuncAlways = (uiZFunc == D3DCMP_ALWAYS);

    // UI/HUD 往往会把投影切到正交（非透视），这是一个比 render state
    // 更稳的分界信号。 允许 m23 = ±1：兼容 LH/RH 的常见投影形式。
    const Matrix4 &proj =
        m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];
    const float m23 = proj[2][3];
    const float m33 = proj[3][3];
    uiProjIsPerspective =
        (std::abs(std::abs(m23) - 1.0f) <= 1e-3f) && (std::abs(m33) <= 1e-3f);

    // 另一条兜底分界信号：UI/HUD 往往会把 viewport 复位到整张
    // RT（或至少不同于主世界 viewport）。 只要我们已捕获过有效世界相机，就能用
    // viewport 差异作为辅助判定，避免依赖 PositionT。
    const bool haveWorldVp =
        m_war3Scene.worldCamera.valid || m_war3LastGoodCamera.valid;
    if (haveWorldVp) {
      const D3DVIEWPORT9 &worldVp = m_war3Scene.worldCamera.valid
                                        ? m_war3Scene.worldCamera.viewport
                                        : m_war3LastGoodCamera.viewport;
      uiViewportDiffersFromWorld = worldVp.X != m_state.viewport.X ||
                                   worldVp.Y != m_state.viewport.Y ||
                                   worldVp.Width != m_state.viewport.Width ||
                                   worldVp.Height != m_state.viewport.Height;
    }
  }

  // War3 1.27a 渲染顺序（IDA）：world stage20 之后进入
  // stage15/18（UI/observer），最后才会到 stage21（收尾/后处理）。
  // 注意：stage15/18 可能“有阶段但本帧无 draw”（例如未拖拽选择框），因此
  // BeforeUi 不应依赖 “stage15/18 的第一条
  // draw”。更稳的分界信号通常是：从世界透视 viewport 切到 UI/HUD 的正交
  // viewport。
  //
  // 说明：
  // - 仅用 "reached/completed stage20" 作为硬门槛并不安全。若本帧更早的
  //   offscreen/minimap/world-side pass 先触到了 stage20，后续 Stage11 的
  //   某些透明/特殊批次就会被误当成 UI 分界候选。
  // - 由于 RenderQueue_FlushSortedItems 会按材质/状态全局排序，这种误判会表现为
  //   “全世界单位都固定缺同一批部位/材质”，而不是随机缺失。
  // - 这里把硬门槛收紧为：至少观测到 stage15（HUD/GameUI 入口）或 stage21
  //   主屏收尾完成，再允许 BeforeUi 边界生效。
  const bool reachedStage15 = War3RenderState::HasReachedStageThisFrame(15);
  const bool reachedStage21 = War3RenderState::HasReachedStageThisFrame(21);
  const bool completedStage21 = War3RenderState::HasCompletedStageThisFrame(21);
  const bool completedMainWorld21 =
      War3RenderState::HasMainWorldCompletedStageThisFrame(21);
  // 优先等待“主屏世界收尾阶段”完成再插入（更接近最终 world RT/DS），避免
  // minimap/offscreen 的阶段误触发。若主屏 stage21 尚未可见，则允许在
  // WorldDispatch 已经进入 stage15 后再进行 UI 分界检测。
  const bool readyForUiBoundary =
      completedMainWorld21 || completedStage21 || reachedStage15;
  const bool haveWorldInput =
      (m_war3LastWorldRt0 != nullptr || m_state.renderTargets[0] != nullptr) &&
      (m_war3LastWorldDs != nullptr || m_state.depthStencil != nullptr) &&
      (m_war3Scene.worldCamera.valid || m_war3LastGoodCamera.valid);

  const bool depthTestEffectivelyOff = (!uiZTestEnabled) || uiZFuncAlways;
  const bool uiMatrixOrViewportHint =
      uiHasPositionT || uiViewportDiffersFromWorld || !uiProjIsPerspective;
  // 允许用环境变量回退到“严格 UI
  // 特征触发”，便于对比排查插入时机导致的抽搐问题。
  // - DXVK_WAR3_BEFOREUI_TIER1=0：禁用 Tier1 兜底（仅当满足 Tier2/3/4
  // 时才插入）
  static const bool s_enableBeforeUiTier1 =
      env::getEnvVar("DXVK_WAR3_BEFOREUI_TIER1") != "0";
  if (!s_enableBeforeUiTier1) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      WAR3_RENDER_LOG(
          "DXVK War3Pipeline: DXVK_WAR3_BEFOREUI_TIER1=0 (禁用 Tier1 兜底)\n");
    }
  }
  // 避免“误把世界 overlay 当成 UI
  // 分界”：弱信号（Tier1/2/5）默认要求本帧已观测到 UiDispatch 或 UI batch。
  // - DXVK_WAR3_BEFOREUI_REQUIRE_UIDISPATCH=0：允许弱信号不依赖
  // UiDispatch（回退旧行为，用于对比排查）
  static const bool s_requireUiDispatchForWeakBeforeUi =
      env::getEnvVar("DXVK_WAR3_BEFOREUI_REQUIRE_UIDISPATCH") != "0";
  if (!s_requireUiDispatchForWeakBeforeUi) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      WAR3_RENDER_LOG(
          "DXVK War3Pipeline: DXVK_WAR3_BEFOREUI_REQUIRE_UIDISPATCH=0 "
          "(弱信号不要求 UiDispatch)\n");
    }
  }
  uint32_t uiBoundaryTier = 0u;

  // BeforeUi 分界判定（稳定优先）：
  //
  // 经验：
  // - Warcraft III 的 UI/observer 系统会“穿插”驱动一些世界
  // overlay（光环/水面/隐身等），
  //   仅靠“zWrite=0 / 正交投影”等单点信号很容易误判，导致：
  //     过早插入 → 后续世界 draw 覆盖 → 阴影一帧好一帧坏/抽搐/拉丝。
  //
  // 策略（分级）：
  // - Tier6：强语义信号。检测到主屏 world stage20 已完成且已进入
  // stage15（HUD/GameUI 入口），直接插入。
  // - Tier5：强信号。UI/HUD 开始的典型特征：正交投影 + viewport
  // 与世界不同且更大 + 不写深度。
  // - Tier4：强信号。RenderDispatcher Stage67（UI/菜单批次）已进入（可作为 UI
  // 开始的“武装”信号）。
  // - Tier3：强信号。处于 UI layer（UiRenderableRender hook
  // 作用域）且不写深度，并伴随正交/viewport 差异。
  // - Tier2：中信号。UiDispatch 已出现，且 viewport 与世界不同，满足典型 UI
  // render state（不写深度/正交等）。
  // - Tier1：兜底信号。仅在已进入“主屏 UI
  // 路径”且仍找不到更强信号时使用（可通过环境变量禁用）。
  //
  // 额外硬约束：必须确认已经进入主屏 UI 路径（stage15）且 world stage20
  // 已到达，避免 offscreen pass 误触发。
  if (inWorldThisFrame && readyForUiBoundary && haveWorldInput &&
      !m_war3Pipeline->HasInsertedBeforeUi()) {
    // 默认要求看到“强 UI 语义标记”后，才允许弱信号触发 BeforeUi。
    // 这是为了解决当前日志里的模式：
    // - “UI-like draw without UI layer …” + 仍有 world draw after BeforeUi
    //   => 分界误判/过早触发 => 本帧 caster 列表被 move/清空后仍有世界
    //   draw，导致阴影闪烁/撕裂且对象越多越严重。
    static const bool s_requireStrongUiMarker =
        env::getEnvVar("DXVK_WAR3_BEFOREUI_REQUIRE_STRONG_UI_MARKER") != "0";
    if (!s_requireStrongUiMarker) {
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        WAR3_RENDER_LOG(
            "DXVK War3Pipeline: DXVK_WAR3_BEFOREUI_REQUIRE_STRONG_UI_MARKER=0 "
            "(允许弱信号不依赖 UI layer/Stage67)\n");
      }
    }

    // UI/HUD 往往会把 viewport
    // 切回更大的区域（覆盖面板/全屏），并使用正交投影。 该信号比 “stage15/18
    // 的第一条 draw” 更可靠：不会依赖拖拽选择框等交互。 过滤
    // minimap/offscreen：UI/HUD 的 viewport 通常“不小于世界主 viewport”，而
    // minimap 往往明显更小。
    bool uiViewportLargerThanWorld = false;
    if (m_war3Scene.worldCamera.valid || m_war3LastGoodCamera.valid) {
      const D3DVIEWPORT9 &worldVp = m_war3Scene.worldCamera.valid
                                        ? m_war3Scene.worldCamera.viewport
                                        : m_war3LastGoodCamera.viewport;
      const uint64_t worldArea =
          uint64_t(worldVp.Width) * uint64_t(worldVp.Height);
      const uint64_t curArea =
          uint64_t(m_state.viewport.Width) * uint64_t(m_state.viewport.Height);
      uiViewportLargerThanWorld = (worldArea > 0u) && (curArea >= worldArea);
    }

    const bool weakUiBoundaryAllowed =
        !s_requireUiDispatchForWeakBeforeUi || uiDispatchSeen || inUiBatchStage;

    // Tier7：强信号（UI layer）
    // 一旦进入 UiRenderableRender hook 作用域，说明“真实 UI 节点”正在渲染。
    // 这里不再要求 zWrite/正交等特征，避免 UI 的“depth reset / state restore”
    // draw 影响分界。
    if (uiBoundaryTier == 0u && uiStrongMarkerThisFrame && inUiLayer) {
      uiBoundaryTier = 7u;
    }

    // Tier6：强语义信号（推荐）
    //
    // 依据 IDA 结论：RenderWorld stage15 会进入局内 HUD/GameUI
    // 的驱动入口（GetGameUI()->vfunc）， 且该路径只会在主屏 world
    // pass（a5==0）后出现。
    //
    // 因此：一旦满足“主屏 world 收尾阶段已完成（stage21）”且“本帧已进入
    // stage15”，就直接把“第一条后续 draw” 视为 BeforeUi 分界，不再依赖
    // viewport/zwrite/正交投影等弱信号。
    //
    // 作用：
    // - 解决“必须按左键（选择框 draw）才触发 BeforeUi”的情况（stage15
    // 可能无典型 UI render state）。
    // - 降低分界漂移导致的 caster 数量大幅波动（过早 move/clear scene）。
    static const bool s_enableBeforeUiTier6 =
        env::getEnvVar("DXVK_WAR3_BEFOREUI_TIER6") != "0";
    if (!s_enableBeforeUiTier6) {
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        WAR3_RENDER_LOG("DXVK War3Pipeline: DXVK_WAR3_BEFOREUI_TIER6=0 (禁用 "
                        "Stage15 强信号)\n");
      }
    }
    if (uiBoundaryTier == 0u && s_enableBeforeUiTier6 && completedMainWorld21 &&
        reachedStage15) {
      uiBoundaryTier = 6u;
    }

    if (uiBoundaryTier == 0u &&
        (!s_requireStrongUiMarker || uiStrongMarkerThisFrame) &&
        weakUiBoundaryAllowed && !uiZWriteEnabled && !uiProjIsPerspective &&
        uiViewportDiffersFromWorld && uiViewportLargerThanWorld) {
      uiBoundaryTier = 5u;
    }

    // Tier4：RenderDispatcher Stage67（UI/菜单批次）。对菜单/战网 UI
    // 较稳；对局内 HUD 更推荐用 stage15（Tier6/7）。 说明：Stage67 的 draw
    // 往往不携带 World stage（会显示 stage=-1），因此必须用 dispatcher stage
    // 判定。
    if (uiBoundaryTier == 0u && inUiBatchStage) {
      uiBoundaryTier = 4u;
    } else if (uiBoundaryTier == 0u && inUiLayer && !uiZWriteEnabled &&
               uiMatrixOrViewportHint && uiViewportDiffersFromWorld) {
      uiBoundaryTier = 3u;
    } else if (uiBoundaryTier == 0u &&
               (!s_requireStrongUiMarker || uiStrongMarkerThisFrame) &&
               weakUiBoundaryAllowed && !dispIsSceneWorld && !uiZWriteEnabled &&
               uiMatrixOrViewportHint && uiViewportDiffersFromWorld &&
               uiViewportLargerThanWorld &&
               (depthTestEffectivelyOff || !uiProjIsPerspective)) {
      // Tier2：疑似 UI/HUD draw，但未处于 UI layer（典型：UI draw
      // 发生在更下游的 batch submit，或 hook 作用域未覆盖）。 这里不强依赖
      // zFunc=ALWAYS：实测部分 UI draw 会保留
      // zTest=LESSEQUAL（zWrite=0），因此放宽条件：
      // - 若 viewport 已复位/投影已切正交，则允许在 zTest=LESSEQUAL
      // 下触发分界。
      uiBoundaryTier = 2u;
    }

    // Tier1：兜底。只在 viewport 已从世界主视口切换到 UI/HUD
    // 区域（不同于世界且不更小）， 且矩阵/顶点格式呈现 UI 特征（正交或
    // PositionT）时启用，避免把 post-process/offscreen 误判成 UI 分界。
    if (uiBoundaryTier == 0u && s_enableBeforeUiTier1 &&
        (!s_requireStrongUiMarker || uiStrongMarkerThisFrame) &&
        weakUiBoundaryAllowed && uiViewportDiffersFromWorld &&
        uiViewportLargerThanWorld && (uiHasPositionT || !uiProjIsPerspective)) {
      // Tier1：兜底。允许 zWrite=1 / zFunc=ALWAYS 的 UI “depth reset” draw
      // 触发分界， 否则可能需要依赖选择框等交互才能遇到 zWrite=0 的 UI
      // draw，表现为“阴影只在按左键时出现/消失”。
      uiBoundaryTier = 1u;
    }
  }

  bool semanticSceneTailBoundary = false;
  if constexpr (dxvk::war3::internal::
                    kShadowSemanticCoreSceneTailBoundaryFallbackEnabled) {
    semanticSceneTailBoundary =
        uiBoundaryTier == 0u &&
        dxvk::war3::internal::
            IsSemanticSceneTailBoundaryFallbackRuntimeEnabled() &&
        inWorldThisFrame &&
        (completedMainWorld21 || completedStage21 || reachedStage21 ||
         dispStage == 21) &&
        haveWorldInput &&
        !m_war3Pipeline->HasInsertedBeforeUi();
  }
  if (semanticSceneTailBoundary) {
    uiBoundaryTier = 8u;
    m_war3Scene.shadowStats.semanticSceneTailBoundaryCandidateCount++;
  }

  isUiBoundaryDraw = (uiBoundaryTier != 0u);

  // 诊断：如果在对局内观察到大量“疑似 HUD”的 draw，但 layer 未标记为 UI，
  // 说明 UI hook 的作用域可能未覆盖真实 Draw* 提交点（需要进一步找 UI flush
  // 点）。 诊断：观察到“强 UI 信号”但 layer 未标记为 UI，说明 UI hook
  // 作用域可能未覆盖真实 Draw*。 注意：此处仅用于诊断，不参与分界判定。
  const bool uiStrongHintWithoutLayer =
      inWorldThisFrame && !inUiLayer && readyForUiBoundary &&
      (!uiZWriteEnabled) &&
      (uiHasPositionT || uiViewportDiffersFromWorld || uiZFuncAlways ||
       !uiProjIsPerspective);

  if (inWorldThisFrame && !m_war3Pipeline->HasInsertedBeforeUi() &&
      uiStrongHintWithoutLayer) {
    static uint32_t s_uiHintWithoutLayer = 0;
    if (s_uiHintWithoutLayer < 8) {
      s_uiHintWithoutLayer++;
      WAR3_RENDER_LOG(
          "DXVK War3Pipeline: UI-like draw without UI layer stage=%d disp=%d "
          "cat=%d tag=%d uiBatchSeen=%d projPersp=%d vpDiff=%d posT=%d zEn=%d "
          "zFunc=%u zWrite=%d\n",
          War3RenderState::GetStage(), dispStage, static_cast<int>(cat),
          static_cast<int>(tag), uiBatchSeenThisFrame ? 1 : 0,
          uiProjIsPerspective ? 1 : 0, uiViewportDiffersFromWorld ? 1 : 0,
          uiHasPositionT ? 1 : 0, uiZTestEnabled ? 1 : 0, uiZFunc,
          uiZWriteEnabled ? 1 : 0);
    }
  }

  // Debug: 记录 UI layer 里前若干个 draw 的关键状态，方便定位分界信号
  if (inWorldThisFrame && inUiLayer && !m_war3Pipeline->HasInsertedBeforeUi()) {
    static uint32_t s_uiTraceCount = 0;
    if (s_uiTraceCount < 32) {
      s_uiTraceCount++;
      WAR3_RENDER_LOG(
          "DXVK War3UITrace: stage=%d disp=%d cat=%d tag=%d posT=%d zEn=%d "
          "zFunc=%u zWrite=%d vp=%ux%u@(%u,%u) sc=(%ld,%ld)-(%ld,%ld)\n",
          War3RenderState::GetStage(), dispStage, static_cast<int>(cat),
          static_cast<int>(tag), uiHasPositionT ? 1 : 0, uiZTestEnabled ? 1 : 0,
          uiZFunc, uiZWriteEnabled ? 1 : 0, m_state.viewport.Width,
          m_state.viewport.Height, m_state.viewport.X, m_state.viewport.Y,
          m_state.scissorRect.left, m_state.scissorRect.top,
          m_state.scissorRect.right, m_state.scissorRect.bottom);
    }
  }

  if (isUiBoundaryDraw) {
    static uint32_t s_loggedUiBoundary = 0;
    if (s_loggedUiBoundary < 8) {
      s_loggedUiBoundary++;
      WAR3_RENDER_LOG(
          "DXVK War3Pipeline: BeforeUi boundary draw detected tier=%u stage=%d "
          "disp=%d layer=%d cat=%d tag=%d uiBatch=%d uiDispatch=%d ready=%d "
          "done21Main=%d done21=%d projPersp=%d vpDiff=%d posT=%d zEn=%d "
          "zFunc=%u zWrite=%d\n",
          static_cast<unsigned>(uiBoundaryTier), War3RenderState::GetStage(),
          dispStage, static_cast<int>(layer), static_cast<int>(cat),
          static_cast<int>(tag), inUiBatchStage ? 1 : 0, uiDispatchSeen ? 1 : 0,
          readyForUiBoundary ? 1 : 0, completedMainWorld21 ? 1 : 0,
          completedStage21 ? 1 : 0, uiProjIsPerspective ? 1 : 0,
          uiViewportDiffersFromWorld ? 1 : 0, uiHasPositionT ? 1 : 0,
          uiZTestEnabled ? 1 : 0, uiZFunc, uiZWriteEnabled ? 1 : 0);
    }
  }

  // 诊断：若 BeforeUi 已插入但后续仍出现“明确世界 draw”，说明分界判定过早，
  // 阴影/后处理结果可能被后续世界渲染覆盖，从而表现为“乱飞/闪烁/随镜头变化”。
  if (m_war3Pipeline->HasInsertedBeforeUi() && inWorldThisFrame && !inUiLayer) {
    // 仅在“看起来仍然是可投影的世界不透明几何”时才报警：
    // - 避免 stage 值残留为 21（PostProcess）导致误报；
    // - 避免 UI/overlay draw（zWrite=0 / alphaBlend=1）刷屏。
    const bool tagWorldObject = (tag == War3BatchTag::WorldObjects ||
                                 tag == War3BatchTag::SelectionOverlay ||
                                 tag == War3BatchTag::Decorations);
    const bool terrainActive = War3RenderState::IsTerrainRendering();
    const bool zTestEnabled = (m_state.renderStates[D3DRS_ZENABLE] != FALSE) &&
                              (m_state.depthStencil != nullptr);
    const bool zWriteEnabled =
        (m_state.renderStates[D3DRS_ZWRITEENABLE] != FALSE);
    const bool alphaBlend =
        (m_state.renderStates[D3DRS_ALPHABLENDENABLE] != FALSE);
    const bool programmable = (m_state.vertexShader != nullptr);
    const bool looksOpaqueDepthWrite =
        zTestEnabled && zWriteEnabled && !alphaBlend && !programmable;

    const bool looksLikeWorldCaster =
        tagWorldObject || terrainActive || looksOpaqueDepthWrite;
    if (looksLikeWorldCaster) {
      static uint32_t s_worldAfterCount = 0;
      if (s_worldAfterCount < 8) {
        s_worldAfterCount++;
        WAR3_RENDER_LOG(
            "DXVK War3Pipeline: WARN opaque world draw after BeforeUi stage=%d "
            "disp=%d layer=%d cat=%d tag=%d projPersp=%d vpDiff=%d posT=%d "
            "zEn=%d zFunc=%u zWrite=%d blend=%d vs=%d ps=%d\n",
            War3RenderState::GetStage(), dispStage, static_cast<int>(layer),
            static_cast<int>(cat), static_cast<int>(tag),
            uiProjIsPerspective ? 1 : 0, uiViewportDiffersFromWorld ? 1 : 0,
            uiHasPositionT ? 1 : 0, zTestEnabled ? 1 : 0,
            static_cast<unsigned>(m_state.renderStates[D3DRS_ZFUNC]),
            zWriteEnabled ? 1 : 0, alphaBlend ? 1 : 0,
            m_state.vertexShader != nullptr ? 1 : 0,
            m_state.pixelShader != nullptr ? 1 : 0);
      }
    }
  }

  if (!m_war3Pipeline->NotifyDraw(layer, cat, tag, isUiBoundaryDraw))
    return;
  if (uiBoundaryTier == 8u)
    m_war3Scene.shadowStats.semanticSceneTailBoundaryCommitCount++;

  War3PipelineInput input;
  // 优先使用“世界阶段缓存的 RT/DS”，避免 UI draw 已切换目标/置空 DS
  // 时导致输入不一致。
  auto rt0 = m_war3LastWorldRt0 != nullptr ? m_war3LastWorldRt0
                                           : m_state.renderTargets[0];
  if (m_war3LastWorldRt0 != nullptr) {
    static bool s_loggedRtFallback = false;
    if (!s_loggedRtFallback) {
      s_loggedRtFallback = true;
      WAR3_RENDER_LOG(
          "DXVK War3Pipeline: using cached world RT0 for BeforeUi\n");
    }
  }
  if (rt0 != nullptr)
    input.colorView = rt0->GetRenderTargetView(false);

  auto ds =
      m_war3LastWorldDs != nullptr ? m_war3LastWorldDs : m_state.depthStencil;
  if (m_war3LastWorldDs != nullptr) {
    static bool s_loggedDsFallback = false;
    if (!s_loggedDsFallback) {
      s_loggedDsFallback = true;
      WAR3_RENDER_LOG(
          "DXVK War3Pipeline: using cached world DS for BeforeUi\n");
    }
  }
  if (ds != nullptr)
    input.depthView = ds->GetDepthStencilView(true);
  constexpr size_t kMaxShadowCasterReserve = 8192;
  constexpr size_t kMaxShadowPaletteReserve = 256;
  if (War3SemanticConsumerEnabled() &&
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled()) {
    // Once we have reached the actual BeforeUi insertion point, semantic scene
    // submission should no longer depend on the legacy shadow-capture gate.
    // Otherwise units can disappear after frame 1 when the pipeline decides
    // receiver/capture heuristics are temporarily unnecessary.
    War3TryPopulateSemanticShadowScene(
        dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly);
  }
  dxvk::war3::render::NoteShadowSceneStats(m_war3Scene.shadowStats);
  m_shadowCasterReserveHint = std::min(
      kMaxShadowCasterReserve, std::max(m_shadowCasterReserveHint,
                                        m_war3Scene.shadowCasters.capacity()));
  m_shadowPaletteReserveHint =
      std::min(kMaxShadowPaletteReserve,
               std::max(m_shadowPaletteReserveHint,
                        m_war3Scene.shadowPalettes.capacity()));
  input.scene = std::move(m_war3Scene);
  m_war3Scene = War3FrameScene{};
  m_war3ShadowPaletteHashIndex.clear();
  m_war3SemanticPaletteCache.clear();
  m_war3Scene.shadowPersistentPool.bytesCap =
      War3GetShadowPersistentPoolCapBytes();
  m_war3Scene.shadowPersistentPool.bytesUsed =
      m_war3ShadowPersistentBytesUsed;
  m_war3Scene.shadowPersistentPool.bytesEvicted =
      m_war3ShadowPersistentBytesEvicted;
  m_war3Scene.shadowPersistentPool.liveGeometryCount =
      static_cast<uint32_t>(m_war3ShadowPersistentGeometries.size());
  m_war3Scene.shadowStats.persistentPoolBytesUsed =
      m_war3ShadowPersistentBytesUsed;
  m_war3Scene.shadowStats.persistentPoolBytesEvicted =
      m_war3ShadowPersistentBytesEvicted;
  m_war3Scene.shadowStats.fallbackBudgetBytes =
      m_war3ShadowFallbackBudgetCapBytes;
  m_war3Scene.shadowStats.fallbackBudgetUsedBytes =
      m_war3ShadowFallbackBudgetUsedBytes;
  m_war3Scene.shadowStats.fallbackArenaBytes =
      dxvk::war3::memory::ShadowArena_UsedBytes();
  if (m_shadowCasterReserveHint > 0) {
    m_war3Scene.shadowCasters.reserve(m_shadowCasterReserveHint);
    m_war3Scene.shadowInstances.reserve(m_shadowCasterReserveHint);
    m_war3Scene.shadowFallbacks.reserve(m_shadowCasterReserveHint);
  }
  if (m_shadowPaletteReserveHint > 0) {
    m_war3Scene.shadowPalettes.reserve(m_shadowPaletteReserveHint);
  }
  War3ResetShadowAllocator();
  if (!input.scene.worldCamera.valid && m_war3LastGoodCamera.valid) {
    // 兜底：本帧未成功捕获透视相机（例如被临时正交矩阵污染），则使用最近一次有效相机。
    // 这样可避免阴影“整帧消失→下一帧恢复”导致的明显闪烁。
    input.scene.worldCamera = m_war3LastGoodCamera;
    static bool s_loggedCamFallback = false;
    if (!s_loggedCamFallback) {
      s_loggedCamFallback = true;
      WAR3_RENDER_LOG("DXVK War3Camera: using last-good camera fallback\n");
    }
  }
  input.settings = &m_war3Pipeline->GetSettings();
  input.frameIndex =
      m_war3FrameIndex; // Capture Current Frame Index for CS Thread

  EmitCs([this, cInput = std::move(input)](DxvkContext *ctx) mutable {
    Rc<DxvkCommandList> cmd;
    {
      auto externalScope = war3::War3PerfMonitor::instance().cpuScope(
          "War3Pipeline/BeforeUi/BeginExternalRendering");
      cmd = ctx->beginExternalRendering();
    }

    {
      auto executeScope = war3::War3PerfMonitor::instance().cpuScope(
          "War3Pipeline/BeforeUi/Execute");
      // Execute shadow pass if enabled
      m_war3Pipeline->Execute(War3InsertionPoint::BeforeUi, cmd, cInput);
    }
  });

  const bool inUiPhase = War3RenderState::IsUiPhase();
  const bool uiLikelyStarted = inUiPhase || m_war3UiDrawSeenThisFrame;
  if (dxvk::war3::internal::kWar3RenderModuleTakeoverEnabled &&
      m_war3PostProcess && m_war3Pipeline &&
      war3::runtime::IsWar3RuntimeModuleEnabled(
          war3::runtime::War3RuntimeModule::PostFx)) {
    const auto &settings = m_war3Pipeline->GetSettings();
    if (!uiLikelyStarted && settings.postFx.enabled &&
        !war3shader::internal::IsNativePostProcessDisabled()) {
      D3D9Surface *rt0Surface = m_state.renderTargets[0].ptr();
      if (m_war3LastWorldRt0 != nullptr)
        rt0Surface = m_war3LastWorldRt0.ptr();
      if (rt0Surface != nullptr) {
        auto postFxScope = war3::War3PerfMonitor::instance().cpuScope("PostFX");
        const bool ok = m_war3PostProcess->ApplyFromSurface(
            rt0Surface, rt0Surface, settings.postFx);
        if (!ok) {
          static bool s_logged = false;
          if (!s_logged) {
            s_logged = true;
            Logger::err("War3PostProcess: 执行失败，已回退到原版渲染");
          }
          m_war3Pipeline->MutableSettings().postFx.enabled = false;
        }
      }
    }
  }

  // beginExternalRendering() 会结束并重启 command list，且我们的外部 fullscreen
  // pass 会修改 Vulkan 动态状态。 为避免后续 D3D9 draw 复用“已失效的 backend
  // 状态”（表现为 FXAA 画面裁剪/放大、viewport/scissor 错位等），
  // 这里强制标记关键状态为 dirty，让下一次 draw 重新下发绑定。
  m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
  m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
  m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
  m_dirty.set(D3D9DeviceDirtyFlag::RasterizerState);

  // [Fix] SMAA/FXAA pass modifies Vulkan Viewport/Scissor state directly via
  // `cmdSetViewport`, bypassing the DxvkContext's internal state tracker. The
  // Context still thinks the Viewport is set to the Game Viewport. When we call
  // `BindViewportAndScissor()` below, `ctx->setViewports()` will compare the
  // requested Game Viewport with its cached Game Viewport, see they are
  // identical, and SKIP the emission. Result: The GPU stays in Fullscreen
  // Viewport (from SMAA) while the Context thinks it's Game Viewport.
  //
  // SOLUTION: Manually set the Context's state to a "Dummy" viewport first.
  // This forces the Context to update its tracker. Then, when we restore the
  // real viewport, the tracker sees a difference and emits the correct command.
  EmitCs([](DxvkContext *ctx) {
    DxvkViewport dummyVP = {};
    dummyVP.viewport.width = 1.0f;
    dummyVP.viewport.height = 1.0f;
    dummyVP.scissor.extent = {1, 1};
    ctx->setViewports(1, &dummyVP);
  });

  // Now restore the correct Game Viewport.
  // Context will see Dummy -> Game, and emit the command.
  BindViewportAndScissor();

  m_dirty.set(D3D9DeviceDirtyFlag::DepthBias);
  m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
  m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);
  m_dirty.set(D3D9DeviceDirtyFlag::VertexBuffers);
  m_dirty.set(D3D9DeviceDirtyFlag::IndexBuffer);
  m_dirty.set(D3D9DeviceDirtyFlag::FFViewport);
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
  m_dirty.set(D3D9DeviceDirtyFlag::FFPixelData);
  War3DrawDebugOverlayTriangle();
}

void D3D9DeviceEx::War3RecordWorldCamera() {
  War3WorldCameraState &cam = m_war3Scene.worldCamera;
  cam.view = m_state.transforms[GetTransformIndex(D3DTS_VIEW)];
  cam.proj = m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];
  cam.viewProj = cam.proj * cam.view;
  cam.invViewProj = inverse(cam.viewProj);
  cam.viewport = m_state.viewport;
  cam.scissor = m_state.scissorRect;
  // 仅在“可解析的透视投影”下认为相机有效（CSM 依赖 near/far 与透视 frustum）。
  // 允许 m23 = ±1：兼容 LH/RH 的常见投影形式。
  const float m23 = cam.proj[2][3];
  const float m33 = cam.proj[3][3];
  const bool isPerspective =
      (std::abs(std::abs(m23) - 1.0f) <= 1e-3f) && (std::abs(m33) <= 1e-3f);

  cam.valid = isPerspective;
  if (isPerspective) {
    m_war3LastGoodCamera = cam;
  }
}

bool D3D9DeviceEx::War3IsLikelyMainWorldViewport() const {
  const auto &vp = m_state.viewport;
  if (vp.Width == 0 || vp.Height == 0)
    return false;

  VkExtent3D baseExtent = {0u, 0u, 1u};
  if (m_state.renderTargets[0] != nullptr) {
    baseExtent =
        m_state.renderTargets[0]->GetRenderTargetView(false)->mipLevelExtent(
            0u);
  } else if (m_state.depthStencil != nullptr) {
    baseExtent =
        m_state.depthStencil->GetDepthStencilView(true)->mipLevelExtent(0u);
  } else {
    // 无法获取基准尺寸时，保守地认为是主视口（避免误判导致整帧无阴影）
    return true;
  }

  const uint64_t vpArea = uint64_t(vp.Width) * uint64_t(vp.Height);
  const uint64_t refArea =
      uint64_t(baseExtent.width) * uint64_t(baseExtent.height);
  if (refArea == 0)
    return true;

  // 视口面积至少覆盖渲染目标的一半，才认为是主世界视口
  // （主世界视口通常会因底部 UI
  // 留出空间；阈值过高会导致整帧无法捕获相机而无阴影）
  return vpArea * 2u >= refArea;
}

uint32_t D3D9DeviceEx::War3GetOrCreateShadowMatrixPalette() {
  // 捕获 0..255 的 WORLDMATRIX palette（用于 fixed-function / indexed vertex
  // blending）。 说明：
  // - 同一帧内同一个单位通常会重复使用同一套 palette，因此用 hash
  // 去重，避免重复拷贝。
  // - hash 仅用于快速筛选，最终会用 memcmp 做确认，避免碰撞误命中。

  uint64_t h = 1469598103934665603ull; // FNV-1a 64
  auto hashWord = [&h](uint32_t w) {
    h ^= uint64_t(w);
    h *= 1099511628211ull;
  };

  for (uint32_t i = 0; i < 256; i++) {
    const Matrix4 &m =
        m_state.transforms[GetTransformIndex(D3DTS_WORLDMATRIX(i))];
    for (uint32_t r = 0; r < 4; r++) {
      for (uint32_t c = 0; c < 4; c++) {
        hashWord(bit::cast<uint32_t>(m[r][c]));
      }
    }
  }

  if (m_war3Scene.shadowPalettes.empty() && !m_war3ShadowPaletteHashIndex.empty())
    m_war3ShadowPaletteHashIndex.clear();

  const size_t bytes = sizeof(Matrix4) * 256;
  const auto range = m_war3ShadowPaletteHashIndex.equal_range(h);
  for (auto it = range.first; it != range.second; ++it) {
    const uint32_t idx = it->second;
    if (idx >= m_war3Scene.shadowPalettes.size())
      continue;

    auto& p = m_war3Scene.shadowPalettes[idx];
    if (std::memcmp(
            p.worldMatrices.data(),
            &m_state.transforms[GetTransformIndex(D3DTS_WORLDMATRIX(0))],
            bytes) == 0) {
      return idx;
    }
  }

  War3ShadowMatrixPalette palette = {};
  palette.hash = h;
  for (uint32_t i = 0; i < 256; i++) {
    palette.worldMatrices[i] =
        m_state.transforms[GetTransformIndex(D3DTS_WORLDMATRIX(i))];
  }
  m_war3Scene.shadowPalettes.emplace_back(std::move(palette));
  const uint32_t newIndex = uint32_t(m_war3Scene.shadowPalettes.size() - 1);
  m_war3ShadowPaletteHashIndex.emplace(h, newIndex);
  return newIndex;
}

uint32_t D3D9DeviceEx::War3GetOrCreateShadowMatrixPaletteFromData(
    const Matrix4 *matrices, uint32_t matrixCount, uint64_t knownHash) {
  if (matrices == nullptr || matrixCount == 0)
    return 0u;

  const uint32_t boundedCount =
      std::min<uint32_t>(matrixCount, uint32_t(256u));

  uint64_t h = knownHash;
  if (h == 0u) {
    h = bit::fnv1a_init();
    h = bit::fnv1a_iter(h, boundedCount);
    for (uint32_t i = 0; i < boundedCount; ++i) {
      const Matrix4 &m = matrices[i];
      for (uint32_t r = 0; r < 4; ++r) {
        for (uint32_t c = 0; c < 4; ++c)
          h = bit::fnv1a_iter(h, bit::cast<uint32_t>(m[r][c]));
      }
    }
  }

  if (m_war3Scene.shadowPalettes.empty() && !m_war3ShadowPaletteHashIndex.empty())
    m_war3ShadowPaletteHashIndex.clear();

  const size_t bytes = sizeof(Matrix4) * boundedCount;
  const auto range = m_war3ShadowPaletteHashIndex.equal_range(h);
  for (auto it = range.first; it != range.second; ++it) {
    const uint32_t idx = it->second;
    if (idx >= m_war3Scene.shadowPalettes.size())
      continue;

    auto& existing = m_war3Scene.shadowPalettes[idx];
    if (std::memcmp(existing.worldMatrices.data(), matrices, bytes) == 0)
      return idx;
  }

  War3ShadowMatrixPalette palette = {};
  palette.hash = h;
  for (uint32_t i = 0; i < boundedCount; ++i)
    palette.worldMatrices[i] = matrices[i];
  m_war3Scene.shadowPalettes.emplace_back(std::move(palette));
  const uint32_t newIndex = uint32_t(m_war3Scene.shadowPalettes.size() - 1);
  m_war3ShadowPaletteHashIndex.emplace(h, newIndex);
  return newIndex;
}

uint32_t D3D9DeviceEx::War3GetOrCreateSemanticShadowPalette(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind,
    const Matrix4* overrideMatrices,
    uint32_t overrideMatrixCount,
    uint64_t overrideMatrixHash) {
  const bool hasOverridePalette =
      overrideMatrices != nullptr && overrideMatrixCount != 0u;
  if (!hasOverridePalette &&
      (!packet.hasRuntimeGroupPalette || packet.runtimeGroupPalette.empty()))
    return 0u;

  const uint32_t matrixCount = std::min<uint32_t>(
      hasOverridePalette ? overrideMatrixCount
                         : uint32_t(packet.runtimeGroupPalette.size()),
      uint32_t(256u));
  if (matrixCount == 0u)
    return 0u;

  const Matrix4* sourceMatrices =
      hasOverridePalette ? overrideMatrices : packet.runtimeGroupPalette.data();
  bool composeWorldPalette = false;
  uint64_t worldHash = 0u;
  const bool directGeosetUnitPalette =
      War3SemanticPacketUsesDirectGeosetData(packet) &&
      War3IsSemanticUnitObject(resolvedObjectKind);
  if (directGeosetUnitPalette && packet.pose.hasWorldTransform &&
      War3SemanticPaletteLooksModelLocal(sourceMatrices, matrixCount,
                                         packet.pose.worldTransform,
                                         static_cast<uint8_t>(
                                             resolvedObjectKind),
                                         false)) {
    composeWorldPalette = true;
    worldHash = War3SemanticHashMatrix4(packet.pose.worldTransform);
  }

  uint64_t matrixHash =
      overrideMatrixHash != 0u
          ? overrideMatrixHash
          : packet.runtimeGroupPaletteHash != 0u
          ? packet.runtimeGroupPaletteHash
          : (packet.pose.matrixHash != 0u ? packet.pose.matrixHash
                                          : packet.resource.contentHash);
  if (directGeosetUnitPalette && worldHash != 0u)
    matrixHash = bit::fnv1a_iter(matrixHash, worldHash);

  for (const auto& entry : m_war3SemanticPaletteCache) {
    if (entry.runtimeModelPtr == packet.renderable.runtimeModelPtr &&
        entry.matrixHash == matrixHash &&
        entry.worldHash == worldHash &&
        entry.matrixCount == matrixCount &&
        entry.objectKind == static_cast<uint8_t>(resolvedObjectKind) &&
        entry.composedWorldPalette == composeWorldPalette) {
      return entry.paletteIndex;
    }
  }

  War3SemanticPaletteCacheEntry entry = {};
  entry.runtimeModelPtr = packet.renderable.runtimeModelPtr;
  entry.paletteData = sourceMatrices;
  entry.matrixHash = matrixHash;
  entry.worldHash = worldHash;
  entry.matrixCount = matrixCount;
  entry.objectKind = static_cast<uint8_t>(resolvedObjectKind);
  entry.composedWorldPalette = composeWorldPalette;

  const Matrix4* effectiveMatrices = sourceMatrices;
  uint64_t uploadMatrixHash = matrixHash;
  if (composeWorldPalette) {
    entry.composedPalette.reserve(matrixCount);
    for (uint32_t i = 0u; i < matrixCount; ++i)
      entry.composedPalette.push_back(sourceMatrices[i] *
                                      packet.pose.worldTransform);
    effectiveMatrices = entry.composedPalette.data();
    uploadMatrixHash = War3SemanticHashMatrixPalette(effectiveMatrices,
                                                     matrixCount);
  }

  entry.paletteIndex =
      War3GetOrCreateShadowMatrixPaletteFromData(effectiveMatrices, matrixCount,
                                                 uploadMatrixHash);
  const uint32_t paletteIndex = entry.paletteIndex;
  m_war3SemanticPaletteCache.emplace_back(std::move(entry));
  return paletteIndex;
}

const D3D9DeviceEx::War3ShadowDeclInfo &
D3D9DeviceEx::War3GetShadowDeclInfo(D3D9VertexDecl *decl) {
  auto it = m_war3ShadowDeclCache.find(decl);
  if (it != m_war3ShadowDeclCache.end())
    return it->second;

  War3ShadowDeclInfo info = {};
  if (decl != nullptr) {
    for (const auto &e : decl->GetElements()) {
      if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0) {
        info.hasPosition = true;
        info.posType = D3DDECLTYPE(e.Type);
        info.posStream = e.Stream;
        info.posOffset = e.Offset;
      } else if (e.Usage == D3DDECLUSAGE_BLENDWEIGHT && e.UsageIndex == 0) {
        info.hasBlendWeight = true;
        info.weightType = D3DDECLTYPE(e.Type);
        info.weightStream = e.Stream;
        info.weightOffset = e.Offset;
      } else if (e.Usage == D3DDECLUSAGE_BLENDINDICES && e.UsageIndex == 0) {
        info.hasBlendIndex = true;
        info.indexType = D3DDECLTYPE(e.Type);
        info.indexStream = e.Stream;
        info.indexOffset = e.Offset;
      }
    }
  }

  if (info.hasPosition) {
    info.posCompCount = GetDecltypeCount(info.posType);
    info.posFormat = DecodeDecltype(info.posType);
  }
  if (info.hasBlendWeight) {
    info.weightFormat = DecodeDecltype(info.weightType);
  }
  if (info.hasBlendIndex) {
    info.indexFormat = DecodeDecltype(info.indexType);
  }

  auto result = m_war3ShadowDeclCache.emplace(decl, info);
  return result.first->second;
}

Rc<DxvkBuffer> D3D9DeviceEx::War3AllocFreezeBuffer(VkDeviceSize size,
                                                   VkDeviceSize &outOffset,
                                                   bool hostVisible,
                                                   void** outMapPtr) {
  // WAR3_RENDER_LOG("DEBUG: AllocFreezeBuffer Size=%llu Frame=%u\n", size,
  // m_war3FrameIndex);
  const VkDeviceSize align = 256;
  size = (size + align - 1) & ~(align - 1);
  if (outMapPtr)
    *outMapPtr = nullptr;

  if (!hostVisible && dxvk::war3::render::IsShadowArenaCaptureEnabled()) {
    if (!dxvk::war3::memory::ShadowArena_IsInitialized())
      dxvk::war3::memory::ShadowArena_Init();

    const auto arenaAlloc = dxvk::war3::memory::ShadowArena_Alloc(
        static_cast<uint32_t>(size), static_cast<uint32_t>(align));
    if (arenaAlloc) {
      outOffset = arenaAlloc.offset;
      return arenaAlloc.storage;
    }
  }

  if (hostVisible) {
    auto& allocator = m_war3ShadowMappedAllocators[m_war3FrameIndex];

    if (allocator.currentChunk != nullptr &&
        allocator.currentOffset + size > allocator.currentChunk->info().size) {
      allocator.currentChunk = nullptr;
      allocator.currentMapPtr = nullptr;
    }

    if (allocator.currentChunk == nullptr) {
      VkDeviceSize chunkSize =
          std::max(size, War3ShadowMappedBufferAllocator::ChunkSize);
      DxvkBufferCreateInfo info;
      info.size = chunkSize;
      info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
      info.access =
          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
      info.debugName = "War3ShadowFreezeMappedChunk";

      allocator.currentChunk = m_dxvkDevice->createBuffer(
          info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (allocator.currentChunk == nullptr) {
        WAR3_RENDER_LOG("CRITICAL: War3AllocFreezeBuffer(host) OOM/Null "
                        "detected!\n");
        return nullptr;
      }

      allocator.currentMapPtr = allocator.currentChunk->mapPtr(0);
      allocator.currentOffset = 0;
      allocator.chunks.push_back(allocator.currentChunk);
    }

    outOffset = allocator.currentOffset;
    allocator.currentOffset += size;
    if (outMapPtr && allocator.currentMapPtr != nullptr)
      *outMapPtr =
          reinterpret_cast<char*>(allocator.currentMapPtr) + outOffset;
    return allocator.currentChunk;
  }

  // Select the allocator for the CURRENT frame
  auto &allocator = m_war3ShadowAllocators[m_war3FrameIndex];

  if (allocator.currentChunk != nullptr &&
      allocator.currentOffset + size > allocator.currentChunk->info().size) {
    allocator.currentChunk = nullptr;
  }

  if (allocator.currentChunk == nullptr) {
    VkDeviceSize chunkSize =
        std::max(size, War3ShadowBufferAllocator::ChunkSize);
    DxvkBufferCreateInfo info;
    info.size = chunkSize;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    info.stages =
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT |
                  VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                  VK_ACCESS_INDEX_READ_BIT;
    info.debugName = "War3ShadowFreezeChunk";

    allocator.currentChunk =
        m_dxvkDevice->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocator.currentChunk == nullptr) {
      WAR3_RENDER_LOG("CRITICAL: War3AllocFreezeBuffer OOM/Null detected!\n");
      return nullptr;
    }

    allocator.currentOffset = 0;
    allocator.chunks.push_back(allocator.currentChunk);
  }

  outOffset = allocator.currentOffset;
  allocator.currentOffset += size;
  return allocator.currentChunk;
}

War3ShadowSemanticContext D3D9DeviceEx::War3BuildShadowSemanticContext(
    const dxvk::war3::render::RenderObjectInfo *currentObj) const {
  War3ShadowSemanticContext semantic = {};
  const auto &tls = War3RenderState::GetTlsShadowSemanticState();

  semantic.renderablePart = tls.renderablePart;
  semantic.sceneNode = tls.sceneNode;
  semantic.worldObjectEntry = tls.worldObjectEntry;
  semantic.object = tls.object ? tls.object : currentObj;
  semantic.jHandle = tls.jHandle;
  semantic.rawcode = tls.rawcode;
  semantic.objectKind = tls.objectKind;
  semantic.tag = tls.tag;
  semantic.stage = tls.stage;

  if (semantic.object != nullptr) {
    if (semantic.sceneNode == nullptr)
      semantic.sceneNode = semantic.object->sceneNode;
    if (semantic.worldObjectEntry == nullptr)
      semantic.worldObjectEntry = semantic.object->worldObjectEntry;
    if (semantic.jHandle == 0u)
      semantic.jHandle = semantic.object->jHandle;
    if (semantic.rawcode == 0u)
      semantic.rawcode = semantic.object->rawcode;
    if (static_cast<uint32_t>(semantic.objectKind) == 0u)
      semantic.objectKind = semantic.object->kind;
  }

  dxvk::war3::render::AugmentShadowSemanticContext(semantic, currentObj);
  War3AugmentShadowSemanticFromVisibleManifest(semantic);

  if (semantic.tag == War3BatchTag::Unknown) {
    const auto execTag = War3RenderState::GetTlsBatchTag();
    semantic.tag = execTag != War3BatchTag::Unknown
                       ? execTag
                       : War3RenderState::GetCurrentBatchTag();
  }
  if (semantic.stage < 0)
    semantic.stage = War3RenderState::GetStage();

  dxvk::war3::native::War3NativeShadowHint hint = {};
  if (War3TryResolveNativeShadowHint(semantic, currentObj, hint)) {
    if (semantic.jHandle == 0u)
      semantic.jHandle = hint.jHandle;
    if (semantic.rawcode == 0u)
      semantic.rawcode = hint.rawcode;
    if (semantic.objectKind == dxvk::war3::render::ObjectKind::Unknown)
      semantic.objectKind = hint.objectKind;
  }

  return semantic;
}

War3ShadowReplayMode D3D9DeviceEx::War3ClassifyShadowReplayMode(
    bool vertexBlendEnabled, bool vertexBlendIndexed) const {
  if (m_state.vertexShader != nullptr)
    return War3ShadowReplayMode::Unsupported;
  if (vertexBlendEnabled || vertexBlendIndexed)
    return War3ShadowReplayMode::PaletteSkinnedFF;
  return War3ShadowReplayMode::FixedWorld;
}

bool D3D9DeviceEx::War3TryPublishSemanticDrawTimePose() {
  if (!War3SemanticDrawTimePoseRuntime())
    return false;
  m_war3Scene.shadowStats.semanticSceneDrawTimePoseAttemptCount++;

  const int stage = War3RenderState::GetStage();
  const auto layer = War3RenderState::CurrentLayer();
  const auto cat = War3RenderState::GetStageCategory();
  if (layer == War3RenderLayer::UI ||
      cat == War3RenderState::StageCategory::PostProcess ||
      cat == War3RenderState::StageCategory::Skybox ||
      cat == War3RenderState::StageCategory::UI ||
      cat == War3RenderState::StageCategory::Effect) {
    m_war3Scene.shadowStats.semanticSceneDrawTimePoseRejectUiOrEffectCount++;
    return false;
  }

  if (m_state.vertexShader != nullptr) {
    m_war3Scene.shadowStats.semanticSceneDrawTimePoseRejectVertexShaderCount++;
    return false;
  }

  const DWORD vbState = m_state.renderStates[D3DRS_VERTEXBLEND];
  const bool vbIndexed =
      (m_state.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE] != FALSE);
  const bool vertexBlendEnabled =
      (vbState != D3DVBF_DISABLE || vbIndexed) &&
      !(vbState == D3DVBF_0WEIGHTS && !vbIndexed);
  if (!vertexBlendEnabled)
    m_war3Scene.shadowStats.semanticSceneDrawTimePoseRejectNoVertexBlendCount++;
  if (!vertexBlendEnabled)
    return false;

  const auto tag = War3RenderState::GetCurrentBatchTag();
  const auto execTag = War3RenderState::GetTlsBatchTag();
  const auto& shadowSemantic = War3RenderState::GetTlsShadowSemanticState();
  const bool objectCasterByTls =
      (tag == War3BatchTag::WorldObjects ||
       tag == War3BatchTag::SelectionOverlay ||
       tag == War3BatchTag::Decorations ||
       tag == War3BatchTag::RangeIndicatorTarget ||
       execTag == War3BatchTag::WorldObjects ||
       execTag == War3BatchTag::SelectionOverlay ||
       execTag == War3BatchTag::Decorations ||
       execTag == War3BatchTag::RangeIndicatorTarget);
  const bool objectCasterByStage =
      (cat == War3RenderState::StageCategory::WorldObject) &&
      (stage == 7 || stage == 10 || stage == 11 || stage == 12);
  const auto* currentObj = dxvk::war3::render::GetCurrentBatchObject();
  const bool objectCasterByCurrentObj =
      currentObj != nullptr &&
      currentObj->kind != dxvk::war3::render::ObjectKind::Unknown;
  if (!objectCasterByTls && !objectCasterByStage &&
      !objectCasterByCurrentObj && !shadowSemantic.HasAnyContext()) {
    m_war3Scene.shadowStats.semanticSceneDrawTimePoseRejectNoContextCount++;
    return false;
  }

  War3ShadowSemanticContext semantic = War3BuildShadowSemanticContext(currentObj);
  if (semantic.runtimeModelPtr == nullptr) {
    m_war3Scene.shadowStats
        .semanticSceneDrawTimePoseRejectNoRuntimeModelCount++;
    return false;
  }

  const uint64_t frameSerial = m_war3ShadowPersistentFrameSerial;
  if (m_war3SemanticDrawTimePoseFrameSerial != frameSerial) {
    m_war3SemanticDrawTimePoseFrameSerial = frameSerial;
    m_war3SemanticDrawTimePoseKeys.clear();
    m_war3SemanticDrawTimePoseKeys.reserve(256u);
  }

  std::array<Matrix4, 256> palette = {};
  uint64_t hash = bit::fnv1a_init();
  for (uint32_t i = 0; i < 256u; ++i) {
    palette[i] = m_state.transforms[GetTransformIndex(D3DTS_WORLDMATRIX(i))];
    const Matrix4& m = palette[i];
    for (uint32_t r = 0; r < 4u; ++r) {
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(m[r].x));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(m[r].y));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(m[r].z));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(m[r].w));
    }
  }

  const uint64_t dedupeKey =
      (uint64_t(reinterpret_cast<uintptr_t>(semantic.runtimeModelPtr)) >> 4u) ^
      (hash + 0x9E3779B97F4A7C15ull);
  if (std::find(m_war3SemanticDrawTimePoseKeys.begin(),
                m_war3SemanticDrawTimePoseKeys.end(),
                dedupeKey) != m_war3SemanticDrawTimePoseKeys.end()) {
    m_war3Scene.shadowStats.semanticSceneDrawTimePoseDedupedCount++;
    return false;
  }
  m_war3SemanticDrawTimePoseKeys.push_back(dedupeKey);

  void* unitPtr = semantic.object != nullptr ? semantic.object->unitPtr : nullptr;
  dxvk::war3::model::PoseRegistry::instance().recordMatrixPalette(
      semantic.runtimeModelPtr, semantic.sceneNode, unitPtr, palette.data(),
      uint32_t(palette.size()));
  m_war3Scene.shadowStats.semanticSceneDrawTimePosePublishedCount++;
  War3NoteDrawTimePoseMotion(m_war3Scene.shadowStats, semantic.runtimeModelPtr,
                             frameSerial, hash);
  m_war3SemanticDrawTimePoseDirtyFrameSerial = frameSerial;
  return true;
}

bool D3D9DeviceEx::War3CanPromoteShadowPersistentGeometry(
    const War3ShadowSemanticContext &semantic, War3ShadowReplayMode mode,
    bool objectCaster, bool indexed, bool captureAlphaTest, bool alphaBlendEnabled,
    bool dynamicSysmemVBOs, bool dynamicSysmemIBO, bool posDynamic,
    bool blendDynamic, bool ibDynamic, uint32_t blendBinding,
    const Rc<DxvkBuffer> &posStorage,
    const Rc<DxvkBuffer> &blendStorage,
    const Rc<DxvkBuffer> &indexStorage) const {
  if (!semantic.HasStableIdentity())
    return false;
  if (!(mode == War3ShadowReplayMode::FixedWorld ||
        mode == War3ShadowReplayMode::PaletteSkinnedFF)) {
    return false;
  }
  // 现阶段还没有“静态模型 + 每帧姿态更新”的正式主路径。
  // 一旦把蒙皮/混合路径误晋升到 persistent，阴影就会停在首帧或跟丢单位。
  // 在动态姿态 runtime 真正接管前，先彻底禁止 skinned polygons 进入缓存。
  if (mode == War3ShadowReplayMode::PaletteSkinnedFF)
    return false;
  using dxvk::war3::render::ObjectKind;
  const bool poseDrivenObject =
      semantic.runtimeModelPtr != nullptr || semantic.hasPoseTransform ||
      semantic.poseFromSpriteFrame || semantic.poseMatrixCount != 0u;
  const bool runtimeAnimatedObject =
      poseDrivenObject &&
      semantic.objectKind != ObjectKind::Building &&
      semantic.objectKind != ObjectKind::Destructible;
  if (semantic.objectKind == ObjectKind::Unit ||
      semantic.objectKind == ObjectKind::Effect || runtimeAnimatedObject)
    return false;
  if (posStorage == nullptr)
    return false;
  if (indexed && indexStorage == nullptr)
    return false;
  if (mode == War3ShadowReplayMode::PaletteSkinnedFF && blendBinding == 1 &&
      blendStorage == nullptr) {
    return false;
  }

  // 当前最重要的是别再把会动的 CUnit/Effect/Unknown world-object 几何
  // 错误晋升到 persistent，否则阴影会停在首帧。与此同时，Building /
  // Destructible 这类明确静态对象又必须保留缓存资格，否则低压图性能会
  // 明显掉回去。这里先收成“只允许明确静态的 world objects”。
  if (objectCaster) {
    switch (semantic.objectKind) {
    case ObjectKind::Building:
    case ObjectKind::Destructible:
      break;
    default:
      return false;
    }
  }

  const bool dynamicSource =
      dynamicSysmemVBOs || dynamicSysmemIBO || posDynamic || blendDynamic ||
      ibDynamic;
  const bool allowDynamicSourceForStaticWorldObject =
      objectCaster &&
      (semantic.objectKind == dxvk::war3::render::ObjectKind::Building ||
       semantic.objectKind == dxvk::war3::render::ObjectKind::Destructible);

  if (dynamicSource && !allowDynamicSourceForStaticWorldObject)
    return false;

  return true;
}

bool D3D9DeviceEx::War3CreateShadowPersistentBuffer(
    const War3ShadowPersistentUpload &upload, Rc<DxvkBuffer> &outStorage,
    DxvkResourceBufferInfo &outInfo) {
  if (upload.bytes == 0)
    return false;

  const bool hasSliceSource = upload.slice.buffer() != nullptr;
  const bool hasHostSource = upload.hostData != nullptr;
  if (!hasSliceSource && !hasHostSource)
    return false;

  DxvkBufferCreateInfo info = {};
  info.size = War3AlignPersistentBytes(upload.bytes);
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | upload.usage;
  info.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
  info.access = VK_ACCESS_TRANSFER_WRITE_BIT |
                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                VK_ACCESS_INDEX_READ_BIT;
  info.debugName = upload.debugName;

  auto dst = m_dxvkDevice->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (dst == nullptr)
    return false;

  if (hasHostSource) {
    DxvkBufferCreateInfo stagingInfo = {};
    stagingInfo.size = War3AlignPersistentBytes(upload.bytes);
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    stagingInfo.access = VK_ACCESS_TRANSFER_READ_BIT;
    stagingInfo.debugName = upload.debugName;

    auto staging = m_dxvkDevice->createBuffer(
        stagingInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (staging == nullptr)
      return false;

    void* mapPtr = staging->mapPtr(0);
    if (mapPtr == nullptr)
      return false;

    std::memcpy(mapPtr, upload.hostData, size_t(upload.bytes));
    EmitCs([cDst = dst, cSrc = staging, cBytes = upload.bytes](DxvkContext *ctx) {
      ctx->copyBuffer(cDst, 0, cSrc, 0, cBytes);
    });
  } else {
    auto srcBuffer = upload.slice.buffer();
    EmitCs([cDst = dst, cSrc = srcBuffer, cSrcOff = upload.slice.offset(),
            cBytes = upload.bytes](DxvkContext *ctx) {
      ctx->copyBuffer(cDst, 0, cSrc, cSrcOff, cBytes);
    });
  }

  outStorage = std::move(dst);
  outInfo = outStorage->getSliceInfo(0, upload.bytes);
  return true;
}

void D3D9DeviceEx::War3GcShadowPersistentGeometry(bool forceTrimToBudget) {
  const uint64_t currentFrame = m_war3ShadowPersistentFrameSerial;
  const uint64_t maxAge = War3GetShadowPersistentMaxAgeFrames();
  std::vector<uint32_t> staleIds;
  staleIds.reserve(m_war3ShadowPersistentGeometries.size());

  for (const auto &entryIt : m_war3ShadowPersistentGeometries) {
    const auto &entry = entryIt.second;
    if (currentFrame > entry.lastSeenFrame &&
        (currentFrame - entry.lastSeenFrame) > maxAge) {
      staleIds.push_back(entryIt.first);
    }
  }

  auto evictGeometry = [this](uint32_t geometryId) {
    auto entryIt = m_war3ShadowPersistentGeometries.find(geometryId);
    if (entryIt == m_war3ShadowPersistentGeometries.end())
      return;

    const auto bytes = entryIt->second.totalBytes;
    m_war3ShadowGeometryRegistry.erase(entryIt->second.key);
    if (m_war3ShadowPersistentBytesUsed >= bytes)
      m_war3ShadowPersistentBytesUsed -= bytes;
    else
      m_war3ShadowPersistentBytesUsed = 0;
    m_war3ShadowPersistentBytesEvicted += bytes;
    m_war3Scene.shadowPersistentPool.evictedThisFrame++;
    m_war3ShadowPersistentGeometries.erase(entryIt);
  };

  for (uint32_t geometryId : staleIds)
    evictGeometry(geometryId);

  const uint64_t capBytes = War3GetShadowPersistentPoolCapBytes();
  if (!forceTrimToBudget && m_war3ShadowPersistentBytesUsed <= capBytes)
    return;

  if (m_war3ShadowPersistentBytesUsed <= capBytes)
    return;

  std::vector<std::pair<uint64_t, uint32_t>> byAge;
  byAge.reserve(m_war3ShadowPersistentGeometries.size());
  for (const auto &entryIt : m_war3ShadowPersistentGeometries) {
    byAge.emplace_back(entryIt.second.lastSeenFrame, entryIt.first);
  }
  std::sort(byAge.begin(), byAge.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  for (const auto &item : byAge) {
    if (m_war3ShadowPersistentBytesUsed <= capBytes)
      break;
    evictGeometry(item.second);
  }
}

bool D3D9DeviceEx::War3TryFindShadowPersistentGeometry(
    const War3ShadowGeometryRegistryKey &key, uint32_t &outGeometryId,
    const War3ShadowPersistentGeometry *&outGeometry) {
  outGeometryId = 0;
  outGeometry = nullptr;

  auto regIt = m_war3ShadowGeometryRegistry.find(key);
  if (regIt != m_war3ShadowGeometryRegistry.end()) {
    auto geomIt =
        m_war3ShadowPersistentGeometries.find(regIt->second.geometryId);
    if (geomIt != m_war3ShadowPersistentGeometries.end()) {
      regIt->second.instances++;
      geomIt->second.lastSeenFrame = m_war3ShadowPersistentFrameSerial;
      geomIt->second.geometry.lastSeenFrame = m_war3ShadowPersistentFrameSerial;
      outGeometryId = regIt->second.geometryId;
      outGeometry = &geomIt->second.geometry;
      return true;
    }

    m_war3ShadowGeometryRegistry.erase(regIt);
  }
  return false;
}

bool D3D9DeviceEx::War3FindOrCreateShadowPersistentGeometry(
    const War3ShadowGeometryRegistryKey &key,
    const War3ShadowPersistentGeometry &candidate,
    const std::array<War3ShadowPersistentUpload, 4> &uploads,
    uint32_t &outGeometryId, const War3ShadowPersistentGeometry *&outGeometry,
    bool &outCreatedNew) {
  outCreatedNew = false;
  if (War3TryFindShadowPersistentGeometry(key, outGeometryId, outGeometry))
    return true;

  uint64_t bytesNeeded = 0;
  for (const auto &upload : uploads)
    bytesNeeded += War3AlignPersistentBytes(upload.bytes);

  War3GcShadowPersistentGeometry(false);
  const uint64_t capBytes = War3GetShadowPersistentPoolCapBytes();
  if (m_war3ShadowPersistentBytesUsed + bytesNeeded > capBytes) {
    War3GcShadowPersistentGeometry(true);
  }
  if (m_war3ShadowPersistentBytesUsed + bytesNeeded > capBytes)
    return false;

  War3ShadowPersistentGeometry stored = candidate;
  stored.key = key;
  stored.totalBytes = bytesNeeded;
  stored.lastSeenFrame = m_war3ShadowPersistentFrameSerial;

  if (uploads[0].bytes > 0) {
    if (!War3CreateShadowPersistentBuffer(uploads[0], stored.positionStorage,
                                          stored.positionInfo)) {
      return false;
    }
  }
  if (uploads[1].bytes > 0) {
    if (!War3CreateShadowPersistentBuffer(uploads[1], stored.indexStorage,
                                          stored.indexInfo)) {
      return false;
    }
  }
  if (uploads[2].bytes > 0) {
    if (!War3CreateShadowPersistentBuffer(uploads[2], stored.blendStorage,
                                          stored.blendInfo)) {
      return false;
    }
  }
  if (uploads[3].bytes > 0) {
    if (!War3CreateShadowPersistentBuffer(uploads[3], stored.uvStorage,
                                          stored.uvInfo)) {
      return false;
    }
  }

  const uint32_t geometryId = m_war3NextShadowGeometryId++;
  War3ShadowPersistentGeometryEntry entry = {};
  entry.key = key;
  entry.geometry = std::move(stored);
  entry.totalBytes = bytesNeeded;
  entry.lastSeenFrame = m_war3ShadowPersistentFrameSerial;

  auto inserted =
      m_war3ShadowPersistentGeometries.emplace(geometryId, std::move(entry));
  if (!inserted.second)
    return false;

  auto &registryEntry = m_war3ShadowGeometryRegistry[key];
  registryEntry.geometryId = geometryId;
  registryEntry.instances = 1;
  registryEntry.instanceable = true;

  m_war3ShadowPersistentBytesUsed += bytesNeeded;
  outGeometryId = geometryId;
  outGeometry = &inserted.first->second.geometry;
  outCreatedNew = true;
  return true;
}

bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  auto toVkTopology = [](dxvk::war3::shadow::ShadowPrimitiveTopology topology) {
    switch (topology) {
    case dxvk::war3::shadow::ShadowPrimitiveTopology::TriangleStrip:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case dxvk::war3::shadow::ShadowPrimitiveTopology::TriangleFan:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case dxvk::war3::shadow::ShadowPrimitiveTopology::LineList:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case dxvk::war3::shadow::ShadowPrimitiveTopology::LineStrip:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case dxvk::war3::shadow::ShadowPrimitiveTopology::TriangleList:
    default:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
  };

  const auto& packetPositions = packet.resource.positionVec();
  const auto& packetIndices = packet.resource.indexVec();
  const auto& packetVertexGroups = packet.resource.vertexGroupIndexVec();
  const auto& packetBlendWeights = packet.resource.vertexBlendWeightVec();
  const auto& packetBlendIndices = packet.resource.vertexBlendIndexVec();
  const auto ownedDynamicIndices = packet.resource.ownedDynamicIndices;
  const bool hasOwnedDynamicIndexStream =
      ownedDynamicIndices != nullptr && !ownedDynamicIndices->empty();
  const bool hasDynamicPositionStream =
      packet.usesDynamicMeshPositions &&
      packet.resource.dynamicPositionStream != nullptr &&
      packet.resource.dynamicPositionStride >= 12u &&
      packet.resource.vertexCount != 0u;
  const bool hasDynamicIndexStream =
      hasOwnedDynamicIndexStream ||
      (packet.resource.dynamicIndexStream != nullptr &&
       packet.resource.dynamicIndexCount != 0u);
  const uint32_t positionVertexCount =
      hasDynamicPositionStream ? packet.resource.vertexCount
                               : uint32_t(packetPositions.size() / 3u);
  const uint32_t declaredVertexCount =
      packet.resource.vertexCount != 0u ? packet.resource.vertexCount
                                        : positionVertexCount;
  const uint32_t vertexCount =
      (std::min)(declaredVertexCount, positionVertexCount);
  const auto resolvedObjectKind =
      War3ResolveSemanticPacketObjectKindFast(packet);
  if (vertexCount == 0u) {
    m_war3Scene.shadowStats.semanticSceneRejectedNoVertex++;
    return false;
  }
  if (!hasDynamicPositionStream &&
      packetPositions.size() < size_t(vertexCount) * 3u) {
    m_war3Scene.shadowStats.semanticSceneRejectedGeometry++;
    return false;
  }
  if (hasDynamicPositionStream) {
    const auto* srcBase =
        reinterpret_cast<const uint8_t*>(packet.resource.dynamicPositionStream);
    const size_t srcStride = size_t(packet.resource.dynamicPositionStride);
    const size_t lastReadOffset =
        vertexCount != 0u ? size_t(vertexCount - 1u) * srcStride : 0u;
    if (!dxvk::war3::IsReadableRange(srcBase, lastReadOffset + sizeof(float) * 3u)) {
      m_war3Scene.shadowStats.semanticSceneRejectedGeometry++;
      return false;
    }
  }

  const bool skinned =
      packet.path == dxvk::war3::shadow::ShadowDrawPath::Skinned;
  const bool useIndices = hasDynamicIndexStream || !packetIndices.empty();
  if constexpr (dxvk::war3::internal::
                    kShadowSemanticCoreSceneRequireVisibleIndexSliceForSkinned) {
    const bool hasLiveVisibleMeshContext =
        packet.renderable.meshData != nullptr ||
        packet.renderable.renderablePart != nullptr;
    const bool allowCanonicalSinglePrimitiveFullIndex =
        War3SemanticAllowCanonicalSinglePrimitiveFullIndexRuntime() &&
        War3SemanticPacketUsesDirectGeosetData(packet) &&
        packet.resource.primitiveRecordCount <= 1u &&
        War3HasSemanticDynamicUnitEvidence(packet);
    if (skinned && hasLiveVisibleMeshContext && !hasDynamicIndexStream &&
        !packetIndices.empty() &&
        War3SemanticRequireVisibleIndexSliceForSkinnedRuntime() &&
        !allowCanonicalSinglePrimitiveFullIndex) {
      m_war3Scene.shadowStats
          .semanticSceneSkinnedMissingVisibleIndexSliceRejectCount++;
      return false;
    }
  }
  if (skinned) {
    if (hasDynamicIndexStream) {
      m_war3Scene.shadowStats.semanticSceneSkinnedDynamicIndexSliceCount++;
    } else if (!packetIndices.empty()) {
      m_war3Scene.shadowStats.semanticSceneSkinnedFullIndexFallbackCount++;
      m_war3Scene.shadowStats
          .semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr =
          reinterpret_cast<uintptr_t>(packet.renderable.runtimeModelPtr);
      m_war3Scene.shadowStats
          .semanticSceneSkinnedFullIndexFallbackLastIndexCount =
          uint32_t(packetIndices.size());
    }
  }
  const uint16_t* effectiveIndexData =
      hasOwnedDynamicIndexStream
          ? ownedDynamicIndices->data()
          : hasDynamicIndexStream ? packet.resource.dynamicIndexStream
                                  : packetIndices.data();
  const uint32_t effectiveIndexCount =
      hasOwnedDynamicIndexStream
          ? uint32_t(ownedDynamicIndices->size())
          : hasDynamicIndexStream ? packet.resource.dynamicIndexCount
                                  : uint32_t(packetIndices.size());
  const VkPrimitiveTopology effectiveTopology =
      toVkTopology(packet.resource.topology);
  if (useIndices && !hasDynamicIndexStream &&
      packetIndices.size() < size_t(effectiveIndexCount)) {
    m_war3Scene.shadowStats.semanticSceneRejectedGeometry++;
    return false;
  }
  if (hasDynamicIndexStream && !hasOwnedDynamicIndexStream &&
      !dxvk::war3::IsReadableRange(effectiveIndexData,
                                   size_t(effectiveIndexCount) *
                                       sizeof(uint16_t))) {
    m_war3Scene.shadowStats.semanticSceneRejectedGeometry++;
    return false;
  }
  const uint64_t effectiveModelKey =
      packet.renderable.modelKey != 0u ? packet.renderable.modelKey
                                       : packet.resource.modelKey;
  const auto effectiveAlphaMode = packet.material.alphaMode;
  const bool alphaCutoutEnabled =
      effectiveAlphaMode == dxvk::war3::shadow::ShadowAlphaMode::Cutout;
  const bool alphaBlendEnabled =
      effectiveAlphaMode == dxvk::war3::shadow::ShadowAlphaMode::AlphaBlend;
  const float alphaCutoutRef =
      alphaCutoutEnabled ? packet.material.alphaCutoutRef : 0.5f;
  std::vector<Matrix4> liveRuntimeGroupPalette;
  uint32_t liveMaxVertexGroupSlot = 0u;
  uint64_t liveRuntimeGroupPaletteHash = 0u;
  uint64_t liveRuntimeRawPaletteHash = 0u;
  void* liveRuntimePoseModelPtr = nullptr;

  War3ShadowGeometryRegistryKey key = {};
  key.sourceHash = bit::fnv1a_init();
  key.sourceHash = bit::fnv1a_iter(key.sourceHash, packet.resource.contentHash);
  key.sourceHash = bit::fnv1a_iter(key.sourceHash, effectiveModelKey);
  key.sourceHash = bit::fnv1a_iter(
      key.sourceHash,
      reinterpret_cast<uintptr_t>(packet.resource.modelResourcePtr));
  key.sourceHash = bit::fnv1a_iter(
      key.sourceHash,
      reinterpret_cast<uintptr_t>(packet.renderable.modelResourcePtr));
  key.sourceHash = bit::fnv1a_iter(
      key.sourceHash,
      reinterpret_cast<uintptr_t>(packet.renderable.runtimeGeosetPtr));
  key.sourceHash = bit::fnv1a_iter(
      key.sourceHash,
      reinterpret_cast<uintptr_t>(packet.renderable.runtimeGeosetDataPtr));
  key.sourceHash = bit::fnv1a_iter(
      key.sourceHash, reinterpret_cast<uintptr_t>(packet.renderable.meshData));
  key.sourceHash = bit::fnv1a_iter(
      key.sourceHash,
      reinterpret_cast<uintptr_t>(packet.renderable.renderablePart));
  key.sourceHash = bit::fnv1a_iter(
      key.sourceHash, uint64_t(packet.resource.geosetIndex));
  key.sourceHash =
      bit::fnv1a_iter(key.sourceHash, packet.material.signatureHash);
  key.sourceHash =
      bit::fnv1a_iter(key.sourceHash, uint32_t(effectiveAlphaMode));
  key.sourceHash = bit::fnv1a_iter(
      key.sourceHash, bit::cast<uint32_t>(alphaCutoutRef));
  if (hasDynamicIndexStream) {
    key.sourceHash =
        bit::fnv1a_iter(key.sourceHash, packet.resource.dynamicIndexHash);
    key.sourceHash = bit::fnv1a_iter(
        key.sourceHash, packet.resource.dynamicPrimitiveBaseIndex);
  }
  key.layoutHash = bit::fnv1a_init();
  key.layoutHash = bit::fnv1a_iter(key.layoutHash, uint32_t(useIndices ? 1u : 0u));
  key.layoutHash = bit::fnv1a_iter(key.layoutHash, uint32_t(effectiveTopology));
  key.layoutHash =
      bit::fnv1a_iter(key.layoutHash, uint32_t(sizeof(float) * 3u));
  key.layoutHash =
      bit::fnv1a_iter(key.layoutHash, uint32_t(VK_FORMAT_R32G32B32_SFLOAT));
  key.layoutHash = bit::fnv1a_iter(key.layoutHash, vertexCount);
  key.layoutHash = bit::fnv1a_iter(
      key.layoutHash, uint32_t(useIndices ? effectiveIndexCount : 0u));
  if (hasDynamicIndexStream)
    key.layoutHash = bit::fnv1a_iter(
        key.layoutHash, packet.resource.dynamicIndexCount);
  key.layoutHash = bit::fnv1a_iter(key.layoutHash, uint32_t(skinned ? 1u : 0u));
  key.layoutHash = bit::fnv1a_iter(
      key.layoutHash, uint32_t(packet.resource.explicitBlendCount));
  key.layoutHash = bit::fnv1a_iter(
      key.layoutHash,
      uint32_t(skinned ? VK_FORMAT_R8G8B8A8_USCALED : VK_FORMAT_UNDEFINED));
  key.mode = skinned ? War3ShadowReplayMode::PaletteSkinnedFF
                     : War3ShadowReplayMode::FixedWorld;

  uint32_t cachedGeometryId = 0u;
  const War3ShadowPersistentGeometry* cachedGeometry = nullptr;
  const bool cachedPersistentGeometry =
      !packet.usesDynamicMeshPositions &&
      War3TryFindShadowPersistentGeometry(key, cachedGeometryId,
                                          cachedGeometry) &&
      cachedGeometry != nullptr;

  Matrix4 sceneNodeWorldMatrix = Matrix4();
  const bool hasSceneNodeWorldMatrix = [&]() {
    if (!packet.usesDynamicMeshPositions || packet.renderable.sceneNode == nullptr)
      return false;

    float raw[12] = {};
    const auto* matrixBase =
        reinterpret_cast<const uint8_t*>(packet.renderable.sceneNode) +
        dxvk::war3::SceneNodeOffsets::WorldMatrix;
    if (!dxvk::war3::IsReadableRange(matrixBase, sizeof(raw)))
      return false;

    std::memcpy(raw, matrixBase, sizeof(raw));
    sceneNodeWorldMatrix =
        Matrix4(Vector4(raw[0], raw[1], raw[2], 0.0f),
                Vector4(raw[3], raw[4], raw[5], 0.0f),
                Vector4(raw[6], raw[7], raw[8], 0.0f),
                Vector4(raw[9], raw[10], raw[11], 1.0f));
    return true;
  }();

  uint32_t paletteIndex = 0u;
  std::vector<std::array<uint8_t, 4>> blendIndices;
  std::vector<std::array<float, 3>> blendWeights;
  uint64_t submittedRuntimeGroupPaletteHash = 0u;
  const bool hasExplicitBlendContract =
      packet.resource.explicitBlendCount != 0u &&
      packetBlendWeights.size() >= size_t(vertexCount) &&
      packetBlendIndices.size() >= size_t(vertexCount);
  bool liveRuntimeGroupPaletteReady = false;
  if (skinned && War3SemanticLivePaletteRefreshRuntime()) {
    m_war3Scene.shadowStats.semanticSceneLivePaletteRefreshAttemptCount++;
    {
      auto livePaletteScope = War3SemanticSubmitScope(
          "War3SemanticScene/SubmitFrame/LivePaletteBuild");
      liveRuntimeGroupPaletteReady = War3TryBuildLiveRuntimeGroupPalette(
          packet.resource, packet.renderable.runtimeModelPtr,
          m_war3ShadowPersistentFrameSerial,
          liveRuntimeGroupPalette, liveMaxVertexGroupSlot,
          liveRuntimeGroupPaletteHash, &liveRuntimeRawPaletteHash,
          &liveRuntimePoseModelPtr);
    }
    if (liveRuntimeGroupPaletteReady) {
      m_war3Scene.shadowStats.semanticSceneLivePaletteRefreshHitCount++;
      m_war3Scene.shadowStats.semanticSceneLivePaletteRefreshLastRuntimeModelPtr =
          reinterpret_cast<uintptr_t>(
              liveRuntimePoseModelPtr != nullptr ? liveRuntimePoseModelPtr
                                                 : packet.renderable.runtimeModelPtr);
      m_war3Scene.shadowStats.semanticSceneLivePaletteRefreshLastMatrixCount =
          uint32_t(liveRuntimeGroupPalette.size());
      m_war3Scene.shadowStats.semanticSceneLivePaletteRefreshLastMatrixHash =
          liveRuntimeGroupPaletteHash;
      War3NoteLivePaletteMotion(
          m_war3Scene.shadowStats,
          liveRuntimePoseModelPtr != nullptr ? liveRuntimePoseModelPtr
                                             : packet.renderable.runtimeModelPtr,
          m_war3ShadowPersistentFrameSerial, liveRuntimeRawPaletteHash,
          liveRuntimeGroupPaletteHash);
    } else {
      m_war3Scene.shadowStats.semanticSceneLivePaletteRefreshMissCount++;
    }
  }
  const std::vector<Matrix4>& effectiveRuntimeGroupPalette =
      liveRuntimeGroupPaletteReady ? liveRuntimeGroupPalette
                                   : packet.runtimeGroupPalette;
  const uint32_t effectiveMaxVertexGroupSlot =
      liveRuntimeGroupPaletteReady ? liveMaxVertexGroupSlot
                                   : packet.maxVertexGroupSlot;
  if (skinned) {
    if ((!liveRuntimeGroupPaletteReady && !packet.hasRuntimeGroupPalette) ||
        effectiveRuntimeGroupPalette.empty() ||
        effectiveRuntimeGroupPalette.size() > 256u ||
        effectiveMaxVertexGroupSlot >= effectiveRuntimeGroupPalette.size() ||
        (!cachedPersistentGeometry && !hasExplicitBlendContract &&
         packetVertexGroups.size() < size_t(vertexCount))) {
      m_war3Scene.shadowStats.semanticSceneRejectedSkinnedContract++;
      return false;
    }

    const uint64_t submittedPaletteHash =
        liveRuntimeGroupPaletteReady && liveRuntimeGroupPaletteHash != 0u
            ? liveRuntimeGroupPaletteHash
            : packet.runtimeGroupPaletteHash != 0u
                  ? packet.runtimeGroupPaletteHash
                  : War3SemanticHashMatrixPalette(
                        effectiveRuntimeGroupPalette.data(),
                        uint32_t(effectiveRuntimeGroupPalette.size()));
    submittedRuntimeGroupPaletteHash = submittedPaletteHash;
    War3NoteSubmittedPaletteMotion(
        m_war3Scene.shadowStats, packet.renderable.runtimeModelPtr,
        m_war3ShadowPersistentFrameSerial, submittedPaletteHash);

    {
      auto paletteIndexScope = War3SemanticSubmitScope(
          "War3SemanticScene/SubmitFrame/PaletteIndex");
      paletteIndex = War3GetOrCreateSemanticShadowPalette(
          packet, resolvedObjectKind,
          liveRuntimeGroupPaletteReady ? effectiveRuntimeGroupPalette.data()
                                       : nullptr,
          liveRuntimeGroupPaletteReady
              ? uint32_t(effectiveRuntimeGroupPalette.size())
              : 0u,
          liveRuntimeGroupPaletteReady ? liveRuntimeGroupPaletteHash : 0u);
    }

    if (hasExplicitBlendContract) {
      auto blendScope = War3SemanticSubmitScope(
          "War3SemanticScene/SubmitFrame/BlendContract");
      if (packetBlendWeights.size() < size_t(vertexCount) ||
          packetBlendIndices.size() < size_t(vertexCount)) {
        m_war3Scene.shadowStats.semanticSceneRejectedSkinnedContract++;
        return false;
      }
      if (!cachedPersistentGeometry) {
        blendIndices.resize(vertexCount);
        blendWeights.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i) {
          blendWeights[i] = packetBlendWeights[i];
          blendIndices[i] = packetBlendIndices[i];
          const uint32_t maxInfluence =
              uint32_t(packet.resource.explicitBlendCount) + 1u;
          for (uint32_t influence = 0u; influence < maxInfluence; ++influence) {
          const uint32_t groupSlot = blendIndices[i][influence];
          if (groupSlot >= effectiveRuntimeGroupPalette.size() ||
              groupSlot >= 256u) {
            m_war3Scene.shadowStats.semanticSceneRejectedSkinnedContract++;
            return false;
            }
          }
        }
      }
    } else if (!cachedPersistentGeometry) {
      auto blendScope = War3SemanticSubmitScope(
          "War3SemanticScene/SubmitFrame/BlendContract");
      blendIndices.resize(vertexCount);
      for (uint32_t i = 0; i < vertexCount; ++i) {
        const uint32_t groupSlot = packetVertexGroups[i];
        if (groupSlot >= effectiveRuntimeGroupPalette.size() ||
            groupSlot >= 256u) {
          m_war3Scene.shadowStats.semanticSceneRejectedSkinnedContract++;
          return false;
        }
        blendIndices[i] = {uint8_t(groupSlot), 0u, 0u, 0u};
      }
    }
  }

  std::vector<float> frameLocalDynamicPositions;
  const std::vector<float>* effectivePositions = &packetPositions;
  if (hasDynamicPositionStream) {
    auto dynamicPositionScope = War3SemanticSubmitScope(
        "War3SemanticScene/SubmitFrame/DynamicPositionCopy");
    frameLocalDynamicPositions.resize(size_t(vertexCount) * 3u);
    const auto* srcBase =
        reinterpret_cast<const uint8_t*>(packet.resource.dynamicPositionStream);
    const size_t srcStride = size_t(packet.resource.dynamicPositionStride);
    for (uint32_t i = 0u; i < vertexCount; ++i) {
      std::memcpy(frameLocalDynamicPositions.data() + size_t(i) * 3u,
                  srcBase + size_t(i) * srcStride,
                  sizeof(float) * 3u);
    }
    effectivePositions = &frameLocalDynamicPositions;
  }

  auto computeLocalBounds = [&](Vector4& outCenter, float& outRadius) {
    outCenter = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
    outRadius = 0.0f;
    if (effectivePositions->size() < 3u)
      return;

    float minX = (*effectivePositions)[0];
    float minY = (*effectivePositions)[1];
    float minZ = (*effectivePositions)[2];
    float maxX = minX;
    float maxY = minY;
    float maxZ = minZ;

    for (uint32_t i = 0u; i < vertexCount; ++i) {
      const size_t base = size_t(i) * 3u;
      const float x = (*effectivePositions)[base + 0u];
      const float y = (*effectivePositions)[base + 1u];
      const float z = (*effectivePositions)[base + 2u];
      minX = std::min(minX, x);
      minY = std::min(minY, y);
      minZ = std::min(minZ, z);
      maxX = std::max(maxX, x);
      maxY = std::max(maxY, y);
      maxZ = std::max(maxZ, z);
    }

    outCenter = Vector4((minX + maxX) * 0.5f, (minY + maxY) * 0.5f,
                        (minZ + maxZ) * 0.5f, 1.0f);
    float radiusSq = 0.0f;
    for (uint32_t i = 0u; i < vertexCount; ++i) {
      const size_t base = size_t(i) * 3u;
      const float dx = (*effectivePositions)[base + 0u] - outCenter.x;
      const float dy = (*effectivePositions)[base + 1u] - outCenter.y;
      const float dz = (*effectivePositions)[base + 2u] - outCenter.z;
      radiusSq = std::max(radiusSq, dx * dx + dy * dy + dz * dz);
    }
    outRadius = std::sqrt(radiusSq);
  };

  War3ShadowPersistentGeometry candidate = {};
  candidate.key = key;
  candidate.indexed = useIndices;
  candidate.positionStride = sizeof(float) * 3u;
  candidate.positionOffset = 0u;
  candidate.positionFormat = VK_FORMAT_R32G32B32_SFLOAT;
  candidate.indexType = VK_INDEX_TYPE_UINT16;
  candidate.vertexBlendEnabled = skinned;
  candidate.vertexBlendIndexed = skinned;
  candidate.vertexBlendCount =
      skinned ? packet.resource.explicitBlendCount : 0u;
  candidate.blendWeightOffset = 0u;
  candidate.blendWeightFormat =
      skinned && hasExplicitBlendContract ? VK_FORMAT_R32G32B32_SFLOAT
                                          : VK_FORMAT_UNDEFINED;
  candidate.blendIndexOffset =
      skinned && hasExplicitBlendContract ? 12u : 0u;
  candidate.blendIndexFormat =
      skinned ? VK_FORMAT_R8G8B8A8_USCALED : VK_FORMAT_UNDEFINED;
  candidate.blendStride =
      skinned ? (hasExplicitBlendContract ? 16u : 4u) : 0u;
  candidate.blendBinding = skinned ? 1u : 0u;
  candidate.alphaTestEnabled = alphaCutoutEnabled;
  candidate.alphaRef = alphaCutoutRef;
  candidate.topology = effectiveTopology;
  candidate.indexCount =
      useIndices ? effectiveIndexCount : 0u;
  candidate.firstIndex = 0u;
  candidate.vertexOffset = 0;
  candidate.vertexCount = useIndices ? 0u : vertexCount;
  candidate.firstVertex = 0u;
  candidate.minVertexIndex = 0u;
  candidate.numVertices = vertexCount;

  std::array<War3ShadowPersistentUpload, 4> uploads = {};
  uploads[0].hostData = effectivePositions->data();
  uploads[0].bytes =
      VkDeviceSize(effectivePositions->size() * sizeof(float));
  uploads[0].usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  uploads[0].debugName = "War3SemanticShadowPos";
  if (useIndices) {
    uploads[1].hostData = effectiveIndexData;
    uploads[1].bytes =
        VkDeviceSize(size_t(effectiveIndexCount) * sizeof(uint16_t));
    uploads[1].usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    uploads[1].debugName = "War3SemanticShadowIdx";
  }
  struct SemanticBlendVertex {
    float weights[3];
    uint8_t indices[4];
  };
  std::vector<SemanticBlendVertex> blendVertices;
  if (skinned && !blendIndices.empty()) {
    if (hasExplicitBlendContract) {
      blendVertices.resize(vertexCount);
      for (uint32_t i = 0u; i < vertexCount; ++i) {
        blendVertices[i].weights[0] = blendWeights[i][0];
        blendVertices[i].weights[1] = blendWeights[i][1];
        blendVertices[i].weights[2] = blendWeights[i][2];
        blendVertices[i].indices[0] = blendIndices[i][0];
        blendVertices[i].indices[1] = blendIndices[i][1];
        blendVertices[i].indices[2] = blendIndices[i][2];
        blendVertices[i].indices[3] = blendIndices[i][3];
      }
      uploads[2].hostData = blendVertices.data();
      uploads[2].bytes =
          VkDeviceSize(blendVertices.size() * sizeof(blendVertices[0]));
    } else {
      uploads[2].hostData = blendIndices.data();
      uploads[2].bytes =
          VkDeviceSize(blendIndices.size() * sizeof(blendIndices[0]));
    }
    uploads[2].usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    uploads[2].debugName = "War3SemanticShadowBlend";
  }

  uint32_t geometryId = 0u;
  const War3ShadowPersistentGeometry* geometry = nullptr;
  bool createdNewGeometry = false;
  const bool frameLocalDynamicGeometry = packet.usesDynamicMeshPositions;
  if (frameLocalDynamicGeometry &&
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
      !dxvk::war3::internal::kShadowSemanticCoreAllowFrameLocalDynamicGeometry) {
    static std::atomic<uint32_t> s_frameLocalRejectLogCount{0u};
    const uint32_t logIndex =
        s_frameLocalRejectLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (logIndex < 16u || (logIndex % 512u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK SemanticShadow: reject frame-local dynamic geometry "
          "runtime=%p model=%p geoIdx=%u handle=%u raw=%08X scene=%p mesh=%p\n",
          packet.renderable.runtimeModelPtr, packet.resource.modelResourcePtr,
          static_cast<unsigned>(packet.resource.geosetIndex),
          static_cast<unsigned>(packet.renderable.jHandle),
          static_cast<unsigned>(packet.renderable.rawcode),
          packet.renderable.sceneNode, packet.renderable.meshData);
    }
    m_war3Scene.shadowStats.semanticSceneRejectedGeometry++;
    m_war3Scene.shadowStats.semanticSceneRejectedGeometryFrameLocal++;
    return false;
  }
  War3ShadowPersistentGeometry frameLocalGeometry = {};
  {
  auto geometryScope = War3SemanticSubmitScope(
      "War3SemanticScene/SubmitFrame/GeometryLookupCreate");
  if (frameLocalDynamicGeometry) {
    computeLocalBounds(candidate.localBoundsCenter, candidate.localBoundsRadius);
    auto uploadFrameLocalBuffer =
        [this](const War3ShadowPersistentUpload& upload,
               Rc<DxvkBuffer>& outStorage,
               DxvkResourceBufferInfo& outInfo) -> bool {
      if (upload.bytes == 0)
        return true;

      VkDeviceSize uploadOffset = 0;
      void* mapPtr = nullptr;
      auto storage =
          War3AllocFreezeBuffer(upload.bytes, uploadOffset, true, &mapPtr);
      if (storage == nullptr || mapPtr == nullptr)
        return false;

      if (upload.hostData != nullptr) {
        std::memcpy(mapPtr, upload.hostData, size_t(upload.bytes));
      } else {
        auto srcBuffer = upload.slice.buffer();
        if (srcBuffer == nullptr)
          return false;
        void* srcPtr = srcBuffer->mapPtr(upload.slice.offset());
        if (srcPtr == nullptr)
          return false;
        std::memcpy(mapPtr, srcPtr, size_t(upload.bytes));
      }

      outStorage = storage;
      outInfo = storage->getSliceInfo(uploadOffset, upload.bytes);
      return true;
    };

    frameLocalGeometry = candidate;
    frameLocalGeometry.key = key;
    frameLocalGeometry.totalBytes = 0u;
    frameLocalGeometry.lastSeenFrame = m_war3ShadowPersistentFrameSerial;
    for (const auto& upload : uploads)
      frameLocalGeometry.totalBytes += War3AlignPersistentBytes(upload.bytes);

    if (!uploadFrameLocalBuffer(uploads[0], frameLocalGeometry.positionStorage,
                                frameLocalGeometry.positionInfo) ||
        !uploadFrameLocalBuffer(uploads[1], frameLocalGeometry.indexStorage,
                                frameLocalGeometry.indexInfo) ||
        !uploadFrameLocalBuffer(uploads[2], frameLocalGeometry.blendStorage,
                                frameLocalGeometry.blendInfo) ||
        !uploadFrameLocalBuffer(uploads[3], frameLocalGeometry.uvStorage,
                                frameLocalGeometry.uvInfo)) {
      m_war3Scene.shadowStats.semanticSceneRejectedGeometry++;
      m_war3Scene.shadowStats.semanticSceneRejectedGeometryFrameLocal++;
      return false;
    }

    geometry = &frameLocalGeometry;
  } else if (cachedPersistentGeometry) {
    geometryId = cachedGeometryId;
    geometry = cachedGeometry;
  } else {
    if (!War3TryFindShadowPersistentGeometry(key, geometryId, geometry)) {
      computeLocalBounds(candidate.localBoundsCenter, candidate.localBoundsRadius);
      if (!War3FindOrCreateShadowPersistentGeometry(key, candidate, uploads,
                                                    geometryId, geometry,
                                                    createdNewGeometry) ||
          geometry == nullptr) {
        m_war3Scene.shadowStats.semanticSceneRejectedGeometry++;
        m_war3Scene.shadowStats.semanticSceneRejectedGeometryPersistent++;
          return false;
      }
    }
  }
  }

  auto resolveWorldMatrix = [&]() {
    if (skinned)
      return Matrix4();
    if (packet.usesDynamicMeshPositions && hasSceneNodeWorldMatrix)
      return sceneNodeWorldMatrix;
    if (packet.pose.hasWorldTransform)
      return packet.pose.worldTransform;
    if (!packet.pose.matrixPalette.empty())
      return packet.pose.matrixPalette[0];
    return Matrix4();
  };

  auto drawBuildScope = War3SemanticSubmitScope(
      "War3SemanticScene/SubmitFrame/DrawBuild");
  War3ShadowCasterDraw draw = {};
  draw.indexed = geometry->indexed;
  draw.positionStorage = geometry->positionStorage;
  draw.positionInfo = geometry->positionInfo;
  draw.positionStride = geometry->positionStride;
  draw.positionOffset = geometry->positionOffset;
  draw.positionFormat = geometry->positionFormat;
  draw.topology = geometry->topology;
  draw.worldMatrix = resolveWorldMatrix();
  draw.vertexBlendEnabled = geometry->vertexBlendEnabled;
  draw.vertexBlendIndexed = geometry->vertexBlendIndexed;
  draw.vertexBlendCount = geometry->vertexBlendCount;
  draw.paletteIndex = paletteIndex;
  draw.blendWeightOffset = geometry->blendWeightOffset;
  draw.blendWeightFormat = geometry->blendWeightFormat;
  draw.blendIndexOffset = geometry->blendIndexOffset;
  draw.blendIndexFormat = geometry->blendIndexFormat;
  draw.blendBinding = geometry->blendBinding;
  draw.blendStride = geometry->blendStride;
  draw.alphaTestEnabled = geometry->alphaTestEnabled;
  draw.alphaRef = geometry->alphaRef;
  draw.alphaBlendEnabled = alphaBlendEnabled;
  draw.depthWriteEnabled = true;
  draw.depthTestEnabled = true;
  draw.additiveBlend = false;
  draw.uvStride = 0u;
  draw.uvOffset = 0u;
  draw.uvFormat = VK_FORMAT_UNDEFINED;
  if (geometry->indexed) {
    draw.indexStorage = geometry->indexStorage;
    draw.indexInfo = geometry->indexInfo;
    draw.indexType = geometry->indexType;
    draw.indexCount = geometry->indexCount;
    draw.firstIndex = geometry->firstIndex;
    draw.vertexOffset = geometry->vertexOffset;
    draw.vertexCount = 0u;
    draw.firstVertex = 0u;
    draw.minVertexIndex = geometry->minVertexIndex;
    draw.numVertices = geometry->numVertices;
  } else {
    draw.indexCount = 0u;
    draw.firstIndex = 0u;
    draw.vertexOffset = 0;
    draw.vertexCount = geometry->vertexCount;
    draw.firstVertex = geometry->firstVertex;
    draw.minVertexIndex = geometry->minVertexIndex;
    draw.numVertices = geometry->numVertices;
  }
  if (geometry->blendBinding == 1u) {
    draw.blendStorage = geometry->blendStorage;
    draw.blendInfo = geometry->blendInfo;
  }

  draw.category = War3RenderState::StageCategory::WorldObject;
  draw.batchTag = War3BatchTag::Unknown;
  draw.batchHandle = War3NormalizeShadowHandle(packet.renderable.jHandle);
  draw.objectKind = static_cast<uint8_t>(resolvedObjectKind);

  const bool unitLikeObject = War3IsSemanticUnitObject(resolvedObjectKind);
  {
  auto boundsScope = War3SemanticSubmitScope(
      "War3SemanticScene/SubmitFrame/Bounds");
  const Matrix4* semanticBoundsMatrix = nullptr;
  if (skinned) {
    if (packet.pose.hasWorldTransform)
      semanticBoundsMatrix = &packet.pose.worldTransform;
    else if (!packet.pose.matrixPalette.empty())
      semanticBoundsMatrix = &packet.pose.matrixPalette[0];
    else if (liveRuntimeGroupPaletteReady && !liveRuntimeGroupPalette.empty())
      semanticBoundsMatrix = &liveRuntimeGroupPalette[0];
    else if (!packet.runtimeGroupPalette.empty())
      semanticBoundsMatrix = &packet.runtimeGroupPalette[0];
  } else {
    semanticBoundsMatrix = &draw.worldMatrix;
  }

  const Vector4 localBoundsCenter =
      geometry != nullptr ? geometry->localBoundsCenter
                          : candidate.localBoundsCenter;
  const float localBoundsRadius =
      geometry != nullptr ? geometry->localBoundsRadius
                          : candidate.localBoundsRadius;

  if (semanticBoundsMatrix != nullptr && localBoundsRadius > 0.0f) {
    War3ApplySemanticBoundsFromMatrix(draw, *semanticBoundsMatrix,
                                      localBoundsCenter,
                                      localBoundsRadius);
  } else {
    const float baseRadius =
        War3SemanticBoundsRadiusForObjectKind(draw.objectKind);
    if (baseRadius > 0.0f) {
      const Matrix4* boundsMatrix =
          semanticBoundsMatrix != nullptr ? semanticBoundsMatrix : &draw.worldMatrix;
      War3ApplySemanticBoundsFromMatrix(draw, *boundsMatrix,
                                        localBoundsCenter,
                                        baseRadius);
    }
  }

  if (unitLikeObject && skinned && !(draw.boundsRadius > 0.0f)) {
    float baseRadius =
        War3SemanticBoundsRadiusForObjectKind(draw.objectKind);
    if (baseRadius <= 0.0f)
      baseRadius = 260.0f;
    if (localBoundsRadius > baseRadius)
      baseRadius = localBoundsRadius;

    if (semanticBoundsMatrix != nullptr) {
      War3ApplySemanticBoundsFromMatrix(draw, *semanticBoundsMatrix,
                                        localBoundsCenter,
                                        baseRadius);
    } else {
      draw.boundsCenter = War3SemanticBoundsTranslation(draw.worldMatrix);
      draw.boundsRadius =
          baseRadius * War3SemanticBoundsMaxScale(draw.worldMatrix);
    }
  }
  }

  War3ShadowInstanceRef instance = {};
  instance.geometryId = geometryId;
  instance.materialId = uint32_t(packet.material.signatureHash & 0xFFFFFFFFu);
  instance.replayDrawIndex =
      static_cast<uint32_t>(m_war3Scene.shadowCasters.size());
  instance.batchHandle = draw.batchHandle;
  instance.paletteIndex = draw.paletteIndex;
  instance.worldMatrix = draw.worldMatrix;
  instance.boundsCenter = draw.boundsCenter;
  instance.boundsRadius = draw.boundsRadius;
  instance.category = draw.category;
  instance.batchTag = draw.batchTag;
  instance.objectKind = draw.objectKind;
  instance.mode = key.mode;

  const bool dynamicSemanticCaster =
      skinned || packet.usesDynamicMeshPositions || unitLikeObject;
  if (dynamicSemanticCaster) {
    m_war3Scene.shadowStats.dynamicPoseCount++;
    if (skinned)
      m_war3Scene.shadowStats.dynamicSkinnedOutputCount++;

    uint64_t dynamicHash = bit::fnv1a_init();
    dynamicHash = bit::fnv1a_iter(
        dynamicHash,
        reinterpret_cast<uintptr_t>(packet.renderable.runtimeModelPtr));
    dynamicHash = bit::fnv1a_iter(dynamicHash, effectiveModelKey);
    dynamicHash = bit::fnv1a_iter(dynamicHash, packet.renderable.jHandle);
    if (skinned || packet.usesDynamicMeshPositions ||
        packet.pose.matrixHash != 0u || packet.pose.matrixCount != 0u) {
      dynamicHash = bit::fnv1a_iter(dynamicHash, packet.pose.matrixCount);
      dynamicHash = bit::fnv1a_iter(dynamicHash, packet.pose.matrixHash);
      for (uint32_t i = 0u; i < 4u; ++i) {
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, bit::cast<uint32_t>(draw.worldMatrix[i].x));
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, bit::cast<uint32_t>(draw.worldMatrix[i].y));
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, bit::cast<uint32_t>(draw.worldMatrix[i].z));
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, bit::cast<uint32_t>(draw.worldMatrix[i].w));
      }
      dynamicHash = bit::fnv1a_iter(
          dynamicHash, bit::cast<uint32_t>(draw.boundsCenter.x));
      dynamicHash = bit::fnv1a_iter(
          dynamicHash, bit::cast<uint32_t>(draw.boundsCenter.y));
      dynamicHash = bit::fnv1a_iter(
          dynamicHash, bit::cast<uint32_t>(draw.boundsCenter.z));
      dynamicHash = bit::fnv1a_iter(
          dynamicHash, bit::cast<uint32_t>(draw.boundsRadius));
      if (skinned) {
        dynamicHash =
            bit::fnv1a_iter(dynamicHash, submittedRuntimeGroupPaletteHash);
        dynamicHash =
            bit::fnv1a_iter(dynamicHash, uint32_t(effectiveRuntimeGroupPalette.size()));
        dynamicHash = bit::fnv1a_iter(dynamicHash, effectiveMaxVertexGroupSlot);
      }
      if (packet.usesDynamicMeshPositions) {
        dynamicHash =
            bit::fnv1a_iter(dynamicHash, packet.resource.contentHash);
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, packet.resource.dynamicIndexHash);
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, packet.resource.dynamicPrimitiveBaseIndex);
      }
      if (liveRuntimeGroupPaletteReady)
        dynamicHash = bit::fnv1a_iter(dynamicHash, liveRuntimeGroupPaletteHash);
    } else {
      for (uint32_t i = 0u; i < 4u; ++i) {
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, bit::cast<uint32_t>(draw.worldMatrix[i].x));
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, bit::cast<uint32_t>(draw.worldMatrix[i].y));
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, bit::cast<uint32_t>(draw.worldMatrix[i].z));
        dynamicHash = bit::fnv1a_iter(
            dynamicHash, bit::cast<uint32_t>(draw.worldMatrix[i].w));
      }
    }

    if (m_war3Scene.shadowStats.dynamicPoseSignature == 0u)
      m_war3Scene.shadowStats.dynamicPoseSignature = dynamicHash;
    else
      m_war3Scene.shadowStats.dynamicPoseSignature = bit::fnv1a_iter(
          m_war3Scene.shadowStats.dynamicPoseSignature, dynamicHash);
  }

  m_war3Scene.shadowInstances.emplace_back(std::move(instance));
  m_war3Scene.shadowCasters.emplace_back(std::move(draw));
  m_war3Scene.shadowStats.captured++;
  if (useIndices)
    m_war3Scene.shadowStats.capturedIndexed++;
  else
    m_war3Scene.shadowStats.capturedNonIndexed++;
  m_war3Scene.shadowStats.capturedWorldObject++;
  m_war3Scene.shadowStats.semanticSceneSubmitted++;
  if (unitLikeObject) {
    m_war3Scene.shadowStats.capturedUnitObject++;
    m_war3Scene.shadowStats.persistentUnitInstanceCount++;
    m_war3Scene.shadowStats.semanticSceneSubmittedUnit++;
    if (skinned)
      m_war3Scene.shadowStats.capturedUnitVertexBlend++;
  }
  if (skinned)
    m_war3Scene.shadowStats.semanticSceneSubmittedSkinned++;
  if (!unitLikeObject && !skinned &&
      resolvedObjectKind == dxvk::war3::render::ObjectKind::Unknown &&
      packet.renderable.worldObjectEntry != nullptr &&
      packet.renderable.sceneNode != nullptr &&
      packet.pose.hasWorldTransform) {
    m_war3Scene.shadowStats.semanticSceneAcceptedExplicitResourceOwnerRigid++;
  }
  m_war3Scene.shadowStats.persistentInstanceCount++;

  if (!unitLikeObject ||
      resolvedObjectKind == dxvk::war3::render::ObjectKind::Building ||
      resolvedObjectKind ==
          dxvk::war3::render::ObjectKind::Destructible) {
    m_war3Scene.shadowStats.staticPersistentCount++;
  }

  if (!frameLocalDynamicGeometry) {
    if (createdNewGeometry) {
      m_war3Scene.shadowStats.persistentGeometryCount++;
      m_war3Scene.shadowStats.uniqueGeometryCount++;
      m_war3Scene.shadowPersistentPool.promotedThisFrame++;
    } else {
      m_war3Scene.shadowStats.duplicateGeometryInstances++;
      m_war3Scene.shadowStats.reuseEligibleDuplicates++;
      m_war3Scene.shadowStats.potentialFreezeReuseHits++;
      m_war3Scene.shadowStats.instancedGeometryDrawsSaved++;
    }
  } else {
    m_war3Scene.shadowStats.semanticSceneSubmittedFrameLocal++;
  }

  m_war3Scene.shadowPersistentPool.bytesUsed = m_war3ShadowPersistentBytesUsed;
  m_war3Scene.shadowPersistentPool.bytesEvicted =
      m_war3ShadowPersistentBytesEvicted;
  m_war3Scene.shadowPersistentPool.liveGeometryCount =
      static_cast<uint32_t>(m_war3ShadowPersistentGeometries.size());
  m_war3Scene.shadowStats.persistentPoolBytesUsed =
      m_war3ShadowPersistentBytesUsed;
  m_war3Scene.shadowStats.persistentPoolBytesEvicted =
      m_war3ShadowPersistentBytesEvicted;
  return true;
}

uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(
    bool unitsOnly,
    bool executeNativeBackendValidation) {
  m_war3Scene.shadowStats.semanticScenePopulateAttemptCount++;
  if (unitsOnly)
    m_war3Scene.shadowStats.semanticScenePopulateUnitsOnlyCount++;

  if (!War3SemanticConsumerEnabled() ||
      !dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled())
    return 0u;
  const bool semanticRuntimeReady =
      dxvk::war3::War3Events::get().isGameStarted() ||
      (dxvk::war3::War3Events::get().isJassReady() &&
       dxvk::war3::internal::IsSemanticShadowPreReadyValidationRuntimeEnabled());
  if (!semanticRuntimeReady)
    return 0u;
  auto populateScope =
      war3::War3PerfMonitor::instance().cpuScope("War3SemanticScene/Populate");
  struct SemanticConsumerPerfScope {
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    ~SemanticConsumerPerfScope() {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      dxvk::war3::render::NoteSemanticDataPerf(
          dxvk::war3::render::SemanticDataPerfTag::ConsumerBuild,
          elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0u);
    }
  } semanticConsumerPerf;

  if (!War3SemanticContractCaptureEnabled()) {
    return 0u;
  }
  const bool semanticSceneHasSubmittedOnce =
      m_war3SemanticSceneLastSuccessfulSubmitFrameSerial != 0u;
  const auto semanticSceneHasReusableSkinnedFrame = [&]() {
    if (m_war3Scene.shadowStats.semanticSceneLastInputSkinnedCount != 0u)
      return true;
    if (!m_war3SemanticSceneLastReusableFrame ||
        m_war3SemanticSceneLastReusableFrame->frameSerial == 0u ||
        m_war3SemanticSceneLastReusableFrame->draws.empty())
      return false;
    for (const auto& draw : m_war3SemanticSceneLastReusableFrame->draws) {
      if (draw.path == dxvk::war3::shadow::ShadowDrawPath::Skinned)
        return true;
    }
    return false;
  };
  const bool semanticLivePaletteSubmitNeeded =
      War3SemanticLivePaletteRefreshRuntime() &&
      semanticSceneHasReusableSkinnedFrame();
  const uint64_t kSceneContractCaptureSteadyFramePeriod =
      War3SemanticContractCapturePeriodRuntime();
  dxvk::war3::shadow::ShadowPublishedContractBundle sceneBundle = {};
  // The semantic scene may be queried more than once before a DXVK frame is
  // presented. Capturing the whole live contract on every boundary was the main
  // source of the semantic.data stall, so make capture single-flight per host
  // frame and let the core reuse the last completed/pending contract.
  bool shouldCaptureContract =
      m_war3SemanticSceneLastCaptureFrameSerial !=
      m_war3ShadowPersistentFrameSerial;
  const bool semanticSceneHasCapturedContract =
      m_war3SemanticSceneLastCapturePublishRevision != 0u;
  const bool semanticDrawTimePoseDirty =
      m_war3SemanticDrawTimePoseDirtyFrameSerial ==
      m_war3ShadowPersistentFrameSerial;
  const uint64_t matrixPublisherPoseRevision =
      dxvk::war3::model::RuntimeMatrixPublisherPoseRevision();
  const bool semanticMatrixPublisherPoseDirty =
      matrixPublisherPoseRevision != 0u &&
      matrixPublisherPoseRevision !=
          m_war3SemanticLastMatrixPublisherPoseRevision;
  bool shouldCapturePoseOnlyContract = false;
  if (shouldCaptureContract &&
      (semanticSceneHasSubmittedOnce || semanticSceneHasCapturedContract)) {
    const bool capturePeriodElapsed =
        m_war3SemanticSceneLastCaptureFrameSerial == 0u ||
        m_war3ShadowPersistentFrameSerial >=
            m_war3SemanticSceneLastCaptureFrameSerial +
                kSceneContractCaptureSteadyFramePeriod;
    // Once packets exist, static topology can be refreshed periodically, but
    // War3's final matrix publisher is the animated pose source for skinned
    // semantic units. When it advances, capture the contract this frame instead
    // of waiting for the steady-state period; otherwise shadows lock to an
    // old/initial pose for ~DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD.
    shouldCapturePoseOnlyContract =
        semanticMatrixPublisherPoseDirty && semanticSceneHasCapturedContract &&
        !capturePeriodElapsed;
    shouldCaptureContract =
        semanticMatrixPublisherPoseDirty || semanticDrawTimePoseDirty ||
        capturePeriodElapsed ||
        m_war3SemanticSceneLastCapturePublishRevision == 0u;
  }
  if (shouldCaptureContract) {
    auto captureScope = war3::War3PerfMonitor::instance().cpuScope(
        "War3SemanticScene/CaptureContract");
    if (War3SemanticPublishRegistriesBeforeSceneRuntime())
      War3PublishSemanticRegistriesForScene();
    auto& contractCache =
        dxvk::war3::shadow::ShadowRuntimeContractCache::instance();
    if (shouldCapturePoseOnlyContract)
      contractCache.capturePoseOnlyLiveState();
    else
      contractCache.captureLiveState();
    m_war3SemanticSceneLastCaptureFrameSerial =
        m_war3ShadowPersistentFrameSerial;
    sceneBundle =
        contractCache.snapshotBundleShared();
    m_war3SemanticSceneLastCapturePublishRevision =
        (sceneBundle.valid() && sceneBundle.manifest != nullptr)
            ? sceneBundle.manifest->publishRevision
            : 0u;
    m_war3SemanticLastMatrixPublisherPoseRevision =
        matrixPublisherPoseRevision;
  }
  if (!sceneBundle.valid()) {
    auto snapshotScope = war3::War3PerfMonitor::instance().cpuScope(
        "War3SemanticScene/SnapshotBundle");
    sceneBundle = dxvk::war3::shadow::ShadowRuntimeContractCache::instance()
                      .snapshotBundleShared();
  }
  if (m_war3SemanticSceneLastSuccessfulSubmitFrameSerial ==
          m_war3ShadowPersistentFrameSerial &&
      m_war3SemanticSceneLastSuccessfulSubmitPublishRevision != 0u &&
      m_war3SemanticSceneLastSuccessfulSubmitPublishRevision ==
          m_war3SemanticSceneLastCapturePublishRevision &&
      m_war3SemanticSceneLastSuccessfulSubmitComplete &&
      m_war3SemanticSceneLastSuccessfulSubmitUnitsOnly == unitsOnly &&
      (!executeNativeBackendValidation ||
       m_war3SemanticSceneLastSuccessfulSubmitNativeValidation) &&
      !semanticLivePaletteSubmitNeeded) {
    return 0u;
  }
  auto& semanticRuntime =
      dxvk::war3::shadow::ShadowValidationRuntime::instance();
  auto supplementedBundle = sceneBundle;
  std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame>
      preferredSupplementedFrame;
  uint64_t preferredSupplementedRevision = 0u;
  if (supplementedBundle.valid() && supplementedBundle.manifest != nullptr) {
    const uint64_t desiredRevision =
        supplementedBundle.manifest->publishRevision;
    const uint64_t desiredFrameSerial =
        supplementedBundle.manifest->frameSerial;
    preferredSupplementedRevision = desiredRevision;
    // The preview manifest is capped to a small render-thread budget. Consume
    // a few chunks here so the first semantic frame can progress before the
    // scene submit gate judges the latest revision as still pending.
    //
    // Once the semantic path has produced a complete frame, keep this path
    // incremental. Re-running several build chunks every BeforeUi frame was
    // showing up as "UntrackedActive" CPU and could stall low-pressure maps even
    // when SubmitFrame itself was single-flight.
    constexpr uint32_t kSceneSupplementedBuildBootstrapPasses = 4u;
    constexpr uint32_t kSceneSupplementedBuildSteadyPasses = 2u;
    constexpr uint64_t kSceneSupplementedBuildSteadyFramePeriod = 1u;
    bool shouldProgressSteadyBuild = !semanticSceneHasSubmittedOnce;
    if (semanticSceneHasSubmittedOnce) {
      const auto currentFrame = semanticRuntime.snapshotFrameShared();
      const auto currentBuildState = semanticRuntime.buildStateSnapshot();
      const uint64_t currentFrameRevision =
          currentFrame ? currentFrame->sourcePublishRevision : 0u;
      const uint64_t currentFrameSerial =
          currentFrame ? currentFrame->frameSerial : 0u;
      const bool currentFrameFresh =
          currentFrame && currentFrame->frameSerial != 0u &&
          !currentFrame->draws.empty() &&
          currentFrame->sourcePublishRevision >= desiredRevision &&
          currentFrameSerial >= desiredFrameSerial;
      const bool currentFramePoseStale =
          desiredFrameSerial != 0u &&
          (currentFrameSerial == 0u ||
           currentFrameSerial < desiredFrameSerial);
      // After the first successful semantic scene submit we still need to drain
      // the chunked build queue. Otherwise the renderer keeps reusing an old
      // completed frame forever, which makes multiple current casters receive a
      // stale caster's shadow silhouette. Skinned animation freshness now comes
      // from PoseRegistry palettes published by War3's source-range matrix
      // producer, not from raw CModel fallback, so stale packet topology may be
      // reused only while fresh palettes are available. Keep the drain periodic
      // to avoid turning freshness into a build storm.
      const bool steadyBuildPeriodElapsed =
          m_war3SemanticSceneLastSteadyBuildFrameSerial == 0u ||
          m_war3ShadowPersistentFrameSerial >=
              m_war3SemanticSceneLastSteadyBuildFrameSerial +
                  kSceneSupplementedBuildSteadyFramePeriod;
      shouldProgressSteadyBuild =
          (!currentFrameFresh || currentFramePoseStale) &&
          (currentBuildState.buildInProgress ||
           currentBuildState.buildRequestPending ||
           desiredRevision > currentFrameRevision ||
           currentFramePoseStale) &&
          steadyBuildPeriodElapsed;
    }
    const uint32_t sceneSupplementedBuildMaxPasses =
        semanticSceneHasSubmittedOnce
            ? (shouldProgressSteadyBuild ? kSceneSupplementedBuildSteadyPasses
                                         : 0u)
            : kSceneSupplementedBuildBootstrapPasses;
    if (semanticSceneHasSubmittedOnce && sceneSupplementedBuildMaxPasses != 0u)
      m_war3SemanticSceneLastSteadyBuildFrameSerial =
          m_war3ShadowPersistentFrameSerial;
    constexpr uint64_t kSceneSupplementedBuildRecordCeiling = 2048u;
    auto supplementedBuildScope =
        sceneSupplementedBuildMaxPasses != 0u
            ? war3::War3PerfMonitor::instance().cpuScope(
                  "War3SemanticScene/SupplementedBuild")
            : war3::War3PerfMonitor::ScopedCpuScope{};
    for (uint32_t i = 0u; i < sceneSupplementedBuildMaxPasses; ++i) {
      semanticRuntime.ensureFrameBuiltForContract(
          supplementedBundle.manifest, supplementedBundle.resources,
          supplementedBundle.poses, supplementedBundle.attachments);
      const auto supplementedStats = semanticRuntime.snapshot();
      auto candidateFrame = semanticRuntime.snapshotFrameShared();
      if (supplementedStats.sourcePublishRevision == desiredRevision &&
          supplementedStats.frameSerial >= desiredFrameSerial) {
        if (candidateFrame && candidateFrame->frameSerial != 0u &&
            !candidateFrame->draws.empty() &&
            candidateFrame->sourcePublishRevision == desiredRevision &&
            candidateFrame->frameSerial >= desiredFrameSerial) {
          preferredSupplementedFrame = std::move(candidateFrame);
        }
        break;
      }
      const auto supplementedBuildState =
          semanticRuntime.buildStateSnapshot();
      if (!supplementedBuildState.buildInProgress &&
          !supplementedBuildState.buildRequestPending)
        break;
      if (supplementedBuildState.buildInProgress &&
          supplementedBuildState.buildRecordCount >
              kSceneSupplementedBuildRecordCeiling) {
        break;
      }
    }
  }
  dxvk::war3::shadow::ShadowValidationFrameStats stats = {};
  dxvk::war3::shadow::ShadowValidationBuildState buildState = {};
  std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame> latestFrame;
  {
    auto stateScope = war3::War3PerfMonitor::instance().cpuScope(
        "War3SemanticScene/RequestBuildAndSnapshot");
    if (supplementedBundle.valid()) {
      semanticRuntime.requestFrameBuildForContract(
          supplementedBundle.manifest, supplementedBundle.resources,
          supplementedBundle.poses, supplementedBundle.attachments);
    } else {
      semanticRuntime.requestLatestFrameBuild();
    }
    stats = semanticRuntime.snapshot();
    buildState = semanticRuntime.buildStateSnapshot();
    latestFrame = semanticRuntime.snapshotFrameShared();
  }
  auto semanticCatchupTargetRevision = [&]() {
    uint64_t target = stats.sourcePublishRevision;
    if (buildState.buildInProgress)
      target = std::max(target, buildState.buildPublishRevision);
    if (buildState.buildRequestPending)
      target = std::max(target, buildState.pendingPublishRevision);
    if (preferredSupplementedRevision != 0u)
      target = std::max(target, preferredSupplementedRevision);
    return target;
  };
  auto semanticCatchupTargetFrameSerial = [&]() {
    uint64_t target = stats.frameSerial;
    if (buildState.buildInProgress)
      target = std::max(target, buildState.buildFrameSerial);
    if (buildState.buildRequestPending)
      target = std::max(target, buildState.pendingFrameSerial);
    if (supplementedBundle.valid() && supplementedBundle.manifest != nullptr)
      target = std::max(target, supplementedBundle.manifest->frameSerial);
    return target;
  };
  auto semanticFrameSafeForCurrentTarget =
      [&](const std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame>&
              candidate,
          bool allowNearSteadyReuse) {
        if (candidate == nullptr || candidate->frameSerial == 0u ||
            candidate->draws.empty())
          return false;

        const uint64_t targetRevision = semanticCatchupTargetRevision();
        const uint64_t targetFrameSerial = semanticCatchupTargetFrameSerial();
        const bool currentCoreEmpty =
            stats.sourcePublishRevision != 0u && stats.drawPacketCount == 0u &&
            stats.resolve.considered == 0u;
        if (currentCoreEmpty)
          return allowNearSteadyReuse && semanticSceneHasSubmittedOnce;
        if (stats.drawPacketCount != 0u &&
            candidate->draws.size() > stats.drawPacketCount) {
          return false;
        }

        if (targetRevision != 0u &&
            candidate->sourcePublishRevision < targetRevision) {
          if (!allowNearSteadyReuse)
            return false;
          const uint64_t revisionGap =
              targetRevision - candidate->sourcePublishRevision;
          if (revisionGap > 2u)
            return false;
        }
        if (targetFrameSerial != 0u &&
            candidate->frameSerial < targetFrameSerial) {
          const uint64_t frameGap = targetFrameSerial - candidate->frameSerial;
          if (!allowNearSteadyReuse || frameGap > 8u)
            return false;
        }

        return true;
      };
  m_war3Scene.shadowStats.semanticSceneLastTargetPublishRevision =
      semanticCatchupTargetRevision();
  if (!semanticLivePaletteSubmitNeeded &&
      m_war3Scene.shadowStats.semanticSceneLastSubmittedDrawCount != 0u &&
      stats.drawPacketCount != 0u &&
      m_war3Scene.shadowStats.semanticSceneLastSourcePublishRevision ==
          m_war3Scene.shadowStats.semanticSceneLastTargetPublishRevision &&
      m_war3SemanticSceneLastSuccessfulSubmitComplete &&
      !buildState.buildInProgress && !buildState.buildRequestPending) {
    return m_war3Scene.shadowStats.semanticSceneLastSubmittedDrawCount;
  }

  const uint64_t preEnsureTargetRevision = semanticCatchupTargetRevision();
  const uint64_t preEnsureTargetFrameSerial =
      semanticCatchupTargetFrameSerial();
  const bool canReuseSteadyStateFrame =
      semanticSceneHasSubmittedOnce && latestFrame &&
      latestFrame->frameSerial != 0u && !latestFrame->draws.empty();
  const bool latestFrameAlreadyFresh =
      latestFrame && latestFrame->frameSerial != 0u && !latestFrame->draws.empty() &&
      latestFrame->sourcePublishRevision >= preEnsureTargetRevision &&
      latestFrame->frameSerial >= preEnsureTargetFrameSerial &&
      !buildState.buildInProgress && !buildState.buildRequestPending;
  if (!latestFrameAlreadyFresh && !canReuseSteadyStateFrame) {
    {
      auto buildScope = war3::War3PerfMonitor::instance().cpuScope(
          "War3SemanticScene/EnsureLatestFrameBuilt");
      semanticRuntime.ensureLatestFrameBuilt();
    }
    stats = semanticRuntime.snapshot();
    buildState = semanticRuntime.buildStateSnapshot();
    latestFrame = semanticRuntime.snapshotFrameShared();
  }
  std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame> frame;
  {
    auto selectScope = war3::War3PerfMonitor::instance().cpuScope(
        "War3SemanticScene/FrameSelect");
    if (!preferredSupplementedFrame && preferredSupplementedRevision != 0u &&
        latestFrame && latestFrame->frameSerial != 0u &&
        !latestFrame->draws.empty() &&
        latestFrame->sourcePublishRevision == preferredSupplementedRevision &&
        latestFrame->frameSerial >= semanticCatchupTargetFrameSerial()) {
      preferredSupplementedFrame = latestFrame;
    }
    m_war3Scene.shadowStats.semanticSceneLastTargetPublishRevision =
        semanticCatchupTargetRevision();
    frame = preferredSupplementedFrame ? preferredSupplementedFrame
                                       : latestFrame;
    if (!semanticFrameSafeForCurrentTarget(frame, canReuseSteadyStateFrame))
      frame.reset();
    if (War3ShouldPreferSemanticSceneFrame(latestFrame, frame, unitsOnly))
      frame = latestFrame;
    if (!semanticFrameSafeForCurrentTarget(frame, canReuseSteadyStateFrame))
      frame.reset();
    auto renderableFrame =
        dxvk::war3::shadow::ShadowValidationRuntime::instance()
            .snapshotRenderableFrameShared();
    if (renderableFrame && renderableFrame->frameSerial != 0u &&
        !renderableFrame->draws.empty() &&
        semanticFrameSafeForCurrentTarget(renderableFrame,
                                          canReuseSteadyStateFrame) &&
        War3ShouldPreferSemanticSceneFrame(renderableFrame, frame, unitsOnly)) {
          frame = std::move(renderableFrame);
    }
  }
  auto tryReuseLastNonZeroSemanticFrame = [&]() {
    if (!m_war3SemanticSceneLastReusableFrame ||
        m_war3SemanticSceneLastReusableFrame->frameSerial == 0u ||
        m_war3SemanticSceneLastReusableFrame->draws.empty() ||
        m_war3SemanticSceneLastReusableUnitsOnly != unitsOnly) {
      m_war3Scene.shadowStats.semanticSceneReusableFrameUnavailableCount++;
      return false;
    }
    if (executeNativeBackendValidation &&
        !m_war3SemanticSceneLastReusableNativeValidation) {
      m_war3Scene.shadowStats
          .semanticSceneReusableFrameRejectedNativeValidationCount++;
      return false;
    }
    frame = m_war3SemanticSceneLastReusableFrame;
    m_war3Scene.shadowStats.semanticSceneReusableFrameForcedCount++;
    m_war3Scene.shadowStats.semanticSceneLastReusableFrameSerial =
        frame->frameSerial;
    return true;
  };
  auto tryReuseLastNonZeroIfSelectedFrameCannotSubmit = [&]() {
    const auto selectedScore = War3ScoreSemanticSceneFrame(frame.get(), unitsOnly);
    if (selectedScore.eligibleDrawCount != 0u)
      return false;
    m_war3Scene.shadowStats.semanticSceneSelectedFrameEligibleZeroCount++;
    return tryReuseLastNonZeroSemanticFrame();
  };
  tryReuseLastNonZeroIfSelectedFrameCannotSubmit();
  if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneBootstrapCatchupEnabled) {
    if (dxvk::war3::internal::
            IsSemanticSceneBootstrapCatchupRuntimeEnabled()) {
      auto semanticFrameNeedsCatchup = [&]() {
        if (!frame || frame->frameSerial == 0u || frame->draws.empty())
          return true;
        const uint64_t targetRevision = semanticCatchupTargetRevision();
        if (targetRevision != 0u &&
            frame->sourcePublishRevision < targetRevision) {
          return true;
        }
        const uint64_t targetFrameSerial = semanticCatchupTargetFrameSerial();
        if (targetFrameSerial != 0u &&
            frame->frameSerial < targetFrameSerial) {
          return true;
        }
        return stats.sourcePublishRevision != 0u &&
               stats.sourcePublishRevision == frame->sourcePublishRevision &&
               stats.drawPacketCount != 0u &&
               frame->draws.size() < stats.drawPacketCount;
      };
      const uint32_t maxCatchupAttempts =
          m_war3SemanticSceneLastSuccessfulSubmitFrameSerial != 0u
              ? 1u
              : dxvk::war3::internal::
                    kShadowSemanticCoreSceneBootstrapCatchupMaxAttempts;
      const bool needInitialBootstrapCatchup =
          m_war3SemanticSceneLastSuccessfulSubmitFrameSerial == 0u &&
          semanticFrameNeedsCatchup() &&
          (buildState.buildInProgress || buildState.buildRequestPending ||
           stats.submittedDrawCount == 0u);
      if (!canReuseSteadyStateFrame && needInitialBootstrapCatchup) {
        for (uint32_t attempt = 0u;
             attempt < maxCatchupAttempts;
             ++attempt) {
          m_war3Scene.shadowStats.semanticSceneCatchupAttemptCount++;
          auto catchupScope = war3::War3PerfMonitor::instance().cpuScope(
              "War3SemanticScene/BootstrapCatchup");
          dxvk::war3::shadow::ShadowValidationRuntime::instance()
              .ensureLatestFrameBuilt();
          stats =
              dxvk::war3::shadow::ShadowValidationRuntime::instance().snapshot();
          buildState = dxvk::war3::shadow::ShadowValidationRuntime::instance()
                           .buildStateSnapshot();
          m_war3Scene.shadowStats.semanticSceneLastTargetPublishRevision =
              semanticCatchupTargetRevision();
          latestFrame = dxvk::war3::shadow::ShadowValidationRuntime::instance()
                            .snapshotFrameShared();
          if (!preferredSupplementedFrame &&
              preferredSupplementedRevision != 0u && latestFrame &&
              latestFrame->frameSerial != 0u && !latestFrame->draws.empty() &&
              latestFrame->sourcePublishRevision ==
                  preferredSupplementedRevision &&
              latestFrame->frameSerial >= semanticCatchupTargetFrameSerial()) {
            preferredSupplementedFrame = latestFrame;
          }
          frame = preferredSupplementedFrame ? preferredSupplementedFrame
                                             : latestFrame;
          if (!semanticFrameSafeForCurrentTarget(frame,
                                                 canReuseSteadyStateFrame))
            frame.reset();
          if (War3ShouldPreferSemanticSceneFrame(latestFrame, frame, unitsOnly))
            frame = latestFrame;
          if (!semanticFrameSafeForCurrentTarget(frame,
                                                 canReuseSteadyStateFrame))
            frame.reset();
          auto renderableFrame =
              dxvk::war3::shadow::ShadowValidationRuntime::instance()
                  .snapshotRenderableFrameShared();
          if (renderableFrame && renderableFrame->frameSerial != 0u &&
              !renderableFrame->draws.empty() &&
              semanticFrameSafeForCurrentTarget(renderableFrame,
                                                canReuseSteadyStateFrame) &&
              War3ShouldPreferSemanticSceneFrame(renderableFrame, frame,
                                                 unitsOnly)) {
            frame = std::move(renderableFrame);
          }
          if (!frame || frame->frameSerial == 0u || frame->draws.empty())
            tryReuseLastNonZeroSemanticFrame();
          if (frame && frame->frameSerial != 0u && !frame->draws.empty() &&
              !semanticFrameNeedsCatchup()) {
            static std::atomic<uint32_t> s_catchupLogCount{0u};
            const uint32_t logIndex =
                s_catchupLogCount.fetch_add(1u, std::memory_order_relaxed);
            if (logIndex < 4u) {
              war3dbg::Print(
                  "DXVK War3Shadow: semantic scene bootstrap catchup success "
                  "unitsOnly=%d attempts=%u frameSerial=%llu drawCount=%zu\n",
                  unitsOnly ? 1 : 0, attempt + 1u,
                  static_cast<unsigned long long>(frame->frameSerial),
                  frame->draws.size());
            }
            m_war3Scene.shadowStats.semanticSceneCatchupSuccessCount++;
            break;
          }
          if (!buildState.buildInProgress && !buildState.buildRequestPending)
            break;
        }
      }
    }
  }
  if (!frame || frame->frameSerial == 0u || frame->draws.empty())
    tryReuseLastNonZeroSemanticFrame();
  tryReuseLastNonZeroIfSelectedFrameCannotSubmit();
  if (!frame || frame->frameSerial == 0u || frame->draws.empty()) {
    m_war3Scene.shadowStats.semanticSceneSkippedEmptyFrameCount++;
    static std::atomic<uint32_t> s_emptyLogCount{0u};
    const uint32_t logIndex =
        s_emptyLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (logIndex < 3u) {
      war3dbg::Print(
          "DXVK War3Shadow: semantic scene skipped empty frame unitsOnly=%d "
          "frame=%p frameSerial=%llu drawCount=%zu buildInProgress=%d "
          "sourceResolved=%llu\n",
          unitsOnly ? 1 : 0, frame.get(),
          frame ? static_cast<unsigned long long>(frame->frameSerial) : 0ull,
          frame ? frame->draws.size() : size_t(0u),
          buildState.buildInProgress ? 1 : 0,
          static_cast<unsigned long long>(stats.resolve.resolved));
    }
    return 0u;
  }
  {
    auto statsScope = war3::War3PerfMonitor::instance().cpuScope(
        "War3SemanticScene/FrameStats");
    m_war3Scene.shadowStats.semanticSceneLastFrameSerial = frame->frameSerial;
    m_war3Scene.shadowStats.semanticSceneLastSelectedFrameSerial =
        frame->frameSerial;
    m_war3Scene.shadowStats.semanticSceneLastSourcePublishRevision =
        frame->sourcePublishRevision;
    m_war3Scene.shadowStats.semanticSceneInputDrawCount =
        static_cast<uint32_t>(
            std::min<size_t>(frame->draws.size(), size_t(0xFFFFFFFFu)));
    m_war3Scene.shadowStats.semanticSceneLastInputDrawCount =
        m_war3Scene.shadowStats.semanticSceneInputDrawCount;
    uint32_t semanticSceneInputSkinned = 0u;
    for (const auto& draw : frame->draws) {
      if (draw.path == dxvk::war3::shadow::ShadowDrawPath::Skinned)
        ++semanticSceneInputSkinned;
    }
    m_war3Scene.shadowStats.semanticSceneInputSkinnedCount =
        semanticSceneInputSkinned;
    m_war3Scene.shadowStats.semanticSceneLastInputSkinnedCount =
        semanticSceneInputSkinned;
    if (unitsOnly) {
      uint32_t unitsOnlyFiltered = 0u;
      for (const auto& draw : frame->draws) {
        if (!War3LooksSubmitEligibleForScoringFast(draw, true))
          ++unitsOnlyFiltered;
      }
      m_war3Scene.shadowStats.semanticSceneSkippedUnitsOnlyFilter =
          unitsOnlyFiltered;
      m_war3Scene.shadowStats.semanticSceneLastUnitsOnlyFilteredCount =
          unitsOnlyFiltered;
    }
  }

  if (frame != latestFrame) {
    static std::atomic<uint32_t> s_renderableFallbackLogCount{0u};
    const uint32_t logIndex =
        s_renderableFallbackLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (logIndex < 8u) {
      war3dbg::Print(
          "DXVK War3Shadow: semantic scene reusing last renderable frame "
          "unitsOnly=%d latestSerial=%llu latestDraws=%zu renderableSerial=%llu "
          "renderableDraws=%zu\n",
          unitsOnly ? 1 : 0,
          latestFrame
              ? static_cast<unsigned long long>(latestFrame->frameSerial)
              : 0ull,
          latestFrame ? latestFrame->draws.size() : size_t(0u),
          static_cast<unsigned long long>(frame->frameSerial),
          frame->draws.size());
    }
  }

  constexpr uint64_t kSceneZeroSubmitCooldownFrames = 60u;
  const uint64_t frameSourceRevision = frame->sourcePublishRevision;
  const bool zeroSubmitCooldownActive =
      m_war3SemanticSceneLastSuccessfulSubmitFrameSerial == 0u &&
      frameSourceRevision != 0u &&
      m_war3SemanticSceneLastZeroSubmitPublishRevision == frameSourceRevision &&
      m_war3SemanticSceneLastZeroSubmitUnitsOnly == unitsOnly &&
      m_war3SemanticSceneLastZeroSubmitNativeValidation ==
          executeNativeBackendValidation &&
      m_war3SemanticSceneLastZeroSubmitFrameSerial != 0u &&
      m_war3ShadowPersistentFrameSerial <
          m_war3SemanticSceneLastZeroSubmitFrameSerial +
              kSceneZeroSubmitCooldownFrames;
  if (zeroSubmitCooldownActive) {
    return 0u;
  }

  War3DxvkSemanticShadowHost host(*this);
  const bool trackSubmittedIdentities =
      unitsOnly && !m_war3Scene.shadowFallbacks.empty();
  m_war3SemanticDxvkBackend.configureHost(&host, unitsOnly,
                                          trackSubmittedIdentities);
  dxvk::war3::shadow::ShadowRendererCore core = {};
  {
    auto submitScope = war3::War3PerfMonitor::instance().cpuScope(
        "War3SemanticScene/SubmitFrame");
    uint32_t submitCap = 0u;
    if constexpr (dxvk::war3::internal::
                      kShadowSemanticCoreSceneSubmitDrawCapEnabled) {
      submitCap = War3SemanticSubmitDrawCapRuntime();
    }
    core.submitFrameLimited(*frame, m_war3SemanticDxvkBackend, submitCap);
  }
  m_war3SemanticDxvkBackend.configureHost(nullptr, false);
  bool nativePrepared = false;
  bool nativeExecuted = false;
  if (executeNativeBackendValidation) {
    nativePrepared = dxvk::war3::platform::DriveNativeShadowBackend(false, 3u);
    if (nativePrepared &&
        dxvk::war3::internal::
            IsNativeRendererHostExecuteValidationRuntimeEnabled()) {
      nativeExecuted =
          dxvk::war3::platform::ExecuteNativeShadowBackendPreparedFrame();
    }
  }
  const uint32_t submitted =
      static_cast<uint32_t>(m_war3SemanticDxvkBackend.submittedDrawCount());
  auto postSubmitScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3SemanticScene/PostSubmit");
  m_war3Scene.shadowStats.semanticSceneLastSubmittedDrawCount = submitted;
  const uint64_t submittedTargetRevision = semanticCatchupTargetRevision();
  const uint64_t submittedTargetFrameSerial =
      semanticCatchupTargetFrameSerial();
  const bool submittedFrameCoversCompletedBuild =
      stats.sourcePublishRevision != 0u &&
      stats.sourcePublishRevision == frame->sourcePublishRevision &&
      frame->frameSerial >= submittedTargetFrameSerial &&
      stats.drawPacketCount != 0u &&
      frame->draws.size() >= stats.drawPacketCount;
  const bool semanticSubmitComplete =
      !buildState.buildInProgress && !buildState.buildRequestPending &&
      frame->sourcePublishRevision != 0u &&
      frame->sourcePublishRevision >= submittedTargetRevision &&
      submittedFrameCoversCompletedBuild;

  if (submitted == 0u) {
    m_war3SemanticSceneLastZeroSubmitFrameSerial =
        m_war3ShadowPersistentFrameSerial;
    m_war3SemanticSceneLastZeroSubmitPublishRevision = frameSourceRevision;
    m_war3SemanticSceneLastZeroSubmitUnitsOnly = unitsOnly;
    m_war3SemanticSceneLastZeroSubmitNativeValidation =
        executeNativeBackendValidation;
    m_war3Scene.shadowStats.semanticSceneZeroSubmitCount++;
    static std::atomic<uint32_t> s_zeroSubmitLogCount{0u};
    const uint32_t logIndex =
        s_zeroSubmitLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (logIndex < 3u) {
      war3dbg::Print(
          "DXVK War3Shadow: semantic scene submitted 0 draws unitsOnly=%d "
          "frameSerial=%llu sourceResolved=%llu sourceConsidered=%llu\n",
          unitsOnly ? 1 : 0,
          static_cast<unsigned long long>(frame->frameSerial),
          static_cast<unsigned long long>(stats.resolve.resolved),
          static_cast<unsigned long long>(stats.resolve.considered));
    }
    return 0u;
  }

  m_war3SemanticSceneLastSuccessfulSubmitFrameSerial =
      m_war3ShadowPersistentFrameSerial;
  m_war3SemanticSceneLastSuccessfulSubmitPublishRevision =
      frame->sourcePublishRevision;
  m_war3SemanticSceneLastReusableFrame = frame;
  m_war3SemanticSceneLastSuccessfulSubmitUnitsOnly = unitsOnly;
  m_war3SemanticSceneLastSuccessfulSubmitNativeValidation =
      executeNativeBackendValidation && nativeExecuted;
  m_war3SemanticSceneLastReusableUnitsOnly = unitsOnly;
  m_war3SemanticSceneLastReusableNativeValidation =
      executeNativeBackendValidation && nativeExecuted;
  m_war3SemanticSceneLastSuccessfulSubmitComplete = semanticSubmitComplete;
  m_war3SemanticSceneLastZeroSubmitFrameSerial = 0u;
  m_war3SemanticSceneLastZeroSubmitPublishRevision = 0u;

  static std::atomic<uint32_t> s_nonZeroSubmitLogCount{0u};
  const uint32_t logIndex =
      s_nonZeroSubmitLogCount.fetch_add(1u, std::memory_order_relaxed);
  if (logIndex < 3u) {
    war3dbg::Print(
        "DXVK War3Shadow: semantic scene submitted draws unitsOnly=%d "
        "submitted=%u frameSerial=%llu sourceResolved=%llu\n",
        unitsOnly ? 1 : 0, submitted,
        static_cast<unsigned long long>(frame->frameSerial),
        static_cast<unsigned long long>(stats.resolve.resolved));
  }

  if (nativeExecuted) {
    static std::atomic<uint32_t> s_nativeExecutedLogCount{0u};
    const uint32_t nativeLogIndex =
        s_nativeExecutedLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (nativeLogIndex < 3u || (nativeLogIndex % 300u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3Shadow: host validation executed native backend draws "
          "unitsOnly=%d submitted=%u\n",
          unitsOnly ? 1 : 0, submitted);
    }
  }

  War3Hook::MaybeInstallNativeRendererTakeover(
      unitsOnly ? "semantic-shadow-units-warm" : "semantic-shadow-scene-warm");

  static uint32_t s_logCount = 0u;
  if (unitsOnly) {
    uint32_t prunedFallbacks = 0u;
    uint32_t prunedByHandle = 0u;
    uint32_t prunedByWorldObjectEntry = 0u;
    uint32_t prunedBySceneNode = 0u;
    uint32_t prunedByRuntimeModel = 0u;
    auto newEnd = std::remove_if(
        m_war3Scene.shadowFallbacks.begin(), m_war3Scene.shadowFallbacks.end(),
        [&](const War3ShadowFallbackDraw& fallback) {
          const bool matchesHandle =
              fallback.normalizedHandle != 0u &&
              m_war3SemanticDxvkBackend.submittedHandles().find(
                  fallback.normalizedHandle) !=
                  m_war3SemanticDxvkBackend.submittedHandles().end();
          const bool matchesWorldObjectEntry =
              fallback.worldObjectEntry != nullptr &&
              m_war3SemanticDxvkBackend.submittedWorldObjectEntries().find(
                  fallback.worldObjectEntry) !=
                  m_war3SemanticDxvkBackend.submittedWorldObjectEntries().end();
          const bool matchesSceneNode =
              fallback.sceneNode != nullptr &&
              m_war3SemanticDxvkBackend.submittedSceneNodes().find(
                  fallback.sceneNode) !=
                  m_war3SemanticDxvkBackend.submittedSceneNodes().end();
          const bool matchesRuntimeModel =
              fallback.runtimeModelPtr != nullptr &&
              m_war3SemanticDxvkBackend.submittedRuntimeModels().find(
                  fallback.runtimeModelPtr) !=
                  m_war3SemanticDxvkBackend.submittedRuntimeModels().end();

          if (!matchesHandle && !matchesWorldObjectEntry && !matchesSceneNode &&
              !matchesRuntimeModel)
            return false;

          // unitsOnly 模式下，semantic scene 只会提交单位语义包。
          // 因此凡是和已提交语义对象在 handle/worldObject/scene/runtimeModel
          // 任一维度上对齐的 legacy fallback，都视为过期重复项并直接剔除，
          // 避免“新 semantic 单位阴影 + 旧 fallback 单位阴影”双写成静态重影。

          prunedFallbacks++;
          if (matchesHandle)
            prunedByHandle++;
          if (matchesWorldObjectEntry)
            prunedByWorldObjectEntry++;
          if (matchesSceneNode)
            prunedBySceneNode++;
          if (matchesRuntimeModel)
            prunedByRuntimeModel++;
          return true;
        });
    m_war3Scene.shadowFallbacks.erase(newEnd, m_war3Scene.shadowFallbacks.end());
    War3RecomputeFallbackBreakdown(m_war3Scene);
    m_war3Scene.shadowStats.semanticFallbackPruned += prunedFallbacks;
    m_war3Scene.shadowStats.semanticFallbackPrunedByHandle += prunedByHandle;
    m_war3Scene.shadowStats.semanticFallbackPrunedByWorldObjectEntry +=
        prunedByWorldObjectEntry;
    m_war3Scene.shadowStats.semanticFallbackPrunedBySceneNode +=
        prunedBySceneNode;
    m_war3Scene.shadowStats.semanticFallbackPrunedByRuntimeModel +=
        prunedByRuntimeModel;

    if (prunedFallbacks != 0u &&
        (s_logCount < 12u || (s_logCount % 300u) == 0u)) {
      WAR3_RENDER_LOG(
          "DXVK SemanticShadow: fallback prune submitted=%u pruned=%u "
          "byHandle=%u byEntry=%u byScene=%u byRuntime=%u\n",
          static_cast<unsigned>(submitted), static_cast<unsigned>(prunedFallbacks),
          static_cast<unsigned>(prunedByHandle),
          static_cast<unsigned>(prunedByWorldObjectEntry),
          static_cast<unsigned>(prunedBySceneNode),
          static_cast<unsigned>(prunedByRuntimeModel));
    }
  }

  if (s_logCount < 12u || (s_logCount % 300u) == 0u) {
    WAR3_RENDER_LOG(
        "DXVK SemanticShadow: scene submit frame=%llu submitted=%u "
        "unitsOnly=%d coreResolved=%llu coreSubmitted=%llu\n",
        static_cast<unsigned long long>(frame->frameSerial),
        static_cast<unsigned>(submitted), unitsOnly ? 1 : 0,
        static_cast<unsigned long long>(stats.resolve.resolved),
        static_cast<unsigned long long>(stats.submittedDrawCount));
  }
  s_logCount++;
  return submitted;
}

bool D3D9DeviceEx::War3ExecuteSemanticShadowSceneForValidation(
    bool unitsOnly,
    bool executeNativeBackendValidation) {
  if (!War3SemanticConsumerEnabled() ||
      !dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled())
    return false;

  if (m_war3Pipeline == nullptr || m_shadowReceiverPass == nullptr ||
      !m_war3Pipeline->WantsBeforeUiInsertion()) {
    return false;
  }

  const uint32_t submitted = War3TryPopulateSemanticShadowScene(
      unitsOnly, executeNativeBackendValidation);
  dxvk::war3::render::NoteShadowSceneStats(m_war3Scene.shadowStats);
  if (submitted == 0u && m_war3Scene.shadowCasters.empty() &&
      m_war3Scene.shadowFallbacks.empty()) {
    return false;
  }

  War3PipelineInput input;
  auto rt0 = m_war3LastWorldRt0 != nullptr ? m_war3LastWorldRt0
                                           : m_state.renderTargets[0];
  if (rt0 == nullptr)
    return false;
  input.colorView = rt0->GetRenderTargetView(false);

  auto ds =
      m_war3LastWorldDs != nullptr ? m_war3LastWorldDs : m_state.depthStencil;
  if (ds != nullptr)
    input.depthView = ds->GetDepthStencilView(true);

  input.scene = std::move(m_war3Scene);
  m_war3Scene = War3FrameScene{};
  m_war3ShadowPaletteHashIndex.clear();
  m_war3SemanticPaletteCache.clear();
  m_war3Scene.shadowPersistentPool.bytesCap =
      War3GetShadowPersistentPoolCapBytes();
  m_war3Scene.shadowPersistentPool.bytesUsed =
      m_war3ShadowPersistentBytesUsed;
  m_war3Scene.shadowPersistentPool.bytesEvicted =
      m_war3ShadowPersistentBytesEvicted;
  m_war3Scene.shadowPersistentPool.liveGeometryCount =
      static_cast<uint32_t>(m_war3ShadowPersistentGeometries.size());
  m_war3Scene.shadowStats.persistentPoolBytesUsed =
      m_war3ShadowPersistentBytesUsed;
  m_war3Scene.shadowStats.persistentPoolBytesEvicted =
      m_war3ShadowPersistentBytesEvicted;
  m_war3Scene.shadowStats.fallbackBudgetBytes =
      m_war3ShadowFallbackBudgetCapBytes;
  m_war3Scene.shadowStats.fallbackBudgetUsedBytes =
      m_war3ShadowFallbackBudgetUsedBytes;
  m_war3Scene.shadowStats.fallbackArenaBytes =
      dxvk::war3::memory::ShadowArena_UsedBytes();
  War3ResetShadowAllocator();

  if (!input.scene.worldCamera.valid && m_war3LastGoodCamera.valid)
    input.scene.worldCamera = m_war3LastGoodCamera;
  input.settings = &m_war3Pipeline->GetSettings();
  input.frameIndex = m_war3FrameIndex;

  EmitCs([this, cInput = std::move(input)](DxvkContext *ctx) mutable {
    Rc<DxvkCommandList> cmd;
    {
      auto externalScope = war3::War3PerfMonitor::instance().cpuScope(
          "War3Pipeline/SemanticValidation/BeginExternalRendering");
      cmd = ctx->beginExternalRendering();
    }
    if (m_shadowReceiverPass != nullptr) {
      auto executeScope = war3::War3PerfMonitor::instance().cpuScope(
          "War3Pipeline/SemanticValidation/RunReceiver");
      m_shadowReceiverPass->Run(cmd, cInput);
    }
  });

  m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor);
  m_dirty.set(D3D9DeviceDirtyFlag::DepthStencilState);
  m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
  m_dirty.set(D3D9DeviceDirtyFlag::RasterizerState);
  EmitCs([](DxvkContext *ctx) {
    DxvkViewport dummyVP = {};
    dummyVP.viewport.width = 1.0f;
    dummyVP.viewport.height = 1.0f;
    dummyVP.scissor.extent = {1, 1};
    ctx->setViewports(1, &dummyVP);
  });
  BindViewportAndScissor();
  m_dirty.set(D3D9DeviceDirtyFlag::DepthBias);
  m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
  m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);
  m_dirty.set(D3D9DeviceDirtyFlag::VertexBuffers);
  m_dirty.set(D3D9DeviceDirtyFlag::IndexBuffer);
  m_dirty.set(D3D9DeviceDirtyFlag::FFViewport);
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
  m_dirty.set(D3D9DeviceDirtyFlag::FFPixelData);
  return true;
}

void D3D9DeviceEx::War3TryCaptureShadowCasterDrawIndexed(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT StartIndex,
    UINT IndexCount, bool DynamicSysmemVBOs, bool DynamicSysmemIBO) {
  War3TryCaptureShadowCaster(PrimitiveType, BaseVertexIndex, 0, 0, StartIndex,
                             IndexCount, true, DynamicSysmemVBOs,
                             DynamicSysmemIBO);
}

void D3D9DeviceEx::War3TryCaptureShadowCasterDrawNonIndexed(
    D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT VertexCount,
    bool DynamicSysmemVBOs) {
  War3TryCaptureShadowCaster(PrimitiveType, 0, StartVertex, VertexCount,
                             StartVertex, VertexCount, false, DynamicSysmemVBOs,
                             false);
}

bool D3D9DeviceEx::War3ShouldOverrideWorldMaterial() const {
  if (dxvk::War3Hook::IsInShadowPass())
    return false;
  if (!war3::ShaderManager::get().hasOverride(war3shader::RenderStageId::World))
    return false;

  const auto layer = War3RenderState::CurrentLayer();
  if (layer == War3RenderLayer::UI)
    return false;

  const auto cat = War3RenderState::GetStageCategory();
  if (cat != War3RenderState::StageCategory::Terrain &&
      cat != War3RenderState::StageCategory::WorldObject &&
      cat != War3RenderState::StageCategory::Effect)
    return false;

  if (m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT))
    return false;

  if (m_state.vertexShader != nullptr || m_state.pixelShader != nullptr)
    return false;

  return true;
}

bool D3D9DeviceEx::War3ShouldOverridePostProcessMaterial() const {
  if (dxvk::War3Hook::IsInShadowPass())
    return false;
  if (!war3::ShaderManager::get().hasOverride(
          war3shader::RenderStageId::PostProcess))
    return false;

  const auto layer = War3RenderState::CurrentLayer();
  if (layer == War3RenderLayer::UI)
    return false;

  const auto cat = War3RenderState::GetStageCategory();
  if (cat != War3RenderState::StageCategory::PostProcess)
    return false;

  return true;
}

bool D3D9DeviceEx::War3ShouldDrawOutline() const {
  static std::atomic<uint32_t> s_outlineLogCount{0};
  uint32_t c = s_outlineLogCount.fetch_add(1);
  const bool log = (c < 20 || (c % 1000 == 0));

  if (!war3::ShaderManager::get().hasOverride(
          war3shader::RenderStageId::Outline)) {
    if (log)
      // WAR3_RENDER_LOG("DXVK_Outline: Skip - No shader override\n");
      if (!war3dbg::RenderLogEnabled() &&
          War3RenderState::HasOutlineHandles()) {
        static uint32_t s_outlineNoOverrideLogs = 0;
        if (s_outlineNoOverrideLogs++ < 3) {
          war3dbg::Print("DXVK_Outline: no shader override (handles=%u)\n",
                         War3RenderState::GetOutlineHandleCount());
        }
      }
    return false;
  }
  if (dxvk::War3Hook::IsInShadowPass())
    return false;

  const auto layer = War3RenderState::CurrentLayer();
  if (layer == War3RenderLayer::UI)
    return false;

  const War3RenderSettings *settings =
      m_war3Pipeline ? &m_war3Pipeline->GetSettings() : nullptr;
  if (!settings || !settings->occludedOutline.enabled) {
    if (log)
      WAR3_RENDER_LOG(
          "DXVK_Outline: Skip - Disabled in settings (enabled=%d)\n",
          settings ? settings->occludedOutline.enabled : -1);
    if (!war3dbg::RenderLogEnabled() && War3RenderState::HasOutlineHandles()) {
      static uint32_t s_outlineDisabledLogs = 0;
      if (s_outlineDisabledLogs++ < 3) {
        war3dbg::Print("DXVK_Outline: disabled in settings (handles=%u)\n",
                       War3RenderState::GetOutlineHandleCount());
      }
    }
    return false;
  }

  const uint32_t handle = War3RenderState::GetTlsBatchHandle();
  if (!handle) {
    if (War3RenderState::HasOutlineHandles() && log) {
      WAR3_RENDER_LOG("DXVK_Outline: Skip - No batch handle (handles=%u)\n",
                      War3RenderState::GetOutlineHandleCount());
    }
    if (!war3dbg::RenderLogEnabled() && War3RenderState::HasOutlineHandles()) {
      static uint32_t s_outlineNoBatchLogs = 0;
      if (s_outlineNoBatchLogs++ < 3) {
        war3dbg::Print("DXVK_Outline: no batch handle (handles=%u)\n",
                       War3RenderState::GetOutlineHandleCount());
      }
    }
    return false;
  }

  bool isOutline = War3RenderState::IsOutlineHandle(handle);
  if (log) {
    WAR3_RENDER_LOG("DXVK_Outline: Checking handle=%08X isOutline=%d\n", handle,
                    isOutline);
  }
  return isOutline;
}

void D3D9DeviceEx::War3UpdateMaterialUniforms(war3::War3Material *material,
                                              War3MaterialKind kind) {
  if (!material)
    return;

  const Matrix4 &world = m_state.transforms[GetTransformIndex(D3DTS_WORLD)];
  const Matrix4 &view = m_state.transforms[GetTransformIndex(D3DTS_VIEW)];
  const Matrix4 &proj = m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];
  const Matrix4 viewProj = proj * view;

  material->setMatrix("World", world);
  if (material->getVertexConstants().hasAlias("ViewProj"))
    material->setMatrix("ViewProj", viewProj);

  auto &shaderManager = war3::ShaderManager::get();
  shaderManager.setGlobalMatrix("ViewProj", viewProj);

  const float gameTime = War3RenderState::GetGameTime();
  const Vector4 timeParams(gameTime, 0.0f, 0.0f, 0.0f);
  shaderManager.setGlobalFloat4("Time", timeParams);
  if (material->getPixelConstants().hasAlias("Time"))
    material->setFloat4("Time", timeParams);

  if (kind == War3MaterialKind::Outline) {
    const War3RenderSettings *settings =
        m_war3Pipeline ? &m_war3Pipeline->GetSettings() : nullptr;
    const float widthPx = settings ? settings->occludedOutline.widthPx : 2.0f;
    const float widthScale = 0.0015f; // 经验系数：将像素宽度映射到世界空间
    const Vector4 outlineColor(
        settings ? settings->occludedOutline.colorR : 1.0f,
        settings ? settings->occludedOutline.colorG : 1.0f,
        settings ? settings->occludedOutline.colorB : 0.0f,
        settings ? settings->occludedOutline.colorA : 0.8f);

    material->setFloat4("OutlineColor", outlineColor);
    material->setFloat4("OutlineParams",
                        Vector4(widthPx * widthScale, 0.0f, 0.0f, 0.0f));
    return;
  }

  if (kind == War3MaterialKind::PostProcess) {
    const float w = static_cast<float>(m_state.viewport.Width);
    const float h = static_cast<float>(m_state.viewport.Height);
    const float invW = w > 1e-6f ? 1.0f / w : 0.0f;
    const float invH = h > 1e-6f ? 1.0f / h : 0.0f;
    const Vector4 screenParams(w, h, invW, invH);
    shaderManager.setGlobalFloat4("ScreenParams", screenParams);
    if (material->getPixelConstants().hasAlias("ScreenParams"))
      material->setFloat4("ScreenParams", screenParams);
  }

  const War3RenderSettings *settings =
      m_war3Pipeline ? &m_war3Pipeline->GetSettings() : nullptr;
  Vector4 lightDir =
      settings ? settings->sun.direction : Vector4(-0.3f, -1.0f, -0.2f, 0.0f);
  if (length(lightDir) > 1e-6f)
    lightDir = normalize(lightDir);
  const float lightIntensity = settings ? settings->sun.intensity : 1.0f;
  const Vector4 lightColor =
      settings ? settings->sun.color : Vector4(1.0f, 1.0f, 1.0f, 0.0f);

  Vector4 ambientColor = {};
  if (settings && settings->ambient.overrideEnabled) {
    ambientColor = settings->ambient.color;
  } else {
    DecodeD3DCOLOR(D3DCOLOR(m_state.renderStates[D3DRS_AMBIENT]),
                   ambientColor.data);
    ambientColor.w = 1.0f;
  }

  Matrix4 invView = inverse(view);
  Vector4 cameraPos = invView[3];
  cameraPos.w = 1.0f;

  const float specPower = std::max(1.0f, m_state.material->Power);

  const bool fogEnabled = (m_state.renderStates[D3DRS_FOGENABLE] != FALSE);
  const DWORD fogTableMode = m_state.renderStates[D3DRS_FOGTABLEMODE];
  const DWORD fogVertexMode = m_state.renderStates[D3DRS_FOGVERTEXMODE];
  DWORD fogMode = fogTableMode != D3DFOG_NONE ? fogTableMode : fogVertexMode;
  float fogModeValue = 0.0f;
  if (fogEnabled) {
    if (fogMode == D3DFOG_LINEAR)
      fogModeValue = 1.0f;
    else if (fogMode == D3DFOG_EXP)
      fogModeValue = 2.0f;
    else if (fogMode == D3DFOG_EXP2)
      fogModeValue = 3.0f;
  }

  const float fogStart = bit::cast<float>(m_state.renderStates[D3DRS_FOGSTART]);
  const float fogEnd = bit::cast<float>(m_state.renderStates[D3DRS_FOGEND]);
  const float fogDensity =
      bit::cast<float>(m_state.renderStates[D3DRS_FOGDENSITY]);
  Vector4 fogColor = {};
  DecodeD3DCOLOR(D3DCOLOR(m_state.renderStates[D3DRS_FOGCOLOR]), fogColor.data);

  Vector4 teamColor = {};
  DecodeD3DCOLOR(D3DCOLOR(m_state.renderStates[D3DRS_TEXTUREFACTOR]),
                 teamColor.data);

  const Vector4 lightDirParam(lightDir.x, lightDir.y, lightDir.z,
                              lightIntensity);
  const Vector4 lightColorParam(lightColor.x, lightColor.y, lightColor.z,
                                specPower);
  const Vector4 fogParamsParam(fogStart, fogEnd, fogDensity, fogModeValue);
  const Vector4 fogColorParam(fogColor.x, fogColor.y, fogColor.z,
                              fogEnabled ? 1.0f : 0.0f);

  shaderManager.setGlobalFloat4("LightDir", lightDirParam);
  shaderManager.setGlobalFloat4("LightColor", lightColorParam);
  shaderManager.setGlobalFloat4("AmbientColor", ambientColor);
  shaderManager.setGlobalFloat4("CameraPos", cameraPos);
  shaderManager.setGlobalFloat4("FogParams", fogParamsParam);
  shaderManager.setGlobalFloat4("FogColor", fogColorParam);
  shaderManager.setGlobalFloat4("TeamColor", teamColor);

  if (material->getPixelConstants().hasAlias("LightDir"))
    material->setFloat4("LightDir", lightDirParam);
  if (material->getPixelConstants().hasAlias("LightColor"))
    material->setFloat4("LightColor", lightColorParam);
  if (material->getPixelConstants().hasAlias("AmbientColor"))
    material->setFloat4("AmbientColor", ambientColor);
  if (material->getPixelConstants().hasAlias("CameraPos"))
    material->setFloat4("CameraPos", cameraPos);
  if (material->getPixelConstants().hasAlias("FogParams"))
    material->setFloat4("FogParams", fogParamsParam);
  if (material->getPixelConstants().hasAlias("FogColor"))
    material->setFloat4("FogColor", fogColorParam);
  if (material->getPixelConstants().hasAlias("TeamColor"))
    material->setFloat4("TeamColor", teamColor);
}

bool D3D9DeviceEx::War3ApplyMaterialOverride(
    war3::War3Material *material, War3MaterialKind kind,
    War3MaterialOverrideBackup &backup) {
  if (!material || !material->isCompiled())
    return false;

  War3UpdateMaterialUniforms(material, kind);

  backup.prevVs = m_state.vertexShader;
  backup.prevPs = m_state.pixelShader;
  backup.renderStates.clear();
  for (const auto &kv : material->getRenderStates()) {
    backup.renderStates.emplace_back(kv.first, m_state.renderStates[kv.first]);
  }

  auto collectRanges = [](const war3::ShaderConstantStore &store,
                          std::vector<War3ConstRange> &out) {
    std::vector<war3::ShaderConstantRange> ranges;
    store.collectRanges(ranges);
    for (const auto &r : ranges) {
      out.push_back({r.start, r.count});
    }
  };

  backup.vsRanges.clear();
  backup.psRanges.clear();
  collectRanges(war3::ShaderManager::get().getGlobalVertexConstants(),
                backup.vsRanges);
  collectRanges(war3::ShaderManager::get().getGlobalPixelConstants(),
                backup.psRanges);
  collectRanges(material->getVertexConstants(), backup.vsRanges);
  collectRanges(material->getPixelConstants(), backup.psRanges);

  auto mergeRanges = [](std::vector<War3ConstRange> &ranges) {
    if (ranges.empty())
      return;
    std::sort(ranges.begin(), ranges.end(),
              [](const War3ConstRange &a, const War3ConstRange &b) {
                return a.start < b.start;
              });
    std::vector<War3ConstRange> merged;
    merged.push_back(ranges.front());
    for (size_t i = 1; i < ranges.size(); ++i) {
      War3ConstRange &cur = merged.back();
      const War3ConstRange &next = ranges[i];
      const uint32_t curEnd = cur.start + cur.count;
      if (next.start <= curEnd) {
        const uint32_t nextEnd = next.start + next.count;
        cur.count = std::max(curEnd, nextEnd) - cur.start;
      } else {
        merged.push_back(next);
      }
    }
    ranges.swap(merged);
  };

  mergeRanges(backup.vsRanges);
  mergeRanges(backup.psRanges);

  backup.vsConsts.clear();
  for (const auto &range : backup.vsRanges) {
    for (uint32_t i = 0; i < range.count; ++i) {
      backup.vsConsts.push_back(m_state.vsConsts->fConsts[range.start + i]);
    }
  }
  backup.psConsts.clear();
  for (const auto &range : backup.psRanges) {
    for (uint32_t i = 0; i < range.count; ++i) {
      backup.psConsts.push_back(m_state.psConsts->fConsts[range.start + i]);
    }
  }

  material->apply(this);
  return true;
}

void D3D9DeviceEx::War3RestoreMaterialOverride(
    const War3MaterialOverrideBackup &backup) {
  size_t vsOffset = 0;
  for (const auto &range : backup.vsRanges) {
    if (range.count == 0)
      continue;
    SetVertexShaderConstantF(
        range.start,
        reinterpret_cast<const float *>(backup.vsConsts.data() + vsOffset),
        range.count);
    vsOffset += range.count;
  }

  size_t psOffset = 0;
  for (const auto &range : backup.psRanges) {
    if (range.count == 0)
      continue;
    SetPixelShaderConstantF(
        range.start,
        reinterpret_cast<const float *>(backup.psConsts.data() + psOffset),
        range.count);
    psOffset += range.count;
  }

  for (const auto &kv : backup.renderStates) {
    SetRenderState(kv.first, kv.second);
  }

  SetVertexShader(backup.prevVs.ptr());
  SetPixelShader(backup.prevPs.ptr());
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
    UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) {
  D3D9DeviceLock lock = LockDevice();

  // [War3] 16位索引溢出防护：拆分过大的批次，避免索引越界导致模型撕裂
  // 说明：
  // - 该保护只在 War3 管线存在时启用
  // - 仅对 TRIANGLELIST/LINELIST/POINTLIST 做安全拆分（strip/fan 会破坏拓扑）
  // - 这是“防越界兜底”，不是 32bit 索引替换
  static thread_local bool s_war3IndexSplitGuard = false;
  if (!s_war3IndexSplitGuard && m_war3Pipeline &&
      dxvk::war3::internal::kWar3IndexOverflowGuardEnabled) {
    auto *ibo = GetCommonBuffer(m_state.indices);
    if (ibo && ibo->Desc()->Format == D3D9Format::INDEX16 && PrimitiveCount) {
      const uint32_t indexCount = GetVertexCount(PrimitiveType, PrimitiveCount);
      const uint32_t indexStride = 2u;
      const uint32_t bufferIndexCap =
          static_cast<uint32_t>(ibo->Desc()->Size / indexStride);
      const uint32_t maxIndexCount =
          std::min(dxvk::war3::internal::kWar3IndexOverflowGuardMaxIndices,
                   bufferIndexCap);
      const bool exceedsCount = indexCount > maxIndexCount;
      const bool exceedsRange = (StartIndex + indexCount) > maxIndexCount;

      if (exceedsCount || exceedsRange) {
        uint32_t indicesPerPrim = 0;
        bool allowSplit = true;
        switch (PrimitiveType) {
        case D3DPT_TRIANGLELIST:
          indicesPerPrim = 3;
          break;
        case D3DPT_LINELIST:
          indicesPerPrim = 2;
          break;
        case D3DPT_POINTLIST:
          indicesPerPrim = 1;
          break;
        default:
          allowSplit = false;
          break;
        }

        if (!allowSplit) {
          if (dxvk::war3::internal::kWar3IndexOverflowGuardVerboseLogging) {
            WAR3_RENDER_LOG("DXVK War3: Skip split (unsupported prim) type=%d "
                            "indexCount=%u start=%u max=%u\n",
                            int(PrimitiveType), indexCount, StartIndex,
                            maxIndexCount);
          }
        } else if (indicesPerPrim > 0) {
          if (dxvk::war3::internal::kWar3IndexOverflowGuardVerboseLogging) {
            WAR3_RENDER_LOG(
                "DXVK War3: Split draw type=%d prim=%u start=%u maxIdx=%u\n",
                int(PrimitiveType), PrimitiveCount, StartIndex, maxIndexCount);
          }

          s_war3IndexSplitGuard = true;
          HRESULT hr = D3D_OK;
          UINT primLeft = PrimitiveCount;
          UINT start = StartIndex;
          while (primLeft > 0) {
            const uint32_t indicesLeft =
                start < maxIndexCount ? (maxIndexCount - start) : 0;
            const uint32_t maxPrimPerDraw = indicesLeft / indicesPerPrim;
            if (maxPrimPerDraw == 0) {
              if (dxvk::war3::internal::kWar3IndexOverflowGuardVerboseLogging) {
                WAR3_RENDER_LOG("DXVK War3: Split draw stop (no room) type=%d "
                                "start=%u maxIdx=%u\n",
                                int(PrimitiveType), start, maxIndexCount);
              }
              break;
            }

            const UINT drawPrim = std::min<uint32_t>(primLeft, maxPrimPerDraw);
            hr = DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex,
                                      MinVertexIndex, NumVertices, start,
                                      drawPrim);
            if (FAILED(hr))
              break;
            start += drawPrim * indicesPerPrim;
            primLeft -= drawPrim;
          }
          s_war3IndexSplitGuard = false;
          return hr;
        }
      }
    }
  }

  // [War3] Auto-Instancing Hook
  if (auto *activeBuf = war3::reimpl::War3InstanceBuffer::GetActive()) {
    // Check if current shader supports instancing
    bool safeToInstance = false;
    if (m_state.vertexShader != nullptr) {
      safeToInstance = war3::reimpl::War3ShaderPatcher::IsShaderInstanced(
          m_state.vertexShader.ptr());
    }

    if (safeToInstance) {
      activeBuf->CaptureDrawParams(PrimitiveType, BaseVertexIndex,
                                   MinVertexIndex, NumVertices, StartIndex,
                                   PrimitiveCount);
      activeBuf->AdvanceInstance();
      return D3D_OK; // Suppress draw call
    }
  }

  if (unlikely(m_state.vertexDecl == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(!PrimitiveCount || !NumVertices))
    return D3D_OK;

  // War3 渲染管线插入点检测应发生在 PrepareDraw 之前，
  // 避免 beginExternalRendering 影响本次 draw 的绑定状态。
  War3MaybeInsertBeforeUi();

  bool dynamicSysmemVBOs;
  bool dynamicSysmemIBO;
  uint32_t indexCount = GetVertexCount(PrimitiveType, PrimitiveCount);
  UploadPerDrawData(MinVertexIndex, NumVertices, StartIndex, indexCount,
                    BaseVertexIndex, &dynamicSysmemVBOs, &dynamicSysmemIBO);

  War3TryCaptureShadowCasterDrawIndexed(PrimitiveType, BaseVertexIndex,
                                        StartIndex, indexCount,
                                        dynamicSysmemVBOs, dynamicSysmemIBO);

  War3MaterialOverrideBackup worldBackup = {};
  bool worldOverrideActive = false;
  if (War3ShouldOverrideWorldMaterial()) {
    auto *worldMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::World);
    if (worldMat != nullptr) {
      worldOverrideActive = War3ApplyMaterialOverride(
          worldMat, War3MaterialKind::World, worldBackup);
    }
  }
  War3MaterialOverrideBackup postBackup = {};
  bool postOverrideActive = false;
  if (War3ShouldOverridePostProcessMaterial()) {
    auto *postMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::PostProcess);
    if (postMat != nullptr) {
      postOverrideActive = War3ApplyMaterialOverride(
          postMat, War3MaterialKind::PostProcess, postBackup);
    }
  }

  PrepareDraw(PrimitiveType, !dynamicSysmemVBOs, !dynamicSysmemIBO);

  // War3 调试过滤逻辑（DXVK 内部调试用）
  // 使用 WorldRenderStage + UI Observer 层级，根据 DebugRenderMode
  // 选择性渲染特定类型的内容
  {
    const auto debugMode = War3RenderState::GetDebugRenderMode();
    const auto batchTag = War3RenderState::GetCurrentBatchTag();
    const bool terrainActive = War3RenderState::IsTerrainRendering();
    bool skipDraw = false;
    static bool s_loggedSkipdebugMode = false;
    static bool s_loggedSkipUi = false;
    // 1) 显式 Debug 模式：TerrainOnly / ObjectsOnly / SkyboxOnly / UIOnly
    if (debugMode != War3RenderState::DebugRenderMode::Normal) {
      if (!War3RenderState::ShouldRenderCurrent(debugMode)) {
        if (!s_loggedSkipdebugMode) {
          s_loggedSkipdebugMode = true;
          WAR3_RENDER_LOG("DXVK War3Debug: SkipDraw debugMode=%d stage=%d "
                          "category=%d layer=%d batchTag=%d terrain=%d\n",
                          static_cast<int>(debugMode),
                          War3RenderState::GetStage(),
                          static_cast<int>(War3RenderState::GetStageCategory()),
                          static_cast<int>(War3RenderState::CurrentLayer()),
                          static_cast<int>(batchTag), terrainActive ? 1 : 0);
        }
        skipDraw = true;
      }
    }
    // 2) 独立 UI 过滤：Normal 模式下仍可屏蔽 UI 或当前处于 UI Observer 层

    else if (War3RenderState::ShouldSkipUi() &&
             (War3RenderState::GetStageCategory() ==
                  War3RenderState::StageCategory::UI ||
              War3RenderState::CurrentLayer() == War3RenderLayer::UI)) {
      skipDraw = true;
      if (!s_loggedSkipUi) {
        s_loggedSkipUi = true;
        WAR3_RENDER_LOG("DXVK War3Debug: SkipUI debugMode=%d stage=%d "
                        "category=%d layer=%d batchTag=%d terrain=%d\n",
                        static_cast<int>(debugMode),
                        War3RenderState::GetStage(),
                        static_cast<int>(War3RenderState::GetStageCategory()),
                        static_cast<int>(War3RenderState::CurrentLayer()),
                        static_cast<int>(batchTag), terrainActive ? 1 : 0);
      }
    }

    // 若处于调试模式或开启了跳过
    // UI，但本批次未被跳过，打印一次当前阶段信息帮助诊断
    static bool s_loggedSkipFailure = false;
    if (!skipDraw && !s_loggedSkipFailure &&
        (debugMode != War3RenderState::DebugRenderMode::Normal ||
         War3RenderState::ShouldSkipUi())) {
      s_loggedSkipFailure = true;

      auto cat = War3RenderState::GetStageCategory();
      int rawStage = War3RenderState::GetStage();
      auto layer = War3RenderState::CurrentLayer();

      WAR3_RENDER_LOG(
          "DXVK War3Debug: SkipDraw FAILED debugMode=%d shouldSkipUi=%d "
          "stage=%d category=%d layer=%d batchTag=%d terrain=%d\n",
          static_cast<int>(debugMode), War3RenderState::ShouldSkipUi() ? 1 : 0,
          rawStage, static_cast<int>(cat), static_cast<int>(layer),
          static_cast<int>(batchTag), terrainActive ? 1 : 0);
    }

    // 路径追踪占位：记录有效 draw（自动过滤 UI），默认不影响渲染
    if (!skipDraw) {
      War3PathTracer::Get().RecordDraw(War3RenderState::CurrentLayer(),
                                       War3RenderState::GetStageCategory(),
                                       War3RenderState::GetCurrentBatchTag());
    }

    if (skipDraw) {
      if (postOverrideActive)
        War3RestoreMaterialOverride(postBackup);
      if (worldOverrideActive)
        War3RestoreMaterialOverride(worldBackup);
      return D3D_OK;
    }
  }

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    if (War3ShadowPassTraceEnabled()) {
      WAR3_RENDER_LOG("ShadowPass: DrawIndexedPrimitive type=%d count=%u\n",
                      static_cast<int>(PrimitiveType), PrimitiveCount);
      if (m_state.vertexShader != nullptr)
        WAR3_RENDER_LOG("ShadowPass WARNING: Real Vertex Shader is NOT NULL!\n");
      if (m_state.pixelShader != nullptr)
        WAR3_RENDER_LOG("ShadowPass WARNING: Real Pixel Shader is NOT NULL!\n");
    }
  }

  EmitCs([this, cPrimType = PrimitiveType, cPrimCount = PrimitiveCount,
          cStartIndex = StartIndex, cBaseVertexIndex = BaseVertexIndex,
          cInstanceCount = GetInstanceCount()](DxvkContext *ctx) {
    auto drawInfo = GenerateDrawInfo(cPrimType, cPrimCount, cInstanceCount);

    ApplyPrimitiveType(ctx, cPrimType);

    VkDrawIndexedIndirectCommand draw = {};
    draw.indexCount = drawInfo.vertexCount;
    draw.instanceCount = drawInfo.instanceCount;
    draw.firstIndex = cStartIndex;
    draw.vertexOffset = cBaseVertexIndex;

    ctx->drawIndexed(1u, &draw);
  });

  if (postOverrideActive)
    War3RestoreMaterialOverride(postBackup);
  if (worldOverrideActive)
    War3RestoreMaterialOverride(worldBackup);

  if (War3ShouldDrawOutline()) {
    auto *outlineMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::Outline);
    if (outlineMat != nullptr) {
      War3MaterialOverrideBackup outlineBackup = {};
      if (War3ApplyMaterialOverride(outlineMat, War3MaterialKind::Outline,
                                    outlineBackup)) {
        PrepareDraw(PrimitiveType, !dynamicSysmemVBOs, !dynamicSysmemIBO);
        EmitCs([this, cPrimType = PrimitiveType, cPrimCount = PrimitiveCount,
                cStartIndex = StartIndex, cBaseVertexIndex = BaseVertexIndex,
                cInstanceCount = GetInstanceCount()](DxvkContext *ctx) {
          auto drawInfo =
              GenerateDrawInfo(cPrimType, cPrimCount, cInstanceCount);

          ApplyPrimitiveType(ctx, cPrimType);

          VkDrawIndexedIndirectCommand draw = {};
          draw.indexCount = drawInfo.vertexCount;
          draw.instanceCount = drawInfo.instanceCount;
          draw.firstIndex = cStartIndex;
          draw.vertexOffset = cBaseVertexIndex;

          ctx->drawIndexed(1u, &draw);
        });
        War3RestoreMaterialOverride(outlineBackup);
      }
    }
  }

  // [自定义描边]
  // 说明：当 Outline 材质启用且 useScreenSpace=false
  // 时，此处直接进行几何描边重绘。 若使用屏幕空间描边，则由 BeforeUi 阶段的
  // Outline pass 处理。

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(!VertexStreamZeroStride))
    return D3DERR_INVALIDCALL;

  if (unlikely(m_state.vertexDecl == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(!PrimitiveCount))
    return D3D_OK;

  War3MaybeInsertBeforeUi();

  War3MaterialOverrideBackup worldBackup = {};
  bool worldOverrideActive = false;
  if (War3ShouldOverrideWorldMaterial()) {
    auto *worldMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::World);
    if (worldMat != nullptr) {
      worldOverrideActive = War3ApplyMaterialOverride(
          worldMat, War3MaterialKind::World, worldBackup);
    }
  }
  War3MaterialOverrideBackup postBackup = {};
  bool postOverrideActive = false;
  if (War3ShouldOverridePostProcessMaterial()) {
    auto *postMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::PostProcess);
    if (postMat != nullptr) {
      postOverrideActive = War3ApplyMaterialOverride(
          postMat, War3MaterialKind::PostProcess, postBackup);
    }
  }

  PrepareDraw(PrimitiveType, false, false);

  uint32_t vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);

  const uint32_t dataSize = GetUPDataSize(vertexCount, VertexStreamZeroStride);
  const uint32_t bufferSize =
      GetUPBufferSize(vertexCount, VertexStreamZeroStride);

  auto upSlice = AllocUPBuffer(bufferSize);
  FillUPVertexBuffer(upSlice.mapPtr, pVertexStreamZeroData, dataSize,
                     bufferSize);

  // War3：UP 绘制同样需要 ShadowCapture（装饰物/地形可能走 UP）
  if (m_war3Pipeline) {
    m_war3PerDrawUpload = War3PerDrawUploadInfo{};
    Rc<DxvkResourceAllocation> upAlloc = nullptr;
    if (m_upBuffer != nullptr && upSlice.slice.buffer() == m_upBuffer &&
        m_upBufferAllocation != nullptr)
      upAlloc = m_upBufferAllocation;
    else if (upSlice.slice.buffer() != nullptr)
      upAlloc = upSlice.slice.buffer()->storage();
    m_war3PerDrawUpload.storage = upAlloc;
    m_war3PerDrawUpload.vbSlices[0] = upSlice.slice;
    m_war3PerDrawUpload.vbStrides[0] = VertexStreamZeroStride;
    m_war3PerDrawUpload.vbValid[0] = true;
  }
  War3TryCaptureShadowCasterDrawNonIndexed(PrimitiveType, 0, vertexCount, true);

  EmitCs([this, cBufferSlice = std::move(upSlice.slice),
          cPrimType = PrimitiveType, cStride = VertexStreamZeroStride,
          cVertexCount = vertexCount](DxvkContext *ctx) mutable {
    ApplyPrimitiveType(ctx, cPrimType);

    // Tests on Windows show that D3D9 does not do non-indexed instanced draws.
    VkDrawIndirectCommand draw = {};
    draw.vertexCount = cVertexCount;
    draw.instanceCount = 1u;

    ctx->bindVertexBuffer(0, std::move(cBufferSlice), cStride);
    ctx->draw(1u, &draw);
    ctx->bindVertexBuffer(0, DxvkBufferSlice(), 0);
  });

  m_state.vertexBuffers[0].vertexBuffer = nullptr;
  m_state.vertexBuffers[0].offset = 0;
  m_state.vertexBuffers[0].stride = 0;

  if (postOverrideActive)
    War3RestoreMaterialOverride(postBackup);
  if (worldOverrideActive)
    War3RestoreMaterialOverride(worldBackup);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
    UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(!VertexStreamZeroStride))
    return D3DERR_INVALIDCALL;

  if (unlikely(m_state.vertexDecl == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(!PrimitiveCount || !NumVertices))
    return D3D_OK;

  War3MaybeInsertBeforeUi();

  War3MaterialOverrideBackup worldBackup = {};
  bool worldOverrideActive = false;
  if (War3ShouldOverrideWorldMaterial()) {
    auto *worldMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::World);
    if (worldMat != nullptr) {
      worldOverrideActive = War3ApplyMaterialOverride(
          worldMat, War3MaterialKind::World, worldBackup);
    }
  }
  War3MaterialOverrideBackup postBackup = {};
  bool postOverrideActive = false;
  if (War3ShouldOverridePostProcessMaterial()) {
    auto *postMat = war3::ShaderManager::get().getMaterial(
        war3shader::RenderStageId::PostProcess);
    if (postMat != nullptr) {
      postOverrideActive = War3ApplyMaterialOverride(
          postMat, War3MaterialKind::PostProcess, postBackup);
    }
  }

  PrepareDraw(PrimitiveType, false, false);

  uint32_t vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);

  const uint32_t vertexDataSize =
      GetUPDataSize(MinVertexIndex + NumVertices, VertexStreamZeroStride);
  const uint32_t vertexBufferSize =
      GetUPBufferSize(MinVertexIndex + NumVertices, VertexStreamZeroStride);

  const uint32_t indexSize = IndexDataFormat == D3DFMT_INDEX16 ? 2 : 4;
  const uint32_t indicesSize = vertexCount * indexSize;

  const uint32_t upSize = vertexBufferSize + indicesSize;

  auto upSlice = AllocUPBuffer(upSize);
  uint8_t *data = reinterpret_cast<uint8_t *>(upSlice.mapPtr);
  FillUPVertexBuffer(data, pVertexStreamZeroData, vertexDataSize,
                     vertexBufferSize);
  std::memcpy(data + vertexBufferSize, pIndexData, indicesSize);

  // War3：UP Indexed 绘制需要 ShadowCapture（装饰物/地形可能走 UP）
  if (m_war3Pipeline) {
    m_war3PerDrawUpload = War3PerDrawUploadInfo{};
    Rc<DxvkResourceAllocation> upAlloc = nullptr;
    if (m_upBuffer != nullptr && upSlice.slice.buffer() == m_upBuffer &&
        m_upBufferAllocation != nullptr)
      upAlloc = m_upBufferAllocation;
    else if (upSlice.slice.buffer() != nullptr)
      upAlloc = upSlice.slice.buffer()->storage();
    m_war3PerDrawUpload.storage = upAlloc;
    m_war3PerDrawUpload.vbSlices[0] =
        upSlice.slice.subSlice(0, vertexBufferSize);
    m_war3PerDrawUpload.vbStrides[0] = VertexStreamZeroStride;
    m_war3PerDrawUpload.vbValid[0] = true;
    m_war3PerDrawUpload.ibSlice = upSlice.slice.subSlice(
        vertexBufferSize, upSlice.slice.length() - vertexBufferSize);
    m_war3PerDrawUpload.ibType =
        DecodeIndexType(static_cast<D3D9Format>(IndexDataFormat));
    m_war3PerDrawUpload.ibValid = true;
    m_war3PerDrawUpload.ibStorage = m_war3PerDrawUpload.storage;
  }
  War3TryCaptureShadowCasterDrawIndexed(PrimitiveType, 0, 0, vertexCount, true,
                                        true);
  /**
   * 此段代码的作用：
   * 1. 针对魔兽争霸3 (War3) 的自定义渲染管线，处理通过 User Pointer (UP)
   * 方式提交的索引绘制数据。
   * 2.
   * 它从分配的临时缓冲区（upSlice）中切分出顶点数据切片（vbSlices）和索引数据切片（ibSlice）。
   * 3. 填充 `m_war3PerDrawUpload`
   * 结构体，确保后续的阴影捕获逻辑（War3TryCaptureShadowCasterDrawIndexed）
   *    能够正确访问这些临时的几何信息，从而为装饰物或地形生成阴影。
   *
   * EmitCs 的作用：
   * EmitCs (Emit Command Stream) 是 DXVK 架构中用于实现异步渲染的核心机制。
   * 它将包含渲染指令（如绑定缓冲区、执行绘制等）的 lambda
   * 表达式提交到后端的命令流线程（CS Thread）中排队执行。 这种设计实现了 D3D9
   * API 调用（前端）与 Vulkan
   * 指令提交（后端）的解耦，大幅提升多核环境下的渲染性能。
   */

  EmitCs([this, cVertexSize = vertexBufferSize,
          cBufferSlice = std::move(upSlice.slice), cPrimType = PrimitiveType,
          cPrimCount = PrimitiveCount, cStride = VertexStreamZeroStride,
          cInstanceCount = GetInstanceCount(),
          cIndexType = DecodeIndexType(
              static_cast<D3D9Format>(IndexDataFormat))](DxvkContext *ctx) {
    auto drawInfo = GenerateDrawInfo(cPrimType, cPrimCount, cInstanceCount);

    ApplyPrimitiveType(ctx, cPrimType);

    VkDrawIndexedIndirectCommand draw = {};
    draw.indexCount = drawInfo.vertexCount;
    draw.instanceCount = drawInfo.instanceCount;

    ctx->bindVertexBuffer(0, cBufferSlice.subSlice(0, cVertexSize), cStride);
    ctx->bindIndexBuffer(
        cBufferSlice.subSlice(cVertexSize, cBufferSlice.length() - cVertexSize),
        cIndexType);
    ctx->drawIndexed(1u, &draw);
    ctx->bindVertexBuffer(0, DxvkBufferSlice(), 0);
    ctx->bindIndexBuffer(DxvkBufferSlice(), VK_INDEX_TYPE_UINT32);
  });

  m_state.vertexBuffers[0].vertexBuffer = nullptr;
  m_state.vertexBuffers[0].offset = 0;
  m_state.vertexBuffers[0].stride = 0;

  m_state.indices = nullptr;

  if (postOverrideActive)
    War3RestoreMaterialOverride(postBackup);
  if (worldOverrideActive)
    War3RestoreMaterialOverride(worldBackup);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ProcessVertices(
    UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
    IDirect3DVertexBuffer9 *pDestBuffer,
    IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pDestBuffer == nullptr))
    return D3DERR_INVALIDCALL;

  // When vertex shader 3.0 or above is set as the current vertex shader,
  // the output vertex declaration must be present.
  if (UseProgrammableVS()) {
    const auto &programInfo = GetCommonShader(m_state.vertexShader)->GetInfo();

    if (unlikely(programInfo.majorVersion() >= 3) && (pVertexDecl == nullptr))
      return D3DERR_INVALIDCALL;
  }

  if (!SupportsSWVP()) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::err("D3D9DeviceEx::ProcessVertices: SWVP emu unsupported "
                  "(vertexPipelineStoresAndAtomics)");

    return D3D_OK;
  }

  if (unlikely(!VertexCount))
    return D3D_OK;

  D3D9CommonBuffer *dst =
      static_cast<D3D9VertexBuffer *>(pDestBuffer)->GetCommonBuffer();
  D3D9VertexDecl *decl = static_cast<D3D9VertexDecl *>(pVertexDecl);

  bool dynamicSysmemVBOs;
  uint32_t firstIndex = 0;
  int32_t baseVertexIndex = 0;
  UploadPerDrawData(SrcStartIndex, VertexCount, firstIndex, 0, baseVertexIndex,
                    &dynamicSysmemVBOs, nullptr);

  PrepareDraw(D3DPT_FORCE_DWORD, !dynamicSysmemVBOs, false);

  if (decl == nullptr) {
    DWORD FVF = dst->Desc()->FVF;

    auto iter = m_fvfTable.find(FVF);

    if (iter == m_fvfTable.end()) {
      decl = new D3D9VertexDecl(this, FVF);
      m_fvfTable.insert(std::make_pair(FVF, decl));
    } else
      decl = iter->second.ptr();
  }

  uint32_t offset = DestIndex * decl->GetSize(0);

  D3D9CompactVertexElements elements;
  for (const D3DVERTEXELEMENT9 &element : decl->GetElements()) {
    elements.emplace_back(element);
  }

  EmitCs([this, cVertexElements = std::move(elements),
          cVertexCount = VertexCount, cStartIndex = SrcStartIndex,
          cInstanceCount = GetInstanceCount(),
          cBufferSlice = dst->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(),
          cBufferOffset = offset](DxvkContext *ctx) mutable {
    Rc<DxvkShader> shader =
        m_swvpEmulator.GetShaderModule(this, std::move(cVertexElements));

    auto drawInfo =
        GenerateDrawInfo(D3DPT_POINTLIST, cVertexCount, cInstanceCount);

    if (drawInfo.vertexCount == 0 || drawInfo.instanceCount == 0)
      return; // Return from lambda, not from function.

    if (drawInfo.instanceCount != 1) {
      drawInfo.instanceCount = 1;

      Logger::warn("D3D9DeviceEx::ProcessVertices: instancing unsupported");
    }

    ApplyPrimitiveType(ctx, D3DPT_POINTLIST);

    // We need to bind the buffer as a view rather than a raw buffer.
    // In order to avoid view bloat, create a format-less view for
    // the entire buffer and pass the offset in via a push constant.
    DxvkBufferViewKey viewKey;
    viewKey.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    viewKey.offset = cBufferSlice.offset();
    viewKey.size = cBufferSlice.length();

    auto bufferView = cBufferSlice.buffer()->createView(viewKey);

    // Unbind the pixel shader, we aren't drawing
    // to avoid val errors / UB.
    ctx->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(nullptr);

    VkDrawIndirectCommand draw = {};
    draw.vertexCount = drawInfo.vertexCount;
    draw.instanceCount = drawInfo.instanceCount;
    draw.firstVertex = cStartIndex;

    uint32_t byteOffset = cBufferOffset;

    ctx->bindShader<VK_SHADER_STAGE_GEOMETRY_BIT>(std::move(shader));
    ctx->bindResourceBufferView(VK_SHADER_STAGE_GEOMETRY_BIT,
                                getSWVPBufferSlot(), std::move(bufferView));
    ctx->pushData(VK_SHADER_STAGE_GEOMETRY_BIT, 0u, sizeof(byteOffset),
                  &byteOffset);
    ctx->draw(1u, &draw);
    ctx->bindResourceBufferView(VK_SHADER_STAGE_GEOMETRY_BIT,
                                getSWVPBufferSlot(), nullptr);
    ctx->bindShader<VK_SHADER_STAGE_GEOMETRY_BIT>(nullptr);
  });

  // We unbound the pixel shader before,
  // let's make sure that gets rebound.
  if (m_state.pixelShader != nullptr) {
    BindShader<DxsoProgramTypes::PixelShader>(
        GetCommonShader(m_state.pixelShader));
  } else {
    m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
    BindFFUbershader<DxsoProgramType::PixelShader>();
  }

  if (dst->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_BUFFER) {
    uint32_t copySize = VertexCount * decl->GetSize(0);

    EmitCs([cSrcBuffer = dst->GetBuffer<D3D9_COMMON_BUFFER_TYPE_REAL>(),
            cDstBuffer = dst->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>(),
            cOffset = offset, cCopySize = copySize](DxvkContext *ctx) {
      ctx->copyBuffer(cDstBuffer, cOffset, cSrcBuffer, cOffset, cCopySize);
    });
  }

  dst->SetNeedsReadback(true);
  TrackBufferMappingBufferSequenceNumber(dst);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements,
                                      IDirect3DVertexDeclaration9 **ppDecl) {
  InitReturnPtr(ppDecl);

  if (unlikely(ppDecl == nullptr || pVertexElements == nullptr))
    return D3DERR_INVALIDCALL;

  const D3DVERTEXELEMENT9 *counter = pVertexElements;
  while (counter->Stream != 0xFF)
    counter++;

  const uint32_t declCount = uint32_t(counter - pVertexElements);

  try {
    const Com<D3D9VertexDecl> decl =
        new D3D9VertexDecl(this, pVertexElements, declCount);
    *ppDecl = decl.ref();
    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_INVALIDCALL;
  }
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
  D3D9DeviceLock lock = LockDevice();

  D3D9VertexDecl *decl = static_cast<D3D9VertexDecl *>(pDecl);

  if (unlikely(ShouldRecord()))
    return m_recorder->SetVertexDeclaration(decl);

  if (decl == m_state.vertexDecl.ptr())
    return D3D_OK;

  bool dirtyFFShader = decl == nullptr || m_state.vertexDecl == nullptr;
  if (!dirtyFFShader)
    dirtyFFShader |=
        decl->GetFlags() != m_state.vertexDecl->GetFlags() ||
        decl->GetTexcoordMask() != m_state.vertexDecl->GetTexcoordMask();

  if (dirtyFFShader)
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);

  const bool wasUsingProgrammableVS = UseProgrammableVS();

  m_state.vertexDecl = decl;

  const bool usesProgrammableVS = UseProgrammableVS();

  if (unlikely(usesProgrammableVS != wasUsingProgrammableVS)) {
    if (usesProgrammableVS) {
      BindShader<DxsoProgramType::VertexShader>(
          GetCommonShader(m_state.vertexShader));
    } else {
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      BindFFUbershader<DxsoProgramType::VertexShader>();
    }
  }

  m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
  D3D9DeviceLock lock = LockDevice();

  InitReturnPtr(ppDecl);

  if (unlikely(ppDecl == nullptr))
    return D3DERR_INVALIDCALL;

  if (m_state.vertexDecl == nullptr)
    return D3D_OK;

  *ppDecl = m_state.vertexDecl.ref();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetFVF(DWORD FVF) {
  D3D9DeviceLock lock = LockDevice();

  if (FVF == 0)
    return D3D_OK;

  D3D9VertexDecl *decl = nullptr;

  auto iter = m_fvfTable.find(FVF);

  if (iter == m_fvfTable.end()) {
    decl = new D3D9VertexDecl(this, FVF);
    m_fvfTable.insert(std::make_pair(FVF, decl));
  } else
    decl = iter->second.ptr();

  return this->SetVertexDeclaration(decl);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetFVF(DWORD *pFVF) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pFVF == nullptr))
    return D3DERR_INVALIDCALL;

  *pFVF = m_state.vertexDecl != nullptr ? m_state.vertexDecl->GetFVF() : 0;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVertexShader(
    const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
  // CreateVertexShader does not init the
  // return ptr unlike CreatePixelShader

  if (unlikely(ppShader == nullptr))
    return D3DERR_INVALIDCALL;

  DxsoModuleInfo moduleInfo;
  moduleInfo.options = m_dxsoOptions;

  D3D9CommonShader module;
  uint32_t bytecodeLength;

  // 1. Create ORIGINAL Shader (Unpatched) - For UI/Portrait
  if (FAILED(this->CreateShaderModule(&module, &bytecodeLength,
                                      VK_SHADER_STAGE_VERTEX_BIT, pFunction,
                                      &moduleInfo)))
    return D3DERR_INVALIDCALL;

  D3D9VertexShader *pOriginalShader = new D3D9VertexShader(
      this, &m_shaderAllocator, module, pFunction, bytecodeLength);

  // Return original shader to game
  *ppShader = ref(pOriginalShader);

  // 2. Create INSTANCED Shader (Patched) - For World
  std::vector<DWORD> patchedBytecode;
  bool patched = war3::reimpl::War3ShaderPatcher::PatchVertexShader(
      pFunction, patchedBytecode);

  if (patched) {
    D3D9CommonShader instModule;
    uint32_t instLen;
    if (SUCCEEDED(this->CreateShaderModule(
            &instModule, &instLen, VK_SHADER_STAGE_VERTEX_BIT,
            patchedBytecode.data(), &moduleInfo))) {
      D3D9VertexShader *pInstancedShader =
          new D3D9VertexShader(this, &m_shaderAllocator, instModule,
                               patchedBytecode.data(), instLen);

      war3::reimpl::War3ShaderPatcher::SetShaderInstanced(pInstancedShader,
                                                          true);

      // Link Instanced Shader to Original Shader
      pOriginalShader->SetPartner(pInstancedShader);

      pInstancedShader->Release();
    }
  }

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetVertexShader(IDirect3DVertexShader9 *pShader) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    m_shadowFakeVS = pShader;
    if (War3ShadowPassTraceEnabled()) {
      WAR3_RENDER_LOG("ShadowPass: SetVertexShader intercepted ptr=%p\n",
                      pShader);
    }
    return D3D_OK;
  }

  D3D9VertexShader *shader = static_cast<D3D9VertexShader *>(pShader);

  if (unlikely(ShouldRecord()))
    return m_recorder->SetVertexShader(shader);

  if (shader == m_state.vertexShader.ptr())
    return D3D_OK;

  auto *oldShader = GetCommonShader(m_state.vertexShader);
  auto *newShader = GetCommonShader(shader);

  bool oldCopies = oldShader && oldShader->GetMeta().needsConstantCopies;
  bool newCopies = newShader && newShader->GetMeta().needsConstantCopies;

  m_consts[DxsoProgramTypes::VertexShader].dirty |=
      oldCopies || newCopies || !oldShader;
  m_consts[DxsoProgramTypes::VertexShader].meta =
      newShader ? newShader->GetMeta() : DxsoShaderMetaInfo();

  if (newShader && oldShader) {
    m_consts[DxsoProgramTypes::VertexShader].dirty |=
        newShader->GetMeta().maxConstIndexF >
            oldShader->GetMeta().maxConstIndexF ||
        newShader->GetMeta().maxConstIndexI >
            oldShader->GetMeta().maxConstIndexI ||
        newShader->GetMeta().maxConstIndexB >
            oldShader->GetMeta().maxConstIndexB;
  }

  const bool wasUsingProgrammableVS = UseProgrammableVS();

  m_state.vertexShader = shader;

  const bool usesProgrammableVS = UseProgrammableVS();

  if (usesProgrammableVS) {
    BindShader<DxsoProgramTypes::VertexShader>(GetCommonShader(shader));

    UpdateTextureTypeMismatchesForShader(newShader, VSShaderMasks().samplerMask,
                                         FirstVSSamplerSlot);
  } else if (wasUsingProgrammableVS) {
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
    BindFFUbershader<DxsoProgramType::VertexShader>();
  }

  m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
  D3D9DeviceLock lock = LockDevice();

  InitReturnPtr(ppShader);

  if (unlikely(ppShader == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    *ppShader = m_shadowFakeVS.ref();
    return D3D_OK;
  }

  *ppShader = m_state.vertexShader.ref();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantF(
    UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  D3D9DeviceLock lock = LockDevice();

  // [War3] Capture constants for Auto-Instancing
  if (auto *activeBuf = war3::reimpl::War3InstanceBuffer::GetActive()) {
    activeBuf->CaptureConstants(StartRegister, pConstantData, Vector4fCount);
  }

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    if (War3ShadowPassTraceEnabled()) {
      WAR3_RENDER_LOG("ShadowPass: SetVSConstF Start=%u Count=%u\n",
                      StartRegister, Vector4fCount);
    }
  }

  return SetShaderConstants<DxsoProgramTypes::VertexShader,
                            D3D9ConstantType::Float>(
      StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantF(
    UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  D3D9DeviceLock lock = LockDevice();

  return GetShaderConstants<DxsoProgramTypes::VertexShader,
                            D3D9ConstantType::Float>(
      StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantI(
    UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  D3D9DeviceLock lock = LockDevice();

  return SetShaderConstants<DxsoProgramTypes::VertexShader,
                            D3D9ConstantType::Int>(StartRegister, pConstantData,
                                                   Vector4iCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantI(
    UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  D3D9DeviceLock lock = LockDevice();

  return GetShaderConstants<DxsoProgramTypes::VertexShader,
                            D3D9ConstantType::Int>(StartRegister, pConstantData,
                                                   Vector4iCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantB(
    UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  D3D9DeviceLock lock = LockDevice();

  return SetShaderConstants<DxsoProgramTypes::VertexShader,
                            D3D9ConstantType::Bool>(StartRegister,
                                                    pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantB(
    UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  D3D9DeviceLock lock = LockDevice();

  return GetShaderConstants<DxsoProgramTypes::VertexShader,
                            D3D9ConstantType::Bool>(StartRegister,
                                                    pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes,
    UINT Stride) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(StreamNumber >= caps::MaxStreams))
    return D3DERR_INVALIDCALL;

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    if (War3ShadowPassTraceEnabled()) {
      WAR3_RENDER_LOG("ShadowPass: SetStreamSource stream=%u ptr=%p\n",
                      static_cast<unsigned>(StreamNumber), pStreamData);
    }
  }

  D3D9VertexBuffer *buffer = static_cast<D3D9VertexBuffer *>(pStreamData);

  if (unlikely(ShouldRecord()))
    return m_recorder->SetStreamSource(StreamNumber, buffer, OffsetInBytes,
                                       Stride);

  auto &vbo = m_state.vertexBuffers[StreamNumber];
  bool needsUpdate = vbo.vertexBuffer != buffer;

  if (needsUpdate)
    vbo.vertexBuffer = buffer;

  const uint32_t bit = 1u << StreamNumber;
  m_vbSlotTracking.bound &= ~bit;
  m_vbSlotTracking.uploadPerDraw &= ~bit;
  m_vbSlotTracking.needsUpload &= ~bit;

  if (buffer != nullptr) {
    needsUpdate |= vbo.offset != OffsetInBytes || vbo.stride != Stride;

    vbo.offset = OffsetInBytes;
    vbo.stride = Stride;

    const D3D9CommonBuffer *commonBuffer = GetCommonBuffer(buffer);
    m_vbSlotTracking.bound |= bit;
    if (commonBuffer->DoPerDrawUpload() || CanOnlySWVP())
      m_vbSlotTracking.uploadPerDraw |= bit;
    if (commonBuffer->NeedsUpload()) {
      m_vbSlotTracking.needsUpload |= bit;
    }
  } else {
    // D3D9 doesn't actually unbind any vertex buffer when passing null.
    // Operation Flashpoint: Red River relies on this behavior.
    needsUpdate = false;
  }

  if (needsUpdate)
    BindVertexBuffer(StreamNumber, buffer, OffsetInBytes, Stride);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
    UINT *pOffsetInBytes, UINT *pStride) {
  D3D9DeviceLock lock = LockDevice();

  InitReturnPtr(ppStreamData);

  if (likely(pOffsetInBytes != nullptr))
    *pOffsetInBytes = 0;

  if (likely(pStride != nullptr))
    *pStride = 0;

  if (unlikely(ppStreamData == nullptr || pOffsetInBytes == nullptr ||
               pStride == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(StreamNumber >= caps::MaxStreams))
    return D3DERR_INVALIDCALL;

  const auto &vbo = m_state.vertexBuffers[StreamNumber];

  *ppStreamData = vbo.vertexBuffer.ref();
  *pOffsetInBytes = vbo.offset;
  *pStride = vbo.stride;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetStreamSourceFreq(UINT StreamNumber,
                                                            UINT Setting) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(StreamNumber >= caps::MaxStreams))
    return D3DERR_INVALIDCALL;

  const bool indexed = Setting & D3DSTREAMSOURCE_INDEXEDDATA;
  const bool instanced = Setting & D3DSTREAMSOURCE_INSTANCEDATA;

  if (unlikely(StreamNumber == 0 && instanced))
    return D3DERR_INVALIDCALL;

  if (unlikely(instanced && indexed))
    return D3DERR_INVALIDCALL;

  if (unlikely(Setting == 0))
    return D3DERR_INVALIDCALL;

  if (unlikely(ShouldRecord()))
    return m_recorder->SetStreamSourceFreq(StreamNumber, Setting);

  if (m_state.streamFreq[StreamNumber] == Setting)
    return D3D_OK;

  m_state.streamFreq[StreamNumber] = Setting;

  if (instanced)
    m_vbSlotTracking.instanced |= 1u << StreamNumber;
  else
    m_vbSlotTracking.instanced &= ~(1u << StreamNumber);

  m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetStreamSourceFreq(UINT StreamNumber,
                                                            UINT *pSetting) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(StreamNumber >= caps::MaxStreams))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSetting == nullptr))
    return D3DERR_INVALIDCALL;

  *pSetting = m_state.streamFreq[StreamNumber];

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    if (War3ShadowPassTraceEnabled())
      WAR3_RENDER_LOG("ShadowPass: SetIndices ptr=%p\n", pIndexData);
  }

  D3D9IndexBuffer *buffer = static_cast<D3D9IndexBuffer *>(pIndexData);

  if (unlikely(ShouldRecord()))
    return m_recorder->SetIndices(buffer);

  if (buffer == m_state.indices.ptr())
    return D3D_OK;

  m_state.indices = buffer;

  // Don't unbind the buffer if the game sets a nullptr here.
  // Operation Flashpoint Red River breaks if we do that.
  // EndScene will clean it up if necessary.
  if (buffer != nullptr)
    BindIndices();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) {
  D3D9DeviceLock lock = LockDevice();
  InitReturnPtr(ppIndexData);

  if (unlikely(ppIndexData == nullptr))
    return D3DERR_INVALIDCALL;

  *ppIndexData = m_state.indices.ref();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreatePixelShader(
    const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
  InitReturnPtr(ppShader);

  if (unlikely(ppShader == nullptr))
    return D3DERR_INVALIDCALL;

  DxsoModuleInfo moduleInfo;
  moduleInfo.options = m_dxsoOptions;

  D3D9CommonShader module;
  uint32_t bytecodeLength;

  if (FAILED(this->CreateShaderModule(&module, &bytecodeLength,
                                      VK_SHADER_STAGE_FRAGMENT_BIT, pFunction,
                                      &moduleInfo)))
    return D3DERR_INVALIDCALL;

  *ppShader = ref(new D3D9PixelShader(this, &m_shaderAllocator, module,
                                      pFunction, bytecodeLength));

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetPixelShader(IDirect3DPixelShader9 *pShader) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    m_shadowFakePS = pShader;
    if (War3ShadowPassTraceEnabled()) {
      WAR3_RENDER_LOG("ShadowPass: SetPixelShader intercepted ptr=%p\n", pShader);
    }
    return D3D_OK;
  }

  D3D9PixelShader *shader = static_cast<D3D9PixelShader *>(pShader);

  if (unlikely(ShouldRecord()))
    return m_recorder->SetPixelShader(shader);

  if (shader == m_state.pixelShader.ptr())
    return D3D_OK;

  auto *oldShader = GetCommonShader(m_state.pixelShader);
  auto *newShader = GetCommonShader(shader);

  bool oldCopies = oldShader && oldShader->GetMeta().needsConstantCopies;
  bool newCopies = newShader && newShader->GetMeta().needsConstantCopies;

  m_consts[DxsoProgramTypes::PixelShader].dirty |=
      oldCopies || newCopies || !oldShader;
  m_consts[DxsoProgramTypes::PixelShader].meta =
      newShader ? newShader->GetMeta() : DxsoShaderMetaInfo();

  if (newShader && oldShader) {
    m_consts[DxsoProgramTypes::PixelShader].dirty |=
        newShader->GetMeta().maxConstIndexF >
            oldShader->GetMeta().maxConstIndexF ||
        newShader->GetMeta().maxConstIndexI >
            oldShader->GetMeta().maxConstIndexI ||
        newShader->GetMeta().maxConstIndexB >
            oldShader->GetMeta().maxConstIndexB;
  }

  const D3D9ShaderMasks oldShaderMasks = PSShaderMasks();
  m_state.pixelShader = shader;
  const D3D9ShaderMasks newShaderMasks = PSShaderMasks();

  if (shader != nullptr) {
    BindShader<DxsoProgramTypes::PixelShader>(newShader);

    UpdateTextureTypeMismatchesForShader(newShader, newShaderMasks.samplerMask,
                                         0);

    bool dirty =
        m_specInfo.set<D3D9SpecConstantId::SpecFFLastActiveTextureStage>(0u);
    dirty |=
        m_specInfo.set<D3D9SpecConstantId::SpecFFGlobalSpecularEnabled>(0u);
    constexpr uint32_t perTextureStageSpecConsts =
        static_cast<uint32_t>(D3D9SpecConstantId::SpecFFTextureStage1ColorOp) -
        static_cast<uint32_t>(D3D9SpecConstantId::SpecFFTextureStage0ColorOp);
    for (uint32_t i = 0; i < 4; i++) {
      dirty |=
          m_specInfo.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0ColorOp +
                             perTextureStageSpecConsts * i),
                         0u);
      dirty |=
          m_specInfo.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0ColorArg1 +
                             perTextureStageSpecConsts * i),
                         0u);
      dirty |=
          m_specInfo.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0ColorArg2 +
                             perTextureStageSpecConsts * i),
                         0u);
      dirty |=
          m_specInfo.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0AlphaOp +
                             perTextureStageSpecConsts * i),
                         0u);
      dirty |=
          m_specInfo.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0AlphaArg1 +
                             perTextureStageSpecConsts * i),
                         0u);
      dirty |=
          m_specInfo.set(static_cast<D3D9SpecConstantId>(
                             D3D9SpecConstantId::SpecFFTextureStage0AlphaArg2 +
                             perTextureStageSpecConsts * i),
                         0u);
      dirty |= m_specInfo.set(
          static_cast<D3D9SpecConstantId>(
              D3D9SpecConstantId::SpecFFTextureStage0ResultIsTemp +
              perTextureStageSpecConsts * i),
          0u);
    }
    if (dirty) {
      m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
    }
  } else {
    m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
    BindFFUbershader<DxsoProgramType::PixelShader>();

    // TODO: What fixed function textures are in use?
    // Currently we are making all 8 of them as in use here.
    // Fixed function always uses spec constants to decide the texture type.
    m_textureSlotTracking.textureDirty |=
        newShaderMasks.samplerMask &
        m_textureSlotTracking.mismatchingTextureType;
    m_textureSlotTracking.mismatchingTextureType &= ~newShaderMasks.samplerMask;
  }

  // Check whether the color output mask or the mask of the used samplers
  // forces us to deal with hazards in a different way.
  if (likely(oldShaderMasks.samplerMask != newShaderMasks.samplerMask ||
             oldShaderMasks.rtMask != newShaderMasks.rtMask))
    UpdateActiveHazardsRT(oldShaderMasks.samplerMask |
                          newShaderMasks.samplerMask);

  if (likely(oldShaderMasks.samplerMask != newShaderMasks.samplerMask))
    UpdateActiveHazardsDS(oldShaderMasks.samplerMask |
                          newShaderMasks.samplerMask);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
  D3D9DeviceLock lock = LockDevice();

  InitReturnPtr(ppShader);

  if (unlikely(ppShader == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(dxvk::War3Hook::IsInShadowPass())) {
    *ppShader = m_shadowFakePS.ref();
    return D3D_OK;
  }

  *ppShader = m_state.pixelShader.ref();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantF(
    UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  D3D9DeviceLock lock = LockDevice();

  return SetShaderConstants<DxsoProgramTypes::PixelShader,
                            D3D9ConstantType::Float>(
      StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantF(
    UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  D3D9DeviceLock lock = LockDevice();

  return GetShaderConstants<DxsoProgramTypes::PixelShader,
                            D3D9ConstantType::Float>(
      StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantI(
    UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  D3D9DeviceLock lock = LockDevice();

  return SetShaderConstants<DxsoProgramTypes::PixelShader,
                            D3D9ConstantType::Int>(StartRegister, pConstantData,
                                                   Vector4iCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantI(
    UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  D3D9DeviceLock lock = LockDevice();

  return GetShaderConstants<DxsoProgramTypes::PixelShader,
                            D3D9ConstantType::Int>(StartRegister, pConstantData,
                                                   Vector4iCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantB(
    UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  D3D9DeviceLock lock = LockDevice();

  return SetShaderConstants<DxsoProgramTypes::PixelShader,
                            D3D9ConstantType::Bool>(StartRegister,
                                                    pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantB(
    UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  D3D9DeviceLock lock = LockDevice();

  return GetShaderConstants<DxsoProgramTypes::PixelShader,
                            D3D9ConstantType::Bool>(StartRegister,
                                                    pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::DrawRectPatch(UINT Handle, const float *pNumSegs,
                            const D3DRECTPATCH_INFO *pRectPatchInfo) {
  static bool s_errorShown = false;

  if (!std::exchange(s_errorShown, true))
    Logger::warn("D3D9DeviceEx::DrawRectPatch: Stub");

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawTriPatch(
    UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo) {
  static bool s_errorShown = false;

  if (!std::exchange(s_errorShown, true))
    Logger::warn("D3D9DeviceEx::DrawTriPatch: Stub");

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DeletePatch(UINT Handle) {
  static bool s_errorShown = false;

  if (!std::exchange(s_errorShown, true))
    Logger::warn("D3D9DeviceEx::DeletePatch: Stub");

  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateQuery(D3DQUERYTYPE Type,
                                                    IDirect3DQuery9 **ppQuery) {
  HRESULT hr = D3D9Query::QuerySupported(this, Type);

  if (ppQuery == nullptr || hr != D3D_OK)
    return hr;

  try {
    *ppQuery = ref(new D3D9Query(this, Type));
    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_NOTAVAILABLE;
  }
}

// Ex Methods

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetConvolutionMonoKernel(
    UINT width, UINT height, float *rows, float *columns) {
  // We don't advertise support for this.
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ComposeRects(
    IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst,
    IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
    IDirect3DVertexBuffer9 *pDstRectDescs, D3DCOMPOSERECTSOP Operation,
    int Xoffset, int Yoffset) {
  static bool s_errorShown = false;

  if (!std::exchange(s_errorShown, true))
    Logger::warn("D3D9DeviceEx::ComposeRects: Stub");

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetGPUThreadPriority(INT *pPriority) {
  static bool s_errorShown = false;

  if (!std::exchange(s_errorShown, true))
    Logger::warn("D3D9DeviceEx::GetGPUThreadPriority: Stub");

  if (unlikely(pPriority == nullptr))
    return D3DERR_INVALIDCALL;

  *pPriority = 0;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetGPUThreadPriority(INT Priority) {
  static bool s_errorShown = false;

  if (!std::exchange(s_errorShown, true))
    Logger::warn("D3D9DeviceEx::SetGPUThreadPriority: Stub");

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::WaitForVBlank(UINT iSwapChain) {
  if (unlikely(iSwapChain != 0))
    return D3DERR_INVALIDCALL;

  return m_implicitSwapchain->WaitForVBlank();
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CheckResourceResidency(
    IDirect3DResource9 **pResourceArray, UINT32 NumResources) {
  static bool s_errorShown = false;

  if (!std::exchange(s_errorShown, true))
    Logger::warn("D3D9DeviceEx::CheckResourceResidency: Stub");

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::SetMaximumFrameLatency(UINT MaxLatency) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(MaxLatency > 30))
    return D3DERR_INVALIDCALL;

  if (MaxLatency == 0)
    MaxLatency = DefaultFrameLatency;

  if (MaxLatency > MaxFrameLatency)
    MaxLatency = MaxFrameLatency;

  m_frameLatency = MaxLatency;

  m_implicitSwapchain->SyncFrameLatency();

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::GetMaximumFrameLatency(UINT *pMaxLatency) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(pMaxLatency == nullptr))
    return D3DERR_INVALIDCALL;

  *pMaxLatency = m_frameLatency;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::CheckDeviceState(HWND hDestinationWindow) {
  static bool s_errorShown = false;

  if (!std::exchange(s_errorShown, true))
    Logger::warn("D3D9DeviceEx::CheckDeviceState: Stub");

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx(const RECT *pSourceRect,
                                                  const RECT *pDestRect,
                                                  HWND hDestWindowOverride,
                                                  const RGNDATA *pDirtyRegion,
                                                  DWORD dwFlags) {
  if (m_war3Pipeline) {
    m_war3Pipeline->OnFrameStart();
  }
  War3RenderState::OnFrameStart();
  const bool wantsShadowCapture =
      m_war3Pipeline && m_war3Pipeline->WantsShadowCapture();
  const bool wantsSemanticSceneIdentity =
      War3SemanticConsumerEnabled() &&
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled();
  bool wantsShadowObjectIdentity =
      (wantsShadowCapture || wantsSemanticSceneIdentity) &&
      dxvk::war3::internal::kShadowRuntimeBridgeEnabled;
  bool wantsShadowFallbackBridge =
      wantsShadowCapture && dxvk::war3::internal::kShadowRuntimeBridgeEnabled;
  if ((wantsShadowCapture || wantsSemanticSceneIdentity) &&
      dxvk::war3::internal::kShadowRuntimeBridgeEnabled) {
    const auto trackingDecision =
        dxvk::war3::render::ComputeShadowRuntimeBridgeTracking();
    wantsShadowObjectIdentity = trackingDecision.wantsObjectIdentity;
    wantsShadowFallbackBridge =
        wantsShadowCapture && trackingDecision.wantsFallbackBridge;
  }
  if (wantsSemanticSceneIdentity &&
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly) {
    wantsShadowObjectIdentity = true;
  }
  War3RenderState::SetShadowObjectIdentityTrackingEnabled(
      wantsShadowObjectIdentity);
  War3RenderState::SetShadowDrawFallbackBridgeEnabled(
      wantsShadowFallbackBridge);
  War3RenderState::SetShadowSemanticTrackingEnabled(
      wantsSemanticSceneIdentity || wantsShadowObjectIdentity ||
      wantsShadowFallbackBridge);
  bool needsTracking = false;
  {
    const bool wantsPipelineTracking =
        m_war3Pipeline && (m_war3Pipeline->WantsBeforeUiInsertion() ||
                           m_war3Pipeline->WantsShadowCapture());
    const bool debugTracking = War3RenderState::GetDebugRenderMode() !=
                                   War3RenderState::DebugRenderMode::Normal ||
                               War3RenderState::ShouldSkipUi();
    needsTracking = wantsPipelineTracking || debugTracking ||
                    War3RenderState::NeedsObjectTracking() ||
                    dxvk::war3::render::NativeRendererProbe::IsEnabled();
    War3RenderState::SetBatchTagTrackingEnabled(needsTracking);
  }
  if (needsTracking) {
    dxvk::war3::render::War3Renderer::instance().BeginFrame();
    dxvk::war3::render::ExecBatchProcessor::ResetFrameCaches();
  }
  if (dxvk::war3::internal::kWar3RenderModuleTakeoverEnabled &&
      war3shader::internal::HasAnyRenderListeners()) {
    war3shader::internal::BeginFrame();
    m_war3FrameIndex =
        (m_war3FrameIndex + 1) % 3; // Sync with war3shader global ring index
    war3shader::internal::DispatchRenderEvent(
        war3shader::RenderEventID::FRAME_BEGIN);
  }
  dxvk::war3::render::BeginShadowArenaCaptureFrame();
  if (dxvk::war3::render::IsShadowArenaCaptureEnabled()) {
    if (!dxvk::war3::memory::ShadowArena_IsInitialized())
      dxvk::war3::memory::ShadowArena_Init();
    dxvk::war3::memory::ShadowArena_BeginFrame(m_war3FrameIndex);
  }
  if (dxvk::war3::memory::StormHook_IsInstalled())
    dxvk::war3::memory::StormHook_PrintPeriodicReport();
  // m_shadowDataPool.newFrame();
  m_war3Scene = War3FrameScene{};
  m_war3ShadowPaletteHashIndex.clear();
  m_war3SemanticPaletteCache.clear();
  m_war3ShadowPersistentFrameSerial++;
  War3GcShadowPersistentGeometry(false);
  m_war3ShadowFallbackBudgetCapBytes = War3GetShadowFallbackBudgetCapBytes();
  m_war3ShadowFallbackBudgetUsedBytes = 0;
  m_war3ShadowFallbackBudgetExceeded = false;
  dxvk::war3::native::War3NativeShadowHintRegistry::instance().beginFrame(
      dxvk::war3::render::RenderObjectRegistry::instance().getFrameNumber());
  m_war3Scene.shadowPersistentPool.bytesCap =
      War3GetShadowPersistentPoolCapBytes();
  m_war3Scene.shadowPersistentPool.bytesUsed =
      m_war3ShadowPersistentBytesUsed;
  m_war3Scene.shadowPersistentPool.bytesEvicted =
      m_war3ShadowPersistentBytesEvicted;
  m_war3Scene.shadowPersistentPool.liveGeometryCount =
      static_cast<uint32_t>(m_war3ShadowPersistentGeometries.size());
  m_war3Scene.shadowStats.persistentPoolBytesUsed =
      m_war3ShadowPersistentBytesUsed;
  m_war3Scene.shadowStats.persistentPoolBytesEvicted =
      m_war3ShadowPersistentBytesEvicted;
  m_war3Scene.shadowStats.fallbackBudgetBytes =
      m_war3ShadowFallbackBudgetCapBytes;
  m_war3Scene.shadowStats.fallbackBudgetUsedBytes =
      m_war3ShadowFallbackBudgetUsedBytes;
  m_war3Scene.shadowStats.fallbackArenaBytes =
      dxvk::war3::memory::ShadowArena_UsedBytes();
  m_war3LastWorldRt0 = nullptr;
  m_war3LastWorldDs = nullptr;
  m_war3BestWorldViewportArea = 0u;
  m_war3BestWorldCameraTier = 0u;
  m_war3DebugOverlayDrawn = false;
  m_war3UiDrawSeenThisFrame = false;

  if (m_cursor.IsSoftwareCursor()) {
    D3D9_SOFTWARE_CURSOR *pSoftwareCursor = m_cursor.GetSoftwareCursor();

    UINT cursorWidth = pSoftwareCursor->DrawCursor ? pSoftwareCursor->Width : 0;
    UINT cursorHeight =
        pSoftwareCursor->DrawCursor ? pSoftwareCursor->Height : 0;

    m_implicitSwapchain->SetCursorPosition(
        pSoftwareCursor->X - pSoftwareCursor->XHotSpot,
        pSoftwareCursor->Y - pSoftwareCursor->YHotSpot, cursorWidth,
        cursorHeight);

    // Once a hardware cursor has been set or the device has been reset,
    // we need to ensure that we render a 0-sized rectangle first, and
    // only then fully clear the software cursor.
    if (unlikely(pSoftwareCursor->ClearCursor)) {
      pSoftwareCursor->Width = 0;
      pSoftwareCursor->Height = 0;
      pSoftwareCursor->XHotSpot = 0;
      pSoftwareCursor->YHotSpot = 0;
      pSoftwareCursor->ClearCursor = false;
    }
  }

  if (dxvk::war3::internal::kWar3RenderModuleTakeoverEnabled &&
      war3shader::internal::HasAnyRenderListeners()) {
    if (m_war3Pipeline && m_war3Pipeline->HasInsertedBeforeUi()) {
      war3shader::internal::DispatchRenderEvent(
          war3shader::RenderEventID::UI_RENDER_END);
    }
    war3shader::internal::DispatchRenderEvent(
        war3shader::RenderEventID::FRAME_END);
  }

  War3DebugRunIndexOverflowTest(this);

  return m_implicitSwapchain->Present(
      pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTargetEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface,
    HANDLE *pSharedHandle, DWORD Usage) {
  InitReturnPtr(ppSurface);

  if (unlikely(ppSurface == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(MultiSample > D3DMULTISAMPLE_16_SAMPLES))
    return D3DERR_INVALIDCALL;

  // The new Create functions added in 9Ex only accept the new USAGE flags added
  // with 9Ex. Yes, it actually fails when explicitly passing
  // D3DUSAGE_RENDERTARGET.
  if (unlikely(Usage & ~(D3DUSAGE_RESTRICTED_CONTENT |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)))
    return D3DERR_INVALIDCALL;

  if (unlikely((Usage & (D3DUSAGE_RESTRICT_SHARED_RESOURCE |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)) != 0 &&
               pSharedHandle == nullptr))
    return D3DERR_INVALIDCALL;

  // Check if the sample count is valid and supported and
  // specifically return D3DERR_NOTAVAILABLE on failure.
  if (FAILED(DecodeMultiSampleType(m_dxvkDevice, MultiSample,
                                   MultisampleQuality, nullptr)))
    return D3DERR_NOTAVAILABLE;

  D3D9_COMMON_TEXTURE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Depth = 1;
  desc.ArraySize = 1;
  desc.MipLevels = 1;
  desc.Usage = Usage | D3DUSAGE_RENDERTARGET;
  desc.Format = EnumerateFormat(Format);
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Discard = FALSE;
  desc.MultiSample = MultiSample;
  desc.MultisampleQuality = MultisampleQuality;
  desc.IsBackBuffer = FALSE;
  desc.IsAttachmentOnly = TRUE;
  desc.IsLockable = Lockable;

  if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(
          this, D3DRTYPE_SURFACE, &desc)))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
               !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_SURFACE, desc)))
    return E_INVALIDARG;

  try {
    const Com<D3D9Surface> surface =
        new D3D9Surface(this, &desc, IsExtended(), nullptr, pSharedHandle);
    m_initializer->InitTexture(surface->GetCommonTexture());
    *ppSurface = surface.ref();
    m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_OUTOFVIDEOMEMORY;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateOffscreenPlainSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage) {
  InitReturnPtr(ppSurface);

  if (unlikely(ppSurface == nullptr))
    return D3DERR_INVALIDCALL;

  // The new Create functions added in 9Ex only accept the new USAGE flags added
  // with 9Ex.
  if (unlikely(Usage & ~(D3DUSAGE_RESTRICTED_CONTENT |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)))
    return D3DERR_INVALIDCALL;

  if (unlikely((Usage & (D3DUSAGE_RESTRICT_SHARED_RESOURCE |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)) != 0 &&
               pSharedHandle == nullptr))
    return D3DERR_INVALIDCALL;

  D3D9_COMMON_TEXTURE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Depth = 1;
  desc.ArraySize = 1;
  desc.MipLevels = 1;
  desc.Usage = Usage;
  desc.Format = EnumerateFormat(Format);
  desc.Pool = Pool;
  desc.Discard = FALSE;
  desc.MultiSample = D3DMULTISAMPLE_NONE;
  desc.MultisampleQuality = 0;
  desc.IsBackBuffer = FALSE;
  desc.IsAttachmentOnly = TRUE;
  // Docs: Off-screen plain surfaces are always lockable, regardless of their
  // pool types.
  desc.IsLockable = TRUE;

  // Because they are always lockable, image surfaces / offscreen plain surfaces
  // are restricted to using lockable depth stencil formats.
  if (unlikely(IsDepthStencilFormat(desc.Format) &&
               !IsLockableDepthStencilFormat(desc.Format)))
    return D3DERR_INVALIDCALL;

  if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(
          this, D3DRTYPE_SURFACE, &desc)))
    return D3DERR_INVALIDCALL;

  try {
    void *initialData = nullptr;

    // On Windows Vista (so most likely D3D9Ex), pSharedHandle can be used to
    // pass initial data for an offscreen plain surface, but only for a very
    // specific type of offscreen plain surface.
    if (unlikely(pSharedHandle != nullptr && Pool == D3DPOOL_SYSTEMMEM)) {
      initialData = *(reinterpret_cast<void **>(pSharedHandle));
      pSharedHandle = nullptr;
    }

    // Shared offscreen plain surfaces have to be in POOL_DEFAULT
    if (unlikely(pSharedHandle != nullptr && Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    if (unlikely(
            pSharedHandle != nullptr && *pSharedHandle != nullptr &&
            !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_SURFACE, desc)))
      return E_INVALIDARG;

    const Com<D3D9Surface> surface =
        new D3D9Surface(this, &desc, IsExtended(), nullptr, pSharedHandle);
    m_initializer->InitTexture(surface->GetCommonTexture(), initialData);
    *ppSurface = surface.ref();

    if (desc.Pool == D3DPOOL_DEFAULT)
      m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_OUTOFVIDEOMEMORY;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateDepthStencilSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface,
    HANDLE *pSharedHandle, DWORD Usage) {
  InitReturnPtr(ppSurface);

  if (unlikely(ppSurface == nullptr))
    return D3DERR_INVALIDCALL;

  // The new Create functions added in 9Ex only accept the new USAGE flags added
  // with 9Ex. Yes, it actually fails when explicitly passing
  // D3DUSAGE_DEPTHSTENCIL.
  if (unlikely(Usage & ~(D3DUSAGE_RESTRICTED_CONTENT |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)))
    return D3DERR_INVALIDCALL;

  if (unlikely((Usage & (D3DUSAGE_RESTRICT_SHARED_RESOURCE |
                         D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)) != 0 &&
               pSharedHandle == nullptr))
    return D3DERR_INVALIDCALL;

  D3D9_COMMON_TEXTURE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Depth = 1;
  desc.ArraySize = 1;
  desc.MipLevels = 1;
  desc.Usage = Usage | D3DUSAGE_DEPTHSTENCIL;
  desc.Format = EnumerateFormat(Format);
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Discard = Discard;
  desc.MultiSample = MultiSample;
  desc.MultisampleQuality = MultisampleQuality;
  desc.IsBackBuffer = FALSE;
  desc.IsAttachmentOnly = TRUE;
  desc.IsLockable = IsLockableDepthStencilFormat(desc.Format);

  if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(
          this, D3DRTYPE_SURFACE, &desc)))
    return D3DERR_INVALIDCALL;

  if (unlikely(pSharedHandle != nullptr && *pSharedHandle != nullptr &&
               !ValidateSharedTexture(*pSharedHandle, D3DRTYPE_SURFACE, desc)))
    return E_INVALIDARG;

  try {
    const Com<D3D9Surface> surface =
        new D3D9Surface(this, &desc, IsExtended(), nullptr, pSharedHandle);
    m_initializer->InitTexture(surface->GetCommonTexture());
    *ppSurface = surface.ref();
    m_losableResourceCounter++;

    return D3D_OK;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_OUTOFVIDEOMEMORY;
  }
}

HRESULT STDMETHODCALLTYPE
D3D9DeviceEx::ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters,
                      D3DDISPLAYMODEEX *pFullscreenDisplayMode) {
  D3D9DeviceLock lock = LockDevice();

  HRESULT hr;
  if (likely(m_deviceType != D3DDEVTYPE_NULLREF)) {
    hr = m_parent->ValidatePresentationParametersEx(pPresentationParameters,
                                                    pFullscreenDisplayMode);

    if (unlikely(FAILED(hr)))
      return hr;
  }

  hr = ResetSwapChain(pPresentationParameters, pFullscreenDisplayMode);
  if (FAILED(hr))
    return hr;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDisplayModeEx(
    UINT iSwapChain, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  if (unlikely(iSwapChain != 0))
    return D3DERR_INVALIDCALL;

  return m_implicitSwapchain->GetDisplayModeEx(pMode, pRotation);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateAdditionalSwapChainEx(
    D3DPRESENT_PARAMETERS *pPresentationParameters,
    const D3DDISPLAYMODEEX *pFullscreenDisplayMode,
    IDirect3DSwapChain9 **ppSwapChain) {
  D3D9DeviceLock lock = LockDevice();

  InitReturnPtr(ppSwapChain);

  if (ppSwapChain == nullptr || pPresentationParameters == nullptr)
    return D3DERR_INVALIDCALL;

  // Additional fullscreen swapchains are forbidden.
  if (!pPresentationParameters->Windowed)
    return D3DERR_INVALIDCALL;

  // We can't make another swapchain if we are fullscreen.
  if (!m_implicitSwapchain->GetPresentParams()->Windowed)
    return D3DERR_INVALIDCALL;

  if (unlikely(IsDeviceLost())) {
    return D3DERR_DEVICELOST;
  }

  m_implicitSwapchain->Invalidate(pPresentationParameters->hDeviceWindow);

  try {
    auto *swapchain = new D3D9SwapChainEx(this, pPresentationParameters,
                                          pFullscreenDisplayMode, false);
    *ppSwapChain = ref(swapchain);
    m_losableResourceCounter++;
  } catch (const DxvkError &e) {
    Logger::err(e.message());
    return D3DERR_NOTAVAILABLE;
  }

  return D3D_OK;
}

HRESULT D3D9DeviceEx::SetStateSamplerState(DWORD StateSampler,
                                           D3DSAMPLERSTATETYPE Type,
                                           DWORD Value) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(ShouldRecord()))
    return m_recorder->SetStateSamplerState(StateSampler, Type, Value);

  auto &state = m_state.samplerStates;

  if (state[StateSampler][Type] == Value)
    return D3D_OK;

  state[StateSampler][Type] = Value;

  const uint32_t samplerBit = 1u << StateSampler;

  if (Type == D3DSAMP_ADDRESSU || Type == D3DSAMP_ADDRESSV ||
      Type == D3DSAMP_ADDRESSW || Type == D3DSAMP_MAGFILTER ||
      Type == D3DSAMP_MINFILTER || Type == D3DSAMP_MIPFILTER ||
      Type == D3DSAMP_MAXANISOTROPY || Type == D3DSAMP_MIPMAPLODBIAS ||
      Type == D3DSAMP_MAXMIPLEVEL || Type == D3DSAMP_BORDERCOLOR)
    m_textureSlotTracking.samplerStateDirty |= samplerBit;
  else if (Type == D3DSAMP_SRGBTEXTURE &&
           (m_textureSlotTracking.bound & samplerBit))
    m_textureSlotTracking.textureDirty |= samplerBit;

  constexpr DWORD Fetch4Enabled = MAKEFOURCC('G', 'E', 'T', '4');
  constexpr DWORD Fetch4Disabled = MAKEFOURCC('G', 'E', 'T', '1');

  if (unlikely(Type == D3DSAMP_MIPMAPLODBIAS)) {
    if (unlikely(Value == Fetch4Enabled))
      m_textureSlotTracking.fetch4SamplerState |= samplerBit;
    else if (unlikely(Value == Fetch4Disabled))
      m_textureSlotTracking.fetch4SamplerState &= ~samplerBit;

    UpdateActiveFetch4(StateSampler);
  }

  if (unlikely(Type == D3DSAMP_MAGFILTER &&
               (m_textureSlotTracking.fetch4SamplerState & samplerBit)))
    UpdateActiveFetch4(StateSampler);

  return D3D_OK;
}

HRESULT D3D9DeviceEx::SetStateTexture(DWORD StateSampler,
                                      IDirect3DBaseTexture9 *pTexture) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(ShouldRecord()))
    return m_recorder->SetStateTexture(StateSampler, pTexture);

  if (m_state.textures[StateSampler] == pTexture)
    return D3D_OK;

  auto oldTexture = GetCommonTexture(m_state.textures[StateSampler]);
  auto newTexture = GetCommonTexture(pTexture);

  // We need to check our ops and disable respective stages.
  // Given we have transition from a null resource to
  // a valid resource or vice versa.
  const bool isPSSampler = StateSampler < caps::MaxTexturesPS;
  if (isPSSampler) {
    // If we either bind a new texture or unbind the old one,
    // we need to update the fixed function shader
    // because we generate a different shader based on whether each texture is
    // bound.
    if (newTexture == nullptr || oldTexture == nullptr)
      m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
  }

  bool oldTextureIsCube = oldTexture != nullptr && oldTexture->IsCube();
  bool newTextureIsCube = newTexture != nullptr && newTexture->IsCube();
  if (unlikely(oldTextureIsCube != newTextureIsCube)) {
    m_textureSlotTracking.samplerStateDirty |= 1u << StateSampler;
  }

  DWORD oldUsage = oldTexture != nullptr ? oldTexture->Desc()->Usage : 0;
  DWORD newUsage = newTexture != nullptr ? newTexture->Desc()->Usage : 0;
  DWORD combinedUsage = oldUsage | newUsage;
  TextureChangePrivate(m_state.textures[StateSampler], pTexture);
  m_textureSlotTracking.textureDirty |= 1u << StateSampler;
  UpdateTextureBitmasks(StateSampler, combinedUsage);

  return D3D_OK;
}

HRESULT D3D9DeviceEx::SetStateTransform(uint32_t idx,
                                        const D3DMATRIX *pMatrix) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(ShouldRecord()))
    return m_recorder->SetStateTransform(idx, pMatrix);

  m_state.transforms[idx] = ConvertMatrix(pMatrix);

  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

  if (idx == GetTransformIndex(D3DTS_VIEW) ||
      idx >= GetTransformIndex(D3DTS_WORLD))
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);

  return D3D_OK;
}

HRESULT D3D9DeviceEx::SetStateTextureStageState(DWORD Stage,
                                                D3D9TextureStageStateTypes Type,
                                                DWORD Value) {

  // Clamp values instead of checking and returning INVALID_CALL
  // Matches tests + Dawn of Magic 2 relies on it.
  Stage = std::min(Stage, DWORD(caps::TextureStageCount - 1));
  Type = std::min(Type, D3D9TextureStageStateTypes(DXVK_TSS_COUNT - 1));

  D3D9DeviceLock lock = LockDevice();

  if (unlikely(ShouldRecord()))
    return m_recorder->SetStateTextureStageState(Stage, Type, Value);

  if (likely(m_state.textureStages[Stage][Type] != Value)) {
    m_state.textureStages[Stage][Type] = Value;

    switch (Type) {
    case DXVK_TSS_COLOROP:
    case DXVK_TSS_COLORARG0:
    case DXVK_TSS_COLORARG1:
    case DXVK_TSS_COLORARG2:
    case DXVK_TSS_ALPHAOP:
    case DXVK_TSS_ALPHAARG0:
    case DXVK_TSS_ALPHAARG1:
    case DXVK_TSS_ALPHAARG2:
    case DXVK_TSS_RESULTARG:
      m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
      break;

    case DXVK_TSS_TEXCOORDINDEX:
      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      break;

    case DXVK_TSS_TEXTURETRANSFORMFLAGS:
      m_textureSlotTracking.projected &= ~(1 << Stage);
      if (Value & D3DTTFF_PROJECTED)
        m_textureSlotTracking.projected |= 1 << Stage;

      m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
      m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
      break;

    case DXVK_TSS_BUMPENVMAT00:
    case DXVK_TSS_BUMPENVMAT01:
    case DXVK_TSS_BUMPENVMAT10:
    case DXVK_TSS_BUMPENVMAT11:
    case DXVK_TSS_BUMPENVLSCALE:
    case DXVK_TSS_BUMPENVLOFFSET:
    case DXVK_TSS_CONSTANT:
      m_dirty.set(D3D9DeviceDirtyFlag::SharedPixelShaderData);
      break;

    default:
      break;
    }
  }

  return D3D_OK;
}

bool D3D9DeviceEx::IsExtended() { return m_parent->IsExtended(); }

bool D3D9DeviceEx::SupportsSWVP() {
  return m_dxvkDevice->features()
             .core.features.vertexPipelineStoresAndAtomics &&
         m_dxvkDevice->features().vk12.shaderInt8;
}

bool D3D9DeviceEx::SupportsVCacheQuery() const {
  return m_adapter->GetVendorId() == uint32_t(DxvkGpuVendor::Nvidia);
}

HWND D3D9DeviceEx::GetWindow() { return m_window; }

void D3D9DeviceEx::DetermineConstantLayouts(bool canSWVP) {
  D3D9ConstantSets &vsConstSet = m_consts[DxsoProgramType::VertexShader];
  vsConstSet.layout.floatCount =
      canSWVP ? caps::MaxFloatConstantsSoftware : caps::MaxFloatConstantsVS;
  vsConstSet.layout.intCount =
      canSWVP ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
  vsConstSet.layout.boolCount =
      canSWVP ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
  vsConstSet.layout.bitmaskCount = align(vsConstSet.layout.boolCount, 32) / 32;

  D3D9ConstantSets &psConstSet = m_consts[DxsoProgramType::PixelShader];
  psConstSet.layout.floatCount = caps::MaxSM3FloatConstantsPS;
  psConstSet.layout.intCount = caps::MaxOtherConstants;
  psConstSet.layout.boolCount = caps::MaxOtherConstants;
  psConstSet.layout.bitmaskCount = align(psConstSet.layout.boolCount, 32) / 32;
}

D3D9BufferSlice D3D9DeviceEx::AllocUPBuffer(VkDeviceSize size) {
  constexpr VkDeviceSize UPBufferSize = 1 << 20;

  if (unlikely(m_upBuffer == nullptr || size > UPBufferSize)) {
    VkMemoryPropertyFlags memoryFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    DxvkBufferCreateInfo info;
    info.size = std::max(UPBufferSize, size);
    info.usage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    info.access =
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    info.stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    info.debugName = "UP buffer";

    Rc<DxvkBuffer> buffer = m_dxvkDevice->createBuffer(info, memoryFlags);
    void *mapPtr = buffer->mapPtr(0);

    if (size <= UPBufferSize) {
      m_upBuffer = std::move(buffer);
      m_upBufferMapPtr = mapPtr;
      // 记录当前 backing allocation，供 War3 shadow 捕获固定 VkBuffer 使用
      m_upBufferAllocation = m_upBuffer->storage();
    } else {
      // Temporary buffer
      D3D9BufferSlice result;
      result.slice = DxvkBufferSlice(std::move(buffer), 0, size);
      result.mapPtr = mapPtr;
      return result;
    }
  }

  VkDeviceSize alignedSize = align(size, CACHE_LINE_SIZE);

  if (unlikely(m_upBufferOffset + alignedSize > UPBufferSize)) {
    auto slice = m_upBuffer->allocateStorage();

    m_upBufferOffset = 0;
    m_upBufferMapPtr = slice->mapPtr();
    // 记录“即将生效”的新 allocation。注意：invalidateBuffer 在 CS 线程执行，
    // 但本次 draw 产生的数据已经写入该 allocation 的 mapPtr，需要提前固化。
    m_upBufferAllocation = slice;

    EmitCs([cBuffer = m_upBuffer,
            cSlice = std::move(slice)](DxvkContext *ctx) mutable {
      ctx->invalidateBuffer(cBuffer, std::move(cSlice));
    });
  }

  D3D9BufferSlice result;
  result.slice = DxvkBufferSlice(m_upBuffer, m_upBufferOffset, size);
  result.mapPtr = reinterpret_cast<char *>(m_upBufferMapPtr) + m_upBufferOffset;

  m_upBufferOffset += alignedSize;
  return result;
}

D3D9BufferSlice D3D9DeviceEx::AllocStagingBuffer(VkDeviceSize size) {
  D3D9BufferSlice result;
  result.slice = m_stagingBuffer.alloc(size);
  result.mapPtr = result.slice.mapPtr(0);
  return result;
}

void D3D9DeviceEx::ThrottleAllocation() {
  // Treshold for staging memory in flight. Since the staging buffer granularity
  // is somewhat coars, it is possible for one additional allocation to be in
  // use, but otherwise this is a hard upper bound.
  constexpr VkDeviceSize MaxStagingMemoryInFlight =
      env::is32BitHostPlatform() ? StagingBufferSize * 4
                                 : StagingBufferSize * 16;

  // Threshold at which to submit eagerly. This is useful to ensure
  // that staging buffer memory gets recycled relatively soon.
  constexpr VkDeviceSize MaxStagingMemoryPerSubmission =
      MaxStagingMemoryInFlight / 3u;

  DxvkStagingBufferStats stats = GetStagingMemoryStatistics();

  VkDeviceSize stagingBufferAllocated = stats.allocatedTotal;

  if (stagingBufferAllocated >
      m_stagingMemorySignaled + MaxStagingMemoryPerSubmission) {
    // Perform submission. If the amount of staging memory allocated since the
    // last submission exceeds the hard limit, we need to submit to guarantee
    // forward progress. Ideally, this should not happen very often.
    GpuFlushType flushType =
        stagingBufferAllocated <=
                m_stagingMemorySignaled + MaxStagingMemoryInFlight
            ? GpuFlushType::ImplicitSynchronization
            : GpuFlushType::ExplicitFlush;

    ConsiderFlush(flushType);
  }

  // Wait for staging memory to get recycled.
  if (stagingBufferAllocated > MaxStagingMemoryInFlight)
    m_dxvkDevice->waitForFence(*m_stagingBufferFence,
                               stagingBufferAllocated -
                                   MaxStagingMemoryInFlight);
}

DxvkStagingBufferStats D3D9DeviceEx::GetStagingMemoryStatistics() const {
  DxvkStagingBufferStats stats = m_stagingBuffer.getStatistics();
  stats.allocatedTotal += m_discardMemoryCounter;
  stats.allocatedSinceLastReset +=
      m_discardMemoryCounter - m_discardMemoryOnFlush;
  return stats;
}

D3D9_VK_FORMAT_MAPPING D3D9DeviceEx::LookupFormat(D3D9Format Format) const {
  return m_adapter->GetFormatMapping(Format);
}

const DxvkFormatInfo *
D3D9DeviceEx::UnsupportedFormatInfo(D3D9Format Format) const {
  return m_adapter->GetUnsupportedFormatInfo(Format);
}

bool D3D9DeviceEx::WaitForResource(const DxvkPagedResource &Resource,
                                   uint64_t SequenceNumber, DWORD MapFlags) {
  // Wait for the any pending D3D9 command to be executed
  // on the CS thread so that we can determine whether the
  // resource is currently in use or not.

  // Determine access type to wait for based on map mode
  DxvkAccess access =
      (MapFlags & D3DLOCK_READONLY) ? DxvkAccess::Write : DxvkAccess::Read;

  if (!Resource.isInUse(access))
    SynchronizeCsThread(SequenceNumber);

  if (Resource.isInUse(access)) {
    if (MapFlags & D3DLOCK_DONOTWAIT) {
      // We don't have to wait, but misbehaving games may
      // still try to spin on `Map` until the resource is
      // idle, so we should flush pending commands
      ConsiderFlush(GpuFlushType::ImplicitWeakHint);
      return false;
    } else {
      // Make sure pending commands using the resource get
      // executed on the the GPU if we have to wait for it
      Flush();
      SynchronizeCsThread(SequenceNumber);

      m_dxvkDevice->waitForResource(Resource, access);
    }
  }

  return true;
}

bool D3D9DeviceEx::War3ShouldDrawDebugOverlay() const {
  if (dxvk::War3Hook::IsInShadowPass())
    return false;
  if (m_war3DebugOverlayDrawn)
    return false;
  if (!war3::ShaderManager::get().hasOverride(
          war3shader::RenderStageId::Overlay))
    return false;
  return true;
}

void D3D9DeviceEx::War3DrawDebugOverlayTriangle() {
  if (!War3ShouldDrawDebugOverlay())
    return;

  auto *material = war3::ShaderManager::get().getMaterial(
      war3shader::RenderStageId::Overlay);
  if (material == nullptr || !material->isCompiled())
    return;

  if (m_war3DebugOverlayDecl == nullptr) {
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
         0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,
         0},
        D3DDECL_END()};
    Com<IDirect3DVertexDeclaration9> decl;
    if (FAILED(CreateVertexDeclaration(elements, &decl))) {
      Logger::warn("War3DebugOverlay: 创建顶点声明失败");
      return;
    }
    m_war3DebugOverlayDecl = std::move(decl);
  }

  const auto prevDecl = m_state.vertexDecl;
  const auto prevVs = m_state.vertexShader;
  const auto prevPs = m_state.pixelShader;
  const D3D9VBO prevVbo = m_state.vertexBuffers[0];
  const std::array<std::pair<D3DRENDERSTATETYPE, DWORD>, 8> prevStates = {{
      {D3DRS_ZENABLE, m_state.renderStates[D3DRS_ZENABLE]},
      {D3DRS_ZWRITEENABLE, m_state.renderStates[D3DRS_ZWRITEENABLE]},
      {D3DRS_ZFUNC, m_state.renderStates[D3DRS_ZFUNC]},
      {D3DRS_CULLMODE, m_state.renderStates[D3DRS_CULLMODE]},
      {D3DRS_ALPHABLENDENABLE, m_state.renderStates[D3DRS_ALPHABLENDENABLE]},
      {D3DRS_SRCBLEND, m_state.renderStates[D3DRS_SRCBLEND]},
      {D3DRS_DESTBLEND, m_state.renderStates[D3DRS_DESTBLEND]},
      {D3DRS_ALPHATESTENABLE, m_state.renderStates[D3DRS_ALPHATESTENABLE]},
  }};

  War3UpdateMaterialUniforms(material, War3MaterialKind::PostProcess);
  material->apply(this);

  SetVertexDeclaration(m_war3DebugOverlayDecl.ptr());
  SetRenderState(D3DRS_ZENABLE, FALSE);
  SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
  SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

  struct War3DebugOverlayVertex {
    float x;
    float y;
    float z;
    float w;
    float r;
    float g;
    float b;
    float a;
  };

  const War3DebugOverlayVertex verts[3] = {
      {-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
      {3.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
      {-1.0f, 3.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
  };

  const uint32_t vertexCount = 3;
  const uint32_t stride = sizeof(War3DebugOverlayVertex);

  PrepareDraw(D3DPT_TRIANGLELIST, false, false);

  const uint32_t dataSize = GetUPDataSize(vertexCount, stride);
  const uint32_t bufferSize = GetUPBufferSize(vertexCount, stride);
  auto upSlice = AllocUPBuffer(bufferSize);
  FillUPVertexBuffer(upSlice.mapPtr, verts, dataSize, bufferSize);

  EmitCs([this, cBufferSlice = std::move(upSlice.slice), cStride = stride,
          cVertexCount = vertexCount](DxvkContext *ctx) mutable {
    ApplyPrimitiveType(ctx, D3DPT_TRIANGLELIST);
    VkDrawIndirectCommand draw = {};
    draw.vertexCount = cVertexCount;
    draw.instanceCount = 1u;
    ctx->bindVertexBuffer(0, std::move(cBufferSlice), cStride);
    ctx->draw(1u, &draw);
    ctx->bindVertexBuffer(0, DxvkBufferSlice(), 0);
  });

  SetVertexDeclaration(prevDecl.ptr());
  SetVertexShader(prevVs.ptr());
  SetPixelShader(prevPs.ptr());
  for (const auto &state : prevStates) {
    SetRenderState(state.first, state.second);
  }
  SetStreamSource(0, prevVbo.vertexBuffer.ptr(), prevVbo.offset,
                  prevVbo.stride);

  m_war3DebugOverlayDrawn = true;
}

uint32_t D3D9DeviceEx::CalcImageLockOffset(uint32_t SlicePitch,
                                           uint32_t RowPitch,
                                           const DxvkFormatInfo *FormatInfo,
                                           const D3DBOX *pBox) {
  if (pBox == nullptr)
    return 0;

  std::array<uint32_t, 3> offsets = {pBox->Front, pBox->Top, pBox->Left};

  uint32_t elementSize = 1;

  if (FormatInfo != nullptr) {
    elementSize = FormatInfo->elementSize;
    VkExtent3D blockSize = FormatInfo->blockSize;
    if (unlikely(FormatInfo->flags.test(DxvkFormatFlag::MultiPlane))) {
      elementSize = FormatInfo->planes[0].elementSize;
      blockSize = {FormatInfo->planes[0].blockSize.width,
                   FormatInfo->planes[0].blockSize.height, 1u};
    }

    offsets[0] = offsets[0] / blockSize.depth;
    offsets[1] = offsets[1] / blockSize.height;
    offsets[2] = offsets[2] / blockSize.width;
  }

  return offsets[0] * SlicePitch + offsets[1] * RowPitch +
         offsets[2] * elementSize;
}

HRESULT D3D9DeviceEx::LockImage(D3D9CommonTexture *pResource, UINT Face,
                                UINT MipLevel, D3DLOCKED_BOX *pLockedBox,
                                const D3DBOX *pBox, DWORD Flags) {
  D3D9DeviceLock lock = LockDevice();

  UINT Subresource = pResource->CalcSubresource(Face, MipLevel);

  // Don't allow multiple lockings.
  if (unlikely(pResource->GetLocked(Subresource)))
    return D3DERR_INVALIDCALL;

  auto &desc = *(pResource->Desc());

  if (unlikely((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) ==
                   (D3DLOCK_DISCARD | D3DLOCK_READONLY) &&
               desc.Pool == D3DPOOL_DEFAULT))
    return D3DERR_INVALIDCALL;

  // We only ever wait for textures that were used with GetRenderTargetData or
  // GetFrontBufferData anyway. Games like Beyond Good and Evil break if this
  // doesn't succeed.
  Flags &= ~D3DLOCK_DONOTWAIT;

  if (unlikely((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) ==
               (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)))
    Flags &= ~D3DLOCK_DISCARD;

  // Tests show that D3D9 drivers ignore DISCARD when the device is lost.
  if (unlikely(m_deviceLostState != D3D9DeviceLostState::Ok))
    Flags &= ~D3DLOCK_DISCARD;

  if (unlikely(!desc.IsLockable))
    return D3DERR_INVALIDCALL;

  if (unlikely(pBox != nullptr)) {
    D3DRESOURCETYPE type = pResource->GetType();
    D3D9_FORMAT_BLOCK_SIZE blockSize = GetFormatAlignedBlockSize(desc.Format);

    bool isBlockAlignedFormat = blockSize.Width > 0 && blockSize.Height > 0;
    bool isNotLeftAligned = pBox->Left && (pBox->Left & (blockSize.Width - 1));
    bool isNotTopAligned = pBox->Top && (pBox->Top & (blockSize.Height - 1));
    bool isNotRightAligned =
        pBox->Right && (pBox->Right & (blockSize.Width - 1));
    bool isNotBottomAligned =
        pBox->Bottom && (pBox->Bottom & (blockSize.Height - 1));

    // LockImage calls on D3DPOOL_DEFAULT surfaces and volume textures with
    // formats which need to be block aligned, must be validated for mip level
    // 0.
    if (MipLevel == 0 && isBlockAlignedFormat &&
        (type == D3DRTYPE_VOLUMETEXTURE || desc.Pool == D3DPOOL_DEFAULT) &&
        (isNotLeftAligned || isNotTopAligned || isNotRightAligned ||
         isNotBottomAligned))
      return D3DERR_INVALIDCALL;
  }

  auto &formatMapping = pResource->GetFormatMapping();
  const DxvkFormatInfo *formatInfo =
      formatMapping.IsValid()
          ? lookupFormatInfo(formatMapping.FormatColor)
          : UnsupportedFormatInfo(pResource->Desc()->Format);

  auto subresource =
      pResource->GetSubresourceFromIndex(formatInfo->aspectMask, Subresource);

  VkExtent3D levelExtent = pResource->GetExtentMip(MipLevel);
  VkExtent3D blockCount =
      util::computeBlockCount(levelExtent, formatInfo->blockSize);

  bool fullResource = pBox == nullptr;
  if (unlikely(!fullResource)) {
    // Check whether the box passed as argument matches or exceeds the entire
    // texture.
    VkOffset3D lockOffset;
    VkExtent3D lockExtent;

    ConvertBox(*pBox, lockOffset, lockExtent);

    fullResource = lockOffset == VkOffset3D{0, 0, 0} &&
                   lockExtent.width >= levelExtent.width &&
                   lockExtent.height >= levelExtent.height &&
                   lockExtent.depth >= levelExtent.depth;
  }

  // If we are not locking the entire image
  // a partial discard is meant to occur.
  // We can't really implement that, so just ignore discard
  // if we are not locking the full resource.

  // DISCARD is also ignored for MANAGED and SYSTEMEM.
  // DISCARD is not ignored for non-DYNAMIC unlike what the docs say.

  if (!fullResource || desc.Pool != D3DPOOL_DEFAULT)
    Flags &= ~D3DLOCK_DISCARD;

  if (desc.Usage & D3DUSAGE_WRITEONLY)
    Flags &= ~D3DLOCK_READONLY;

  // If we recently wrote to the texture on the gpu,
  // then we need to copy -> buffer
  // We are also always dirty if we are a render target,
  // a depth stencil, or auto generate mipmaps.
  bool renderable =
      desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL);
  bool needsReadback = pResource->NeedsReadback(Subresource) || renderable;

  // Skip readback if we discard is specified. We can only do this for textures
  // that have an associated Vulkan image. Any other texture might write to the
  // Vulkan staging buffer directly. (GetBackbufferData for example)
  needsReadback &=
      pResource->GetImage() != nullptr || !(Flags & D3DLOCK_DISCARD);
  pResource->SetNeedsReadback(Subresource, false);

  if (unlikely(pResource->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_BACKED ||
               needsReadback)) {
    // Create mapping buffer if it doesn't exist yet. (POOL_DEFAULT)
    pResource->CreateBuffer(!needsReadback);
  }

  // Don't use MapTexture here to keep the mapped list small while the resource
  // is still locked.
  void *mapPtr = pResource->GetData(Subresource);

  if (unlikely(needsReadback)) {
    // The texture was written to on the GPU.
    // This can be either the image (for D3DPOOL_DEFAULT)
    // or the buffer directly (for D3DPOOL_SYSTEMMEM).

    DxvkBufferSlice mappedBufferSlice = pResource->GetBufferSlice(Subresource);
    const Rc<DxvkBuffer> mappedBuffer = pResource->GetBuffer();

    if (unlikely(
            pResource->GetFormatMapping().ConversionFormatInfo.FormatType !=
            D3D9ConversionFormat_None)) {
      Logger::err(str::format(
          "Reading back format", pResource->Desc()->Format,
          " is not supported. It is uploaded using the fomrat converter."));
    }

    if (pResource->GetImage() != nullptr) {
      Rc<DxvkImage> resourceImage = pResource->GetImage();

      Rc<DxvkImage> mappedImage;
      if (resourceImage->info().sampleCount != 1) {
        mappedImage = pResource->GetResolveImage();
      } else {
        mappedImage = std::move(resourceImage);
      }

      // When using any map mode which requires the image contents
      // to be preserved, and if the GPU has write access to the
      // image, copy the current image contents into the buffer.
      auto subresourceLayers = vk::makeSubresourceLayers(subresource);

      // We need to resolve this, some games
      // lock MSAA render targets even though
      // that's entirely illegal and they explicitly
      // tell us that they do NOT want to lock them...
      //
      // resourceImage is null because the image reference was moved to
      // mappedImage for images that need to be resolved.
      if (resourceImage != nullptr) {
        EmitCs([cMainImage = resourceImage, cResolveImage = mappedImage,
                cSubresource = subresourceLayers](DxvkContext *ctx) {
          VkFormat format = cMainImage->info().format;

          VkImageResolve region;
          region.srcSubresource = cSubresource;
          region.srcOffset = VkOffset3D{0, 0, 0};
          region.dstSubresource = cSubresource;
          region.dstOffset = VkOffset3D{0, 0, 0};
          region.extent = cMainImage->mipLevelExtent(cSubresource.mipLevel);

          ctx->resolveImage(cResolveImage, cMainImage, region, format,
                            getDefaultResolveMode(format),
                            VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
        });
      }

      // if packedFormat is VK_FORMAT_UNDEFINED
      // DxvkContext::copyImageToBuffer will automatically take the format from
      // the image
      VkFormat packedFormat = GetPackedDepthStencilFormat(desc.Format);

      EmitCs([cImageBufferSlice = std::move(mappedBufferSlice),
              cImage = std::move(mappedImage),
              cSubresources = subresourceLayers, cLevelExtent = levelExtent,
              cPackedFormat = packedFormat](DxvkContext *ctx) {
        ctx->copyImageToBuffer(cImageBufferSlice.buffer(),
                               cImageBufferSlice.offset(), 4, 0, cPackedFormat,
                               cImage, cSubresources, VkOffset3D{0, 0, 0},
                               cLevelExtent);
      });
      TrackTextureMappingBufferSequenceNumber(pResource, Subresource);
    }

    // Wait until the buffer is idle which may include the copy (and resolve) we
    // just issued.
    if (!WaitForResource(*mappedBuffer,
                         pResource->GetMappingBufferSequenceNumber(Subresource),
                         Flags))
      return D3DERR_WASSTILLDRAWING;
  }

  const bool atiHack =
      desc.Format == D3D9Format::ATI1 || desc.Format == D3D9Format::ATI2;
  // Set up map pointer.
  if (atiHack) {
    // The API didn't treat this as a block compressed format here.
    // So we need to lie here. The game is expected to use this info and do a
    // workaround. It's stupid. I know.
    pLockedBox->RowPitch = align(std::max(desc.Width >> MipLevel, 1u), 4);
    pLockedBox->SlicePitch =
        pLockedBox->RowPitch * std::max(desc.Height >> MipLevel, 1u);
  } else if (likely(!formatInfo->flags.test(DxvkFormatFlag::MultiPlane))) {
    pLockedBox->RowPitch = align(formatInfo->elementSize * blockCount.width, 4);
    pLockedBox->SlicePitch = pLockedBox->RowPitch * blockCount.height;
  } else {
    auto plane = &formatInfo->planes[0];
    uint32_t planeElementSize = plane->elementSize;
    VkExtent3D planeBlockSize = {plane->blockSize.width,
                                 plane->blockSize.height, 1u};
    VkExtent3D blockCount =
        util::computeBlockCount(levelExtent, planeBlockSize);
    pLockedBox->RowPitch = align(planeElementSize * blockCount.width, 4);
    pLockedBox->SlicePitch = pLockedBox->RowPitch * blockCount.height;
  }

  pResource->SetLocked(Subresource, true);

  // Make sure the amount of mapped texture memory stays below the threshold.
  UnmapTextures();

  const bool readOnly = Flags & D3DLOCK_READONLY;
  const bool noDirtyUpdate = Flags & D3DLOCK_NO_DIRTY_UPDATE;
  if ((desc.Pool == D3DPOOL_DEFAULT || !noDirtyUpdate) && !readOnly) {
    if (pBox && MipLevel != 0) {
      D3DBOX scaledBox = *pBox;
      scaledBox.Left <<= MipLevel;
      scaledBox.Right =
          std::min(scaledBox.Right << MipLevel, pResource->Desc()->Width);
      scaledBox.Top <<= MipLevel;
      scaledBox.Bottom =
          std::min(scaledBox.Bottom << MipLevel, pResource->Desc()->Height);
      scaledBox.Back <<= MipLevel;
      scaledBox.Front =
          std::min(scaledBox.Front << MipLevel, pResource->Desc()->Depth);
      pResource->AddDirtyBox(&scaledBox, Face);
    } else {
      pResource->AddDirtyBox(pBox, Face);
    }
  }

  if (IsPoolManaged(desc.Pool) && !readOnly) {
    // Managed textures are uploaded at draw time.
    pResource->SetNeedsUpload(Subresource, true);

    for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
      // Guaranteed to not be nullptr...
      auto texInfo = GetCommonTexture(m_state.textures[i]);

      if (texInfo == pResource) {
        m_textureSlotTracking.needsUpload |= 1 << i;
      }
    }
  }

  const uint32_t offset =
      CalcImageLockOffset(pLockedBox->SlicePitch, pLockedBox->RowPitch,
                          (!atiHack) ? formatInfo : nullptr, pBox);

  uint8_t *data = reinterpret_cast<uint8_t *>(mapPtr);
  data += offset;
  pLockedBox->pBits = data;
  return D3D_OK;
}

HRESULT D3D9DeviceEx::UnlockImage(D3D9CommonTexture *pResource, UINT Face,
                                  UINT MipLevel) {
  D3D9DeviceLock lock = LockDevice();

  UINT Subresource = pResource->CalcSubresource(Face, MipLevel);

  // Don't allow multiple unlockings, except for D3DRTYPE_TEXTURE
  if (unlikely(!pResource->GetLocked(Subresource))) {
    if (pResource->GetType() == D3DRTYPE_TEXTURE)
      return D3D_OK;
    else
      return D3DERR_INVALIDCALL;
  }

  MapTexture(pResource, Subresource); // Add it to the list of mapped resources
  pResource->SetLocked(Subresource, false);

  // Flush image contents from staging if we aren't read only
  // and we aren't deferring for managed.
  const D3DBOX &box = pResource->GetDirtyBox(Face);
  bool shouldFlush =
      pResource->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_BACKED;
  shouldFlush &=
      box.Left < box.Right && box.Top < box.Bottom && box.Front < box.Back;
  shouldFlush &= !pResource->IsManaged();

  if (shouldFlush) {
    this->FlushImage(pResource, Subresource);
    if (!pResource->IsAnySubresourceLocked())
      pResource->ClearDirtyBoxes();
  }

  // Toss our staging buffer if we're not dynamic
  // and we aren't managed (for sysmem copy.)
  bool shouldToss =
      pResource->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_BACKED;
  shouldToss &= !pResource->IsDynamic();
  shouldToss &= !pResource->IsManaged();
  shouldToss &= !pResource->IsAnySubresourceLocked();

  // The texture converter cannot handle converting back. So just keep textures
  // in memory as a workaround.
  shouldToss &= pResource->GetFormatMapping().ConversionFormatInfo.FormatType ==
                D3D9ConversionFormat_None;

  if (shouldToss)
    pResource->DestroyBuffer();

  UnmapTextures();
  return D3D_OK;
}

HRESULT D3D9DeviceEx::FlushImage(D3D9CommonTexture *pResource,
                                 UINT Subresource) {

  const Rc<DxvkImage> image = pResource->GetImage();
  auto formatInfo = lookupFormatInfo(image->info().format);
  auto subresource =
      pResource->GetSubresourceFromIndex(formatInfo->aspectMask, Subresource);

  const D3DBOX &box = pResource->GetDirtyBox(subresource.arrayLayer);

  // The dirty box is only tracked for mip 0. Scale it for the mip level we're
  // gonna upload.
  VkExtent3D mip0Extent = {box.Right - box.Left, box.Bottom - box.Top,
                           box.Back - box.Front};
  VkExtent3D extent =
      util::computeMipLevelExtent(mip0Extent, subresource.mipLevel);
  VkOffset3D mip0Offset = {int32_t(box.Left), int32_t(box.Top),
                           int32_t(box.Front)};
  VkOffset3D offset =
      util::computeMipLevelOffset(mip0Offset, subresource.mipLevel);

  UpdateTextureFromBuffer(pResource, pResource, Subresource, Subresource,
                          offset, extent, offset);

  if (pResource->IsAutomaticMip())
    MarkTextureMipsDirty(pResource);

  return D3D_OK;
}

void D3D9DeviceEx::UpdateTextureFromBuffer(
    D3D9CommonTexture *pDestTexture, D3D9CommonTexture *pSrcTexture,
    UINT DestSubresource, UINT SrcSubresource, VkOffset3D SrcOffset,
    VkExtent3D SrcExtent, VkOffset3D DestOffset) {
  // Wait until the amount of used staging memory is under a certain threshold
  // to avoid using too much memory and even more so to avoid using too much
  // address space.
  ThrottleAllocation();

  const Rc<DxvkImage> image = pDestTexture->GetImage();

  // Now that data has been written into the buffer,
  // we need to copy its contents into the image

  auto formatInfo =
      lookupFormatInfo(pDestTexture->GetFormatMapping().FormatColor);
  auto srcSubresource = pSrcTexture->GetSubresourceFromIndex(
      formatInfo->aspectMask, SrcSubresource);

  auto dstSubresource = pDestTexture->GetSubresourceFromIndex(
      formatInfo->aspectMask, DestSubresource);
  VkImageSubresourceLayers dstLayers = {dstSubresource.aspectMask,
                                        dstSubresource.mipLevel,
                                        dstSubresource.arrayLayer, 1};

  VkExtent3D dstTexLevelExtent = image->mipLevelExtent(dstSubresource.mipLevel);
  VkExtent3D srcTexLevelExtent = util::computeMipLevelExtent(
      pSrcTexture->GetExtent(), srcSubresource.mipLevel);

  auto convertFormat = pDestTexture->GetFormatMapping().ConversionFormatInfo;

  if (unlikely(pSrcTexture->NeedsReadback(SrcSubresource))) {
    // The src texutre has to be in POOL_SYSTEMEM, so it cannot use AUTOMIPGEN.
    // That means that NeedsReadback is only true if the texture has been used
    // with GetRTData or GetFrontbufferData before. Those functions create a
    // buffer, so the buffer always exists here.
    const Rc<DxvkBuffer> &buffer = pSrcTexture->GetBuffer();
    WaitForResource(*buffer,
                    pSrcTexture->GetMappingBufferSequenceNumber(SrcSubresource),
                    0);
    pSrcTexture->SetNeedsReadback(SrcSubresource, false);
  }

  if (likely(convertFormat.FormatType == D3D9ConversionFormat_None)) {
    // The texture does not use a format that needs to be converted in a compute
    // shader. So we just need to make sure the passed size and offset are not
    // out of range and properly aligned, copy the data to a staging buffer and
    // then copy that on the GPU to the actual image.
    VkOffset3D alignedDestOffset = {
        int32_t(alignDown(DestOffset.x, formatInfo->blockSize.width)),
        int32_t(alignDown(DestOffset.y, formatInfo->blockSize.height)),
        int32_t(alignDown(DestOffset.z, formatInfo->blockSize.depth))};
    VkOffset3D alignedSrcOffset = {
        int32_t(alignDown(SrcOffset.x, formatInfo->blockSize.width)),
        int32_t(alignDown(SrcOffset.y, formatInfo->blockSize.height)),
        int32_t(alignDown(SrcOffset.z, formatInfo->blockSize.depth))};
    SrcExtent.width += SrcOffset.x - alignedSrcOffset.x;
    SrcExtent.height += SrcOffset.y - alignedSrcOffset.y;
    SrcExtent.depth += SrcOffset.z - alignedSrcOffset.z;
    VkExtent3D extentBlockCount =
        util::computeBlockCount(SrcExtent, formatInfo->blockSize);
    VkExtent3D alignedExtent =
        util::computeBlockExtent(extentBlockCount, formatInfo->blockSize);

    alignedExtent =
        util::snapExtent3D(alignedDestOffset, alignedExtent, dstTexLevelExtent);
    alignedExtent =
        util::snapExtent3D(alignedSrcOffset, alignedExtent, srcTexLevelExtent);

    VkOffset3D srcOffsetBlockCount =
        util::computeBlockOffset(alignedSrcOffset, formatInfo->blockSize);
    VkExtent3D srcTexLevelExtentBlockCount =
        util::computeBlockCount(srcTexLevelExtent, formatInfo->blockSize);
    VkDeviceSize pitch =
        align(srcTexLevelExtentBlockCount.width * formatInfo->elementSize, 4);
    VkDeviceSize copySrcOffset =
        srcOffsetBlockCount.z * srcTexLevelExtentBlockCount.height * pitch +
        srcOffsetBlockCount.y * pitch +
        srcOffsetBlockCount.x * formatInfo->elementSize;

    // Get the mapping pointer from MapTexture to map the texture and keep track
    // of that in case it is unmappable.
    const void *mapPtr = MapTexture(pSrcTexture, SrcSubresource);
    VkDeviceSize dirtySize = extentBlockCount.width * extentBlockCount.height *
                             extentBlockCount.depth * formatInfo->elementSize;
    D3D9BufferSlice slice = AllocStagingBuffer(dirtySize);
    const void *srcData =
        reinterpret_cast<const uint8_t *>(mapPtr) + copySrcOffset;
    util::packImageData(slice.mapPtr, srcData, extentBlockCount,
                        formatInfo->elementSize, pitch,
                        pitch * srcTexLevelExtentBlockCount.height);

    VkFormat packedDSFormat =
        GetPackedDepthStencilFormat(pDestTexture->Desc()->Format);

    EmitCs([cSrcSlice = slice.slice, cDstImage = image, cDstLayers = dstLayers,
            cDstLevelExtent = alignedExtent, cOffset = alignedDestOffset,
            cPackedDSFormat = packedDSFormat](DxvkContext *ctx) {
      ctx->copyBufferToImage(cDstImage, cDstLayers, cOffset, cDstLevelExtent,
                             cSrcSlice.buffer(), cSrcSlice.offset(), 0, 0,
                             cPackedDSFormat);
    });

    TrackTextureMappingBufferSequenceNumber(pSrcTexture, SrcSubresource);
  } else {
    // The texture uses a format which gets converted by a compute shader.
    const void *mapPtr = MapTexture(pSrcTexture, SrcSubresource);

    // The compute shader does not support only converting a subrect of the
    // texture
    if (unlikely(SrcOffset.x != 0 || SrcOffset.y != 0 || SrcOffset.z != 0 ||
                 DestOffset.x != 0 || DestOffset.y != 0 || DestOffset.z != 0 ||
                 SrcExtent != srcTexLevelExtent)) {
      Logger::warn("Offset and rect not supported with the texture converter.");
    }

    if (unlikely(srcTexLevelExtent != dstTexLevelExtent)) {
      Logger::err(
          "Different extents are not supported with the texture converter.");
      return;
    }

    uint32_t formatElementSize = formatInfo->elementSize;
    VkExtent3D srcBlockSize = formatInfo->blockSize;
    if (formatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
      formatElementSize = formatInfo->planes[0].elementSize;
      srcBlockSize = {formatInfo->planes[0].blockSize.width,
                      formatInfo->planes[0].blockSize.height, 1u};
    }
    VkExtent3D srcBlockCount =
        util::computeBlockCount(srcTexLevelExtent, srcBlockSize);
    srcBlockCount.height *= std::min(pSrcTexture->GetPlaneCount(), 2u);

    // the converter can not handle the 4 aligned pitch so we always repack into
    // a staging buffer
    D3D9BufferSlice slice =
        AllocStagingBuffer(pSrcTexture->GetMipSize(SrcSubresource));
    VkDeviceSize pitch = align(srcBlockCount.width * formatElementSize, 4);

    const DxvkFormatInfo *convertedFormatInfo =
        lookupFormatInfo(convertFormat.FormatColor);
    VkImageSubresourceLayers convertedDstLayers = {
        convertedFormatInfo->aspectMask, dstSubresource.mipLevel,
        dstSubresource.arrayLayer, 1};

    util::packImageData(slice.mapPtr, mapPtr, srcBlockCount, formatElementSize,
                        pitch,
                        std::min(pSrcTexture->GetPlaneCount(), 2u) * pitch *
                            srcBlockCount.height);

    EmitCs([this, cConvertFormat = convertFormat, cDstImage = std::move(image),
            cDstLayers = convertedDstLayers,
            cSrcSlice = std::move(slice.slice)](DxvkContext *ctx) {
      auto contextObjects = ctx->beginExternalRendering();

      m_converter->ConvertFormat(contextObjects, cConvertFormat, cDstImage,
                                 cDstLayers, cSrcSlice);
    });
  }
  UnmapTextures();
  ConsiderFlush(GpuFlushType::ImplicitWeakHint);
}

void D3D9DeviceEx::EmitGenerateMips(D3D9CommonTexture *pResource) {
  if (pResource->IsManaged())
    UploadManagedTexture(pResource);

  EmitCs([cImageView = pResource->GetSampleView(false),
          cFilter = pResource->GetMipFilter()](DxvkContext *ctx) {
    ctx->generateMipmaps(cImageView, DecodeFilter(cFilter));
  });
}

HRESULT D3D9DeviceEx::LockBuffer(D3D9CommonBuffer *pResource, UINT OffsetToLock,
                                 UINT SizeToLock, void **ppbData, DWORD Flags) {
  D3D9DeviceLock lock = LockDevice();

  if (unlikely(ppbData == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(!m_d3d9Options.allowDiscard))
    Flags &= ~D3DLOCK_DISCARD;

  auto &desc = *pResource->Desc();

  // Ignore DISCARD if NOOVERWRITE or READONLY is set
  if (unlikely((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE |
                         D3DLOCK_READONLY)) != D3DLOCK_DISCARD))
    Flags &= ~D3DLOCK_DISCARD;

  // Ignore DISCARD and NOOVERWRITE if the buffer is not DEFAULT pool (tests +
  // Halo 2) The docs say DISCARD and NOOVERWRITE are ignored if the buffer is
  // not DYNAMIC but tests say otherwise!
  if (desc.Pool != D3DPOOL_DEFAULT || CanOnlySWVP())
    Flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);

  // Ignore DONOTWAIT if we are DYNAMIC
  // Yes... D3D9 is a good API.
  if (desc.Usage & D3DUSAGE_DYNAMIC)
    Flags &= ~D3DLOCK_DONOTWAIT;

  // Tests show that D3D9 drivers ignore DISCARD when the device is lost.
  if (unlikely(m_deviceLostState != D3D9DeviceLostState::Ok))
    Flags &= ~D3DLOCK_DISCARD;

  // In SWVP mode, we always use the per-draw upload path.
  // So the buffer will never be in use on the device.
  // FVF Buffers are the exception. Those can be used as a destination for
  // ProcessVertices.
  if (unlikely(CanOnlySWVP() && !pResource->NeedsReadback()))
    Flags |= D3DLOCK_NOOVERWRITE;

  // READONLY is ignored for non-managed pools
  if ((Flags & D3DLOCK_READONLY) && !IsPoolManaged(desc.Pool))
    Flags &= ~D3DLOCK_READONLY;

  // We only bounds check for MANAGED.
  // (TODO: Apparently this is meant to happen for DYNAMIC too but I am not sure
  //  how that works given it is meant to be a DIRECT access..?)
  const bool respectUserBounds =
      !(Flags & D3DLOCK_DISCARD) && SizeToLock != 0 &&
      (desc.Pool == D3DPOOL_MANAGED || (desc.Usage & D3DUSAGE_DYNAMIC));

  // If we don't respect the bounds, encompass it all in our tests/checks
  // These values may be out of range and don't get clamped.
  uint32_t offset = respectUserBounds ? OffsetToLock : 0;
  uint32_t size =
      respectUserBounds ? std::min(SizeToLock, desc.Size - offset) : desc.Size;
  D3D9Range lockRange = D3D9Range(offset, offset + size);

  bool updateDirtyRange =
      (desc.Pool == D3DPOOL_DEFAULT || !(Flags & D3DLOCK_NO_DIRTY_UPDATE)) &&
      !(Flags & D3DLOCK_READONLY);
  if (updateDirtyRange) {
    pResource->DirtyRange().Conjoin(lockRange);

    for (uint32_t i :
         bit::BitMask(static_cast<uint32_t>(m_vbSlotTracking.bound))) {
      auto commonBuffer =
          GetCommonBuffer(m_state.vertexBuffers[i].vertexBuffer);
      if (commonBuffer == pResource) {
        m_vbSlotTracking.needsUpload |= 1 << i;
      }
    }
  }

  const bool directMapping =
      pResource->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT;
  const bool needsReadback = pResource->NeedsReadback();

  uint8_t *data = nullptr;

  if ((Flags & D3DLOCK_DISCARD) && (directMapping || needsReadback)) {
    // If we're not directly mapped and don't need readback,
    // the buffer is not currently getting used anyway
    // so there's no reason to waste memory by discarding.

    m_discardMemoryCounter += desc.Size;
    ThrottleAllocation();

    // Allocate a new backing slice for the buffer and set
    // it as the 'new' mapped slice. This assumes that the
    // only way to invalidate a buffer is by mapping it.
    Rc<DxvkBuffer> mappingBuffer =
        pResource->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>();
    auto bufferSlice = pResource->DiscardMapSlice();
    data = reinterpret_cast<uint8_t *>(bufferSlice->mapPtr());

    EmitCs([cBuffer = std::move(mappingBuffer),
            cBufferSlice = std::move(bufferSlice)](DxvkContext *ctx) mutable {
      ctx->invalidateBuffer(cBuffer, std::move(cBufferSlice));
    });

    pResource->SetNeedsReadback(false);
  } else {
    // The application either didn't specify DISCARD or the buffer is guaranteed
    // to be idle anyway.

    // Use map pointer from previous map operation. This
    // way we don't have to synchronize with the CS thread
    // if the map mode is D3DLOCK_NOOVERWRITE.
    data = reinterpret_cast<uint8_t *>(pResource->GetMappedSlice()->mapPtr());

    const bool readOnly = Flags & D3DLOCK_READONLY;
    // NOOVERWRITE promises that they will not write in a currently used area.
    const bool noOverwrite = Flags & D3DLOCK_NOOVERWRITE;
    const bool directMapping =
        pResource->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT;

    // If we're not directly mapped, we can rely on needsReadback to tell us if
    // a sync is required.
    const bool skipWait =
        (!needsReadback && (readOnly || !directMapping)) || noOverwrite;

    if (!skipWait) {
      const Rc<DxvkBuffer> mappingBuffer =
          pResource->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>();
      if (!WaitForResource(*mappingBuffer,
                           pResource->GetMappingBufferSequenceNumber(), Flags))
        return D3DERR_WASSTILLDRAWING;

      pResource->SetNeedsReadback(false);
    }
  }

  // The offset/size is not clamped to or affected by the desc size.
  data += OffsetToLock;
  *ppbData = reinterpret_cast<void *>(data);

  DWORD oldFlags = pResource->GetMapFlags();

  // We need to remove the READONLY flags from the map flags
  // if there was ever a non-readonly upload.
  if (!(Flags & D3DLOCK_READONLY))
    oldFlags &= ~D3DLOCK_READONLY;

  pResource->SetMapFlags(Flags | oldFlags);
  pResource->IncrementLockCount();

  // We just mapped a buffer which may have come with an address space cost.
  // Unmap textures if the amount of mapped texture memory is exceeding the
  // threshold.
  UnmapTextures();

  return D3D_OK;
}

HRESULT D3D9DeviceEx::FlushBuffer(D3D9CommonBuffer *pResource) {
  // Wait until the amount of used staging memory is under a certain threshold
  // to avoid using too much memory and even more so to avoid using too much
  // address space.
  ThrottleAllocation();

  auto dstBuffer = pResource->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>();
  auto srcSlice = pResource->GetMappedSlice();

  D3D9Range &range = pResource->DirtyRange();

  D3D9BufferSlice slice = AllocStagingBuffer(range.max - range.min);
  void *srcData = reinterpret_cast<uint8_t *>(srcSlice->mapPtr()) + range.min;
  memcpy(slice.mapPtr, srcData, range.max - range.min);

  EmitCs([cDstSlice = dstBuffer, cSrcSlice = slice.slice,
          cDstOffset = range.min,
          cLength = range.max - range.min](DxvkContext *ctx) {
    ctx->copyBuffer(cDstSlice.buffer(), cDstSlice.offset() + cDstOffset,
                    cSrcSlice.buffer(), cSrcSlice.offset(), cLength);
  });

  pResource->DirtyRange().Clear();
  TrackBufferMappingBufferSequenceNumber(pResource);

  UnmapTextures();
  ConsiderFlush(GpuFlushType::ImplicitWeakHint);
  return D3D_OK;
}

HRESULT D3D9DeviceEx::UnlockBuffer(D3D9CommonBuffer *pResource) {
  D3D9DeviceLock lock = LockDevice();

  if (pResource->DecrementLockCount() != 0)
    return D3D_OK;

  // Nothing else to do for directly mapped buffers. Those were already written.
  if (pResource->GetMapMode() != D3D9_COMMON_BUFFER_MAP_MODE_BUFFER)
    return D3D_OK;

  // There is no part of the buffer that hasn't been uploaded yet.
  // This shouldn't happen.
  if (pResource->DirtyRange().IsDegenerate())
    return D3D_OK;

  pResource->SetMapFlags(0);

  // Only D3DPOOL_DEFAULT buffers get uploaded in UnlockBuffer.
  // D3DPOOL_SYSTEMMEM and D3DPOOL_MANAGED get uploaded at draw time.
  if (pResource->Desc()->Pool != D3DPOOL_DEFAULT)
    return D3D_OK;

  FlushBuffer(pResource);

  return D3D_OK;
}

void D3D9DeviceEx::UploadPerDrawData(UINT &FirstVertexIndex, UINT NumVertices,
                                     UINT &FirstIndex, UINT NumIndices,
                                     INT &BaseVertexIndex, bool *pDynamicVBOs,
                                     bool *pDynamicIBO) {
  // War3：清空本次 per-draw upload 固化信息，避免被后续 draw 复用
  m_war3PerDrawUpload = War3PerDrawUploadInfo{};

  const uint32_t usedBuffersMask =
      (m_state.vertexDecl != nullptr ? m_state.vertexDecl->GetStreamMask()
                                     : ~0u) &
      static_cast<uint32_t>(m_vbSlotTracking.bound);
  bool dynamicSysmemVBOs = usedBuffersMask == m_vbSlotTracking.uploadPerDraw;

  D3D9CommonBuffer *ibo = GetCommonBuffer(m_state.indices);
  bool dynamicSysmemIBO = NumIndices != 0 && ibo != nullptr &&
                          (ibo->DoPerDrawUpload() || CanOnlySWVP());

  *pDynamicVBOs = dynamicSysmemVBOs;

  if (unlikely(pDynamicIBO))
    *pDynamicIBO = dynamicSysmemIBO;

  if (likely(!dynamicSysmemVBOs && !dynamicSysmemIBO))
    return;

  uint32_t vertexBuffersToUpload;
  if (likely(dynamicSysmemVBOs))
    vertexBuffersToUpload = m_vbSlotTracking.uploadPerDraw & usedBuffersMask;
  else
    vertexBuffersToUpload = 0;

  // The UP buffer allocator will invalidate,
  // so we can only use 1 UP buffer slice per draw.
  // First we calculate the size of that UP buffer slice
  // and store all sizes and offsets into it.

  struct VBOCopy {
    uint32_t srcOffset;
    uint32_t dstOffset;
    uint32_t copyBufferLength;
    uint32_t copyElementCount;
    uint32_t copyElementSize;
    uint32_t copyElementStride;
  };
  uint32_t totalUpBufferSize = 0;
  std::array<VBOCopy, caps::MaxStreams> vboCopies = {};

  for (uint32_t i : bit::BitMask(vertexBuffersToUpload)) {
    auto *vbo = GetCommonBuffer(m_state.vertexBuffers[i].vertexBuffer);
    if (likely(vbo == nullptr)) {
      continue;
    }

    if (unlikely(vbo->NeedsReadback())) {
      // There's only one way the GPU might write new data to a vertex buffer:
      // - Write to the primary buffer using ProcessVertices which gets copied
      // over to the staging buffer at the end.
      //   So it could end up writing to the buffer on the GPU while the same
      //   buffer gets read here on the CPU. That is why we need to ensure the
      //   staging buffer is idle here.
      WaitForResource(*vbo->GetBuffer<D3D9_COMMON_BUFFER_TYPE_STAGING>(),
                      vbo->GetMappingBufferSequenceNumber(), D3DLOCK_READONLY);
    }

    const uint32_t vertexSize = m_state.vertexDecl->GetSize(i);
    const uint32_t vertexStride = m_state.vertexBuffers[i].stride;
    const uint32_t srcStride = vertexStride;
    const uint32_t dstStride = std::min(vertexStride, vertexSize);

    uint32_t elementCount = NumVertices;
    if (m_state.streamFreq[i] & D3DSTREAMSOURCE_INSTANCEDATA) {
      elementCount = GetInstanceCount();
    }
    const uint32_t vboOffset = m_state.vertexBuffers[i].offset;
    const uint32_t vertexOffset =
        (FirstVertexIndex + BaseVertexIndex) * srcStride;
    const uint32_t vertexBufferSize = vbo->Desc()->Size;
    const uint32_t srcOffset = vboOffset + vertexOffset;

    if (unlikely(srcOffset > vertexBufferSize)) {
      // All vertices are out of bounds
      vboCopies[i].copyBufferLength = 0;
    } else if (unlikely(srcOffset + elementCount * srcStride >
                        vertexBufferSize)) {
      // Some vertices are (partially) out of bounds
      uint32_t boundVertexBufferRange = vertexBufferSize - vboOffset;
      elementCount = boundVertexBufferRange / srcStride;
      // Copy all complete vertices
      vboCopies[i].copyBufferLength = elementCount * dstStride;
      // Copy the remaining partial vertex
      vboCopies[i].copyBufferLength +=
          std::min(dstStride, boundVertexBufferRange % srcStride);
    } else {
      // No vertices are out of bounds
      vboCopies[i].copyBufferLength = elementCount * dstStride;
    }

    vboCopies[i].copyElementCount = elementCount;
    vboCopies[i].copyElementStride = srcStride;
    vboCopies[i].copyElementSize = dstStride;
    vboCopies[i].srcOffset = srcOffset;
    vboCopies[i].dstOffset = totalUpBufferSize;
    totalUpBufferSize += vboCopies[i].copyBufferLength;
  }

  uint32_t iboUPBufferSize = 0;
  uint32_t iboUPBufferOffset = 0;
  if (dynamicSysmemIBO) {
    auto *ibo = GetCommonBuffer(m_state.indices);
    if (likely(ibo != nullptr)) {
      uint32_t indexStride = ibo->Desc()->Format == D3D9Format::INDEX16 ? 2 : 4;
      uint32_t offset = indexStride * FirstIndex;
      uint32_t indexBufferSize = ibo->Desc()->Size;
      if (offset < indexBufferSize) {
        iboUPBufferSize =
            std::min(NumIndices * indexStride, indexBufferSize - offset);
        iboUPBufferOffset = totalUpBufferSize;
        totalUpBufferSize += iboUPBufferSize;
      }
    }
  }

  if (unlikely(totalUpBufferSize == 0)) {
    *pDynamicVBOs = false;
    if (pDynamicIBO)
      *pDynamicIBO = false;

    return;
  }

  auto upSlice = AllocUPBuffer(totalUpBufferSize);

  // War3：固化 UP buffer 的 backing allocation（UP buffer 在 wrap 时会
  // invalidate/换 VkBuffer）。 本次 draw 的数据已经写入
  // upSlice.mapPtr，因此必须保存对应 allocation，否则 shadow caster 重放
  // 可能读到后续 invalidation 后的新 VkBuffer
  // 内容（表现为阴影残缺/错位/乱飞）。
  if (m_war3Pipeline && upSlice.slice.buffer() != nullptr) {
    Rc<DxvkResourceAllocation> upAlloc = nullptr;
    if (m_upBuffer != nullptr && upSlice.slice.buffer() == m_upBuffer &&
        m_upBufferAllocation != nullptr)
      upAlloc = m_upBufferAllocation;
    else
      upAlloc = upSlice.slice.buffer()->storage();

    m_war3PerDrawUpload.storage = upAlloc;
  }

  // Now copy the actual data and bind it.
  if (dynamicSysmemVBOs) {
    for (uint32_t i : bit::BitMask(vertexBuffersToUpload)) {
      const VBOCopy &copy = vboCopies[i];

      if (likely(copy.copyBufferLength != 0)) {
        const auto *vbo =
            GetCommonBuffer(m_state.vertexBuffers[i].vertexBuffer);
        uint8_t *data =
            reinterpret_cast<uint8_t *>(upSlice.mapPtr) + copy.dstOffset;
        const uint8_t *src =
            reinterpret_cast<uint8_t *>(vbo->GetMappedSlice()->mapPtr()) +
            copy.srcOffset;

        if (likely(copy.copyElementStride == copy.copyElementSize)) {
          std::memcpy(data, src, copy.copyBufferLength);
        } else {
          for (uint32_t j = 0; j < copy.copyElementCount; j++) {
            std::memcpy(data + j * copy.copyElementSize,
                        src + j * copy.copyElementStride, copy.copyElementSize);
          }
          if (unlikely(copy.copyBufferLength >
                       copy.copyElementCount * copy.copyElementSize)) {
            // Partial vertex at the end
            std::memcpy(data + copy.copyElementCount * copy.copyElementSize,
                        src + copy.copyElementCount * copy.copyElementStride,
                        copy.copyBufferLength -
                            copy.copyElementCount * copy.copyElementSize);
          }
        }
      }

      auto vboSlice =
          upSlice.slice.subSlice(copy.dstOffset, copy.copyBufferLength);

      // War3：记录本次 draw 实际绑定的 VB 子切片与 stride（用于 shadow caster
      // 捕获）
      if (m_war3Pipeline && m_war3PerDrawUpload.storage != nullptr) {
        m_war3PerDrawUpload.vbSlices[i] = vboSlice;
        m_war3PerDrawUpload.vbStrides[i] = copy.copyElementSize;
        m_war3PerDrawUpload.vbValid[i] = true;
      }

      EmitCs([cStream = i, cBufferSlice = std::move(vboSlice),
              cStride = copy.copyElementSize](DxvkContext *ctx) mutable {
        ctx->bindVertexBuffer(cStream, std::move(cBufferSlice), cStride);
      });
      m_dirty.set(D3D9DeviceDirtyFlag::VertexBuffers);
    }

    // Change the draw call parameters to reflect the changed vertex buffers
    if (NumIndices != 0) {
      BaseVertexIndex = -FirstVertexIndex;
    } else {
      FirstVertexIndex = 0;
    }
  }

  if (dynamicSysmemIBO) {
    if (unlikely(iboUPBufferSize == 0)) {
      EmitCs([](DxvkContext *ctx) {
        ctx->bindIndexBuffer(DxvkBufferSlice(), VK_INDEX_TYPE_UINT32);
      });
      m_dirty.set(D3D9DeviceDirtyFlag::IndexBuffer);
    } else {
      auto *ibo = GetCommonBuffer(m_state.indices);
      uint32_t indexStride = ibo->Desc()->Format == D3D9Format::INDEX16 ? 2 : 4;
      VkIndexType indexType = DecodeIndexType(ibo->Desc()->Format);
      uint32_t offset = indexStride * FirstIndex;
      uint8_t *data =
          reinterpret_cast<uint8_t *>(upSlice.mapPtr) + iboUPBufferOffset;
      uint8_t *src =
          reinterpret_cast<uint8_t *>(ibo->GetMappedSlice()->mapPtr()) + offset;
      std::memcpy(data, src, iboUPBufferSize);

      auto iboSlice =
          upSlice.slice.subSlice(iboUPBufferOffset, iboUPBufferSize);

      // War3：记录本次 draw 实际绑定的 IB 子切片（用于 shadow caster 捕获）
      if (m_war3Pipeline && m_war3PerDrawUpload.storage != nullptr) {
        m_war3PerDrawUpload.ibSlice = iboSlice;
        m_war3PerDrawUpload.ibType = indexType;
        m_war3PerDrawUpload.ibValid = true;
        m_war3PerDrawUpload.ibStorage = m_war3PerDrawUpload.storage;
      }

      EmitCs([cBufferSlice = std::move(iboSlice),
              cIndexType = indexType](DxvkContext *ctx) mutable {
        ctx->bindIndexBuffer(std::move(cBufferSlice), cIndexType);
      });
      m_dirty.set(D3D9DeviceDirtyFlag::IndexBuffer);
    }

    // Change the draw call parameters to reflect the changed index buffer
    FirstIndex = 0;
  }
}

void D3D9DeviceEx::InjectCsChunk(DxvkCsChunkRef &&Chunk, bool Synchronize) {
  m_csThread.injectChunk(DxvkCsQueue::HighPriority, std::move(Chunk),
                         Synchronize);
}

void D3D9DeviceEx::EmitCsChunk(DxvkCsChunkRef &&chunk) {
  // Flush init commands so that the CS thread
  // can processe them before the first use.
  m_initializer->FlushCsChunk();

  m_csSeqNum = m_csThread.dispatchChunk(std::move(chunk));
}

void D3D9DeviceEx::ConsiderFlush(GpuFlushType FlushType) {
  uint64_t chunkId = GetCurrentSequenceNumber();
  uint64_t submissionId = m_submissionFence->value();

  if (m_flushTracker.considerFlush(FlushType, chunkId, submissionId, 0u))
    Flush();
}

void D3D9DeviceEx::SynchronizeCsThread(uint64_t SequenceNumber) {
  D3D9DeviceLock lock = LockDevice();

  // Dispatch current chunk so that all commands
  // recorded prior to this function will be run
  if (SequenceNumber > m_csSeqNum)
    FlushCsChunk();

  m_csThread.synchronize(SequenceNumber);
}

void D3D9DeviceEx::SetupFPU() {
  // Should match d3d9 float behaviour.

#if defined(_MSC_VER)
  // For MSVC we can use these cross arch and platform funcs to set the FPU.
  // This will work on any platform, x86, x64, ARM, etc.

  // Clear exceptions.
  _clearfp();

  // Disable exceptions
  _controlfp(_MCW_EM, _MCW_EM);

#ifndef _WIN64
  // Use 24 bit precision
  _controlfp(_PC_24, _MCW_PC);
#endif

  // Round to nearest
  _controlfp(_RC_NEAR, _MCW_RC);
#elif (defined(__GNUC__) || defined(__MINGW32__)) &&                           \
    (defined(__i386__) || (defined(__x86_64__) && !defined(__arm64ec__)) ||    \
     defined(__ia64))
  // For GCC/MinGW we can use inline asm to set it.
  // This only works for x86 and x64 processors however.

  uint16_t control;

  // Get current control word.
  __asm__ __volatile__("fnstcw %0" : "=m"(*&control));

  // Clear existing settings.
  control &= 0xF0C0;

  // Disable exceptions
  // Use 24 bit precision
  // Round to nearest
  control |= 0x003F;

  // Set new control word.
  __asm__ __volatile__("fldcw %0" : : "m"(*&control));
#else
  Logger::warn("D3D9DeviceEx::SetupFPU: not supported on this arch.");
#endif
}

int64_t D3D9DeviceEx::DetermineInitialTextureMemory() {
  auto memoryProp = m_adapter->GetDXVKAdapter()->memoryProperties();

  VkDeviceSize availableTextureMemory = 0;

  for (uint32_t i = 0; i < memoryProp.memoryHeapCount; i++)
    availableTextureMemory += memoryProp.memoryHeaps[i].size;

  constexpr VkDeviceSize Megabytes = 1024 * 1024;
  // Windows will typically "reserve" some amount of video memory,
  // presumably for back buffers, which gets subtracted from the
  // reported size, e.g. in case of 4 GB it will report a total of
  // 4286578687 available bytes. The reserved amount varies depending
  // on the number of back buffers and the back buffer resolution,
  // however 8 MB has been generally observed for 1080p.
  constexpr VkDeviceSize ReservedMemory = 8 * Megabytes;

  // The value returned is a 32-bit value, so we need to clamp it.
  VkDeviceSize maxMemory =
      (VkDeviceSize(m_d3d9Options.maxAvailableMemory) * Megabytes) - 1;
  availableTextureMemory =
      std::min(availableTextureMemory, maxMemory) - ReservedMemory;

  return int64_t(availableTextureMemory);
}

void D3D9DeviceEx::CreateConstantBuffers() {
  constexpr VkDeviceSize DefaultConstantBufferSize = 1024ull << 10;
  constexpr VkDeviceSize SmallConstantBufferSize = 64ull << 10;

  m_consts[DxsoProgramTypes::VertexShader].buffer = D3D9ConstantBuffer(
      this, DxsoProgramType::VertexShader,
      DxsoConstantBuffers::VSConstantBuffer, DefaultConstantBufferSize);

  m_consts[DxsoProgramTypes::VertexShader].swvp.intBuffer = D3D9ConstantBuffer(
      this, DxsoProgramType::VertexShader,
      DxsoConstantBuffers::VSIntConstantBuffer, SmallConstantBufferSize);

  m_consts[DxsoProgramTypes::VertexShader].swvp.boolBuffer = D3D9ConstantBuffer(
      this, DxsoProgramType::VertexShader,
      DxsoConstantBuffers::VSBoolConstantBuffer, SmallConstantBufferSize);

  m_consts[DxsoProgramTypes::PixelShader].buffer = D3D9ConstantBuffer(
      this, DxsoProgramType::PixelShader, DxsoConstantBuffers::PSConstantBuffer,
      DefaultConstantBufferSize);

  m_vsClipPlanes = D3D9ConstantBuffer(
      this, DxsoProgramType::VertexShader, DxsoConstantBuffers::VSClipPlanes,
      caps::MaxClipPlanes * sizeof(D3D9ClipPlane));

  m_vsFixedFunction = D3D9ConstantBuffer(this, DxsoProgramType::VertexShader,
                                         DxsoConstantBuffers::VSFixedFunction,
                                         sizeof(D3D9FixedFunctionVS));

  m_psFixedFunction = D3D9ConstantBuffer(this, DxsoProgramType::PixelShader,
                                         DxsoConstantBuffers::PSFixedFunction,
                                         sizeof(D3D9FixedFunctionPS));

  m_psShared =
      D3D9ConstantBuffer(this, DxsoProgramType::PixelShader,
                         DxsoConstantBuffers::PSShared, sizeof(D3D9SharedPS));

  m_vsVertexBlend = D3D9ConstantBuffer(
      this, DxsoProgramType::VertexShader,
      DxsoConstantBuffers::VSVertexBlendData,
      CanSWVP() ? sizeof(D3D9FixedFunctionVertexBlendDataSW)
                : sizeof(D3D9FixedFunctionVertexBlendDataHW));

  // Allocate constant buffer for values that would otherwise get passed as spec
  // constants for fast-linked pipelines to use.
  if (m_usingGraphicsPipelines) {
    m_specBuffer = D3D9ConstantBuffer(
        this, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        getSpecConstantBufferSlot(), D3D9SpecializationInfo::UBOSize);
  }
}

inline void D3D9DeviceEx::UploadSoftwareConstantSet(
    const D3D9ShaderConstantsVSSoftware &Src,
    const D3D9ConstantLayout &Layout) {
  /*
   * SWVP raises the amount of constants by a lot.
   * To avoid copying huge amounts of data for every draw call,
   * we track the highest set constant and only use a buffer big enough
   * to fit that. We rely on robustness to return 0 for OOB reads.
   */

  D3D9ConstantSets &constSet = m_consts[DxsoProgramType::VertexShader];

  if (!constSet.dirty)
    return;

  constSet.dirty = false;

  uint32_t floatCount = constSet.maxChangedConstF;
  if (constSet.meta.needsConstantCopies) {
    // If the shader requires us to preserve shader defined constants,
    // we copy those over. We need to adjust the amount of used floats
    // accordingly.
    auto shader = GetCommonShader(m_state.vertexShader);
    floatCount = std::max(
        floatCount,
        static_cast<uint32_t>(shader->GetMaxDefinedFloatConstant() + 1));
  }
  // If we statically know which is the last float constant accessed by the
  // shader, we don't need to copy the rest.
  floatCount = std::min(floatCount, constSet.meta.maxConstIndexF);

  // Calculate data sizes for each constant type.
  const uint32_t floatDataSize = floatCount * sizeof(Vector4);
  const uint32_t intDataSize =
      std::min(constSet.meta.maxConstIndexI, constSet.maxChangedConstI) *
      sizeof(Vector4i);
  const uint32_t boolDataSize =
      divCeil(std::min(constSet.meta.maxConstIndexB, constSet.maxChangedConstB),
              32u) *
      uint32_t(sizeof(uint32_t));

  // Max copy source size is 8192 * 16 => always aligned to any plausible value
  // => we won't copy out of bounds
  if (likely(constSet.meta.maxConstIndexF != 0)) {
    auto mapPtr =
        CopySoftwareConstants(constSet.buffer, Src.fConsts, floatDataSize);

    if (constSet.meta.needsConstantCopies) {
      // Copy shader defined constants over so they can be accessed
      // with relative addressing.
      Vector4 *data = reinterpret_cast<Vector4 *>(mapPtr);

      auto &shaderConsts =
          GetCommonShader(m_state.vertexShader)->GetConstants();

      for (const auto &constant : shaderConsts) {
        if (constant.uboIdx < constSet.meta.maxConstIndexF)
          data[constant.uboIdx] =
              *reinterpret_cast<const Vector4 *>(constant.float32);
      }
    }
  }

  // Max copy source size is 2048 * 16 => always aligned to any plausible value
  // => we won't copy out of bounds
  if (likely(constSet.meta.maxConstIndexI != 0))
    CopySoftwareConstants(constSet.swvp.intBuffer, Src.iConsts, intDataSize);

  if (likely(constSet.meta.maxConstIndexB != 0))
    CopySoftwareConstants(constSet.swvp.boolBuffer, Src.bConsts, boolDataSize);
}

inline void *D3D9DeviceEx::CopySoftwareConstants(D3D9ConstantBuffer &dstBuffer,
                                                 const void *src,
                                                 uint32_t size) {
  uint32_t alignment = dstBuffer.GetAlignment();
  size = std::max(size, alignment);
  size = align(size, alignment);

  auto mapPtr = dstBuffer.Alloc(size);
  std::memcpy(mapPtr, src, size);
  return mapPtr;
}

template <DxsoProgramType ShaderStage, typename HardwareLayoutType,
          typename SoftwareLayoutType, typename ShaderType>
inline void D3D9DeviceEx::UploadConstantSet(const SoftwareLayoutType &Src,
                                            const D3D9ConstantLayout &Layout,
                                            const ShaderType &Shader) {
  /*
   * We just copy the float constants that have been set by the application and
   * rely on robustness to return 0 on OOB reads.
   */
  D3D9ConstantSets &constSet = m_consts[ShaderStage];

  if (!constSet.dirty)
    return;

  constSet.dirty = false;

  uint32_t floatCount = constSet.maxChangedConstF;
  if (constSet.meta.needsConstantCopies) {
    // If the shader requires us to preserve shader defined constants,
    // we copy those over. We need to adjust the amount of used floats
    // accordingly.
    auto shader = GetCommonShader(Shader);
    floatCount = std::max(
        floatCount,
        static_cast<uint32_t>(shader->GetMaxDefinedFloatConstant() + 1));
  }
  // If we statically know which is the last float constant accessed by the
  // shader, we don't need to copy the rest.
  floatCount = std::min(constSet.meta.maxConstIndexF, floatCount);

  // There are very few int constants, so we put those into the same buffer at
  // the start. We always allocate memory for all possible int constants to make
  // sure alignment works out.
  const uint32_t intRange = caps::MaxOtherConstants * sizeof(Vector4i);
  uint32_t floatDataSize = floatCount * sizeof(Vector4);
  // Determine amount of floats and buffer size based on highest used float
  // constant and alignment
  const uint32_t alignment = constSet.buffer.GetAlignment();
  const uint32_t bufferSize =
      align(std::max(floatDataSize + intRange, alignment), alignment);
  floatDataSize = bufferSize - intRange;

  void *mapPtr = constSet.buffer.Alloc(bufferSize);
  auto *dst = reinterpret_cast<HardwareLayoutType *>(mapPtr);

  const uint32_t intDataSize = constSet.meta.maxConstIndexI * sizeof(Vector4i);
  if (constSet.meta.maxConstIndexI != 0)
    std::memcpy(dst->iConsts, Src.iConsts, intDataSize);
  if (constSet.meta.maxConstIndexF != 0)
    std::memcpy(dst->fConsts, Src.fConsts, floatDataSize);

  if (constSet.meta.needsConstantCopies) {
    // Copy shader defined constants over so they can be accessed
    // with relative addressing.
    Vector4 *data = reinterpret_cast<Vector4 *>(dst->fConsts);

    auto &shaderConsts = GetCommonShader(Shader)->GetConstants();

    for (const auto &constant : shaderConsts) {
      if (constant.uboIdx < constSet.meta.maxConstIndexF)
        data[constant.uboIdx] =
            *reinterpret_cast<const Vector4 *>(constant.float32);
    }
  }
}

template <DxsoProgramType ShaderStage> void D3D9DeviceEx::UploadConstants() {
  if constexpr (ShaderStage == DxsoProgramTypes::VertexShader) {
    if (CanSWVP())
      return UploadSoftwareConstantSet(m_state.vsConsts.get(),
                                       m_consts[ShaderStage].layout);
    else
      return UploadConstantSet<ShaderStage, D3D9ShaderConstantsVSHardware>(
          m_state.vsConsts.get(), m_consts[ShaderStage].layout,
          m_state.vertexShader);
  } else {
    return UploadConstantSet<ShaderStage, D3D9ShaderConstantsPS>(
        m_state.psConsts.get(), m_consts[ShaderStage].layout,
        m_state.pixelShader);
  }
}

void D3D9DeviceEx::UpdateClipPlanes() {
  m_dirty.clr(D3D9DeviceDirtyFlag::ClipPlanes);

  auto mapPtr = m_vsClipPlanes.AllocSlice();
  auto dst = reinterpret_cast<D3D9ClipPlane *>(mapPtr);

  uint32_t clipPlaneCount = 0u;
  for (uint32_t i = 0; i < caps::MaxClipPlanes; i++) {
    D3D9ClipPlane clipPlane =
        (m_state.renderStates[D3DRS_CLIPPLANEENABLE] & (1 << i))
            ? m_state.clipPlanes[i]
            : D3D9ClipPlane();

    if (clipPlane != D3D9ClipPlane())
      dst[clipPlaneCount++] = clipPlane;
  }

  // Write the rest to 0 for GPL.
  for (uint32_t i = clipPlaneCount; i < caps::MaxClipPlanes; i++)
    dst[i] = D3D9ClipPlane();

  if (m_specInfo.set<SpecClipPlaneCount>(clipPlaneCount))
    m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
}

template <uint32_t Offset, uint32_t Length>
void D3D9DeviceEx::UpdatePushConstant(const void *pData) {
  struct ConstantData {
    uint8_t Data[Length];
  };

  const ConstantData *constData = reinterpret_cast<const ConstantData *>(pData);

  EmitCs([cData = *constData](DxvkContext *ctx) {
    // Render state uses the shared push constant block
    ctx->pushData(VK_SHADER_STAGE_ALL_GRAPHICS, Offset, Length, &cData);
  });
}

template <D3D9RenderStateItem Item> void D3D9DeviceEx::UpdatePushConstant() {
  auto &rs = m_state.renderStates;

  if constexpr (Item == D3D9RenderStateItem::AlphaRef) {
    uint32_t alpha = rs[D3DRS_ALPHAREF] & 0xFF;
    UpdatePushConstant<offsetof(D3D9RenderStateInfo, alphaRef),
                       sizeof(uint32_t)>(&alpha);
  } else if constexpr (Item == D3D9RenderStateItem::FogColor) {
    Vector4 color;
    DecodeD3DCOLOR(D3DCOLOR(rs[D3DRS_FOGCOLOR]), color.data);
    UpdatePushConstant<offsetof(D3D9RenderStateInfo, fogColor),
                       sizeof(D3D9RenderStateInfo::fogColor)>(&color);
  } else if constexpr (Item == D3D9RenderStateItem::FogDensity) {
    float density = bit::cast<float>(rs[D3DRS_FOGDENSITY]);
    UpdatePushConstant<offsetof(D3D9RenderStateInfo, fogDensity),
                       sizeof(float)>(&density);
  } else if constexpr (Item == D3D9RenderStateItem::FogEnd) {
    float end = bit::cast<float>(rs[D3DRS_FOGEND]);
    UpdatePushConstant<offsetof(D3D9RenderStateInfo, fogEnd), sizeof(float)>(
        &end);
  } else if constexpr (Item == D3D9RenderStateItem::FogScale) {
    float end = bit::cast<float>(rs[D3DRS_FOGEND]);
    float start = bit::cast<float>(rs[D3DRS_FOGSTART]);

    float scale = 1.0f / (end - start);
    UpdatePushConstant<offsetof(D3D9RenderStateInfo, fogScale), sizeof(float)>(
        &scale);
  } else if constexpr (Item == D3D9RenderStateItem::PointSize) {
    UpdatePushConstant<offsetof(D3D9RenderStateInfo, pointSize), sizeof(float)>(
        &rs[D3DRS_POINTSIZE]);
  } else if constexpr (Item == D3D9RenderStateItem::PointSizeMin) {
    UpdatePushConstant<offsetof(D3D9RenderStateInfo, pointSizeMin),
                       sizeof(float)>(&rs[D3DRS_POINTSIZE_MIN]);
  } else if constexpr (Item == D3D9RenderStateItem::PointSizeMax) {
    UpdatePushConstant<offsetof(D3D9RenderStateInfo, pointSizeMax),
                       sizeof(float)>(&rs[D3DRS_POINTSIZE_MAX]);
  } else if constexpr (Item == D3D9RenderStateItem::PointScaleA) {
    float scale = bit::cast<float>(rs[D3DRS_POINTSCALE_A]);
    scale /= float(m_state.viewport.Height * m_state.viewport.Height);

    UpdatePushConstant<offsetof(D3D9RenderStateInfo, pointScaleA),
                       sizeof(float)>(&scale);
  } else if constexpr (Item == D3D9RenderStateItem::PointScaleB) {
    float scale = bit::cast<float>(rs[D3DRS_POINTSCALE_B]);
    scale /= float(m_state.viewport.Height * m_state.viewport.Height);

    UpdatePushConstant<offsetof(D3D9RenderStateInfo, pointScaleB),
                       sizeof(float)>(&scale);
  } else if constexpr (Item == D3D9RenderStateItem::PointScaleC) {
    float scale = bit::cast<float>(rs[D3DRS_POINTSCALE_C]);
    scale /= float(m_state.viewport.Height * m_state.viewport.Height);

    UpdatePushConstant<offsetof(D3D9RenderStateInfo, pointScaleC),
                       sizeof(float)>(&scale);
  } else
    Logger::warn("D3D9: Invalid push constant set to update.");
}

template <bool Synchronize9On12> void D3D9DeviceEx::ExecuteFlush() {
  D3D9DeviceLock lock = LockDevice();

  if constexpr (Synchronize9On12)
    m_submitStatus.result = VK_NOT_READY;

  // Update signaled staging buffer counter and signal the fence
  m_stagingMemorySignaled = GetStagingMemoryStatistics().allocatedTotal;

  // Reset counter for discarded memory in flight
  m_discardMemoryOnFlush = m_discardMemoryCounter;

  // Add commands to flush the threaded
  // context, then flush the command list
  uint64_t submissionId = ++m_submissionId;

  EmitCs<false>(
      [cSubmissionFence = m_submissionFence, cSubmissionId = submissionId,
       cSubmissionStatus = Synchronize9On12 ? &m_submitStatus : nullptr,
       cStagingBufferFence = m_stagingBufferFence,
       cStagingBufferAllocated = m_stagingMemorySignaled](DxvkContext *ctx) {
        ctx->signal(cSubmissionFence, cSubmissionId);
        ctx->signal(cStagingBufferFence, cStagingBufferAllocated);
        ctx->flushCommandList(nullptr, cSubmissionStatus);
      });

  FlushCsChunk();

  m_flushSeqNum = m_csSeqNum;
  m_flushTracker.notifyFlush(m_flushSeqNum, submissionId);

  // If necessary, block calling thread until the
  // Vulkan queue submission is performed.
  if constexpr (Synchronize9On12)
    m_dxvkDevice->waitForSubmission(&m_submitStatus);

  // Notify the device that the context has been flushed,
  // this resets some resource initialization heuristics.
  m_initializer->NotifyContextFlush();
}

void D3D9DeviceEx::Flush() { ExecuteFlush<false>(); }

void D3D9DeviceEx::FlushAndSync9On12() { ExecuteFlush<true>(); }

void D3D9DeviceEx::BeginFrame(Rc<DxvkLatencyTracker> LatencyTracker,
                              uint64_t FrameId) {
  D3D9DeviceLock lock = LockDevice();

  EmitCs<false>([cTracker = std::move(LatencyTracker),
                 cFrameId = FrameId](DxvkContext *ctx) {
    if (cTracker && cTracker->needsAutoMarkers())
      ctx->beginLatencyTracking(cTracker, cFrameId);
  });
}

void D3D9DeviceEx::EndFrame(Rc<DxvkLatencyTracker> LatencyTracker) {
  D3D9DeviceLock lock = LockDevice();

  EmitCs<false>([cTracker = std::move(LatencyTracker)](DxvkContext *ctx) {
    ctx->endFrame();

    if (cTracker && cTracker->needsAutoMarkers())
      ctx->endLatencyTracking(cTracker);
  });
}

inline void D3D9DeviceEx::UpdateActiveRTs(uint32_t index) {
  const uint32_t bit = 1 << index;

  m_rtSlotTracking.canBeSampled &= ~bit;

  if (HasRenderTargetBound(index) &&
      m_state.renderTargets[index]->GetBaseTexture() != nullptr)
    m_rtSlotTracking.canBeSampled |= bit;

  UpdateActiveHazardsRT(std::numeric_limits<uint32_t>::max());
}

template <uint32_t Index> inline void D3D9DeviceEx::UpdateAnyColorWrites() {
  // Writes to a render target have been enabled => check for hazards
  UpdateActiveHazardsRT(std::numeric_limits<uint32_t>::max());

  // Writes to render target 0 have been enabled and the RT might not be bound
  // due to the 1x1 hack.
  if (Index == 0 && m_state.depthStencil != nullptr)
    m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
}

inline void D3D9DeviceEx::UpdateTextureBitmasks(uint32_t index,
                                                DWORD combinedUsage) {
  const uint32_t bit = 1 << index;

  m_textureSlotTracking.rtUsage &= ~bit;
  m_textureSlotTracking.dsUsage &= ~bit;
  m_textureSlotTracking.bound &= ~bit;
  m_textureSlotTracking.needsUpload &= ~bit;
  m_textureSlotTracking.needsMipGen &= ~bit;
  m_textureSlotTracking.mismatchingTextureType &= ~bit;

  auto tex = GetCommonTexture(m_state.textures[index]);

  if (likely(IsPSSampler(index))) {
    const uint32_t textureType =
        tex != nullptr ? uint32_t(tex->GetType() - D3DRTYPE_TEXTURE) : 0;
    // There are 3 texture types, so we need 2 bits.
    const uint32_t offset = index * 2;
    const uint32_t textureBitMask = 0b11u << offset;
    const uint32_t textureBits = textureType << offset;

    // In fixed function shaders and SM < 2 we put the type mask
    // into a spec constant to select the used sampler type.
    m_textureSlotTracking.textureType &= ~textureBitMask;
    m_textureSlotTracking.textureType |= textureBits;
  }

  if (likely(tex != nullptr)) {
    m_textureSlotTracking.bound |= bit;

    if (unlikely(tex->IsRenderTarget()))
      m_textureSlotTracking.rtUsage |= bit;

    if (unlikely(tex->IsDepthStencil()))
      m_textureSlotTracking.dsUsage |= bit;

    if (unlikely(tex->NeedsAnyUpload()))
      m_textureSlotTracking.needsUpload |= bit;

    if (unlikely(tex->NeedsMipGen()))
      m_textureSlotTracking.needsMipGen |= bit;

    // Update shadow sampler mask
    const bool oldDepth = m_textureSlotTracking.depth & bit;
    const bool newDepth = tex->IsShadow();

    if (oldDepth != newDepth) {
      m_textureSlotTracking.depth ^= bit;
      m_textureSlotTracking.samplerStateDirty |= bit;
    }

    // Update dref clamp mask
    m_textureSlotTracking.drefClamp &= ~bit;
    m_textureSlotTracking.drefClamp |= uint32_t(tex->IsUpgradedToD32f())
                                       << index;

    if (unlikely(m_textureSlotTracking.fetch4SamplerState & bit))
      UpdateActiveFetch4(index);

    UpdateTextureTypeMismatchesForTexture(index);
  } else {
    if (unlikely(m_textureSlotTracking.fetch4 & bit))
      UpdateActiveFetch4(index);
  }

  if (unlikely(combinedUsage & D3DUSAGE_RENDERTARGET)) {
    UpdateActiveHazardsRT(bit);
  } else {
    m_textureSlotTracking.hazardRT &= ~bit;
    m_textureSlotTracking.unresolvableHazardRT &= ~bit;
  }

  if (unlikely(combinedUsage & D3DUSAGE_DEPTHSTENCIL)) {
    UpdateActiveHazardsDS(bit);
  } else {
    m_textureSlotTracking.hazardDS &= ~bit;
    m_textureSlotTracking.unresolvableHazardDS &= ~bit;
  }
}

inline void D3D9DeviceEx::UpdateActiveHazardsRT(uint32_t texMask) {
  uint32_t oldHazardMask = m_textureSlotTracking.hazardRT;
  uint32_t oldUnresolvableHazardMask =
      m_textureSlotTracking.unresolvableHazardRT;
  m_textureSlotTracking.hazardRT &= ~texMask;
  m_textureSlotTracking.unresolvableHazardRT &= ~texMask;

  auto psMasks = PSShaderMasks();
  uint32_t rtMask = m_rtSlotTracking.canBeSampled;
  texMask &= m_textureSlotTracking.rtUsage;

  for (uint32_t rtIdx : bit::BitMask(rtMask)) {
    bool anyColorWrite = m_state.renderStates[ColorWriteIndex(rtIdx)] != 0;
    bool shaderWritesToRt = (psMasks.rtMask & (1 << rtIdx)) != 0;
    for (uint32_t samplerIdx : bit::BitMask(texMask)) {
      D3D9Surface *rtSurf = m_state.renderTargets[rtIdx].ptr();

      IDirect3DBaseTexture9 *rtBase = rtSurf->GetBaseTexture();
      IDirect3DBaseTexture9 *texBase = m_state.textures[samplerIdx];

      // HACK: Don't mark for hazards if we aren't rendering to mip 0!
      // Some games use screenspace passes like this for blurring
      // Sampling from mip 0 (texture) -> mip 1 (rt)
      // and we'd trigger the hazard path otherwise which is unnecessary,
      // and would shove us into GENERAL and emitting readback barriers.
      if (likely(rtSurf->GetMipLevel() != 0 || rtBase != texBase))
        continue;

      const bool sampledInShader = !!(psMasks.samplerMask & (1 << samplerIdx));
      const bool wasHazard = !!(oldHazardMask & (1 << samplerIdx));

      // If the shader doesn't actually use the texture, keep it marked as a
      // hazard to avoid spilling the render pass over and over again because of
      // shader changes.
      if (unlikely(!sampledInShader &&
                   ((anyColorWrite && shaderWritesToRt) || !wasHazard)))
        continue;

      // We can resolve the hazard by unbinding the RT.
      m_textureSlotTracking.hazardRT |= 1 << samplerIdx;

      // Don't mark texture as an unresolvable hazard if the shader doesn't
      // actually use it.
      if (unlikely(!sampledInShader))
        continue;

      // The hazard can be resolved by not binding it.
      if (likely(!anyColorWrite || !shaderWritesToRt))
        continue;

      // We have to bind the RT, so we need FEEDBACK_LOOP_LAYOUT.
      m_textureSlotTracking.unresolvableHazardRT |= 1 << samplerIdx;
    }
  }

  // Only dirty the framebuffer if we need to make changes for a new hazard
  if (unlikely(m_textureSlotTracking.hazardRT != oldHazardMask ||
               m_textureSlotTracking.unresolvableHazardRT !=
                   oldUnresolvableHazardMask)) {
    m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  }
}

inline void D3D9DeviceEx::UpdateActiveHazardsDS(uint32_t texMask) {
  uint32_t oldHazardMask = m_textureSlotTracking.hazardDS;
  uint32_t oldUnresolvableHazardMask =
      m_textureSlotTracking.unresolvableHazardDS;
  m_textureSlotTracking.hazardDS &= ~texMask;
  m_textureSlotTracking.unresolvableHazardDS &= ~texMask;

  if (m_state.depthStencil == nullptr ||
      m_state.depthStencil->GetBaseTexture() == nullptr)
    return;

  auto psMasks = PSShaderMasks();
  texMask &= m_textureSlotTracking.dsUsage;

  const bool depthWrite = m_state.renderStates[D3DRS_ZENABLE] &&
                          m_state.renderStates[D3DRS_ZWRITEENABLE];

  for (uint32_t samplerIdx : bit::BitMask(texMask)) {
    IDirect3DBaseTexture9 *dsBase = m_state.depthStencil->GetBaseTexture();
    IDirect3DBaseTexture9 *texBase = m_state.textures[samplerIdx];

    if (likely(dsBase != texBase))
      continue;

    const bool sampledInShader = !!(psMasks.samplerMask & (1 << samplerIdx));
    const bool wasHazard = !!(oldHazardMask & (1 << samplerIdx));

    // Don't mark it as a hazard if the current shader doesn't actually sample
    // it but we need to render to it. If the shader doesn't actually sample the
    // texture, we don't render to it and it was a hazard before, keep it marked
    // as a hazard to avoid spilling the render pass over and over again because
    // of shader changes.
    if (unlikely(!sampledInShader && (depthWrite || !wasHazard)))
      continue;

    m_textureSlotTracking.hazardDS |= 1 << samplerIdx;

    // Don't mark texture as an unresolvable hazard if the shader doesn't
    // actually use it.
    if (unlikely(!sampledInShader))
      continue;

    // The hazard can be resolved by binding it as READONLY.
    if (unlikely(!depthWrite))
      continue;

    // We have to bind the DS as writable, so we need FEEDBACK_LOOP_LAYOUT.
    m_textureSlotTracking.unresolvableHazardDS |= 1 << samplerIdx;
  }

  // Only dirty the framebuffer if we need to make changes for a new hazard
  if (unlikely(m_textureSlotTracking.hazardDS != oldHazardMask ||
               m_textureSlotTracking.unresolvableHazardDS !=
                   oldUnresolvableHazardMask)) {
    m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  }
}

void D3D9DeviceEx::EmitFeedbackLoopBarriers() {
  struct {
    uint8_t RT : 1;
    uint8_t DS : 1;
  } hazardState;
  hazardState.RT = m_textureSlotTracking.unresolvableHazardRT != 0;
  hazardState.DS = m_textureSlotTracking.unresolvableHazardDS != 0;

  EmitCs([cHazardState = hazardState](DxvkContext *ctx) {
    VkPipelineStageFlags srcStages = 0;
    VkAccessFlags srcAccess = 0;

    if (cHazardState.RT != 0) {
      srcStages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      srcAccess |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (cHazardState.DS != 0) {
      srcStages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      srcAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    ctx->emitGraphicsBarrier(srcStages, srcAccess,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT);
  });

  for (uint32_t samplerIdx :
       bit::BitMask(m_textureSlotTracking.unresolvableHazardRT |
                    m_textureSlotTracking.unresolvableHazardDS)) {
    // Guaranteed to not be nullptr...
    auto tex = GetCommonTexture(m_state.textures[samplerIdx]);
    if (unlikely(!tex->MarkTransitionedToHazardLayout())) {
      TransitionImage(tex, m_hazardLayout);
      m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
    }
  }
}

void D3D9DeviceEx::UpdateActiveFetch4(uint32_t stateSampler) {
  auto &state = m_state.samplerStates;

  const uint32_t samplerBit = 1u << stateSampler;

  auto texture = GetCommonTexture(m_state.textures[stateSampler]);
  const bool textureSupportsFetch4 =
      texture != nullptr && texture->SupportsFetch4();

  const bool fetch4Enabled =
      m_textureSlotTracking.fetch4SamplerState & samplerBit;
  const bool pointSampled =
      state[stateSampler][D3DSAMP_MAGFILTER] == D3DTEXF_POINT;
  const bool shouldFetch4 =
      fetch4Enabled && textureSupportsFetch4 && pointSampled;

  if (unlikely(shouldFetch4 != !!(m_textureSlotTracking.fetch4 & samplerBit))) {
    if (shouldFetch4)
      m_textureSlotTracking.fetch4 |= samplerBit;
    else
      m_textureSlotTracking.fetch4 &= ~samplerBit;
  }
}

void D3D9DeviceEx::UploadManagedTexture(D3D9CommonTexture *pResource) {
  for (uint32_t subresource = 0; subresource < pResource->CountSubresources();
       subresource++) {
    if (!pResource->NeedsUpload(subresource))
      continue;

    this->FlushImage(pResource, subresource);
  }

  pResource->ClearDirtyBoxes();
  pResource->ClearNeedsUpload();
}

void D3D9DeviceEx::UploadManagedTextures(uint32_t mask) {
  // Guaranteed to not be nullptr...
  for (uint32_t texIdx : bit::BitMask(mask))
    UploadManagedTexture(GetCommonTexture(m_state.textures[texIdx]));

  m_textureSlotTracking.needsUpload &= ~mask;
}

void D3D9DeviceEx::UpdateTextureTypeMismatchesForShader(
    const D3D9CommonShader *shader, uint32_t shaderSamplerMask,
    uint32_t shaderSamplerOffset) {
  const uint32_t stageCorrectedShaderSamplerMask = shaderSamplerMask
                                                   << shaderSamplerOffset;
  if (unlikely(shader->GetInfo().majorVersion() < 2 ||
               m_d3d9Options.forceSamplerTypeSpecConstants)) {
    // SM 1 shaders don't define the texture type in the shader.
    // We always use spec constants for those.
    m_textureSlotTracking.textureDirty |=
        stageCorrectedShaderSamplerMask &
        m_textureSlotTracking.mismatchingTextureType;
    m_textureSlotTracking.mismatchingTextureType &=
        ~stageCorrectedShaderSamplerMask;
    return;
  }

  for (const uint32_t i : bit::BitMask(stageCorrectedShaderSamplerMask)) {
    const D3D9CommonTexture *texture = GetCommonTexture(m_state.textures[i]);
    if (unlikely(texture == nullptr)) {
      // Unbound textures are not mismatching texture types
      m_textureSlotTracking.textureDirty |=
          m_textureSlotTracking.mismatchingTextureType & (1 << i);
      m_textureSlotTracking.mismatchingTextureType &= ~(1 << i);
      continue;
    }

    VkImageViewType boundViewType =
        D3D9CommonTexture::GetImageViewTypeFromResourceType(
            texture->GetType(), D3D9CommonTexture::AllLayers);
    VkImageViewType shaderViewType =
        shader->GetImageViewType(i - shaderSamplerOffset);
    if (unlikely(boundViewType != shaderViewType)) {
      m_textureSlotTracking.textureDirty |= 1 << i;
      m_textureSlotTracking.mismatchingTextureType |= 1 << i;
    } else {
      // The texture type is no longer mismatching, make sure we bind the
      // texture now.
      m_textureSlotTracking.textureDirty |=
          m_textureSlotTracking.mismatchingTextureType & (1 << i);
      m_textureSlotTracking.mismatchingTextureType &= ~(1 << i);
    }
  }
}

void D3D9DeviceEx::UpdateTextureTypeMismatchesForTexture(
    uint32_t stateSampler) {
  uint32_t shaderTextureIndex;
  const D3D9CommonShader *shader;
  if (likely(IsPSSampler(stateSampler))) {
    shader = GetCommonShader(m_state.pixelShader);
    shaderTextureIndex = stateSampler;
  } else if (unlikely(IsVSSampler(stateSampler))) {
    shader = GetCommonShader(m_state.vertexShader);
    shaderTextureIndex = stateSampler - caps::MaxTexturesPS - 1;
  } else {
    // Do not type check the fixed function displacement map texture.
    return;
  }

  if (unlikely(shader == nullptr || shader->GetInfo().majorVersion() < 2 ||
               m_d3d9Options.forceSamplerTypeSpecConstants)) {
    // This function only gets called by UpdateTextureBitmasks
    // which clears the dirty and mismatching bits for the texture before
    // anyway.
    return;
  }

  const D3D9CommonTexture *tex =
      GetCommonTexture(m_state.textures[stateSampler]);
  VkImageViewType boundViewType =
      D3D9CommonTexture::GetImageViewTypeFromResourceType(
          tex->GetType(), D3D9CommonTexture::AllLayers);
  VkImageViewType shaderViewType = shader->GetImageViewType(shaderTextureIndex);
  // D3D9 does not have 1D textures. The value of VIEW_TYPE_1D is 0
  // which is the default when there is no declaration for the type.
  bool shaderUsesTexture = shaderViewType != VkImageViewType(0);
  if (unlikely(boundViewType != shaderViewType && shaderUsesTexture)) {
    const uint32_t samplerBit = 1u << stateSampler;
    m_textureSlotTracking.mismatchingTextureType |= samplerBit;
  }
}

void D3D9DeviceEx::GenerateTextureMips(uint32_t mask) {
  for (uint32_t texIdx : bit::BitMask(mask)) {
    // Guaranteed to not be nullptr...
    auto texInfo = GetCommonTexture(m_state.textures[texIdx]);

    if (likely(texInfo->NeedsMipGen())) {
      this->EmitGenerateMips(texInfo);
      if (likely(!IsTextureBoundAsAttachment(texInfo))) {
        texInfo->SetNeedsMipGen(false);
      }
    }
  }

  m_textureSlotTracking.needsMipGen &= ~mask;
}

void D3D9DeviceEx::MarkTextureMipsDirty(D3D9CommonTexture *pResource) {
  pResource->SetNeedsMipGen(true);

  for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
    // Guaranteed to not be nullptr...
    auto texInfo = GetCommonTexture(m_state.textures[i]);

    if (texInfo == pResource) {
      m_textureSlotTracking.needsMipGen |= 1 << i;
      // We can early out here, no need to add another index for this.
      break;
    }
  }
}

void D3D9DeviceEx::MarkTextureMipsUnDirty(D3D9CommonTexture *pResource) {
  if (likely(!IsTextureBoundAsAttachment(pResource))) {
    // We need to keep the texture marked as needing mipmap generation because
    // we don't set that when rendering.
    pResource->SetNeedsMipGen(false);

    for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
      // Guaranteed to not be nullptr...
      auto texInfo = GetCommonTexture(m_state.textures[i]);

      if (unlikely(texInfo == pResource)) {
        m_textureSlotTracking.needsMipGen &= ~(1 << i);
      }
    }
  }
}

void D3D9DeviceEx::MarkTextureUploaded(D3D9CommonTexture *pResource) {
  for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
    // Guaranteed to not be nullptr...
    auto texInfo = GetCommonTexture(m_state.textures[i]);

    if (texInfo == pResource)
      m_textureSlotTracking.needsUpload &= ~(1 << i);
  }
}

void D3D9DeviceEx::UpdatePointMode(bool pointList) {
  if (!pointList) {
    UpdatePointModeSpec(0);
    return;
  }

  auto &rs = m_state.renderStates;

  const bool scale = rs[D3DRS_POINTSCALEENABLE] && !UseProgrammableVS();
  const bool sprite = rs[D3DRS_POINTSPRITEENABLE];

  const uint32_t scaleBit = scale ? 1u : 0u;
  const uint32_t spriteBit = sprite ? 2u : 0u;

  uint32_t mode = scaleBit | spriteBit;

  if (rs[D3DRS_POINTSCALEENABLE] &&
      m_dirty.test(D3D9DeviceDirtyFlag::PointScale)) {
    m_dirty.clr(D3D9DeviceDirtyFlag::PointScale);

    UpdatePushConstant<D3D9RenderStateItem::PointScaleA>();
    UpdatePushConstant<D3D9RenderStateItem::PointScaleB>();
    UpdatePushConstant<D3D9RenderStateItem::PointScaleC>();
  }

  UpdatePointModeSpec(mode);
}

void D3D9DeviceEx::UpdateFog() {
  auto &rs = m_state.renderStates;

  bool fogEnabled = rs[D3DRS_FOGENABLE];

  bool pixelFog = rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE && fogEnabled;
  bool vertexFog =
      rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE && fogEnabled && !pixelFog;

  auto UpdateFogConstants = [&](D3DFOGMODE FogMode) {
    if (m_dirty.test(D3D9DeviceDirtyFlag::FogColor)) {
      m_dirty.clr(D3D9DeviceDirtyFlag::FogColor);
      UpdatePushConstant<D3D9RenderStateItem::FogColor>();
    }

    if (FogMode == D3DFOG_LINEAR) {
      if (m_dirty.test(D3D9DeviceDirtyFlag::FogScale)) {
        m_dirty.clr(D3D9DeviceDirtyFlag::FogScale);
        UpdatePushConstant<D3D9RenderStateItem::FogScale>();
      }

      if (m_dirty.test(D3D9DeviceDirtyFlag::FogEnd)) {
        m_dirty.clr(D3D9DeviceDirtyFlag::FogEnd);
        UpdatePushConstant<D3D9RenderStateItem::FogEnd>();
      }
    } else if (FogMode == D3DFOG_EXP || FogMode == D3DFOG_EXP2) {
      if (m_dirty.test(D3D9DeviceDirtyFlag::FogDensity)) {
        m_dirty.clr(D3D9DeviceDirtyFlag::FogDensity);
        UpdatePushConstant<D3D9RenderStateItem::FogDensity>();
      }
    }
  };

  if (vertexFog) {
    D3DFOGMODE mode = D3DFOGMODE(rs[D3DRS_FOGVERTEXMODE]);

    UpdateFogConstants(mode);

    if (m_dirty.test(D3D9DeviceDirtyFlag::FogState)) {
      m_dirty.clr(D3D9DeviceDirtyFlag::FogState);

      UpdateFogModeSpec(true, mode, D3DFOG_NONE);
    }
  } else if (pixelFog) {
    D3DFOGMODE mode = D3DFOGMODE(rs[D3DRS_FOGTABLEMODE]);

    UpdateFogConstants(mode);

    if (m_dirty.test(D3D9DeviceDirtyFlag::FogState)) {
      m_dirty.clr(D3D9DeviceDirtyFlag::FogState);

      UpdateFogModeSpec(true, D3DFOG_NONE, mode);
    }
  } else {
    if (fogEnabled)
      UpdateFogConstants(D3DFOG_NONE);

    if (m_dirty.test(D3D9DeviceDirtyFlag::FogState)) {
      m_dirty.clr(D3D9DeviceDirtyFlag::FogState);

      UpdateFogModeSpec(fogEnabled, D3DFOG_NONE, D3DFOG_NONE);
    }
  }
}

void D3D9DeviceEx::BindFramebuffer() {
  m_dirty.clr(D3D9DeviceDirtyFlag::Framebuffer);

  DxvkRenderTargets attachments;

  bool srgb = m_state.renderStates[D3DRS_SRGBWRITEENABLE];

  // The extents and sample counts of all render targets need to match.
  // There's a few exceptions for mismatching extents of depth stencil surfaces:
  //   - It is allowed to be larger than RT0.
  //   - It is allowed to be smaller than RT0 IF only one render target is
  //   bound, RT0 has the NULL format
  //     or the extents 1x1 and the color write mask for RT0 is 0.

  // Dead Space uses this behavior to render shadow maps if it detects AMD
  // hardware. It detects AMD hardware by checking whether DF texture formats
  // are supported. That's only the case on the AMD D3D9 driver. On AMD hardware
  // Dead Space renders shadow maps by binding a 1x1 RT and setting the color
  // write mask to 0. On Nvidia hardware it uses a render target with the NULL
  // texture format.

  // We also unbind render targets if they aren't used for rendering but get
  // sampled. But we want to minimize frame buffer changes because those break
  // up the current render pass, so we dont unbind for disabled color write
  // masks unless the RT gets sampled.

  const auto &psMasks = PSShaderMasks();

  VkSampleCountFlags sampleCount = VK_SAMPLE_COUNT_1_BIT;
  VkExtent2D renderArea = {0u, 0u};
  const D3D9CommonTexture *rt0 =
      GetCommonTexture(m_state.renderTargets[0].ptr());
  if (likely(rt0 != nullptr && !rt0->IsNull())) {
    const DxvkImageCreateInfo &rt0Info = rt0->GetImage()->info();
    sampleCount = rt0Info.sampleCount;
    renderArea = {rt0Info.extent.width, rt0Info.extent.height};
  }

  if (m_state.depthStencil != nullptr) {
    const D3D9CommonTexture *ds = m_state.depthStencil->GetCommonTexture();
    const DxvkImageCreateInfo &dsInfo = ds->GetImage()->info();

    uint32_t boundRTCount = 0;
    for (uint32_t i = 0; i < m_state.renderTargets.size(); i++) {
      if (m_state.renderTargets[i] ==
          nullptr) // NULL format textures are counted
        continue;

      boundRTCount++;
    }

    const bool rt0WrittenTo = (psMasks.rtMask & 1u) != 0 &&
                              m_state.renderStates[D3DRS_COLORWRITEENABLE] != 0;

    // D3D9 has a special case for 1x1 textures. (Tested on Windows on the
    // Nvidia D3D9 driver.)
    const bool noRT0Bound = rt0 == nullptr || rt0->IsNull();
    const bool ignoreRT0 = boundRTCount == 1 && renderArea.width == 1 &&
                           renderArea.height == 1 && !rt0WrittenTo;

    // The depth stencil surface is allowed to be larger than the RTs.
    const bool mismatch = dsInfo.extent.width < renderArea.width ||
                          dsInfo.extent.height < renderArea.height ||
                          dsInfo.sampleCount != sampleCount;
    const bool bindDS = !mismatch || noRT0Bound || ignoreRT0;

    if (likely(bindDS)) {
      if (unlikely(noRT0Bound || ignoreRT0)) {
        renderArea = {dsInfo.extent.width, dsInfo.extent.height};
        sampleCount = dsInfo.sampleCount;
      }

      // If the DS is also bound as a texture for sampling
      // and it's either unused as DS or not written to,
      // use the readonly layout.
      bool readOnly = m_textureSlotTracking.hazardDS != 0;
      readOnly &= !m_state.renderStates[D3DRS_ZENABLE] ||
                  !m_state.renderStates[D3DRS_ZWRITEENABLE];

      // The layout of the view will be ignored by the backend anyway
      // if it has been transitioned to the feedback loop layout.
      readOnly &= !m_state.depthStencil->GetCommonTexture()
                       ->HasBeenTransitionedToHazardLayout();

      attachments.depth.view =
          m_state.depthStencil->GetDepthStencilView(!readOnly);
    }
  }

  for (uint32_t i = 0u; i < m_state.renderTargets.size(); i++) {
    if (!HasRenderTargetBound(i))
      continue;

    const D3D9CommonTexture *rt = m_state.renderTargets[i]->GetCommonTexture();
    const DxvkImageCreateInfo &rtInfo = rt->GetImage()->info();
    if (unlikely(rtInfo.extent.width != renderArea.width ||
                 rtInfo.extent.height != renderArea.height ||
                 rtInfo.sampleCount != sampleCount))
      continue;

    // Check if the render target is also bound as a texture for sampling.
    // If that's the case, check whether we can skip binding the render target
    // because writing to it is disabled anyway
    // (using the color write mask or by the current pixel shader).
    bool hasHazard = false;
    for (uint32_t samplerIdx : bit::BitMask(m_textureSlotTracking.hazardRT)) {
      D3D9Surface *rtSurf = m_state.renderTargets[i].ptr();

      IDirect3DBaseTexture9 *rtBase = rtSurf->GetBaseTexture();
      IDirect3DBaseTexture9 *texBase = m_state.textures[samplerIdx];

      if (likely(rtSurf->GetMipLevel() == 0 && rtBase == texBase)) {
        hasHazard = true;
        break;
      }
    }

    const uint32_t rtBit = 1u << i;
    const bool writtenTo = (psMasks.rtMask & rtBit) != 0 &&
                           m_state.renderStates[ColorWriteIndex(i)] != 0;

    if (hasHazard && !writtenTo)
      continue;

    attachments.color[i].view =
        m_state.renderTargets[i]->GetRenderTargetView(srgb);
  }

  // Work out feedback loop layouts based on bound render targets
  VkImageAspectFlags feedbackLoopAspects = 0u;

  if (m_hazardLayout == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT) {
    if (m_textureSlotTracking.unresolvableHazardRT != 0)
      feedbackLoopAspects |= VK_IMAGE_ASPECT_COLOR_BIT;
    if (m_textureSlotTracking.unresolvableHazardDS != 0)
      feedbackLoopAspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
  }

  // Create and bind the framebuffer object to the context
  EmitCs(
      [cAttachments = std::move(attachments),
       cFeedbackLoopAspects = feedbackLoopAspects](DxvkContext *ctx) mutable {
        ctx->bindRenderTargets(std::move(cAttachments), cFeedbackLoopAspects);
      });
}

void D3D9DeviceEx::BindViewportAndScissor() {
  m_dirty.clr(D3D9DeviceDirtyFlag::ViewportScissor);

  // D3D9's coordinate system has its origin in the bottom left,
  // but the viewport coordinates are aligned to the top-left
  // corner so we can get away with flipping the viewport.
  const D3DVIEWPORT9 &vp = m_state.viewport;

  // Correctness Factor for 1/2 texel offset
  constexpr float cf = 0.5f;

  // How much to bias MinZ by to avoid a depth
  // degenerate viewport.
  // Tests show that the bias is only applied below minZ values of 0.5
  float zBias;
  if (vp.MinZ >= 0.5f) {
    zBias = 0.0f;
  } else {
    zBias = 0.001f;
  }

  DxvkViewport state = {};
  state.viewport = VkViewport{
      float(vp.X) + cf,
      float(vp.Height + vp.Y) + cf,
      float(vp.Width),
      -float(vp.Height),
      std::clamp(vp.MinZ, 0.0f, 1.0f),
      std::clamp(std::max(vp.MaxZ, vp.MinZ + zBias), 0.0f, 1.0f),
  };

  // Scissor rectangles. Vulkan does not provide an easy way
  // to disable the scissor test, so we'll have to set scissor
  // rects that are at least as large as the framebuffer.
  bool enableScissorTest = m_state.renderStates[D3DRS_SCISSORTESTENABLE];

  if (enableScissorTest) {
    RECT sr = m_state.scissorRect;

    VkOffset2D srPosA;
    srPosA.x = std::max<int32_t>(0, sr.left);
    srPosA.x = std::max<int32_t>(vp.X, srPosA.x);
    srPosA.y = std::max<int32_t>(0, sr.top);
    srPosA.y = std::max<int32_t>(vp.Y, srPosA.y);

    VkOffset2D srPosB;
    srPosB.x = std::max<int32_t>(srPosA.x, sr.right);
    srPosB.x = std::min<int32_t>(vp.X + vp.Width, srPosB.x);
    srPosB.y = std::max<int32_t>(srPosA.y, sr.bottom);
    srPosB.y = std::min<int32_t>(vp.Y + vp.Height, srPosB.y);

    VkExtent2D srSize;
    srSize.width = uint32_t(srPosB.x - srPosA.x);
    srSize.height = uint32_t(srPosB.y - srPosA.y);

    state.scissor = VkRect2D{srPosA, srSize};
  } else {
    state.scissor = VkRect2D{VkOffset2D{int32_t(vp.X), int32_t(vp.Y)},
                             VkExtent2D{vp.Width, vp.Height}};
  }

  EmitCs([cViewport = state](DxvkContext *ctx) {
    ctx->setViewports(1, &cViewport);
  });
}

void D3D9DeviceEx::UpdateAlphaToCoverangeAndAlphaTest() {
  if (likely(!m_isD3D8Compatible)) {
    // ATOC is not supported by D3D8
    bool alphaToCoverageEnabled = true;

    // Check render states
    // The AMD ATOC enable state or the Nvidia ATOC enable state with alpha test
    // enabled (also supported by Intel) could potentially enable ATOC overall
    const bool isAMDATOCEnabled =
        m_state.renderStates[D3DRS_POINTSIZE] == uint32_t(D3D9Format::A2M1);
    const bool isNVATOCEnabled =
        m_state.renderStates[D3DRS_ADAPTIVETESS_Y] ==
            uint32_t(D3D9Format::ATOC) &&
        m_state.renderStates[D3DRS_ALPHATESTENABLE] != 0;
    const bool isAMD = m_adapter->GetVendorId() == uint32_t(DxvkGpuVendor::Amd);
    alphaToCoverageEnabled &=
        (isAMD && isAMDATOCEnabled) || (!isAMD && isNVATOCEnabled);

    // Check sample count of RT 0
    const D3D9CommonTexture *rt0 =
        GetCommonTexture(m_state.renderTargets[0].ptr());
    const bool isMultisampled =
        rt0 != nullptr &&
        (rt0->Desc()->MultiSample >= D3DMULTISAMPLE_2_SAMPLES ||
         (rt0->Desc()->MultiSample == D3DMULTISAMPLE_NONMASKABLE &&
          rt0->Desc()->MultisampleQuality > 0));
    alphaToCoverageEnabled &= isMultisampled;

    if (m_atocEnabled != alphaToCoverageEnabled) {
      m_dirty.set(D3D9DeviceDirtyFlag::MultiSampleState);
      m_atocEnabled = alphaToCoverageEnabled;
    }
  }

  // Update alpha test state
  bool alphaTestEnabled =
      m_state.renderStates[D3DRS_ALPHATESTENABLE] && !m_atocEnabled;
  if (m_alphaTestEnabled != alphaTestEnabled) {
    m_dirty.set(D3D9DeviceDirtyFlag::AlphaTestState);
    m_alphaTestEnabled = alphaTestEnabled;
  }
}

void D3D9DeviceEx::BindMultiSampleState() {
  m_dirty.clr(D3D9DeviceDirtyFlag::MultiSampleState);

  DxvkMultisampleState msState = {};
  msState.setSampleMask(
      m_validSampleMask ? uint16_t(m_state.renderStates[D3DRS_MULTISAMPLEMASK])
                        : uint16_t(0xffffu));
  msState.setAlphaToCoverage(m_atocEnabled);

  EmitCs([cState = msState](DxvkContext *ctx) {
    ctx->setMultisampleState(cState);
  });
}

void D3D9DeviceEx::BindBlendState() {
  m_dirty.clr(D3D9DeviceDirtyFlag::BlendState);

  auto &state = m_state.renderStates;

  DxvkBlendMode mode = {};
  mode.setBlendEnable(state[D3DRS_ALPHABLENDENABLE]);

  D3D9BlendState color = {};

  color.Src = D3DBLEND(state[D3DRS_SRCBLEND]);
  color.Dst = D3DBLEND(state[D3DRS_DESTBLEND]);
  color.Op = D3DBLENDOP(state[D3DRS_BLENDOP]);
  FixupBlendState(color);

  D3D9BlendState alpha = color;

  if (state[D3DRS_SEPARATEALPHABLENDENABLE]) {
    alpha.Src = D3DBLEND(state[D3DRS_SRCBLENDALPHA]);
    alpha.Dst = D3DBLEND(state[D3DRS_DESTBLENDALPHA]);
    alpha.Op = D3DBLENDOP(state[D3DRS_BLENDOPALPHA]);
    FixupBlendState(alpha);
  }

  mode.setColorOp(DecodeBlendFactor(color.Src, false),
                  DecodeBlendFactor(color.Dst, false), DecodeBlendOp(color.Op));

  mode.setAlphaOp(DecodeBlendFactor(alpha.Src, true),
                  DecodeBlendFactor(alpha.Dst, true), DecodeBlendOp(alpha.Op));

  uint16_t writeMasks = 0;

  for (uint32_t i = 0; i < 4; i++)
    writeMasks |= (state[ColorWriteIndex(i)] & 0xfu) << (4u * i);

  EmitCs([cMode = mode, cWriteMasks = writeMasks,
          cAlphaMasks = m_rtSlotTracking.hasAlphaSwizzle](DxvkContext *ctx) {
    for (uint32_t i = 0; i < 4; i++) {
      DxvkBlendMode mode = cMode;
      mode.setWriteMask(cWriteMasks >> (4u * i));

      // Adjust the blend factor based on the render target alpha swizzle bit
      // mask. Specific formats such as the XRGB ones require a ONE swizzle for
      // alpha which cannot be directly applied with the image view of the
      // attachment.
      if (cAlphaMasks & (1 << i)) {
        auto NormalizeFactor = [](VkBlendFactor Factor) {
          if (Factor == VK_BLEND_FACTOR_DST_ALPHA)
            return VK_BLEND_FACTOR_ONE;
          else if (Factor == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA)
            return VK_BLEND_FACTOR_ZERO;
          return Factor;
        };

        mode.setColorOp(NormalizeFactor(mode.colorSrcFactor()),
                        NormalizeFactor(mode.colorDstFactor()),
                        mode.colorBlendOp());
        mode.setAlphaOp(NormalizeFactor(mode.alphaSrcFactor()),
                        NormalizeFactor(mode.alphaDstFactor()),
                        mode.alphaBlendOp());
      }

      mode.normalize();

      ctx->setBlendMode(i, mode);
    }
  });
}

void D3D9DeviceEx::BindBlendFactor() {
  DxvkBlendConstants blendConstants;
  DecodeD3DCOLOR(D3DCOLOR(m_state.renderStates[D3DRS_BLENDFACTOR]),
                 reinterpret_cast<float *>(&blendConstants));

  EmitCs([cBlendConstants = blendConstants](DxvkContext *ctx) {
    ctx->setBlendConstants(cBlendConstants);
  });
}

void D3D9DeviceEx::BindDepthStencilState() {
  m_dirty.clr(D3D9DeviceDirtyFlag::DepthStencilState);

  auto &rs = m_state.renderStates;

  bool stencil = rs[D3DRS_STENCILENABLE];
  bool twoSidedStencil = stencil && rs[D3DRS_TWOSIDEDSTENCILMODE];

  DxvkDepthStencilState state = {};
  state.setDepthTest(rs[D3DRS_ZENABLE]);
  state.setDepthWrite(rs[D3DRS_ZWRITEENABLE]);
  state.setStencilTest(stencil);
  state.setDepthCompareOp(DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_ZFUNC])));

  DxvkStencilOp frontOp = {};

  if (stencil) {
    frontOp.setFailOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILFAIL])));
    frontOp.setPassOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILPASS])));
    frontOp.setDepthFailOp(
        DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILZFAIL])));
    frontOp.setCompareOp(DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_STENCILFUNC])));
    frontOp.setCompareMask(rs[D3DRS_STENCILMASK]);
    frontOp.setWriteMask(rs[D3DRS_STENCILWRITEMASK]);
  }

  DxvkStencilOp backOp = frontOp;

  if (twoSidedStencil) {
    backOp.setFailOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILFAIL])));
    backOp.setPassOp(DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILPASS])));
    backOp.setDepthFailOp(
        DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILZFAIL])));
    backOp.setCompareOp(DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_CCW_STENCILFUNC])));
    backOp.setCompareMask(rs[D3DRS_STENCILMASK]);
    backOp.setWriteMask(rs[D3DRS_STENCILWRITEMASK]);
  }

  state.setStencilOpFront(frontOp);
  state.setStencilOpBack(backOp);

  EmitCs([cState = state](DxvkContext *ctx) mutable {
    cState.normalize();

    ctx->setDepthStencilState(cState);
  });
}

void D3D9DeviceEx::BindRasterizerState() {
  m_dirty.clr(D3D9DeviceDirtyFlag::RasterizerState);

  auto &rs = m_state.renderStates;

  DxvkRasterizerState state = {};
  state.setCullMode(DecodeCullMode(D3DCULL(rs[D3DRS_CULLMODE])));
  state.setDepthClip(true);
  state.setFrontFace(VK_FRONT_FACE_CLOCKWISE);
  state.setPolygonMode(DecodeFillMode(D3DFILLMODE(rs[D3DRS_FILLMODE])));
  state.setFlatShading(m_state.renderStates[D3DRS_SHADEMODE] == D3DSHADE_FLAT);
  state.setSampleCount(m_state.renderStates[D3DRS_MULTISAMPLEANTIALIAS]
                           ? VkSampleCountFlags(0u)
                           : VkSampleCountFlags(VK_SAMPLE_COUNT_1_BIT));

  EmitCs(
      [cState = state](DxvkContext *ctx) { ctx->setRasterizerState(cState); });
}

void D3D9DeviceEx::BindDepthBias() {
  m_dirty.clr(D3D9DeviceDirtyFlag::DepthBias);

  auto &rs = m_state.renderStates;

  float depthBias = bit::cast<float>(rs[D3DRS_DEPTHBIAS]) * m_depthBiasScale;
  float slopeScaledDepthBias = bit::cast<float>(rs[D3DRS_SLOPESCALEDEPTHBIAS]);

  DxvkDepthBias biases;
  biases.depthBiasConstant = depthBias;
  biases.depthBiasSlope = slopeScaledDepthBias;
  biases.depthBiasClamp = 0.0f;

  EmitCs([cBiases = biases](DxvkContext *ctx) { ctx->setDepthBias(cBiases); });
}

uint32_t D3D9DeviceEx::GetAlphaTestPrecision() {
  if (m_state.renderTargets[0] == nullptr)
    return 0;

  D3D9Format format =
      m_state.renderTargets[0]->GetCommonTexture()->Desc()->Format;

  switch (format) {
  case D3D9Format::A2B10G10R10:
  case D3D9Format::A2R10G10B10:
  case D3D9Format::A2W10V10U10:
  case D3D9Format::A2B10G10R10_XR_BIAS:
    return 0x2; /* 10 bit */

  case D3D9Format::R16F:
  case D3D9Format::G16R16F:
  case D3D9Format::A16B16G16R16F:
    return 0x7; /* 15 bit */

  case D3D9Format::G16R16:
  case D3D9Format::A16B16G16R16:
  case D3D9Format::V16U16:
  case D3D9Format::L16:
  case D3D9Format::Q16W16V16U16:
    return 0x8; /* 16 bit */

  case D3D9Format::R32F:
  case D3D9Format::G32R32F:
  case D3D9Format::A32B32G32R32F:
    return 0xF; /* float */

  default:
    return 0x0; /* 8 bit */
  }
}

void D3D9DeviceEx::BindAlphaTestState() {
  m_dirty.clr(D3D9DeviceDirtyFlag::AlphaTestState);

  auto &rs = m_state.renderStates;

  VkCompareOp alphaOp = m_alphaTestEnabled
                            ? DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_ALPHAFUNC]))
                            : VK_COMPARE_OP_ALWAYS;

  uint32_t precision =
      alphaOp != VK_COMPARE_OP_ALWAYS ? GetAlphaTestPrecision() : 0u;

  UpdateAlphaTestSpec(alphaOp, precision);
}

void D3D9DeviceEx::BindDepthStencilReference() {
  auto &rs = m_state.renderStates;

  uint32_t ref = uint32_t(rs[D3DRS_STENCILREF]) & 0xff;

  EmitCs([cRef = ref](DxvkContext *ctx) { ctx->setStencilReference(cRef); });
}

void D3D9DeviceEx::BindSampler(DWORD Sampler) {
  auto samplerInfo = RemapStateSamplerShader(Sampler);

  const uint32_t slot = computeResourceSlotId(
      samplerInfo.first, DxsoBindingType::Image, samplerInfo.second);

  m_samplerBindCount++;

  const D3D9CommonTexture *tex = GetCommonTexture(m_state.textures[Sampler]);

  EmitCs([this, cSlot = slot,
          cState = D3D9SamplerInfo(m_state.samplerStates[Sampler]),
          cIsCube = tex && tex->IsCube(),
          cIsMultiMip = tex && (tex->Desc()->MipLevels > 1u),
          cIsDepth = bool(m_textureSlotTracking.depth & (1u << Sampler)),
          cBindId = m_samplerBindCount](DxvkContext *ctx) {
    DxvkSamplerKey key = {};

    key.setFilter(DecodeFilter(cState.minFilter),
                  DecodeFilter(cState.magFilter),
                  DecodeMipFilter(cState.mipFilter));

    if (cIsCube) {
      key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                          VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                          VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

      key.setLegacyCubeFilter(!m_d3d9Options.seamlessCubes);
    } else {
      key.setAddressModes(DecodeAddressMode(cState.addressU),
                          DecodeAddressMode(cState.addressV),
                          DecodeAddressMode(cState.addressW));
    }

    key.setDepthCompare(cIsDepth, VK_COMPARE_OP_LESS_OR_EQUAL);

    if (cState.mipFilter) {
      uint32_t anisotropy = cState.maxAnisotropy;

      // Anisotropic filtering doesn't make any sense with only one mip
      if (cState.minFilter != D3DTEXF_ANISOTROPIC || !cIsMultiMip)
        anisotropy = 0u;

      // Forcing anisotropic filtering doesn't make any sense with only one mip
      if (m_d3d9Options.samplerAnisotropy != -1 && cIsMultiMip &&
          cState.minFilter > D3DTEXF_POINT)
        anisotropy = m_d3d9Options.samplerAnisotropy;

      key.setAniso(anisotropy);

      float lodBias = cState.mipLodBias;
      lodBias += m_d3d9Options.samplerLodBias;

      if (m_d3d9Options.clampNegativeLodBias)
        lodBias = std::max(lodBias, 0.0f);

      key.setLodRange(float(cState.maxMipLevel), 16.0f, lodBias);
    }

    if (key.u.p.hasBorder)
      DecodeD3DCOLOR(cState.borderColor, key.borderColor.float32);

    VkShaderStageFlags stage =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    ctx->bindResourceSampler(stage, cSlot, m_dxvkDevice->createSampler(key));

    // Let the main thread know about current sampler stats
    uint64_t liveCount = m_dxvkDevice->getSamplerStats().liveCount;
    m_lastSamplerStats.store(liveCount | (cBindId << SamplerCountBits),
                             std::memory_order_relaxed);
  });
}

void D3D9DeviceEx::BindTexture(DWORD StateSampler) {
  auto shaderSampler = RemapStateSamplerShader(StateSampler);

  uint32_t slot =
      computeResourceSlotId(shaderSampler.first, DxsoBindingType::Image,
                            uint32_t(shaderSampler.second));

  const bool srgb =
      m_state.samplerStates[StateSampler][D3DSAMP_SRGBTEXTURE] & 0x1;

  D3D9CommonTexture *commonTex =
      GetCommonTexture(m_state.textures[StateSampler]);

  Rc<DxvkImageView> imageView = commonTex->GetSampleView(srgb);

  EmitCs([cSlot = slot,
          cImageView = std::move(imageView)](DxvkContext *ctx) mutable {
    VkShaderStageFlags stage =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    ctx->bindResourceImageView(stage, cSlot, std::move(cImageView));
  });
}

void D3D9DeviceEx::UnbindTextures(uint32_t mask) {
  EmitCs([cMask = mask](DxvkContext *ctx) {
    for (uint32_t i : bit::BitMask(cMask)) {
      auto shaderSampler = RemapStateSamplerShader(i);

      uint32_t slot =
          computeResourceSlotId(shaderSampler.first, DxsoBindingType::Image,
                                uint32_t(shaderSampler.second));

      VkShaderStageFlags stage =
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      ctx->bindResourceImageView(stage, slot, nullptr);
    }
  });
}

void D3D9DeviceEx::UndirtySamplers(uint32_t mask) {
  EnsureSamplerLimit();

  for (uint32_t i : bit::BitMask(mask))
    BindSampler(i);

  m_textureSlotTracking.samplerStateDirty &= ~mask;
}

void D3D9DeviceEx::UndirtyTextures(uint32_t usedMask) {
  const uint32_t activeMask =
      usedMask & (m_textureSlotTracking.bound &
                  ~m_textureSlotTracking.mismatchingTextureType);
  const uint32_t inactiveMask =
      usedMask & (~m_textureSlotTracking.bound |
                  m_textureSlotTracking.mismatchingTextureType);

  for (uint32_t i : bit::BitMask(activeMask))
    BindTexture(i);

  if (inactiveMask)
    UnbindTextures(inactiveMask);

  m_textureSlotTracking.textureDirty &= ~usedMask;
}

void D3D9DeviceEx::MarkTextureBindingDirty(IDirect3DBaseTexture9 *texture) {
  D3D9DeviceLock lock = LockDevice();

  for (uint32_t i : bit::BitMask(m_textureSlotTracking.bound)) {
    if (m_state.textures[i] == texture)
      m_textureSlotTracking.textureDirty |= 1u << i;
  }
}

D3D9DrawInfo D3D9DeviceEx::GenerateDrawInfo(D3DPRIMITIVETYPE PrimitiveType,
                                            UINT PrimitiveCount,
                                            UINT InstanceCount) {
  D3D9DrawInfo drawInfo;
  drawInfo.vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);
  drawInfo.instanceCount =
      (m_iaState.streamsInstanced & m_iaState.streamsUsed) ? InstanceCount : 1u;
  return drawInfo;
}

uint32_t D3D9DeviceEx::GetInstanceCount() const {
  return std::max(m_state.streamFreq[0] & 0x7FFFFFu, 1u);
}

void D3D9DeviceEx::PrepareDraw(D3DPRIMITIVETYPE PrimitiveType, bool UploadVBOs,
                               bool UploadIBO) {
  if (unlikely(m_textureSlotTracking.unresolvableHazardRT != 0 ||
               m_textureSlotTracking.unresolvableHazardDS != 0))
    EmitFeedbackLoopBarriers();

  if (likely(UploadVBOs)) {
    const uint32_t usedBuffersMask = m_state.vertexDecl != nullptr
                                         ? m_state.vertexDecl->GetStreamMask()
                                         : ~0u;
    const uint32_t buffersToUpload =
        m_vbSlotTracking.needsUpload & usedBuffersMask;
    for (uint32_t bufferIdx : bit::BitMask(buffersToUpload)) {
      auto *vbo =
          GetCommonBuffer(m_state.vertexBuffers[bufferIdx].vertexBuffer);
      if (likely(vbo != nullptr && vbo->NeedsUpload()))
        FlushBuffer(vbo);
    }
    m_vbSlotTracking.needsUpload &= ~buffersToUpload;
  }

  const uint32_t usedSamplerMask =
      PSShaderMasks().samplerMask | VSShaderMasks().samplerMask;
  const uint32_t usedTextureMask =
      m_textureSlotTracking.bound & usedSamplerMask;

  const uint32_t texturesToUpload =
      m_textureSlotTracking.needsUpload & usedTextureMask;
  if (unlikely(texturesToUpload != 0))
    UploadManagedTextures(texturesToUpload);

  const uint32_t texturesToGen =
      m_textureSlotTracking.needsMipGen & usedTextureMask;
  if (unlikely(texturesToGen != 0))
    GenerateTextureMips(texturesToGen);

  auto *ibo = GetCommonBuffer(m_state.indices);
  if (unlikely(UploadIBO && ibo != nullptr && ibo->NeedsUpload()))
    FlushBuffer(ibo);

  UpdateFog();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::Framebuffer)))
    BindFramebuffer();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::ViewportScissor)))
    BindViewportAndScissor();

  const uint32_t activeDirtySamplers =
      m_textureSlotTracking.samplerStateDirty & usedTextureMask;
  if (unlikely(activeDirtySamplers))
    UndirtySamplers(activeDirtySamplers);

  const uint32_t usedDirtyTextures =
      m_textureSlotTracking.textureDirty & usedSamplerMask;
  if (likely(usedDirtyTextures))
    UndirtyTextures(usedDirtyTextures);

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::BlendState)))
    BindBlendState();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::DepthStencilState)))
    BindDepthStencilState();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::RasterizerState)))
    BindRasterizerState();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::DepthBias)))
    BindDepthBias();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::MultiSampleState)))
    BindMultiSampleState();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::AlphaTestState)))
    BindAlphaTestState();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::ClipPlanes)))
    UpdateClipPlanes();

  UpdatePointMode(PrimitiveType == D3DPT_POINTLIST);

  if (likely(UseProgrammableVS())) {
    UploadConstants<DxsoProgramTypes::VertexShader>();

    if (likely(!CanSWVP())) {
      UpdateVertexBoolSpec(
          m_state.vsConsts->bConsts[0] &
          m_consts[DxsoProgramType::VertexShader].meta.boolConstantMask);
    } else
      UpdateVertexBoolSpec(0);
  } else {
    UpdateVertexBoolSpec(0);
    UpdateFixedFunctionVS();
  }

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::InputLayout)))
    BindInputLayout();

  uint32_t projected = m_textureSlotTracking.projected;
  if (likely(UseProgrammablePS())) {
    UploadConstants<DxsoProgramTypes::PixelShader>();

    const uint32_t psTextureMask =
        usedTextureMask & ((1u << caps::MaxTexturesPS) - 1u);
    const uint32_t fetch4 = m_textureSlotTracking.fetch4 & psTextureMask;
    uint32_t textureTypes = m_textureSlotTracking.textureType;

    const auto &programInfo = GetCommonShader(m_state.pixelShader)->GetInfo();
    const bool useProgrammableVS = UseProgrammableVS();

    // Fixed function shaders use the projected spec constant too.
    if (likely(useProgrammableVS && (programInfo.majorVersion() > 2 ||
                                     programInfo.minorVersion() > 3))) {
      projected = 0u;
    } else if (useProgrammableVS) {
      // Programmable shaders can only sample textures in SM3 which doesn't use
      // the projected state anymore. So we can restrict it to the ones that the
      // pixel shader uses.
      projected &= psTextureMask;
    }

    if (likely(programInfo.majorVersion() >= 2 &&
               !m_d3d9Options.forceSamplerTypeSpecConstants)) {
      // SM2 and up need to declare the sampler type in the shader.
      textureTypes = 0u;
    }

    UpdatePixelShaderSamplerSpec(textureTypes, fetch4);

    UpdatePixelBoolSpec(
        m_state.psConsts->bConsts[0] &
        m_consts[DxsoProgramType::PixelShader].meta.boolConstantMask);
  } else {
    // Fixed function shaders use the projected spec constant too.
    if (likely(UseProgrammableVS())) {
      const uint32_t psTextureMask = usedTextureMask & ((1u << 8u) - 1u);
      projected &= psTextureMask;
    }

    UpdatePixelBoolSpec(0);
    UpdatePixelShaderSamplerSpec(m_textureSlotTracking.textureType, 0u);

    UpdateFixedFunctionPS();
  }

  const uint32_t nullTextureMask = usedSamplerMask & ~usedTextureMask;
  const uint32_t depthTextureMask =
      m_textureSlotTracking.depth & usedTextureMask;
  const uint32_t drefClampMask =
      m_textureSlotTracking.drefClamp & depthTextureMask;
  UpdateCommonSamplerSpec(nullTextureMask, depthTextureMask, drefClampMask,
                          projected);

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::SharedPixelShaderData))) {
    m_dirty.clr(D3D9DeviceDirtyFlag::SharedPixelShaderData);

    auto mapPtr = m_psShared.AllocSlice();
    D3D9SharedPS *data = reinterpret_cast<D3D9SharedPS *>(mapPtr);

    for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
      DecodeD3DCOLOR(D3DCOLOR(m_state.textureStages[i][DXVK_TSS_CONSTANT]),
                     data->Stages[i].Constant);

      // Flip major-ness so we can get away with a nice easy
      // dot in the shader without complex access
      data->Stages[i].BumpEnvMat[0][0] =
          bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVMAT00]);
      data->Stages[i].BumpEnvMat[1][0] =
          bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVMAT01]);
      data->Stages[i].BumpEnvMat[0][1] =
          bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVMAT10]);
      data->Stages[i].BumpEnvMat[1][1] =
          bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVMAT11]);

      data->Stages[i].BumpEnvLScale =
          bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVLSCALE]);
      data->Stages[i].BumpEnvLOffset =
          bit::cast<float>(m_state.textureStages[i][DXVK_TSS_BUMPENVLOFFSET]);
    }
  }

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::DepthBounds))) {
    m_dirty.clr(D3D9DeviceDirtyFlag::DepthBounds);

    DxvkDepthBounds db = {};
    db.minDepthBounds = 0.0f;
    db.maxDepthBounds = 1.0f;

    if (m_nvdbEnabled) {
      db.minDepthBounds = std::clamp(
          bit::cast<float>(m_state.renderStates[D3DRS_ADAPTIVETESS_Z]), 0.0f,
          1.0f);
      db.maxDepthBounds = std::clamp(
          bit::cast<float>(m_state.renderStates[D3DRS_ADAPTIVETESS_W]), 0.0f,
          1.0f);

      if (db.maxDepthBounds < db.minDepthBounds) {
        db.minDepthBounds = 0.0f;
        db.maxDepthBounds = 1.0f;
      }
    }

    EmitCs([cDepthBounds = db](DxvkContext *ctx) {
      ctx->setDepthBounds(cDepthBounds);
    });
  }

  BindSpecConstants();

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::VertexBuffers) &&
               UploadVBOs)) {
    for (uint32_t i = 0; i < caps::MaxStreams; i++) {
      const D3D9VBO &vbo = m_state.vertexBuffers[i];
      BindVertexBuffer(i, vbo.vertexBuffer.ptr(), vbo.offset, vbo.stride);
    }
    m_dirty.clr(D3D9DeviceDirtyFlag::VertexBuffers);
  }

  if (unlikely(m_dirty.test(D3D9DeviceDirtyFlag::IndexBuffer) && UploadIBO)) {
    BindIndices();
    m_dirty.clr(D3D9DeviceDirtyFlag::IndexBuffer);
  }
}

void D3D9DeviceEx::EnsureSamplerLimit() {
  constexpr uint32_t MaxSamplerCount =
      DxvkSamplerPool::MaxSamplerCount - SamplerCount;

  // Maximum possible number of live samplers we can have
  // since last reading back from the CS thread.
  if (likely(m_lastSamplerLiveCount + m_samplerBindCount -
                 m_lastSamplerBindCount <=
             MaxSamplerCount))
    return;

  // Update current stats from CS thread and check again. We
  // don't want to do this every time due to potential cache
  // thrashing.
  uint64_t lastStats = m_lastSamplerStats.load(std::memory_order_relaxed);
  m_lastSamplerLiveCount = lastStats & SamplerCountMask;
  m_lastSamplerBindCount = lastStats >> SamplerCountBits;

  if (likely(m_lastSamplerLiveCount + m_samplerBindCount -
                 m_lastSamplerBindCount <=
             MaxSamplerCount))
    return;

  // If we have a large number of sampler updates in flight, wait for
  // the CS thread to complete some and re-evaluate. We should not hit
  // this path under normal gameplay conditions.
  ConsiderFlush(GpuFlushType::ImplicitSynchronization);

  uint64_t sequenceNumber = m_csThread.lastSequenceNumber();

  while (++sequenceNumber <= GetCurrentSequenceNumber()) {
    SynchronizeCsThread(sequenceNumber);

    uint64_t lastStats = m_lastSamplerStats.load(std::memory_order_relaxed);
    m_lastSamplerLiveCount = lastStats & SamplerCountMask;
    m_lastSamplerBindCount = lastStats >> SamplerCountBits;

    if (m_lastSamplerLiveCount + m_samplerBindCount - m_lastSamplerBindCount <=
        MaxSamplerCount)
      return;
  }

  // If we end up here, the game somehow managed to queue up so
  // many samplers that we need to wait for the GPU to free some.
  // We should absolutely never hit this path in the real world.
  Logger::warn("Sampler pool exhausted, synchronizing with GPU.");

  Flush();
  SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

  uint64_t submissionId = m_submissionFence->value();

  while (++submissionId <= m_submissionId) {
    m_submissionFence->wait(submissionId);

    // Need to manually update sampler stats here since we
    // might otherwise hit this path again the next time
    auto samplerStats = m_dxvkDevice->getSamplerStats();
    m_lastSamplerStats =
        samplerStats.liveCount | (m_samplerBindCount << SamplerCountBits);

    if (samplerStats.liveCount <= MaxSamplerCount)
      return;
  }

  // If we end up *here*, good luck.
  Logger::warn("Sampler pool exhausted, cannot create any new samplers.");
}

template <DxsoProgramType ShaderStage>
void D3D9DeviceEx::BindShader(const D3D9CommonShader *pShaderModule) {
  auto shader = pShaderModule->GetShader();

  if (unlikely(shader->needsCompile()))
    m_dxvkDevice->requestCompileShader(shader);

  EmitCs([cShader = std::move(shader)](DxvkContext *ctx) mutable {
    constexpr VkShaderStageFlagBits stage = GetShaderStage(ShaderStage);
    ctx->bindShader<stage>(std::move(cShader));
  });
}

template <DxsoProgramType ShaderStage> void D3D9DeviceEx::BindFFUbershader() {
  if (ShaderStage == DxsoProgramType::VertexShader) {
    EmitCs([&cShaders = m_ffModules](DxvkContext *ctx) {
      auto shader = cShaders.GetVSUbershaderModule();
      ctx->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(shader.GetShader());
    });
  } else {
    EmitCs([&cShaders = m_ffModules](DxvkContext *ctx) {
      auto shader = cShaders.GetFSUbershaderModule();
      ctx->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(shader.GetShader());
    });
  }
}

void D3D9DeviceEx::BindInputLayout() {
  m_dirty.clr(D3D9DeviceDirtyFlag::InputLayout);

  if (m_state.vertexDecl == nullptr) {
    EmitCs([&cIaState = m_iaState](DxvkContext *ctx) {
      cIaState.streamsUsed = 0;
      ctx->setInputLayout(0, nullptr, 0, nullptr);
    });
  } else {
    std::array<uint32_t, caps::MaxStreams> streamFreq;

    for (uint32_t i = 0; i < caps::MaxStreams; i++)
      streamFreq[i] = m_state.streamFreq[i];

    Com<D3D9VertexDecl, false> vertexDecl = m_state.vertexDecl;
    Com<D3D9VertexShader, false> vertexShader;

    if (UseProgrammableVS())
      vertexShader = m_state.vertexShader;

    EmitCs([&cIaState = m_iaState, cVertexDecl = std::move(vertexDecl),
            cVertexShader = std::move(vertexShader),
            cStreamsInstanced = m_vbSlotTracking.instanced,
            cStreamFreq = streamFreq](DxvkContext *ctx) {
      cIaState.streamsInstanced = cStreamsInstanced;
      cIaState.streamsUsed = 0;

      const auto &elements = cVertexDecl->GetElements();

      std::array<DxvkVertexInput, 2 * caps::InputRegisterCount> attrList = {};
      std::array<DxvkVertexInput, 2 * caps::InputRegisterCount> bindList = {};
      std::array<uint32_t, 2 * caps::InputRegisterCount> vertexSizes = {};

      uint32_t attrMask = 0;
      uint32_t bindMask = 0;

      const auto &isgn = cVertexShader != nullptr
                             ? GetCommonShader(cVertexShader)->GetIsgn()
                             : GetFixedFunctionIsgn();

      for (uint32_t i = 0; i < isgn.elemCount; i++) {
        const auto &decl = isgn.elems[i];

        DxvkVertexAttribute attrib = {};
        attrib.location = i;
        attrib.binding = NullStreamIdx;
        attrib.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrib.offset = 0;

        for (const auto &element : elements) {
          DxsoSemantic elementSemantic = {static_cast<DxsoUsage>(element.Usage),
                                          element.UsageIndex};
          if (elementSemantic.usage == DxsoUsage::PositionT)
            elementSemantic.usage = DxsoUsage::Position;

          if (elementSemantic == decl.semantic) {
            attrib.binding = uint32_t(element.Stream);
            attrib.format = DecodeDecltype(D3DDECLTYPE(element.Type));
            attrib.offset = element.Offset;

            cIaState.streamsUsed |= 1u << attrib.binding;
            break;
          }
        }

        attrList[i] = DxvkVertexInput(attrib);

        vertexSizes[attrib.binding] =
            std::max(vertexSizes[attrib.binding],
                     uint32_t(attrib.offset +
                              lookupFormatInfo(attrib.format)->elementSize));

        DxvkVertexBinding binding = {};
        binding.binding = attrib.binding;
        binding.extent = vertexSizes[attrib.binding];

        uint32_t instanceData = cStreamFreq[binding.binding % caps::MaxStreams];
        if (instanceData & D3DSTREAMSOURCE_INSTANCEDATA) {
          binding.divisor =
              instanceData &
              0x7FFFFF; // Remove instance packed-in flags in the data.
          binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        } else {
          binding.divisor = 0u;
          binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        }

        bindList[binding.binding] = DxvkVertexInput(binding);

        attrMask |= 1u << i;
        bindMask |= 1u << binding.binding;
      }

      // Compact the attribute and binding lists to filter
      // out attributes and bindings not used by the shader
      uint32_t attrCount = CompactSparseList(attrList.data(), attrMask);
      uint32_t bindCount = CompactSparseList(bindList.data(), bindMask);

      ctx->setInputLayout(attrCount, attrList.data(), bindCount,
                          bindList.data());
    });
  }
}

void D3D9DeviceEx::BindVertexBuffer(UINT Slot, D3D9VertexBuffer *pBuffer,
                                    UINT Offset, UINT Stride) {
  EmitCs([cSlotId = Slot,
          cBufferSlice =
              pBuffer != nullptr
                  ? pBuffer->GetCommonBuffer()
                        ->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(Offset)
                  : DxvkBufferSlice(),
          cStride = pBuffer != nullptr ? Stride : 0](DxvkContext *ctx) mutable {
    ctx->bindVertexBuffer(cSlotId, std::move(cBufferSlice), cStride);
  });
}

void D3D9DeviceEx::BindIndices() {
  D3D9CommonBuffer *buffer = GetCommonBuffer(m_state.indices);

  D3D9Format format =
      buffer != nullptr ? buffer->Desc()->Format : D3D9Format::INDEX32;

  const VkIndexType indexType = DecodeIndexType(format);

  EmitCs([cBufferSlice =
              buffer != nullptr
                  ? buffer->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>()
                  : DxvkBufferSlice(),
          cIndexType = indexType](DxvkContext *ctx) mutable {
    ctx->bindIndexBuffer(std::move(cBufferSlice), cIndexType);
  });
}

void D3D9DeviceEx::Begin(D3D9Query *pQuery) {
  D3D9DeviceLock lock = LockDevice();

  EmitCs([cQuery = Com<D3D9Query, false>(pQuery)](DxvkContext *ctx) {
    cQuery->Begin(ctx);
  });
}

void D3D9DeviceEx::End(D3D9Query *pQuery) {
  D3D9DeviceLock lock = LockDevice();

  EmitCs([cQuery = Com<D3D9Query, false>(pQuery)](DxvkContext *ctx) {
    cQuery->End(ctx);
  });

  pQuery->NotifyEnd();
  if (unlikely(pQuery->IsEvent())) {
    pQuery->IsStalling() ? Flush()
                         : ConsiderFlush(GpuFlushType::ImplicitStrongHint);
  } else if (pQuery->IsStalling()) {
    ConsiderFlush(GpuFlushType::ImplicitWeakHint);
  }
}

void D3D9DeviceEx::SetVertexBoolBitfield(uint32_t idx, uint32_t mask,
                                         uint32_t bits) {
  m_state.vsConsts->bConsts[idx] &= ~mask;
  m_state.vsConsts->bConsts[idx] |= bits & mask;

  m_consts[DxsoProgramTypes::VertexShader].dirty = true;
}

void D3D9DeviceEx::SetPixelBoolBitfield(uint32_t idx, uint32_t mask,
                                        uint32_t bits) {
  m_state.psConsts->bConsts[idx] &= ~mask;
  m_state.psConsts->bConsts[idx] |= bits & mask;

  m_consts[DxsoProgramTypes::PixelShader].dirty = true;
}

HRESULT D3D9DeviceEx::CreateShaderModule(D3D9CommonShader *pShaderModule,
                                         uint32_t *pLength,
                                         VkShaderStageFlagBits ShaderStage,
                                         const DWORD *pShaderBytecode,
                                         const DxsoModuleInfo *pModuleInfo) {
  const char *stageName = ShaderStage == VK_SHADER_STAGE_VERTEX_BIT ? "VS"
                          : ShaderStage == VK_SHADER_STAGE_FRAGMENT_BIT
                              ? "PS"
                              : "Unknown";
  try {
    Logger::info(
        str::format("DEBUG_SHADER: CreateShaderModule ", stageName, " start."));
    m_shaderModules->GetShaderModule(this, pShaderModule, pLength, ShaderStage,
                                     pModuleInfo, pShaderBytecode);

    return D3D_OK;
  } catch (const DxvkError &e) {
    const uint32_t token0 = pShaderBytecode ? pShaderBytecode[0] : 0u;
    const char *stageName = ShaderStage == VK_SHADER_STAGE_VERTEX_BIT ? "VS"
                            : ShaderStage == VK_SHADER_STAGE_FRAGMENT_BIT
                                ? "PS"
                                : "Unknown";

    // Force INFO level logging for visibility
    Logger::info(str::format("DEBUG_SHADER: NOTIFY CreateShaderModule failed (",
                             stageName, ", token0=0x", std::hex, token0,
                             std::dec, "): ", e.message()));

    Logger::err(str::format("CreateShaderModule failed (", stageName,
                            ", token0=0x", std::hex, token0, std::dec,
                            "): ", e.message()));
    return D3DERR_INVALIDCALL;
  } catch (const std::exception &e) {
    Logger::info(str::format(
        "DEBUG_SHADER: UNHANDLED STD EXCEPTION in CreateShaderModule: ",
        e.what()));
    return D3DERR_INVALIDCALL;
  } catch (...) {
    Logger::info(
        "DEBUG_SHADER: UNHANDLED UNKNOWN EXCEPTION in CreateShaderModule");
    return D3DERR_INVALIDCALL;
  }
}

template <DxsoProgramType ProgramType, D3D9ConstantType ConstantType,
          typename T>
HRESULT D3D9DeviceEx::SetShaderConstants(UINT StartRegister,
                                         const T *pConstantData, UINT Count) {
  const uint32_t regCountHardware =
      DetermineHardwareRegCount<ProgramType, ConstantType>();
  constexpr uint32_t regCountSoftware =
      DetermineSoftwareRegCount<ProgramType, ConstantType>();

  // Error out in case of StartRegister + Count overflow
  if (unlikely(StartRegister > std::numeric_limits<uint32_t>::max() - Count))
    return D3DERR_INVALIDCALL;

  if (unlikely(StartRegister + Count > regCountSoftware))
    return D3DERR_INVALIDCALL;

  Count = UINT(std::max<INT>(
      std::clamp<INT>(Count + StartRegister, 0, regCountHardware) -
          INT(StartRegister),
      0));

  if (unlikely(Count == 0))
    return D3D_OK;

  if (unlikely(pConstantData == nullptr))
    return D3DERR_INVALIDCALL;

  if (unlikely(ShouldRecord()))
    return m_recorder->SetShaderConstants<ProgramType, ConstantType, T>(
        StartRegister, pConstantData, Count);

  D3D9ConstantSets &constSet = m_consts[ProgramType];

  if constexpr (ConstantType == D3D9ConstantType::Float) {
    constSet.maxChangedConstF =
        std::max(constSet.maxChangedConstF, StartRegister + Count);
  } else if constexpr (ConstantType == D3D9ConstantType::Int &&
                       ProgramType == DxsoProgramType::VertexShader) {
    // We only track changed int constants for vertex shaders (and it's only
    // used when the device uses the SWVP UBO layout). Pixel shaders (and vertex
    // shaders on HWVP devices) always copy all int constants into the same UBO
    // as the float constants
    constSet.maxChangedConstI =
        std::max(constSet.maxChangedConstI, StartRegister + Count);
  } else if constexpr (ConstantType == D3D9ConstantType::Bool &&
                       ProgramType == DxsoProgramType::VertexShader) {
    // We only track changed bool constants for vertex shaders (and it's only
    // used when the device uses the SWVP UBO layout). Pixel shaders (and vertex
    // shaders on HWVP devices) always put all bool constants into a single spec
    // constant.
    constSet.maxChangedConstB =
        std::max(constSet.maxChangedConstB, StartRegister + Count);
  }

  if constexpr (ConstantType != D3D9ConstantType::Bool) {
    uint32_t maxCount = ConstantType == D3D9ConstantType::Float
                            ? constSet.meta.maxConstIndexF
                            : constSet.meta.maxConstIndexI;

    constSet.dirty |= StartRegister < maxCount;
  } else if constexpr (ProgramType == DxsoProgramType::VertexShader) {
    if (unlikely(CanSWVP())) {
      constSet.dirty |= StartRegister < constSet.meta.maxConstIndexB;
    }
  }

  UpdateStateConstants<ProgramType, ConstantType, T>(
      &m_state, StartRegister, pConstantData, Count,
      m_d3d9Options.d3d9FloatEmulation == D3D9FloatEmulation::Enabled);

  return D3D_OK;
}

D3D9FFShaderKeyVS
D3D9DeviceEx::BuildFFKeyVS(D3D9FF_VertexBlendMode vertexBlendMode,
                           bool indexedVertexBlend) const {
  D3D9FFShaderKeyVS key;
  key.Data.Contents.VertexHasPositionT =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
  key.Data.Contents.VertexHasColor0 =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor0);
  key.Data.Contents.VertexHasColor1 =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor1);
  key.Data.Contents.VertexHasPointSize =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPointSize);
  key.Data.Contents.VertexHasFog =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasFog);

  bool lighting = m_state.renderStates[D3DRS_LIGHTING] != 0 &&
                  !key.Data.Contents.VertexHasPositionT;
  bool colorVertex = m_state.renderStates[D3DRS_COLORVERTEX] != 0;
  uint32_t mask =
      (lighting && colorVertex)
          ? (key.Data.Contents.VertexHasColor0 ? D3DMCS_COLOR1
                                               : D3DMCS_MATERIAL) |
                (key.Data.Contents.VertexHasColor1 ? D3DMCS_COLOR2
                                                   : D3DMCS_MATERIAL)
          : 0;

  key.Data.Contents.UseLighting = lighting;
  key.Data.Contents.NormalizeNormals =
      m_state.renderStates[D3DRS_NORMALIZENORMALS];
  key.Data.Contents.LocalViewer =
      m_state.renderStates[D3DRS_LOCALVIEWER] && lighting;

  key.Data.Contents.RangeFog = m_state.renderStates[D3DRS_RANGEFOGENABLE];

  key.Data.Contents.DiffuseSource =
      m_state.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] & mask;
  key.Data.Contents.AmbientSource =
      m_state.renderStates[D3DRS_AMBIENTMATERIALSOURCE] & mask;
  key.Data.Contents.SpecularSource =
      m_state.renderStates[D3DRS_SPECULARMATERIALSOURCE] & mask;
  key.Data.Contents.EmissiveSource =
      m_state.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] & mask;

  key.Data.Contents.SpecularEnabled =
      m_state.renderStates[D3DRS_SPECULARENABLE];

  uint32_t lightCount = 0;

  if (key.Data.Contents.UseLighting) {
    for (uint32_t i = 0; i < caps::MaxEnabledLights; i++) {
      if (m_state.enabledLightIndices[i] !=
          std::numeric_limits<uint32_t>::max())
        lightCount++;
    }
  }

  key.Data.Contents.LightCount = lightCount;

  for (uint32_t i = 0; i < caps::MaxTextureBlendStages; i++) {
    uint32_t transformFlags =
        m_state.textureStages[i][DXVK_TSS_TEXTURETRANSFORMFLAGS] &
        ~(D3DTTFF_PROJECTED);
    uint32_t index = m_state.textureStages[i][DXVK_TSS_TEXCOORDINDEX];
    uint32_t indexFlags = (index & TCIMask) >> TCIOffset;

    transformFlags &= 0b111;
    index &= 0b111;

    key.Data.Contents.TransformFlags |= transformFlags << (i * 3);
    key.Data.Contents.TexcoordFlags |= indexFlags << (i * 3);
    key.Data.Contents.TexcoordIndices |= index << (i * 3);
  }

  key.Data.Contents.VertexTexcoordDeclMask =
      m_state.vertexDecl != nullptr ? m_state.vertexDecl->GetTexcoordMask() : 0;

  key.Data.Contents.VertexBlendMode = uint32_t(vertexBlendMode);

  if (vertexBlendMode == D3D9FF_VertexBlendMode_Normal) {
    key.Data.Contents.VertexBlendIndexed = indexedVertexBlend;
    key.Data.Contents.VertexBlendCount =
        m_state.renderStates[D3DRS_VERTEXBLEND] & 0xff;
  }

  key.Data.Contents.VertexClipping =
      m_state.renderStates[D3DRS_CLIPPLANEENABLE] != 0;

  return key;
}

void D3D9DeviceEx::UpdateFixedFunctionVS() {
  bool hasPositionT =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
  bool hasBlendWeight =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasBlendWeight);
  bool hasBlendIndices =
      m_state.vertexDecl != nullptr &&
      m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasBlendIndices);

  bool indexedVertexBlend =
      hasBlendIndices && m_state.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE];
  D3D9FF_VertexBlendMode vertexBlendMode = D3D9FF_VertexBlendMode_Disabled;

  if (m_state.renderStates[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE &&
      !hasPositionT) {
    vertexBlendMode = m_state.renderStates[D3DRS_VERTEXBLEND] == D3DVBF_TWEENING
                          ? D3D9FF_VertexBlendMode_Tween
                          : D3D9FF_VertexBlendMode_Normal;

    if (m_state.renderStates[D3DRS_VERTEXBLEND] != D3DVBF_0WEIGHTS) {
      if (!hasBlendWeight)
        vertexBlendMode = D3D9FF_VertexBlendMode_Disabled;
    } else if (!indexedVertexBlend)
      vertexBlendMode = D3D9FF_VertexBlendMode_Disabled;
  }

  // Shader...
  const bool useUbershader = m_d3d9Options.ffUbershaderVS;

  if (useUbershader && m_dirty.test(D3D9DeviceDirtyFlag::FFVertexShader)) {
    m_dirty.clr(D3D9DeviceDirtyFlag::FFVertexShader);
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
  } else if (m_dirty.test(D3D9DeviceDirtyFlag::FFVertexShader)) {
    m_dirty.clr(D3D9DeviceDirtyFlag::FFVertexShader);

    D3D9FFShaderKeyVS key = BuildFFKeyVS(vertexBlendMode, indexedVertexBlend);

    EmitCs([this, cKey = key, &cShaders = m_ffModules](DxvkContext *ctx) {
      auto shader = cShaders.GetShaderModule(this, cKey);
      ctx->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(shader.GetShader());
    });
  }

  // Viewport...
  if (hasPositionT && (m_dirty.test(D3D9DeviceDirtyFlag::FFViewport) ||
                       m_ffZTest != IsZTestEnabled())) {
    m_dirty.clr(D3D9DeviceDirtyFlag::FFViewport);
    m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

    const auto &vp = m_state.viewport;
    // For us to account for the Vulkan viewport rules
    // when translating Window Coords -> Real Coords:
    // We need to negate the inverse extent we multiply by,
    // this follows through to the offset when that gets
    // timesed by it.
    // The 1.0f additional offset however does not,
    // so we account for that there manually.

    m_ffZTest = IsZTestEnabled();

    m_viewportInfo.inverseExtent =
        Vector4(2.0f / float(vp.Width), -2.0f / float(vp.Height),
                m_ffZTest ? 1.0f : 0.0f, 1.0f);

    m_viewportInfo.inverseOffset =
        Vector4(-float(vp.X), -float(vp.Y), 0.0f, 0.0f);

    m_viewportInfo.inverseOffset =
        m_viewportInfo.inverseOffset * m_viewportInfo.inverseExtent;

    m_viewportInfo.inverseOffset =
        m_viewportInfo.inverseOffset + Vector4(-1.0f, 1.0f, 0.0f, 0.0f);
  }

  // Constants...
  if (m_dirty.test(D3D9DeviceDirtyFlag::FFVertexData)) {
    m_dirty.clr(D3D9DeviceDirtyFlag::FFVertexData);

    auto mapPtr = m_vsFixedFunction.AllocSlice();

    auto WorldView = m_state.transforms[GetTransformIndex(D3DTS_VIEW)] *
                     m_state.transforms[GetTransformIndex(D3DTS_WORLD)];
    auto NormalMatrix = inverse(WorldView);

    D3D9FixedFunctionVS *data = reinterpret_cast<D3D9FixedFunctionVS *>(mapPtr);
    data->WorldView = WorldView;
    data->NormalMatrix = NormalMatrix;
    data->InverseView =
        transpose(inverse(m_state.transforms[GetTransformIndex(D3DTS_VIEW)]));
    data->Projection = m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];

    for (uint32_t i = 0; i < data->TexcoordMatrices.size(); i++)
      data->TexcoordMatrices[i] =
          m_state.transforms[GetTransformIndex(D3DTS_TEXTURE0) + i];

    data->ViewportInfo = m_viewportInfo;

    DecodeD3DCOLOR(m_state.renderStates[D3DRS_AMBIENT],
                   data->GlobalAmbient.data);

    uint32_t lightIdx = 0;
    for (uint32_t i = 0; i < caps::MaxEnabledLights; i++) {
      auto idx = m_state.enabledLightIndices[i];
      if (idx == std::numeric_limits<uint32_t>::max())
        continue;

      data->Lights[lightIdx++] =
          D3D9Light(m_state.lights[idx].value(),
                    m_state.transforms[GetTransformIndex(D3DTS_VIEW)]);
    }

    data->Material = m_state.material;
    data->TweenFactor =
        bit::cast<float>(m_state.renderStates[D3DRS_TWEENFACTOR]);
    if (useUbershader) {
      data->Key = BuildFFKeyVS(vertexBlendMode, indexedVertexBlend).Data;
    }
  }

  if (m_dirty.test(D3D9DeviceDirtyFlag::FFVertexBlend) &&
      vertexBlendMode == D3D9FF_VertexBlendMode_Normal) {
    m_dirty.clr(D3D9DeviceDirtyFlag::FFVertexBlend);

    auto mapPtr = m_vsVertexBlend.AllocSlice();
    auto UploadVertexBlendData = [&](auto data) {
      for (uint32_t i = 0; i < std::size(data->WorldView); i++)
        data->WorldView[i] =
            m_state.transforms[GetTransformIndex(D3DTS_VIEW)] *
            m_state.transforms[GetTransformIndex(D3DTS_WORLDMATRIX(i))];
    };

    (m_isSWVP && indexedVertexBlend)
        ? UploadVertexBlendData(
              reinterpret_cast<D3D9FixedFunctionVertexBlendDataSW *>(mapPtr))
        : UploadVertexBlendData(
              reinterpret_cast<D3D9FixedFunctionVertexBlendDataHW *>(mapPtr));
  }
}

D3D9FFShaderKeyFS D3D9DeviceEx::BuildFFKeyFS() const {
  // Used args for a given operation.
  auto ArgsMask = [](DWORD Op) {
    switch (Op) {
    case D3DTOP_DISABLE:
      return 0b000u; // No Args
    case D3DTOP_SELECTARG1:
    case D3DTOP_PREMODULATE:
      return 0b010u; // Arg 1
    case D3DTOP_SELECTARG2:
      return 0b100u; // Arg 2
    case D3DTOP_MULTIPLYADD:
    case D3DTOP_LERP:
      return 0b111u; // Arg 0, 1, 2
    default:
      return 0b110u; // Arg 1, 2
    }
  };

  D3D9FFShaderKeyFS key;

  uint32_t activeTextureStageCount = 0;
  for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
    auto &stage = key.Stages[i].Contents;
    auto &data = m_state.textureStages[i];

    // Subsequent stages do not occur if this is true.
    if (data[DXVK_TSS_COLOROP] == D3DTOP_DISABLE)
      break;

    // If the stage is invalid (ie. no texture bound),
    // this and all subsequent stages get disabled.
    if (m_state.textures[i] == nullptr) {
      if (((data[DXVK_TSS_COLORARG0] & D3DTA_SELECTMASK) == D3DTA_TEXTURE &&
           (ArgsMask(data[DXVK_TSS_COLOROP]) & (1 << 0u))) ||
          ((data[DXVK_TSS_COLORARG1] & D3DTA_SELECTMASK) == D3DTA_TEXTURE &&
           (ArgsMask(data[DXVK_TSS_COLOROP]) & (1 << 1u))) ||
          ((data[DXVK_TSS_COLORARG2] & D3DTA_SELECTMASK) == D3DTA_TEXTURE &&
           (ArgsMask(data[DXVK_TSS_COLOROP]) & (1 << 2u))))
        break;
    }

    stage.ColorOp = data[DXVK_TSS_COLOROP];
    stage.AlphaOp = data[DXVK_TSS_ALPHAOP];

    stage.ColorArg0 = data[DXVK_TSS_COLORARG0];
    stage.ColorArg1 = data[DXVK_TSS_COLORARG1];
    stage.ColorArg2 = data[DXVK_TSS_COLORARG2];

    stage.AlphaArg0 = data[DXVK_TSS_ALPHAARG0];
    stage.AlphaArg1 = data[DXVK_TSS_ALPHAARG1];
    stage.AlphaArg2 = data[DXVK_TSS_ALPHAARG2];

    stage.ResultIsTemp = data[DXVK_TSS_RESULTARG] == D3DTA_TEMP;

    activeTextureStageCount = i + 1;
  }

  auto &stage0 = key.Stages[0].Contents;

  if (stage0.ResultIsTemp && stage0.ColorOp != D3DTOP_DISABLE &&
      stage0.AlphaOp == D3DTOP_DISABLE) {
    stage0.AlphaOp = D3DTOP_SELECTARG1;
    stage0.AlphaArg1 = D3DTA_DIFFUSE;
  }

  stage0.GlobalSpecularEnable = m_state.renderStates[D3DRS_SPECULARENABLE];

  // The last stage *always* writes to current.
  if (activeTextureStageCount >= 1)
    key.Stages[activeTextureStageCount - 1].Contents.ResultIsTemp = false;

  return key;
}

void D3D9DeviceEx::UpdateFixedFunctionPS() {
  if (unlikely(!m_dirty.test(D3D9DeviceDirtyFlag::FFPixelShader) &&
               !m_dirty.test(D3D9DeviceDirtyFlag::FFPixelData)))
    return;

  // Shader...
  const bool useUbershader = m_d3d9Options.ffUbershaderFS;

  D3D9FFShaderKeyFS key = BuildFFKeyFS();
  if (useUbershader && m_dirty.test(D3D9DeviceDirtyFlag::FFPixelShader)) {
    // The flags are set based on the specialized shaders.
    m_dirty.clr(D3D9DeviceDirtyFlag::FFPixelShader);
    m_dirty.set(D3D9DeviceDirtyFlag::FFPixelData);

    // Spec constants...
    uint32_t activeTextureStageCount;
    for (activeTextureStageCount = 0;
         activeTextureStageCount < caps::TextureStageCount;
         activeTextureStageCount++) {
      auto &stage = key.Stages[activeTextureStageCount].Contents;
      if (stage.ColorOp == D3DTOP_DISABLE)
        break;
    }

    const auto repackArg = [](uint32_t arg) {
      return (arg & 0b111u) | ((arg & 0b110000u) >> 1u);
    };

    uint32_t lastActiveTextureStage = std::max(activeTextureStageCount, 1u) -
                                      1u; // Subtract 1 to make it fit 3 bits
    bool dirty =
        m_specInfo.set<D3D9SpecConstantId::SpecFFLastActiveTextureStage>(
            lastActiveTextureStage);
    dirty |= m_specInfo.set<D3D9SpecConstantId::SpecFFGlobalSpecularEnabled>(
        m_state.renderStates[D3DRS_SPECULARENABLE]);
    constexpr uint32_t perTextureStageSpecConsts =
        static_cast<uint32_t>(D3D9SpecConstantId::SpecFFTextureStage1ColorOp) -
        static_cast<uint32_t>(D3D9SpecConstantId::SpecFFTextureStage0ColorOp);
    for (uint32_t i = 0; i < 4; i++) {
      if (i <= activeTextureStageCount) {
        dirty |=
            m_specInfo.set(static_cast<D3D9SpecConstantId>(
                               D3D9SpecConstantId::SpecFFTextureStage0ColorOp +
                               perTextureStageSpecConsts * i),
                           key.Stages[i].Contents.ColorOp);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ColorArg1 +
                perTextureStageSpecConsts * i),
            repackArg(key.Stages[i].Contents.ColorArg1));
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ColorArg2 +
                perTextureStageSpecConsts * i),
            repackArg(key.Stages[i].Contents.ColorArg2));
        dirty |=
            m_specInfo.set(static_cast<D3D9SpecConstantId>(
                               D3D9SpecConstantId::SpecFFTextureStage0AlphaOp +
                               perTextureStageSpecConsts * i),
                           key.Stages[i].Contents.AlphaOp);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0AlphaArg1 +
                perTextureStageSpecConsts * i),
            repackArg(key.Stages[i].Contents.AlphaArg1));
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0AlphaArg2 +
                perTextureStageSpecConsts * i),
            repackArg(key.Stages[i].Contents.AlphaArg2));
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ResultIsTemp +
                perTextureStageSpecConsts * i),
            key.Stages[i].Contents.ResultIsTemp);
      } else {
        dirty |=
            m_specInfo.set(static_cast<D3D9SpecConstantId>(
                               D3D9SpecConstantId::SpecFFTextureStage0ColorOp +
                               perTextureStageSpecConsts * i),
                           0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ColorArg1 +
                perTextureStageSpecConsts * i),
            0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ColorArg2 +
                perTextureStageSpecConsts * i),
            0);
        dirty |=
            m_specInfo.set(static_cast<D3D9SpecConstantId>(
                               D3D9SpecConstantId::SpecFFTextureStage0AlphaOp +
                               perTextureStageSpecConsts * i),
                           0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0AlphaArg1 +
                perTextureStageSpecConsts * i),
            0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0AlphaArg2 +
                perTextureStageSpecConsts * i),
            0);
        dirty |= m_specInfo.set(
            static_cast<D3D9SpecConstantId>(
                D3D9SpecConstantId::SpecFFTextureStage0ResultIsTemp +
                perTextureStageSpecConsts * i),
            0);
      }
    }
    if (dirty) {
      m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
    }
  } else if (m_dirty.test(D3D9DeviceDirtyFlag::FFPixelShader)) {
    m_dirty.clr(D3D9DeviceDirtyFlag::FFPixelShader);

    EmitCs([this, cKey = key, &cShaders = m_ffModules](DxvkContext *ctx) {
      auto shader = cShaders.GetShaderModule(this, cKey);
      ctx->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(shader.GetShader());
    });
  }

  // Constants...
  if (m_dirty.test(D3D9DeviceDirtyFlag::FFPixelData)) {
    m_dirty.clr(D3D9DeviceDirtyFlag::FFPixelData);

    auto mapPtr = m_psFixedFunction.AllocSlice();
    auto &rs = m_state.renderStates;

    D3D9FixedFunctionPS *data = reinterpret_cast<D3D9FixedFunctionPS *>(mapPtr);
    DecodeD3DCOLOR((D3DCOLOR)rs[D3DRS_TEXTUREFACTOR], data->textureFactor.data);
    if (useUbershader) {
      data->Key = key;
    }
  }
}

bool D3D9DeviceEx::UseProgrammableVS() {
  return m_state.vertexShader != nullptr && m_state.vertexDecl != nullptr &&
         !m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
}

bool D3D9DeviceEx::UseProgrammablePS() {
  return m_state.pixelShader != nullptr;
}

void D3D9DeviceEx::ApplyPrimitiveType(DxvkContext *pContext,
                                      D3DPRIMITIVETYPE PrimType) {
  if (m_iaState.primitiveType != PrimType) {
    m_iaState.primitiveType = PrimType;

    auto iaState = DecodeInputAssemblyState(PrimType);
    pContext->setInputAssemblyState(iaState);
  }
}

void D3D9DeviceEx::ResolveZ() {
  D3D9Surface *src = m_state.depthStencil.ptr();
  IDirect3DBaseTexture9 *dst = m_state.textures[0];

  if (unlikely(!src || !dst))
    return;

  D3D9CommonTexture *srcTextureInfo = GetCommonTexture(src);
  D3D9CommonTexture *dstTextureInfo = GetCommonTexture(dst);

  const D3D9_COMMON_TEXTURE_DESC *srcDesc = srcTextureInfo->Desc();
  const D3D9_COMMON_TEXTURE_DESC *dstDesc = dstTextureInfo->Desc();

  VkSampleCountFlagBits dstSampleCount;
  DecodeMultiSampleType(m_dxvkDevice, dstDesc->MultiSample,
                        dstDesc->MultisampleQuality, &dstSampleCount);

  if (unlikely(dstSampleCount != VK_SAMPLE_COUNT_1_BIT)) {
    Logger::warn("D3D9DeviceEx::ResolveZ: dstSampleCount != 1. Discarding.");
    return;
  }

  const D3D9_VK_FORMAT_MAPPING srcFormatInfo = LookupFormat(srcDesc->Format);
  const D3D9_VK_FORMAT_MAPPING dstFormatInfo = LookupFormat(dstDesc->Format);

  VkImageSubresource dstSubresource =
      dstTextureInfo->GetSubresourceFromIndex(dstFormatInfo.Aspect, 0);

  VkImageSubresource srcSubresource = srcTextureInfo->GetSubresourceFromIndex(
      srcFormatInfo.Aspect, src->GetSubresource());

  if ((dstSubresource.aspectMask & srcSubresource.aspectMask) != 0) {
    // for depthStencil -> depth or depthStencil -> stencil copies, only copy
    // the aspect that both images support
    dstSubresource.aspectMask =
        dstSubresource.aspectMask & srcSubresource.aspectMask;
    srcSubresource.aspectMask =
        dstSubresource.aspectMask & srcSubresource.aspectMask;
  } else if (unlikely(dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT &&
                      srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)) {
    Logger::err(str::format("D3D9DeviceEx::ResolveZ: Trying to blit from ",
                            srcFormatInfo.FormatColor, " (aspect ",
                            srcSubresource.aspectMask, ")", " to ",
                            dstFormatInfo.FormatColor, " (aspect ",
                            dstSubresource.aspectMask, ")"));
    return;
  }

  const VkImageSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspectMask, dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1};

  const VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask, srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1};

  VkSampleCountFlagBits srcSampleCount;
  DecodeMultiSampleType(m_dxvkDevice, srcDesc->MultiSample,
                        srcDesc->MultisampleQuality, &srcSampleCount);

  if (srcSampleCount == VK_SAMPLE_COUNT_1_BIT) {
    EmitCs([cDstImage = dstTextureInfo->GetImage(),
            cSrcImage = srcTextureInfo->GetImage(),
            cDstLayers = dstSubresourceLayers,
            cSrcLayers = srcSubresourceLayers](DxvkContext *ctx) {
      ctx->copyImage(cDstImage, cDstLayers, VkOffset3D{0, 0, 0}, cSrcImage,
                     cSrcLayers, VkOffset3D{0, 0, 0},
                     cDstImage->mipLevelExtent(cDstLayers.mipLevel));
    });
  } else {
    EmitCs([cDstImage = dstTextureInfo->GetImage(),
            cSrcImage = srcTextureInfo->GetImage(),
            cDstSubres = dstSubresourceLayers,
            cSrcSubres = srcSubresourceLayers](DxvkContext *ctx) {
      // We should resolve using the first sample according to
      // http://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2012/10/Advanced-DX9-Capabilities-for-ATI-Radeon-Cards_v2.pdf
      // "The resolve operation copies the depth value from the *first sample
      // only* into the resolved depth stencil texture."
      VkImageResolve region;
      region.srcSubresource = cSrcSubres;
      region.srcOffset = VkOffset3D{0, 0, 0};
      region.dstSubresource = cDstSubres;
      region.dstOffset = VkOffset3D{0, 0, 0};
      region.extent = cDstImage->mipLevelExtent(cDstSubres.mipLevel);

      ctx->resolveImage(cDstImage, cSrcImage, region, cSrcImage->info().format,
                        VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
                        VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
    });
  }

  dstTextureInfo->MarkAllNeedReadback();
}

void D3D9DeviceEx::TransitionImage(D3D9CommonTexture *pResource,
                                   VkImageLayout NewLayout) {
  EmitCs([cImage = pResource->GetImage(), cNewLayout = NewLayout](
             DxvkContext *ctx) { ctx->changeImageLayout(cImage, cNewLayout); });
}

void D3D9DeviceEx::TransformImage(D3D9CommonTexture *pResource,
                                  const VkImageSubresourceRange *pSubresources,
                                  VkImageLayout OldLayout,
                                  VkImageLayout NewLayout) {
  EmitCs([cImage = pResource->GetImage(), cSubresources = *pSubresources,
          cOldLayout = OldLayout, cNewLayout = NewLayout](DxvkContext *ctx) {
    ctx->transformImage(cImage, cSubresources, cOldLayout, cNewLayout);
  });
}

void D3D9DeviceEx::ResetState(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  SetDepthStencilSurface(nullptr);

  for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++)
    SetRenderTargetInternal(i, nullptr);

  auto &rs = m_state.renderStates;

  rs[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
  rs[D3DRS_ALPHABLENDENABLE] = FALSE;
  rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  rs[D3DRS_COLORWRITEENABLE] = 0x0000000f;
  rs[D3DRS_COLORWRITEENABLE1] = 0x0000000f;
  rs[D3DRS_COLORWRITEENABLE2] = 0x0000000f;
  rs[D3DRS_COLORWRITEENABLE3] = 0x0000000f;
  rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  BindBlendState();

  rs[D3DRS_BLENDFACTOR] = 0xffffffff;
  BindBlendFactor();

  rs[D3DRS_ZENABLE] = pPresentationParameters->EnableAutoDepthStencil
                          ? D3DZB_TRUE
                          : D3DZB_FALSE;
  rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  rs[D3DRS_TWOSIDEDSTENCILMODE] = FALSE;
  rs[D3DRS_ZWRITEENABLE] = TRUE;
  rs[D3DRS_STENCILENABLE] = FALSE;
  rs[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
  rs[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_STENCILMASK] = 0xFFFFFFFF;
  rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
  BindDepthStencilState();

  rs[D3DRS_STENCILREF] = 0;
  BindDepthStencilReference();

  rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
  rs[D3DRS_CULLMODE] = D3DCULL_CCW;
  rs[D3DRS_DEPTHBIAS] = bit::cast<DWORD>(0.0f);
  rs[D3DRS_SLOPESCALEDEPTHBIAS] = bit::cast<DWORD>(0.0f);
  BindRasterizerState();
  BindDepthBias();

  rs[D3DRS_SCISSORTESTENABLE] = FALSE;

  rs[D3DRS_ALPHATESTENABLE] = FALSE;
  rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  BindAlphaTestState();
  rs[D3DRS_ALPHAREF] = 0;
  UpdatePushConstant<D3D9RenderStateItem::AlphaRef>();

  rs[D3DRS_MULTISAMPLEMASK] = 0xffffffff;
  BindMultiSampleState();

  rs[D3DRS_TEXTUREFACTOR] = 0xffffffff;
  m_dirty.set(D3D9DeviceDirtyFlag::FFPixelData);

  rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  rs[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  rs[D3DRS_LIGHTING] = TRUE;
  rs[D3DRS_COLORVERTEX] = TRUE;
  rs[D3DRS_LOCALVIEWER] = TRUE;
  rs[D3DRS_RANGEFOGENABLE] = FALSE;
  rs[D3DRS_NORMALIZENORMALS] = FALSE;
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);

  // PS
  rs[D3DRS_SPECULARENABLE] = FALSE;

  rs[D3DRS_AMBIENT] = 0;
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);

  rs[D3DRS_FOGENABLE] = FALSE;
  rs[D3DRS_FOGCOLOR] = 0;
  rs[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
  rs[D3DRS_FOGSTART] = bit::cast<DWORD>(0.0f);
  rs[D3DRS_FOGEND] = bit::cast<DWORD>(1.0f);
  rs[D3DRS_FOGDENSITY] = bit::cast<DWORD>(1.0f);
  rs[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
  m_dirty.set(D3D9DeviceDirtyFlag::FogColor);
  m_dirty.set(D3D9DeviceDirtyFlag::FogDensity);
  m_dirty.set(D3D9DeviceDirtyFlag::FogEnd);
  m_dirty.set(D3D9DeviceDirtyFlag::FogScale);
  m_dirty.set(D3D9DeviceDirtyFlag::FogState);

  rs[D3DRS_CLIPPLANEENABLE] = 0;
  m_dirty.set(D3D9DeviceDirtyFlag::ClipPlanes);

  const auto &limits =
      m_dxvkDevice->adapter()->deviceProperties().core.properties.limits;

  rs[D3DRS_POINTSPRITEENABLE] = FALSE;
  rs[D3DRS_POINTSCALEENABLE] = FALSE;
  rs[D3DRS_POINTSCALE_A] = bit::cast<DWORD>(1.0f);
  rs[D3DRS_POINTSCALE_B] = bit::cast<DWORD>(0.0f);
  rs[D3DRS_POINTSCALE_C] = bit::cast<DWORD>(0.0f);
  rs[D3DRS_POINTSIZE] = bit::cast<DWORD>(1.0f);
  rs[D3DRS_POINTSIZE_MIN] =
      m_isD3D8Compatible ? bit::cast<DWORD>(0.0f) : bit::cast<DWORD>(1.0f);
  rs[D3DRS_POINTSIZE_MAX] = bit::cast<DWORD>(limits.pointSizeRange[1]);
  UpdatePushConstant<D3D9RenderStateItem::PointSize>();
  UpdatePushConstant<D3D9RenderStateItem::PointSizeMin>();
  UpdatePushConstant<D3D9RenderStateItem::PointSizeMax>();
  m_dirty.set(D3D9DeviceDirtyFlag::PointScale);
  UpdatePointMode(false);

  rs[D3DRS_SRGBWRITEENABLE] = 0;

  rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;

  rs[D3DRS_VERTEXBLEND] = D3DVBF_DISABLE;
  rs[D3DRS_INDEXEDVERTEXBLENDENABLE] = FALSE;
  rs[D3DRS_TWEENFACTOR] = bit::cast<DWORD>(0.0f);
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexBlend);

  // Render States not implemented beyond this point.
  rs[D3DRS_LASTPIXEL] = TRUE;
  rs[D3DRS_DITHERENABLE] = FALSE;
  rs[D3DRS_WRAP0] = 0;
  rs[D3DRS_WRAP1] = 0;
  rs[D3DRS_WRAP2] = 0;
  rs[D3DRS_WRAP3] = 0;
  rs[D3DRS_WRAP4] = 0;
  rs[D3DRS_WRAP5] = 0;
  rs[D3DRS_WRAP6] = 0;
  rs[D3DRS_WRAP7] = 0;
  rs[D3DRS_CLIPPING] = TRUE;
  rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
  rs[D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE;
  rs[D3DRS_DEBUGMONITORTOKEN] = D3DDMT_ENABLE;
  rs[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
  rs[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
  rs[D3DRS_ANTIALIASEDLINEENABLE] = FALSE;
  rs[D3DRS_MINTESSELLATIONLEVEL] = bit::cast<DWORD>(1.0f);
  rs[D3DRS_MAXTESSELLATIONLEVEL] = bit::cast<DWORD>(1.0f);
  rs[D3DRS_ADAPTIVETESS_X] = bit::cast<DWORD>(0.0f);
  rs[D3DRS_ADAPTIVETESS_Y] = bit::cast<DWORD>(0.0f);
  rs[D3DRS_ADAPTIVETESS_Z] = bit::cast<DWORD>(1.0f);
  rs[D3DRS_ADAPTIVETESS_W] = bit::cast<DWORD>(0.0f);
  rs[D3DRS_ENABLEADAPTIVETESSELLATION] = FALSE;
  rs[D3DRS_WRAP8] = 0;
  rs[D3DRS_WRAP9] = 0;
  rs[D3DRS_WRAP10] = 0;
  rs[D3DRS_WRAP11] = 0;
  rs[D3DRS_WRAP12] = 0;
  rs[D3DRS_WRAP13] = 0;
  rs[D3DRS_WRAP14] = 0;
  rs[D3DRS_WRAP15] = 0;
  // End Unimplemented Render States

  for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
    auto &stage = m_state.textureStages[i];

    stage[DXVK_TSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    stage[DXVK_TSS_COLORARG1] = D3DTA_TEXTURE;
    stage[DXVK_TSS_COLORARG2] = D3DTA_CURRENT;
    stage[DXVK_TSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    stage[DXVK_TSS_ALPHAARG1] = D3DTA_TEXTURE;
    stage[DXVK_TSS_ALPHAARG2] = D3DTA_CURRENT;
    stage[DXVK_TSS_BUMPENVMAT00] = bit::cast<DWORD>(0.0f);
    stage[DXVK_TSS_BUMPENVMAT01] = bit::cast<DWORD>(0.0f);
    stage[DXVK_TSS_BUMPENVMAT10] = bit::cast<DWORD>(0.0f);
    stage[DXVK_TSS_BUMPENVMAT11] = bit::cast<DWORD>(0.0f);
    stage[DXVK_TSS_TEXCOORDINDEX] = i;
    stage[DXVK_TSS_BUMPENVLSCALE] = bit::cast<DWORD>(0.0f);
    stage[DXVK_TSS_BUMPENVLOFFSET] = bit::cast<DWORD>(0.0f);
    stage[DXVK_TSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
    stage[DXVK_TSS_COLORARG0] = D3DTA_CURRENT;
    stage[DXVK_TSS_ALPHAARG0] = D3DTA_CURRENT;
    stage[DXVK_TSS_RESULTARG] = D3DTA_CURRENT;
    stage[DXVK_TSS_CONSTANT] = 0x00000000;
  }
  m_dirty.set(D3D9DeviceDirtyFlag::SharedPixelShaderData);
  m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);

  for (uint32_t i = 0; i < caps::MaxStreams; i++)
    m_state.streamFreq[i] = 1;

  for (uint32_t i = 0; i < m_state.textures->size(); i++) {
    SetStateTexture(i, nullptr);
  }

  EmitCs([cSize = m_state.textures->size()](DxvkContext *ctx) {
    VkShaderStageFlags stage =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    for (uint32_t i = 0; i < cSize; i++) {
      auto samplerInfo = RemapStateSamplerShader(DWORD(i));
      uint32_t slot =
          computeResourceSlotId(samplerInfo.first, DxsoBindingType::Image,
                                uint32_t(samplerInfo.second));
      ctx->bindResourceImageView(stage, slot, nullptr);
    }
  });

  m_textureSlotTracking.textureDirty = 0;
  m_textureSlotTracking.depth = 0;

  auto &ss = m_state.samplerStates.get();
  for (uint32_t i = 0; i < ss.size(); i++) {
    auto &state = ss[i];
    state[D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
    state[D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
    state[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    state[D3DSAMP_BORDERCOLOR] = 0x00000000;
    state[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
    state[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    state[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    state[D3DSAMP_MIPMAPLODBIAS] = bit::cast<DWORD>(0.0f);
    state[D3DSAMP_MAXMIPLEVEL] = 0;
    state[D3DSAMP_MAXANISOTROPY] = 1;
    state[D3DSAMP_SRGBTEXTURE] = 0;
    state[D3DSAMP_ELEMENTINDEX] = 0;
    state[D3DSAMP_DMAPOFFSET] = 0;

    BindSampler(i);
  }

  m_textureSlotTracking.samplerStateDirty = 0;

  for (uint32_t i = 0; i < caps::MaxClipPlanes; i++) {
    float plane[4] = {0, 0, 0, 0};
    SetClipPlane(i, plane);
  }

  // We should do this...
  m_dirty.set(D3D9DeviceDirtyFlag::InputLayout);

  UpdatePixelShaderSamplerSpec(0u, 0u);
  UpdateVertexBoolSpec(0u);
  UpdatePixelBoolSpec(0u);
  UpdateCommonSamplerSpec(0u, 0u, 0u, 0u);

  UpdateAnyColorWrites<0>();
  UpdateAnyColorWrites<1>();
  UpdateAnyColorWrites<2>();
  UpdateAnyColorWrites<3>();

  SetIndices(nullptr);
  for (uint32_t i = 0; i < caps::MaxStreams; i++) {
    SetStreamSource(i, nullptr, 0, 0);
  }

  // In D3D8, this represents the value of D3DRS_PATCHSEGMENTS.
  // It defaults to 1.0f and is reset as any other render state.
  if (m_isD3D8Compatible)
    m_state.nPatchSegments = 1.0f;

  m_alphaTestEnabled = false;
  m_atocEnabled = false;
  m_nvdbEnabled = false;
}

HRESULT
D3D9DeviceEx::ResetSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters,
                             D3DDISPLAYMODEEX *pFullscreenDisplayMode) {
  D3D9Format backBufferFmt =
      EnumerateFormat(pPresentationParameters->BackBufferFormat);
  bool unlockedFormats = m_parent->HasFormatsUnlocked();

  Logger::info(str::format(
      "D3D9DeviceEx::ResetSwapChain:\n",
      "  Requested Presentation Parameters\n", "    - Width:              ",
      pPresentationParameters->BackBufferWidth, "\n",
      "    - Height:             ", pPresentationParameters->BackBufferHeight,
      "\n", "    - Format:             ", backBufferFmt,
      "\n"
      "    - Auto Depth Stencil: ",
      pPresentationParameters->EnableAutoDepthStencil ? "true" : "false", "\n",
      "                ^ Format: ",
      EnumerateFormat(pPresentationParameters->AutoDepthStencilFormat), "\n",
      "    - Windowed:           ",
      pPresentationParameters->Windowed ? "true" : "false", "\n",
      "    - Swap effect:        ", pPresentationParameters->SwapEffect, "\n"));

  // Black Desert creates a D3DDEVTYPE_NULLREF device and
  // expects this validation to not prevent a swapchain reset.
  if (likely(m_deviceType != D3DDEVTYPE_NULLREF) &&
      unlikely(!pPresentationParameters->Windowed &&
               (pPresentationParameters->BackBufferWidth == 0 ||
                pPresentationParameters->BackBufferHeight == 0))) {
    return D3DERR_INVALIDCALL;
  }

  if (backBufferFmt != D3D9Format::Unknown && !unlockedFormats) {
    if (!IsSupportedBackBufferFormat(backBufferFmt)) {
      Logger::err(str::format(
          "D3D9DeviceEx::ResetSwapChain: Unsupported backbuffer format: ",
          EnumerateFormat(pPresentationParameters->BackBufferFormat)));
      return D3DERR_INVALIDCALL;
    }
  }

  if (m_implicitSwapchain != nullptr) {
    HRESULT hr = m_implicitSwapchain->Reset(pPresentationParameters,
                                            pFullscreenDisplayMode);
    if (FAILED(hr))
      return hr;
  } else {
    m_implicitSwapchain = new D3D9SwapChainEx(this, pPresentationParameters,
                                              pFullscreenDisplayMode, true);
    m_mostRecentlyUsedSwapchain = m_implicitSwapchain.ptr();
  }

  if (pPresentationParameters->EnableAutoDepthStencil) {
    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width = pPresentationParameters->BackBufferWidth;
    desc.Height = pPresentationParameters->BackBufferHeight;
    desc.Depth = 1;
    desc.ArraySize = 1;
    desc.MipLevels = 1;
    desc.Usage = D3DUSAGE_DEPTHSTENCIL;
    desc.Format =
        EnumerateFormat(pPresentationParameters->AutoDepthStencilFormat);
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Discard = (pPresentationParameters->Flags &
                    D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL) != 0;
    desc.MultiSample = pPresentationParameters->MultiSampleType;
    desc.MultisampleQuality = pPresentationParameters->MultiSampleQuality;
    desc.IsBackBuffer = FALSE;
    desc.IsAttachmentOnly = TRUE;
    desc.IsLockable = IsLockableDepthStencilFormat(desc.Format);

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(
            this, D3DRTYPE_SURFACE, &desc)))
      return D3DERR_NOTAVAILABLE;

    m_autoDepthStencil =
        new D3D9Surface(this, &desc, IsExtended(), nullptr, nullptr);
    m_initializer->InitTexture(m_autoDepthStencil->GetCommonTexture());
    SetDepthStencilSurface(m_autoDepthStencil.ptr());
    m_losableResourceCounter++;
  }

  if (!IsExtended()) {
    SetRenderTarget(0, m_implicitSwapchain->GetBackBuffer(0));
  } else {
    // Extended devices will not reset the MinZ/MaxZ viewport values
    const float MinZ = m_state.viewport.MinZ;
    const float MaxZ = m_state.viewport.MaxZ;

    SetRenderTarget(0, m_implicitSwapchain->GetBackBuffer(0));

    // Previous MinZ/MaxZ values (saved above) need to be restored
    m_state.viewport.MinZ = MinZ;
    m_state.viewport.MaxZ = MaxZ;
  }

  // Force this if we end up binding the same RT to make scissor change go into
  // effect.
  BindViewportAndScissor();

  if (m_war3PostProcess && m_implicitSwapchain != nullptr) {
    Com<IDirect3DSurface9> backbuffer = m_implicitSwapchain->GetBackBuffer(0);
    if (backbuffer != nullptr) {
      D3DSURFACE_DESC desc = {};
      if (SUCCEEDED(backbuffer->GetDesc(&desc))) {
        m_war3PostProcess->OnResize(desc.Width, desc.Height, desc.Format);
      }
    }
  }

  return D3D_OK;
}

HRESULT
D3D9DeviceEx::InitialReset(D3DPRESENT_PARAMETERS *pPresentationParameters,
                           D3DDISPLAYMODEEX *pFullscreenDisplayMode) {
  // [War3] FPS 解锁时强制禁用 VSync
  War3ForceImmediatePresent(pPresentationParameters);

  ResetState(pPresentationParameters);

  HRESULT hr = ResetSwapChain(pPresentationParameters, pFullscreenDisplayMode);
  if (FAILED(hr))
    return hr;

  Flush();
  SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

  return D3D_OK;
}

void D3D9DeviceEx::TrackBufferMappingBufferSequenceNumber(
    D3D9CommonBuffer *pResource) {
  uint64_t sequenceNumber = GetCurrentSequenceNumber();
  pResource->TrackMappingBufferSequenceNumber(sequenceNumber);
}

void D3D9DeviceEx::TrackTextureMappingBufferSequenceNumber(
    D3D9CommonTexture *pResource, UINT Subresource) {
  uint64_t sequenceNumber = GetCurrentSequenceNumber();
  pResource->TrackMappingBufferSequenceNumber(Subresource, sequenceNumber);
}

uint64_t D3D9DeviceEx::GetCurrentSequenceNumber() {
  // We do not flush empty chunks, so if we are tracking a resource
  // immediately after a flush, we need to use the sequence number
  // of the previously submitted chunk to prevent deadlocks.
  return m_csChunk->empty() ? m_csSeqNum : m_csSeqNum + 1;
}

void *D3D9DeviceEx::MapTexture(D3D9CommonTexture *pTexture, UINT Subresource) {
  // Will only be called inside the device lock
  void *ptr = pTexture->GetData(Subresource);

#ifdef D3D9_ALLOW_UNMAPPING
  if (likely(pTexture->GetMapMode() ==
             D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE)) {
    m_mappedTextures.insert(pTexture);
  }
#endif

  return ptr;
}

void D3D9DeviceEx::TouchMappedTexture(D3D9CommonTexture *pTexture) {
#ifdef D3D9_ALLOW_UNMAPPING
  if (pTexture->GetMapMode() != D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE)
    return;

  D3D9DeviceLock lock = LockDevice();
  m_mappedTextures.touch(pTexture);
#endif
}

void D3D9DeviceEx::RemoveMappedTexture(D3D9CommonTexture *pTexture) {
#ifdef D3D9_ALLOW_UNMAPPING
  if (pTexture->GetMapMode() != D3D9_COMMON_TEXTURE_MAP_MODE_UNMAPPABLE)
    return;

  D3D9DeviceLock lock = LockDevice();
  m_mappedTextures.remove(pTexture);
#endif
}

void D3D9DeviceEx::UnmapTextures() {
  // Will only be called inside the device lock

#ifdef D3D9_ALLOW_UNMAPPING
  uint32_t mappedMemory = m_memoryAllocator.MappedMemory();
  if (likely(mappedMemory < uint32_t(m_d3d9Options.textureMemory)))
    return;

  uint32_t threshold = (m_d3d9Options.textureMemory / 4) * 3;

  auto iter = m_mappedTextures.leastRecentlyUsedIter();
  while (m_memoryAllocator.MappedMemory() >= threshold &&
         iter != m_mappedTextures.leastRecentlyUsedEndIter()) {
    if (unlikely((*iter)->IsAnySubresourceLocked() != 0)) {
      iter++;
      continue;
    }
    (*iter)->UnmapData();

    iter = m_mappedTextures.remove(iter);
  }
#endif
}

////////////////////////////////////
// D3D9 Device Lost
////////////////////////////////////

void D3D9DeviceEx::NotifyFullscreen(HWND window, bool fullscreen) {
  D3D9DeviceLock lock = LockDevice();

  if (fullscreen) {
    if (unlikely(window != m_fullscreenWindow && m_fullscreenWindow != NULL)) {
      Logger::warn("Multiple fullscreen windows detected.");
    }
    m_fullscreenWindow = window;
  } else {
    if (unlikely(m_fullscreenWindow != window)) {
      Logger::warn("Window was not fullscreen in the first place.");
    } else {
      m_fullscreenWindow = 0;
    }
  }
}

void D3D9DeviceEx::NotifyWindowActivated(HWND window, bool activated) {
  D3D9DeviceLock lock = LockDevice();

  if (likely(!m_d3d9Options.deviceLossOnFocusLoss || IsExtended()))
    return;

  if (activated && m_deviceLostState == D3D9DeviceLostState::Lost) {
    Logger::info("Device not reset");
    m_deviceLostState = D3D9DeviceLostState::NotReset;
  } else if (!activated && m_deviceLostState != D3D9DeviceLostState::Lost &&
             m_fullscreenWindow == window) {
    Logger::info("Device lost");
    m_deviceLostState = D3D9DeviceLostState::Lost;
    m_fullscreenWindow = NULL;
  }
}

////////////////////////////////////
// D3D9 Device Specialization State
////////////////////////////////////

void D3D9DeviceEx::UpdateAlphaTestSpec(VkCompareOp alphaOp,
                                       uint32_t precision) {
  bool dirty = m_specInfo.set<SpecAlphaCompareOp>(uint32_t(alphaOp));
  dirty |= m_specInfo.set<SpecAlphaPrecisionBits>(precision);

  if (dirty)
    m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
}

void D3D9DeviceEx::UpdateVertexBoolSpec(uint32_t value) {
  if (m_specInfo.set<SpecVertexShaderBools>(value))
    m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
}

void D3D9DeviceEx::UpdatePixelBoolSpec(uint32_t value) {
  if (m_specInfo.set<SpecPixelShaderBools>(value))
    m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
}

void D3D9DeviceEx::UpdatePixelShaderSamplerSpec(uint32_t types,
                                                uint32_t fetch4) {
  bool dirty = m_specInfo.set<SpecSamplerType>(types);
  dirty |= m_specInfo.set<SpecSamplerFetch4>(fetch4);

  if (dirty)
    m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
}

void D3D9DeviceEx::UpdateCommonSamplerSpec(uint32_t nullMask,
                                           uint32_t depthMask,
                                           uint32_t drefMask,
                                           uint32_t projections) {
  bool dirty = m_specInfo.set<SpecSamplerDepthMode>(depthMask);
  dirty |= m_specInfo.set<SpecSamplerNull>(nullMask);
  dirty |= m_specInfo.set<SpecSamplerDrefClamp>(drefMask);
  dirty |= m_specInfo.set<SpecSamplerProjected>(projections);

  if (dirty)
    m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
}

void D3D9DeviceEx::UpdatePointModeSpec(uint32_t mode) {
  if (m_specInfo.set<SpecPointMode>(mode))
    m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
}

void D3D9DeviceEx::UpdateFogModeSpec(bool fogEnabled, D3DFOGMODE vertexFogMode,
                                     D3DFOGMODE pixelFogMode) {
  bool dirty = m_specInfo.set<SpecFogEnabled>(fogEnabled);
  dirty |= m_specInfo.set<SpecVertexFogMode>(vertexFogMode);
  dirty |= m_specInfo.set<SpecPixelFogMode>(pixelFogMode);

  if (dirty)
    m_dirty.set(D3D9DeviceDirtyFlag::SpecializationEntries);
}

void D3D9DeviceEx::BindSpecConstants() {
  if (!m_dirty.test(D3D9DeviceDirtyFlag::SpecializationEntries))
    return;

  EmitCs([cSpecInfo = m_specInfo](DxvkContext *ctx) {
    for (size_t i = 0; i < cSpecInfo.data.size(); i++)
      ctx->setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, i,
                           cSpecInfo.data[i]);
  });

  // Write spec constants into buffer for fast-linked pipelines to use it.
  if (m_usingGraphicsPipelines) {
    // TODO: Make uploading specialization information less naive.
    auto mapPtr = m_specBuffer.AllocSlice();
    memcpy(mapPtr, m_specInfo.data.data(), D3D9SpecializationInfo::UBOSize);
  }

  m_dirty.clr(D3D9DeviceDirtyFlag::SpecializationEntries);
}

GpuFlushType D3D9DeviceEx::GetMaxFlushType() const {
  if (m_d3d9Options.reproducibleCommandStream)
    return GpuFlushType::ExplicitFlush;
  else if (m_dxvkDevice->perfHints().preferRenderPassOps)
    return GpuFlushType::ImplicitStrongHint;
  else
    return GpuFlushType::ImplicitWeakHint;
}

bool D3D9DeviceEx::ValidateSharedTexture(
    HANDLE handle, D3DRESOURCETYPE type,
    const D3D9_COMMON_TEXTURE_DESC &textureDesc) const {
  if (!(reinterpret_cast<uintptr_t>(handle) & 0xc0000000)) {
    Logger::warn(str::format(
        "D3D9DeviceEx::ValidateSharedTexture: not a D3DKMT handle: ", handle));
    return false;
  }

  union d3dkmt_desc desc;

  D3DKMT_QUERYRESOURCEINFO query = {};
  query.hDevice = m_dxvkDevice->kmtLocal();
  query.hGlobalShare = reinterpret_cast<uintptr_t>(handle);
  query.pPrivateRuntimeData = &desc;
  query.PrivateRuntimeDataSize = sizeof(desc);

  if (D3DKMTQueryResourceInfo(&query)) {
    Logger::warn(str::format(
        "D3D9DeviceEx::ValidateSharedTexture: Failed to query resource: ",
        handle));
  } else if (query.PrivateRuntimeDataSize < sizeof(desc.dxgi) ||
             query.PrivateRuntimeDataSize > sizeof(desc)) {
    Logger::warn(
        str::format("D3D9DeviceEx::ValidateSharedTexture: Unexpected size: ",
                    query.PrivateRuntimeDataSize));
  } else {
    D3DDDI_OPENALLOCATIONINFO2 alloc = {};
    D3DKMT_OPENRESOURCE open = {};
    open.hDevice = m_dxvkDevice->kmtLocal();
    open.hGlobalShare = reinterpret_cast<uintptr_t>(handle);
    open.NumAllocations = 1;
    open.pOpenAllocationInfo2 = &alloc;
    open.pPrivateRuntimeData = &desc;
    open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

    if (D3DKMTOpenResource2(&open)) {
      Logger::warn(str::format(
          "D3D9DeviceEx::ValidateSharedTexture: Failed to open resource: ",
          handle));
    } else {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = m_dxvkDevice->kmtLocal();
      destroy.hResource = open.hResource;
      D3DKMTDestroyAllocation(&destroy);

      if (desc.dxgi.size != sizeof(desc.d3d9) || desc.dxgi.version != 1) {
        Logger::warn(
            str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid size: ",
                        desc.dxgi.size, " or version: ", desc.dxgi.version));
        return false;
      }
      if (desc.d3d9.type != type) {
        Logger::warn(
            str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid type: ",
                        desc.d3d9.type));
        return false;
      }
      if (desc.d3d9.dxgi.width != textureDesc.Width ||
          desc.d3d9.dxgi.height != textureDesc.Height) {
        Logger::warn(str::format(
            "D3D9DeviceEx::ValidateSharedTexture: Invalid dimensions: ",
            desc.d3d9.dxgi.width, "x", desc.d3d9.dxgi.height));
        return false;
      }
      if (desc.d3d9.format != static_cast<D3DFORMAT>(textureDesc.Format)) {
        Logger::warn(
            str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid format: ",
                        desc.d3d9.format));
        return false;
      }
      if (textureDesc.Usage & ~desc.d3d9.usage) {
        Logger::warn(
            str::format("D3D9DeviceEx::ValidateSharedTexture: Invalid usage: ",
                        desc.d3d9.usage));
        return false;
      }
      if (type == D3DRTYPE_TEXTURE &&
          desc.d3d9.texture.levels != textureDesc.MipLevels) {
        Logger::warn(str::format(
            "D3D9DeviceEx::ValidateSharedTexture: Invalid mip levels: ",
            desc.d3d9.texture.levels));
        return false;
      }

      Logger::debug(str::format("Found D3D9 desc: ", desc.d3d9.type));
      Logger::debug(str::format("  dxgi.width: ", desc.d3d9.dxgi.width));
      Logger::debug(str::format("  dxgi.height: ", desc.d3d9.dxgi.height));
      Logger::debug(str::format("  format: ", desc.d3d9.format));
      Logger::debug(str::format("  usage: ", desc.d3d9.usage));
      if (desc.d3d9.type == D3DRTYPE_TEXTURE) {
        Logger::debug(
            str::format("  texture.width: ", desc.d3d9.texture.width));
        Logger::debug(
            str::format("  texture.height: ", desc.d3d9.texture.height));
        Logger::debug(
            str::format("  texture.depth: ", desc.d3d9.texture.depth));
        Logger::debug(
            str::format("  texture.levels: ", desc.d3d9.texture.levels));
      } else if (desc.d3d9.type == D3DRTYPE_SURFACE) {
        Logger::debug(
            str::format("  surface.width: ", desc.d3d9.surface.width));
        Logger::debug(
            str::format("  surface.height: ", desc.d3d9.surface.height));
      }
      return true;
    }
  }

  /* ignore failures for legacy Proton implementation */
  return true;
}

bool D3D9DeviceEx::ValidateSharedBuffer(
    HANDLE handle, const dxvk::D3D9_BUFFER_DESC &bufferDesc) const {
  if (!(reinterpret_cast<uintptr_t>(handle) & 0xc0000000)) {
    Logger::warn(str::format(
        "D3D9DeviceEx::ValidateSharedBuffer: not a D3DKMT handle: ", handle));
    return false;
  }

  union d3dkmt_desc desc;

  D3DKMT_QUERYRESOURCEINFO query = {};
  query.hDevice = m_dxvkDevice->kmtLocal();
  query.hGlobalShare = reinterpret_cast<uintptr_t>(handle);
  query.pPrivateRuntimeData = &desc;
  query.PrivateRuntimeDataSize = sizeof(desc);

  if (D3DKMTQueryResourceInfo(&query)) {
    Logger::warn(str::format(
        "D3D9DeviceEx::ValidateSharedBuffer: Failed to query resource: ",
        handle));
  } else if (query.PrivateRuntimeDataSize < sizeof(desc.dxgi) ||
             query.PrivateRuntimeDataSize > sizeof(desc)) {
    Logger::warn(
        str::format("D3D9DeviceEx::ValidateSharedBuffer: Unexpected size: ",
                    query.PrivateRuntimeDataSize));
  } else {
    D3DDDI_OPENALLOCATIONINFO2 alloc = {};
    D3DKMT_OPENRESOURCE open = {};
    open.hDevice = m_dxvkDevice->kmtLocal();
    open.hGlobalShare = reinterpret_cast<uintptr_t>(handle);
    open.NumAllocations = 1;
    open.pOpenAllocationInfo2 = &alloc;
    open.pPrivateRuntimeData = &desc;
    open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

    if (D3DKMTOpenResource2(&open)) {
      Logger::warn(str::format(
          "D3D9DeviceEx::ValidateSharedBuffer: Failed to open resource: ",
          handle));
    } else {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = m_dxvkDevice->kmtLocal();
      destroy.hResource = open.hResource;
      D3DKMTDestroyAllocation(&destroy);

      if (desc.dxgi.size != sizeof(desc.d3d9) || desc.dxgi.version != 1) {
        Logger::warn(
            str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid size: ",
                        desc.dxgi.size, " or version: ", desc.dxgi.version));
        return false;
      }
      if (desc.d3d9.type != bufferDesc.Type) {
        Logger::warn(
            str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid type: ",
                        desc.d3d9.type));
        return false;
      }
      if (desc.d3d9.dxgi.width != bufferDesc.Size ||
          desc.d3d9.buffer.width != bufferDesc.Size) {
        Logger::warn(str::format(
            "D3D9DeviceEx::ValidateSharedBuffer: Invalid dimensions: ",
            desc.d3d9.dxgi.width, "x", desc.d3d9.dxgi.height));
        return false;
      }
      if (desc.d3d9.buffer.format != static_cast<UINT>(bufferDesc.Format)) {
        Logger::warn(
            str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid format: ",
                        desc.d3d9.format));
        return false;
      }
      if (bufferDesc.Usage & ~desc.d3d9.usage) {
        Logger::warn(
            str::format("D3D9DeviceEx::ValidateSharedBuffer: Invalid usage: ",
                        desc.d3d9.usage));
        return false;
      }

      Logger::debug(str::format("Found D3D9 desc: ", desc.d3d9.type));
      Logger::debug(str::format("  dxgi.width: ", desc.d3d9.buffer.width));
      Logger::debug(str::format("  format: ", desc.d3d9.buffer.format));
      Logger::debug(str::format("  usage: ", desc.d3d9.usage));
      return true;
    }
  }

  /* ignore failures for legacy Proton implementation */
  return true;
}
void D3D9DeviceEx::War3TryCaptureShadowCaster(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
    UINT NumVertices, UINT StartVal, UINT CountVal, bool indexed,
    bool DynamicSysmemVBOs, bool DynamicSysmemIBO) {

  if (war3::runtime::IsWar3RuntimeModuleDisabled(
          war3::runtime::War3RuntimeModule::ShadowCapture))
    return;

  const int stage = War3RenderState::GetStage();
  const auto layer = War3RenderState::CurrentLayer();
  const auto cat = War3RenderState::GetStageCategory();
  const auto &shadowSemantic = War3RenderState::GetTlsShadowSemanticState();

  // [诊断] 极致暴力诊断：记录所有进入逻辑的 Draw Call
  static uint32_t s_rawCallCount = 0;
  s_rawCallCount++;
  if ((s_rawCallCount < 50 || (s_rawCallCount % 2000 == 0)) &&
      (stage == 10 || stage == 11 || stage == 12)) {
    war3dbg::Print("DXVK ShadowDebug: War3TryCaptureShadowCaster Call #%u "
                   "stage=%d layer=%d cat=%d sem(entry=%p scene=%p "
                   "handle=0x%08X raw=0x%08X tag=%d semStage=%d)\n",
                   s_rawCallCount, stage, (int)layer, (int)cat,
                   shadowSemantic.worldObjectEntry, shadowSemantic.sceneNode,
                   shadowSemantic.jHandle, shadowSemantic.rawcode,
                   static_cast<int>(shadowSemantic.tag), shadowSemantic.stage);
  }

  static bool s_disableShadowCapture = false;
  static bool s_checkedShadowCaptureEnv = false;
  if (!s_checkedShadowCaptureEnv) {
    s_checkedShadowCaptureEnv = true;
    const std::string envV = env::getEnvVar("DXVK_WAR3_DISABLE_SHADOW_CAPTURE");
    if (!envV.empty() && envV != "0") {
      s_disableShadowCapture = true;
      Logger::info("DXVK War3Shadow: Shadow capture disabled by env var");
    }
  }
  if (s_disableShadowCapture)
    return;

  if (dxvk::war3::internal::kNativeShadowDisableShadowCaptureWhenMode1 &&
      War3RenderState::GetNativeShadowMode() >= 1u) {
    return;
  }

  // DXVK semantic scene submission is the active object-shadow producer in the
  // new path. Keeping the old draw-time ShadowCapture hook alive here means we
  // still walk VB/IB/state recovery for every draw even when its output is
  // intentionally not consumed. Disable that legacy capture surface under the
  // semantic preview runtime gate; the receiver/shadow-map passes still consume
  // the semantic packets submitted later in the frame.
  if (War3SemanticConsumerEnabled() &&
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
      dxvk::war3::internal::
          IsSemanticSceneDisableLegacyShadowCaptureRuntimeEnabled()) {
    War3TryPublishSemanticDrawTimePose();
    return;
  }

  // [性能] 若本帧不需要阴影/描边捕获，则跳过整个 ShadowCapture 热路径。
  if (!m_war3Pipeline || !m_war3Pipeline->WantsShadowCapture()) {
    if (s_rawCallCount < 100 && (stage == 10 || stage == 11)) {
      war3dbg::Print(
          "DXVK ShadowDebug: ShadowCapture Exit - WantsShadowCapture=0\n");
    }
    return;
  }

  if (m_war3Pipeline && m_war3Pipeline->HasInsertedBeforeUi()) {
    if (s_rawCallCount < 100 && (stage == 10 || stage == 11)) {
      war3dbg::Print(
          "DXVK ShadowDebug: ShadowCapture Exit - HasInsertedBeforeUi=1\n");
    }
    return;
  }

  if (layer == War3RenderLayer::UI)
    return;

  // [性能优化] 仅在确认是可能需要捕获的世界渲染批次后，才开启性能采样点。
  // 这将减少 UI 渲染（每帧几十万次）产生的冗余 cpuScope Overhead。
  auto shadowCaptureScope = [&]() -> war3::War3PerfMonitor::ScopedCpuScope {
    if constexpr (dxvk::war3::internal::kNativeOptimizationPerfTrackingEnabled) {
      return war3::War3PerfMonitor::instance().cpuScope("ShadowCapture");
    }
    return {};
  }();
  const auto &captureSettings = m_war3Pipeline->GetSettings();
  if (!captureSettings.shadows.enabled &&
      !captureSettings.occludedOutline.enabled)
    return;
  if (!captureSettings.shadows.enabled &&
      captureSettings.occludedOutline.enabled &&
      !War3RenderState::HasOutlineHandles())
    return;

  if (!m_war3Scene.worldCamera.valid && War3IsLikelyMainWorldViewport())
    War3RecordWorldCamera();

  if (m_war3Scene.shadowCasters.size() >= 8192u) {
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      Logger::info(
          "DXVK War3Shadow: caster drawlist overflow, skip further draws");
    }
    return;
  }

  const auto tag = War3RenderState::GetCurrentBatchTag();
  const auto execTag = War3RenderState::GetTlsBatchTag();
  // const auto cat = War3RenderState::GetStageCategory(); // 已在顶层声明
  if (cat == War3RenderState::StageCategory::PostProcess ||
      cat == War3RenderState::StageCategory::Skybox ||
      cat == War3RenderState::StageCategory::UI ||
      cat == War3RenderState::StageCategory::Effect)
    return;

  // const int stage = War3RenderState::GetStage(); // 已在顶层声明
  const bool terrainTileCaster = War3RenderState::IsTerrainRendering();
  const bool terrainDoodadCaster =
      (cat == War3RenderState::StageCategory::Terrain) && (stage == 10);
  // [教学示例] S1 地形主体渲染显式检测
  // S1 是地形瓦片的主要渲染阶段，需要显式添加以确保投射阴影
  const bool terrainS1Caster =
      (cat == War3RenderState::StageCategory::Terrain) && (stage == 1);
  const bool terrainCaster =
      terrainTileCaster || terrainDoodadCaster || terrainS1Caster;

  // 装饰物阴影过滤（Debug Control）
  if (terrainDoodadCaster && !dxvk::war3::internal::kShadowRenderDecorations) {
    static uint32_t s_decorationSkipLog = 0;
    if (s_decorationSkipLog < 5) {
      s_decorationSkipLog++;
      Logger::info("DXVK: Skipping decoration shadow (disabled by "
                   "kShadowRenderDecorations)");
    }
    m_war3Scene.shadowStats.skippedNotCaster++;
    return;
  }

  const bool objectCasterByTls = (tag == War3BatchTag::WorldObjects ||
                                  tag == War3BatchTag::SelectionOverlay ||
                                  tag == War3BatchTag::Decorations ||
                                  tag == War3BatchTag::RangeIndicatorTarget);
  const bool objectCasterByStage =
      (cat == War3RenderState::StageCategory::WorldObject) &&
      (stage == 7 || stage == 10 || stage == 11 ||
       stage == 12); // S7=选择圈, S10=装饰物, S11=单位, S12=范围指示器目标
  const auto *currentObj =
      terrainCaster ? nullptr : dxvk::war3::render::GetCurrentBatchObject();
  const bool objectCasterByCurrentObj =
      currentObj != nullptr &&
      currentObj->kind != dxvk::war3::render::ObjectKind::Unknown;
  const bool needsSemanticContext =
      !terrainCaster &&
      (objectCasterByTls || objectCasterByStage || objectCasterByCurrentObj ||
       shadowSemantic.HasAnyContext());

  War3ShadowSemanticContext semantic = {};
  if (needsSemanticContext)
    semantic = War3BuildShadowSemanticContext(currentObj);

  const bool objectCasterByRuntimeSemantic =
      needsSemanticContext && semantic.runtimeModelPtr != nullptr &&
      (semantic.objectKind == dxvk::war3::render::ObjectKind::Unit ||
       (currentObj != nullptr &&
        currentObj->kind == dxvk::war3::render::ObjectKind::Unit));
  const bool objectCaster = objectCasterByTls || objectCasterByStage ||
                            objectCasterByCurrentObj ||
                            objectCasterByRuntimeSemantic;
  const bool hasSemanticBridgeContext =
      shadowSemantic.HasAnyContext() || objectCasterByCurrentObj ||
      (needsSemanticContext && semantic.HasStableIdentity());
  auto noteSemanticSceneBypassCandidate = [&]() {
    m_war3Scene.shadowStats.semanticBridgeBypassed++;
    m_war3Scene.shadowStats.semanticSceneBypassUnitLikeCount++;
    if (semantic.runtimeModelPtr != nullptr)
      m_war3Scene.shadowStats.semanticSceneBypassUnitLikeWithRuntimeModel++;
    if (semantic.modelResourcePtr != nullptr || semantic.modelKey != 0u)
      m_war3Scene.shadowStats.semanticSceneBypassUnitLikeWithModelResource++;
    if (semantic.hasPoseTransform || semantic.poseFromSpriteFrame ||
        semantic.poseMatrixCount != 0u)
      m_war3Scene.shadowStats.semanticSceneBypassUnitLikeWithPose++;
    if (semantic.renderablePart != nullptr || semantic.sceneNode != nullptr ||
        semantic.worldObjectEntry != nullptr)
      m_war3Scene.shadowStats.semanticSceneBypassUnitLikeWithRenderable++;
    if (War3PublishSemanticSceneBypassCandidate(semantic, currentObj))
      m_war3Scene.shadowStats.semanticSceneBypassPublishedVisibleCandidate++;
    else
      m_war3Scene.shadowStats.semanticSceneBypassPublishMiss++;
  };

  if (objectCaster) {
    if (hasSemanticBridgeContext)
      m_war3Scene.shadowStats.semanticBridgeHit++;
    else
      m_war3Scene.shadowStats.semanticBridgeMiss++;
  } else {
    m_war3Scene.shadowStats.semanticBridgeBypassed++;
  }

  // [诊断] Stage 10 调试日志
  if (stage == 10) {
    static uint32_t s_stage10Log = 0;
    if (s_stage10Log < 10) {
      s_stage10Log++;
      Logger::info(str::format("DXVK: Stage 10 Shadow Check: cat=", (int)cat,
                               " terrainTile=", terrainTileCaster ? 1 : 0,
                               " terrainDoodad=", terrainDoodadCaster ? 1 : 0,
                               " terrainCaster=", terrainCaster ? 1 : 0,
                               " objCaster=", objectCaster ? 1 : 0));
    }
  }

  // [教学示例] Stage 1 地形调试日志
  // 这会在控制台输出 S1 的检测结果，方便验证修改是否生效
  if (stage == 1) {
    static uint32_t s_stage1Log = 0;
    if (s_stage1Log < 10) {
      s_stage1Log++;
      Logger::info(str::format("DXVK: Stage 1 Shadow Check: cat=", (int)cat,
                               " terrainS1=", terrainS1Caster ? 1 : 0,
                               " terrainCaster=", terrainCaster ? 1 : 0,
                               " tag=", (int)tag));
    }
  }

  if (!terrainCaster && !objectCaster) {
    if (stage == 10) {
      static uint32_t s_stage10SkipLog = 0;
      if (s_stage10SkipLog < 5) {
        s_stage10SkipLog++;
        Logger::info("DXVK: Stage 10 SKIPPED - not caster");
      }
    }
    m_war3Scene.shadowStats.skippedNotCaster++;
    return;
  }
  m_war3Scene.shadowStats.considered++;

  const bool earlySemanticSceneUnitLikeCandidate =
      War3SemanticConsumerEnabled() &&
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
      dxvk::war3::internal::
          IsSemanticSceneBypassLegacyUnitCaptureRuntimeEnabled() &&
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly &&
      !terrainCaster &&
      (semantic.objectKind == dxvk::war3::render::ObjectKind::Unit ||
       (currentObj != nullptr &&
        currentObj->kind == dxvk::war3::render::ObjectKind::Unit) ||
       ((cat == War3RenderState::StageCategory::WorldObject) && stage == 11));
  if (earlySemanticSceneUnitLikeCandidate) {
    noteSemanticSceneBypassCandidate();
    return;
  }

  const bool zTestEnabled = (m_state.renderStates[D3DRS_ZENABLE] != FALSE) &&
                            (m_state.depthStencil != nullptr);
  if (!zTestEnabled) {
    m_war3Scene.shadowStats.skippedNoZTest++;
    return;
  }

  const bool allowVertexShaderCaster =
      terrainCaster ||
      (objectCaster &&
       tag == War3BatchTag::Decorations); // 允许装饰物走 VS 以捕获阴影
  const bool allowRuntimePoseFallbackCaster =
      objectCaster &&
      semantic.runtimeModelPtr != nullptr &&
      semantic.objectKind == dxvk::war3::render::ObjectKind::Unit;
  if (m_state.vertexShader != nullptr && !allowVertexShaderCaster &&
      !allowRuntimePoseFallbackCaster) {
    m_war3Scene.shadowStats.skippedVertexShader++;
    return;
  }

  const bool alphaTestEnabled =
      (m_state.renderStates[D3DRS_ALPHATESTENABLE] != FALSE);
  const bool zWriteEnabled =
      (m_state.renderStates[D3DRS_ZWRITEENABLE] != FALSE);
  const bool alphaBlend =
      (m_state.renderStates[D3DRS_ALPHABLENDENABLE] != FALSE);
  bool additiveBlend = false;

  // ===== 过滤逻辑 =====
  // 目标：允许"Transparent"过滤模式的物体投射阴影
  // War3的Transparent模式通常：启用AlphaBlend + 可能禁用zWrite + 没有AlphaTest

  // [修复] 区分地形类型：S1 主体、地形瓦片、装饰物
  // - S1 地形主体 (terrainS1Caster): 支持透明贴图，需要 Alpha Test
  // - 地形瓦片 (terrainTileCaster): 必须写深度，否则是覆盖层/特效
  // - 装饰物 (terrainDoodadCaster): 允许半透明、不写深度（如树叶）

  if (terrainS1Caster) {
    // S1 地形主体：支持透明贴图的地形
    // 某些地图作者会使用透明贴图替换地形，因此需要支持 Alpha Test
    if (!zWriteEnabled && !alphaTestEnabled) {
      // 既不写深度也没有 Alpha Test，可能是覆盖层或特效，跳过
      static uint32_t s_s1SkipLog = 0;
      if (s_s1SkipLog < 5) {
        s_s1SkipLog++;
        Logger::info("DXVK: S1 Terrain SKIPPED - no zWrite and no alphaTest");
      }
      m_war3Scene.shadowStats.skippedOverlay++;
      return;
    }
    // 允许：zWriteEnabled=1 || alphaTestEnabled=1
    // 这样可以捕获普通地形和带透明贴图的地形
    static uint32_t s_s1ShadowLog = 0;
    if (s_s1ShadowLog < 10) {
      s_s1ShadowLog++;
      Logger::info(str::format(
          "DXVK: S1 Terrain Shadow Capture - zWrite=", zWriteEnabled ? 1 : 0,
          " alphaTest=", alphaTestEnabled ? 1 : 0,
          " alphaBlend=", alphaBlend ? 1 : 0));
    }
  } else if (terrainTileCaster) {
    // 非 S1 的地形瓦片：必须写深度
    if (!zWriteEnabled) {
      m_war3Scene.shadowStats.skippedOverlay++;
      return;
    }
  } else if (terrainDoodadCaster) {
    // 装饰物：宽松过滤，允许半透明物体投射阴影
    // 不强制要求 zWriteEnabled
    static uint32_t s_doodadShadowLog = 0;
    if (s_doodadShadowLog < 10) {
      s_doodadShadowLog++;
      Logger::info(str::format(
          "DXVK: Stage 10 Decoration Shadow Capture - zWrite=",
          zWriteEnabled ? 1 : 0, " alphaBlend=", alphaBlend ? 1 : 0));
    }
  } else {
    // [DEBUG] 暴力测试：移除所有过滤条件
    // 任何非地形物体都允许尝试投射阴影
    /*
    if (!zWriteEnabled && !alphaTestEnabled && !alphaBlend) {
        m_war3Scene.shadowStats.skippedOverlay++;
        return;
    }
    */
  }

  if (alphaBlend) {
    const DWORD srcBlend = m_state.renderStates[D3DRS_SRCBLEND];
    const DWORD dstBlend = m_state.renderStates[D3DRS_DESTBLEND];
    const DWORD blendOp = m_state.renderStates[D3DRS_BLENDOP];
    // 仅跳过纯叠加混合（光效、粒子等不应该投射阴影）
    const bool isAdditive =
        (blendOp == D3DBLENDOP_ADD) &&
        ((srcBlend == D3DBLEND_ONE && dstBlend == D3DBLEND_ONE) ||
         (srcBlend == D3DBLEND_SRCALPHA && dstBlend == D3DBLEND_ONE));
    additiveBlend = isAdditive;

    // [DEBUG] 暂时允许 Additive 物体投射阴影，以排除误判
    if (isAdditive) { /* m_war3Scene.shadowStats.skippedOverlay++; return; */
    }

    // [修复] 不再根据 ZFUNC == ALWAYS 过滤
    // if (isAdditive || m_state.renderStates[D3DRS_ZFUNC] == D3DCMP_ALWAYS) {
    // m_war3Scene.shadowStats.skippedOverlay++; return; }
  }

  VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  if (PrimitiveType == D3DPT_TRIANGLELIST)
    topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  else if (PrimitiveType == D3DPT_TRIANGLESTRIP)
    topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  else
    return;

  auto *decl = m_state.vertexDecl.ptr();
  if (decl == nullptr)
    return;

  const auto &declInfo = War3GetShadowDeclInfo(decl);
  if (!declInfo.hasPosition) {
    m_war3Scene.shadowStats.skippedNoPosition++;
    return;
  }

  // Pos Checks
  const D3DDECLTYPE posType = declInfo.posType;
  const uint32_t posCompCount = declInfo.posCompCount;
  const VkFormat posFormat = declInfo.posFormat;
  if (posType == D3DDECLTYPE_D3DCOLOR || posCompCount < 3 ||
      posFormat == VK_FORMAT_UNDEFINED) {
    m_war3Scene.shadowStats.skippedPosFormat++;
    return;
  }

  const uint32_t posStream = declInfo.posStream;
  if (posStream >= caps::MaxStreams)
    return;

  // Blend Checks
  uint32_t paletteIndex = 0;
  uint32_t blendWeightOffset = 0;
  VkFormat blendWeightFormat = VK_FORMAT_R32_SFLOAT;
  uint32_t blendIndexOffset = 0;
  VkFormat blendIndexFormat = VK_FORMAT_R8G8B8A8_USCALED;
  uint32_t blendStride = 0;
  uint32_t blendBinding = 0;
  uint32_t blendStream = posStream;

  const DWORD vbState = m_state.renderStates[D3DRS_VERTEXBLEND];
  const bool vbIndexed =
      (m_state.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE] != FALSE);
  bool vertexBlendEnabled = false;
  uint32_t vbCount = 0;

  auto logVbSkipOnce = [&](const char *reason) {
    static bool s_logged = false;
    if (s_logged)
      return;
    s_logged = true;
    WAR3_RENDER_LOG("DXVK War3Shadow: skip vertexBlend caster (%s)\n", reason);
  };

  if (vbState != D3DVBF_DISABLE || vbIndexed) {
    if (!(vbState == D3DVBF_0WEIGHTS && !vbIndexed)) {
      vertexBlendEnabled = true;
      if (vbState == D3DVBF_DISABLE || vbState == D3DVBF_0WEIGHTS)
        vbCount = 0;
      else if (vbState >= D3DVBF_1WEIGHTS && vbState <= D3DVBF_3WEIGHTS)
        vbCount = vbState;
      else {
        m_war3Scene.shadowStats.skippedVertexBlend++;
        return;
      }
    }
  }

  if (vertexBlendEnabled) {
    if (vbCount > 0) {
      if (!declInfo.hasBlendWeight) {
        m_war3Scene.shadowStats.skippedVertexBlend++;
        logVbSkipOnce("missing weights");
        return;
      }
      blendWeightOffset = declInfo.weightOffset;
      blendWeightFormat = declInfo.weightFormat;
      if (blendWeightFormat == VK_FORMAT_UNDEFINED) {
        m_war3Scene.shadowStats.skippedVertexBlend++;
        return;
      }

      if (declInfo.weightStream != posStream) {
        blendBinding = 1;
        blendStream = declInfo.weightStream;
        if (declInfo.weightStream < caps::MaxStreams)
          blendStride = m_state.vertexBuffers[declInfo.weightStream].stride;
      } else {
        blendBinding = 0;
        blendStream = posStream;
        blendStride = m_state.vertexBuffers[posStream].stride;
      }
    }
    if (vbIndexed) {
      if (!declInfo.hasBlendIndex) {
        m_war3Scene.shadowStats.skippedVertexBlend++;
        logVbSkipOnce("missing indices");
        return;
      }
      blendIndexOffset = declInfo.indexOffset;
      blendIndexFormat = declInfo.indexFormat;
      if (blendIndexFormat == VK_FORMAT_UNDEFINED) {
        m_war3Scene.shadowStats.skippedVertexBlend++;
        return;
      }

      const uint32_t indexStream = declInfo.indexStream;
      if (indexStream != posStream) {
        // Shadow replay currently supports at most one secondary vertex binding.
        // Accept layouts where weights/indices share the same secondary stream,
        // or index-only layouts that use a dedicated secondary stream.
        if (blendBinding == 0) {
          if (vbCount > 0) {
            m_war3Scene.shadowStats.skippedVertexBlend++;
            logVbSkipOnce("weights/index on different bindings");
            return;
          }
          blendBinding = 1;
          blendStream = indexStream;
          if (indexStream < caps::MaxStreams)
            blendStride = m_state.vertexBuffers[indexStream].stride;
        } else if (indexStream != blendStream) {
          m_war3Scene.shadowStats.skippedVertexBlend++;
          logVbSkipOnce("weights/index split across streams");
          return;
        }
      } else if (blendBinding == 1) {
        m_war3Scene.shadowStats.skippedVertexBlend++;
        logVbSkipOnce("weights/index on different bindings");
        return;
      }
    }
    paletteIndex = War3GetOrCreateShadowMatrixPalette();
  }

  // ===== Alpha测试阴影支持：捕获UV流和纹理 =====
  const D3DVERTEXELEMENT9 *texcoordElem = nullptr;
  uint32_t uvOffset = 0;
  uint32_t uvStride = 0;
  VkFormat uvFormat = VK_FORMAT_UNDEFINED;
  uint32_t uvStream = 0; // [Fix] Declare uvStream here
  DxvkBufferSlice uvSlice;
  Rc<DxvkBuffer> uvAlloc;
  DxvkResourceBufferInfo uvInfo = {};
  Rc<DxvkImageView> diffuseTexView;
  float alphaRefFloat = 0.5f;
  bool captureAlphaTest = false;

  // 只有在启用Alpha测试或者Alpha混合时才尝试捕获UV和纹理
  // 对于"Transparent"过滤模式，War3只开Alpha混合，我们需要强制转为阴影的Alpha测试
  bool shouldCaptureUV = alphaTestEnabled || alphaBlend;

  if (shouldCaptureUV) {
    // 1. 查找TEXCOORD0顶点元素
    for (const auto &e : decl->GetElements()) {
      if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0) {
        texcoordElem = &e;
        break;
      }
    }

    // 2. 获取Alpha阈值 (0-255 -> 0.0-1.0)
    DWORD alphaRefDword = 128; // 默认值 (0.5)
    if (alphaTestEnabled) {
      // 如果原本就开启了Alpha测试，使用设置的值
      alphaRefDword = m_state.renderStates[D3DRS_ALPHAREF];
    } else {
      // 如果没开启Alpha测试(仅AlphaBlend)，默认使用0.5作为阈值
      // 这对于大多数Transparent物体是合理的
      alphaRefDword = 128;
    }
    alphaRefFloat = float(alphaRefDword & 0xFF) / 255.0f;

    // 3. 尝试获取Stage 0纹理
    if (m_state.textures[0] != nullptr) {
      // 通过GetCommonTexture获取内部纹理对象，然后获取Vulkan视图
      auto *commonTex = GetCommonTexture(m_state.textures[0]);
      if (commonTex) {
        diffuseTexView = commonTex->GetSampleView(false);
      }
    }

    // 4. 如果有UV和纹理，标记为需要Alpha测试
    if (texcoordElem && diffuseTexView) {
      captureAlphaTest = true;
      uvOffset = texcoordElem->Offset;
      const D3DDECLTYPE uvType = D3DDECLTYPE(texcoordElem->Type);
      uvFormat = DecodeDecltype(uvType);

      // UV通常和Position在同一个流中
      uvStream = texcoordElem->Stream;
      if (uvStream < caps::MaxStreams) {
        uvStride = m_state.vertexBuffers[uvStream].stride;
      } else {
        captureAlphaTest = false; // Invalid Stream
      }
    }
  }

  if (vertexBlendEnabled && semantic.runtimeModelPtr != nullptr) {
    dxvk::war3::model::PoseRecord posePaletteRecord = {};
    auto& poseRegistry = dxvk::war3::model::PoseRegistry::instance();
    if (poseRegistry.findByRuntimeModel(semantic.runtimeModelPtr,
                                        posePaletteRecord) &&
        posePaletteRecord.matrixCount != 0 &&
        !posePaletteRecord.matrixPalette.empty()) {
      // 旧的 legacy-freeze fallback 之前对 Unit 排除了 runtime palette
      // 覆盖，结果就是“单位没进 upper-layer authoritative，又没吃到
      // CModel 最终 pose palette”，阴影会直接缺失或姿态不对。
      // 这里统一改成：只要拿得到 runtimeModel 的最终 palette，就优先用
      // runtime pose，而不是继续依赖 FF vertex-blend 状态。
      paletteIndex = War3GetOrCreateShadowMatrixPaletteFromData(
          posePaletteRecord.matrixPalette.data(), posePaletteRecord.matrixCount);
    }
  }

  // 说明：batchHandle 用于描边/高亮匹配；优先使用 TLS 句柄，
  // 若 TLS 为空则回退到当前批次对象的 jHandle，避免漏匹配导致描边残缺。
  uint32_t batchHandle = War3RenderState::GetTlsBatchHandle();
  if (batchHandle == 0 && currentObj && currentObj->jHandle != 0) {
    batchHandle = currentObj->jHandle;
  }
  if (batchHandle == 0 && semantic.jHandle != 0) {
    batchHandle = semantic.jHandle;
  }
  // 统一句柄格式，避免不同来源携带额外高位导致描边匹配失败。
  if (batchHandle != 0) {
    const uint32_t handleId = batchHandle & 0x0FFFFFu;
    batchHandle = handleId ? (0x100000u | handleId) : 0u;
  }

  const dxvk::war3::render::RenderObjectInfo *pathBlockObj = currentObj;
  if ((!pathBlockObj || pathBlockObj->rawcode == 0) && batchHandle != 0) {
    if (const auto *lookupObj =
            dxvk::war3::render::RenderObjectRegistry::instance().findByHandle(
                batchHandle)) {
      pathBlockObj = lookupObj;
    }
  }

  if (dxvk::war3::internal::kPathBlockerHideEnabled && pathBlockObj) {
    if (pathBlockObj->rawcode != 0) {
      const uint32_t rawcode = pathBlockObj->rawcode;

      if (dxvk::war3::internal::kPathBlockerDebugEnabled) {
        static uint32_t s_debugLog = 0;
        if (s_debugLog < 50) {
          s_debugLog++;
          char rawcodeStr[5] = {0};
          FormatFourCcEditorString(rawcode, rawcodeStr);
          WAR3_RENDER_LOG("DXVK: Shadow caster rawcode='%s' (0x%08X), "
                          "handle=0x%X, groupIdx=%d\n",
                          rawcodeStr, rawcode, batchHandle,
                          pathBlockObj->groupIdx);
        }
      }

      if (IsLosBlockerFourCc(rawcode)) {
        static uint32_t s_blockerLog = 0;
        if (s_blockerLog < 10 &&
            dxvk::war3::internal::kPathBlockerDebugEnabled) {
          s_blockerLog++;
          char rawcodeStr[5] = {0};
          FormatFourCcEditorString(rawcode, rawcodeStr);
          WAR3_RENDER_LOG("DXVK: Skipping path blocker shadow, rawcode='%s' "
                          "(0x%08X), handle=0x%X\n",
                          rawcodeStr, rawcode, batchHandle);
        }
        m_war3Scene.shadowStats.skippedNotCaster++;
        return;
      }
    }
  }

  const uint8_t resolvedObjectKind =
      currentObj && currentObj->kind != dxvk::war3::render::ObjectKind::Unknown
          ? static_cast<uint8_t>(currentObj->kind)
          : static_cast<uint8_t>(semantic.objectKind);
  const bool forceFreezeUnitLikeGeometry =
      resolvedObjectKind ==
          static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Unit) ||
      ((cat == War3RenderState::StageCategory::WorldObject) && stage == 11) ||
      (semantic.runtimeModelPtr != nullptr &&
       (objectCaster ||
        semantic.objectKind == dxvk::war3::render::ObjectKind::Unit)) ||
      ((cat == War3RenderState::StageCategory::WorldObject) &&
       resolvedObjectKind == 0u && (vertexBlendEnabled || vbIndexed));
  dxvk::war3::native::War3NativeShadowHint nativeHint = {};
  const bool hasNativeHint =
      War3TryResolveNativeShadowHint(semantic, currentObj, nativeHint);
  const bool nativeHintUnitLike =
      hasNativeHint &&
      (nativeHint.objectKind == dxvk::war3::render::ObjectKind::Unit ||
       nativeHint.objectKind == dxvk::war3::render::ObjectKind::Effect);
  const bool forceFreezeUnitLikeOrHint = forceFreezeUnitLikeGeometry ||
                                         nativeHintUnitLike;
  const bool semanticSceneUnitLikeCandidate =
      resolvedObjectKind ==
          static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Unit) ||
      semantic.objectKind == dxvk::war3::render::ObjectKind::Unit ||
      nativeHintUnitLike ||
      (semantic.runtimeModelPtr != nullptr &&
       (objectCaster || vertexBlendEnabled || vbIndexed || stage == 11 ||
        semantic.hasPoseTransform || semantic.poseFromSpriteFrame ||
        semantic.poseMatrixCount != 0u));
  const bool unitLikeObject = semanticSceneUnitLikeCandidate;
  const bool semanticSceneOwnsUnitCapture =
      War3SemanticConsumerEnabled() &&
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
      dxvk::war3::internal::
          IsSemanticSceneBypassLegacyUnitCaptureRuntimeEnabled() &&
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly &&
      semanticSceneUnitLikeCandidate &&
      (objectCaster || stage == 11 || nativeHintUnitLike ||
       vertexBlendEnabled || vbIndexed || semantic.runtimeModelPtr != nullptr ||
       semantic.hasPoseTransform || semantic.poseFromSpriteFrame ||
       semantic.poseMatrixCount != 0u);
  if (semanticSceneOwnsUnitCapture) {
    noteSemanticSceneBypassCandidate();
    return;
  }
  const bool poseDrivenOrVertexBlendGeometry =
      vertexBlendEnabled || vbIndexed || semantic.runtimeModelPtr != nullptr ||
      semantic.hasPoseTransform || semantic.poseFromSpriteFrame ||
      semantic.poseMatrixCount != 0u;
  const Matrix4 currentWorldMatrix =
      m_state.transforms[GetTransformIndex(D3DTS_WORLD)];
  auto matrixTranslationLenSq = [](const Matrix4 &m) {
    return m[3].x * m[3].x + m[3].y * m[3].y + m[3].z * m[3].z;
  };
  auto matrixIdentityDeviation = [](const Matrix4 &m) {
    return std::abs(m[0].x - 1.0f) + std::abs(m[1].y - 1.0f) +
           std::abs(m[2].z - 1.0f) + std::abs(m[0].y) +
           std::abs(m[0].z) + std::abs(m[1].x) + std::abs(m[1].z) +
           std::abs(m[2].x) + std::abs(m[2].y);
  };
  auto noteDynamicPoseUsage = [&](bool dynamicSource) {
    if (!unitLikeObject)
      return;
    if (semantic.hasPoseTransform)
      m_war3Scene.shadowStats.dynamicPoseCount++;
    if (dynamicSource || poseDrivenOrVertexBlendGeometry)
      m_war3Scene.shadowStats.dynamicSkinnedOutputCount++;

    uint64_t hash = bit::fnv1a_init();
    hash = bit::fnv1a_iter(hash,
                           reinterpret_cast<uintptr_t>(semantic.runtimeModelPtr));
    hash = bit::fnv1a_iter(hash, semantic.modelKey);
    hash = bit::fnv1a_iter(hash, semantic.jHandle);
    if (semantic.hasPoseTransform) {
      hash = bit::fnv1a_iter(hash, semantic.poseMatrixCount);
      hash = bit::fnv1a_iter(hash, semantic.poseMatrixHash);
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(semantic.poseHeight));
      for (uint32_t i = 0; i < 4; i++) {
        hash = bit::fnv1a_iter(hash,
                               bit::cast<uint32_t>(semantic.poseTransform[i].x));
        hash = bit::fnv1a_iter(hash,
                               bit::cast<uint32_t>(semantic.poseTransform[i].y));
        hash = bit::fnv1a_iter(hash,
                               bit::cast<uint32_t>(semantic.poseTransform[i].z));
        hash = bit::fnv1a_iter(hash,
                               bit::cast<uint32_t>(semantic.poseTransform[i].w));
      }
    } else {
      // 运行时姿态主路径尚未正式接管前，飞行单位/移动单位即使没有 pose hook
      // 也必须参与 shadow-map 更新判定。否则在相机稳定时会错误复用上一帧
      // shadow map，表现为阴影固定在原地、落在单位右侧或完全不跟随。
      for (uint32_t i = 0; i < 4; i++) {
        hash = bit::fnv1a_iter(hash,
                               bit::cast<uint32_t>(currentWorldMatrix[i].x));
        hash = bit::fnv1a_iter(hash,
                               bit::cast<uint32_t>(currentWorldMatrix[i].y));
        hash = bit::fnv1a_iter(hash,
                               bit::cast<uint32_t>(currentWorldMatrix[i].z));
        hash = bit::fnv1a_iter(hash,
                               bit::cast<uint32_t>(currentWorldMatrix[i].w));
      }
    }
    if (m_war3Scene.shadowStats.dynamicPoseSignature == 0u)
      m_war3Scene.shadowStats.dynamicPoseSignature = hash;
    else
      m_war3Scene.shadowStats.dynamicPoseSignature =
          bit::fnv1a_iter(m_war3Scene.shadowStats.dynamicPoseSignature, hash);
  };

  auto finalizeShadowDrawCommon = [&](War3ShadowCasterDraw &draw) {
    draw.category = cat;
    draw.batchTag = (execTag != War3BatchTag::Unknown) ? execTag : tag;
    draw.batchHandle = batchHandle;
    draw.objectKind = resolvedObjectKind;

    const bool dynamicUnitLikeNoCull =
        unitLikeObject &&
        (draw.vertexBlendEnabled || semantic.runtimeModelPtr != nullptr ||
         semantic.hasPoseTransform || semantic.poseFromSpriteFrame ||
         semantic.poseMatrixCount != 0u);
    if (dynamicUnitLikeNoCull) {
      const Matrix4* boundsMatrix = &draw.worldMatrix;
      if (draw.vertexBlendEnabled &&
          draw.paletteIndex < m_war3Scene.shadowPalettes.size()) {
        boundsMatrix = &m_war3Scene.shadowPalettes[draw.paletteIndex].worldMatrices[0];
      }
      draw.boundsCenter = War3SemanticBoundsTranslation(*boundsMatrix);
      float baseRadius = War3SemanticBoundsRadiusForObjectKind(draw.objectKind);
      if (baseRadius <= 0.0f)
        baseRadius = 260.0f;
      if (hasNativeHint && nativeHint.radiusHint > baseRadius)
        baseRadius = nativeHint.radiusHint;
      draw.boundsRadius = baseRadius * War3SemanticBoundsMaxScale(*boundsMatrix);
      return;
    }

    if (draw.category == War3RenderState::StageCategory::WorldObject ||
        draw.category == War3RenderState::StageCategory::Effect) {
      float baseRadius = 0.0f;
      if (draw.objectKind != 0) {
        using dxvk::war3::render::ObjectKind;
        switch (static_cast<ObjectKind>(draw.objectKind)) {
        case ObjectKind::Unit:
          baseRadius = 260.0f;
          break;
        case ObjectKind::Building:
          baseRadius = 900.0f;
          break;
        case ObjectKind::Destructible:
          baseRadius = 750.0f;
          break;
        case ObjectKind::Item:
          baseRadius = 220.0f;
          break;
        case ObjectKind::Effect:
          baseRadius = 900.0f;
          break;
        default:
          baseRadius = 0.0f;
          break;
        }
      }
      if (hasNativeHint && nativeHint.radiusHint > baseRadius)
        baseRadius = nativeHint.radiusHint;

      if (baseRadius > 0.0f) {
        const Matrix4 *m = &draw.worldMatrix;
        if (draw.vertexBlendEnabled &&
            draw.paletteIndex < m_war3Scene.shadowPalettes.size()) {
          m = &m_war3Scene.shadowPalettes[draw.paletteIndex].worldMatrices[0];
        }

        draw.boundsCenter = Vector4((*m)[3].x, (*m)[3].y, (*m)[3].z, 1.0f);

        auto axisLen3 = [](const Vector4 &v) {
          return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        };

        const float sx = axisLen3((*m)[0]);
        const float sy = axisLen3((*m)[1]);
        const float sz = axisLen3((*m)[2]);
        const float maxScale =
            (std::max)(1.0f, (std::max)(sx, (std::max)(sy, sz)));

        draw.boundsRadius = baseRadius * maxScale;
      }
    }
  };

  auto tryCaptureUpperLayerShadow = [&]() -> bool {
    if (!dxvk::war3::internal::kUpperLayerShadowConsumerEnabled || !objectCaster)
      return false;

    dxvk::war3::render::UpperLayerShadowResolvedItem upperItem = {};
    auto &upperRegistry = dxvk::war3::render::UpperLayerShadowRegistry::instance();
    if (!upperRegistry.resolve(semantic, upperItem))
      return false;

    const bool authoritativeRigid = upperItem.HasAuthoritativeRigidPath();
    const bool authoritativeSkinned = upperItem.HasAuthoritativeSkinnedPath();
    if (!authoritativeRigid && !authoritativeSkinned)
      return false;

    if (dxvk::war3::internal::kUpperLayerShadowConsumerObserveOnly)
      return false;

    const uint32_t vertexCount = std::min<uint32_t>(
        upperItem.geoset.vertexCount,
        uint32_t(upperItem.geoset.positions.size() / 3u));
    if (vertexCount == 0u)
      return false;
    const bool useIndices = !upperItem.geoset.indices.empty();
    bool resolvedAlphaTest = captureAlphaTest;
    if (resolvedAlphaTest) {
      const bool hasUvLayer =
          !upperItem.geoset.uvLayers.empty() &&
          !upperItem.geoset.uvLayers[0].uvPairs.empty() &&
          upperItem.geoset.uvLayers[0].uvPairs.size() >=
              size_t(vertexCount) * 2u &&
          diffuseTexView != nullptr;
      if (!hasUvLayer) {
        resolvedAlphaTest = false;
      }
    }

    uint32_t upperPaletteIndex = 0u;
    std::vector<std::array<uint8_t, 4>> blendIndices;
    if (authoritativeSkinned) {
      if (upperItem.runtimeGroupPalette.empty() ||
          upperItem.runtimeGroupPalette.size() > 256u ||
          upperItem.maxVertexGroupSlot >= upperItem.runtimeGroupPalette.size()) {
        return false;
      }

      upperPaletteIndex = War3GetOrCreateShadowMatrixPaletteFromData(
          upperItem.runtimeGroupPalette.data(),
          uint32_t(upperItem.runtimeGroupPalette.size()));

      if (upperItem.geoset.vertexGroupIndices.size() < size_t(vertexCount))
        return false;

      blendIndices.resize(vertexCount);
      for (uint32_t i = 0; i < vertexCount; ++i) {
        const uint32_t groupSlot = upperItem.geoset.vertexGroupIndices[i];
        if (groupSlot >= upperItem.runtimeGroupPalette.size() ||
            groupSlot >= 256u) {
          return false;
        }

        blendIndices[i] = {uint8_t(groupSlot), 0u, 0u, 0u};
      }
    }

    if (!upperRegistry.tryMarkEmitted(upperItem))
      return true;

    auto computeUpperLocalBounds =
        [&](Vector4 &outCenter, float &outRadius) {
          outCenter = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
          outRadius = 0.0f;
          if (upperItem.geoset.positions.size() < 3u)
            return;

          float minX = upperItem.geoset.positions[0];
          float minY = upperItem.geoset.positions[1];
          float minZ = upperItem.geoset.positions[2];
          float maxX = minX;
          float maxY = minY;
          float maxZ = minZ;

          for (uint32_t i = 0; i < vertexCount; ++i) {
            const size_t base = size_t(i) * 3u;
            const float x = upperItem.geoset.positions[base + 0u];
            const float y = upperItem.geoset.positions[base + 1u];
            const float z = upperItem.geoset.positions[base + 2u];
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            minZ = std::min(minZ, z);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
            maxZ = std::max(maxZ, z);
          }

          outCenter = Vector4((minX + maxX) * 0.5f, (minY + maxY) * 0.5f,
                              (minZ + maxZ) * 0.5f, 1.0f);
          float radiusSq = 0.0f;
          for (uint32_t i = 0; i < vertexCount; ++i) {
            const size_t base = size_t(i) * 3u;
            const float dx = upperItem.geoset.positions[base + 0u] - outCenter.x;
            const float dy = upperItem.geoset.positions[base + 1u] - outCenter.y;
            const float dz = upperItem.geoset.positions[base + 2u] - outCenter.z;
            radiusSq = std::max(radiusSq, dx * dx + dy * dy + dz * dz);
          }
          outRadius = std::sqrt(radiusSq);
        };

    War3ShadowGeometryRegistryKey key = {};
    key.sourceHash = bit::fnv1a_init();
    key.sourceHash =
        bit::fnv1a_iter(key.sourceHash, upperItem.geoset.contentHash);
    key.sourceHash =
        bit::fnv1a_iter(key.sourceHash, upperItem.visible.modelKey);
    key.sourceHash = bit::fnv1a_iter(key.sourceHash,
                                     uint64_t(upperItem.visible.geosetIndex));
    key.sourceHash = bit::fnv1a_iter(
        key.sourceHash,
        reinterpret_cast<uintptr_t>(upperItem.visible.modelResourcePtr));
    key.sourceHash = bit::fnv1a_iter(
        key.sourceHash, reinterpret_cast<uintptr_t>(diffuseTexView.ptr()));
    key.sourceHash = bit::fnv1a_iter(
        key.sourceHash, uint32_t(m_state.renderStates[D3DRS_CULLMODE]));

    key.layoutHash = bit::fnv1a_init();
    key.layoutHash = bit::fnv1a_iter(key.layoutHash, uint32_t(useIndices ? 1u : 0u));
    key.layoutHash = bit::fnv1a_iter(
        key.layoutHash, uint32_t(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST));
    key.layoutHash = bit::fnv1a_iter(key.layoutHash, uint32_t(sizeof(float) * 3u));
    key.layoutHash = bit::fnv1a_iter(key.layoutHash,
                                     uint32_t(VK_FORMAT_R32G32B32_SFLOAT));
    key.layoutHash = bit::fnv1a_iter(key.layoutHash, uint32_t(vertexCount));
    key.layoutHash = bit::fnv1a_iter(
        key.layoutHash,
        uint32_t(useIndices ? upperItem.geoset.indices.size() : 0u));
    key.layoutHash = bit::fnv1a_iter(
        key.layoutHash, uint32_t(authoritativeSkinned ? 1u : 0u));
    key.layoutHash = bit::fnv1a_iter(
        key.layoutHash,
        uint32_t(authoritativeSkinned ? VK_FORMAT_R8G8B8A8_USCALED
                                      : VK_FORMAT_UNDEFINED));
    key.layoutHash = bit::fnv1a_iter(
        key.layoutHash, uint32_t(resolvedAlphaTest ? 1u : 0u));
    key.layoutHash = bit::fnv1a_iter(
        key.layoutHash,
        uint32_t(resolvedAlphaTest ? VK_FORMAT_R32G32_SFLOAT
                                   : VK_FORMAT_UNDEFINED));
    key.layoutHash = bit::fnv1a_iter(
        key.layoutHash, bit::cast<uint32_t>(alphaRefFloat));
    key.mode = authoritativeSkinned ? War3ShadowReplayMode::PaletteSkinnedFF
                                    : War3ShadowReplayMode::FixedWorld;

    War3ShadowPersistentGeometry candidate = {};
    candidate.key = key;
    candidate.indexed = useIndices;
    candidate.positionStride = sizeof(float) * 3u;
    candidate.positionOffset = 0u;
    candidate.positionFormat = VK_FORMAT_R32G32B32_SFLOAT;
    candidate.indexType = VK_INDEX_TYPE_UINT16;
    candidate.vertexBlendEnabled = authoritativeSkinned;
    candidate.vertexBlendIndexed = authoritativeSkinned;
    candidate.vertexBlendCount = 0u;
    candidate.blendWeightOffset = 0u;
    candidate.blendWeightFormat = VK_FORMAT_UNDEFINED;
    candidate.blendIndexOffset = 0u;
    candidate.blendIndexFormat = authoritativeSkinned ? VK_FORMAT_R8G8B8A8_USCALED
                                                      : VK_FORMAT_UNDEFINED;
    candidate.blendStride = authoritativeSkinned ? 4u : 0u;
    candidate.blendBinding = authoritativeSkinned ? 1u : 0u;
    candidate.alphaTestEnabled = resolvedAlphaTest;
    candidate.alphaRef = alphaRefFloat;
    candidate.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    candidate.indexCount =
        useIndices ? uint32_t(upperItem.geoset.indices.size()) : 0u;
    candidate.firstIndex = 0u;
    candidate.vertexOffset = 0;
    candidate.vertexCount = useIndices ? 0u : vertexCount;
    candidate.firstVertex = 0u;
    candidate.minVertexIndex = 0u;
    candidate.numVertices = vertexCount;
    candidate.uvStride = resolvedAlphaTest ? sizeof(float) * 2u : 0u;
    candidate.uvOffset = 0u;
    candidate.uvFormat = resolvedAlphaTest ? VK_FORMAT_R32G32_SFLOAT
                                           : VK_FORMAT_UNDEFINED;
    candidate.uvBinding = resolvedAlphaTest ? 2u : 0u;
    candidate.diffuseTexture = diffuseTexView;
    if (resolvedAlphaTest && candidate.diffuseTexture != nullptr &&
        m_shadowReceiverPass != nullptr) {
      bool alphaUseMip = false;
      float alphaMipLodBias = 0.0f;
      if (m_war3Pipeline) {
        const auto &shadowSettings = m_war3Pipeline->GetSettings().shadows;
        alphaUseMip = shadowSettings.alphaShadowUseMip;
        alphaMipLodBias = shadowSettings.alphaShadowMipLodBias;
      }
      candidate.diffuseSampler =
          m_shadowReceiverPass->getFallbackSampler(alphaUseMip, alphaMipLodBias);
      if (candidate.diffuseSampler != nullptr)
        candidate.diffuseSamplerIndex =
            candidate.diffuseSampler->getDescriptor().samplerIndex;
      candidate.textureDescriptor = *candidate.diffuseTexture->getDescriptor();
    }
    computeUpperLocalBounds(candidate.localBoundsCenter,
                            candidate.localBoundsRadius);

    std::array<War3ShadowPersistentUpload, 4> uploads = {};
    uploads[0].hostData = upperItem.geoset.positions.data();
    uploads[0].bytes =
        VkDeviceSize(upperItem.geoset.positions.size() * sizeof(float));
    uploads[0].usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    uploads[0].debugName = "War3UpperShadowPos";
    if (useIndices) {
      uploads[1].hostData = upperItem.geoset.indices.data();
      uploads[1].bytes =
          VkDeviceSize(upperItem.geoset.indices.size() * sizeof(uint16_t));
      uploads[1].usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
      uploads[1].debugName = "War3UpperShadowIdx";
    }
    if (authoritativeSkinned && !blendIndices.empty()) {
      uploads[2].hostData = blendIndices.data();
      uploads[2].bytes =
          VkDeviceSize(blendIndices.size() * sizeof(blendIndices[0]));
      uploads[2].usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      uploads[2].debugName = "War3UpperShadowBlend";
    }
    if (resolvedAlphaTest) {
      const auto &uvLayer = upperItem.geoset.uvLayers[0];
      uploads[3].hostData = uvLayer.uvPairs.data();
      uploads[3].bytes = VkDeviceSize(size_t(vertexCount) * 2u * sizeof(float));
      uploads[3].usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      uploads[3].debugName = "War3UpperShadowUv";
    }

    uint32_t geometryId = 0u;
    const War3ShadowPersistentGeometry *geometry = nullptr;
    bool createdNewGeometry = false;
    if (!War3FindOrCreateShadowPersistentGeometry(key, candidate, uploads,
                                                  geometryId, geometry,
                                                  createdNewGeometry) ||
        geometry == nullptr) {
      return false;
    }

    War3ShadowCasterDraw draw = {};
    draw.indexed = geometry->indexed;
    draw.positionStorage = geometry->positionStorage;
    draw.positionInfo = geometry->positionInfo;
    draw.positionStride = geometry->positionStride;
    draw.positionOffset = geometry->positionOffset;
    draw.positionFormat = geometry->positionFormat;
    draw.topology = geometry->topology;
    draw.worldMatrix = authoritativeSkinned ? Matrix4() : currentWorldMatrix;
    draw.vertexBlendEnabled = geometry->vertexBlendEnabled;
    draw.vertexBlendIndexed = geometry->vertexBlendIndexed;
    draw.vertexBlendCount = geometry->vertexBlendCount;
    draw.paletteIndex = upperPaletteIndex;
    draw.blendWeightOffset = geometry->blendWeightOffset;
    draw.blendWeightFormat = geometry->blendWeightFormat;
    draw.blendIndexOffset = geometry->blendIndexOffset;
    draw.blendIndexFormat = geometry->blendIndexFormat;
    draw.blendBinding = geometry->blendBinding;
    draw.blendStride = geometry->blendStride;
    draw.alphaTestEnabled = geometry->alphaTestEnabled;
    draw.alphaRef = geometry->alphaRef;
    draw.alphaBlendEnabled = alphaBlend;
    draw.depthWriteEnabled = zWriteEnabled;
    draw.depthTestEnabled = zTestEnabled;
    draw.additiveBlend = additiveBlend;
    draw.uvStride = geometry->uvStride;
    draw.uvOffset = geometry->uvOffset;
    draw.uvFormat = geometry->uvFormat;
    draw.diffuseTexture = geometry->diffuseTexture;
    draw.diffuseSampler = geometry->diffuseSampler;
    draw.textureDescriptor = geometry->textureDescriptor;
    draw.diffuseSamplerIndex = geometry->diffuseSamplerIndex;
    if (geometry->indexed) {
      draw.indexStorage = geometry->indexStorage;
      draw.indexInfo = geometry->indexInfo;
      draw.indexType = geometry->indexType;
      draw.indexCount = geometry->indexCount;
      draw.firstIndex = geometry->firstIndex;
      draw.vertexOffset = geometry->vertexOffset;
      draw.vertexCount = 0u;
      draw.firstVertex = 0u;
      draw.minVertexIndex = geometry->minVertexIndex;
      draw.numVertices = geometry->numVertices;
    } else {
      draw.indexCount = 0u;
      draw.firstIndex = 0u;
      draw.vertexOffset = 0;
      draw.vertexCount = geometry->vertexCount;
      draw.firstVertex = geometry->firstVertex;
      draw.minVertexIndex = geometry->minVertexIndex;
      draw.numVertices = geometry->numVertices;
    }
    if (geometry->blendBinding == 1) {
      draw.blendStorage = geometry->blendStorage;
      draw.blendInfo = geometry->blendInfo;
    }
    if (geometry->alphaTestEnabled && geometry->uvBinding == 2u) {
      draw.uvStorage = geometry->uvStorage;
      draw.uvInfo = geometry->uvInfo;
    }

    finalizeShadowDrawCommon(draw);

    War3ShadowInstanceRef instance = {};
    instance.geometryId = geometryId;
    instance.materialId = 0u;
    instance.replayDrawIndex =
        static_cast<uint32_t>(m_war3Scene.shadowCasters.size());
    instance.batchHandle = batchHandle;
    instance.paletteIndex = draw.paletteIndex;
    instance.worldMatrix = draw.worldMatrix;
    instance.boundsCenter = draw.boundsCenter;
    instance.boundsRadius = draw.boundsRadius;
    instance.category = cat;
    instance.batchTag = (execTag != War3BatchTag::Unknown) ? execTag : tag;
    instance.objectKind = resolvedObjectKind;
    instance.mode = key.mode;

    if (authoritativeSkinned)
      noteDynamicPoseUsage(true);

    m_war3Scene.shadowInstances.emplace_back(std::move(instance));
    m_war3Scene.shadowCasters.emplace_back(std::move(draw));
    m_war3Scene.shadowStats.captured++;
    if (useIndices)
      m_war3Scene.shadowStats.capturedIndexed++;
    else
      m_war3Scene.shadowStats.capturedNonIndexed++;
    if (objectCaster)
      m_war3Scene.shadowStats.capturedWorldObject++;
    if (resolvedObjectKind ==
        static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Unit)) {
      m_war3Scene.shadowStats.capturedUnitObject++;
      if (authoritativeSkinned)
        m_war3Scene.shadowStats.capturedUnitVertexBlend++;
      m_war3Scene.shadowStats.persistentUnitInstanceCount++;
    }
    m_war3Scene.shadowStats.persistentInstanceCount++;
    if (!objectCaster ||
        resolvedObjectKind ==
            static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Building) ||
        resolvedObjectKind == static_cast<uint8_t>(
                                  dxvk::war3::render::ObjectKind::Destructible)) {
      m_war3Scene.shadowStats.staticPersistentCount++;
    }
    if (createdNewGeometry) {
      m_war3Scene.shadowStats.persistentGeometryCount++;
      m_war3Scene.shadowStats.uniqueGeometryCount++;
      m_war3Scene.shadowPersistentPool.promotedThisFrame++;
    } else {
      m_war3Scene.shadowStats.duplicateGeometryInstances++;
      m_war3Scene.shadowStats.reuseEligibleDuplicates++;
      m_war3Scene.shadowStats.potentialFreezeReuseHits++;
      m_war3Scene.shadowStats.instancedGeometryDrawsSaved++;
    }
    m_war3Scene.shadowPersistentPool.bytesUsed = m_war3ShadowPersistentBytesUsed;
    m_war3Scene.shadowPersistentPool.bytesEvicted =
        m_war3ShadowPersistentBytesEvicted;
    m_war3Scene.shadowPersistentPool.liveGeometryCount =
        static_cast<uint32_t>(m_war3ShadowPersistentGeometries.size());
    m_war3Scene.shadowStats.persistentPoolBytesUsed =
        m_war3ShadowPersistentBytesUsed;
    m_war3Scene.shadowStats.persistentPoolBytesEvicted =
        m_war3ShadowPersistentBytesEvicted;
    return true;
  };

  if (tryCaptureUpperLayerShadow())
    return;
  if (dxvk::war3::internal::kUpperLayerShadowConsumerEnabled &&
      dxvk::war3::internal::kUpperLayerShadowObjectNoCaptureFallbackEnabled &&
      objectCaster) {
    return;
  }

  // Capture Resources
  static const bool s_freezeDynamicShadowBuffers =
      env::getEnvVar("DXVK_WAR3_SHADOW_FREEZE_DYNAMIC") != "0";
  // 注意：部分魔兽的 indexed draw 会传入不严格的 MinVertexIndex/NumVertices。
  // 若直接用它裁剪 VB，阴影重放可能读取越界，表现为阴影缺失/闪烁。
  // 因此默认仅对非 indexed 裁剪；如需强制启用可设置：
  //   DXVK_WAR3_SHADOW_TRIM_INDEXED=1
  static const bool s_trimIndexedShadowVb =
      env::getEnvVar("DXVK_WAR3_SHADOW_TRIM_INDEXED") == "1";

  // Position Buffer
  DxvkBufferSlice posSlice;
  uint32_t posStride = 0;
  Rc<DxvkBuffer> posAlloc;
  if (DynamicSysmemVBOs && m_war3PerDrawUpload.vbValid[posStream]) {
    posSlice = m_war3PerDrawUpload.vbSlices[posStream];
    posStride = m_war3PerDrawUpload.vbStrides[posStream];
    posAlloc = posSlice.buffer();
  } else {
    auto *vb = m_state.vertexBuffers[posStream].vertexBuffer.ptr();
    if (vb) {
      auto *vbCommon = vb->GetCommonBuffer();
      if (vbCommon) {
        posSlice = vbCommon->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(
            m_state.vertexBuffers[posStream].offset);
        posStride = m_state.vertexBuffers[posStream].stride;
        posAlloc = posSlice.buffer();
      }
    }
  }
  if (posStride == 0) {
    m_war3Scene.shadowStats.skippedPosFormat++;
    return;
  }
  if (!posAlloc)
    return;

  DxvkResourceBufferInfo posInfo = posSlice.getSliceInfo();

  if (captureAlphaTest) {
    if (uvStream != posStream) {
      captureAlphaTest = false;
      uvFormat = VK_FORMAT_UNDEFINED;
    } else if (uvStride == 0 || uvOffset >= posStride) {
      captureAlphaTest = false;
      uvFormat = VK_FORMAT_UNDEFINED;
    }
  }

  // [Perf] 冻结动态 VB/IB 时，只拷贝本次 Draw 实际会用到的数据范围
  // 说明：GetBufferSlice(offset) 的 sliceInfo.size 通常是“从 offset 到 buffer
  // 末尾”， 若直接全量
  // copyBuffer，会造成不必要的带宽与分配压力（尤其是阴影重放多级联时）。
  // 注意：indexed draw 的顶点范围默认不裁剪（见
  // DXVK_WAR3_SHADOW_TRIM_INDEXED）。
  int64_t vertexRangeEnd = -1; // end (exclusive), 以“顶点序号”计
  bool vertexRangeValid = false;
  if (indexed) {
    if (s_trimIndexedShadowVb && NumVertices > 0) {
      const int64_t start = static_cast<int64_t>(BaseVertexIndex) +
                            static_cast<int64_t>(MinVertexIndex);
      const int64_t end = start + static_cast<int64_t>(NumVertices);
      // BaseVertexIndex 为负且范围落到 0 之前时，无法安全裁剪，退回全量
      if (start >= 0 && end > start) {
        vertexRangeEnd = end;
        vertexRangeValid = true;
      }
    }
  } else {
    if (CountVal > 0) {
      const int64_t start = static_cast<int64_t>(StartVal);
      const int64_t end = start + static_cast<int64_t>(CountVal);
      if (start >= 0 && end > start) {
        vertexRangeEnd = end;
        vertexRangeValid = true;
      }
    }
  }

  VkDeviceSize posBytesNeeded = posInfo.size;
  if (vertexRangeValid && posStride != 0) {
    const VkDeviceSize wanted = static_cast<VkDeviceSize>(vertexRangeEnd) *
                                static_cast<VkDeviceSize>(posStride);
    if (wanted > 0 && wanted < posBytesNeeded)
      posBytesNeeded = wanted;
  }

  auto *vb = m_state.vertexBuffers[posStream].vertexBuffer.ptr();
  const auto *vbCommon = vb ? vb->GetCommonBuffer() : nullptr;
  const bool posDynamic =
      vbCommon && (vbCommon->Desc()->Usage & D3DUSAGE_DYNAMIC);

  // Blend Buffer (if Binding 1)
  DxvkBufferSlice blendSlice;
  Rc<DxvkBuffer> blendAlloc;
  DxvkResourceBufferInfo blendInfo = {};
  bool blendDynamic = false;
  VkDeviceSize blendBytesNeeded = 0;
  if (blendBinding == 1) {
    if (blendStream >= caps::MaxStreams)
      return;

    if (DynamicSysmemVBOs && m_war3PerDrawUpload.vbValid[blendStream]) {
      blendSlice = m_war3PerDrawUpload.vbSlices[blendStream];
      blendAlloc = blendSlice.buffer();
    } else {
      auto *vb = m_state.vertexBuffers[blendStream].vertexBuffer.ptr();
      if (vb) {
        auto *vbCommon = vb->GetCommonBuffer();
        if (vbCommon) {
          blendSlice = vbCommon->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(
              m_state.vertexBuffers[blendStream].offset);
          blendAlloc = blendSlice.buffer();
        }
      }
    }

    if (blendAlloc) {
      blendInfo = blendSlice.getSliceInfo();
      blendBytesNeeded = blendInfo.size;
      if (vertexRangeValid && blendStride != 0) {
        const VkDeviceSize wanted =
            static_cast<VkDeviceSize>(vertexRangeEnd) *
            static_cast<VkDeviceSize>(blendStride);
        if (wanted > 0 && wanted < blendBytesNeeded)
          blendBytesNeeded = wanted;
      }

      auto *vb = m_state.vertexBuffers[blendStream].vertexBuffer.ptr();
      const auto *blendCommon = vb ? vb->GetCommonBuffer() : nullptr;
      blendDynamic =
          blendCommon && (blendCommon->Desc()->Usage & D3DUSAGE_DYNAMIC);
    }
  }
  if (blendBinding == 1) {
    if (!blendAlloc || blendInfo.buffer == VK_NULL_HANDLE ||
        blendInfo.size == 0 || blendStride == 0) {
      m_war3Scene.shadowStats.skippedVertexBlend++;
      return;
    }
  }

  // Index Buffer
  DxvkBufferSlice idxSlice;
  Rc<DxvkBuffer> idxAlloc;
  DxvkResourceBufferInfo idxInfo = {};
  VkIndexType indexType = VK_INDEX_TYPE_UINT16;
  bool ibDynamic = false;
  VkDeviceSize idxBytesNeeded = 0;
  if (indexed) {
    if (DynamicSysmemIBO) {
      idxSlice = m_war3PerDrawUpload.ibSlice;
      idxAlloc = idxSlice.buffer();
    } else {

      if (!m_state.indices.ptr())
        return;
      auto *ibCommon = m_state.indices.ptr()->GetCommonBuffer();
      if (!ibCommon)
        return;
      idxSlice = ibCommon->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(0);
      idxAlloc = idxSlice.buffer();

      if (!DynamicSysmemIBO && ibCommon->Desc()->Format == D3D9Format::INDEX32)
        indexType = VK_INDEX_TYPE_UINT32;
    }
    if (DynamicSysmemIBO && m_war3PerDrawUpload.ibType == VK_INDEX_TYPE_UINT32)
      indexType = VK_INDEX_TYPE_UINT32;

    if (!idxAlloc)
      return;
    idxInfo = idxSlice.getSliceInfo();
    if (idxInfo.size == 0)
      return;
    idxBytesNeeded = idxInfo.size;
    const VkDeviceSize indexSize =
        (indexType == VK_INDEX_TYPE_UINT16) ? 2u : 4u;
    const VkDeviceSize needed = (static_cast<VkDeviceSize>(StartVal) +
                                 static_cast<VkDeviceSize>(CountVal)) *
                                indexSize;
    if (needed > 0 && needed < idxBytesNeeded)
      idxBytesNeeded = needed;

    auto *ibCommon = m_state.indices.ptr()
                         ? m_state.indices.ptr()->GetCommonBuffer()
                         : nullptr;
    ibDynamic = DynamicSysmemIBO ||
                (ibCommon && (ibCommon->Desc()->Usage & D3DUSAGE_DYNAMIC));
  }

  const bool dynamicShadowSource =
      DynamicSysmemVBOs || DynamicSysmemIBO || posDynamic || blendDynamic ||
      ibDynamic;
  auto chooseShadowWorldMatrix = [&](const Matrix4 &currentWorld) {
    if (!semantic.hasPoseTransform || !unitLikeObject)
      return currentWorld;
    if (vertexBlendEnabled || vbIndexed)
      return currentWorld;

    const float poseTranslationLenSq =
        matrixTranslationLenSq(semantic.poseTransform);
    if (poseTranslationLenSq <= 1.0e-6f)
      return currentWorld;

    // runtime pose 链已经接通，但对动态 CUnit/飞行单位来说，这份 pose world
    // transform 还不能无条件替代 draw-time world matrix。否则一旦采到的是
    // 中间态/压缩姿态，阴影就会停在原地或整体偏移。这里先收成“仅在 draw-time
    // world 明显是 identity-ish 占位值，且源本身不是动态 per-frame upload”
    // 时才用 pose 兜底；否则保守返回当前 draw 的 world matrix。
    const float currentTranslationLenSq = matrixTranslationLenSq(currentWorld);
    const float currentIdentityDeviation = matrixIdentityDeviation(currentWorld);
    if (semantic.runtimeModelPtr != nullptr) {
      // runtime-model 单位默认仍以 draw-time world 为准，避免旧的 pose world
      // 误采样把阴影钉在原地。但对飞行单位这类常见 case，如果我们拿到的是
      // 更晚的 sprite-frame pose，且当前 draw-time world 明显像占位矩阵，
      // 就允许它做一次极窄的纠偏。
      if (semantic.poseFromSpriteFrame &&
          currentTranslationLenSq <= 1.0f &&
          currentIdentityDeviation <= 0.25f) {
        return semantic.poseTransform;
      }
      return currentWorld;
    }

    if (currentTranslationLenSq <= 1.0f && currentIdentityDeviation <= 0.25f)
      return semantic.poseTransform;

    return currentWorld;
  };
  const Matrix4 shadowWorldMatrix = chooseShadowWorldMatrix(currentWorldMatrix);

  const auto replayMode =
      War3ClassifyShadowReplayMode(vertexBlendEnabled, vbIndexed);
  const bool skinnedOrPoseDrivenGeometry =
      replayMode == War3ShadowReplayMode::PaletteSkinnedFF ||
      poseDrivenOrVertexBlendGeometry;
  const bool dynamicPersistentSource =
      (replayMode == War3ShadowReplayMode::PaletteSkinnedFF ||
       replayMode == War3ShadowReplayMode::FixedWorld) &&
      (DynamicSysmemVBOs || DynamicSysmemIBO || posDynamic || blendDynamic ||
       ibDynamic);
  enum class War3PersistentRejectReason : uint8_t {
    None = 0,
    NoIdentity,
    UnsupportedMode,
    DynamicSource,
    AlphaBlend,
    MissingStorage,
    CreateOrBudget,
  };

  War3PersistentRejectReason persistentRejectReason =
      War3PersistentRejectReason::None;
  if (!semantic.HasStableIdentity()) {
    persistentRejectReason = War3PersistentRejectReason::NoIdentity;
  } else if (!(replayMode == War3ShadowReplayMode::FixedWorld ||
               replayMode == War3ShadowReplayMode::PaletteSkinnedFF)) {
    persistentRejectReason = War3PersistentRejectReason::UnsupportedMode;
  } else if (forceFreezeUnitLikeOrHint || skinnedOrPoseDrivenGeometry) {
    // 现阶段动态单位/飞行单位/带运行时姿态线索的 world-object
    // 一律只允许走 fallback freeze。哪怕它表面上是 FixedWorld，
    // 只要这里命中，就不能再晋升 persistent，否则阴影会停在原地。
    persistentRejectReason = War3PersistentRejectReason::DynamicSource;
  } else if (additiveBlend) {
    persistentRejectReason = War3PersistentRejectReason::AlphaBlend;
  } else if (posAlloc == nullptr || (indexed && idxAlloc == nullptr) ||
             (replayMode == War3ShadowReplayMode::PaletteSkinnedFF &&
              blendBinding == 1 && blendAlloc == nullptr)) {
    persistentRejectReason = War3PersistentRejectReason::MissingStorage;
  }

  const bool persistentEligible =
      dxvk::war3::internal::kShadowPersistentGeometryCacheEnabled &&
      persistentRejectReason == War3PersistentRejectReason::None &&
      War3CanPromoteShadowPersistentGeometry(
          semantic, replayMode, objectCaster, indexed, captureAlphaTest, alphaBlend,
          DynamicSysmemVBOs, DynamicSysmemIBO, posDynamic, blendDynamic,
          ibDynamic, blendBinding, posAlloc, blendAlloc, idxAlloc);
  if (!persistentEligible &&
      persistentRejectReason == War3PersistentRejectReason::None) {
    persistentRejectReason = dynamicPersistentSource
                                 ? War3PersistentRejectReason::DynamicSource
                                 : War3PersistentRejectReason::UnsupportedMode;
  }

  // 现阶段 world-object 的 fallback 正确性优先于带宽节省。
  // 只要某个世界对象没有晋升到 persistent，就强制冻结它的 VB/IB/Blend/UV，
  // 避免后续 draw/帧把原始缓冲区改写后，shadow pass 读到被污染的数据。
  // 这正是当前 CUnit 阴影“挂掉、闪烁、停在原地”的高概率根因。
  const bool forceFreezeFallbackWorldGeometry =
      !persistentEligible && objectCaster;

  if (persistentEligible) {
    auto hashSlice = [](uint64_t hash, const DxvkBufferSlice &slice,
                        VkDeviceSize bytes) {
      hash = bit::fnv1a_iter(
          hash, reinterpret_cast<uintptr_t>(slice.buffer().ptr()));
      hash = bit::fnv1a_iter(hash, uint64_t(slice.offset()));
      hash = bit::fnv1a_iter(hash, uint64_t(bytes));
      return hash;
    };

    uint64_t sourceHash = bit::fnv1a_init();
    if (dynamicPersistentSource) {
      sourceHash = bit::fnv1a_iter(sourceHash,
                                   War3BuildShadowSemanticIdentityHash(semantic));
      sourceHash = bit::fnv1a_iter(
          sourceHash,
          reinterpret_cast<uintptr_t>(GetCommonTexture(m_state.textures[0])));
      sourceHash = bit::fnv1a_iter(
          sourceHash, uint32_t(m_state.renderStates[D3DRS_CULLMODE]));
    } else {
      sourceHash = hashSlice(sourceHash, posSlice, posBytesNeeded);
      if (indexed)
        sourceHash = hashSlice(sourceHash, idxSlice, idxBytesNeeded);
      if (blendBinding == 1)
        sourceHash = hashSlice(sourceHash, blendSlice, blendBytesNeeded);
    }

    uint64_t layoutHash = bit::fnv1a_init();
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(indexed ? 1u : 0u));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(topo));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(posStride));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(declInfo.posOffset));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(posFormat));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(indexType));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(CountVal));
    // dynamic/per-draw upload 往往只是在大 ring/upload buffer 中换了一套
    // 打包偏移；真正的模型/子网格并没有变。把这些“偏移类 draw 参数”塞进
    // key 会导致相同单位/相同 geoset 永远 miss，persistent pool 很快被
    // 新 key 灌满却一个 hit 都没有。
    layoutHash = bit::fnv1a_iter(
        layoutHash, uint32_t(dynamicPersistentSource ? 0u : StartVal));
    layoutHash = bit::fnv1a_iter(
        layoutHash, uint32_t(dynamicPersistentSource ? 0u : MinVertexIndex));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(NumVertices));
    layoutHash = bit::fnv1a_iter(
        layoutHash, uint32_t(dynamicPersistentSource ? 0u : BaseVertexIndex));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(vbCount));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(vbIndexed ? 1u : 0u));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(blendBinding));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(blendStride));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(blendWeightOffset));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(blendWeightFormat));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(blendIndexOffset));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(blendIndexFormat));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(captureAlphaTest ? 1u : 0u));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(uvStride));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(uvOffset));
    layoutHash = bit::fnv1a_iter(layoutHash, uint32_t(uvFormat));
    layoutHash = bit::fnv1a_iter(layoutHash, bit::cast<uint32_t>(alphaRefFloat));
    layoutHash = bit::fnv1a_iter(
        layoutHash, reinterpret_cast<uintptr_t>(GetCommonTexture(m_state.textures[0])));
    layoutHash = bit::fnv1a_iter(
        layoutHash, uint32_t(m_state.renderStates[D3DRS_CULLMODE]));

    War3ShadowGeometryRegistryKey key = {};
    key.sourceHash = sourceHash;
    key.layoutHash = layoutHash;
    key.mode = replayMode;

    War3ShadowPersistentGeometry candidate = {};
    candidate.key = key;
    candidate.indexed = indexed;
    candidate.positionStride = posStride;
    candidate.positionOffset = declInfo.posOffset;
    candidate.positionFormat = posFormat;
    candidate.indexType = indexType;
    candidate.vertexBlendEnabled = vertexBlendEnabled;
    candidate.vertexBlendIndexed = vbIndexed;
    candidate.vertexBlendCount = uint8_t(vbCount);
    candidate.blendWeightOffset = blendWeightOffset;
    candidate.blendWeightFormat = blendWeightFormat;
    candidate.blendIndexOffset = blendIndexOffset;
    candidate.blendIndexFormat = blendIndexFormat;
    candidate.blendStride = blendStride;
    candidate.blendBinding = blendBinding;
    candidate.alphaTestEnabled = captureAlphaTest;
    candidate.alphaRef = alphaRefFloat;
    candidate.uvStride = uvStride;
    candidate.uvOffset = uvOffset;
    candidate.uvFormat = uvFormat;
    candidate.diffuseTexture = diffuseTexView;
    if (captureAlphaTest && candidate.diffuseTexture != nullptr &&
        m_shadowReceiverPass != nullptr) {
      bool alphaUseMip = false;
      float alphaMipLodBias = 0.0f;
      if (m_war3Pipeline) {
        const auto &shadowSettings = m_war3Pipeline->GetSettings().shadows;
        alphaUseMip = shadowSettings.alphaShadowUseMip;
        alphaMipLodBias = shadowSettings.alphaShadowMipLodBias;
      }
      candidate.diffuseSampler =
          m_shadowReceiverPass->getFallbackSampler(alphaUseMip, alphaMipLodBias);
      if (candidate.diffuseSampler != nullptr)
        candidate.diffuseSamplerIndex =
            candidate.diffuseSampler->getDescriptor().samplerIndex;
      candidate.textureDescriptor = *candidate.diffuseTexture->getDescriptor();
    }
    candidate.topology = topo;
    candidate.indexCount = CountVal;
    candidate.firstIndex = StartVal;
    candidate.vertexOffset = BaseVertexIndex;
    candidate.vertexCount = indexed ? 0u : CountVal;
    candidate.firstVertex = indexed ? 0u : StartVal;
    candidate.minVertexIndex = indexed ? MinVertexIndex : StartVal;
    candidate.numVertices = indexed ? NumVertices : CountVal;
    const bool uvSharedPos = captureAlphaTest && uvStream == posStream;
    const bool uvSharedBlend =
        captureAlphaTest && blendBinding == 1 && uvStream == blendStream;
    candidate.uvBinding = captureAlphaTest ? (uvSharedPos ? 0u : uvSharedBlend ? 1u : 2u) : 0u;

    std::array<War3ShadowPersistentUpload, 4> uploads = {};
    uploads[0].slice = posSlice;
    uploads[0].bytes = posBytesNeeded;
    uploads[0].usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    uploads[0].debugName = "War3ShadowPersistentPos";
    if (indexed) {
      uploads[1].slice = idxSlice;
      uploads[1].bytes = idxBytesNeeded;
      uploads[1].usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
      uploads[1].debugName = "War3ShadowPersistentIdx";
    }
    if (blendBinding == 1) {
      uploads[2].slice = blendSlice;
      uploads[2].bytes = blendBytesNeeded;
      uploads[2].usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      uploads[2].debugName = "War3ShadowPersistentBlend";
    }
    if (captureAlphaTest && !uvSharedPos && !uvSharedBlend && uvAlloc &&
        uvInfo.buffer != VK_NULL_HANDLE && uvInfo.size != 0) {
      uploads[3].slice = uvSlice;
      uploads[3].bytes = uvInfo.size;
      uploads[3].usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      uploads[3].debugName = "War3ShadowPersistentUv";
    }

    uint32_t geometryId = 0;
    const War3ShadowPersistentGeometry *geometry = nullptr;
    bool createdNewGeometry = false;
    if (War3FindOrCreateShadowPersistentGeometry(
            key, candidate, uploads, geometryId, geometry,
            createdNewGeometry) &&
        geometry != nullptr) {
      War3ShadowInstanceRef instance = {};
      instance.geometryId = geometryId;
      instance.materialId = 0;
      instance.replayDrawIndex =
          static_cast<uint32_t>(m_war3Scene.shadowCasters.size());
      instance.batchHandle = batchHandle;
      instance.paletteIndex = paletteIndex;
      instance.worldMatrix = shadowWorldMatrix;
      instance.category = cat;
      instance.batchTag = (execTag != War3BatchTag::Unknown) ? execTag : tag;
      instance.objectKind = resolvedObjectKind;
      instance.mode = replayMode;

      War3ShadowCasterDraw compatDraw = {};
      compatDraw.indexed = geometry->indexed;
      compatDraw.positionStorage = geometry->positionStorage;
      compatDraw.positionInfo = geometry->positionInfo;
      compatDraw.positionStride = geometry->positionStride;
      compatDraw.positionOffset = geometry->positionOffset;
      compatDraw.positionFormat = geometry->positionFormat;
      compatDraw.topology = geometry->topology;
      compatDraw.worldMatrix = instance.worldMatrix;
      compatDraw.vertexBlendEnabled = geometry->vertexBlendEnabled;
      compatDraw.vertexBlendIndexed = geometry->vertexBlendIndexed;
      compatDraw.vertexBlendCount = geometry->vertexBlendCount;
      compatDraw.paletteIndex = paletteIndex;
      compatDraw.blendWeightOffset = geometry->blendWeightOffset;
      compatDraw.blendWeightFormat = geometry->blendWeightFormat;
      compatDraw.blendIndexOffset = geometry->blendIndexOffset;
      compatDraw.blendIndexFormat = geometry->blendIndexFormat;
      compatDraw.blendBinding = geometry->blendBinding;
      compatDraw.blendStride = geometry->blendStride;
      compatDraw.alphaTestEnabled = geometry->alphaTestEnabled;
      compatDraw.alphaRef = geometry->alphaRef;
      compatDraw.alphaBlendEnabled = false;
      compatDraw.depthWriteEnabled = zWriteEnabled;
      compatDraw.depthTestEnabled = zTestEnabled;
      compatDraw.additiveBlend = false;
      compatDraw.uvStride = geometry->uvStride;
      compatDraw.uvOffset = geometry->uvOffset;
      compatDraw.uvFormat = geometry->uvFormat;
      compatDraw.diffuseTexture = geometry->diffuseTexture;
      compatDraw.diffuseSampler = geometry->diffuseSampler;
      compatDraw.textureDescriptor = geometry->textureDescriptor;
      compatDraw.diffuseSamplerIndex = geometry->diffuseSamplerIndex;
      if (geometry->indexed) {
        compatDraw.indexStorage = geometry->indexStorage;
        compatDraw.indexInfo = geometry->indexInfo;
        compatDraw.indexType = geometry->indexType;
        compatDraw.indexCount = geometry->indexCount;
        compatDraw.firstIndex = geometry->firstIndex;
        compatDraw.vertexOffset = geometry->vertexOffset;
        compatDraw.vertexCount = 0;
        compatDraw.firstVertex = 0;
        compatDraw.minVertexIndex = geometry->minVertexIndex;
        compatDraw.numVertices = geometry->numVertices;
      } else {
        compatDraw.vertexCount = geometry->vertexCount;
        compatDraw.firstVertex = geometry->firstVertex;
        compatDraw.indexCount = 0;
        compatDraw.firstIndex = 0;
        compatDraw.vertexOffset = 0;
        compatDraw.minVertexIndex = geometry->minVertexIndex;
        compatDraw.numVertices = geometry->numVertices;
      }
      if (geometry->blendBinding == 1) {
        compatDraw.blendStorage = geometry->blendStorage;
        compatDraw.blendInfo = geometry->blendInfo;
      }
      if (geometry->alphaTestEnabled) {
        if (geometry->uvBinding == 0u) {
          compatDraw.uvStorage = geometry->positionStorage;
          compatDraw.uvInfo = geometry->positionInfo;
        } else if (geometry->uvBinding == 1u) {
          compatDraw.uvStorage = geometry->blendStorage;
          compatDraw.uvInfo = geometry->blendInfo;
        } else {
          compatDraw.uvStorage = geometry->uvStorage;
          compatDraw.uvInfo = geometry->uvInfo;
        }
      }

      finalizeShadowDrawCommon(compatDraw);
      instance.boundsCenter = compatDraw.boundsCenter;
      instance.boundsRadius = compatDraw.boundsRadius;

      m_war3Scene.shadowInstances.emplace_back(std::move(instance));
      m_war3Scene.shadowCasters.emplace_back(std::move(compatDraw));
      m_war3Scene.shadowStats.captured++;
      if (indexed)
        m_war3Scene.shadowStats.capturedIndexed++;
      else
        m_war3Scene.shadowStats.capturedNonIndexed++;
      if (terrainCaster)
        m_war3Scene.shadowStats.capturedTerrain++;
      if (objectCaster)
        m_war3Scene.shadowStats.capturedWorldObject++;
      if (resolvedObjectKind ==
          static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Unit)) {
        m_war3Scene.shadowStats.capturedUnitObject++;
        if (vertexBlendEnabled || vbIndexed)
          m_war3Scene.shadowStats.capturedUnitVertexBlend++;
        m_war3Scene.shadowStats.persistentUnitInstanceCount++;
      }
      m_war3Scene.shadowStats.persistentInstanceCount++;
      if (!objectCaster ||
          resolvedObjectKind ==
              static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Building) ||
          resolvedObjectKind == static_cast<uint8_t>(
                                    dxvk::war3::render::ObjectKind::Destructible)) {
        m_war3Scene.shadowStats.staticPersistentCount++;
      }
      if (createdNewGeometry) {
        m_war3Scene.shadowStats.persistentGeometryCount++;
        m_war3Scene.shadowStats.uniqueGeometryCount++;
        m_war3Scene.shadowPersistentPool.promotedThisFrame++;
      } else {
        m_war3Scene.shadowStats.duplicateGeometryInstances++;
        m_war3Scene.shadowStats.reuseEligibleDuplicates++;
        m_war3Scene.shadowStats.potentialFreezeReuseHits++;
        m_war3Scene.shadowStats.instancedGeometryDrawsSaved++;
      }
      m_war3Scene.shadowPersistentPool.bytesUsed =
          m_war3ShadowPersistentBytesUsed;
      m_war3Scene.shadowPersistentPool.bytesEvicted =
          m_war3ShadowPersistentBytesEvicted;
      m_war3Scene.shadowPersistentPool.liveGeometryCount =
          static_cast<uint32_t>(m_war3ShadowPersistentGeometries.size());
      m_war3Scene.shadowStats.persistentPoolBytesUsed =
          m_war3ShadowPersistentBytesUsed;
      m_war3Scene.shadowStats.persistentPoolBytesEvicted =
          m_war3ShadowPersistentBytesEvicted;
      return;
    }

    persistentRejectReason = War3PersistentRejectReason::CreateOrBudget;
  }

  switch (persistentRejectReason) {
  case War3PersistentRejectReason::NoIdentity:
    m_war3Scene.shadowStats.persistentRejectNoIdentity++;
    break;
    case War3PersistentRejectReason::UnsupportedMode:
      m_war3Scene.shadowStats.persistentRejectUnsupportedMode++;
      break;
    case War3PersistentRejectReason::DynamicSource:
      m_war3Scene.shadowStats.persistentRejectDynamicSource++;
      break;
    case War3PersistentRejectReason::AlphaBlend:
      m_war3Scene.shadowStats.persistentRejectAlphaBlend++;
      break;
  case War3PersistentRejectReason::MissingStorage:
    m_war3Scene.shadowStats.persistentRejectMissingStorage++;
    break;
  case War3PersistentRejectReason::CreateOrBudget:
    m_war3Scene.shadowStats.persistentRejectCreateOrBudget++;
    break;
  default:
    break;
  }

  auto estimateObjectBaseRadius = [&]() -> float {
    using dxvk::war3::render::ObjectKind;
    float baseRadius = 0.0f;
    switch (static_cast<ObjectKind>(resolvedObjectKind)) {
    case ObjectKind::Unit:
      baseRadius = 260.0f;
      break;
    case ObjectKind::Building:
      baseRadius = 900.0f;
      break;
    case ObjectKind::Destructible:
      baseRadius = 750.0f;
      break;
    case ObjectKind::Item:
      baseRadius = 220.0f;
      break;
    case ObjectKind::Effect:
      baseRadius = 900.0f;
      break;
    default:
      break;
    }
    if (hasNativeHint && nativeHint.radiusHint > baseRadius)
      baseRadius = nativeHint.radiusHint;
    return baseRadius;
  };

  auto estimateFallbackUvBytes = [&]() -> uint64_t {
    if (!captureAlphaTest)
      return 0u;
    if (uvStream == posStream)
      return 0u;
    if (blendBinding == 1 && uvStream == blendStream)
      return 0u;
    if (uvStream >= caps::MaxStreams)
      return 0u;

    DxvkBufferSlice uvSlice;
    Rc<DxvkBuffer> uvStorage;
    uint32_t uvStrideBudget = uvStride;
    bool uvDynamic = false;

    if (DynamicSysmemVBOs && m_war3PerDrawUpload.vbValid[uvStream]) {
      uvSlice = m_war3PerDrawUpload.vbSlices[uvStream];
      uvStorage = uvSlice.buffer();
      if (m_war3PerDrawUpload.vbStrides[uvStream] != 0)
        uvStrideBudget = m_war3PerDrawUpload.vbStrides[uvStream];
      uvDynamic = true;
    } else {
      auto* uvVb = m_state.vertexBuffers[uvStream].vertexBuffer.ptr();
      if (!uvVb)
        return 0u;
      auto* uvCommon = uvVb->GetCommonBuffer();
      if (!uvCommon)
        return 0u;
      uvSlice = uvCommon->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(
          m_state.vertexBuffers[uvStream].offset);
      uvStorage = uvSlice.buffer();
      uvDynamic = (uvCommon->Desc()->Usage & D3DUSAGE_DYNAMIC) != 0;
    }

    if (uvStorage == nullptr ||
        !(uvDynamic || forceFreezeUnitLikeGeometry ||
          forceFreezeFallbackWorldGeometry))
      return 0u;

    auto uvInfoBudget = uvSlice.getSliceInfo();
    VkDeviceSize bytes = uvInfoBudget.size;
    if (vertexRangeValid && uvStrideBudget != 0) {
      const VkDeviceSize wanted =
          static_cast<VkDeviceSize>(vertexRangeEnd) *
          static_cast<VkDeviceSize>(uvStrideBudget);
      if (wanted > 0 && wanted < bytes)
        bytes = wanted;
    }
    return static_cast<uint64_t>(bytes);
  };

  const bool shouldFreezePosBuffer =
      s_freezeDynamicShadowBuffers &&
      (DynamicSysmemVBOs || posDynamic || forceFreezeUnitLikeGeometry ||
       forceFreezeFallbackWorldGeometry);
  const bool shouldFreezeBlendBuffer =
      blendBinding == 1 && s_freezeDynamicShadowBuffers &&
      (DynamicSysmemVBOs || blendDynamic || forceFreezeUnitLikeGeometry ||
       forceFreezeFallbackWorldGeometry);
  const bool shouldFreezeIndexBuffer =
      indexed && s_freezeDynamicShadowBuffers &&
      (ibDynamic || forceFreezeUnitLikeGeometry ||
       forceFreezeFallbackWorldGeometry);

  const uint64_t fallbackPosBudgetBytes =
      shouldFreezePosBuffer ? static_cast<uint64_t>(posBytesNeeded) : 0ull;
  const uint64_t fallbackBlendBudgetBytes =
      shouldFreezeBlendBuffer ? static_cast<uint64_t>(blendBytesNeeded) : 0ull;
  uint64_t fallbackUvBudgetBytes = estimateFallbackUvBytes();
  const uint64_t fallbackIndexBudgetBytes =
      shouldFreezeIndexBuffer ? static_cast<uint64_t>(idxBytesNeeded) : 0ull;

  float estimatedBoundsRadius = 0.0f;
  Vector4 estimatedBoundsCenter = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  if (cat == War3RenderState::StageCategory::WorldObject ||
      cat == War3RenderState::StageCategory::Effect) {
      estimatedBoundsRadius = estimateObjectBaseRadius();
      if (estimatedBoundsRadius > 0.0f) {
      const Matrix4* m = &shadowWorldMatrix;
      if (vertexBlendEnabled &&
          paletteIndex < m_war3Scene.shadowPalettes.size()) {
        m = &m_war3Scene.shadowPalettes[paletteIndex].worldMatrices[0];
      }
      estimatedBoundsCenter =
          Vector4((*m)[3].x, (*m)[3].y, (*m)[3].z, 1.0f);
      auto axisLen3 = [](const Vector4& v) {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
      };
      const float sx = axisLen3((*m)[0]);
      const float sy = axisLen3((*m)[1]);
      const float sz = axisLen3((*m)[2]);
      const float maxScale =
          (std::max)(1.0f, (std::max)(sx, (std::max)(sy, sz)));
      estimatedBoundsRadius *= maxScale;
    }
  }

  float estimatedViewDepth = 0.0f;
  if (m_war3Scene.worldCamera.valid && estimatedBoundsRadius > 0.0f) {
    const Vector4 viewPos =
        m_war3Scene.worldCamera.view * estimatedBoundsCenter;
    estimatedViewDepth = std::abs(viewPos.z);
  }

  if constexpr (dxvk::war3::internal::kShadowCaptureCoarseCullEnabled) {
    if (m_war3Scene.worldCamera.valid && estimatedBoundsRadius > 0.0f &&
        !terrainCaster) {
      const float shadowMaxDistance =
          std::max(captureSettings.shadows.csm.maxDistance, 0.0f);
      const bool decorationLike =
          terrainDoodadCaster || tag == War3BatchTag::Decorations ||
          execTag == War3BatchTag::Decorations ||
          resolvedObjectKind ==
              static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Effect);

      bool coarseCull = false;
      if (shadowMaxDistance > 0.0f &&
          estimatedViewDepth - estimatedBoundsRadius >
              shadowMaxDistance *
                  dxvk::war3::internal::kShadowCaptureCoarseCullDistanceScale) {
        coarseCull = true;
      }

      if (!coarseCull && decorationLike) {
        const Matrix4& vp = m_war3Scene.worldCamera.viewProj;
        const Vector4 clip = vp * estimatedBoundsCenter;
        const float absW = std::abs(clip.w);
        if (absW > 1.0e-6f) {
          const float invW = 1.0f / absW;
          const float ndcX = clip.x * invW;
          const float ndcY = clip.y * invW;
          const float ndcZ = clip.z * invW;
          const float row0Len = std::sqrt(vp[0].x * vp[0].x +
                                          vp[1].x * vp[1].x +
                                          vp[2].x * vp[2].x);
          const float row1Len = std::sqrt(vp[0].y * vp[0].y +
                                          vp[1].y * vp[1].y +
                                          vp[2].y * vp[2].y);
          const float row2Len = std::sqrt(vp[0].z * vp[0].z +
                                          vp[1].z * vp[1].z +
                                          vp[2].z * vp[2].z);
          const float rX = estimatedBoundsRadius * row0Len * invW;
          const float rY = estimatedBoundsRadius * row1Len * invW;
          const float rZ = estimatedBoundsRadius * row2Len * invW;
          const float guard =
              dxvk::war3::internal::kShadowCaptureCoarseCullGuardNdc;
          if (ndcX + rX < -1.0f - guard || ndcX - rX > 1.0f + guard ||
              ndcY + rY < -1.0f - guard || ndcY - rY > 1.0f + guard ||
              ndcZ + rZ < -guard || ndcZ - rZ > 1.0f + guard) {
            coarseCull = true;
          }
        }
      }

      if (coarseCull) {
        m_war3Scene.shadowStats.skippedPositionT++;
        return;
      }
    }
  }

  dxvk::war3::render::ShadowCaptureCandidateInfo budgetCandidate = {};
  budgetCandidate.category = cat;
  budgetCandidate.objectKind =
      static_cast<dxvk::war3::render::ObjectKind>(resolvedObjectKind);
  budgetCandidate.terrainTileCaster = terrainTileCaster;
  budgetCandidate.terrainDoodadCaster = terrainDoodadCaster;
  budgetCandidate.terrainS1Caster = terrainS1Caster;
  budgetCandidate.terrainDecorationLike =
      terrainDoodadCaster || tag == War3BatchTag::Decorations ||
      execTag == War3BatchTag::Decorations;
  budgetCandidate.alphaBlend = alphaBlend;
  budgetCandidate.alphaTestEnabled = captureAlphaTest;
  budgetCandidate.depthWriteEnabled = zWriteEnabled;
  budgetCandidate.vertexBlendEnabled = vertexBlendEnabled || vbIndexed;
  budgetCandidate.indexed = indexed;
  budgetCandidate.stage = stage >= 0 ? uint32_t(stage) : 0u;
  budgetCandidate.vertexCount = indexed ? NumVertices : CountVal;
  budgetCandidate.indexCount = indexed ? CountVal : 0u;
  budgetCandidate.viewDepth = estimatedViewDepth;
  budgetCandidate.shadowMaxDistance =
      std::max(captureSettings.shadows.csm.maxDistance, 0.0f);
  budgetCandidate.boundsRadius = estimatedBoundsRadius;

  const bool overWorldFreezeCountCap =
      dxvk::war3::internal::kShadowFallbackFreezeCountCapEnabled &&
      forceFreezeFallbackWorldGeometry &&
      m_war3Scene.shadowStats.forcedFallbackWorldFreezeCount >=
          dxvk::war3::internal::kShadowFallbackWorldFreezeCountCap;
  const bool overUnitFreezeCountCap =
      dxvk::war3::internal::kShadowFallbackFreezeCountCapEnabled &&
      forceFreezeUnitLikeGeometry &&
      m_war3Scene.shadowStats.forcedFallbackUnitFreezeCount >=
          dxvk::war3::internal::kShadowFallbackUnitFreezeCountCap;

  if (overWorldFreezeCountCap || overUnitFreezeCountCap) {
    m_war3Scene.shadowStats.skippedCasterCap++;
    m_war3Scene.shadowStats.fallbackBudgetBytes =
        m_war3ShadowFallbackBudgetCapBytes;
    m_war3Scene.shadowStats.fallbackBudgetUsedBytes =
        m_war3ShadowFallbackBudgetUsedBytes;
    m_war3Scene.shadowStats.fallbackArenaBytes =
        dxvk::war3::memory::ShadowArena_UsedBytes();
    return;
  }

  dxvk::war3::render::ShadowCaptureBudgetPolicy budgetPolicy = {};
  budgetPolicy.hardBudgetBytes = m_war3ShadowFallbackBudgetCapBytes;
  budgetPolicy.usedBudgetBytes = m_war3ShadowFallbackBudgetUsedBytes;
  budgetPolicy.posBytes = fallbackPosBudgetBytes;
  budgetPolicy.blendBytes = fallbackBlendBudgetBytes;
  budgetPolicy.uvBytes = fallbackUvBudgetBytes;
  budgetPolicy.indexBytes = fallbackIndexBudgetBytes;
  budgetPolicy.freezeDynamicEnabled = s_freezeDynamicShadowBuffers;
  budgetPolicy.aggressiveExperimental = false;

  const auto budgetDecision =
      dxvk::war3::render::DecideShadowCaptureBudget(budgetCandidate,
                                                    budgetPolicy);
  const uint64_t requestedFallbackBytes =
      budgetPolicy.posBytes + budgetPolicy.blendBytes + budgetPolicy.uvBytes +
      budgetPolicy.indexBytes;
  const uint64_t predictedFallbackBytes =
      budgetPolicy.usedBudgetBytes + requestedFallbackBytes;

  if (budgetDecision.disableAlphaCapture && captureAlphaTest) {
    captureAlphaTest = false;
    fallbackUvBudgetBytes = 0u;
    budgetPolicy.uvBytes = 0u;
    m_war3Scene.shadowStats.degradedAlphaBudget++;
    m_war3Scene.shadowStats.budgetExceeded = 1u;
    m_war3ShadowFallbackBudgetExceeded = true;
  }

  if (budgetDecision.skipCaster) {
    if (predictedFallbackBytes > budgetPolicy.hardBudgetBytes)
      m_war3Scene.shadowStats.skippedFreezeBudget++;
    else
      m_war3Scene.shadowStats.skippedPriorityBudget++;
    m_war3Scene.shadowStats.budgetExceeded = 1u;
    m_war3Scene.shadowStats.fallbackBudgetBytes =
        m_war3ShadowFallbackBudgetCapBytes;
    m_war3Scene.shadowStats.fallbackBudgetUsedBytes =
        m_war3ShadowFallbackBudgetUsedBytes;
    m_war3Scene.shadowStats.fallbackArenaBytes =
        dxvk::war3::memory::ShadowArena_UsedBytes();
    m_war3ShadowFallbackBudgetExceeded = true;
    return;
  }

  const uint64_t acceptedFallbackBytes =
      budgetPolicy.posBytes + budgetPolicy.blendBytes +
      (captureAlphaTest ? budgetPolicy.uvBytes : 0ull) +
      budgetPolicy.indexBytes;
  m_war3ShadowFallbackBudgetUsedBytes += acceptedFallbackBytes;
  if (m_war3ShadowFallbackBudgetUsedBytes > m_war3ShadowFallbackBudgetCapBytes) {
    m_war3ShadowFallbackBudgetExceeded = true;
    m_war3Scene.shadowStats.budgetExceeded = 1u;
  }
  m_war3Scene.shadowStats.fallbackBudgetBytes =
      m_war3ShadowFallbackBudgetCapBytes;
  m_war3Scene.shadowStats.fallbackBudgetUsedBytes =
      m_war3ShadowFallbackBudgetUsedBytes;
  m_war3Scene.shadowStats.fallbackArenaBytes =
      dxvk::war3::memory::ShadowArena_UsedBytes();

  auto tryFreezeStableUploadSnapshot =
      [&](const DxvkBufferSlice& srcSlice, VkDeviceSize bytes,
          bool requiresStableUploadSource, Rc<DxvkBuffer>& ioAlloc,
          DxvkResourceBufferInfo& ioInfo) -> bool {
    if (!requiresStableUploadSource)
      return false;

    auto srcBuffer = srcSlice.buffer();
    if (srcBuffer == nullptr || bytes == 0)
      return false;
    if ((srcBuffer->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
      return false;

    void* srcPtr = srcBuffer->mapPtr(srcSlice.offset());
    if (srcPtr == nullptr)
      return false;

    VkDeviceSize finalOffset = 0;
    auto frozenAlloc = War3AllocFreezeBuffer(bytes, finalOffset);
    if (frozenAlloc == nullptr)
      return false;

    VkDeviceSize freezeOffset = 0;
    void* snapshotPtr = nullptr;
    auto snapshotAlloc =
        War3AllocFreezeBuffer(bytes, freezeOffset, true, &snapshotPtr);
    if (snapshotAlloc == nullptr || snapshotPtr == nullptr)
      return false;

    std::memcpy(snapshotPtr, srcPtr, static_cast<size_t>(bytes));
    EmitCs([cDst = frozenAlloc, cDstOff = finalOffset, cSrc = snapshotAlloc,
            cSrcOff = freezeOffset, cBytes = bytes](DxvkContext* ctx) {
      ctx->copyBuffer(cDst, cDstOff, cSrc, cSrcOff, cBytes);
    });
    ioAlloc = frozenAlloc;
    ioInfo = frozenAlloc->getSliceInfo(finalOffset, bytes);
    return true;
  };

  if (shouldFreezePosBuffer) {
    VkDeviceSize bytes = posBytesNeeded;
    const bool posRequiresStableUploadSource =
        DynamicSysmemVBOs && m_war3PerDrawUpload.vbValid[posStream];
    if (!tryFreezeStableUploadSnapshot(posSlice, bytes,
                                       posRequiresStableUploadSource, posAlloc,
                                       posInfo)) {
      VkDeviceSize freezeOffset = 0;
      auto frozenAlloc = War3AllocFreezeBuffer(bytes, freezeOffset);
      if (frozenAlloc) {
        auto srcBuffer = posSlice.buffer();
        EmitCs([cDst = frozenAlloc, cDstOff = freezeOffset, cSrc = srcBuffer,
                cSrcOff = posSlice.offset(),
                cBytes = bytes](DxvkContext *ctx) {
          ctx->copyBuffer(cDst, cDstOff, cSrc, cSrcOff, cBytes);
        });
        posAlloc = frozenAlloc;
        posInfo = frozenAlloc->getSliceInfo(freezeOffset, bytes);
      } else {
        WAR3_RENDER_LOG("DEBUG: Freeze POS Failed (Alloc Null)\n");
      }
    }
  }

  if (shouldFreezeBlendBuffer) {
    VkDeviceSize bytes = blendBytesNeeded;
    const bool blendRequiresStableUploadSource =
        DynamicSysmemVBOs && m_war3PerDrawUpload.vbValid[blendStream];
    if (!tryFreezeStableUploadSnapshot(blendSlice, bytes,
                                       blendRequiresStableUploadSource,
                                       blendAlloc, blendInfo)) {
      VkDeviceSize freezeOffset = 0;
      auto frozenAlloc = War3AllocFreezeBuffer(bytes, freezeOffset);
      if (frozenAlloc) {
        auto srcBuffer = blendSlice.buffer();
        EmitCs([cDst = frozenAlloc, cDstOff = freezeOffset, cSrc = srcBuffer,
                cSrcOff = blendSlice.offset(),
                cBytes = bytes](DxvkContext *ctx) {
          ctx->copyBuffer(cDst, cDstOff, cSrc, cSrcOff, cBytes);
        });
        blendAlloc = frozenAlloc;
        blendInfo = frozenAlloc->getSliceInfo(freezeOffset, bytes);
      }
    }
  }

  // [Fix] Capture UV Buffer (if alpha test enabled)
  if (captureAlphaTest) {
    if (uvStream == posStream) {
      uvAlloc = posAlloc;
      uvInfo = posInfo;
    } else if (blendBinding == 1 && uvStream == blendStream && blendAlloc) {
      uvAlloc = blendAlloc;
      uvInfo = blendInfo;
    } else if (uvStream < caps::MaxStreams) {
      if (DynamicSysmemVBOs && m_war3PerDrawUpload.vbValid[uvStream]) {
        uvSlice = m_war3PerDrawUpload.vbSlices[uvStream];
        uvAlloc = uvSlice.buffer();
      } else {
        auto *vb = m_state.vertexBuffers[uvStream].vertexBuffer.ptr();
        if (vb) {
          auto *vbCommon = vb->GetCommonBuffer();
          if (vbCommon) {
            uvSlice = vbCommon->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(
                m_state.vertexBuffers[uvStream].offset);
            uvAlloc = uvSlice.buffer();
          }
        }
      }

      if (uvAlloc) {
        uvInfo = uvSlice.getSliceInfo();
        auto *uvVb = m_state.vertexBuffers[uvStream].vertexBuffer.ptr();
        const auto *uvCommon = uvVb ? uvVb->GetCommonBuffer() : nullptr;
        const bool uvDynamic =
            uvCommon && (uvCommon->Desc()->Usage & D3DUSAGE_DYNAMIC);
        const bool uvRequiresStableUploadSource =
            DynamicSysmemVBOs && m_war3PerDrawUpload.vbValid[uvStream];

  if (s_freezeDynamicShadowBuffers &&
      (DynamicSysmemVBOs || uvDynamic || forceFreezeUnitLikeGeometry ||
       forceFreezeFallbackWorldGeometry)) {
          VkDeviceSize bytes = uvInfo.size;
          if (vertexRangeValid && uvStride != 0) {
            const VkDeviceSize wanted =
                static_cast<VkDeviceSize>(vertexRangeEnd) *
                static_cast<VkDeviceSize>(uvStride);
            if (wanted > 0 && wanted < bytes)
              bytes = wanted;
          }
          if (!tryFreezeStableUploadSnapshot(uvSlice, bytes,
                                             uvRequiresStableUploadSource,
                                             uvAlloc, uvInfo)) {
            VkDeviceSize freezeOffset = 0;
            auto frozenAlloc = War3AllocFreezeBuffer(bytes, freezeOffset);
            if (frozenAlloc) {
              auto srcBuffer = uvSlice.buffer();
              EmitCs([cDst = frozenAlloc, cDstOff = freezeOffset,
                      cSrc = srcBuffer, cSrcOff = uvSlice.offset(),
                      cBytes = bytes](DxvkContext *ctx) {
                ctx->copyBuffer(cDst, cDstOff, cSrc, cSrcOff, cBytes);
              });
              uvAlloc = frozenAlloc;
              uvInfo = frozenAlloc->getSliceInfo(freezeOffset, bytes);
            }
          }
        }
      } else {
        captureAlphaTest = false;
      }
    }
  }

  if (shouldFreezeIndexBuffer) {
    VkDeviceSize bytes = idxBytesNeeded;
    const bool idxRequiresStableUploadSource =
        DynamicSysmemIBO && m_war3PerDrawUpload.ibValid;
    if (!tryFreezeStableUploadSnapshot(idxSlice, bytes,
                                       idxRequiresStableUploadSource, idxAlloc,
                                       idxInfo)) {
      VkDeviceSize freezeOffset = 0;
      auto frozenAlloc = War3AllocFreezeBuffer(bytes, freezeOffset);
      if (frozenAlloc) {
        auto srcBuffer = idxSlice.buffer();
        EmitCs([cDst = frozenAlloc, cDstOff = freezeOffset, cSrc = srcBuffer,
                cSrcOff = idxSlice.offset(),
                cBytes = bytes](DxvkContext *ctx) {
          ctx->copyBuffer(cDst, cDstOff, cSrc, cSrcOff, cBytes);
        });
        idxAlloc = frozenAlloc;
        idxInfo = frozenAlloc->getSliceInfo(freezeOffset, bytes);
      } else {
        WAR3_RENDER_LOG("DEBUG: Freeze IDX Failed (Alloc Null)\n");
      }
    }
  }

  noteDynamicPoseUsage(dynamicShadowSource);

  War3ShadowCasterDraw draw = {};
  draw.indexed = indexed;
  draw.positionStorage = std::move(posAlloc);
  draw.positionInfo = posInfo;
  draw.positionStride = posStride;
  draw.positionOffset = declInfo.posOffset;
  draw.positionFormat = posFormat;

  if (indexed) {
    draw.indexStorage = std::move(idxAlloc);
    draw.indexInfo = idxInfo;
    draw.indexType = indexType;
    draw.indexCount = CountVal;
    draw.firstIndex = StartVal;
    draw.vertexOffset = BaseVertexIndex;
    draw.vertexCount = 0;
    draw.firstVertex = 0;
    draw.minVertexIndex = MinVertexIndex;
    draw.numVertices = NumVertices;
  } else {
    draw.vertexCount = CountVal;
    draw.firstVertex = StartVal;
    draw.indexCount = 0;
    draw.firstIndex = 0;
    draw.vertexOffset = 0;
    draw.minVertexIndex = StartVal;
    draw.numVertices = CountVal;
  }

  draw.topology = topo;
  draw.worldMatrix = shadowWorldMatrix;
  draw.vertexBlendEnabled = vertexBlendEnabled;
  draw.vertexBlendIndexed = vbIndexed;
  draw.vertexBlendCount = uint8_t(vbCount);
  draw.paletteIndex = paletteIndex;
  draw.blendWeightOffset = blendWeightOffset;
  draw.blendWeightFormat = blendWeightFormat;
  draw.blendIndexOffset = blendIndexOffset;
  draw.blendIndexFormat = blendIndexFormat;
  draw.blendBinding = blendBinding;
  draw.blendStride = blendStride;
  if (blendBinding == 1) {
    draw.blendStorage = std::move(blendAlloc);
    draw.blendInfo = blendInfo;
  }

  // ===== Alpha测试阴影字段 =====
  draw.alphaTestEnabled = captureAlphaTest;
  draw.alphaRef = alphaRefFloat;
  if (captureAlphaTest) {
    draw.uvOffset = uvOffset;
    draw.uvStride = uvStride;
    draw.uvFormat = uvFormat;
    // UV通常和Position在同一个缓冲区，共享存储
    // 注意：这里假设了UV在Stream 0。如果是其他Stream，UV数据可能错乱。
    // 但现有逻辑似乎也只处理Stream 0的情况。
    draw.uvStride = uvStride;
    draw.uvFormat = uvFormat;
    // [Fix] 使用正确捕获的 UV Buffer (不再硬编码为 PositionStorage)
    draw.uvStorage = std::move(uvAlloc);
    draw.uvInfo = uvInfo;
    draw.diffuseTexture = std::move(diffuseTexView);

    // 捕获采样器 (用于获取 Bindless Index)
    bool alphaUseMip = false;
    float alphaMipLodBias = 0.0f;
    if (m_war3Pipeline) {
      const auto &shadowSettings = m_war3Pipeline->GetSettings().shadows;
      alphaUseMip = shadowSettings.alphaShadowUseMip;
      alphaMipLodBias = shadowSettings.alphaShadowMipLodBias;
    }
    draw.diffuseSampler =
        m_shadowReceiverPass->getFallbackSampler(alphaUseMip, alphaMipLodBias);
    draw.diffuseSamplerIndex =
        draw.diffuseSampler->getDescriptor().samplerIndex;

    // [Route B] 仅拷贝 Image Info (用于绑定 SAMPLED_IMAGE)
    // 不需要注入 Sampler Object，因为 Shader 使用 Bindless Samplers
    draw.textureDescriptor = *draw.diffuseTexture->getDescriptor();
    // (可选) 为了安全，可以把 samplerObject 设为 Null 或不管它，因为
    // descriptorType 将是 SAMPLED_IMAGE
  }

  draw.alphaBlendEnabled = alphaBlend;
  draw.depthWriteEnabled = zWriteEnabled;
  draw.depthTestEnabled = zTestEnabled;
  draw.additiveBlend = additiveBlend;
  finalizeShadowDrawCommon(draw);
  if (forceFreezeFallbackWorldGeometry) {
    m_war3Scene.shadowStats.forcedFallbackWorldFreezeCount++;
  }
  if (forceFreezeUnitLikeOrHint &&
      resolvedObjectKind ==
          static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Unit)) {
    m_war3Scene.shadowStats.forcedFallbackUnitFreezeCount++;
  }
  m_war3Scene.shadowFallbacks.push_back(
      {draw,
       "legacy-freeze",
       War3ShadowReplayMode::SnapshotFallback,
       draw.batchHandle,
       semantic.worldObjectEntry,
       semantic.sceneNode,
       semantic.runtimeModelPtr,
       semantic.modelKey});
  m_war3Scene.shadowStats.fallbackSnapshotCount++;
  War3RecomputeFallbackBreakdown(m_war3Scene);
  m_war3Scene.shadowStats.fallbackArenaBytes =
      dxvk::war3::memory::ShadowArena_UsedBytes();
  m_war3Scene.shadowCasters.emplace_back(std::move(draw));
  m_war3Scene.shadowStats.captured++;
  if (indexed)
    m_war3Scene.shadowStats.capturedIndexed++;
  else
    m_war3Scene.shadowStats.capturedNonIndexed++;
  if (terrainCaster)
    m_war3Scene.shadowStats.capturedTerrain++;
  if (objectCaster)
    m_war3Scene.shadowStats.capturedWorldObject++;
  if (resolvedObjectKind ==
      static_cast<uint8_t>(dxvk::war3::render::ObjectKind::Unit)) {
    m_war3Scene.shadowStats.capturedUnitObject++;
    if (vertexBlendEnabled || vbIndexed)
      m_war3Scene.shadowStats.capturedUnitVertexBlend++;
  }
}
} // namespace dxvk
