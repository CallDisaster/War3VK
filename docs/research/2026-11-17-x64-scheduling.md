# x86 前端与 x64 宿主的等待/唤醒、调度与电源设置（P3 预研）

日期：2026-11-17。只读研究，没有构建、没有跑游戏/宿主/探针。代码行号以当前工作树为准。
标记：**已核对** = 读过代码/日志/文档或在本机实测；**推断** = 由代码和数字推出，未在运行中直接观测。

## 0. 一句话

等待/唤醒确实有浪费（生产者每次同步调用平均睡 1.8–5.8 次，宿主每 2–3 条记录就 SetEvent 一次），但按名义唤醒延迟算，每帧只占约 1–3 ms（失落神庙/回声群岛）、3–9 ms（生与死），不是 30 fps 对 146 fps 差距的主因。
最实在的问题有两个：一是宿主进程用 `BELOW_NORMAL_PRIORITY_CLASS` 启动；二是生产者的 2000 次自旋每次等待只跑一回，而且宿主有任何进展就退出。P3 应先补计时计数（QPC），再按开关逐项 A/B。

---

## 1. 两侧的等待/唤醒代码

### 1.1 共享对象（已核对）

- 每个连接两块共享内存，外加两个**命名自动复位事件** `work`（x86→宿主）和 `progress`（宿主→x86）：`tools/render_host/d3d9_ring_channel.h:61-66`（`CreateEventExW(..., 0, ...)`：自动复位，初始无信号）。
- `RecheckMs = 1000`：`d3d9_ring_channel.h:23`。`SpinPolls = 2000`：`d3d9_ring_channel.h:26`。
- 握手标志：生产者的 `waiting` 在 `d3d9_command_ring.h:45`，消费者的 `parked` 在 `d3d9_command_ring.h:58`。两边都用 seq_cst 的“先写标志，再看一次谓词”（Dekker 式）：`tail` 的写在 `:193`，`consumerParked()` 在 `:196`；`head`/`applied` 的写在 `:292`、`:309`，`producerWaiting()` 在 `:295`。按代码推理不会丢唤醒（已核对）。

### 1.2 x86 生产者（游戏线程）

- `ringPublish`（`d3d9_connection.h:813-843`）：写入记录。环满时循环 `tryWrite` → `++fullObservations`（`:826`）→ `ringAwait`（`:828`）。写成功后，如果宿主 `parked`，就 `++ringWakes` 并 `SetEvent(work)`（`:842`）。
  **所以代理行里的 `wakes` 是“生产者叫醒宿主”的次数。**
- `ringCall`（`:846-889`）：发布同步记录后 `++ringSyncCalls`（`:849`），然后 `while (applied < seq) ringAwait(...)`（`:851-858`）。
- `ringAwait`（`:773-810`）的实际行为：
  1. 第一次调用只记下当前 head/applied（`:776-780`）。
  2. `if(!wait.polled)`：**每个 RingWait 只自旋一回**（`:790-796`）。每次 poll 是 `YieldProcessor()`（x86 上就是 PAUSE）加一次 `ringObserve()`（两次 seq_cst 读，再做校验）。**head 或 applied 任一变化就返回**，不管是不是自己等的那条记录。
  3. 自旋用完，或者之后的每次调用，都走：`setWaiting(true)` → 再看一次 → `++ringParks`（`:803`）→ `WaitForMultipleObjects({progress, producerProgress}, min(剩余, 1000 ms))`（`:806-807`）→ `setWaiting(false)`。
  - **所以 `parks` 是“生产者真正进入内核等待”的次数**，包括立刻返回的那些（事件里留着旧信号时）。
- 后果（已核对，按代码推理）：同步记录前面通常还排着异步记录（每次同步调用平均对应 6 / 7.3 / 18.5 条记录）。宿主一处理下一条，自旋就退出了，所以 2000 次自旋在这种场景里几乎没用。之后每一步都是“睡下 → 被宿主叫醒 → 发现还没轮到 → 再睡”。
- 生产者里没有 `Sleep`、`SwitchToThread` 或 `timeBeginPeriod`（已核对：`tools/render_host` 的 d3d9 前端文件里只有 `:793` 一处 `YieldProcessor`）。

### 1.3 x64 宿主（消费者，wmain 线程）

- 环空时（`dxvk_core/d3d9_host_main.cpp:889-917`）：每次 poll 是 `tryRead` 加 `YieldProcessor`，最多 2000 次（`:903`）。用完就 `setParked(true)` → 再读一次 → `++ringParks`（`:911`）→ `WaitForMultipleObjects({work, parent进程}, 1000 ms)`（`:912`）。被唤醒后 `polls=0`，会重新自旋（`:916`）。宿主的自旋**每次进入空闲都会重新来一遍**，生产者不会。
- 执行完一条记录后：`publishApplied` 之后，只要生产者 `waiting`，就 `++ringWakes; SetEvent(progress)`（`:984`）。**每执行完一条都这样做**，不分同步和异步。
- 释放环空间：`head` 在 `tryRead` 里、执行**之前**就写回了（`d3d9_command_ring.h:292`），但通知要等这条记录执行完才发（`:984`）。环满的生产者会被唤醒，前提是某条记录执行完、并且宿主看到了 `waiting`。
- 宿主计数打在宿主日志里：`warvk-d3d9-command-ring-host/v1`（`d3d9_host_main.cpp:1067-1070`）。

### 1.4 实测计数（已核对：从 `AutoTest/artifacts/x64-phase52-ab-20261117-r1/<map>-r1-proxy/game.stderr` 和 `host.log.2` 抽取）

| r1 | 失落神庙 | 回声群岛 | 生与死 |
|---|---:|---:|---:|
| 记录数 | 1,164,359 | 1,564,270 | 3,354,066 |
| 同步调用 syncCalls | 195,650 | 213,602 | 181,753 |
| 记录/同步 | 5.95 | 7.32 | 18.45 |
| 生产者 parks | 350,562 | 522,174 | 1,059,616 |
| parks/同步 | 1.79 | 2.44 | 5.83（含环满） |
| 生产者→宿主 wakes | 20,671 | 18,534 | 22,024 |
| 宿主 parks | 18,738 | 16,859 | 21,388 |
| 宿主→生产者 SetEvent（宿主 wakes） | 507,109 | 792,977 | 1,809,726 |
| 宿主 SetEvent/记录 | 0.44 | 0.51 | 0.54 |
| 环满 fullObservations / 次数 / 累计 ms / 最大 ms | 0 | 0 | 368,044 / 38,118 / 12,664 / 16 |

r2 和 r1 的差别都在 ±10% 以内。

### 1.5 “parks ≫ wakes” 的意思（已核对）

1. **两个数方向相反，不能直接比。** `parks` 是生产者睡下的次数，`wakes` 是生产者叫醒宿主的次数。真正对应生产者 parks 的是宿主日志里的 `wakes`（宿主的 SetEvent 次数），它比 parks 还多。
2. **不是超时。** 唯一的超时是 1000 ms 复查。35 万次 park 如果都超时，就要 35 万秒，而整场才 70 s。所以这些 park 几乎都是被事件叫醒的。
3. **宿主 SetEvent 比生产者 park 多出 16–75 万次。** 生产者被叫醒、但还没清掉 `waiting` 的这几微秒里，宿主每执行一条记录就再 SetEvent 一次。自动复位事件已经有信号，这些调用白做了。而且会留下一个旧信号，让生产者下一次 park 立刻返回（这也算一次 park，但没睡）。目前没有计数能区分这种“空 park”（还不知道）。
4. 宿主自己很少睡：每帧 10–17 次（宿主 parks/帧）。这通常发生在游戏线程两次提交之间 CPU 忙超过约 50 µs 的时候。

### 1.6 自旋有多长、睡醒要多久

- 本机 PAUSE（已核对，本机实测，`D:\tmp\AppData\ADMINI~1\Local\Temp\claude\a66d1abe2490ff62\0261171f-6707-49c0-8309-6ba7b932d19e\scratchpad\research\sched\timing.py`）：每条 16–22 ns。2000 次 PAUSE 约 38–43 µs。再加上每次 poll 的两次共享读，**2000 次 poll 约 40–60 µs**（推断）。生产者在 32 位进程里读 64 位原子量：全局开了 `-msse2`（`meson.build:19-22`），GCC 应该会生成普通 SSE 读而不是 `lock cmpxchg8b`（推断，没看反汇编）。
- 睡的原语是**事件加 WaitForMultipleObjects**，不是 `Sleep(1)`，也不是 WaitOnAddress。事件唤醒不受时钟中断精度影响。精度只影响 1000 ms 复查的超时，可以忽略（已核对）。
- 本机事件往返（两线程互相 SetEvent/Wait，含 Python 开销，普通优先级）：p50 15.5 µs，p90 26.6 µs，p99 335 µs。单程大约 5–8 µs，尾部到 ~170 µs（已核对，实测；机器上同时跑着 ML，所以尾部偏重）。`SetEvent` 打到已经有信号的事件上约 1.5 µs/次（含 ctypes）。
- `Sleep(1)`（本机实测，已核对）：这台机器的全局计时器已经被别的进程拉到 1.0 ms（`NtQueryTimerResolution` current=1.000 ms），但**没有自己申请精度的进程**调用 `Sleep(1)`，中位数仍是 18–20 ms（最小 6.9 ms，最大 60 ms）。`timeBeginPeriod(1)` 之后是 1.0–2.3 ms。这和微软文档一致：从 Win10 2004 起计时精度按进程计算；Win11 起，拥有窗口的进程不可见时，系统可能忽略它的精度请求（timeBeginPeriod 和 SetProcessInformation 的文档，见第 4 节链接）。

### 1.7 每次同步调用、每帧的代价（推断，名义值，不含机器负载）

- 一次需要睡的同步调用：先自旋到宿主第一次有进展就停（微秒级）。之后每个进展步一个循环：生产者 park/wake 约 3–5 µs CPU，宿主 SetEvent 约 1.5–5 µs（**在宿主关键路径上**）。最后一次唤醒的延迟约 5–10 µs 会直接加到这次调用上。
- 每帧（用每帧同步数 ≈122 / 121 / 147，生产者 parks ≈219 / 295 / 855，宿主 SetEvent ≈317 / 448 / 1,459）：

| 项 | 失落神庙 | 回声群岛 | 生与死 |
|---|---|---|---|
| 宿主 SetEvent（关键路径） | 0.5–1.6 ms | 0.7–2.2 ms | 2.2–7 ms |
| 生产者最后一次唤醒延迟（上限） | 0.6–1.2 ms | 0.6–1.2 ms | 0.7–1.5 ms |
| 宿主被叫醒（10–17 次/帧） | ~0.1 ms | ~0.1 ms | ~0.2 ms |
| 合计约 | 1–3 ms（帧 34 ms 的 3–8%） | 1.4–3.5 ms | 3–9 ms（帧 102 ms 的 3–9%） |

- 生与死的“每帧”数据要打折扣：同一个宿主会话在 48–74 s 之间跑出了 228–365 fps（见下一段），所以它的 9,647 次 present 里有大约 7,000 次落在这一段。
- 尾部风险：宿主是 BelowNormal（第 3 节），同机 ML 线程是 Normal。CPU 吃满时，被唤醒的宿主线程要等有空闲核才能跑。它得到的唤醒加成（+1）仍然低于 Normal 的 8，所以单次唤醒可能从微秒级拖到毫秒级。代价可能远大于上表（推断，没有测到）。

**为什么说等待/唤醒不是主因**（已核对数据，推断结论）：生与死代理在 48.5–73.7 s 之间，同一宿主会话（`host.log.2`，syncInterval=1）跑出了 228–365 fps，也就是每帧 2.7–4.4 ms。这段走的也是环加 Present（每帧至少一次 `Sleep(1)`）。所以传输加 Present 的固定开销上限大约就是每帧 3 ms。失落神庙/回声群岛稳定在 28–30 fps（frameSeries 最大也只有 33–36），差的二十多毫秒主要是宿主执行本身，包括每次 Present/Destroy 时的整 GPU 排空（`d3d9_backend.cpp:438-479` 的 `settle`、`:2256`、`:2321`、destroy 的 `:2355-2356`）。这属于已知的“宿主 GPU 等待”问题，不在 P3 范围。

---

## 2. 宿主等新记录、环满时的唤醒

- 环空时：先自旋约 40–60 µs，再睡 `work` 事件（`d3d9_host_main.cpp:891-916`）。生产者只在看到 `parked` 时才 SetEvent（`d3d9_connection.h:842`）（已核对）。
- **宿主释放空间时有没有叫醒生产者？** 只是间接地：空间在 `tryRead` 时就还回去了（`d3d9_command_ring.h:292`），但 SetEvent 只在那条记录**执行完**后、并且生产者 `waiting` 时才发（`d3d9_host_main.cpp:984`）。如果宿主卡在一条很长的记录上（GPU 等待），环满的生产者要等这条记录执行完才会被叫醒（已核对）。
- 生与死环满的原因（推断）：平均每条记录约 2.4 KB（7.9 GB / 3.35 M），每帧约 2,700 条，也就是**每帧约 6.4 MB，超过 4 MB 的环**。只要宿主一卡，环就满。这是吞吐问题，不是唤醒问题；唤醒在每个环满片段的结尾大约只多 10–20 µs，整场合计 0.4–0.8 s。
- `backpressureMs` 和 `maxBackpressureMs` 用 `GetTickCount64` 计时（`d3d9_connection.h:827`、`:831-832`），粒度约 15.6 ms。所以 `max 16` 是量化出来的，不代表真有 16 ms 的单次等待；累计值 12,664 ms 只是期望无偏，单次不可信（已核对代码，结论推断）。
- 在宿主 `tryRead` 之后、生产者 `waiting` 时也补发一次 SetEvent，环满的生产者就能早一点写。这只是多发一个通知，不改共享字段和顺序，但改了“宿主何时通知”的约定。**算传输语义边界，建议交给 Fable 或经 Fable 复核**（见第 5 节）。

---

## 3. 进程/线程设置现状

| 项 | 现状 | 依据 |
|---|---|---|
| 宿主进程优先级类 | **BELOW_NORMAL**（基准 6） | `tools/render_host/owned_child.cpp:64-65`，`CREATE_NO_WINDOW\|BELOW_NORMAL_PRIORITY_CLASS`；D3D9 宿主就是用它启动的：`d3d9_connection.h:120`（已核对） |
| AutoTest 里的游戏进程 | 也是 BelowNormal，而且在**非输入桌面**上 | `AutoTest/isolated_render_desktop.py:381`（`0x00004000`）；收据里 `desktopSafety.scope=NON_INPUT_DESKTOP_LAUNCH_CONTEXT`（已核对） |
| 线程优先级 | render_host 里没有设置；DXVK 管线编译线程是 Lowest | grep 无命中；`src/dxvk/dxvk_pipemanager.cpp:124`（已核对） |
| MMCSS / 亲和性 / 电源节流 / timeBeginPeriod | 两侧都没有 | grep `tools/render_host`、`src` 无命中（已核对） |
| DXVK 的 `NtSetTimerResolution` | 只在 `dxvk::Sleep::initialize` 里调用（`src/util/util_sleep.cpp:36-59`）。宿主 `setFrameRateLimit(0,1)`（`d3d9_backend.cpp:1212`），限帧器直接返回（`util_fps_limiter.cpp` `delay()`），所以宿主不会走到这里 | 已核对代码，推断不会触发 |
| 宿主窗口 | 本轮 `surface-owner mode=peer`：直接画进游戏的 HWND，宿主自己没有窗口 | `host.log.2`（已核对） |
| 宿主的 `Sleep(1)` 轮询 | Present 里两处：`d3d9_backend.cpp:2308-2319`（等 present 结果）和 `:666-679`（等帧信号，FIFO 下要等 `vkWaitForPresentKHR`，见 `src/dxvk/dxvk_presenter.cpp:1386-1466`） | 已核对 |

- 宿主计时精度：生与死 48–74 s 每帧只有 2.7 ms，而每次 Present 至少走一次 `Sleep(1)`。所以宿主进程里**一定有模块申请了高精度计时器**（很可能是显卡驱动），否则每帧至少 6–16 ms（推断）。这不是我们自己申请的，Win11 可能在“不可见”时忽略它，存在风险。
- Windows 会不会把宿主当后台进程？微软 QoS 文档原文是“拥有窗口的进程（**或其后代**）按窗口可见/焦点状态定级”，另有“降低线程优先级可能意味着更低 QoS”的启发式（已核对：https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service）。宿主是游戏的子进程，自己没有窗口，又是 BelowNormal。
  - 产品里游戏在前台时，宿主大概会跟着拿到 High，但 BelowNormal 可能把它拉低（推断）。
  - Remix 的注释说它的服务进程会落到较低 QoS（见第 4 节）。
  - AutoTest 里游戏窗口在非输入桌面上，可以认为一直不可见，所以两边很可能都是 Low（推断）。
- 本机 CPU：**AMD Ryzen 5 5600X，6 核 12 线程，Zen 3，不是大小核**。台式机，没有电池。电源方案“高性能”，Windows 11 专业版 26200（已核对）。
  - 文档里 Low QoS 的降频/小核调度写的是“On battery”，所以在这台机器上，EcoQoS/小核问题基本不起作用（推断）。在 Intel 混合架构的笔记本上就是实打实的问题。
- 其他只读读到的设置（已核对）：
  - `Win32PrioritySeparation=2`（前台进程量子加长；AutoTest 里游戏不在前台，拿不到）。
  - MMCSS `Tasks\Games`：Scheduling Category=Medium，Priority=2，Clock Rate=10000。
  - `SystemResponsiveness=20`。
  - 没有设置 `GlobalTimerResolutionRequests`。

---

## 4. Remix bridge 对照（commit `afb025228646328613a6d234e2202517ea75f739`，2026-11-16）

- 队列：`bridge/src/util/util_atomiccircularqueue.h`，单生产者单消费者。
  - 注释 `:42-43`：等待方先自旋，再置 sleeping 标志、睡事件；对方只在标志置位时才 SetEvent。
  - `kSleepSliceMs = 2`（`:72-73`）。自旋时长 `m_spinTicks` 由 `ipcSpinMicroseconds` 换算成 QPC 刻度（`:107`）。
  - `push` 发布后 `wakeIfSleeping(readerSleeping)`（`:129-144`）；`pull` **每取走一项就**唤醒在等空间的写端（`:166-180`）。也就是说空间一释放就通知，不等执行完。
  - `wakeIfSleeping` 先做 seq_cst 栅栏，再看标志，置位才 `SetEvent`（`:233-239`）。
  - `waitUntil`（`:242-308`）：
    - **按 QPC 时间自旋**，每次 poll 一个 `YieldProcessor`；每 64 次查一次时间，并调一次 `std::this_thread::yield()`（`:259-277`）。
    - **每次调用 waitUntil 都重新自旋**，谓词就是调用者真正要等的条件（有数据 / 有空间）。
    - 之后循环：置 sleeping → 栅栏 → 再查 → `WaitForSingleObject(ev, 2 ms)` → 清 sleeping（`:282-304`）。
- 默认值：`ipcSpinMicroseconds = 100`（`bridge/src/util/config/global_options.h:421-422`），`commandTimeout = 1000`（`:378`），`presentSemaphoreMaxFrames = 1`（`:409`）。
- Present 节流：用命名信号量 `createPresent(framesAhead)`（`bridge/src/util/util_semaphore.h:73-76`）。客户端最多可以领先一帧，而不是每帧同步等 Present（推断：我们的 Present 是同步 call，`d3d9_proxy.cpp:417-429`）。
- 服务进程关掉执行速度节流（HighQoS）：`bridge/src/server/main.cpp:3732-3741`，`ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED, StateMask=0`。注释原意：前台窗口属于游戏，服务进程的线程拿到的 QoS 更低、常落在能效核上，而游戏的帧在等它。服务端没有改优先级类、线程优先级、MMCSS 或 timeBeginPeriod（grep 无命中，已核对）。
- 和我们的区别：
  - Remix 的等待谓词精确，自旋按时间、每次等待都重新来。我们的谓词是“任意进展”，自旋按次数、每次等待只有一回。
  - Remix 的同步应答走单独的回程队列，客户端只在应答到达时被唤醒（推断，没读客户端）。我们在每条记录执行完后都叫醒等待的生产者。
  - Remix 在空间释放时就唤醒写端，我们要等记录执行完。

相关微软文档（已核对原文）：
- https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod （2004 起按进程计算；Win11 起拥有窗口但不可见的进程不保证）
- https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation
  - EXECUTION_SPEED：设上就成为 EcoQoS，`StateMask=0` 就是 HighQoS。
  - IGNORE_TIMER_RESOLUTION：`ControlMask` 里放这一位、`StateMask=0`，表示“始终尊重本进程的计时器请求”。

---

## 5. P3 方案（每项都有环境开关；不设 = 现状）

顺序：先加计数（不改行为），再按收益/风险依次 A/B。全部不碰 5 s 传输头部期限、`RecheckMs`、`RingNoProgressMs` 和共享区布局。

### P3-0 计时计数（先做，不改行为）

- **前端** `d3d9_connection.h`：
  - `ringCall`（`:846-858`）：用 QPC 统计同步等待总时长和最大值，外加直方图（<10 µs、<50、<200、<1 ms、<5 ms、≥5 ms）。
  - `ringAwait`（`:790-809`）：统计自旋时长和睡眠时长。等待返回 `WAIT_OBJECT_0` 后再 `ringObserve` 一次，没有进展就记作 `parksNoProgress`。
  - `ringPublish`：环满时长改用 QPC。
  - 新加一行独立 schema `warvk-d3d9-ring-wait/v1`。冻结的 `command-ring-proxy/v1` 那行不动，沿用代码里“另起版本行”的做法（参考 `d3d9_host_main.cpp:1039`）。
- **宿主**：
  - `d3d9_host_main.cpp:889-917` 统计睡眠时长和忙碌时长。
  - `d3d9_backend.cpp` 的 `present()` 统计 Present 墙钟总时长和最大值，`:2318`、`:678` 两处 `Sleep(1)` 的循环次数。
  - 启动时实测 3 次 `Sleep(1)` 并打日志，用来确认宿主里到底有没有人申请高精度计时器。
  - 结束时打 `GetProcessTimes`（CPU 用量）。新加一行 `warvk-d3d9-host-sched/v1`。
- 这些计数能回答两件事：宿主每帧有多少时间在忙（如果接近 100%，等待/唤醒就不是瓶颈）；同步等待的分布是什么样。

### P3-1 宿主优先级类：`WARVK_D3D9_HOST_PRIORITY=below|parent|normal|above`

- **改法**：在 `d3d9_host_main.cpp` 的 `wmain` 里，`AdoptPeerDpiAwareness(parent.get())`（`:857`）之后，新增 `ApplyHostScheduling(parent)`。
  - `parent` 取 `GetPriorityClass(parent)`（父进程句柄带 `PROCESS_QUERY_LIMITED_INFORMATION`，见 `win32_transport.cpp:86`）。
  - 然后 `SetPriorityClass(GetCurrentProcess(), …)`，拒绝 HIGH/REALTIME。日志里写实际生效的类。
  - **不改 `owned_child.cpp`**：其他实验室子进程是故意 BelowNormal 的（CI 安全策略，见 `docs/plan/2026-09-16-architecture-delegation-plan.md:29`）。
- **预期**：产品里游戏是 Normal，宿主现在比它低一级。任何 Normal 的后台负载（浏览器、ML）都能抢走宿主，Present/同步调用的尾延迟会变长。设成 `parent` 就能去掉这个不对称（推断）。
  - AutoTest 里游戏本身也是 BelowNormal，所以 `parent` 等于不变。要看效果得用 `normal`。这时宿主比游戏高一级，结论只能说明方向，不能代表产品。
- **风险**：`above` 会和同机 ML 抢 CPU。默认不建议超过游戏。

### P3-2 生产者自旋：`WARVK_D3D9_RING_SPIN_US=<0..1000>`，或 `alt:<us>`

- **改法**：
  - `d3d9_connection.h` 的 `RingWait`（`:768`）加一个“上次进展的 QPC”。
  - `ringAwait` 里把一次性的 `if(!wait.polled){2000 polls}`（`:790-796`）换成：只要距离上次进展不到 N µs 就继续 PAUSE 自旋；看到进展就更新时间并返回（调用方重查谓词，下一次调用还会接着自旋）；超过 N µs 没有进展才走原来的 `setWaiting` → 再看一次 → park（`:797-809`，原样保留）。
  - 环境变量在 Connection 构造时读一次。不加 `SwitchToThread`（会把整个时间片让出去）。
- **预期**（推断）：
  - 只要宿主在持续推进，生产者就不睡。parks/同步从 1.8–5.8 降到 <0.5。
  - 宿主的 SetEvent 随之大幅减少，宿主关键路径每帧省 0.5–3 ms（生与死更多）。
  - 每次同步调用少一次唤醒延迟（名义 5–10 µs，负载下更多）。
  - 宿主卡在长记录上（GPU 等待、Present）时，超过 N µs 照样睡，不白烧。
- **风险**：游戏线程在宿主忙时会空转。每帧空转上限约等于宿主执行同步前缀的时间。共享机器上会抢 ML 的 CPU，SMT 兄弟线程会略慢。建议候选值 0 / 50 / 100 / 200。
- **传输语义**：标志、顺序、事件、期限、布局都不变，只改本地等待策略，可以由 Opus 做。但改的是传输核心文件，默认打开前建议 Fable 过目。

### P3-3 电源节流与计时器：`WARVK_D3D9_HOST_POWER=1`，`WARVK_D3D9_HOST_TIMER_MS=1`

- **改法**：放在同一个 `ApplyHostScheduling` 里。
  - `SetProcessInformation(ProcessPowerThrottling, {Version=CURRENT, ControlMask=EXECUTION_SPEED|IGNORE_TIMER_RESOLUTION, StateMask=0})`。
  - `TIMER_MS=1` 时调用 `timeBeginPeriod(1)`（`dxvk_core/meson.build` 给宿主链接 winmm），退出前 `timeEndPeriod`。返回值写进日志。
- **预期**：在本机（5600X、台式、交流电、高性能方案）EXECUTION_SPEED 基本无效果（推断）。计时器这一项是保险：让 Present 里的 `Sleep(1)` 不依赖别的模块碰巧申请的精度，也不被 Win11 的“不可见即忽略”影响。在混合架构笔记本上有实际收益（Remix 先例）。
- **风险**：很小。本机全局计时器本来就在 1 ms。

### P3-4 Present 里的 `Sleep(1)` 轮询换成阻塞等待：`WARVK_D3D9_HOST_PRESENT_WAIT=sleep|block`

- **改法**（`d3d9_backend.cpp`）：
  - `present()` 里 `:2308-2319` 改成 `owner.device->waitForSubmissionUntil(&owner.presentSubmission, deadline)`。这个接口已经有了（`src/dxvk/dxvk_device.cpp:670-676`），本文件 `:389` 也在用。
  - `waitPresentRetired()` 里 `:666-679` 需要带超时的等待。`sync::Fence` 目前只有无限等（`src/util/sync/sync_signal.h:109-117`），可以加一个只增不改的 `waitUntil(value, deadline)`（`m_cond.wait_until`）。注意这个头文件 x86 产品 DLL 也在用。
  - 用 50 ms 一片的分片等待，每片之间照旧检查 `healthy()`，保留原来的 5 s `CpuWait` 语义和报错文字。
- **预期**：每次 Present 省掉 `Sleep(1)` 的 1–2 ms（精度正常时），精度被忽略时省掉 15.6 ms。在 30 fps 下是 3–6%，在高帧率段（生与死 48–74 s）是 30–50%（推断）。

### P3-5（可选，排最后）

- **宿主主线程优先级**：`WARVK_D3D9_HOST_THREAD=above`，只设 wmain 消费线程，DXVK 的提交/完成线程先不动。
- **宿主自旋改成按时间**：`WARVK_D3D9_HOST_SPIN_US`，改 `d3d9_host_main.cpp:903`。宿主每帧只睡 10–17 次，收益小。
- **不建议做**：
  - MMCSS（本机 Games 任务会把线程提到实时段，配合自旋可能饿死系统线程，推断）。
  - 线程亲和性（共享机器上，调度器比我们更清楚哪里有空）。
  - HIGH/REALTIME 优先级类。

### 属于 Fable 的（改传输语义，Opus 不做）

- 宿主少发 SetEvent（只对同步记录发、按空间阈值发，或者用“park 纪元号”去重）。这需要新的共享字段或新的唤醒约定。做错就丢唤醒，代价是 1 s 复查。
- 生产者告诉宿主“我在等第几号 / 需要多少字节”（新共享字段）。
- 宿主在 `tryRead` 释放空间后也通知（只是多一个通知，但改了通知约定，至少要 Fable 复核）。
- Present 异步化 / 允许领先若干帧（Remix 的 present 信号量做法），这同时也是“宿主 GPU 等待”问题的正解方向。
- 任何对 `RecheckMs`、`RingNoProgressMs`、5 s 头部期限的修改：**不改**。

### 怎么测（机器噪声大）

1. **同一场内交替**：自旋预算、计时器、优先级、Present 等待方式都能在运行中切换。开关值设成 `alt`，每 2–5 s 切一次相位（按 `GetTickCount64`），每个相位单独累计 Present 数、Present 时长、同步等待直方图和 parks。用相邻相位的成对差的中位数下结论，不受机器负载缓慢漂移影响。每个相位同时记录 `GetSystemTimes` 的整机 CPU 占用，用来标注噪声。
2. **跨场 ABBA**：只能在启动时生效的项按 A B B A 的顺序在同一张图上跑，每场 70 s，做 2 轮。比较 frameSeries 中位数和宿主计数。
3. **护栏指标**：游戏和宿主的 CPU 时间（`GetProcessTimes`）不超过基线加 X%；parks/同步；宿主 SetEvent/记录；`parksNoProgress`；同步等待 p50/p99。
4. AutoTest 里游戏是 BelowNormal、在不可见桌面上，**QoS 和前台相关的效果在 AutoTest 里看不到**。只有改测试框架（游戏用 Normal、放在可见桌面）才能测，这不在 P3 范围。

---

## 给计划的结论

1. 等待/唤醒浪费是真的，但量级每帧约 1–3 ms（失落神庙/回声群岛）、3–9 ms（生与死），只占帧时间 3–9%。30 对 146 fps 的大头在宿主执行和 GPU 排空（证据：同一宿主会话在生与死 48–74 s 跑到了 228–365 fps）。P3 的预期收益要定得保守。
2. 最该先修的是**宿主 BelowNormal 启动**（`owned_child.cpp:65`），在宿主内用 `WARVK_D3D9_HOST_PRIORITY=parent|normal` 自设。这在产品里直接影响尾延迟，改动只有几行，Opus 可以做。
3. 第二是生产者自旋改成**按时间、有进展就续、每次等待都重来**（`ringAwait`，`WARVK_D3D9_RING_SPIN_US`）。parks 和宿主 SetEvent 应该同时大降。不改语义，Opus 可以做，默认打开前请 Fable 过目。
4. 电源节流退出加 `timeBeginPeriod(1)` 是低成本保险（Remix 也这么做）。本机不是大小核，预期没有可测收益。
5. Present 里两处 `Sleep(1)` 轮询换成阻塞等待，现有接口够用，只有 Fence 需要补一个带超时的等待。属于宿主后端改动，不碰传输。
6. 一切都先加 QPC 计数（`warvk-d3d9-ring-wait/v1`、`warvk-d3d9-host-sched/v1`），用同场相位交替的方式测。`backpressureMs/maxBackpressureMs` 是 15.6 ms 粒度，不能拿来判断单次等待。
7. 宿主少发 SetEvent、空间释放即通知、Present 异步化都属于传输语义，留给 Fable。5 s 头部期限不动。

## 还不知道的

- 宿主里是谁申请了高精度计时器。只是从 365 fps 推断有人申请了，很可能是驱动；P3-0 的启动时 `Sleep(1)` 自测可以确认。
- BelowNormal 加上同机 ML 负载时，真实的唤醒延迟分布。现在只有 Python、普通优先级下的单次测量：p99 单程约 170 µs。
- 宿主每帧有多少时间在忙、多少时间在睡；有多少 park 是被旧信号弹回来的。现在都没有计数。
- 产品里（游戏在前台）宿主的有效 QoS。按文档的“后代”规则应该是 High，但 Remix 实测说偏低，没有公开 API 可以直接查。
- 32 位 GCC 对 64 位 seq_cst 原子读的实际指令（没看反汇编）。
- 非输入桌面上 FIFO 加 present-wait 的实际节奏（游戏内路径 164 fps，说明不受刷新率限制，原因未查）。
- Remix 客户端同步应答具体怎么等（没读客户端代码）。
