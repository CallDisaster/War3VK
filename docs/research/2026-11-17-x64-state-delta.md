# x64 固定管线 draw 的整份状态：多少是重复的、怎么只发变化（P4 状态差量 / 脏跟踪预研）

日期：2026-11-17。只读调研：没有构建，没有跑游戏、宿主、探针或 AutoTest 脚本。
标记：**已核对** = 在代码、产物日志或网页源码里亲眼看到；**推断** = 由代码和数字推出来，没有实测。
代码行号以当前工作区为准（工作区有很多别人未提交的改动，行号可能漂移）。
数据来自 5.2 A/B 第 1 轮产物 `AutoTest/artifacts/x64-phase52-ab-20261117-r1/<图>-r1-proxy/`（`game.stderr`、`host.log.2`）。
同日的兄弟笔记：[宿主 GPU 等待](2026-11-17-x64-host-gpu-waits.md)、[调度](2026-11-17-x64-scheduling.md)、[宿主路径 B](2026-11-17-x64-host-path-b.md)。

## 0. 先说结论

1. 每个固定管线 draw 发 **3,180 B**（795 个 32 位字），是一份完整的状态，和上一条没有差量关系（已核对，`tools/render_host/d3d9_ff_wire.h:5-12`）。
   三张图里它占环字节的 **58.7% / 63.2% / 49.3%**（LT / EI / 生与死，已核对，按 draws×3,180 ÷ 环字节算）。
2. 这 3,180 B 里有 **1,024 B 永远是零**：可编程着色器的 256 个浮点常量。War3 没有着色器（宿主 `shadersCompiled=0`，可编程批次 `admittedDraws=0`，已核对），
   固定管线 draw 却照样带着它们（`d3d9_proxy.cpp:1858,1906` 把整个 `draw` 拷进命令，已核对）。另外未启用的灯位按零填满 104 B/盏（`d3d9_ff_wire.h:22-25`，已核对）。
3. 剩下的状态里，真正逐 draw 变的大约只有 draw 参数（24 B）、顶点偏移、0 号纹理、世界矩阵、材质和少量混合/阶段状态。
   按分组脏跟踪估计每条 **约 90–250 B，中位估计约 140 B**（推断；分组变化率靠推理，需要 P4a 实测）。
4. 每张图能省的环字节（推断，中位估计）：LT 约 1.43 GB（环的 56%），EI 约 2.18 GB（61%），生与死约 3.73 GB（47%）。
   只在一条消息内做差量（不跨消息保存基线）也能拿到其中约九成：51% / 57% / 43%。
5. **帧时间收益不大**（推断）：宿主每 draw 大约省 1.5–3 µs、x86 每 draw 省约 0.5–1 µs，两端合计折成每帧 LT 约 0.6–1.2 ms、生与死约 2–4 ms，是帧时间的 2–5%。
   环在 LT/EI 从没满过（`fullObservations=0`），只在生与死满过（368,044 次），所以字节本身不是 LT/EI 的瓶颈。
   而且代理臂是 FIFO、进程内臂是 IMMEDIATE（已核对，见 §6.3），现在的 fps 量不出这种量级的变化。
6. P4 对 x86 内存（第一目标）几乎没有影响：基线只多几 KB；顶多让 4 MiB 环以后可以缩到 2 MiB（省 2 MiB 地址空间）。
7. 建议：**先做 P4a 测量模式（Opus，约 1 天）**，和同步调用计数放在一起做；格式交 Fable 定；P4 实施放在 P1（宿主 GPU 等待）和同步调用削减之后。

## 1. 现在每个固定管线 draw 发了什么

### 1.1 逐字段（已核对）

记录 = `DrawCommand`（`d3d9_draw_wire.h:15,20-33`）+ 固定管线状态（`d3d9_ff_wire.h:5,13-27`；结构体 `d3d9_ff_state.h:29-45`）。
下表"字"是记录内 32 位字的序号，后面 P4a 按这个范围分组。

| 字 | 内容 | 字节 | War3 里的情况 |
|---|---|---:|---|
| 0–11 | 6 个资源引用 (id, generation)：VB、IB、0 号纹理、VS、PS、声明 | 48 | VS/PS 恒为 {0,0}（已核对：无着色器） |
| 12–13 | streamOffset、stride | 8 | |
| 14–19 | 视口 X、Y、W、H、MinZ、MaxZ | 24 | |
| 20–28 | zEnable、zWrite、zFunc、cull、alphaBlend、srcBlend、destBlend、blendOp、colorWriteMask | 36 | |
| 29–34 | baseVertexIndex、minVertexIndex、numVertices、startIndex、primitiveCount、primitiveType | 24 | 每条都变 |
| 35–162 | VS 浮点常量 32×4 | 512 | 固定管线 draw 宿主不读（只做 isfinite），恒零 |
| 163–290 | PS 浮点常量 32×4 | 512 | 同上 |
| 291–306 | WORLD | 64 | |
| 307–338 | VIEW、PROJECTION | 128 | |
| 339–466 | TEXTURE0–7 变换 | 512 | |
| 467–480 | 14 个固定管线渲染状态（LIGHTING…CLIPPLANEENABLE，`d3d9_ff_state.h:13-17`） | 56 | |
| 481–568 | 8 个纹理阶段 × 11 项（`d3d9_ff_state.h:18-21`） | 352 | |
| 569–585 | 材质 17 个浮点 | 68 | |
| 586 | 启用灯数 | 4 | |
| 587–794 | 8 盏灯 × 26 字（类型 + 3 色 ×4 + 位置 3 + 方向 3 + 7 个标量） | 832 | 未启用的灯位填零，但照样发 |
| | **合计** | **3,180** | `FixedStateWords=504`，`DrawWords=291` |

不在这条记录里、已经"变了才发"的三类状态（已核对）：
- `SetEffects` 80 B：alpha test/ref、雾 7 项、scissor 开关与矩形、分离 alpha 混合、depth bias（`d3d9_effects_wire.h:9-15`）。
- `SetSamplerState`（0 号采样器）56 B（`d3d9_sampler_wire.h:6`）。
- `SetTextureStages`（1–7 号纹理引用 + 采样器）448 B（`d3d9_texture_stages_wire.h:11-12`）。

前端在 `Device::submit` 里比较"当前 vs 上次已发"，变了才发（`d3d9_proxy.cpp:558-573`、`1757-1772`），宿主把它们存成会话状态（`d3d9_host_main.cpp:512-529`，`d3d9_backend.cpp:1448-1463`）。
也就是说**跨消息的持久宿主状态已经有先例**，Reset 时前端会把默认值重发一遍（`d3d9_proxy.cpp:1457-1466`）。
代价是：这几条是独立的有序消息，每发一次都先冲刷已暂存的 draw 批次（`d3d9_connection.h:446-447`）。

FVF：`SetFVF` 映射到缓存的声明，只发声明引用（`d3d9_proxy.cpp:1705-1732`）。裁剪平面和顶点混合不支持（必须为 0，`d3d9_ff_state.h:53-54`）。

### 1.2 可编程 draw

`DrawPacket` 1,164 B = 上表字 0–290（同样 1,024 B 常量）（`d3d9_draw_wire.h:15`，已核对）。War3 在三张图里一条也没有
（`draw-batch-proxy` 的 `admittedDraws=0`，`game.stderr:34`；宿主 `shadersCompiled=0`，`host.log.2:22`；已核对）。数据层自己的闪电带走 `DrawPrimitiveUP`，也是固定管线（`d3d9_proxy.cpp:1863-1889`）。
所以 P4 只需要管固定管线；可编程 draw 以后要做时照 Remix 把常量收窄到变化的寄存器段即可（§4）。

### 1.3 实测流量（已核对，r1 代理臂）

| | Lost Temple | Echo Isles | 生与死 |
|---|---:|---:|---:|
| 固定管线 draw | 471,178 | 715,900 | 1,226,824 |
| 其中状态字节（×3,180） | 1,498 MB | 2,277 MB | 3,901 MB |
| 环字节 | 2,554 MB | 3,603 MB | 7,907 MB |
| 状态占环 | **58.7%** | **63.2%** | **49.3%** |
| `DrawFixedBatch` 消息数 / 平均每批 | 126,399 / **3.73** | 150,597 / **4.75** | 318,644 / **3.85** |
| 有序 setter 消息（Effects/Sampler/Stages）/ 每 draw | 99,829 / 0.21 | 107,902 / 0.15 | 400,829 / 0.33 |
| 同步记录 | 195,650 | 213,602 | 181,753 |
| 环满次数 `fullObservations` / 背压调用 | 0 / 0 | 0 / 0 | **368,044 / 38,118** |
| 帧数（简报）/ 每帧 draw | ≈1,600 / 294 | ≈1,770 / 404 | ≈1,240 / 989 |

来源：`game.stderr` 的 `ordered-async`、`fixed-batch-proxy`、`command-ring-proxy`、`async-clear` 行（LT 为第 35–39、33 行）；`host.log.2` 的 `warvk-d3d9-host/v1`、`fixed-batch-host` 行。
LT 的环记录数可以完全对上：126,399 批 + 696,914 上传 + 99,829 setter + 43,448 注解 + 2,119 清屏 + 195,650 同步 = 1,164,359（已核对）。

两个要点：
- 简报说"每批最多 20 条"是对的（`d3d9_backend.h:53`），但**实际平均每批不到 5 条**：任何 setter 变化、非 NOOVERWRITE 上传、同步调用、清屏都会先冲刷批次。
- 每条记录额外 144 B 头（16 B 环记录头 + 128 B 消息头，`d3d9_command_ring.h:22,34-36`、`d3d9_wire.h:12`）。

## 2. War3 里相邻 draw 之间哪些字段在变

War3 没有逐 draw 的状态统计，下面是推理（推断），依据有四条：
- 2026-10-23 原生 census（地图阶段未证明，大概是菜单场景）：到第 1,024 次 Present 共 24,648 次 DrawIndexedPrimitive，
  SetTransform 108,618 次（每 draw 4.4 次）、SetFVF/SetStreamSource/SetIndices 每 draw 各 1 次、SetRenderState 2.25 次、SetSamplerState 0.74 次、SetTexture 0.67 次、SetMaterial 0.34 次，
  SetTextureStageState 只有 12 次、SetLight 24 次、LightEnable 16 次（`AutoTest/artifacts/native-game-census-20261023-r2/receipt.json`，已核对）。
  War3 每个 draw 都重设变换，**调用次数说明不了值有没有变**，所以差量必须按值比较。
- 进图后的有序 setter 消息每 draw 0.15–0.33 条（§1.3，已核对）：效果/采样器/1–7 号阶段这一级的变化率就是这个量级。
- 投射体注解：LT 471,178 个 draw 里 298,964 个是模型投射体（63%），EI 70%，生与死 28%（`host.log.2` 的 `shadow-host` 行，已核对）。模型 draw 每个 geoset/layer 一条，世界矩阵按模型实例变。
- War3 的渲染队列先排序再分发，分发时自己算 `stateChanged`/`layerChanged`（`docs/research/war3_render_issues/20_renderqueue_dispatch_layer_reverse/README.md:254-288`，已核对文档）；所以相邻 draw 常常共享材质级状态（推断）。

| 组 | 字节 | 相邻 draw 变化率 低 / 中 / 高（推断） | 理由 |
|---|---:|---|---|
| draw 参数 | 24 | 1 | 每条都变 |
| VB 引用 + offset + stride | 16 | 0.5 / 0.9 / 1 | 游戏只建了 3 个缓冲（`buffersCreated=3`），大动态 VB 追加写，偏移多半每条变 |
| IB 引用 | 8 | 0.02 | |
| 0 号纹理 | 8 | 0.3 / 0.5 / 0.8 | 每个 layer 一张 |
| 声明 | 8 | 0.05 | 整局只有 2 个声明（`declarationsCreated=2`，`fvf-proxy cached=2`，已核对） |
| 视口 | 24 | 0.01 | 按通道（主画面/小地图/头像） |
| z/cull/混合 9 项 | 36 | 0.15 / 0.25 / 0.5 | 按 layer 过滤模式 |
| WORLD | 64 | 0.3 / 0.45 / 0.8 | 按模型实例；地形/UI 为单位阵 |
| VIEW + PROJ | 128 | 0.005 / 0.01 / 0.05 | 按通道 |
| TEXTURE0 变换 | 64 | 0.02 / 0.1 / 0.3 | UV 动画 |
| TEXTURE1–7 变换 | 448 | ≈0 | |
| 14 个固定管线渲染状态 | 56 | 0.03 / 0.08 / 0.2 | LIGHTING 在世界/UI 间切换，AMBIENT 随昼夜慢变 |
| 0–1 号阶段 | 88 | 0.08 / 0.15 / 0.35 | |
| 2–7 号阶段 | 264 | ≈0 | |
| 材质 | 68 | 0.2 / 0.35 / 0.7 | geoset 透明度/颜色动画 |
| 灯（数量 + 实际启用的约 2 盏） | 212 | 0.005 / 0.01 / 0.05 | 每帧或每通道 |
| VS/PS 常量 | 1,024 | 0 | 无着色器 |
| 未启用灯位的零填充 | ~624 | 0 | |

数据层的世界光照改写（`WorldLightingScope`，`d3d9_proxy.cpp:579,600-634`）对世界场景内每个 draw 写同样的值，只在世界/UI 切换时让灯 0、材质环境光、AMBIENT 变一次（已核对代码，变化率是推断）。

估算（脚本 `D:\tmp\AppData\ADMINI~1\Local\Temp\claude\a66d1abe2490ff62\0261171f-6707-49c0-8309-6ba7b932d19e\scratchpad\research\statedelta\model.py`，推断）：

| 方案 | 每 draw 字节 | 相对 3,180 B |
|---|---:|---:|
| 现状 | 3,180 | — |
| P4-0 只去掉恒零部分（常量、VS/PS 引用、空灯位），不做差量 | ≈1,516 | −52% |
| 分组差量（持久基线），低 / 中 / 高 | 90 / 137 / 247 | −92% ~ −97% |
| 每组每 draw 都变的最坏情况 | ≈1,516 | −52% |

## 3. 宿主侧：每个 draw 现在全量重做了什么

一条 `DrawFixedBatch` 在宿主上的路径（全部已核对）：

1. **解码 + 校验**：`DecodeFixedDraw` 逐字解 795 字，`ValidFixedFunction` 检查 176 个矩阵浮点、材质、灯、88 个阶段值（`d3d9_ff_wire.h:28-46`，`d3d9_ff_state.h:91-99`）。
2. **解析资源**：6 个引用 + 7 个附加纹理按 (id, generation) 查表（`d3d9_host_main.cpp:248-267`）。
3. **预检 `prepareDraw`**（`d3d9_backend.cpp:1580-1739`）：重建输入布局（声明 × ISGN 双循环）、逐个索引检查（1667-1675，O(索引数)，与状态无关）、
   光栅/深度/混合对象、对 256 个常量做 isfinite（1726-1729）、**第二次** `ValidFixedFunction`（1732），再构造 `FixedKeyView`（两个 1 KiB 数组清零填充，`d3d9_fixed_state.h:33-46`）并重建 VS/PS 固定管线键（1733-1736）。
   前端暂存前还做过一次 `ValidFixedFunction`（`d3d9_proxy.cpp:582`），所以同一份状态一共校验三次。
4. **录制 `recordDraw`**（`d3d9_backend.cpp:1741-1887`），流模式下每条：
   - 取新的环切片，`FillFixedVertex` 写约 1.8 KB 的 VS UBO：两次 4×4 求逆、一次乘法、最多 8 盏灯变换到视空间（`d3d9_fixed_state.h:50-66`，`d3d9_fixed_function_keys.h:113-122`），再写 PS UBO（1760-1775）。
     原生 DXVK 只在 `FFVertexData`/`FFPixelData` 脏时才重算（`src/d3d9/d3d9_device.cpp:39684,39781`）。
   - 每条绑 4 个 UBO（`bindUniformBuffer` 无条件标脏描述符，`src/dxvk/dxvk_context.h:222-229`），`pushData`，规格常量比较（1782-1805）。
   - 每条 `bindShader` ×2：无条件置 `GpDirtyPipeline|GpDirtyPipelineState`（`dxvk_context.h:312-351`），所以 DXVK 每个 draw 都重新查一次管线。
   - 8 个纹理槽的采样器/图像视图循环（1830-1845）：DXVK 对同一对象会跳过（`dxvk_context.h:254-264,294-303`），这一项本来就便宜。
   - VB/IB 每条重绑并标脏（`dxvk_context.h:185-192,360-367`）；输入布局、光栅、深度、混合是否重设由 `BatchStateChanges` 决定（`d3d9_batch_state.h:13-22`），
     但它只在 `WARVK_D3D9_BATCH_STATE` 打开时生效（`d3d9_backend.cpp:1040-1042,1821`），并且只和**同一批的上一条**比（1927）。游戏这轮没开：`batchStateReuse=0`、`redundantStateBindingsSkipped=0`（`host.log.2` 的 `draw-batch-host` 行）。

2026-10-29 合成固定批（1,025 draw/帧，每条都改 WORLD 和 TFACTOR）实测的每 draw 宿主时间：解码 1.2 µs、**固定管线常量 1.8 µs**、DXVK draw 0.96 µs、预检（含键）0.42 µs、绑定 0.3 µs，合计约 4.8 µs
（[固定批回执](2026-10-29-x64-fixed-function-batch.md) §"固定批剩余时间"；那轮环境漂移 1.4–1.7 倍，只能看相对大小）。

有了差量之后能省的宿主 CPU（推断）：

| 项 | 现在 | P4 之后 | 每 draw 省 |
|---|---:|---|---:|
| 解码 + 校验 | 1.2 µs | 只解析变化的组、只校验变化的组（基线已校验过） | 0.6–0.9 µs |
| 固定管线常量 | 1.8 µs | VS 输入没变就复用上一个 UBO 切片（不取新切片、不重绑）；只有 WORLD 变时重算 WorldView/NormalMatrix，InverseView 与灯变换缓存到 VIEW/灯变化 | 0.6–1.3 µs |
| 预检里的键与重复校验 | 0.42 µs | 键输入没变就不重建 `FixedKeyView`/键；去掉第二次 `ValidFixedFunction` 和常量 isfinite | 0.1–0.25 µs |
| 绑定 + DXVK draw | 1.3 µs | 管线输入没变就不调 `bindShader`/布局/光栅等，DXVK 不再逐条查管线 | 0.2–0.5 µs |
| **合计** | ≈4.8 µs | | **≈1.5–3 µs** |

注意第 2–4 行（宿主复用）**不依赖线格式**：宿主手里有上一条解码后的完整状态，自己比一次就知道哪些组变了。线上差量的作用是省字节、省解码，并且把"哪些组变了"免费交给宿主。

## 4. RTX Remix bridge 的做法和踩过的坑

来源：github.com/NVIDIAGameWorks/dxvk-remix（网页源码，已核对内容；main 分支行号，可能漂移）。

- **提交 8bbf141**（"Bridge: cut per-frame IPC cost of the D3D9 bridge"）：加了 `client.dropRedundantState` 与 `client.batchState`（默认都开）、异步查询、先自旋再阻塞的队列；
  HL2 RTX 每帧命令 28,000 → 4,000，帧时间 14–15 → 8–8.6 ms（提交说明）。说明里强调新缓存跟踪的是"**客户端已发出的值**"，替换掉旧的、按游戏可见状态比较的 `eliminateRedundantSetterCalls`。
- **`SentState`**（`bridge/src/client/d3d9_sent_state.h:35-264`）：
  - 每个值带一个代号，`update` 用 memcmp 比较值且代号相同才算重复（43-50）；`invalidateAll` 只把代号加一（99-103），O(1) 失效。
  - 常量按寄存器比较，把 `start/count` 收窄到"第一个变化的寄存器到最后一个变化的寄存器"（61-88）。
  - 带驱动 hack 的渲染状态（POINTSIZE 的 RESZ/ATOC/NVDB、自适应细分）永远发送（118-125）。
  - 单独失效：视口+scissor（105-108）、流（110-112）、索引（114-116）；`SetFVF` 和 `SetVertexDeclaration` 互相使对方失效（188-197）。
  - 注释写明"对象 id 不复用，所以 id 相同就是同一对象"（34）。
  - `d3d9_device.cpp` 里 `SetLight`/`LightEnable` 永远发送（理由是它们的运行时会让游戏灯"老化"，与我们无关）；`Reset`/`ResetEx` 调 `invalidateSentState()`（行号未核对）。
- **`batchState`**：`StateMessage` 把 setter 追加成当前 `Bridge_StatePacket` 的一条记录，**下一个非 setter 命令构造时才压包头**，所以服务端先按序应用完 setter 再执行那条命令
  （`bridge/src/util/util_bridgecommand.h:323-329`）；上限 `kMaxPacketWords = 64*1024`、`kMaxPacketRecords = 4096`（同文件 350-351）；
  允许进包的只有 24 个 `Set*` setter（`bridge/src/util/util_commands.h:898-930` 的 `IsBatchableState`），其他命令进包服务端拒绝。
- **提交 3f75d64**（"Bridge: preserve D3D9 synchronization and bound shared upload memory"）修掉的与状态缓存有关的坑（据提交页可见的 diff，已核对；行号是 hunk 里的新行号）：
  1. `DrawPrimitiveUP` 之后流 0 被服务端改掉，但缓存还以为是旧值 → 加 `invalidateStreamSource(0)`（`d3d9_device.cpp` 新 2408 行）。
  2. `DrawIndexedPrimitiveUP` 同理，流 0 和索引都要失效（新 2433-2434 行）。
  3. `SetRenderTarget(0)` 会重置视口/scissor，缓存失效与更新不在同一把锁里 → 挪到同一锁内调用 `invalidateViewportAndScissor()`（新 1021 行）。
  4. 其余是异步查询、回复负载寿命、共享堆上传、Present 信用与设备销毁确认，与状态差量无关。新增了 `test/rtx/unit/test_sent_state.cpp`、`test_sent_state_device.cpp`。

对我们的启示：
- 坑的共同点是"**服务端在没有 setter 的情况下改了状态**"。他们的缓存挂在 setter 上，所以每个这种地方都要手动失效。
  我们前端本来就持有完整状态，**在编码好的记录上做快照差分**（不是在 setter 上记脏）就天然免疫这一类：`DrawPrimitiveUP` 换绑/解绑 VB（`d3d9_proxy.cpp:1885-1887`）、
  窗口 resize 改视口/scissor（1496）、StateBlock `apply`、世界光照改写（579）都会自然出现在差分里。
- "跟踪已发出的值而不是游戏可见值"对我们特别要紧：世界光照是**编码时临时改写、编码后还原**的（`d3d9_proxy.cpp:600-634`），差分必须在改写后的记录上做。
- 我们的资源 id **会复用**（换 generation，`d3d9_wire.h:9`），所以引用必须按 (id, generation) 整对比较；而且宿主每条仍要重新解析引用（中间可能有 Destroy、纹理上传），"引用没变"不等于"可以不解析"。
- 宿主侧如果做 DXVK 绑定的脏跟踪（§3 的第 4 行），"服务端自己改状态"的地方在宿主：BeforeUi 特效与阴影 pass、清屏、Present、新命令列表、Reset、销毁后槽位复用。这些点都必须让"上次已绑定"失效；比较时用对象指针，不用槽号。
- Remix 的大头收益来自命令数（每条命令的固定开销）和异步查询。我们的记录数（LT 每帧约 730 条）主要是上传和同步记录，P4 只能减掉 setter 那一部分（每 draw 0.15–0.33 条）。

## 5. P4 方案

### 5a. 先做的测量模式 P4a（Opus）

目的：把 §2 的推断换成实测，再让 Fable 定格式。默认关闭，环境变量 `WARVK_D3D9_STATE_CENSUS=1`（前端）和 `WARVK_D3D9_HOST_OP_PROFILE=1`（宿主）。关闭时只有一个分支判断。
不能复用 `WARVK_D3D9_ADMISSION_PROFILE`：环模式下前端直接拒绝它（`d3d9_connection.h:84-85`），而且它的样本按 EVENT Issue 切，游戏里 `queryIssues=0`（已核对）。

前端插入点：
1. `d3d9_proxy.cpp:583`：把 `d9::EncodeFixedDraw(command,fixed)` 先存成局部 `packet`，开着时 `census.observe(packet)`，再交给 `submitFixedDraw`。
   此处已在世界光照改写之后（579），比较的正是"已发出的值"。逻辑放进新头文件（如 `tools/render_host/d3d9_state_census.h`），纯函数，便于 CPU 测试。
2. `census.observe` 按 §1.1 的字范围分组，和**上一条已发记录**（跨批次保存）逐字比较，累计：
   - 每组变化次数、变化字数、变化字节；
   - 各候选格式的模拟字节数：现状、P4-0、消息内差量（需要知道批边界，见第 5 条）、分组持久差量（B）、按字位图持久差量（E）——用真实编码器模拟，不靠估算；
   - 每组"新值等于最近 8 个旧值之一"的次数（判断状态块驻留表（方案 D）是否比"对上一条差分"更好）；
   - 每帧不同状态块数：对去掉 draw 参数（字 29–34）和 streamOffset（字 12）的记录求 64 位哈希，放进 4,096 槽的开放寻址表；另算一份再去掉 WORLD 的。在 Present（`d3d9_proxy.cpp:371`）记下本帧数量并清表，输出 p50/p90/最大值。
3. 失效点：`Device::Reset` 的 `resetState`（`d3d9_proxy.cpp:1438,1456`）和 `resizeToWindow`（1477）调用 `census.invalidate()`，与将来真实格式的基线失效点一致。
4. 有序 setter 按种类分开计数：`submit` 中 SetEffects（560-565）、SetSamplerState（568-573）、`syncTextureStages`（1767-1771）。
5. `d3d9_connection.h`：只在有暂存 draw 时，按原因统计冲刷次数和被冲刷的 draw 数：换种类（403、424）、批满（410、429）、有序消息按 op（447）、EndScene（575）、清屏（585）、Poll（611）、
   `call()` 按 op（657）、注解（976）、内容上传/释放（527、544）；再加每批条数直方图（1、2–4、5–9、10–19、20）。这份数据同时回答"把 setter 并进记录能让批变多大"和同步调用削减（②b）的问题。
6. 输出：`Device::~Device` 里 `fvf-proxy` 行旁边（`d3d9_proxy.cpp:1357`）打一行 `warvk-d3d9-state-census/v1`；`Connection::~Connection` 的 `fixed-batch-proxy` 行旁边（`d3d9_connection.h:284-288`）打 `warvk-d3d9-flush-reasons/v1`。

宿主插入点：
7. `d3d9_host_main.cpp:554-558` 解码循环之后：把每条解码结果和会话里保存的上一条（跨消息）比较，计数"VS 常量输入相同"（变换、灯、材质、AMBIENT、视口、zEnable、VS 键）、
   "PS 输入相同"（阶段、TFACTOR、纹理掩码）、"键相同"、"管线输入相同"（声明、9 项渲染状态、图元类型、键、效果规格常量）、"纹理/采样器相同"、"VB/IB 相同"。这直接给出 §3 宿主复用的上限。
8. `d3d9_host_main.cpp:922-967`：按 op 类别（DrawFixedBatch、UploadBuffer、三种 setter、Present、StageMarker、其他）累计 apply 时间和次数，退出时在 `fixed-batch-host` 行旁边（1059-1062）打印累计值，不切样本。
9. `d3d9_backend.cpp` 现有的 `admission*Ns` 计数（prepare/record/constant/bind/sampler/draw）改为也可由第 8 条的开关打开，输出累计值。

运行：三张图各跑一轮即可（机器上还有别的重负载，只看计数和每 draw 微秒，不看 fps）。读方脚本和一个纯测试（字段集合、计数恒等式：各组变化次数 ≤ draws、模拟字节 ≥ 每条最小头）。工作量约 1 天。

### 5b. 线格式选项（交 Fable 决定）

| 方案 | 做法 | 每 draw 字节（推断，中位） | 占环节省 LT / EI / 生与死 | 优点 | 风险/代价 |
|---|---|---:|---|---|---|
| P4-0 紧凑全量 | 新 op，固定管线记录不带常量和 VS/PS 引用，灯只发 `[0,count)` | ≈1,516 | 31% / 33% / 26% | 无持久状态，每条自包含；改动最小 | 只拿到一半 |
| A 消息内差量 | 每个 `DrawFixedBatch` 第一条完整（或对零基线做 E 式位图），后面对上一条差分 | 331–391（第一条约 1,150） | 51% / 57% / 43% | 不跨消息，单条消息可单独解码/重放；拒绝语义不变 | 受每批条数限制（现在 3.7–4.8 条）；把 setter 并进记录后会变好 |
| B 分组掩码 + 持久基线 | 每条 = 32 位组掩码 + draw 参数 + 变化的组；阶段/灯/纹理变换再带子掩码 | 137 | 56% / 60% / 47% | 字节最少；宿主直接拿到"哪组变了" | 依赖有序无损传输和严格的重置纪律；基线出错会一直错到下次重置 |
| C setter 流（Remix 式） | 发 SetTransform/SetMaterial… 记录，宿主维护完整 D3D9 状态机，前端去重 | 视调用而定 | — | 最贴近 D3D9 和 Remix 已验证的路线 | 宿主要新写一个 D3D9 状态机；War3 每 draw 4.4 次 SetTransform，必须去重；与现有"每条完整状态再校验"的模型不合；改动最大 |
| D 状态块驻留 | 前端对状态组求哈希，新块先发 `DefineBlock(id,bytes)`，draw 只带块 id；宿主有界表 | 取决于 A/B/A 交替率 | — | 交替出现的材质（地形层 ↔ 单位）能复用；宿主可按块缓存派生结果（键、灯变换） | 表管理、淘汰协议、容量；x86 侧要一张表（几百 KB，碰到第一目标） |
| E 字位图差量 | 不按语义分组，直接对 795 字记录做差：25 位块掩码 + 每个脏块 32 位字掩码 + 变化的字 | 113 | 57% / 61% / 48% | 格式与记录布局解耦；宿主先还原完整 3,180 B 再走**原封不动**的 `DecodeFixedDraw`，最容易证明逐字节相同；恒零区域自动不发 | 宿主省不了解码/校验（除非之后按块跳过）；块掩码到组的映射要另外维护 |

（A、B、E 的数字来自 §2 的变化率模型，脚本同上；E 的第一条约 1,150 B 是按典型记录约 263 个非零字估的。都是推断，P4a 会给出实测值。）

要 Fable 拍板的问题：
1. **基线范围**：消息内（A）、会话持久（B/E）、还是驻留表（D）？A 能拿到持久方案约九成的字节收益，而且没有跨消息状态。持久方案多出的价值主要是字节；宿主复用（§3）两种都能做。
2. **粒度**：语义分组（B）还是字位图（E）？两者字节差不多，区别在宿主是否保留"还原完整记录 → 原解码"这条单一路径。
3. **要不要把 Effects/Sampler/TextureStages 并进 draw 记录**作为差量组：这样它们不再冲刷批次（LT 每 draw 0.21 次冲刷来源），批会变大。
   代价是同一份宿主状态有两个写入口（单独的 op 仍要给可编程 draw 和 Reset 用），前端的 `sentEffects`/`sentSamplerState`/`sentTextureStages`（`d3d9_proxy.cpp:553-555`）必须是同一份。
4. **重置点**（持久方案必须回答）：
   - 会话开始（Initialize/AttachRing）：基线为空，第一条必须全量。
   - D3D9 `Reset` 和窗口 resize（两者都发 `Op::Reset`）：两端在**收到/发出 Reset 消息时**就失效，不看结果，这样宿主拒绝 Reset 时两端也一致。
   - `SessionReset`（换图）：D3D 状态不变，不是必须；作为防御性重同步代价很小。
   - 设备丢失/终止：会话结束，不需要。
   - BeginScene/Present：不需要；可选每帧一条关键帧（每帧多约 1 KB），只在怀疑漂移时有用。
5. **同步模式下被拒绝的 draw**（不开固定批时，`DrawFixed` 的 INVALIDCALL 可恢复）：建议定为"先把差量应用到基线、再校验 draw"，两端都已更新，状态一致；
   **差量本身解码失败**时宿主整条消息不提交到基线（原子），并按异步规则算终止。
6. **校验模式与协商**：`Initialize` 的 `args[6]` 加一位（如 `InitializeStateDelta=4`，现有位见 `d3d9_wire.h:55`），旧宿主拒绝新 op；
   另设 `WARVK_D3D9_STATE_DELTA_VERIFY=1`：每条附 8 B 完整记录哈希，宿主还原后比对，不一致即终止（细节 "delta-mismatch"）。
7. **批上限**：记录变小后 20 条的上限（`d3d9_backend.h:53`）可以放宽到 64–128；宿主会话的解码数组（`d3d9_host_main.cpp:124-125`）随之变大（128 条约 0.4 MB，x64 侧，可以接受）。

我的倾向（仅供参考，不是决定）：先上 **P4-0 + A（消息内，第一条用零基线位图）**，因为它没有跨消息状态、拒绝和重放语义完全不变、收益已有持久方案的九成；
同时把 setter 并进记录让批变大；宿主复用（§3）独立做。如果 P4a 实测显示批仍然很短、或交替率很高，再上 B/E 持久或 D。

### 5c. 格式定了之后的实施步骤（Opus）

1. 新增 `tools/render_host/d3d9_ff_delta_wire.h`：编码/解码与"还原成完整 `DrawCommand`+`FixedFunctionState`"；新 op 追加在 `Op` 枚举末尾（`d3d9_wire.h:28-35`），`ValidOp` 上界随之扩展（153），与 `DrawFixedBatch` 当初的做法相同。
2. 协商位与开关（默认关）；前端未协商成功时退回现有 `DrawFixedBatch`。
3. 前端：`Connection` 持有基线（或批内上一条），在 `submitFixedDraw`（`d3d9_connection.h:415-430`）暂存时编码；`publishStaged`（983-1009）发新 op；在 §5b 第 4 条的点失效。
4. 宿主：`Session` 持有基线；新 op 的处理先把整条消息还原到 `fixedBatchStates`/`fixedBatchFixed`（`d3d9_host_main.cpp:124-125,549-567`），再调用**同一个** `drawFixedBatch`；`Reset` 处理（745-765）失效基线。
   第一版宿主仍对每条完整状态跑原有校验与 `prepareDraw`，不做任何跳过。
5. （可选）把 Effects/Sampler/TextureStages 作为差量组并入；前端 `submit` 不再为它们单独发有序消息（Reset 重发默认值的路径保留）。
6. 测试与门（§5d），全部通过后才做宿主复用。
7. 宿主复用（P4c，单独一批）：VS/PS UBO 切片复用、键缓存、去掉重复校验、DXVK 绑定脏跟踪扩展到整个流命令列表（在 §4 列出的宿主自改状态点全部失效）。这一批建议由 Fable 先审失效点清单，Opus 实现。

### 5d. 不变量与测试

不变量：
- **逐字节等价**：每个 draw，宿主还原出的完整记录与 `EncodeFixedDraw(command,fixed)` 逐字节相同。
- **校验不变**：宿主仍对还原后的完整状态做 `ValidFixedFunction`、`resolveDraw`、全批原子 `prepareDraw`；延迟拒绝仍是终止性的。
- **基线纪律**：两端在相同的有序点失效；前端在暂存时更新、宿主在解码时更新；差量解码失败不改基线。
- **差分对象是已发出的记录**：在世界光照改写之后的编码结果上比较，绝不在 setter 上记脏。
- **资源身份**：引用按 (id, generation) 比较，宿主每条仍重新解析。
- **顺序**：不在 draw 之间插入新的消息类型；注解按批内 draw 序号绑定的规则不变（`d3d9_connection.h:961-971`）。
- **x86 内存**：基线和计数只多几 KB；census 表只在开关打开时分配。

测试：
- `AutoTest/test_render_host_d3d9_wire.cpp`：新 op 编解码；随机状态序列（含 Reset、resize、`DrawPrimitiveUP`、StateBlock apply、世界光照开关）逐条比较"差量还原"与"完整编码"。
- `AutoTest/test_d3d9_connection_async_cpu.cpp`：仿 `bounded-fixed-batch` 新增用例（2 可编程 → 23 固定管线 → 1 可编程，中间插 Reset），远端收到的还原字节逐条正确。
- `AutoTest/test_render_host_fixed_rejection.cpp`：非法差量（未知掩码位、没有基线就发差量、截断、尾部多余字节、差量组里出现 NaN、灯数 > 8、未用灯位非零）都被拒绝，基线不变，之后的合法差量还原正确。
- GPU 门：`run_d3d9_draw_batch_gate.py --fixed-function`、`run_d3d9_factory_clear_gate.py --fixed-batch`、多纹理/StateBlock 门、`run_x64_shadow_gate.py`、`run_x64_scene_effects_gate.py`，
  开/关差量各一轮，帧哈希必须与 P4 之前的候选**完全相同**（不是"相近"）；全部门至少跑一轮校验模式。
- 5.2 A/B 门：代理臂 ab1/ab2 帧与 P4 前逐字节相同；比较用计数和每 draw 微秒，不用 fps（§6.3）。

## 6. 每张图能省多少

### 6.1 环字节（推断，中位估计）

| | Lost Temple | Echo Isles | 生与死 |
|---|---:|---:|---:|
| 现在固定管线状态字节 | 1,498 MB | 2,277 MB | 3,901 MB |
| P4-0 紧凑全量后省 | 784 MB（31%） | 1,191 MB（33%） | 2,041 MB（26%） |
| A 消息内差量后省 | 1,314 MB（51%） | 2,039 MB（57%） | 3,432 MB（43%） |
| B/E 持久差量后省 | 1,434–1,445 MB（56–57%） | 2,178–2,196 MB（60–61%） | 3,733–3,763 MB（47–48%） |
| B 的低/高变化率区间 | 57% / 54% | 61% / 58% | 48% / 46% |
| 每帧环字节：现在 → B 之后 | 1.60 → 0.70 MB | 2.04 → 0.80 MB | 6.38 → 3.37 MB |

括号里是占整条环字节的比例。之后环里剩下的大头是顶点上传（829 MB / 1.01 GB / 3.43 GB），不属于 P4。

### 6.2 CPU（推断）

| | Lost Temple | Echo Isles | 生与死 |
|---|---:|---:|---:|
| 每帧 draw | 294 | 404 | 989 |
| 宿主每帧省（1.5–3 µs/draw） | 0.4–0.9 ms | 0.6–1.2 ms | 1.5–3.0 ms |
| x86 游戏线程每帧省（编码 3,180 B、三次拷贝、本地校验 → 比较 + 编码约 140 B；0.5–1 µs/draw） | 0.15–0.3 ms | 0.2–0.4 ms | 0.5–1.0 ms |
| 两项合计占代理帧时间（34 / 33 / 102 ms） | 约 2–4% | 约 2–5% | 约 2–4% |

- 生与死的环会满（§1.3），说明那里宿主消费跟不上；字节减半能减少游戏线程撞满环的次数，但如果宿主是瓶颈，等待只是换了地方，真正有用的是宿主每 draw 的时间。
- [调度笔记](2026-11-17-x64-scheduling.md) 观察到生与死同一宿主会话里有 228–365 fps 的片段，传输加 Present 的固定开销上限约每帧 3 ms；LT/EI 差的二十多毫秒主要在宿主执行和 GPU 排空（那份笔记的推断）。所以 P4 不是帧时间的主因。

### 6.3 量不出来的风险：两臂 present mode 不同（已核对，旁注）

代理臂宿主 `VK_PRESENT_MODE_FIFO_KHR`（`losttemple-r1-proxy/logs/host_warvk-d3d9-host.log:282`，EI/生与死同），`syncInterval=1`（`host.log.2:11`）；
进程内臂 `VK_PRESENT_MODE_IMMEDIATE_KHR`（`losttemple-r1-inprocess/logs/War3_d3d9.log:305`）。代理前端把游戏的 DEFAULT 折成 ONE（`d3d9_proxy.cpp:174`，游戏传 `interval=0x0`，`game.stderr:24`）。
[宿主 GPU 等待笔记](2026-11-17-x64-host-gpu-waits.md) §1 第 5 条已经详细记了这件事和原因。对 P4 的影响：每帧省 1 ms 量级在垂直同步量化下很可能完全看不出来（推断），
所以 P4 的验收不能用 fps，只能用环字节、记录数和宿主/前端每 draw 微秒；或者等那份笔记的 S1（同条件 present）先做完。

### 6.4 相关但不在 P4 的冗余（顺手记下）

- 投射体注解 `ShadowCaster` 每条带一份世界矩阵（`src/d3d9/war3/effects/war3_shadow_scene.h:328-346`），与同一 draw 的 WORLD 重复；LT 注解约 45 MB（环的 1.8%，推断）。
- 索引校验前端（`d3d9_proxy.cpp:1901-1905`）和宿主（`d3d9_backend.cpp:1667-1675`）各做一遍，O(索引数)，与状态无关，但也是宿主每 draw 时间。

## 给计划的结论

| 步骤 | 内容 | 谁 | 工作量 | 依赖 |
|---|---|---|---|---|
| P4a | 测量模式（§5a）：前端分组差分/格式模拟/每帧不同状态块/冲刷原因，宿主相邻 draw 相同率和按 op 计时 | **Opus** | 约 1 天（含读方与纯测试）+ 三张图各一轮 | 无；建议和同步调用计数一起做 |
| 格式决定 | 读 P4a 实测 + 本文 §5b 的七个问题 | **Fable** | 约半天 | P4a |
| P4b | 线格式、前端编码、宿主还原、CPU/raw/GPU 门（§5c 1–6） | **Opus** | 2–3 天 | 格式决定 |
| P4c | 宿主复用：UBO 切片/键缓存/去重复校验/DXVK 绑定脏跟踪 | Opus 实现，**Fable 先审失效点清单** | 2–3 天 | P4b；最好在 P1（宿主 GPU 等待）之后，否则收益被等待掩盖 |

- 优先级：P4 主要省环字节（约一半）和两端 CPU（帧时间的 2–5%），**对 x86 内存几乎没有作用**。按"内存第一、性能第二"，它排在 P1（宿主 GPU 等待）、同条件 present（S1）和同步调用削减之后。
  P4a 很便宜，可以先做，用来给后面的排序提供数据。
- 最简单、最稳的一步是 P4-0（不带恒零常量和空灯位），自己就能省环字节 26–33%，没有任何跨消息状态，可以作为 P4b 的第一个提交。

## 还不知道的

1. War3 进图后各状态组相邻 draw 的**实际**变化率（§2 全是推断）；A/B/A 交替有多常见（决定 D 值不值）。→ P4a。
2. 每批条数被谁截断：setter、非 NOOVERWRITE 上传、同步调用各占多少；把 setter 并进记录后批能长到多少。→ P4a 第 5 条。
3. 宿主在游戏负载下每 draw 的真实分项时间（现有数字来自 10-29 的合成场景，环境漂移 1.4–1.7 倍）；宿主是不是生与死的瓶颈、瓶颈在 CPU 还是 GPU 等待。→ P4a 第 8–9 条，以及 P1 笔记。
4. 生与死的帧数：简报给约 1,240 帧，宿主记了 9,647 次 Present、636 次阴影 pass；每帧 draw 数因此可能偏差很大。
5. War3 设置流偏移的方式（每条改 `SetStreamSource` 偏移，还是固定偏移改 `BaseVertexIndex`），影响"VB 组"每条是否都脏（只差约 8–16 B/draw）。
6. Remix `d3d9_device.cpp` 中 `invalidateSentState`、StateBlock `Apply` 的确切行号与实现（网页只读到部分）；`util_bridgecommand.cpp` 里关包的具体时机。
7. 同条件 present 之后代理臂的 fps 基线；没有它，P4 的帧时间收益无法用 fps 验证。
