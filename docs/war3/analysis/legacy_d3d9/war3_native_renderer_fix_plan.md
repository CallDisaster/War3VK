# War3 Native Renderer - 最终修正方案

## 关键发现

### 1. `RenderBatch_Submit` 参数类型确认

**IDA签名：** `void __thiscall sub_6F1375C0(_DWORD *this)`

**代码分析：**
```c
void __thiscall RenderBatch_Submit(SceneNode* this) {
  v2 = (_DWORD *)*(this + 4);  // this[4] = renderableList
  // ...
  v3 = (_DWORD *)*v2;  // *v2 = RenderablePart*
  v20 = v3;  // v20 = RenderablePart*
  
  if ( !v3[4] )  // v3[4] = part->skipFlag
  {
    v4 = (_DWORD *)v3[3];  // v3[3] = part->meshData
    // ...
    v3[5] = this;  // part->sceneNodeBackPtr = this (SceneNode)
    // ...
  }
}
```

**结论：** `RenderBatch_Submit`接收`SceneNode*`参数

### 2. `RenderQueue_AddBatch` 参数类型

**IDA签名：** `void __thiscall RenderQueue_AddBatch(int this)`

**代码分析：**
```c
void __thiscall RenderQueue_AddBatch(SceneNode* this) {
  v2 = *(_DWORD *)(this + 156);  // this[156] = childTable
  RenderBatch_Submit((_DWORD *)this);  // this是SceneNode
  
  if ( (*(_BYTE *)(this + 148) & 0x10) != 0 ) {  // this[148] = flags
    // 透明列表处理
    SceneNode_AddTransparentList0(this, v2);
    // ...
    
    // 子节点递归
    v3 = 0;
    if ( *(_DWORD *)(this + 196) ) {  // this[196] = childCount
      v4 = (int *)(*(_DWORD *)(this + 200) + 8);  // this[200] = childVisFlags
      v10 = v4;
      do {
        if ( ... ) {
          v7 = *v4;  // 解引用childTable[v3]
          if ( *v4 > 0 ) {
            do {
              RenderQueue_AddBatch(v8, v9);  // 递归调用
              v7 = *(_DWORD *)(v7 + 4);
            } while ( v7 > 0 );
            v4 = v10;
          }
        }
        ++v3;
        v4 += 3;  // childTable中每个元素占3个dword
        v10 = v4;
      } while ( v3 < *(_DWORD *)(this + 196) );
    }
  }
}
```

**关键发现：**
- `this`参数是`SceneNode*`（与`RenderBatch_Submit`相同）
- `this[156]` = childTable
- `this[148]` = flags (0x10影响透明)
- `this[196]` = childCount
- `this[200]` = childVisFlags
- **递归调用**：`RenderQueue_AddBatch(v8, v9)`

### 3. 递归参数的困惑

**代码片段：**
```c
v7 = *v4;  // v4指向childTable[v3]
if ( *v4 > 0 ) {  // *v4是计数
  do {
    RenderQueue_AddBatch(v8, v9);  // v8和v9从哪里来？
    v7 = *(_DWORD *)(v7 + 4);  // v7 += 4
  } while ( v7 > 0 );
}
```

**问题：** v8和v9的定义在代码片段中缺失

**推测：**
1. v8 = 某个SceneNode*指针
2. v9 = categoryMode参数（从WorldObjectEntry_Render传递）

**需要进一步分析：**
- v8可能来自childTable的另一个偏移
- v9可能是寄存器传递的第二个参数

### 4. `WorldObjectEntry_Render` 参数类型

**IDA真实代码：**
```c
int __cdecl sub_6F184EE0(int a1, int a2) {
  if (v2[8]) {  // v2[8] = entry->sceneNode
    (*(void (__thiscall **)(_DWORD *))(*v2 + 20))(v2);  // vtable[5]
    return RenderQueue_AddBatch(a1, a2);  // a1是WorldObjectEntry，不是sceneNode！
  }
  return result;
}
```

**WorldObjects_RenderGroup调用：**
```c
result = WorldObjectEntry_Render(v7, v8);  // v8是listData指针
```

**结论：** `WorldObjectEntry_Render`的第一个参数类型有歧义

### 5. `WorldObjectListEntry` 结构推测

根据`WorldObjects_RenderGroup`的代码：
```c
v5 = (void*)listPtr[12];  // List_GetData返回this[12]
result = listPtr[20];  // List_GetCount返回this[20]

do {
    result = WorldObjectEntry_Render(v7, v8);  // v8 = v5
    v5 += 24;  // WorldObjectListEntry stride = 24
} while (i);
```

**推测结构：**
```c
struct WorldObjectListEntry {
    void* objectEntry;  // +0x00: 指向WorldObjectEntry
    // +0x04-0x17: 其他数据
};
```

**但是！** 如果`WorldObjectEntry_Render`的第一个参数是`WorldObjectListEntry*`，那么：
- `v2[8]` 应该对应 `listEntry[8]`
- 这意味着`WorldObjectEntry`在`WorldObjectListEntry`的偏移0处开始

**另一种可能：** `WorldObjectListEntry`就是`WorldObjectEntry`！

## 修正策略

### 方案A：假设`WorldObjectListEntry` == `WorldObjectEntry`

```cpp
// WorldObjects_RenderGroup
void* v5 = (void*)listPtr[12];  // 指向WorldObjectEntry数组
int count = listPtr[20];

for (int i = 0; i < count; i++) {
    WorldObjectEntry* entry = (WorldObjectEntry*)((uint8_t*)v5 + i * 24);
    result = WorldObjectEntry_Render((int*)entry, v7);
}
```

```cpp
// WorldObjectEntry_Render
int WorldObjectEntry_Render(WorldObjectEntry* entry, int categoryMode) {
    if (entry->sceneNode) {  // +0x20 (第8个dword)
        // 调用vtable[5]
        // ...
        return RenderQueue_AddBatch((SceneNode*)entry, categoryMode);  // entry作为SceneNode？
    }
    return 0;
}
```

**问题：** `WorldObjectEntry`和`SceneNode`不是同一个类型！

### 方案B：`WorldObjectListEntry`包含`WorldObjectEntry`指针

```c
struct WorldObjectListEntry {
    WorldObjectEntry* objectEntry;  // +0x00
    // +0x04-0x17: 其他字段
    // total: 24 bytes
};
```

```cpp
// WorldObjects_RenderGroup
WorldObjectListEntry* v5 = (WorldObjectListEntry*)listPtr[12];
int count = listPtr[20];

for (int i = 0; i < count; i++) {
    result = WorldObjectEntry_Render((int*)v5[i].objectEntry, v7);
}
```

**但是：** `WorldObjectEntry_Render`内部访问`v2[8]`，这要求参数直接指向`WorldObjectEntry`，不是包装后的结构。

### 方案C：混合类型（最可能）

`RenderQueue_AddBatch`的参数可能是`WorldObjectEntry`，它包含`SceneNode`需要的所有字段：

```c
struct WorldObjectEntry {
    void* vtable;           // +0x00
    // +0x04-0x03 (7 dwords)
    SceneNode* sceneNode;     // +0x20 (第8个dword)
    // ... 其他字段
};

struct SceneNode {
    // ...
    uint32_t renderableCount;  // +0x0C
    void* renderableList;      // +0x10
    void* cullTable;          // +0x20
    // ...
};
```

**但偏移不匹配！**
- `SceneNode::renderableList`在+0x10
- `RenderBatch_Submit`访问`*(this + 4)`，即第2个dword

**结论：** 参数类型不是简单的`SceneNode*`或`WorldObjectEntry*`

## 最终假设：`RenderQueue_AddBatch`接收`WorldObjectEntry`，但偏移映射不同

根据`RenderBatch_Submit`的访问模式：
- `*(this + 3)` = renderableCount
- `*(this + 4)` = renderableList
- `*(this + 8)` = cullTable

而`RenderQueue_AddBatch`访问：
- `*(this + 148)` = flags
- `*(this + 156)` = childTable
- `*(this + 196)` = childCount
- `*(this + 200)` = childVisFlags

**这意味着：** `this`参数的类型在两个函数中不同！

### 假设：`RenderQueue_AddBatch`有多个重载

可能性：
1. 同名但不同函数
2. 函数指针间接调用
3. 模板函数

**最可能：** `WorldObjectEntry_Render`调用的是另一个`RenderQueue_AddBatch`，不是`0x6f139190`！

## 需要确认

1. 使用IDA查找所有`RenderQueue_AddBatch`的定义
2. 检查`WorldObjectEntry_Render`调用的具体函数地址
3. 分析`WorldObjectEntry`的完整结构
4. 理解为什么`RenderBatch_Submit`和`RenderQueue_AddBatch`访问不同的偏移

## 暂时方案

由于类型不明确，暂时保持当前实现，但添加详细注释：

```cpp
// 注意：RenderQueue_AddBatch的参数类型有歧义
// - RenderBatch_Submit期望SceneNode*（偏移+3/+4/+8）
// - RenderQueue_AddBatch本身访问偏移+148/+156/+196
// - WorldObjectEntry_Render传递WorldObjectEntry*
// 需要进一步IDA分析确认
```

## 下一步行动

1. 使用IDA查找所有`RenderQueue_AddBatch`的交叉引用
2. 确认`WorldObjectEntry_Render(0x6F184EE0)`调用的函数地址
3. 分析`WorldObjectEntry`的完整结构
4. 可能需要Hook或直接使用游戏内存来运行时检查类型