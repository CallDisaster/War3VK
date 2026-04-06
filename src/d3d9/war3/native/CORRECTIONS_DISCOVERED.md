# War3渲染链逻辑核验报告 - 第二轮修正

> **核验时间**: 2026-01-25
> **核验方法**: IDA Pro MCP直接反编译验证
> **状态**: 发现多个关键函数签名错误

---

## 一、发现的函数签名错误

### 1.1 RenderQueue_StageUpdate 参数错误

**原版理解**:
```cpp
void RenderQueue_StageUpdate(int mode);  // 1个参数
```

**IDA反编译结果**:
```cpp
int __thiscall sub_6F13A9B0(void *this)  // 只有this指针
```

**实际调用** (在FlushSortedItems中):
```cpp
RenderQueue_StageUpdate(v15, v16);  // 传递2个参数（EDI/ESI寄存器）
```

**修正方案**:
- 函数签名应为: `void RenderQueue_StageUpdate(void* this, int param_edi, int param_esi)`
- 但函数内部可能不使用`param_edi`和`param_esi`
- 这是`__usercall`约定，参数通过寄存器传递

### 1.2 RenderQueue_FlushSortedItems 签名错误

**原版理解**:
```cpp
void RenderQueue_FlushSortedItems();  // 无参数
```

**IDA反编译结果**:
```cpp
unsigned int __usercall RenderQueue_FlushSortedItems@<eax>(
    int a1@<edi>, 
    int a2@<esi>
)
```

**修正方案**:
- 函数签名应为: `void RenderQueue_FlushSortedItems(void* param_edi, void* param_esi)`
- 使用`__usercall`约定，参数通过EDI/ESI寄存器传递
- `param_edi`: 某种类别参数
- `param_esi`: 上一个应用的状态块指针

### 1.3 RenderQueue_Dispatch_Common 参数数量错误

**原版理解**:
```cpp
int RenderQueue_Dispatch_Common(
    void* meshData,
    void* part,
    int layerIndex,
    int layerChanged,
    int stateChanged
);  // 5个参数
```

**IDA反编译结果**:
```cpp
int __fastcall RenderQueue_Dispatch_Common(
    int a1,  // ECX
    int a2,  // EDX
    int a3,  // 堆栈
    int a4,  // 堆栈
    int a5   // 堆栈
)
```

**实际调用** (在FlushSortedItems中):
```cpp
// 只有Common分支
RenderQueue_Dispatch_Common(*(_DWORD *)(v8 + 8), v14, v21);
// 只传递了3个参数！
```

**关键发现**:
- `a1` = `*(_DWORD *)(v8 + 8)` = RenderablePart指针
- `a2` = `v14` = layerChanged (0或1)
- `a3` = `v21` = stateChanged (0或1)
- `layerIndex`在函数内部通过`*(_DWORD *)(v8 + 8)`获取

**修正方案**:
- 函数签名应为: `int RenderQueue_Dispatch_Common(void* part, int layerChanged, int stateChanged)`
- `layerIndex`不是外部传入的，而是从RenderablePart内部读取的

### 1.4 RenderQueue_Dispatch_Special 参数数量错误

**原版理解**:
```cpp
int RenderQueue_Dispatch_Special(
    void* meshData,
    void* part,
    int layerIndex,
    int stateChanged
);  // 4个参数
```

**IDA反编译结果**:
```cpp
int __fastcall RenderQueue_Dispatch_Special(
    int a1,  // ECX
    int a2,  // EDX
    int a3,  // 堆栈
    int a4   // 堆栈
)
```

**实际调用** (在FlushSortedItems中):
```cpp
// Special分支
RenderQueue_Dispatch_Special(*(_DWORD *)(v8 + 8), v10);
// 只传递了2个参数！
```

**关键发现**:
- `a1` = `*(_DWORD *)(v8 + 8)` = RenderablePart指针
- `a2` = `v10` = stateChanged (0或1)
- `layerIndex`和`meshData`在函数内部通过RenderablePart读取

**修正方案**:
- 函数签名应为: `int RenderQueue_Dispatch_Special(void* part, int stateChanged)`
- `layerIndex`和`meshData`不是外部传入的，而是从RenderablePart内部读取的

### 1.5 WorldObjectEntry_Render 签名错误

**原版理解**:
```cpp
int WorldObjectEntry_Render(WorldObjectEntry* this);
```

**IDA反编译结果**:
```cpp
int __cdecl sub_6F184EE0(int a1, int a2)
```

**实际调用**:
```cpp
return RenderQueue_AddBatch(a1, a2);
```

**修正方案**:
- 函数签名应为: `int WorldObjectEntry_Render(void* param_a1, void* param_a2)`
- 不是`__thiscall`，而是`__cdecl`
- 接收2个参数，都传递给`RenderQueue_AddBatch`

---

## 二、调用约定理解

### 2.1 __usercall 约定

暴雪引擎大量使用`__usercall`约定，参数通过寄存器传递：

```cpp
// RenderQueue_FlushSortedItems示例
unsigned int __usercall RenderQueue_FlushSortedItems@<eax>(
    int a1@<edi>,  // 通过EDI寄存器传递
    int a2@<esi>   // 通过ESI寄存器传递
)
```

**特点**:
- 参数通过指定寄存器传递，不通过堆栈
- 返回值可能通过寄存器指定（如`@<eax>`）
- C++中需要使用汇编或特殊处理

### 2.2 __fastcall 约定

```cpp
int __fastcall RenderQueue_Dispatch_Common(
    int a1,  // ECX寄存器
    int a2,  // EDX寄存器
    int a3,  // 堆栈
    int a4,  // 堆栈
    int a5   // 堆栈
)
```

**特点**:
- 前两个参数通过ECX/EDX寄存器传递
- 后续参数通过堆栈传递
- 效率高于`__cdecl`

### 2.3 __cdecl 约定

```cpp
int __cdecl WorldObjectEntry_Render(int a1, int a2)
```

**特点**:
- 所有参数通过堆栈传递
- 调用者清理堆栈
- 不使用寄存器传参

---

## 三、数据结构偏移核验

### 3.1 RenderBatchElement (已确认)

```cpp
struct RenderBatchElement {
    void* batchEntry;      // +0x00: RenderablePart*
    uint32_t flags;        // +0x04: bit0=meshFlag, bit1=hasMoreLayers
    uint32_t layerIndex;   // +0x08: 层索引
    uint32_t layerCounter; // +0x0C: 可见层计数
    void* layerStatePtr;   // +0x10: 状态块指针
};
```

**验证方法**: 在FlushSortedItems中使用:
- `v8 = g_RenderQueue_SortedPtrs[result]` (v8是RenderBatchElement*)
- `*(_DWORD *)(v8 + 8)` = layerIndex ✓
- `*(_DWORD *)(v8 + 4)` = flags ✓
- `*(_DWORD **)(v8 + 16)` = layerStatePtr ✓

### 3.2 RenderablePart 偏移核验

根据RenderBatch_Submit反编译:
```cpp
v3[3] = this;  // RenderablePart + 12 = sceneNodeBackPtr
```

```cpp
struct RenderablePart {
    uint8_t padding1[12];     // +0x00 ~ +0x0B: 未知
    void* meshData;          // +0x0C: MeshData指针
    uint32_t skipFlag;        // +0x10: 跳过标志
    void* sceneNodeBackPtr;   // +0x14: 回指SceneNode
};
```

**修正**: `sceneNodeBackPtr`在+0x14，而不是之前的理解

### 3.3 MeshData 偏移核验

根据Dispatch_Common反编译:
```cpp
v12 = *(_DWORD *)(a2 + 12);      // RenderablePart + 12 = meshData
v6 = *(_DWORD *)(v12 + 264);     // MeshData + 264 = meshIndex
result = *(_DWORD *)(v12 + 260); // MeshData + 260 = meshFlag
```

```cpp
struct MeshData {
    void* vtable;           // +0x00
    // ... (260 bytes padding)
    uint32_t meshFlag;      // +0x104 (260): 网格标志
    uint32_t meshIndex;      // +0x108 (264): 网格索引
};
```

**修正**: 
- `meshFlag`在+0x104 (十进制260)
- `meshIndex`在+0x108 (十进制264)

---

## 四、关键逻辑差异

### 4.1 FlushSortedItems的stateChanged计算

**原版理解**:
```cpp
bool stateChanged = !g_RenderQueue_StateOptEnabled || 
                    meshData != lastMeshData;
```

**IDA反编译结果**:
```cpp
v10 = !g_RenderQueue_StateOptEnabled || 
      v9 != v19 || 
      v20 != *(_DWORD *)(v8 + 8) || 
      *(_DWORD *)(v9 + 260);
```

**实际逻辑**:
```cpp
bool stateChanged = !g_RenderQueue_StateOptEnabled || 
                    meshData != lastMeshData ||       // v9 != v19
                    layerIndex != lastLayerIndex ||   // v20 != *(_DWORD *)(v8 + 8)
                    meshFlag != 0;                   // *(_DWORD *)(v9 + 260)
```

**修正**: stateChanged的判断条件比原版理解更复杂，包含4个条件

---

## 五、修正后的函数原型

### 5.1 RenderQueue核心函数

```cpp
// RenderQueue_StageUpdate
// 调用约定: __thiscall (实际上是__usercall，但IDA识别为__thiscall)
// 实际接收: void* this (ECX), int param_edi (EDI), int param_esi (ESI)
extern "C" void RenderQueue_StageUpdate(void* this, int param_edi, int param_esi);

// RenderQueue_FlushSortedItems
// 调用约定: __usercall
// 参数: int a1 (EDI), int a2 (ESI)
// 返回: unsigned int (EAX)
extern "C" unsigned int RenderQueue_FlushSortedItems(void* param_edi, void* param_esi);

// RenderQueue_FlushAndReset
// 调用约定: __usercall
extern "C" void RenderQueue_FlushAndReset(int category, CWorld* world);
```

### 5.2 Dispatch函数

```cpp
// RenderQueue_Dispatch_Common
// 调用约定: __fastcall
// 实际参数: void* part (ECX), int layerChanged (EDX), int stateChanged (堆栈)
// 注意: layerIndex在函数内部通过part读取，不作为参数传入
extern "C" int RenderQueue_Dispatch_Common(void* part, int layerChanged, int stateChanged);

// RenderQueue_Dispatch_Special
// 调用约定: __fastcall
// 实际参数: void* part (ECX), int stateChanged (EDX)
// 注意: layerIndex和meshData在函数内部通过part读取，不作为参数传入
extern "C" int RenderQueue_Dispatch_Special(void* part, int stateChanged);
```

### 5.3 WorldObjectEntry函数

```cpp
// WorldObjectEntry_Render
// 调用约定: __cdecl (不是__thiscall!)
// 参数: int a1, int a2
// 这两个参数都传递给RenderQueue_AddBatch
extern "C" int WorldObjectEntry_Render(void* param_a1, void* param_a2);
```

---

## 六、实现建议

### 6.1 调用约定处理

由于暴雪引擎大量使用`__usercall`，在C++中实现时有几种方案：

**方案1: 使用内联汇编（32位）**
```cpp
extern "C" void RenderQueue_FlushSortedItems(void* param_edi, void* param_esi) {
    __asm {
        mov edi, param_edi
        mov esi, param_esi
        call dword ptr [0x6F1380A0]  // 直接调用原版函数
    }
}
```

**方案2: 使用__declspec(naked)**
```cpp
extern "C" __declspec(naked) void RenderQueue_FlushSortedItems(void* param_edi, void* param_esi) {
    __asm {
        mov edi, [esp+4]   // 获取param_edi
        mov esi, [esp+8]   // 获取param_esi
        jmp dword ptr [0x6F1380A0]  // 跳转到原版函数
    }
}
```

**方案3: Hook原版函数**
```cpp
// 使用Detours/Mhook库Hook原版函数
typedef unsigned int (__usercall *RenderQueue_FlushSortedItems_t)(void* param_edi, void* param_esi);
RenderQueue_FlushSortedItems_t original_FlushSortedItems;

unsigned int __usercall Hooked_FlushSortedItems(void* param_edi, void* param_esi) {
    // 自定义逻辑
    return original_FlushSortedItems(param_edi, param_esi);
}
```

### 6.2 参数获取方式

由于Dispatch函数不直接接收layerIndex，需要通过RenderablePart获取：

```cpp
// 在Dispatch_Common内部获取layerIndex
void* meshData = *(void**)((uint8_t*)part + 0x0C);  // part + 12
uint32_t meshIndex = *(uint32_t*)((uint8_t*)meshData + 0x108);  // meshData + 264
```

### 6.3 状态优化逻辑

正确的stateChanged判断：
```cpp
bool stateChanged = !g_RenderQueue_StateOptEnabled || 
                    meshData != lastMeshData || 
                    layerIndex != lastLayerIndex || 
                    meshFlag != 0;
```

---

## 七、下一步行动

1. **更新头文件**: 修正所有函数签名
2. **实现Hook框架**: 选择合适的Hook库
3. **创建测试用例**: 验证每个关键函数
4. **编写文档**: 详细说明调用约定和参数传递
5. **渐进式替换**: 先替换简单函数，再处理复杂函数

---

## 八、关键发现总结

1. **函数签名错误**: Dispatch_Common和Dispatch_Special的参数数量严重错误
2. **调用约定复杂**: 大量使用__usercall，需要特殊处理
3. **参数隐藏**: layerIndex和meshData在函数内部获取，不作为参数传入
4. **状态优化复杂**: stateChanged有4个判断条件，不是简单的比较
5. **寄存器传递**: EDI/ESI寄存器用于传递重要参数

---

**文档版本**: 2.0
**最后更新**: 2026-01-25
**核验方法**: IDA Pro MCP直接反编译
**置信度**: 高（基于实际反编译结果）
