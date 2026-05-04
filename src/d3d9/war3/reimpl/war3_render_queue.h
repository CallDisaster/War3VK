#pragma once
#include <cstring> // For memcmp

// War3 原生渲染队列复现（实验性）
// 说明：偏移与结构体以 Game.dll 1.27.x（32位）为准。

#include "../../d3d9_shader.h"
#include "../../d3d9_war3_debug.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"              // For IsReadableRange
#include "../render/war3_render_exec_batch.h" // For ExecBatchContext
#include "../render/war3_render_objects.h"
#include "../render/war3_render_state.h"      // For War3BatchTag
#include "../render/war3_visible_renderables.h"
#include "../tools/war3_perf_monitor.h"       // For cpuScope
#include "war3_batch_merger.h"
#include "war3_instance_buffer.h"
#include "war3_render_types.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional> // For std::function
#include <limits>
#include <windows.h>  // For SEH and MessageBox

namespace dxvk {
namespace war3 {
namespace reimpl {
// 全局 RenderQueue 变量地址 (RVA, 需 + Game.dll 基址)
// ============================================================================
// 说明（IDA 已确认）：
// - 这里的队列本质上对应 AUCOpaqueLayer（透明对象会被分流到 AUCTransparent
// 列表）
// - SortedBatchPtrs(0xBC6BE8) 是“指针数组本体”，不是“指向指针数组的指针”
//
// NumOfElements:         0xBC6BAC - Opaque 当前批次数量
// BatchArray:            0xBC6BB0 - Opaque 批次数组指针（RenderBatchElement[]）
// SortedBatchCount:      0xBC6BA0 - Opaque 排序后批次数量 (max 10000)
// SortedBatchPtrs:       0xBC6BE8 - Opaque 指针数组本体（RenderBatchElement*
// [10000]） StateOptEnabled:       0xBDA4D0 - Opaque 状态优化开关
// StateCleanupPending:   0xBDA4D4 - Opaque 尾部状态清理标志

// ============================================================================
// [State-Aware Batch Breaking] Constants
// ============================================================================
// 原版游戏仅比较 StateBlock 的前 20 字节（5 个 DWORD）。
// 这已通过逆向 RenderQueue_FlushSortedItems (0x1380A0) 确认。
constexpr size_t kLayerStateCompareBytes = 20;

/**
 * @brief 比较 LayerState 前 20 字节（5 个 DWORD）是否一致。
 *
 * 使用固定宽度比较替代通用 memcmp，减少高频热路径调用开销。
 */
static inline bool LayerStatePrefix20Equal(const void *lhs, const void *rhs) {
  if (lhs == rhs)
    return true;
  if (!lhs || !rhs)
    return false;

  const auto *a = reinterpret_cast<const uint32_t *>(lhs);
  const auto *b = reinterpret_cast<const uint32_t *>(rhs);
  return (a[0] == b[0]) && (a[1] == b[1]) && (a[2] == b[2]) &&
         (a[3] == b[3]) && (a[4] == b[4]);
}

/**
 * @brief RenderQueue 热路径可选性能分段。
 *
 * 关闭 `kNativeOptimizationPerfTrackingEnabled` 时返回空 Scope，
 * 避免每次 Flush/Sort/Transparent 分发都进入 PerfMonitor。
 */
static inline war3::War3PerfMonitor::ScopedCpuScope
MakeQueueCpuScope(const char *name) {
  if constexpr (dxvk::war3::internal::kNativeOptimizationPerfTrackingEnabled) {
    return war3::War3PerfMonitor::instance().cpuScope(name);
  }
  return {};
}

static inline void LogSemanticDispatchSkip(
    const char *reason, const RenderBatchElement *batch, void *sceneNode,
    void *meshData, const dxvk::war3::render::VisibleRenderableRecord *visible,
    uint32_t meshIndex, uint32_t layerCount) {
  if constexpr (!dxvk::war3::internal::kShadowSemanticDispatchContractProbeEnabled)
    return;

  static std::atomic<uint32_t> s_skipLogCount{0};
  const uint32_t logIndex =
      s_skipLogCount.fetch_add(1u, std::memory_order_relaxed);
  if (!(logIndex < 96u || (logIndex % 2048u) == 0u))
    return;

  dxvk::war3::render::VisibleRenderableRecord empty = {};
  const auto &record = visible != nullptr ? *visible : empty;
  dxvk::war3dbg::Print(
      "DXVK SemanticDispatchSkip: reason=%s part=%p scene=%p mesh=%p "
      "layer=%u sub=%u state=%p visKind=%u queue=%u runtime=%p model=%p "
      "meshIdx=%u visLayer=%u layerCount=%u handle=0x%08X raw=0x%08X\n",
      reason, batch != nullptr ? batch->renderablePart : nullptr, sceneNode,
      meshData, batch != nullptr ? batch->layerIndex : 0u,
      batch != nullptr ? batch->subIndex : 0u,
      batch != nullptr ? batch->layerStatePtr : nullptr,
      uint32_t(record.identity.kind), uint32_t(record.queueKind),
      record.runtimeModelPtr, record.modelResourcePtr, meshIndex,
      record.layerIndex, layerCount, record.identity.jHandle,
      record.identity.rawcode);
}

static inline void MaybeLogSemanticDispatchContract(
    const RenderBatchElement *batch, void *sceneNode, void *meshData) {
  if constexpr (!dxvk::war3::internal::kShadowSemanticDispatchContractProbeEnabled)
    return;

  if (batch == nullptr || batch->renderablePart == nullptr || sceneNode == nullptr ||
      meshData == nullptr) {
    LogSemanticDispatchSkip("null-args", batch, sceneNode, meshData, nullptr,
                            dxvk::war3::render::kInvalidVisibleMeshIndex, 0u);
    return;
  }

  dxvk::war3::render::VisibleRenderableRecord visible = {};
  if (!dxvk::war3::render::VisibleRenderableRegistry::instance()
           .queryByRenderablePart(batch->renderablePart, visible)) {
    LogSemanticDispatchSkip("registry-miss", batch, sceneNode, meshData, nullptr,
                            dxvk::war3::render::kInvalidVisibleMeshIndex, 0u);
    return;
  }

  if (visible.runtimeModelPtr == nullptr) {
    LogSemanticDispatchSkip("no-runtime-model", batch, sceneNode, meshData,
                            &visible, visible.meshIndex, 0u);
    return;
  }

  if (visible.identity.kind != dxvk::war3::render::ObjectKind::Unit &&
      visible.identity.kind != dxvk::war3::render::ObjectKind::Unknown) {
    LogSemanticDispatchSkip("non-unit-kind", batch, sceneNode, meshData,
                            &visible, visible.meshIndex, 0u);
    return;
  }

  if (visible.queueKind !=
      dxvk::war3::render::VisibleRenderableQueueKind::MainQueue) {
    LogSemanticDispatchSkip("non-main-queue", batch, sceneNode, meshData,
                            &visible, visible.meshIndex, 0u);
    return;
  }

  uint32_t meshIndex = visible.meshIndex;
  if (meshIndex == dxvk::war3::render::kInvalidVisibleMeshIndex &&
      !dxvk::war3::SafeReadU32Fast(meshData, dxvk::war3::MeshDataOffsets::MeshIndex,
                                   meshIndex)) {
    LogSemanticDispatchSkip("mesh-index-miss", batch, sceneNode, meshData,
                            &visible, meshIndex, 0u);
    return;
  }

  void *meshInfoTable = nullptr;
  if (meshIndex == dxvk::war3::render::kInvalidVisibleMeshIndex ||
      meshIndex > 4096u ||
      !dxvk::war3::SafeReadPtrFast(sceneNode,
                                   dxvk::war3::SceneNodeOffsets::MeshInfoTable,
                                   meshInfoTable) ||
      meshInfoTable == nullptr ||
      !dxvk::war3::IsReadableRange(meshInfoTable,
                                   (size_t(meshIndex) + 1u) * sizeof(void *))) {
    LogSemanticDispatchSkip("mesh-info-table", batch, sceneNode, meshData,
                            &visible, meshIndex, 0u);
    return;
  }

  void *meshInfo = nullptr;
  std::memcpy(&meshInfo,
              reinterpret_cast<const uint8_t *>(meshInfoTable) +
                  size_t(meshIndex) * sizeof(void *),
              sizeof(meshInfo));
  if (meshInfo == nullptr) {
    LogSemanticDispatchSkip("mesh-info-null", batch, sceneNode, meshData,
                            &visible, meshIndex, 0u);
    return;
  }

  uint32_t layerCount = 0u;
  void *layerStates = nullptr;
  void *layerInfo = nullptr;
  if (!dxvk::war3::SafeReadU32Fast(meshInfo, dxvk::war3::MeshInfoOffsets::LayerCount,
                                   layerCount) ||
      batch->layerIndex >= layerCount ||
      !dxvk::war3::SafeReadPtrFast(meshInfo,
                                   dxvk::war3::MeshInfoOffsets::LayerStates,
                                   layerStates) ||
      layerStates == nullptr ||
      !dxvk::war3::SafeReadPtrFast(meshInfo, dxvk::war3::MeshInfoOffsets::LayerInfo,
                                   layerInfo) ||
      layerInfo == nullptr) {
    LogSemanticDispatchSkip("layer-contract", batch, sceneNode, meshData,
                            &visible, meshIndex, layerCount);
    return;
  }

  void *layerRecords = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(layerInfo,
                                   dxvk::war3::LayerInfoOffsets::LayerRecords,
                                   layerRecords) ||
      layerRecords == nullptr) {
    LogSemanticDispatchSkip("layer-records", batch, sceneNode, meshData,
                            &visible, meshIndex, layerCount);
    return;
  }

  const auto *canonicalRecordPtr =
      reinterpret_cast<const uint8_t *>(layerStates) +
      size_t(batch->layerIndex) * 0x24u;
  const auto *canonicalViewPtr =
      canonicalRecordPtr != nullptr ? canonicalRecordPtr + 4u : nullptr;
  const auto *dispatchPtr =
      reinterpret_cast<const uint8_t *>(layerRecords) +
      size_t(batch->layerIndex) * 0x2Cu;
  if (!dxvk::war3::IsReadableRange(canonicalRecordPtr, 0x24u) ||
      !dxvk::war3::IsReadableRange(dispatchPtr, 0x2Cu)) {
    LogSemanticDispatchSkip("layer-range", batch, sceneNode, meshData, &visible,
                            meshIndex, layerCount);
    return;
  }

  uint32_t primaryBinding = 0u;
  uint32_t blendOrDrawMode = 0u;
  uint32_t auxEnable0 = 0u;
  uint32_t auxEnable1 = 0u;
  dxvk::war3::SafeReadU32Fast(
      canonicalRecordPtr,
      dxvk::war3::MeshLayerStateRecordOffsets::PrimaryResourceBinding,
      primaryBinding);
  dxvk::war3::SafeReadU32Fast(
      canonicalRecordPtr, dxvk::war3::MeshLayerStateRecordOffsets::BlendOrDrawMode,
      blendOrDrawMode);
  dxvk::war3::SafeReadU32Fast(
      canonicalRecordPtr, dxvk::war3::MeshLayerStateRecordOffsets::AuxRefEnable0,
      auxEnable0);
  dxvk::war3::SafeReadU32Fast(
      canonicalRecordPtr, dxvk::war3::MeshLayerStateRecordOffsets::AuxRefEnable1,
      auxEnable1);

  uint32_t auxRefIndex0 = 0u;
  uint32_t auxRefIndex1 = 0u;
  uint32_t stageMode0 = 0u;
  uint32_t stageMode1 = 0u;
  dxvk::war3::SafeReadU32Fast(dispatchPtr,
                              dxvk::war3::MeshLayerDispatchRecordOffsets::AuxRefIndex0,
                              auxRefIndex0);
  dxvk::war3::SafeReadU32Fast(dispatchPtr,
                              dxvk::war3::MeshLayerDispatchRecordOffsets::AuxRefIndex1,
                              auxRefIndex1);
  dxvk::war3::SafeReadU32Fast(dispatchPtr,
                              dxvk::war3::MeshLayerDispatchRecordOffsets::StageMode0,
                              stageMode0);
  dxvk::war3::SafeReadU32Fast(dispatchPtr,
                              dxvk::war3::MeshLayerDispatchRecordOffsets::StageMode1,
                              stageMode1);

  void *auxTable = nullptr;
  dxvk::war3::SafeReadPtrFast(meshData,
                              dxvk::war3::MeshDataOffsets::AuxLayerResourceTable,
                              auxTable);

  const uint8_t *auxEntryPtr0 = nullptr;
  const uint8_t *auxEntryPtr1 = nullptr;
  uint32_t auxEntry0Word0 = 0u;
  uint32_t auxEntry0Binding = 0u;
  uint32_t auxEntry1Word0 = 0u;
  uint32_t auxEntry1Binding = 0u;
  auto readAuxEntry = [&](uint32_t auxIndex, const uint8_t *&outEntryPtr,
                          uint32_t &outWord0, uint32_t &outBinding) {
    outEntryPtr = nullptr;
    outWord0 = 0u;
    outBinding = 0u;
    if (auxTable == nullptr || auxIndex > 2048u)
      return;

    outEntryPtr = reinterpret_cast<const uint8_t *>(auxTable) +
                  size_t(auxIndex) * 0x2Cu;
    if (!dxvk::war3::IsReadableRange(outEntryPtr, 0x2Cu)) {
      outEntryPtr = nullptr;
      return;
    }

    dxvk::war3::SafeReadU32Fast(outEntryPtr, 0x00u, outWord0);
    dxvk::war3::SafeReadU32Fast(
        outEntryPtr, dxvk::war3::MeshAuxResourceEntryOffsets::ResourceBinding,
        outBinding);
  };
  if (auxEnable0 != 0u)
    readAuxEntry(auxRefIndex0, auxEntryPtr0, auxEntry0Word0, auxEntry0Binding);
  if (auxEnable1 != 0u)
    readAuxEntry(auxRefIndex1, auxEntryPtr1, auxEntry1Word0, auxEntry1Binding);

  void *primaryStreamPtr = nullptr;
  void *stream1Ptr = nullptr;
  uint32_t primaryStride = 0u;
  uint32_t primaryArg0 = 0u;
  uint32_t stream1Stride = 0u;
  dxvk::war3::SafeReadPtrFast(meshData,
                              dxvk::war3::MeshDataOffsets::PrimaryStreamPtr,
                              primaryStreamPtr);
  dxvk::war3::SafeReadPtrFast(meshData, dxvk::war3::MeshDataOffsets::Stream1Ptr,
                              stream1Ptr);
  dxvk::war3::SafeReadU32Fast(meshData,
                              dxvk::war3::MeshDataOffsets::PrimaryStreamStride,
                              primaryStride);
  dxvk::war3::SafeReadU32Fast(meshData,
                              dxvk::war3::MeshDataOffsets::PrimaryStreamArg0,
                              primaryArg0);
  dxvk::war3::SafeReadU32Fast(meshData, dxvk::war3::MeshDataOffsets::Stream1Stride,
                              stream1Stride);

  uint32_t batchStateWords[5] = {};
  if (batch->layerStatePtr != nullptr &&
      dxvk::war3::IsReadableRange(batch->layerStatePtr, sizeof(batchStateWords))) {
    std::memcpy(batchStateWords, batch->layerStatePtr, sizeof(batchStateWords));
  }

  uint32_t canonicalRecordWords[9] = {};
  std::memcpy(canonicalRecordWords, canonicalRecordPtr, sizeof(canonicalRecordWords));

  static std::atomic<uint32_t> s_probeLogCount{0};
  const uint32_t logIndex =
      s_probeLogCount.fetch_add(1u, std::memory_order_relaxed);
  if (!(logIndex < 48u || (logIndex % 2048u) == 0u))
    return;

  const long long deltaToRecord =
      batch->layerStatePtr != nullptr
          ? static_cast<long long>(
                reinterpret_cast<const uint8_t *>(batch->layerStatePtr) -
                canonicalRecordPtr)
          : 0ll;
  const long long deltaToView =
      batch->layerStatePtr != nullptr
          ? static_cast<long long>(
                reinterpret_cast<const uint8_t *>(batch->layerStatePtr) -
                canonicalViewPtr)
          : 0ll;

  dxvk::war3dbg::Print(
      "DXVK SemanticDispatchProbe: part=%p scene=%p mesh=%p runtime=%p "
      "model=%p kind=%u meshIdx=%u layer=%u visLayer=%u "
      "batchState=%p canon=%p view=%p dRec=%lld dView=%lld "
      "primary=%p/%u/%u stream1=%p/%u dispatch=%p "
      "bind[p=%u mode=%u stage=%u/%u a0=%u/%u/%p/%u/%u "
      "a1=%u/%u/%p/%u/%u] "
      "batchRaw=%08X/%08X/%08X/%08X/%08X "
      "canonRaw=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X "
      "handle=0x%08X raw=0x%08X\n",
      batch->renderablePart, sceneNode, meshData, visible.runtimeModelPtr,
      visible.modelResourcePtr, uint32_t(visible.identity.kind), meshIndex,
      batch->layerIndex, visible.layerIndex, batch->layerStatePtr,
      canonicalRecordPtr, canonicalViewPtr, deltaToRecord, deltaToView,
      primaryStreamPtr, primaryStride, primaryArg0, stream1Ptr, stream1Stride,
      dispatchPtr, primaryBinding, blendOrDrawMode, stageMode0, stageMode1,
      auxEnable0, auxRefIndex0, auxEntryPtr0, auxEntry0Word0, auxEntry0Binding,
      auxEnable1, auxRefIndex1, auxEntryPtr1, auxEntry1Word0, auxEntry1Binding,
      batchStateWords[0], batchStateWords[1], batchStateWords[2],
      batchStateWords[3], batchStateWords[4], canonicalRecordWords[0],
      canonicalRecordWords[1], canonicalRecordWords[2], canonicalRecordWords[3],
      canonicalRecordWords[4], canonicalRecordWords[5], canonicalRecordWords[6],
      canonicalRecordWords[7], canonicalRecordWords[8], visible.identity.jHandle,
      visible.identity.rawcode);
}

// ============================================================================
// 运行时全局指针（由 Hook 侧解析填充）
// ============================================================================
struct RenderQueueGlobals {
  // 注意：以下地址在 Game.dll
  // 中可能是静态数组（地址即基址）或全局指针（需解引用） 按照 1.27.x 逻辑区分。
  uint32_t *batchCapacityPtr = nullptr;
  uint32_t *numOfElementsPtr = nullptr;
  void **batchArrayPtr = nullptr; // *batchArrayPtr -> dynamic array base
  uint32_t *batchGrowStepPtr = nullptr;
  uint32_t *sortedBatchCountPtr = nullptr;
  void **sortedBatchPtrs =
      nullptr; // Address of static array g_SortedPtrs[10000]

  uint32_t *stateOptEnabledPtr = nullptr;
  uint32_t *stateCleanupPendingPtr = nullptr;

  // 透明批次
  uint32_t *aucTransparentCountPtr = nullptr;
  void **aucTransparentArrayBase =
      nullptr; // *aucTransparentArrayBase -> dynamic array base
  void **aucTransparentSortedPtrs =
      nullptr; // Address of static array AUCTransparent_SortedPtrs[10000]
};

// 兼容 qsort comparator 的函数签名（a/b 指向“指针数组的元素”）
using ItemComparatorFn = int(__cdecl *)(const void *a, const void *b);

// Dispatch 函数签名（与现有 Hook 的 Dispatch_Common/Special 对齐）
// 注意：ecx 是 SceneNode（renderablePart+0x14 回填的 backptr），不是 LayerStatePtr。
// 误传 LayerStatePtr 会导致状态解析错误，触发 TeamColor 污染与排序异常。
using DispatchCommonFn = int(__fastcall *)(void *sceneNode,
                                           void *renderablePart,
                                           void *layerIndex, void *layerChanged,
                                           void *stateChanged);
using DispatchSpecialFn = int(__fastcall *)(void *sceneNode,
                                            void *renderablePart,
                                            void *layerIndex,
                                            void *stateChanged);

// RenderBatch_Submit 相关函数
using RenderQueueReserveBatchArrayFn = int(__thiscall *)(uint32_t *globalsBase,
                                                         uint32_t newCapacity);
using RenderQueueComputeBatchGrowStepFn =
    int(__thiscall *)(uint32_t *globalsBase, uint32_t minNeeded);
using RenderBatchCanEnqueueFn = int(__fastcall *)(void *sceneNode,
                                                  void *renderablePart);
using TransformPointFn = float *(__fastcall *)(float *out, float *in,
                                               float *matrix);
using AddTransparentFn = void(__fastcall *)(void *renderablePart, int type,
                                            float *pos, uint32_t key);

// 关键副作用函数（来自 IDA 反编译确认）
// - applyStateBlock: 0x0E34B0，原始 FlushSortedItems 会在遍历前对首元素调用一次
// - stageUpdate: 0x13A9B0，原始 FlushAndReset 传 1，FlushSortedItems 每个元素传
// 0
// - gxCleanup74/78: 0x0E3640/0x0E3670，StateCleanupPending!=0 时触发
using ApplyStateBlockFn = void(__fastcall *)(void *statePtr);
using StageUpdateFn = void(__fastcall *)(void *thisPtr);
using GxCleanupFn = int(__cdecl *)();

// [CONTEXT MERGE] 上下文管理函数签名 (值传递优化版)
using GetTrackerTagStageFn = void (*)(void *renderablePart,
                                      dxvk::War3BatchTag &tag, int &stage);
using ExecBeginValueFn = void (*)(void *element, dxvk::War3BatchTag tag,
                                  int elementStage, bool isType3, void *outCtx);
using ExecEndValueFn = void (*)(void *ctx);

struct RenderQueueFns {
  ApplyStateBlockFn applyStateBlock = nullptr;
  StageUpdateFn stageUpdate = nullptr;
  GxCleanupFn gxCleanup74 = nullptr;
  GxCleanupFn gxCleanup78 = nullptr;

  // 上下文合并专用
  GetTrackerTagStageFn getTrackerTagStage = nullptr;
  ExecBeginValueFn execBeginValue = nullptr;
  ExecEndValueFn execEndValue = nullptr;

  // 透明渲染子分发通路 (RVA 见 d3d9_war3_hook.cpp)
  void(__fastcall *sub_13A0E0)(uint32_t, void *) = nullptr;
  void(__fastcall *sub_198C00)(void *) = nullptr;
  void(__fastcall *sub_19DFF0)(void *) = nullptr;
  void(__fastcall *sub_19BC20)(void *) = nullptr;
  void(__fastcall *sub_13A0B0)(void *) = nullptr;
};

struct RenderBatchFns {
  RenderQueueReserveBatchArrayFn reserveBatchArray = nullptr;
  RenderQueueComputeBatchGrowStepFn computeBatchGrowStep = nullptr;
  RenderBatchCanEnqueueFn canEnqueueToMainQueue = nullptr;
  TransformPointFn transformPoint = nullptr;
  AddTransparentFn addTransparent = nullptr;
};

// ============================================================================
// [State-Aware Batch Breaking] Global Accessors (Forward Declarations)
// ============================================================================

inline auto &GetPendingInstances() {
  static std::vector<RenderBatchElement *> v;
  return v;
}

inline auto &GetPendingTag() {
  static War3BatchTag t = War3BatchTag::Unknown;
  return t;
}

inline auto &GetPendingStage() {
  static int s = -1;
  return s;
}

inline auto &GetPendingVP() {
  static D3DVIEWPORT9 v;
  return v;
}

inline auto &GetPendingView() {
  static D3DMATRIX m;
  return m;
}

inline auto &GetPendingProj() {
  static D3DMATRIX m;
  return m;
}

// Dependencies
inline auto &GetNativeFuncs() {
  static RenderQueueFns f = {};
  return f;
}

inline auto &GetInstBufPtr() {
  static War3InstanceBuffer *p = nullptr;
  return p;
}

inline auto &GetDispatchCommon() {
  static DispatchCommonFn f = nullptr;
  return f;
}

inline auto &GetDispatchSpecial() {
  static DispatchSpecialFn f = nullptr;
  return f;
}

inline auto &GetStateOptEnabled() {
  static bool b = true;
  return b;
}

// Global Flush Callback
inline auto &GetBatchFlushCallback() {
  static std::function<void(D3D9DeviceEx *)> cb;
  return cb;
}

// Flush Logic (Extracted)
inline void FlushPendingBatches(D3D9DeviceEx *device) {
  // [CRITICAL] Recursion Guard: dispatchCommon may call
  // SetTexture/SetRenderState which would trigger this function again, causing
  // infinite recursion
  static bool s_isFlushingBatch = false;
  if (s_isFlushingBatch)
    return;

  auto &pendingInstances = GetPendingInstances();
  if (pendingInstances.empty())
    return;

  auto &fns = GetNativeFuncs();
  auto dispatchCommon = GetDispatchCommon();
  auto *instBuf = GetInstBufPtr();
  if (!instBuf)
    instBuf = War3InstanceBuffer::Get(device);

  if (!fns.execBeginValue || !dispatchCommon)
    return;

  // Set guard AFTER all early returns
  s_isFlushingBatch = true;

  auto &pendingTag = GetPendingTag();
  auto &pendingStage = GetPendingStage();

  // [Context Safety] Restore Pending State
  D3DVIEWPORT9 savedVP;
  device->GetViewport(&savedVP);
  D3DMATRIX savedView;
  device->GetTransform(D3DTS_VIEW, &savedView);
  D3DMATRIX savedProj;
  device->GetTransform(D3DTS_PROJECTION, &savedProj);

  device->SetViewport(&GetPendingVP());
  device->SetTransform(D3DTS_VIEW, &GetPendingView());
  device->SetTransform(D3DTS_PROJECTION, &GetPendingProj());

  // Alloc
  uint32_t count = (uint32_t)pendingInstances.size();
  uint32_t baseOffset = 0;

  // [Fix #7] Viewport Check
  bool isPortrait = false;
  {
    D3DVIEWPORT9 cvp;
    device->GetViewport(&cvp);
    if (cvp.Width < 300)
      isPortrait = true;
  }

  // Path selection
  InstanceData *bufPtr = nullptr;
  if (!isPortrait) {
    bufPtr = instBuf->Alloc(count, baseOffset);
  }

  if (bufPtr) {
    dxvk::war3::render::ExecBatchContext currentCtx;
    bool contextActive = false;

    if (fns.execBeginValue) {
      fns.execBeginValue(pendingInstances[0]->renderablePart, pendingTag,
                         pendingStage, true, &currentCtx);
      contextActive = true;
    }

    // [FIX] 确保 OnSetTexture 不会干扰 dispatchCommon 循环
    // 在循环期间禁用纹理追踪，防止捕获/恢复错误的纹理状态
    War3InstanceBuffer::SetActive(nullptr);

    // Replay Logic: dispatchCommon triggers War3InstanceBuffer hooks
    for (size_t i = 0; i < count; i++) {
      auto *b = pendingInstances[i];
      void *sceneNode = nullptr;
      if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
        sceneNode = *reinterpret_cast<void **>(
            reinterpret_cast<uint8_t *>(b->renderablePart) + 0x14);
      } else {
        dxvk::war3::SafeReadPtrFast(b->renderablePart, 0x14, sceneNode);
      }
      dispatchCommon(sceneNode, b->renderablePart,
                     reinterpret_cast<void *>(uintptr_t(b->layerIndex)),
                     reinterpret_cast<void *>(uintptr_t(1)),
                     reinterpret_cast<void *>(uintptr_t(1)));
    }

    if (contextActive && fns.execEndValue) {
      fns.execEndValue(&currentCtx);
    }

    War3InstanceBuffer::SetActive(nullptr);
    instBuf->Unlock();
    instBuf->FlushBatch();
  } else {
    // Path C: Direct Draw Fallback
    dxvk::war3::render::ExecBatchContext currentCtx;
    bool contextActive = false;
    if (fns.execBeginValue) {
      fns.execBeginValue(pendingInstances[0]->renderablePart, pendingTag,
                         pendingStage, true, &currentCtx);
      contextActive = true;
    }

    War3InstanceBuffer::SetActive(nullptr);
    if (instBuf)
      instBuf->Unlock();

    // [FIX] 强制清除 Stage 1 纹理，防止 Team Color (Stage 1) 被上一个 Batch
    // 污染 用户报告：基尔加丹的纹理泄漏到了头像框背景
    // device->SetTexture(1, nullptr);

    for (auto *b : pendingInstances) {
      void *sceneNode = nullptr;
      if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
        sceneNode = *reinterpret_cast<void **>(
            reinterpret_cast<uint8_t *>(b->renderablePart) + 0x14);
      } else {
        dxvk::war3::SafeReadPtrFast(b->renderablePart, 0x14, sceneNode);
      }
      dispatchCommon(sceneNode, b->renderablePart,
                     reinterpret_cast<void *>(uintptr_t(b->layerIndex)),
                     reinterpret_cast<void *>(uintptr_t(1)),
                     reinterpret_cast<void *>(uintptr_t(1)));
    }

    if (contextActive && fns.execEndValue) {
      fns.execEndValue(&currentCtx);
    }
  }

  pendingInstances.clear();

  // [Context Safety] Restore Current
  device->SetViewport(&savedVP);
  device->SetTransform(D3DTS_VIEW, &savedView);
  device->SetTransform(D3DTS_PROJECTION, &savedProj);

  // Reset recursion guard
  s_isFlushingBatch = false;
}

// ============================================================================
// RenderQueue - 复现游戏渲染队列
// ============================================================================
class RenderQueue {
public:
  static constexpr uint32_t MAX_BATCHES_PER_FRAME = 10000;
  static constexpr size_t kBatchElementStride =
      20; // RenderBatchElement 在 32 位下固定为 20 字节
  static constexpr size_t kLayerStateStride = 36;
  static constexpr size_t kTransparentEntryStride = 24;

  // 透明批次条目结构
  struct AUCTransparentEntry {
    int type;         // +0x00
    uint32_t sortKey; // +0x04
    int arg0;         // +0x08
    void *payload;    // +0x0C
    int arg1;         // +0x10
    int arg2;         // +0x14
  };
  static constexpr size_t kLayerDataStride = 44;
  static constexpr size_t kLayerStateCompareBytes = 20; // 原版仅比较前 20 字节

  // 原始函数签名 (用于 Hook 替换)
  // RVA 0x139800: void FlushAndReset()
  // RVA 0x1380A0: void FlushSortedItems()
  // RVA 0x1375C0: void RenderBatch_Submit(void* this)
  // RVA 0x139190: void RenderQueue_AddBatch(void* this)

  // 批次比较器 (用于 qsort)
  // 原始实现: RVA 0x1378B0 (RenderQueue_ItemComparator)
  static int BatchComparator(const void *a, const void *b) {
    const RenderBatchElement *batchA =
        *static_cast<const RenderBatchElement *const *>(a);
    const RenderBatchElement *batchB =
        *static_cast<const RenderBatchElement *const *>(b);

    // 按 RenderablePart 排序 (减少网格切换)
    if (batchA->renderablePart != batchB->renderablePart) {
      return batchA->renderablePart < batchB->renderablePart ? -1 : 1;
    }
    // 同对象按子索引排序 (对应可见层顺序)
    if (batchA->subIndex != batchB->subIndex) {
      return batchA->subIndex < batchB->subIndex ? -1 : 1;
    }
    return 0;
  }

  // FlushSortedItems：用 std::sort 替换 qsort（先保证兼容性，再逐步内联
  // comparator）
  //
  // - gameComparator: 传入游戏原 comparator（推荐）。为空时退化为本地
  // 内部辅助排序 - 使用内联汇编实现的高性能比较器
  // 注：透明队列崩溃问题已在 FlushTransparent_StdSort 中单独修复

  // 原生 RenderQueue_ItemLess 的内联汇编实现 (RVA 0x137D50)
  // 调用约定: __fastcall (ecx=a, edx=b)
  // 返回值: int (a < b ? 非零 : 0)
#if defined(_M_IX86) && defined(_MSC_VER)
  __declspec(noinline) static int __fastcall
  ItemLess_Asm(const RenderBatchElement *a, const RenderBatchElement *b) {
    // 使用优化的 C++ 实现（与原生算法完全一致，但避免内联汇编的兼容性问题）
    // 阶段 1: 比较 (flags & 3) == 3
    const uint32_t flagsA = a->flags;
    const uint32_t flagsB = b->flags;
    const bool isSpecialA = (flagsA & 3) == 3;
    const bool isSpecialB = (flagsB & 3) == 3;
    if (isSpecialA != isSpecialB)
      return isSpecialA ? 1 : 0;

    // 阶段 2: 比较 flags & 2
    const bool hasFlag2A = (flagsA & 2) != 0;
    const bool hasFlag2B = (flagsB & 2) != 0;

    if (hasFlag2A) {
      if (!hasFlag2B)
        return 1; // A 有 flag2，B 没有

      // 比较 renderablePart->meshData (+0x0C)
      const uint32_t meshA = *reinterpret_cast<const uint32_t *>(
          reinterpret_cast<const uint8_t *>(a->renderablePart) + 0x0C);
      const uint32_t meshB = *reinterpret_cast<const uint32_t *>(
          reinterpret_cast<const uint8_t *>(b->renderablePart) + 0x0C);
      if (meshA != meshB)
        return meshA < meshB ? 1 : 0;

      // 比较 subIndex
      if (a->subIndex != b->subIndex)
        return a->subIndex < b->subIndex ? 1 : 0;

      // 比较 layerStatePtr[3] 和前 20 字节
      const uint32_t *lsA =
          reinterpret_cast<const uint32_t *>(a->layerStatePtr);
      const uint32_t *lsB =
          reinterpret_cast<const uint32_t *>(b->layerStatePtr);
      if (lsA[3] != lsB[3])
        return lsA[3] < lsB[3] ? 1 : 0;

      // 逐 DWORD 比较前 20 字节 (5 DWORDs)
      for (int i = 0; i < 5; i++) {
        if (lsA[i] != lsB[i])
          return lsA[i] < lsB[i] ? 1 : 0;
      }
      return 1; // 完全相等
    }

    // A 没有 flag2
    if (hasFlag2B)
      return 0; // B 有 flag2

    // 两者都没有 flag2: 比较 layerStatePtr
    const uint32_t *lsA = reinterpret_cast<const uint32_t *>(a->layerStatePtr);
    const uint32_t *lsB = reinterpret_cast<const uint32_t *>(b->layerStatePtr);
    if (lsA[3] != lsB[3])
      return lsA[3] < lsB[3] ? 1 : 0;

    // 逐 DWORD 比较前 20 字节
    for (int i = 0; i < 5; i++) {
      if (lsA[i] != lsB[i])
        return lsA[i] < lsB[i] ? 1 : 0;
    }

    // 完全相等：比较 renderablePart->meshData
    const uint32_t meshA = *reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(a->renderablePart) + 0x0C);
    const uint32_t meshB = *reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(b->renderablePart) + 0x0C);
    return meshA <= meshB ? 1 : 0;
  }
#endif

  static void InnerSort(void **sortedPtrs, uint32_t count,
                        ItemComparatorFn gameComparator) {
#if defined(_M_IX86) && defined(_MSC_VER)
    // 使用优化的内联比较器
    std::sort(sortedPtrs, sortedPtrs + count, [](void *lhs, void *rhs) {
      if (lhs == rhs)
        return false;
      return ItemLess_Asm(reinterpret_cast<const RenderBatchElement *>(lhs),
                          reinterpret_cast<const RenderBatchElement *>(rhs)) !=
             0;
    });
#else
    // 回退：使用原生比较器
    const auto addrLess = [](const void *lhs, const void *rhs) {
      return reinterpret_cast<uintptr_t>(lhs) <
             reinterpret_cast<uintptr_t>(rhs);
    };

    std::sort(sortedPtrs, sortedPtrs + count, [&](void *lhs, void *rhs) {
      if (lhs == rhs)
        return false;

      void *a = lhs;
      void *b = rhs;
      const int ab = gameComparator(&a, &b);
      if (ab < 0) {
        void *ra = rhs;
        void *rb = lhs;
        const int ba = gameComparator(&ra, &rb);
        if (ba < 0)
          return addrLess(lhs, rhs);
        return true;
      }
      return (ab == 0) ? addrLess(lhs, rhs) : false;
    });
#endif
  }

  // FlushSortedItems：用 std::sort 替换 qsort
  static inline bool FlushSortedItems_StdSort(D3D9DeviceEx *device,
                                              const RenderQueueGlobals &g,
                                              ItemComparatorFn gameComparator,
                                              DispatchCommonFn dispatchCommon,
                                              DispatchSpecialFn dispatchSpecial,
                                              const RenderQueueFns &fns) {
#if !defined(_M_IX86) && !defined(__i386__)
    return false;
#endif
    if (!g.numOfElementsPtr || !g.batchArrayPtr || !g.sortedBatchCountPtr ||
        !g.sortedBatchPtrs)
      return false;

    const uint32_t num = *g.numOfElementsPtr;
    void *batchArrayRaw = *g.batchArrayPtr;
    void **sortedPtrs = g.sortedBatchPtrs;
    if (!batchArrayRaw || !sortedPtrs)
      return false;

    const uint32_t count = (std::min)(num, MAX_BATCHES_PER_FRAME);
    *g.sortedBatchCountPtr = count;

    uint8_t *batchBase = reinterpret_cast<uint8_t *>(batchArrayRaw);
    for (uint32_t i = 0; i < count; i++) {
      sortedPtrs[i] = batchBase + i * kBatchElementStride;
    }

    // 3) 遍历调度（保持与原始流程一致：Common/Special）
    if (!dispatchCommon || !dispatchSpecial)
      return false;

    // [TEST] 强制禁用状态优化，验证是否是比较逻辑导致的状态泄漏
    const bool stateOptEnabled = (g.stateOptEnabledPtr != nullptr)
                                     ? (*g.stateOptEnabledPtr != 0u)
                                     : true;

    // 渲染队列 Instancing（实验性）开关
    const bool instancingEnabled =
        dxvk::war3::internal::kNativeQueueAutoInstancingEnabled;

    auto &pendingInstances = GetPendingInstances();
    if (!instancingEnabled && !pendingInstances.empty()) {
      // 关闭 Instancing 时清空残留，避免跨帧污染
      pendingInstances.clear();
    }

    War3InstanceBuffer *instBuf = nullptr;
    if (instancingEnabled) {
      // 仅在启用 Instancing 时初始化全局状态，降低常规路径成本
      GetDispatchCommon() = dispatchCommon;
      GetDispatchSpecial() = dispatchSpecial;
      GetNativeFuncs() = fns;
      GetStateOptEnabled() = stateOptEnabled;

      auto &instBufPtr = GetInstBufPtr();
      if (!instBufPtr)
        instBufPtr = dxvk::war3::reimpl::War3InstanceBuffer::Get(device);
      instBuf = instBufPtr;

      // Register Callback (Lazy Init)
      auto &cb = GetBatchFlushCallback();
      if (!cb) {
        cb = [](D3D9DeviceEx *dev) { FlushPendingBatches(dev); };
      }
    }

    // [SAFETY] 指针验证（避免战役/特殊场景下的崩溃）
    //
    // Gemini 版本曾在这里对每个元素调用 IsReadableRange，导致在对象很多时出现
    // 大量 VirtualQuery（非常慢），表现为“镜头靠近对象立刻未响应”。
    //
    // 这里改为：
    // 1) 仅对批次数组与指针数组做“增量范围校验”（最多增长时触发一次
    // VirtualQuery） 2) 对每个元素做纯算术范围校验（不触发 VirtualQuery）

    const size_t batchBytes = size_t(count) * kBatchElementStride;
    const size_t ptrBytes = size_t(count) * sizeof(void *);

    // 快速路径：跳过冗余的内存安全检查（原版游戏不做这些检查）
    if constexpr (!dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
      // 慢速路径：完整的安全检查
      {
        static void *s_lastBatchArray = nullptr;
        static size_t s_lastBatchBytes = 0;
        static bool s_batchArrayOk = false;
        if (batchArrayRaw != s_lastBatchArray ||
            batchBytes > s_lastBatchBytes) {
          s_lastBatchArray = batchArrayRaw;
          s_lastBatchBytes = batchBytes;
          s_batchArrayOk = (batchBytes == 0) ||
                           dxvk::war3::IsReadableRange(batchBase, batchBytes);
        }
        if (!s_batchArrayOk)
          return false;
      }

      {
        static void *s_lastSortedPtrs = nullptr;
        static size_t s_lastPtrBytes = 0;
        static bool s_sortedPtrsOk = false;
        if (sortedPtrs != s_lastSortedPtrs || ptrBytes > s_lastPtrBytes) {
          s_lastSortedPtrs = sortedPtrs;
          s_lastPtrBytes = ptrBytes;
          s_sortedPtrsOk = (ptrBytes == 0) ||
                           dxvk::war3::IsReadableRange(sortedPtrs, ptrBytes);
        }
        if (!s_sortedPtrsOk)
          return false;
      }

      const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(batchBase);
      const uintptr_t endAddr = baseAddr + batchBytes;

      // 预扫描：由于开启了
      // kNativeFlushUnsafePathEnabled，我们相信游戏内部队列的有效性，跳过 O(N)
      // 检查以节省 ~2ms CPU 开销。
      if (!dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
        for (uint32_t i = 0; i < count; i++) {
          auto *batch = reinterpret_cast<RenderBatchElement *>(sortedPtrs[i]);
          const uintptr_t batchAddr = reinterpret_cast<uintptr_t>(batch);
          if (!batch || batchAddr < baseAddr ||
              batchAddr + sizeof(RenderBatchElement) > endAddr ||
              ((batchAddr - baseAddr) % kBatchElementStride) != 0) {
            return false;
          }

          const uintptr_t entryAddr =
              reinterpret_cast<uintptr_t>(batch->renderablePart);
          if (entryAddr < 0x1000u)
            return false;

          if (!dxvk::war3::IsReadableRangeFast(batch->renderablePart, 0x18))
            return false;
        }
      }
    }

    if (!count) {
      // 即便没有批次，原始实现仍可能需要执行一次阶段更新/尾部清理。
      if (fns.stageUpdate)
        fns.stageUpdate(0);

      if (g.stateCleanupPendingPtr && *g.stateCleanupPendingPtr) {
        if (fns.gxCleanup74)
          fns.gxCleanup74();
        if (fns.gxCleanup78)
          fns.gxCleanup78();
        *g.stateCleanupPendingPtr = 0;
      }
      return true;
    }

    {
      auto sortScope = MakeQueueCpuScope("FQ_Sort_Opaque");
      if (count > 1 && gameComparator) {
        bool needSort = true;

        // 中小批次优先做“已排序预检”：若本身有序则直接跳过排序。
        if constexpr (dxvk::war3::internal::kNativeQueueSkipSortIfAlreadySorted) {
          if (count <= dxvk::war3::internal::kNativeQueueSkipSortCheckMaxCount) {
            needSort = false;
            for (uint32_t i = 1; i < count; i++) {
              void **lhs = &sortedPtrs[i - 1];
              void **rhs = &sortedPtrs[i];
              if (gameComparator(lhs, rhs) > 0) {
                needSort = true;
                break;
              }
            }
          }
        }

        if (needSort) {
          InnerSort(sortedPtrs, count, gameComparator);
        }
      } else if (count > 1) {
        // 本地简单回调排序 (Fallback)
        std::sort(sortedPtrs, sortedPtrs + count, [](void *lhs, void *rhs) {
          auto *a = reinterpret_cast<RenderBatchElement *>(lhs);
          auto *b = reinterpret_cast<RenderBatchElement *>(rhs);
          if (a->renderablePart != b->renderablePart)
            return a->renderablePart < b->renderablePart;
          return a->subIndex < b->subIndex;
        });
      }
    }

    // 初始状态应用（必须在排序完成后，取首元素）
    void *lastLayerStatePtr =
        reinterpret_cast<RenderBatchElement *>(sortedPtrs[0])->layerStatePtr;
    if (fns.applyStateBlock && lastLayerStatePtr &&
        dxvk::war3::IsReadableRangeFast(lastLayerStatePtr, 36))
      fns.applyStateBlock(lastLayerStatePtr);

    auto dispatchScope = MakeQueueCpuScope("FQ_Dispatch_Opaque");

    void *lastMeshData = nullptr;
    uint32_t lastLayerIndex = 0;
    bool lastWasSpecial = false;

    constexpr bool kDiagStatsEnabled =
        dxvk::war3::internal::kNativeRenderQueueDiagnosticStatsEnabled;

    // [STATS] 批次合并诊断统计（仅在诊断开关开启时生效）
    uint32_t statsLayerUnchanged = 0; // layerChanged=0 的次数
    uint32_t statsStateUnchanged = 0; // stateChanged=0 的次数
    uint32_t statsCurrentRun = 1;     // 当前连续相同状态的批次数
    uint32_t statsMaxRun = 1;         // 最大连续相同状态批次数
    uint32_t statsCommonCalls = 0;    // dispatchCommon 调用次数
    uint32_t statsSpecialCalls = 0;   // dispatchSpecial 调用次数
    uint32_t statsContextMerges = 0;  // 成功合并上下文的次数

    // 上下文追踪
    void *lastBeginSceneNode = nullptr;
    dxvk::war3::render::ExecBatchContext currentCtx;
    bool contextActive = false;
    bool lastWasType3 = false;

    // 分部分（RenderablePart）缓存：连续批次大概率属于同一部分，避免重复
    // GetTagStage
    void *lastTaggedPart = nullptr;
    dxvk::War3BatchTag lastPartTag = dxvk::War3BatchTag::Unknown;
    int lastPartStage = -1;
    // SceneNode 级缓存：同一单位的多个 RenderablePart 连续出现时，
    // 可复用最近一次 tag/stage，减少 tracker 查询开销。
    void *lastTaggedSceneNode = nullptr;
    dxvk::War3BatchTag lastSceneTag = dxvk::War3BatchTag::Unknown;
    int lastSceneStage = -1;

    // [BATCH MERGE PROTOTYPE]
    War3BatchMerger batchMerger;
    batchMerger.Reset();

    for (uint32_t i = 0; i < count; i++) {
      auto *batch = reinterpret_cast<RenderBatchElement *>(sortedPtrs[i]);
      if (!batch || !batch->renderablePart)
        continue;

      void *renderablePart = batch->renderablePart;

      // 快速路径：直接内存读取（原版游戏的做法）
      void *sceneNode = nullptr;
      void *meshData = nullptr;
      if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
        // 直接读取，不做安全检查
        sceneNode = *reinterpret_cast<void **>(
            reinterpret_cast<uint8_t *>(renderablePart) + 0x14);
        meshData = *reinterpret_cast<void **>(
            reinterpret_cast<uint8_t *>(renderablePart) + 0x0C);
        if (!sceneNode || !meshData)
          continue;
      } else {
        // 慢速路径：安全检查
        if (!dxvk::war3::SafeReadPtrFast(renderablePart, 0x14, sceneNode) ||
            !sceneNode) {
          continue;
        }
        if (!dxvk::war3::SafeReadPtrFast(renderablePart, 0x0C, meshData) ||
            !meshData) {
          continue;
        }
      }

      // stateChanged：严格复现原始逻辑
      bool stateChanged = true;
      if (stateOptEnabled && meshData && meshData == lastMeshData &&
          batch->layerIndex == lastLayerIndex) {
        // 快速路径：直接读取 meshFlag
        if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
          uint32_t meshFlag104 = *reinterpret_cast<uint32_t *>(
              reinterpret_cast<uint8_t *>(meshData) + 0x104);
          if (meshFlag104 == 0) {
            stateChanged = false;
          }
        } else {
          uint32_t meshFlag104 = 0;
          if (dxvk::war3::SafeReadU32Fast(meshData, 0x104, meshFlag104) &&
              meshFlag104 == 0) {
            stateChanged = false;
          }
        }
      }

      const bool isSpecial = ((batch->flags & 3) == 3);

      // [CONTEXT MERGE] 核心优化
      // 如果属于同一个 SceneNode，我们可以复用 ExecBatch 状态

      // We calculate tag/stage early for BatchMerge prototype
      dxvk::War3BatchTag currentTag = dxvk::War3BatchTag::Unknown;
      int currentStage = -1;
      bool tagStageReady = false;
      auto ensureTagStage = [&]() {
        if (tagStageReady)
          return;
        tagStageReady = true;
        // RenderablePart 级缓存
        if (renderablePart == lastTaggedPart) {
          currentTag = lastPartTag;
          currentStage = lastPartStage;
          return;
        }
        // SceneNode 级缓存
        if (sceneNode && sceneNode == lastTaggedSceneNode) {
          currentTag = lastSceneTag;
          currentStage = lastSceneStage;
          lastTaggedPart = renderablePart;
          lastPartTag = currentTag;
          lastPartStage = currentStage;
          return;
        }
        if (fns.getTrackerTagStage) {
          fns.getTrackerTagStage(renderablePart, currentTag, currentStage);
          lastTaggedPart = renderablePart;
          lastPartTag = currentTag;
          lastPartStage = currentStage;
          lastTaggedSceneNode = sceneNode;
          lastSceneTag = currentTag;
          lastSceneStage = currentStage;
        }
      };

      // [BATCH MERGE PROTOTYPE] 仅在诊断模式启用，避免热路径常驻分析开销。
      if constexpr (kDiagStatsEnabled) {
        ensureTagStage();
        batchMerger.Analyze(batch, currentTag);
      }

      // [War3] Auto-Instancing (State-Aware)
      if (instancingEnabled) {
        auto &pendingTag = GetPendingTag();
        auto &pendingStage = GetPendingStage();
        auto &pendingVP = GetPendingVP();
        auto &pendingView = GetPendingView();
        auto &pendingProj = GetPendingProj();

        // 现在可以启用了，因为 d3d9_device.cpp 中的 SetTexture
        // 钩子会检测状态变化并自动拆分 batch
        ensureTagStage();
        bool isInstancing =
            (currentTag == dxvk::War3BatchTag::WorldObjects && !isSpecial);
        bool mergeable = false;

        if (isInstancing && instBuf) {
          if (pendingInstances.empty()) {
            mergeable = true;
            pendingTag = currentTag;
            pendingStage = currentStage;

            // [Context Safety] Capture Initial State
            device->GetViewport(&pendingVP);
            device->GetTransform(D3DTS_VIEW, &pendingView);
            device->GetTransform(D3DTS_PROJECTION, &pendingProj);
          } else {
            // [RESTORED] Merge Logic
            auto *first = pendingInstances[0];

            // [War3] Batch by MeshData (Geometry) instead of RenderablePart
            // (Unit)
            void *firstMeshData = nullptr;
            if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
              firstMeshData = *reinterpret_cast<void **>(
                  reinterpret_cast<uint8_t *>(first->renderablePart) + 0x0C);
            } else {
              dxvk::war3::SafeReadPtrFast(first->renderablePart, 0x0C,
                                          firstMeshData);
            }

            if (firstMeshData == meshData &&
                first->layerIndex == batch->layerIndex &&
                first->layerStatePtr == batch->layerStatePtr &&
                pendingInstances.size() <
                    4096) { // [LIMIT] Prevent massive batches
              mergeable = true;
            }
          }
        }

        // [Context Safety] Check State Consistency
        if (mergeable) {
          D3DVIEWPORT9 currVP;
          device->GetViewport(&currVP);
          D3DMATRIX currView;
          device->GetTransform(D3DTS_VIEW, &currView);
          D3DMATRIX currProj;
          device->GetTransform(D3DTS_PROJECTION, &currProj);

          if (std::memcmp(&currVP, &pendingVP, sizeof(D3DVIEWPORT9)) != 0 ||
              std::memcmp(&currView, &pendingView, sizeof(D3DMATRIX)) != 0 ||
              std::memcmp(&currProj, &pendingProj, sizeof(D3DMATRIX)) != 0) {
            mergeable = false;
          }
        }

        if (!mergeable && !pendingInstances.empty()) {
          // [Context Safety] Save Current & Restore Pending
          D3DVIEWPORT9 savedVP;
          device->GetViewport(&savedVP);
          D3DMATRIX savedView;
          device->GetTransform(D3DTS_VIEW, &savedView);
          D3DMATRIX savedProj;
          device->GetTransform(D3DTS_PROJECTION, &savedProj);

          device->SetViewport(&pendingVP);
          device->SetTransform(D3DTS_VIEW, &pendingView);
          device->SetTransform(D3DTS_PROJECTION, &pendingProj);
          uint32_t count = (uint32_t)pendingInstances.size();
          uint32_t baseOffset = 0;

          // [Fix] Exclude Portraits from Batching (Small Viewport)
          D3DVIEWPORT9 vp;
          bool isPortrait =
              SUCCEEDED(device->GetViewport(&vp)) && (vp.Width < 300);

          auto *bufPtr =
              (isPortrait || !instBuf) ? nullptr : instBuf->Alloc(count, baseOffset);

          if (bufPtr) {
          if (contextActive && fns.execEndValue) {
            fns.execEndValue(&currentCtx);
            contextActive = false;
          }

          if (fns.execBeginValue) {
            auto *first = pendingInstances[0];
            fns.execBeginValue(first->renderablePart, pendingTag, pendingStage,
                               true, &currentCtx);
            contextActive = true;
          }

          dxvk::war3::reimpl::War3InstanceBuffer::SetActive(instBuf);

          // [Step 1] Dispatch First + Check Compatibility
          auto *first = pendingInstances[0];
          void *sceneNodeFirst = nullptr;
          if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
            sceneNodeFirst = *reinterpret_cast<void **>(
                reinterpret_cast<uint8_t *>(first->renderablePart) + 0x14);
          } else {
            dxvk::war3::SafeReadPtrFast(first->renderablePart, 0x14,
                                        sceneNodeFirst);
          }
          dispatchCommon(sceneNodeFirst, first->renderablePart,
                         reinterpret_cast<void *>(uintptr_t(first->layerIndex)),
                         reinterpret_cast<void *>(uintptr_t(1)),
                         reinterpret_cast<void *>(uintptr_t(1)));
          if constexpr (kDiagStatsEnabled)
            statsCommonCalls++;

          bool canInstance = true;
          IDirect3DVertexShader9 *vs = nullptr;
          if (SUCCEEDED(device->GetVertexShader(&vs))) {
            if (!vs) {
              canInstance = false; // Fixed Function Pipeline -> No Instancing
            } else {
              // Use reinterpret_cast to bypass incomplete type / inheritance
              // visibility issues We know IDirect3DVertexShader9* is
              // implemented by dxvk::D3D9VertexShader*
              auto *dxvkVS = reinterpret_cast<dxvk::D3D9VertexShader *>(vs);
              if (!dxvkVS->IsInstanced())
                canInstance = false; // Shader not patched
              vs->Release();
            }
          } else {
            canInstance = false;
          }

          if (canInstance) {
            // [Path A] Instanced Rendering
            for (size_t i = 1; i < pendingInstances.size(); i++) {
              auto *b = pendingInstances[i];
              void *sceneNodeB = nullptr;
              if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
                sceneNodeB = *reinterpret_cast<void **>(
                    reinterpret_cast<uint8_t *>(b->renderablePart) + 0x14);
              } else {
                dxvk::war3::SafeReadPtrFast(b->renderablePart, 0x14,
                                            sceneNodeB);
              }
              dispatchCommon(sceneNodeB, b->renderablePart,
                             reinterpret_cast<void *>(uintptr_t(b->layerIndex)),
                             reinterpret_cast<void *>(uintptr_t(1)),
                             reinterpret_cast<void *>(uintptr_t(1)));
              if constexpr (kDiagStatsEnabled)
                statsCommonCalls++;
            }
            dxvk::war3::reimpl::War3InstanceBuffer::SetActive(nullptr);
            instBuf->Unlock();
            instBuf->FlushBatch();
          } else {
            // [Path B] Fallback: Individual Rendering
            dxvk::war3::reimpl::War3InstanceBuffer::SetActive(nullptr);
            instBuf->Unlock();

            // Draw the first one (captured but suppressed)
            // Since SetActive is null, this will execute immediately
            // Note: ReplayDraw temporarily sets Active=null inside,
            // but we explicitly set it null here regardless.
            instBuf->FlushBatch();

            // Draw remaining individually
            for (size_t i = 1; i < pendingInstances.size(); i++) {
              auto *b = pendingInstances[i];
              void *sceneNodeB = nullptr;
              if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
                sceneNodeB = *reinterpret_cast<void **>(
                    reinterpret_cast<uint8_t *>(b->renderablePart) + 0x14);
              } else {
                dxvk::war3::SafeReadPtrFast(b->renderablePart, 0x14,
                                            sceneNodeB);
              }
              dispatchCommon(sceneNodeB, b->renderablePart,
                             reinterpret_cast<void *>(uintptr_t(b->layerIndex)),
                             reinterpret_cast<void *>(uintptr_t(1)),
                             reinterpret_cast<void *>(uintptr_t(1)));
              if constexpr (kDiagStatsEnabled)
                statsCommonCalls++;
            }
          }
          lastBeginSceneNode = nullptr;
        } else {
          // [Path C] Direct Draw Fallback (Portraits or Alloc Failed)
          if (contextActive && fns.execEndValue) {
            fns.execEndValue(&currentCtx);
            contextActive = false;
          }
          if (fns.execBeginValue) {
            auto *first = pendingInstances[0];
            fns.execBeginValue(first->renderablePart, pendingTag, pendingStage,
                               true, &currentCtx);
            contextActive = true;
          }

          // [FIX] 保留 Stage1 清理以避免 TeamColor 串色，但必须同步失效缓存，
          // 否则后续 layerChanged/stateChanged 仍可能判为“可复用”，导致空纹理沿用。
          device->SetTexture(1, nullptr);
          dxvk::war3::reimpl::War3InstanceBuffer::SetActive(nullptr);
          lastLayerStatePtr = nullptr;
          lastMeshData = nullptr;
          lastLayerIndex = std::numeric_limits<uint32_t>::max();

          // Render All
          for (auto *b : pendingInstances) {
            void *sceneNodeB = nullptr;
            if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
              sceneNodeB = *reinterpret_cast<void **>(
                  reinterpret_cast<uint8_t *>(b->renderablePart) + 0x14);
            } else {
              dxvk::war3::SafeReadPtrFast(b->renderablePart, 0x14,
                                          sceneNodeB);
            }
            dispatchCommon(sceneNodeB, b->renderablePart,
                           reinterpret_cast<void *>(uintptr_t(b->layerIndex)),
                           reinterpret_cast<void *>(uintptr_t(1)),
                           reinterpret_cast<void *>(uintptr_t(1)));
            if constexpr (kDiagStatsEnabled)
              statsCommonCalls++;
          }
        }
        pendingInstances.clear();

        // [Context Safety] Restore Current State
        device->SetViewport(&savedVP);
        device->SetTransform(D3DTS_VIEW, &savedView);
        device->SetTransform(D3DTS_PROJECTION, &savedProj);
      }

      if (isInstancing && mergeable) {
        pendingInstances.push_back(batch);
        continue;
      }
      }

      bool needNewContext = (sceneNode != lastBeginSceneNode) ||
                            (isSpecial != lastWasType3) || !contextActive;

      if (needNewContext) {
        if (contextActive && fns.execEndValue) {
          fns.execEndValue(&currentCtx);
          contextActive = false;
        }

        if (fns.execBeginValue) {
          ensureTagStage();
          fns.execBeginValue(renderablePart, currentTag, currentStage,
                             !isSpecial, &currentCtx);
          contextActive = true;
        }
        lastBeginSceneNode = sceneNode;
        lastWasType3 = isSpecial;
        if constexpr (kDiagStatsEnabled)
          statsContextMerges++;
      }

      MaybeLogSemanticDispatchContract(batch, sceneNode, meshData);

      if (isSpecial) {
        dispatchSpecial(
            sceneNode, renderablePart,
            reinterpret_cast<void *>(uintptr_t(batch->layerIndex)),
            reinterpret_cast<void *>(uintptr_t(stateChanged ? 1 : 0)));
        if constexpr (kDiagStatsEnabled)
          statsSpecialCalls++;
      } else {

        // layerChanged：保守复用策略
        // 说明：
        // - 过去仅比较 layerStatePtr 前 20B，可能在“不同 mesh/layer 但状态块前缀相同”时
        //   误判为可复用，导致纹理/AlphaTest 相关状态沿用上一批次，出现透明贴图发黑。
        // - 这里增加 meshData/layerIndex 一致性约束，优先保证材质正确性。
        int layerChanged = 1;
        if (stateOptEnabled && !lastWasSpecial && lastLayerStatePtr &&
            batch->layerStatePtr && meshData == lastMeshData &&
            batch->layerIndex == lastLayerIndex) {
          // 快速路径：同指针直接视为未变化；仅在指针不同时做 20B 比较。
          if (LayerStatePrefix20Equal(lastLayerStatePtr, batch->layerStatePtr))
            layerChanged = 0;
        }

        // [STATS] 收集统计数据（默认关闭，排障时开启）
        if constexpr (kDiagStatsEnabled) {
          if (layerChanged == 0) {
            statsLayerUnchanged++;
            statsCurrentRun++;
            if (statsCurrentRun > statsMaxRun)
              statsMaxRun = statsCurrentRun;
          } else {
            statsCurrentRun = 1;
          }
          if (!stateChanged)
            statsStateUnchanged++;
          statsCommonCalls++;
        }

        dispatchCommon(
            sceneNode, renderablePart,
            reinterpret_cast<void *>(uintptr_t(batch->layerIndex)),
            reinterpret_cast<void *>(uintptr_t(layerChanged)),
            reinterpret_cast<void *>(uintptr_t(stateChanged ? 1 : 0)));
      }

      // 原版 FlushSortedItems 每个元素调用 StageUpdate(0)（IDA 已确认）。
      // 传非零会强制更新所有 Stage，既偏离原版，又会放大性能回退风险。
      if (fns.stageUpdate)
        fns.stageUpdate(nullptr);

      lastWasSpecial = isSpecial;
      lastLayerStatePtr = batch->layerStatePtr;
      lastMeshData = meshData;
      lastLayerIndex = batch->layerIndex;
    }

    // [Instancing] 处理最后一段未刷新的合批
    if (instancingEnabled && !pendingInstances.empty()) {
      if (contextActive && fns.execEndValue) {
        fns.execEndValue(&currentCtx);
        contextActive = false;
      }
      FlushPendingBatches(device);
    }

    // 清理最后的上下文
    if (contextActive && fns.execEndValue) {
      fns.execEndValue(&currentCtx);
    }

    if constexpr (kDiagStatsEnabled) {
      // [STATS] 每隔一定帧数输出统计
      static uint32_t s_frameCounter = 0;
      static uint32_t s_accLayerUnchanged = 0;
      static uint32_t s_accStateUnchanged = 0;
      static uint32_t s_accCommonCalls = 0;
      static uint32_t s_accSpecialCalls = 0;
      static uint32_t s_accMaxRun = 0;

      s_accLayerUnchanged += statsLayerUnchanged;
      s_accStateUnchanged += statsStateUnchanged;
      s_accCommonCalls += statsCommonCalls;
      s_accSpecialCalls += statsSpecialCalls;
      if (statsMaxRun > s_accMaxRun)
        s_accMaxRun = statsMaxRun;
      s_frameCounter++;

      if (s_frameCounter >= 300) { // 每 300 帧输出一次
        const uint32_t totalCalls = s_accCommonCalls + s_accSpecialCalls;
        const float layerUnchangedPct =
            s_accCommonCalls > 0
                ? (100.0f * s_accLayerUnchanged / s_accCommonCalls)
                : 0.0f;
        const float stateUnchangedPct =
            s_accCommonCalls > 0
                ? (100.0f * s_accStateUnchanged / s_accCommonCalls)
                : 0.0f;
        WAR3_RENDER_LOG(
            "[BatchMergeStats] %u frames: Calls=%u (Common=%u, Special=%u), "
            "LayerUnchanged=%.1f%%, StateUnchanged=%.1f%%, MaxRun=%u\n",
            s_frameCounter, totalCalls, s_accCommonCalls, s_accSpecialCalls,
            layerUnchangedPct, stateUnchangedPct, s_accMaxRun);
        s_frameCounter = 0;
        s_accLayerUnchanged = 0;
        s_accStateUnchanged = 0;
        s_accCommonCalls = 0;
        s_accSpecialCalls = 0;
        s_accSpecialCalls = 0;
        s_accMaxRun = 0;
      }

      // [BATCH MERGE PROTOTYPE]
      batchMerger.LogStats(s_frameCounter);
    }

    // 原始实现尾部清理
    if (g.stateCleanupPendingPtr && *g.stateCleanupPendingPtr) {
      if (fns.gxCleanup74)
        fns.gxCleanup74();
      if (fns.gxCleanup78)
        fns.gxCleanup78();
      *g.stateCleanupPendingPtr = 0;
    }

    return true;
  }

  // FlushTransparent_StdSort：接管并加速透明批次提交
  // RVA: 0x138210
  static bool FlushTransparent_StdSort(const RenderQueueGlobals &g,
                                       ItemComparatorFn gameComparator,
                                       const RenderQueueFns &fns) {
    auto totalScope = MakeQueueCpuScope("FQ_Total_Trans");
    if (!g.aucTransparentCountPtr || !g.aucTransparentArrayBase ||
        !g.aucTransparentSortedPtrs)
      return false;

    const uint32_t count = *g.aucTransparentCountPtr;
    if (count == 0)
      return true;

    const uint32_t processCount = (std::min)(count, 10000u);
    void *arrayRaw = *g.aucTransparentArrayBase;
    void **sortedPtrs = g.aucTransparentSortedPtrs;
    if (!arrayRaw || !sortedPtrs)
      return false;

    // 1) 填充指针数组
    uint8_t *entryBase = reinterpret_cast<uint8_t *>(arrayRaw);
    for (uint32_t i = 0; i < processCount; i++) {
      sortedPtrs[i] = entryBase + i * kTransparentEntryStride;
    }

    // 2) 排序 - 对齐原版 sub_6F1378D0 语义：
    //   - 主键：sortKey 升序；
    //   - 次键：+0x08 浮点字段（arg0）降序；
    //   - 再次键：稳定 tie-break（type/payload/arg1/arg2/地址）避免同键抖动。
    //
    // 说明：
    // 原版 qsort 比较器在同键场景会继续比较 +0x08，若仅按 sortKey 判断“已排序”，
    // 会导致同键条目跨帧顺序不稳定，表现为隐身/半透明材质闪烁。
    auto transparentDepthKey = [](const AUCTransparentEntry *entry) -> float {
      if (!entry)
        return 0.0f;
      float depth = 0.0f;
      std::memcpy(&depth, &entry->arg0, sizeof(depth));
      return depth;
    };
    auto transparentLess = [&](const AUCTransparentEntry *a,
                               const AUCTransparentEntry *b) -> bool {
      if (a == b)
        return false;
      if (!a || !b)
        return b != nullptr; // nullptr 放末尾

      if (a->sortKey != b->sortKey)
        return a->sortKey < b->sortKey; // 主键：升序

      const float depthA = transparentDepthKey(a);
      const float depthB = transparentDepthKey(b);
      if (depthA != depthB)
        return depthA > depthB; // 次键：降序（与原版一致）

      // 稳定 tie-break：避免同键条目在不同帧随机交换顺序。
      if (a->type != b->type)
        return a->type < b->type;
      if (a->payload != b->payload)
        return a->payload < b->payload;
      if (a->arg1 != b->arg1)
        return a->arg1 < b->arg1;
      if (a->arg2 != b->arg2)
        return a->arg2 < b->arg2;
      return a < b;
    };

    // 快速路径：若当前已按“完整比较器”非降序排列，则跳过 sort。
    bool needSort = false;
    if (processCount > 1) {
      auto *prev = reinterpret_cast<AUCTransparentEntry *>(sortedPtrs[0]);
      for (uint32_t i = 1; i < processCount; i++) {
        auto *cur = reinterpret_cast<AUCTransparentEntry *>(sortedPtrs[i]);
        if (transparentLess(cur, prev)) {
          needSort = true;
          break;
        }
        prev = cur;
      }
    }
    if (needSort) {
      std::sort(sortedPtrs, sortedPtrs + processCount,
                [&](void *lhs, void *rhs) {
                  auto *a = reinterpret_cast<AUCTransparentEntry *>(lhs);
                  auto *b = reinterpret_cast<AUCTransparentEntry *>(rhs);
                  return transparentLess(a, b);
                });
    }

    // 3) 分发循环
    for (uint32_t i = 0; i < processCount; i++) {
      auto *entry = reinterpret_cast<AUCTransparentEntry *>(sortedPtrs[i]);
      if (!entry)
        continue;

      // 根据 RVA 0x138210 逻辑分发
      switch (entry->type) {
      case 0:
        if (entry->payload && fns.sub_13A0E0) {
          uintptr_t meshInfoPtr = reinterpret_cast<uintptr_t>(entry->payload);
          uint32_t ctx = *reinterpret_cast<uint32_t *>(meshInfoPtr + 20);
          fns.sub_13A0E0(ctx, entry->payload);
        }
        break;
      case 1:
        if (fns.sub_198C00)
          fns.sub_198C00(entry->payload);
        break;
      case 2:
        if (fns.sub_19DFF0)
          fns.sub_19DFF0(entry->payload);
        break;
      case 3:
        if (fns.sub_19BC20)
          fns.sub_19BC20(entry->payload);
        break;
      case 4:
        if (fns.sub_13A0B0)
          fns.sub_13A0B0(entry->payload);
        break;
      case 5: {
        using ActionFn = void(__fastcall *)(int, int);
        auto *func = reinterpret_cast<ActionFn>(entry->payload);
        if (func)
          func(entry->arg1, entry->arg2);
      } break;
      default:
        break;
      }

      // [FIX] 原版在每个透明入口后调用 StageUpdate(0)（已通过 IDA 确认）
      if (fns.stageUpdate)
        fns.stageUpdate(0);
    }
    return true;
  }
};

// ========================================================================
// RenderBatch_Submit 复现
// ========================================================================
inline bool RenderBatch_Submit_Reimpl(void *sceneNode,
                                      const RenderQueueGlobals &g,
                                      const RenderBatchFns &fns) {
  if (!sceneNode || !g.batchCapacityPtr || !g.numOfElementsPtr ||
      !g.batchArrayPtr || !g.batchGrowStepPtr) {
    return false;
  }

  struct RenderBatchDebugStats {
    uint64_t calls = 0;
    uint64_t fails = 0;
    uint64_t renderableTotal = 0;
    uint64_t renderableNull = 0;
    uint64_t renderableSkipFlag = 0;
    uint64_t renderableCullInvisible = 0;
    uint64_t renderableTransparent = 0;
    uint64_t meshInfoMissing = 0;
    uint64_t layerInfoNull = 0;
    uint64_t stateBlockNull = 0;
    uint64_t layerDataBaseNull = 0;
    uint64_t layerVisPtrNull = 0;
    uint64_t layerVisible = 0;
    uint64_t layerInvisible = 0;
    uint64_t visibilityModeBase = 0;
    uint64_t visibilityModeOffset = 0;
    uint64_t layerVisReadFail = 0;
    uint64_t layerPrecomputeUsed = 0;
    uint64_t layerProbeTotal = 0;
    uint64_t layerProbeVisible = 0;
    uint64_t layerCountTotal = 0;
    uint64_t layerCountMax = 0;
    uint64_t layerLoopTotal = 0;
    uint64_t meshFlagSet = 0;
    uint64_t meshFlagBreak = 0;
    uint64_t batchAdded = 0;
    uint64_t transparentAdded = 0;
  };
  static RenderBatchDebugStats s_stats;

  const uint32_t batchCountStart = *g.numOfElementsPtr;
  const uint32_t transparentCountStart =
      g.aucTransparentCountPtr ? *g.aucTransparentCountPtr : 0u;

  auto readPtrSafe = [](const void *base, size_t offset, void *&out) -> bool {
    if (dxvk::war3::SafeReadPtrFast(base, offset, out))
      return true;
    return dxvk::war3::SafeReadPtr(base, offset, out);
  };
  auto readU32Safe = [](const void *base, size_t offset,
                        uint32_t &out) -> bool {
    if (dxvk::war3::SafeReadU32Fast(base, offset, out))
      return true;
    return dxvk::war3::SafeReadU32(base, offset, out);
  };
  // 原版是直接解引用：这里尽量对齐，避免 SafeRead 误判导致漏渲染。
  auto readPtrRaw = [](const void *base, size_t offset) -> void * {
    return *reinterpret_cast<void *const *>(
        reinterpret_cast<const uint8_t *>(base) + offset);
  };
  auto readU32Raw = [](const void *base, size_t offset) -> uint32_t {
    return *reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(base) + offset);
  };
  auto readU8Raw = [](const void *base, size_t offset) -> uint8_t {
    return *reinterpret_cast<const uint8_t *>(
        reinterpret_cast<const uint8_t *>(base) + offset);
  };

  // SceneNode +0x0C: RenderableCount, +0x10: RenderableList
  uint32_t renderableCount = 0;
  void *renderableList = nullptr;
  if (!readU32Safe(sceneNode, 0x0C, renderableCount) ||
      !readPtrSafe(sceneNode, 0x10, renderableList)) {
    return false;
  }

  if (renderableCount == 0 || !renderableList) {
    return true;
  }

  if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
    ++s_stats.calls;
    s_stats.renderableTotal += renderableCount;
  }

  // SceneNode +0x20: CullTable, +0x30: MeshInfoTable, +0x50: VisibilityOffset
  void *cullTable = nullptr;
  void *meshInfoTable = nullptr;
  uint32_t visibilityOffset = 0;
  if (!readPtrSafe(sceneNode, 0x20, cullTable) ||
      !readPtrSafe(sceneNode, 0x30, meshInfoTable) ||
      !readU32Safe(sceneNode, 0x50, visibilityOffset)) {
    return false;
  }
  if (!cullTable || !meshInfoTable)
    return false;

  if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
    ++s_stats.visibilityModeOffset;
  }

  // 原版逻辑（严格按汇编语义）：
  // addr = *(uint32_t*)layerData + visibilityOffset
  //
  // ⚠️ 重要坑位：
  // 曾经尝试“自动判别 base/offset（把 visibilityOffset 当指针）”会导致
  // 可见性计算错乱，进而改变 batch flags&2 与排序语义，表现为：
  // - 特效层级错位（基尔加丹特效被模型盖住）
  // - TeamColor/贴图污染
  // 该逻辑不可随意改动，必须严格对齐汇编行为。
  auto readLayerVisible = [&](uint32_t layerVisRef,
                              uint8_t &outVisible) -> bool {
    if (layerVisRef == 0) {
      outVisible = 1;
      return true;
    }
    const uintptr_t addrVal =
        reinterpret_cast<uintptr_t>(layerVisRef) + visibilityOffset;
    if (addrVal < 0x1000u) {
      outVisible = 1;
      return true;
    }
    auto *addr = reinterpret_cast<uint8_t *>(addrVal);
    if (!dxvk::war3::IsReadableRangeFast(addr, 1)) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerVisReadFail;
      }
      outVisible = 1; // 读取失败时保守可见，避免漏渲染
      return true;
    }
    outVisible = *addr;
    return true;
  };

  auto *list = reinterpret_cast<uint8_t *>(renderableList);
  for (uint32_t i = 0; i < renderableCount; ++i) {
    void *renderablePart = readPtrRaw(list, 0);
    if (!renderablePart) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.renderableNull;
      }
      list += sizeof(void *);
      continue;
    }
    list += sizeof(void *);

    uint32_t skipFlag = readU32Raw(renderablePart, 0x10);
    if (skipFlag != 0) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.renderableSkipFlag;
      }
      continue;
    }

    void *meshData = readPtrRaw(renderablePart, 0x0C);
    if (!meshData) {
      continue;
    }

    uint32_t cullIndex = readU32Raw(meshData, 0x11C);

    uint8_t cullVisible = 0;
    if (!dxvk::war3::internal::kNativeRenderBypassCull) {
      cullVisible = readU8Raw(cullTable, 16u * cullIndex + 3);
      if (cullVisible == 0) {
        if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
          ++s_stats.renderableCullInvisible;
        }
        continue;
      }
    }

    // RenderablePart +0x14: SceneNodeBackPtr
    *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(renderablePart) +
                               0x14) = sceneNode;

    if (!fns.canEnqueueToMainQueue || !fns.addTransparent ||
        !fns.transformPoint) {
      continue; // 函数指针缺失，跳过
    }

    if (!fns.canEnqueueToMainQueue(sceneNode, renderablePart)) {
      // 透明对象走 AUCTransparent
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.renderableTransparent;
      }
      float worldPos[3] = {0.0f, 0.0f, 0.0f};
      auto *meshPos = reinterpret_cast<float *>(
          reinterpret_cast<uint8_t *>(meshData) + 0x10C);
      auto *worldMatrix = reinterpret_cast<float *>(
          reinterpret_cast<uint8_t *>(sceneNode) + 0x64);
      // [FIX] 必须先将局部坐标转换为世界坐标！原版调用流程：
      // TransformPoint3x4(outBuffer, localPos, worldMatrix);
      // AUCTransparent_AddEntry(entry, type=0, worldPos, sortKey);
      fns.transformPoint(worldPos, meshPos, worldMatrix);

      uint32_t transparentKey = readU32Raw(meshData, 0x120);

      // 原版使用 Type 0，不是 Type 4
      int type = 0;
      fns.addTransparent(renderablePart, type, worldPos, transparentKey);
      continue;
    }

    // RenderablePart -> MeshInfo
    uint32_t meshIndex = readU32Raw(meshData, 0x108);
    void *meshInfo = readPtrRaw(meshInfoTable, meshIndex * sizeof(void *));
    if (!meshInfo) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.meshInfoMissing;
      }
      // [FIX] meshInfo 为空时跳过该 renderable，而非回滚整个 SceneNode
      continue;
    }

    uint32_t layerCount = readU32Raw(meshInfo, 0x0C);
    if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
      s_stats.layerCountTotal += layerCount;
      if (layerCount > s_stats.layerCountMax) {
        s_stats.layerCountMax = layerCount;
      }
    }
    void *stateBlockBase = readPtrRaw(meshInfo, 0x10);
    void *layerInfo = readPtrRaw(meshInfo, 0x38);
    if (layerCount == 0) {
      continue;
    }

    if (!layerInfo) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerInfoNull;
      }
      continue;
    }

    void *layerDataBase = readPtrRaw(layerInfo, 0x10);
    if (!stateBlockBase) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.stateBlockNull;
      }
      continue;
    }
    if (!layerDataBase) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerDataBaseNull;
      }
      continue;
    }

    uint32_t visibleLayerCounter = 0;
    uint8_t *layerData = reinterpret_cast<uint8_t *>(layerDataBase) + 0x1C;
    uint8_t *statePtr = reinterpret_cast<uint8_t *>(stateBlockBase) + 4;

    constexpr uint32_t kMaxPrecomputeLayers =
        dxvk::war3::internal::kNativeRenderBatchPrecomputeMaxLayers;
    uint8_t precomputeVisible[kMaxPrecomputeLayers] = {};
    uint8_t precomputeHasVisibleAfter[kMaxPrecomputeLayers] = {};
    const bool usePrecompute =
        dxvk::war3::internal::kNativeRenderBatchPrecomputeVisibilityEnabled &&
        !dxvk::war3::internal::kNativeRenderBypassLayerVisibility &&
        layerCount > 1 && layerCount <= kMaxPrecomputeLayers;

    if (usePrecompute) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerPrecomputeUsed;
      }

      uint8_t *scanData = reinterpret_cast<uint8_t *>(layerDataBase) + 0x1C;
      for (uint32_t scanIndex = 0; scanIndex < layerCount; ++scanIndex) {
        uint8_t scanVisible = 0;
        const uint32_t layerVisRef = readU32Raw(scanData, 0);
        if (!readLayerVisible(layerVisRef, scanVisible)) {
          precomputeVisible[scanIndex] = 0;
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerVisPtrNull;
          }
        } else if (scanVisible == 0) {
          precomputeVisible[scanIndex] = 0;
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerInvisible;
          }
        } else {
          precomputeVisible[scanIndex] = 1;
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerVisible;
          }
        }
        scanData += RenderQueue::kLayerDataStride;
      }

      bool seenVisible = false;
      for (int32_t scanIndex = static_cast<int32_t>(layerCount) - 1;
           scanIndex >= 0; --scanIndex) {
        precomputeHasVisibleAfter[scanIndex] = seenVisible ? 1u : 0u;
        if (precomputeVisible[scanIndex]) {
          seenVisible = true;
        }
      }
    }

    for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerLoopTotal;
      }

      if (!dxvk::war3::internal::kNativeRenderBypassLayerVisibility) {
        if (usePrecompute) {
          if (precomputeVisible[layerIndex] == 0) {
            layerData += RenderQueue::kLayerDataStride;
            statePtr += RenderQueue::kLayerStateStride;
            continue;
          }
        } else {
          uint8_t layerVisible = 0;
          const uint32_t layerVisRef = readU32Raw(layerData, 0);
          if (!readLayerVisible(layerVisRef, layerVisible)) {
            if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
              ++s_stats.layerVisPtrNull;
            }
            layerData += RenderQueue::kLayerDataStride;
            statePtr += RenderQueue::kLayerStateStride;
            continue;
          }
          if (layerVisible == 0) {
            if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
              ++s_stats.layerInvisible;
            }
            layerData += RenderQueue::kLayerDataStride;
            statePtr += RenderQueue::kLayerStateStride;
            continue;
          }
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerVisible;
          }
        }
      } else {
        if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
          ++s_stats.layerVisible;
        }
      }

      // 确保 RenderQueue 容量
      uint32_t num = *g.numOfElementsPtr;
      uint32_t next = num + 1;
      const uint32_t capacity = *g.batchCapacityPtr;
      if (next > capacity) {
        uint32_t growStep = *g.batchGrowStepPtr;
        if (growStep == 0 && fns.computeBatchGrowStep) {
          growStep = static_cast<uint32_t>(
              fns.computeBatchGrowStep(g.batchCapacityPtr, next));
        }
        if (growStep != 0 && (next % growStep) != 0) {
          next += growStep - (next % growStep);
        }
        if (!fns.reserveBatchArray) {
          break; // 无法扩容，退出层循环
        }
        fns.reserveBatchArray(g.batchCapacityPtr, next);
        num = *g.numOfElementsPtr;
      }

      auto *batchArray = reinterpret_cast<uint8_t *>(*g.batchArrayPtr);
      if (!batchArray) {
        break; // 无法获取数组，退出层循环
      }

      auto *batch = reinterpret_cast<RenderBatchElement *>(
          batchArray + (num * RenderQueue::kBatchElementStride));

      batch->renderablePart = renderablePart;
      batch->flags = 0;
      batch->layerIndex = layerIndex;
      batch->subIndex = visibleLayerCounter;
      // statePtr 已在第 826 行从 meshInfo+0x10 正确计算
      // (第 811 行已验证 stateBlockBase 非空)
      batch->layerStatePtr = statePtr;

      uint32_t meshFlag = readU32Raw(meshData, 0x104);
      if (meshFlag != 0 && !dxvk::war3::internal::kNativeRenderIgnoreMeshFlag) {
        batch->flags |= 1u;
        if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
          ++s_stats.meshFlagSet;
        }
      }

      if (visibleLayerCounter > 0) {
        batch->flags |= 2u;
      } else {
        if (usePrecompute) {
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerProbeTotal;
          }
          if (precomputeHasVisibleAfter[layerIndex] != 0) {
            batch->flags |= 2u;
            if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
              ++s_stats.layerProbeVisible;
            }
          }
        } else {
          // 首层：检查是否存在后续可见层
          uint32_t probeIndex = layerIndex + 1;
          uint8_t *probeData = layerData + RenderQueue::kLayerDataStride;
          for (; probeIndex < layerCount; ++probeIndex) {
            if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
              ++s_stats.layerProbeTotal;
            }
            const uint32_t probeVisRef = readU32Raw(probeData, 0);
            uint8_t probeVisible = 0;
            if (readLayerVisible(probeVisRef, probeVisible) &&
                probeVisible != 0) {
              batch->flags |= 2u;
              if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
                ++s_stats.layerProbeVisible;
              }
              break;
            }
            probeData += RenderQueue::kLayerDataStride;
          }
        }
      }

      *g.numOfElementsPtr = num + 1;
      num = *g.numOfElementsPtr;
      ++visibleLayerCounter;

      // 原版逻辑：只要 meshFlag 触发，立即跳出层循环
      if ((batch->flags & 1u) != 0 &&
          !dxvk::war3::internal::kNativeRenderIgnoreMeshFlag) {
        if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
          ++s_stats.meshFlagBreak;
        }
        break;
      }

      layerData += RenderQueue::kLayerDataStride;
      statePtr += RenderQueue::kLayerStateStride;
    }
  }

  if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
    s_stats.batchAdded += (*g.numOfElementsPtr - batchCountStart);
    if (g.aucTransparentCountPtr) {
      s_stats.transparentAdded +=
          (*g.aucTransparentCountPtr - transparentCountStart);
    }
    if (s_stats.calls != 0 && (s_stats.calls % 300u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3Hook: RenderBatch stats calls=%llu fails=%llu "
          "renderable=%llu null=%llu skip=%llu cull=%llu transp=%llu "
          "meshInfoMiss=%llu layerInfoNull=%llu stateBlockNull=%llu "
          "layerDataNull=%llu visModeBase=%llu visModeOffset=%llu "
          "visReadFail=%llu precompute=%llu layerVisPtrNull=%llu "
          "layerVis=%llu layerHide=%llu layerLoop=%llu layerCountMax=%llu "
          "probe=%llu probeHit=%llu meshFlag=%llu meshBreak=%llu "
          "batchAdd=%llu transpAdd=%llu\n",
          static_cast<unsigned long long>(s_stats.calls),
          static_cast<unsigned long long>(s_stats.fails),
          static_cast<unsigned long long>(s_stats.renderableTotal),
          static_cast<unsigned long long>(s_stats.renderableNull),
          static_cast<unsigned long long>(s_stats.renderableSkipFlag),
          static_cast<unsigned long long>(s_stats.renderableCullInvisible),
          static_cast<unsigned long long>(s_stats.renderableTransparent),
          static_cast<unsigned long long>(s_stats.meshInfoMissing),
          static_cast<unsigned long long>(s_stats.layerInfoNull),
          static_cast<unsigned long long>(s_stats.stateBlockNull),
          static_cast<unsigned long long>(s_stats.layerDataBaseNull),
          static_cast<unsigned long long>(s_stats.visibilityModeBase),
          static_cast<unsigned long long>(s_stats.visibilityModeOffset),
          static_cast<unsigned long long>(s_stats.layerVisReadFail),
          static_cast<unsigned long long>(s_stats.layerPrecomputeUsed),
          static_cast<unsigned long long>(s_stats.layerVisPtrNull),
          static_cast<unsigned long long>(s_stats.layerVisible),
          static_cast<unsigned long long>(s_stats.layerInvisible),
          static_cast<unsigned long long>(s_stats.layerLoopTotal),
          static_cast<unsigned long long>(s_stats.layerCountMax),
          static_cast<unsigned long long>(s_stats.layerProbeTotal),
          static_cast<unsigned long long>(s_stats.layerProbeVisible),
          static_cast<unsigned long long>(s_stats.meshFlagSet),
          static_cast<unsigned long long>(s_stats.meshFlagBreak),
          static_cast<unsigned long long>(s_stats.batchAdded),
          static_cast<unsigned long long>(s_stats.transparentAdded));
      s_stats = {};
    }
  }

  return true;
}

// ============================================================================
// WorldObjects_RenderGroup 复现
// RVA: 0x368E30
// ============================================================================
// 原始逻辑:
// 1. 根据 groupIdx 选择列表 (this[91+groupIdx])
// 2. List_GetData / List_GetCount 取数据与计数
// 3. 遍历每个 WorldObjectListEntry (24 bytes stride)
// 4. 调用 WorldObjectEntry_Render
using ListGetDataFn = void *(__thiscall *)(void *list);
using ListGetCountFn = uint32_t(__thiscall *)(void *list);
using WorldObjectEntryRenderFn = int(__thiscall *)(void *entry);

inline int WorldObjects_RenderGroup_Reimpl(
    void *worldPtr, int groupIdx, ListGetDataFn listGetData,
    ListGetCountFn listGetCount, WorldObjectEntryRenderFn entryRender) {
  if (!worldPtr || groupIdx < 0 || groupIdx > 2 || !listGetData ||
      !listGetCount || !entryRender) {
    return 0;
  }

  // 获取正确的列表指针
  auto *worldDwords = static_cast<uintptr_t *>(worldPtr);
  void *listPtr = reinterpret_cast<void *>(worldDwords[91 + groupIdx]);
  if (!listPtr)
    return 0;

  void *listData = listGetData(listPtr);
  const uint32_t listCount = listGetCount(listPtr);
  if (!listData || listCount == 0)
    return 0;

  int lastResult = 0;
  uint8_t *entryPtr = static_cast<uint8_t *>(listData);
  for (uint32_t i = 0; i < listCount; ++i) {
    void *objectEntry = *reinterpret_cast<void **>(entryPtr);
    if (objectEntry)
      lastResult = entryRender(objectEntry);
    entryPtr += 24;
  }

  return lastResult;
}

} // namespace reimpl
} // namespace war3
} // namespace dxvk
