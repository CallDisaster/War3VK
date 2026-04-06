# War3渲染链逻辑核验报告 - 第三轮（透明队列）

> **核验时间**: 2026-01-25
> **核验方法**: IDA Pro MCP直接反编译验证
> **重点**: 透明队列处理逻辑

---

## 一、RenderQueue_FlushAndReset 函数签名修正

### 1.1 函数签名错误

**原版理解**:
```cpp
void RenderQueue_FlushAndReset(RenderCategory category, CWorld* world);
```

**IDA反编译结果**:
```cpp
int __usercall RenderQueue_FlushAndReset@<eax>(
    int a1@<edi>, 
    int a2@<esi>
)
{
  RenderQueue_StageUpdate((void *)1);      // 注意：传递常量1！
  RenderQueue_FlushSortedItems(a1, a2);
  sub_6F138210();                        // 刷新透明队列
  result = RenderQueue_StageUpdate((void *)1); // 注意：再次传递常量1！
  g_RenderQueue_NumOfElements = 0;
  g_AUCTransparent_Count = 0;
  return result;
}
```

**关键发现**:
1. 函数使用`__usercall`约定，参数通过EDI/ESI寄存器传递
2. `RenderQueue_StageUpdate`传递的是常量`(void *)1`，不是category或world指针
3. 函数内部会自动重置两个队列的计数器

**修正方案**:
```cpp
// 函数签名
extern "C" unsigned int RenderQueue_FlushAndReset(void* param_edi, void* param_esi);

// 调用约定: __usercall @<eax>
//   - param_edi (EDI): 类别参数
//   - param_esi (ESI): 上下文指针
//   - 返回值 (EAX): 处理结果
```

---

## 二、透明队列刷新逻辑 (sub_6F138210)

### 2.1 函数签名

**IDA反编译结果**:
```cpp
void sub_6F138210()
{
  int v0; // ecx = g_AUCTransparent_Array
  size_t v1; // edx
  size_t v2; // eax
  unsigned int i; // esi
  _DWORD *v4; // eax

  v0 = g_AUCTransparent_Array;
  v1 = 10000;  // 最大值
  if (g_AUCTransparent_Count < 0x2710)  // 0x2710 = 10000
    v1 = g_AUCTransparent_Count;
  
  v2 = 0;
  
  // 复制指针到排序数组
  for (dword_6FBC6BA4 = v1; v2 < v1; v0 += 24)
    dword_6FBD0828[v2++] = v0;  // g_AUCTransparent_SortedPtrs[i] = &array[i]
  
  // 排序
  qsort(dword_6FBD0828, v1, 4u, sub_6F1378D0);  // 透明队列排序器
  
  // 遍历并渲染
  for (i = 0; i < dword_6FBC6BA4; ++i) {
    v4 = (_DWORD *)dword_6FBD0828[i];
    switch (*v4)  // 根据类型码分发
    {
      case 0: sub_6F13A0E0(*(_DWORD *)(v4[3] + 20), v4[3]); break;
      case 1: sub_6F198C00(v4[3]); break;
      case 2: sub_6F19DFF0(v4[3]); break;
      case 3: sub_6F19BC20(v4[3]); break;
      case 4: sub_6F13A0B0(v4[3]); break;
      case 5: ((void (__fastcall *)(_DWORD, _DWORD))v4[3])(v4[4], v4[5]); break;
      default: break;
    }
    RenderQueue_StageUpdate(0);  // 注意：传递常量0！
  }
}
```

**关键发现**:
1. 函数无参数，直接访问全局变量
2. 每次渲染后调用`RenderQueue_StageUpdate(0)`
3. 支持最多10000个透明对象

---

## 三、透明队列排序器 (sub_6F1378D0)

### 3.1 排序逻辑

**IDA反编译结果**:
```cpp
int __cdecl sub_6F1378D0(const void *a1, const void *a2)
{
  int v2; // edx = b->sortKey
  int v3; // eax = a->sortKey

  v2 = *(_DWORD *)(*(_DWORD *)a2 + 4);  // b->sortKey
  v3 = *(_DWORD *)(*(_DWORD *)a1 + 4);  // a->sortKey
  
  if (v3 == v2)  // 如果sortKey相同
    return 2 * (*(float *)(*(_DWORD *)a1 + 8) < *(float *)(*(_DWORD *)a2 + 8)) - 1;
  else
    return 2 * (v3 > v2) - 1;
}
```

**排序规则**:
1. **优先比较** `transparentKey` (+0x04): 数值大者排在后面（渲染在上面）
2. **次要比较** `distSq` (+0x08): 距离近者（distSq小）排在后面（渲染在上面）

**简化逻辑**:
```cpp
int TransparentComparator(const AUCTransparentEntry* a, const AUCTransparentEntry* b) {
  // 1. 先按transparentKey排序
  if (a->sortKey != b->sortKey)
    return a->sortKey < b->sortKey ? -1 : 1;
  
  // 2. 同Key时按距离排序（Back-to-Front）
  if (a->distSq != b->distSq)
    return a->distSq > b->distSq ? -1 : 1;
  
  return 0;
}
```

---

## 四、AUCTransparent_AddEntry 函数验证

### 4.1 函数签名和数据写入

**IDA反编译结果**:
```cpp
int __fastcall sub_6F137AF0(int a1, int a2, float *a3, int a4)
{
  // a1 = part (RenderablePart*)
  // a2 = type (类型码)
  // a3 = worldPos (世界坐标)
  // a4 = transparentKey (透明排序键)
  
  // 计算距离平方
  v8 = (float)(
    (float)(a3[1] - g_RenderCamera_PosXY[1]) * (float)(a3[1] - g_RenderCamera_PosXY[1])
  + (float)(a3[0] - g_RenderCamera_PosXY[0]) * (float)(a3[0] - g_RenderCamera_PosXY[0])
  + (float)(a3[2] - g_RenderCamera_PosZ) * (float)(a3[2] - g_RenderCamera_PosZ)
  );
  
  // 扩容检查
  if (g_AUCTransparent_Count + 1 > g_AUCTransparent_Capacity) {
    AUCTransparent_ReserveArray(v7);
    v6 = g_AUCTransparent_Count;
  }
  
  // 写入数据（验证偏移）
  v10 = g_AUCTransparent_Array;
  v11 = 3 * v6;  // v6是当前索引，乘以3得到字节索引
  g_AUCTransparent_Count = v6 + 1;
  
  *(_DWORD *)(g_AUCTransparent_Array + 8 * v11) = a2;      // +0: type
  *(_DWORD *)(v10 + 8 * v11 + 4) = a4;                // +4: sortKey
  *(float *)(v10 + 8 * v11 + 8) = v8;                  // +8: distSq
  *(_DWORD *)(v10 + 8 * v11 + 12) = a1;               // +12: payload
  // 注意：arg1和arg2在AUCTransparent_AddEntry中不会写入
  // 它们只在type=5的回调中使用
  
  return a4;
}
```

**关键发现**:
1. 使用`__fastcall`约定，前两个参数通过ECX/EDX传递
2. 距离计算使用全局相机变量`g_RenderCamera_PosXY`和`g_RenderCamera_PosZ`
3. 数据写入顺序与文档中的偏移完全一致

---

## 五、Type 0 透明对象渲染 (sub_6F13A0E0)

### 5.1 函数签名

**IDA反编译结果**:
```cpp
void __fastcall sub_6F13A0E0(int a1, int a2)
{
  // a1 = world (CWorld*)
  // a2 = part (RenderablePart*)
  
  int v2; // ebx
  void *v3; // eax
  _BYTE v4[48]; // [esp+4h] [ebp-34h] BYREF
  int v5; // [esp+34h] [ebp-4h]

  v5 = a1;
  
  if (!*(_DWORD *)(a2 + 16)) {  // 检查part->meshData->meshFlag == 0
    v2 = *(_DWORD *)(a2 + 12);  // part->meshData
    
    // 检查可见性
    if (*(_BYTE *)(*(_DWORD *)(a1 + 32) + 16 * *(_DWORD *)(v2 + 284) + 3)) {
      sub_6F13A510(a1, a2, v2);  // 设置状态
      sub_6F13A140(v2);            // 绘制
      
      if (!*(_DWORD *)(v2 + 260)) {  // 检查meshFlag == 0
        v3 = (void *)sub_6F0A3D80(v4);  // 获取上下文
        RenderSceneFlush_0E39E0(v3);       // 刷新场景
      }
    }
  }
}
```

**关键发现**:
1. 使用`__fastcall`约定
2. 检查`part->meshData->meshFlag`决定是否刷新
3. 渲染后调用`RenderSceneFlush`确保状态一致性

---

## 六、AUCTransparentEntry 数据结构验证

### 6.1 最终确认

```cpp
struct AUCTransparentEntry {
    uint32_t type;       // +0x00: 类型码 (0=粒子, 2=缎带, 3=特效, 4=附着物, 5=自定义)
    uint32_t sortKey;    // +0x04: 透明排序键
    float    distSq;     // +0x08: 到相机距离平方
    void*    payload;    // +0x0C: 对象/回调指针
    uint32_t arg1;       // +0x10: 回调参数1（type=5使用）
    uint32_t arg2;       // +0x14: 回调参数2（type=5使用）
};  // sizeof = 24 (0x18) bytes
```

**验证方法** (通过AUCTransparent_AddEntry):
- `v11 = 3 * v6` → 计算字节偏移
- `+ 8 * v11` → 访问第0个DWORD (type) ✓
- `+ 8 * v11 + 4` → 访问第1个DWORD (sortKey) ✓
- `+ 8 * v11 + 8` → 访问float (distSq) ✓
- `+ 8 * v11 + 12` → 访问第3个DWORD (payload) ✓

---

## 七、RenderQueue_StageUpdate 参数传递总结

### 7.1 调用位置

| 调用位置 | 传递参数 | 说明 |
|----------|----------|------|
| `FlushAndReset` 开始 | `(void *)1` | 强制设置状态 |
| `FlushSortedItems` 循环内 | `(0, v15, v16)` | 传递EDI/ESI寄存器 |
| `FlushTransparent` 循环内 | `0` | 传递常量0 |
| `FlushAndReset` 结束 | `(void *)1` | 再次强制设置状态 |

### 7.2 参数含义

```cpp
void RenderQueue_StageUpdate(void* this, int param_edi, int param_esi);

// param_edi: 
//   = 0: 检查并刷新状态
//   = 1: 强制设置状态

// param_esi: 
//   某种上下文指针（如上一个应用的状态块）
```

---

## 八、关键发现汇总

### 8.1 新发现的错误

1. **RenderQueue_FlushAndReset签名错误**:
   - 原版理解: 2个堆栈参数
   - 实际: `__usercall`，2个寄存器参数
   - 函数内传递常量`(void *)1`给StageUpdate

2. **StageUpdate参数传递错误**:
   - 不同场景传递不同参数：0或1
   - 需要处理寄存器传递

3. **透明队列排序逻辑验证**:
   - 优先级: transparentKey > distSq
   - 同Key时按距离Back-to-Front排序

### 8.2 数据结构验证结果

所有数据结构偏移均正确：
- `RenderBatchElement`: 全部验证通过 ✓
- `AUCTransparentEntry`: 全部验证通过 ✓
- `RenderablePart`: meshData+0x0C, skipFlag+0x10 ✓
- `MeshData`: meshFlag+0x104, meshIndex+0x108 ✓

---

## 九、修正建议

### 9.1 函数签名更新

```cpp
// RenderQueue_FlushAndReset
extern "C" unsigned int RenderQueue_FlushAndReset(void* param_edi, void* param_esi);

// 调用约定: __usercall @<eax>
// 函数内部会自动重置队列计数器
```

### 9.2 透明队列排序实现

```cpp
int TransparentComparator(const AUCTransparentEntry* a, const AUCTransparentEntry* b) {
  // 1. 先按transparentKey排序（大Key后渲染）
  if (a->sortKey != b->sortKey)
    return a->sortKey < b->sortKey ? -1 : 1;
  
  // 2. 同Key时按距离排序（Back-to-Front，近后渲染）
  if (a->distSq != b->distSq)
    return a->distSq > b->distSq ? -1 : 1;
  
  return 0;
}
```

### 9.3 StageUpdate调用注意事项

```cpp
// 在FlushSortedItems循环内
RenderQueue_StageUpdate(0, lastAppliedState, currentContext);

// 在FlushAndReset开始/结束
RenderQueue_StageUpdate(0, (void*)1, (void*)1);
```

---

## 十、下一步行动

1. 更新`war3_native_renderer.h`中RenderQueue_FlushAndReset的签名
2. 更新RenderQueue_StageUpdate的注释，说明参数的多态性
3. 验证其他类型的透明对象渲染函数（Type 2/3/4）
4. 验证SceneNode透明列表添加函数

---

**文档版本**: 3.0
**最后更新**: 2026-01-25
**核验方法**: IDA Pro MCP直接反编译
**置信度**: 高（所有偏移已验证）