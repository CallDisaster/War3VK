#include "war3_device_semantic_predicates.h"
#include "../debug/war3_shadow_build_context_trace.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../hooks/war3_hook_widget_identity.h"
#include "../render/war3_render_objects.h"
#include "../render/war3_shadow_object_registry.h"
#include "../render/war3_visible_renderables.h"
#include "../render/war3_current_draw_contract.h"
#include "../../../util/util_env.h"
#include "../../../util/util_bit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace dxvk::war3::semantic {

// ---------------------------------------------------------------------------
// 运行时配置 getter（含共享的 env 读取原语）
// ---------------------------------------------------------------------------
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

bool War3SemanticValidateUnitCoreRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_VALIDATE_UNIT_CORE", 1u) != 0u;
  return s_enabled;
}

bool War3SemanticObjectGroupedSelectionRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_OBJECT_GROUPED_SELECTION", 1u) != 0u;
  return s_enabled;
}

bool War3SemanticStickySelectionLeaseRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE", 1u) != 0u;
  return s_enabled;
}

uint32_t War3SemanticStickySelectionLeaseFramesRuntime() {
  static const uint32_t s_frames = std::max<uint32_t>(
      1u, War3GetEnvU32("DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE_FRAMES",
                        120u));
  return s_frames;
}

bool War3SemanticStickySelectionBroadLeasePreferenceRuntime() {
  static const bool s_enabled =
      War3GetEnvU32(
          "DXVK_WAR3_SEMANTIC_STICKY_SELECTION_BROAD_LEASE_PREFERENCE",
          0u) != 0u;
  return s_enabled;
}

bool War3SemanticStickySelectionFillRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL", 1u) != 0u;
  return s_enabled;
}

uint32_t War3SemanticStickySelectionFillMarginRuntime() {
  static const uint32_t s_margin = std::min<uint32_t>(
      64u,
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL_MARGIN", 8u));
  return s_margin;
}

bool War3SemanticStickyPartSelectionRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION", 1u) != 0u;
  return s_enabled;
}

uint32_t War3SemanticStickyPartSelectionMinRecordsRuntime() {
  static const uint32_t s_minRecords = std::max<uint32_t>(
      1u,
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION_MIN_RECORDS",
                    16u));
  return s_minRecords;
}

bool War3SemanticRejectUnsafeAlphaCasterRuntime() {
  // Phase 7.34 AlphaTest 链路已由 Claude heartbeat 2026-05-12 打通：
  // stash（War3TryCaptureShadowCaster 抓 UV/diffuse/alphaRef）→
  // lookup（War3TryAppendSemanticShadowPacket 查 cache）→
  // apply（candidate.alphaTestEnabled + UV/diffuse 注入 draw）。
  // 当 payload 存在时 reject helper 会放行合法 cutout；payload 缺失时仍安全拒绝。
  // 真正的 AlphaBlend 由独立的 fail-closed gate 管理，不能因为 payload 完整就被
  // 强制转换成二值 alpha-test caster。
  // 如果 cutout 仍出现方形卡片回归，可设
  // DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER=1 诊断回退。
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER", 0u) != 0u;
  return s_enabled;
}

bool War3SemanticRejectAlphaBlendCasterRuntime() {
  // A complete UV/texture payload is sufficient to reproduce a binary
  // alpha-test (cutout) material, but it does not turn a true alpha-blended
  // visual layer into a valid shadow caster. Treating AlphaBlend as Cutout
  // produces the large hard-edged effect cards seen for transient spell and
  // particle geosets. Keep the unsafe behavior available only as an explicit
  // diagnostic rollback.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_REJECT_ALPHA_BLEND_CASTER", 1u) != 0u;
  return s_enabled;
}

bool War3WidgetProbeSafeCopyRuntime() {
  // VirtualQuery dominates the anonymous world-object fallback: most pointers
  // are readable render objects but not CWidget instances, so every draw pays
  // a kernel query only to reject the magic. ReadProcessMemory provides a
  // fault-safe, fail-closed 4-byte snapshot without a check-then-dereference
  // window. Keep the old VirtualQuery path for same-DLL A/B and emergency
  // rollback.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_WIDGET_PROBE_SAFE_COPY", 1u) != 0u;
  return s_enabled;
}

bool War3WidgetNegativeFrameCacheRuntime() {
  // Suppress repeated negative magic probes for the exact same pointer within
  // the short TTL below. The positive identity cache is still queried first on
  // every draw, so a later hook publication wins. The fixed direct-mapped table
  // verifies the full pointer; collisions are misses, never false hits.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_WIDGET_NEGATIVE_FRAME_CACHE", 1u) != 0u;
  return s_enabled;
}

uint64_t War3WidgetNegativeFrameCacheTtlFrames() {
  // Keep a deliberately short cross-frame TTL for stable non-CWidget render
  // pointers. The positive identity cache is still checked first every draw,
  // so a hook publication wins immediately; eight frames also bounds a rare
  // pointer-reuse miss to a small fraction of a second. The runtime override
  // retains 1 as the exact same-frame-only rollback.
  static const uint64_t s_ttl = std::clamp<uint32_t>(
      War3GetEnvU32("DXVK_WAR3_WIDGET_NEGATIVE_CACHE_TTL_FRAMES", 8u),
      1u, 120u);
  return s_ttl;
}

// ---------------------------------------------------------------------------
// 选择键构造：记录级 selector（需要 render 层 registry）
// ---------------------------------------------------------------------------
uint64_t War3SemanticDirectRecordSelectionKey(
    const dxvk::war3::render::CurrentDrawContractRecord& record,
    const dxvk::war3::render::VisibleRenderableRecord** outVisibleHint,
    dxvk::war3::render::VisibleRenderablePartLayerQueryCache*
        visibleQueryCache) {
  if (outVisibleHint != nullptr)
    *outVisibleHint = nullptr;
  auto ptrValue = [](const void* ptr) -> uint64_t {
    return uint64_t(reinterpret_cast<uintptr_t>(ptr));
  };
  auto makeKey = [](uint32_t tag, uint64_t value) -> uint64_t {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = bit::fnv1a_iter(hash, tag);
    hash = bit::fnv1a_iter(hash, value);
    return hash;
  };

  // Record-level preselection runs before War3TryBuildShadowPacketFromCurrentDrawRecord()
  // has a chance to merge VisibleRenderable identity into the packet. If we
  // group by the raw producer record here but later hash submitted objects by
  // packet.unitPtr, the temporal lease never matches and the selected caster set
  // churns every frame under cap pressure. Resolve only the current renderable
  // record here; this is bounded by the direct scan cap, not every draw hook.
  if (record.renderablePart != nullptr) {
    const auto& registry =
        dxvk::war3::render::VisibleRenderableRegistry::instance();
    dxvk::war3::render::VisibleRenderableRecord visibleStorage = {};
    const dxvk::war3::render::VisibleRenderableRecord* visible = nullptr;
    if (visibleQueryCache != nullptr) {
      visible = visibleQueryCache->queryPtr(
          registry, record.renderablePart, record.layerIndex);
    } else if (registry.queryByRenderablePartAndLayer(
                   record.renderablePart, record.layerIndex,
                   visibleStorage)) {
      visible = &visibleStorage;
    }
    if (visible != nullptr) {
      // A pointer can be handed off only when the caller supplied the
      // Populate-local cache owner. It must be copied before the next query.
      if (outVisibleHint != nullptr && visibleQueryCache != nullptr)
        *outVisibleHint = visible;
      if (visible->identity.jHandle != 0u)
        return makeKey(2u, visible->identity.jHandle);
      if (visible->identity.handleId != 0u)
        return makeKey(2u, visible->identity.handleId | 0x100000u);
      if (visible->identity.unitPtr != nullptr)
        return makeKey(1u, ptrValue(visible->identity.unitPtr));
      if (visible->identity.worldObjectEntry != nullptr)
        return makeKey(4u, ptrValue(visible->identity.worldObjectEntry));
      if (visible->sceneNode != nullptr)
        return makeKey(5u, ptrValue(visible->sceneNode));
    }
  }

  if (record.jHandle != 0u)
    return makeKey(2u, record.jHandle);
  if (record.unitPtr != nullptr)
    return makeKey(1u, ptrValue(record.unitPtr));
  if (record.worldObjectEntry != nullptr)
    return makeKey(4u, ptrValue(record.worldObjectEntry));
  if (record.sceneNode != nullptr)
    return makeKey(5u, ptrValue(record.sceneNode));
  if (record.meshPayloadPtr != nullptr) {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = bit::fnv1a_iter(hash, 6u);
    hash = bit::fnv1a_iter(hash, ptrValue(record.meshPayloadPtr));
    hash = bit::fnv1a_iter(hash, record.payloadWord108);
    hash = bit::fnv1a_iter(hash, record.payloadWord11C);
    return hash;
  }
  if (record.renderablePart != nullptr)
    return makeKey(7u, ptrValue(record.renderablePart));
  return 0u;
}

// ---------------------------------------------------------------------------
// 对象类型裁决 / 单元证据
// ---------------------------------------------------------------------------
namespace {

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

} // namespace

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

bool War3SemanticDirectPacketMatchesMainWorldVisibleRecord(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    const dxvk::war3::render::VisibleRenderableRecord& visible,
    War3SemanticDirectMainWorldBackingStatus* outStatus) {
  auto setStatus = [&](War3SemanticDirectMainWorldBackingStatus status) {
    if (outStatus != nullptr)
      *outStatus = status;
  };
  const auto& renderable = packet.renderable;
  if (renderable.renderablePart == nullptr) {
    setStatus(War3SemanticDirectMainWorldBackingStatus::NoRenderablePart);
    return false;
  }

  if (visible.queueKind !=
      dxvk::war3::render::VisibleRenderableQueueKind::MainQueue) {
    setStatus(War3SemanticDirectMainWorldBackingStatus::NonMainQueue);
    return false;
  }
  if (visible.identity.groupIdx > 0) {
    setStatus(War3SemanticDirectMainWorldBackingStatus::NonWorldGroup);
    return false;
  }

  const auto& identity = visible.identity;
  bool identityMatches = false;
  if (renderable.unitPtr != nullptr && identity.unitPtr != nullptr)
    identityMatches = renderable.unitPtr == identity.unitPtr;
  if (!identityMatches && renderable.worldObjectEntry != nullptr &&
      identity.worldObjectEntry != nullptr)
    identityMatches =
        renderable.worldObjectEntry == identity.worldObjectEntry;
  if (!identityMatches && renderable.jHandle != 0u) {
    identityMatches = identity.jHandle == renderable.jHandle ||
                      identity.handleId == renderable.jHandle;
  }

  // Rawcode-only matches are intentionally not accepted here. Portrait/model
  // preview draws can share the selected unit rawcode while using a separate
  // UI/preview scene node; the world shadow path needs a live object identity.
  if (!identityMatches) {
    setStatus(War3SemanticDirectMainWorldBackingStatus::IdentityMismatch);
    return false;
  }

  if (visible.sceneNode != nullptr && renderable.sceneNode != nullptr &&
      visible.sceneNode != renderable.sceneNode) {
    setStatus(War3SemanticDirectMainWorldBackingStatus::SceneNodeMismatch);
    return false;
  }
  if (visible.meshData != nullptr && renderable.meshData != nullptr &&
      visible.meshData != renderable.meshData) {
    setStatus(War3SemanticDirectMainWorldBackingStatus::MeshDataMismatch);
    return false;
  }

  setStatus(War3SemanticDirectMainWorldBackingStatus::Pass);
  return true;
}

bool War3SemanticDirectPacketHasMainWorldVisibleBacking(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    War3SemanticDirectMainWorldBackingStatus* outStatus) {
  const auto& renderable = packet.renderable;
  if (renderable.renderablePart == nullptr) {
    if (outStatus != nullptr) {
      *outStatus =
          War3SemanticDirectMainWorldBackingStatus::NoRenderablePart;
    }
    return false;
  }

  dxvk::war3::render::VisibleRenderableRecord visible = {};
  if (!dxvk::war3::render::VisibleRenderableRegistry::instance()
           .queryByRenderablePartAndLayer(renderable.renderablePart,
                                          renderable.layerIndex, visible)) {
    if (outStatus != nullptr)
      *outStatus = War3SemanticDirectMainWorldBackingStatus::LookupMiss;
    return false;
  }

  return War3SemanticDirectPacketMatchesMainWorldVisibleRecord(
      packet, visible, outStatus);
}

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

bool War3SemanticMaterialIsSafeOpaqueWorldCaster(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  const auto& material = packet.material;
  if (War3SemanticRejectAlphaBlendCasterRuntime() &&
      material.alphaMode == dxvk::war3::shadow::ShadowAlphaMode::AlphaBlend) {
    return false;
  }

  if (!War3SemanticRejectUnsafeAlphaCasterRuntime())
    return true;

  if (!material.valid() || !material.layerContractResolved)
    return false;

  if (material.queueKind !=
      uint32_t(dxvk::war3::render::VisibleRenderableQueueKind::MainQueue)) {
    return false;
  }

  return material.alphaMode == dxvk::war3::shadow::ShadowAlphaMode::Opaque &&
         material.blendOrDrawMode == 0u;
}

bool War3IsEligibleSemanticStaticWorldCaster(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind,
    bool hasRenderableGeoset, bool hasPacketGeometry) {
  if (!dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() ||
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly) {
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

  if (!War3SemanticMaterialIsSafeOpaqueWorldCaster(packet))
    return false;

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
  // Phase 7.72/7.98：路径阻断器在 eligibility 层就拦掉，避免上游 producer
  // 还要继续走完整 packet 构建。
  //   - rawcode 已知（>0）：直接判定（O(1)）。
  //   - rawcode 未知（==0）但 jHandle 已知：兜底通过 widget identity cache
  //     反查（destructible 走 CWidget_RegisterFootprintAndShadowMask hook
  //     注册路径，不进 Hook_WorldObjects_RenderGroup，单纯 RenderObjectRegistry
  //     抓不到）。
  //   - 2026-05-31 根因修复：rawcode==0 且 jHandle 兜底也 miss 时，再从
  //     worldObjectEntry / unitPtr widget 指针直读 +0x0C(magic)/+0x30(rawcode)。
  //     这是堵死 explicitUnknownRigid 漏网的关键——path blocker 的匿名 rigid
  //     packet 此前从这里漏过去被当成"未知 world caster"提交。
  // Phase 7.73：拒绝时累加全局原子计数，由 D3D9DeviceEx 在 caster reset 时
  // 折进 shadowStats，让 trace 看到 eligibility 层的命中量。
  if (dxvk::war3::internal::kPathBlockerHideEnabled) {
    if (War3PacketIsPathBlocker(packet)) {
      g_pathBlockerEligibilityGateRejectCount.fetch_add(
          1u, std::memory_order_relaxed);
      return false;
    }
  }
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
      hasPacketGeometry &&
      War3SemanticMaterialIsSafeOpaqueWorldCaster(packet) &&
      War3SemanticDirectPacketHasMainWorldVisibleBacking(packet);

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

// ---------------------------------------------------------------------------
// path blocker 证据判定
// ---------------------------------------------------------------------------
bool IsLosBlockerFourCc(uint32_t rawcode) {
  return dxvk::war3::internal::IsPathBlockerFourCc(rawcode);
}

bool War3ShadowIsLosBlockerByJHandleFallback(uint32_t jHandle) {
  if (jHandle == 0u)
    return false;
  // 1) 每帧 RenderObjectRegistry（命中走 group 0/1/2 的对象）。
  if (const auto* info = dxvk::war3::render::RenderObjectRegistry::instance()
                              .findByHandle(jHandle)) {
    if (info->rawcode != 0u)
      return IsLosBlockerFourCc(info->rawcode);
  }
  // 2) widget identity cache（destructible 永久身份）。
  const uint32_t cachedRawcode =
      dxvk::war3::hooks::QueryWidgetRawcodeByHandle(jHandle);
  return cachedRawcode != 0u && IsLosBlockerFourCc(cachedRawcode);
}

namespace {

struct War3WidgetNegativeFrameCacheEntry {
  const void* widgetPtr = nullptr;
  uint64_t frameSerial = 0u;
};

War3WidgetNegativeFrameCacheEntry& War3WidgetNegativeFrameCacheSlot(
    const void* widgetPtr) {
  static thread_local std::array<War3WidgetNegativeFrameCacheEntry, 512u>
      s_entries = {};
  const uintptr_t key = reinterpret_cast<uintptr_t>(widgetPtr);
  const size_t index = static_cast<size_t>(
      ((key >> 4u) ^ (key >> 13u) ^ (key >> 21u)) &
      (s_entries.size() - 1u));
  return s_entries[index];
}

bool War3WidgetNegativeFrameCacheHit(
    const void* widgetPtr, uint64_t frameSerial) {
  if (!War3WidgetNegativeFrameCacheRuntime() || widgetPtr == nullptr ||
      frameSerial == 0u) {
    return false;
  }
  const auto& entry = War3WidgetNegativeFrameCacheSlot(widgetPtr);
  if (entry.widgetPtr != widgetPtr || frameSerial < entry.frameSerial)
    return false;
  return (frameSerial - entry.frameSerial) <
      War3WidgetNegativeFrameCacheTtlFrames();
}

void War3NoteWidgetNegativeFrameCache(
    const void* widgetPtr, uint64_t frameSerial) {
  if (!War3WidgetNegativeFrameCacheRuntime() || widgetPtr == nullptr ||
      frameSerial == 0u) {
    return;
  }
  auto& entry = War3WidgetNegativeFrameCacheSlot(widgetPtr);
  entry.widgetPtr = widgetPtr;
  entry.frameSerial = frameSerial;
}

} // namespace

bool War3ShadowIsLosBlockerByWidgetPtr(
    void* widgetPtr, uint32_t jHandleForCache,
    bool traceWorldWidgetProbe,
    uint64_t negativeCacheFrameSerial) {
  if (traceWorldWidgetProbe) {
    War3EnterShadowBuildContextPhase(
        War3ShadowBuildContextPhase::PathBlockerWorldCache);
  }
  if (widgetPtr == nullptr)
    return false;
  // 先查 widget identity cache（可能别的 path 已 NoteWidgetIdentityFromDrawcall）。
  const uint32_t cached =
      dxvk::war3::hooks::QueryWidgetRawcodeByPtr(widgetPtr);
  if (cached != 0u)
    return IsLosBlockerFourCc(cached);
  if (traceWorldWidgetProbe) {
    War3EnterShadowBuildContextPhase(
        War3ShadowBuildContextPhase::PathBlockerWorldNegativeCache);
  }
  if (War3WidgetNegativeFrameCacheHit(widgetPtr, negativeCacheFrameSerial))
    return false;
  // miss → 直读 widget+0x0C(magic) + +0x30(rawcode)。
  if (traceWorldWidgetProbe) {
    War3EnterShadowBuildContextPhase(
        War3ShadowBuildContextPhase::PathBlockerWorldMagicRead);
  }
  const uintptr_t widgetAddress = reinterpret_cast<uintptr_t>(widgetPtr);
  if (widgetAddress > std::numeric_limits<uintptr_t>::max() - 0x30u)
    return false;
  uint32_t magic = 0u;
  const bool magicReadable = War3WidgetProbeSafeCopyRuntime()
      ? dxvk::war3::SafeCopy(
            &magic, reinterpret_cast<const void*>(widgetAddress + 0x0Cu),
            sizeof(magic))
      : dxvk::war3::SafeReadU32Fast(widgetPtr, 0x0Cu, magic);
  if (!magicReadable)
    return false;
  if (magic != 0x2B5DB42Cu) {
    War3NoteWidgetNegativeFrameCache(widgetPtr, negativeCacheFrameSerial);
    return false;
  }
  if (traceWorldWidgetProbe) {
    War3EnterShadowBuildContextPhase(
        War3ShadowBuildContextPhase::PathBlockerWorldRawcodeRead);
  }
  uint32_t rawcode = 0u;
  const bool rawcodeReadable = War3WidgetProbeSafeCopyRuntime()
      ? dxvk::war3::SafeCopy(
            &rawcode, reinterpret_cast<const void*>(widgetAddress + 0x30u),
            sizeof(rawcode))
      : dxvk::war3::SafeReadU32Fast(widgetPtr, 0x30u, rawcode);
  if (!rawcodeReadable || rawcode == 0u)
    return false;
  // Write-through：让 widget cache 接管后续 O(1) 查询。
  if (traceWorldWidgetProbe) {
    War3EnterShadowBuildContextPhase(
        War3ShadowBuildContextPhase::PathBlockerWorldWriteThrough);
  }
  dxvk::war3::hooks::NoteWidgetIdentityFromDrawcall(widgetPtr, rawcode,
                                                    jHandleForCache);
  return IsLosBlockerFourCc(rawcode);
}

// 统一 packet 级 path blocker 判定（覆盖 rawcode / jHandle / widget 直读三通道）。
// 用于 eligibility 层堵死 explicitUnknownRigid / static-world 漏网。
bool War3PacketIsPathBlocker(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  const auto& r = packet.renderable;
  if (r.pathBlocker)
    return true;
  if (r.rawcode != 0u)
    return IsLosBlockerFourCc(r.rawcode);
  // rawcode==0：jHandle 兜底（registry + widget cache）。
  if (r.jHandle != 0u && War3ShadowIsLosBlockerByJHandleFallback(r.jHandle))
    return true;
  // 仍未命中：widget 指针直读（worldObjectEntry / unitPtr）。
  if (War3ShadowIsLosBlockerByWidgetPtr(r.worldObjectEntry, r.jHandle))
    return true;
  if (r.unitPtr != r.worldObjectEntry &&
      War3ShadowIsLosBlockerByWidgetPtr(r.unitPtr, r.jHandle))
    return true;
  return false;
}

// Phase 7.73：eligibility gate 拒绝路径阻断器的全局计数（atomic relaxed）。
// 该函数定义在 war3/semantic module，没有 D3D9DeviceEx 上下文，因此用全局
// 原子计数。D3D9DeviceEx 每帧 reset shadow scene 时把累加值移入 shadowStats。
std::atomic<uint32_t> g_pathBlockerEligibilityGateRejectCount{0u};

} // namespace dxvk::war3::semantic
