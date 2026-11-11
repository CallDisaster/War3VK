# x64 宿主流式命令列表与原生常量环（T2 第一批原型）

2026-10-29，Claude 接手 Codex 的 T1/T2 首批测量后实施。只改开发宿主/代理与开发门，
默认关闭；不接游戏、不部署玩家 DLL、不提交/推送/发布。Game.dll guard 保持。

## 为什么先做这一块

[T1/T2 首批测量](2026-10-29-x64-review-t1-t2-baseline.md) 已经实测：1,025 draw 合成帧在
四 flight 合并提交模式下每帧 8 次 flight 复用等待，占完整命令完成时间 51–55%。
源码复核还发现两条比 flight 等待更影响真实游戏的同步路径（本批**未**改，列为下一批）：

- `drawFixed` 每次都 `settle → begin → submit(true)`：每个固定功能 draw 前后各让 GPU
  跑空一次，代理侧又是一次同步 IPC 往返。War3 主体是固定功能，这是游戏规模下的首要瓶颈。
- 缓冲 `Unlock` 逐块同步 `UploadBuffer`，宿主 `begin()` 内 `settle()` 先等 GPU 完成；
  DISCARD/NOOVERWRITE 没有换版本。动态 VB/IB 每次写锁都是一次 GPU 排空。

本批先在宿主侧建立与原生 DXVK D3D9 相同的执行形状，作为后续两条路径的共同基础。

## 实现与所有权

新增默认关闭的 `WARVK_D3D9_STREAM=1`（宿主与代理都读取）。与 `WARVK_D3D9_GPU_FLIGHTS`、
`WARVK_D3D9_GPU_GROUP` 以及 flight 等待计时互斥，组合时拒绝启动。旧模式代码路径不变。

- **常量环**：形状照搬原生 `D3D9ConstantBuffer::Alloc`。每个绑定（SM3 VS/PS、FF VS/PS）
  一个 host-visible UBO，每块存储 1 MiB（原生 `DefaultConstantBufferSize`）。每个 draw
  取一片按 `minUniformBufferOffsetAlignment` 对齐的新切片；用尽时
  `allocateStorage()` + `invalidateBuffer()` 换存储。`invalidateBuffer` 把旧 allocation
  交给正在录制的命令列表跟踪，GPU 退休前不会被改写。没有 CPU 等 GPU。
- **流式命令列表**：draw 批次和固定功能 draw 都录进同一个打开的命令列表，不再每批
  begin/submit。提交点是 `FlushDraws`（代理在 EndScene 发送，与 group 模式相同）、
  EVENT Issue，以及所有走 `settle()` 的非 draw 操作（上传、Clear、Destroy、Readback、
  Present、Finish）。`settle()` 先提交流再按原规则等待，所以这些操作的语义不变。
- **有界**：单个列表最多 4,096 draw，满额先提交；每次流提交后若未完成序号领先 GPU
  超过 3 个，就等待到 `lastSerial-3`（原生帧延迟的形状），正常每帧一次同步不会触发。
  DXVK 自身的提交队列上限 32 仍然存在。
- **固定功能 draw**：流模式下不再 settle，FF 常量走各自的环。代理侧 `DrawFixed`
  仍是同步 IPC，本批不改。
- **诊断**：`hasPendingDraws()` 对打开的流返回真，只读诊断不会隐式提交；
  `pollTimeline` 在列表录制中不再释放 `commands`（group 模式原本也有同样的潜在问题，
  正常情况下被 `resourcesInUse()` 挡住）。
- 冷输出新增独立的 `warvk-d3d9-stream-host/v1` 行；冻结的 `draw-batch-host/v1` 字段不变。

复核中发现：已有 flight/batch UBO 用 `ShaderStages`（着色器阶段位）当作 DXVK 缓冲的
**管线阶段**传入，派生出的 `getShaderStages()` 是 TESS_CONTROL。这些缓冲从不换存储，
所以没有可见影响；但若照搬到会 `invalidateBuffer` 的环上，换存储后 VS/PS 描述符不会被
标脏。新环改用 `VERTEX_SHADER|FRAGMENT_SHADER` 管线阶段。旧缓冲未改，留待相关模块收敛时处理。

## 改动文件

- `tools/render_host/dxvk_core/d3d9_backend.{h,cpp}`：环、流、上限与统计。
- `tools/render_host/dxvk_core/d3d9_host_main.cpp`：stream 冷输出行。
- `tools/render_host/d3d9_connection.h`：读取 `WARVK_D3D9_STREAM`，EndScene 发 FlushDraws。
- `AutoTest/test_render_host_d3d9_batch_rejection.cpp`：流模式下同样验证"只读诊断拒绝隐式提交"。
- `AutoTest/run_d3d9_draw_batch_gate.py`：`--stream` 对照（G1/T1/T2/G2 + 前后本地参考），
  流指标严格读方；环容量上限只对流模式放宽到 4 MiB。
- `AutoTest/test_d3d9_stream_metrics.py`：读方纯测试 3 例。

这些文件在 git 中均为未跟踪状态，没有可回退历史；T0 快照仍待作者确认。

## 构建

`x64-stream-build-20261029-r1`：fresh BelowNormal/-j2，configure 112 s、build 951 s，
no-work、sourcesPreserved、protectedUnchanged 通过。宿主
`build64_render_host_stream_20261029_r1/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe`：
5,881,856 B，SHA-256 `E7CB8E38CF949DCE0CCC390D456F91149B5604E8B33C7EDF76121802BD2FAEDB`。
警告总数 373，与 813688CC 构建相同；backend/host_main 两条为原有警告。

## 正确性

全部使用同一 E7CB8E38 宿主和原 x86 参考 `build32_device_owner_fixed_function_20261029`
（26B411E9）。旧模式（sync/group/flight）在同一宿主上一并复跑。

- **合成批次门** `x64-stream-ab-1025-20261029-r1/r2/r3`、`x64-stream-ab-97-20261029-r1`：
  每轮本地前后、G1/G2、T1/T2 六个计时端采样像素与覆盖哈希相同
  （1,025：`26868a0ae3c1b12a`/`0cd2c8b01a0786e9`；97：`41e448ad78aa1c61`/同覆盖哈希）。
  流模式 29 个原子拒绝/恢复、5 个在途 Destroy、6 类 ID 换代、只读诊断拒绝通过。
  1,025 draw 流端：45,293 draw 全部进流，47 次流提交、最大单列表 1,025、
  353 次常量换存储、0 次 cap 提交、0 次延迟等待；submitted=completed=retired=153
  （group 端 506）。
- **VVL365** `x64-stream-vvl-20261029-r1`：流拒绝、legacy、T1 三场景同步校验激活，
  非状态 warning/error 均为 0，包含 353 次换存储。
- **固定功能 GPU 门** `x64-stream-ff-gpu-20261029-r1`（默认管线）与 `-r2`（强制优化管线），
  均开 VVL：本地前后、sync、group、**stream** 五端各 297 张整图，哈希都是
  `0627ed73c4fa582e`（与 Codex r5/r6 相同）。stream 端 302 draw 中 **281 个固定功能 draw
  走流路径**；StateBlock 247 项远端检查通过。raw 故障用例在流模式下另跑一次：
  20 拒绝/20 恢复、宿主 97 draw/72 拒绝，与非流模式完全相同。VVL 均为 0。
- **资源寿命门** `x64-stream-lifetime-20261029-r1`（默认非流）：COM 96 轮/576 资源/
  256 Query/319,488 像素失败 0，673 submitted=completed=retired、liveResources=0；
  raw 61 拒绝/48 像素恢复/13 Query 恢复，162 提交全退休。与 Codex r1 计数一致。
- 纯测试：`test_d3d9_stream_metrics` 3 例、`test_d3d9_reuse_wait` 3 例通过。

## 性能（合成，不是游戏 FPS）

r1 运行期间 Steam 游戏 `PBZ.exe` 占用约 6–7 个逻辑核（系统忙 95–99.9%），本地参考
中位 11.4/7.0 ms。该轮 stream 16.6 ms 对 group 68.5/73.4 ms，但绝对值无效，只说明
竞争负载下固定 flight 等待放大得更厉害。r2 在游戏退出后但仍有负载（忙 64–97%），
两模式差距在噪声内。**r3 是本批唯一较安静的一轮**（忙约 40%）：

| 1,025 draw，r3 | 完成中位 ms | p99 ms | 到 EndScene 返回的中位 ms | 宿主 CPU ms（44 帧） |
|---|---:|---:|---:|---:|
| 本地前/后 | 0.816 / 0.609 | 1.896 / 1.070 | 0.703 / 0.492 | — |
| group G1/G2 | 3.648 / 3.810 | 4.604 / 5.259 | 3.581 / 3.742 | 187.5 / 187.5 |
| stream T1/T2 | **2.763 / 2.899** | **3.482 / 3.619** | 2.604 / 2.729 | **109.4 / 109.4** |

- 安静条件下流模式完成中位约低 24%，p99 约低 25–34%，宿主 CPU 约少 42%；
  远端仍约为本地的 3.4–4.7 倍，**没有达到建议门槛**（1.5×/2×，作者尚未批准）。
- 97 draw（r1）：stream 0.517/0.439 ms 对 group 0.500/0.511 ms，本地 0.282/0.167 ms。
  97 draw 只有 4 批、1 组，本来没有 flight 复用等待，不应期待差别。
- **剩余时间几乎都在 EndScene 返回之前**：r3 流端 2.6–2.7 ms 中只有约 0.16 ms 在其后。
  也就是 x86 生产者经 4 槽队列、命名管道、宿主逐条处理 DrawBatch 的准入路径，
  而不再是 GPU 等待。下一步应优先测这一段的组成（管道读写、解码/资源解析、
  prepareDraw 的逐索引检查、每 draw 约 8 KB 常量 memset 写入 host-visible 内存）。
- 本批 group 数字（3.6–5.0 ms）明显低于 Codex 同日 14.55–14.61 ms，本地参考也从
  1.74 ms 变为 0.6–0.9 ms。机器状态不同（GPU 时钟/负载未记录），不跨轮比较。
  三轮只有一轮安静，40 帧样本的 p99 即最大样本，不作长期尾延迟认证。

## 未做与下一批

- 代理侧固定功能 `DrawFixed` 仍是每 draw 一次同步 IPC；宿主侧已不再 GPU 排空，
  下一步让 FF draw 也走异步批次（新 opcode 或复用异步准入），并保持整批原子预检。
- `Unlock` 仍逐块同步上传并在宿主 `settle()` 排空 GPU；应改为在打开的流列表内用
  DXVK staging 记录拷贝，DISCARD 走换存储，只有读锁/读回保留同步。
- 共享内存命令环替换命名管道 + 逐条回复，先用上面的准入分解确认收益。
- 常量写入可按原生只绑定实际使用范围（依赖 robustness2 越界读零），需单独 A/B。
- T1 allocator 地址区间、T2 真实逐帧 Lock/Unlock/GetData census 仍未做，需要实际游戏运行。
- 流模式保持默认关闭；是否作为后续开发默认，由作者决定。

玩家 DLL 保持 `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`；
未启动游戏、部署、提交、推送、发布或修改系统。
