# CPU 多线程蒙皮 Phase B：native freeze/join 与 sidecar ABI 证据包

状态：2026-07-15 静态闭合；只读取 Game.dll 1.27a 原始字节、真实 x86 ASM、xref/SEH 与
当前 DXVK 源码。没有启动 War3，没有 build/deploy/AutoTest。结论采用
Confirmed / Inferred / Unknown / Contradicted，不把设计意图当成原生事实。

## 1. 结论先行

**现有 D3D9 Lock/Unlock sidecar 与 callback ABI 9 原样不能在绕过 manager `onUpload` 的同时，
把一个 CPU-only batch 提交给 staging worker。**

这不是说 Phase B 架构不可行，而是现有接口缺失。可行的最小架构是：manager 在 flush candidate
assembly 已得到 immutable static snapshot、palette bytes 与 exact candidate token 后提前提交；
native kernel detour 在成功 Lock 后只做 non-blocking `tryAcquire`，Ready 才把 staging 复制到
`mappedDst`，否则立即走原 kernel。这个路径需要独立的 CPU-MT sidecar，或 ABI 升级后的
submit/acquire/commit/abort 回调；不能把 diagnostics-only Lock evidence 擅自升级为 authority。

`onUpload` 不是当前 upload 的 producer 点：它在原 kernel/outer upload 完成后的 publication
阶段才发生，时间上已经晚于 `0x6F0EEB85`。

## 2. authoritative native window

### 2.1 outer upload 与 caller-owned SEH

`CGxDeviceD3d_UploadDynamicVertices @ 0x6F0EEA50` 是 13 个栈参数的 `thiscall`，以
`retn 0x34` 返回。关键原始字节：

```text
6F0EEA50: 55 8B EC 6A FE 68 F0 7C AD 6F 68 BC 1F 77 6F 64
6F0EEB71: FF 75 08 8B CB E8 55 FA FF FF C7 45 FC 00 00 00 00
6F0EEB82: 50 8B CB E8 36 F2 FF FF C7 45 FC FE FF FF FF EB 13
```

真实指令序列：

```asm
6F0EEB71  push [ebp+arg_0]       ; vertexCount
6F0EEB74  mov  ecx, ebx          ; CGxDeviceD3d*
6F0EEB76  call 6F0EE5D0          ; EAX=mappedDst or NULL
6F0EEB7B  mov  [ebp-4], 0        ; enter inner try level
6F0EEB82  push eax               ; sole kernel argument
6F0EEB83  mov  ecx, ebx
6F0EEB85  call 6F0EDDC0          ; CPU skin/copy kernel
6F0EEB8A  mov  [ebp-4], -2       ; normal return only
...
6F0EEB93  mov  eax, 1            ; accept filter
6F0EEB99  mov  esp, [ebp-18h]    ; handler
6F0EEBA6  ...                     ; normal/fault join
6F0EEBB6  call [vertexBuffer+30h] ; Unlock
```

IDA EH4 records confirm：

- outer function range `[0x6F0EEA50,0x6F0EEC1A)`，scope table `0x6FAD7CF0`；
- inner try range `[0x6F0EEB7B,0x6F0EEB8A)`；
- filter `0x6F0EEB93`，handler `0x6F0EEB99`，join `0x6F0EEBA6`。

因此原 kernel fault 会直接越过 detour 的 post-call 代码，进入 caller handler，再继续 Unlock。
Lock 返回 NULL 时原版仍调用 kernel；非零 vertexCount 的写 fault 也由该 SEH 吸收。替换路径不能
把 NULL 当成功、不能伪造 normal return，也不能依赖 detour 栈析构完成唯一 terminal settlement。

### 2.2 kernel ABI 与可重入性

`CGxDeviceD3d_SkinCopyVerticesToMappedVB @ 0x6F0EDDC0` 只有一个 code xref：
`0x6F0EEB85`。ABI 为：

```cpp
void __thiscall SkinCopyVerticesToMappedVB(
    CGxDeviceD3d* self, void* mappedDst); // retn 4
```

在 detour 调用点稳定可见的业务寄存器/栈参数只有 `ECX=self` 与 `[ESP+4]=mappedDst`。
vertexCount、position/normal/extra/group/UV 指针和 stride 来自
`0x6FBC5EA0..0x6FBC5ED0` 的 process-global scratch；palette、groupCount、skinMode、format
来自 `self+0x19C/+0x1A0/+0x224/+0x228`。generic loop 还会逐顶点推进多个 source globals。

kernel 内未发现锁或 reentry guard。process-global mutable cursors 意味着并发或嵌套进入同一
native path 会互相覆盖；安全接管必须在进入 worker 前复制输入并以 exact token 关联，不能让
worker 直接借用这些 globals。

## 3. 最早 freeze 点

不存在一个“Lock 前、单点同时拥有全部 source/palette/output identity”的位置。必须拆成两段：

| 数据 | 最早精确点 | 已证内容 | 尚未证明 |
|---|---:|---|---|
| palette pointer/count | `0x6F13A5BD..0x6F13A5C5` | `ECX=[renderable+0xF0]`、`EDX=palette3x4`；setter 写 device `+0x19C/+0x1A0` | bytes immutable、content/layout generation |
| position/normal/extra/group/UV + strides | `0x6F138F55` 前 | `RenderQueue_ApplyDrawStateAndSamplerPair` 已从 `CGeosetData` 取齐并调用 wrapper | pointee ownership/generation、跨 worker lifetime |
| wrapper ABI | `0x6F0E35B0` | `ECX=vertexCount`、`EDX=positions`、11 个栈参数、`retn 0x2C` | output resource/Lock identity |
| process-global scratch | `0x6F0EEAB8..0x6F0EEB37` 后 | outer 已发布 13 个 dword，device palette/format 可同时读取 | globals 对重入/并发不稳定 |
| mapped pointer | `0x6F0EEB76` 正常返回 | Game.dll 的 `EAX=mappedDst` | resource/storage/map generations |
| 完整 output Lock identity | 成功 D3D9 `LockBuffer` sidecar | CommonBuffer/COM/device/real/mapping/allocation generations、range/flags/depth/mapped pointer | source/palette/candidate token；且当前 payload 只读诊断 |

因此 Phase B 的正确 producer 点不是 Lock sidecar。producer 必须在 flush candidate assembly
冻结 static bytes/owner generations 与 palette；Lock sidecar/ kernel detour只负责把随后出现的
destination identity 和同一 candidate token 精确相关。

## 4. 最晚 owner join 与 Unlock

CPU staging worker 应在 kernel detour 被调用前已经达到 Ready；detour 只能 non-blocking 获取。
若 Ready，则 owner thread 在 caller-owned SEH 内完成 staging-to-`mappedDst` copy，并在 detour
正常返回前完成该 job 的 join/terminal transition。

- `0x6F0EEB8A`：只在 kernel/detour 正常返回后执行，是可区分 normal-return 的最后 native 点；
- `0x6F0EEBA6`：normal 与 handled-fault 已合流，已经太晚，不能再判断 worker output 是否可提交；
- `0x6F0EEBB6`：Unlock 是 mapped pointer 的物理不可逆边界，之后绝不能 late CPU rescue。

这里的“join”只指当前 native VB upload owner。Main/Outline/Shadow 的 GPU consumer lease 仍按各自
实际消费或 frame retirement 结算，不能和 kernel 前 staging worker join 混为一个点。

## 5. exception / non-local exit / destructor / reset

### Confirmed

- Win32 SEH fault 会跳过 detour trampoline 后的 normal-return callback，并在 caller handler 后继续
  Unlock；因此 worker lease 不能只由 detour 栈 RAII 拥有。
- outer hook 在完整 native trampoline 返回后重新取得控制，可取消/结算外部 sidecar；但 outer
  return value 不能替代 kernel normal-return marker，因为 handled fault 也会到 outer 正常返回。
- 当前 DXVK reset/lifetime 合同会 fail closed、停止新 authorization，并要求 transaction
  quiescence 与 exact owner/reset generation；这属于实现安全合同，不是 Game.dll 原生析构事实。

### Unknown

- Game.dll 的完整 `CGxDeviceD3d` destructor/reset caller 图没有在本 evidence window 中闭合；
  `0x6F0EEA50/0x6F0EDDC0` 本体没有 reset 或析构分支。
- import table 与 IDA strings 中均未发现 `setjmp/longjmp`，但这不能排除静态实现、其他形式的
  non-local exit 或未审 indirect caller；全局 longjmp 语义保持 Unknown。
- Game.dll object teardown、D3D device reset 与某个在途 CPU-MT batch 的原生跨类先后仍 Unknown；
  工程实现必须以独立 owner generation、reset cancel 和最终 quiescent join 覆盖这一空洞。

## 6. 调用线程、OS worker 与重入

Game.dll 确实创建 OS Engine workers：

```asm
6F05E61B  mov  ecx, offset MainLoop_6F05F710 ; StartAddress
6F05E620  call OsThread_BeginThreadEx
...
6F15753A  push edx                            ; ArgList
6F15753B  push ecx                            ; StartAddress
6F157540  call ds:_beginthreadex
```

`EvtScheduler_CreateEngineWorkers @ 0x6F05E410` 创建 `workerCount-1` 个 `EvtSched#N` 线程；
`MainLoop_6F05F710` 用 `GetCurrentThreadId` 命名 `Engine %x`，并执行 registered callbacks。
同一 MainLoop 也由 `SignalAndDrainMainLoop_05E710` 以参数 1 直接调用。

这证明“存在 OS scheduler workers”，但没有证明某个固定 worker 是 render thread。普通
RenderQueue→wrapper→outer→Lock→kernel 是同步嵌套 `call`，其中没有 thread hop；它会留在
触发该 callback 的当前 Engine thread。registered callback/indirect dispatch 使 direct xref
无法闭合所有上传来自同一 OS thread。故：

- exact upload thread affinity：Unknown；
- native path 内部 worker fork/join：未发现；
- 同一调用链内同步顺序：Confirmed；
- process-global scratch 无锁，设计必须拒绝 wrong-thread/nested/reentrant token：Confirmed 的
  机械风险，不能反推“原游戏绝不会重入”。

## 7. 为什么现有 Lock sidecar / ABI 9 不够

现有 `NativeOutsidePoisonVertexLockEvidence` 明文是 diagnostics-only；普通 traffic 在 O0/O1
policy mask 为 0 时不会发布。它只有 output resource/storage/mapping/range/flags/depth，缺少：

1. `renderablePart/geosetData/layerIndex` 与 flush/dispatch/upload epoch、candidate token；
2. position/normal/extra/group/UV 的 immutable bytes、owner/layout/content/map/device generation；
3. palette bytes、count、generation 与 MXCSR 合同；
4. CPU producer generation、batch/job token、staging pointer/size、Ready/cancel/reset 状态；
5. copy-before-Unlock commit 状态、normal/fault/outer/Unlock result 与 exactly-once terminal settlement。

ABI 9 的同步 callback 只有 flush、dispatch begin/end、GPU bypass preflight、normal-return CPU
rewrite proof、upload、DIP 和 fanout。没有 CPU batch submit、lease acquire、staging pointer、
commit 或 abort。fast CPU-only outer path只把 `gxDeviceD3d/vertexCount/outputByteCount` 传给
bridge，不传输入流或 palette。`resolveCpuRewriteOutputProof` 在原 kernel 正常返回后才运行，
只证明 poison rewrite；`onUpload` 在 completed upload publication 才运行，两者都不是当前 job
的提前 producer。

`war3_cpu_skin_mt` 虽已编入 Meson source list，但 manager/device/hook/bridge 没有 producer 集成
引用。这也是“核心已存在”与“native Phase B 已接线”必须分开的证据。

## 8. 最小新增合同（设计，不是已实现）

若保持 ABI 9，应新增独立 CPU-MT sidecar，至少持有：

```text
CpuJobKey = exact(flushEpoch, dispatchEpoch, candidateToken,
                  renderablePart, geosetData, layerIndex,
                  mapEpoch, deviceEpoch, resetGeneration)
SourceProof = owned static snapshot + layout/content generations
PaletteProof = owned bytes + groupCount + generation + MXCSR
StagingLease = producerGeneration + jobToken + pointer/size + Ready state
DestinationProof = successful Lock identity + mapped range + output format/stride
Terminal = exactly one of CopiedNormal / CpuFallback / FaultCancelled /
           OuterCancelled / UnlockFailed / ResetCancelled / Retired
```

flush assembly submit 后，kernel detour只允许 exact-key `tryAcquire`；未 Ready、wrong thread、
reentry、generation drift、Lock mismatch 或 reset pending 均立即调用原 kernel。任何 worker wait
或 assist 都不能越过 `0x6F0EEB8A`，任何 staging write 都不能发生在 `0x6F0EEBB6` 之后。

## 9. IDA 增量写回

本批遵守 read-before-write，读回既有 name/type/comment 后只追加或纠正更窄结论：

- `GxDevice_UploadDynamicVertices @ 0x6F0E35B0`：13 参数 fastcall 类型；
- `0x6F13A5C5/0x6F13A5CA/0x6F138F55`：palette/static input freeze 边界；
- `0x6F0EEB76/0x6F0EEB8A/0x6F0EEBA6`：output identity、latest join、fault join；
- `sub_6F157530 -> OsThread_BeginThreadEx`，`sub_6F05E410 -> EvtScheduler_CreateEngineWorkers`，
  并写入精确 ABI/线程来源注释；
- `MainLoop_6F05F710`、`0x6F05E620/0x6F05F833/0x6F05F880`：OS worker 与 indirect
  callback 的证据边界；旧“QueueFlush 直接触发 render”注释被真实函数体否定；
- kernel/outer/palette producer 的 repeatable comments 追加 Phase B 约束，没有删除既有更精确
  P4 注释。

全部 name/type/comment 已读回。数据库保存到 `E:\Work\War3\Game.dll.i64`，保存后大小
`251,827,343` bytes。

## 10. evidence ledger

### Confirmed

- C-PB-001：唯一 kernel caller、`thiscall` ABI、caller-owned SEH range 与 Unlock 顺序。
- C-PB-002：palette/static streams/output Lock identity 的最早点必须拆成两段。
- C-PB-003：staging copy/join 必须在 detour 内、`0x6F0EEB8A` 前完成；Unlock 后不可 rescue。
- C-PB-004：现有 Lock/Unlock sidecar diagnostics-only，ABI 9 无 CPU producer lifecycle callback。
- C-PB-005：Game.dll 存在 EvtSched `_beginthreadex` workers；native upload 内无 thread hop。

### Inferred

- I-PB-001：保持 ABI 9 的独立 exact-token CPU-MT sidecar，是比复用 O0/O1 payload 更小的安全
  接线面；仍需代码、静态审查和运行证据后才能升级为实现结论。

### Unknown

- U-PB-001：具体哪一个 Engine/main thread 在所有场景拥有 RenderQueue upload。
- U-PB-002：Game.dll 全局 non-local exit、device destructor/reset 与在途 upload 的完整 caller 图。
- U-PB-003：真实 flush lead、worker crossover、Ready hit rate 与 owner assist 收益。

### Contradicted

- X-PB-001：成功 D3D9 Lock evidence 本身已经足够授权 CPU-MT job。
- X-PB-002：`onUpload` 可以作为当前 native upload 的 worker submit 点。
- X-PB-003：到 `0x6F0EEBA6` 或 Unlock 后仍可等待 worker 并补写 mapped VB。

