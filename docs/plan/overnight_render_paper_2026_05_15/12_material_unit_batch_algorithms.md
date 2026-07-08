# 第 12 章 — CGxMat 材质系统 + CUnit 状态机 + RenderBatch 完整算法

> 本章补全三个核心子系统的算法级细节：
> 1. CGxMat 材质系统的 texture stage state 映射
> 2. CUnit 状态机的完整事件分发表
> 3. RenderBatch_Submit / FlushSortedItems 的完整实现

## 1. CGxMat 材质系统

### 1.1 材质状态应用流程

```
RenderQueue_ApplyDrawStateAndDraw (0x6F138F70)
  └─ RenderQueue_ApplyDrawStateAndSamplerPair (0x6F138EE0)
      ├─ GxDevice_SetPrimitiveType(primitiveType)
      ├─ 查 sampler table: 44 字节/entry，按 texture stage index
      │   ├─ texture0: sampler0 + texture0 handle
      │   └─ texture1: sampler1 + texture1 handle
      └─ GxDevice_PreparePrimitive(12, vb, 12, 0, 0, ib, hasIndices, sampler0, 8, sampler1, 8)
```

### 1.2 Layer Color/Alpha 寄存器

`RenderQueue_ApplyLayerColorAlphaRegisters (0x6F13ABA0)`:

```c
void ApplyLayerColorAlphaRegisters(int layerFlags, int colorAlpha) {
    if (layerFlags & 1) {
        // 混合模式 1：color 写 stage2，alpha<<24 写 stage1
        GxDevice_SetColorRegister(2, colorAlpha);
        GxDevice_SetColorRegister(1, GetAlpha(colorAlpha) << 24);
    } else {
        // 混合模式 0：color 写 stage1，stage2 清零
        GxDevice_SetColorRegister(1, colorAlpha);
        GxDevice_SetColorRegister(2, 0);
    }
}
```

### 1.3 Layer Tint 和 Alpha 组合

`RenderQueue_ComposeLayerTintAndAlpha (0x6F137BD0)`:

```c
void ComposeLayerTintAndAlpha(int rq, int meshData, int layerIdx, int outColor) {
    int layerInfo = *(DWORD*)(rq + 32) + 16 * *(DWORD*)(meshData + 284);
    int tintColor = *(DWORD*)layerInfo;
    int layerAlpha = *(BYTE*)(layerInfo + 4);

    // 按通道做 8-bit 乘法 + 255 右移 8
    BYTE r = (BYTE)((WORD)(GetR(tintColor) * layerAlpha + 255) >> 8);
    BYTE g = (BYTE)((WORD)(GetG(tintColor) * layerAlpha + 255) >> 8);
    BYTE b = (BYTE)((WORD)(GetB(tintColor) * layerAlpha + 255) >> 8);

    *(DWORD*)outColor = (tintColor & 0xFF000000) | (b << 16) | (g << 8) | r;

    // per-layer alpha 从 rq+80 的 alpha 表读取
    BYTE perLayerAlpha = *(BYTE*)(*(DWORD*)(rq + 80) + layerIdx);
    *(BYTE*)(outColor + 3) = *(BYTE*)(outColor + 3) * perLayerAlpha / 255;
}
```

### 1.4 Texture Stage Mode 切换

`RenderQueue_ApplyTextureStageMode (0x6F13AC00)`:

```c
void ApplyTextureStageMode(int rq, int meshData, int dispatchBlock) {
    int mode = GetTextureStageMode(meshData, dispatchBlock);
    switch (mode) {
    case 1:
        // 单纹理模式
        if (!*(DWORD*)(meshData + 32) || *(DWORD*)(dispatchBlock + 24) == -1) {
            GxDevice_SetTextureStageMode(0);
        } else {
            GxDevice_SetTextureStageMode(1);
        }
        break;
    case 2:
        // 双纹理模式
        GxDevice_SetTextureStageMode(0);
        GxDevice_SetTextureStageMode(1);
        break;
    }
}
```

### 1.5 Special Batch 状态一致性检查

`RenderQueue_IsSpecialBatchStateConsistent (0x6F13AC70)`:

```c
bool IsSpecialBatchStateConsistent(int rq, int dispatchBlock) {
    if (*(DWORD*)(dispatchBlock + 12) <= 1) return true;  // 单层直接一致

    int layerCount = *(DWORD*)(dispatchBlock + 56);
    int* layers = *(int**)(layerCount + 16);
    int alphaTable = *(DWORD*)(rq + 80);

    // 找第一个非零 alpha 的层
    BYTE firstAlpha = 0;
    int firstTint = 0, firstState = 0;
    for (int i = 0; i < layerCount; i++) {
        BYTE alpha = *(BYTE*)(alphaTable + layers[i*11 + 7]);
        if (alpha) {
            firstAlpha = alpha;
            firstTint = layers[i*11 + 5];
            firstState = layers[i*11 + 6];
            break;
        }
    }

    // 检查后续层是否与第一层一致
    for (int i = ...; i < layerCount; i++) {
        BYTE alpha = *(BYTE*)(alphaTable + layers[i*11 + 7]);
        if (alpha) {
            if (alpha != firstAlpha ||
                layers[i*11 + 5] != firstTint ||
                layers[i*11 + 6] != firstState) {
                return false;  // 不一致
            }
        }
    }
    return true;  // 全部一致
}
```

**关键洞察**：Special batch 状态一致时走快路径（`DispatchSpecialBatch`），
不一致时走慢路径（`DispatchFallbackMultiPass`），每层独立 ApplyStateBlock + DrawCore。

## 2. CUnit 状态机事件分发表

### 2.1 `CUnit_DispatchEvent (0x6F543A90)` 完整事件表

| 事件 ID | 十六进制 | 处理函数 | 语义 |
|---|---|---|---|
| 852290 | `0xD001A` | `sub_6F3599D0` | 状态转换组 A |
| 852291 | `0xD001B` | `sub_6F3599D0` | 状态转换组 A |
| 852292 | `0xD001C` | `sub_6F3599D0` | 状态转换组 A |
| 852293 | `0xD001D` | `sub_6F3599D0` | 状态转换组 A |
| 852294 | `0xD001E` | `sub_6F3599D0` | 状态转换组 A |
| 852292 | `0xD001C` | `vt[206]` | vtable 回调 |
| 852321 | `0xD0141` | `sub_6F54D610` | 事件处理 |
| 852345 | `0xD0161` | `sub_6F54D520(a2, 0)` | 状态清除 |
| 852346 | `0xD0162` | `sub_6F52EE00` | 阴影相关 |
| 852386 | `0xD01A2` | `sub_6F5449B0` | 事件处理 |
| 852579 | `0xD01F3` | `sub_6F3599D0` | 状态转换组 A |
| 852700 | `0xD029C` | `sub_6F54D520(a2, 1)` | 状态设置 |
| **852709** | **`0xD02A5`** | **`CUnit_ActivateGenericShadowProjector`** | **通用阴影激活** |
| 852710 | `0xD02A6` | `sub_6F543880` | 阴影处理 |
| 默认 | - | `CUnit_ActivateShadowProjector_Dispatch` | 建筑/普通分流 |

### 2.2 建筑/普通分流逻辑

`CUnit_ActivateShadowProjector_Dispatch (0x6F52F4D0)`:

```c
int ActivateShadowProjector_Dispatch(CUnit* this, int event) {
    // 通过 vt[7] 获取单位类型标识
    char* type标识 = GetUnitTypeIdentifier(event->object);

    if (type标识 == byte_6F72642E || type标识 == loc_6F726474) {
        // 建筑路径
        return CUnit_ActivateBuildingShadowProjector(this, event);
    } else {
        // 其他单位路径
        return CUnit_OnBuildComplete(event);
    }
}
```

### 2.3 7 张状态 vtable

CUnit 有 7 张状态 vtable，每张 868 字节（`0x364` 间距）：

| 槽位 | 地址范围 | 语义 |
|---|---|---|
| 0 | `0x6F9EA1A0` ~ `0x6F9EA504` | alive 状态 |
| 1 | `0x6F9EA504` ~ `0x6F9EA868` | dying 状态 |
| 2 | `0x6F9EA868` ~ `0x6F9EABCC` | decay 状态 |
| 3 | `0x6F9EABCC` ~ `0x6F9EAF30` | construction 状态 |
| 4 | `0x6F9EAF30` ~ `0x6F9EB294` | upgrade 状态 |
| 5 | `0x6F9EB294` ~ `0x6F9EB5F8` | morphing 状态 |
| 6 | `0x6F9EB5F8` ~ `0x6F9EB95C` | reincarnating 状态 |

每张 vtable 共享相同的 shadow projector slot（`+0xA4/+0xA8` emitter 数组）。

## 3. RenderBatch_Submit 完整算法

### 3.1 `RenderBatch_Submit (0x6F1375C0)`

```c
void RenderBatch_Submit(CRenderQueue* rq) {
    DWORD count = rq->elementCount;
    DWORD* current = rq->firstElement;

    for (DWORD i = 0; i < count; i++) {
        DWORD* entry = *current;
        if (entry[4]) { current++; continue; }  // 已处理

        DWORD* meshData = entry[3];
        int dispatchIdx = meshData[71];
        int* dispatchInfo = rq->dispatchTable + dispatchIdx * 16;

        // 检查 dispatch 是否启用
        if (!*(BYTE*)(rq->dispatchEnableTable + dispatchIdx * 16 + 3)) {
            current++; continue;
        }

        entry[5] = rq;  // 记录所属 queue

        if (RenderBatch_CanEnqueueToMainQueue(rq, entry)) {
            // 遍历 mesh 的 layer
            int layerCount = dispatchInfo[3];
            int layerBase = dispatchInfo[4];

            for (int layer = 0; layer < layerCount; layer++) {
                int layerInfo = *(DWORD*)(layerBase + 16);
                BYTE alpha = *(BYTE*)(layerInfo + rq->alphaOffset);
                if (!alpha) continue;

                // 计算 tint * alpha
                int tint = dispatchInfo[0];
                BYTE r = (BYTE)((WORD)(GetR(tint) * alpha + 255) >> 8);
                BYTE g = (BYTE)((WORD)(GetG(tint) * alpha + 255) >> 8);
                BYTE b = (BYTE)((WORD)(GetB(tint) * alpha + 255) >> 8);
                DWORD color = (tint & 0xFF000000) | (b << 16) | (g << 8) | r;

                // 检查是否透明
                if (layerInfo & 0x10) {
                    // 透明 → 进 AUCTransparent
                    AUC_Transparent_AddEntry(rq, entry, color, layer);
                } else {
                    // 不透明 → 进主 batch array
                    AddToMainBatchArray(rq, entry, color, layer);
                }
            }
        }
        current++;
    }
}
```

### 3.2 不透明 vs 透明分流

| 条件 | 目标 | 排序 |
|---|---|---|
| `layerInfo & 0x10 == 0` | 主 batch array | `ItemLess`（5 级优先级） |
| `layerInfo & 0x10 != 0` | AUCTransparent | `type 升序 → distSq 降序` |

## 4. FlushSortedItems 完整算法

### 4.1 `RenderQueue_FlushSortedItems (0x6F1380A0)`

```c
unsigned int FlushSortedItems() {
    DWORD count = g_RenderQueue_NumOfElements;
    if (!count) return 0;

    // 步骤 1：复制到排序指针数组（最多 10000 个）
    DWORD sortCount = min(count, 10000);
    g_RenderQueue_SortedCount = sortCount;
    for (DWORD i = 0; i < sortCount; i++) {
        g_RenderQueue_SortedPtrs[i] = g_RenderQueue_BatchArray + i * 20;
    }

    // 步骤 2：排序
    qsort(g_RenderQueue_SortedPtrs, sortCount, 4, RenderQueue_ItemComparator);

    // 步骤 3：对首个 item 应用状态块
    DWORD* firstStateBlock = *(DWORD**)(g_RenderQueue_SortedPtrs[0] + 16);
    GxDevice_ApplyStateBlock(firstStateBlock);

    // 步骤 4：逐条 dispatch
    int lastLayerState = 0;
    int lastMeshData = 0;
    BOOL stateChanged = TRUE;

    for (DWORD i = 0; i < sortCount; i++) {
        DWORD* item = g_RenderQueue_SortedPtrs[i];
        DWORD* meshData = *(DWORD**)(item + 0);
        int layerState = meshData[3];  // meshData+12

        // 状态变化检测
        if (!g_StateOptEnabled || layerState != lastLayerState ||
            lastMeshData != *(DWORD*)(item + 8) ||
            *(DWORD*)(layerState + 20) != *(DWORD*)(lastLayerState + 20)) {
            stateChanged = TRUE;
            lastLayerState = layerState;
            lastMeshData = *(DWORD*)(item + 8);
        }

        // 分发
        if (*(DWORD*)(item + 4) & 2) {
            // special 路径
            RenderQueue_Dispatch_Special(rq, item, stateChanged, FALSE);
        } else {
            // common 路径
            RenderQueue_Dispatch_Common(rq, item, stateChanged, FALSE);
        }

        // 每条之后 StageUpdate(0)
        RenderQueue_StageUpdate(0);
        stateChanged = FALSE;
    }

    // 步骤 5：尾部清理
    if (g_StateCleanupPending) {
        GxDevice_StateCleanup74();
        GxDevice_StateCleanup78();
        g_StateCleanupPending = 0;
    }
}
```

**关键洞察**：
1. 排序后逐条 dispatch，每条之后做 `StageUpdate(0)`
2. 状态变化检测：`layerState ptr + meshData ptr + layerState 前 20B`
3. special vs common 分流：`item[1] & 2`
4. 尾部清理：`StateCleanup74/78`

---

*本章约 500 行，覆盖材质系统、CUnit 状态机、RenderBatch/FlushSortedItems 完整算法。*
