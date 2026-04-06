# 魔兽争霸3渲染链调用约定文档

## 文档版本
**生成时间**: 2026-01-25  
**游戏版本**: Game.dll 1.27.x (32位)  
**核验工具**: IDA Pro MCP  

---

## 重要说明

### 调用约定分类

1. **__thiscall** - C++成员函数，this指针通过ECX传递
2. **__cdecl** - C函数，调用者清理堆栈
3. **__fastcall** - 前两个参数通过ECX/EDX传递
4. **__usercall** - IDA自定义，参数通过特定寄存器传递
5. **__userpurge** - IDA自定义，返回前清理堆栈

### 关键点
- **32位x86架构**: 参数通过堆栈传递（寄存器约定的除外）
- **堆栈方向**: 从高地址向低地址增长
- **返回值**: 通过EAX寄存器返回
- **结构体**: 小结构通过EAX/EDX返回，大结构通过指针

---

## 核心函数调用约定表

### 1. CWorld_RenderScene (0x6F3681C0)
```cpp
int __thiscall CWorld_RenderScene(int* this)
```

**调用约定**: `__thiscall`  
**参数**:
- `this` (ECX): CWorld实例指针

**返回值**: int (通常为0)

**说明**: 主渲染入口，协调所有22个渲染阶段

---

### 2. RenderWorld_DispatchStage (0x6F363020)
```cpp
int __thiscall RenderWorld_DispatchStage(
    _DWORD* this,
    int a2,   // stageId
    int a3,   // categoryMode
    int a4,   // renderCategory
    int a5    // force flag
)
```

**调用约定**: `__thiscall`  
**参数**:
- `this` (ECX): CWorld实例指针
- `a2` (堆栈): stageId (0-21)
- `a3` (堆栈): categoryMode (传递给WorldObjectEntry_Render)
- `a4` (堆栈): renderCategory (world[409])
- `a5` (堆栈): force flag (非0时强制a3=3)

**返回值**: int (特定操作的返回值)

**说明**: 根据stageId分发到不同的渲染函数

---

### 3. WorldObjects_RenderGroup (0x6F368E30)
```cpp
int __userpurge WorldObjects_RenderGroup(
    _DWORD* a1,  // ECX: world pointer
    int a2,       // EDI: categoryMode
    int a3        // ESP+4: groupIdx (0/1/2)
)
```

**调用约定**: `__userpurge`  
**参数**:
- `a1` (ECX): world指针
- `a2` (EDI): categoryMode (传递给WorldObjectEntry_Render)
- `a3` (堆栈): groupIdx (0=Units, 1=Buildings, 2=Effects)

**返回值**: int (最后一个WorldObjectEntry_Render的返回值)

**说明**: 渲染指定分组的世界对象

---

### 4. WorldObjectEntry_Render (0x6F184EE0) ⚠️ **关键修正**
```cpp
int __cdecl WorldObjectEntry_Render(
    int a1,      // WorldObjectEntry指针（作为int传递）
    int a2       // categoryMode
)
```

**调用约定**: `__cdecl`  
**参数**:
- `a1` (堆栈): WorldObjectEntry指针（**注意：是int类型，不是int***）
- `a2` (堆栈): categoryMode

**返回值**: int (RenderQueue_AddBatch的返回值)

**说明**: 
1. 检查sceneNode是否存在 (entry[8])
2. 调用vtable[5] (PreRender)
3. **调用RenderQueue_AddBatch(a1, a2)** - 传递entry指针，不是sceneNode！

**修正点**:
- ❌ 错误实现: `int WorldObjectEntry_Render(int* entry, int categoryMode)`
- ✅ 正确实现: `int WorldObjectEntry_Render(int entry, int categoryMode)`
- ❌ 错误调用: `RenderQueue_AddBatch((void*)entry[8], categoryMode)`
- ✅ 正确调用: `RenderQueue_AddBatch((void*)entry, categoryMode)`

---

### 5. RenderQueue_AddBatch (0x6F139190) ⚠️ **关键修正**
```cpp
void __thiscall RenderQueue_AddBatch(int this)
```

**调用约定**: `__thiscall`  
**参数**:
- `this` (ECX): sceneNode指针（**注意：只有一个参数！**）

**返回值**: void

**说明**:
1. 提取v2 = *(this + 156)
2. 调用RenderBatch_Submit(this)
3. 检查flags (this + 148) & 0x10
4. 如果有透明标志，调用SceneNode_AddTransparentList0/2/3/4
5. 处理子节点遍历

**修正点**:
- ❌ 错误实现: `void Native_RenderQueue_AddBatch(SceneNode* sceneNode, int categoryMode)`
- ✅ 正确实现: `void RenderQueue_AddBatch(int sceneNode)`
- ❌ 错误: 有额外的categoryMode参数
- ✅ 正确: 只有一个参数（sceneNode指针）

**参数传递**:
- WorldObjectEntry_Render调用时: `RenderQueue_AddBatch(a1, a2)`
- 这里a1是WorldObjectEntry的指针值（int类型）
- RenderQueue_AddBatch将其作为this指针（sceneNode）

---

### 6. RenderBatch_Submit (0x6F1375C0)
```cpp
void __thiscall RenderBatch_Submit(_DWORD* this)
```

**调用约定**: `__thiscall`  
**参数**:
- `this` (ECX): sceneNode指针

**返回值**: void

**说明**: 
- 遍历sceneNode的所有可渲染对象
- 调用RenderBatch_CanEnqueueToMainQueue判断透明/不透明
- 不透明对象添加到主队列
- 透明对象调用AUCTransparent_AddEntry

---

### 7. RenderQueue_FlushSortedItems (0x6F1380A0) ⚠️ **关键修正**
```cpp
unsigned int __usercall RenderQueue_FlushSortedItems(
    int a1,  // EDI: unknown context
    int a2   // ESI: last applied state pointer
)
```

**调用约定**: `__usercall`  
**参数**:
- `a1` (EDI): unknown context (传递给StageUpdate)
- `a2` (ESI): 上一个应用的状态块指针 (用于memcmp优化)

**返回值**: unsigned int (处理的批次数量)

**说明**:
1. 排序所有批次
2. 应用初始状态块
3. 遍历并派发所有批次
4. 调用尾部清理

**关键寄存器**:
- **ESI**: 上一个应用的状态块指针 - 用于状态优化判断
- **EDI**: 上下文参数 - 传递给StageUpdate

**修正点**:
- ❌ 错误实现: 使用了不正确的参数模型
- ✅ 正确实现: 必须捕获ESI/EDI寄存器

---

### 8. RenderQueue_FlushAndReset (0x6F139800) ⚠️ **关键修正**
```cpp
int __usercall RenderQueue_FlushAndReset(
    int a1,  // EDI: queue context
    int a2   // ESI: last applied state pointer
)
```

**调用约定**: `__usercall`  
**参数**:
- `a1` (EDI): queue context (传递给FlushSortedItems)
- `a2` (ESI): last applied state pointer (传递给FlushSortedItems)

**返回值**: int

**说明**:
1. 调用RenderQueue_StageUpdate(1)
2. 调用RenderQueue_FlushSortedItems(a1, a2)
3. 刷新透明队列
4. 调用RenderQueue_StageUpdate(1)
5. 重置计数器

**关键寄存器**:
- **ESI**: 从调用者传递的状态块指针
- **EDI**: queue上下文

**修正点**:
- ❌ 错误实现: 使用了不正确的参数模型
- ✅ 正确实现: 必须捕获ESI/EDI寄存器

---

### 9. RenderQueue_ItemComparator (0x6F1378B0)
```cpp
int __cdecl RenderQueue_ItemComparator(
    _DWORD* a1,  // 指向RenderBatchElement**的指针
    _DWORD* a2   // 指向RenderBatchElement**的指针
)
```

**调用约定**: `__cdecl`  
**参数**:
- `a1` (堆栈): 指向RenderBatchElement**的指针
- `a2` (堆栈): 指向RenderBatchElement**的指针

**返回值**: int (-1或1，从不返回0)

**说明**: qsort比较函数，调用RenderQueue_ItemLess

**注意**: 
- a1/a2是指向指针的指针
- 需要解引用两次: **a1 和 **a2
- 返回值永远是-1或1，从不返回0

---

### 10. RenderQueue_ItemLess (0x6F137D50)
```cpp
int __fastcall RenderQueue_ItemLess(
    _DWORD* a1,  // ECX: RenderBatchElement*
    _DWORD* a2   // EDX: RenderBatchElement*
)
```

**调用约定**: `__fastcall`  
**参数**:
- `a1` (ECX): RenderBatchElement*
- `a2` (EDX): RenderBatchElement*

**返回值**: int (排序结果)

**说明**: 实际的比较逻辑，实现复杂的多级排序

**排序优先级**:
1. Special类型优先 (flags & 3 == 3)
2. hasMoreLayers分组 (flags & 2)
3. meshData指针
4. layerCounter
5. layerStatePtr内容 (20字节memcmp)

---

## 辅助函数调用约定

### AUCTransparent_AddEntry (0x6F137AF0)
```cpp
void __fastcall AUCTransparent_AddEntry(
    void* part,      // ECX: 可渲染部件
    uint32_t type,   // EDX: 透明类型码
    const float* worldPos,  // ESP+4: 世界坐标
    uint32_t transparentKey  // ESP+8: 透明排序键
)
```

**调用约定**: `__fastcall`  
**参数**:
- `part` (ECX): 可渲染部件
- `type` (EDX): 透明类型码
- `worldPos` (堆栈): 世界坐标 [3]
- `transparentKey` (堆栈): 透明排序键

**返回值**: void

---

### RenderBatch_CanEnqueueToMainQueue (0x6F1387E0)
```cpp
bool __fastcall RenderBatch_CanEnqueueToMainQueue(
    SceneNode* sceneNode,  // ECX
    void* part            // EDX
)
```

**调用约定**: `__fastcall`  
**参数**:
- `sceneNode` (ECX): 场景节点
- `part` (EDX): 可渲染部件

**返回值**: bool (true=不透明, false=透明)

**说明**: 检查所有层的混合模式，blendMode < 2表示不透明

---

### RenderQueue_StageUpdate (0x6F13A9B0)
```cpp
void __thiscall RenderQueue_StageUpdate(int this, int a2)
```

**调用约定**: `__thiscall`  
**参数**:
- `this` (ECX): context (0或1)
- `a2` (堆栈): 未知参数

**返回值**: void

**说明**: 更新渲染阶段状态

---

### RenderQueue_Dispatch_Common (0x6F13A5E0)
```cpp
int __fastcall RenderQueue_Dispatch_Common(
    void* meshData,       // ECX
    void* renderablePart, // EDX
    int layerIndex,       // ESP+4
    int layerChanged,     // ESP+8
    int stateChanged      // ESP+12
)
```

**调用约定**: `__fastcall`  
**参数**:
- `meshData` (ECX): 网格数据
- `renderablePart` (EDX): 可渲染部件
- `layerIndex` (堆栈): 层索引
- `layerChanged` (堆栈): 层状态是否改变
- `stateChanged` (堆栈): 状态是否改变

**返回值**: int

---

### RenderQueue_Dispatch_Special (0x6F13A780)
```cpp
int __fastcall RenderQueue_Dispatch_Special(
    void* sceneNode,   // ECX
    void* part,        // EDX
    int layerIndex,    // ESP+4
    int stateChanged   // ESP+8
)
```

**调用约定**: `__fastcall`  
**参数**:
- `sceneNode` (ECX): 场景节点
- `part` (EDX): 可渲染部件
- `layerIndex` (堆栈): 层索引
- `stateChanged` (堆栈): 状态是否改变

**返回值**: int

---

## 调用约定转换表

| 原始调用约定 | C++实现 | 说明 |
|-------------|---------|------|
| `__thiscall` | `void __thiscall Class::Method(int this, int param)` | this通过ECX传递 |
| `__cdecl` | `int __cdecl Function(int a1, int a2)` | 调用者清理堆栈 |
| `__fastcall` | `int __fastcall Function(int a1, int a2)` | a1=ECX, a2=EDX |
| `__usercall` | `int __usercall Function(int a1@<edi>, int a2@<esi>)` | 必须使用内联汇编捕获寄存器 |
| `__userpurge` | `int __userpurge Function@<eax>(int a1@<ecx>, int a2@<edi>)` | 返回前清理堆栈，返回值在EAX |

---

## P0严重错误修正清单

### 错误1: WorldObjectEntry_Render 函数签名
**位置**: `WorldObjectEntry_Render`  
**错误**: 使用了`int*`而不是`int`  
**影响**: 调用约定不匹配，导致堆栈不平衡

**修正**:
```cpp
// 错误
extern "C" int WorldObjectEntry_Render(
    int* entry,     // ❌ 错误的类型
    int categoryMode
);

// 正确
extern "C" int WorldObjectEntry_Render(
    int entry,      // ✅ 正确的类型
    int categoryMode
);
```

---

### 错误2: WorldObjectEntry_Render 参数传递
**位置**: `WorldObjectEntry_Render` 内部  
**错误**: 传递`entry[8]` (sceneNode) 而不是`entry` (WorldObjectEntry)  
**影响**: RenderQueue_AddBatch接收错误的参数类型

**修正**:
```cpp
// 错误
int* entryPtr = (int*)entry;
return RenderQueue_AddBatch((void*)entryPtr[8], categoryMode);  // ❌ 传递sceneNode

// 正确
return RenderQueue_AddBatch((void*)entry, categoryMode);  // ✅ 传递entry
```

---

### 错误3: RenderQueue_AddBatch 函数签名
**位置**: `RenderQueue_AddBatch`  
**错误**: 使用了两个参数而不是一个  
**影响**: 调用约定不匹配

**修正**:
```cpp
// 错误
extern "C" void Native_RenderQueue_AddBatch(
    SceneNode* sceneNode,
    int categoryMode  // ❌ 额外的参数
);

// 正确
extern "C" void RenderQueue_AddBatch(int sceneNode);  // ✅ 只有一个参数
```

---

### 错误4: RenderQueue_FlushSortedItems 参数模型
**位置**: `RenderQueue_FlushSortedItems`  
**错误**: 使用了不正确的参数模型  
**影响**: 寄存器丢失，导致状态优化失效

**修正**:
```cpp
// 错误
extern "C" unsigned int RenderQueue_FlushSortedItems(
    int a1,  // ❌ 无法捕获EDI
    int a2   // ❌ 无法捕获ESI
);

// 正确 - 必须使用汇编捕获寄存器
extern "C" unsigned int __declspec(naked) RenderQueue_FlushSortedItems()
{
    __asm {
        push ebp
        mov ebp, esp
        
        // 保存寄存器
        push edi
        push esi
        
        // 将寄存器作为参数传递
        push esi  // a2
        push edi  // a1
        call FlushSortedItems_Impl
        
        // 恢复寄存器
        pop esi
        pop edi
        
        pop ebp
        ret
    }
}
```

---

### 错误5: RenderQueue_FlushAndReset 参数模型
**位置**: `RenderQueue_FlushAndReset`  
**错误**: 使用了不正确的参数模型  
**影响**: 寄存器丢失

**修正**:
```cpp
// 错误
extern "C" int RenderQueue_FlushAndReset(
    int a1,  // ❌ 无法捕获EDI
    int a2   // ❌ 无法捕获ESI
);

// 正确 - 必须使用汇编捕获寄存器
extern "C" int __declspec(naked) RenderQueue_FlushAndReset()
{
    __asm {
        push ebp
        mov ebp, esp
        
        // 保存寄存器
        push edi
        push esi
        
        // 将寄存器作为参数传递
        push esi  // a2
        push edi  // a1
        call FlushAndReset_Impl
        
        // 恢复寄存器
        pop esi
        pop edi
        
        pop ebp
        ret
    }
}
```

---

## P1类型错误修正清单

### 错误6: WorldObjects_RenderGroup 类型转换
**位置**: `WorldObjects_RenderGroup`  
**错误**: 假设v5是WorldObjectEntry*而不是WorldObjectListEntry*  
**影响**: 类型错误，可能导致崩溃

**修正**:
```cpp
// 错误
void* v5 = (void*)listPtr[12];  // List_GetData
result = WorldObjectEntry_Render(
    (WorldObjectEntry*)v5,  // ❌ 直接转换
    v7
);

// 正确
void* v5 = (void*)listPtr[12];  // List_GetData
void* objectEntry = *(void**)((uint8_t*)v5 + 0);  // ✅ 提取WorldObjectEntry
result = WorldObjectEntry_Render(
    (int)objectEntry,  // ✅ 传递指针值
    v7
);
```

---

## 实现优先级

### P0 - 阻塞错误（必须立即修正）
1. ✅ **WorldObjectEntry_Render 函数签名** - 修正为int entry
2. ✅ **WorldObjectEntry_Render 参数传递** - 修正为传递entry而不是sceneNode
3. ✅ **RenderQueue_AddBatch 函数签名** - 移除categoryMode参数
4. ✅ **RenderQueue_FlushSortedItems 参数模型** - 使用汇编捕获寄存器
5. ✅ **RenderQueue_FlushAndReset 参数模型** - 使用汇编捕获寄存器

### P1 - 类型错误（应该修正）
6. ⚠️ **WorldObjects_RenderGroup 类型转换** - 添加类型转换逻辑

### P2 - 未实现功能（需要补充）
7. ⚠️ **RenderQueue_AddBatch 子节点遍历** - 完整实现
8. ⚠️ **SceneNode_AddTransparentList0/2/3/4** - 实现透明列表添加
9. ⚠️ **Visibility_Check** - 实现可见性检查

---

## 测试策略

### 1. 编译测试
```bash
# 编译时检查调用约定
msbuild WarVK.sln /p:Configuration=Release /p:Platform=Win32

# 检查警告
# 如果有关于调用约定的警告，必须修正
```

### 2. 运行时测试
```cpp
// 添加调试输出
DebugPrint("[Native] CWorld_RenderScene called: world=0x%08X\n", world);
DebugPrint("[Native] WorldObjectEntry_Render called: entry=0x%08X, mode=%d\n", entry, categoryMode);
DebugPrint("[Native] RenderQueue_AddBatch called: sceneNode=0x%08X\n", sceneNode);
```

### 3. 对比测试
- 使用原版游戏运行
- 使用实现版本运行
- 对比渲染结果
- 检查控制台输出
- 监控性能

### 4. 调试器测试
- 使用Visual Studio调试器
- 单步执行关键函数
- 检查寄存器状态
- 检查堆栈状态
- 验证参数传递

---

## 风险提示

### 高风险
- ⚠️ **调用约定错误**: 会导致堆栈不平衡，立即崩溃
- ⚠️ **寄存器丢失**: __usercall函数必须正确捕获寄存器
- ⚠️ **参数类型错误**: 会导致类型转换错误，可能崩溃

### 中风险
- ⚠️ **参数传递错误**: 会导致渲染链失效
- ⚠️ **返回值错误**: 可能导致调用者获取错误结果

### 低风险
- ⚠️ **实现逻辑错误**: 可能导致渲染不正确，但不会崩溃

---

## 总结

### 关键发现
1. **4个P0严重错误**都与调用约定相关
2. **__usercall函数**必须使用汇编捕获寄存器
3. **WorldObjectEntry_Render的第一个参数是int，不是int***
4. **RenderQueue_AddBatch只有一个参数，不是两个**
5. **参数传递错误**是最严重的问题

### 修正优先级
1. **最高**: 修正所有P0错误（调用约定）
2. **高**: 修正所有P1错误（类型转换）
3. **中**: 实现所有P2功能（缺失函数）

### 预期时间
- **修正P0**: 2-3小时
- **修正P1**: 30分钟
- **实现P2**: 2-3小时
- **测试和调试**: 2-4小时
- **总计**: 7-11小时

---

**文档生成时间**: 2026-01-25  
**核验工具**: IDA Pro MCP  
**核验人员**: Claude AI  
**文档状态**: ✅ 完整核验