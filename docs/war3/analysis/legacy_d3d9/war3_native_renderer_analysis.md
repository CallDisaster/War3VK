# War3 Native Renderer - IDA分析报告

## 问题总结

经过对比IDA Pro的真实反编译代码，发现了多处关键错误：

### 1. `WorldObjectEntry_Render` 参数传递错误

**错误理解：**
```cpp
// 错误：传递entry->sceneNode
RenderQueue_AddBatch((SceneNode*)entry->sceneNode, categoryMode);
```

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

**正确实现：**
```cpp
return RenderQueue_AddBatch((void*)entry, categoryMode);
```

### 2. `RenderQueue_AddBatch` 参数类型混淆

根据IDA反编译 `RenderQueue_AddBatch(0x6f139190)`：

```c
void __thiscall RenderQueue_AddBatch(int this)
{
  int v2 = *(_DWORD *)(this + 156);
  RenderBatch_Submit((_DWORD *)this);  // this传递给RenderBatch_Submit
  
  if ( (*(_BYTE *)(this + 148) & 0x10) != 0 )
  {
    // 透明列表处理
    SceneNode_AddTransparentList0(this, v2);
    // ...
    
    // 子节点递归
    v3 = 0;
    if ( *(_DWORD *)(this + 196) ) {
      v4 = (int *)(*(_DWORD *)(this + 200) + 8);
      do {
        // 可见性检查
        if ( !*(_DWORD *)(this + 152) || ... ) {
          v7 = *v4;
          if ( *v4 > 0 ) {
            do {
              RenderQueue_AddBatch(v8, v9);  // 递归调用！
              v7 = *(_DWORD *)(v7 + 4);
            } while ( v7 > 0 );
            v4 = v10;
          }
        }
        ++v3;
        v4 += 3;
        v10 = v4;
      } while ( v3 < *(_DWORD *)(this + 196) );
    }
  }
}
```

**关键发现：**
1. `RenderQueue_AddBatch`接收的参数类型与`RenderBatch_Submit`相同
2. 内部使用`this + 156`, `this + 148`, `this + 196`等偏移
3. 递归调用`RenderQueue_AddBatch(v8, v9)`，传递两个参数

**问题：**
- 如果参数是`WorldObjectEntry*`，那么偏移+148（sceneNode->flags）和+156（sceneNode->childTable）是合理的
- 但递归调用传递的是`v8`和`v9`，其中`v8`似乎是从`*v4`解引用得到的

### 3. `WorldObjects_RenderGroup` 参数传递

**IDA真实代码：**
```c
int __userpurge WorldObjects_RenderGroup@<eax>(_DWORD *a1@<ecx>, int a2@<edi>, int a3)
{
    // ...
    do {
        result = WorldObjectEntry_Render(v7, v8);  // v8是listData指针
        v5 += 24;  // WorldObjectListEntry stride = 24
    } while (i);
}
```

**关键：** `v8 = v5` 是listData指针，指向`WorldObjectListEntry`，不是`WorldObjectEntry*`！

这意味着：
- `WorldObjectEntry_Render`的第一个参数是`WorldObjectListEntry*`，不是`WorldObjectEntry*`
- 需要检查`WorldObjectListEntry`的结构

### 4. 结构偏移确认

根据文档和IDA：

**WorldObjectEntry:**
```
+0x00: vtable
+0x04-0x1C: 未知
+0x20: sceneNode (第8个dword)
```

**WorldObjectListEntry:**
```
stride = 24 (0x18)
内容待分析
```

## 需要修正的内容

### 高优先级
1. ✅ 重写`Native_RenderWorld_DispatchStage`（已完成）
2. ✅ 重写`Native_WorldObjects_RenderGroup`（已完成）
3. ✅ 重写`Native_WorldObjectEntry_Render`（已完成）
4. ❓ 修正`RenderQueue_AddBatch`的参数类型

### 中优先级
5. ❓ 分析`WorldObjectListEntry`结构
6. ❓ 理解递归调用的参数含义
7. ❓ 确认SceneNode的所有偏移

## 下一步行动

1. 使用IDA反编译`RenderBatch_Submit`，确认参数类型
2. 分析`WorldObjectListEntry`的内存布局
3. 理解`RenderQueue_AddBatch`的完整递归逻辑
4. 修正所有相关函数实现

## 修正原则

**严格遵循：**
- IDA反编译代码的逐行翻译
- 不做任何"理解"或"假设"
- 所有结构偏移必须从IDA确认

**避免：**
- "猜测"参数类型
- "推断"函数用途
- 基于文档而非IDA的假设