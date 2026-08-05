# 28. Warcraft III 1.27a GPU 蒙皮接管可行性与实施基线

> 日期：2026-07-20
> 状态：P1A、P1B、P2 与 P3 主画面已完成隔离桌面正确性验证；P3/VS-A 的 outline 共享 slice
> 已通过 `--outline-all` 专项覆盖。P4 native kernel bypass 的
> owner/poison/callback/reset/index-ticket/consumer settlement 已实现，既有 full/light hard gate
> 已通过；Common exact-negative seal、O0 successful-Lock sidecar 与 O1a-v2 Lock/kernel/Unlock
> sidecar 也已有隔离 light/full 运行证据。O0/O1 当前始终 report-only、authority=0，不能据此
> 删除 SafeCopy。旧 compute/bypass 已证明语义正确，但同图完整 ABBA 仍有约 `+2.35 ms/frame`
> 的 CPU 侧负增量，因此性能主线已转为 fixed-function ubershader 内的 VS-in-draw hybrid。
> 真实 consumer-fenced C++ input lease 与 fixed-function VS-A prelude 已接线，并通过显式
> `vertex_shader` 隔离 crash-gate。随后新增的显式 `vertex_shader_input_only`（VS-B0）已在普通图、
> lifecycle 与高压图通过：目标 cohort 不再分配 compute output、不建 compute dispatch，Main/Shadow
> 直接消费 input lease；原生 CPU kernel 与 P4 零权限仍完整保留。默认 Compute 路线未改变，
> 独立显式 `vertex_shader_bypass`（VS-B1）现已在普通隔离图真实跳过 22,055 次 CPU kernel，
> Main/Shadow input consumer、poison/index/ledger 全闭合；reset 冷窗口分类修复后的 lifecycle、
> 高压格式图与第二进程 relaunch 均已 PASS。随后加入候选前 8 字节 skinMode/format 筛选，以及
> production O1 actual-Lock NoOverlap 轻量端点；两者的普通 crash-gate 与 lifecycle 均闭合。
> 但是多轮隔离 ABBA 的 B1 CPU 负增量仍约为 `+2.79～+3.28 ms/frame`，没有稳定正收益，
> 因此 B1 仍只能显式启用，不能作为产品默认。当前仍没有正式前台 FPS 结论。
> 全部 takeover/skip 模式仍默认关闭。旧的 whole-`0x6F0EEA50` bypass 合同已判废，新合同保留
> 完整 outer upload，只在 `0x6F0EDDC0` 收到真实 mapped pointer 后 gate CPU 写循环。
> 目标版本：`Game.dll`，ImageBase `0x6F000000`，MD5 `267861a0dfd416dbad13e7ee3ec7794a`。

原生函数 ABI、vtable 三槽、0/1/N DIP fan-out、hook 风险与 IDA 写回记录见 [native_asm_contract.md](native_asm_contract.md)。该页是后续 native hook 的地址与调用约定基线。

CPU 多线程蒙皮 Phase B 的 source/palette/output-Lock freeze、caller SEH、最晚 owner join、
EvtSched OS worker 与现有 sidecar/ABI 缺口见
[cpu_mt_phase_b_native_evidence.md](cpu_mt_phase_b_native_evidence.md)。该证据包确认“核心可行但尚未
接线”：现有 O0/O1 Lock payload 仍是 diagnostics-only，不能替代 CPU job authority。

`CWorld` 历史短名的类级归属已于 2026-07-15 闭合：stage/RenderScene/RenderWorldGroup 的
`ECX` 是 `CWorldFrameWar3*`，不是 RTTI 类 `CWorldObjects*`。完整类族、字段、vtable 与
producer/lifecycle 分卷见
[30_cworld_class_family_full_reverse](../30_cworld_class_family_full_reverse/README.md)。

## 1. 阶段结论

### 1.1 可行性判断

对 Warcraft III 1.27a 的普通单位/建筑 `CGeosetData` 路径，GPU 蒙皮接管是可行的，当前置信度为高。

真正需要复刻的算法很小：

1. 每顶点读取一个 `uint8_t matrixGroupSlot`；
2. 用该 slot 读取一条已经由 War3 CPU 生成好的 3x4 group palette 矩阵；
3. 变换 position 与 normal；
4. 将结果写成 War3 原来交给 D3D9 的 FVF 交错顶点格式。

War3 并没有在 D3D9 上启用 vertex blending，也没有把原始 bone weights 交给 GPU。复杂的动画树、骨骼层级、序列插值和 matrix-group 平均仍由 War3 生成。第一阶段 GPU 接管只替换最后一段“逐顶点 CPU 乘矩阵 + 动态 VB 上传”，不需要重写整套动画系统。

### 1.2 推荐技术路线（2026-07-17 更新）

compute pre-skin 仍是已经通过 parity、Shadow 与 Main 正确性门的基线，也是多消费者大模型的可复用
路径；但它不再是默认性能扩展方向。最新 ABBA 已证明当前退化来自同步 CPU render lane 的逐调用
管理成本，而非 measured GPU 饱和。新的性能主线是在 DXVK 自己的 fixed-function VS ubershader
前部增加受严格白名单控制的 War3 skin prelude，而不是给所有可编程 War3 vertex shader 打补丁。

compute 基线仍保留以下价值：

- compute 输出和原版完全相同的 post-skin FVF 顶点；
- 原来的材质、雾、纹理、alpha test、pixel shader、索引和 draw call 保持不变；
- 同一份 GPU skin output 可供主画面、CSM、点阴影、几何描边和未来 G-buffer 重用；
- 任一识别或资源条件在 kernel skip 前失败时，可以逐 draw 回退原版 CPU 路径；skip 后
  native slice 已 stale，只能 suppress + fuse。

这条路线的难点是 DXVK 资源接线、生命周期和 draw 状态覆盖，不是蒙皮公式本身。

## 2. 已证实的原版调用链

以下链路由 IDA 汇编、vtable 槽和最终 D3D9 调用共同闭环，不是仅凭旧文档推断：

```text
RenderQueue_FlushSortedItems                         VA 0x6F1380A0
  -> RenderQueue_Dispatch_Common                    VA 0x6F13A5E0
  -> RenderQueue_UpdateItemWorldMatrix              VA 0x6F13A510
  -> RenderQueue_BindDispatchBlock                  VA 0x6F13A710
  -> sub_6F138F70
  -> RenderQueue_ApplyDrawStateAndSamplerPair       VA 0x6F138EE0
  -> Gx dynamic vertex upload wrapper               VA 0x6F0E35B0
  -> CGxDeviceD3d dynamic vertex upload             VA 0x6F0EEA50
  -> CGxDeviceD3d_SkinCopyVerticesToMappedVB        VA 0x6F0EDDC0
  -> IDirect3DVertexBuffer9::Unlock
  -> IDirect3DDevice9::SetFVF
  -> IDirect3DDevice9::SetStreamSource
  -> CGxDeviceD3d_UploadBindDynamicIndices          VA 0x6F0EEC20
  -> CGxDeviceD3d_FlushIndexedPrimitiveBatch        VA 0x6F0EE9F0
  -> IDirect3DDevice9::DrawIndexedPrimitive
```

`RenderQueue_FlushSortedItems` 对 `CGeosetData+0x104 != 0` 的条目每次 draw 都令状态更新生效，因此动态姿态不会依赖 D3D9 vertex blending 状态。最终送入 D3D9 的 stream 0 已经是 CPU 蒙皮完成的顶点。

## 3. `0x6F138EE0` 的真实参数

IDA 的伪代码遗漏了 `__fastcall` 的 ECX/EDX 参数。汇编确认调用 `0x6F0E35B0` 时：

| 位置 | 值 | 含义 |
|---|---|---|
| `ECX` | `[CGeosetData+0x0C]` | vertex count |
| `EDX` | `[CGeosetData+0x10]` | bind-pose positions |
| stack | `12` | position stride |
| stack | `[CGeosetData+0x58]` | bind-pose normals |
| stack | `12` | normal stride |
| stack | null | optional diffuse/extra stream，按格式决定 |
| stack | `[CGeosetData+0x4C]` | per-vertex matrix-group slot |
| stack | `([CGeosetData+0x48] != 0)` | group-slot stride，蒙皮模型为 1 byte |
| stack | UV record pointers from `[+0x94]` | 最多两层 UV |
| stack | `8` / `8` | UV stride |

因此 `+0x4C` 不是 `uint32_t bone index`，而是每顶点一个 `uint8_t` group slot。旧资料中若把它描述成每顶点独立 weights/indices，不能直接作为 GPU 接管契约。

## 4. Palette 的来源与语义

### 4.1 draw 前的 palette 绑定

`RenderQueue_UpdateItemWorldMatrix`（VA `0x6F13A510`）完成以下动作：

1. 从 `renderablePart+0x08` 取得当前 palette slot；
2. 当 `CGeosetData+0x104 != 0` 时，以 `[CGeosetData+0xF0]` group count 和当前 palette 调用 `0x6F0E3920`；
3. 调用 `0x6F0E38E0` 写入 skin mode。

已识别的 `CGxDeviceD3d` 状态写入函数：

| VA | 写入字段 | 含义 |
|---|---|---|
| `0x6F0E70E0` | `+0x19C` / `+0x1A0` | group palette pointer / group count |
| `0x6F0E7050` | `+0x224` | skin mode |
| `0x6F0E7030` | `+0x228` | output vertex format |

P4 字段硬门：palette count 的真实字段是 `+0x1A0`，不是 `+0x198`。后者只在
设备构造的前一段 matrix/state block 中初始化，palette setter 不读写它。共享工作区当前
`kGxPaletteCountOffset` 已为 `0x1A0`；后续实现不得回退，也不得在 count 不可读时 skip kernel。

项目现有 `Hook_RuntimeMatrixWrite`（`0x6F12E600`）已经能够观察 group palette，但生产 GPU 路径不应依赖有上限的诊断快照。最稳的来源是 draw/flush 时的 live palette pointer，快照只用于比对与回退诊断。

### 4.2 matrix-group 构造

```text
CModel_AllocAndFillGroupPalette                     VA 0x6F12FED0
  -> CGeosetData_BuildGroupBlendedPalette           VA 0x6F12E600
     -> CMatrixGroup_BlendOutputMatrix              VA 0x6F12E200
```

`CMatrixGroup_BlendOutputMatrix` 的语义：

- group size 1：直接复制该 bone matrix；
- group size 2：两矩阵求和后乘 `0.5`；
- group size 3：求和后乘 `1/3`；
- group size N：求和后除以 N。

没有逐顶点浮点 bone weights。每个 vertex 最终只索引一条已经混合好的 group matrix。项目现有最大 capture 数 256 与 `uint8_t` slot 相符。

## 5. CPU skin kernel 的精确公式

核心函数：VA `0x6F0EDDC0`。

### 5.1 模式

- `skinMode == 0`：按输入 stride 拷贝 position/normal/extra/UV；
- `skinMode == 1`：执行 group-palette 蒙皮；
- 当前普通模型生产路径只需要优先覆盖 0/1，其他值必须回退原版。

### 5.2 position

对顶点 `(x, y, z)`，矩阵存储为连续 12 个 float：

```text
out.x = M[0] * x + M[3] * y + M[6] * z + M[9]
out.y = M[1] * x + M[4] * y + M[7] * z + M[10]
out.z = M[2] * x + M[5] * y + M[8] * z + M[11]
```

### 5.3 normal

```text
out.nx = M[0] * nx + M[3] * ny + M[6] * nz
out.ny = M[1] * nx + M[4] * ny + M[7] * nz
out.nz = M[2] * nx + M[5] * ny + M[8] * nz
```

原版不做 inverse-transpose，也不在此处 normalize normal。GPU parity 模式必须先严格照做，不能在第一版“顺便修正”法线算法。

### 5.4 数值一致性

原函数包含 SSE 优化分支，但与标量公式同义。GPU shader 应：

- 原样使用这套 3x4 布局，不经通用 `Matrix4` 转置；
- parity 阶段避免 fast-math/FMA 造成不必要差异，可使用 precise/NoContraction；
- 以 position/normal 的最大绝对误差和屏幕图像差分做验收。

## 6. 原版动态 VB 与 FVF 输出

`0x6F0EEA50` 在 6 个动态 VB 中选择一个，使用 16384 顶点 ring，通过 DISCARD/NOOVERWRITE lock 写入。输出格式如下：

| format | D3D9 FVF | 内容 | stride |
|---:|---:|---|---:|
| 0 | `0x012` | XYZ + NORMAL | 24 |
| 1 | `0x052` | XYZ + NORMAL + DIFFUSE | 28 |
| 2 | `0x112` | XYZ + NORMAL + TEX1 | 32 |
| 3 | `0x152` | XYZ + NORMAL + DIFFUSE + TEX1 | 36 |
| 4 | `0x212` | XYZ + NORMAL + TEX2 | 40 |
| 5 | `0x252` | XYZ + NORMAL + DIFFUSE + TEX2 | 44 |

外层函数随后调用 `SetFVF` 和 `SetStreamSource`。`0x6F0EEC20` 上传 `uint16_t` index，并根据当前 vertex ring offset 设置 base vertex。`0x6F0EE9F0` 最终把该 base vertex 传入 `DrawIndexedPrimitive`。

这意味着生产接管不能只“跳过 CPU memcpy”：还必须保持或覆盖 stream 0 与 `BaseVertexIndex` 的配对关系。

在已闭环的 `RenderQueue_ApplyDrawStateAndSamplerPair` 普通 geoset 路径中，optional diffuse/extra 输入固定传 null，因此实际优先覆盖的是 format `0/2/4`（0/1/2 层 UV）。format `1/3/5` 仍应由 P1A 统计验证后再决定是否纳入，不能仅凭格式表假定普通单位一定会命中。

## 7. DXVK 侧已锁定的接入点

源码锚点：

- `src/d3d9/d3d9_device.cpp`：`D3D9DeviceEx::DrawIndexedPrimitive`
- `src/d3d9/d3d9_device.cpp`：`D3D9DeviceEx::PrepareDraw`
- `src/d3d9/d3d9_device.cpp`：`D3D9DeviceEx::BindVertexBuffer`
- `src/d3d9/d3d9_device.cpp`：提交 `VkDrawIndexedIndirectCommand` 的主 draw/outline lambda

共享工作区有其他实现线程持续改动，行号会漂移；以函数名为稳定锚点。

当前顺序为：

```text
UploadPerDrawData
ResolveCommittedDip
if post-skip resolve failed: suppress + fuse + return
War3TryCaptureShadowCasterDrawIndexed
material override
PrepareDraw
EmitCs(drawIndexed)
optional geometry outline PrepareDraw + drawIndexed
```

`BindVertexBuffer` 最终只是向 DXVK CS 队列发出：

```cpp
ctx->bindVertexBuffer(slot, DxvkBufferSlice, stride);
```

`DrawIndexedPrimitive` 的 draw lambda 独立捕获 `BaseVertexIndex`，写到 Vulkan 的 `vertexOffset`。

因此最小侵入的 DXVK draw 接管面已经明确：

1. `PrepareDraw` 完成后、`drawIndexed` 之前只做 slot 0 的 one-shot GPU output 绑定；
2. 直接绑定 `DxvkBufferSlice` 与原 FVF stride；
3. draw 时把 `vertexOffset` 改为 GPU output slice 对应的 base vertex，通常可设计为 0；
4. 主 draw 与同函数内的 geometry outline 重用这份绑定；
5. 函数退出时把 D3D9 `VertexBuffers` dirty flag 重新置位，保证下一个普通 draw 恢复 D3D9 记录的 stream state。

不建议把 GPU output 伪装成新的 COM `IDirect3DVertexBuffer9` 再走一次完整 D3D9 状态机；DXVK 内部 one-shot override 更直接，也更容易保证逐 draw 回退。

### 7.1 compute 不能逐 draw 调度

进一步读码确认，`DxvkContext::dispatch()` 会在 `commitComputeState()` 中调用 `spillRenderPass(false)`。所以“每个单位在 `PrepareDraw` 后 dispatch 一次 compute”虽然功能上能排序正确，却会在世界绘制期间反复结束 render pass，是不可接受的生产设计。

生产结构必须拆成两段：

```text
Hook_FlushSortedItems（原版/reimpl 遍历开始前）
  -> 枚举本批可见 RenderBatchElement
  -> 复制本帧 live group palette 到 upload ring
  -> 批量 enqueue 一次或极少数 compute dispatch
  -> compute pass 结束，后续世界 draw 开始新的 graphics render pass

每个 DrawIndexedPrimitive
  -> PrepareDraw 恢复原 D3D9 图形状态
  -> 只绑定预计算好的 GPU output slice
  -> drawIndexed
```

当前工程已经 Hook `RenderQueue_FlushSortedItems`，并可访问：

- `g_numOfElementsPtr`；
- `g_batchArrayPtr`；
- `RenderBatchElement`（20 bytes）；
- `renderablePart`、`layerIndex` 和 `layerStatePtr`。

`renderablePart+0x0C` 可取得 `CGeosetData/meshData`，`renderablePart+0x08` 是 palette slot。全局 palette buffer 的地址与 `base + 48 * slot` 读取路径也已在现有 shadow runtime 中实现。因此 flush 级批量准备不需要再逆向一套可见集合。

FVF/output format 可在 P1A 由 `0x6F0EEA50` 的真实调用学习为稳定映射：

```text
(resource generation, CGeosetData*, layerIndex/subIndex) -> format/FVF/stride
```

首次未知组合继续走 CPU 并记录；下一帧 flush 使用当前帧 palette 预计算。这里缓存的是静态 layout 契约，不是上一帧 pose，因此不会引入一帧姿态延迟。

## 8. 推荐的数据与同步架构

### 8.1 静态资源

每个 `CGeosetData` 建一次 device-local immutable 资源：

- bind-pose positions；
- bind-pose normals；
- `uint8_t` group slots；
- diffuse/extra（存在时）；
- 1~2 层 UV；
- indices 可继续使用 War3 原 index path，第一阶段不必接管。

项目的 `War3ModelResourceCache` 与 `CanonicalSkinContract` 已经具备大部分 CPU 侧资源语义，应该扩展既有契约，而不是再建第二套模型解析器。

### 8.2 每帧动态资源

- palette upload ring：连续 native 3x4 float；
- skin job descriptor ring：源 slices、palette offset、vertex count、output format；
- device-local output arena：按 fence 回收，至少覆盖多帧 in-flight 生命周期；
- output slice 以 `Rc<DxvkBuffer>`/`DxvkBufferSlice` 形式同时交给主画面和阴影重放。

### 8.3 compute job

基础实现为一线程一个 vertex：

```text
slot = groupSlots[vertexId]
M = palette[slot]
skin position/normal with the exact formulas in section 5
copy optional diffuse and UVs
write exact FVF interleaved output
```

不同 FVF 可用 shader specialization/output-format 分支，第一版不需要做复杂 meshlet 或 indirect pipeline。

### 8.4 同步

必须显式满足：

```text
host/palette upload -> compute shader read
compute shader write -> vertex input read
```

DXVK command stream 天然保证提交顺序，但 shader binding metadata 仍必须正确声明 input read/output write。使用高层 `DxvkContext` compute 路径时，已有 hazard tracker 可以完成这条同步：

1. output storage-buffer binding 声明 `VK_ACCESS_SHADER_WRITE_BIT`；
2. `dispatch()` 将 compute store 记录到 buffer；
3. one-shot `bindVertexBuffer()` 令 `GpDirtyVertexBuffers` 生效；
4. `checkGraphicsHazards()` 发现该 vertex slice `hasGfxStores()`；
5. DXVK 自动插入 compute-write 到 `VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT` 的 barrier。

这比 `beginExternalRendering()` + 手写 command-list pipeline 更适合主线。后者会 flush/invalidate 全部状态，现有源码也明确把它视为重路径。

项目可复用的 shader 构造模板是 `D3D9SWVPEmulator`：它用 `DxvkSpirvShaderCreateInfo + DxvkBindingInfo + DxvkSpirvShader` 注册运行时 shader，再由 `DxvkContext` 高层状态机绑定资源。GPU skin 可使用预编译 SPIR-V header 构造同类 compute shader，不需要为此新增裸 Vulkan 提交系统。

输出 slice 的 `Rc<>` 必须被 command stream 持有到 GPU 使用结束，不能用裸指针跨帧。

## 9. native hook 与 D3D9 draw 的配对

建议使用 flush 级预计算表与严格的 dispatch-scope / upload-epoch / DIP-ordinal contract。
ASM 已确认一个 vertex upload 可对应 0、1 或 N 个实际 DIP，因此旧的全局 one-shot
pending token 假设必须废弃。P4 最终时序是：

```text
Hook_FlushSortedItems
  -> publish current-frame PrecomputedGpuSkinDraw table
outer upload observer 0x6F0EEA50
  -> allocate upload epoch and arm TLS only after manager+host preauthorization
  -> ALWAYS call original +0x68
     -> native globals / ensure / Lock / ring
     -> kernel hook 0x6F0EDDC0 receives real mapped pointer
        -> NULL or any mismatch: call original kernel
        -> exact authorization: skip only per-vertex writes
     -> native Unlock / SetFVF / SetStreamSource
  -> publish committed skipped-upload ledger
D3D9 DrawIndexedPrimitive
  -> resolve exact DIP reservation
  -> PrepareDraw
  -> bind GPU output stream 0 and draw with vertexOffset=0
  -> shadow/outline consume the same lease
```

至少用以下字段做匹配：

- render thread id；
- `CGeosetData*` / immutable resource generation；
- vertex count；
- output format/FVF/stride；
- palette pointer + group count；
- 期望的原版 stream/base-vertex 特征；
- dispatch scope id、单调 upload epoch 与该 epoch 下的 DIP ordinal；
- native dispatch 路径标志，明确 common、special、multipass、split、skip 与 recursion。

任何字段不一致、scope 泄漏、epoch stale、资源未就绪或 format 不支持，在 kernel skip
之前都调用原 kernel。P1-P3 继续无条件调用原 kernel；P4 一旦 kernel 已 skip，不能等到
DIP 阶段再补救。

### 9.1 为什么必须保留完整 `0x6F0EEA50`

`0x6F0EEA50` 不只是“执行 CPU 矩阵乘法”。它还会：

1. 累加 Gx vertex 统计；
2. 写 `dword_6FBC5EA0 = vertexCount`，最终 `0x6F0EE9F0` 用它作为 `DrawIndexedPrimitive::NumVertices`；
3. 按 `CGxDeviceD3d+0x228` 选择/创建对应 FVF 动态 VB；
4. 推进 vertex ring offset；
5. `SetFVF`；
6. `SetStreamSource`。

随后 `0x6F0EEC20` 又会把当前 vertex ring offset 写成最终 `BaseVertexIndex`。如果只跳过 CPU kernel/outer upload，而不处理这些副作用，就会出现错误 vertex count、旧 input layout、旧 stream 或错误 base vertex；这种错误在画面上会伪装成“GPU 蒙皮撕裂/部件乱飞”。

更关键的是，`0x6F0EE5D0` Lock 失败会返回 NULL；原函数仍在 SEH 内调用 kernel，异常
被 handler 吸收后继续 Unlock/FVF/stream。入口 bypass 无法忠实保留这个失败语义。

因此主画面接管分成两个子阶段：

1. **P3 覆盖消费**：原 kernel 继续写 native VB，GPU output 只在 DXVK draw 前覆盖
   stream/base vertex。任一失败都可使用 native output。
2. **P4 kernel gate**：outer upload 仍完整执行；只有 kernel 收到真实非空 mapped pointer、
   manager+host 已预授权、GPU batch 为 Submitted、palette/static bytes bitwise exact、全部
   consumer reservation 完整时，才不执行写循环。

成功 skip 后 native ring 已由原 Lock 推进，native VB 也会正常 Unlock，但该 slice 没有被写，
内容必然视为 stale。此后 DIP resolve、shadow backing 或 outline 任一失败，都必须抑制该
consumer 并 fuse stable key，绝不能晚调 CPU kernel 或悄悄读取 native stream。

### 9.2 CPU baseline 先学习，P4 后消费

`expectedIndexCount` 的 CPU baseline 不能依赖 P4 authorization 自己产出的
`upload.expectedIndexCount/predictedStartIndex`：

1. 未确认 key 始终调用原 kernel；outer observer 打开 CPU probe。
2. prepared draw 从 exact static primitive record 提供 expected index count；CPU 路径用它和
   当次真实 DIP 的 `StartIndex/PrimitiveCount/NumVertices` 校验 ring 预测。
3. dispatch end 只在 exactly one triangle-list DIP、signature 全精确时建立 baseline；
   `0/N` fan-out 只记录 mismatch，绝不能提前按 `1` 学习。
4. 后续 P4 才把该 CPU baseline 与 live primitive、compute job、palette/static bytes 再次比较。
5. post-skip mismatch 会 fuse stable key 与 layout；在 manager 生命周期内不可通过后续
   CPU draw 或 epoch reset 重新授权。

具体 hook typedef、TLS 字段、opcode fingerprint、MinHook rollback/unload 和验收计数见
[native_asm_contract.md](native_asm_contract.md)。

### 9.3 Consumer reservation / settlement ledger（2026-07-11）

manager 现把“lease 可用”和“consumer 已消费”拆成两个不可混用的状态：

- `GpuSkinConsumerPlan` 用 `known/notRequested/leaseBacked + DipSignature` 固化每个 prepared
  draw 的预留分类；三组 mask 必须是合法 consumer 的无交叠分区，signature 绑定 render
  thread、token、dispatch/upload epoch、stream 0、DIP ordinal 与完整 draw 参数；
- `GpuSkinConsumerLedger` 只保存 `resolved/consumed/cpuFallback/suppressFuse`；`ResolveDip` 和
  `ResolveShadowLease` 只允许置 `resolved`，返回 lease 绝不再增加 consumed；
- `PreparedDraw` 独立保存 `leaseId`、plan、ledger 和 consumer window state。`PlanConsumers`、
  `CommitConsumer`、`FailConsumer` 都校验完整 key + leaseId；commit/fail 只接受合法 single-bit
  consumer，并分别统计 unreserved、duplicate 和 plan mismatch；
- P3 `FailConsumer(CpuFallback)` 合法，原 native VB 仍可消费；P4 拒绝 CpuFallback，任何 pending
  只能进入 `SuppressAndFuse`。P4 host 必须在 `preflightNativeBypass` 同步回调内使用 request 的
  `expectedDipSignature` 完成 plan，否则 manager 不签发 kernel-skip authorization；
- `CloseBatchConsumerWindow` 关闭 batch 内所有 prepared draw。未结算 lease-backed reservation
  会增加 reservation leak：P1/P2/P3 记 CPU fallback，P4 记 suppressed 并 fuse；不做 GPU wait；
- `RetireBatch` 遇到 open/unsettled ledger 时先增加 retire deferred、执行上述 fail-closed
  结算并返回 false，下一次无等待重试才允许 queue output retirement，不能静默抹掉 ledger。

向后兼容边界：device 尚未接入新结算 API 时，Dual/Shadow/Main 仍可 resolve 和走原 CPU/P3
fallback；首次 frame retirement 会以可观测的 `reservationLeak + cpuFallback + retireDeferred`
结算并延后一轮释放。Bypass 不享受该兼容放宽：无显式完整 plan 不授权 kernel skip，任何遗留
pending 仍只 suppress+fuse。新增累计计数为 `classified/resolved/consumed/cpuFallback/suppressed`
以及 `reservationLeak/unreserved/duplicate/planMismatch/retireDeferred`。

## 10. 同一帧消费的边界

### 10.1 可以做到的部分

GPU skin 可以在当前帧消费当前 palette，并在当前帧完成主画面与 CSM 绘制：

- War3 在 draw 前已经完成本帧 group palette；
- DXVK compute 与 draw 可进入同一 command stream；
- 当前 CSM 位于世界阶段后的 `BeforeUi`，理论上可以重用本帧主 draw 生成的 output slices；
- 阴影无需再从 CPU-skinned D3D9 dynamic VB 做一份 GPU copy。

### 10.2 GPU skin 不会自动消除的延迟

以下延迟属于其他模块，不能归咎于 CPU skin data：

- shadow TAA/history blend；
- adaptive ShadowMap 的 2/3/4 帧复用策略；
- point-shadow update period 与每帧 cube-face budget；
-体积光若采用 N-1 CSM snapshot；
- War3 自身动画逻辑 tick 的更新频率。

所以“GPU skin 完成后所有东西天然零延迟”是不准确的。正确目标是先消除 **pose geometry 生产与各 pass 之间不必要的一帧缓存/复制**，再分别治理历史滤波和分帧更新策略。

## 11. 分阶段实施方案

### P1A：只观测，不接管

- 在 `0x6F138EE0` 建立 semantic dispatch scope，并观察 `0x6F0EEA50`；
- 记录 skin mode、format、vertex count、group count、palette pointer；
- 建立 dispatch scope + upload epoch + DIP ordinal 的 0/1/N fan-out 统计；
- 覆盖 common、special 与 multipass，fast return 前校准 stage/tag；
- 不改变原版输出。

验收：配对错误、epoch 泄漏和白名单 palette mismatch 必须为 0；普通单位/建筑命中率与特殊模型比例可量化。

### P1B：GPU/CPU 双跑比对

- 原版 CPU skin 与 draw 保持不变；
- 在 `Hook_FlushSortedItems` 入口按已学习 layout 批量生成 GPU output；
- 用 staging/readback 抽样比对 position/normal/FVF bytes；
- 先放行 format `0/2/4`，再用真实字节合同验证写白色 diffuse 的 `1/3/5`；
- 建立每 format 的误差报告和模型黑名单。

验收：position/normal 误差 `<= 2e-5 * max(1, abs(cpu))`，UV/其余字节 bitwise 一致；无画面行为变化。

**当前状态（已通过）**：Dual v1 的 `alignas(16)`/32 位 `std::vector` 崩溃已修复；最终 artifact
`AutoTest/artifacts/gpu_skin_dual_isolated_v3_20260711_174405` 完成 `1824/1824` parity，
position/normal 与其余 FVF 字节均无 mismatch，协议、palette 与生命周期计数为 0 错配。

### P2：阴影先消费 GPU output

- 主画面仍使用原版 CPU VB；
- CSM/点阴影优先改用本帧 GPU output，描边按共享 backing 的接入顺序单独验收；
- 与现有 `War3DrawTimeVBEntry` 做 A/B，逐步移除重复 post-skin VB copy。

验收：动态 pose 无冻结、无一帧滞后；S1/path blocker/静态阴影行为不回退。

**当前状态（已通过）**：artifact
`AutoTest/artifacts/gpu_skin_shadow_isolated_v3_20260711_201657` 记录 exact GPU shadow backing
`34990` 次、backing reject `0`，并跳过 `485260384` bytes 的 CPU shadow VB 二次复制；动态 pose、
S1、path blocker、UberSplat 与建筑静态阴影检查无回退。

### P3：接管主画面 stream 0

- 对已验证的 skinMode 1 + format 0..5 使用 one-shot DXVK stream override；
- 原 CPU skin/upload 仍然执行，确保任意 draw 可无损回退；
- 原始材质与 index path 不变；
- 不支持的 draw 逐个回退；
- 主 draw 和 geometry outline 重用同一 output。

验收：主画面与原版逐帧截图一致；描边消费相同 slice；无跨 draw stream 污染；契约错配为 0。

**当前状态（主画面通过，outline runtime 待覆盖，仍默认关闭）**：artifact
`AutoTest/artifacts/gpu_skin_main_isolated_v1_20260712_010442` 在 90 秒/1828 帧内记录 main
hit/submitted `26035/26035`、restore `26035/26035`、overlap/pending `0/0`，consumer ledger
闭合且无 leak/unreserved/duplicate/plan mismatch。原 artifact 唯一 FAIL 是把 tracker 的全局
`10671` 次 layer conflict 错设为硬零门；这些 dispatch 全部经 `forceFailClosed` 留在 CPU fallback，
没有进入 exact GPU job。P3 的正确硬门是“任何 conflict 不得进入 exact takeover”，而不是要求
War3 不得在同一 epoch 以多个 layer 复用 renderable。两张截图中主画面、狮鹫、S1、path blocker、
UberSplat 与建筑静态阴影均无可见回退；但 `outlineSubmitted=0`，不能把这次 artifact 作为
outline consumer 的 runtime 证据，最终视觉矩阵必须补一个明确触发 geometry outline 的场景。

### P4：严格白名单跳过 CPU skin kernel

- `0x6F0EEA50` outer detour 只建立 upload TLS 并始终调用原 trampoline；
- `0x6F0EDDC0` kernel detour 是唯一 skip 点；`mappedDst==nullptr` 必须调用原 kernel；
- 仅 Stage 11/common/opaque/single-DIP、`skinMode==1`、format 0/2/4，且 CPU baseline、
  manager+host authorization、Submitted batch、palette/static bytes、lease 和所有 consumer
  reservation 全部 exact 时允许 skip；
- 原生统计、globals、ensure、Lock、ring、SEH、Unlock、FVF、stream、index upload 与 actual DIP
  全部保留，不手写重放；
- `+0x6C` index path 保持原生；manager 用 prepared expected index、live primitive bytes、
  kernel-time ring state与真实 DIP 做闭环，不新增 index detour；
- kernel skip 前失败走逐 draw CPU fallback；skip 后失败只能 suppress consumer + fuse key；
- live state/palette/static/index 校验只能用 fault-safe `SafeCopy/SafeEqual`；任何读取失败在 skip
  前返回 CPU fallback，禁止 `VirtualQuery` 后裸读让原生 SEH 吸收；
- 普通嵌套 dispatch/semantic/upload 与 stack overflow 全部可计数并 fail closed；overflow TLS
  barrier 不得把父 scope/token 暴露给子调用；
- kernel detour 返回 `CallOriginal/BypassCommitted/IrreversibleNoCpuRescue` 三态；重复终态
  kernel call 只记录并抑制，不得再触发 CPU rescue；
- 主 draw 在 `PrepareDraw` 后 one-shot bind GPU stream 0，`vertexOffset=0`；shadow/outline
  必须复用同一 output lease；
- 实验开关默认关闭；fused key/layout 在 manager 生命周期内永久拒绝再次授权。

2026-07-11 实现状态：旧 whole-upload bypass/replay 执行体已删除，并有
`static_assert(!kWholeNativeUploadBypassEnabled)`；apply/outer/kernel 三钩按事务安装，outer
永远调用原上传一次，kernel 是唯一 skip 点。该 P4 代码骨架实现轮按约束未 build/deploy/run，
device suppress 消费由独立 consumer 线程接入。

同日 native hook 安全补丁已新增 fault-safe `SafeCopy/SafeEqual`，bridge 的 PE/opcode/vtable、
device state 与 13 globals 读取已接入；nested/overflow TLS barrier、三态 kernel outcome 及
`postSkipNativeFallback/nested/duplicate/irreversible` 计数接口也已落地。manager 对 live
palette、primitive/index、static stream 的裸 `memcpy/memcmp` 尚属其 owner 接线范围；在这些
比较改用新 API 前，整条 P4 authorization 仍不具备 fault-safe 闭环。

2026-07-12 runtime 前安全收口新增：

- host authorization 绑定真实 `D3D9DeviceEx`、native D3D device、VB/IB COM identity、resource
  generation、active Lock range、mapped pointer 与 device state；
- vertex Lock flags 按真实 ASM 校正为 wrap `0x2800`、non-wrap `0x1800`，即
  `DISCARD/NOOVERWRITE | D3DLOCK_NOSYSLOCK`；index Lock 仍为 `0x2000/0x1000`；
- kernel skip commit 前登记固定容量 native VB poison range；所有 DIP 出口均检查 poison，manager
  缺失、owner 错误或 lease 不 exact 时只能 suppress；真实成功的 CPU kernel rewrite 才能清除覆盖区间；
- poison reset 使用全局 generation，使 map/device reset 即使由非观察线程发起，也会在下一次渲染
  线程入口清除旧 TLS ledger；诊断导出 create/clear/hit/overflow/resetStale/outstanding；
- callback ABI 6 使用 owner + expected-table CAS、RAII active pin 与 quiescence drain，失败时保留
  owner/table 的进程期 quarantine，禁止释放仍可能被回调引用的 manager；
- device reset 进入 `RebindPending`，只有 manager `SetDevice(candidateEpoch)` 成功后才发布新 epoch；
- index ticket 已闭合 `Expected -> Lock -> mapped bytes hash/range -> Unlock -> SetIndices -> DIP`，
  任一 identity、generation、ring、hash、range、顺序或 draw signature 不符均 suppress/fuse。

**当前状态（2026-07-14，等待最新 DLL runtime）**：P4 安全合同与本轮算法优化已完成
独立静态终审，P0/P1=0。eligible upload 的跨进程读取由约 52 次压到 4 次并保留 exact
fallback；upload-page pool、per-frame retirement poll 与 lifetime 计数已接线。alpha consumer
现在显式传播 `uvBinding=0/1/2` 到 CSM/point/outline；P4 irreversible alpha draw 仍只允许
原生 GPU output 自带 UV 的 format 2/4，否则在 kernel skip 前 fail closed，未放宽白名单。

06:13 的 native snapshot2 优化继续保持 callback ABI 9：bridge 将
`CGeosetData+0x0C..0xF3` 合为一次 0xE8-byte fault-safe snapshot，失败时逐字段 exact
fallback；同一 snapshot 供 upload preflight 与 kernel-time expected-index proof。manager
仍独立读取 live proof，但把四个相邻 geoset 字段与 primitive record 各合为一次读取。
INDEX16 FNV hash 与 min/max 由双遍合成单遍，render-thread TLS 复用高水位 byte vector。
成功 bypass attempt 的 metadata `ReadProcessMemory` 理论上 bridge `11->2`、manager
`6->2`，合计少约 13 次；index/palette/static contents exact compare、poison、normal-return
ack、ticket、ledger 与 skip 白名单均未改变。outside-dispatch fastpath 与两阶段授权因
证据/ABI 不足明确未做。

07:49 的 compute 热路径继续做了两项不改变接管语义的算法收窄：

1. `war3_gpu_skin.comp` 在每个 `(64,1,1)` workgroup 内由 9 个 lane 协作缓存
   job words `4..9`、`13..15`，所有 lane 在任何 early return 前经过同一 barrier；
   `GpuSkinJob` 64-byte ABI、二维 dispatch、`precise` 3x4 公式和六种 FVF 写出不变。
2. manager 在任何 output lease 分配前按
   `floor(log2(ceil(vertexCount/64)))` 做 9 桶稳定 counting partition；同桶保留原生
   candidate 顺序，再沿用既有 1 MiB upload/page 边界切批。native 上限 16384 顶点
   对应 256 个 X group；零或超限输入返回 sentinel，并由 host/canRecord 双重拒绝。

`GpuSkinComputeBatch` 现在携带并由 mapped immutable jobs 重新计算
`actualVertexCount / roundedInvocationCount / launchedInvocationCount`；诊断输出 tail padding、
cross-job padding、桶分布及 prepared/submitted exact 对账。静态正确性复核 P0/P1=0。
性能仍是待测 P2：小批跨桶会增加 dispatch/bind，并减少跨桶 palette dedup；shared barrier
在完全 padding workgroup 上也可能抵消 uniform SSBO cache 的理论收益。后续必须同时看
GPU-skin p95、cross-job waste、dispatch 数和 palette bytes，再决定是否自适应合并相邻桶。

最新 build-only artifact：
`AutoTest/artifacts/gpu_skin_p4_build_only_isolated_gpu_skin_bucket_shared_jobcache_monotonic_frame__20260714_074947`。
完整 71/71 编译/链接通过，重新生成 `war3_gpu_skin.h` 与
`war3_volumetric_light.h`；当前目标 error=0、warning=0。DLL SHA256
`833B5372149CAFE0D9E93612EB510F8D5D9AC6DFBEAB4E91C4FE84C56E670F3E`；
`launchPerformed=false`、`deployPerformed=false`。因此仍不能宣称 P4 runtime 通过。下一门是
隔离桌面短时 crash gate，再依次做 bypass ledger、`--outline-all`、resize/reset/map reset、
第二进程与格式/透明 fallback；P4 仍为实验开关且默认关闭。

### 11.1 P4 fingerprint `0x12` 精确闭环（2026-07-11）

隔离桌面日志是 `failureMask=0x12`，即 `PeImage(0x2) | Opcodes(0x10)`。目标文件仍为
version `1.27.0.52240`、MD5 `267861a0dfd416dbad13e7ee3ec7794a`、SHA1
`88ab432160fb84c23b096c3fc022bfbcb3cb1a1a`，不是文件版本漂移。

#### PE 子项

原始文件头给出的事实是：DOS `e_lfanew=0x138`；raw `0x138` 为 `50 45 00 00`；
`Machine=0x014C`；`OptionalHeader.Magic=0x010B`；header ImageBase `0x6F000000`；
`SizeOfImage=0x00CD8000`；`CheckSum=0x00C96841`。关键时间戳有两个不同字段：

- COFF `FileHeader.TimeDateStamp` 位于 raw `0x140`，字节 `1C 0E BD 56`，真实值是
  **`0x56BD0E1C`**；fingerprint 必须比较这个字段。
- Export Directory 位于 RVA `0x00B549C0` / raw `0x00B53DC0`，其 `TimeDateStamp` 位于
  raw `0x00B53DC4`，字节 `1B 0E BD 56`。`objdump -x` 末尾显示的
  `Time/Date stamp 56bd0e1b` 属于导出目录，不能当成 COFF header。

因此旧修订恰好把两个字段混淆，将正确的 `0x56BD0E1C` 改成了 `0x56BD0E1B`，直接触发
`PeImage`。`IsReadableRange` 不是本次根因：同一次安装在进入 GPU-skin fingerprint 前，
`GetModuleInfo` 已用同一函数成功读取 DOS/NT header；其实现还会遍历跨 region 范围，
并非 `IsReadableRangeFast` 的单-region 语义。新校验先复制可读 DOS header，要求精确
`e_lfanew==0x138` 后才计算 NT 地址，再复制 NT32 header，避免使用未约束的 `e_lfanew`。

隔离桌面 v2 进一步给出：`loadedBase=0x62290000`、mapped header
`OptionalHeader.ImageBase=0x62290000`，其余 PE/version/hash/opcode/vtable 全部通过，因而旧
校验只缺 `peValidMask 0x80`，得到 `peValidMask=0x77F / peFailureMask=0x80`。IDA MCP 对同一
hash 文件的 raw PE 与完整 relocation directory 复核结果是：

- raw `OptionalHeader.ImageBase` 位于 raw/RVA `0x16C`，字节 `00 00 00 6F`，仍是首选
  `0x6F000000`；`DllCharacteristics=0x0140`，包含 `DYNAMIC_BASE`；
- `.reloc` RVA/size 为 `0x00BFD000/0x000DA478`，共 2911 个 block；首个 page RVA
  `0x1000`、最小 fixup RVA `0x1002`，不存在 RVA `0x16C` 的 fixup；
- 相比之下，flush operand `0x1380A4` 与 upload operands
  `0x0EEA56/0x0EEA5B/0x0EEA6D` 均有 type 3 HIGHLOW。由此可排除“`.reloc` 把 header
  ImageBase 改成实际基址”；该变化是 loader/mapper 在映射头上的基址归一化。

安全接受条件因此严格限定为二选一：mapped header ImageBase 必须等于 raw/hash 绑定的
`kExpectedImageBase`，或必须精确等于本次已验证模块的 `gameBase`。任意第三值仍失败；后者
也只有在 version、MD5、SHA1、其余 PE、全部 opcode/rel32 与 vtable 同时通过时才可能令
顶层 fingerprint exact，不会单独放行未知映像。

PE 子位 `0x001..0x400` 依次表示 DOS readable、MZ、`e_lfanew`、NT readable、PE signature、
i386 Machine、PE32 Magic、header ImageBase、COFF timestamp、SizeOfImage、CheckSum；
required mask 仍为 `0x000007FF`。新增 provenance-only valid 位 `0x800=preferred`、
`0x1000=runtime-relocated`，两者互斥；v2 路径预期为 `peValidMask=0x17FF`、
`peFailureMask=0`，Logger 另输出 `imageBaseKind`。运行时仍同时输出全部观测字段。

#### Opcode 子项

IDA MCP `get_bytes`、真实 ASM 与磁盘 RVA->raw 映射逐项一致；除 flush 的运行时重定位比较外，
原常量/offset 均正确：

| failure bit | 检查点 | VA / RVA / raw | 真实字节或目标 | 结论 |
|---:|---|---|---|---|
| `0x001` | applyPrefix | `6F138EE0 / 138EE0 / 1382E0` | `55 8B EC 53 56 57 8B FA 8B F1 8B 0F E8 DF A9 FA FF` | match |
| `0x002` | applyTail | `6F138F5A / +7A / 13835A` | `5F 5E 5B 5D C2 04 00` | match |
| `0x004` | uploadPrefix | `6F0EEA50 / 0EEA50 / 0EDE50` | `55 8B EC 6A FE` | match |
| `0x008` | uploadEH4 | `6F0EEA5F / +0F / 0EDE5F` | `64 A1 00 00 00 00 50 83 EC 0C 53 56 57` | match |
| `0x010` | uploadTail | `6F0EEC10 / +1C0 / 0EE010` | `59 5F 5E 5B 8B E5 5D C2 34 00` | match |
| `0x020` | kernelPrefix | `6F0EDDC0 / 0EDDC0 / 0ED1C0` | `55 8B EC 83 EC 2C 8B 91 90 00 00 00 89 4D FC 53 8B 5D 08 F6 C2 06 74 76` | match |
| `0x040` | kernelTail | `6F0EE1E8 / +428 / 0ED5E8` | `5F 5E 5B 8B E5 5D C2 04 00` | match |
| `0x080` | Lock rel32 | `6F0EEB76 / 0EEB76 / 0EDF76` | `E8 55 FA FF FF -> 6F0EE5D0` | match |
| `0x100` | kernel rel32 | `6F0EEB85 / 0EEB85 / 0EDF85` | `E8 36 F2 FF FF -> 6F0EDDC0` | match |
| `0x200` | flushPrefix | `6F1380A0 / 1380A0 / 1374A0` | raw `55 8B EC A1 AC 6B BC 6F` | old runtime mismatch |
| `0x400` | dispatchPrefix | `6F13A5E0 / 13A5E0 / 1399E0` | `55 8B EC 83 EC 3C 53 56 8B 72 0C 8B D9 57 56 89 75 F8` | match |

`Game.dll` 有 `DYNAMIC_BASE`，该次日志中的实际 `gameBase=0x62290000`。IDA ASM
`0x6F1380A3` 是 `mov eax, g_RenderQueue_NumOfElements`，其 `A1` operand 从 RVA
`0x1380A4` 开始；`.reloc` 明确存在 `[1380a4] HIGHLOW`。因此运行时 operand 必须从 raw
`0x6FBC6BAC` 重定位为 `gameBase+0xBC6BAC=0x62E56BAC`，完整入口字节应为
`55 8B EC A1 AC 6B E5 62`。旧 `flushPrefix` 却静态比较到 `... AC 6B BC`，在 RVA
`0x1380A6` 必然失败。修复没有使用通配：现在按实际 `gameBase` 构造并比较完整 8-byte
`push/mov` 入口和精确 `moffs32`。

`ValidateOpcodeFingerprint` 现对上述 11 项全部执行并返回 `opcodeFailureMask`，不再由 `&&`
短路成单一 bool。`Logger::info` 分三行输出总 mask、PE 全观测值、version/fileSize/MD5/SHA1；
version、hash、PE、opcode、rel32、vtable 仍任一失败即整组三钩 fail closed，P4 授权、
kernel-only skip、post-skip suppress/fuse 和 rollback 护栏均未放宽。

验收硬计数：`originalUploadCalls==uploads`、
`originalKernelCalls+bypassedKernelCalls+irreversibleKernelSuppressions==kernelHookCalls`、
`duplicateKernelCalls==irreversibleKernelSuppressions==0`、nested 必须对应 fail-closed CPU
fallback 且 overflow 验收为 0、
`nullMappedKernelFallbacks` 只增加 original kernel、
`postSkipNativeFallback==0`；DIP/shadow/outline suppress、fused key、reservation leak、
unreserved consumer 和 restore failure 在验收地图中必须全 0。支持对象的 CPU kernel
调用/字节必须下降，而 outer Lock/Unlock/ring/FVF/stream 调用保持。

### 11.2 Observe v3：upload -> DIP 时序与配对口径修复（2026-07-11）

隔离桌面 v3 的真实计数为：dispatch begin/end `195206/195206`，uploads `679102`，
全部 D3D DIP `839891`，旧 correlated/unmatched `217162/622729`，upload fan-out
`0=507251 / 1=126540 / many=45311 / max=2`，旧 manager `pairErr=1637231`，
`eligible=0`，epoch leak/pending 均为 0。两条精确等式先证明 upload ledger 本身没有泄漏：

```text
507251 + 126540 + 45311 = 679102 uploads
126540 + 2 * 45311      = 217162 correlated DIPs
217162 + 622729         = 839891 raw D3D DIPs
```

IDA MCP 真实 ASM/bytes 给出的时序不是“upload 后下一次任意 DIP”：

- `CGxDeviceD3d_UploadDynamicVertices` `0x6F0EEA50..0x6F0EEC17` 只做 vertex
  globals/ring/Lock/kernel/Unlock/FVF/stream；函数内没有 DIP，并在独立 index upload 前返回；
- `CGxDeviceD3d_UploadBindDynamicIndices` `0x6F0EEC20..0x6F0EECF4` 只上传 index、
  `SetIndices` 并写 `this+0x71C` base vertex；函数内也没有 DIP；
- actual DIP 只在 `CGxDeviceD3d_FlushIndexedPrimitiveBatch+0x53 = 0x6F0EEA43`，即
  D3D device vtable `+0x148`；
- immediate helper `GxDevice_DrawIndexedRange` 先在 `0x6F0E3526` 调 index upload，再在
  `0x6F0E352B` 调 primitive-batch flush；common dispatch 又在 `0x6F13A63D` begin batch、
  `0x6F13A6BE` 无条件 tail flush、`0x6F13A6C3` end batch。因而同一 active upload 可在
  下一次 upload/dispatch end 前得到 0、1，或 immediate + tail 两次 DIP；v3 的 `max=2`
  正好符合该控制流。

旧实现的问题是每个 D3D DIP 都先增加 dispatch `dipCount`，且 dispatch 外或 dispatch 内
尚无 active upload 时仍调用 manager。普通 UI/terrain/其他 D3D draw 因而同时污染
manager `nativeDips`、dispatch summary 和 `pairErr`。修订后的合同是：

- `dips` 仍是全部 raw D3D DIP；新增 `outsideDispatchDips`、
  `dispatchNoUploadDips`、`correlatedDips`；前两类普通 draw 只进入 raw/unmatched，不调用
  manager、不增加 dispatch summary；若 nested/overflow barrier 隐藏了 committed-skip 父
  upload，则例外发送 uncorrelated callback，保留 irreversible source 以 suppress/fuse；
- 只有存在 active upload 且 render-thread/flush/dispatch epoch 精确匹配，才增加
  dispatch `dipCount`、active fan-out ordinal、`correlatedDips` 并发送 correlated callback；
- active epoch/ordinal/count 真的失配才增加 manager `truePairingErrors`。旁路 source 已存在时，
  即使 epoch 错误也必须发送 manager callback 并进入 device resolver，继续执行不可逆 P4 的
  suppress/fuse；
- 每个 dispatch 的 `summary.dipCount` 现在等于该 dispatch 发给 manager 的 correlated
  callback 数；全局稳态预期 `manager.nativeDips == native.correlatedDips`。另有
  `raw == correlated + unmatched`，以及
  `unmatched == outsideDispatch + dispatchNoUpload + activeEpochMismatch`。

`eligible=0` 没有通过放宽白名单处理。native bridge 对
`kNativeUploadInputRequiredPreflight` 的 18 个低位逐位累计缺失：exact fingerprint、common、
stage11、world tag、single caller、skin1、format、vertex count、position source/stride12、
normal source/stride12、group source/stride1、palette present/count/readable、UV layout。
manager 另按首个 strict rejection 累计 `path/stage/skin/input/output/identity/cpu/bypass`；
device 每 300 帧把两组计数写入 `war3_d3d9.log`。下一轮先读非零最大桶，再决定定位点，
required mask、Stage 11/common/skin1/format 白名单及 P4 suppress/fuse 均保持不变。

### 11.3 Observe v5/v6：配对子门与 sidecar 闭合（2026-07-11）

以下为 Test Conductor 在**隔离桌面**得到的观察日志；它们证明观测口径和白名单前分类的闭合，
不证明 GPU skin 输出、画面正确性或性能。

**已证明（Observe v5 配对子门）**：dispatch `begin == end`；`truePairErr`、epoch leak 与
pending 均为 0；dispatch 外 upload 满足 `outside upload == 0-fanout`。因此 0-fanout 不是泄漏的
代名词，不能再被 one-shot pending token 模型误记为配对错误。

**已证明（ASM sidecar + unified tag/stage upsert，v6 最终日志）**：

```text
raw uploads                    = 1,449,543
outside                         = 1,078,139
inside                          =   371,404
eligible                        =   123,790
reject path / stage / skin      = 98,217 / 31,146 / 118,251
```

上述 raw/inside/outside 与 eligible/reject 分类按 v6 定义严格闭合。`Stage/world missing` 已被证明是
`outside + stageReject` 的分类结果，而不是白名单错误；不得通过放宽 Stage/world 条件来“修复”它。

**该阶段已验证**：P1B Dual v3 为 `1824/1824` parity；P2 Shadow exact `34990`；P3 Main
GPU submission `26035` 且 restore clean。当时仍待 P3 outline 专项与 P4 crash/ledger/lifecycle/
格式覆盖；其中 outline 与 lifecycle 后续结果见 28.7。所有当前 GPU-skin 运行测试都在隔离桌面执行，性能结论暂缓；最终
`dual_perf` 必须以前台 FG 运行，隔离桌面 FPS 不得用于 PASS/FAIL。

### P5：可选的批处理深化与动画系统进一步接管

只有 P1A~P4 稳定后，才评估：

- shared static arena、跨材质 job 去重与更少 dispatch；
- GPU 构造 bone/world palette；
- attachment、particle、ribbon 与 lights 的 GPU/CPU 一致性；
- GPU-driven culling、indirect draw、mesh batching；
- motion vector、per-bone effects、现代 G-buffer。

这一步的难度远高于“只接管最后的顶点 skin”，不应与第一版混做。

## 12. 风险与硬护栏

1. **特殊 draw 不得泛化**：粒子、ribbon、软件生成几何、UI、地形和未知 skin mode 默认走原版。
2. **BaseVertexIndex 必须成对处理**：stream override 与 draw vertexOffset 不能只改一个。
3. **状态污染必须可证明不存在**：one-shot override 后要恢复/置 dirty，不能影响下一 draw。
4. **palette 生命周期**：只读 live pointer 时必须在 War3 内存有效窗口内完成 CPU copy；GPU 不得直接延迟读取宿主裸指针。
5. **资源 generation**：地图卸载、模型释放和 device reset 后旧缓存必须失效。
6. **数值 parity 优先**：第一版不 normalize normal、不改权重算法、不“优化”矩阵布局。
7. **现有视觉红线保持**：S1 period 固定 1；不得回退静态阴影、path blocker 身份门、uberSplat 和动态 pose。
8. **永远保留逐 draw fallback**：只在 kernel skip 前可回退；skip 后 native slice 已 stale，
   必须 suppress + fuse，不能晚回退。
9. **NULL mapped pointer 不优化**：Lock 失败必须进入原 kernel，让原 caller SEH 保持语义。

## 13. 尚待锁定的问题

1. 预编译 compute SPIR-V 转 `DxvkSpirvShader` 时的 binding slots 与 push-data 契约；
2. output arena 最合适的 buffer usage、对齐和 fence 回收实现；
3. one-shot stream override 在主 draw、几何 outline、多次 PrepareDraw 之间的精确恢复时机；
4. 三张固定地图中的真实 upload -> DIP fan-out 分布，以及 split/instancing/debug skip 的 flag 口径；
5. flush palette 与 upload-time live palette 在所有白名单路径中能否稳定逐字节一致；
6. 哪些非普通模型走不同的 vertex producer，需要长期留在 CPU fallback；
7. P3/P4 build 与三图前台 runtime 证明，尤其是 successful skip 后 native VB 已
   Lock/Unlock 但未写，对后续 CPU fallback、ring wrap、reset 的影响；
8. 性能收益上限：当前项目主线程瓶颈并不只在 CPU skin，必须用调用/字节计数和前台 perf 证明收益后再扩大覆盖面。

`0x6F0E5BC0` 已由 6 条真实 ASM 闭环：唯一数据副作用是 `this+0x5C += vertexCount`，不再列为 Unknown。

## 14. 一句话交接

> War3 1.27a 的普通模型 skin 已被还原为“CPU 生成 group palette + 每顶点 uint8 group slot + 一次 3x4 矩阵变换”。最稳的接管方案是在 `Hook_FlushSortedItems` 入口批量 compute，在完整保留 `0x6F0EEA50` 的前提下只 gate `0x6F0EDDC0` 写循环，并于 `D3D9DeviceEx::DrawIndexedPrimitive` 的 `PrepareDraw` 后 one-shot 覆盖 stream 0。skip 前可回 CPU；skip 后只能 suppress + fuse。绝不能逐 draw dispatch compute。

## 15. 历史 GPU/true-skinning 实验为什么失败

### 15.1 它并不是今天定义的“主渲染 GPU skin takeover”

Phase 7.8~7.38 的所谓 GPU/true skinning，主要是在 CSM replay 中用：

```text
静态 geoset positions
+ current-draw/registry 中重建的 palette
+ 猜测或捕获的 group/explicit-blend stream
-> shadow caster vertex shader
```

它没有替换 War3 正常画面的 `0x6F0EDDC0 -> dynamic VB -> DrawIndexedPrimitive`，也没有共享一份权威 post-skin output 给所有 pass。因此当时大量故障其实属于“shadow semantic reconstruction”，不能直接证明 DXVK compute 接管原 CPU kernel 不可行。

### 15.2 误把不存在的 explicit weights 当作缺失真相

旧研究一度认为：

- 一个 group slot 只代表“硬刚体近似”；
- 真蒙皮必须再找到 D3D9 `BLENDWEIGHT/BLENDINDICES`；
- generic resolver 因而从 auxiliary stream 猜 4-byte tuple，并读取两个 float weight。

现在 IDA 已证明这条前提错误：War3 先把一个 MDL matrix group 内的 N 条 bone matrix 做均值，生成一条 group matrix；最终 CPU kernel 每顶点只读一个 `uint8_t groupSlot`。所以历史 explicit resolver 的 0 hit 不是“GPU 蒙皮做不到”，而是它在寻找最终 draw 根本不会消费的数据。

旧文档 `SKINNING_PIPELINE_REVERSED_2026_04_19.md` 对“均权 group palette”判断正确，但在渲染提交处误判为 D3D9 `INDEXEDVERTEXBLEND`。真实链路是 CPU kernel 先输出 post-skin FVF，D3D9 vertex blending 保持关闭。

### 15.3 stream 合同曾被读错

历史 current-draw 路径曾把 `CGeosetData+0x58` 当成 `+0x4C` group-slot stream 的 stride，造成错误步进、撕裂和抽搐。后续才确认：

- `+0x48` 是 group-slot stream enable；
- `+0x4C` 是每顶点 byte slot；
- stride 恒为 1；
- `+0x58` 是另一条 stride 12 的 normal 输入。

指针值还曾进入 geometry identity hash，使同一逻辑几何因 ring/地址变化被误判成新对象。

### 15.4 palette 来源不权威或不完整

早期 `0x6F12E600` hook 一度只捕获输出首矩阵，而函数真实语义是连续写 `groupCount * 48` bytes。缺失部分随后通过共享 global arena、raw pointer 或 partial result 补齐，导致：

- 读到前一个对象残留；
- trusted palette 变短；
- 短结果尾部被零矩阵填充；
- group slot 越界后退化成 root matrix 广播。

当前 hook 已按 `CGeosetData+0xF0` 捕获完整 group palette，但仍有每帧 20000 次的数据层上限。新 GPU 主线直接在 flush/draw 权威窗口读取 live group range，不把通用 shadow cache 当唯一来源。

### 15.5 matrix 空间曾重复组合

current-draw group palette 已经是原版 CPU kernel 将要使用的最终空间矩阵。历史路径曾再乘一次 `sceneNodeWorldMatrix`，产生绕单位旋转、偏移到世界中心或大块漂移。后来把该 palette 标成 `CurrentDrawPaletteWorld` 并令额外 world transform 为 identity，才消除这类双变换。

新 compute 路径直接消费 native 12-float 矩阵，使用与 `0x6F0EDDC0` 相同公式，不经过通用 `Matrix4` 推断与二次 world 组合。

### 15.6 record/lease 引入跨帧旧 pose

历史 shadow contract 的 publish 与 `UpdateItemWorldMatrix` 绑定；War3 可能跳过该状态更新，但动画 producer 仍每帧更新 palette。于是：

- record 首次 publish 时快照 palette；
- 后续 1~14 帧复用旧 record；
- 达到阈值后才 live rebuild；
- receiver hold 又把闪烁变成“阴影冻结”。

这解释了“模型先动，阴影晚一拍/卡几帧”的主要来源。新方案在同一 flush 中复制当前 live palette，并在同一 command stream 生成/消费 output，不把 pose 绑在跨帧 lease 上。

### 15.7 visible slice、身份和材质问题伪装成 skinning 失败

过去同时存在：

- prepared primitive slice `0/32`，全部依赖 layer-index fallback；
- record cap 导致部件集合 24~32 间变化；
- portrait/preview 共享选中单位 identity，漏进 world caster；
- alpha/cutout 缺少权威 UV/texture contract，投成方形卡片；
- invalid current draw 覆盖同一 part 的 ready snapshot；
- whole-map identity hold 让整张 shadow map冻结。

这些会表现为“只有半只单位”“像刚体块”“世界中心冒出模型”“全体阴影闪”，但并非 3x4 skin 数学错误。以 main draw 的真实 index/FVF/material 为主、只替换 stream 0，可以绕开大部分 semantic replay 重建面。

### 15.8 热点 hook 与资源生命周期不合格

`0x6F138F70` prepared-slice probe 曾把约 19.4 FPS 压到 3.44 FPS；兴趣门控后仍约 12.69 FPS。它证明了“热函数装重 detour + 每 draw 解析/复制”不能成为生产架构。

同时，War3 的原 dynamic VB 是 ring，后续 draw 会覆盖；保存裸 pointer/slice 跨帧必然出错。现在的 draw-time VB fallback 会立即 GPU copy 到自有 device-local arena，这个经验必须保留：GPU skin output 也要由 `Rc<>` 和 fence/frame arena 管理，而不是借用 War3 ring 生命周期。

### 15.9 当前应保留与淘汰的部分

保留：

- `uint8 groupSlot + group palette` 的 caster shader 模式；
- 完整 `0x12E600` palette capture 作为诊断；
- draw-time post-skin VB capture 作为 parity/fallback oracle；
- static geoset resource cache；
- path blocker、material 和 world-context 安全门。

不应作为新主线合同：

- generic explicit-weight tuple 猜测；
- raw global arena 无 owner/frame 校验读取；
- direct/sparse/uniform pose 广播救援链；
- 跨帧 current-draw lease 作为当前 pose；
- 每 draw compute 或重型 prepared-slice hook。

## 16. P3 前 RenderQueue sidecar 冲突/递归契约（2026-07-11）

sidecar producer 补丁严格限于 `war3_render_queue_tracker.h/.cpp`。本轮 consumer 补丁只修改
render dispatch hook 与 GPU-skin native bridge，没有改动 device、GPU-skin manager 或 tracker。
ASM 已证明 20-byte queue record 的 `+0x00` 是随后传给 Common `EDX` 的 `renderablePart`，
`+0x08` 是 Common 首个栈参 `layerIndex`；因此 tracker 继续以 `renderablePart` 为 key，并在
同一次 range upsert 中直接采集该 record 的 layer。

同一 tracker semantic epoch 内采用以下 first-known 合同：

- `Unknown` tag 或负 stage 只补空字段；已知同值更新幂等；
- 首个已知 tag/stage/layer 保留，不再被后写的不同已知值覆盖；
- 不同已知值分别设置 sticky `tag/stage/layer` conflict bit；每个 entry/epoch/field 只在首次
  置位时计数；`conflictingEntries` 在该 entry/epoch 首次出现任一 conflict 时计数；
- `Reset()` 推进现有 packed epoch，普通相邻 epoch 不合并 conflict；若 producer 递归在 reset 前
  复用同一 `renderablePart` 且语义不同，它会在当前 epoch 内冲突。若递归 reset 已推进 epoch，
  旧查询失效。

新增只读 ABI 为 `GetSemanticState(renderablePart, RenderQueueSemanticState&)` 和
`GetSemanticConflictStats()`。snapshot 暴露 canonical tag/stage/layer、conflict mask 与 epoch；
旧 `GetTagStage` 为兼容接口，仍返回 first-known tag/stage，不能单独作为 P3 白名单依据。后续
GPU-skin eligibility 必须在 kernel skip 前拒绝 query miss、任一 conflict、未知必需字段、snapshot
layer 与 live dispatch layer 不同，以及持有 snapshot 后发生的 epoch 变化。

native Common/Special hook 与 reimpl Common/Special wrapper 现在都通过
`GetSemanticState(renderablePart, ...)` 构造 GPU-skin `NativeDispatchScope`：

- query miss、任一 conflict、未知 tag/stage/layer，或已知 snapshot layer 与 live dispatch layer
  不同，都会设置显式 `forceFailClosed`；scope 中的 tag/stage 只来自 snapshot，不再回退
  `War3RenderState`；
- 非 GPU 渲染继续使用原有局部 `tag/elementStage` 与兼容 `GetTagStage` 路径，sidecar consumer
  不覆写这些变量；
- `BeginNativeDispatchScope`/`NativeDispatchScope` 的新参数默认 `false`，进入 frame 后与 nested
  条件 OR；dispatch overflow 仍由既有 TLS overflow barrier fail closed；
- failure bitmask 通过 `ReportNativeDispatchSemanticFailures` 累计，`NativeBridgeCounters` 导出
  `queryMiss/conflict/unknown/layerMismatch/scopeForced`。同一 dispatch 可命中多个 reason，
  `scopeForced` 每个显式强制请求只计一次。

`GetSemanticState` 自身的双读与 epoch 校验保证查询返回时 snapshot 属于当前 epoch，当前同步
wrapper 随即构造 scope；本轮没有新增跨 scope epoch pin。长期 16-bit wrap 与未来若引入跨线程或
跨 scope snapshot 持有，仍必须按下述残余风险单独处理。

tracker 现有 epoch 仍为 16-bit packed serial；本补丁没有扩大该 ABI。极长运行后 wrap 与陈旧
entry 指针重合仍是残余风险，P3 长时验收前应由 owner 决定增加 full-generation 还是 wrap 清表，
不能把本轮 conflict gate 当作该问题已解决。

静态 ASM 只能证明 key/layer 的 producer-consumer 同一性，不能证明固定地图中是否真的发生
跨 group/stage/layer 复用；真实频率仍须由 Test Conductor 读取上述累计计数。

IDA MCP 已在 MD5 `267861a0dfd416dbad13e7ee3ec7794a` 的数据库中写入并回读：

- `0x6F1376EE`：record `+0x08` layer 与同 epoch layer-conflict 合同；
- `0x6F1376F4`：record `+0x00` sidecar key、first-known 与 fail-closed 合同；
- `0x6F1381A7`：Common 从 record `+0x08` 压入 live layer，P3 必须与 snapshot 比较。

数据库已显式保存到 `E:\Work\War3\Game.dll.i64`。

该 sidecar 补丁轮当时未暴露 IDA MCP；后续 MCP 恢复后的增量写回与数据库保存结果已记录在
`native_asm_contract.md` 第 13.8 节。该历史说明不得再解释为当前仍有 IDA backlog。

## 17. P4 RVA-only 真实 ASM 复核（2026-07-11）

本节只使用目标 `Game.dll` 的真实 x86 反汇编，文件 MD5 为
`267861a0dfd416dbad13e7ee3ec7794a`，SHA1 为
`88ab432160fb84c23b096c3fc022bfbcb3cb1a1a`。为避免 ASLR 混淆，以下静态位置全部仅写 RVA。

### 17.1 kernel 调用闭环

- `RVA 0x0EDDC0` 在全 `.text` 中只有一条 direct call：`RVA 0x0EEB85`。
- caller 在 `RVA 0x0EEB76` 先调 vertex-ring Lock，再于 `0x0EEB82`
  `push eax` 传入 mapped pointer，于 `0x0EEB83` 恢复 `ECX=device`，然后调 kernel。
- kernel 在 `RVA 0x0EE1EE` 以 `retn 4` 返回，caller 不读其 `EAX`。因此 ABI
  仅是 `thiscall(device, mappedDst) -> void`，不是伪代码曾推测的多参数/整数返回。
- `RVA 0x0EEB7B..0x0EEB8A` 是覆盖 kernel call 的 caller SEH window；Lock 返回
  NULL 时原版仍调 kernel，由该 SEH 吸收写异常后继续 native 后续。

### 17.2 vertex/index/DIP 不是一个成功 token

真实 ASM 确认 native 依次维护可变状态，而不是返回一个可传递的“成功票据”：

1. vertex Lock 产生 format 分槽的 `base/next`；outer upload 后续调 VB Unlock、
   `SetFVF`和 `SetStreamSource`。
2. index Lock 产生 `StartIndex=this+0x70C`；index upload 写
   `primitiveCode/indexCount=this+0x714/+0x718`，然后 Unlock、`SetIndices`，并于
   `RVA 0x0EECDD` 把当前 vertex base 复制到 `this+0x71C`。
3. actual DIP 在 `RVA 0x0EEA43` 才消费 `StartIndex(+0x70C)`、
   `BaseVertexIndex(+0x71C)`、`NumVertices(RVA 0xBC5EA0)`、
   primitive code 和 index count。

index-copy fault 由 `RVA 0x0EEC7C..0x0EEC93` 的 SEH 吸收，随后仍会走
Unlock/`SetIndices`；`SetIndices` HRESULT 在 `RVA 0x0EECCA` 后没有被分支检查。
`RVA 0x0E3526` 的 index upload 与 `0x0E352B` 的 immediate flush，再加上
`RVA 0x13A6BE` 的 common tail flush，证明一个 vertex upload 可对应 0/1/N 个 DIP。
因此 P4 必须使用 `dispatch scope + upload epoch + DIP ordinal + 完整 draw signature`，
不能把 Lock、Unlock 或 `SetIndices` 的发生当作 DIP 已成功消费。

### 17.3 安全 skip 与不可逆边界

- 整个 `RVA 0x0EEA50` outer upload 不可 bypass；它必须 exactly once 执行原函数。
- 唯一允许的 skip 点是 `RVA 0x0EDDC0` kernel detour，且只能在真实
  `mappedDst != NULL` 、全部预授权与 consumer reservation 已 exact 时提交。
- 逻辑提交边界是 detour 决定不调 original kernel 并返回；caller 从
  `RVA 0x0EEB8A` 继续时，native slice 已必须按 stale 处理。
- 物理不可逆边界是 `RVA 0x0EEBB6` 的 VB Unlock；它之后 mapped pointer
  已失效，不存在合法的晚 CPU rescue。
- skip 后遇到 index/stream/DIP/fan-out/lease/shadow/outline 任一 mismatch，只能
  suppress 对应 consumer 并 fuse stable key，不得重放 outer upload、晚调 kernel 或读 native slice。

逐指令表、Lock 失败语义、DIP 状态元组以及最终 IDA 写回/回读结果见
[native_asm_contract.md](native_asm_contract.md) 第 13 节；第 13.8 节记录 7/7 function comment、
11/11 instruction comment 与数据库保存均成功。

## 18. P4 format 分槽与 poison 安全合同（2026-07-12）

本节是 P4 上机前的控制性安全审查要求。并行代码修复仍在实施中；这里不把静态审查、
已接线实现或 IDA 写回描述成 build/runtime/画面测试通过。P1-P3 的既有验证结论不自动为
P4 kernel bypass 背书。

### 18.1 每个 output format 拥有独立动态 VB

目标 `Game.dll` 的真实 ASM 为：

```asm
RVA 0x0EEB40  mov ecx,[ebx+0228h]       ; outputFormat
RVA 0x0EEB52  lea eax,[ebx+06C0h]       ; dynamic-VB pointer table base
RVA 0x0EEB58  lea eax,[eax+ecx*4]       ; &VB[outputFormat]
RVA 0x0EEB5B  push eax
RVA 0x0EEB64  call RVA 0x0EDD10          ; ensure selected VB + shared IB

RVA 0x0EDD3E  cmp dword ptr [ecx],0     ; check only supplied VB slot
RVA 0x0EE5E3  mov ecx,[ebx+0228h]       ; Lock rereads outputFormat
RVA 0x0EE5E9  mov edx,[ebx+ecx*8+06E0h]; per-format oldNext
```

`RVA 0x0EEB52/0x0EEB58` 没有遍历或合并格式，只把
`this+0x6C0+format*4` 这一槽传给 ensure；`RVA 0x0EDD3E` 也只检查该槽。
`RVA 0x0EE5D0` 又以 `format*8` 维护各自的 `base(+0x6DC)` 与
`next(+0x6E0)`。因此 format 0/2/4 不只是不同 FVF，而是不同 VB 指针和不同 ring
游标。

直接后果：看到 format 0 的 VB 被创建、替换、Lock、CPU 覆写或重新绑定，均不能退休
format 2/4 的 poison。后续重新选择 format 2/4 时，其独立 VB 仍可被绑定；
`RVA 0x0EEA43` 或同一 upload 的后续 fan-out DIP 仍可能消费其中的 stale bytes。
poison 所属身份不能只写 format、COM 槽位或当前 stream 0。

### 18.2 caller-owned SEH：选择 trampoline 不等于正常返回

```asm
RVA 0x0EEB7B  mov [ebp-04h],0           ; caller SEH try level 0
RVA 0x0EEB82  push eax                   ; mappedDst, including NULL
RVA 0x0EEB85  call RVA 0x0EDDC0          ; detoured CPU kernel
RVA 0x0EEB8A  mov [ebp-04h],-2          ; only normal-return path reaches here
RVA 0x0EEB99  ...                        ; caller handler landing
RVA 0x0EEBA6  mov eax,[ebx+0228h]        ; normal/handler join before Unlock
```

SEH frame 属于 `RVA 0x0EEA50` caller，不属于 kernel detour。detour 选择并调用 original
trampoline 后，如果 original 因 NULL/坏 mapped pointer 等原因 fault，异常会越过 detour，
由 caller handler 接住并继续 Unlock/FVF/stream；detour 的 post-call 路径不会正常执行。

所以 `CallOriginal selected`、`originalKernelCalls++` 或“进入 trampoline”都不能证明 CPU
写入完成。只有 original trampoline **正常返回到 detour 后**产生的 exact owner
acknowledgement，才可作为 CPU overwrite 的一个必要条件；还必须同时匹配资源身份、generation、
range、stride、FVF 与 format。handler 路径必须保留 poison。

### 18.3 Lock flags 与 stale DIP 不能充当 retirement

`RVA 0x0EE60B` 的 wrap Lock flags 是 `0x2800`，即
`D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK`；`RVA 0x0EE656` 的 append flags 是
`0x1800`，即 `D3DLOCK_NOOVERWRITE | D3DLOCK_NOSYSLOCK`。`NOSYSLOCK` 只是 Lock
标志的一部分；`DISCARD`、`NOOVERWRITE`、Lock/Unlock 成功或 ring 前进都不是“这些字节已被
CPU 精确覆写”的证明。

`RVA 0x0EEA43` 的 actual DIP 只消费当时绑定的 stream、BaseVertex、StartIndex、FVF 与 draw
参数，不消费 kernel normal-return bit。结合 0/1/N fan-out，任何尚未由下述两种合法事件退休的
range 都必须在每个相关 DIP 入口继续命中 poison 检查。

### 18.4 authoritative poison / reset / lifetime contract

1. **Poison identity**：每个 range 必须至少绑定 exact
   `(DXVK D3D9CommonBuffer* commonResource, identityGeneration)`，再带 byte/vertex interval、
   stride、FVF、format 与必要的 native device/COM 佐证。裸 COM pointer、format 或可复用地址
   均不能单独作为 identity。
2. **Create before commit**：kernel detour 决定 bypass 前必须先成功登记 exact poison；登记失败
   或 ledger overflow 只能调 original。登记后不得因 format 切换、consumer suppress、dispatch
   结束、map/device reset、manager detach、Lock flags 或 SetStreamSource 而遗忘。
3. **唯一两种 clear**：
   - exact CPU overwrite：original trampoline 正常返回，且 overwrite 的
     `commonResource+identityGeneration+interval+layout` 与 poison 精确相交；只减去实际覆盖区间，
     不能整槽清空；
   - `D3D9CommonBuffer` 析构 retirement：析构通知携带并精确匹配同一
     `commonResource+identityGeneration`，只退休该 identity 的 ranges。
4. **明确禁止的 clear 依据**：`CallOriginal` 分支选择、caller SEH 吸收、Lock/Unlock/HRESULT、
   `DISCARD`、另一 format 的 CPU upload/VB replacement、reset 请求本身、owner 注销或新一帧，
   均不得清 poison。
5. **Reset/owner exact pair**：每次 reset 发布单调 `resetGeneration=R`；只有同一 owner 对同一
   `R` 的 acknowledgement 才能推进该 reset。不得以 `ack>=R`、新 owner、旧 manager 或另一次
   reset 的 ack 代替精确配对。reset 可立即禁止新 authorization 并清理不含 poison 的控制状态，
   但 outstanding poison 仍只能由第 3 项退休；两者未同时闭合时 reset 必须 deferred/fail closed。
6. **Process lifetime**：并行实现当前已启用 detour 与 original trampoline storage 的
   process-lifetime 策略。map/device epoch 变化只重绑 owner/manager/resource 状态，不拆 hook，
   也不清仍可被 native DIP 触达的 trampoline 或 poison ledger。

### 18.5 本轮 IDA 写回

数据库仍为 preferred ImageBase `0x6F000000`、MD5
`267861a0dfd416dbad13e7ee3ec7794a`。静态地址统一写 RVA；runtime 地址必须按
`verified Game.dll module base + RVA` 计算，禁止把 `0x6Fxxxxxx` 直接当 ASLR 后地址。

本轮保留 5/5 个既有精确函数名，新增/修订 8 个 function-comment fields（覆盖 5 个函数）和
17 个 instruction comments；全部 disassembly comment 已回读。`RVA 0x0EEB99` 的反汇编注释
已落库，但该 handler landing 无法映射 Hex-Rays ctree comment，不影响 IDA disassembly 证据。
数据库已显式保存到 `E:\Work\War3\Game.dll.i64`。未修改 type 或 binary bytes。

## 19. Bypass 12 FPS 的离线根因与 production 混合路由（2026-07-14）

用户在 `DXVK_WAR3_GPU_SKIN_MODE=bypass` 下录得
`war3_perf_report_2026_07_14_08_29_06.html`：avg FPS `11.545`、frame
`86.620 ms`、process CPU `89.286 ms`，而 measured GPU 只有 `2.188 ms`。同一份
runtime 日志为 raw uploads `117691`、outside `93731`、eligible `9474`、jobs
`4504`、compute dispatch `451`、actual/launched vertices
`1908631/2513600`。真正绕过的 native kernel 只有 `4094`，仍有 `113597` 次 original
kernel；这不是全模型完全接管，也不是 GPU compute 饱和。

Game.dll ASM 再次确认 `CWorld_RenderScene -> FlushSortedItems -> upload -> kernel ->
index -> DIP` 位于同一同步 render lane。DXVK command submission 随后才异步；当前实现
没有 CPU 等待 GPU fence/compute retirement 的路径。War3 创建 device 的 flags 不含
`D3DCREATE_MULTITHREADED`，故本实例的 D3D9 device lock 是 no-op。报告里的 WaitGate
又混合了两条 EvtSched lane，不能拿 `Idle≈86 ms` 推导 GPU 等待。

离线工具 `AutoTest/analyze_gpu_skin_offline_cost.py` 在不启动/附加 War3 的前提下：

1. 严格复现 group-slot 选择 3x4 palette 的 position/normal 公式并与 scalar reference
   exact 对齐；
2. 对 `ReadProcessMemory`、`VirtualQuery`、8-byte binding snapshot 建立本机相对成本模型；
3. 同时给出 process-lifetime/frameSerial 下界与“全部事件都落在 238 帧报告窗口”的上界，
   不把 cumulative counters 错除以短报告帧数；
4. 证明旧 bridge 对 scope 外、skinMode0 和 normal-return proof 的工作存在数量级放大。

因此 production bypass 改为：

- T0 仅凭 exact TLS scope 先拒绝，普通 scope-less reject 不读取 GX/geoset；只有 live poison
  存在时才预测当前 CPU upload 是否可能覆盖 poison；
- T1 只读取一次 GX snapshot 分类 skinMode/output format；
- T2 仅真正候选读取 geoset/input range，并且 CPU normal-return rewrite proof 只对 exact
  poison overlap 执行；
- scope-less outside reject 可跳过 manager upload/fanout callbacks，但必须同时满足
  `dispatchEpoch==0`。Begin 时有 epoch、Complete 时丢 scope 的协议异常不能被 elision 掩盖；
- manager 用 learned `(renderablePart, layer)` 反向索引拒绝未知 RenderQueue element；命中后
  只读取相邻 8-byte live `(paletteSlot, geosetData)`，后续 content hash、native proof、token、
  lease、generation 终门不变；
- raw QPC 直接累计 Begin/Evaluate/Complete/normal/DIP/outer/original-kernel 与 manager
  flush/host/prepare 时间，禁止在每 upload 上安装重型 perf scope。

exact job bucket histogram 为 `344,0,2908,1252,0,0,0,0,0`，分别对应主要的
`1..64`、`193..448`、`449..960` 顶点区间。仅凭 exact job counts 与 total vertices 可求得：
production 最小 GPU 顶点设为 `449` 时，保留 `1252/4504 = 27.80%` GPU jobs，保留顶点
的紧界为 `30.59%..62.97%`。Game.dll format-2 SSE 对小 geoset 只做线性 palette transform
和两次 16-byte non-temporal store；让这些小批继续走 CPU，比为它们支付 proof/resource/
dispatch 固定成本更合理。阈值只作用于 `Bypass` production；Observe/Dual/Shadow/Main 的证据
与既有接管口径不改变。

当前 build-only artifact：

`AutoTest/artifacts/gpu_skin_p4_build_only_isolated_hybrid_threshold_callback_hist_volume_budget_20260714_102749`

`build32_safe.cmd ... -j4` exit 0，DLL SHA256
`09C80A56AF4F4E58F4167FF78DA899AC043EEEA7188654387958387D7A75CB86`。未部署、未启动
War3、未运行 AutoTest。上述结论是根因与静态/build 收口，不是 FPS 已恢复；下一次唯一
隔离 crash gate 必须读取新的 QPC counter delta，最终 FPS 仍只认用户让出前台后的
foreground `dual_perf`。

## 20. 48 FPS 后续归因：正路径静态资源提示（2026-07-14）

用户在前一轮分层门与 449 顶点混合路由后复测，报告
`war3_perf_report_2026_07_14_11_40_58.html` 为 `48.330 FPS`、frame
`20.691 ms`、main thread `17.283 ms`，而 measured GPU 仍只有 `2.102 ms`。
这证明 10–12 FPS 的数量级退化已经显著收回，但 GPU compute 仍不是当前墙；相对未启用
GPU skin 的约 90–100 FPS，P4 仍没有形成净收益。

对同一窗口做 counter delta 后，manager flush 约 `7.346 ms/frame`，其中
`prepareArray` 约 `7.185 ms/frame`。每帧约扫描 2360 个 RenderQueue element，但
无正样本区间的扫描约为 `23.9 ns/element`；剩余成本集中在阈值后的约 50 个候选，主要是
model/resource lookup、live binding 与 palette/static proof。窗口内
`8,496,585` 个 element 分类为 special `725,524`、invalid renderable
`2,744,577`、reverse miss `4,377,466`、reverse hit `649,018`；命中后又有
`474,656` 个小模型回 CPU，仅约 `174,362` 次进入 live binding。最终 job/kernel bypass
占全体 upload 仍不足 1%，所以不能把当前阶段称为全模型 GPU 蒙皮。

本轮把 production Bypass 正路径继续收窄：

- 4096-bit、双 hash 的 `RenderableLayoutBloom` 只做 negative rejection；clear bit 才能
  证明不存在，positive 仍必须通过 exact reverse map 与全部 live proof。epoch/reset 与
  exact map 同时清空，stale bit 最多造成 false positive。运行计数必须满足
  `bloomRejects + bloomMaybes == reverseHits + reverseMisses`。
- learned layout 只缓存一个 `shared_ptr<const GpuSkinStaticResource>` admission hint。
  hint 必须精确匹配 map/device epoch、geoset/content/layout/vertex、static slice 与 index hash；
  它只能省掉首轮 model-cache 与 resource probe，不能授权 destructive bypass。
- preflight 仍重读 current model stamp、palette，并逐字节验证 position/normal/group/UV/index；
  completion 再验证 current stamp 与 palette。live binding ABA、结构性 palette mismatch、
  static proof 失败、layout 改变、fuse 与 epoch turnover 都会清 hint。
- native 失败若最初只知道 fuse-key，preflight 一旦恢复 exact geoset/layer 就升级为 layout
  fuse；下一 flush 在 static lookup、palette copy 和 compute submission 之前拒绝，不能永久
  重复提交无消费者 job。
- global palette base 每个 flush 只做一次 fault-safe snapshot；每个 candidate 仍复制 exact
  palette bytes，preflight/completion 的 pointer/count/content proof 不变。旧地址变化只会
  fail-closed。
- 仅 Bypass production 停止计算不参与授权的 palette/source byte hash，并取消本来
  `hit=0` 的跨 job palette dedup；每 job 使用独立 palette slice。Dual/Shadow/Main 的
  signature、dedup 与 parity 合同完全不变。

静态双审结论 P0/P1 均为 0。离线重放产物
`AutoTest/artifacts/gpu_skin_offline_cost_20260714_122644/result.json` 再次确认：当前报告
`48.330 FPS`、main `17.283 ms`、GPU `2.102 ms`，旧 bridge 到分层 early gate 的本机
操作成本模型剩余比约 `12.70%`；该比值只用于算法方向，不是新的 War3 FPS 预测。

下一次运行必须先在隔离桌面验证 warm-up 后 `bypassStaticHintHits` 占主导、Bloom 闭合、
P4 ledger/poison/index/nativeKernelNormal 全 clean，再看 manager prepare delta 是否实降。
在 crash/outline/lifecycle/格式门和最终 foreground `dual_perf` 通过前，仍不得宣称 P4 完成
或全模型 GPU 蒙皮。

本节代码的最新 build-only 为
`AutoTest/artifacts/gpu_skin_light_build_only_20260714_123117`：
`build32_safe.cmd src/d3d9/d3d9.dll -j4` 56/56、exit 0、error=0，manager、device、
Volumetric C++、两个修改 shader header 与最终 x86 link 均实际重建。DLL SHA256 为
`0620F24CDB7BDCB3D44ADBDDA8431C59571257CC50A475485E05C259D1D71E83`。
未部署、未启动/附加 War3、未运行 AutoTest；该结果只证明编译闭合。

## 21. 全链路低扰动计时与 clean-pair 窗口归因（2026-07-14）

用户再次实测确认 48 FPS 版本没有继续恢复到未启用 GPU skin 的 90–100 FPS，因此本轮
停止凭代码形状猜热点，给同步 render lane 补齐 raw-QPC 取证。设计原则是计时本身不能
重新制造百万级开销：Bloom miss、special、null renderable、reverse miss 和 449 顶点以下
CPU 路由均不做逐 element QPC；RenderQueue 只在每个 array 外计一次 scan，四段深层计时只
从 exact reverse hit、格式/阈值/重复门全部通过后开始。

producer 现在发布以下唯一诊断行，所有字段统一为 `calls/ticks/maxTicks`：

- `nativeTiming`：begin、evaluate、complete、normal-return notify、DIP、outer upload、
  Game.dll CPU kernel；
- `managerHot`：inclusive flush assembly、host submit callback、prepareArray；
- `managerQueueTime`：flush control/static、queue scan、transparent collision、positive
  candidate，以及 binding/static lookup/palette copy/candidate build 四个互斥子阶段；
- `managerBatchTime`：flush request query、host submission finalize、candidate assemble、
  output lease、compute finalize、mapped upload allocation+palette/job memcpy、publication；
- `managerProofTime`：P4 manager/host/finalize preflight、palette/static exact proof、CPU
  normal-return manager/host/finalize、upload completion；
- `managerConsumerTime`：positive DIP/shadow resolve、plan/commit/fail/close、bypass draw
  result、irreversible fuse 与 termination。

锁与开销合同：manager 普通计数只在 `m_mutex` 内更新；host callback 在锁外测 elapsed，
回锁后一次聚合。candidate 的 `RawPhaseTimer` 在 early return 时把拒绝成本归到当前阶段，
切换边界只读一次 QPC；completion timer 覆盖所有真实 manager onUpload，但 production
scope-less outside fast reject 仍完全跳过 manager callback。flush query 在锁外测、在既有
submit/noteFlushOnly 锁内记账；accepted/rejected host finalize 在各自既有锁域计时。

这些计时存在严格的嵌套关系，分析时禁止横向重复求和：

```text
flushAssembly
  ├─ control + staticPrepare + prepareArray + collision
  ├─ assemble
  │    ├─ outputLease
  │    └─ finalizeCompute
  │          └─ batchUpload
  └─ publish

prepareArray ─> queueScan ─> positiveCandidate
                              └─ binding + staticLookup + paletteCopy + build
```

此外 `planConsumer` 位于 preflight host callback 内，`validatePalette` 同时位于 preflight
manager 与 completion manager 内，`validateStatic` 只位于 preflight manager。parser 因而
分别验证 containment，而不是把这些 nested totals 再加到父项上。native outer/evaluate/
notify/complete 也给出相应 parent residual，用于暴露 bridge-only 与锁等待成本。

`AutoTest/run_gpu_skin_p4_isolated.py` 不再只保存 process-lifetime total。唯一 forced
Test Conductor 会选两个 chronological、quiescent 且 frame/flush/jobs 有进度的 clean
snapshot，计算 calls/ticks 差，输出：

- 每阶段窗口 `totalMs`、`averageUs`；
- 第二快照的 lifetime maximum（max 不能做差，明确不伪装成窗口 max）；
- `rankedByInclusiveTotalMs`；
- 累计与窗口两套 parent/child residual、频率一致性和 counter 单调性。

新硬门 `hotPathTimingContractClean` 要求四条新行与旧两条行字段完整、raw shape 合法、
native/manager frequency 一致、累计和窗口所有 closure 成立、Bloom exact closure 成立，
并要求 clean-pair delta 有效。lifecycle 的普通预探测刻意排除此门，因为在 forced pair
产生前它不可能为真；最终收集阶段仍严格执行。

静态复审 P0/P1=0。最新 build-only：
`AutoTest/artifacts/gpu_skin_timing_volume_build_only_20260714_132000`，相关增量 5/5、
exit 0、error=0，parser AST/最小字段闭合自测通过；DLL SHA256
`13927E439386E1837343632914DA4D919DB56145FCD7B95991CBC55C4EB3742B`。未部署、未启动
War3、未运行 AutoTest 或性能测试。下一次隔离 crash gate 应先读取
`cleanPairHotPathDelta.rankedByInclusiveTotalMs`，再按最大 inclusive 段及其 residual 继续
优化；在运行证据出现前仍不得宣称 FPS 已恢复。

## 22. 13:51 CPU 墙闭合：两处 readability 放大与诊断自扰（2026-07-14）

用户报告 `war3_perf_report_2026_07_14_13_51_15.html` 共 `1911` 帧，avg frame
`19.219 ms`、main thread `15.650 ms`、process `18.724 ms`、GPU `2.208 ms`，继续证明
瓶颈位于同步 CPU render lane，而不是 compute shader。新 full-QPC clean-pair 证据把这个
“CPU 管理成本”拆到了具体函数和每帧量级。

### 22.1 三个确定热点及修复

| 版本 / artifact | native outer | manager flush | 合计 | 本轮语义 |
|---|---:|---:|---:|---|
| v1b `native_begin_sample_v1b_20260714_141348` | 8.577 ms/f | 1.454 ms/f | 10.032 ms/f | T0/T1/T2 分层前的可复算起点 |
| v2 `native_small_gate_v2_20260714_142631` | 3.178 ms/f | 1.549 ms/f | 4.727 ms/f | 449 顶点以下在一次 GX state 后回 Game.dll SSE |
| v3 `palette_safecopy_v3_20260714_143419` | 3.257 ms/f | 0.091 ms/f | 3.348 ms/f | manager palette copy 改 bounded SafeCopy |
| v5 `native_palette_safecopy_v5_20260714_152122` | 1.459 ms/f | 0.090 ms/f | 1.550 ms/f | production native T2 palette readability 改 TLS scratch SafeCopy |
| v6 full `diagnostics_light_full_v6_full_20260714_153530` | 1.898 ms/f | 0.118 ms/f | 2.016 ms/f | 全量 QPC/atomic 取证，仅作诊断上界 |

这里的 native outer 包含最终仍走 Game.dll 的 CPU kernel，不能把绝对值全部声称为 takeover
开销；同图、同 clean-pair 合同下的版本 delta 才是优化证据。v1b→v5 的同步段合计下降
`8.482 ms/frame`。

两个放大点都不是 memcpy 带宽本身，而是对合法连续 palette range 反复做 page metadata /
VirtualQuery 风格可读性证明：

- manager lifetime `paletteCopy` 从 `228.591 us/candidate` 降到 `2.223 us/candidate`
  (`-99.03%`)；prepare/scan/positive 的父项随之整体坍缩；
- native T2 lifetime `paletteProof` 从 `281.328 us/candidate` 降到 `2.193 us/candidate`
  (`-99.22%`)，完整 exact T2 从 `292.588` 降到 `12.769 us/candidate`
  (`-95.64%`)；
- production Bypass 使用独立 `thread_local` 0x3000-byte readability scratch，只把
  fault-safe copy 成败当可读性布尔证明，不复用其内容授权 destructive skip。Observe/Dual/
  Shadow/Main 仍保留原 metadata policy，callback ABI 9、palette content preflight/completion
  双重证明不变。

静态调用链与运行计时均未发现 fence/retirement wait。retirement 只查询 completed fence
value，host-visible upload 是普通 mapped pointer；compute 约 `0.534 dispatch/frame`，host
submit 约 `0.017/frame`，descriptor/view 绑定和剩余 VirtualQuery proof 都不足以解释多毫秒墙。

### 22.2 每 300 帧尖峰是同步诊断输出

13:51 报告中所有超过 100 ms 的帧为：

`72:203.154, 372:172.120, 672:201.221, 972:215.446, 1272:184.240,
1572:206.978, 1872:176.769 ms`。

索引差严格为 `300`。去掉这 7 帧后 trimmed avg 为 `18.575 ms` (`53.835 FPS`)；尖峰超出
trimmed baseline 的总量均摊 `0.644 ms/frame`。这只能解释平均值的一小部分，却完整解释了
周期性 170–215 ms 卡顿。

production 现默认：

- `fullDiagnostics=false`；
- `diagnosticPeriodFrames=0`；
- `DXVK_WAR3_GPU_SKIN_DIAGNOSTICS=full|1` 才开启全量 QPC/atomic/preflight histogram；
- `DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES` 必须显式非零才自动 dump；
- P4/Dual AutoTest 显式使用 full + period 0，forced snapshot 不受周期门影响。

light 模式仍无条件保留 authorization、poison、index ticket、reset、retirement、ledger、pairing
和资源 lifetime 安全计数，只移除 report-only 的 full timing 与 18-bit preflight histogram。
1/127 Begin/T2 sampler 保留，便于在 production-light 下继续定位而不恢复全量扰动。

### 22.3 full/light 运行硬门

最新 build-only：

`AutoTest/artifacts/gpu_skin_p4_build_only_isolated_diagnostics_light_full_v6_build_20260714_153241`

exit 0，DLL SHA256：
`26FE1807A0D4346B45141E59D9F44CB5FDF0325D6DA9CEE15E3FF62C0859036F`。

同一 DLL 的 production-light crash gate：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_diagnostics_light_full_v6_light_20260714_154649`

- verdict `PASS`，`failedHardGates=[]`，policy `0/0/0`；
- clean raw `3788`，44 个 full timing stage 的 `calls/ticks` 全部精确为 0；
- lifetime kernel bypass `13099` 次、`247,459,104` bytes；
- protocol/resource/ledger/poison/index/restore/retirement/reset/nativeKernelNormal 与 crash scan
  全部通过。第二次 light 复验也独立 PASS。

full crash gate 在脚本修正后：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_full_v6_full_clean_endpoint_gap_fix_20260714_160456`

- verdict `PASS`，`failedHardGates=[]`，policy `1/1/0`；
- clean raw `6055`，begin/eval/complete/outer 均为 `6055`；
- fast partition `5287 + 0 + 288 + 352 + 0 + 128 = 6055`；
- timing frequency、counter monotonic、所有 parent/child closure、resource delta 全闭合。

旧 conductor 要求两个 clean snapshot 调用必须相邻。在持续 workload 中，合法 poison range
可能恰好跨过采样点；一次 full 复验因此出现 19 个 clean endpoint，却与
`poisonRanges=1/3` 严格交替。修正后的合同允许中间 transient，但绝不把它当 clean：两端仍须
exact PID/requestId、完整 block、同 map/device/reset generation、各自 quiescent；最终重新
抽取两端并验证中间 attempt 全 non-clean、counters 单调有进度、resource retirement 等量和
全部 timing/fast closure。上述 PASS 实际选择 attempts `[2,4]`，gap `[3]` 的
`poisonRanges=3`，两个 endpoint 均为 0。

### 22.4 当前性能结论与剩余边界

把同场景 delta 回代 13:51 报告：

- 使用 v5 实测同步段：`19.219 - (10.032 - 1.550) - 0.644 = 10.093 ms`
  (`99.1 FPS`)；
- 使用诊断扰动更大的 v6 full 上界：`19.219 - (10.032 - 2.016) - 0.644 =
  10.559 ms` (`94.7 FPS`)。

因此当前 production-light 的可复算预测为约 `94.7..99.1 FPS`。这不是新的 foreground
结果：隔离桌面的 frame advance 在两轮 light 间自身波动约 20%，只能证明功能进度与相对
趋势，禁止拿来发布 FPS。最终仍必须等用户明确让出前台后运行 foreground `dual_perf`。

若前台仍慢，剩余盲区只有 dispatch begin/end 的 manager mutex/map 生命周期、DIP bridge
timer 之前的 resource identity/storage diagnostics，以及 semantic scope/pin/counter；预计低于
`1 ms/frame`。下一轮应先给这些边界做 prime-period sampler，再决定是否做 scope-less fast
pass 或固定池；不得再次弱化 static/palette exact proof，也不得把当前 P4 称为全模型完全接管。

## 23. 2.60 ms 增量闭合与 Common dispatch CPU-only seal（2026-07-15）

### 23.1 最新真实覆盖与同步成本

production-light 最新 lifetime 为：

- raw/kernel `1,121,900`；original `1,111,353`；bypass `10,547`，调用覆盖仅
  `0.9401%`；
- original bytes `248,651,648`，bypass bytes `198,956,224`；按 bytes 计 bypass
  为 `44.4488%`；
- scope reject `977,195`、skin/format reject `54,089`、small CPU `66,960`、
  candidates `23,656`；candidate 结果 bypass/fallback `10,547/13,109`；
- physical dispatch `316,234`，Common `242,309`，Special `73,925`；Common no-upload
  end `28,610`，约 `9.36/frame`。

同一隔离场景 ABBA 只用于功能归因，不作为 FPS：off/bypass mean frame
`10.9855/13.5825 ms`，main `8.8365/12.0065 ms`，delta 为
`+2.597/+3.170 ms`，measured GPU 基本不变。1/256 sampled root 按正确 parent nesting
回代为：outer accepted `1.173 ms/frame`、outer fallback `0.973`、DIP device `0.536`、
dispatch begin/end `0.295`、semantic `0.087`、manager flush `0.074`；总增量
`2.600 ms/frame`，与 ABBA frame delta `2.597` 闭合。

这组数据也解释了为什么“把全部小模型一起送 GPU”不是第一刀：native original kernel 的
总上界只有约 `0.217 ms/frame`，而 outside admission/settlement、fallback 与 DIP device
固定成本明显更大。当前接管虽然只占 `0.94%` 调用，却已覆盖 `44.45%` 输出 bytes；剩余调用
主要是大量短小或根本不在可授权 scope 的 CPU 工作。

### 23.2 小任务离线模型

`AutoTest/analyze_gpu_skin_small_job_batching.py` 的产物：

`AutoTest/artifacts/gpu_skin_small_job_batching_offline_20260715_080551/result.json`

当前 449 阈值模型为 `11,782 jobs`、`1,630 dispatches`、actual/launched vertices
`6,940,763/11,176,384`，利用率 `62.10%`。tail waste 只有 `3.64%`，真正的大项是
跨 job 的二维 dispatch waste `34.26%`。历史 4,504-job histogram
`344,0,2908,1252,0,0,0,0,0` 证明：

- 449 阈值只保留 `27.80%` jobs，但保留 `30.59..62.97%` vertices；
- 193 阈值覆盖 `92.36%` jobs、`98.85..99.98%` vertices，是未来 runtime crossover
  为正时的首个安全扩展点；
- local size 64→32 理论只减少 `0.57..5.54%` invocation，却至少增加 `24.2%`
  workgroups；不采用；
- `<64` 顶点 micro-pack 在历史分布中最多只占 `1.13%` vertices，暂缓。

### 23.3 exact-negative seal 的授权边界

Common 可证明 CPU-only 的 path/stage/skin/small 上传合计 `157,855`，约
`51.62/frame`，占 raw `14.07%`、占 Common-managed `86.97%`。本轮新增 dispatch seal，
只移除这批已知不可能消费 GPU output 的 ABI-9 管理事务，不跳原生 CPU 蒙皮：

1. 每个 flush 入口先令旧 candidate view 失效。只有完整 `prepareArray`、collision、
   candidate assembly 完成，并且有 batch 时 host submission 已 accepted，manager 才发布
   `m_dispatchCpuOnlySealFlushEpoch`。空但完整 assembly 的 flush 也可发布 authoritative empty
   view；early return、host reject/throw、callback exception、epoch retirement/reset 都不能发布
   或会清除它。
2. production-light Bypass 的 Common dispatch 改为 eager manager begin。manager 在现有 mutex
   内要求 device ready、无 pending epoch/quarantine/host submission，并保守扫描当前
   `CandidateKey`：任何 exact `(flushEpoch, renderablePart, layer)` token 都拒绝 seal；忽略
   geoset/format 只会增加 false reject，不会产生 false accept。
3. manager sidecar 只能在 begin callback 的 bridge pin 内 propose。bridge 在 dispatch stack、
   manager pairing 容器均已建立且 callback 正常返回后，先置 `BeginIssued` 再 commit。
   `dispatchEpoch % 127 == 0` 的整 dispatch 证据 cohort 永不 seal；full diagnostics、Special、
   Observe/Dual/Shadow/Main 证据合同不变，callback ABI 保持 9。
4. 每个 sealed upload 仍调用完整 Game.dll outer trampoline 与原生 SSE skin/copy kernel。
   独立 TLS marker 证明 exact GX self、thread、dispatch/semantic cookie、reset generation、
   kernel entered 和真实 normal return；caller-owned SEH 仍位于原生调用者，NULL destination
   继续进入原 kernel。
5. sealed DIP 输出非零 dispatch epoch，但 ABI-9 upload/dip ordinals 为 0、correlated=false。
   D3D `ProvenCpuOnly` 只关闭 GPU-skin parity/lease/consumer resolution；ShadowCapture、
   UploadPerDrawData、material override、PrepareDraw、CPU stream main/outline draw、
   BaseVertexIndex 与 restore 路径全部保留。
6. poison/pending authorization/reset/retirement/ingress、nested dispatch/semantic、generic
   upload/DIP、external index ticket 或 marker mismatch 都会拒绝、abort 或 invalidate seal。
   reject/abort/invalidation 是正常 fail-closed；active marker conflict 才是硬错误。上一 sealed
   upload 的 0/1/N fanout 会在下一 upload、generic event 或 dispatch end 前结算。

### 23.4 独立闭合与验收门

新增唯一诊断行：

- `nativeDispatchSeal`：manager view/proposal/scope、upload vertices/bytes、kernel normal-return、
  DIP、0/1/N fanout、conflict 与七条 closure；
- `nativeProdDispatchSeal`：1/256 admission/inclusive/body/complete/cancel raw-QPC 及
  calls/ticks/max/cancelZero closure。

sealed physical upload 仍进入全局 raw uploads、original upload/kernel bytes、f7/s7 unknown
bucket 与 native fanout，但不进入 outside。parser 因而要求：

- fast partition 额外加 `dispatchSealUploads`；
- `f7 == s7 == unknownState + dispatchSealUploads`；
- inside strict closure 加 seal；
- begin 1/127 cadence 的分母使用 `raw-sealUploads`；
- telemetry exact-add 同时覆盖 seal 独立计数和 raw/kernel/bytes/hist/fanout aliases；
- light manager policy 用
  `eager-common == special-never >= 0` 表示 evidence/fail-closed Special eager 集合，不能错误
  写成 `eager==common && never==special`。

两轮内存合成故障注入已覆盖：clean positive、seal closure mismatch、f7/s7 漏 seal、inside/
fast partition 漏 seal、cancel 非零、evidence-Special 差值不等、transient proposal
reject/abort/invalidation，以及 marker conflict。应通过的安全 fallback 全通过；每个破坏都被
对应硬门捕获。native/manager/hook/device 与 parser 最终静态复核 P0/P1=0。

### 23.5 build-only 与下一步

最新 build-only：

`AutoTest/artifacts/gpu_skin_p4_build_only_isolated_diag_light_steady_fastpath_incremental_build_j2_v2_20260715_091416`

唯一 Test Conductor 使用 `ninja -C build32 src/d3d9/d3d9.dll -j2`，`14/14` 最终 x86 link，
exit 0、error 0。DLL SHA256：

`45AB5F7A5CE121F7AA53EF8CB37F9346F30167E9BDE8794CB6BD878D43190F51`

`launchPerformed=false`、`deployPerformed=false`、`autoTestPerformed=false`；未部署、未启动
War3，编译器/linker/runner 自有残留为 0。因此这里没有新的运行/FPS 结论。Common seal 的
离线独立收益估计为 `0.32..0.48 ms/frame`，约占当前 frame penalty 的 `12..18%`。

本节收口时曾计划用固定 cache 对照旧 SafeCopy；后续逐字段审计证明同源 cache 不能提供独立
证据，已明确否决且未实现。真正的 O0 改为观察成功的 D3D9 vertex Lock，详见第 24 节。
`0.20..0.27 ms/frame` 仍只是待运行证明的 outside O(1) 上限估计，不是已获得收益。

后续验证顺序保持：isolated production-light/full crash gate，随后 outline、lifecycle、格式/
透明/special fallback；只有 runtime crossover 证明 193..448 顶点 GPU 路由为正，才下调
449 阈值。最终 foreground `dual_perf` 仍须用户明确让出前台，S1 period 必须保持 1。

## 24. outside-poison D3D9 Lock O0 独立侧证（2026-07-15）

### 24.1 为什么否决同源 SafeCopy cache

原计划先缓存 `ProveOutsideCpuUploadNoPoisonOverlap` 的结果，再比较 cache 与旧路径。但逐字段
审计后确认它不能构成独立晋级证据：

- outer hook 在 1252-byte GX `SafeCopy` 之前只有 `gxDeviceD3d`、vertex count 与 TLS poison
  ledger；当前 output format、六个动态 VB/ring 中实际选中的一条、native D3D device 与
  ring next 都在 snapshot 之后才知道；
- 如果 cache key 使用本次 SafeCopy 才读出的字段，它只是在验证刚完成的同一次读取，不能证明
  下一次读取可安全删除；
- 如果 key 只使用 pre-read 的 GX/count，它无法区分 format、resource、backing、ring advance、
  DISCARD、reset 或 ABA，结构上会 stale。

因此没有实现或宣称这个 cache。旧 SafeCopy 保持唯一 admission authority，下一阶段证据必须
来自真正独立于 GX snapshot 的观察点。

### 24.2 O0 的独立观察点与零权限边界

O0 在一次成功的 D3D9 vertex `LockBuffer` 之后取证。通知发生在
`War3RecordActiveLock + IncrementLockCount` 之后，采集：

- native D3D device、D3D9CommonBuffer、COM vertex buffer 与 resource generation；
- real/mapping `DxvkBuffer`、mapped allocation identity/base，以及三类 storage generation；
- map mode、desc type/pool/usage/size/FVF；
- offset/size、调用者 requested flags、DXVK normalization 后 effective flags、mapped pointer、
  lock depth 与成功 HRESULT。

FVF 独立解析六种 output format/stride。Game.dll vertex ring 的真实 ASM flags 为 wrap
`0x2800`、append `0x1800`（共同包含 `D3DLOCK_NOSYSLOCK`）；它们使用 sidecar-only 常量，
index ring 既有 `0x2000/0x1000` 未改。desc size 必须为 `16384 * stride`，range、mapped base、
normal-return `mappedDst` 与旧 prediction 在可读时均需 exact。

该 payload 始终是 report-only：

- 不参与 fast admission、kernel bypass、poison clear、DIP suppression 或 consumer settlement；
- 不进入 TLS/global quiescence，不阻塞 reset；
- 不改变 callback ABI 9、原生 outer upload、CPU skin kernel 或 caller-owned SEH；
- `authorizationAuthority` 在 parser snapshot/delta/policy 全链固定为 0。

### 24.3 cookie、mutation 与 normal-return 时序

每次 `productionOutsidePoisonScanAttempts` 在旧 SafeCopy 之前创建独立 outer-owned cookie。
固定 TLS LIFO 深度为 8；overflow 在创建时终结分类，真实 probe 则只能由 settle、cancel 或
reset-abort 之一结算。运行闭合为：

`shadowAttempts == poisonScanAttempts == created + overflow`

`created == settled + cancelled + resetAborted + active`

`settled == comparable + unprovable`

poison ledger 的每次物理 create、remove、left/right trim、middle split 和 completed reset 都推进
独立 `uint64_t` mutation generation，wrap 时跳过 0。merge 中的 remove 与最终 append 各自是
真实 commit，分别推进。direct DISCARD retirement 仍通过既有 remove 路径。

两个 kernel normal-return 通知都把 sidecar freeze 作为第一项语义动作；generic manager callback
与 CPU rewrite poison clear 均在其后。fast path 先 settle shadow，再执行可能处理 pending reset
的 completion；generic path 在 `NativeUploadInFlightScope` 内、`CompleteNativeUpload` 后 settle，
但 freeze 已发生在 clear 之前。outer RAII 析构先 cancel shadow，再 cancel旧 fast marker。

reset/retirement、flush epoch、poison mutation、owner/LIFO、multi-Lock、resource/storage/range、
非正常 kernel return 等不稳定状态只会进入 12 类 unprovable。SafeCopy 后两处异步 revalidation
失败会按 exact cookie 先锁存 lifecycle failure，第二处在 `ProcessPendingBridgeReset` 之前完成，
不能误入九格。

### 24.4 N/O/R 九格、离线模型与 parser 门

旧 SafeCopy 与独立 Lock scan 各有 `{NoOverlap, Overlap, ReadFailure}` 三态，形成 row-major 3×3
矩阵。`old N -> Lock O` 单独命名 `legacyMissedOverlap`，是未来晋级前必须保持为 0 的最高优先级
错误。O0 允许 `old O -> Lock N` 等保守 off-diagonal，也允许 unprovable；这些样本都没有授权。

离线模型：

`AutoTest/analyze_gpu_skin_outside_lock_shadow.py`

最终产物：

`AutoTest/artifacts/gpu_skin_outside_lock_shadow_offline_20260715_095245_834601/result.json`

- 27 个确定性边界用例与 50,000 次有界 fuzz 全部通过；
- eligible `N->O=0`；
- strict exact identity 共 `15,862` 例，off-diagonal=0；
- eligible `O->N=2,666` 均来自旧 GX 不能区分 common/resource generation 的保守假阳性；
- mutation/reset generation 违规为 0，四个负向故障注入均被捕获；
- `oldNext + count == 16384` 精确保持 NOOVERWRITE，只有 `>16384` 才 DISCARD。

`AutoTest/run_gpu_skin_p4_isolated.py` 新增两条日志解析、9 matrix、12 reasons、clean-pair 单调
delta 与 `nativePoisonShadowContractClean`。full 模式要求全部 O0 counter 精确为 0；light 模式
在 poison scan delta>0 时要求 attempts delta 与其精确相等，并要求双端 active=0、
`legacyMissedOverlap=0`。unprovable 与其余保守 off-diagonal 在 O0 被报告但不阻断。`py_compile`
和 17/17 纯合成合同测试通过；独立只读复核 P0/P1=0。

### 24.5 build-only 结果与下一晋级条件

唯一 Test Conductor 的低并发 build-only：

`AutoTest/artifacts/gpu_skin_o0_sidecar_build_only_j2_v1_20260715_101034`

命令为 `ninja -C build32 src/d3d9/d3d9.dll -j2`，`23/23` 最终 x86 link，exit 0、error 0，
DLL SHA256：

`83F124FBCAC9E5498456A33299B2DF252038363CA66504EAF6AB001DE7EB2C13`

`launchPerformed=false`、`deployPerformed=false`、`autoTestPerformed=false`、
`foregroundActionPerformed=false`。67 个精确自有构建进程全部自然退出，残留 0；当时 War3 与
World Editor 进程均为 0。C++ 与 parser 最终静态复核均 P0/P1=0。

这里仍没有运行或 FPS 结论。游戏可用后先跑 isolated production-light/full O0 crash gate，
要求九格/三层 closure/scan cross-closure 全部成立、active=0、`legacyMissedOverlap=0`，并读取
unprovable 原因分布。只有独立 sidecar 在目标 cohort 稳定后，才可设计 O1 的 pre-Lock exact
tracker/cache；当前 O0 不能直接删除 SafeCopy，也不能证明预估的 `0.20..0.27 ms/frame` 已获得。
之后继续 outline、lifecycle、格式/透明/special fallback，最终 foreground `dual_perf` 仍须用户
明确让出前台，S1 period 必须保持 1。

## 25. Common/O0/O1 实际运行证据与下一晋级边界（2026-07-15）

### 25.1 已存在但此前未回写的隔离 artifact

Common seal 与 O0 的 production-light gate：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_common_seal_compact_o0_light_parser_v2_20260715_132536`

- verdict `PASS`，运行 DLL SHA256
  `DE1EFBA2787178C3F6C5BEE3CC0F42524F09806842AFB605E79C6CCDE77FF237`；
- seal view publish/query `31,964/213,758`，candidate reject `12,634`，proposal
  `201,124`，commit `138,314`，sealed upload `107,292`，marker conflict `0`；
- O0 attempt/settled `439,486/439,486`，active `0`，comparable `438,656`，unprovable
  `830`，`legacyMissedOverlap=0`；
- 对应 full control
  `gpu_skin_p4_crash_gate_isolated_diag_full_common_seal_compact_o0_full_control_20260715_132706`
  同 DLL `PASS`，seal 与 O0 counter 全冷且 closure 全闭合。

O1a-v2 joint build-only：

`AutoTest/artifacts/gpu_skin_p4_build_only_isolated_diag_full_o1_v2_joint_build_v1_20260715_172136`

唯一 Test Conductor 使用 `ninja -C build32 src/d3d9/d3d9.dll -j2`，`23/23` 最终 link、
exit 0；DLL SHA256
`63AFE0A8B9260387A2CBEF5C39B9AFBFC45E356F0D9D04963AE2AC16B9BCB25D`，未 deploy、
未 launch。

该 DLL 随后的 light joint gate：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_o1_v2_joint_light_v1_20260715_172633`

- verdict `PASS`，控制管道 ready、取证时 exact launch PID 存活，未报告 runtime failure；
- O1 attempt/Lock/kernel/Unlock/frozen/settled 均为 `360,224`，active `0`、unprovable
  `0`、comparisonMissing `0`、authority `0`；
- successful-Lock 独立 verdict 为 `N/O/R = 360,224/0/0`，`legacy N->O=0`；
- legacy outer 与 O1 的矩阵为
  `359,654/0/0/570/0/0/0/0/0`。其中 `570` 个 old O→new N 全部由同 probe
  direct DISCARD retirement receipt 解释，other old O→N 为 `0`；
- O1 scanner 的 `differentTarget=188,630`、storage diagnostic mismatch/real drift/mapping
  drift 各 `161,532` 都没有改变 logical verdict；Unlock 后 diagnostic drift 为
  real/mapping `93/93`，hard-first 四项全 `0`，physical verdict 仍为
  `N/O/notReady = 360,224/0/0`；
- 同场 Common seal commit `111,686`、sealed upload `86,602`、marker conflict `0`，各层
  closure 继续闭合。

对应 full control：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_full_o1_v2_joint_full_v1_20260715_172818`

同 DLL verdict `PASS`；full diagnostics 下 Common seal、O0、O1 全部为 0，cold-control 与
closure 均闭合。

### 25.2 这些 PASS 能证明什么、不能证明什么

上述 light O1 证据证明 successful Lock 是独立于旧 GX SafeCopy 的稳定观察点，并且在该测试
cohort 内没有出现旧 `N`、新 `O` 的漏毒。O1 还把旧 SafeCopy 的大量 read-failure/保守状态
收敛成了当前 Lock identity 下的 exact logical `N`。这足以开始设计 O1 authority promotion，
但仍不能直接删除旧 1252-byte SafeCopy：

1. O1 当前只证明 poison/output-Lock identity，不自动提供 manager 所需的 model/static/palette、
   candidate token、consumer plan 与 normal-return rewrite 授权；
2. O1 verdict 发生在成功 Lock 之后；若要替代 outer-entry SafeCopy，manager/native transaction
   必须允许把 admission 延后到 Lock→kernel 窗口，不能伪造此前不存在的 ABI-9 前缀；
3. storage diagnostic mismatch 虽不影响当前 logical poison verdict，却证明不能拿旧同源 storage
   指针当未来 cache key；晋级必须以 resource generation、Lock range/FVF、mapped destination、
   poison mutation generation 和 exact outer cookie 联合授权；
4. authority counter 仍为 `0`，运行中没有任何 CPU rewrite 因 O0/O1 被跳过。

后续独立审查又发现当时 runner 的清理层有三项 P1：exact-alive 后仍退化为 PID taskkill、部分
pre-STATE launch failure 没恢复视频注册表、fingerprint/duplicate 获取失败可能遗留已启动会话。
这些问题不改变 artifact 在取证时记录的 exact PID、控制管道与 GPU-skin counter，但会否决
“完整测试基础设施/清理验收已经通过”的更宽声明。修复 runner 后必须用同代 DLL 重跑，不能把
上述历史 PASS 直接当发布门。

### 25.3 固定后续顺序

1. 修复并独立复核 runner 的 owned native HANDLE cleanup；诊断 evidence 可以 fail-closed，但
   cleanup authority 必须始终保留到 exact death、视频恢复、桌面关闭与 STATE 清理完成。
2. 串行运行 production-light `none -> o0 -> o1 -> both`，再运行 full `both`；performance ABBA
   必须显式使用 `sidecar=none`，避免把取证开销混入产品路径。
3. 先评估把当前 manager-mutex 内的 Common negative view 发布为 native render-thread-owned
   immutable snapshot，使已证 CPU-only Common dispatch 连 begin/end callback 也不进入；这不需要
   新 Game.dll 语义，但必须保留 evidence cohort、reset/quiescence 与 candidate-positive fail-close。
4. O1 晋级只允许先做显式默认关闭的 shadow authority：在 Lock→kernel 窗口重建完整 native
   admission，仍与旧 SafeCopy 双跑且 authority=0。只有 same-generation lifecycle/outline/格式门
   和 crossover 都通过，才允许单独移除 SafeCopy 的 poison 子证明；不能一次删除整份 GX snapshot。
5. CPU-MT 继续保持独立 producer。原 native kernel 上界约 `0.217 ms/frame`，没有 bucket-specific
   crossover 与足够 flush lead 之前，不把 193..448 小模型重新送进昂贵 manager 正路径。

## 26. actual-Lock O1 production authority 与 TLS 诊断批量化（2026-07-16）

### 26.1 晋级边界：替代 poison pre-scan，不替代 CPU kernel

O0/O1a 已证明 successful D3D9 vertex Lock 是独立于 GX snapshot 的稳定观察点。本轮 production
O1 只晋级这一条窄权限：产品 `sidecar=none` 下，不再为每个 poison-bearing outside upload 预读
1252-byte GX；outer 创建 provisional token，successful Lock 后再以真实 output identity 判定。

O1 不能被理解成“全模型 GPU 蒙皮”或“跳过 CPU skin”：

- 原 native CPU skin kernel 始终精确执行一次；
- kernel 第一语义仍是冻结 actual-Lock/poison state，normal-return 必须真实到达；
- successful Unlock、outer result、resource/storage/reset/poison generation 全部 exact 后才能 commit；
- commit 只表示该 outside CPU-only transaction 可安全结算，不能给 manager/GPU consumer 伪造
  model/static/palette、candidate token 或 P4 authorization；
- overlap 只有具备 exact rewrite/physical proof 才允许清对应 poison，否则 retain；任何冲突都
  fail closed 并保留原 CPU 输出。

actual Lock authority 至少绑定：FVF→唯一 stride、`descSize == 16384 * stride`、offset/size 与
vertexCount、mapped allocation/base/pointer、D3D device/CommonBuffer/COM、resource generation、
real/mapping/storage generation、Lock flags/depth、poison mutation generation、kernel mappedDst 与
Unlock identity。token 是固定 TLS LIFO、outer-owned；overflow/reentry/reset/cancel 都有独立终点。

### 26.2 legacy evidence 与双层 parser 合同

产品路径每 127 个 poison attempt 保留一个 immutable legacy evidence；该 cohort 永不授权。
任意 O0/O1a sidecar 开启时，legacy scan 仍覆盖全部 attempt，production O1 只能 retain：

```text
sidecar none:
  poisonScanDelta == evidenceAttemptsDelta
  poisonScanTotal == evidenceAttemptsTotal
  authorityAttempts == acceptedWithPoison + evidenceOverlap + evidenceReadFail

sidecar o0/o1/both:
  poisonScanDelta == authorityAttemptsDelta
  poisonScanTotal == authorityAttemptsTotal
  acceptedWithPoison == legacyNoOverlap
  production authority == 0
```

clean pair 同时要求 attempt/created/settled、Lock N/O/reject、kernel ready/reject/normal-return、
Unlock exact/reject、commit authority/retained/clear 与 evidence 九格闭合；两端 active、overflow、
cancel、resetAbort、mismatch、unprovable、evidenceAuthority、legacyBackedAuthority 必须为 0。
累计 endpoint 合同是必要的：只校验 pair delta 会漏掉首次 clean endpoint 之前遗留的一次额外
non-evidence scan。synthetic 已加入 persistent-baseline mismatch，证明 delta 可看似 exact、但
累计门必须拒绝。

离线 transaction 模型：

`AutoTest/analyze_gpu_skin_o1_transaction.py`

最终 100,000 fuzz 产物：

`AutoTest/artifacts/gpu_skin_o1_transaction_offline_20260716_070846/result.json`

unsafe case 为 0；outer 不能预计算 output bytes 的 case 仍可由 successful Lock 的完整 range/FVF/
identity proof 独立授权，非正常返回、漂移、reset、nested/reentry 都只能 retain。

### 26.3 poison pre-scan 退为 evidence 后的隔离运行证据

production-light / sidecar-none 正式门：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_o1_tls_batch_prod_none_v1_20260716_073859`

- DLL SHA256
  `074C4DDA1C1B50A1FB8C79608D7E929DE0A09C0437ED33B95063A967977C9736`；
- lifetime O1 attempt/authority/retained `491,686/487,815/3,871`；legacy scan `3,871`，即
  `0.787%` attempts，较全量 scan 约少 `99.21%`；
- clean pair `2,476` attempt/armed/Lock/kernel/Unlock/settled exact，commit N `2,457`，
  evidence-retained `19`，所有 reject/mismatch/clear/leak 为 0；
- cumulative scan `3,871 == evidence attempts 3,871`；evidence matrix 为
  `3,868 N→N + 3 O→N`，后者均保守 retain 且 authority 仍为 0。clean pair 的 19 例才全部为 N→N。

production-light / sidecar-both retained 控制：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_both_o1_tls_batch_both_retained_v1_20260716_074428`

- clean pair attempt/created/settled `2,492`，armed/retained `2,489`，production authority `0`；
- O0 有 3 个 poison-mutation unprovable，O1a 以 3 个 exact direct-discard receipt 解释 O→N；
  `legacyMissedOverlap=0`，所有 report-only authority 为 0；
- cumulative scan `397,126 == authority attempts 397,126`。

full / sidecar-none 最终冷控制：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_full_sidecar_none_o1_tls_batch_full_none_final_v2_20260716_075835`

full 下 manager-free `outsideNativeFastPath`、poison scan/O1 authority 与 TLS flush/batched-add 全零；
raw uploads、outside callback skip 与既有 P4 bypass 仍按 full 证据路径正常为正。旧 P4 lease/commit、
poison、index、ledger、normal-return 及 steady-state reset/retirement fault/pending diagnostics 与 crash
gate 均继续闭合。三轮 `lifecycle.enabled=false`，尚未执行 resize/map/device reset/second-process；
三轮均使用 exact retained HANDLE 终止，隔离桌面/视频恢复，War3/runner/build 自有残留为 0。

### 26.4 O1 diagnostic atomic 的 TLS batching

production O1 正常事务原先每个候选执行约 12 次 process-wide relaxed `fetch_add`。15 个只用于
诊断的正常路径字段现并入既有 render-thread-owned `NativeTelemetryDelta`：

```text
attempt / created / armed / settled
Lock notification / N / O
kernel ready / normal-return
Unlock notification / exact
commit N / rewrite / poison-clear / authority
```

`g_nativeOutsidePoisonAuthorityActive` 仍是即时 acquire/release atomic，并继续控制 direct-discard、
kernel capture 与 normal-return。overflow/cancel/resetAbort、Lock/kernel/Unlock reject、retained、
全部 evidence/matrix 也保持 atomic；这些异常字段收益低，且 reset abort 可能发生在第一次 delta
flush 之后。非 render thread、首 flush 前、full diagnostics、telemetry fault/overflow 都自动回退
atomic。pending 在首个 TLS mutation 前 release 发布；常规 flush、forced counter/quiescence snapshot
在 owner render thread 合并，off-thread snapshot 看到 pending 时不能成为 clean endpoint。

prod-none clean pair 的 29,674 次 O1 logical counter update，保守估算至少减少 27,190 次全局 RMW
（约 1,182 次/frame proxy）。运行 exact-add：

`21,167,306 == 21,167,306`

sidecar retained exact-add：

`12,596,985 == 12,596,985`

这些大数包含 bytes 等数值加和，只证明每个 TLS add 精确进入 aggregate，不能解释成 RMW 次数或
毫秒收益。full 明确禁用 batching，最终 parser 显示 `batchingEnabled=false`、flush/batched-add
`0/0`、expected `0`、`exactAdds=true`；light-mode 理论公式只保留为 report-only 字段。

build-only：

`AutoTest/artifacts/gpu_skin_o1_authority_tls_batch_build_only_j2_v1_20260716_073601`

唯一 Test Conductor 的 `ninja -C build32 src/d3d9/d3d9.dll -j2` 为 `2/2`、warning/error `0/0`；
artifact 明确记录 build performed、deploy/launch/AutoTest/foreground 均 false，8 个精确 owned build
进程自然退出且残留 0。该 build-only 本身不声称运行验证 deploy/launch 权限链。

### 26.5 当前性能解释与下一步

以上只证明 poison pre-scan SafeCopy 次数和 diagnostic RMW 数量下降，不能声明 FPS 已恢复。
RMW 硬下降由代码路径与 exact authority/TLS ledger 证明；production period-256
样本中，最大可见 parent 是 Game.dll 原生 `ApplyDrawStateAndSamplerPair`，约 `1.03 ms/frame`；
GPU-skin detour/scope residual 约 `0.069 ms/frame`。GPU-skin 固定税的其他可见 parent 约为 outer
residual `0.19`、DIP device residual `0.17`、dispatch begin+end `0.23 ms/frame`；它们可能嵌套在
semantic original 内，禁止横向相加。pre-batching baseline 为
`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_o1_authority_product_none_v3_20260716_071731`，
TLS build 样本为本节 26.3 的 prod-none artifact；两轮相位不同且都含约 2 ms 单次尖峰，不能把
差值归功 TLS batching。

新增可执行 kernel-route enum 现阶段否决：既有 dispatch/outside/sentinel cookie 已承担路由，
新 enum 只能省 1–2 个 TLS miss，却增加 outer set/clear 与 kernel read，且不能删除 O1 capture、
选中 marker 的 exact predicate 或 generic P4 proof。下一步先用相同 DLL 的 isolated ABBA 归因
当前 disabled↔bypass 总增量；该测试没有 TLS batching runtime off 开关，不能隔离其独立收益。
若仍需拆热点，应在同一 semantic sampled scope 内建立 outer/DIP/remaining-state child closure。
之后按 outline→resize/maximize/restore + map/device reset + second-process lifecycle→格式/透明/
special fallback 前进，再做高压/光影图覆盖。最终 foreground
`dual_perf` 仍必须等用户明确让出前台，S1 period 必须保持 1。

### 26.6 额度收尾：ABBA 未完成与 runtime-policy provenance WIP

首次同 DLL ABBA：

`AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260716_081116`

只完成 A1 disabled 与 B1 bypass；B1 的 raw AutoTest 正常，但旧 parser 将 O1 产品分区误套成
`acceptedWithPoison == poisonNoOverlap`，case 被判 false 后按 failure-stop 合同终止，B2/A2 未运行。
两窗 frame/main/GPU 分别为 `9.785/7.862/1.430` 与 `13.992/12.097/1.500 ms`，单窗未平衡差
`+4.207/+4.235/+0.070 ms` 只能参考，不能称 order-balanced 或正式 FPS 结论。模块、DLL SHA、隔离
桌面/视频恢复与双重清理均 exact，最终 War3/runner/build 自有残留 0。

修正旧公式后，审计进一步否决仅靠 launch env 声称 sidecar-none。当前 WIP 在 performance report
序列化 immutable runtime config 与 `NativeBridgeCounters` 的 policy/value/explicit/invalid/
closure/config-counter exact；ABBA parser 必须逐字段证明 explicit `none`，missing/non-none/mismatch
均 fail closed。manager 差值改名 `derivedUnscannedAcceptedWithPoison`，并显式
`independentAuthorityVerified=false`；真正 O1 authority 仍只认 P4 forced snapshot ledger。Python
`py_compile` 与四组 synthetic 通过，旧报告缺字段而被预期拒绝。

本节 WIP **尚未完成 C++/parser 终审、build、deploy 或运行**。部署版本仍为
`074C4DDA1C1B50A1FB8C79608D7E929DE0A09C0437ED33B95063A967977C9736`；下一步必须先 build 得到新
SHA、更新 runner exact hash、跑 production-light P4 回归，再从头完成 A1/B1/B2/A2。

旧两窗 draw-chain 预定位为 sampled leaf `18.005 -> 23.195 us/call`，按约 473 leaf/frame 折算
约 `+2.46 ms/frame`：HostOther `+1.24`、GpuSkinDip `+0.60`、BeforeUi `+0.38`。该拆分仍未平衡，
只说明继续应查 CPU host/render lane。`semanticOriginal` 不含 caller tail 的 DIP；同域 closure 只能是
`semanticOriginal = outerChild + remainingApplyState`，若需要把 DIP 纳入同一 parent，应提升到
Common/Special `dispatchOriginal`，不能把不同物理 scope 强行相加。

## 27. 2026-07-17：完整 ABBA、outside DIP reader 与资源驻留普查

此前 runtime-policy provenance WIP 已完成静态、build、deploy 与运行闭合。当前 DLL SHA256 为
`CB72906DF8A4EBD0E53A8860A4122A622EACACE161984BF9B98A6B314C0C0C4B`。production-light P4：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_native_outside_dip_reader_o1_prod_none_resource__20260717_025248`

P4 hard gate 全部通过：bypass attempt/authority/commit/fallback
`16,539/14,471/14,471/2,068`，ledger classified/resolved/consumed
`46,335/28,942/28,942`，poison/index/planMismatch/leak/unreserved/duplicate、reset/retirement pending 与
所有 active endpoint 均为 0。O1 lifetime attempt/authority/retained
`544,943/540,653/4,290`，original kernel 仍精确执行；这不是扩大接管权限。

同 DLL 完整 isolated A1/B1/B2/A2：

`AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260717_030038`

| mode | FPS mean | frame ms | main ms | measured GPU ms |
|---|---:|---:|---:|---:|
| disabled | 104.8405 | 9.5490 | 7.6180 | 1.4205 |
| bypass | 84.7475 | 11.8025 | 10.3010 | 1.4250 |
| bypass - disabled | -20.0930 | +2.2535 | +2.6830 | +0.0045 |

因此剩余退化仍在 CPU render lane，不是 compute shader/GPU 饱和。period-300 尖峰摊销差只有
`+0.00033 ms/frame`，也不是主因。draw-chain period-256 的 order-balanced 预定位按约
473.67 leaf/frame 外推：LeafHostRoot `+1.049 ms/frame`，其中 GpuSkinDip `+0.482`、HostOther
`+0.458`、BeforeUi `+0.100`；约 `1.63 ms` main delta 位于 leaf root 之外，禁止把嵌套 scope 横加。

本轮 outside DIP reader 已把结束端旧 load+CAS 循环改为单次 seq_cst `fetch_sub`，并删除 admission
后重复的整套 TLS exact walk；underflow 保留非零 fail-closed sentinel。上述 P4 证明 begin/end、
cover、reader/evidence 分区及 clean endpoint 全闭合，但 ABBA 仍表明还需继续合并 transaction cover。

资源普查产物：

`AutoTest/artifacts/resource_residency_census_isolated_20260717_025503`

完整数字、managed texture Lock 语义、allocator 64 MiB section 与 Seal-and-Evict 边界见
[resource_residency_census_2026_07_17.md](resource_residency_census_2026_07_17.md)。首轮看到
`44,735,294 B` duplicate host backing，其中 `44,576,936 B` 是 587 张 managed texture；
`23,045,172 B` 仅为未来可 materialize 的静止候选，**不是现在可直接 free 的权限**。最窄安全路线
不是直接 `UnmapData()`：该操作不能保证下一次 Lock 获得相同地址，且共享 1 MiB mapping range
可能让单资源 unmap 完全不释放 VA。当前顺序应为 allocator tail closure 正确性、GPU static mirror
与 model cache immutable snapshot 共享，最后才在重图 chunk-cohort 证据闭合后设计 managed texture
Seal；真正释放 backing/commit 仍需精确 loader proof、device-authoritative/materialize 状态机和
chunk closure。

## 28. 2026-07-17：wrapper 微调止损与 VS-in-draw 新路线

### 28.1 no-poison direct receipt 的安全门与负性能结论

本轮把严格 outside/no-poison/no-dispatch/no-pending/reset-retirement-clean cohort 直接路由到原
outer upload 与原 CPU kernel，跳过 generic manager/ABI-9/O1 admission。该 receipt 仍保留
1/127 evidence、真实 kernel normal-return、TLS quiescence、poison mutation/reset generation 与
conflict/cancel/active 闭合；它不授权 GPU output，也不扩大 P4 bypass 白名单。

build-only artifact：

`AutoTest/artifacts/gpu_skin_p4_build_only_isolated_diag_light_sidecar_none_outside_no_poison_direct_resource_census_v2_20260717_064440`

DLL SHA256：

`1FD0118FDE49C632C5731FC5C0E1A3B2788ABAF2B87394D6F61063351163CE3E`

production-light P4：

`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_outside_no_poison_direct_prod_none_v1_20260717_064657`

- lifetime direct attempt/kernel/normal/completed 均为 `532,916`；
- clean pair 四项均为 `4,349`；
- no-normal、conflict、cancel、active、reset-while-active 与 late-poison 均为 0；
- 既有 poison/index/ledger/native-kernel-normal 与进程/桌面清理硬门全部通过。

同 DLL 完整 isolated A1/B1/B2/A2：

`AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260717_065042`

| mode | FPS mean | frame ms | process ms | main ms | measured GPU ms |
|---|---:|---:|---:|---:|---:|
| disabled | 105.1770 | 9.5225 | 9.7555 | 7.6200 | 1.4205 |
| bypass | 84.2920 | 11.8700 | 12.7185 | 10.4635 | 1.4195 |
| bypass - disabled | -20.8850 | +2.3475 | +2.9630 | +2.8435 | -0.0010 |

B1/B2 分别有 `460,088/442,849` 次 direct receipt，约 `142.5/143.7 calls/frame`，但相较前一
同图完整 ABBA 的 frame/main delta `+2.3125/+2.7595 ms` 没有改善，差异为噪声或略差。
因此 receipt 的安全价值成立，性能价值没有被测出；不得把它写成 FPS 优化。

### 28.2 为什么停止继续削低层 wrapper

本轮 B1 raw 约为：

- `379.2 uploads/frame`；
- `474.3 DIPs/frame`；
- `4.26 candidates/frame`；
- 仅 `3.73 bypassed kernels/frame`。

也就是说，大量调用支付低层分类与安全合同，只为极少数实际 GPU job 服务。三轮 ABBA 中
`War3NotifyGpuSkinDip` 增量稳定为 `+0.4826/+0.4790/+0.4853 ms/frame`；把整个函数删除都低于
0.5 ms，而且 correlated draw 不能删除。outer fast/fallback 两条互斥路径的全部 bookkeeping
理论总上限均值约 `0.5044 ms/frame`，但这包含 admission、poison/O1、cover、completion 与
evidence，全部删除等同破坏 P4，不是可实施 patch。其他稳定单段上限为 kernel evaluate
`0.2120`、dispatch begin/end `0.1260/0.0982`、semantic shell `0.0680`、DIP bridge
`0.1745 ms/frame`。

结论：当前不存在可证明收益 `>=0.5 ms/frame` 的单一安全 wrapper patch。继续做十几个小 TLS/
atomic/branch 微调已经进入收益饱和；剩余约 2 ms main delta 也不能用这些片段解释。后续若仍需
观测，parent 必须提升到 Common/Special `dispatchOriginal`，再在同一采样域拆
`ApplyDrawStateAndSamplerPair` 与 caller tail，不能拼接不同物理 scope。

### 28.3 新主线：fixed-function VS skin prelude

性能路线转为 **VS-in-draw hybrid**，而不是逐 draw compute 或独立简化 VS。正确边界是在 DXVK
D3D9 fixed-function VS ubershader 的最前面增加可选 War3 skin prelude：prelude 从 static atlas
与本 flush exact 3x4 palette 取得 position/normal/UV，随后继续执行原 fixed-function 的
world/view/projection、lighting、fog、texgen 与 pixel-interface 逻辑。任意 programmable/custom
VS 首期 fail closed，避免丢失 Warcraft 材质语义。

可复用资产：

1. `GpuSkinStaticResource` 的 SoA position/normal/group-byte/UV0/UV1、content/layout/map/device
   generation；
2. 已验证 `1824/1824` 的 native 3x4 precise scalar 顺序；
3. flush-time exact palette、group count/signature 与 live/trusted proof；
4. `frameTag/flushEpoch/batchId/renderablePart/geosetData/layer/token`、DIP signature、FVF/stride
   与 native index ticket；
5. P2/P3/P4 的 poison、reset、retirement、consumer settlement 和 index local-range 合同。

首个可运行正确性切片 **VS-A** 固定为 Common/stage11/skinMode1、triangle list、instance1、
fixed-function、标准一层 UV format。CPU kernel 与 compute output 继续执行，Main 只对 exact draw
试用 VS prelude，任一条件不符画原 CPU stream。它验证 shader parity、状态恢复和 input lifetime，
不宣称性能收益。

首个有性能意义的 **VS-B** 必须等 VS-A 通过后才启用：main-only candidate 不再分配 post-skin
output、不建 compute job，只保留 static+palette draw-input lease；随后才允许该 cohort 跳 CPU
kernel。Shadow/Outline 尚未成为 input-backed consumer 时必须在 kernel 前拒绝 bypass，不能晚回
CPU。

最终保留 hybrid：小/中或 main-only 模型用 direct VS，极大模型或 Main+多级 CSM+点阴影+outline
等多 consumer 高 fan-out 模型可继续 compute pre-skin；交叉阈值必须由隔离 A/B 决定。

### 28.4 input lease 是 VS 路线的 P0 前置条件

static atlas 当前主要为 compute/transfer 声明 stage；VS 路线要增加 vertex-shader read。更重要的
是 palette upload page 目前在 compute producer fence 后即可回收，若 Main/Shadow/Outline VS
继续引用同一页，allocator 可能在 draw 前覆写 bytes。lambda 持有 `Rc<DxvkBuffer>` 只能保住
buffer 对象，不能阻止同页重用。

因此必须新增 owning `GpuSkinDrawInputLease`：绑定 static/palette slices、epochs、token、consumer
bits 与 page id；palette page 只能在 consumer window 关闭、且排在全部对应 draw 后的 consumer
fence 完成后回收。map/device reset 时旧 lease 进入 retiring，pending/active 最终必须为 0。
Main/Shadow/Outline 共享的是同一 input lease，而不再要求同一 post-skin output slice。

默认关闭的 build-only scaffold 可以先加入 route/config、32-byte draw params、input-lease描述与
生命周期合成测试；在 isolated VS-A 之前不得绑定 shader、改变默认 fixed-function 行为或扩大
P4 authorization。

### 28.5 资源驻留的当前执行顺序

轻图 census 中真正可见的 CPU/GPU duplicate 为 `44,735,294 B`，但其中 managed texture 的
`23,045,172 B` 静止观察值没有 free authority。当前低风险顺序：

1. 修复 `D3D9MemoryChunk::AllocLocked` 吞掉 `<4 KiB` 尾段却无法在 Free 时归还的 closure 缺陷；
   它只保证最终空 chunk 真能关闭，不能先验声称 steady-map 立刻少 64 MiB；
2. 让 GPU static resource 与 `ShadowModelResourceCache` 共享 immutable snapshot，第一阶段轻图
   可消除约 `158,102 B` persistent mirror 与 `55,867 B` transient queued copy；第二阶段再合并
   cache aliases，理论再消除 `331,046 B`；Windows heap 是否立即 decommit 仍未知；
3. 重图按 chunk 补 `candidate/survivor/mapped-range-ref` cohort，之后才判断 managed texture Seal、
   分池、小 chunk 或 survivor evacuation。直接 `UnmapData()` 与直接 free 当前都不安全。

本节 VS 路线尚未运行；不能据此声明全模型接管、FPS 恢复或 P4 发布完成。运行顺序仍是
VS-A parity/restore -> VS-B bypass/ledger -> outline -> CSM+point shadow -> resize/reset/relaunch ->
格式/透明/special fallback -> SunkenCity isolated pressure。最终 foreground `dual_perf` 必须等用户
明确让出前台，S1 period 保持 1。

### 28.6 默认关闭脚手架、快照去重与同代运行门

本轮已经加入默认不激活的 VS-A 数据合同：

- `GpuSkinExecutionRoute` 只解析未来使用的
  `DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE=vertex_shader`；默认和非法显式值都保持 `Compute`，当前任何
  manager/device/native/P4 路径都不读取该字段；
- `GpuSkinVsDrawParams` 固定 32 字节，`GpuSkinInputLeaseDesc` 固定 88 字节；当前没有 shader 绑定、
  draw 激活或 P4 authority；
- static atlas 的 stage mask 已允许未来 vertex shader 读取，但当前资源与绘制行为不变；
- D3D9 fixed-function ubershader 默认启用，未来 VS-A 只允许进入该路径；custom VS 或关闭
  ubershader 时必须逐 draw fail closed。

输入租约离线产物：

`AutoTest/artifacts/gpu_skin_vs_input_lease_offline_20260717_081253/result.json`

7 个确定性场景覆盖不可逆 submission ticket、reset 后旧 ticket receipt、page generation 重开、
stale/copy ticket ABA 拒绝、pending 与 consumer fence 双门；`unsafePageReuses=0`。该产物明确
`runtimeImplementationPresent=false`，不能冒充运行证明。未来 C++ 实现还必须绑定真实
`Rc<DxvkFence>` identity 与 device generation，不能只靠 fence 数值。

资源侧已完成第一层不可变快照共享，并修复 D3D9 allocator 的 `<4 KiB` 尾段丢失、零分配和
`MapViewOfFile` 失败账本。离线门为快照共享 6/6、allocator 7/7。唯一 Test Conductor 随后完成：

- 最终中文注释版本 build artifact：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_zh_comments_final_v1_20260717_091811_build`，
  x86 `-j2` build PASS，DLL SHA256
  `CF8922D617EA4698A5945D924E5A527795AE32A3ACFA9326ECB098509468FEC2`；
- 最终 P4 artifact：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_zh_comments_final_v1_20260717_091811`，
  bypass `11,789` calls / `222,122,784 B`；poison、index、ledger、restore、
  nativeKernelNormal、reset/retirement 与 crash scan 全部闭合；
- census artifact：
  `AutoTest/artifacts/resource_residency_census_isolated_20260717_084857`，GPU static mirror 与
  queued/peak queued host copy 均为 0，总 duplicate `44,577,192 B`，其中 managed texture 仍占
  `44,576,936 B`；`23,094,324 B` lazy-readback candidate 仍不具备释放权限。

资源 census 在仅有注释差异的前一 DLL
`2DED992F29C78F6DB527CBAB2599A1B449CE125D9615FA40318D591891CE7F69` 上采集；最终中文注释 DLL
没有重跑 census，也没有改变任何资源逻辑。以上运行只证明当前 compute/P4 基线没有被这次重构
破坏，并证明 `158,102 B` 的 GPU static 持久 CPU mirror 已消除；它在当时没有证明 VS-A 已运行，
也没有产生新的 FPS 收益结论。真实 input lease 与 VS-A 的后续结果见 28.7；VS-B 仍必须等其余
正确性门通过后才可跳 compute/CPU kernel。

### 28.7 真实 consumer fence 与 VS-A 运行门闭合（2026-07-19）

本轮发现工作区已有私有 fixed-function VS 变体和 draw 接线草案，但 input lease 仍是不可激活的
半成品：`GpuSkinInputLeaseReceipt` 从未创建、`storagePageGeneration` 未写入，native
dispatch/upload epoch 也没有同步到 receipt，因此 `GpuSkinInputLease::operator bool()` 永远为 false。
现已把这条能力链闭合：

1. 每个受支持 candidate 把 palette 从 producer upload page 复制到 device-local output arena 的
   独立 storage slice；租约同时持有 immutable static atlas slice、palette slice、storage lease id、
   page id、page generation 与共享 receipt；
2. receipt 初始为 `Pending`，其 desc、buffer identity、range、token、map/device/frame epoch、
   consumer mask 必须与 draw lease 和 storage lease 逐字段一致；native upload 成功后，
   dispatch/upload epoch 在 manager mutex 内同时写入 draw lease 与同一 receipt；
3. 正常帧尾先关闭 consumer window，再把 fence signal 排在 Main/Shadow draw 之后；只有
   `War3GpuSkinResources::retireOutput` 接受同一 `Rc<DxvkFence>`/value 后，receipt 才进入
   `ConsumerCommitted`。producer-only 恢复路径进入 `ProducerOnly`，未录制或被拒绝批次进入
   `Cancelled`；三种终态都禁止新的 CPU 侧 draw 使用旧 capability；
4. output arena 仍以 active lease + page generation + fence completion 为唯一重开条件。仅持有
   `Rc<DxvkBuffer>` 不能授权 page reuse；错误 storage identity、旧 generation、错误 fence 或终态
   receipt 都 fail closed；
5. VS-A 只接受显式 `DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE=vertex_shader`、Common/stage11/
   skinMode1、triangle list、instance1、fixed-function FVF `0x112`、单 UV、format2。custom VS、
   PositionT、SWVP、材质 override 或任意租约漂移继续使用既有 compute output；CPU kernel、compute
   job 与 output VB 仍完整执行，P4 绕过权限没有扩大；
6. 私有 ubershader 在 stock fixed-function VS 前读取 static/palette storage，按已验证的 precise 3x4
   标量顺序生成 position/normal/UV0，再继续原 world/view/projection、lighting、fog、texgen 与
   pixel interface。每次 draw 在同一 CS 闭包内完成私有 shader/binding/push-data 的绑定、清零、解绑
   与 stock shader 恢复；palette copy 后有 transfer-write -> vertex-shader-read barrier。

离线 `AutoTest/analyze_gpu_skin_vs_input_lease.py --self-test` 与 runner 路线合成门通过；x86 build
和随后 no-work 通过。DLL 29,261,468 bytes，SHA-256
`49ECC06749FA13C413943D93D7AC34067B6C139C907BBAC5CEA9595E8FF3379E`，source/deployed exact。

- Compute 控制：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_compute_vs_a_compute_control_v1_20260719_133219`，
  P4 PASS，VS 字段全冷，旧 compute/bypass 与 StormBreaker stable 默认没有运行回归；
- VS-A 正式门：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_vs_a_consumer_fenced_v2_20260719_143012`，
  P4 PASS。input prepared/submitted `12,540/12,540`，palette bytes
  `35,560,752/35,560,752`，compute jobs `12,540/12,540`，Main VS draw/clear
  `11,863/11,863`，Shadow capture/direct/replay `11,863/148/148`；全部 input/state reject、consumer
  mismatch、binding clear mismatch 与 unknown replay 为 0；output+input-storage retirement
  `25,080`，pending `0`，upload page allocate/reclaim `1/1`；两张隔离截图人工复核正常，无 crash；
- VS-A 首轮 v1 的实际 Main/Shadow/资源数据也闭合，但 runner 曾错误要求每个很短的 clean-pair
  间隔都必须新增 Shadow replay，因该窗口 direct delta 为 0 被误拒。生命周期已有 144 次 direct；
  parser 已改为允许“零进展但所有相关增量严格为零”，并加入合成回归，v2 正式通过。
- outline 专项：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_vs_a_outline_regression_v1_20260719_150932`，
  P4 PASS。input prepared `5,918`，Main draw/clear `5,535/5,535`，Shadow direct/replay
  `7,376/7,376`，outline submitted/same-slice `5,026/5,026`；slice mismatch、restore overlap/pending
  均为 0，output+input-storage retirement `11,836`、pending `0`；
- lifecycle 专项：
  `AutoTest/artifacts/gpu_skin_p4_lifecycle_isolated_diag_light_sidecar_none_route_vertex_shader_vs_a_lifecycle_v1_20260719_151401`，
  P4 PASS。完成 1280x720 resize、maximize、restore、两次 reset generation 与第二进程 relaunch；
  reset request/completion/ack `2/2/2`，generation `2/2/2`，wrong-thread、retirement fault、
  fail-closed 与 retirement overflow/invalid/pending/fault 全为 0；两个进程均由 retained native handle
  精确终止；
- 最新离线产物：
  `AutoTest/artifacts/gpu_skin_vs_input_lease_offline_20260719_153556/result.json`，7 个确定性场景、
  17 项 runtime integration audit 全通过，`unsafePageReuses=0`。它仍是 offline-only，不能替代上述
  build/runtime artifact。

该结果不能声明性能收益，也不能立即进入 VS-B 的 compute/CPU skip。outline 和 lifecycle 已通过，
当前正确性门只剩格式/透明/special fallback。该门通过后，才允许为 main-only cohort 设计不分配
compute output 的 VS-B，并由隔离 A/B 决定 hybrid 交叉阈值。foreground `dual_perf` 仍需用户明确
让出前台。

### 28.8 VS-B0 input-only 中间门闭合（2026-07-19）

为避免把“省略 compute output”与“跳过原生 CPU kernel”一次性混在同一个不可逆门中，新增显式
`DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE=vertex_shader_input_only`，内部称 **VS-B0**。它不是最终
VS-B：只对白名单内的 opaque、format2、单 UV、fixed-function candidate 分配 static+palette input
lease，不分配 post-skin output slice，也不记录 compute dispatch；P4 preflight 则在 manager 内明确
拒绝授权，所以原生 CPU kernel 仍精确执行一次，Outline 和所有拒绝项继续使用原生 CPU stream。

实现边界如下：

1. palette 只从既有 upload page 分配并复制所需字节，复用真实 input receipt、page generation 与
   producer/consumer fence；palette storage lease 只充当 draw capability，绝不伪装成 post-skin
   vertex buffer；
2. Main fixed-function draw 使用同一 ubershader prelude；Shadow 在语义 capture 后附加同一 input
   lease，同时保留 CPU position/IB/UV capture 作为逐 draw fallback；Outline 未获得 input-only
   authority，要求 Outline 的 DIP 整体回到旧路径；
3. transparent、非 format2、非单 UV、Special path、custom VS、PositionT、SWVP 与任意状态漂移都
   不进入 `finalizeInputOnlyCandidate`，继续走旧 compute/CPU 路径；本轮两张自动图的 transparent
   计数均为 0，因此这里只能声明静态 fail-open 边界，不能虚称透明材质已有运行覆盖；
4. input-only 的 P4 冷合同要求 bypass authorization/commit、poison、index ticket、direct discard、
   bypassed kernel calls/bytes 全为 0，并要求 CPU kernel original calls/bytes 为正；旧 compute/P4
   的 positive-bypass 硬门未放宽；
5. Bypass 模式旧 settlement 曾无条件拒绝 `CpuFallback`。高压图首次运行恰有 2 个已预留 Shadow
   consumer 在逐 draw capture 前退出，导致 `planMismatch/suppress/leak=2`。现在只在“显式 VS-B0、
   `bypassCommitted=false`、failure=CpuFallback”时允许该终态；真正 P4 kernel skip 仍只能
   suppress+fuse。复测出现 3 个合法 Shadow CPU fallback，账本精确为
   `resolved = consumed + cpuFallback`，且 suppress/leak/planMismatch 全为 0。

最终 DLL 为 29,276,269 bytes，SHA-256
`F9629893627140E98D5CA874B0C6F65F40E1535FA8B302957D014B648897A6DF`，source/deployed exact。
最新离线产物
`AutoTest/artifacts/gpu_skin_vs_input_lease_offline_20260719_223014/result.json` 为 schema 4，
7 个确定性生命周期场景与 24 项 VS-A/VS-B0 runtime integration 静态审计全部通过，
`unsafePageReuses=0`、末端 pending=0；它不替代以下隔离运行证据。

- 普通图正式 crash-gate：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_input_only_vs_b0_input_only_v2_20260719_213844`，
  PASS。input prepared/submitted、Main draw/clear 与 Shadow capture 均为 `21,631`；省略
  `21,631` 个 compute output/job 与 `408,213,536 B` output 写入；Shadow direct/replay `280/280`；
  CPU kernel `2,084,826/2,084,826` 全部 original，P4/poison/index ticket 全冷；ledger
  `43,262/43,262`，所有 fault/leak 为 0；
- lifecycle：
  `AutoTest/artifacts/gpu_skin_p4_lifecycle_isolated_diag_light_sidecar_none_route_vertex_shader_input_only_vs_b0_input_only_lifecycle_v1_20260719_214555`，
  PASS。resize/maximize/restore、reset request/completion/ack `2/2/2`、generation `2/2/2` 与第二进程
  relaunch 全部通过，wrong-thread、retirement fault/pending、ledger leak 均为 0；
- 高压格式/fallback：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_input_only_vs_b0_high_pressure_format_v2_20260719_220731`，
  PASS。input/Main `21,933/21,933`，Shadow `21,930`，合法 Shadow CPU fallback `3`；省略
  `384,015,904 B` output 写入；Special fallback `118,212`、非目标 layout fallback `580,591`；
  ledger classified/resolved/consumed/fallback 为 `65,799/43,866/43,863/3`，suppressed、leak、
  unreserved、duplicate、planMismatch 与 retireDeferred 全为 0。

以上数字是正确性/工作量计数，不是显存永久节省，也不是 FPS 收益。VS-B1 在跳原生 CPU kernel 前
仍必须新增独立显式路线，并证明：Outline 在 preflight 前已排除、每个请求的 Shadow 都能以 input
lease 提交或在 kernel 前拒绝、transparent/特殊格式保持旧路径、native normal-return/poison/index
合同不会把 palette capability 误当 output。完成这些后才能跑 isolated A/B；foreground
`dual_perf` 仍需用户明确让出前台。

### 28.9 VS-B1：input capability 驱动的真实 kernel bypass（2026-07-20）

VS-B1 使用独立显式路线 `DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE=vertex_shader_bypass`，枚举值为 3。
它没有给 VS-B0 直接升权，也没有改变默认 Compute。候选仍由既有 Common/stage11/skinMode1、opaque、
format2、单 UV 与 single-DIP baseline 产生，但 kernel 前的不可逆授权改为验证 input capability，而不是
验证 post-skin output slice：

1. `GpuSkinNativeBypassHostRequest` 同时携带 route、palette storage capability 与完整
   `GpuSkinInputLease`。Manager 和 D3D9 host 分别验证 token、frame/map/device/native epoch、static/
   palette range、buffer identity、storage lease/page generation、consumer mask 与资源 usage/stage/access；
2. Main 必须确定进入 fixed-function VS ubershader：无 custom VS、PositionT、SWVP、录制、实例化、
   world/post material override 或 vertex blend，FVF 必须为 `0x112`、stream mask 为 1、stride 为 32；
3. Outline 尚没有 input-backed 绘制，因此 `War3ShouldDrawOutline()` 为真时在 kernel 前拒绝。Shadow
   请求为真时必须证明 semantic scene producer 与 disable-legacy-capture 路径都已启用，并且同一
   renderable 的 IB scratch 已暖好；冷态先执行一次 CPU 路径建立 IB，下一帧才可能晋级；
4. B1 只上传 palette 到 consumer-fenced device-local storage，不创建 compute job/output。Main 使用
   WVS2 activation magic 读取 static/palette；Shadow 的 position binding 精确别名 static atlas 起始的
   `vertexCount*12` 位置段，UV0 精确别名 `texcoord0Offset` 的 `vertexCount*8` 段；
5. WVS2 与 Shadow no-fallback flag 都把私有门失败处理为裁掉 primitive。CPU kernel 已跳过后，任何
   shader-side fallback 都禁止读取 native dynamic VB，因为该区间仍由 P4 poison 合同保护；
6. ABI-9、Game.dll hook 与 preflight 位布局未改。位 24/32 的历史名称仍服务 compute；同值别名
   `GpuConsumerCapabilityReady/ExactGpuWorkContract` 只澄清 B1 的 route-specific 语义，不改变 wire
   contract。

首轮 artifact
`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_first_runtime_20260720_014654`
证明了 7 次 kernel skip，但 draw-side `War3BuildExactGpuSkinDrawInput` 错把 ResolveDip 已收窄为 Main 的
consumer view 与 producer 的完整 `Main|Shadow` capability 混为一谈，导致 7 次 draw 安全 suppress；
没有 crash，截图正常，但 ledger 不满足，因此正式判 FAIL。最终修复只要求返回 view 的 mask 为非零
且是 Main/Shadow 子集；完整 mask 仍在 manager 的 kernel 前原始 capability 上做 exact 校验。

正式普通图 artifact：
`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_consumer_mask_fix_20260720_015650`，
PASS。DLL SHA-256 为
`170515583671EDE25855F5CC370A5442654CAD021B8D13D280C0526C78125380`：

- input prepared/submitted `23,359/23,359`，input bytes `66,325,296`；compute dispatch `0/0`，省略
  `23,359` 个 output/job 与 `440,855,616 B` 输出；
- Main attempt/input reject/state reject/submitted 为 `22,055/0/0/22,055`，binding clear `22,055`；
- Shadow capture attempt/input reject/state reject/commit 为 `22,055/0/0/22,055`，direct/replay
  `356/356`，no-fallback replay 与 clear 均 exact；
- native kernel original/bypassed `2,195,685/22,055`，跳过 `415,749,920 B`；P4 auth/commit、
  poison create/hit、index ticket exact 均为 `22,055`，mismatch 与 late poison 为 0；
- ledger `classified=70,077`、`resolved=consumed=44,110`，CPU fallback、suppressed、leak、unreserved、
  duplicate、planMismatch、retireDeferred 全为 0。forced snapshot 三次 clean，画面人工复核正常。

这些字节是“原生 CPU 动态 VB 写入/compute output 分配被省略”的工作量证据，不等于永久显存节省，
也不是 FPS 结论。当前白名单仍只覆盖 opaque format2 单 UV；其余约束故意保留旧路径。

lifecycle 两次运行都完成 resize/maximize/restore、map/device reset、第二进程 relaunch，且 B1 Main/
Shadow、reset generation、ledger 与清理均闭合，但不能标为 PASS，因为 runner 的独立 dip-fast sampled
硬门仍发现 reset 冷窗口被归为 `NoTransactionCover`：

- v1：early/late `2/1`；
- 第一笔重分类后的 v2：early/late `1/0`。v2 首进程 B1 `21,521` 次、reset `2/2/2`，第二进程
  B1 `8,307` 次，均零 B1 reject/ledger fault。

代码现已进一步识别“observer cookie/depth 仍完整、但 requested/applied reset generation 暂时不等”的
情况，将 sampled 结果归入 `ResetOrRetirement`，不改变 fast-path 返回值。最终源码 DLL 29,285,294
bytes，SHA-256 `1975BB891C5CA597178CBDF2E5C9D2AAAB884CA1A02A7293B7C3C14E0803E151`，build 与
no-work 通过；用户随后启动 World Editor，因此未部署、未做第三次 lifecycle。部署版仍为
`21A7BA47FB7F2C82B74C8E3D0564EAF2E700669D2FD19BA9BAE9FE6C0FA8E964`。

离线 schema 6 产物：
`AutoTest/artifacts/gpu_skin_vs_input_lease_offline_20260720_vs_b1_runtime_v1/result.json`。7 个租约生命周期
场景与 31 项静态接线检查通过，`unsafePageReuses=0`、末端 pending=0。下一硬门是部署 exact
`1975...E151` 后复跑 B1 lifecycle；之后才做高压/Special/透明与 isolated A/B。B1 outline 必须另建
input-backed 路线，不能通过放宽当前 kernel 前拒绝来“覆盖”。
