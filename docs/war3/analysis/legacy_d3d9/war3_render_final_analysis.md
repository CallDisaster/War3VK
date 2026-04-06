# War3 Native Renderer - 最终分析报告

## 已确认的信息

### 1. `WorldObjectEntry_Render` 的真实实现

```c
int __cdecl sub_6F184EE0(int a1, int a2) {
  _DWORD *v2; // ecx
  int result; // eax

  if ( v2[8] ) {  // v2[8] = entry->sceneNode (+0x20)
    // 调用 vtable[5] (偏移20 = 5 * 4)
    (*(void (__thiscall **)(_DWORD *))(*v2 + 20))(v2);
    
    // 直接传递 a1 (参数1) 给 RenderQueue_AddBatch
    return RenderQueue_AddBatch(a1, a2);
  }
  return result;
}
```

**关键发现：**
- `v2[8]` 访问的是第8个dword (+0x20)
- `vtable[5]` 调用后，直接传递 `a1` 给 `RenderQueue_AddBatch`
- **没有从 v2[8] (sceneNode) 提取指针！**

### 2. CWorldObjects vtable[5] 的功能

```c
int __thiscall sub_6F7594D0(_DWORD *this) {
  // 调用 vtable[5] (递归调用基类的vtable[5])
  result = (*(int (__thiscall **)(_DWORD *))(*this + 20))(this);
  
  v3 = 0;
  v4 = result;
  v8 = result;
  
  if ( *(this + 4) ) {  // this[4] = count
    v5 = 0;
    do {
      v6 = *(this + 5);  // this[5] = 某个表
      if ( (*(_DWORD *)(v6 + v5 + 132) & 0x80000) == 0 ) {
        v7 = *(_DWORD *)(v6 + v5 + 136);
        if ( v7 != -1 ) {
          sub_6F73DDC0(v7, v4);  // 设置标志
          v4 = v8;
        }
      }
      result = *(_DWORD *)(*(this + 5) + v5 + 144);
      if ( result != -1 )
        result = sub_6F73DDC0(result, v4);
      v4 = v8;
      ++v3;
      v5 += 392;  // stride = 392 bytes
    } while ( v3 < *(this + 4) );
  }
  return result;
}
```

**关键发现：**
- `this[4]` = 计数
- `this[5]` = 指向某个数组的指针
- 数组元素stride = 392 bytes
- `sub_6F73DDC0` 设置标志：`*(result + 160 * a2 + 4) |= 2u`

### 3. `sub_6F73DDC0` 的功能

```c
int __thiscall sub_6F73DDC0(_DWORD *this, int a2, int a3) {
  int result; // eax

  result = *(this + 179);  // this[179]
  if ( a3 )
    *(_DWORD *)(result + 160 * a2 + 4) |= 2u;
  else
    *(_DWORD *)(result + 160 * a2 + 4) &= ~2u;
  return result;
}
```

**关键发现：**
- `this[179]` = 某个数组基址
- 设置偏移 `160 * a2 + 4` 处的 bit 1 (0x02)

## 结构推断

基于以上信息，我推断 `CWorldObjects` 的结构大致如下：

```c
struct CWorldObjects {
    void* vtable;           // +0x00
    
    // ... 未知字段 (3个dword)
    
    void* unknownArray;     // +0x04 (this[4]的值？)
    void* tableBase;        // +0x05 (this[5])
    
    // ... 未知字段 ...
    
    void* flagArray;        // +0x179 (this[179])
    
    // ... 其他字段 ...
};
```

**但是**，这个结构与 `RenderQueue_AddBatch` 访问的偏移不匹配：
- `RenderQueue_AddBatch` 访问 +156, +148, +196, +200
- CWorldObjects 的 vtable[5] 访问 +4, +5, +179

## 关键矛盾

### 问题1: `RenderQueue_AddBatch` 的参数类型

**证据A: `WorldObjectEntry_Render` 传递 `a1`**
```c
return RenderQueue_AddBatch(a1, a2);  // a1是第一个参数
```
- 如果 `a1` 是 `WorldObjectEntry*`，那么应该传递 `v2[8]` (sceneNode)
- 但代码直接传递 `a1`

**证据B: `RenderQueue_AddBatch` 内部访问**
```c
v2 = *(_DWORD *)(this + 156);  // +156
RenderBatch_Submit((_DWORD *)this);  // 传递同一个this
```

**证据C: `RenderBatch_Submit` 期望的偏移**
```c
v2 = (_DWORD *)*(this + 4);  // +4
```

### 问题2: vtable[5] 的真正作用

从 CWorldObjects 的 vtable[5] 代码来看：
- 它调用基类的 vtable[5]
- 然后遍历一个 392-byte stride 的数组
- 设置标志位

**这不是 `PreRender` 函数！**

而是某种**更新可见性标志**的函数。

## 新假设：多态调用链

可能的调用链：
```
WorldObjectEntry_Render(obj, mode)
  └─ obj->vtable[5](obj)  // 虚函数调用，可能是CWorldObjects::vtable[5]
       └─ (CWorldObjects内部)
            ├─ 调用基类的 vtable[5] (可能是PreRender)
            └─ 更新可见性标志
       └─ RenderQueue_AddBatch(obj, mode)
            └─ 传递同一个obj
```

## 类型解决方案

### 方案A: `RenderQueue_AddBatch` 接收 `void*` (通用指针)

由于访问的偏移差异太大，`RenderQueue_AddBatch` 可能：
1. 接收 `void*` 类型
2. 内部根据运行时类型信息进行不同的处理
3. 或者通过某种方式转换为正确的结构指针

### 方案B: 使用偏移映射表

`RenderQueue_AddBatch` 可能有内部的偏移映射：
```c
struct RenderQueue_AddBatch_Params {
    union {
        struct {
            void* sceneNodeData[4];    // +0x00 ~ +0x0C
            void* renderableList;       // +0x10 (偏移4)
            // ...
        } sceneNode;
        
        struct {
            void* unknown[37];         // +0x00 ~ +0x92
            void* childTable;          // +0x94 (偏移37)
            // ...
        } worldObject;
    };
};
```

但这过于复杂，不太可能。

### 方案C: 不同的函数，相同的名字

最可能的情况：存在多个同名函数，通过不同的地址调用：
- `0x6F139190`: `RenderQueue_AddBatch(SceneNode*)`
- 另一个地址: `RenderQueue_AddBatch(CWorldObjects*)`

**但交叉引用显示只有 0x6F139190 被调用！**

## 最终结论

基于所有证据，我倾向于以下理解：

### 1. `WorldObjectEntry` 包含完整的渲染节点信息

```c
struct WorldObjectEntry {
    void* vtable;           // +0x00
    // ... 3个dword ...
    void* renderableList;     // +0x10 (偏移4)
    // ... 未知字段 ...
    void* sceneNodeData;     // +0x20 (偏移8)
    // ... 未知字段 ...
    void* childTable;        // +0x9C (偏移39，即156)
    uint32_t flags;         // +0x94 (偏移37，即148)
    // ... 未知字段 ...
    uint32_t childCount;     // +0xC4 (偏移49，即196)
    void* childVisFlags;     // +0xC8 (偏移50，即200)
};
```

**解释：**
- `RenderBatch_Submit` 将其视为 `SceneNode`，访问低偏移字段
- `RenderQueue_AddBatch` 将其视为 `WorldObjectEntry`，访问高偏移字段
- **这是同一个结构体！**

### 2. 为什么偏移不匹配？

因为IDA的偏移计算是基于 `int*` (4-byte) 的：
- `*(this + 4)` = `this[4]` = 偏移 4 * 4 = 16 (0x10)
- `*(this + 156)` = `this[156]` = 偏移 156 * 4 = 624 (0x270)

让我重新检查：

**`RenderBatch_Submit` 的实际偏移：**
```c
v2 = (_DWORD *)*(this + 4);  // this[4] = +0x10 (16 bytes)
if ( !v3[4] )  // v3[4] = +0x10 (16 bytes)
v4 = (_DWORD *)v3[3];  // v3[3] = +0x0C (12 bytes)
v3[5] = this;  // v3[5] = +0x14 (20 bytes)
```

**`RenderQueue_AddBatch` 的实际偏移：**
```c
v2 = *(_DWORD *)(this + 156);  // this[156] = +0x270 (624 bytes)
if ( (*(_BYTE *)(this + 148) & 0x10)  // this[148] = +0x254 (596 bytes)
*(_DWORD *)(this + 196)  // this[196] = +0x310 (784 bytes)
*(_DWORD *)(this + 200)  // this[200] = +0x320 (800 bytes)
```

**结论：这确实不是一个结构体！偏移相差太大。**

## 最终答案：函数重载或间接调用

最合理的解释是：

1. **`WorldObjectEntry_Render` 调用的是另一个 `RenderQueue_AddBatch`**
   - 不是 0x6F139190
   - 可能是内联函数
   - 或者是函数指针间接调用

2. **交叉引用可能不完整**
   - IDA 可能没有捕获所有的交叉引用
   - 或者存在动态调用

3. **`WorldObjectEntry_Render` 的第一个参数类型**
   - 很可能是 `CWorldObjects*`
   - 调用 `vtable[5]` 后，传递给某个重载的 `RenderQueue_AddBatch`
   - 这个重载版本访问 +156, +148, +196, +200 等偏移

## 建议

由于IDA信息有限，建议采用以下策略：

### 策略1: 运行时验证
在游戏中Hook这些函数，打印参数地址和内容，验证真实类型。

### 策略2: 保守实现
基于当前的文档理解，实现一个可以工作的版本，即使不是100%精确。

### 策略3: 暂时绕过
不实现完整的 `RenderQueue_AddBatch`，而是直接调用原版函数，只Hook `FlushSortedItems` 等关键函数。

## 当前实现建议

基于以上分析，建议保持当前的保守实现：

```cpp
// WorldObjectEntry_Render
int Native_WorldObjectEntry_Render(void* entry, int categoryMode) {
    if (*((void**)((uint8_t*)entry + 0x20)) {  // v2[8]
        // 调用vtable[5] (PreRender)
        void** vtable = *(void**)entry;
        if (vtable) {
            typedef void (__thiscall* PreRenderFunc)(void*);
            PreRenderFunc preRender = (PreRenderFunc)vtable[5];
            preRender(entry);
        }
        
        // 调用 RenderQueue_AddBatch
        // 注意：这里传递的可能是CWorldObjects*，不是SceneNode*
        return (int)RenderQueue_AddBatch((void*)entry, categoryMode);
    }
    return 0;
}
```

**但需要确认 `RenderQueue_AddBatch` 的地址和签名。**

## 待解决的关键问题

1. ✅ `WorldObjectEntry_Render` 的完整代码
2. ✅ CWorldObjects vtable[5] 的功能
3. ❓ `RenderQueue_AddBatch` 是否有多个版本
4. ❓ `WorldObjectEntry` 的完整结构
5. ❓ 为什么偏移差异如此之大
6. ❓ 如何正确实现 `RenderQueue_AddBatch`