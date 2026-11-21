# R6：代理模式每帧流量剖析（2026-11-17）

上接 [代理模式性能计划](../plan/2026-11-17-x64-proxy-performance-plan.md) §6 的 R6。只读调研：没有构建、没有跑游戏或探针。

## 0. 范围、来源与版本

- 产物：`AutoTest/artifacts/x64-phase52-ab-20261117-r1/`，三张图（losttemple、echoisles、shengyusi=生与死）× 两轮 × 两种模式。
  每个代理目录读了 `game.stderr`、`host.log*`、`logs/War3_d3d9.log`、`logs/host_warvk-d3d9-host.log`、`receipt.json`；
  每个进程内目录读了 `logs/War3_d3d9.log`（它的 `game.stderr` 是空的）；还读了顶层 `receipt.json` 和每次运行的 `<图>-r<轮>-<模式>.log`（探针回执，含控制面 `frameSeries`）。
  提取与计算脚本在 scratchpad：`D:\tmp\AppData\ADMINI~1\Local\Temp\claude\a66d1abe2490ff62\0261171f-6707-49c0-8309-6ba7b932d19e\scratchpad\research\traffic\`（`extract.py`、`table.py`、`split.py`、`series.py`）。
- **代码版本。** A/B 用的 `front.dll` 构建于 01:20、`host.exe` 于 01:39。下文引用的行号都是**那一版工作树**的行号。
  调研期间（10:41–10:46）P0 的实现者在改 `d3d9_connection.h`、`d3d9_proxy.cpp`、`dxvk_core/d3d9_backend.cpp`、`dxvk_core/d3d9_backend.h`、`dxvk_core/d3d9_host_main.cpp`，这五个文件的行号已经移动（例如 `warvk-d3d9-desktop/v1` 从 :342 移到了 :408），按括号里的函数名或打印的 schema 名找。
  `d3d9_wire.h`、`d3d9_resource_registry.h`、`d3d9_command_ring.h`、`d3d9_ring_channel.h`、`src/d3d9/war3/platform/war3_proxy_frame.cpp` 都早于 A/B，行号即当前行号。
- **更正任务说明里的一处前提（已核对）：** 5.2 门传给探针的是 `--dump-every 100000`（`AutoTest/run_x64_phase52_ab_gate.py:71`），不是 240。
  所以 `frames/` 里只有第 0 帧（空白帧，门本身也把 `frameNotFlat` 排除了，同文件 :86–87），没有可用的时间序列帧。

## 1. 结论先说

1. **每进图帧的量**（按进图帧，不是按 Present）：Lost Temple 约 1.50 MB、108 次同步往返、68 次主机提交；EchoIsles 约 1.95 MB、111、70；
   生与死约 8.85 MB、85、49。计划 §1 表里生与死的“6.4 MB/帧”低估了约 40%，“~150 次同步/帧”高估（混进了加载帧与一次性建纹理）。（§4）
2. **进图后的同步往返大头是纹理更新，不是 Present。** 每进图帧 23–33 次 `UnlockRect` 更新已初始化的纹理层，每次 Begin+Chunk+Commit 三次同步往返，
   占 LT/Echo 每帧同步往返的约 90%（96/108、99/111），生与死约 82%（69/85）。Present+FrameScene+StageMarker 每帧只有 9 次。（§3、§4）
3. **主机每进图帧约 50–70 次提交，几乎每次都等 GPU 空。** 提交数可以精确分解成：流列表冲刷 + 纹理更新 + 建纹理 + Clear + Present + 读回 + 1（六次运行全部精确相等）。
   每次纹理更新先 `settle()`（把正在录的世界绘制列表冲掉并等 GPU 空）再 `submit(true)`（再等一次），所以流列表被切成约“纹理更新数 + 1”段。（§3）
4. **生与死的 15,791 个纹理是这张图自己的，不是循环。** 只有 `CreateTexture(MANAGED)` 会在主机建纹理；两轮完全相同；句柄复用只有 62 次，
   同时存活的句柄峰值 15,734，占 `MaxResources=16384` 的 96%——**离上限只剩约 650 个**。（§5）
5. **`discardCarriedBytes` 是 DISCARD 改名时把整段旧内容复制到新存储**（D3D9 语义下这些内容本是未定义的），每次约 333–378 KB；
   源内存按 DXVK 分配规则落在不带 CACHED 的 HOST_VISIBLE|COHERENT 类型上（推断），即主机 CPU 在读不缓存内存。
   生与死每进图帧约 3.85 MB 搬运 + 3.97 MB 阴影投射体复制，都从这块内存读。（§6）
6. **生与死另外被主机吞吐卡住**：环满等待每进图帧约 20–30 ms（约占进图时间 19%），而且环满时 x86 线程每进图帧要睡醒约 1,670 次——
   主机每执行一条记录就唤醒一次等待中的生产者。（§6.4）
7. 两轮之间：确定性的量（建纹理、初传次数、句柄、回存次数）完全相同；每进图帧的比率相差 <0.5%。代理帧率稳定（两轮差 ≤6%），进程内帧率在两轮间摆动最多 38%。（§7）

## 2. 日志里的 JSON 行：谁打印、什么时候、字段含义

除特别说明，全部是**退出时一次性的累计值**（代理在 `Connection` 析构、主机在 `wmain` 结尾）。每个代理运行有三个会话（两个只做工厂查询的短会话 + 一个真正的设备），
所以 `game.stderr` 里同一 schema 出现三次，最后一组才是游戏；主机对应 `host.log`、`host.log.1`、`host.log.2`（第三个）。

### 2.1 代理端（x86 前端，`game.stderr`/`game.stdout`）

| 行 | 打印位置 | 要点（已核对，读代码） |
|---|---|---|
| `warvk-d3d9-proxy/v1`（stdout） | `d3d9_connection.h:244-253`（`~Connection`） | `exchanges`：环模式下 = 环记录数（:237）。会话级握手/退出状态 |
| `async-clear/v1` | :256-266 | 名字是管道时代留下的。`clearPublished`：异步整屏 Clear 次数。**环模式下** `fullObservations/backpressureCalls/backpressureMs/maxBackpressureMs` 是 `ringPublish` 里环满的统计，**所有操作都算**（:826-833）；ms 用 `GetTickCount64`，粒度约 15.6 ms，所以生与死的 `maxBackpressureMs=16` 只是一个 tick，不是真实最大值。`maxObservedOutstanding`：已发布未被主机执行的记录数峰值（:841）。`lastPublished/lastReplySettled`：序号 |
| `draw-batch-proxy/v1` | :267-276 | 可编程 `DrawBatch` 路径；War3 全是固定管线，全 0 |
| `ordered-async/v1` | :278-281 | `uploadsPublished`：异步 `UploadBuffer`；`statesPublished`：异步 `SetEffects/SetSamplerState/SetTextureStages`（:435-437）。`passedStaged`：NOOVERWRITE 上传在有暂存绘制时**没有先冲刷暂存批**就发出去的次数（:438-451）——批没被它切断。`remoteSucceeded`：干净结束时直接等于前两项之和（:241），不是独立测量 |
| `fixed-batch-proxy/v1` | :284-288 | `admittedDraws`：固定管线绘制数；`flushedBatches`：发出的 `DrawFixedBatch` 数；`payloadCapacityBytes=63600=20×3180`，即每次绘制 3,180 B 全量状态 |
| `annotation-proxy/v1` | :290-295 | `annotatedDraws/records`：带阴影投射体等记录的绘制；`messages`：`AnnotateDraws` 记录数；`frameScenes`（每进图帧 5 个）、`stageMarkers`（每进图帧 3 个），这两种都是**同步调用**（:486-499）；`sessionResets*`：换图 |
| `content-proxy/v1` | :297-302 | 内容块通道（几何/骨骼矩阵 blob，`d3d9_content_wire.h:5-21`），本轮完全没用，全 0 |
| `command-ring-proxy/v1` | :304-308 | `records/bytes`：环记录数与字节（含 16 B 环头与 128 B 消息头，按 16 对齐，:835）；`syncCalls`：`ringCall` 次数（:849）；`parks`：x86 线程在 `ringAwait` 里睡下的次数（:803，同步等待与环满等待都算）；`wakes`：x86 发现主机睡着而去叫醒它的次数（:842） |
| `terminal/v1` | :310-313 | 会话的第一个终止原因 |
| `admission-worker/v1`、`admission-producer/v1` | :317-326 | 仅 `WARVK_D3D9_ADMISSION_PROFILE=1`；只在管道模式累计（环模式的 `ringCall` 不更新 `callWaitNs`），而且只在 `Query::Issue` 时采样（:689-690）——游戏里没有查询，等于没有 |
| `backing-proxy/v1` | `d3d9_proxy.cpp:1341` | 回存（x86 常驻副本）预算、驻留峰值、`fetchCalls`（向主机 `ReadContent` 的次数） |
| `upload-window-proxy/v1` | `d3d9_proxy.cpp:1350` | 上传窗口：`bufferLocks/textureLocks` **只数走窗口的锁**（未初始化的纹理层、或缓冲 DISCARD/追加写）；`requestedBytes` 是窗口字节 |
| `fvf-proxy/v1`、`window-proxy/v1` | `d3d9_proxy.cpp:1357`、:1359 | FVF 声明缓存；窗口钩子与 resize |
| `D3D9 game: ...` 文本 | `d9::GameTrace`（如 `d3d9_proxy.cpp:1517-1518`） | CreateDevice 参数、帧转储 |

### 2.2 主机端（x64，`host.log.N`，stderr 加 stdout）

| 行 | 打印位置（`dxvk_core/d3d9_host_main.cpp`） | 要点 |
|---|---|---|
| `warvk-d3d9-desktop/v1`（stdout） | :342/:357 | 初始化时一次 |
| `host init-apply`、`command-ring attached`、`exit-milestone` 文本 | :933-955、`ExitMilestone` | 时间点（`processElapsedMs`） |
| `draw-batch-host/v1` | :1030-1038 | 批计数、`submitted/gpuCompleted/retired` 序号、flight 相关（流模式下多为 0） |
| `stream-host/v1` | :1040-1047 | `streamSubmissions`：流列表冲刷次数；`streamCapSubmissions`（4096 绘制上限触发，0）；`constantRenames`；`latencyWaits`（0） |
| `dynamic-buffer-host/v1` | :1049-1053 | 见 §6：`dynamicBuffers`、三种上传模式计数、`discardRenames`、`discardCarriedBytes`、`uploadSettles` |
| `reset-host/v1` | :1056-1058 | 重置次数、尺寸、`syncInterval=1`（见 §8.3 的 FIFO 问题） |
| `fixed-batch-host/v1` | :1060-1062 | 收到的 `DrawFixedBatch` |
| `backing-host/v1` | :1063-1066 | 主机留存内容（`retainContent`）峰值、`contentReads` |
| `command-ring-host/v1` | :1068-1070 | `records`、`syncRecords`（= 代理 `syncCalls`）、`parks`（主机等记录时睡下，:911）、`wakes`（主机执行完一条记录、看到 x86 在等就 `SetEvent`，:984） |
| `annotation-host/v1` | :1075-1080 | `worldSceneEnters` = 进图帧数（本文的 W） |
| `scene-effects-host/v1` | :1083-1092 | `beforeUiRuns`、`beforeUiNs`（BeforeUi 特效 CPU 时间，LT 每进图帧约 0.47 ms，生与死约 1.5 ms） |
| `shadow-host/v2` | :1115-1135 | 阴影：`casterRecords`、`copiedBytes`（动态缓冲里投射体顶点的复制量）。**唯一的逐帧序列**：`runLog` 与 `deviceMemoryLog`，每进图帧一条，但只留前 `ShadowRunLogLimit=256` 条（:168），内容是阴影与显存，不含流量 |
| `content-host/v1` | :1138-1144 | 同 content-proxy，全 0 |
| `reuse-wait/v1`、`admission-host/v1` | :1146-1167 | 都是可选剖析，而且都在 `Issue` 时采样（:996；`d3d9_backend.cpp:2179-2186`）——游戏 `queries=0`，不会有样本 |
| `warvk-d3d9-host/v1`（stdout） | :1170-1186 | 总账：`accepted`、`backendCalls`、`submitted`、`draws`、`clears`（含区域清除）、`uploads`（= `UploadBuffer` 条数 + 纹理 Commit 次数）、`uploadedBytes`、`readbacks`（条带数）、`texturesCreated`、`peakLogicalBytes`、`present*`、`logicalHandles*`（:315-322） |
| `memory-host`、`shader-cache` | :789-816 | 只在 `DiagnosticCheckpoint` 时 |
| `command-ring-fault`、`host-fault` | :899、:986、:1000 | 只在故障时 |

### 2.3 进程内模式与探针

- 进程内 `game.stderr` 为空。`War3_d3d9.log` 只有：前 6 个 `BeforeUi trace`（含该帧 `calls=` 绘制数）、`world lighting scope`、换图信号。
  进程内的调用普查（`m_callerCensus`，含按纹理形状计数）是编译期开关 `WARVK_D3D9_MIGRATION_CENSUS_DEV`（如 `src/d3d9/d3d9_device.cpp:14441`），产品构建里没有。（已核对）
- 代理的 `War3_d3d9.log` 里有两条**周期**数据：`first BeforeUi frame=N`（`war3_proxy_frame.cpp:397-403`），以及每 900 帧一次的 `casters frame=...`（:604-605，内容见 :437-443），断开时再打一次 `frames= worldScenes=`（:529-536）。
- 探针回执 `controlPlane.frameSeries`（`AutoTest/run_x64_game_proxy_probe.py:453`）：约每 2.1 s 一条 `[探针秒, 产品 timestampMs（Unix ms）, frameIndex, isInGame]`；`observer.samples` 是 x86 私有字节的同频采样。

**没有任何按时间切片的流量计数。** 能用来分加载/进图的只有：进图帧数 W、首个 BeforeUi 帧号、`frameSeries` 的帧号-时间曲线、每 900 帧的投射体累计、两轮之间加载帧数不同。

## 3. 三条精确恒等式（六次代理运行全部成立，已核对）

记 P = Present 次数（`presentAttempts`），T = 纹理 Commit 次数 = 主机 `uploads` − 代理 `uploadsPublished`，T₀ = 走窗口的纹理锁（初次上传），Tᵤ = T − T₀（更新已初始化的层）。

1. **环记录** = `uploadsPublished` + `statesPublished` + `flushedBatches` + `annotation messages` + `syncCalls` + `clearPublished`。
   例：LT r1 696,914 + 99,829 + 126,399 + 43,448 + 195,650 + 2,119 = 1,164,359。
2. **主机提交** = `streamSubmissions` + T + `texturesCreated` + `clears` + P + `readbacks` + 1（初始化）。
   例：LT r1 55,263 + 57,273 + 2,019 + 2,119 + 2,118 + 484 + 1 = 119,277；生与死 r1 24,475 + 39,807 + 15,791 + 9,648 + 9,647 + 484 + 1 = 99,853。
   对应代码：`createTexture2D`、`uploadTextureRegion`、`clearRegions`、`readback`、`present` 各提交一次（`d3d9_backend.cpp:1397`、:1426、:1562、:2134、:2299），其余都是流列表冲刷（:633-641）。
3. **同步往返** ≈ P + `frameScenes` + `stageMarkers` + `texturesCreated` + `logicalHandlesDestroyed` + `readbacks` + 3·T，余数 LT 3,946/3,957、Echo 4,155/4,040、生与死 15,525/14,790。
   余数是超过 64 KiB 的纹理上传多出来的块、`GetDeviceCaps`/工厂查询（每次都是同步 `FactoryQuery`，`d3d9_proxy.cpp:96-100`、:268）和建缓冲/声明等。
   因为 Tᵤ（LT 约 53K）远大于余数，**纹理更新几乎都只有一个块（≤65,408 B）**。（算术已核对，块数推断）

另外：`readbacks=484` = 4 次整屏读回 × 121 个条带（1902×963，每条带 8 行，`d3d9_wire.h:118`）：第 0 帧转储 + ab1 + ab2 + 控制面截图。

为什么每次纹理更新是 3 次往返、主机 2 次等 GPU（已核对）：
`Texture::UnlockRect`（`d3d9_proxy.cpp:914-944`）依次同步发 `BeginTextureUpload`、每 64 KiB 一个 `TextureUploadChunk`、`CommitTextureUpload`；
主机在 Commit 时调 `uploadTextureRegion`（`d3d9_backend.cpp:1408-1432`）：先 `settle()`（:1417，冲掉正在录的流列表并 `vkWaitSemaphores` 等到最后一个序号，:438-479），
再 `begin()`（又一次 settle，此时空转），录拷贝后 `submit(true)`（:1426，再等一次 GPU）。
同理：建纹理 `begin`+`submit(true)`（:1395-1397）；整屏 Clear 也是 `begin`+`submit(true)`（:1538、:1562）；Present 前后各一次 `settle`（:2256、:2321），中间两段 `Sleep(1)` 轮询（:2309-2319，`waitPresentRetired` :666-683）；`Destroy` 一次 `settle`（:2355）。
每次 `submit` 还要等 DXVK 提交线程把它交给队列（`waitForSubmissionUntil`，:704）。

## 4. 加载与进图拆分

### 4.1 方法

- W = `worldSceneEnters`（= `frameScenes/5`，从 `first BeforeUi frame` 到结束，已核对），L = P − W。
  生与死的 L 里又分两段（`frameSeries`）：开头约 1,600 帧是真正读图（约 6 fps，35–46 s），之后约 7,400 帧是加载画面/开局对话框（代理约 300 fps，进程内约 3,200 fps）。
- **绘制数**：进程内 trace 给出加载画面每帧 24 次（LT/Echo）或 39 次（生与死）、首个进图帧 275 / 387 / 1,161–1,313 次。
  于是进图帧绘制 = (draws − 24|39 × L)/W：LT 278.0 / 277.6，Echo 389.3 / 389.7，生与死 1,376.4 / 1,375.6——两轮差 <0.3%，和进程内首帧对得上。（数据已核对，模型推断但被两轮一致性支持）
- **生与死**两轮的 L 差 733、W 差 30，可以对每个计数解 `总量 = L·a + W·b`。先扣掉一次性的量（建纹理、初次纹理上传 3·T₀、退出时销毁、读回；字节按 `peakLogicalBytes`）。
  校验：只在进图帧发生的量解出来 a≈0（投射体复制 a=−30 B，环满 a≈−0.7 ms，纹理更新 a=0.015），绘制 a=39.5（trace 是 39）。（推断，自洽）
- **LT/Echo** 两轮 L 太接近，联立病态；用 (总量 − 一次性)/W 当上界，加载帧只有约 460 帧、每帧约 0.1 MB，偏高 <5%。

### 4.2 每进图帧（r1 / r2）

| | Lost Temple | EchoIsles | 生与死 |
|---|---|---|---|
| 进图帧 W / 加载帧 L | 1,655 / 463；1,670 / 495 | 1,812 / 437；1,772 / 418 | 636 / 9,011；666 / 8,278 |
| 进图平均帧时间（代理 / 进程内） | 36.5 / 6.8；35.9 / 8.2 ms | 34.0 / 8.5；33.0 / 7.1 ms | 108.1 / 19.9；104.5 / 20.2 ms |
| 固定管线绘制 | 278 | 389 | 1,376 |
| 其中 3,180 B 全量状态 | 0.88 MB | 1.24 MB | 4.38 MB |
| 环字节 | ≤1.50 MB | ≤1.95 MB | 8.85 MB（未扣一次性 9.2） |
| 环记录 | ≤693 | ≤856 | ≈3,810 |
| 缓冲上传条数 / 字节 | ≈416 / ≤0.46 MB | ≈590 / ≤0.51 MB | ≈2,430 / 3.79 MB |
| 状态记录（SetEffects 等） | ≈57 | ≈57 | ≈556 |
| 每批绘制数（上限 20） | 3.7 | 4.8 | 3.8 |
| **同步往返** | **108** | **111** | **85** |
| 　其中纹理更新 Tᵤ×3 | 32.1×3 = 96 | 33.1×3 = 99 | 23.1×3 = 69 |
| 　Present + FrameScene×5 + StageMarker×3 | 9 | 9 | 9 |
| **主机提交（≈GPU 等空次数）** | **68** | **70** | **49** |
| 　流列表冲刷 | ≈33 | ≈34 | ≈24 |
| x86 睡下（parks） | ≤212 | ≤288 | ≈1,670 |
| 环满等待 | 0 | 0 | ≈60 次，20–30 ms |
| 阴影投射体 / 复制字节 | 181 / 337 KB | 275 / 384 KB | 545 / 3.97 MB |
| DISCARD 次数 × 搬运 | 1.08 × 363 KB = 0.39 MB | 1.20 × 378 KB = 0.45 MB | 12.2 × 333 KB = 3.85 MB |

生与死的**加载帧**：约 39 次绘制、0.21 MB、2.3 次同步、3 次提交（Present、Clear、一次流冲刷）、79 条缓冲上传。
加载画面阶段代理约 300 fps、进程内约 3,200 fps：一个几乎空的帧在代理里固定要花约 3 ms（推断：Present 往返 + 主机两次等 GPU + 呈现轮询）。

计划 §1 表要改：“约每帧流量”按进图帧应为 1.5 / 1.95 / 8.85 MB，“约每帧同步调用”为 108 / 111 / 85，“约每帧提交”为 68 / 70 / 49。
16 MiB 环在生与死上约装 1.9 个进图帧，不是 2.5 帧。A/B 用的环是 4 MiB（`command-ring-host.capacity=4194304`），在生与死上不到半个进图帧，在 LT/Echo 上是 2–3 帧——这就是只有生与死出现环满的原因。（已核对）

## 5. 生与死为什么建 15,791 个纹理

**哪些调用会在主机建纹理（已核对）。** 只有 `Device::CreateTexture`（`d3d9_proxy.cpp:1540-1549`），且只接受 `D3DPOOL_MANAGED`、`usage==0`，发 `CreateTexture2D`。
渲染目标不存在（`SetRenderTarget` 只认后备缓冲，:1559）；深度只有自动深度；`CreateOffscreenPlainSurface` 只造 x86 端的系统内存 `Surface`（:1550-1553），不建主机纹理；
`GetRenderTargetData` 走 `Readback` 条带（:1560-1566 → :444-458）。所以 15,791 次全是游戏自己的 `CreateTexture(MANAGED)`。

**是不是循环（已核对）。**
- 两轮完全一样：建纹理 15,791、初次窗口锁 24,976、`peakLogicalBytes` 367,124,288、回存 33 次分配、`fetchCalls` 68。
- 句柄按“最小空闲 id”分配（`d3d9_resource_registry.h:42-53`），复用才会让 generation>1（`d3d9_host_main.cpp:318`）。`logicalHandlesReused=62`，
  即整局只有约 62 次“先销毁后新建”。**没有建/毁循环。** 同时存活的句柄峰值 = 新建 − 复用 = 15,796 − 62 = 15,734（最小空闲分配下，拿到最高 id 时下面全是活的）。
- 每个纹理平均约 23 KB（367 MB / 15,791），平均 1.58 次初次锁；没有逐个尺寸/格式的日志。（平均数已核对，“多为单层小图标”是推断）
- 何时建：不知道。最可能在头 1,600 帧的读图阶段。代理与进程内的 `readyAtSeconds` 是 75.8/75.6 vs 74.6/74.7 s，**加载墙钟时间没有因此明显变长**。

**进程内是多少：没有数据。** 进程内的纹理普查编译期关掉了（§2.3）。推断两边相同：前端自己不建纹理；
进程内 x86 私有字节峰值比代理多约 360 MB（1.28 GB vs 0.917 GB），和主机留存的 367 MB 内容吻合。

**风险（已核对）：** `MaxResources = 16384`（`d3d9_resource_registry.h:11`，注释还写着“一张图几千个”）。生与死峰值 15,734 + 3 缓冲 + 2 声明，**用掉 96%**。
再多约 4% 的纹理，`reserve()` 返回空，`CreateTexture` 得到 `E_OUTOFMEMORY`（`d3d9_proxy.cpp:1545`）。更大的 RPG 图或更长的对局可能直接撞上。

**每帧的纹理流量是另一回事：更新而不是新建。** Tᵤ 在 LT/Echo 每进图帧 32/33 次、生与死 23 次（加载帧上 ≈0），每次 ≤64 KiB。
这些是对已初始化层的 `LockRect/UnlockRect`，x86 端有常驻副本（LT 39 个回存分配、峰值 1.4 MB）。具体是哪些纹理（战争迷雾？小地图？字形？）不知道——P0 要记。

## 6. 动态缓冲与其它几个怪数

### 6.1 `discardCarriedBytes`（已核对代码）

`D3D9Backend::uploadBuffer`（`d3d9_backend.cpp:1324-1369`）对动态缓冲：
- NOOVERWRITE：直接写进映射内存（:1352）。
- DISCARD 且流模式（:1341-1347）：`invalidateBuffer(..., allocateStorage())` 换一块新存储，**然后 `memcpy(新, 旧, initializedPrefix)` 把旧内容整段搬过去**，再写这次的字节。`discardCarriedBytes += initializedPrefix`。
- 其它模式先 `settle()`（:1349，本轮 `uploadSettles=0`，`orderedUploads=0`）。

为什么搬：注释（:1336-1340，`d3d9_wire.h:21-23`）说是为了让“已初始化前缀”和索引影子继续描述新存储。主机按前缀做校验：首次上传不许有空洞（:1332）、绘制范围要在前缀内（:1656-1673）；
`initializedPrefix` 只增不减（:1365），x86 端的绘制校验同样用前缀（`d3d9_proxy.cpp:1854`、:1894-1904），虽然它另记了 `definedEnd`（:791）。
D3D9 语义里 DISCARD 之后旧内容是未定义的，进程内 DXVK 不搬。所以**这是为维持校验不变式的额外工作，不是 D3D9 要求的**。

量（已核对）：`discardRenames == discardUploads`，每次搬的就是缓冲高水位（LT 363 KB、Echo 378 KB、生与死 333 KB，上限 576 KiB，`d3d9_wire.h:16`）。
War3 只有 3 个缓冲，全是动态的（`buffersCreated=3`、`dynamicBuffers=3`），所以没有静态缓冲上传的 settle。
生与死每进图帧约 12 次 DISCARD（缓冲环每帧绕好几圈：3.8 MB 上传 / 333 KB），即每帧多搬约 3.85 MB。

**源内存可能是不缓存的（推断，置信度高）。** 动态缓冲用 `coherentBuffer`（:1309、:711-724）只要求 HOST_VISIBLE|HOST_COHERENT；
DXVK 对这组标志只取系统内存类型（`src/dxvk/dxvk_memory.cpp:2147-2159`），并按下标从小到大试（:758-766）；主机日志里 Type 2 是 HOST_VISIBLE|HOST_COHERENT、Type 3 才带 HOST_CACHED。
于是旧存储多半在 Type 2，`memcpy` 是在读不缓存（通常是写合并）内存，比普通内存慢一个量级。
同一块内存还被阴影投射体复制读：`addShadowCaster` 把 `vertexData = vb.object->mapPtr(0)`（:2036）交给 `ShadowCasterReplayGeometry`，后者 `pool.copy` 复制（`src/d3d9/war3/effects/war3_shadow_scene_input.h:370-375`），
即 `shadow-host.copiedBytes`（LT 557 MB、生与死 2.52 GB）。生与死每进图帧主机要从这块内存读约 7.8 MB。花多少时间不知道（P0 要量）。

### 6.2 `passedStaged`

NOOVERWRITE 上传在有暂存绘制时不冲刷暂存批、直接越过它们发出去（`d3d9_connection.h:438-451`）。LT 696,914 条上传里 674,215 条（97%）这样走。
所以**批不是被上传切断的**；切断批的是状态记录（`SetEffects/SetSamplerState/SetTextureStages` 不带 passStaged，先 `flushDraws`，:447）、DISCARD 上传、以及所有同步调用（`call` 先冲刷，:656-658）。
结果每批只有 3.7–4.8 次绘制（上限 20）。（代码已核对，“主要是状态切的”推断：生与死每进图帧 556 条状态、443 个批）

### 6.3 `content-proxy` / `content-host`

内容块通道（x86 数据层把几何或骨骼矩阵按 key 上传一次，绘制记录只带引用）。本轮没有任何上传，全 0。（已核对）

### 6.4 `async-clear` 里的环满字段与睡醒风暴

- 生与死 r1：`backpressureCalls=38,118` 次环满，`fullObservations=368,044`（每次平均重试 9.7 次），`backpressureMs=12,664`（tick 粒度，总和可用、单次不可用），
  `maxObservedOutstanding=2,549`（LT 211）。按 §4 的联立，环满几乎全在进图帧上（a≈0），每进图帧约 20–30 ms，约占进图时间的 19%。
- **睡醒风暴（推断，代码已核对）**：生产者环满时在 `ringAwait` 里等，主机只要“执行了任何一条记录”就算有进展（:776-780），
  而主机每执行完一条记录、看到生产者在等就 `SetEvent`（`d3d9_host_main.cpp:984`）。生产者醒来重试，空间还不够它这条记录就再睡。
  生与死每进图帧 x86 睡下约 1,670 次、主机唤醒约 2,890 次。同步等待也一样：LT 每次同步调用平均睡下 1.8 次（被前面异步记录的进展叫醒，自己的记录还没执行）。
- 计划里“parks 远多于 wakes”比错了对：x86 的 `parks`（350,562）对应的是主机的 `wakes`（507,109）；x86 的 `wakes`（20,671）对应的是主机的 `parks`（18,738）（数字都是 LT r1）。主机很少睡（LT/Echo 每进图帧约 8–11 次，生与死上限约 34 次），多半在自旋或干活。

## 7. 两轮对比

| | LT r1 / r2 | Echo r1 / r2 | 生与死 r1 / r2 |
|---|---|---|---|
| 建纹理、初次锁、句柄、回存分配、读回 | 全同 | 全同 | 全同 |
| 进图帧绘制 | 278.0 / 277.6 | 389.3 / 389.7 | 1,376.4 / 1,375.6 |
| 每进图帧纹理更新 | 32.1 / 32.1 | 33.1 / 33.1 | 23.3 / 23.3 |
| 每进图帧投射体 | 180.6 / 180.4 | 275.2 / 275.5 | 545.2 / 545.3 |
| 每次 DISCARD 搬运 | 363.4 / 363.4 KB | 377.6 / 377.7 KB | 333.2 / 331.5 KB |
| 每进图帧同步（扣一次性） | 108.0 / 108.0 | 110.9 / 110.8 | 联立 84.8 |
| 每进图帧提交（扣一次性） | 68.1 / 68.1 | 69.9 / 69.9 | 联立 49.2 |
| 代理中位帧率 | 27.9 / 29.4 | 29.8 / 30.3 | 9.5 / 10.1 |
| 进程内中位帧率 | 146.7 / 126.4 | 112.0 / 154.8 | 51.9 / 50.7 |

帧率是 `frameSeries` 里 `readyAt`+2 s 之后、两端都在游戏中的约 2 s 窗口的中位数（scratchpad 里的 `series.py` 加同法求中位）。
总量差别（环字节 +0.8%、−2.2%、+1.4%）完全来自 W 与 L 不同。代理侧稳定；进程内帧率在 LT/Echo 两轮间摆动 16%/38%（Echo r2 进程内中途还从约 110 跳到约 158），说明本机负载漂移，比较只能看同一轮。（已核对）

## 8. P0 计数规格（交叉核对用）

原则：只加计数；QPC 计时；不往线上加任何东西；固定表、一次写一行。下面“位置”是 A/B 构建版的行号加函数名。

### 8.1 代理端（建议 schema 名沿用实现中的 `warvk-d3d9-sync-profile/v1`）

| 计数 | 位置 | 现有覆盖 |
|---|---|---|
| 按线上操作（`d9::Op`，43 个）：同步次数、等待 µs 总和/最大、直方图 | `ringCall`（`d3d9_connection.h:846-889`）：在 :848 取 QPC，`while` 退出后（:858）结算；`call()` 管道路径 :667-684 同理 | 只有总 `syncCalls`（:849）；`producerAdmission.callWaitNs` 只在管道路径 |
| 同步等待期间睡下次数 | `ringAwait` 的 `++ringParks`（:803）按当前操作记 | 只有总 `parks` |
| 按操作：异步发布次数、记录字节（含头）、负载字节 | `ringPublish` 写成功后（:834-835） | 只有总 `records/bytes` |
| 环满：按**正在发布的操作**记次数、重试次数、µs（QPC）、期间睡下次数 | `ringPublish` 的满循环（:818-833） | 只有总数，ms 用 `GetTickCount64` |
| 暂存批冲刷原因与批大小直方图（1、2–4、5–9、10–19、20） | `flushDraws`（:565-573）加原因参数；调用者：`call` :656-658、`enqueueOrdered` :447、`enqueueClear` :585、`submitDraw/submitFixedDraw` :403/:424、满 20 自冲 :410/:429、`endScene` :575 | 只有 `flushedBatches` |
| 按 D3D9 入口：调用次数，以及由它引起的同步条数、异步条数、字节、等待 µs | 入口处设“当前入口”：`Present` :371-379、`Clear` :1810-1843（分整屏/区域）、`DrawPrimitive/DrawIndexedPrimitive/DrawPrimitiveUP` :1849/:1890/:1867、`Buffer::Lock/Unlock` :748/:776（分 DISCARD/NOOVERWRITE/普通、窗口/常驻）、`Texture::LockRect/UnlockRect` :892/:914（分初次/更新）、`SetLOD` :878、`CreateTexture` :1540、`MakeBuffer` :1521、`Child::~Child`→`destroyResource` :691/:490（按类型）、`GetDeviceCaps` :268（每次都是同步工厂查询）、`Factory::query` :1103、`GetRenderTargetData` :1560、`Reset` :1443、`resizeToWindow` :1477、`Query::Issue/GetData` :1053/:1065、`EndScene` :366、数据层导出（`FrameScene/StageMarker/AnnotateNextDraw`） | `upload-window` 的两种锁数、`fvf`、`annotation` 计数；其余没有 |
| **纹理更新归属**：每次 `UnlockRect` 记纹理 id（或尺寸+格式+层）、字节、块数；固定表保留前 32 个最常更新的纹理 | `UnlockRect` :914-944 | 没有。这是找出每帧 23–33 次更新是谁的唯一办法 |
| `CreateTexture` 形状直方图：格式 × 尺寸档 × 层数 | :1540-1549 | 没有 |
| 每 N 次 Present 一个累计快照：present 序号、QPC µs、**Unix ms**（对齐控制面 `frameSeries`）、**`frameScenesSent`**（区分进图帧）、环 `applied`、上面全部累计；换图（会话重置）时和退出时各一次 | `Device::Present` :371-379 之后 | 没有任何周期快照（只有每 900 帧的投射体） |

### 8.2 主机端（`warvk-d3d9-host-profile/v1`）

| 计数 | 位置 | 现有覆盖 |
|---|---|---|
| 按操作：执行次数、执行 µs 总和/最大、直方图 | `wmain` 主循环 `session->apply`（`d3d9_host_main.cpp:957`），在 :922 与 :959 之间计时 | 可选 admission 剖析只分 batch/flush/waitEvent/other（:960-967），且只在 `Issue` 采样（:996） |
| 等记录的空闲：自旋 µs、睡下 µs、睡下次数 | 环读循环 :891-917（睡下在 :911-913） | 只有 `ringParks` 次数 |
| 回复与唤醒：µs、`SetEvent` 次数（区分同步回复 / 进展唤醒） | :969-990 | 只有 `ringWakes` 次数 |
| **等 GPU，按原因**：调用次数、真的在等（`pending`）的次数、`vkWaitSemaphores` µs、`waitRetired` µs、`checkFlightSubmissions` µs | `settle()`（`d3d9_backend.cpp:438-479`）加原因参数。原因 = 调用点：`present` 前/后 :2256/:2321、`begin` 来自 `clearRegions` :1538、`createTexture2D` :1395、`uploadTextureRegion` :1417/:1422、`readback` :2131、`present` :2283、`reset` :1251/:1282、`runBeforeUi` 非流 :2087；`submit(true)` 尾部 settle（:707-708）来自 `reset` :1285、静态缓冲上传 :1361、建纹理 :1397、纹理上传 :1426、清除 :1562、非流模式 `runBeforeUi` :2100、读回 :2134，以及 `drawBatch/drawFixedBatch` 的非流路径（流模式下不走）；`setTextureLod` :1438；`destroy` :2355；有序动态上传 :1349；静态上传 :1356；`finish` :2400 | `reuse-wait` 只量 `beginDrawFlight` 里的 settle（流模式下不走），也只在 `Issue` 采样 |
| **提交，按原因**：次数、`waitForSubmissionUntil` µs | `Owner::submit` :693-709 的调用者（同上）；`flushStream` :633-664 的调用者：settle 引起（带上 settle 的原因）、`drawBatch/drawFixedBatch` 里的 `MaxStreamDraws=4096` 上限（`++streamCapSubmissions` 处）、`FlushDraws` 操作 :2013、`issueEvent` :2173 | 只有总 `submitted` 与 `streamSubmissions`；§3 的恒等式能还原总分解，但没有时间 |
| Present 分段 µs：前 settle、acquire（:2269）、录制、submit、**`Sleep(1)` 等呈现结果的循环 µs 与圈数**（:2309-2319）、后 settle、`waitPresentRetired` µs 与圈数（:666-683） | `present` :2221-2349 | 没有 |
| CPU 复制：DISCARD 搬运 µs（:1343-1347）、上传 memcpy µs（:1352）、纹理暂存 memcpy µs（:1421）、投射体复制 µs（`war3_shadow_scene_input.h:370-375` 或在 `addShadowCaster` :2016-2066 外包） | 同左 | 只有字节（`discardCarriedBytes`、`copiedBytes`） |
| 每 N 次 Present 一个累计快照（在 `Present` 操作之后触发，不是 `Issue`）：present 序号、QPC、Unix ms、`worldSceneEnters`、上面全部累计 | :727-744 之后、:991 附近 | 没有 |

### 8.3 对正在实现的 P0 的核对点（10:41–10:44 看到的半成品，`tools/render_host/d3d9_sync_profile.h`）

- 已有：按 `d9::Op` 的名字表、`SyncApi` 入口表（含 `Caps`、`Factory`、`TextureUnlock`、`Release`）、QPC、`UnixMs()`、`WARVK_D3D9_SYNC_PROFILE_EVERY`。好。
- 直方图是 `<10 µs, <50, <200, <1 ms, <5 ms, 更长` 六档。纹理 Commit（带两次等 GPU）和环满多半落在 1–30 ms，建议把“≥5 ms”再拆成 `<20 ms` 和更长。
- 快照必须带 `frameScenesSent`（或主机的 `worldSceneEnters`），否则分析脚本仍然分不开加载与进图。
- 环满归因要用 QPC，并记在**被阻塞的那条记录的操作**上；同时记“满循环里睡下几次”，才能量出 §6.4 的睡醒风暴。
- 主机快照要在 Present 上触发；现有的 `Issue` 触发在游戏里一个样本都不会有。
- 主机等 GPU 的原因里要包含 Present 的两个 `Sleep(1)` 轮询，以及 DISCARD 搬运、投射体复制的 µs。
- 纹理更新要能落到具体纹理（§8.1 倒数第三行），否则 P1/P2 不知道该优化哪一类纹理。

### 8.4 别的需要顺手记的

- 代理用 FIFO（`syncInterval=1`，`d3d9_wire.h:49` 说 x86 把 DEFAULT 折成 ONE），进程内日志是 `VK_PRESENT_MODE_IMMEDIATE_KHR`（两边 `War3_d3d9.log:305` 与 `host_warvk-d3d9-host.log:282/289`，已核对）。
  A/B 两臂的呈现模式不同。生与死代理的加载画面能跑到 365 fps，说明隔离桌面上 FIFO 至少没有卡在 165 Hz 以下；但进图帧是否被 FIFO 量化不知道——P0 的 Present 分段计时能回答。

## 9. 给计划的结论

1. **改 §1 表的“约每帧”三行**：按进图帧应是 LT 1.50 MB / 108 同步 / 68 提交，Echo 1.95 / 111 / 70，生与死 8.85 / 85 / 49。16 MiB 环在生与死上约 1.9 帧（现在的 4 MiB 不到半帧）。
2. **P2 的第一刀是纹理更新，不是 Present。** 进图帧 82–90% 的同步往返来自 23–33 次纹理更新，每次 Begin/Chunk/Commit 全同步。
   计划写的“纹理分块异步，只有提交需要确认”还不够：Commit 也应该异步（错误走 fail-closed），否则每次更新仍是一次往返加主机两次等 GPU。
3. **P1 的第一刀是 `uploadTextureRegion` 的 `settle()`**：它把每帧的世界绘制列表切成约“纹理更新数 + 1”段，每段都等 GPU 空。其次是 Clear 的 `begin/submit(true)` 和 Present 前后的两次 settle。
4. **生与死另外要两样**：P4 的状态增量（3,180 B × 1,376 次 = 每帧 4.4 MB，占一半流量）；以及把主机读不缓存内存的 7.8 MB/帧量出来（DISCARD 搬运 + 投射体复制）。
   如果确实贵，候选做法：DISCARD 不搬（按 D3D9 语义），或动态缓冲换带 CACHED 的内存类型，或保留一份缓存的 CPU 影子。这三种都改主机语义或内存，归 Fable 设计（P1）。
5. **环满时的唤醒要加门槛**（空间够下一条记录再叫醒，或按字节水位），否则加大环只是把睡醒风暴挪后。这可以进 P3，但改的是传输的唤醒规则，要 Fable 过目。
6. **`MaxResources=16384` 在生与死上只剩 4%**。切默认或试用前必须处理：P2 的“客户端分配句柄”一并把上限放大或改成可增长；在那之前至少让 `CreateTexture` 失败时有清楚的诊断。
7. A/B 门的呈现模式不一致（代理 FIFO、进程内 IMMEDIATE），5.2 第二轮之前要么统一，要么在回执里写明。
8. 分析脚本（`analyze_x64_sync_profile.py`）可以直接复用 §3 的三条恒等式做自检，用 §4 的方法（扣一次性、按 `frameScenes` 切）给每进图帧数字。

## 10. 还不知道的

- 每种同步调用各等多久、主机每帧花在执行记录 / 等 GPU / 空闲 / 唤醒上的时间：要 P0。
- 每进图帧被更新 23–33 次的是哪些纹理（尺寸、格式、层），为什么 LT/Echo 比生与死还多。
- 动态缓冲实际落在哪个内存类型，DISCARD 搬运与投射体复制各花多少 µs。
- 15,791 个纹理在哪段时间建（读图阶段还是进图后陆续），尺寸与格式分布；进程内同一张图的实际纹理数（需要带 `WARVK_D3D9_MIGRATION_CENSUS_DEV` 的构建）。
- 同步往返余数（LT 约 4K、生与死约 15K）里，多出来的上传块与 `GetDeviceCaps` 各占多少。
- FIFO 在隔离桌面上会不会把代理的进图帧量化到刷新周期的整数倍。
- 进程内 LT/Echo 两轮之间帧率为什么摆动 14–42%（本机负载？），以及这对帧时间比的影响。
