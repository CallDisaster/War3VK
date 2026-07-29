# GPU 蒙皮 Native ASM 契约

> 核对日期：2026-07-11
> 目标：Warcraft III 1.27a `Game.dll`
> ImageBase：`0x6F000000`
> 版本：`1.27.0.52240`
> MD5：`267861a0dfd416dbad13e7ee3ec7794a`
> SHA1：`88ab432160fb84c23b096c3fc022bfbcb3cb1a1a`
> 证据规则：本页结论来自 IDA MCP 的真实 x86 ASM、xref 与 vtable 数据；伪代码不作为 ABI 证据。
> P4 最终裁决：完整保留 `0x6F0EEA50`；只在 `0x6F0EDDC0` 收到真实非空 mapped pointer 后 gate CPU 写循环。
>
> 2026-07-15 类名纠偏：本页历史 `CWorld*` 短名在以下 stage 链中均指
> `CWorldFrameWar3*`，不是 RTTI 类 `CWorldObjects*`。类族 raw RTTI/字段/vtable 与 Unknown
> 边界见 [30_cworld_class_family_full_reverse](../30_cworld_class_family_full_reverse/README.md)。
> 同日进一步确认 `0x6F184EE0` 的 ECX 为 `CSprite*`；旧 `WorldObjectEntry_Render` 名和
> slot5=PreRender 解释已 supersede，但 producer-time stage/group sidecar 边界不变。

## 1. 最终地址表

| 角色 | VA | RVA | IDA 名称 | ABI 摘要 |
|---|---:|---:|---|---|
| WorldFrame 主场景调度 | `0x6F3681C0` | `0x3681C0` | `CWorldFrameWar3_RenderScene` | `ECX=CWorldFrameWar3*`；Stage 11 返回后才调用 queue flush |
| WorldFrame stage 分发 | `0x6F363020` | `0x363020` | `CWorldFrameWar3_DispatchStage` | `thiscall`；栈参依次为 `stageId/renderMode/categoryMask/activeQueue`，`retn 10h` |
| WorldGroup producer | `0x6F368E30` | `0x368E30` | `CWorldFrameWar3_RenderWorldGroup` | `ECX=CWorldFrameWar3*`，`[ESP+4]=groupIdx`，`retn 4` |
| CSprite attached-object producer | `0x6F184EE0` | `0x184EE0` | `CSprite_PrepareAndQueueAttachedRenderObject` | `ECX=CSprite*`；非空 `+0x20` 时调 vslot5并尾跳 `RenderQueue_AddBatch` |
| opaque queue record writer | `0x6F1375C0` | `0x1375C0` | `RenderBatch_Submit` | `thiscall`；写 20-byte record，record 内无 CWorld stage/tag |
| queue flush + reset | `0x6F139800` | `0x139800` | `RenderQueue_FlushAndReset` | 无参数；调用 sorted/transparent flush 后清零队列 |
| 排序队列 flush | `0x6F1380A0` | `0x1380A0` | `RenderQueue_FlushSortedItems` | 无显式参数，普通 `retn` |
| common queue dispatch | `0x6F13A5E0` | `0x13A5E0` | `RenderQueue_Dispatch_Common` | `ECX=sceneNode`，`EDX=renderablePart`，3 个栈参，`retn 0Ch` |
| special queue dispatch | `0x6F13A780` | `0x13A780` | `RenderQueue_Dispatch_Special` | `ECX=sceneNode`，`EDX=renderablePart`，2 个栈参，`retn 8` |
| Gx stage 全量更新 | `0x6F13A9B0` | `0x13A9B0` | `RenderQueue_UpdateGxStages` | `ECX=force`；这里的 stage 不是 CWorld stage |
| Stage 11 前置 TerrainShadow producer | `0x6F7668D0` | `0x7668D0` | `TerrainShadow_Selector12_EnqueueBatches` | 无参数；可调用 `RenderQueue_AddBatch` |
| 非渲染命令分发 | `0x6F7B28F0` | `0x7B28F0` | `BattleNet_DispatchCommandOpcode` | 第二个栈参低字节是 `0..0x82` opcode，不是 CWorld stage |
| 非 scene-batch 入口 | `0x6F7B7830` | `0x7B7830` | `BattleNet_ApplyICONDATARecord` | `BattleNet::ICONDATA` record 路径，不具备 RenderQueue 语义 |
| geoset draw-state / vertex input 提交 | `0x6F138EE0` | `0x138EE0` | `RenderQueue_ApplyDrawStateAndSamplerPair` | `ECX=CGeosetData*`，`EDX=layerDispatch*`，栈上 1 参数，`retn 4` |
| 普通单 slice 提交 | `0x6F138F70` | `0x138F70` | `CGeosetData_SubmitSingleIndexedSlice` | `thiscall`，一次 ApplyDrawState + 一次 index range，`retn 4` |
| Gx vertex wrapper | `0x6F0E35B0` | `0x0E35B0` | `GxDevice_UploadDynamicVertices` | fastcall/栈混合 wrapper，归一化 null stride 后走 vtable `+0x68` |
| Gx index wrapper | `0x6F0E3550` | `0x0E3550` | `GxDevice_UploadBindDynamicIndices` | fastcall/栈混合 wrapper，走 vtable `+0x6C` |
| palette bind wrapper | `0x6F0E3920` | `0x0E3920` | `GxDevice_SetSkinPalette` | `ECX=groupCount`，`EDX=palette3x4`，走 vtable `+0x58` |
| palette setter | `0x6F0E70E0` | `0x0E70E0` | `CGxDevice_SetSkinPalette` | `this+0x19C=palette`，`this+0x1A0=count`，`retn 8` |
| vertex submit 统计 | `0x6F0E5BC0` | `0x0E5BC0` | `CGxDevice_RecordSubmittedVertexCount` | 只读首参数，`this+0x5C += count`，`retn 34h` |
| dynamic VB/IB ensure | `0x6F0EDD10` | `0x0EDD10` | `CGxDeviceD3d_EnsureDynamicVertexAndIndexBuffers` | 已存在则跳过创建，成功 1 / 失败 0，`retn 14h` |
| CPU skin / copy kernel | `0x6F0EDDC0` | `0x0EDDC0` | `CGxDeviceD3d_SkinCopyVerticesToMappedVB` | `ECX=CGxDeviceD3d*`，栈上 mapped-VB 目标，`retn 4` |
| dynamic index ring lock | `0x6F0EE530` | `0x0EE530` | `CGxDeviceD3d_LockDynamicIndexRing` | capacity `0xC000`，`retn 4` |
| dynamic vertex ring lock | `0x6F0EE5D0` | `0x0EE5D0` | `CGxDeviceD3d_LockDynamicVertexRing` | capacity `0x4000`，format 分槽，`retn 4` |
| dynamic vertex upload | `0x6F0EEA50` | `0x0EEA50` | `CGxDeviceD3d_UploadDynamicVertices` | `thiscall`，13 个栈参数，`retn 34h` |
| dynamic index upload / bind | `0x6F0EEC20` | `0x0EEC20` | `CGxDeviceD3d_UploadBindDynamicIndices` | `thiscall`，3 个栈参数，`retn 0Ch` |
| indexed primitive batch flush / actual DIP | `0x6F0EE9F0` | `0x0EE9F0` | `CGxDeviceD3d_FlushIndexedPrimitiveBatch` | `thiscall`，无栈参数，普通 `retn` |

### 1.1 已纠正的旧入口错误

`RenderQueue_FlushSortedItems` 的真实函数头是 `0x6F1380A0`，不是旧文档中的 `0x6F1380B0`。

ASM 从 `0x6F1380A0` 开始建立栈帧：

```asm
6F1380A0  push ebp
6F1380A1  mov  ebp, esp
6F1380A3  mov  eax, g_RenderQueue_NumOfElements
6F1380A8  sub  esp, 10h
6F1380AB  test eax, eax
6F1380AD  jz   loc_6F138205
```

`0x6F1380B0` 位于这段开头控制流的指令字节内部，不能作为 MinHook target。后续地址簿、文档和 hook 必须只使用 RVA `0x1380A0`。

## 2. 原生数据流

普通 geoset 的关键链路为：

```text
RenderQueue_FlushSortedItems                    0x6F1380A0
  -> Common / Special dispatch
  -> RenderQueue_ApplyDrawStateAndSamplerPair   0x6F138EE0
  -> Gx dynamic-vertex wrapper                  0x6F0E35B0
  -> vtable +0x68
  -> CGxDeviceD3d_UploadDynamicVertices         0x6F0EEA50
     -> CGxDeviceD3d_SkinCopyVerticesToMappedVB 0x6F0EDDC0
     -> VB Unlock / SetFVF / SetStreamSource
  -> Gx indexed-range wrapper                   0x6F0E3550
  -> vtable +0x6C
  -> CGxDeviceD3d_UploadBindDynamicIndices      0x6F0EEC20
  -> batch flush through vtable +0x70
  -> CGxDeviceD3d_FlushIndexedPrimitiveBatch    0x6F0EE9F0
  -> IDirect3DDevice9::DrawIndexedPrimitive     0x6F0EEA43
```

关键点是 `+0x68`、`+0x6C`、`+0x70` 是三个分离的方法。顶点 upload 不是 DIP 本身，index submit 也不等于唯一一次 DIP。

## 3. `RenderQueue_FlushSortedItems`

### 3.1 已确认行为

- 从 `g_RenderQueue_NumOfElements` 读取元素数，上限截断为 `0x2710`，即 10000。
- 以 20 字节步长构建排序指针表并调用 `qsort`。
- 根据 item flag 的低两位分发 `RenderQueue_Dispatch_Special` 或 `RenderQueue_Dispatch_Common`。
- 函数尾部为普通 `retn`，ASM 未消费调用者栈参数。

### 3.2 GPU skin hook 约束

compute 批量提交必须位于这个真实入口、原生与 reimplementation 分叉之前。dispatch 观察域必须覆盖：

- common；
- special；
- fallback multipass；
- 后续明确纳入的 transparent/special 子路径。

只在 common detour 建 TLS scope 会把 special/multipass upload 误报为 `outside-dispatch`。

## 4. `RenderQueue_ApplyDrawStateAndSamplerPair`

### 4.1 ABI

函数入口直接保存寄存器：

```asm
6F138EE6  mov edi, edx
6F138EE8  mov esi, ecx
...
6F138EF5  mov ebx, [ebp+arg_0]
...
6F138F5E  retn 4
```

因此契约是：

| 位置 | 含义 |
|---|---|
| `ECX` | `CGeosetData*` |
| `EDX` | layer dispatch / state record |
| `[ESP+4]` | layer/material record |

它解析最多两层 UV，随后向 `0x6F0E35B0` 提交 positions、normals、group-slot、UV 和 stride。wrapper 在 `0x6F0E362E` 调用设备 vtable `+0x68`。

### 4.2 三个 ASM caller

| call site | 路径 |
|---:|---|
| `0x6F138F79` | 普通路径 |
| `0x6F13A305` | fallback multipass |
| `0x6F13A439` | special/alpha 子路径 |

stage/tag 不是这个 ABI 的显式参数。权威 stage 必须在 queue producer 时写入 sidecar；P1A
的精确 WorldObjects 身份来自 `CWorld_WorldObjects_RenderGroup(groupIdx=0)` 的新增 record
范围。任何 fast return 都必须先查询该 producer sidecar，不能退回 flush-time ambient stage。

## 5. CPU skin / copy kernel

### 5.1 ABI、唯一 caller 与返回值

真实入口/尾声为：

```asm
6F0EDDC0  push ebp
6F0EDDC1  mov  ebp, esp
...
6F0EE1EE  retn 4
```

`0x6F0EDDC0` 只有一个 code xref：`0x6F0EEB85`。调用点先把
`CGxDeviceD3d_LockDynamicVertexRing` 的 `EAX` 压栈，再令 `ECX=CGxDeviceD3d*`：

```asm
6F0EEB71  push [ebp+arg_0]       ; vertexCount
6F0EEB74  mov  ecx, ebx          ; CGxDeviceD3d*
6F0EEB76  call 0x6F0EE5D0       ; EAX = real mapped pointer or NULL
6F0EEB7B  mov  [ebp-4], 0        ; enter SEH try level 0
6F0EEB82  push eax               ; sole kernel stack argument
6F0EEB83  mov  ecx, ebx
6F0EEB85  call 0x6F0EDDC0
6F0EEB8A  mov  [ebp-4], -2
```

因此真实 C++ ABI 是：

```cpp
using SkinCopyKernelFn =
    void (__thiscall*)(void* gxDeviceD3d, void* mappedDst);

static void __fastcall Hook_SkinCopyKernel(
    void* gxDeviceD3d, void* /*edx*/, void* mappedDst);
```

kernel 各出口没有稳定 `EAX` 语义，caller 在 `0x6F0EEB8A` 也不读取返回值，
所以类型必须是 `void`，不能把 Hex-Rays 误推的 4 参数/整数返回当成 ABI。

### 5.2 全部输入来源

`0x6F0EEA50` 在 Lock 前发布 13 个 scratch dword；kernel 的单个栈参数只负责
目标地址，其余输入全部来自 `CGxDeviceD3d` 与这些 globals：

| 地址 / 字段 | IDA 名称 | 含义 | 普通 `CGeosetData` 来源 |
|---:|---|---|---|
| `0x6FBC5EA0` | `g_GxUploadVertexCount` | vertex count | `CGeosetData+0x0C` |
| `0x6FBC5EA4` | `g_GxUploadPositionPtr` | `float3` position | `CGeosetData+0x10` |
| `0x6FBC5EA8` | `g_GxUploadPositionStride` | position stride | 常规路径固定 12 |
| `0x6FBC5EAC` | `g_GxUploadNormalPtr` | `float3` normal | `CGeosetData+0x58`，null 时用 `0x6FB66CB8=(0,1,0)` |
| `0x6FBC5EB0` | `g_GxUploadNormalStride` | normal stride | 常规路径固定 12 |
| `0x6FBC5EB4` | `g_GxUploadExtraPtr` | optional diffuse/extra dword | 常规路径 null，替换为 `0x6FB66CC4=0xFFFFFFFF` |
| `0x6FBC5EB8` | `g_GxUploadExtraStride` | extra stride | 常规路径 0 |
| `0x6FBC5EBC` | `g_GxUploadGroupSlotPtr` | 每顶点 `uint8_t groupSlot` | `CGeosetData+0x4C` |
| `0x6FBC5EC0` | `g_GxUploadGroupSlotStride` | group-slot stride | `CGeosetData+0x48 != 0` 时为 1，否则 0 |
| `0x6FBC5EC4` | `g_GxUploadUv0Ptr` | UV0 `float2` | layer record 经 `CGeosetData+0x94` 解析 |
| `0x6FBC5EC8` | `g_GxUploadUv1Ptr` | UV1 `float2` | layer record 经 `CGeosetData+0x94` 解析 |
| `0x6FBC5ECC` | `g_GxUploadUv0Stride` | UV0 stride | 常规路径 8 |
| `0x6FBC5ED0` | `g_GxUploadUv1Stride` | UV1 stride | 常规路径 8 |
| `this+0x224` | - | skin mode | 0=copy，1=group-palette skin，其他值不写 vertex |
| `this+0x228` | - | output format | 0..5 |
| `this+0x19C` | - | 3x4 group palette base | `slot * 48` 定位矩阵 |
| `this+0x1A0` | - | palette group count | setter 与 `+0x19C` 同时发布；kernel 本身不读 |

palette bind 的真实 ASM 也已闭环：`RenderQueue_UpdateItemWorldMatrix @ 0x6F13A5BD`
令 `ECX=[renderable+0xF0]`、`EDX=palette address`，调用
`GxDevice_SetSkinPalette @ 0x6F0E3920`；vtable `+0x58` 最终到
`CGxDevice_SetSkinPalette @ 0x6F0E70E0`，其中 `0x6F0E70E6` 写
`this+0x19C=palette`，`0x6F0E70EF` 写 `this+0x1A0=groupCount`。

`this+0x198` 不是 palette count：在设备构造函数中它位于前一段 matrix/state 数据尾部，
只看到 `0x6F0E40C6` 初始化为 0，palette setter 不读写它。共享工作区当前
`kGxPaletteCountOffset` 已为 `0x1A0`；这是硬指纹，后续不得回退到 `0x198`。kernel 的
`0x6F0EDF30..41` 不读取 count，也不做 slot bounds check，所以授权阶段仍必须验证
`max(groupSlot) < groupCount`。

### 5.3 输出格式与三个 SIMD 快路

`GxVertexFormat_GetStride @ 0x6F0E38F0` 直接读取
`g_GxVertexFormatStrideTable[format]`。generic switch 的真实目标布局为：

| format | FVF | stride | mapped output |
|---:|---:|---:|---|
| 0 | `0x012` | 24 | `P3 N3` |
| 1 | `0x052` | 28 | `P3 N3 EXTRA1` |
| 2 | `0x112` | 32 | `P3 N3 UV0_2` |
| 3 | `0x152` | 36 | `P3 N3 EXTRA1 UV0_2` |
| 4 | `0x212` | 40 | `P3 N3 UV0_2 UV1_2` |
| 5 | `0x252` | 44 | `P3 N3 EXTRA1 UV0_2 UV1_2` |

format 2、position/normal/UV0 stride 分别为 12/12/8、目标 16 字节对齐且
CPU feature bit 满足时，入口会走三个唯一内部 caller 的 helper：

| VA | IDA 名称 | 条件 / 行为 |
|---:|---|---|
| `0x6F0EE210` | `Gx_CopyVerticesFormat2_MMX` | skinMode 0，MMX interleave/copy |
| `0x6F0EE310` | `Gx_CopyVerticesFormat2_SSE` | skinMode 0，SSE non-temporal copy，尾部 `sfence/emms` |
| `0x6F0EE430` | `Gx_SkinVerticesFormat2_SSE` | skinMode 1，SSE non-temporal skin，尾部 `sfence` |

不满足快路条件时，从 `0x6F0EDE4E` 进入 generic 0..5 布局；不存在的 optional
output 被指向栈 scratch，不会写出 mapped slice。

### 5.4 精确 skin 公式

skin mode 1 的标量 ASM 在 `0x6F0EDF30` 开始。每顶点先读取一个
`uint8_t groupSlot`，再以 `palette + groupSlot * 48` 取得一条 3x4 matrix。
position 公式为：

```text
out.x = M[0] * x + M[3] * y + M[6] * z + M[9]
out.y = M[1] * x + M[4] * y + M[7] * z + M[10]
out.z = M[2] * x + M[5] * y + M[8] * z + M[11]
```

normal 使用相同 3x3 项，不加平移：

```text
out.nx = M[0] * nx + M[3] * ny + M[6] * nz
out.ny = M[1] * nx + M[4] * ny + M[7] * nz
out.nz = M[2] * nx + M[5] * ny + M[8] * nz
```

该 kernel 不 normalize normal。skin mode 0 的 `0x6F0EE114` 路径按 stride 复制 position、normal、optional diffuse/extra 和 UV。

GPU parity 首版必须保持原来的运算次序、3x4 布局与“不 normalize”语义，不能在接管时顺带替换为 inverse-transpose 或通用 4x4 变换。

### 5.5 SEH 与 mapped-pointer 失败语义

IDA `get_tryblks` 给出的内层 SEH range 是
`[0x6F0EEB7B, 0x6F0EEB8A)`，正好覆盖 kernel call。filter
`0x6F0EEB93` 返回 1，handler `0x6F0EEB99` 恢复栈后继续到
`0x6F0EEBA6` 的 Unlock，而不是退出 upload。

完整 caller/EH 表：

| 对象 | ASM 证据 |
|---|---|
| kernel `0x6F0EDDC0` | 唯一 code xref `0x6F0EEB85`；函数自身没有 SEH prologue |
| format-2 MMX/SSE/SSE-skin helpers | 各只有 kernel 内 `0x6F0EDE3F/30/1B` 一个 caller |
| outer upload `0x6F0EEA50` | 无 direct code xref；vtable `0x6F95D8E4/+0x68` 指向它，wrapper `0x6F0E362E` 间接调用 |
| outer EH4 function range | `[0x6F0EEA50,0x6F0EEC1A)`，scope table `0x6FAD7CF0` |
| kernel-call inner SEH range | `[0x6F0EEB7B,0x6F0EEB8A)`，filter `0x6F0EEB93`，handler `0x6F0EEB99` |

`0x6F0EE5D0` 在 Lock HRESULT 失败时从 `0x6F0EE63C` 返回 `EAX=0`：ring base
可能已经选择，但 `ring next` 尚未提交。原上传函数不检查 NULL，仍把它传给 kernel。
当 vertexCount 非零时，原 kernel 的第一次写会触发异常，由上述 caller SEH 吸收，随后
原流程仍执行 Unlock/FVF/SetStreamSource。这是原版可观察语义，P4 不得“修正”它。

因此 kernel detour 的第一条硬规则是：`mappedDst == nullptr` 时直接调用原 trampoline，
不做授权、不吞异常、不写替代返回值。只有拿到真实非空 mapped pointer 后，才允许评估
是否跳过逐顶点写入。

### 5.6 CPU-MT Phase B freeze/join 补充（2026-07-15）

完整 evidence package 见
[cpu_mt_phase_b_native_evidence.md](cpu_mt_phase_b_native_evidence.md)。新增读回/ASM 结论为：

- palette 最早在 `0x6F13A5BD..0x6F13A5C5` 以 groupCount + pointer 精确出现，static streams
  在 `0x6F138F55` 前取齐；两者都早于 output Lock identity；
- `0x6F0EEB76` 后才有 Game.dll `mappedDst`，完整 CommonBuffer/storage/mapping generation 只在
  successful D3D9 Lock sidecar 可见；不存在一个 pre-Lock 单点拥有全部 source + destination；
- worker output 若 Ready，staging copy 与 owner join 必须在 kernel detour 内完成，并在正常返回
  到 `0x6F0EEB8A` 前结算；`0x6F0EEBA6` 已混合 normal/fault，`0x6F0EEBB6` Unlock 后不可 rescue；
- 当前 Lock/Unlock payload 明确 diagnostics-only，ABI 9 无 CPU batch submit/acquire/commit/abort；
  `onUpload` 发生在原 kernel/outer completion 后，不能提交当前 upload；
- Game.dll 已确认创建 `MainLoop_6F05F710` 的 EvtSched `_beginthreadex` workers，但 registered
  callback/indirect dispatch 仍使 exact render-thread affinity 为 Unknown。普通 native upload
  本身是同步嵌套 call，无内部 thread hop、锁或 worker join。

## 6. vtable 三槽与副作用

vtable 基准地址为 `0x6F95D87C`，已直接读取以下三个指针：

| 槽 | 数据地址 | 指针值 | 含义 |
|---:|---:|---:|---|
| `+0x68` | `0x6F95D8E4` | `0x6F0EEA50` | dynamic vertex upload |
| `+0x6C` | `0x6F95D8E8` | `0x6F0EEC20` | dynamic index upload / bind |
| `+0x70` | `0x6F95D8EC` | `0x6F0EE9F0` | indexed batch flush / actual DIP |

### 6.1 `+0x68` dynamic vertex upload

`CGxDeviceD3d_UploadDynamicVertices`：

1. 接收 13 个栈参数并在 `0x6F0EEC17` 以 `retn 34h` 返回；
2. `0x6F0EEAB0` 调用 `CGxDevice_RecordSubmittedVertexCount`；其完整函数只有 6 条指令，唯一数据副作用是 `0x6F0E5BC6: add [ecx+0x5C], eax`；
3. `0x6F0EEAB8..0x6F0EEB37` 写入 `0x6FBC5EA0..0x6FBC5ED0` 共 13 个 dword，并为 null normal/extra/UV 使用固定默认地址；
4. `0x6F0EEB64` 调用 ensure helper；format VB 或共享 16-bit IB 不存在时分别通过 D3D device vtable `+0x68/+0x6C` 创建，HRESULT 失败返回 0；
5. `0x6F0EEB76` 调用 vertex-ring helper；
6. `0x6F0EEB85` 调用 CPU skin/copy kernel；
7. `0x6F0EEBB6` 对 format VB 调用 `Unlock(+0x30)`；
8. `0x6F0EEBCF` 调用 `SetFVF(+0x164)`，原生代码忽略其 HRESULT；
9. `0x6F0EEC00` 调用 `SetStreamSource(+0x190)`，正常路径将这个 HRESULT 原样返回；ensure 失败路径以 EAX=0 走同一 epilogue。

wrapper `GxDevice_UploadDynamicVertices` 在进入 vtable `+0x68` 之前已执行：

- `0x6F0E35B6`：`dword_6FBC543C++`；
- null stream 对应 stride 归零；
- `0x6F0E362E`：调用 vtable `+0x68`。

P4 可以保留 `0x6F0EEA50` detour 作为 outer observer/TLS scope，但 detour 必须始终
调用原 trampoline。wrapper 统计与 null-stride 归一化仍已在更上游完成；本次修订不再
重放 `0x6F0EEA50` 的任何副作用，也不从这个入口提前返回。

vertex ring 的逐指令合同：

| 条件 | base 写入 | Lock offset / bytes | flags | next 写入 |
|---|---:|---|---:|---:|
| `oldNext + count > 0x4000` | `this+0x6DC+format*8 = 0` | `0 / count*stride` | `0x2800` | `count` |
| 否则 | `this+0x6DC+format*8 = oldNext` | `oldNext*stride / count*stride` | `0x1800` | `oldNext+count` |

证据地址为 `0x6F0EE5E9/0x6F0EE5F3/0x6F0EE5FA/0x6F0EE60B/0x6F0EE647/0x6F0EE656/0x6F0EE6A7`。
base 在 Lock 前写入；只有 Lock HRESULT 成功才在 `0x6F0EE697..A7` 写 next 并返回
真实 mapped pointer。失败则从 `0x6F0EE63C` 返回 NULL。

所以 P4 的最终安全点不是 `0x6F0EEA50` 入口，而是它内部完成 ensure 和成功 Lock 后的
`0x6F0EDDC0` kernel call。这样统计、13 globals、VB/IB ensure、Lock、ring、SEH、
Unlock、FVF 与 stream 全部保持原生顺序。

### 6.2 `+0x6C` dynamic index upload / bind

`CGxDeviceD3d_UploadBindDynamicIndices`：

1. `0x6F0EEC63` 调用 index-range 统计 helper，`0x6F0EEC68/6E` 把 range/count 写入 `this+0x714/+0x718`；
2. `0x6F0EEC77` 调用 index-ring helper，`0x6F0EEC8B` 按 `count * 2` 复制 `uint16_t` indices；
3. `0x6F0EECB8` Unlock，`0x6F0EECCA` 通过 device vtable `+0x1A0` 调用 `SetIndices`；
4. `0x6F0EECDD` 将当前 native vertex-ring base 写入 `this+0x71C`。

IDA `get_tryblks` 同时确认 index-copy inner SEH range 为
`[0x6F0EEC7C,0x6F0EEC93)`，filter `0x6F0EEC9C`，handler `0x6F0EECA2`。
因此 Lock 成功后发生的 `memcpy` fault 会继续到 Unlock/SetIndices，outer return 无独立失败位。

index ring 的 capacity 为 `0xC000`：`oldNext+count > 0xC000` 时 base=0、Lock flags=`0x2000`；否则 base=oldNext、flags=`0x1000`；成功后 `this+0x710=base+count`。关键地址为 `0x6F0EE543/0x6F0EE552/0x6F0EE565/0x6F0EE590/0x6F0EE5B9`。

首版 GPU skin 继续使用这条原生 index path。主画面 stream 0 override 必须与其 BaseVertexIndex 语义成对处理。

### 6.3 `+0x70` actual DIP

`CGxDeviceD3d_FlushIndexedPrimitiveBatch` 在 `0x6F0EEA43` 调用 D3D9 vtable `+0x148`，即 `IDirect3DDevice9::DrawIndexedPrimitive`。关键参数来源：

- `BaseVertexIndex = this+0x71C`；
- `MinVertexIndex = 0`；
- `NumVertices = dword_6FBC5EA0`；
- `StartIndex = this+0x70C`；
- indexed range / primitive count 来自 `this+0x714/+0x718` 与 helper；
- primitive type / count 由当前设备状态与 helper 计算。

到达这个函数时，调用者已经假设 vertex upload 的全局状态、FVF、stream 与 ring bookkeeping 有效。

## 7. 0 / 1 / N DIP fan-out 契约

### 7.1 ASM 证据

`RenderQueue_ApplyDrawStateAndSamplerPair` 不是只从普通路径调用。两个特殊 caller 都在一次 ApplyDrawState / vertex upload 后读取 `CGeosetData+0xC8` 并进入 primitive-slice 循环：

```asm
; fallback multipass
6F13A305  call RenderQueue_ApplyDrawStateAndSamplerPair
...
6F13A31A  mov  edi, [eax+0C8h]
6F13A32C  test edi, edi
6F13A32E  jz   short loc_6F13A347
6F13A330  ...
6F13A336  call GxDevice_DrawIndexedRange
...
6F13A344  dec  edi
6F13A345  jnz  short loc_6F13A330
```

```asm
; special / alpha
6F13A439  call RenderQueue_ApplyDrawStateAndSamplerPair
6F13A43E  mov  eax, [ebx+0C8h]
6F13A44F  test eax, eax
6F13A451  jz   short loc_6F13A490
6F13A453  ...
6F13A461  call GxDevice_DrawIndexedRange
...
6F13A48B  cmp  esi, [ebp+var_14]
6F13A48E  jb   short loc_6F13A453
```

设备层又把 vertex upload、index upload 和 actual DIP 拆成三个 vtable 方法，batch begin/end 或状态变化可以改变 flush 时机。因此不能建立“一个 upload 后紧跟且只跟一个 DIP”的全局假设。

### 7.2 正确关联键

GPU skin 观察与 lease 契约必须使用：

```text
dispatch scope
  + monotonic upload epoch
  + DIP ordinal within that scope/epoch
```

并显式允许：

- `0 DIP`：primitive count 为 0、路径提前结束或 draw 被上层跳过；
- `1 DIP`：普通单 slice 路径；
- `N DIP`：multiple primitive slices、special/multipass 或 batch flush 产生多个消费点。

一个全局 one-shot pending token 会在 0-DIP 时泄漏到下一 draw，在 N-DIP 时只覆盖第一个消费点，因此从原生控制流上就不成立。

### 7.3 Observe v3 对时（2026-07-11）

本轮重新用 IDA MCP `disasm` 与 `get_bytes` 读取控制流，不使用伪代码推断：

```asm
; 0x6F0E3520 GxDevice_DrawIndexedRange
6F0E3526  call GxDevice_UploadBindDynamicIndices
6F0E352B  call GxDevice_FlushPrimitiveBatch

; 0x6F13A5E0 RenderQueue_Dispatch_Common
6F13A63D  call GxDevice_BeginPrimitiveBatch
...
6F13A6BE  call GxDevice_FlushPrimitiveBatch
6F13A6C3  call GxDevice_EndPrimitiveBatch

; 0x6F0EE9F0 CGxDeviceD3d_FlushIndexedPrimitiveBatch
6F0EEA1C  push [edi+70Ch]       ; StartIndex
6F0EEA28  push g_GxUploadVertexCount
6F0EEA30  push [edi+71Ch]       ; BaseVertexIndex from +0x6C
...
6F0EEA43  call dword ptr [esi+148h] ; actual D3D9 DIP
```

对应原始字节包括 `0x6F0E3526: E8 25 00 00 00`、`0x6F0E352B: E8 10 00 00 00`，
common tail 的 `0x6F13A6BE: E8 7D 8E FA FF`，以及 actual DIP 尾部
`0x6F0EEA3D: FF B7 84 05 00 00 FF 96 48 01 00 00`。`+0x68` 在
`0x6F0EEC17 retn 34h` 返回；`+0x6C` 到 `0x6F0EECF4 retn 0Ch` 都没有 DIP。

v3 fan-out `0=507251 / 1=126540 / many=45311 / max=2` 满足
`507251+126540+45311=679102 uploads`，且
`126540+2*45311=217162 correlated DIPs`。因此 active window 的正确结算点是下一次
vertex upload 或 dispatch end；immediate `0x6F0E352B` 与 common tail `0x6F13A6BE`
解释了最多两次消费。

全部 `839891` 个 D3D DIP 仍记 raw，但 dispatch 外与 dispatch 内无 active upload 的普通
draw 只记 `outsideDispatchDips` / `dispatchNoUploadDips` 和 unmatched，不进入 manager。
唯一例外是 nested/overflow barrier 隐藏了一个已 committed skip 的父 active upload：此时仍
发布 `sourceUploadKernelBypassed=true` 的 uncorrelated callback 并让 resolver 返回 true，强制
suppress/fuse，不能把它降级成普通 draw。
dispatch summary 只计 active epoch 匹配的 correlated callback；manager
`truePairingErrors` 只计真实 epoch/ordinal/count 合同错误。若 active source 已跳过 CPU kernel，
epoch 错误仍必须回调并进入 resolver，以保持 suppress/fuse fail-closed。

## 8. 已确认的实现陷阱

### 8.1 Special / multipass scope 不能遗漏

只在 common detour 建立 `NativeDispatchScope` 会漏掉 `0x6F13A305` 与 `0x6F13A439`。reimplementation 若直接调用 common trampoline、绕开 detour，也必须显式建立同一观察 scope。

### 8.2 fast return 前必须读取 producer sidecar

`0x6F138EE0` 与 Common ABI 都没有 stage/tag。`CWorld_DispatchStage` 返回后才发生 queue flush，
所以 flush-time ambient stage 不是权威来源。observer-only 模式若走早期 fast return，必须先按
`renderablePart/layerIndex` 查询 producer-time queue sidecar；查询失败只能记录 unknown 并
fail closed，不能把 unknown 当作已证明不属于 Stage 11，也不能用 BattleNet opcode 入口补值。

### 8.3 MinHook partial rollback / live trampoline

安装事务必须区分“本次创建”与 `MH_ERROR_ALREADY_CREATED`：

- `ALREADY_CREATED` 不保证当前调用拿到了本 hook 的 original trampoline；
- rollback 只能移除本事务拥有的 hook；
- `MH_DisableHook` 或 `MH_RemoveHook` 失败时，detour 可能仍存活；
- 只要 live detour 仍可能被调用，就不能清空 trampoline 指针；
- rollback 失败时应禁用 GPU-skin bridge、保留可调用 trampoline 并报告 fatal diagnostic。

### 8.4 复审裁决：禁止再绕过整个 `0x6F0EEA50`

旧首版从 `0x6F0EEA50` 入口提前返回，并尝试手工重放统计、globals、ring、FVF 与
stream。真实 ASM 证明这条路线不满足两个原生合同：

1. `0x6F0EE5D0` 的 Lock 失败返回 NULL，caller 仍在 SEH 内调用 kernel；异常被吸收后
   继续 Unlock/FVF/stream。入口 bypass 无法等价重现这个失败语义。
2. 入口 bypass 成功后，任何未覆盖 consumer 都会读取旧 native VB。consumer 在 DIP
   才失败时，原 mapped pointer 已经不存在，晚调 `0x6F0EEA50` 也会重复统计、ring 和状态。

因此旧的 `ReplayNativeBypassSideEffects`、预测 vertex ring 后空返回，以及“DIP mismatch
只记诊断”均判废。新路线完整保留原 `+0x68`，只在其内部 kernel 已拿到真实 mapped
pointer 后选择跳过写循环。

### 8.5 两层 hook 与 TLS in-flight 时序

outer upload hook 仍有价值，因为它持有完整 13 参数并能包住 kernel 的嵌套调用；但它
只能观察和建立事务，必须始终调用原函数：

```cpp
using UploadDynamicVerticesFn = int32_t (__thiscall*)(
    void* gxDeviceD3d,
    uint32_t vertexCount,
    const void* positions, uint32_t positionStride,
    const void* normals, uint32_t normalStride,
    const void* extra, uint32_t extraStride,
    const uint8_t* groupSlots, uint32_t groupSlotStride,
    const void* uv0, uint32_t uv0Stride,
    const void* uv1, uint32_t uv1Stride);

static int32_t __fastcall Hook_UploadDynamicVertices(
    void* gxDeviceD3d, void* /*edx*/,
    uint32_t vertexCount,
    const void* positions, uint32_t positionStride,
    const void* normals, uint32_t normalStride,
    const void* extra, uint32_t extraStride,
    const uint8_t* groupSlots, uint32_t groupSlotStride,
    const void* uv0, uint32_t uv0Stride,
    const void* uv1, uint32_t uv1Stride);

using SkinCopyKernelFn =
    void (__thiscall*)(void* gxDeviceD3d, void* mappedDst);

static void __fastcall Hook_SkinCopyKernel(
    void* gxDeviceD3d, void* /*edx*/, void* mappedDst);
```

最小 TLS 状态不是一个全局 one-shot token，而是一条嵌套事务：

```cpp
enum class KernelTxnPhase : uint8_t {
  Idle,
  Armed,
  Entered,
  CalledOriginal,
  Skipped,
  UploadReturned,
  Poisoned,
};

struct KernelConsumerReservation {
  uint32_t expectedMainDipCount;       // 首版必须为 1
  uint32_t expectedIndexCount;         // 只来自已学习 CPU baseline
  uint32_t expectedPrimitiveCount;
  uint32_t knownConsumerBits;          // Main|Shadow|Outline 全部已分类
  uint32_t requestedConsumerBits;
  uint32_t reservedConsumerBits;       // 必须等于 requestedConsumerBits
  uint64_t baselineGeneration;
  uint64_t fuseKey;                    // stable layout/resource identity
};

struct KernelUploadTls {
  KernelTxnPhase phase;
  uint32_t depth;
  uint64_t renderThreadId;
  uint64_t flushEpoch;
  uint64_t dispatchEpoch;
  uint64_t uploadEpoch;
  uint64_t authorizationId;
  uintptr_t stableKey;
  void* gxDeviceD3d;
  void* mappedDst;
  uint32_t skinMode;
  uint32_t outputFormat;
  uint32_t outputStride;
  uint32_t vertexCount;
  uint32_t nativeRingBase;
  uint32_t nativeRingNext;
  int32_t uploadResult;
  uint64_t preparedToken;
  uint64_t outputLeaseId;              // Rc/lease 实体由 committed ledger 持有
  KernelConsumerReservation consumers;
};

static thread_local KernelUploadTls g_kernelUploadTls;
```

本轮实际 bridge/manager/host API 收敛为：

```cpp
class NativeUploadInFlightScope {
public:
  explicit NativeUploadInFlightScope(NativeUploadObservation&);
  ~NativeUploadInFlightScope();
};

enum class NativeKernelDetourOutcome : uint8_t {
  CallOriginal,
  BypassCommitted,
  IrreversibleNoCpuRescue,
};

NativeKernelDetourOutcome EvaluateNativeSkinKernelDetour(
    uintptr_t gxDeviceD3d, void* mappedDst);

void CompleteNativeUpload(
    NativeUploadObservation&, int32_t originalResult);

struct NativeBypassAuthorization {
  uint64_t fuseKey;
  uint32_t token;
  uint32_t expectedIndexCount;
  uint32_t requiredConsumerBits;
  uint32_t predictedStartIndex;
  uint64_t approvedPreflight;
};
```

outer scope 只建立可撤销 TLS window；`EvaluateNativeSkinKernelDetour` 在真实 mapped pointer
到达后执行 manager+host preflight，并在返回 `BypassCommitted` 时成为唯一不可逆点。
`CompleteNativeUpload` 在原 outer 返回后只发布一次，保证 authorization 不泄漏。

具体时序：

1. outer hook 分配 upload epoch、采集完整参数并建立 TLS in-flight window；无论 mode 和
   preflight 状态如何，都调用原 `0x6F0EEA50` exactly once。
2. 原 trampoline 执行统计、globals、ensure、Lock 和 ring；outer hook 此时不做 manager
   authorization，也不重放任何副作用。
3. 原函数在 `0x6F0EEB85` 进入 kernel detour。detour 必须核对
   TLS window、self/thread/upload epoch 精确且无递归冲突。
4. `mappedDst==nullptr` 时，TLS 先记 `CalledOriginal`，随后无条件调用 kernel trampoline；
   不得把 trampoline 包进本地 `__try`。caller 的原 SEH 继续决定后续流程。
5. mapped pointer 非空时，bridge 通过 fault-safe live reads 重验 13 globals、VB/IB、ring、
   palette/format/state，再同步调用 manager+host preflight；任一拒绝都返回 `CallOriginal`，
   detour 在原生 SEH 保护范围内调用 kernel trampoline exactly once。
6. 只有 manager authorization 的 token/fuse/count/predicted start/preflight bits 全精确时，
   bridge 才记录 `cpuSkinKernelBypassed=true/cpuSkinBytesSkipped` 并返回
   `BypassCommitted`；这是最后一个可逆边界。
7. 原 `0x6F0EEA50` 继续执行 Unlock、SetFVF、SetStreamSource。outer hook 收到原返回值后，
   调用 `CompleteNativeUpload` 一次并发布 `originalUploadExecuted=true`；返回失败或 post-state
   mismatch 会立即标记 post-skip mismatch/fuse。
8. 原生 `+0x6C` index path 不新增 detour；actual DIP 使用 prepared expected index、live
   primitive bytes 与 ring prediction 验证。skip 后 0/N 或 mismatch 只能 suppress + fuse。
9. TLS in-flight 在 outer trampoline 返回时清空；供 DIP/shadow 使用的是带 epoch 和 lease 的 committed
   ledger，不能把裸 TLS pointer 留给后续 draw。

普通嵌套 dispatch 使用独立 observation，但从创建起标记 `failClosed`；dispatch/semantic 栈
overflow 会建立 TLS 遮蔽屏障，屏障期间 `CurrentDispatch/CurrentSemantic` 返回空，绝不能回落到
父 frame。嵌套 outer upload 同样使用独立 blocked observation，并 poison 父 in-flight epoch；
对应 kernel 只允许 CPU fallback。`nestedDispatchScopes/nestedSemanticScopes/nestedUploadScopes`、
两类 stack overflow、`scopeFailClosedUploads/scopeFailClosedKernelFallbacks` 必须可观测。

kernel 无 outer TLS、self/thread/epoch 不符、同线程存在未结束事务时全部调用原 kernel。
manager callback 的 C++ `catch (...)` 不能替代 fault-safe memory read；原 kernel trampoline 也不被
本地异常处理包住。

验证必须使用 fault-safe read API：kernel call 本来就在原生 SEH try 内。如果 detour 的
验证访问 fault 并直接逃出，`0x6F0EEB93/99` 会把它当作原 kernel fault 吸收，随后继续
Unlock；但 CPU kernel 实际从未写入，native VB 会在没有 committed skip ledger 的情况下
变 stale。`war3_memory` 的窄 `SafeCopy/SafeEqual` 使用当前进程 `ReadProcessMemory`，不在
`VirtualQuery` 后裸解引用；NULL、空范围、地址溢出或任一块读取失败都返回 false，调用方必须
转入 `CallOriginal`。正确伪代码是：

```cpp
if (mappedDst == nullptr) {
  markCalledOriginal();
  g_originalKernel(gxDeviceD3d, mappedDst); // 不包本地 SEH
  return;
}

const auto outcome =
    EvaluateNativeSkinKernelDetour(gxDeviceD3d, mappedDst);
if (outcome == NativeKernelDetourOutcome::CallOriginal) {
  g_originalKernel(gxDeviceD3d, mappedDst); // 不包 detour-local SEH
}
// BypassCommitted / IrreversibleNoCpuRescue: 绝不进入原 kernel。
```

`cpuSkinKernelCalled` 已终态、`cpuSkinKernelBypassed` 已提交或同一 mapped pointer 重入时，返回
`IrreversibleNoCpuRescue`，增加 `duplicateKernelCalls/irreversibleKernelSuppressions`，detour
直接返回。不得再用旧 `bool false` 触发 CPU rescue。唯一绝对例外仍是 `mappedDst==NULL`：
必须返回 `CallOriginal` 交给原生 SEH；若它异常地发生在 committed skip 之后，
`postSkipNativeFallback` 会非零并让硬门失败。

仅比较 `this+0x70C/+0x710` ring arithmetic 也不能把 0/N fan-out 猜成 1。生产 P4 以此前
CPU exact single-DIP baseline 为授权前提，并在当前 actual DIP 再次验证完整参数；若原 index
path 没有产生预期 DIP，device consumer 必须依据不可逆 flag 抑制 draw 并 fuse，不能回读
未写入的 native vertex slice。

### 8.6 唯一允许 skip 的授权交集

kernel skip commit 发生在 native Lock 成功之后，仍必须逐项重验：

- exact Game.dll hash/opcode/vtable/callsite；
- opaque、Stage 11、WorldObjects、common、普通 `0x6F138F70` 单 slice；
- `skinMode==1`，首版 format 仅 0/2/4，vertex count/stride/FVF 精确；
- GPU batch 状态为 `Submitted`。这里表示 compute 已按同一 command stream 的先后关系记录，
  output lease 已成立，不要求 CPU 等待 GPU 完成；
- palette pointer (`this+0x19C`)、group count (`this+0x1A0`) 和
  `groupCount*48` live bytes 与 compute job 的 CPU copy bitwise 相同，并验证每个
  group slot 都小于 group count；严禁从 `this+0x198` 取 count；
- positions、normals、group slots，以及 format 2/4 的 UV bytes 与 immutable GPU static resource
  逐元素 bitwise 相同。只比 hash 不算 exact；stride 非紧密时按元素宽度比较；
- live `CGeosetData+0xC8/+0xCC/+0xE0` 仍为一条 type 3 triangle-list primitive，index bytes
  与已学习 baseline/compute resource bitwise 相同；
- output lease 的 frame/map/device/batch/token/offset/length/usage 全部精确，且状态仍为
  `Submitted`；
- Main、Shadow、Outline 三类 consumer 均已 reservation。没有该 consumer 时必须显式记录
  `NotRequested`，不能把 unknown 当成“不需要”；parity readback 在 skipped upload 上禁用；
- 透明、special/multipass、0/N fan-out、auto-instancing、index split、debug skip、递归、
  非 main pass、未知 format/consumer 全部调用原 kernel。

kernel 点可以直接读取 Lock 成功后原生写好的
`this+0x6DC+format*8` base 与 `this+0x6E0+format*8` next，要求
`next==base+vertexCount`。不再预测或手写 vertex ring。

generic CPU kernel 会推进部分 source scratch global；skip 时这些指针保持 pre-kernel 值。
xref 表明 `0x6FBC5EA4..0x6FBC5ED0` 在本 indexed 路径的后续 consumer 中不再读取，下一次
`+0x68` 又会覆盖；`0x6FBC5EA0` 由 outer 原函数发布，actual DIP 仍能取得 NumVertices。

### 8.7 不可逆边界：失败必须 suppress + fuse

kernel 已返回 `Skipped` 后，native VB 当前 slice 已成功 Lock 但没有任何顶点写入。紧接着
的 Unlock 会让 mapped pointer 失效，所以此后不存在合法 CPU rescue。规则必须是二分的：

| 失败时点 | 动作 |
|---|---|
| kernel skip 之前 | 调原 kernel，正常 native fallback |
| kernel skip 之后 | 绝不调原 upload/kernel；抑制对应 consumer，并 fuse stable key |

为避免当前 `GpuSkinResolvedDraw{}` 把“普通 fallback”和“已 skip 后解析失败”混为一谈，
DIP API 至少要返回：

```cpp
struct DipResolution {
  bool kernelSkipped;
  bool exact;
  bool suppressDraw;
  bool fuseKey;
  FailureReason reason;
  PreparedDrawKey key;
  OutputLease output;
};

DipResolution ResolveCommittedDip(const NativeDipInput&) noexcept;
ShadowBackingResolution ResolveShadowBacking(
    const PreparedDrawKey&, const OutputLease&) noexcept;
void FuseTakeoverKey(const StableLayoutKey&, FailureReason) noexcept;
```

具体消费规则：

1. `DrawIndexedPrimitive` 若发现 pending skipped upload，但 epoch/signature/lease/main reservation
   任一不精确，必须在发出 draw 前返回 `D3D_OK` 抑制该 draw，并 fuse key。不能落回 native
   stream 0。
2. 成功 resolve 后，在 `PrepareDraw` 完成之后绑定 GPU output stream 0，设置
   `vertexOffset=0`；`StartIndex` 继续使用原 index ring。这样 `PrepareDraw` 不会覆盖最终
   one-shot binding。
3. 几何 outline 必须复用同一 lease；outline reservation 失败则抑制 outline 并 fuse。
4. shadow capture/backing 若无法取得 exact lease，必须跳过本 caster/shadow draw 并 fuse。
   禁止从 native stream 复制，也禁止复用该 key 的旧帧 CPU backing，因为两者都是 stale pose。
5. native `SetStreamSource` 返回失败、`+0x6C` 实际 index upload/ring 不符、DIP 时
   FVF/stride 不符、stream restore 无法 arm、consumer 超出 reservation，均进入同一
   suppress/fuse 路径。
6. fuse 后该 stable key 与 layout 在 manager 生命周期内永久只走原 CPU kernel；后续 CPU
   DIP 即使再次 exact 也不得重新授权，map/device epoch teardown 也不清除 fuse set。

### 8.8 CPU baseline 学习，消除 `expectedIndexCount` 循环依赖

旧实现让 baseline 验证读取只有 P4 authorization 才会填写的
`NativeUploadObservation.expectedIndexCount/predictedIndexRingBase`，于是未授权 CPU 路径
永远无法把 `exactSingleDipConfirmed` 从 false 推到 true。修复后的状态机不新增
`0x6F0EEC20` detour，而是区分两个来源：

- prepared draw 的 expected index count 来自 exact static primitive record，是 CPU probe
  已有的独立输入；
- upload observation 的 expected/predicted 字段只属于已授权 kernel skip，不能参与 CPU
  baseline 的建立。

具体学习顺序：

1. 未确认 layout 一律调用原 kernel；outer upload observer 建立 CPU probe。
2. actual DIP observer 按 dispatch scope + upload epoch + DIP ordinal 记录真实
   PrimitiveType/BaseVertex/MinVertex/NumVertices/StartIndex/PrimitiveCount/flags。
3. CPU DIP 用 prepared expected count 计算当次 native start-index prediction，并与真实
   `StartIndex` 比较；同时要求 triangle list、flags 0、`NumVertices==prepared vertexCount`、
   `PrimitiveCount*3==prepared expectedIndexCount`。
4. dispatch end 只有在 fan-out exactly 1 且该 DIP 全部 exact 时，才保存完整 CPU DIP
   signature；`0 DIP`、`N DIP`、ordinal/signature mismatch 一律清除 baseline，绝不能把
   0/N 提前按 1 学习。
5. P4 authorization 再读取该 CPU baseline，与当帧 live primitive/index bytes、submitted
   compute job、palette/static bytes 和 host draw preflight 逐项比较。
6. kernel skip 后任一 0/N/mismatch 都只增加 mismatch/fuse 诊断并熔断 key/layout；当前
   generation 内不再通过 CPU baseline 重学解除。

这样 first-run 永远有可达路径：CPU draw -> 观察真实 index/DIP -> 确认合同 -> 后续帧才允许
kernel skip。post-skip fuse 是不可逆边界，不走该学习路径恢复授权。

### 8.9 opcode 指纹与 MinHook 安装事务

目标文件再次本机核对：version `1.27.0.52240`，MD5
`267861a0dfd416dbad13e7ee3ec7794a`，SHA1
`88ab432160fb84c23b096c3fc022bfbcb3cb1a1a`。raw PE 证据必须区分两个字段：COFF
`FileHeader.TimeDateStamp` 在 raw `0x140` 是 `1C 0E BD 56`，即
**`0x56BD0E1C`**；Export Directory 的时间戳在 raw `0x00B53DC4` 才是
`0x56BD0E1B`。`objdump -x` 末尾的 `56bd0e1b` 属于 export table。C++ fingerprint
现恢复比较 COFF `0x56BD0E1C`，并用 PE valid/failure 子位记录 DOS、MZ、精确
`e_lfanew=0x138`、NT、signature、Machine `0x014C`、Magic `0x010B`、header ImageBase、
timestamp、image size 与 checksum。

隔离桌面 v2 的 mapped header 报告 `loadedBase==OptionalHeader.ImageBase==0x62290000`，仅
旧 ImageBase 位 `0x80` 失败。IDA MCP 直接解析 hash-matched 文件：raw/RVA `0x16C` 是
`00 00 00 6F`；`DllCharacteristics=0x0140` 含 `DYNAMIC_BASE`；`.reloc` 为
RVA `0x00BFD000`、size `0x000DA478`、2911 blocks，首个 page `0x1000`、最小 fixup
`0x1002`，没有 `0x16C`。因此 mapped header 的实际基址不是 HIGHLOW relocation 结果，而是
loader/mapper 的映射头归一化。C++ 只接受 `headerImageBase==0x6F000000` 或
`headerImageBase==gameBase`；任意其他值失败。必需 acceptance 位仍是 `0x80`，另以
`0x800=preferred`、`0x1000=runtime-relocated` 两个互斥 valid 位记录来源；hash/version/
其余 PE/opcode/vtable 仍全部参与顶层 fail-closed。

| 检查点 | VA / RVA | expected bytes |
|---|---|---|
| kernel entry | `0x6F0EDDC0 / 0x0EDDC0` | `55 8B EC 83 EC 2C 8B 91 90 00 00 00 89 4D FC 53 8B 5D 08 F6 C2 06 74 76` |
| kernel tail | `0x6F0EE1E8 / +0x428` | `5F 5E 5B 8B E5 5D C2 04 00`（只覆盖函数自身；后继首字节是 `8D`，不得猜成 `CC`） |
| Lock + kernel call block | `0x6F0EEB71 / 0x0EEB71` | `FF 75 08 8B CB E8 55 FA FF FF C7 45 FC 00 00 00 00 50 8B CB E8 36 F2 FF FF` |
| SEH/call window | `0x6F0EEB7B / 0x0EEB7B` | `C7 45 FC 00 00 00 00 50 8B CB E8 36 F2 FF FF C7 45 FC FE FF FF FF` |
| upload entry | `0x6F0EEA50 / 0x0EEA50` | raw `55 8B EC 6A FE 68 F0 7C AD 6F 68 BC 1F 77 6F 64 A1 00 00 00 00 50 83 EC 0C 53 56 57 A1 74 A7 B6 6F` |
| apply entry | `0x6F138EE0 / 0x138EE0` | `55 8B EC 53 56 57 8B FA 8B F1 8B 0F E8 DF A9 FA FF` |
| flush entry | `0x6F1380A0 / 0x1380A0` | raw `55 8B EC A1 AC 6B BC 6F`；运行时 `A1 imm32 == gameBase+0xBC6BAC` |
| dispatch entry | `0x6F13A5E0 / 0x13A5E0` | `55 8B EC 83 EC 3C 53 56 8B 72 0C 8B D9 57 56 89 75 F8` |

upload entry 的 RVA `0x0EEA56/0x0EEA5B/0x0EEA6D` 均为 HIGHLOW relocation，语义目标依次是
`gameBase+0xAD7CF0`、`gameBase+0x771FBC`、`gameBase+0xB6A774`。当前 C++ 子项精确比较
5-byte `uploadPrefix`、从 `+0x0F` 开始的 13-byte `uploadEH4` 和 tail；没有把已比较字节
放宽为通配。若后续把三个 operand 纳入同一连续 pattern，必须按上述 RVA 构造运行时精确值，
不能使用泛型 wildcard。还必须解码两个 `E8 rel32`，分别确认目标为 `0x6F0EE5D0` 与 `0x6F0EDDC0`，
并确认 vtable `0x6F95D8E4/+0x68 == 0x6F0EEA50`、
`0x6F95D8E8/+0x6C == 0x6F0EEC20`、`0x6F95D8EC/+0x70 == 0x6F0EE9F0`，以及
outer 内 `0x6F0EEB85` 的 rel32 目标确为 kernel。任一不符时整个 native bridge 禁用，
不能只关 kernel gate 而留下半套 observer。

隔离日志实际 `gameBase=0x62290000`，而 PE 具备 `DYNAMIC_BASE`。flush 的绝对 operand
在 RVA `0x1380A4` 有 HIGHLOW relocation，所以该次运行应为
`gameBase+0xBC6BAC=0x62E56BAC`，入口字节 `55 8B EC A1 AC 6B E5 62`。旧 C++ 只比较
7-byte 静态 `... AC 6B BC`，在 `0x1380A6` 产生 opcode false negative。现改为按
`gameBase` 构造完整 8-byte exact pattern；不是通配。opcode 的 apply prefix/tail、upload
prefix/EH4/tail、kernel prefix/tail、两处 rel32、flush、dispatch 共 11 个失败位全部独立
求值并输出，任何一位仍会令顶层 `Opcodes` fail closed。

安装/回滚/卸载顺序：

1. fingerprint 全部通过且 dispatch/flush prerequisite 已安装后，按
   apply `0x6F138EE0` -> outer upload `0x6F0EEA50` -> kernel `0x6F0EDDC0` 顺序创建并启用
   三个 owner hook，保存各自 original trampoline。
2. `MH_ERROR_ALREADY_CREATED` 只有在统一 hook registry 能证明 target、detour、owner 与
   trampoline 完全相同时才可复用；否则视为失败。
3. 三个 hook 全部成功后才 publish `bridgeEnabled=true`。中间态 detour 因 bridge disabled
   只透传 original，但不得宣布安装成功；任一创建/启用失败按 kernel -> outer -> apply
   逆序整组 rollback。
4. partial rollback 只 disable/remove `createdByThisTxn` 的 hook。任何 disable/remove
   失败都保留 trampoline 和模块 lifetime，bridge fail closed；绝不能把仍可能执行的
   original pointer 清零。
5. 正常卸载先令 `acceptAuthorization=false`，等待 active detour count 为 0、render-thread
   TLS 为 Idle、committed ledger 排空，再 queued-disable 三个 hook 并 remove owner hook。
   drain 或 MinHook 操作失败时拒绝卸载 native bridge。
6. map/device reset 只取消 authorization、fuse/baseline/lease 并提升 generation；process
   lifetime hook 无需反复拆装。
7. fingerprint 结果、三钩安装成功、部分失败回滚及 prerequisite 导致的整组关闭都写入
   `Logger::info` (`war3_d3d9.log`)；DBWIN/`war3dbg` 仅作附加通道。

### 8.10 最小计数与验收门

| 计数 | 验收要求 |
|---|---|
| `uploads` / `originalUploadCalls` | 完全相等；P4 不再有 whole-upload bypass |
| `kernelHookCalls` | 等于 outer ensure 成功并进入 kernel detour 的次数 |
| `nullMappedKernelFallbacks` | 可非零；每次都计入 `originalKernelCalls`，skip 必须为 0 |
| `originalKernelCalls` + `bypassedKernelCalls` + `irreversibleKernelSuppressions` | 等于 `kernelHookCalls`；重复终态调用不得进入 original |
| `originalKernelBytes` / `bypassedKernelBytes` | skip bytes 为 `vertexCount*outputStride`；二者覆盖 kernel 工作量 |
| `kernelPreflightRejects` | 可观测；每次 reject 必须进入 original kernel exactly once |
| `duplicateKernelCalls` / `irreversibleKernelSuppressions` | 两者应相等；验收地图必须为 0，非零时也只能 suppress，不能 CPU rescue |
| `nestedDispatchScopes` / `nestedSemanticScopes` / `nestedUploadScopes` | 可观测；对应 upload 必须计入 `scopeFailClosedUploads` 并走 CPU fallback |
| `dispatchStackOverflow` / `semanticStackOverflow` | 验收必须为 0；发生时 TLS barrier 不得暴露父 scope |
| `pendingKernelAuthorizations` / manager `pendingBypassAuthorizations` | 稳态与 dispatch end 必须回到 0 |
| `nativeLockSuccess/Unlock/Fvf/Stream` | skipped draw 仍走原路径，计数相对 outer 成功调用不缺失 |
| `cpuBaselineSingleDipLearned/ZeroDip/MultiDip/IndexMismatch` | 首次 skip 前必须已有 learned；0/N 不得晋级 |
| `postSkipUploadFailure` | 验收地图必须为 0 |
| `postSkipMismatchFuses` / manager `postSkipMismatches` | 验收地图必须为 0；发生后 DIP 必须 suppress + fuse |
| `postSkipNativeFallback` | committed skip 后实际进入 original kernel 的次数；永久硬门，必须为 0 |
| `dipSuppressedAfterSkip` / `shadowSuppressedAfterSkip` / `outlineSuppressedAfterSkip` | 验收必须全 0；出现即 fuse，不允许悄悄 fallback |
| manager `bypassFuses` | 验收必须无新增 fuse；manager 生命周期内不允许 CPU baseline/epoch reset 清除 |
| `reservationLeaks` / `unreservedConsumers` / `restoreFailures` | 必须全 0 |
| `dips` / `correlatedDips` / `unmatchedDips` | `dips == correlatedDips + unmatchedDips` |
| `outsideDispatchDips` / `dispatchNoUploadDips` | 普通路径只属 raw/unmatched，不增加 callback/summary；隐藏 committed-skip hazard 时例外发送 uncorrelated suppress/fuse callback |
| manager `nativeDips` / native `correlatedDips` | 稳态完全相等；二者都只计 correlated callback |
| manager `truePairingErrors` | 只允许真实 active epoch/ordinal/count 错误，普通 D3D draw 不得增加 |
| `inputPreflightMissing[0..17]` / manager `strictUploadRejectByReason` | 定位 eligible 拒绝；仅诊断，不放宽 required mask/白名单 |

此外每条 committed skip 必须在 dispatch end 满足：exact one main DIP 已消费、Requested
outline 已消费、Requested shadow 已成功附着 exact backing/retained lease，NotRequested 有
预授权证据，upload ledger 无未归属 pending。真正的 CSM/point-shadow 消费可晚于 dispatch，
由 retained lease 在 frame retirement 前继续核销，不能在 dispatch end 提前释放。
功能验收还要证明 CPU kernel 调用/字节确实下降，而 outer ensure/Lock/Unlock/ring/FVF/stream
调用保持；性能与画质测试由唯一 Test Conductor 执行，本逆向线程不 build/deploy/run。

### 8.11 当前 C++ 实现边界

2026-07-11 已落地：

1. `war3_gpu_skin_native_bridge.cpp` 已物理删除
   `TryBypassNativeUpload -> ReplayNativeBypassSideEffects -> early return`；outer 永远调用
   original，新增 `RVA 0x0EDDC0` kernel gate。
2. render hook lifecycle 把 apply、outer、kernel 作为同一三钩 transaction；任一失败整组
   fail closed，并把 fingerprint/安装结果写入 `Logger::info`。
3. manager authorization 绑定 Submitted batch、exact palette/static bytes、format 0/2/4、
   opaque Stage 11 common single primitive、CPU exact single-DIP baseline 与 stable fuse key。
4. `NativeDipObservation.sourceUploadKernelBypassed/sourceUploadFuseKey` 独立于 resolve；
   `GpuSkinResolvedDraw.nativeUploadBypassed/bypassFuseKey/requiresSuppression()` 在失败结果中仍保留。
5. CPU baseline 用 prepared static expected index 与 actual DIP 学习；P4-only expected/predicted
   字段不参与 CPU baseline，0/N fan-out 不晋级。
6. `D3D9DeviceEx::DrawIndexedPrimitive` 的独立 consumer 线程需在 shadow capture/main emit 前处理
   `suppressDraw`；成功路径保持 `PrepareDraw` 后 bind GPU stream 0。所有提前返回、split、
   instancing 与 debug skip 都必须先通知 committed ledger。
7. shadow backing API 返回三态 `NotRequested/LeaseBacked/SuppressAndFuse`；skipped upload
   禁止走 native VB copy 或旧 cache fallback。
8. `war3_memory` 已新增 fault-safe `SafeCopy/SafeEqual`；native bridge 的 opcode/PE/vtable、
   device fields、13 globals 等读取已改为该 API，不再 `VirtualQuery` 后裸读。
9. dispatch/semantic overflow 使用 TLS barrier；普通嵌套 dispatch/semantic 和嵌套 outer upload
   均显式 fail closed，并导出 nested/overflow/scope-fallback 计数；observation 布局变化后
   `kNativeBridgeCallbackAbi` 已提升到 4，旧 callback table 必须注册失败。
10. kernel detour 已由 `bool TryBypass...` 改为
    `CallOriginal/BypassCommitted/IrreversibleNoCpuRescue` 三态。重复 kernel call 只计数并抑制；
    `mappedDst==NULL` 仍无条件调用原函数。

**尚未闭合的所有权边界**：manager 当前对 live palette、primitive/index 和 static stream bytes
仍存在 `IsReadableRange* + memcpy/memcmp`。这些读取发生在 `0x6F0EEB85` 的原生 SEH 范围内，
必须由 manager owner 改接 `SafeCopy/SafeEqual`；任一 false 返回 CPU fallback。在该接线完成前，
P4 runtime 开关继续默认关闭，不能把 bridge 自身已 fault-safe 写成整条授权链已闭合。

这些改动只触及 GPU-skin bridge/manager、DIP consumer 与 shadow backing 合同；不需要
改 Game.dll 顶点 shader，也不改变原生 index producer。

### 8.12 Observe v4：Stage/Batch 语义闭环（2026-07-11）

本节针对
`AutoTest/artifacts/gpu_skin_observe_isolated_v4_20260711_141533/observe_diagnostics_final.json`
重新读取真实 ASM、xref、原始字节和当前 C++ tracker，不使用伪代码推断 ABI。本线程未
build、deploy 或运行 War3。

#### 8.12.1 v4 计数先验

| 项 | v4 值 | 直接含义 |
|---|---:|---|
| dispatch begin / end | `229052 / 229052` | scope 数量闭合，不是 begin/end 泄漏 |
| uploads | `786944` | 全部 original upload 都被观察到 |
| missing Stage 11 | `786944` | 没有一条 upload 收到 stage 11 |
| missing WorldObjects | `601963` | `184981` 条收到内部 WorldObjects tag |
| missing common | `638262` | `148682` 条是 common |
| fan-out 0 / 1 / 2 | `585217 / 148682 / 53045` | `maxFanout=2`，总和等于 uploads |
| strict reject path / stage | `53045 / 148682` | 2-DIP 先被 path 拒绝，1-DIP common 全被 stage 拒绝 |

`148682` 同时等于 common 数、1-DIP 数和 strict stage reject 数。结合 begin/end 完全相等，
v4 的主故障不是 upload/DIP scope 遗失，而是 queue sidecar 没有把生产时 stage 保存到
dispatch 时可查询的 key。

#### 8.12.2 `FlushSortedItems -> Dispatch_Common` 的真实参数

`RenderBatch_Submit` 在 `0x6F1376D6..0x6F137719` 写 20-byte queue record：

```text
+0x00  renderablePart / dispatch record pointer
+0x04  path flags；低两位决定 Common/Special
+0x08  layerIndex
+0x0C  本次 submit 内递增的 layer ordinal
+0x10  layer-state record pointer
```

关键写入：

```asm
6F1376EE  mov [edi+08h], edx     ; layerIndex
6F1376F4  mov [edi+00h], ecx     ; renderablePart
6F1376F6  mov [edi+10h], eax     ; layer-state record
6F137716  mov [edi+0Ch], eax     ; local var_14 ordinal
```

`+0x0C` 来自局部 `var_14`，随后 `inc eax`；它不是 CWorld stage。该写入区原始字节
`0x6F1376D6..` 以
`A1 B0 6B BC 6F 8D 0C 92 42 89 15 AC 6B BC 6F` 开始，其中两个绝对地址操作数
受 relocation 影响；hook fingerprint 必须按 RVA/relocation 处理。

排序遍历在唯一 common callsite `0x6F1381AA` 这样装参：

```asm
6F13812E  mov ecx, [edi]         ; queueRecord+0x00
...
6F138157  mov eax, [ecx+14h]     ; sceneNode / submit owner
6F13815A  mov [ebp+var_8], eax
...
6F13819E  push [ebp+var_4]       ; stack arg3: drawStateChanged
6F1381A1  mov  edx, [edi]        ; EDX = renderablePart
6F1381A3  mov  ecx, [ebp+var_8]  ; ECX = sceneNode
6F1381A6  push eax               ; stack arg2: stateBlockChanged
6F1381A7  push [edi+8]           ; stack arg1: layerIndex
6F1381AA  call RenderQueue_Dispatch_Common
```

`0x6F13819E..0x6F1381AE` 原始字节为
`FF 75 FC 8B 17 8B 4D F8 50 FF 77 08 E8 31 24 00 00`。最终 ABI：

| 位置 | 语义 |
|---|---|
| `ECX` | `sceneNode/submit owner = [renderablePart+0x14]` |
| `EDX` | `renderablePart/dispatch record = queueRecord+0x00` |
| `[ESP+4]` | `layerIndex = queueRecord+0x08` |
| `[ESP+8]` | `stateBlockChanged` |
| `[ESP+0x0C]` | `drawStateChanged` |
| 返回 | `retn 0Ch`；stage/tag 不在返回值中 |

`RenderQueue_Dispatch_Common` 入口字节为
`55 8B EC 83 EC 3C 53 56 8B 72 0C 8B D9 57 56 89 75 F8`，入口
`mov esi,[edx+0x0C]` 直接取得 `CGeosetData*`。该函数只有 `0x6F1381AA` 一个 code xref。
结论是 queue record 和 Common ABI 都没有原生 CWorld stage 或 native batch tag 字段。

#### 8.12.3 Stage 11 的唯一权威表达

`CWorld_RenderScene` 的 Stage 11 调用为：

```asm
6F368310  push edi               ; activeQueue
6F368311  push 4                 ; categoryMask
6F368313  push 2                 ; renderMode
6F368315  push 0Bh               ; stageId = 11
6F368317  mov  ecx, esi          ; CWorld*
6F368319  call CWorld_DispatchStage
6F36831E  call RenderQueue_FlushAndReset
```

对应原始字节是
`57 6A 04 6A 02 6A 0B 8B CE E8 02 AD FF FF E8 DD 14 DD FF`。因此权威 ABI 为：

```text
ECX       = CWorld*
[ESP+04]  = stageId
[ESP+08]  = renderMode
[ESP+0C]  = categoryMask
[ESP+10]  = activeQueue
callee    = retn 10h
```

`CWorld_DispatchStage` 入口原始字节为
`55 8B EC 83 7D 14 00 B8 03 00 00 00 53 8B 5D 10 56 57 8B 7D 0C 8B F1`。
其中 `[EBP+0x14]` 是 `activeQueue`，`[EBP+0x10]` 是 `categoryMask`，`[EBP+0x0C]`
是 `renderMode`，switch key 在 `[EBP+0x08]`；不能照旧的 IDA stack-var 标签顺序猜 ABI。

`CWorld_DispatchStage` 在 `0x6F363092` 从首个栈参读取 switch key。case 11 的真实顺序是：

```asm
6F3631C0  mov  ecx, 0Ch
6F3631C5  call CWorld_TerrainShadow_Dispatch
6F3631CA  push 0
6F3631CC  mov  ecx, esi
6F3631CE  call CWorld_WorldObjects_RenderGroup
```

而 TerrainShadow selector 12 在 `0x6F76F11C` 跳到
`TerrainShadow_Selector12_EnqueueBatches`；其 tail chunk 逐个检查 0x3C-byte entry，并在
`0x6F76B446` 调用 `RenderQueue_AddBatch`。所以 **Stage 11 的新增 queue range 先包含
TerrainShadow producer，再包含 WorldObjects group 0 producer**。仅用 `stageId==11` 生成
WorldObjects tag 会过标。

selector 12 跳板字节为 `E9 AF 77 FF FF`；目标 root `0x6F7668D0` 字节为
`E8 6B FF FF FF 8B C8 E9 44 4B 00 00`。它的 tail chunk `0x6F76B420` 在
`0x6F76B446` 以 `E8 45 DD 9C FF` 调 `RenderQueue_AddBatch`。

另一个时序事实是 `0x6F36831E` 的 flush 位于 `CWorld_DispatchStage` 返回之后。围绕
`CWorld_DispatchStage` 建立的 ambient TLS stage scope 到 `Dispatch_Common` 时已经结束，不能作为
flush-time fallback；stage 必须在 producer 时固化到 queue sidecar。

#### 8.12.4 WorldObjects tag 裁决

`CWorld_WorldObjects_RenderGroup` 的真实 ABI 是：

```text
ECX       = CWorld*
[ESP+04]  = groupIdx
return    = retn 4；EAX 无稳定业务合同
```

入口字节为
`55 8B EC 8B 45 08 56 83 E8 00 74 16 48 74 0B 48 75 3F`。group 选择和全部 code xref
已闭环：

| groupIdx | list pointer | 唯一 CWorld stage callsite |
|---:|---:|---:|
| `0` | `CWorld+0x16C` | case 11，`push 0` at `0x6F3631CA`，call at `0x6F3631CE` |
| `1` | `CWorld+0x170` | case 12，call at `0x6F3631DE` |
| `2` | `CWorld+0x174` | case 13，call at `0x6F3631B4` |

每个 `WorldGroupRecord` 步长 `0x18`，其 `+0` 是 strong `CSprite*`；
`CSprite_PrepareAndQueueAttachedRenderObject` 从 `sprite+0x20` 取 attached render object，尾跳
`RenderQueue_AddBatch`。因此 P1A 所需的最窄权威条件不是“整个 Stage 11”，而是：

```text
queue record 在 CWorld_WorldObjects_RenderGroup(groupIdx == 0) 的
beforeCount..afterCount 范围内产生
```

当前 `kWorldObjectsTag=1` 与项目内部 `War3BatchTag::WorldObjects=1` 数值一致，可以继续作为
DXVK 内部枚举值；**Game.dll 不存在经 ASM 证实的 native tag 值 1**。错误点不是枚举数字，
而是把 `MapStageToTag(11)` 当成精确 native producer 身份。静态 ASM 也不能证明
`+0x16C/+0x170/+0x174` 三个 list 的业务内容恰好分别是“单位/建筑/装饰物”；本页只把
`groupIdx=0` 定义为 Stage-11 WorldObjects group-0 producer，不扩张其对象分类。

#### 8.12.5 三个禁止用作 Stage 11 的伪来源

1. `0x6F13A9B0` 已改名 `RenderQueue_UpdateGxStages`。`ECX` 是 force/update-all boolean，
   内部 `ECX=esi` 调 `GxDevice_UpdateStage` 时的 stage 是 Gx/device stage，不是 CWorld stage。
2. `0x6F7B28F0` 已改名 `BattleNet_DispatchCommandOpcode`。入口
   `55 8B EC 8B 45 0C 56 0F B6 C0 33 F6 3D 82 00 00 00` 明确从第二个栈参取低字节，
   对 `0..0x82` command opcode 做 switch；当前 C++ 将其参数命名为 `stage` 不具备
   CWorld Stage 11 语义。
3. `0x6F7B7830` 已改名 `BattleNet_ApplyICONDATARecord`。`0x6F7B7882` 只写一个 byte field，
   `0x6F7B78BC` 引用 RTTI 字符串 `.?AUICONDATA@BattleNet@@`；它不是 `SceneSubmitBatch`，
   不能向 GPU-skin dispatch scope 提供 render stage。

#### 8.12.6 当前 C++ 丢 Stage 11 的确定根因

当前 `TrackRenderQueueUpdates` 先调用 `RenderQueueTracker::MarkStages`，再调用 `MarkTags`。
但真实实现是：

1. `MarkStages` 只在 key 已存在且 epoch 相同时更新，空槽不创建 key；
2. `MarkTags` 会创建/覆盖 key，但总是写 `PackState(tag, -1, currentEpoch)`；
3. 因此即使 `MarkStages` 曾命中，随后的 `MarkTags` 也会把 stage 重新清成 `-1`。

这与 v4 的 `missing Stage11=786944/786944` 完全一致。`War3RenderState::GetStage()` 也不能补救，
因为 `Hook_WorldDispatch` 在返回后设回 `-1`，而原版到随后 `FlushAndReset` 才进入 common dispatch。
当前 `kNativeTagWorldByGroupIdx=false` 还关闭了已有的 group-range sidecar；因此 v4 中出现的
`184981` 条内部 WorldObjects tag 来自 broad stage-to-tag 路径，不是 group0 精确身份。

#### 8.12.7 最窄 ASLR-safe bridge 与最小修复点

不新增 Game.dll hook 即可复用当前已有的 `Hook_WorldObjects_RenderGroup` 和
`Hook_RenderQueue_Dispatch_Common`：

1. 在 `CWorld_WorldObjects_RenderGroup` RVA `0x368E30` 的现有 detour 中，于调用 original 前后
   读取 `g_RenderQueue_NumOfElements`；original trampoline ABI 为
   `void (__thiscall*)(void* world, int groupIdx)`，detour 可用
   `void __fastcall Hook(void* world, void* edx, int groupIdx)` 接收。
2. 仅 `groupIdx==0` 时，把当前 batch array 的 `[before, after)` 记录统一 upsert 为
   `(stage=11, batchTag=War3BatchTag::WorldObjects)`。不要再用两个会互相清字段的
   `MarkStages`/`MarkTags` 顺序调用；新增一个同时保留 tag+stage+epoch 的窄 upsert 是最小安全修复。
3. `Dispatch_Common` 继续以 `EDX=queueRecord+0x00` 查询。producer 与 consumer key 的同一性已由
   `0x6F1376F4` 和 `0x6F1381A1` 闭环；`layerIndex` 来自 stack arg1，可作为冲突诊断字段。
4. general stage tracker 若仍要覆盖其他 stage，也必须改为同一次 upsert，或至少先创建 key 再
   保留 stage；Stage 11 的 broad `MapStageToTag` 不得覆盖 group0 精确身份。Stage 11 前置
   TerrainShadow range 应只有 stage 11，不应获得 WorldObjects tag。
5. 地址全部使用 `gameBase+RVA`，并校验本节 entry bytes。不要 patch
   `0x6F3631CE/0x6F368319` callsite，也不要依赖 preferred VA；现有 function-entry MinHook 是更窄、
   更稳定的 ASLR-safe 接法。

如果只求先验证 v4 根因，`TrackRenderQueueUpdates` 改为单次 tag+stage upsert 即可令 stage 可见；
但这仍会把 Stage 11 前置 TerrainShadow 误标为 WorldObjects，不能作为 P4 白名单的最终修复。
本逆向线程按约束未修改任何 C++。

## 9. IDA 写回记录

### 9.1 Rename

| 地址 | 原名 | 新名 | 结果 |
|---:|---|---|---|
| `0x6F1380A0` | `RenderQueue_FlushSortedItems` | 保留 | 已有名称更准确 |
| `0x6F138EE0` | `RenderQueue_ApplyDrawStateAndSamplerPair` | 保留 | 已有名称更准确 |
| `0x6F138F70` | `sub_6F138F70` | `CGeosetData_SubmitSingleIndexedSlice` | 成功 |
| `0x6F0E35B0` | `sub_6F0E35B0` | `GxDevice_UploadDynamicVertices` | 成功 |
| `0x6F0E3550` | `sub_6F0E3550` | `GxDevice_UploadBindDynamicIndices` | 成功 |
| `0x6F0E38D0` | `sub_6F0E38D0` | `GxDevice_SetOutputVertexFormat` | 成功 |
| `0x6F0E38E0` | `sub_6F0E38E0` | `GxDevice_SetSkinMode` | 成功 |
| `0x6F0E38F0` | `sub_6F0E38F0` | `GxVertexFormat_GetStride` | 成功 |
| `0x6F0E3900` | `sub_6F0E3900` | `GxDevice_CopySkinPaletteMatrix` | 成功 |
| `0x6F0E3920` | `sub_6F0E3920` | `GxDevice_SetSkinPalette` | 成功 |
| `0x6F0E5BC0` | `sub_6F0E5BC0` | `CGxDevice_RecordSubmittedVertexCount` | 成功 |
| `0x6F0E5BD0` | `sub_6F0E5BD0` | `CGxDevice_RecordIndexedRange` | 成功 |
| `0x6F0E7030` | `sub_6F0E7030` | `CGxDevice_SetOutputVertexFormat` | 成功 |
| `0x6F0E7050` | `sub_6F0E7050` | `CGxDevice_SetSkinMode` | 成功 |
| `0x6F0E7060` | `sub_6F0E7060` | `CGxDevice_CopySkinPaletteMatrix` | 成功 |
| `0x6F0E70E0` | `sub_6F0E70E0` | `CGxDevice_SetSkinPalette` | 成功 |
| `0x6F0EDD10` | `sub_6F0EDD10` | `CGxDeviceD3d_EnsureDynamicVertexAndIndexBuffers` | 成功 |
| `0x6F0EDDC0` | `sub_6F0EDDC0` | `CGxDeviceD3d_SkinCopyVerticesToMappedVB` | 成功 |
| `0x6F0EE210` | `sub_6F0EE210` | `Gx_CopyVerticesFormat2_MMX` | 成功 |
| `0x6F0EE310` | `sub_6F0EE310` | `Gx_CopyVerticesFormat2_SSE` | 成功 |
| `0x6F0EE430` | `sub_6F0EE430` | `Gx_SkinVerticesFormat2_SSE` | 成功 |
| `0x6F0EE530` | `sub_6F0EE530` | `CGxDeviceD3d_LockDynamicIndexRing` | 成功 |
| `0x6F0EE5D0` | `sub_6F0EE5D0` | `CGxDeviceD3d_LockDynamicVertexRing` | 成功 |
| `0x6F0EEA50` | `sub_6F0EEA50` | `CGxDeviceD3d_UploadDynamicVertices` | 成功 |
| `0x6F0EEC20` | `sub_6F0EEC20` | `CGxDeviceD3d_UploadBindDynamicIndices` | 成功 |
| `0x6F0EE9F0` | `sub_6F0EE9F0` | `CGxDeviceD3d_FlushIndexedPrimitiveBatch` | 成功 |
| `0x6F13A9B0` | `RenderQueue_StageUpdate` | `RenderQueue_UpdateGxStages` | 成功；避免与 CWorld stage 混淆 |
| `0x6F7668D0` | `sub_6F7668D0` | `TerrainShadow_Selector12_EnqueueBatches` | 成功 |
| `0x6F7B28F0` | `sub_6F7B28F0` | `BattleNet_DispatchCommandOpcode` | 成功 |
| `0x6F7B7830` | `sub_6F7B7830` | `BattleNet_ApplyICONDATARecord` | 成功 |

另将 `0x6FBC5EA0..0x6FBC5ED0` 的 13 个 scratch dword 分别命名为：

```text
g_GxUploadVertexCount, g_GxUploadPositionPtr, g_GxUploadPositionStride,
g_GxUploadNormalPtr, g_GxUploadNormalStride, g_GxUploadExtraPtr,
g_GxUploadExtraStride, g_GxUploadGroupSlotPtr, g_GxUploadGroupSlotStride,
g_GxUploadUv0Ptr, g_GxUploadUv1Ptr, g_GxUploadUv0Stride,
g_GxUploadUv1Stride
```

stride/FVF table 与三个默认 source 也已写回语义名。kernel、outer upload、vertex-ring
Lock 和三个 SIMD helper 均写入了真实 function type。

### 9.2 Comments

2026-07-11 kernel-gate 复审后，相关函数均有稳定合同的 repeatable function comment，
kernel 与 outer upload 另有不可逆风险 comment；本轮新增/更新 60 余个关键 instruction/data
comment。重点包括：

- `0x6F0E5BC6`：`this+0x5C` 唯一 helper 副作用；
- `0x6F0EDD3E/0x6F0EDD6C/0x6F0EDD7F/0x6F0EDD9B`：existing-buffer 短路与 D3D VB/IB 创建；
- `0x6F0EE5F3..0x6F0EE6A7`、`0x6F0EE552..0x6F0EE5B9`：两条 ring 的 wrap、flags、base/next；
- `0x6F0E362E`：vtable `+0x68` dispatch；
- `0x6F0E3569`：vtable `+0x6C` dispatch；
- `0x6F0EDDC0..0x6F0EE1EE`：真实 2 参数 ABI、format switch、skin/copy loop、
  palette slot 无 bounds check 与 source scratch 推进；
- `0x6F0EE210/310/430`：format-2 MMX/SSE copy 与 SSE skin 三个快路；
- `0x6F13A5BD/0x6F0E3920/0x6F0E70E0`：groupCount/palette bind 链，最终写
  `CGxDevice+0x1A0/+0x19C`；`0x6F0E40C6` 明确标注 `+0x198` 不是 count；
- `0x6F0EEB71..0x6F0EEBA6`：Lock 返回 mapped pointer、SEH try/filter/handler、唯一
  kernel caller 与异常后继续 Unlock；
- `0x6F0EEAB0..0x6F0EEC17`：统计、全局、ensure、ring、CPU kernel、Unlock、FVF/stream 与返回语义；
- `0x6F0EEB85`：新 P4 kernel gate，NULL mapped pointer 必须调原函数；
- `0x6F0EEC68..0x6F0EECDD`：index range/ring/copy/Unlock/SetIndices/BaseVertex 配对；
- `0x6F0EEC7C..0x6F0EECA2`：index memcpy 的 SEH normal/filter/handler，证明 handled
  copy fault 对 outer return 不可见；`0x6F0EECCA` 标注 SetIndices HRESULT 未检查；
- `0x6F0EECDD`：`CGxDeviceD3d+0x71C` native base 写入；
- `0x6F0EEA43`：实际 D3D9 DIP；
- `0x6F138F79..0x6F138F8F`：普通单-slice caller；`0x6F138F8A` 读取内部 primitive type，table `0x6FB66C94` 将值 3 映射到 `D3DPT_TRIANGLELIST(4)`；
- `0x6F13A305/0x6F13A32C`：fallback multipass upload 与 0/N slice loop；
- `0x6F13A439/0x6F13A44F`：special/alpha upload 与 0/N slice loop；
- `0x6F95D8E4/0x6F95D8E8/0x6F95D8EC`：vtable 三槽目标。

IDA 数据库已再次显式保存到 `E:\Work\War3\Game.dll.i64`，`idc.save_database(...)`
返回成功；本轮最终保存后文件大小为 241068987 bytes。保存后抽查 helper/palette/state rename、function type、
两类 function comment、关键指令 comment 与 vtable data comment 均可读回。

本轮另用 MCP `ida_bytes.get_bytes` 逐项复核 C++ opcode fingerprint。kernel tail 真实函数内
字节只有 `0x6F0EE1E8..0x6F0EE1F0` 的
`5F 5E 5B 8B E5 5D C2 04 00`；后继首字节为 `8D`。因此 C++ tail fingerprint 已从错误的
10-byte `... 00 CC` 收紧为函数自身 9 bytes，避免第二个静态 false negative。

2026-07-11 fingerprint `0x12` 复审又写回：`0x6F0EEA50` function comment 已纠正
COFF/export timestamp 混淆；`0x6F1380A3` 标注 `A1 moffs32` 的 RVA `0x1380A4`
HIGHLOW 与 relocation-aware exact fingerprint；`0x6FB549C4` 标注
`0x56BD0E1B` 仅为 Export Directory timestamp。IDB 随后显式保存并回读确认。

同日 v2 `0x80` 复审再次更新 `0x6F0EEA50` comment：记录 raw header ImageBase
`0x6F000000`、mapped header `0x62290000`、RVA `0x16C` 不在 `.reloc`，以及只允许
preferred/runtime-loaded 两个精确值的 fail-closed 规则；IDB 再次保存并回读。

同日 P4 irreversible safety patch 又更新 `0x6F0EDDC0` function-entry comment 与
`0x6F0EEB85` sole-callsite comment：写入三态 detour outcome、fault-safe `SafeCopy/SafeEqual`、
nested/overflow 不得继承父 authorization、重复终态调用不得 CPU rescue，以及 NULL mapped
pointer 仍必须进原函数。两处 `set_comments` 均返回成功；
`ida_loader.save_database('E:\Work\War3\Game.dll.i64', 0)` 返回 `True`，保存后两条 comment
均通过 `ida_bytes.get_cmt` 回读一致。本轮未修改或重新裁决 `+0x6C` index path 注释。

同日 observe v3 配对复审写回 `0x6F0E352B` immediate flush、`0x6F13A63D` batch begin、
`0x6F13A6BE` unconditional tail flush、`0x6F0EEA43` actual DIP raw/correlated 边界、
`0x6F0EEC17` vertex upload 无 DIP 返回点与 `0x6F0EECF4` index upload 无 DIP 返回点。
`ida_loader.save_database` 已将数据库显式保存到 `E:\Work\War3\Game.dll.i64`，返回成功；
六处 comment 随后均由 IDA MCP 读回，文件大小仍为 241068987 bytes。

同日 observe v4 Stage/Batch 复审又完成以下 IDA 写回：

- 为 `0x6F1380A0/0x6F13A5E0/0x6F13A780/0x6F1375C0/0x6F363020/0x6F3681C0/`
  `0x6F368E30/0x6F184EE0/0x6F139800/0x6F13A9B0/0x6F7668D0/0x6F7B28F0/0x6F7B7830`
  写入或替换 repeatable function comment；
- 写入真实 function type，重点纠正无参 flush、Common fastcall、CWorld stage thiscall、group
  thiscall，以及两个 BattleNet 伪 stage 来源；
- 在 queue record writer、Common call setup、Stage-11 caller/case、group switch、TerrainShadow
  enqueue 与 BattleNet RTTI 等位置新增 37 个 instruction comment；
- 上表 4 个 rename 均由 IDA MCP 回读成功，关键 type/function comment/instruction comment 也逐项回读；
- `ida_loader.save_database(path, 0)` 已显式保存到 `E:\Work\War3\Game.dll.i64`，返回 `True`，
  保存后文件大小为 `241068987` bytes。

## 10. 仍为 Unknown

以下项目不能从当前静态 ASM 直接升级为生产结论：

1. v4 已得到 `585217/148682/53045` 的 `0/1/2` fan-out；其余固定地图分布及 0-DIP 在
   `outsideDispatch/noActive/activeEpochMismatch` 之外的业务原因占比仍待实机；
2. DXVK 侧原始 stream-0 COM wrapper 与 native `IDirect3DVertexBuffer9*` 的对象身份是否可直接比较；
3. transparent、auto-instancing、index split、debug skip 和递归 draw 的完整 ordinal/flag 生成规则；
4. format `1/3/5` 在普通 geoset 中的真实覆盖率，以及 optional diffuse 非空时的字节合同；
5. MinHook rollback 失败在目标环境中的实际触发概率；设计上仍必须 fail closed；
6. flush palette 与 upload-time live palette 在所有特殊路径中的实际匹配率；P4 当前逐字节检查，不匹配即回退；
7. P3/P4 尚未 build/runtime，新合同的 `dip/shadow/outlineSuppressedAfterSkip`、fuse、
   reservation leak 与 restore failure 是否能在三图前台测试保持全 0；
8. 成功 skip 后 native VB 已 Lock/Unlock 但内容未写。虽然所有授权 consumer 都必须使用
   GPU lease，仍需 runtime 证明后续 CPU fallback、device reset 与 ring wrap 无回归。
9. device consumer 的 suppress/one-shot stream override 由独立线程接入；在它能消费
   `GpuSkinResolvedDraw.requiresSuppression()` 并完成主画面/shadow/outline 合同前，P4 runtime
   开关必须保持默认 off。
10. `CWorld+0x16C/+0x170/+0x174` 三个 group list 的完整对象分类。当前 ASM 只闭环了
    groupIdx、list offset、stage callsite 和入队链，不能把旧注释中的“单位/建筑/装饰物”升级为事实。
11. 同一 `renderablePart=queueRecord+0x00` 在一个 flush window 内是否可能跨 group/stage 复用。
    原版 Common ABI 只提供该指针与 layerIndex；P1A 应增加冲突计数，发生冲突时拒绝晋级。

`0x6F0E5BC0` 不再是 Unknown：真实函数共 6 条指令，只读取首个栈参数并执行 `this+0x5C += vertexCount`，没有其他数据读写或调用。

这些 Unknown 不阻止 P1 只观察或 P1B 双跑，但任何一项不满足时都必须阻止对应 draw 晋级到 P3/P4。

## 11. 后续逆向写回规则

1. 涉及 Game.dll 渲染语义、hook ABI 或 native bypass 的结论，必须先查看真实 ASM；伪代码只能辅助定位。
2. 新事实达到“地址、调用约定、关键字段和上下游控制流均闭环”后，先在 IDA 对应函数或关键指令写 rename/comment，再更新本页。
3. 若现有 IDA 名称已经更准确则保留，只补充 comment，不为统一风格而降级名称。
4. 尚未由 ASM 或运行时观测闭环的内容只进入 Unknown，不写成函数名或确定性 comment。
5. 每次文档更新记录 IDA 操作的地址、成功/失败及数据库保存结果，避免后续 Agent 重复踩同一地址和 ABI 的坑。

## 12. P3 前 RenderQueue sidecar 冲突/递归契约（2026-07-11）

### 12.1 ASM 边界

本轮通过 IDA MCP 在目标 MD5 `267861a0dfd416dbad13e7ee3ec7794a` 上重新回读：

- `RenderBatch_Submit` 仍位于 `0x6F1375C0`；`0x6F1376D6` 起 75 bytes 与 8.12.2
  记录一致，其中 `0x6F1376EE` 写 `queueRecord+0x08=layerIndex`，`0x6F1376F4` 写
  `queueRecord+0x00=renderablePart`；
- `RenderQueue_FlushSortedItems` 的 Common call setup `0x6F13819E` 起 17 bytes 仍为
  `FF 75 FC 8B 17 8B 4D F8 50 FF 77 08 E8 31 24 00 00`，即 `EDX=[record+0x00]`，
  首个栈参为 `[record+0x08]`。

这些 ASM 事实只证明 sidecar key 与 live layer 的来源和 consumer 同一性。它们不证明同一
`renderablePart` 是否会在一个 flush window 内以不同 group/stage/layer 重现，也不证明这种重现
是否属于合法 multipass；该频率仍是 runtime Unknown，不能用静态分析猜测为 0。

### 12.2 C++ fail-closed sidecar

`RenderQueueTracker::MarkTagStage` 现在在每条 20-byte record 上统一 upsert tag、stage 和
`+0x08 layerIndex`。同一 semantic epoch/key 内：未知字段可被首个已知值补齐；相同已知值幂等；
不同已知值保留首值并设置 sticky field conflict，禁止 last-writer-wins。累计统计口径为：

- `conflictingEntries`：entry/epoch 首次出现任一 conflict；
- `tagConflicts/stageConflicts/layerConflicts`：entry/epoch 对应 bit 首次置位。

只读 `GetSemanticState` 返回 canonical tag/stage/layer、conflict mask 与当前 epoch；只读
`GetSemanticConflictStats` 返回累计计数。兼容 `GetTagStage` 不会替 P3 做 conflict gate。
后续 eligibility 必须拒绝 query miss、conflict、未知必需字段、live layer mismatch 或 snapshot
epoch stale。producer 递归若在同 epoch 复用 key 会被 conflict 捕获；若递归 flush/reset 推进
epoch，旧 sidecar 自动失效而不是与新语义合并。

现有 tracker epoch 仍按 16 bit 打包；本补丁不声称解决 wrap 后陈旧 entry 与当前 key 偶合的
长期风险。P3 长时运行前仍需确定 full-generation 或 wrap invalidation 策略。

### 12.3 IDA 写回与验证边界

IDA instruction comments 已写入并回读 `0x6F1376EE`、`0x6F1376F4`、`0x6F1381A7`，明确区分
native field/key 事实与 DXVK fail-closed policy；`ida_loader.save_database` 已成功保存
`E:\Work\War3\Game.dll.i64`。本轮没有 rename、type 修改或 binary patch。

按 Test Conductor 独占约束，本线程未 build、deploy、启动 War3 或运行 AutoTest；冲突计数是否为
0、合法 multipass 是否触发 layer conflict，以及 P3 eligibility 是否正确消费新 snapshot，仍待独占
runtime 验证。

## 13. P4 kernel/index/DIP RVA-only ASM 复核（2026-07-11）

### 13.1 证据范围与地址规则

本节使用磁盘目标 `Game.dll` 的真实 x86 反汇编，不使用伪代码推 ABI。
文件 MD5 为 `267861a0dfd416dbad13e7ee3ec7794a`，SHA1 为
`88ab432160fb84c23b096c3fc022bfbcb3cb1a1a`。由于运行时基址可变，本节全部静态位置只以
RVA 表达；运行时地址必须由 `gameBase + RVA` 计算。

### 13.2 `RVA 0x0EDDC0` 的唯一 caller 与真实 ABI

对全 `.text` 的 direct-call 目标搜索只找到一条 `call RVA 0x0EDDC0`：

```asm
RVA 0x0EEB71  push [ebp+08h]          ; vertexCount -> vertex-ring Lock
RVA 0x0EEB74  mov  ecx, ebx           ; CGxDeviceD3d*
RVA 0x0EEB76  call RVA 0x0EE5D0       ; EAX = mappedDst or NULL
RVA 0x0EEB7B  mov  [ebp-04h], 0       ; enter caller SEH level 0
RVA 0x0EEB82  push eax                 ; sole kernel stack argument
RVA 0x0EEB83  mov  ecx, ebx           ; this = CGxDeviceD3d*
RVA 0x0EEB85  call RVA 0x0EDDC0
RVA 0x0EEB8A  mov  [ebp-04h], -2      ; leave inner SEH level
```

kernel 入口在 `RVA 0x0EDDC0`，尾部在 `RVA 0x0EE1EE` 执行 `retn 4`。
caller 在 call 后直接改 SEH level，不检查 `EAX`。因此唯一可写入 hook 合同的 ABI 是：

```cpp
using SkinCopyKernelFn =
    void (__thiscall*)(void* gxDeviceD3d, void* mappedDst);
```

kernel 不是纯函数。除 mapped VB 写入外，generic loop 会在
`RVA 0x0EE0A9..0x0EE0F3` 推进 normal/extra/group-slot/UV/position scratch pointer。
但在本次已读取的后续 outer/index/DIP ASM 中，actual DIP 只从
`RVA 0xBC5EA0` 取 `NumVertices`，没有重读其他 source scratch pointer。这只能支持
当前 exact path 的 kernel-only gate，不能推广成“任意 caller 都可跳过这些副作用”。

### 13.3 vertex Lock -> kernel -> Unlock 票据

vertex-ring helper `RVA 0x0EE5D0` 的确定状态转换是：

| RVA | 真实指令/动作 | 合同含义 |
|---:|---|---|
| `0x0EE5E9` | `mov edx,[this+format*8+0x6E0]` | 读 old vertex `next` |
| `0x0EE5F3` | `cmp oldNext+count,0x4000` | 判断 ring wrap |
| `0x0EE5FA` | wrap 分支写 `base=0` | DISCARD 路径 |
| `0x0EE647` | non-wrap 分支写 `base=oldNext` | NOOVERWRITE 路径 |
| `0x0EE60B` / `0x0EE656` | 压入 `0x2800` / `0x1800` | native Lock flags |
| `0x0EE635` / `0x0EE68D` | `call [VB-vtable+0x2C]` | 真实 VB Lock |
| `0x0EE63C` | Lock HRESULT 失败后 `xor eax,eax` 返回 | mappedDst=NULL，`next` 未推进 |
| `0x0EE6A7` | 成功后写 `next=base+count` | 只在 Lock 成功后 commit |
| `0x0EE6AE` | 返回 Lock 输出指针 | kernel 的 mappedDst |

两条分支都在 Lock 前写 `base`，但只在 Lock 成功后写 `next`。因此
`base` 存在不能单独证明 Lock 成功。

outer upload 的后续顺序也由 ASM 闭环：

| RVA | 动作 | HRESULT 处理 |
|---:|---|---|
| `0x0EEB64` | ensure dynamic VB/IB | 失败则跳到 epilogue，不进 kernel |
| `0x0EEB76` | vertex Lock | 返回 pointer/NULL，由 kernel caller SEH 保持原语义 |
| `0x0EEB85` | CPU skin/copy kernel | 只有这一个 code xref |
| `0x0EEBB6` | VB Unlock | 返值不分支检查 |
| `0x0EEBCF` | `SetFVF` | HRESULT 忽略 |
| `0x0EEC00` | `SetStreamSource` | 正常路径将其 HRESULT 作为 outer 返回值 |

`RVA 0x0EEB7B..0x0EEB8A` 是 kernel call 的 inner SEH window。Lock 失败返回
NULL 时，原版仍调 kernel；在会写顶点的 skin mode 0/1 且 vertex count 非零时，
首次写会 fault，filter/handler 后继续到
`RVA 0x0EEBA6`，随后仍调 Unlock/FVF/stream。P4 不得在 NULL 上做“更合理”的早返。

### 13.4 index Lock -> copy -> Unlock -> SetIndices 票据

index-ring helper `RVA 0x0EE530` 维护另一组 ring 状态：

| RVA | 真实指令/动作 | 合同含义 |
|---:|---|---|
| `0x0EE543` | 读 `this+0x710` | old index `next` |
| `0x0EE552` | `cmp oldNext+count,0xC000` | 判断 index ring wrap |
| `0x0EE565` / `0x0EE590` | 写 `this+0x70C=0/oldNext` | 产生 native `StartIndex` |
| `0x0EE575` / `0x0EE59E` | `call [IB-vtable+0x2C]` | flags 分别为 `0x2000/0x1000` |
| `0x0EE57C` / `0x0EE5A5` | Lock 失败返回 NULL | `next` 未推进，`StartIndex` 已写 |
| `0x0EE5B9` | 成功后写 `this+0x710=base+count` | index ring commit |

index upload `RVA 0x0EEC20` 的真实后续为：

```asm
RVA 0x0EEC68  mov [this+0714h], primitiveCode
RVA 0x0EEC6E  mov [this+0718h], indexCount
RVA 0x0EEC77  call RVA 0x0EE530       ; index Lock
RVA 0x0EEC7C  mov [ebp-04h], 0        ; enter copy SEH
RVA 0x0EEC83  lea ecx,[count+count]    ; byteCount = indexCount*2
RVA 0x0EEC8B  call memcpy
RVA 0x0EECAF  ...                      ; normal/handler join
RVA 0x0EECB8  call [IB-vtable+0x30]    ; Unlock
RVA 0x0EECCA  call [device-vtable+0x1A0] ; SetIndices
RVA 0x0EECD0  mov eax,[this+0228h]     ; output format
RVA 0x0EECD6  mov eax,[this+eax*8+06DCh] ; current vertex base
RVA 0x0EECDD  mov [this+071Ch],eax     ; BaseVertexIndex ticket
RVA 0x0EECF4  retn 0Ch                 ; no DIP in this function
```

copy SEH range 是 `RVA 0x0EEC7C..0x0EEC93`。如果 Lock 返回 NULL 且 copy fault，
handler 仍合流到 `RVA 0x0EECAF`，继续 Unlock/`SetIndices`/`+0x71C` 写入。
`SetIndices` HRESULT 没有 `test/cmp/jcc`，并在 `RVA 0x0EECD0` 被后续 `EAX` 读取覆盖。
所以 index upload 的“函数返回”不是 SetIndices 成功票据。

### 13.5 actual DIP 消费的状态元组

`RVA 0x0EE9F0` 在 `RVA 0x0EEA43` 通过 D3D9 device vtable `+0x148` 发出 actual
`DrawIndexedPrimitive`。参数压栈证据为：

| RVA | 被消费值 |
|---:|---|
| `0x0EEA08` / `0x0EEA0E` | `indexCount(+0x718)` / `primitiveCode(+0x714)` 传给 primitive-count helper |
| `0x0EEA1B` | helper 返回的 `PrimitiveCount` |
| `0x0EEA1C` | `StartIndex=this+0x70C` |
| `0x0EEA28` | `NumVertices=[RVA 0xBC5EA0]` |
| `0x0EEA2E` | `MinVertexIndex=0` |
| `0x0EEA30` | `BaseVertexIndex=this+0x71C` |
| `0x0EEA36` | `PrimitiveType=[RVA 0xB66C88 + primitiveCode*4]` |
| `0x0EEA43` | actual D3D9 DIP |

该 DIP 不携带任何来自 vertex/index Lock、Unlock 或 `SetIndices` 的成功位。
它只消费当时的 mutable device/global 状态。因此 P4 的 native ticket 必须按下列元组观察：

```text
render thread
+ dispatch/flush scope
+ monotonic vertex-upload epoch
+ DIP ordinal within that upload
+ output format/FVF/stride/stream identity
+ vertexCount + native vertex base
+ primitiveCode + indexCount + StartIndex + BaseVertexIndex
+ actual PrimitiveType/NumVertices/PrimitiveCount draw signature
```

`GxDevice_DrawIndexedRange` 在 `RVA 0x0E3526` 先调 index upload，再于
`RVA 0x0E352B` 立即 flush；common dispatch 又在 `RVA 0x13A6BE` 无条件 tail flush。
特殊/multipass 的 slice loop 还可重复这一流程。所以真实 ASM 只允许 0/1/N fan-out
合同，不允许“一次 upload 天然对应下一个 DIP”。

### 13.6 可逆窗口、逻辑 commit 与物理不可逆点

P4 必须区分三个边界：

1. **可逆窗口**：在 `RVA 0x0EDDC0` detour 内，真实 mapped pointer 已由 native
   Lock 获得，但 detour 尚未返回。任一校验失败都还能 exactly once 调 original
   kernel，并继续处于 caller 的原 SEH window。
2. **逻辑 commit**：detour 决定不调 original kernel 并返回。原 caller 从
   `RVA 0x0EEB8A` 继续，不再有第二个 kernel callsite。从这一刻起，native slice 必须在
   ledger 中标记为 stale，不得作 CPU fallback。
3. **物理不可逆点**：`RVA 0x0EEBB6` 调 VB Unlock。之后 mapped pointer 已失效，
   即使 DIP 阶段发现 index/stream/signature mismatch，也不存在合法的晚 CPU rescue。

从 ASM 得出的最窄裁决如下：

- 不得 bypass `RVA 0x0EEA50` outer upload；原函数必须 exactly once 执行统计、
  globals、ensure、Lock/ring、SEH、Unlock、FVF 与 stream。
- `mappedDst==NULL` 必须调 original kernel，不授权。
- 只有在 kernel detour 的可逆窗口内，完整 consumer plan、GPU lease、palette/static
  bytes、CPU single-DIP baseline 和当前 ring state 均 exact 时才能 commit skip。
- commit 之后的 `SetStreamSource` 失败、index Lock/copy/`SetIndices` 异常、0/N fan-out、
  DIP signature 或 shadow/outline lease mismatch，全部只能 suppress 对应 consumer 并 fuse
  stable key。不得重放 outer upload，不得晚调 kernel，不得读未写 native slice。

### 13.7 IDA 未写回 backlog

用户已确认 IDA 当前不可用，并要求停止所有 IDA/MCP/前台交互。本轮因此
**没有执行任何 rename、set-comment 或 database save**，也没有把历史文档中的旧写回记录
冒充为本轮成功。恢复连接后待办如下：

| RVA | 待回读/写回项 | 建议名称或注释要点 | 本轮状态 |
|---:|---|---|---|
| `0x0EDDC0` | 复核 function name/type/comment | 保留/补齐 `CGxDeviceD3d_SkinCopyVerticesToMappedVB`；2 参数 void ABI、sole caller、kernel-only gate | PENDING: IDA unavailable |
| `0x0EE530` | 复核 function comment | `CGxDeviceD3d_LockDynamicIndexRing`；base 先写、next 仅成功后写、失败返回 NULL | PENDING: IDA unavailable |
| `0x0EE5D0` | 复核 function comment | `CGxDeviceD3d_LockDynamicVertexRing`；format ring、base/next commit 差异 | PENDING: IDA unavailable |
| `0x0EE9F0` | 更新 function comment | `CGxDeviceD3d_FlushIndexedPrimitiveBatch`；记录 `this + 6` 个显式 DIP 参数来源与无 Lock/SetIndices 成功位 | PENDING: IDA unavailable |
| `0x0EEA50` | 更新 function comment | `CGxDeviceD3d_UploadDynamicVertices`；whole-upload bypass 禁止，outer exactly once，只返回 stream HRESULT | PENDING: IDA unavailable |
| `0x0EEC20` | 更新 function comment | `CGxDeviceD3d_UploadBindDynamicIndices`；copy SEH、SetIndices HRESULT 忽略、`+0x71C`、函数内无 DIP | PENDING: IDA unavailable |
| `0x0E3520` | 复核 name/comment | 若现有名称缺失，建议 `GxDevice_DrawIndexedRange`；`0x0E3526` index upload -> `0x0E352B` immediate flush | PENDING: IDA unavailable |
| `0x0EEB76` / `0x0EEB85` / `0x0EEBB6` | instruction comments | Lock 返回 mappedDst、sole kernel call/逻辑 commit、Unlock 物理不可逆点 | PENDING: IDA unavailable |
| `0x0EEC77` / `0x0EECB8` / `0x0EECCA` / `0x0EECDD` | instruction comments | index Lock/Unlock/SetIndices/BaseVertexIndex ticket；特别标注 HRESULT 未检查 | PENDING: IDA unavailable |
| `0x0EEA43` | instruction comment | actual DIP；只有这里能证明真实 draw consumer | PENDING: IDA unavailable |
| `0x0E3526` / `0x0E352B` / `0x13A6BE` | instruction comments | index upload、immediate flush、common unconditional tail flush；支持 0/1/N fan-out | PENDING: IDA unavailable |

待 IDA 恢复后，必须先回读现有 name/type/comment，只做增量更新；不得覆盖更准确的
已有名称，不得在未回读时假定上表任何项已写入数据库。

### 13.8 IDA 写回状态（2026-07-12）

本轮在 IDA MCP 恢复后重新读取真实 `disasm`，未使用伪代码推 ABI。数据库 identity 为：

- preferred base：`0x6F000000`；所有新增/修订注释优先使用 RVA；
- input MD5：`267861a0dfd416dbad13e7ee3ec7794a`；
- IDB：`E:\Work\War3\Game.dll.i64`。

真实 ASM 复核再次确认：`RVA 0x0EDDC0` 共 281 条指令、尾部 `retn 4`，唯一 code xref
为 `RVA 0x0EEB85`；generic loop 在 `RVA 0x0EE0A9..0x0EE0F3` 推进 normal/extra/
group-slot/UV/position source pointer。两个 ring helper 均在 Lock 前发布 base，只在 Lock
成功后提交 next。`RVA 0x0EEA43` 的 DIP 只消费当时的 6 个显式参数与 device pointer，
不消费 Lock/Unlock/SetIndices 成功位。index upload 内无 DIP，而 `RVA 0x0E352B` immediate
flush 与 `RVA 0x13A6BE` common tail flush 共同要求 0/1/N fan-out 合同。

7 个目标函数的最终名字均已在本轮写入前存在且比建议名不差，因此按 11.3 规则保留，
没有执行同名 rename；地址级失败为 0：

| RVA | 最终名字 | name 状态 | function comment 状态 |
|---:|---|---|---|
| `0x0EDDC0` | `CGxDeviceD3d_SkinCopyVerticesToMappedVB` | SUCCESS：保留既有精确名 | SUCCESS：补 generic pointer 副作用、sole xref、`retn 4` |
| `0x0EE530` | `CGxDeviceD3d_LockDynamicIndexRing` | SUCCESS：保留既有精确名 | SUCCESS：补 base 先写、失败 NULL/next 不提交 |
| `0x0EE5D0` | `CGxDeviceD3d_LockDynamicVertexRing` | SUCCESS：保留既有精确名 | SUCCESS：既有 format ring/base-next 注释回读通过 |
| `0x0EE9F0` | `CGxDeviceD3d_FlushIndexedPrimitiveBatch` | SUCCESS：保留既有精确名 | SUCCESS：补齐 6 个 DIP 参数来源和无成功位 |
| `0x0EEA50` | `CGxDeviceD3d_UploadDynamicVertices` | SUCCESS：保留既有精确名 | SUCCESS：既有 outer exactly-once/whole-bypass 禁止注释回读通过 |
| `0x0EEC20` | `CGxDeviceD3d_UploadBindDynamicIndices` | SUCCESS：保留既有精确名 | SUCCESS：补 copy SEH、HRESULT 覆盖、`+0x71C`、无 DIP |
| `0x0E3520` | `GxDevice_DrawIndexedRange` | SUCCESS：保留既有精确名 | SUCCESS：新增 index upload -> immediate flush 与 0/1/N 边界 |

instruction comment 写回/回读状态：

| RVA | 状态 | 写回内容 |
|---:|---|---|
| `0x0EEB76` / `0x0EEB85` | SUCCESS：既有注释回读 | Lock mappedDst/NULL、sole kernel call 与可逆窗口 |
| `0x0EEBB6` | SUCCESS：新增并回读 | VB Unlock 是物理不可逆点，之后不能晚 CPU rescue |
| `0x0EEC77` / `0x0EECB8` | SUCCESS：新增并回读 | index Lock 的 NULL/commit 规则；正常/异常路径均到 Unlock |
| `0x0EECCA` / `0x0EECDD` | SUCCESS：既有注释回读 | SetIndices HRESULT 未检查；写实际 BaseVertexIndex ticket |
| `0x0EEA43` | SUCCESS：既有注释回读 | actual D3D9 DIP consumer |
| `0x0E3526` | SUCCESS：新增并回读 | native index upload/bind 先于 immediate flush |
| `0x0E352B` / `0x13A6BE` | SUCCESS：修订并回读 | 移除过窄的 fanout max=2 表述，收紧为静态 ASM 允许 0/1/N |

`ida_loader.save_database('E:\Work\War3\Game.dll.i64', 0)` 返回 `True`；保存后再次通过
IDA MCP 回读 7/7 最终名字、7/7 function comment marker 与 11/11 instruction comment
marker，全部成功。保存后的 IDB 大小为 `241068987` bytes。本线程未修改 type 或 binary，
也未 build、deploy、启动 War3 或运行 AutoTest。

## 14. P4 format-resident VB / poison 最终安全合同（2026-07-12）

本节补充第 13 节尚未覆盖的 format 分槽、caller-owned SEH normal-return 语义与 DXVK
resource retirement 合同。它是 P4 首次上机前的安全审查要求；并行实现仍在修复中，本节不表示
build、runtime、画面或性能测试通过。

### 14.1 数据库身份与地址规则

- input MD5：`267861a0dfd416dbad13e7ee3ec7794a`；
- preferred ImageBase：`0x6F000000`；
- IDB：`E:\Work\War3\Game.dll.i64`；
- 文档与 IDA 新注释中的静态位置以 RVA 为权威；表中的 preferred VA 仅为
  `0x6F000000 + RVA`；
- runtime 地址必须计算为 `verified Game.dll module base + RVA`。不得把 preferred VA
  直接用于 ASLR 后进程，也不得仅凭可读内存跳过 hash/PE/opcode 身份门。

### 14.2 真实 ASM：format 只索引自己的 VB 与 ring

| RVA | preferred VA | 真实指令/动作 | 证明 |
|---:|---:|---|---|
| `0x0EEB40` | `0x6F0EEB40` | `mov ecx,[ebx+228h]` | 从 `CGxDeviceD3d` 读取当前 output format |
| `0x0EEB52` | `0x6F0EEB52` | `lea eax,[ebx+6C0h]` | 取得 per-format VB pointer table 基址 |
| `0x0EEB58` | `0x6F0EEB58` | `lea eax,[eax+ecx*4]` | 只形成 `&VB[format]`，随后作为 ensure 的 `arg_8` |
| `0x0EDD3E` | `0x6F0EDD3E` | `cmp dword ptr [ecx],0` | ensure 只检查 caller 传入的当前 format VB 槽；非空即跳过该槽创建 |
| `0x0EE5E3` | `0x6F0EE5E3` | `mov ecx,[ebx+228h]` | vertex Lock helper 再次读取当前 format |
| `0x0EE5E9` | `0x6F0EE5E9` | `mov edx,[ebx+ecx*8+6E0h]` | `next` 以 `format*8` 分槽 |
| `0x0EE5FA/0x0EE647` | `0x6F0EE5FA/0x6F0EE647` | 写 `this+0x6DC+format*8` | wrap/non-wrap 的 `base` 也按 format 分槽 |
| `0x0EE6A7` | `0x6F0EE6A7` | 写 `this+0x6E0+format*8` | 只有成功 Lock 才提交该 format 的 next |
| `0x0EEBA6/0x0EEBAC` | `0x6F0EEBA6/0x6F0EEBAC` | 重读 format，再取 `[this+0x6C0+format*4]` | normal/SEH handler 合流后也只 Unlock 当前 format VB |

outer upload 对 ensure 的实参顺序由 `RVA 0x0EEB46..0x0EEB64` 闭合为：

```text
this = CGxDeviceD3d*
arg_0  = outputFormat
arg_4  = 0x4000 vertices
arg_8  = &this->dynamicVB[outputFormat]
arg_C  = 0xC000 indices
arg_10 = &this->sharedDynamicIB          // this+0x6D8
```

因此 `VB[0]`、`VB[2]`、`VB[4]` 是三个独立指针，且各自拥有独立 base/next。format 是 layout
分类，不是可替代 resource identity。以下时序是危险且必须被合同覆盖的：

```text
format 2 kernel skip -> VB[2] range becomes poison
format 0 upload/replacement/CPU write -> touches only VB[0]
format 2 selected again -> SetStreamSource(VB[2])
RVA 0x0EEA43 or another fan-out DIP -> may consume VB[2] stale bytes
```

所以“看到 format 0 replacement”绝不能退休 format 2/4 poison。即使某个 COM/DXVK 地址后来
被复用，也必须由 identity generation 区分旧资源与新资源。

### 14.3 caller-owned SEH 与 trampoline normal-return 证明

`CGxDeviceD3d_UploadDynamicVertices` 的真实窗口为：

```asm
RVA 0x0EEB76  call RVA 0x0EE5D0       ; EAX = mappedDst or NULL
RVA 0x0EEB7B  mov [ebp-04h],0         ; caller-owned try level begins
RVA 0x0EEB82  push eax
RVA 0x0EEB85  call RVA 0x0EDDC0       ; target is detoured
RVA 0x0EEB8A  mov [ebp-04h],-2        ; only normal call return reaches here
RVA 0x0EEB93  mov eax,1               ; EXCEPTION_EXECUTE_HANDLER filter
RVA 0x0EEB99  ...                      ; handler restores state
RVA 0x0EEBA6  ...                      ; handler/normal join before Unlock
```

MinHook 只改变 `RVA 0x0EDDC0` 的入口，不改变 SEH frame 的 owner。detour 在
`CallOriginal` 分支调用 original trampoline 后，若 trampoline fault，stack unwind 会越过 detour，
直接由 outer caller 的 handler 接住；detour 中 trampoline call 之后的计数、RAII 或 callback
不能假定执行。

由此必须严格区分：

```text
original selected  != original entered normally
original entered   != original returned normally
normal return      != exact overwrite identity/range already matched
```

CPU overwrite retirement 必须由 detour 在 trampoline **正常返回后**发布 normal-return ack，
再由 poison owner 对 exact resource identity、generation、interval 和 layout 做交集扣除。NULL
mapped pointer 进入 original 后由 caller SEH 吸收的路径没有 normal-return ack，不能清任何 range。

### 14.4 Lock flags 与 actual DIP 的 stale-byte 后果

vertex-ring Lock 的立即数经真实 ASM 复核：

| RVA | flags | D3D9 语义 | poison 裁决 |
|---:|---:|---|---|
| `0x0EE60B` | `0x2800` | `DISCARD | NOSYSLOCK` | wrap/rename 意图，不证明 CPU 已写，也不等同 CommonBuffer 析构 |
| `0x0EE656` | `0x1800` | `NOOVERWRITE | NOSYSLOCK` | append 意图，不证明目标 interval 已被完整覆写 |

`NOSYSLOCK` 是两条路径共有的 `0x0800` bit。Lock 成功、ring next 推进、Unlock、FVF 或
SetStreamSource 成功都不能替代 normal-return CPU overwrite proof。

`RVA 0x0EEA43` 的 DIP 从 mutable device/global state 取得 stream、FVF、NumVertices、
StartIndex 与 BaseVertexIndex，不读取“kernel 正常返回”或“CPU bytes valid”位。`RVA 0x0E352B`
immediate flush、`RVA 0x13A6BE` tail flush 与 special/multipass 路径又允许 0/1/N fan-out。
因此每个相关 DIP 都必须独立查询 poison；一次 consumer suppress 或另一个 format 的正常 draw
不能令旧 range 消失。

### 14.5 DXVK poison、reset 与 lifetime ABI

以下是语义合同，字段名可在并行实现中按现有类型落位，但不得削弱比较维度：

```text
PoisonIdentity = exact(commonResource, identityGeneration)
PoisonRange    = PoisonIdentity + byte/vertex interval + stride/FVF/format
CpuOverwriteAck = same PoisonIdentity + exact covered interval/layout
                  + originalTrampolineReturnedNormally
ResourceRetirementAck = same PoisonIdentity + D3D9CommonBuffer destructor event
ResetOwnerAck  = exact(owner identity, resetGeneration)
```

强制规则：

1. kernel skip commit 前先登记 `PoisonRange`；失败/overflow 调 original，不得先 skip 后补 ledger。
2. poison 至少绑定 DXVK `D3D9CommonBuffer* commonResource + identityGeneration`。COM pointer、
   native slot、format 或裸地址只能作附加佐证，不能替代该 pair。
3. 只允许 `CpuOverwriteAck` 扣除它实际覆盖的区间；中间覆盖需要 split，容量不足时保留较宽
   poison fail closed，不能扩大 clear。
4. 只允许 exact `ResourceRetirementAck` 一次性退休同一
   `commonResource+identityGeneration` 的剩余 ranges。普通 reset、format replacement、
   `DISCARD` 或 owner detach 不是析构通知。
5. reset 请求生成 `R` 后立即关闭新 authorization；owner acknowledgement 必须来自发起 reset
   时绑定的同一 owner，且 `ackGeneration == R`。不得用 `>=`、新 owner 或其他 reset 的 ack
   代替。reset 控制状态与 poison retirement 是两道独立门。
6. reset 完成要求 exact owner ack、TLS/global transaction quiescence，以及 outstanding poison
   已通过第 3/4 项归零；否则保持 deferred/fail closed。
7. 当前并行实现已启用 detour 与 original trampoline storage 的 process-lifetime 策略。
   map/device epoch 变化可替换 manager/owner/resource generation，但不拆仍可调用的 hook，
   不清 trampoline，也不把旧 poison 静默算作 reset 完成。

### 14.6 本轮 IDA 写回与回读

本轮先回读再增量写回，没有覆盖更精确的函数名：

| RVA | 最终函数名 | 本轮动作 |
|---:|---|---|
| `0x0EEA50` | `CGxDeviceD3d_UploadDynamicVertices` | 保留名称；追加 format/SEH/ASLR/process-lifetime comment |
| `0x0EDD10` | `CGxDeviceD3d_EnsureDynamicVertexAndIndexBuffers` | 保留名称；修订 current-format-only ensure comment |
| `0x0EE5D0` | `CGxDeviceD3d_LockDynamicVertexRing` | 保留名称；移除旧 whole-Lock bypass 暗示，补 flags/poison comment |
| `0x0EDDC0` | `CGxDeviceD3d_SkinCopyVerticesToMappedVB` | 保留名称；补 caller-owned SEH/normal-return ack comment |
| `0x0EE9F0` | `CGxDeviceD3d_FlushIndexedPrimitiveBatch` | 保留名称；把旧 whole-upload skip 表述修订为 kernel-only bypass/stale DIP |

写回计数：

- function comment fields：`8`（5 个 repeatable marker + 3 个过时 non-repeatable 修订），覆盖
  `5` 个函数；
- instruction comments：`17`，覆盖 `0x0EEB40/52/58`、`0x0EDD3E`、`0x0EEB64`、
  `0x0EE5E3/E9`、`0x0EE60B`、`0x0EE656`、`0x0EEB7B/82/85/8A/99/A6/AC` 与
  `0x0EEA43`；
- function renames：`0`，因为 5/5 既有名称均已精确；
- 合计 comment-field writes：`25`。

保存后 IDA MCP 回读为 function name `5/5`、repeatable marker `5/5`、修订后的
non-repeatable function comment `3/3`、instruction disassembly comment `17/17`。其中
`RVA 0x0EEB99` 的 disassembly comment 已保存，但 handler landing 无对应 Hex-Rays ctree
placement；这不是数据库写入失败。`ida_loader.save_database(...)` 返回 `True`。本轮未修改
function type、local/stack 名称或 binary bytes。
