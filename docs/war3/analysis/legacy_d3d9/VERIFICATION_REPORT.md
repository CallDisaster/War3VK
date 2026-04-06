# 魔兽争霸3渲染链核验报告

## 核验日期
2026-01-25

## 核验方法
使用 IDA Pro MCP 工具获取关键函数的反编译代码，与实现进行对比。

---

## ✅ 核验通过的函数

### 1. RenderWorld_DispatchStage (0x6F363020)
**状态**: ✅ 完全正确

**IDA反编译关键点**:
- `if (a5) a3 = 3;` - a5非0时强制a3=3
- `v7 = *(this + 409);` - world[409]存储当前类别
- `if (a4 != v7)` - 状态切换逻辑
- `v8 = *(this + 408);` - world[408]存储当前模式
- `if (a3 != v8)` - 模式切换逻辑
- switch(stageId) - 22个case，完全匹配

**实现对比**:
- ✅ 使用了正确的偏移（408, 409）
- ✅ 状态切换逻辑完全一致
- ✅ switch case 完全匹配
- ✅ 每个case调用的函数正确

**结论**: 实现完全正确，可以放心使用。

---

### 2. WorldObjects_RenderGroup (0x6F368E30)
**状态**: ✅ 基本正确（有类型转换问题）

**IDA反编译关键点**:
- groupIdx=0: `v4 = a1[91]` (Units list)
- groupIdx=1: `v4 = a1[92]` (Buildings list)
- groupIdx=2: `v4 = a1[93]` (Effects list)
- `v5 = List_GetData(v4)` - 获取列表数据
- 遍历每个条目（stride=24）
- 调用 `WorldObjectEntry_Render(v7, v8)`
- v7 = a2 (categoryMode)
- v8 = v5 (listData)

**实现对比**:
- ✅ 使用了正确的列表索引（91, 92, 93）
- ✅ List_GetData/List_GetCount 调用正确
- ✅ 遍历逻辑正确
- ⚠️ 参数类型错误：v5应该是WorldObjectListEntry，不是WorldObjectEntry
- ✅ 传递了正确的参数

**结论**: 实现基本正确，但有类型转换问题，需要调整。

---

### 3. WorldObjectEntry_Render (0x6F184EE0)
**状态**: ✅ 正确（但WorldObjectEntry_Render函数签名有问题）

**IDA反编译关键点**:
```cpp
int __cdecl sub_6F184EE0(int a1, int a2) {
  _DWORD *v2; // ecx
  int result; // eax

  if ( v2[8] ) {  // v2[8] = entry+0x20 = sceneNode
    (*(void (__thiscall **)(_DWORD *))(*v2 + 20))(v2);  // vtable[5] PreRender
    return RenderQueue_AddBatch(a1, a2);
  }
  return result;
}
```

**实现对比**:
- ✅ 正确检查 `entry[8]` (sceneNode偏移0x20)
- ✅ 正确调用vtable[5] (PreRender)
- ⚠️ **参数传递错误**: 应该传递 `a1` (entry) 而不是 `entry[8]` (sceneNode)
- ❌ **函数签名错误**: IDA显示 `int __cdecl sub_6F184EE0(int a1, int a2)`，但我们的实现是 `int WorldObjectEntry_Render(int* entry, int categoryMode)`

**结论**: 
- **严重错误**: 参数传递错误！
- 应该传递 `a1` (entry指针) 给 `RenderQueue_AddBatch`，而不是 `entry[8]` (sceneNode)

---

### 4. RenderQueue_AddBatch (0x6F139190)
**状态**: ❌ 未实现

**IDA反编译关键点**:
```cpp
void __thiscall RenderQueue_AddBatch(int this) {
  int v2; // edi
  unsigned int v3; // ebx
  
  v2 = *(_DWORD *)(this + 156);  // sceneNode+156
  RenderBatch_Submit((_DWORD *)this);  // 调用RenderBatch_Submit
  
  if ( (*(_BYTE *)(this + 148) & 0x10) != 0 ) {
    SceneNode_AddTransparentList0(this, v2);
    SceneNode_AddTransparentList2(this, v2);
    SceneNode_AddTransparentList3(this, v2);
    SceneNode_AddTransparentList4(this);
    
    // 复杂的子节点遍历逻辑...
  }
}
```

**实现对比**:
- ❌ 我们的 `Native_RenderQueue_AddBatch` 参数是 `(SceneNode*, int)`，但IDA显示只有 `(int)`（sceneNode）
- ❌ 实现逻辑完全不匹配
- ❌ 缺少子节点遍历的完整实现

**结论**: 
- **严重错误**: 函数签名和实现都不正确
- 需要完全重写

---

## ❌ 发现的错误

### 错误1: WorldObjectEntry_Render 参数传递错误
**位置**: `WorldObjectEntry_Render` 函数

**问题描述**:
- IDA显示应该传递 `a1` (entry指针) 给 `RenderQueue_AddBatch`
- 我们实现传递的是 `entry[8]` (sceneNode指针)

**错误代码**:
```cpp
// 错误的实现
return RenderQueue_AddBatch((void*)entry[8], categoryMode);  // ❌ 传递了sceneNode
```

**正确代码**:
```cpp
// 正确的实现
return RenderQueue_AddBatch((void*)entry, categoryMode);  // ✅ 传递entry
```

**影响**: 
- **严重**: 会导致渲染链完全失效
- `RenderQueue_AddBatch` 期望接收 WorldObjectEntry，而不是 SceneNode

---

### 错误2: WorldObjectEntry_Render 函数签名错误
**位置**: `WorldObjectEntry_Render` 函数签名

**问题描述**:
- IDA显示: `int __cdecl sub_6F184EE0(int a1, int a2)`
- 我们实现: `int WorldObjectEntry_Render(int* entry, int categoryMode)`
- 第一个参数是 `int` (entry指针的数值)，不是 `int*`

**影响**:
- 可能导致编译错误或运行时错误
- 需要修改为 `int WorldObjectEntry_Render(int entry, int categoryMode)`

---

### 错误3: RenderQueue_AddBatch 函数签名错误
**位置**: `RenderQueue_AddBatch` 函数

**问题描述**:
- IDA显示: `void __thiscall RenderQueue_AddBatch(int this)`
- 第一个参数是 `sceneNode` (this指针)
- 我们实现: `void Native_RenderQueue_AddBatch(SceneNode* sceneNode, int categoryMode)`
- 额外的 `categoryMode` 参数不在IDA反编译中

**影响**:
- 函数签名不匹配，可能导致调用失败
- 需要移除 `categoryMode` 参数

---

### 错误4: WorldObjects_RenderGroup 类型错误
**位置**: `WorldObjects_RenderGroup` 函数

**问题描述**:
- IDA显示v5指向的是 `WorldObjectListEntry`（stride=24）
- 我们实现假设v5是 `WorldObjectEntry*`
- 需要先从 `WorldObjectListEntry` 中提取 `WorldObjectEntry`

**影响**:
- 可能导致类型错误和渲染失败
- 需要添加类型转换逻辑

---

## 🔧 修正方案

### 修正1: WorldObjectEntry_Render 参数传递
**文件**: `src/d3d9/war3/native/war3_native_renderer.cpp`

**修改前**:
```cpp
return RenderQueue_AddBatch((void*)entry[8], categoryMode);  // ❌
```

**修改后**:
```cpp
return RenderQueue_AddBatch((void*)entry, categoryMode);  // ✅
```

---

### 修正2: WorldObjectEntry_Render 函数签名
**文件**: `src/d3d9/war3/native/war3_native_renderer.cpp`

**修改前**:
```cpp
extern "C" int WorldObjectEntry_Render(
    int* entry,     // a1: WorldObjectEntry指针
    int categoryMode // a2: categoryMode
) {
    if (entry[8] != 0) {  // ❌ entry是int*，entry[8]是int
```

**修改后**:
```cpp
extern "C" int WorldObjectEntry_Render(
    int entry,      // a1: WorldObjectEntry指针（作为int传递）
    int categoryMode // a2: categoryMode
) {
    int* entryPtr = (int*)entry;  // ✅ 转换为int*
    if (entryPtr[8] != 0) {  // ✅ 使用entryPtr[8]
```

---

### 修正3: RenderQueue_AddBatch 函数签名和实现
**文件**: `src/d3d9/war3/native/war3_native_renderer.cpp`

**修改前**:
```cpp
extern "C" void Native_RenderQueue_AddBatch(
    SceneNode* sceneNode,
    int categoryMode  // ❌ 额外的参数
) {
    // ❌ 实现逻辑不匹配
}
```

**修改后**:
```cpp
extern "C" void RenderQueue_AddBatch(int sceneNode) {
    SceneNode* node = (SceneNode*)sceneNode;
    
    // sceneNode+156
    int v2 = *((int*)((uint8_t*)node + 156));
    
    // 调用RenderBatch_Submit
    RenderBatch_Submit(node);
    
    // 检查flags (sceneNode+148)
    if (*((uint8_t*)((uint8_t*)node + 148)) & 0x10) {
        SceneNode_AddTransparentList0(node, v2);
        SceneNode_AddTransparentList2(node, v2);
        SceneNode_AddTransparentList3(node, v2);
        SceneNode_AddTransparentList4(node);
        
        // TODO: 实现子节点遍历逻辑
    }
}
```

---

### 修正4: WorldObjects_RenderGroup 类型转换
**文件**: `src/d3d9/war3/native/war3_native_renderer.cpp`

**修改前**:
```cpp
void* v5 = (void*)listPtr[12];  // List_GetData
// ...
result = WorldObjectEntry_Render(
    (WorldObjectEntry*)v5,  // ❌ 直接转换为WorldObjectEntry
    v7
);
```

**修改后**:
```cpp
void* v5 = (void*)listPtr[12];  // List_GetData
// v5指向WorldObjectListEntry，需要提取WorldObjectEntry
void* objectEntry = *(void**)((uint8_t*)v5 + 0);  // WorldObjectListEntry+0
result = WorldObjectEntry_Render(
    (int)objectEntry,  // ✅ 传递WorldObjectEntry的指针值
    v7
);
```

---

## 📊 修正优先级

### P0 - 阻塞错误（必须立即修正）
1. ✅ **WorldObjectEntry_Render 参数传递错误** - 修正为传递entry而不是sceneNode
2. ✅ **WorldObjectEntry_Render 函数签名错误** - 修正为int entry而不是int* entry
3. ✅ **RenderQueue_AddBatch 函数签名错误** - 移除categoryMode参数
4. ✅ **RenderQueue_AddBatch 实现逻辑错误** - 重写以匹配IDA反编译

### P1 - 类型错误（应该修正）
5. ⚠️ **WorldObjects_RenderGroup 类型错误** - 添加类型转换逻辑

### P2 - 未实现功能（需要补充）
6. ⚠️ **RenderQueue_AddBatch 子节点遍历** - 完整实现子节点遍历逻辑
7. ⚠️ **SceneNode_AddTransparentList0/2/3/4** - 实现透明列表添加函数
8. ⚠️ **Visibility_Check** - 实现可见性检查函数

---

## 🎯 修正后的预期结果

### 修正P0后
- 🟢 **渲染链正确**: WorldObjectEntry_Render正确调用RenderQueue_AddBatch
- 🟢 **参数正确**: 所有函数参数传递正确
- 🟢 **签名正确**: 函数签名与IDA反编译一致
- 🟢 **基本可运行**: 可能渲染部分内容

### 修正P1后
- 🟢 **类型正确**: 所有类型转换正确
- 🟢 **稳定运行**: 不会因为类型错误崩溃

### 完成P2后
- 🟢 **功能完整**: 所有渲染路径实现
- 🟢 **可以正常使用**: 替代原版渲染器

---

## 📝 总结

### 发现的问题
1. **4个P0严重错误**: 参数传递、函数签名、实现逻辑
2. **1个P1类型错误**: 类型转换
3. **3个P2未实现功能**: 子节点遍历、透明列表添加、可见性检查

### 当前状态
- **完成度**: 约50%（有严重错误）
- **可用性**: ❌ **无法运行** - 参数传递错误会导致渲染链失效
- **下一步**: 立即修正P0错误

### 预期时间
- **修正P0**: 1-2小时
- **修正P1**: 30分钟
- **完成P2**: 2-3小时
- **总计**: 约4-6小时

---

## 🔍 建议

### 立即行动
1. ✅ **修正WorldObjectEntry_Render参数传递** - 最高优先级
2. ✅ **修正WorldObjectEntry_Render函数签名** - 最高优先级
3. ✅ **重写RenderQueue_AddBatch** - 最高优先级
4. ✅ **修正WorldObjects_RenderGroup类型** - 高优先级

### 测试策略
1. 编译后立即测试基本渲染
2. 检查控制台输出和错误日志
3. 使用调试器单步执行关键函数
4. 对比原版和实现的渲染结果

### 风险提示
- ⚠️ 修正错误后可能会有新的错误出现
- ⚠️ 需要充分测试才能确保稳定性
- ⚠️ 可能需要多次迭代才能完全正确

---

**报告生成时间**: 2026-01-25  
**核验工具**: IDA Pro MCP  
**核验人员**: Claude AI