# x64 固定功能 draw 异步批次（T2 路线 ②）

2026-10-29，Claude 在[准入段拆解](2026-10-29-x64-admission-breakdown.md)之后实施。只改开发宿主/代理与开发门，
默认关闭；不接游戏、不部署玩家 DLL、不提交/推送/发布。Game.dll guard 保持。

## 为什么做这一块

War3 主体是固定功能。代理此前对每个固定功能 draw 都发一次同步 `DrawFixed` 调用，等宿主回复后才返回；
流模式只去掉了宿主侧的 GPU 排空，没有去掉这次 IPC 往返。准入拆解已经量出一次往返的传输与唤醒
约 20 µs，而可编程 draw 是 32 个共摊一次。本批让固定功能 draw 走与可编程批次相同的有界异步准入。

## 协议与所有权

- **新 opcode `DrawFixedBatch`**，追加在枚举末尾，`ValidOp` 上界随之扩展，已有 op 编号不变。
  `args[0]` 为条数 1–20，`payload` 恰为 条数 × 3,180 B，每条是一个完整、独立的 `FixedDrawPacket`
  （draw 本体 + 该 draw 自己的全部固定功能状态），与前一条没有差量关系。20 条 63,600 B，
  受单包 65,408 B 上限约束（`static_assert`）。
- **开关**：新增默认关闭的 `WARVK_D3D9_FIXED_BATCH=1`，要求同时开 `WARVK_D3D9_BATCH_DRAWS=1`，
  是否异步跟随 `WARVK_D3D9_ASYNC_BATCH`。不开时固定功能 draw 仍是原来的同步 `DrawFixed`，
  调用方仍可从 `INVALIDCALL` 恢复。宿主始终接受新 op，不需要环境变量。
- **顺序**：代理同一时刻只暂存一种 draw。可编程与固定功能切换时先把另一种冲刷出去，
  所以远端顺序就是调用顺序；满 20 条、EndScene、以及任何其他远端调用（上传、销毁、读回、
  Query、setter 同步等）都先冲刷，与可编程批次规则相同。只读诊断在有暂存固定功能 draw 时拒绝，不隐式提交。
- **拒绝语义**：与 `DrawBatch` 相同，延迟拒绝是终止性的（DEVICELOST），不重放、不静默丢弃。
  为了让最常见的状态域拒绝仍可恢复，代理在暂存前本地执行一次 `ValidFixedFunction`
  （与宿主解码时的检查相同），失败直接返回 `INVALIDCALL`、不入队；各 setter 本来就逐值校验，
  draw 入口的资源/范围检查也照旧在代理本地完成。
- **宿主**：先把整包全部解码并解析资源（`DecodeFixedDraw` + `resolveDraw`）到会话持有的数组，
  再调用 `drawFixedBatch`；后者先对所有条目 `prepareDraw`，全部通过后才创建 FF 着色器、取环切片、
  录制或计数。任一条失败，整包不留任何已渲染前缀。
  - 流模式：整包录进当前打开的命令列表，每条取自己的 FF 常量环切片，不等 GPU。
  - 非流模式（sync/group/flight）：保持原有逐条 settle → begin → 录制 → submit(true)，
    因为那里的 FF UBO 只有一块、每条都会重写；只是 IPC 被合并。
  - 原 `drawFixed` 现在就是 1 条的 `drawFixedBatch`，单条路径行为不变；
    唯一可见差别是 `largestBatch` 现在也统计固定功能（同步 FF 记为 1）。
- **统计**：新增独立的 `warvk-d3d9-fixed-batch-proxy/v1` 与 `warvk-d3d9-fixed-batch-host/v1` 冷输出行
  （每次都输出，未使用时为 0）；冻结的 `draw-batch-proxy/v1`、`draw-batch-host/v1` 字段不变，
  前者仍只统计可编程批次。准入计时把 `DrawFixedBatch` 与 `DrawBatch` 一起计为批次消息；
  `recordDraw` 内 FF 常量填充移入常量计时块（只影响计时归属）。

## 改动文件

- `tools/render_host/d3d9_wire.h`：`DrawFixedBatch` op 与 `ValidOp` 上界。
- `tools/render_host/d3d9_ff_wire.h`：`MaxFixedDrawBatch`、`FixedDrawBatchPacket` 与单包上限断言。
- `tools/render_host/d3d9_connection.h`：开关、固定功能暂存、按种类冲刷、`publishStaged`、
  异步分类、诊断拒绝、`fixed-batch-proxy/v1` 行。
- `tools/render_host/d3d9_proxy.cpp`：FF draw 经 `submitFixedDraw`，批模式下本地 `ValidFixedFunction`。
- `tools/render_host/dxvk_core/d3d9_backend.{h,cpp}`：`MaxFixedDrawBatch=20`、`drawFixedBatch`、
  FF 常量填充计时归属。
- `tools/render_host/dxvk_core/d3d9_host_main.cpp`：`DrawFixedBatch` 解码、会话数组、
  准入分类、`fixed-batch-host/v1` 行。
- `AutoTest/test_render_host_d3d9_wire.cpp`：新 op 编解码与越界 op 拒绝。
- `AutoTest/test_d3d9_connection_async_cpu.cpp`（+ `run_d3d9_connection_async_cpu.py`、读方测试）：
  新 CPU 用例 `bounded-fixed-batch`。
- `AutoTest/test_render_host_fixed_rejection.cpp`：raw 固定功能批次 12 个原子拒绝/恢复用例。
- `AutoTest/test_render_host_d3d9_batch.cpp`：`-DWARVK_BATCH_TEST_FIXED=1` 固定功能工作负载
  （WORLD 变换复现 SM3 的 shape 常量，TEXTUREFACTOR × 纹理复现 tint，像素判据不变）。
- `AutoTest/run_d3d9_factory_clear_gate.py`：`--fixed-batch`。
- `AutoTest/run_d3d9_draw_batch_gate.py`：`--fixed-function`；`fixed_batch_rows` 严格读方。
- `AutoTest/analyze_d3d9_admission.py`、`AutoTest/test_d3d9_admission.py`：批容量参数（32/20）。

这些文件在 git 中均为未跟踪状态，没有可回退历史；T0 快照仍待作者确认。

## 构建

`x64-ffbatch-build-20261029-r1`：fresh BelowNormal/-j2，sourcesPreserved、protectedUnchanged 通过。宿主
`build64_render_host_ffbatch_20261029_r1/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe`：
5,890,048 B，SHA-256 `C32C135C4CBF067460701C57E2EFDE85928088E93A89DE008A306DFE53D14889`。

## 正确性

参考 x86 DLL 仍为 `build32_device_owner_fixed_function_20261029`（26B411E9）。

- **CPU 门** `x64-ffbatch-cpu-20261029-r1`：x86/x64 各 210,734 项检查，wire 各 1,454,646 项，全部通过。
  新用例 `bounded-fixed-batch`：2 个可编程 → 23 个固定功能 → 1 个可编程，远端依次收到
  `DrawBatch(2)`、`DrawFixedBatch(20)`、`DrawFixedBatch(3)`、`DrawBatch(1)`，逐条字节正确；
  暂存期间诊断拒绝且不改序号/输出；未开开关时仍是一条同步 `DrawFixed` 且 `INVALIDCALL` 可恢复；
  延迟拒绝（INVALIDCALL/DEVICELOST 两种）为终止性，不重放。
- **固定功能 GPU 门** `x64-ffbatch-ff-gpu-20261029-r1`（默认管线）与 `-r2`（强制优化管线），均开 VVL365：
  本地前后、sync、group、stream、**group+固定批**、**stream+固定批** 七端各 297 张整图，
  哈希都是 `0627ed73c4fa582e`。两个固定批端 302 draw 中 281 个固定功能 draw 全部经 `DrawFixedBatch`
  到达宿主（stream 端 `streamFixedDraws` 也是 281）。这个 COM fixture 在 draw 之间有读回，
  所以批大小都是 1；20 条满批由下面的 raw 用例和计时 fixture 覆盖。VVL 非状态 warning/error 均为 0。
- **raw 故障门**（同上两轮，各三种环境）：
  - 非异步：先接受一个 20 条满批；再 12 个拒绝用例——空包、条数越界、条数与字节数不符、多余参数、
    非零 id、末条 NaN 矩阵、中间条 9 盏灯、末条过期声明代、首条双着色器、末条 0 图元、
    末条视口宽 0、末条未用灯非规范字节。每个都返回 `INVALIDCALL` 且连接健康，
    读回 64×64 全部仍是清屏色（12×4,096 像素），随后 3 条有效批次恢复并像素正确。
    其中 0 图元、视口宽 0 两例是解码通过、由后端全条目 prepare 拒绝的原子性用例。
    宿主 draws 153 = 原 97 + 56，rejected 84 = 原 72 + 12，提交全部退休。
  - 流 + 异步（原 `fixed-rejection-stream` 环境）：异步拒绝按契约是终止性的，因此只跑被接受的批次。
  - **新增流 + 同步**（`WARVK_D3D9_STREAM=1`）：宿主流模式下同样 12 拒绝/12 恢复、像素不变。
- **固定功能计时门 VVL** `x64-ffbatch-vvl-20261029-r1`：45,293 个固定功能 draw 经 2,299 个批次
  （44 帧 × 52 + 覆盖段 11），VVL 0；SM3 raw 拒绝与 legacy 端固定批计数为 0。
- **SM3 回归** `x64-ffbatch-sm3-regression-1025-20261029-r1`：像素/覆盖哈希 `26868a0ae3c1b12a`/
  `0cd2c8b01a0786e9` 与流批次相同，所有端固定批计数为 0。同轮开计时的
  `x64-ffbatch-sm3-admission-1025-20261029-r1` 用于排除回归（见下）。
- **资源寿命门** `x64-ffbatch-lifetime-20261029-r1`：COM 96 轮/576 资源/256 Query/319,488 像素失败 0，
  673 submitted=completed=retired、liveResources=0；raw 61 拒绝/48 像素恢复/13 Query 恢复，
  162 提交全退休。与流批次计数一致。
- 纯测试：`test_d3d9_admission` 4 例、`test_d3d9_stream_metrics` 3 例、`test_d3d9_draw_batch_reader`、
  CPU 读方 21 例通过。

## 性能（合成，不是游戏 FPS）

固定功能 1,025 draw/帧，流模式，同一宿主、同一轮 ABBA。期间 WSL 虚拟机占约 3 个逻辑核，系统忙 41–71%。

| `x64-ffbatch-1025-20261029-r1` | 完成中位 ms | p99 ms | 宿主 CPU ms（44 帧） |
|---|---:|---:|---:|
| 本地前/后 | 3.568 / 3.309 | 6.552 / 4.902 | — |
| 流 + 同步 DrawFixed T1/T2 | 46.833 / 45.078 | 124.993 / 51.698 | 1,750 / 1,125 |
| 流 + 固定批 F1/F2 | **9.182 / 8.240** | **14.160 / 11.183** | **344 / 359** |

开准入计时的复测 `x64-ffbatch-admission-1025-20261029-r1`：本地 3.42/3.34 ms，T 46.26/46.34 ms，
F **7.82/7.85** ms，计时开销在噪声内。

- 固定批让固定功能帧完成时间降到原来的约 1/5，宿主 CPU 降到约 1/3–1/5，像素完全一致。
- 同步路径每个固定功能 draw 约 44 µs（≈45 ms / 1,025）。这就是当前游戏路径里每个固定功能 draw
  付出的代价。
- 固定批仍为本地固定功能的约 2.3–2.8 倍，**没有达到建议门槛**（1.5×/2×，作者尚未批准）。
- **环境漂移**：同一会话的 SM3 回归中，未改动的本地参考为 1.65 ms，而前几轮是 0.5–0.6 ms；
  开计时对照显示 SM3 每批各阶段一致地慢 1.4–1.7 倍，包括本批没碰的解码和预检，GPU 等待项增幅更大。
  因此本批数字只做轮内比较，不与前几轮的绝对值拼接。

### 固定批剩余时间（F1，每帧 52 批）

| 每帧中位 | ms | 每批 µs | 每 draw |
|---|---:|---:|---:|
| 完成 / 到 EndScene 返回 | 7.82 / 7.09 | | |
| 生产者等队列 / 同步调用等待 | 3.57 / 2.22 | | |
| 工作线程批次往返 | 6.70 | 128.9 | |
| └ 写管道（63,600 B/批） | 1.39 | 26.8 | |
| └ 往返中宿主工作以外（估计） | 1.45 | 27.8 | |
| 宿主批次 apply | 4.94 | 95.0 | 4.8 µs |
| └ 解码（3,180 B + 校验 + 资源解析） | 1.24 | 23.8 | 1.2 µs |
| └ 固定功能常量填充 | **1.83** | 35.3 | **1.8 µs** |
| └ DXVK draw | 0.99 | 18.9 | 0.96 µs |
| └ 预检（含 FF key） | 0.44 | 8.4 | 0.42 µs |
| └ 绑定（含采样器） | 0.31 | 6.0 | 0.3 µs |
| 宿主读批次 | 0.54 | 10.3 | |
| 宿主 WaitEvent / 其他 op | 0.80 / 1.71 | | |

"其他 op" 1.7 ms 与同轮 SM3 端相同（1.67 ms），是本轮环境下读回/清屏等待 GPU 的成本，不是固定批引入的。

## 下一步建议

1. **固定批差量编码**。每条 3,180 B 中绝大部分是与上一条相同的固定功能状态（矩阵、阶段、灯、材质）。
   包内首条完整，后续条只带变化的状态组（按组位图），宿主在校验前先还原成完整状态，
   仍对每条完整状态执行 `ValidFixedFunction` 与全条目原子 prepare。直接影响写管道、宿主读、
   往返外开销和解码，合计约 3–4 ms/帧（占 7.8 ms 的一半左右），且可提高每包条数。
2. **固定功能常量按需重建**。原生 DXVK 只在相关状态变脏时重建 FF VS/PS 常量块；宿主现在每条都完整重算
   （1.8 µs/draw，最大单项）。可按输入是否与上一条相同跳过重算、只复制上一份结果。
   War3 每个模型都会改 WORLD，收益需真实 census 确认。
3. 之后再做准入拆解给出的通用项：把现有仅限批内的 `WARVK_D3D9_BATCH_STATE` 状态复用扩展到整个流列表并覆盖
   着色器/缓冲/纹理/规格常量；预检后先回复让传输与录制重叠；共享内存环。
4. `SetSamplerState`/`SetEffects`/`SetTextureStages` 仍是"有变化才发"的同步调用，每次变化都会冲刷批次并付一次往返。
   游戏中的变化频率未知，先用真实 census 计数再决定是否改为异步。

流模式与固定批都保持默认关闭；是否作为后续开发默认，由作者决定。T1 allocator 区间与
T2 真实逐帧 census 仍需游戏运行。

玩家 DLL 保持 `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`；
未启动游戏、部署、提交、推送、发布或修改系统。
