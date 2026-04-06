# War3 渲染系统逆向分析报告

> **版本**: Game.dll 1.27.x (32位)
> **基址**: 0x6F000000 (运行时)
> **生成时间**: 2026-01-22 (updated with RTTI)

---

## 0. RTTI 类结构 (暴雪命名)

### 0.1 核心渲染类

| 类名 | RTTI地址 | 说明 |
|------|----------|------|
| `CModel` | 0x6FB7FE90 | 模型主类 |
| `CModelComplex_` | 0x6FB7FF50 | 复杂模型 (带动画/骨骼) |
| `CModelData` | 0x6FB7FEF8 | 模型静态数据 |
| `CGeoset` | 0x6FB7FEA8 | Geoset (几何集) |
| `CMaterial` | 0x6FB7FDE4 | 材质类 |
| `CRenderBatch` | 0x6FB7D80C | 渲染批次 |

### 0.2 游戏逻辑类

| 类名 | 字符串地址 | 说明 |
|------|----------|------|
| `CAgent` | 0x6F950C30 | Agent基类 |
| `CWidget` | 0x6FA4A0E4 | Widget基类 |
| `CUnit` | 0x6FA4A9F4 | 单位类 |
| `CHandleObject` | 0x6FB7E2BC | 句柄对象基类 |
| `CTeamColorData` | 0x6FB89810 | 阵营颜色数据 |

### 0.3 TeamColor 系统

| 变量/函数 | RVA | 说明 |
|--------|-----|------|
| `g_TeamColorCount` | 0xBE6184 | TeamColor数量 (=24) |
| `g_TeamColorTextures` | 0xBE6188 | 纹理指针数组 |
| `g_TeamColorFriend` | 0xBE62F0 | 盟友颜色 |
| `g_TeamColorEnemy` | 0xBE62FC | 敌人颜色 |
| `TeamColor_LoadAll()` | 0x335E90 | 加载所有纹理 |
| `TeamColor_GetByPlayerId()` | 0x31F0D0 | 根据玩家ID获取 |

---

## 1. 核心函数地址表

### 1.1 RenderQueue 核心函数

| 函数名 | RVA | 大小 | 调用约定 |
|--------|-----|------|----------|
| `RenderBatch_Submit` | 0x1375C0 | 0x1C9 | `__thiscall` |
| `RenderQueue_AddBatch` | 0x139190 | 0x103 | `__thiscall` |
| `RenderQueue_FlushSortedItems` | 0x1380A0 | 0x169 | `__usercall` |
| `RenderQueue_FlushAndReset` | 0x139800 | 0x33 | `__usercall` |
| `RenderQueue_ItemComparator` | 0x1378B0 | 0x1C | `__cdecl` |
| `RenderQueue_ItemLess` | 0x137D50 | 0x180 | `__fastcall` |
| `RenderQueue_StageUpdate` | 0x13A9B0 | 0xBF | `__thiscall` |

### 1.2 Dispatch 函数

| 函数名 | RVA | 大小 | 调用约定 |
|--------|-----|------|----------|
| `RenderQueue_Dispatch_Common` | 0x13A5E0 | 0x126 | `__fastcall` |
| `RenderQueue_Dispatch_Special` | 0x13A780 | 0xA1 | `__fastcall` |

### 1.3 透明对象处理

| 函数名 | RVA | 大小 | 调用约定 |
|--------|-----|------|----------|
| `RenderBatch_CanEnqueueToMainQueue` | 0x1387E0 | 0x4E | `__fastcall` |
| `AUCTransparent_AddEntry` | 0x137AF0 | 0xC0 | `__fastcall` |
| `AUCTransparent_ComputeGrowStep` | 0x137870 | 0x39 | `__thiscall` |
| `AUCTransparent_ReserveArray` | 0x13A020 | 0x8C | `__thiscall` |

### 1.4 WorldObjects 渲染

| 函数名 | RVA | 大小 | 调用约定 |
|--------|-----|------|----------|
| `WorldObjects_RenderGroup` | 0x368E30 | 0x56 | `__userpurge` |
| `WorldObjectEntry_Render` | 0x184EE0 | 0x19 | `__cdecl` |

### 1.5 辅助函数

| 函数名 | RVA | 大小 | 说明 |
|--------|-----|------|------|
| `List_GetData` | 0x0CAE80 | 0x4 | 返回 `this[12]` (偏移 0x0C) |
| `List_GetCount` | 0x0CAE90 | 0x4 | 返回 `this[20]` (偏移 0x14) |
| `GxDevice_ApplyStateBlock` | 0x0E34B0 | 0x10 | 状态块应用 |
| `GxDevice_StateCleanup74` | 0x0E3640 | 0x11 | 清理函数1 |
| `GxDevice_StateCleanup78` | 0x0E3670 | 0x11 | 清理函数2 |
| `TransformPoint3x4` | 0x1AA530 | 0x9E | 3x4矩阵变换 |

---

## 2. 全局变量地址表

### 2.1 RenderQueue 全局变量

| 变量名 | RVA | 类型 | 说明 |
|--------|-----|------|------|
| `g_RenderQueue_BatchCapacity` | 0xBC6BA8 | `uint32_t` | 批次数组容量 |
| `g_RenderQueue_NumOfElements` | 0xBC6BAC | `uint32_t` | 当前批次数量 |
| `g_RenderQueue_BatchArray` | 0xBC6BB0 | `void*` | 批次数组指针 |
| `g_RenderQueue_SortedCount` | 0xBC6BA0 | `uint32_t` | 排序后批次数量 (max 10000) |
| `g_RenderQueue_BatchGrowStep` | 0xBC6BB4 | `uint32_t` | 扩容步长 |
| `g_RenderQueue_SortedPtrs` | 0xBC6BE8 | `void*[10000]` | 排序指针数组本体 |
| `g_RenderQueue_StateOptEnabled` | 0xBDA4D0 | `uint32_t` | 状态优化开关 |
| `g_RenderQueue_StateCleanupPending` | 0xBDA4D4 | `uint32_t` | 尾部状态清理标志 |

### 2.2 AUCTransparent 全局变量

| 变量名 | RVA | 类型 | 说明 |
|--------|-----|------|------|
| `g_AUCTransparent_Capacity` | 0xBC6BB8 | `uint32_t` | 透明队列容量 |
| `g_AUCTransparent_Count` | 0xBC6BBC | `uint32_t` | 透明队列计数 |
| `g_AUCTransparent_Array` | 0xBC6BC0 | `void*` | 透明队列数组 |
| `g_AUCTransparent_GrowStep` | 0xBC6BC4 | `uint32_t` | 透明队列扩容步长 |
| `g_AUCTransparent_SortedCount` | 0xBC6BA4 | `uint32_t` | 透明排序计数 (max 10000) |
| `g_AUCTransparent_SortedPtrs` | 0xBD0828 | `void*[10000]` | 透明排序指针数组 |

### 2.3 其他重要全局变量

| 变量名 | RVA | 类型 | 说明 |
|--------|-----|------|------|
| `gx_device` | 0xBC5420 | `void*` | GxDevice 实例指针 |
| `g_RenderCamera_PosXY` | (反编译中引用) | `float[2]` | 相机XY位置 |
| `g_RenderCamera_PosZ` | (反编译中引用) | `float` | 相机Z位置 |

---

## 3. 结构体偏移详解

### 3.1 RenderBatchElement 结构 (20 字节)

```c
struct RenderBatchElement {   // sizeof = 20 (0x14)
    void*    batchEntry;      // +0x00: RenderablePart* 指针
    uint32_t flags;           // +0x04: 标志位 (bit0=meshFlag, bit1=hasMoreLayers, bit2|3=special)
    uint32_t layerIndex;      // +0x08: 层索引
    uint32_t layerCounter;    // +0x0C: 可见层计数器
    void*    layerStatePtr;   // +0x10: 层状态块指针 (36 bytes)
};
```

**flags 字段详解**:
- `bit 0` (0x01): meshFlag - 来自 MeshData+0x104，若非0则设置
- `bit 1` (0x02): hasMoreLayers - 存在后续可见层
- `(flags & 3) == 3`: 标记为 Special 类型，走 Dispatch_Special 分支

### 3.2 SceneNode 结构偏移

```c
struct SceneNode {
    // +0x00 ~ +0x08: 未知
    uint32_t renderableCount;    // +0x0C: 可渲染对象数量
    void*    renderableList;     // +0x10: 可渲染对象列表 (指针数组)
    // +0x14 ~ +0x1C: 未知
    void*    cullTable;          // +0x20: 剔除表
    // +0x24 ~ +0x2C: 未知
    void*    meshInfoTable;      // +0x30: MeshInfo 表 (指针数组)
    // +0x34 ~ +0x4C: 未知
    uint32_t visibilityOffset;   // +0x50: 层可见性偏移量
    // +0x54 ~ +0x60: 未知
    float    worldMatrix[12];    // +0x64: 世界变换矩阵 (3x4)
    uint32_t flags;              // +0x94: 标志位 (bit4=0x10 影响透明处理)
    // ...
    uint32_t lastBatchCount;     // +0x9C (156): 上次批次计数
    // ...
    void*    childList[3];       // +0xC8 (200): 子列表 (+8 偏移开始)
    uint32_t childCount;         // +0xC4 (196): 子节点数量
    void*    childVisFlags;      // +0xD4 (212): 子节点可见性标志
    // 透明列表（IDA 已确认）
    uint32_t list4Count;         // +0xA8 (168): List4 数量
    void*    list4Data;          // +0xAC (172): List4 数据指针 (stride=36)
    uint32_t list0Count;         // +0xDC (220): List0 数量
    void*    list0Data;          // +0xE0 (224): List0 数据指针 (stride=104)
    uint32_t list2Count;         // +0xE8 (232): List2 数量
    void**   list2Ptrs;          // +0xEC (236): List2 指针数组
    uint32_t list3Count;         // +0xF4 (244): List3 数量
    void*    list3Data;          // +0xF8 (248): List3 数据指针 (stride=356)
};
```

### 3.3 RenderablePart 结构偏移

```c
struct RenderablePart {
    // +0x00 ~ +0x08: 未知
    void*    meshData;           // +0x0C: MeshData 指针
    uint32_t skipFlag;           // +0x10: 跳过标志 (非0则跳过)
    void*    sceneNodeBackPtr;   // +0x14: 回指 SceneNode (由 RenderBatch_Submit 写入)
    // ...
};
```

### 3.4 MeshData 结构偏移

```c
struct MeshData {
    // +0x00 ~ +0x0C: 未知
    float    boundingPos[3];     // +0x10C: 包围盒中心位置 (用于透明排序)
    uint32_t meshFlag_104;       // +0x104: 网格标志 (非0触发 meshBreak)
    uint32_t meshIndex;          // +0x108: 网格索引 (用于查表)
    // +0x10C: boundingPos
    // +0x118: 未知
    uint32_t cullIndex;          // +0x11C: 剔除索引
    uint32_t transparentKey;     // +0x120: 透明排序键
    // ...
};
```

### 3.5 MeshInfo 结构偏移

```c
struct MeshInfo {
    // +0x00 ~ +0x08: 未知
    uint32_t layerCount;         // +0x0C: 层数量
    void*    stateBlockBase;     // +0x10: 状态块基址 (+4 开始使用)
    // +0x14 ~ +0x34: 未知
    void*    layerInfo;          // +0x38: LayerInfo 指针
    // ...
};
```

### 3.6 LayerInfo 结构偏移

```c
struct LayerInfo {
    // +0x00 ~ +0x0C: 未知
    void*    layerDataBase;      // +0x10: LayerData 基址
    // ...
};
```

### 3.7 LayerData 结构 (44 字节步长)

```c
struct LayerData {               // stride = 44 (0x2C)
    // 实际使用从 +0x1C 开始
    void*    layerVisibilityPtr; // +0x00 (相对于 layerDataBase+0x1C)
    // ...
    uint8_t  layerFlags;         // +0x08 (相对): bit0 影响颜色处理
    // ...
};
```

### 3.8 LayerState 结构 (36 字节步长)

```c
struct LayerState {              // stride = 36 (0x24)
    // 状态块基址 +4 开始
    // 用于 memcmp 比较判断 layerChanged（仅比较前 20 字节）
    // 传递给 GxDevice_ApplyStateBlock
    uint8_t  data[36];
};
```

### 3.9 CullTable 结构

```c
struct CullEntry {               // stride = 16 (0x10)
    // +0x00 ~ +0x02: 未知
    uint8_t  visible;            // +0x03: 可见标志 (0=不可见)
    // +0x04 ~ +0x0F: 未知
};
```

---

## 4. 核心函数伪代码

### 4.1 RenderBatch_Submit

```c
void __thiscall RenderBatch_Submit(SceneNode* this) {
    uint32_t renderableCount = this->renderableCount;  // +0x0C
    void** renderableList = this->renderableList;      // +0x10
    void* cullTable = this->cullTable;                 // +0x20
    void** meshInfoTable = this->meshInfoTable;        // +0x30
    uint32_t visibilityOffset = this->visibilityOffset; // +0x50

    for (uint32_t i = 0; i < renderableCount; i++) {
        RenderablePart* part = renderableList[i];
        if (!part || part->skipFlag != 0) continue;    // +0x10

        MeshData* meshData = part->meshData;           // +0x0C
        if (!meshData) continue;

        uint32_t cullIndex = meshData->cullIndex;      // +0x11C
        if (cullTable[cullIndex * 16 + 3] == 0) continue;  // 剔除检查

        part->sceneNodeBackPtr = this;                 // +0x14 写回

        // 透明/不透明分流
        if (!RenderBatch_CanEnqueueToMainQueue(this, part)) {
            // 透明对象 -> AUCTransparent
            float worldPos[3];
            TransformPoint3x4(worldPos, meshData->boundingPos, this->worldMatrix);
            AUCTransparent_AddEntry(part, 0, worldPos, meshData->transparentKey);
            continue;
        }

        // 不透明对象 -> 主队列
        uint32_t meshIndex = meshData->meshIndex;      // +0x108
        MeshInfo* meshInfo = meshInfoTable[meshIndex];
        if (!meshInfo) continue;

        uint32_t layerCount = meshInfo->layerCount;    // +0x0C
        void* stateBlockBase = meshInfo->stateBlockBase; // +0x10
        LayerInfo* layerInfo = meshInfo->layerInfo;    // +0x38
        void* layerDataBase = layerInfo->layerDataBase; // +0x10

        uint32_t visibleLayerCounter = 0;
        uint8_t* layerData = (uint8_t*)layerDataBase + 0x1C;
        uint8_t* statePtr = (uint8_t*)stateBlockBase + 4;

        for (uint32_t layerIdx = 0; layerIdx < layerCount; layerIdx++) {
            void* layerVisPtr = *(void**)layerData;
            if (!layerVisPtr) {
                layerData += 44; statePtr += 36;
                continue;
            }

            uint8_t layerVisible = *(uint8_t*)((uint8_t*)layerVisPtr + visibilityOffset);
            if (layerVisible == 0) {
                layerData += 44; statePtr += 36;
                continue;
            }

            // 确保容量
            EnsureRenderQueueCapacity();

            // 写入 RenderBatchElement
            RenderBatchElement* batch = GetNextBatchSlot();
            batch->batchEntry = part;
            batch->flags = 0;
            batch->layerIndex = layerIdx;
            batch->layerCounter = visibleLayerCounter;
            batch->layerStatePtr = statePtr;

            // 设置 meshFlag
            if (meshData->meshFlag_104 != 0) {
                batch->flags |= 1;
            }

            // 设置 hasMoreLayers
            if (visibleLayerCounter > 0) {
                batch->flags |= 2;
            } else {
                // 首层：检查后续是否有可见层
                for (uint32_t probe = layerIdx + 1; probe < layerCount; probe++) {
                    // ... 探测逻辑
                    if (probeVisible) { batch->flags |= 2; break; }
                }
            }

            g_RenderQueue_NumOfElements++;
            visibleLayerCounter++;

            // [关键] meshFlag 设置时跳出层循环
            if (batch->flags & 1) break;

            layerData += 44;
            statePtr += 36;
        }
    }
}
```

### 4.2 RenderQueue_FlushSortedItems

```c
void RenderQueue_FlushSortedItems() {
    uint32_t num = g_RenderQueue_NumOfElements;
    if (num == 0) return;

    uint32_t count = min(num, 10000);
    g_RenderQueue_SortedCount = count;

    // 1. 复制指针
    void* batchArray = g_RenderQueue_BatchArray;
    for (uint32_t i = 0; i < count; i++) {
        g_RenderQueue_SortedPtrs[i] = (uint8_t*)batchArray + i * 20;
    }

    // 2. 排序
    qsort(g_RenderQueue_SortedPtrs, count, 4, RenderQueue_ItemComparator);

    // 3. 初始状态应用
    RenderBatchElement* first = g_RenderQueue_SortedPtrs[0];
    GxDevice_ApplyStateBlock(first->layerStatePtr);

    void* lastMeshData = NULL;
    uint32_t lastLayerIndex = 0;
    void* lastLayerStatePtr = first->layerStatePtr;
    bool lastWasSpecial = false;

    // 4. 遍历调度
    for (uint32_t i = 0; i < count; i++) {
        RenderBatchElement* batch = g_RenderQueue_SortedPtrs[i];
        void* meshData = batch->batchEntry->meshData;  // batchEntry+0x0C

        // stateChanged 判断
        bool stateChanged = true;
        if (g_RenderQueue_StateOptEnabled) {
            if (meshData == lastMeshData &&
                batch->layerIndex == lastLayerIndex &&
                meshData->meshFlag_104 == 0) {
                stateChanged = false;
            }
        }

        if ((batch->flags & 3) == 3) {
            // Special 分支
            Dispatch_Special(meshData, batch->batchEntry, batch->layerIndex, stateChanged);
        } else {
            // Common 分支
            int layerChanged = 1;
            if (g_RenderQueue_StateOptEnabled && !lastWasSpecial) {
                if (memcmp(lastLayerStatePtr, batch->layerStatePtr, 20) == 0) {
                    layerChanged = 0;
                }
            }
            Dispatch_Common(meshData, batch->batchEntry, batch->layerIndex, layerChanged, stateChanged);
        }

        RenderQueue_StageUpdate(0);

        lastWasSpecial = ((batch->flags & 3) == 3);
        lastLayerStatePtr = batch->layerStatePtr;
        lastMeshData = meshData;
        lastLayerIndex = batch->layerIndex;
    }

    // 5. 尾部清理
    if (g_RenderQueue_StateCleanupPending) {
        GxDevice_StateCleanup74();
        GxDevice_StateCleanup78();
        g_RenderQueue_StateCleanupPending = 0;
    }
}
```

### 4.3 RenderQueue_ItemComparator

```c
int __cdecl RenderQueue_ItemComparator(_DWORD* a, _DWORD* b) {
    // a/b 指向 SortedPtrs 数组元素 (即 RenderBatchElement**)
    return RenderQueue_ItemLess(*a, *b) != 0 ? -1 : 1;
}
```

### 4.4 RenderQueue_ItemLess (排序核心)

```c
int __fastcall RenderQueue_ItemLess(RenderBatchElement* a, RenderBatchElement* b) {
    bool aIsSpecial = ((a->flags & 3) == 3);
    bool bIsSpecial = ((b->flags & 3) == 3);

    // 1. 先按 Special 类型分组
    if (aIsSpecial != bIsSpecial) {
        return aIsSpecial;  // Special 排前面
    }

    // 2. 若都有 hasMoreLayers (flags & 2)
    if ((a->flags & 2) && (b->flags & 2)) {
        void* meshDataA = a->batchEntry->meshData;
        void* meshDataB = b->batchEntry->meshData;
        if (meshDataA != meshDataB) return meshDataA < meshDataB;

        if (a->layerCounter != b->layerCounter) return a->layerCounter < b->layerCounter;

        // 比较 layerStatePtr 内容
        return memcmp(a->layerStatePtr, b->layerStatePtr, 20) < 0;
    }

    // 3. 仅其中一个有 hasMoreLayers
    if ((a->flags & 2) && !(b->flags & 2)) return true;
    if (!(a->flags & 2) && (b->flags & 2)) return false;

    // 4. 都没有 hasMoreLayers：比较 layerStatePtr 内容
    return memcmp(a->layerStatePtr, b->layerStatePtr, 20) < 0 ||
           (memcmp(...) == 0 && a->batchEntry->meshData < b->batchEntry->meshData);
}
```

### 4.5 WorldObjects_RenderGroup

```c
int __userpurge WorldObjects_RenderGroup(void* worldPtr, int groupIdx) {
    // groupIdx: 0, 1, 2 分别对应不同列表
    void* listPtr;
    switch (groupIdx) {
        case 0: listPtr = ((uint32_t*)worldPtr)[91]; break;
        case 1: listPtr = ((uint32_t*)worldPtr)[92]; break;
        case 2: listPtr = ((uint32_t*)worldPtr)[93]; break;
        default: return 0;
    }
    if (!listPtr) return 0;

    void* listData = List_GetData(listPtr);  // listPtr[12]
    uint32_t listCount = List_GetCount(listPtr);  // listPtr[20]
    if (!listData || listCount == 0) return 0;

    int lastResult = 0;
    uint8_t* entryPtr = (uint8_t*)listData;
    for (uint32_t i = 0; i < listCount; i++) {
        void* objectEntry = *(void**)entryPtr;
        if (objectEntry) {
            lastResult = WorldObjectEntry_Render(objectEntry);
        }
        entryPtr += 24;  // WorldObjectListEntry stride = 24
    }
    return lastResult;
}
```

### 4.6 RenderBatch_CanEnqueueToMainQueue (透明分流)

```c
BOOL __fastcall RenderBatch_CanEnqueueToMainQueue(SceneNode* sceneNode, RenderablePart* part) {
    MeshData* meshData = part->meshData;
    void** meshInfoTable = sceneNode->meshInfoTable;  // +0x30
    MeshInfo* meshInfo = meshInfoTable[meshData->meshIndex_264];  // meshData+0x108

    uint32_t layerCount = meshInfo->layerCount;  // +0x0C
    if (layerCount == 0) return TRUE;

    void* stateBlockBase = meshInfo->stateBlockBase;  // +0x10
    LayerInfo* layerInfo = meshInfo->layerInfo;  // +0x38
    void* layerDataBase = layerInfo->layerDataBase;  // +0x10

    uint8_t* layerData = (uint8_t*)layerDataBase + 0x1C;
    uint8_t* statePtr = (uint8_t*)stateBlockBase + 4;
    uint32_t visibilityOffset = sceneNode->visibilityOffset;  // +0x50

    for (uint32_t i = 0; i < layerCount; i++) {
        void* layerVisPtr = *(void**)layerData;
        if (layerVisPtr) {
            uint8_t visible = *(uint8_t*)((uint8_t*)layerVisPtr + visibilityOffset);
            if (visible != 0) {
                // 检查该层的混合模式
                uint32_t blendMode = *(uint32_t*)(statePtr + 24);  // statePtr+0x18
                return blendMode < 2;  // < 2 表示不透明
            }
        }
        layerData += 44;
        statePtr += 36;
    }
    return TRUE;  // 没找到可见层，默认不透明
}
```

### 4.7 Dispatch_Common 签名

```c
int __fastcall RenderQueue_Dispatch_Common(
    void* meshData,       // ecx: MeshData 指针
    void* renderablePart, // edx: RenderablePart 指针
    int   layerIndex,     // a3: 层索引
    int   layerChanged,   // a4: 层状态是否改变 (0/1)
    int   stateChanged    // a5: 状态是否改变 (0/1)
);
```

### 4.8 Dispatch_Special 签名

```c
int __fastcall RenderQueue_Dispatch_Special(
    void* meshData,       // ecx: MeshData 指针
    void* renderablePart, // edx: RenderablePart 指针
    int   layerIndex,     // a3: 层索引
    int   stateChanged    // a4: 状态是否改变 (0/1)
);
```

---

## 5. 关键常量

| 常量 | 值 | 说明 |
|------|-----|------|
| MAX_BATCHES | 10000 | 最大批次数量 |
| RenderBatchElement stride | 20 | 批次元素大小 |
| LayerState stride | 36 | 层状态块大小 |
| LayerData stride | 44 | 层数据大小 |
| CullEntry stride | 16 | 剔除表条目大小 |
| WorldObjectListEntry stride | 24 | 世界对象列表条目大小 |

---

## 6. 调用链路

```
RenderQueue_FlushAndReset (0x139800)
├── RenderQueue_StageUpdate(1)
├── RenderQueue_FlushSortedItems (0x1380A0)
│   ├── qsort(SortedPtrs, count, 4, ItemComparator)
│   │   └── RenderQueue_ItemComparator (0x1378B0)
│   │       └── RenderQueue_ItemLess (0x137D50)
│   ├── GxDevice_ApplyStateBlock (0x0E34B0)
│   └── for each batch:
│       ├── Dispatch_Common (0x13A5E0) 或 Dispatch_Special (0x13A780)
│       └── RenderQueue_StageUpdate(0)
├── sub_6F138210 (透明队列刷新)
│   ├── qsort(AUCTransparentSortedPtrs, ...)
│   └── RenderQueue_StageUpdate(0)
├── RenderQueue_StageUpdate(1)
└── Reset counters

WorldObjects_RenderGroup (0x368E30)
├── List_GetData / List_GetCount
└── for each entry:
    └── WorldObjectEntry_Render (0x184EE0)
        └── RenderQueue_AddBatch (0x139190)
            └── RenderBatch_Submit (0x1375C0)
                ├── RenderBatch_CanEnqueueToMainQueue (0x1387E0)
                │   └── 透明: AUCTransparent_AddEntry (0x137AF0)
                └── 不透明: 写入 g_RenderQueue_BatchArray
```

---

## 7. 重要注意事项

1. **批次数组是动态分配的**：`g_RenderQueue_BatchArray` 是堆指针，容量不足时会扩容
2. **SortedPtrs 是静态数组**：`g_RenderQueue_SortedPtrs` 是 10000 元素的固定数组，不是指针
3. **ItemComparator 不返回 0**：原版比较器在相等时返回 1 而非 0，使用 std::sort 时需要额外处理
4. **透明对象走独立队列**：满足 `blendMode >= 2` 的层会被分流到 `AUCTransparent` 队列
5. **meshFlag 触发提前退出**：`meshData+0x104` 非0时，只处理首个可见层然后 break
6. **状态优化依赖连续性**：相同 meshData + layerIndex + meshFlag==0 时可跳过状态切换

---

本报告由 IDA Pro MCP 自动生成，供 Codex 和 Claude 进行后续开发使用。

---

## 8. GxDevice 虚函数表 (关键偏移)

| vtable 偏移 | 函数名 | 说明 |
|-------------|--------|------|
| +0x54 (84) | `RenderSceneFlush` | 场景刷新 (0x0E39E0 包装) |
| +0x6C (108) | `SetVertexBuffer` | 设置顶点缓冲 (0x0E3550 包装) |
| +0x70 (112) | `DrawPrimitive` | 绘制图元 (0x0E3540 包装) |
| +0x74 (116) | `StateCleanup74` | 状态清理1 (0x0E3640 包装) |
| +0x78 (120) | `StateCleanup78` | 状态清理2 (0x0E3670 包装) |
| +0x98 (152) | `ApplyStateBlock` | 应用状态块 (0x0E34B0 包装) |

**全局变量**:
- `gx_device` @ 0xBC5420: GxDevice 实例指针
- `dword_6FBC5440`: Draw 调用计数器 (StateCleanup74 递减)
- `dword_6FBC543C`: 另一个计数器 (StateCleanup78 递减)
  
**IDA 确认**:
- `0x0E39E0`/`0x0E34B0`/`0x0E3550`/`0x0E3540`/`0x0E3640`/`0x0E3670`
  均为 vtable 间接调用封装，对应表内偏移。

---

## 9. WorldObjectEntry 结构

```c
struct WorldObjectEntry {
    void*    vtable;             // +0x00: 虚函数表
    // +0x04 ~ +0x1C: 未知
    void*    sceneNode;          // +0x20: SceneNode 指针
    // ...
};

// WorldObjectEntry::vtable
//   [0]: 析构函数
//   [1-4]: 未知
//   [5] (+0x14): PreRender() - 渲染前准备函数
```

**IDA 确认**:
- `sceneNode` 在 `+0x20`，`WorldObjectEntry_Render` 调用 `vtable[5]` 后直接跳入 `RenderQueue_AddBatch`。

**WorldObjectEntry_Render 逻辑** (0x184EE0):
```c
int __thiscall WorldObjectEntry_Render(WorldObjectEntry* this) {
    if (this->sceneNode == 0) return 0;  // +0x20
    
    // 调用虚函数 PreRender
    this->vtable[5](this);  // vtable+0x14
    
    // 调用 RenderQueue_AddBatch
    return RenderQueue_AddBatch(this->sceneNode);
}
```

---

## 10. 透明对象类型码

`AUCTransparent_AddEntry` 接收的类型码和对应 stride:

| 类型码 | 来源 | stride | 说明 |
|--------|------|--------|------|
| 0 | `SceneNode_AddTransparentList0` | 104 | 粒子发射器 |
| 1 | (未见调用) | - | 保留 |
| 2 | `SceneNode_AddTransparentList2` | 指针数组 | 缎带发射器 |
| 3 | `SceneNode_AddTransparentList3` | 356 | 特效 |
| 4 | `SceneNode_AddTransparentList4` | 36 | 附着物 |
| 5 | (自定义回调) | - | 自定义渲染函数 |

**透明队列刷新** (sub_6F138210):
```c
void TransparentQueue_Flush() {
    uint32_t count = min(g_AUCTransparent_Count, 10000);
    g_AUCTransparent_SortedCount = count; // 0xBC6BA4

    // 复制指针并排序
    for (int i = 0; i < count; i++) {
        SortedPtrs[i] = &g_AUCTransparent_Array[i * 24];
    }
    qsort(SortedPtrs, count, 4, TransparentComparator /* sub_6F1378D0 */);
    
    // 按类型分发
    for (int i = 0; i < count; i++) {
        void* entry = SortedPtrs[i];
        switch (entry[0]) {  // 类型码
            case 0: RenderTransparent_Type0(...); break;
            case 1: RenderTransparent_Type1(...); break;
            case 2: RenderTransparent_Type2(...); break;
            case 3: RenderTransparent_Type3(...); break;
            case 4: RenderTransparent_Type4(...); break;
            case 5: entry->callback(entry->arg1, entry->arg2); break;
        }
        RenderQueue_StageUpdate(0);
    }
}
```

**AUCTransparentEntry (24 bytes, IDA 已确认)**:
```c
struct AUCTransparentEntry {    // sizeof = 24 (0x18)
    uint32_t type;              // +0x00: 类型码
    uint32_t sortKey;           // +0x04: 透明排序键
    float    distSq;            // +0x08: 到相机距离平方
    void*    payload;           // +0x0C: 对象/回调指针
    uint32_t arg1;              // +0x10: 回调参数1（type=5）
    uint32_t arg2;              // +0x14: 回调参数2（type=5）
};
```

**IDA 确认**:
- `AUCTransparent_AddEntry` 以 `g_RenderCamera_PosXY/PosZ` 计算 `distSq` 写入 +0x08。

---

## 11. SceneNode 透明列表偏移

| 偏移 | 类型 | 说明 |
|------|------|------|
| +0xA8 (168) | `uint32_t` | List4 数量 |
| +0xAC (172) | `void*` | List4 数据指针 (stride=36) |
| +0xDC (220) | `uint32_t` | List0 数量 |
| +0xE0 (224) | `void*` | List0 数据指针 (stride=104) |
| +0xE8 (232) | `uint32_t` | List2 数量 |
| +0xEC (236) | `void**` | List2 指针数组 |
| +0xF4 (244) | `uint32_t` | List3 数量 |
| +0xF8 (248) | `void*` | List3 数据指针 (stride=356) |

**IDA 确认**:
- `SceneNode_AddTransparentList4` (0x137460) 使用 +0xA8/+0xAC (stride=36)
- `SceneNode_AddTransparentList0` (0x137540) 使用 +0xDC/+0xE0 (stride=104)
- `SceneNode_AddTransparentList2` (0x1374C0) 使用 +0xE8/+0xEC (指针数组)
- `SceneNode_AddTransparentList3` (0x137790) 使用 +0xF4/+0xF8 (stride=356)

---

## 12. Native Renderer 优化路径

### 12.1 Instancing 优化

**获取模型 ID 路径**:
```
WorldObjectEntry
    └─ +0x20: SceneNode
        └─ +0x10: RenderableList[]
            └─ RenderablePart
                └─ +0x0C: MeshData
                    └─ +0x108: meshIndex
                        └─ MeshInfoTable[meshIndex]
                            └─ (模型唯一标识)
```

**模型句柄分配** (0x128140):
- 使用 `JassFrameAllocator_NewFrame` 分配
- vtable: `TAllocatedHandleObjectLeaf<CModelComplex_,128>::vftable`
- 类型名: "HMODEL"

### 12.2 状态排序优化

当前排序键:
1. `(flags & 3) == 3` → Special 优先
2. `flags & 2` → hasMoreLayers 分组
3. `meshData` 指针 → 模型分组
4. `layerCounter` → 层顺序
5. `layerStatePtr` 内容 (20 bytes) → 状态块排序

**优化方向**: 可以用 `meshData+0x108` (meshIndex) 替代指针比较，实现跨帧稳定排序。

### 12.3 Draw 调用合并

Draw 调用路径:
```
Dispatch_Common / Dispatch_Special
    └─ sub_6F0E3520
        ├─ sub_6F0E3550(vertexOffset) → gx_device->vtable[27](...)  // SetVertexBuffer
        ├─ sub_6F0E3540()              → gx_device->vtable[28](...)  // DrawPrimitive
        └─ GxDevice_StateCleanup74()   → gx_device->vtable[29](...)  // Cleanup
```

**优化方向**: Hook `sub_6F0E3520` 收集 Draw 参数，批量发送到 GPU。

### 12.4 完全替换渲染循环

需要 Hook 的函数:
1. `WorldObjects_RenderGroup` (0x368E30) - 遍历入口
2. `RenderBatch_Submit` (0x1375C0) - 批次提交
3. `RenderQueue_FlushSortedItems` (0x1380A0) - 排序和调度

**替换策略**:
1. 在 `WorldObjects_RenderGroup` 收集所有对象
2. 按模型 ID 分组，构建 Instance 缓冲
3. 跳过原版 `RenderBatch_Submit`，直接发送 Instanced Draw Calls

---

## 13. 关键发现汇总

1. **vtable[5] 是 PreRender**: WorldObjectEntry 在渲染前调用 `vtable+0x14` 准备数据
2. **meshIndex 是模型标识**: `MeshData+0x108` 可用于 Instancing 分组
3. **透明对象有 6 种类型**: 0-5，各有不同的渲染路径
4. **Draw 调用通过 GxDevice 虚函数表**: vtable[27/28/29] 分别是 SetVerts/Draw/Cleanup
5. **状态优化比较 20 字节**: `memcmp(layerState, 20)` 判断 layerChanged
6. **LayerState 内 +0x18 是混合模式**: `blendMode >= 2` 表示透明
