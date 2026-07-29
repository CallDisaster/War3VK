#pragma once

#include "../core/war3_internal_test_config.h"
#include "../tools/war3_perf_monitor.h"
#include "../../../util/util_time.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace dxvk::war3::hooks {

// 高频 Hook 只允许使用固定 ID。禁止在调用点创建动态字符串、map key 或
// PerfMonitor scope；名称解析和报告回填仅在外层 native/frame 边界执行一次。
enum class War3HotHookId : uint32_t {
  DispatchCommon = 0u,
  DispatchSpecial,
  WorldObjectEntryRender,
  RenderQueueAddBatch,
  RenderBatchSubmit,
  CurrentDrawUpdateWorldMatrix,
  CurrentDrawContextGate,
  CurrentDrawRecordSeed,
  CurrentDrawVisibleBackfill,
  CurrentDrawFrameIdentity,
  CurrentDrawBindFieldRefresh,
  CurrentDrawPublishContract,
  CurrentDrawPublishLocalGateCache,
  CurrentDrawPublishTrustedPaletteQueryPack,
  CurrentDrawPublishSnapshotCommit,
  CurrentDrawPublishGlobalMaps,
  UiDispatch,
  UiRenderableRender,
  EventDispatch,
  ExecuteJassFunction,
  ModelSpriteHostBind,
  ModelAttachedEffectInit,
  ModelAttachedEffectDirectAttach,
  ModelAttachToPoint,
  ModelCreateSpriteRuntime,
  ModelCreateGeoset,
  ModelPromoteRuntime,
  ModelBuildChildLinks,
  ModelSpriteFrameUpdate,
  ModelSpriteMiniFrameUpdate,
  ModelSpriteFrameLiteUpdate,
  ModelSpriteMiniFrameLiteUpdate,
  WidgetRegisterFootprint,
  ApplyDrawStateAndDraw,
  ApplyDrawStateAndSamplerPair,
  GpuSkinDynamicVertexUpload,
  GpuSkinCopyKernel,
  RenderQueueStageUpdate,
  FlushTransparent,
  TransparentDispatchType0,
  TransparentDispatchType1,
  TransparentDispatchType2,
  TransparentDispatchType3,
  TransparentDispatchType4,
  D3D9DrawPrimitive,
  D3D9DrawIndexedPrimitive,
  D3D9DrawPrimitiveUP,
  D3D9DrawIndexedPrimitiveUP,
  ShadowCaptureCallback,
  ShadowTerrainListB,
  ShadowTerrainLayer,
  ShadowUnitUiRecord,
  ShadowStructureUiRecord,
  ShadowProjectorFromObject,
  ShadowProjectorSimple,
  StormAlloc,
  StormFree,
  StormGetSize,
  StormReAlloc,
  WorldPrepareFlushDeferredSelectionObjects,
  WorldPrepareGlobalRenderCallbackPass,
  WorldPrepareRenderWaypointIndicators,
  WorldPrepareFrameUpdateGate,
  WorldPrepareGameUiFrameSync,
  WorldPrepareUpdateIndicatorAnchor,
  WorldPrepareCameraAdvance,
  WorldPrepareCameraPrepareConstants,
  WorldPrepareViewProjPrepare,
  WorldPrepareSceneQueryFlushSync,
  WorldPrepareFixedPointRemap,
  WorldPreparePostVisibilityGlobalAdvanceA,
  WorldPreparePostVisibilityFrameAnchorUpdate,
  WorldPreparePostVisibilityFrameAnchorVisibilityQuery,
  WorldPreparePostVisibilityGlobalAdvanceB,
  WorldPrepareVisibilityTailAdvanceA,
  WorldPrepareVisibilityTailAdvanceB,
  ModelSpriteBuildPoseStackRoot,
  ModelSpriteSetWorldMatrixAndEvaluateRootPose,
  ModelSpriteBuildStagePresetsWithOverrides,
  ModelSpriteBuildStagePresetsSimple,
  ModelSpriteEvaluateOverrideGraph,
  ModelSpriteEvaluateChildStagePresetTree,
  ModelSpriteAssignVisiblePartStagePresetSpan,
  ModelSpriteCopyResolvedStagePresetsToOutput,
  ModelSpriteAssignDefaultVisiblePartStagePresets,
  ModelSpriteFlushCurrentPoseStackToMatrices,
  ModelSpriteEvalPoseStackAndChildren,
  ModelSpritePerTrackPresetWalker,
  ModelSpritePerStagePresetWalker,
  EngineTlsPump,
  EngineSelectWorker,
  EngineRunCallbacks,
  EngineQueueFlush,
  EngineFinalizeTick,
  EngineReschedule,
  EnginePrepareDispatch,
  EngineFinalizeDispatch,
  EngineTickUpdate,
  EngineFinalizeWorker,
  EngineComputeWakeDelta,
  PublishVisible,
  PublishVisibleSafeRead,
  PublishVisibleTlsIdentityMerge,
  PublishVisibleCurrentIdentity,
  PublishVisibleRenderQueueCache,
  PublishVisiblePriorVisibleLookup,
  PublishVisibleUnitFill,
  PublishVisibleRecordBuild,
  PublishVisibleRegistryRegister,
  Count,
};

inline const char* War3HotHookName(War3HotHookId id) noexcept {
  switch (id) {
  case War3HotHookId::DispatchCommon:
    return "Hook_Dispatch_Common";
  case War3HotHookId::DispatchSpecial:
    return "Hook_Dispatch_Special";
  case War3HotHookId::PublishVisible:
    return "PublishVisible";
  case War3HotHookId::PublishVisibleSafeRead:
    return "PublishVisible_SafeRead";
  case War3HotHookId::PublishVisibleTlsIdentityMerge:
    return "PublishVisible_TLSIdentityMerge";
  case War3HotHookId::PublishVisibleCurrentIdentity:
    return "PublishVisible_CurrentIdentity";
  case War3HotHookId::PublishVisibleRenderQueueCache:
    return "PublishVisible_RenderQueueCache";
  case War3HotHookId::PublishVisiblePriorVisibleLookup:
    return "PublishVisible_PriorVisibleLookup";
  case War3HotHookId::PublishVisibleUnitFill:
    return "PublishVisible_UnitFill";
  case War3HotHookId::PublishVisibleRecordBuild:
    return "PublishVisible_RecordBuild";
  case War3HotHookId::PublishVisibleRegistryRegister:
    return "PublishVisible_RegistryRegister";
  case War3HotHookId::WorldObjectEntryRender:
    return "Hook_WorldObjectEntry_Render";
  case War3HotHookId::RenderQueueAddBatch:
    return "Hook_RenderQueue_AddBatch";
  case War3HotHookId::RenderBatchSubmit:
    return "Hook_RenderBatch_Submit";
  case War3HotHookId::CurrentDrawUpdateWorldMatrix:
    return "Hook_CurrentDraw_UpdateWorldMatrix";
  case War3HotHookId::CurrentDrawContextGate:
    return "CurrentDraw_ContextGate";
  case War3HotHookId::CurrentDrawRecordSeed:
    return "CurrentDraw_RecordSeed";
  case War3HotHookId::CurrentDrawVisibleBackfill:
    return "CurrentDraw_VisibleBackfill";
  case War3HotHookId::CurrentDrawFrameIdentity:
    return "CurrentDraw_FrameIdentity";
  case War3HotHookId::CurrentDrawBindFieldRefresh:
    return "CurrentDraw_BindFieldRefresh";
  case War3HotHookId::CurrentDrawPublishContract:
    return "CurrentDraw_PublishContract";
  case War3HotHookId::CurrentDrawPublishLocalGateCache:
    return "CurrentDraw_Publish_LocalGateCache";
  case War3HotHookId::CurrentDrawPublishTrustedPaletteQueryPack:
    return "CurrentDraw_Publish_TrustedPaletteQueryPack";
  case War3HotHookId::CurrentDrawPublishSnapshotCommit:
    return "CurrentDraw_Publish_SnapshotCommit";
  case War3HotHookId::CurrentDrawPublishGlobalMaps:
    return "CurrentDraw_Publish_GlobalMaps";
  case War3HotHookId::UiDispatch:
    return "Hook_UiDispatch";
  case War3HotHookId::UiRenderableRender:
    return "Hook_UiRenderableRender";
  case War3HotHookId::EventDispatch:
    return "Hook_EventDispatch";
  case War3HotHookId::ExecuteJassFunction:
    return "Hook_ExecuteJassFunction";
  case War3HotHookId::ModelSpriteHostBind:
    return "Hook_Model_CreateSpriteAndBindSourceObject";
  case War3HotHookId::ModelAttachedEffectInit:
    return "Hook_Model_AttachedEffectInit";
  case War3HotHookId::ModelAttachedEffectDirectAttach:
    return "Hook_Model_AttachedEffectDirectAttach";
  case War3HotHookId::ModelAttachToPoint:
    return "Hook_Model_AttachModelToPoint";
  case War3HotHookId::ModelCreateSpriteRuntime:
    return "Hook_Model_CreateSpriteRuntime";
  case War3HotHookId::ModelCreateGeoset:
    return "Hook_Model_CreateGeosetFromRawArrays";
  case War3HotHookId::ModelPromoteRuntime:
    return "Hook_Model_PromoteRuntimeModel";
  case War3HotHookId::ModelBuildChildLinks:
    return "Hook_Model_BuildChildRuntimeModelLinks";
  case War3HotHookId::ModelSpriteFrameUpdate:
    return "Hook_Model_SpriteFrameUpdate";
  case War3HotHookId::ModelSpriteMiniFrameUpdate:
    return "Hook_Model_SpriteMiniFrameUpdate";
  case War3HotHookId::ModelSpriteFrameLiteUpdate:
    return "Hook_Model_SpriteFrameLiteUpdate";
  case War3HotHookId::ModelSpriteMiniFrameLiteUpdate:
    return "Hook_Model_SpriteMiniFrameLiteUpdate";
  case War3HotHookId::WidgetRegisterFootprint:
    return "Hook_Widget_RegisterFootprintAndShadowMask";
  case War3HotHookId::ApplyDrawStateAndDraw:
    return "Hook_ApplyDrawStateAndDraw";
  case War3HotHookId::ApplyDrawStateAndSamplerPair:
    return "Hook_ApplyDrawStateAndSamplerPair";
  case War3HotHookId::GpuSkinDynamicVertexUpload:
    return "Hook_GpuSkin_DynamicVertexUpload";
  case War3HotHookId::GpuSkinCopyKernel:
    return "Hook_GpuSkin_CopyKernel";
  case War3HotHookId::RenderQueueStageUpdate:
    return "Hook_RenderQueue_StageUpdate";
  case War3HotHookId::FlushTransparent:
    return "Hook_RenderQueue_FlushTransparent";
  case War3HotHookId::TransparentDispatchType0:
    return "Hook_TransparentDispatch_Type0_RenderBatch";
  case War3HotHookId::TransparentDispatchType1:
    return "Hook_TransparentDispatch_Type1_ParticleEmitter";
  case War3HotHookId::TransparentDispatchType2:
    return "Hook_TransparentDispatch_Type2_ImageLike";
  case War3HotHookId::TransparentDispatchType3:
    return "Hook_TransparentDispatch_Type3_RibbonEmitter";
  case War3HotHookId::TransparentDispatchType4:
    return "Hook_TransparentDispatch_Type4_CallbackWrapper";
  case War3HotHookId::D3D9DrawPrimitive:
    return "DXVK_D3D9_DrawPrimitive";
  case War3HotHookId::D3D9DrawIndexedPrimitive:
    return "DXVK_D3D9_DrawIndexedPrimitive";
  case War3HotHookId::D3D9DrawPrimitiveUP:
    return "DXVK_D3D9_DrawPrimitiveUP";
  case War3HotHookId::D3D9DrawIndexedPrimitiveUP:
    return "DXVK_D3D9_DrawIndexedPrimitiveUP";
  case War3HotHookId::ShadowCaptureCallback:
    return "WarVKCallback_ShadowCapture";
  case War3HotHookId::ShadowTerrainListB:
    return "Hook_Shadow_TerrainListB";
  case War3HotHookId::ShadowTerrainLayer:
    return "Hook_Shadow_TerrainRenderLayer";
  case War3HotHookId::ShadowUnitUiRecord:
    return "Hook_Shadow_CUnitUI_RecordSetUnitShadow";
  case War3HotHookId::ShadowStructureUiRecord:
    return "Hook_Shadow_CUnitUI_RecordSetStructureShadow";
  case War3HotHookId::ShadowProjectorFromObject:
    return "Hook_ShadowProjector_FromObject";
  case War3HotHookId::ShadowProjectorSimple:
    return "Hook_ShadowProjector_Simple";
  case War3HotHookId::StormAlloc:
    return "Hook_Storm_Alloc";
  case War3HotHookId::StormFree:
    return "Hook_Storm_Free";
  case War3HotHookId::StormGetSize:
    return "Hook_Storm_GetSize";
  case War3HotHookId::StormReAlloc:
    return "Hook_Storm_ReAlloc";
  case War3HotHookId::WorldPrepareFlushDeferredSelectionObjects:
    return "Hook_WorldPrepare_FlushDeferredSelectionObjects";
  case War3HotHookId::WorldPrepareGlobalRenderCallbackPass:
    return "Hook_WorldPrepare_GlobalRenderCallbackPass";
  case War3HotHookId::WorldPrepareRenderWaypointIndicators:
    return "Hook_WorldPrepare_RenderWaypointIndicators";
  case War3HotHookId::WorldPrepareFrameUpdateGate:
    return "Hook_WorldPrepare_FrameUpdateGate";
  case War3HotHookId::WorldPrepareGameUiFrameSync:
    return "Hook_WorldPrepare_GameUiFrameSync";
  case War3HotHookId::WorldPrepareUpdateIndicatorAnchor:
    return "Hook_WorldPrepare_UpdateIndicatorAnchor";
  case War3HotHookId::WorldPrepareCameraAdvance:
    return "Hook_WorldPrepare_CameraAdvance";
  case War3HotHookId::WorldPrepareCameraPrepareConstants:
    return "Hook_WorldPrepare_CameraPrepareConstants";
  case War3HotHookId::WorldPrepareViewProjPrepare:
    return "Hook_WorldPrepare_ViewProjPrepare";
  case War3HotHookId::WorldPrepareSceneQueryFlushSync:
    return "Hook_WorldPrepare_SceneQueryFlushSync";
  case War3HotHookId::WorldPrepareFixedPointRemap:
    return "Hook_WorldPrepare_FixedPointRemap";
  case War3HotHookId::WorldPreparePostVisibilityGlobalAdvanceA:
    return "Hook_WorldPrepare_PostVisibilityGlobalAdvanceA";
  case War3HotHookId::WorldPreparePostVisibilityFrameAnchorUpdate:
    return "Hook_WorldPrepare_PostVisibility_FrameAnchorUpdate";
  case War3HotHookId::WorldPreparePostVisibilityFrameAnchorVisibilityQuery:
    return "Hook_WorldPrepare_PostVisibility_FrameAnchorVisibilityQuery";
  case War3HotHookId::WorldPreparePostVisibilityGlobalAdvanceB:
    return "Hook_WorldPrepare_PostVisibilityGlobalAdvanceB";
  case War3HotHookId::WorldPrepareVisibilityTailAdvanceA:
    return "Hook_WorldPrepare_VisibilityTailAdvanceA";
  case War3HotHookId::WorldPrepareVisibilityTailAdvanceB:
    return "Hook_WorldPrepare_VisibilityTailAdvanceB";
  case War3HotHookId::ModelSpriteBuildPoseStackRoot:
    return "Hook_Model_Native_BuildPoseStackRoot";
  case War3HotHookId::ModelSpriteSetWorldMatrixAndEvaluateRootPose:
    return "Hook_Model_Native_SetWorldMatrixAndEvaluateRootPose";
  case War3HotHookId::ModelSpriteBuildStagePresetsWithOverrides:
    return "Hook_Model_Native_BuildVisiblePartStagePresets_WithOverrides";
  case War3HotHookId::ModelSpriteBuildStagePresetsSimple:
    return "Hook_Model_Native_BuildVisiblePartStagePresets_Simple";
  case War3HotHookId::ModelSpriteEvaluateOverrideGraph:
    return "Hook_Model_Native_EvaluateOverrideGraph";
  case War3HotHookId::ModelSpritePerTrackPresetWalker:
    return "Hook_Model_Native_PerTrackPresetWalker";
  case War3HotHookId::ModelSpritePerStagePresetWalker:
    return "Hook_Model_Native_PerStagePresetWalker";
  case War3HotHookId::ModelSpriteEvaluateChildStagePresetTree:
    return "Hook_Model_Native_EvaluateChildStagePresetTree";
  case War3HotHookId::ModelSpriteAssignVisiblePartStagePresetSpan:
    return "Hook_Model_Native_AssignVisiblePartStagePresetSpan";
  case War3HotHookId::ModelSpriteCopyResolvedStagePresetsToOutput:
    return "Hook_Model_Native_CopyResolvedStagePresetsToOutput";
  case War3HotHookId::ModelSpriteAssignDefaultVisiblePartStagePresets:
    return "Hook_Model_Native_AssignDefaultVisiblePartStagePresets";
  case War3HotHookId::ModelSpriteFlushCurrentPoseStackToMatrices:
    return "Hook_Model_Native_FlushCurrentPoseStackToMatrices";
  case War3HotHookId::ModelSpriteEvalPoseStackAndChildren:
    return "Hook_Model_Native_EvalPoseStackAndChildren";
  case War3HotHookId::EngineTlsPump:
    return "Hook_EngineTlsPump";
  case War3HotHookId::EngineSelectWorker:
    return "Hook_EngineSelectWorker";
  case War3HotHookId::EngineRunCallbacks:
    return "Hook_EngineRunCallbacks";
  case War3HotHookId::EngineQueueFlush:
    return "Hook_EngineQueueFlush";
  case War3HotHookId::EngineFinalizeTick:
    return "Hook_EngineFinalizeTick";
  case War3HotHookId::EngineReschedule:
    return "Hook_EngineReschedule";
  case War3HotHookId::EnginePrepareDispatch:
    return "Hook_EnginePrepareDispatch";
  case War3HotHookId::EngineFinalizeDispatch:
    return "Hook_EngineFinalizeDispatch";
  case War3HotHookId::EngineTickUpdate:
    return "Hook_EngineTickUpdate";
  case War3HotHookId::EngineFinalizeWorker:
    return "Hook_EngineFinalizeWorker";
  case War3HotHookId::EngineComputeWakeDelta:
    return "Hook_EngineComputeWakeDelta";
  default:
    return "Hook_Unknown";
  }
}

inline bool War3HotHookIsSyntheticPhase(War3HotHookId id) noexcept {
  switch (id) {
  case War3HotHookId::CurrentDrawContextGate:
  case War3HotHookId::CurrentDrawRecordSeed:
  case War3HotHookId::CurrentDrawVisibleBackfill:
  case War3HotHookId::CurrentDrawFrameIdentity:
  case War3HotHookId::CurrentDrawBindFieldRefresh:
  case War3HotHookId::CurrentDrawPublishContract:
  case War3HotHookId::CurrentDrawPublishLocalGateCache:
  case War3HotHookId::CurrentDrawPublishTrustedPaletteQueryPack:
  case War3HotHookId::CurrentDrawPublishSnapshotCommit:
  case War3HotHookId::CurrentDrawPublishGlobalMaps:
    return true;
  default:
    return false;
  }
}

struct War3HotHookBucket {
  uint64_t customTicks = 0u;
  uint64_t nativeTicks = 0u;
  uint64_t calls = 0u;
  uint64_t nativeCalls = 0u;
};

constexpr size_t kWar3HotHookCount =
    static_cast<size_t>(War3HotHookId::Count);
static_assert(kWar3HotHookCount <=
                  size_t((std::numeric_limits<uint8_t>::max)()) + 1u,
              "War3HotHookPathKey stores fixed IDs in uint8_t");
constexpr size_t kWar3HotHookBoundaryDepth = 8u;
constexpr size_t kWar3HotHookCallDepth = 8u;
constexpr size_t kWar3HotHookPathCapacity = 192u;
static_assert(kWar3HotHookPathCapacity >= kWar3HotHookCount,
              "HotHook path storage must cover at least one path per fixed ID");

enum class War3HotHookPhase : uint8_t {
  Custom = 0u,
  Native = 1u,
};

inline const char* War3HotHookPhaseName(War3HotHookId id,
                                       War3HotHookPhase phase) noexcept {
  if (phase == War3HotHookPhase::Native)
    return "NativeOriginalInclusive";
  switch (id) {
  case War3HotHookId::D3D9DrawPrimitive:
  case War3HotHookId::D3D9DrawIndexedPrimitive:
  case War3HotHookId::D3D9DrawPrimitiveUP:
  case War3HotHookId::D3D9DrawIndexedPrimitiveUP:
    return "DXVKFrontendLogic";
  case War3HotHookId::ShadowCaptureCallback:
    return "WarVKCallbackLogic";
  case War3HotHookId::RenderQueueStageUpdate:
  case War3HotHookId::FlushTransparent:
  case War3HotHookId::TransparentDispatchType0:
  case War3HotHookId::TransparentDispatchType1:
  case War3HotHookId::TransparentDispatchType2:
  case War3HotHookId::TransparentDispatchType3:
  case War3HotHookId::TransparentDispatchType4:
  case War3HotHookId::WorldPrepareFlushDeferredSelectionObjects:
  case War3HotHookId::WorldPrepareGlobalRenderCallbackPass:
  case War3HotHookId::WorldPrepareRenderWaypointIndicators:
  case War3HotHookId::WorldPrepareFrameUpdateGate:
  case War3HotHookId::WorldPrepareGameUiFrameSync:
  case War3HotHookId::WorldPrepareUpdateIndicatorAnchor:
  case War3HotHookId::WorldPrepareCameraAdvance:
  case War3HotHookId::WorldPrepareCameraPrepareConstants:
  case War3HotHookId::WorldPrepareViewProjPrepare:
  case War3HotHookId::WorldPrepareSceneQueryFlushSync:
  case War3HotHookId::WorldPrepareFixedPointRemap:
  case War3HotHookId::WorldPreparePostVisibilityGlobalAdvanceA:
  case War3HotHookId::WorldPreparePostVisibilityFrameAnchorUpdate:
  case War3HotHookId::WorldPreparePostVisibilityFrameAnchorVisibilityQuery:
  case War3HotHookId::WorldPreparePostVisibilityGlobalAdvanceB:
  case War3HotHookId::WorldPrepareVisibilityTailAdvanceA:
  case War3HotHookId::WorldPrepareVisibilityTailAdvanceB:
  case War3HotHookId::ModelSpriteBuildPoseStackRoot:
  case War3HotHookId::ModelSpriteSetWorldMatrixAndEvaluateRootPose:
  case War3HotHookId::ModelSpriteBuildStagePresetsWithOverrides:
  case War3HotHookId::ModelSpriteBuildStagePresetsSimple:
  case War3HotHookId::ModelSpriteEvaluateOverrideGraph:
  case War3HotHookId::ModelSpritePerTrackPresetWalker:
  case War3HotHookId::ModelSpritePerStagePresetWalker:
  case War3HotHookId::ModelSpriteEvaluateChildStagePresetTree:
  case War3HotHookId::ModelSpriteAssignVisiblePartStagePresetSpan:
  case War3HotHookId::ModelSpriteCopyResolvedStagePresetsToOutput:
  case War3HotHookId::ModelSpriteAssignDefaultVisiblePartStagePresets:
  case War3HotHookId::ModelSpriteFlushCurrentPoseStackToMatrices:
  case War3HotHookId::ModelSpriteEvalPoseStackAndChildren:
  case War3HotHookId::EngineTlsPump:
  case War3HotHookId::EngineSelectWorker:
  case War3HotHookId::EngineRunCallbacks:
  case War3HotHookId::EngineQueueFlush:
  case War3HotHookId::EngineFinalizeTick:
  case War3HotHookId::EngineReschedule:
  case War3HotHookId::EnginePrepareDispatch:
  case War3HotHookId::EngineFinalizeDispatch:
  case War3HotHookId::EngineFinalizeWorker:
  case War3HotHookId::EngineComputeWakeDelta:
    // These opt-in detours contain no WarVK business work. The residual is
    // strictly profiler wrapper/QPC overhead and must not appear as an
    // optimization candidate named "WarVKHookLogic".
    return "ObserverOverhead";
  default:
    return "WarVKHookLogic";
  }
}

struct War3HotHookPathKey {
  std::array<uint8_t, kWar3HotHookCallDepth> ids{};
  std::array<uint8_t, kWar3HotHookCallDepth> parentPhases{};
  uint8_t depth = 0u;
};

struct War3HotHookPathBucket {
  War3HotHookPathKey key{};
  War3HotHookBucket timing{};
  bool used = false;
};

struct War3HotHookActiveCall {
  War3HotHookId id = War3HotHookId::DispatchCommon;
  War3HotHookPhase phase = War3HotHookPhase::Custom;
};

struct War3HotHookBoundaryFrame {
  std::array<War3HotHookPathBucket, kWar3HotHookPathCapacity> paths{};
  std::array<War3HotHookActiveCall, kWar3HotHookCallDepth> calls{};
  size_t pathCount = 0u;
  size_t callDepth = 0u;
  uint64_t pathOverflowCalls = 0u;
};

struct War3HotHookThreadState {
  std::array<War3HotHookBoundaryFrame, kWar3HotHookBoundaryDepth> frames{};
  std::array<uint64_t, kWar3HotHookCount> sampleOrdinals{};
  size_t depth = 0u;
  bool publishing = false;
};

inline War3HotHookThreadState& War3HotHookState() noexcept {
  static thread_local War3HotHookThreadState state;
  return state;
}

inline uint32_t War3HotHookNarrowCalls(uint64_t calls) noexcept {
  return static_cast<uint32_t>((std::min)(
      calls, uint64_t((std::numeric_limits<uint32_t>::max)())));
}

inline bool War3HotHookShouldSample(uint64_t ordinal,
                                   uint32_t period) noexcept {
  if (period <= 1u)
    return true;
  // 所有生产调用点当前都使用 2 的幂；保留通用周期回退，避免 API
  // 暗含仅可使用 power-of-two 的隐藏合同。
  if ((period & (period - 1u)) == 0u)
    return (ordinal & uint64_t(period - 1u)) == 0u;
  return (ordinal % period) == 0u;
}

inline bool War3HotHookPathKeyEquals(const War3HotHookPathKey& lhs,
                                    const War3HotHookPathKey& rhs) noexcept {
  if (lhs.depth != rhs.depth)
    return false;
  for (uint8_t i = 0u; i < lhs.depth; ++i) {
    if (lhs.ids[i] != rhs.ids[i])
      return false;
    if (i + 1u < lhs.depth &&
        lhs.parentPhases[i] != rhs.parentPhases[i])
      return false;
  }
  return true;
}

inline War3HotHookPathBucket* War3HotHookResolvePath(
    War3HotHookBoundaryFrame& frame, War3HotHookId id,
    uint32_t overflowCallWeight = 1u) noexcept {
  if (frame.callDepth >= kWar3HotHookCallDepth) {
    frame.pathOverflowCalls +=
        uint64_t((std::max)(1u, overflowCallWeight));
    return nullptr;
  }

  War3HotHookPathKey key{};
  key.depth = static_cast<uint8_t>(frame.callDepth + 1u);
  for (size_t i = 0u; i < frame.callDepth; ++i) {
    key.ids[i] = static_cast<uint8_t>(frame.calls[i].id);
    key.parentPhases[i] = static_cast<uint8_t>(frame.calls[i].phase);
  }
  key.ids[frame.callDepth] = static_cast<uint8_t>(id);

  for (size_t i = 0u; i < frame.pathCount; ++i) {
    auto& path = frame.paths[i];
    if (War3HotHookPathKeyEquals(path.key, key))
      return &path;
  }
  if (frame.pathCount >= frame.paths.size()) {
    frame.pathOverflowCalls +=
        uint64_t((std::max)(1u, overflowCallWeight));
    return nullptr;
  }
  War3HotHookPathBucket* freeBucket = &frame.paths[frame.pathCount++];
  freeBucket->used = true;
  freeBucket->key = key;
  freeBucket->timing = {};
  return freeBucket;
}

enum class War3HotHookBoundaryMode : uint8_t {
  NestedAllowed = 0u,
  RootOnly = 1u,
};

// 外层边界必须在对应 PerfMonitor scope 内创建，并在该 scope 退出前销毁。
// 嵌套边界只把高频 Hook 记到最内层：外层通过动态子树自然扣除，避免双计。
class War3HotHookBoundaryScope final {
public:
  explicit War3HotHookBoundaryScope(
      War3HotHookBoundaryMode mode =
          War3HotHookBoundaryMode::NestedAllowed) noexcept {
    auto& perf = War3PerfMonitor::instance();
    // Level 1 是产品默认的低频 frame scope；完整热 Hook 动态树属于
    // detail 诊断，必须显式 PERF_LEVEL=2，避免观察器常驻改变被测帧时。
    if (internal::War3PerfHookLevel() < 2 ||
        !perf.isEnabled() || !perf.isRecording()) {
      return;
    }

    auto& state = War3HotHookState();
    if (mode == War3HotHookBoundaryMode::RootOnly && state.depth != 0u)
      return;
    if (state.publishing || state.depth >= state.frames.size())
      return;
    m_frameIndex = state.depth++;
    // frame.paths 是一组较大的固定桶。每个 Native/frame 边界只需
    // 重置有效区间；新桶在首次占用时会完整覆盖 key/timing。逐边界将
    // 整块 TLS 数组清零会让诊断器本身成为可见热点。
    state.frames[m_frameIndex].pathCount = 0u;
    state.frames[m_frameIndex].callDepth = 0u;
    state.frames[m_frameIndex].pathOverflowCalls = 0u;
    m_active = true;
  }

  ~War3HotHookBoundaryScope() noexcept {
    if (!m_active)
      return;

    auto& state = War3HotHookState();
    if (state.depth == 0u || m_frameIndex >= state.frames.size())
      return;
    War3HotHookBoundaryFrame& frame = state.frames[m_frameIndex];
    state.publishing = true;
    const double ticksToMs = 1000.0 /
        double(dxvk::high_resolution_clock::get_frequency());
    auto& perf = War3PerfMonitor::instance();
    for (size_t pathIndex = 0u; pathIndex < frame.pathCount; ++pathIndex) {
      const auto& path = frame.paths[pathIndex];
      if (!path.used || path.key.depth == 0u)
        continue;
      const War3HotHookBucket& bucket = path.timing;
      if (bucket.calls == 0u)
        continue;
      const auto id = static_cast<War3HotHookId>(
          path.key.ids[path.key.depth - 1u]);
      const char* hookName = War3HotHookName(id);
      std::string relativeParentPath;
      for (uint8_t i = 0u; i + 1u < path.key.depth; ++i) {
        if (!relativeParentPath.empty())
          relativeParentPath.push_back('/');
        relativeParentPath.append(War3HotHookName(
            static_cast<War3HotHookId>(path.key.ids[i])));
        const auto parentId =
            static_cast<War3HotHookId>(path.key.ids[i]);
        if (!War3HotHookIsSyntheticPhase(parentId)) {
          relativeParentPath.push_back('/');
          relativeParentPath.append(War3HotHookPhaseName(
              parentId,
              static_cast<War3HotHookPhase>(path.key.parentPhases[i])));
        }
      }
      const uint64_t totalTicks = bucket.customTicks + bucket.nativeTicks;
      perf.addCpuSampleToCurrentScopeRelative(
          relativeParentPath.c_str(), hookName, double(totalTicks) * ticksToMs,
          War3HotHookNarrowCalls(bucket.calls));

      if (!relativeParentPath.empty())
        relativeParentPath.push_back('/');
      relativeParentPath.append(hookName);
      if (bucket.nativeCalls != 0u) {
        perf.addCpuSampleToCurrentScopeRelative(
            relativeParentPath.c_str(), "NativeOriginalInclusive",
            double(bucket.nativeTicks) * ticksToMs,
            War3HotHookNarrowCalls(bucket.nativeCalls));
      }
      // Synthetic phase IDs already name the measured WarVK region. Giving
      // them another generic WarVKHookLogic child would make the hotspot table
      // ambiguous again and would leave the meaningful phase name inclusive
      // with zero self time. Real Hook IDs retain the explicit
      // NativeOriginalInclusive / WarVKHookLogic contract.
      if (!War3HotHookIsSyntheticPhase(id)) {
        perf.addCpuSampleToCurrentScopeRelative(
            relativeParentPath.c_str(),
            War3HotHookPhaseName(id, War3HotHookPhase::Custom),
            double(bucket.customTicks) * ticksToMs,
            War3HotHookNarrowCalls(bucket.calls));
      }
    }
    if (frame.pathOverflowCalls != 0u) {
      perf.addCpuSampleToCurrentScope(
          "Profiler_HotHookPathOverflow", 0.0,
          War3HotHookNarrowCalls(frame.pathOverflowCalls));
    }
    if (frame.pathCount * 4u >= frame.paths.size() * 3u) {
      perf.addCpuSampleToCurrentScope(
          "Profiler_HotHookPathNearCapacity", 0.0,
          War3HotHookNarrowCalls(frame.pathCount));
    }
    state.publishing = false;
    state.frames[m_frameIndex].pathCount = 0u;
    state.frames[m_frameIndex].callDepth = 0u;
    state.frames[m_frameIndex].pathOverflowCalls = 0u;
    state.depth = m_frameIndex;
  }

  War3HotHookBoundaryScope(const War3HotHookBoundaryScope&) = delete;
  War3HotHookBoundaryScope& operator=(const War3HotHookBoundaryScope&) = delete;

private:
  size_t m_frameIndex = 0u;
  bool m_active = false;
};

// samplePeriod=1 给低频边界；高频 producer 应用 4/8/32/64 等固定周期。
// 采样命中后以 period 为权重估算 calls/ticks。未命中调用仍只维护极小的
// 活动父链，确保被抽中的嵌套 Hook 能挂回正确的 Custom/Native 父阶段，
// 但不再构造 path key、线性扫描 96 桶或读取 QPC。
/**
 * @brief 描述由外层统一选中的固定周期样本及其估算权重。
 * @note 该令牌不会推进目标 ID 的独立采样序号；调用方负责先完成选择。
 */
struct War3HotHookPreselectedSample {
  uint32_t weight = 1u;
};

class War3HotHookCallTiming final {
public:
  explicit War3HotHookCallTiming(War3HotHookId id,
                                 uint32_t samplePeriod = 1u) noexcept {
    Initialize(id, samplePeriod, false);
  }

  // 多阶段诊断先对整次调用统一抽样，再用此入口让所有子阶段共享同一
  // 样本选择。weight 只负责 Horvitz-Thompson 加权，不会再次推进 ID
  // 自己的 ordinal，避免阶段之间错相抽样。
  explicit War3HotHookCallTiming(
      War3HotHookId id, War3HotHookPreselectedSample sample) noexcept {
    Initialize(id, sample.weight, true);
  }

  ~War3HotHookCallTiming() noexcept {
    if (m_bucket && m_sampled && !m_suspended)
      Accumulate(dxvk::high_resolution_clock::get_counter());
    PopCall();
  }

  void BeginNative() noexcept {
    if (!m_frame || m_native || m_suspended)
      return;
    if (m_bucket) {
      m_bucket->nativeCalls += uint64_t(m_weight);
      Accumulate(dxvk::high_resolution_clock::get_counter());
    }
    m_native = true;
    if (m_frame && m_pushed)
      m_frame->calls[m_stackIndex].phase = War3HotHookPhase::Native;
  }

  void EndNative() noexcept {
    if (!m_frame || !m_native)
      return;
    if (m_bucket)
      Accumulate(dxvk::high_resolution_clock::get_counter());
    m_native = false;
    if (m_frame && m_pushed)
      m_frame->calls[m_stackIndex].phase = War3HotHookPhase::Custom;
  }

  void SuspendForNestedAttributedWork() noexcept {
    if (!m_frame || !m_pushed || m_suspended)
      return;
    if (m_sampled)
      Accumulate(dxvk::high_resolution_clock::get_counter());
    if (m_frame->callDepth > m_stackIndex)
      m_frame->callDepth = m_stackIndex;
    m_suspended = true;
  }

  void ResumeAfterNestedAttributedWork() noexcept {
    if (!m_frame || !m_pushed || !m_suspended)
      return;
    m_frame->calls[m_stackIndex] = {
        m_id, m_native ? War3HotHookPhase::Native
                       : War3HotHookPhase::Custom};
    m_frame->callDepth = m_stackIndex + 1u;
    if (m_sampled)
      m_last = dxvk::high_resolution_clock::get_counter();
    m_suspended = false;
  }

  bool hasSampledTiming() const noexcept {
    return m_sampled && m_bucket != nullptr;
  }

  War3HotHookPreselectedSample preselectedSample() const noexcept {
    return {m_weight};
  }

  War3HotHookCallTiming(const War3HotHookCallTiming&) = delete;
  War3HotHookCallTiming& operator=(const War3HotHookCallTiming&) = delete;

private:
  void Initialize(War3HotHookId id, uint32_t samplePeriod,
                  bool preselected) noexcept {
    auto& state = War3HotHookState();
    if (state.publishing || state.depth == 0u)
      return;
    const size_t idIndex = static_cast<size_t>(id);
    if (idIndex >= kWar3HotHookCount)
      return;
    m_id = id;
    m_frame = &state.frames[state.depth - 1u];
    if (m_frame->callDepth >= m_frame->calls.size()) {
      m_frame = nullptr;
      return;
    }
    m_weight = (std::max)(1u, samplePeriod);
    const bool selected = preselected ||
        War3HotHookShouldSample(++state.sampleOrdinals[idIndex], m_weight);
    if (selected) {
      War3HotHookPathBucket* path =
          War3HotHookResolvePath(*m_frame, id, m_weight);
      m_bucket = path ? &path->timing : nullptr;
      if (m_bucket) {
        m_bucket->calls += uint64_t(m_weight);
        m_sampled = true;
        m_last = dxvk::high_resolution_clock::get_counter();
      }
    }
    m_stackIndex = m_frame->callDepth++;
    m_frame->calls[m_stackIndex] = {id, War3HotHookPhase::Custom};
    m_pushed = true;
  }
  void PopCall() noexcept {
    if (!m_frame || !m_pushed)
      return;
    if (m_frame->callDepth > m_stackIndex)
      m_frame->callDepth = m_stackIndex;
    m_pushed = false;
  }

  void Accumulate(int64_t now) noexcept {
    const uint64_t ticks = now >= m_last ? uint64_t(now - m_last) : 0u;
    const uint64_t weightedTicks = ticks * uint64_t(m_weight);
    if (m_native)
      m_bucket->nativeTicks += weightedTicks;
    else
      m_bucket->customTicks += weightedTicks;
    m_last = now;
  }

  War3HotHookBucket* m_bucket = nullptr;
  War3HotHookBoundaryFrame* m_frame = nullptr;
  War3HotHookId m_id = War3HotHookId::DispatchCommon;
  size_t m_stackIndex = 0u;
  int64_t m_last = 0;
  uint32_t m_weight = 1u;
  bool m_sampled = false;
  bool m_native = false;
  bool m_pushed = false;
  bool m_suspended = false;
};

// Lifecycle 等 Hook 并不总在 EventPump/Render 的既有边界内执行。这个包装器
// 仅在当前没有边界时临时建立一个 Level-2 根边界；已有边界时保持原动态父树。
// 成员声明顺序保证析构为 timing -> boundary：先结束并记入调用，再发布边界。
class War3HotHookRootedCallTiming final {
public:
  explicit War3HotHookRootedCallTiming(
      War3HotHookId id, uint32_t samplePeriod = 1u) noexcept
      : m_boundary(War3HotHookBoundaryMode::RootOnly),
        m_timing(id, samplePeriod) {
  }

  War3HotHookCallTiming& timing() noexcept {
    return m_timing;
  }

  War3HotHookRootedCallTiming(
      const War3HotHookRootedCallTiming&) = delete;
  War3HotHookRootedCallTiming& operator=(
      const War3HotHookRootedCallTiming&) = delete;

private:
  War3HotHookBoundaryScope m_boundary;
  War3HotHookCallTiming m_timing;
};

class War3HotHookNativeScope final {
public:
  explicit War3HotHookNativeScope(
      War3HotHookCallTiming& timing) noexcept : m_timing(timing) {
    m_timing.BeginNative();
  }
  explicit War3HotHookNativeScope(
      War3HotHookRootedCallTiming& timing) noexcept
      : War3HotHookNativeScope(timing.timing()) {
  }
  ~War3HotHookNativeScope() noexcept { m_timing.EndNative(); }

private:
  War3HotHookCallTiming& m_timing;
};

class War3HotHookSuspendScope final {
public:
  explicit War3HotHookSuspendScope(
      War3HotHookCallTiming& timing) noexcept : m_timing(timing) {
    m_timing.SuspendForNestedAttributedWork();
  }
  ~War3HotHookSuspendScope() noexcept {
    m_timing.ResumeAfterNestedAttributedWork();
  }

private:
  War3HotHookCallTiming& m_timing;
};

// 原函数可能由调用方 SEH 恢复的 Hook（GPU skin upload/kernel 等）不能在
// trampoline 前把持久 TLS 调用栈改成“未闭合”状态。此版本只保存局部 QPC；
// 正常返回后才一次性提交桶。若 SEH 越过析构点，则不产生样本，也不污染后帧。
class War3HotHookCompletionTiming final {
public:
  explicit War3HotHookCompletionTiming(
      War3HotHookId id, uint32_t samplePeriod = 1u) noexcept : m_id(id) {
    auto& state = War3HotHookState();
    const size_t idIndex = static_cast<size_t>(id);
    if (state.publishing || state.depth == 0u ||
        idIndex >= kWar3HotHookCount)
      return;
    m_frame = &state.frames[state.depth - 1u];
    m_weight = (std::max)(1u, samplePeriod);
    const uint64_t ordinal = ++state.sampleOrdinals[idIndex];
    m_sampled = m_weight == 1u || (ordinal % m_weight) == 0u;
    if (m_sampled)
      m_totalStart = dxvk::high_resolution_clock::get_counter();
  }

  ~War3HotHookCompletionTiming() noexcept {
    if (!m_frame)
      return;
    if (m_native)
      EndNative();
    War3HotHookPathBucket* path = War3HotHookResolvePath(*m_frame, m_id);
    if (!path)
      return;
    ++path->timing.calls;
    path->timing.nativeCalls += m_nativeCalls;
    if (!m_sampled)
      return;
    const int64_t totalEnd = dxvk::high_resolution_clock::get_counter();
    const uint64_t totalTicks = totalEnd >= m_totalStart
        ? uint64_t(totalEnd - m_totalStart) * uint64_t(m_weight) : 0u;
    const uint64_t nativeTicks = m_nativeTicks * uint64_t(m_weight);
    path->timing.nativeTicks += (std::min)(nativeTicks, totalTicks);
    path->timing.customTicks += totalTicks >= nativeTicks
        ? totalTicks - nativeTicks : 0u;
  }

  void BeginNative() noexcept {
    if (!m_frame || m_native)
      return;
    ++m_nativeCalls;
    m_native = true;
    if (m_sampled)
      m_nativeStart = dxvk::high_resolution_clock::get_counter();
  }

  void EndNative() noexcept {
    if (!m_frame || !m_native)
      return;
    if (m_sampled) {
      const int64_t now = dxvk::high_resolution_clock::get_counter();
      if (now >= m_nativeStart)
        m_nativeTicks += uint64_t(now - m_nativeStart);
    }
    m_native = false;
  }

private:
  War3HotHookId m_id = War3HotHookId::DispatchCommon;
  War3HotHookBoundaryFrame* m_frame = nullptr;
  int64_t m_totalStart = 0;
  int64_t m_nativeStart = 0;
  uint64_t m_nativeTicks = 0u;
  uint64_t m_nativeCalls = 0u;
  uint32_t m_weight = 1u;
  bool m_sampled = false;
  bool m_native = false;
};

class War3HotHookCompletionNativeScope final {
public:
  explicit War3HotHookCompletionNativeScope(
      War3HotHookCompletionTiming& timing) noexcept : m_timing(timing) {
    m_timing.BeginNative();
  }
  ~War3HotHookCompletionNativeScope() noexcept { m_timing.EndNative(); }

private:
  War3HotHookCompletionTiming& m_timing;
};

} // namespace dxvk::war3::hooks
