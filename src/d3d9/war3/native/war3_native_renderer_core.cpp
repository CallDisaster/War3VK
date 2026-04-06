// war3_native_renderer_core.cpp
// 核心渲染函数实现
// 基于IDA逆向分析

#include "war3_native_renderer.h"

#include <cstdlib>
#include <cstring>

namespace war3 {
namespace native {

// ============================================================================
// RenderQueue_AddBatch 实现
// ============================================================================

/**
 * @brief RenderQueue_AddBatch 的 native 参考骨架
 *
 * 原版 `0x6F139190` 不是“直接跳转到 Submit 就结束”的薄包装。
 * 它的真实顺序是：
 * 1. `RenderBatch_Submit(sceneNode)`
 * 2. 若 `sceneNode->flags & 0x10`：
 *    - 追加四类透明列表（0/2/3/4）
 *    - 按 child bucket 递归处理可见子节点
 *
 * 本实现只落地 ASM 已确认的骨架，透明子链细节仍由外部桩函数承载。
 */
extern "C" void WAR3_NATIVE_CB RenderQueue_AddBatch(int sceneNode,
                                                    int reserved) {
  auto* node = reinterpret_cast<SceneNode*>(sceneNode);
  if (!node)
    return;

  RenderBatch_Submit(node);

  if ((node->flags & 0x10) == 0)
    return;

  void* world = node->world;
  SceneNode_AddTransparentList0(node, world);
  SceneNode_AddTransparentList2(node, world);
  SceneNode_AddTransparentList3(node, world);
  SceneNode_AddTransparentList4(node, world);

  if (!node->childCount || !node->childPtrArray)
    return;

  auto* buckets = node->childPtrArray;
  auto* visibilityCache = reinterpret_cast<uint8_t*>(node->childVisFlags);

  for (uint32_t childIndex = 0; childIndex < node->childCount; childIndex++) {
    bool visible = true;

    if (node->childVisibilityContext && visibilityCache) {
      uint8_t cached = visibilityCache[childIndex];
      if (cached & 0x1) {
        visible = (cached & 0x2) != 0;
      } else {
        visible = Visibility_Check(node->childVisibilityContext, childIndex) != 0;
        visibilityCache[childIndex] = static_cast<uint8_t>((visible ? 0x2 : 0x0) | 0x1);
      }
    }

    if (!visible)
      continue;

    for (SceneNodeChildLink* link = buckets[childIndex].first_link;
         link != nullptr;
         link = link->next_link) {
      if (link->child_scene)
        RenderQueue_AddBatch(reinterpret_cast<int>(link->child_scene), 0);
    }
  }
}

// ============================================================================
// AUCTransparent_AddEntry 实现
// ============================================================================

extern "C" void AUCTransparent_AddEntry(void *part, uint32_t type,
                                        const float *worldPos,
                                        uint32_t transparentKey) {
  if (!part || !worldPos) {
    return;
  }

  // 确保容量
  if (global::g_AUCTransparent_Count >= global::g_AUCTransparent_Capacity) {
    // TODO: 实现扩容逻辑
    return;
  }

  // 获取数组槽位
  AUCTransparentEntry *entry =
      (AUCTransparentEntry *)((uint8_t *)global::g_AUCTransparent_Array +
                              global::g_AUCTransparent_Count * 24);

  if (!entry) {
    return;
  }

  // 计算到相机的距离
  float dx = worldPos[0] - global::g_RenderCamera_PosXY[0];
  float dy = worldPos[1] - global::g_RenderCamera_PosXY[1];
  float dz = worldPos[2] - global::g_RenderCamera_PosZ;
  float distSq = dx * dx + dy * dy + dz * dz;

  // 填充条目
  entry->type = type;
  entry->sortKey = transparentKey;
  entry->distSq = distSq;
  entry->payload = part;
  entry->arg1 = 0;
  entry->arg2 = 0;

  global::g_AUCTransparent_Count++;
}

// ============================================================================
// RenderBatch_CanEnqueueToMainQueue 实现
// ============================================================================

extern "C" bool RenderBatch_CanEnqueueToMainQueue(SceneNode *sceneNode,
                                                  void *part) {
  if (!sceneNode || !part) {
    return true; // 默认不透明
  }

  // 获取 MeshData
  void *meshData = *(void **)((uint8_t *)part + 0x0C);
  if (!meshData) {
    return true;
  }

  // 获取 meshIndex
  uint32_t meshIndex = *(uint32_t *)((uint8_t *)meshData + 0x108);

  // 获取 meshInfoTable (sceneNode+0x1C)
  void **meshInfoTable = sceneNode->meshInfoTable;
  if (!meshInfoTable) {
    return true;
  }

  // 获取 meshInfo
  void *meshInfo = meshInfoTable[meshIndex];
  if (!meshInfo) {
    return true;
  }

  // 获取 layerCount (meshInfo+0x0C)
  uint32_t layerCount = *(uint32_t *)((uint8_t *)meshInfo + 0x0C);
  if (layerCount == 0) {
    return true;
  }

  // 获取 stateBlockBase (meshInfo+0x10)
  void *stateBlockBase = *(void **)((uint8_t *)meshInfo + 0x10);
  if (!stateBlockBase) {
    return true;
  }

  // 获取 layerInfo (meshInfo+0x38)
  void *layerInfo = *(void **)((uint8_t *)meshInfo + 0x38);
  if (!layerInfo) {
    return true;
  }

  // 获取 layerDataBase (layerInfo+0x10)
  void *layerDataBase = *(void **)((uint8_t *)layerInfo + 0x10);
  if (!layerDataBase) {
    return true;
  }

  // 移除 visibilityOffset 获取

  // 遍历所有层
  uint8_t *layerDataPtr = (uint8_t *)layerDataBase + 0x1C;
  uint8_t *statePtr = (uint8_t *)stateBlockBase + 4;

  for (uint32_t i = 0; i < layerCount; i++) {
    // 获取 layerVisibilityOffset
    uint32_t layerVisOffset = *(uint32_t *)layerDataPtr;
    if (layerVisOffset != (uint32_t)-1) {
      // 检查可见性表 (sceneNode+0x54)
      uint8_t *visibilityTable = (uint8_t *)sceneNode->visibilityTable;
      if (visibilityTable && visibilityTable[layerVisOffset] != 0) {
        // 获取 blendMode (statePtr+0x18 = 24)
        uint32_t blendMode = *(uint32_t *)(statePtr + 24);
        // blendMode < 2 表示不透明
        return blendMode < 2;
      }
    }

    // 步进到下一层
    layerDataPtr += 44; // LayerData stride
    statePtr += 36;     // LayerState stride
  }

  // 没有可见层，默认不透明
  return true;
}

// ============================================================================
// RenderBatch_Submit 实现
// ============================================================================

extern "C" void RenderBatch_Submit(SceneNode *sceneNode) {
  if (!sceneNode) {
    return;
  }

  uint32_t renderableCount = sceneNode->renderableCount;
  if (renderableCount == 0) {
    return;
  }

  void **renderableList = sceneNode->renderableList;
  if (!renderableList) {
    return;
  }

  // 遍历所有可渲染对象
  for (uint32_t i = 0; i < renderableCount; i++) {
    void *part = renderableList[i];
    if (!part) {
      continue;
    }

    // 检查 skipFlag (RenderablePart+0x10)
    uint32_t skipFlag = *(uint32_t *)((uint8_t *)part + 0x10);
    if (skipFlag != 0) {
      continue;
    }

    // 写回 sceneNode 指针 (RenderablePart+0x14)
    *(void **)((uint8_t *)part + 0x14) = sceneNode;

    // 检查透明/不透明
    if (RenderBatch_CanEnqueueToMainQueue(sceneNode, part)) {
      // 不透明对象 -> 添加到主队列 (0x6F13768F)
      MeshData *meshData = (MeshData *)((uint8_t *)part + 0x0C);
      void **meshInfoTable = (void **)sceneNode->meshInfoTable;
      void *meshInfo = meshInfoTable[meshData->meshIndex];
      uint32_t layerCount = *(uint32_t *)((uint8_t *)meshInfo + 0x0C);
      void *stateBlockBase = *(void **)((uint8_t *)meshInfo + 0x10);
      void *layerInfo = *(void **)((uint8_t *)meshInfo + 0x38);
      void *layerDataBase = *(void **)((uint8_t *)layerInfo + 0x10);

      uint8_t *layerDataPtr = (uint8_t *)layerDataBase + 0x1C;
      uint8_t *statePtr = (uint8_t *)stateBlockBase + 4;
      uint8_t *visibilityTable = (uint8_t *)sceneNode->visibilityTable;

      for (uint32_t layerIdx = 0; layerIdx < layerCount; layerIdx++) {
        uint32_t visOffset = *(uint32_t *)layerDataPtr;
        if (visOffset == (uint32_t)-1 ||
            (visibilityTable && visibilityTable[visOffset])) {
          // 提交到全局批次数组
          uint32_t num = global::g_RenderQueue_NumOfElements;
          if (num < global::g_RenderQueue_BatchCapacity) {
            RenderBatchElement *entry =
                (RenderBatchElement *)((uint8_t *)
                                           global::g_RenderQueue_BatchArray +
                                       num * 20);
            entry->batchEntry = part;
            entry->layerIndex = layerIdx;
            entry->layerCounter = layerIdx; // Simple implementation
            entry->layerStatePtr = statePtr;
            entry->flags = (meshData->meshFlag != 0) ? 1 : 0;
            if (layerIdx < layerCount - 1)
              entry->flags |= 2; // hasMoreLayers

            global::g_RenderQueue_NumOfElements++;
          }
        }
        layerDataPtr += 44;
        statePtr += 36;
      }
    } else {
      // 透明对象 -> 添加到透明队列
      // ✅ 修正：boundingPos和transparentKey在MeshData中，不在part中
      float worldPos[3];
      void *meshData = *(void **)((uint8_t *)part + 0x0C);
      float *boundingPos = (float *)((uint8_t *)meshData + 0x10C);
      float *worldMatrix = (float *)sceneNode->worldMatrix;
      TransformPoint3x4(worldPos, boundingPos, worldMatrix);

      uint32_t transparentKey = *(uint32_t *)((uint8_t *)meshData + 0x120);
      AUCTransparent_AddEntry(part, 0, worldPos, transparentKey);
    }
  }
}

// ============================================================================
// RenderQueue_ItemComparator 实现
// ============================================================================

extern "C" int RenderQueue_ItemComparator(const void *a, const void *b) {
  return RenderQueue_ItemLess(*(RenderBatchElement **)a,
                              *(RenderBatchElement **)b) != 0
             ? -1
             : 1;
}

// ============================================================================
// RenderQueue_ItemLess 实现
// ============================================================================

extern "C" bool RenderQueue_ItemLess(const RenderBatchElement *a,
                                     const RenderBatchElement *b) {
  bool aIsSpecial = ((a->flags & 3) == 3);
  bool bIsSpecial = ((b->flags & 3) == 3);

  // 1. Special 优先
  if (aIsSpecial != bIsSpecial) {
    return aIsSpecial;
  }

  // 2. hasMoreLayers 分组
  if ((a->flags & 2) && (b->flags & 2)) {
    void *meshDataA = *(void **)((uint8_t *)a->batchEntry + 0x0C);
    void *meshDataB = *(void **)((uint8_t *)b->batchEntry + 0x0C);
    if (meshDataA != meshDataB)
      return meshDataA < meshDataB;

    if (a->layerCounter != b->layerCounter)
      return a->layerCounter < b->layerCounter;

    return memcmp(a->layerStatePtr, b->layerStatePtr, 20) < 0;
  }

  // 3. 仅一个有 hasMoreLayers
  if ((a->flags & 2) && !(b->flags & 2))
    return true;
  if (!(a->flags & 2) && (b->flags & 2))
    return false;

  // 4. 都没有 hasMoreLayers
  return memcmp(a->layerStatePtr, b->layerStatePtr, 20) < 0 ||
         (memcmp(a->layerStatePtr, b->layerStatePtr, 20) == 0 &&
          *(void **)((uint8_t *)a->batchEntry + 0x0C) <
              *(void **)((uint8_t *)b->batchEntry + 0x0C));
}

// ============================================================================
// RenderQueue_StageUpdate 实现
// ============================================================================

extern "C" void RenderQueue_StageUpdate(void *this_ptr, int param_edi,
                                        int param_esi) {
  // 原版 0x6F13A9B0 的真实语义：
  // 1. 首次调用会通过 sub_6F0E2DA0 初始化 stage 数量；
  // 2. 入参为 nullptr / 0 时，仅刷新尚未初始化的 stage slot；
  // 3. 入参非空时，强制刷新全部 stage slot。
  //
  // 这里仍保留桩实现，因为 GxDevice_UpdateStage / stage 描述块尚未完全接进 native。
  (void)this_ptr;
  (void)param_edi;
  (void)param_esi;
}

// ============================================================================
// RenderQueue_FlushSortedItems 实现
// ============================================================================

extern "C" unsigned int RenderQueue_FlushSortedItems() {

  uint32_t num = global::g_RenderQueue_NumOfElements;
  if (num == 0)
    return 0;

  uint32_t count = (num < 10000) ? num : 10000;
  global::g_RenderQueue_SortedCount = count;

  // 复制指针
  void *batchArray = global::g_RenderQueue_BatchArray;
  for (uint32_t i = 0; i < count; i++) {
    global::g_RenderQueue_SortedPtrs[i] =
        (void *)((uint8_t *)batchArray + i * 20);
  }

  // 排序
  qsort(global::g_RenderQueue_SortedPtrs, count, 4, RenderQueue_ItemComparator);

  // 初始状态应用
  RenderBatchElement *first =
      (RenderBatchElement *)global::g_RenderQueue_SortedPtrs[0];
  GxDevice_ApplyStateBlock(first->layerStatePtr);

  void *lastMeshData = nullptr;
  uint32_t lastLayerIndex = 0;
  void *lastLayerStatePtr = first->layerStatePtr;
  bool lastWasSpecial = false;

  // 循环调度
  for (uint32_t i = 0; i < count; i++) {
    RenderBatchElement *batch =
        (RenderBatchElement *)global::g_RenderQueue_SortedPtrs[i];
    void *meshData = *(void **)((uint8_t *)batch->batchEntry + 0x0C);

    // stateChanged 判断
    bool stateChanged = true;
    if (global::g_RenderQueue_StateOptEnabled) {
      if (meshData == lastMeshData && batch->layerIndex == lastLayerIndex &&
          *(uint32_t *)((uint8_t *)meshData + 0x104) == 0) {
        stateChanged = false;
      }
    }

    if ((batch->flags & 3) == 3) {
      // Special 分支
      RenderQueue_Dispatch_Special(batch->batchEntry, stateChanged);
    } else {
      // Common 分支
      int layerChanged = 1;
      if (global::g_RenderQueue_StateOptEnabled && !lastWasSpecial) {
        if (memcmp(lastLayerStatePtr, batch->layerStatePtr, 20) == 0) {
          layerChanged = 0;
        }
      }
      RenderQueue_Dispatch_Common(batch->batchEntry, layerChanged,
                                  stateChanged);
    }

    RenderQueue_StageUpdate(nullptr, 0, 0);

    lastWasSpecial = ((batch->flags & 3) == 3);
    lastLayerStatePtr = batch->layerStatePtr;
    lastMeshData = meshData;
    lastLayerIndex = batch->layerIndex;
  }

  // 尾部清理
  if (global::g_RenderQueue_StateCleanupPending) {
    GxDevice_StateCleanup74();
    GxDevice_StateCleanup78();
    global::g_RenderQueue_StateCleanupPending = 0;
  }

  return count;
}

// ============================================================================
// RenderQueue_Dispatch_Common 实现（桩函数）
// ============================================================================

extern "C" int RenderQueue_Dispatch_Common(void *part, int layerChanged,
                                           int stateChanged) {
  // 已确认语义：
  // 1. 先做 RenderQueue_UpdateItemWorldMatrix
  // 2. 通过 meshIndex -> dispatch block 取材质/层描述
  // 3. BindDispatchBlock + 若需要则 ApplyStateBlock
  // 4. 若当前 mesh 非 special，则走一次 RenderSceneFlush_0E39E0
  //
  // 这里保留返回 0 的桩，仅用于提醒后续 native 接管必须先补齐上述 4 步。
  (void)part;
  (void)layerChanged;
  (void)stateChanged;
  return 0;
}

// ============================================================================
// RenderQueue_Dispatch_Special 实现（桩函数）
// ============================================================================

extern "C" int RenderQueue_Dispatch_Special(void *part, int stateChanged) {
  // 已确认语义：
  // 1. 先做 RenderQueue_UpdateItemWorldMatrix
  // 2. 若 special batch 状态一致，则走 DispatchSpecialBatch
  // 3. 否则清理状态并回退到 FallbackMultiPass
  //
  // 这里仍是语义桩，避免把错误的伪实现当成事实。
  (void)part;
  (void)stateChanged;
  return 0;
}

// ============================================================================
// GxDevice 函数实现（桩函数）
// ============================================================================

extern "C" void GxDevice_ApplyStateBlock(void *stateBlock) {
  // TODO: 实现状态块应用
}

extern "C" void GxDevice_StateCleanup74() {
  // TODO: 实现状态清理
}

extern "C" void GxDevice_StateCleanup78() {
  // TODO: 实现状态清理
}

extern "C" void GxDevice_RenderSceneFlush() {
  // TODO: 实现场景刷新
}

extern "C" void GxDevice_SetVertexBuffer(int offset) {
  // TODO: 实现顶点缓冲设置
}

extern "C" void GxDevice_DrawPrimitive() {
  // TODO: 实现绘制图元
}

// ============================================================================
// 辅助函数实现
// ============================================================================

extern "C" void TransformPoint3x4(float *result, const float *point,
                                  const float *matrix) {
  if (!result || !point || !matrix) {
    return;
  }

  // 3x4矩阵变换：result = matrix * point
  result[0] = matrix[0] * point[0] + matrix[3] * point[1] +
              matrix[6] * point[2] + matrix[9];
  result[1] = matrix[1] * point[0] + matrix[4] * point[1] +
              matrix[7] * point[2] + matrix[10];
  result[2] = matrix[2] * point[0] + matrix[5] * point[1] +
              matrix[8] * point[2] + matrix[11];
}

} // namespace native
} // namespace war3
