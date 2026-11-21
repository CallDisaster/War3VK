# x64 命令环约 1.01 s 停顿：丢失唤醒的定位与开关修复

日期：2026-11-17。范围：`tools/render_host/d3d9_connection.h`（x86 前端，生产者）与 `tools/render_host/dxvk_core/d3d9_host_main.cpp`（x64 宿主，消费者）。修复在开关 `WARVK_D3D9_RING_WAKE_FIX` 之后，默认关闭（行为与现状相同）。唤醒协议属于传输语义，改默认值前须经 Fable 审核。没有改动 RecheckMs（1000 ms）、无进展上限（10 s）、5 s 头部期限、线格式或故障语义。

## 1. 结论

停顿发生在**前端（生产者）**，位置是 `ringCall` → `ringAwait` 的首轮调用，属于判定条件与基线的先后竞态，不是内存序问题。

1. `ringCall` 先用 `while(ringObserve().applied<h.sequence)` 检查条件（d3d9_connection.h:978）。
2. 宿主恰好在这次检查之后、`ringAwait` 首轮 `ringObserve()` 之前发布了该记录的 `applied`。
3. 首轮调用把这个已满足条件的读数记为基线（`wait.applied==h.sequence`），然后自旋、置 `waiting`、再看一次，都只和这条新基线比较：
   - 宿主此时已经空闲（它在等前端的下一条记录），不会再有变化；
   - 宿主发布时生产者尚未置 `waiting`，按协议也不发信号。
4. 结果是前端睡满 `RecheckMs`，超时后 `ringCall` 重新判定条件，立刻通过。

这就是约 1.01 s 停顿的来源。宿主侧的 Dekker 握手（发布 `applied` → 读 `producerWaiting`）本身是对的：被测到的每一次停顿，宿主回复时读到的 `producerWaiting` 都是 false。错在前端拿已经满足条件的值当作“等待变化”的起点。

`ringPublish` 等环空间时（d3d9_connection.h:949）用的是同一个 `ringAwait`，有同样的窗口。不过环满时宿主通常还有积压，命中概率更低；本轮实测的 9 次停顿都在 `ringCall`。

## 2. 计数器（两侧，默认采集，只读窥视）

**前端**（`d9::profile::WakeLog`，sync-profile 的 `wake`）：
- 每次 `ringAwait` 睡眠的结束原因：被通知、其它句柄、超时。
- 超时时窥视 `applied`/`head`；记录事件 `[kind,status,op,sequence,before,after,peerFlag,signaled,beginQpc,endQpc,us]`：
  - `before` 是基线 `wait.applied`；
  - `peerFlag` 是宿主当时是否 parked；
  - `signaled` 是本端最后一次发布时有没有发 `work` 信号。

**宿主**（host-profile 的 `wake`）：
- 每次 park 的结束原因。
- 同步回复时 `producerWaiting` 的读数和是否 `SetEvent`（`notifyChecks`/`notified`）。
- 从同步回复到下一条记录 ≥500 ms 的“慢周转”事件：`signaled` 是那次回复有没有发信号，`peerFlag` 是下一条记录到达时 `producerWaiting` 的值。

QPC 跨进程可比，两侧事件可以逐条对齐。

## 3. Lost Temple 复现（wake-r1）

**证据目录**：`AutoTest/artifacts/x64-lost-wakeup-20261117-wake-r1/`，150 s。
- 宿主：`build64_render_host_p0_20261117_dev`，`9ecf4a47b2c8e6c1`（SHA-256 前 16 位）。
- 前端：`b8944a94af92483f`。
- 分析：`scratchpad/wake_analyze.py`。

| 计数 | 前端 | 宿主（游戏宿主 host.log.2） |
|---|---|---|
| 睡眠 | 882,933 | 31,529 |
| 被通知唤醒 | 882,924 | 31,516 |
| 超时 | 9 | 13 |
| 超时时进展已发布（相对基线） | 0 | 0 |
| 同步调用 / 同步记录 | 484,843 | 484,843 |
| 慢周转（≥500 ms） | — | 10 |

前端的 9 次超时全部是：
- `before == after == 被等待的 sequence`，即基线已经等于所等记录；
- `peerFlag=1`（宿主已 park）；
- 时长 1,002.7–1,014.2 ms。

按 op 分：TextureUploadChunk 7 次、BeginTextureUpload 1 次、FrameScene 1 次。

每一次都对应一个宿主“慢周转”事件：
- 同一 sequence，宿主回复时刻比前端睡眠开始早约 40 µs；
- 宿主回复时 `signaled=0`（`producerWaiting` 为 false）；
- 下一条记录到达时 `peerFlag=1`（前端已在睡）。

宿主自己的 13 次 park 超时都没有待读工作，是前端停住造成的空闲，属于结果而不是原因。

第 10 个宿主慢周转在加载期（CommitTextureUpload，1,897 ms，`signaled=1`）：前端那时在忙别的，没有超时事件，与本问题无关。

**量级**：
- 约每 5.4 万次同步调用一次；
- 本轮合计约 9.08 s，占 150 s 运行的 6%；
- 每次落在一帧上，就是一个约 1 s 的卡顿帧。

## 4. 修复（`WARVK_D3D9_RING_WAKE_FIX`，默认关）

**取值**：`1` 常开；`alternate:N` 按 sync-profile 帧数每 N 帧切换，奇数块开启（需要 profile 打开）；其它值或未设置时关闭。只读一次。

**改法**：`ringAwait` 首轮只取基线就返回，由调用者再判定一次条件。下一轮比较的基线因此早于调用者最近一次判定：
- 发布落在“取基线→判定”之间：判定直接看到；
- 落在判定之后：下一轮看到相对基线的变化。

不会再出现“基线已满足却去睡”的情况。

不变的部分：
- 无进展计时仍从首轮开始（`wait.moved`）；
- 健康检查、宿主故障检查、期限检查都还在第二轮执行，调用者循环里也照旧判定 `healthy()`/宿主状态；
- 代价是每次等待多一次 `ringObserve` 和一次循环判定。

**新计数**：sync-profile 的 `wakeFix.modes[phase] = [waits, earlyReturns, sleeps, timeouts, satisfiedTimeouts, timeoutUs]`。`satisfiedTimeouts` 是“超时且基线已满足所等记录”的次数。开启相位按构造必须为 0。

**夹具验证**：`x64-p1-staging-slices-20261117-order-r3` 在开关为 `1` 时 4 个分支全部通过，pixelHash 与参考一致（`13c9a69ab106d325`）。每个代理分支约 16.7k 次等待、16.7k 次首轮返回、0 次超时。

## 5. 同一运行交替 A/B（Lost Temple）

**证据**：`AutoTest/artifacts/x64-lost-wakeup-20261117-wakefix-ab-r2/`，150 s，`WARVK_D3D9_RING_WAKE_FIX=alternate:150`。
- 前端：`scratchpad/fixture_build/proxy_wakefix.dll`，`e9599d5be6502ccd`。
- 宿主：`build64_render_host_p1_20261117_dev`，`a9342ae65becca82`；该宿主内的其它开关全部未设。
- 运行前 CPU 负载约 4%。

（r1 因代理路径写成相对路径，探针启动前即报 missing path，没有运行。）

| 指标 | 关 | 开 |
|---|---|---|
| ringAwait 等待 | 208,798 | 201,077 |
| 首轮直接返回 | 0 | 201,077 |
| 睡眠 | 368,572 | 357,205 |
| 超时 | 4 | **0** |
| 超时且基线已满足（丢失唤醒） | 4（合计 4.03 s） | **0** |
| 前端 >900 ms 帧 | 5 | **0** |
| 前端稳态帧均值 ms | 39.82 | 35.68 |
| 前端稳态 p50 / p95 / p99 ms | 35.39 / 50.71 / 75.02 | 34.45 / 44.30 / 59.78 |

关闭相位的 4 次超时仍然全部是 TextureUploadChunk、基线等于所等 sequence，与 wake-r1 的机制相同；开启相位一次也没有。

p50 基本不变，这符合预期：修复只去掉偶发的约 1 s 停顿，不改变常规帧的路径。均值和 p95 的差异里，约 2.2 ms 可以由停顿直接解释，其余属于运行噪声，不作为收益来声明。

同一宿主这轮退出也撞上了 `2026-11-17-x64-host-exit-delay.md` 的 opengl32 轮询（约 6.29 s），所以回执 `sessionsEndedClean=false`。这与本开关无关：这轮没有设置退出开关。

## 6. 未做与风险

- 默认值没有改，需要 Fable 审核唤醒协议变化。
- `ringPublish` 的环满等待已被同一开关覆盖，但本轮没有观测到它触发；需要更长或环更小（`WARVK_D3D9_COMMAND_RING_KIB`）的运行另行确认。
- 计数只证明“超时且基线已满足”在开启相位消失；帧时间受机器负载影响（本机有其它会话的 ML 负载），只看同一运行的交替块。
