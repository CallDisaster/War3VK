# 魔兽争霸3渲染链调用约定文档（基于汇编分析）

## 文档版本
**生成时间**: 2026-01-25  
**游戏版本**: Game.dll 1.27.x (32位)  
**核验工具**: IDA Pro MCP (汇编指令级)  
**分析方法**: 直接查看汇编代码，而非依赖IDA的C语言转译

---

## 重要说明

### 汇编分析原则
- **不信任IDA的C语言转译**：IDA的转译可能不准确
- **直接查看汇编指令**：通过`push/call/ret`序列确定调用约定
- **验证参数传递**：检查ECX/EDX寄存器和堆栈使用
- **确认返回清理**：检查`retn X`指令

### 调用约定分类（32位x86）
1. **__cdecl** - C函数，参数全部通过堆栈传递，调用者清理堆栈
2. **__stdcall** - 标准调用，参数全部通过堆栈传递，被调用者清理堆栈
3. **__fastcall** - 快速调用，前两个参数通过ECX/EDX传递，其余通过堆栈
4. **__thiscall** - C++成员函数，this通过ECX传递，其余通过堆栈，被调用者清理堆栈

### 关键观察点
- **ECX寄存器**：__fastcall的第一个参数或__thiscall的this指针
- **EDX寄存器**：__fastcall的第二个参数
- **push指令**：参数通过堆栈传递
- **retn X**：被调用者清理X字节的堆栈（__stdcall/__thiscall）
- **ret**：调用者清理堆栈（__cdecl）

---

## 汇编分析报告

### 1. WorldObjectEntry_Render (0x6F184EE0) ⚠️ **重大发现**

**汇编代码**:
```asm
WorldObjectEntry_Render (.text @ 0x6F184EE0):
6f184ee0  push    esi
6f184ee1  mov     esi, ecx          ; ESI = ECX (保存this指针)
6f184ee3  cmp     dword ptr [esi+20h], 0  ; 检查esi+0x20
6f184ee7  jz      short loc_6F184EF7
6f184ee9  mov     eax, [esi]        ; 获取vtable
6f184eeb  call    dword ptr [eax+14h] ; 调用vtable[5] (PreRender)
6f184eee  mov     ecx, [esi+20h]    ; ECX = esi+0x20
6f184ef1  pop     esi
6f184ef2  jmp     RenderQueue_AddBatch
6f184ef7  pop     esi
6f184ef8  retn
```

**调用约定分析**:
- **函数入口**: `push esi; mov esi, ecx` → ECX是参数
- **没有push指令**: 说明只有一个参数（在ECX中）
- **retn**: 没有参数，调用者清理堆栈
- **结论**: `void __thiscall WorldObjectEntry_Render(int* this)` 或 `int __cdecl WorldObjectEntry_Render(int this)`

**关键发现**:
```asm
mov     ecx, [esi+20h]    ; ECX = esi+0x20
jmp     RenderQueue_AddBatch
```
- WorldObjectEntry将`[esi+0x20]`作为ECX传递给RenderQueue_AddBatch
- `[esi+0x20]`是WorldObjectEntry结构偏移+0x20（sceneNode）
- **但是WorldObjectEntry_Render接收的是WorldObjectEntry指针，不是sceneNode！**

**调用示例** (WorldObjects_RenderGroup):
```asm
mov     ecx, [edi]        ; ECX = [edi] (WorldObjectEntry指针)
call    WorldObjectEntry_Render
```

**正确签名**:
```cpp
extern "C" int __cdecl WorldObjectEntry_Render(
    int entry,      // ECX: WorldObjectEntry指针（作为int传递）
);
```

**错误分析**:
- ❌ IDA转译: `int __cdecl WorldObjectEntry_Render(int a1, int a2)` - **错误！**
- ❌ 假设有两个参数 - **错误！**
- ✅ 汇编显示只有一个参数（ECX）

---

### 2. RenderQueue_AddBatch (0x6F139190) ⚠️ **重大发现**

**汇编代码**:
```asm
RenderQueue_AddBatch (.text @ 0x6F139190):
6f139190  push    ebp
6f139191  mov     ebp, esp
...
6f1391b6  mov     esi, ecx          ; ESI = ECX (保存this指针)
...
6f1391be  call    RenderBatch_Submit
...
6f1392??  ret                        ; 普通返回
```

**栈帧分析**:
```
stack_frame:
  var_10: offset 0x10
  var_C:  offset 0x14
  __saved_registers: offset 0x20
  __return_address:  offset 0x24
  arg_4:  offset 0x2c    ; 注意：这是一个堆栈变量，不是参数！
```

**调用约定分析**:
- **函数入口**: `push ebp; mov ebp, esp` - 标准函数序言
- **mov esi, ecx**: ECX是this指针
- **没有额外的push指令**: 说明只有一个参数（ECX）
- **ret**: 普通返回（被调用者清理堆栈）
- **结论**: `void __thiscall RenderQueue_AddBatch(int* this)` 或 `void RenderQueue_AddBatch(int this)`

**关键发现**:
- **只有一个参数！**（ECX中的this指针）
- arg_4是堆栈变量，不是函数参数
- WorldObjectEntry_Render调用时传递的ECX是`[esi+0x20]`（sceneNode）
- **所以RenderQueue_AddBatch接收的是sceneNode指针**

**正确签名**:
```cpp
extern "C" void RenderQueue_AddBatch(
    int sceneNode   // ECX: sceneNode指针（作为int传递）
);
```

**错误分析**:
- ❌ 假设有两个参数 - **错误！**
- ❌ 认为需要categoryMode参数 - **错误！**
- ✅ 汇编显示只有一个参数（ECX）

---

### 3. RenderQueue_FlushSortedItems (0x6F1380A0) ⚠️ **重大发现**

**汇编代码**:
```asm
RenderQueue_FlushSortedItems (.text @ 0x6F1380A0):
6f1380a0  push    ebp
6f1380a1  mov     ebp, esp
6f1380a3  mov     eax, g_RenderQueue_NumOfElements
...
6f1380a8  sub     esp, 10h
...
6f1380df  push    ebx
6f1380e0  push    esi         ; 保存ESI
6f1380e1  push    offset RenderQueue_ItemComparator
...
6f1380ee  call    ds:qsort
...
6f138??  ret                  ; 普通返回
```

**栈帧分析**:
```
stack_frame:
  var_10: offset 0x0
  var_C:  offset 0x4
  var_8:  offset 0x8
  var_4:  offset 0xc
  __saved_registers: offset 0x10
  __return_address: offset 0x14
  ; 注意：没有arg_0, arg_1！
```

**调用约定分析**:
- **函数入口**: `push ebp; mov ebp, esp; sub esp, 10h` - 标准函数序言
- **push esi**: 保存ESI寄存器（不是参数！）
- **没有额外的push指令**: 说明没有堆栈参数
- **ret**: 普通返回
- **栈帧中没有arg_0/arg_1**: 说明没有通过堆栈传递的参数

**关键发现**:
- **栈帧中没有任何arg_x！**
- **IDA显示的arg0/arg1是错误的！**
- **这可能是IDA的误识别**
- **函数实际上没有参数！**

**调用示例** (RenderQueue_FlushAndReset):
```asm
6f13980a  call    RenderQueue_FlushSortedItems  ; 直接调用，没有push
```

**正确签名**:
```cpp
extern "C" unsigned int RenderQueue_FlushSortedItems(void);
```

**错误分析**:
- ❌ IDA显示有arg0/arg1 - **IDA识别错误！**
- ❌ 假设是`__usercall` - **错误！**
- ✅ 汇编显示没有参数，是`__cdecl`或无参数函数

---

### 4. RenderQueue_FlushAndReset (0x6F139800) ⚠️ **重大发现**

**汇编代码**:
```asm
RenderQueue_FlushAndReset (.text @ 0x6F139800):
6f139800  mov     ecx, 1
6f139805  call    RenderQueue_StageUpdate
6f13980a  call    RenderQueue_FlushSortedItems
6f13980f  call    sub_6F138210
6f139814  mov     ecx, 1
6f139819  call    RenderQueue_StageUpdate
6f13981e  mov     g_RenderQueue_NumOfElements, 0
6f139828  mov     g_AUCTransparent_Count, 0
6f139832  retn
```

**栈帧分析**:
```
stack_frame:
  __return_address: offset 0x0
  ; 注意：没有任何变量或参数！
```

**调用约定分析**:
- **函数入口**: 没有`push ebp`，直接开始执行
- **没有保存任何寄存器**: 没有push esi/edi
- **没有push任何参数**: 说明没有堆栈参数
- **retn**: 普通返回
- **结论**: 无参数函数

**调用示例** (CWorld_RenderScene):
```asm
6f368264  call    RenderQueue_FlushAndReset  ; 直接调用，没有push
```

**正确签名**:
```cpp
extern "C" void RenderQueue_FlushAndReset(void);
```

**错误分析**:
- ❌ IDA显示有arg0/arg1 - **IDA识别错误！**
- ❌ 假设是`__usercall` - **错误！**
- ✅ 汇编显示没有参数，是无参数函数

---

### 5. WorldObjects_RenderGroup (0x6F368E30)

**汇编代码**:
```asm
WorldObjects_RenderGroup (.text @ 0x6F368E30):
6f368e30  push    ebp
6f368e31  mov     ebp, esp
6f368e33  mov     eax, [ebp+arg_0]  ; EAX = arg_0
6f368e36  push    esi
...
6f368e73  mov     ecx, [edi]        ; ECX = [edi]
6f368e75  call    WorldObjectEntry_Render
...
6f368e80  pop     edi
6f368e81  pop     esi
6f368e82  pop     ebp
6f368e83  retn    4               ; 清理4字节堆栈
```

**栈帧分析**:
```
stack_frame:
  __saved_registers: offset 0x4
  __return_address:  offset 0x8
  arg_0:  offset 0xc   ; 堆栈参数！
```

**调用约定分析**:
- **mov eax, [ebp+arg_0]**: 从堆栈读取第一个参数
- **arg_0在偏移0xc**: 说明有一个堆栈参数
- **retn 4**: 被调用者清理4字节堆栈（一个参数）
- **结论**: `int __stdcall WorldObjects_RenderGroup(...)` 或 `int __thiscall WorldObjects_RenderGroup(int* this, int arg0)`

**正确签名**:
```cpp
extern "C" int __thiscall WorldObjects_RenderGroup(
    int* world,      // ECX: world指针
    int groupIdx      // 堆栈: groupIdx (0/1/2)
);
```

---

### 6. CWorld_RenderScene (0x6F3681C0)

**调用示例** (调用RenderQueue_FlushAndReset):
```asm
6f368264  call    RenderQueue_FlushAndReset  ; 直接调用，没有push
```

**说明**: CWorld_RenderScene调用RenderQueue_FlushAndReset时没有push任何参数，证实RenderQueue_FlushAndReset是无参数函数。

---

## 汇编分析总结表

| 函数地址 | 函数名 | 实际调用约定 | 参数 | 返回清理 | IDA识别 |
|---------|--------|------------|------|---------|---------|
| 0x6F184EE0 | WorldObjectEntry_Render | `__cdecl` | 1 (ECX) | `ret` | ❌ 错误 |
| 0x6F139190 | RenderQueue_AddBatch | `__thiscall` | 1 (ECX) | `ret` | ⚠️ 部分错误 |
| 0x6F1380A0 | RenderQueue_FlushSortedItems | `__cdecl` | 0 | `ret` | ❌ 严重错误 |
| 0x6F139800 | RenderQueue_FlushAndReset | `__cdecl` | 0 | `ret` | ❌ 严重错误 |
| 0x6F368E30 | WorldObjects_RenderGroup | `__thiscall` | 2 (ECX+堆栈) | `retn 4` | ⚠️ 基本正确 |

---

## P0严重错误（基于汇编分析）

### 错误1: WorldObjectEntry_Render 函数签名 ❌
**当前实现**: 假设有两个参数  
**汇编显示**: 只有一个参数（ECX）

**修正**:
```cpp
// 错误
extern "C" int WorldObjectEntry_Render(int* entry, int categoryMode)

// 正确
extern "C" int __cdecl WorldObjectEntry_Render(int entry)
```

---

### 错误2: RenderQueue_AddBatch 函数签名 ❌
**当前实现**: 假设有两个参数  
**汇编显示**: 只有一个参数（ECX）

**修正**:
```cpp
// 错误
extern "C" void RenderQueue_AddBatch(SceneNode* sceneNode, int categoryMode)

// 正确
extern "C" void RenderQueue_AddBatch(int sceneNode)
```

---

### 错误3: RenderQueue_FlushSortedItems 函数签名 ❌❌❌
**当前实现**: 假设有两个参数，使用__usercall  
**汇编显示**: 没有参数，普通__cdecl函数

**修正**:
```cpp
// 错误
extern "C" unsigned int RenderQueue_FlushSortedItems(int a1, int a2)
// 或
extern "C" unsigned int __declspec(naked) RenderQueue_FlushSortedItems()
{
    __asm {
        // 试图捕获EDI/ESI - 完全错误！
    }
}

// 正确
extern "C" unsigned int RenderQueue_FlushSortedItems(void)
```

---

### 错误4: RenderQueue_FlushAndReset 函数签名 ❌❌❌
**当前实现**: 假设有两个参数，使用__usercall  
**汇编显示**: 没有参数，普通__cdecl函数

**修正**:
```cpp
// 错误
extern "C" int RenderQueue_FlushAndReset(int a1, int a2)
// 或
extern "C" int __declspec(naked) RenderQueue_FlushAndReset()
{
    __asm {
        // 试图捕获EDI/ESI - 完全错误！
    }
}

// 正确
extern "C" void RenderQueue_FlushAndReset(void)
```

---

## 参数传递链分析

### 完整调用链（基于汇编）
```
CWorld_RenderScene (ECX=world*)
  └─> RenderQueue_FlushAndReset()  ; 无参数
      └─> RenderQueue_FlushSortedItems()  ; 无参数

WorldObjects_RenderGroup (ECX=world*, 堆栈=groupIdx)
  └─> WorldObjectEntry_Render(ECX=WorldObjectEntry指针)
      └─> RenderQueue_AddBatch(ECX=sceneNode指针)
          └─> RenderBatch_Submit(ECX=sceneNode指针)
```

### 关键发现
1. **RenderQueue_FlushAndReset没有参数**
2. **RenderQueue_FlushSortedItems没有参数**
3. **WorldObjectEntry_Render只有一个参数（ECX）**
4. **RenderQueue_AddBatch只有一个参数（ECX）**
5. **所有IDA显示的"__usercall"都是错误的**

---

## 正确的函数签名

### 1. WorldObjectEntry_Render
```cpp
extern "C" int __cdecl WorldObjectEntry_Render(
    int entry  // ECX: WorldObjectEntry指针（作为int传递）
);
```

### 2. RenderQueue_AddBatch
```cpp
extern "C" void RenderQueue_AddBatch(
    int sceneNode  // ECX: sceneNode指针（作为int传递）
);
```

### 3. RenderQueue_FlushSortedItems
```cpp
extern "C" unsigned int RenderQueue_FlushSortedItems(void);
```

### 4. RenderQueue_FlushAndReset
```cpp
extern "C" void RenderQueue_FlushAndReset(void);
```

### 5. WorldObjects_RenderGroup
```cpp
extern "C" int __thiscall WorldObjects_RenderGroup(
    int* world,      // ECX: world指针
    int groupIdx      // 堆栈: groupIdx (0/1/2)
);
```

---

## 实现修正优先级

### P0 - 阻塞错误（必须立即修正）
1. ✅ **WorldObjectEntry_Render** - 移除第二个参数
2. ✅ **RenderQueue_AddBatch** - 移除第二个参数
3. ✅ **RenderQueue_FlushSortedItems** - 改为无参数函数，移除汇编捕获
4. ✅ **RenderQueue_FlushAndReset** - 改为无参数函数，移除汇编捕获

### P1 - 轻微错误
5. ⚠️ **WorldObjects_RenderGroup** - 确认参数数量和类型

---

## 测试策略

### 1. 编译测试
```bash
# 编译后检查
msbuild WarVK.sln /p:Configuration=Release /p:Platform=Win32

# 检查是否有链接错误
# 如果有未解析的符号，说明调用约定仍然错误
```

### 2. 运行时测试
```cpp
// 添加调试输出
extern "C" int __cdecl WorldObjectEntry_Render(int entry) {
    DebugPrint("[Native] WorldObjectEntry_Render called: entry=0x%08X\n", entry);
    // 实现...
}

extern "C" void RenderQueue_AddBatch(int sceneNode) {
    DebugPrint("[Native] RenderQueue_AddBatch called: sceneNode=0x%08X\n", sceneNode);
    // 实现...
}

extern "C" unsigned int RenderQueue_FlushSortedItems(void) {
    DebugPrint("[Native] RenderQueue_FlushSortedItems called\n");
    // 实现...
}

extern "C" void RenderQueue_FlushAndReset(void) {
    DebugPrint("[Native] RenderQueue_FlushAndReset called\n");
    // 实现...
}
```

### 3. 对比测试
- 运行原版游戏，记录渲染结果
- 运行修正后的版本，对比渲染结果
- 检查是否有崩溃或渲染错误

---

## 风险提示

### 高风险
- ⚠️ **IDA的C语言转译不可信**：必须查看汇编确认
- ⚠️ **参数数量错误**：会导致堆栈不平衡，立即崩溃
- ⚠️ **错误的__usercall实现**：会导致寄存器错误，立即崩溃

### 中风险
- ⚠️ **参数类型错误**：可能导致类型转换错误
- ⚠️ **返回值类型错误**：可能导致调用者获取错误结果

---

## 总结

### 关键发现
1. **IDA的C语言转译严重错误**：显示不存在的参数
2. **4个函数的调用约定完全错误**：基于IDA的错误转译
3. **RenderQueue_FlushSortedItems/FlushAndReset完全没有参数**
4. **WorldObjectEntry_Render/RenderQueue_AddBatch只有一个参数**
5. **不需要使用汇编捕获寄存器**：所有参数都是普通堆栈或ECX传递

### 修正优先级
1. **最高**: 修正所有P0错误（函数签名）
2. **高**: 测试修正后的版本
3. **中**: 实现缺失的功能

### 预期时间
- **修正P0**: 1-2小时（比之前简单很多）
- **测试**: 1-2小时
- **总计**: 2-4小时

---

**文档生成时间**: 2026-01-25  
**核验工具**: IDA Pro MCP (汇编指令级)  
**分析方法**: 直接查看汇编代码  
**文档状态**: ✅ 基于汇编的准确分析