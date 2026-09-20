#pragma once

// M1（device.cpp 语义职责迁移第一步）：无 device 成员依赖的纯判定与常量。
// 迁出范围：对象类型裁决、提交闸、选择键构造、帧优先策略、path blocker 证据判定、
//           运行时配置 getter（War3SemanticSticky*Runtime 一类）。
// 方案见 docs/plan/2026-09-16-device-semantic-responsibility-migration.md §3 M1。
//
// 行为约束：本模块只搬运 d3d9_device.cpp 中原有的定义，判定结果、判定顺序与计数
// 口径逐点不变；d3d9_device.cpp 只保留调用点。纯判定以 inline 定义在本头文件中，
// 因此宿主机单测可以直接包含本头文件（见 tests/）。

#include "../render/war3_render_objects.h"
#include "../render/war3_visible_renderables.h"
#include "../render/war3_current_draw_contract.h"
#include "../shadow/war3_shadow_renderer_core.h"
#include "../../../util/util_bit.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace dxvk::war3::semantic {

// ---------------------------------------------------------------------------
// 选择键构造（纯身份 -> key 哈希）
// ---------------------------------------------------------------------------
enum class War3SemanticDirectSelectionKeySource : uint32_t {
  None = 0,
  UnitPtr,
  JHandle,
  RuntimeModel,
  WorldObject,
  SceneNode,
  ModelMesh,
  RenderablePart,
};

inline uint64_t War3SemanticDirectSelectionKey(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    const dxvk::war3::render::CurrentDrawAuthoritativeSample& sample,
    War3SemanticDirectSelectionKeySource* outSource = nullptr) {
  auto setSource = [&](War3SemanticDirectSelectionKeySource source) {
    if (outSource != nullptr)
      *outSource = source;
  };
  auto ptrValue = [](const void* ptr) -> uint64_t {
    return uint64_t(reinterpret_cast<uintptr_t>(ptr));
  };
  auto makeKey = [](uint32_t tag, uint64_t value) -> uint64_t {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = bit::fnv1a_iter(hash, tag);
    hash = bit::fnv1a_iter(hash, value);
    return hash;
  };

  const auto& renderable = packet.renderable;
  const auto& contract = sample.contract;

  // Prefer true object/runtime identity over sceneNode. A single visible unit can
  // expose multiple scene nodes or child runtime parts, and grouping by those
  // draw-local nodes reintroduces cap-boundary flicker.
  if (renderable.jHandle != 0u) {
    setSource(War3SemanticDirectSelectionKeySource::JHandle);
    return makeKey(2u, renderable.jHandle);
  }
  if (contract.jHandle != 0u) {
    setSource(War3SemanticDirectSelectionKeySource::JHandle);
    return makeKey(2u, contract.jHandle);
  }
  if (renderable.unitPtr != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::UnitPtr);
    return makeKey(1u, ptrValue(renderable.unitPtr));
  }
  if (contract.unitPtr != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::UnitPtr);
    return makeKey(1u, ptrValue(contract.unitPtr));
  }
  if (renderable.runtimeModelPtr != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::RuntimeModel);
    return makeKey(3u, ptrValue(renderable.runtimeModelPtr));
  }
  if (renderable.worldObjectEntry != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::WorldObject);
    return makeKey(4u, ptrValue(renderable.worldObjectEntry));
  }
  if (contract.worldObjectEntry != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::WorldObject);
    return makeKey(4u, ptrValue(contract.worldObjectEntry));
  }
  if (renderable.sceneNode != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::SceneNode);
    return makeKey(5u, ptrValue(renderable.sceneNode));
  }
  if (contract.sceneNode != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::SceneNode);
    return makeKey(5u, ptrValue(contract.sceneNode));
  }
  if (renderable.modelResourcePtr != nullptr && renderable.meshData != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::ModelMesh);
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = bit::fnv1a_iter(hash, 6u);
    hash = bit::fnv1a_iter(hash, ptrValue(renderable.modelResourcePtr));
    hash = bit::fnv1a_iter(hash, ptrValue(renderable.meshData));
    return hash;
  }
  if (renderable.renderablePart != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::RenderablePart);
    return makeKey(7u, ptrValue(renderable.renderablePart));
  }
  if (contract.renderablePart != nullptr) {
    setSource(War3SemanticDirectSelectionKeySource::RenderablePart);
    return makeKey(7u, ptrValue(contract.renderablePart));
  }
  setSource(War3SemanticDirectSelectionKeySource::None);
  return 0u;
}

uint64_t War3SemanticDirectRecordSelectionKey(
    const dxvk::war3::render::CurrentDrawContractRecord& record,
    const dxvk::war3::render::VisibleRenderableRecord** outVisibleHint = nullptr,
    dxvk::war3::render::VisibleRenderablePartLayerQueryCache*
        visibleQueryCache = nullptr);

// ---------------------------------------------------------------------------
// 对象类型裁决（纯部分：rawcode / 部件资源 / 包级资源形状）
// ---------------------------------------------------------------------------
inline bool War3IsSemanticUnitObject(
    dxvk::war3::render::ObjectKind objectKind) {
  return objectKind == dxvk::war3::render::ObjectKind::Unit;
}

constexpr uint32_t War3SemanticByteSwapU32(uint32_t v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

inline bool War3SemanticFourCcHasPrefix(uint32_t rawcode, char c0, char c1) {
  const auto matches = [=](uint32_t value) {
    return ((value >> 24) & 0xFFu) == static_cast<uint8_t>(c0) &&
           ((value >> 16) & 0xFFu) == static_cast<uint8_t>(c1);
  };
  return matches(rawcode) || matches(War3SemanticByteSwapU32(rawcode));
}

inline bool War3SemanticFourCcEqualEitherOrder(uint32_t a, uint32_t b) {
  if (a == b)
    return true;
  return a != 0u && b != 0u && War3SemanticByteSwapU32(a) == b;
}

inline bool War3SemanticRawcodeLooksStaticWorldCaster(uint32_t rawcode) {
  if (rawcode == 0u)
    return false;

  // Trees/pathing doodads such as LTbr/YTxx can share CWidget-like offsets
  // with CUnit and were observed entering the skinned unit path as obj=Unit.
  // Keep this as a surgical reject list instead of broad rawcode class guesses.
  return War3SemanticFourCcHasPrefix(rawcode, 'L', 'T') ||
         War3SemanticFourCcHasPrefix(rawcode, 'Y', 'T');
}

inline bool War3SemanticPacketHasStableUnitResource(
    const dxvk::war3::shadow::ShadowDrawPacket& packet) {
  const auto& renderable = packet.renderable;
  return renderable.runtimeModelPtr != nullptr &&
         (renderable.modelResourcePtr != nullptr || renderable.modelKey != 0u ||
          packet.resource.modelResourcePtr != nullptr ||
          packet.resource.modelKey != 0u);
}

bool War3SemanticReadUnitCore(
    const dxvk::war3::shadow::ShadowRenderableRecord& renderable,
    uint32_t& outRawcode, uint32_t& outFlags5C, void*& outSpritePtr);
bool War3SemanticPacketHasConsistentUnitCore(
    const dxvk::war3::shadow::ShadowDrawPacket& packet);
bool War3SemanticValidateUnitCoreRuntime();
bool War3IsEligibleSemanticDynamicUnit(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind);
bool War3HasSemanticDynamicUnitEvidence(
    const dxvk::war3::shadow::ShadowDrawPacket& packet);
dxvk::war3::render::ObjectKind War3ResolveSemanticPacketObjectKind(
    const dxvk::war3::shadow::ShadowRenderableRecord& renderable);
dxvk::war3::render::ObjectKind War3ResolveSemanticPacketObjectKindFast(
    const dxvk::war3::shadow::ShadowDrawPacket& packet);

// ---------------------------------------------------------------------------
// main-world 可见 backing 裁决
// ---------------------------------------------------------------------------
enum class War3SemanticDirectMainWorldBackingStatus : uint8_t {
  NotChecked = 0,
  Pass,
  NoRenderablePart,
  LookupMiss,
  NonMainQueue,
  NonWorldGroup,
  IdentityMismatch,
  SceneNodeMismatch,
  MeshDataMismatch,
};

bool War3SemanticDirectPacketMatchesMainWorldVisibleRecord(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    const dxvk::war3::render::VisibleRenderableRecord& visible,
    War3SemanticDirectMainWorldBackingStatus* outStatus = nullptr);
bool War3SemanticDirectPacketHasMainWorldVisibleBacking(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    War3SemanticDirectMainWorldBackingStatus* outStatus = nullptr);

// ---------------------------------------------------------------------------
// 提交闸 / 材质与静态 world caster 准入 / 帧优先策略
// ---------------------------------------------------------------------------
bool War3SemanticRejectAlphaBlendCasterRuntime();
bool War3SemanticRejectUnsafeAlphaCasterRuntime();
bool War3SemanticMaterialIsSafeOpaqueWorldCaster(
    const dxvk::war3::shadow::ShadowDrawPacket& packet);
bool War3IsEligibleSemanticStaticWorldCaster(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind,
    bool hasRenderableGeoset, bool hasPacketGeometry);
bool War3ShouldSubmitSemanticPacket(
    const dxvk::war3::shadow::ShadowDrawPacket& packet,
    dxvk::war3::render::ObjectKind resolvedObjectKind, bool unitsOnly);
bool War3ShouldSubmitSemanticPacketFast(
    const dxvk::war3::shadow::ShadowDrawPacket& packet, bool unitsOnly);
bool War3LooksSubmitEligibleForScoringFast(
    const dxvk::war3::shadow::ShadowDrawPacket& packet, bool unitsOnly);
struct War3SemanticSceneFrameScore {
  uint32_t inputDrawCount = 0u;
  uint32_t eligibleDrawCount = 0u;
  uint32_t skinnedDrawCount = 0u;
};
War3SemanticSceneFrameScore War3ScoreSemanticSceneFrame(
    const dxvk::war3::shadow::ShadowSubmissionFrame* frame, bool unitsOnly);
bool War3ShouldPreferSemanticSceneFrame(
    const std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame>&
        candidate,
    const std::shared_ptr<const dxvk::war3::shadow::ShadowSubmissionFrame>&
        current,
    bool unitsOnly);

// ---------------------------------------------------------------------------
// path blocker 证据判定（rawcode / jHandle / widget 指针直读）
// ---------------------------------------------------------------------------
bool IsLosBlockerFourCc(uint32_t rawcode);
bool War3ShadowIsLosBlockerByJHandleFallback(uint32_t jHandle);
bool War3ShadowIsLosBlockerByWidgetPtr(
    void* widgetPtr, uint32_t jHandleForCache,
    bool traceWorldWidgetProbe = false,
    uint64_t negativeCacheFrameSerial = 0u);
bool War3PacketIsPathBlocker(
    const dxvk::war3::shadow::ShadowDrawPacket& packet);
// eligibility gate 拒绝路径阻断器的全局计数（atomic relaxed）。该判定定义在
// war3/semantic 模块，没有 D3D9Ex 上下文，因此用全局原子计数；D3D9Ex 每帧
// reset shadow scene 时把累加值移入 shadowStats。
extern std::atomic<uint32_t> g_pathBlockerEligibilityGateRejectCount;

// ---------------------------------------------------------------------------
// 选择/探测相关的运行时配置 getter
// ---------------------------------------------------------------------------
bool War3SemanticObjectGroupedSelectionRuntime();
bool War3SemanticStickySelectionLeaseRuntime();
uint32_t War3SemanticStickySelectionLeaseFramesRuntime();
bool War3SemanticStickySelectionBroadLeasePreferenceRuntime();
bool War3SemanticStickySelectionFillRuntime();
uint32_t War3SemanticStickySelectionFillMarginRuntime();
bool War3SemanticStickyPartSelectionRuntime();
uint32_t War3SemanticStickyPartSelectionMinRecordsRuntime();
bool War3WidgetProbeSafeCopyRuntime();
bool War3WidgetNegativeFrameCacheRuntime();
uint64_t War3WidgetNegativeFrameCacheTtlFrames();
uint32_t War3GetEnvU32(const char *name, uint32_t fallback);

} // namespace dxvk::war3::semantic
