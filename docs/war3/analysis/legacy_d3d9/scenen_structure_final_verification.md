# SceneNode 结构最终验证 - 汇编代码确认

## 汇编代码对比分析

### RenderBatch_Submit (0x6F1375C0)

```asm
mov     ebx, ecx                      ; ebx = this (SceneNode*)
cmp     dword ptr [ebx+0Ch], 0        ; 检查 [ebx+0x0C]
mov     edi, [ebx+10h]                ; edi = [ebx+0x10]
```

**确认的偏移：**
- `+0x0C` (12): `renderableCount`
- `+0x10` (16): `renderableList`

### RenderQueue_AddBatch (0x6F139190)

```asm
mov     esi, ecx                      ; esi = this (SceneNode*)
mov     edi, [esi+9Ch]                ; edi = [esi+0x9C]
call    RenderBatch_Submit
test    byte ptr [esi+94h], 10h       ; 检查 [esi+0x94] 的 bit 4
```

**确认的偏移：**
- `+0x9C` (156): `childTable`
- `+0x94` (148): `flags`

**透明列表处理：**
```asm
call    SceneNode_AddTransparentList0    ; List0: 粒子发射器
call    SceneNode_AddTransparentList2    ; List2: 缎带发射器
call    SceneNode_AddTransparentList3    ; List3: 特效
call    SceneNode_AddTransparentList4    ; List4: 附着物
```

**子节点递归：**
```asm
mov     ecx, [esi+0C8h]               ; [esi+0xC8]
xor     ebx, ebx
cmp     [esi+0C4h], ebx               ; 检查 [esi+0xC4]
add     ecx, 8                         ; ecx = [esi+0xD0]
```

**确认的偏移：**
- `+0xC8` (200): `childVisFlags`
- `+0xC4` (196): `childCount`

**循环中的偏移：**
```asm
mov     edi, [esi+98h]                ; [esi+0x98]
test    edi, edi
mov     eax, [esi+0D4h]                ; [esi+0xD4]
mov     al, [ebx+eax]
test    al, 1
movzx   eax, al
and     eax, 2
```

**确认的偏移：**
- `+0x98` (152): 未知字段（可能是childPtrArray）
- `+0xD4` (212): `childVisibilityArray`

## SceneNode 完整结构

### 低偏移部分（0x00 ~ 0x20）- RenderBatch_Submit 使用

```c
struct SceneNode {
    void* vtable;                      // +0x00
    
    // ... 未知字段 (0x04 ~ 0x0B) ...
    
    uint32_t renderableCount;          // +0x0C: 可渲染对象数量
    void**   renderableList;           // +0x10: 可渲染对象列表（指针数组）
    
    // ... 未知字段 (0x14 ~ 0x1F) ...
    
    void*    sceneNode;                // +0x20: 在WorldObjectEntry中访问
};
```

### 中偏移部分（0x20 ~ 0x90）- 未知

可能包含：
- `cullTable` @ +0x20
- `meshInfoTable` @ +0x30
- `visibilityOffset` @ +0x50
- `worldMatrix[12]` @ +0x64

### 高偏移部分（0x90 ~ 0xE0）- RenderQueue_AddBatch 使用

```c
    // ... 未知字段 (0x90 ~ 0x93) ...
    
    uint32_t flags;                   // +0x94: 标志位 (bit4=0x10 影响透明处理)
    
    // ... 未知字段 (0x95 ~ 0x97) ...
    
    void*    childPtrArray;            // +0x98: 子节点指针数组
    
    // ... 未知字段 (0x9C ~ 0x9B) ...
    
    void*    childTable;              // +0x9C: 子节点表
    
    // ... 未知字段 (0xA0 ~ 0xC3) ...
    
    uint32_t childCount;              // +0xC4: 子节点数量
    void*    childVisFlags;           // +0xC8: 子节点可见性标志
    
    // ... 未知字段 (0xCC ~ 0xD3) ...
    
    void*    childVisibilityArray;      // +0xD4: 子节点可见性数组
    
    // ... 其他字段 ...
};
```

## 偏移汇总表

| 偏移 (十六进制) | 偏移 (十进制) | 名称 | 类型 | 来源 |
|-----------------|----------------|------|------|------|
| 0x00 | 0 | vtable | void* | 通用 |
| 0x0C | 12 | renderableCount | uint32_t | RenderBatch_Submit |
| 0x10 | 16 | renderableList | void** | RenderBatch_Submit |
| 0x14 | 20 | sceneNode | void* | WorldObjectEntry_Render |
| 0x20 | 32 | cullTable | void* | 文档推测 |
| 0x30 | 48 | meshInfoTable | void** | 文档推测 |
| 0x50 | 80 | visibilityOffset | uint32_t | 文档推测 |
| 0x64 | 100 | worldMatrix[12] | float[12] | 文档推测 |
| 0x94 | 148 | flags | uint32_t | RenderQueue_AddBatch |
| 0x98 | 152 | childPtrArray | void* | RenderQueue_AddBatch |
| 0x9C | 156 | childTable | void* | RenderQueue_AddBatch |
| 0xC4 | 196 | childCount | uint32_t | RenderQueue_AddBatch |
| 0xC8 | 200 | childVisFlags | void* | RenderQueue_AddBatch |
| 0xD4 | 212 | childVisibilityArray | void* | RenderQueue_AddBatch |

## 透明列表偏移（来自文档）

| 偏移 | 名称 | 类型 | Stride | 说明 |
|------|------|------|--------|------|
| 0xA8 | 168 | list4Count | uint32_t | List4 数量 |
| 0xAC | 172 | list4Data | void* | 36 | 附着物 |
| 0xDC | 220 | list0Count | uint32_t | List0 数量 |
| 0xE0 | 224 | list0Data | void* | 104 | 粒子发射器 |
| 0xE8 | 232 | list2Count | uint32_t | List2 数量 |
| 0xEC | 236 | list2Ptrs | void** | 指针数组 | 缎带发射器 |
| 0xF4 | 244 | list3Count | uint32_t | List3 数量 |
| 0xF8 | 248 | list3Data | void* | 356 | 特效 |

## 关键发现总结

### 1. RenderBatch_Submit 的作用

```asm
mov     ebx, ecx                      ; ebx = SceneNode*
cmp     dword ptr [ebx+0Ch], 0        ; 检查 renderableCount
mov     edi, [ebx+10h]                ; edi = renderableList
```

**功能：**
- 遍历 `SceneNode->renderableList`
- 每个 `RenderablePart` 检查剔除标志
- 按材质层拆分为 `RenderBatch`
- 不透明对象 → 主队列
- 透明对象 → AUCTransparent 队列

**访问的偏移：**
- `+0x0C`: renderableCount
- `+0x10`: renderableList

### 2. RenderQueue_AddBatch 的作用

```asm
mov     edi, [esi+9Ch]                ; edi = childTable
call    RenderBatch_Submit
test    byte ptr [esi+94h], 10h       ; 检查 flags bit 4
```

**功能：**
1. 调用 `RenderBatch_Submit` 处理主节点
2. 如果 `flags & 0x10`，处理透明列表
3. 递归处理子节点

**访问的偏移：**
- `+0x94`: flags
- `+0x98`: childPtrArray
- `+0x9C`: childTable
- `+0xC4`: childCount
- `+0xC8`: childVisFlags
- `+0xD4`: childVisibilityArray

### 3. 子节点递归逻辑

```asm
mov     ecx, [esi+0C8h]               ; ecx = childVisFlags
xor     ebx, ebx
cmp     [esi+0C4h], ebx               ; 检查 childCount
add     ecx, 8                         ; ecx = childVisFlags + 8
```

**循环逻辑：**
1. 从 `[esi+0xC4]` 获取 childCount
2. 从 `[esi+0xC8]` 获取 childVisFlags
3. 从 `[esi+0x98]` 获取 childPtrArray
4. 从 `[esi+0xD4]` 获取 childVisibilityArray

**循环中：**
```asm
mov     edi, [esi+98h]                ; edi = childPtrArray
mov     eax, [esi+0D4h]                ; eax = childVisibilityArray
mov     al, [ebx+eax]                 ; al = childVisibilityArray[ebx]
test    al, 1                          ; 检查 bit 0
movzx   eax, al
and     eax, 2                          ; 检查 bit 1
```

**含义：**
- `childVisibilityArray[ebx] & 1`: 是否可见
- `childVisibilityArray[ebx] & 2`: 是否需要递归

## 修正后的完整实现

### WorldObjectEntry_Render

```cpp
int __cdecl Native_WorldObjectEntry_Render(WorldObjectEntry* entry, int categoryMode) {
    // 汇编: mov ecx, [esi+20h]; jmp RenderQueue_AddBatch
    void* sceneNode = *(void**)((uint8_t*)entry + 0x20);
    
    if (sceneNode) {
        // 调用 vtable[5] (PreRender)
        void** vtable = *(void**)entry;
        if (vtable && vtable[5]) {
            typedef void (__thiscall* PreRenderFunc)(void*);
            PreRenderFunc preRender = (PreRenderFunc)vtable[5];
            preRender(entry);
        }
        
        // 传递 sceneNode 给 RenderQueue_AddBatch
        return (int)RenderQueue_AddBatch(sceneNode, categoryMode);
    }
    
    return 0;
}
```

### RenderQueue_AddBatch

```cpp
void __thiscall Native_RenderQueue_AddBatch(SceneNode* node, int categoryMode) {
    // 汇编: mov edi, [esi+9Ch]; call RenderBatch_Submit
    void* childTable = *(void**)((uint8_t*)node + 0x9C);
    
    // 调用 RenderBatch_Submit
    RenderBatch_Submit(node);
    
    // 汇编: test byte ptr [esi+94h], 10h
    uint32_t flags = *(uint32_t*)((uint8_t*)node + 0x94);
    
    if (flags & 0x10) {
        // 处理透明列表
        void* unknownParam = childTable;  // 使用相同的参数
        
        // List0: 粒子发射器
        SceneNode_AddTransparentList0(node, unknownParam);
        
        // List2: 缎带发射器
        SceneNode_AddTransparentList2(node, unknownParam);
        
        // List3: 特效
        SceneNode_AddTransparentList3(node, unknownParam);
        
        // List4: 附着物
        SceneNode_AddTransparentList4(node);
        
        // 子节点递归
        // 汇编: mov ecx, [esi+0C8h]; add ecx, 8
        void* childVisFlags = *(void**)((uint8_t*)node + 0xC8);
        uint32_t childVisFlagsOffset = (uint32_t)((uint8_t*)childVisFlags + 8);
        
        // 汇编: cmp [esi+0C4h], ebx
        uint32_t childCount = *(uint32_t*)((uint8_t*)node + 0xC4);
        
        if (childCount > 0) {
            // 汇编: mov edi, [esi+98h]
            void* childPtrArray = *(void**)((uint8_t*)node + 0x98);
            
            // 汇编: mov eax, [esi+0D4h]
            void* childVisibilityArray = *(void**)((uint8_t*)node + 0xD4);
            
            for (uint32_t i = 0; i < childCount; i++) {
                // 汇编: mov al, [ebx+eax]; test al, 1
                uint8_t* visibility = (uint8_t*)childVisibilityArray + i;
                
                if (*visibility & 1) {
                    // 子节点可见
                    SceneNode* child = *(SceneNode**)((uint8_t*)childPtrArray + i * 4);
                    
                    // 汇编: and eax, 2
                    if (*visibility & 2) {
                        // 需要递归
                        RenderQueue_AddBatch(child, categoryMode);
                    } else {
                        // 不需要递归，但可能需要其他处理
                        // 这里需要进一步分析
                    }
                }
            }
        }
    }
}
```

### RenderBatch_Submit

```cpp
void __thiscall Native_RenderBatch_Submit(SceneNode* node) {
    // 汇编: cmp dword ptr [ebx+0Ch], 0
    uint32_t renderableCount = *(uint32_t*)((uint8_t*)node + 0x0C);
    
    if (renderableCount == 0) return;
    
    // 汇编: mov edi, [ebx+10h]
    void** renderableList = *(void***)((uint8_t*)node + 0x10);
    
    for (uint32_t i = 0; i < renderableCount; i++) {
        RenderablePart* part = renderableList[i];
        
        if (!part) continue;
        
        // 汇编: cmp dword ptr [edi+10h], 0
        uint32_t skipFlag = *(uint32_t*)((uint8_t*)part + 0x10);
        
        if (skipFlag != 0) continue;
        
        // 写回 sceneNode 指针
        // 汇编: mov [edi+14h], ebx
        *(void**)((uint8_t*)part + 0x14) = node;
        
        // 检查是否可以加入主队列
        if (RenderBatch_CanEnqueueToMainQueue(node, part)) {
            // 不透明对象 → 主队列
            AddToMainQueue(part);
        } else {
            // 透明对象 → AUCTransparent 队列
            AddToTransparentQueue(part);
        }
    }
}
```

## 结论

通过汇编代码验证，我们确认了：

### ✅ 已确认
1. `WorldObjectEntry_Render` 传递 `entry->sceneNode`（+0x20）
2. `RenderQueue_AddBatch` 接收 `SceneNode*` 参数
3. `RenderBatch_Submit` 使用低偏移（+0x0C, +0x10）
4. `RenderQueue_AddBatch` 使用高偏移（+0x94, +0x98, +0x9C, +0xC4, +0xC8, +0xD4）
5. SceneNode 结构确实非常大（>220 bytes）

### ⚠️ 需要进一步验证
1. SceneNode 的完整中间部分（0x20 ~ 0x90）
2. 透明列表的具体偏移（0xA8 ~ 0xF8）
3. 子节点递归的完整逻辑

### 📋 下一步
1. 使用IDA分析 SceneNode 的完整内存布局
2. 验证所有透明列表的偏移
3. 完成所有关键函数的实现
4. 运行时测试验证

**关键成功因素：**
- 汇编代码是最终真理
- IDA 反编译在尾调用优化场景下有错误
- 偏移访问基于字节地址，不是 dword 索引