# 实现就绪性评估报告

## 当前实现状态

### ✅ 已完成的函数

| 函数名 | 实现状态 | 可用性 | 备注 |
|--------|----------|--------|------|
| `Native_CWorld_RenderScene` | ✅ 完整 | ⚠️ 部分可用 | 依赖外部函数 |
| `Native_RenderWorld_DispatchStage` | ✅ 完整 | ⚠️ 部分可用 | 依赖外部函数 |
| `WorldObjects_RenderGroup` | ✅ 完整 | ✅ 可用 | 基于IDA反编译 |
| `WorldObjectEntry_Render` | ⚠️ 有误 | ❌ 不可用 | **存在严重错误** |
| `Native_RenderQueue_AddBatch` | ⚠️ 不完整 | ❌ 不可用 | 依赖未实现函数 |

### ❌ 未实现的函数

| 函数名 | 重要性 | 依赖关系 |
|--------|--------|----------|
| `RenderBatch_Submit` | 🔴 **关键** | 所有渲染 |
| `RenderQueue_FlushSortedItems` | 🔴 **关键** | 队列刷新 |
| `RenderQueue_ItemComparator` | 🔴 **关键** | 排序 |
| `RenderQueue_ItemLess` | 🔴 **关键** | 排序 |
| `RenderBatch_CanEnqueueToMainQueue` | 🔴 **关键** | 透明分流 |
| `AUCTransparent_AddEntry` | 🔴 **关键** | 透明队列 |
| `RenderQueue_StageUpdate` | 🔴 **关键** | 状态更新 |
| `RenderQueue_Dispatch_Common` | 🔴 **关键** | GPU派发 |
| `RenderQueue_Dispatch_Special` | 🔴 **关键** | 特殊派发 |
| `SceneNode_AddTransparentList0/2/3/4` | 🟡 中等 | 透明列表 |

## 🔴 严重错误分析

### 错误1: WorldObjectEntry_Render 参数传递错误

**当前实现（错误）：**
```cpp
extern "C" int WorldObjectEntry_Render(
    int* entry,     // a1: WorldObjectEntry指针
    int categoryMode // a2: categoryMode
) {
    if (entry[8] != 0) {  // entry->sceneNode
        // ...
        return RenderQueue_AddBatch((void*)entry, categoryMode);  // ❌ 错误！
    }
    return 0;
}
```

**汇编代码（正确）：**
```asm
mov     ecx, [esi+20h]  ; ecx = entry->sceneNode
jmp     RenderQueue_AddBatch
```

**问题：**
- IDA反编译显示：`return RenderQueue_AddBatch(a1, a2);`
- 汇编代码显示：传递 `entry->sceneNode`（+0x20），不是 `entry` 本身
- **这是致命错误！会导致渲染队列接收到错误参数**

**修正方案：**
```cpp
extern "C" int WorldObjectEntry_Render(
    int* entry,     // a1: WorldObjectEntry指针
    int categoryMode // a2: categoryMode
) {
    if (entry[8] != 0) {  // entry->sceneNode @ +0x20
        // 调用 PreRender
        void** vtable = (void**)entry[0];
        typedef void (__thiscall* PreRenderFunc)(void*);
        PreRenderFunc preRender = (PreRenderFunc)vtable[5];
        preRender((void*)entry);

        // 传递 sceneNode，不是 entry！
        return RenderQueue_AddBatch((void*)entry[8], categoryMode);  // ✅ 修正
    }
    return 0;
}
```

### 错误2: SceneNode 偏移未正确使用

**当前实现（错误）：**
```cpp
extern "C" void Native_RenderQueue_AddBatch(
    SceneNode* sceneNode,
    int categoryMode
) {
    RenderBatch_Submit(sceneNode);
    
    // ❌ 使用了错误的偏移
    if (sceneNode->flags & 0x10) {  // +0x94 正确
        SceneNode_AddTransparentList0(nullptr, sceneNode);
        // ...
    }
}
```

**问题：**
- `RenderBatch_Submit` 需要访问 `sceneNode+0x0C` 和 `sceneNode+0x10`
- `RenderQueue_AddBatch` 需要访问 `sceneNode+0x94`, `sceneNode+0x98`, `sceneNode+0x9C` 等
- **当前 SceneNode 结构定义不完整，缺少中间偏移**

**修正方案：**
```cpp
struct SceneNode {
    void* vtable;              // +0x00
    uint8_t padding1[12];      // +0x04 ~ +0x0F
    uint32_t renderableCount;   // +0x0C ✅
    void** renderableList;      // +0x10 ✅
    uint8_t padding2[20];      // +0x14 ~ +0x27
    void* sceneNode;           // +0x28 (在WorldObjectEntry中)
    uint8_t padding3[108];     // +0x2C ~ +0x93
    uint32_t flags;            // +0x94 ✅
    uint8_t padding4[4];       // +0x95 ~ +0x98
    void* childPtrArray;        // +0x9C ✅
    uint8_t padding5[36];      // +0xA0 ~ +0xC3
    uint32_t childCount;        // +0xC4 ✅
    void* childVisFlags;       // +0xC8 ✅
    uint8_t padding6[12];      // +0xCC ~ +0xD7
    void* childVisibilityArray; // +0xD4 ✅
    // ...
};
```

## 🔴 缺失的关键实现

### 1. RenderBatch_Submit - 批次提交核心

**重要性：** 🔴 **最高**
**状态：** ❌ 未实现
**依赖：** `RenderBatch_CanEnqueueToMainQueue`, `AUCTransparent_AddEntry`

**功能：**
- 遍历 `SceneNode->renderableList`
- 检查剔除标志
- 按材质层拆分为 `RenderBatch`
- 不透明对象 → 主队列
- 透明对象 → AUCTransparent 队列

**必须实现的代码：**
```cpp
extern "C" void RenderBatch_Submit(SceneNode* sceneNode) {
    uint32_t renderableCount = *(uint32_t*)((uint8_t*)sceneNode + 0x0C);
    if (renderableCount == 0) return;
    
    void** renderableList = *(void***)((uint8_t*)sceneNode + 0x10);
    
    for (uint32_t i = 0; i < renderableCount; i++) {
        void* part = renderableList[i];
        if (!part) continue;
        
        uint32_t skipFlag = *(uint32_t*)((uint8_t*)part + 0x10);
        if (skipFlag != 0) continue;
        
        // 写回 sceneNode 指针
        *(void**)((uint8_t*)part + 0x14) = sceneNode;
        
        // 检查透明/不透明
        if (RenderBatch_CanEnqueueToMainQueue(sceneNode, part)) {
            AddToMainQueue(part);  // 需要实现
        } else {
            // 计算世界坐标
            float worldPos[3];
            float* boundingPos = (float*)((uint8_t*)part + 0x10C);
            float* worldMatrix = (float*)((uint8_t*)sceneNode + 0x64);
            TransformPoint3x4(worldPos, boundingPos, worldMatrix);
            
            uint32_t transparentKey = *(uint32_t*)((uint8_t*)part + 0x120);
            AUCTransparent_AddEntry(part, 0, worldPos, transparentKey);
        }
    }
}
```

### 2. RenderQueue_FlushSortedItems - 队列刷新核心

**重要性：** 🔴 **最高**
**状态：** ❌ 未实现
**依赖：** `RenderQueue_ItemComparator`, `RenderQueue_ItemLess`, `RenderQueue_Dispatch_Common/Special`, `RenderQueue_StageUpdate`

**功能：**
- 排序批次数组
- 循环分发到 GPU
- 状态优化和清理

**必须实现的代码：**
```cpp
extern "C" unsigned int RenderQueue_FlushSortedItems(
    void* param_edi,  // 类别参数
    void* param_esi   // 上一个应用的状态块指针
) {
    uint32_t num = global::g_RenderQueue_NumOfElements;
    if (num == 0) return 0;
    
    uint32_t count = min(num, 10000);
    global::g_RenderQueue_SortedCount = count;
    
    // 复制指针
    void* batchArray = global::g_RenderQueue_BatchArray;
    for (uint32_t i = 0; i < count; i++) {
        global::g_RenderQueue_SortedPtrs[i] = (void*)((uint8_t*)batchArray + i * 20);
    }
    
    // 排序
    qsort(global::g_RenderQueue_SortedPtrs, count, 4, RenderQueue_ItemComparator);
    
    // 初始状态应用
    RenderBatchElement* first = (RenderBatchElement*)global::g_RenderQueue_SortedPtrs[0];
    GxDevice_ApplyStateBlock(first->layerStatePtr);
    
    void* lastMeshData = nullptr;
    uint32_t lastLayerIndex = 0;
    void* lastLayerStatePtr = first->layerStatePtr;
    bool lastWasSpecial = false;
    
    // 循环调度
    for (uint32_t i = 0; i < count; i++) {
        RenderBatchElement* batch = (RenderBatchElement*)global::g_RenderQueue_SortedPtrs[i];
        void* meshData = *(void**)((uint8_t*)batch->batchEntry + 0x0C);
        
        // stateChanged 判断
        bool stateChanged = true;
        if (global::g_RenderQueue_StateOptEnabled) {
            if (meshData == lastMeshData &&
                batch->layerIndex == lastLayerIndex &&
                *(uint32_t*)((uint8_t*)meshData + 0x104) == 0) {
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
            RenderQueue_Dispatch_Common(batch->batchEntry, layerChanged, stateChanged);
        }
        
        RenderQueue_StageUpdate(nullptr, 0, 0);  // 需要正确实现
        
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
```

### 3. RenderQueue_ItemComparator - 排序比较器

**重要性：** 🔴 **高**
**状态：** ❌ 未实现
**依赖：** `RenderQueue_ItemLess`

**必须实现的代码：**
```cpp
extern "C" int RenderQueue_ItemComparator(const void* a, const void* b) {
    return RenderQueue_ItemLess(*(RenderBatchElement**)a, *(RenderBatchElement**)b) != 0 ? -1 : 1;
}

extern "C" bool RenderQueue_ItemLess(
    const RenderBatchElement* a,
    const RenderBatchElement* b
) {
    bool aIsSpecial = ((a->flags & 3) == 3);
    bool bIsSpecial = ((b->flags & 3) == 3);
    
    // 1. Special 优先
    if (aIsSpecial != bIsSpecial) {
        return aIsSpecial;
    }
    
    // 2. hasMoreLayers 分组
    if ((a->flags & 2) && (b->flags & 2)) {
        void* meshDataA = *(void**)((uint8_t*)a->batchEntry + 0x0C);
        void* meshDataB = *(void**)((uint8_t*)b->batchEntry + 0x0C);
        if (meshDataA != meshDataB) return meshDataA < meshDataB;
        
        if (a->layerCounter != b->layerCounter) return a->layerCounter < b->layerCounter;
        
        return memcmp(a->layerStatePtr, b->layerStatePtr, 20) < 0;
    }
    
    // 3. 仅一个有 hasMoreLayers
    if ((a->flags & 2) && !(b->flags & 2)) return true;
    if (!(a->flags & 2) && (b->flags & 2)) return false;
    
    // 4. 都没有 hasMoreLayers
    return memcmp(a->layerStatePtr, b->layerStatePtr, 20) < 0 ||
           (memcmp(a->layerStatePtr, b->layerStatePtr, 20) == 0 && 
            *(void**)((uint8_t*)a->batchEntry + 0x0C) < *(void**)((uint8_t*)b->batchEntry + 0x0C));
}
```

### 4. RenderBatch_CanEnqueueToMainQueue - 透明分流

**重要性：** 🔴 **高**
**状态：** ❌ 未实现
**依赖：** 无

**必须实现的代码：**
```cpp
extern "C" bool RenderBatch_CanEnqueueToMainQueue(
    SceneNode* sceneNode,
    void* part
) {
    void* meshData = *(void**)((uint8_t*)part + 0x0C);
    uint32_t meshIndex = *(uint32_t*)((uint8_t*)meshData + 0x108);
    
    void** meshInfoTable = *(void***)((uint8_t*)sceneNode + 0x30);  // +0x30 需要验证
    void* meshInfo = meshInfoTable[meshIndex];
    
    uint32_t layerCount = *(uint32_t*)((uint8_t*)meshInfo + 0x0C);
    if (layerCount == 0) return true;
    
    void* stateBlockBase = *(void**)((uint8_t*)meshInfo + 0x10);
    void* layerInfo = *(void**)((uint8_t*)meshInfo + 0x38);
    void* layerDataBase = *(void**)((uint8_t*)layerInfo + 0x10);
    
    uint32_t visibilityOffset = *(uint32_t*)((uint8_t*)sceneNode + 0x50);
    
    uint8_t* layerData = (uint8_t*)layerDataBase + 0x1C;
    uint8_t* statePtr = (uint8_t*)stateBlockBase + 4;
    
    for (uint32_t i = 0; i < layerCount; i++) {
        void* layerVisPtr = *(void**)layerData;
        if (layerVisPtr) {
            uint8_t visible = *(uint8_t*)((uint8_t*)layerVisPtr + visibilityOffset);
            if (visible != 0) {
                uint32_t blendMode = *(uint32_t*)(statePtr + 24);
                return blendMode < 2;  // < 2 表示不透明
            }
        }
        layerData += 44;
        statePtr += 36;
    }
    
    return true;
}
```

### 5. AUCTransparent_AddEntry - 透明队列添加

**重要性：** 🔴 **高**
**状态：** ❌ 未实现
**依赖：** 全局变量 `g_RenderCamera_PosXY/PosZ`

**必须实现的代码：**
```cpp
extern "C" void AUCTransparent_AddEntry(
    void* part,
    uint32_t type,
    const float* worldPos,
    uint32_t transparentKey
) {
    // 确保容量
    if (global::g_AUCTransparent_Count >= global::g_AUCTransparent_Capacity) {
        // 需要实现扩容逻辑
        // 暂时跳过
        return;
    }
    
    // 获取数组槽位
    AUCTransparentEntry* entry = (AUCTransparentEntry*)
        ((uint8_t*)global::g_AUCTransparent_Array + 
         global::g_AUCTransparent_Count * 24);
    
    // 计算到相机的距离
    float dx = worldPos[0] - global::g_RenderCamera_PosXY[0];
    float dy = worldPos[1] - global::g_RenderCamera_PosXY[1];
    float dz = worldPos[2] - global::g_RenderCamera_PosZ;
    float distSq = dx*dx + dy*dy + dz*dz;
    
    // 填充条目
    entry->type = type;
    entry->sortKey = transparentKey;
    entry->distSq = distSq;
    entry->payload = part;
    entry->arg1 = 0;
    entry->arg2 = 0;
    
    global::g_AUCTransparent_Count++;
}
```

## 🟡 中等优先级实现

### 6. RenderQueue_StageUpdate - 状态更新

**重要性：** 🟡 中等
**状态：** ❌ 未实现

### 7. RenderQueue_Dispatch_Common/Special - GPU派发

**重要性：** 🟡 中等
**状态：** ❌ 未实现

### 8. SceneNode_AddTransparentList0/2/3/4 - 透明列表处理

**重要性：** 🟡 中等
**状态：** ❌ 未实现

## 📋 实现优先级

### P0 - 立即实现（阻塞功能）

1. **修正 WorldObjectEntry_Render** - 5分钟
   - 修改参数传递：`entry` → `entry->sceneNode`

2. **实现 RenderBatch_Submit** - 30分钟
   - 遍历可渲染列表
   - 透明分流逻辑

3. **实现 RenderQueue_FlushSortedItems** - 45分钟
   - 排序逻辑
   - 循环派发

4. **实现 RenderQueue_ItemComparator/Less** - 20分钟
   - 排序比较器

5. **实现 RenderBatch_CanEnqueueToMainQueue** - 20分钟
   - 检查混合模式

6. **实现 AUCTransparent_AddEntry** - 15分钟
   - 透明队列添加

### P1 - 近期实现（功能完整）

7. **实现 RenderQueue_StageUpdate** - 30分钟
8. **实现 RenderQueue_Dispatch_Common** - 45分钟
9. **实现 RenderQueue_Dispatch_Special** - 30分钟
10. **实现透明列表添加函数** - 60分钟

### P2 - 长期优化（性能提升）

11. **实现 Instancing** - 4小时+
12. **实现多线程渲染** - 8小时+
13. **实现 GPU-Driven Culling** - 6小时+

## 📊 可用性评估

### 当前状态：❌ **不可用**

**原因：**
1. 存在严重的参数传递错误（WorldObjectEntry_Render）
2. 缺少核心函数实现（RenderBatch_Submit, RenderQueue_FlushSortedItems等）
3. SceneNode 结构不完整
4. 缺少透明队列处理

### 修正后可用性：🟡 **基本可用**

**条件：**
1. 修正 WorldObjectEntry_Render 错误 ✅
2. 实现 P0 所有函数 ✅
3. 补充 SceneNode 中间偏移 ✅

**预期结果：**
- 可以渲染基本场景（地形、单位、建筑）
- 透明对象可能有问题
- 性能与原版相似

### 完整实现后：🟢 **生产可用**

**条件：**
1. 实现 P0 + P1 所有函数 ✅
2. 通过所有回归测试 ✅
3. 性能验证通过 ✅

**预期结果：**
- 完整的渲染功能
- 性能与原版相当
- 可以进行优化

## 🎯 建议行动计划

### 第1天：紧急修复（3小时）
1. 修正 WorldObjectEntry_Render 错误（5分钟）
2. 实现 RenderBatch_Submit（30分钟）
3. 实现 RenderQueue_FlushSortedItems（45分钟）
4. 实现 RenderQueue_ItemComparator/Less（20分钟）
5. 实现 RenderBatch_CanEnqueueToMainQueue（20分钟）
6. 实现 AUCTransparent_AddEntry（15分钟）
7. 编译测试（30分钟）

### 第2-3天：功能完善（8小时）
1. 实现 RenderQueue_StageUpdate（30分钟）
2. 实现 RenderQueue_Dispatch_Common（45分钟）
3. 实现 RenderQueue_Dispatch_Special（30分钟）
4. 实现透明列表添加函数（60分钟）
5. 调试和测试（4小时）
6. 性能测试（2小时）

### 第4-7天：优化和验证（24小时）
1. 回归测试（8小时）
2. 性能分析（4小时）
3. 优化实现（8小时）
4. 文档更新（4小时）

## 📝 总结

### 当前问题
- ❌ 存在严重的参数传递错误
- ❌ 缺少5个关键函数实现
- ❌ SceneNode 结构不完整

### 必须修正的错误
1. WorldObjectEntry_Render 传递 `entry->sceneNode`
2. SceneNode 补充所有中间偏移
3. 实现所有 P0 优先级函数

### 预估工作量
- **紧急修复：** 3小时
- **功能完善：** 8小时
- **优化验证：** 24小时
- **总计：** 35小时（约1周）

**结论：** 当前实现**不可用**，需要至少完成 P0 优先级的所有函数才能基本运行。