# P0：代理模式同步等待与宿主等待路径剖析回执（2026-11-17）

只加测量，不改顺序、线上格式、协议、超时（5 s 头部期限未动）与失败语义。
标注：**已核对**＝计数/日志/代码直接给出，或恒等式成立；**推断**＝由计数与代码推理，未单独实验证实。
所有宿主数字都是**宿主拥有线程的墙钟时间**（QPC），不是 GPU 执行时间（没有 Vulkan 时间戳）；
x86 同步等待 ⊃ 宿主执行 ⊃ 宿主等待路径，三层**嵌套，不能相加**。

## 1. 结论先说

1. **Lost Temple / Echo Isles（进图稳态，4 MiB 环）**：x86 游戏线程约 88% 的帧时间阻塞在同步往返里（已核对）。
   最大一项是**纹理更新**：每世界帧约 32 次 16×16 A8R8G8B8 单层整层更新（每张每帧一次，1 KiB），每次 Begin/Chunk/Commit 三个同步往返，
   合计约 97 次同步/帧（全部同步的 90%），x86 墙钟 25–32 ms/帧；宿主每次 Commit 先 settle（把世界流列表切开、提交、等 GPU 空闲）再 submit(true)，
   每帧约 67 次宿主提交里有 64 次来自纹理上传（上传前切断的流列表 + 拷贝本身）。其次是 Present（8–10 ms/帧，宿主大头是 Present 前的 settle 等本帧 GPU 完成）。（已核对）
2. **呈现模式**：新 x86 DLL 后宿主确为 IMMEDIATE（`syncInterval=2147483648`，DXVK 日志 `VK_PRESENT_MODE_IMMEDIATE_KHR`，已核对）。
   但 FIFO→IMMEDIATE 只从 19.8 变到 21.1 世界 fps（不同窗口、同类负载）；两种模式下 Acquire≈0、PresentQueue≈1.5 ms（每帧约 1 次 Sleep(1)），
   代理模式现在**不是被垂直同步卡住**，瓶颈在同步往返与宿主等待路径（已核对计数；“不是 vsync 限制”是由计数推断）。
3. **生与死（进图稳态，4 MiB 环）**：6.75 世界 fps。宿主才是瓶颈：宿主执行占墙钟 80%，其中 `CasterCopy` 路径 36.5 ms/帧、`DiscardCarry` 路径 30.2 ms/帧
   （都是包住分配/改名/整个 addCaster 的墙钟路径，不是纯 memcpy）；x86 等在 StageMarker（35 ms/帧，等宿主追上）与环满（27.7 ms/帧）上。（已核对）
4. **16 MiB 环（生与死）没有解决问题**：环满等待从 27.7 ms/帧降到 0，但 x86 在 StageMarker 上的等待从 35.0 升到 55.3 ms/帧——等宿主追上的时间只是换了个地方。
   世界 fps 6.75→8.26（快照窗口）、控制面中位 7.09→7.41；两轮负载不同（ring16 运行前 CPU 98%），窗口只有 120 世界帧，差值不能归功于环大小（已核对计数；结论是推断）。
5. **约 1.01 s 的周期性卡顿**（第二批帧时间环发现）：Lost Temple 进图后每 4–9 s 左右出现一帧 1.03–1.16 s，FIFO/IMMEDIATE 都有。
   这些帧与 x86 等 `TextureUploadChunk` 回复约 1,006–1,015 ms 一一对应（快照内 chunk 同步墙钟 ≈ 1 s × 卡顿帧数，已核对）；
   宿主执行该操作只要微秒级。1,000 ms 正是环两侧睡眠的复查超时 `d9::ring::RecheckMs`（已核对常量）——**某一侧漏了唤醒、靠 1 s 超时自救**是推断，哪一侧未定。
   影响：LT IMMEDIATE 窗口 2.09 s/21.3 s（9.8%）、LT FIFO 3.26 s/19.7 s（16.5%）；第一批 LT/Echo 窗口里也各有 1–3 次（chunk ≥20 ms 计数与 ~1 s 墙钟吻合）。
   这是最便宜的一项：修好唤醒，LT 世界 fps 约 28→31（推断）。下一步：两侧各记“超时醒来却发现有进展”的次数，定位是哪一侧漏唤醒（交 P3 / Fable 审传输唤醒规则）。
6. 同一会话（下午、GPU 空闲）重测：IMMEDIATE 28.1 世界 fps（控制面 30.2，帧时间 p50 29.7 / p95 42.1 / p99 79.6 ms），FIFO 24.4（27.9；p50 30.5 / p95 48.1 / p99 227 ms）；
   中位帧几乎相同，差别在尾部与平均（卡顿次数不同），仍不能把差值全归给呈现模式。进程内同设置 194.9 fps。
7. 计数恒等式在全部代理运行上成立（环记录、宿主提交、我的剖析与既有计数逐条对账，§8）。
8. 仪表关闭对照（`WARVK_D3D9_SYNC_PROFILE_TIMING=0`、无周期快照）：控制面中位 30.75 fps，对比同会话计时打开 30.22 fps，差 1.7%，在噪声内（单次，不能更细）。

## 2. 改了哪些文件

| 文件 | 内容 |
|---|---|
| `tools/render_host/d3d9_sync_profile.h`（新） | 共享：操作名表、`SyncApi` 入口枚举、QPC、`UnixMs`、`WARVK_D3D9_SYNC_PROFILE_EVERY`、`WARVK_D3D9_SYNC_PROFILE_TIMING=0`（热路径计时关闭的对照开关）、固定缓冲 JSON 行 |
| `tools/render_host/d3d9_connection.h` | 前端 `warvk-d3d9-sync-profile/v1`：按线上操作的同步次数/µs/最大/7 档直方图/等待中睡下次数；异步发布次数与字节；环满（按被阻塞记录的操作）次数/µs/最大/直方图/睡下次数；按 D3D9 入口与（入口,操作）对；纹理更新归属表（固定 512 槽，按 id/generation/level，含创建调用点、最后 StageMarker）；游戏线程帧时间环（512）；快照带 `presents`、`frameScenes`、`stageMarkers`、QPC 秒、Unix ms |
| `tools/render_host/d3d9_proxy.cpp` | 各 D3D9 入口加 `ProfileScope`；Present 后 `profilePresented()`；`UnlockRect` 成功后记一次纹理更新；`CreateTexture` 记返回地址 |
| `tools/render_host/dxvk_core/d3d9_backend.h/.cpp` | `WaitSite`（每个 settle/submit/wait 调用点一个，Present 分 Before/Present/After）× `WaitKind`（SettleIdle/SettleGpu/Retire/SubmitCpu/PresentRetire/Acquire/PresentQueue/StreamLatency/FlightReuse/Event + DiscardCarry/UploadCopy/CasterCopy 三条 CPU 路径）；次数/µs/最大/Sleep(1) 次数/≥1·5·20 ms 次数；按调用点的提交与 waitForGpu 提交 |
| `tools/render_host/dxvk_core/d3d9_host_main.cpp` | 宿主 `warvk-d3d9-host-profile/v1`：按操作执行次数/µs/最大/≥1·5·20 ms；等记录空闲 µs；按 Present 计数触发快照（不是 Issue）；带 `frameScenes`、`worldSceneEnters` |
| `AutoTest/analyze_x64_sync_profile.py`（新） | 读快照，按控制面 ready+12 s 到首个截帧/VA 动作取窗口，按宿主 `worldSceneEnters` 归一到世界帧；`sampleValidForGameplay`；帧时间分位；§8 恒等式自检 |
| `docs/research/2026-11-17-x64-upload-ownership.md`（新） | 纹理更新所有权链（只读分析） |

产品源码 `src/d3d9` 我没有改（`war3_proxy_mode.cpp` 的 IMMEDIATE 改动是 lead 的）。没有提交任何东西。

## 3. 构建与输入

| 产物 | 路径 | 说明 |
|---|---|---|
| 宿主（第一批） | `build64_render_host_p0_20261117_dev`，11:02 构建，留存副本 `scratchpad/fixture_build/host_p0_r1.exe`（sha256 前缀 `11e50bcfbaab7e03`） | FIFO/IMMEDIATE LT、Echo、生与死 r1/r2、ring16 用它 |
| 前端（第一批） | `scratchpad/fixture_build/proxy.dll` 11:02，留存 `proxy_p0_r1.dll`（`e64ae39092508484`） | 同上；5.2 前端留存为 `proxy_phase52.dll` |
| 宿主/前端（第二批） | 宿主同目录 15:57 重建（`dd6a0a885821b420`）；前端 `fixture_build/proxy.dll` 15:52（`629718a81b7fed64`） | 加了计时开关、帧时间环、纹理创建调用点、最后 StageMarker；前端构建时 `d3d9_proxy.cpp`/`war3_d3d9_observer.h` 里已有其它 agent 同期的设置面板改动（我未审） |
| 新 x86 产品 DLL | `build32_device_owner_p0_20261117_dev/src/d3d9/d3d9.dll`（`8d3b0f081d14a980`） | 含 lead 的 IMMEDIATE 改动 |
| 旧 x86 产品 DLL | `build32_device_owner_phase52_20261117_a/src/d3d9/d3d9.dll`（`C4787B3EE50F`） | FIFO 基线；与 5.2 A/B 同一 DLL，故选它而不是 probe.sh 默认的 phase51_dev |

测试：改动文件涉及的静态测试与基线相同的 5 个失败（`test_dxso_shader_module_cache_contract`、`test_host_surface_integration`、`test_render_host_initialization`、
`test_render_host_surface_owner`、`test_surface_owner_continuation`，失败信息与改动前逐字相同，属既有）；`test_proxy_mode_lifecycle_static` 15/15；
`run_d3d9_connection_async_cpu.py`：第一批改动后通过（`AutoTest/artifacts/x64-p0-connection-cpu-20261117-r1`）；
第二批改动后**两次失败**（`-r2`、`-r3`，均为 x86 用例 `ring-reply-invalid`：期望终止细节 `reply-invalid`，实得 `applied-invalid`，测试源第 627 行 `expect` 的细节比较）。
已核对的机制：该用例的夹具先用 `reply(other)` 发布 `applied = 序号+1`（大于已发布）并在生产者等待时 `SetEvent`，下一句才把 `applied` 改回本序号；
生产者若在这两句之间观察，就判 `applied-invalid`。这是夹具里的竞态；两次失败时机器 CPU 负载很高（其它 agent 构建/跑门）。
我的第二批改动在这条路径上只多了一次 `ringParks` 读取与计时开关判断，不改任何发布/观察顺序——“与改动无关、由负载触发”是**推断**，未在空闲机器上复跑证实；按要求如实记为红。

## 4. 运行（沙盒、隔离桌面、一次一个、BelowNormal）

产品 profile；`--dump-every 100000`；动作沿用 5.2 A/B（s−16 截帧 ab1、s−6 ab2、s−3 VA）；`WARVK_D3D9_SYNC_PROFILE_EVERY=120`。
目录前缀 `AutoTest/artifacts/x64-p0-sync-profile-20261117-`；分析器全量输出在 `AutoTest/artifacts/x64-p0-sync-profile-20261117-analysis/analysis.{md,json}`（`python -B AutoTest/analyze_x64_sync_profile.py <运行目录...>`）。运行前 nvidia-smi 记录在 `scratchpad/logs/p0_nvsmi_<名>.txt`。

| 运行 | x86 DLL | 结果 | 运行前 GPU | 备注 |
|---|---|---|---|---|
| `losttemple-fifo-r1` | 旧（FIFO） | 进图；红项仅 `frameNotFlat` | 39%，11.2 GiB 已用 | 宿主 `syncInterval=1`，DXVK FIFO |
| `losttemple-r1` | 新 | 进图；红项仅 `frameNotFlat` | 33%，11.0 GiB | IMMEDIATE |
| `echoisles-r1` | 新 | 进图；红项 `frameNotFlat`、`sessionsEndedClean` | 26%，11.0 GiB | 退出时前端等宿主 5,047 ms 超时，见 §7 |
| `shengyusi-r1` | 新 | **失败：未进图**；`gameReady`/`captureVerified`/`frameNotFlat` 红 | 24%，11.1 GiB | 保留。SPACE 在 72.3 s 投递时还在加载（帧号≈1,551），“按任意键”画面约 90 s 才出现；截图停在加载完成页 |
| `shengyusi-r2` | 新 | 进图（ready 75.8 s，ab1 截图确认在游戏内）；红项 `frameNotFlat`、`sessionsEndedClean` | 21%，3.1 GiB，CPU 34% | 170 s，SPACE 在 72/90/105/120 s；第一下即被消费。退出超时同 §7 |
| `shengyusi-ring16-r1` | 新 | 进图（ready 108.2 s，ab1 截图在游戏内）；红项 `frameNotFlat`、`actionsPosted`（ab2 截帧请求超时） | 9%，3.5 GiB，**CPU 98%** | `WARVK_D3D9_COMMAND_RING_KIB=16384`，前端确认 capacity 16,777,216；同第一批二进制；加载慢、窗口只有 120 世界帧 |
| `losttemple-inprocess-r1` | 新（进程内） | 全绿 | 6%，3.3 GiB，CPU 39% | 同一探针设置；控制面中位 194.9 fps，IMMEDIATE |
| `losttemple-r2` | 新 | 进图；红项仅 `frameNotFlat` | 25%，3.2 GiB，CPU 30% | 第二批二进制；IMMEDIATE |
| `losttemple-timingoff-r1` | 新 | 进图；红项仅 `frameNotFlat` | 9%，3.3 GiB，CPU 33% | `WARVK_D3D9_SYNC_PROFILE_TIMING=0`，无周期快照；只有控制面 fps |
| `losttemple-fifo-r2` | 旧（FIFO） | 进图；红项仅 `frameNotFlat` | 13%，3.2 GiB，CPU 63% | 第二批二进制；DXVK FIFO |

`frameNotFlat` 全红的原因（已核对）：该检查只看代理自己的帧转储，`--dump-every 100000` 只留下第 0 帧（黑）。
P0 有意不在窗口里做转储（每次转储是 121 个同步读回条带，会扰动被测帧）；进图画面的证据用控制面截图 `ab1/ab2`（`captureVerified`）并人工看了
`losttemple-r1/ab1.bmp`、`shengyusi-r2/ab1.bmp`。不把它说成视觉通过，旧失败保留。

## 5. 每世界帧拆分（进图窗口，宿主 `worldSceneEnters` 归一）

窗口：ready+12 s 到第一个截帧动作，两端为周期快照。不同运行窗口长度与背景负载不同，只作诊断，不是发布基线。

| 量（上午第一批，GPU 有其它负载） | LT FIFO | LT IMMEDIATE | Echo IMMEDIATE | 生与死 IMMEDIATE (r2，下午) |
|---|---:|---:|---:|---:|
| 世界帧 / 窗口秒 | 480 / 24.3 | 240 / 11.4 | 480 / 20.7 | 360 / 53.3 |
| 世界 fps（控制面中位） | 19.76 (19.53) | 21.13 (21.95) | 23.15 | 6.75 (7.09) |
| 平均帧时间 ms | 50.6 | 47.3 | 43.2 | 148.2 |
| 同步调用 / 帧 | 107.0 | 106.9 | 109.8 | 81.6 |
| x86 同步等待墙钟 ms/帧 | 44.85 | 41.63 | 37.22 | 82.27 |
| 　其中 TextureUnlock（Begin+Chunk+Commit） | 31.71 | 29.56 | 24.93 | 29.73 |
| 　其中 Present | 9.85 | 8.60 | 7.94 | 14.36 |
| 　其中 StageMarker（3 个/帧） | 3.03 | 3.04 | 4.02 | 35.02 |
| x86 环满等待 ms/帧 | 0 | 0 | 0 | 27.65（DrawFixedBatch 22.1，UploadBuffer 5.3） |
| x86 阻塞占比 | 0.886 | 0.880 | 0.862 | 0.742 + 环满 |
| 异步记录 KiB/帧 | 1,339 | 1,339 | 1,782 | 8,329 |
| 宿主执行占墙钟 / 等记录空闲 | 0.80 / 0.19 | 0.81 / 0.18 | 0.80 / 0.18 | 0.81 / 0.17 |
| 宿主提交 / 帧（其中纹理上传前+后） | 67.3（64.3） | 67.3（64.3） | 69.2（66.2） | 49.8（46.7） |

宿主按操作执行墙钟 ms/帧（嵌套在 x86 等待里）：

| 操作 | LT FIFO | LT IMM | Echo | 生与死 |
|---|---:|---:|---:|---:|
| CommitTextureUpload | 20.86 | 19.42 | 16.24 | 24.13 |
| Present | 9.80 | 8.55 | 7.90 | 14.33 |
| DrawFixedBatch | 5.52 | 5.72 | 5.91 | 47.13 |
| UploadBuffer | 3.38 | 3.39 | 3.51 | 31.19 |
| StageMarker | 0.57 | 0.58 | 0.78 | 1.82 |

宿主等待路径（墙钟，按种类，ms/帧；包含于上表）：

| 路径 | LT FIFO | LT IMM | Echo | 生与死 |
|---|---:|---:|---:|---:|
| SettleGpu（vkWaitSemaphores 等到最后序号） | 17.74 | 16.03 | 14.05 | 27.21 |
| Retire（DXVK 资源退休，含 CPU 工作） | 5.48 | 5.45 | 4.30 | 5.50 |
| SubmitCpu（等提交线程） | 4.57 | 3.98 | 3.37 | 3.51 |
| PresentQueue（等呈现结果，Sleep(1) 轮询，约 1 次/帧） | 1.59 | 1.50 | 1.49 | 1.52 |
| DiscardCarry（allocateStorage+invalidateBuffer+memcpy） | 3.23 | 3.24 | 3.35 | 30.19 |
| CasterCopy（整个 ShadowBeforeUi::addCaster） | 2.66 | 2.69 | 2.81 | 36.48 |

按调用点（等待路径 + 提交等待，ms/帧）：LT IMM `TextureUploadBefore` 10.04、`TextureUploadAfter` 8.24、`PresentBefore` 6.64、`Present` 1.66；
生与死 `TextureUploadBefore` 15.91、`PresentBefore` 12.36、`TextureUploadAfter` 7.31。

### FIFO 与 IMMEDIATE 分开报（Lost Temple）

| | FIFO（旧 DLL） | IMMEDIATE（新 DLL） |
|---|---:|---:|
| 世界 fps | 19.76 | 21.13 |
| Present x86 ms/帧 | 9.85 | 8.60 |
| 宿主 Present 执行 ms/帧 | 9.80 | 8.55 |
| PresentBefore SettleGpu ms/帧 | 7.41 | 6.46 |
| PresentAfter（SettleGpu+Retire+PresentRetire）ms/帧 | 0.27 | 0.11 |
| Acquire ms/帧 | 0.0008 | 0.0011 |
| PresentQueue ms/帧（Sleep(1) 次数/帧） | 1.59（1.01） | 1.50（1.00） |
| TextureUnlock x86 ms/帧 | 31.71 | 29.56 |

两轮背景负载、窗口长度不同，差值不能认证为 IMMEDIATE 的净收益（推断）。同会话重测与帧时间分布见 §6。

## 6. 第二批二进制：同会话 FIFO/IMMEDIATE、帧时间、计时关闭对照

| 量（LT，16:20–16:24 连续三轮） | IMMEDIATE `losttemple-r2` | FIFO `losttemple-fifo-r2` | 计时关闭 `losttemple-timingoff-r1`（IMMEDIATE） |
|---|---:|---:|---:|
| 世界帧 / 窗口秒 | 600 / 21.3 | 480 / 19.7 | —（无周期快照） |
| 世界 fps（控制面中位） | 28.14（30.22） | 24.35（27.92） | （30.75） |
| 帧时间 p50 / p95 / p99 / 最大 ms | 29.68 / 42.10 / 79.58 / 1,049.7 | 30.52 / 48.12 / 227.2 / 1,158.6 | — |
| ≥50 ms / ≥100 ms 帧数 | 18 / 5 | 20 / 11 | — |
| ~1 s 卡顿帧（≥0.9 s）次数，合计 | 2，2.09 s | 3，3.26 s | — |
| x86 同步等待 ms/帧（TextureUnlock / Present / StageMarker） | 30.89（21.05 / 7.42 / 2.29） | 35.87（26.58 / 6.41 / 2.68） | — |
| 宿主执行占墙钟 / 空闲 | 0.80 / 0.19 | 0.74 / 0.25 | — |
| PresentBefore SettleGpu / PresentQueue ms/帧（Sleep(1)/帧） | 5.39 / 1.50（1.00） | 4.16 / 1.66（1.09） | — |
| 纹理更新 / 帧 | 32.1 | 32.1 | — |

帧时间 = x86 游戏线程两次 Present 返回之间（含快照写入，约 0.1 ms/120 帧）。FIFO 下 PresentBefore 反而略短、PresentQueue 略长，Acquire≈0：
两种模式的中位帧相同，FIFO 的平均与尾部更差主要来自 ~1 s 卡顿与 100–300 ms 级的帧（已核对分布；归因推断）。
宿主确认：r2 `syncInterval=2147483648`、DXVK `VK_PRESENT_MODE_IMMEDIATE_KHR`；fifo-r2 `VK_PRESENT_MODE_FIFO_KHR`；计时关闭轮 IMMEDIATE。

~1 s 卡顿的证据链（已核对）：卡顿帧所在的每个快照区间里，`TextureUploadChunk` 同步墙钟 ≈ 1,010 ms × 卡顿帧数（如 presents 840 区间 2 帧、chunk 2,023 ms），
单次最大 1,006–1,126 ms（各轮），而宿主执行 Chunk 的 ≥1 ms 次数为 0；`d9::ring::RecheckMs=1000` 是宿主停车等记录与 x86 停车等回复两边的超时。
代码看过的握手（`setParked`/`tail`、`setWaiting`/`applied` 都是 seq_cst）在 C++ 语义下没有明显漏洞——所以“哪一侧漏唤醒”还没答案（推断，需要上面说的超时计数）。

## 7. 退出超时（echoisles-r1、shengyusi-r2）

前端红项是真的：Finish 回复后前端等宿主进程退出 5 s，超时，记 `local-contract`、`hostExit=259`（仍在运行）。宿主随后自己正常结束。时序（宿主里程碑 + QPC，已核对）：

| 运行 | Finish 回复→退出报告写完（before-session-reset） | wmain 结束后到进程退出 | 前端等待 |
|---|---:|---:|---:|
| 5.2 A/B 各轮 | 0.31–0.36 s | ≈0.09 s | 0.42–0.45 s |
| LT FIFO | 1.83 s | ≈0.14 s | 1.97 s |
| LT IMMEDIATE | 0.58 s | ≈0.11 s | 0.69 s |
| Echo | 1.49 s（其中退出快照之后的退出报告 fprintf 1.45 s） | **>3.56 s** | 5.05 s 超时 |
| 生与死 r2 | 0.38 s | **>4.6 s** | 5.00 s 超时 |

- 我的退出快照本身：宿主每次快照写入约 0.11–0.13 ms（`profileWriteUs` 增量，已核对），退出快照写在 Finish 回复后约 35 ms（QPC 与 tick 对齐，已核对）。不是它卡住。
- 两次超时的大头在 **wmain 返回之后**（CRT/DLL 卸载、驱动线程收尾），这一段没有任何 P0 代码，也没有计时；原因未知。
  当时机器上其它 agent 在构建/跑门（CPU 30–40%），宿主继承 BelowNormal；这是推断，未证实。Echo 那次退出报告阶段也慢（1.45 s，unbuffered stderr 写文件），同样只能推断为 I/O/调度争用。
- 没有改 5 s 期限，也不拿宿主的最终 exit 0 覆盖前端红项。

## 8. 自检恒等式（全部成立，已核对）

每个代理运行的设备会话：
- 环记录 = uploadsPublished + statesPublished + flushedBatches + annotation messages + syncCalls + clearPublished；
- 宿主 submitted = streamSubmissions + 纹理 Commit 数 + texturesCreated + clears + presentAttempts + readbacks + 1；
- 剖析自身：Σ(同步+异步) = 已发布记录；Σ同步 − 环 syncCalls ∈ [0, 管道期 6 条]；Σ按调用点提交 = submitted；Σ宿主按操作执行 = accepted；纹理首传+更新 = 纹理 Commit 数。
例：LT FIFO 环记录 735,041 = 435,076 + 62,525 + 79,202 + 27,175 + 129,697 + 1,366；宿主提交 77,416 = 34,709 + 37,472 + 2,019 + 1,366 + 1,365 + 484 + 1。

## 9. 各后续阶段能拿走多少（以 LT IMMEDIATE 47.3 ms/帧为例；全部是推断上限，不能相加）

- **P1 宿主等待**：纹理上传前后的等待路径 18.3 ms/帧 + Present 前 settle 6.6 ms/帧 + 其它约 2 ms；这些嵌套在 x86 的 TextureUnlock/Present 等待里。
  只去掉 x86 往返而不动宿主等待，宿主执行仍约 38 ms/帧（宿主执行占 80%），帧时间下限仍近 38 ms。
- **P2 异步调用 + Present 额度**：TextureUnlock 29.6 + Present 8.6 = 38.2 ms/帧 x86 阻塞；要兑现必须与 P1 一起（每次更新的宿主 settle 不去，宿主就是新瓶颈）。
- **P3 调度/功耗/唤醒**：先修 ~1 s 的 `TextureUploadChunk` 漏唤醒（LT 窗口 10–17% 墙钟，§6）；Present 两段 Sleep(1) 合计约 1.5 ms/帧；每帧几十次小提交、GPU 时钟样本偏低（运行前 210–585 MHz 仅是快照，未连续采样）。
- **P4 状态增量**：LT/Echo 环满为 0，P4 只省宿主解码（DrawFixedBatch 约 5.7 ms/帧）；生与死 DrawFixedBatch 异步 4.26 MB/帧、环满 22 ms/帧，P4 能直接减环满。
- **生与死另算**：宿主 CPU 路径先行——CasterCopy 36.5 ms/帧、DiscardCarry 30.2 ms/帧（墙钟路径，包含分配与整个 addCaster），x86 的 StageMarker 35 ms 与环满 27.7 ms 是在等宿主追上。

## 10. 纹理更新是谁（能说到哪一步）

已核对：
- LT 四轮完全相同的 32 个逻辑 id（387–391、427–432、467–473、506–513、549–553、593），Echo 是 713–846 的另一组 32 个，生与死 10346–10709 的一组；
  都是 16×16、格式 21（A8R8G8B8）、1 层、`D3DPOOL_MANAGED`、usage 0、每帧每张一次整层更新（1,024 B，1 块）。id 呈每组 5–8 个连续、组间相隔约 40 的格局。
- 第二批记录了创建调用点：32 张全部由 **`Game.dll+0xF0408`** 调 `CreateTexture`（返回地址）。按仓库的 1.27 基址约定即 `0x6F0F0408`，
  落在已记录的 Game.dll D3D9 后端函数群旁（`0x6F0EF2D0`、`0x6F0EFB60`、`0x6F0F01E0`，见 `docs/research/2026-09-14-native-light-abi-correction.md`）——
  也就是引擎自己的通用建纹理包装，不是具体功能；要再往上一层需要调用栈（Game.dll 无帧指针，直接回溯不可靠）。
- 每次更新前最后一个 StageMarker 都是 5（`WorldSceneEnter`，`war3_effect_scene.h` 的 Stage 枚举）：这些更新发生在进入世界场景之后、下一个标记之前。

不命名为战争迷雾/地形阴影等：现有证据只到“引擎 D3D9 后端的通用建纹理点、按地图固定的一组 16×16、每帧在世界场景开头整层重写”。

## 11. 计时口径与限制

- `WARVK_D3D9_SYNC_PROFILE_EVERY` 未设只停周期输出；热路径计时由 `WARVK_D3D9_SYNC_PROFILE_TIMING=0` 关闭（第二批二进制起；计数仍累加，只是没有 QPC 调用）。
- 帧时间来自 x86 游戏线程两次 Present 返回之间（第二批二进制起），每个快照带上一个快照以来的全部帧（≤512）。
- 首批 LT/Echo/生与死数字来自第一批二进制，没有帧时间分位；不能拿 2 s 斜率窗口认证 p99。
- 分析器：没有 ready 或窗口里没有世界帧时 `sampleValidForGameplay=false`，`worldFps` 置空，Present 频率只以 `loadingPresentFps`（加载期诊断）给出；
  宿主列名改为“等待路径墙钟 / 提交等待 / CPU 路径墙钟（嵌套）”，不再叫 GPU 时间。

## 12. 没做的

- **E0（Present 的 Sleep(1) 改阻塞等待 + 1 ms 计时精度）没有做。** 实测上限：LT/Echo/生与死每帧 `PresentQueue` 约 1.5 ms、约 1 次 Sleep(1)，`PresentRetire` ≤0.16 ms——
  占 LT 帧时间约 3%，远小于纹理上传路径；加上今天 `game` 锁排队很长，同一会话交替 A/B 做不出可信差值。留作 P3 的小项。
- 冻结门轮（阴影/特效/GPU 参考/切图）按要求没跑。
