# IDA 汇编验证结果 - 关键发现

## WorldObjectEntry_Render 的真实汇编

```asm
WorldObjectEntry_Render @ 0x6f184ee0:
push    esi
mov     esi, ecx                  ; esi = arg0 (第一个参数)
cmp     dword ptr [esi+20h], 0    ; 检查 [esi+0x20]
jz      short loc_6F184EF7
mov     eax, [esi]                ; eax = [esi] = vtable
call    dword ptr [eax+14h]        ; 调用 vtable[5] (0x14 = 5*4)
mov     ecx, [esi+20h]            ; 【关键】ecx = [esi+0x20] = sceneNode
pop     esi
jmp     RenderQueue_AddBatch        ; 跳转到 RenderQueue_AddBatch
loc_6F184EF7:
pop     esi
retn
```

## 关键发现

### 1. IDA 反编译的错误

**IDA 反编译显示：**
```c
return RenderQueue_AddBatch(a1, a2);  // 传递 a1
```

**实际汇编代码：**
```asm
mov     ecx, [esi+20h]  ; ecx = [esi+0x20]
jmp     RenderQueue_AddBatch
```

**结论：** IDA反编译错误！实际传递的是 `[esi+0x20]`（sceneNode），不是 `a1`（WorldObjectEntry）。

### 2. 参数传递的真实流程

```
WorldObjectEntry_Render(esi, edx)  // esi = WorldObjectEntry*
    │
    ├─ 检查 [esi+0x20] (sceneNode) 是否为空
    │
    ├─ 调用 [esi]->vtable[5](esi)  // PreRender
    │
    └─ mov ecx, [esi+0x20]       // ecx = sceneNode*
         │
         └─ jmp RenderQueue_AddBatch
              // 参数1 (ecx) = sceneNode*
              // 参数2 (edx) = 保持不变 (categoryMode)
```

### 3. 类型确认

**WorldObjectEntry 结构：**
```c
struct WorldObjectEntry {
    void* vtable;           // +0x00
    // ... 未知字段 (7个dword = 28 bytes)
    void* sceneNode;         // +0x20 (第8个dword)
    // ... 其他字段
};
```

**RenderQueue_AddBatch 签名确认：**
```c
void __thiscall RenderQueue_AddBatch(SceneNode* this, int categoryMode)
    // this (ecx) = WorldObjectEntry->sceneNode
    // categoryMode (edx) = 从WorldObjectEntry_Render传递
```

## 为什么IDA反编译会出错？

可能的原因：

### 原因1: 尾调用优化（Tail Call Optimization）

```asm
mov     ecx, [esi+20h]
pop     esi
jmp     RenderQueue_AddBatch  ; 不是call，是jmp
```

- 使用`jmp`而不是`call`表示尾调用优化
- IDA可能没有正确识别这种模式
- 反编译器可能直接传递了原始参数`a1`

### 原因2: 寄存器使用混淆

- `ecx`在`__thiscall`约定中是`this`指针
- 汇编代码在跳转前修改了`ecx`
- IDA的反编译器可能没有跟踪到这个修改

### 原因3: 函数签名推断错误

- IDA可能推断`RenderQueue_AddBatch`接收`WorldObjectEntry*`
- 但实际它接收`SceneNode*`
- 导致反编译时参数类型错误

## 验证其他关键函数

### RenderQueue_AddBatch 的参数

基于以上发现，`RenderQueue_AddBatch`确实接收`SceneNode*`：

```c
void __thiscall RenderQueue_AddBatch(SceneNode* this, int categoryMode)
{
    v2 = *(_DWORD *)(this + 156);  // SceneNode->childTable
    RenderBatch_Submit((_DWORD *)this);  // 传递SceneNode*
    
    if ( (*(_BYTE *)(this + 148) & 0x10) != 0 ) {  // SceneNode->flags
        // 透明列表处理
        SceneNode_AddTransparentList0(this, v2);
        // ...
    }
}
```

### WorldObjects_RenderGroup 的调用

```c
result = WorldObjectEntry_Render(v7, v8);
```

**参数解释：**
- `v7` = `WorldObjectEntry*`（指针）
- `v8` = `categoryMode`（整数）

**调用流程：**
```
WorldObjects_RenderGroup
    └─ 遍历WorldObjectListEntry数组
         └─ 每个entry调用 WorldObjectEntry_Render(entry, mode)
              └─ entry->vtable[5](entry)  // PreRender
              └─ RenderQueue_AddBatch(entry->sceneNode, mode)
                   └─ 提交SceneNode的所有渲染批次
```

## 修正后的理解

### 1. WorldObjectListEntry 的结构

根据`WorldObjects_RenderGroup`的遍历逻辑（stride=24）：

```c
struct WorldObjectListEntry {
    WorldObjectEntry* entry;  // +0x00: 指向WorldObjectEntry
    // +0x04 ~ +0x17: 其他字段（20 bytes）
    // total: 24 bytes
};
```

### 2. 调用链路确认

```
WorldObjects_RenderGroup(CWorld* world, int groupIdx)
    │
    ├─ 获取WorldObjectListEntry数组
    │
    └─ for each entry:
         └─ WorldObjectEntry_Render(entry->objectEntry, categoryMode)
              │
              ├─ if (entry->sceneNode):
              │    └─ entry->vtable[5](entry)  // PreRender
              │
              └─ RenderQueue_AddBatch(entry->sceneNode, categoryMode)
                   │
                   └─ RenderBatch_Submit(sceneNode)
                        │
                        └─ 遍历SceneNode的可渲染部件
                             │
                             └─ 按材质层拆分为RenderBatch
                                  │
                                  └─ 添加到主队列或透明队列
```

### 3. 类型关系图

```
CWorldObjects (vtable @ 0x6FA59AC8, 103个方法)
    │
    ├─ vtable[0]: 析构函数
    ├─ vtable[1-4]: 未知方法
    ├─ vtable[5]: PreRender
    │    └─ 更新动画、状态
    ├─ vtable[...]: 其他渲染相关方法
    └─ 包含 sceneNode 指针 @ +0x20
         │
         └─ SceneNode
              │
              ├─ +0x04: renderableList
              ├─ +0x0C: renderableCount
              ├─ +0x10: renderableList (重复？)
              ├─ +0x20: cullTable
              ├─ +0x64: worldMatrix (3x4矩阵)
              ├─ +0x94: flags
              ├─ +0x148: flags (重复？)
              ├─ +0x156: childTable
              ├─ +0x196: childCount
              └─ +0x200: childVisFlags
```

## 偏移矛盾的解决

### 问题重现

**RenderBatch_Submit 访问：**
```c
v2 = (_DWORD *)*(this + 4);  // this[4] = SceneNode+0x10
if ( !v3[4] )  // v3[4] = SceneNode+0x10
```

**RenderQueue_AddBatch 访问：**
```c
v2 = *(_DWORD *)(this + 156);  // this[156] = SceneNode+0x270
if ( (*(_BYTE *)(this + 148) & 0x10)  // this[148] = SceneNode+0x254
```

### 可能的解释

**假设1: SceneNode 结构非常大**

SceneNode 可能是一个包含大量字段的结构：
```c
struct SceneNode {
    // 基础部分 (0x00 ~ 0x14)
    void* vtable;
    void* renderableList;     // +0x04
    // ...
    
    // 中间部分 (0x14 ~ 0x250)
    // 大量未知字段...
    
    // 子节点管理部分 (0x250 ~ 0x330)
    uint32_t flags;         // +0x254 (偏移148)
    void* childTable;        // +0x270 (偏移156)
    // ...
};
```

**假设2: 使用了不同的基类**

RenderBatch_Submit 和 RenderQueue_AddBatch 可能接收不同的基类：
- `RenderBatch_Submit(SceneNode*)` - 基础SceneNode
- `RenderQueue_AddBatch(ExtendedSceneNode*)` - 扩展SceneNode

但汇编代码显示它们传递的是同一个指针`this`！

**假设3: IDA偏移计算错误（最可能）**

IDA的反编译可能使用了错误的偏移计算方式。让我检查原始的反编译：

```c
v2 = *(_DWORD *)(this + 156);  // 这是 dword 指针算术
// 实际访问: this + 156 * 4 = this + 624 = this + 0x270
```

如果改为字节偏移：
```c
v2 = *(_DWORD *)((uint8_t*)this + 0x254);  // this + 0x254
```

这更合理！

### 最终解释

**SceneNode 的真实偏移：**
```c
struct SceneNode {
    void* vtable;           // +0x00
    
    // ... 未知字段 ...
    
    void* renderableList;     // +0x10 (第4个dword)
    uint32_t renderableCount; // +0x0C (第3个dword)
    
    // ... 大量未知字段 ...
    
    uint32_t flags;         // +0x254
    void* childTable;        // +0x270
    uint32_t childCount;     // +0x310
    void* childVisFlags;     // +0x320
    
    // ... 其他字段 ...
    void* sceneNode;         // +0x20 (在WorldObjectEntry中)
};
```

**关键：**
- `RenderBatch_Submit` 访问低偏移（0x00 ~ 0x20）
- `RenderQueue_AddBatch` 访问高偏移（0x250 ~ 0x330）
- **这是同一个结构体，只是很大！**

## 修正后的实现

### WorldObjectEntry_Render

```cpp
int __cdecl Native_WorldObjectEntry_Render(WorldObjectEntry* entry, int categoryMode) {
    void* sceneNode = entry->sceneNode;  // +0x20
    
    if (sceneNode) {
        // 调用 vtable[5] (PreRender)
        void** vtable = entry->vtable;
        if (vtable) {
            typedef void (__thiscall* PreRenderFunc)(void*);
            PreRenderFunc preRender = (PreRenderFunc)vtable[5];
            preRender(entry);
        }
        
        // 调用 RenderQueue_AddBatch，传递 sceneNode
        return (int)RenderQueue_AddBatch(sceneNode, categoryMode);
    }
    
    return 0;
}
```

### RenderQueue_AddBatch

```cpp
void __thiscall Native_RenderQueue_AddBatch(SceneNode* node, int categoryMode) {
    void* childTable = *(void**)((uint8_t*)node + 0x270);  // +156 * 4
    RenderBatch_Submit(node);
    
    uint32_t flags = *(uint32_t*)((uint8_t*)node + 0x254);  // +148 * 4
    
    if (flags & 0x10) {
        // 透明列表处理
        // ...
        
        // 子节点递归
        uint32_t childCount = *(uint32_t*)((uint8_t*)node + 0x310);  // +196 * 4
        void* childVisFlags = *(void**)((uint8_t*)node + 0x320);  // +200 * 4
        
        for (uint32_t i = 0; i < childCount; i++) {
            if (CheckChildVisibility(childVisFlags, i)) {
                SceneNode* child = GetChildNode(childTable, i);
                RenderQueue_AddBatch(child, categoryMode);
            }
        }
    }
}
```

## 结论

通过汇编代码验证，我们确认了：

1. ✅ `WorldObjectEntry_Render` 传递的是 `entry->sceneNode`，不是 `entry` 本身
2. ✅ `RenderQueue_AddBatch` 接收 `SceneNode*` 参数
3. ✅ IDA 反编译在尾调用优化场景下有错误
4. ⚠️ SceneNode 结构可能非常大（>800 bytes），包含大量字段
5. ⚠️ 需要进一步验证 SceneNode 的完整结构

**下一步：**
- 使用IDA分析SceneNode的具体内存布局
- 验证所有关键偏移的字段含义
- 基于确认的结构完成最终实现