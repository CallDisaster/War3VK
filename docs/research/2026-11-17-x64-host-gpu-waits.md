# x64 主机里所有等 GPU / 强制提交的地方，以及怎么去掉（P1 准备）

日期：2026-11-17。只读调研，没有构建、没有跑游戏或 host。
标记：**已核对** = 我在代码或产物数据里亲眼看到；**推断** = 由代码/数据推出来，但没测过。
代码行号以当前工作区为准（工作区有很多别人未提交的改动，行号可能漂移）。
数据来自 `AutoTest/artifacts/x64-phase52-ab-20261117-r1/<map>-r1-proxy/`（`host.log.2`、`game.stderr`、`logs/*.log`）和该目录的 `receipt.json`。

---

## 0. 一页结论

1. **x64 主机现在的模型是“任何非绘制操作都先把 GPU 全部排空”**。`settle()` = 提交当前流式命令列表 + `vkWaitSemaphores` 等到最后一个提交完成 + 等 DXVK 回收线程把**所有**资源（16,320 个贴图槽逐个扫）标记为不在用。贴图上传、建贴图、删资源、Clear、读回、Present、改 LOD 都会走它，很多还走两遍（前一次 `settle`，后一次 `submit(true)` 里又 `settle`）。（已核对，见 §2、§3）
2. **提交次数已经能逐项对账，精确到 1**：Lost Temple 119,277 次提交 = 流式列表 55,263 + 贴图区域上传 57,273 + Clear 2,119 + Present 2,118 + 建贴图 2,019 + 读回 484 + 初始化 1。Echo Isles、生与死同样精确对上。（已核对，§4）
3. **元凶是贴图区域上传**：LT 每次 Present 约 27 次贴图上传，每次上传 = 关掉流式列表（1 次提交）+ 自己的拷贝列表（1 次提交）+ 最多 2 次全量 GPU 排空 + 3 次全槽扫描。每帧约 56 次提交里约 54 次是它造成的。它也占了同步调用的 88%（每次上传 Begin/Chunk/Commit 至少 3 次同步往返）。（提交数已核对；88% 的拆分是推断，见 §5）
4. 任务表里“texture locks (x86 side) 4,126”**不是**贴图上传总数，只是“第一次锁一个未初始化 mip、走 upload window”的次数（`d3d9_proxy.cpp:903-905`）。真实的主机贴图区域上传是 57,273 / 63,281 / 39,807。（已核对）
5. **Present 是完全同步的，而且两边 present mode 不一样**：代理模式 host 用 `VK_PRESENT_MODE_FIFO_KHR`（垂直同步），进程内用 `IMMEDIATE`（已核对，日志）。host 的 `present()` 要等本帧的 present-wait（真正上屏）完成才回复 x86，中间还有两个 `Sleep(1)` 轮询，而 host 进程没有提高计时器精度（日志里没有 “Setting timer interval”）。这本身就可能把帧率钉在 ~30。**A/B 门禁目前不是同条件比较。**（present mode 已核对；对 fps 的影响是推断）
6. DXVK 自己的 D3D9（本仓库 `src/d3d9`）对上述每一项都**不等 GPU**：上传用 staging 环（4 MiB 块，靠命令列表引用计数续命）+ 录进同一个上下文 + 启发式提交；建贴图只录 `initImage`；删除靠引用计数；Present 录进同一列表、只按帧延迟（War3 实际为 2 帧）等。只有读回、超出 staging 预算这两种情况才等，而且只等特定值，不排空全部。（已核对，§7）
7. P1 的设计就是把 host 改成 DXVK D3D9 那套：**一个一直开着的录制列表 + staging 环 + timeline 记账 + 延迟回收 + GpuFlushTracker 决定何时提交 + Present 按 2 帧延迟节流**；再把代理端贴图上传改成异步有序消息。可以分 8 步做，前 5 步 Opus 能机械完成，涉及“谁拥有、何时释放、错误何时报”的 4 项留给 Fable。（§9、§10）

---

## 1. 口径

- 代理运行开了 `WARVK_D3D9_STREAM=1`、`FIXED_BATCH=1`、`ASYNC_BATCH=1`、`COMMAND_RING=1`、`ANNOTATIONS=1`（已核对，`receipt.json` 的 `childEnvironment`）。所以 host 走的是“stream profile”：绘制录进一个开着的列表，不是每批一提交。flight/group 两个旧 profile 没开（`flightCapacity:1`、`groupedSubmissions:0`，已核对 `host.log.2:8`）。
- 下面“每 Present”= 总数 / host 的 `presentAttempts`（LT 2,118，EI 2,249，生与死 9,647）。生与死只有 636 个世界场景帧，绝大多数 Present 在加载期，所以它的“每 Present”数字偏低，别直接拿来比。“每 pace 帧”用任务里给的 1,600 / 1,770 / 1,240（推断口径：假设事件都发生在游戏帧里，会偏高）。

---

## 2. 现在的同步原语（全部在 `tools/render_host/dxvk_core/d3d9_backend.cpp`）

| 原语 | 位置 | 做什么 | 代价 |
|---|---|---|---|
| `resourcesInUse()` | 354-376 | 17 个内部缓冲 + 4 个 flight + 色/深/交换链图 + **8 个缓冲槽 + 16,320 个贴图槽**逐个 `isInUse` | 每次扫一个约 5 MB 的 `std::array<Texture>`（每个 `Texture` 含 11 级 `TextureLayout`，约 330 B；`d3d9_backend.h:49`、`d3d9_texture_wire.h:23-28`）。已核对结构；时间是推断 |
| `waitRetired()` | 408-414 | `device->waitForResourceRetirementUntil(!resourcesInUse())`，等 DXVK 回收线程 | 一次跨线程唤醒 + 至少一次全槽扫描。`waitForResourceRetirementUntil` 就是 `m_submissionQueue.synchronizeUntil`（`src/dxvk/dxvk_device.h:748-752`） |
| `settle()` | 438-479 | `flushRecordedGroup` + `flushStream`（会提交）+ 等 flight + 若有 pending 则 `vkWaitSemaphores(completion, lastSerial)` 等**全部**提交 + `waitRetired()` | 一次全量 GPU 排空 + 回收。没有 pending 时也要走 `waitRetired()`（448-451） |
| `begin()` | 685-691 | `settle()`，然后新建命令列表 | 非流式操作都从这里开列表 |
| `submit(waitForGpu)` | 693-709 | 发 timeline 信号、`endRecording`、`submitCommandList`，**然后总是** `waitForSubmissionUntil` 等提交线程真的调完 `vkQueueSubmit`（704）；`true` 时再 `settle()` | 每次提交都和 DXVK 提交线程握一次手。原因：只有一个 `submission` 状态对象（276）被反复复用，必须等它写完才能下一次用 |
| `flushStream()` | 633-664 | 流式列表 `submit(false)`；若未完成的流式提交超过 `MaxStreamLatency=3`（`d3d9_backend.h:58`）就 `vkWaitSemaphores` 等到 `lastSerial-3` | 三张图里 `latencyWaits:0`——因为别处的 settle 早就排空了 |
| `waitPresentRetired()` | 666-683 | 轮询 `presentSignal->value()`，不到就 `Sleep(1)`（678） | 见 §6 |

说明：`settle()` 和 `submit()` 的注释里写明这是有意的“一个 owner、所有东西都退休了才前进”的保守模型（187-189、631-632）。这不是 bug，是实验室阶段的正确性优先设计；P1 要做的是把它换成 DXVK 原本的生命周期模型。

---

## 3. 等待清单（每一处）

“频率”列给 LT / EI / 生与死 的总次数，括号里是每 Present。等待类型：**G** = 全量 GPU 排空（`vkWaitSemaphores` 到最后提交），**R** = 全槽回收扫描，**S** = 等提交线程，**P** = present 相关等待。

### 3.1 游戏里真的在发生的

| # | 位置（d3d9_backend.cpp） | 触发的 wire op / 代理调用 | 等什么 | 为什么等（正确性理由） | 频率 |
|---|---|---|---|---|---|
| W1 | `uploadTextureRegion` 1417 `settle` | `CommitTextureUpload`（host_main 433-442）← 代理 `Texture::UnlockRect`（d3d9_proxy.cpp 914-944） | G（若流里有绘制）+R | **唯一的 `owner.staging` 缓冲（1139-1140，1 MiB，超了就 1418-1420 换大的）会被 1421 的 memcpy 覆盖**，前一次拷贝可能还在读它（主机可见内存的写后读冲突）。另外 `begin()` 要求当前没有开着的列表（688），所以必须先关掉流式列表 | 57,273 / 63,281 / 39,807（27.0 / 28.1 / 4.1）。已核对（对账见 §4） |
| W2 | 同上 1422 `begin` → `settle` | 同上 | R（此时一般已无 pending） | 同上：一次只能有一个列表 | 同 W1 |
| W3 | 同上 1426 `submit(true)` | 同上 | S+G+R | **没有数据上的必要**：下一次上传的 W1 会处理 staging 复用；这里是“非流式操作结束时 owner 全部退休”的保守约定。`uploaded` 标志只给 CPU 校验用，不需要 GPU 完成 | 同 W1 |
| W4 | `createTexture2D` 1395 `begin`、1397 `submit(true)` | `CreateTexture2D`（host_main 402-417）← 代理 `Device::CreateTexture`（d3d9_proxy.cpp 1540-1548，同步调用） | G+R，S+G+R | `initImage` 要在第一次使用前执行——这只需要“录在前面”，不需要等 GPU。等待来自“单列表”模型 | 2,019 / 1,185 / 15,791 |
| W5 | `destroy` 2355 `settle`、2356 `waitPresentRetired` | `Destroy`（host_main 711-726）← 代理 `destroyResource`（d3d9_proxy.cpp 490-500，同步） | G+R+P | 生命周期：释放槽里的 `Rc` 前确认 GPU 不再用；host 注释写明“逻辑 ID 只在退休路径成功后才 tombstone”（host_main 718-719），代理注释也依赖这个（d3d9_proxy.cpp 498-500）。但 DXVK 的 `Rc` + 命令列表跟踪本来就保证对象活到 GPU 用完（见 §7.4） | 2,024 / 1,190 / 15,796（`logicalHandlesDestroyed`，含 3 个缓冲 2 个声明） |
| W6 | `clearRegions` 1538 `begin`、1562 `submit(true)` | `Clear`（异步，代理 `enqueueClear` connection.h 578-598）和 `ClearRegions`（同步） | G+R，S+G+R | 无数据理由，同 W4 的“单列表 + 保守退休”。讽刺的是代理端已经把全屏 Clear 做成异步，host 却在里面排空两次 GPU | 2,119 / 2,250 / 9,648（≈1/Present；三张图的 ClearRegions 实际为 0，全是 Clear） |
| W7 | `present` 2256 `settle`、2257 `waitPresentRetired` | `Present`（host_main 727-744）← 代理 `Device::Present`（d3d9_proxy.cpp 420-424，同步） | G+R+P | 流式列表要关掉才能 `begin()` 开 present 列表；上一帧的 present 必须完全退休，`acquiredImage` 才算空（2258-2259） | 2,118 / 2,249 / 9,647 |
| W8 | `present` 2283 `begin` | 同上 | R | 单列表模型 | 同 W7 |
| W9 | `present` 2299 `submit(false)` | 同上 | S | 单个 `submission` 状态对象 | 同 W7 |
| W10 | `present` 2308-2319：`Sleep(1)` 轮询 `presentSubmission.result` | 同上 | P | 要把 `vkQueuePresentKHR` 的真实结果同步回给 x86（“D3D_OK 需要 acquire、队列结果、GPU 完成和帧退休”，d3d9_backend.h 271-276） | 同 W7 |
| W11 | `present` 2321 `settle` | 同上 | G+R | 只有诊断用的 `presentedReadback`（2328-2338）真需要 blit 完成；平时是保守约定 | 同 W7 |
| W12 | `present` 2323 `waitPresentRetired` | 同上 | P（FIFO 下 = 等真正上屏） | 同 W10 的“零延迟”语义。见 §6 | 同 W7 |
| W13 | `readback` 2131 `begin`、2134 `submit(true)` | `Readback`（host_main 660-677） | G+R，S+G+R | **真需要等**：CPU 要读映射内存（读后写顺序），而且读回缓冲只有一块 64 KiB（1141-1142）。但只需要等“这次拷贝”那个 serial，不需要排空全部 | 484 / 484 / 484（门禁截图用，非游戏路径） |
| W14 | 每次 `submit()` 704 `waitForSubmissionUntil` | 所有提交 | S | 单个 `submission` 状态对象复用 | 119,277 / 131,691 / 99,853 |
| W15 | `setTextureLod` 1438 `settle` | `TextureLod`（host_main 465-469）← 代理 `Texture::SetLOD`（d3d9_proxy.cpp 878-883，同步） | G+R | 换 `slot.view`（旧 view 可能被已录制的绘制引用）。DXVK 的 view 跟着 image 活，而 image 被命令列表跟踪；DXVK D3D9 的 SetLOD 不等（§7.5） | host 没计数；和 FlushDraws 合计约 3.9K / 4.1K / 15.5K（残差，见 §5） |

### 3.2 存在但这次游戏没走到的

| # | 位置 | 何时触发 | 理由 | 这次次数 |
|---|---|---|---|---|
| W16 | `uploadBuffer` 1348-1351 `settle` | 动态缓冲非 NoOverwrite、且非流式的 Discard | 原地写主机可见内存 | 0（`uploadSettles:0`，`orderedUploads:0`，已核对 `host.log.2:10`） |
| W17 | `uploadBuffer` 1355-1361 `settle`+`begin`+`submit(true)` | 静态（非 dynamic）缓冲上传 | 单 staging | 0（游戏只有 3 个动态缓冲） |
| W18 | `drawFixedBatch` 1932/1943、`drawBatch` 1985/1997 | 非流式 profile | 单个 FF UBO / 常量缓冲 | 0（流式） |
| W19 | `beginDrawFlight` 497、510-531；`flushRecordedGroup` 567 | flight/group profile | 4 个 flight 槽复用 | 0 |
| W20 | `runBeforeUi` 2087/2100 | 非流式 profile | 同 W6 | 0（流式下 2086 直接录进开着的列表，**这正是 P1 要推广的做法**） |
| W21 | `flushStream` 645-657 | 流式提交领先 GPU 超过 3 个 serial | 限制在途的重命名常量存储 | 0（被 settle 掩盖；P1 后会变成主要节流点，见 §9.3） |
| W22 | `waitEvent` 2200 | `WaitEvent`（Query） | Query 语义 | 0（`queries:0`） |
| W23 | `initialize` 1227/1230、`reset` 1251/1252/1282/1285、`finish` 2400/2401、`shutdown` 921 `waitForIdle` | 一次性 | 设备建立/重置/拆除需要静止 | 1 / 0 / 1 |

### 3.3 不等 GPU 的（确认过）

- `beginStream` 608-617、`ringSlice` 619-629（满了用 `invalidateBuffer` 重命名，旧存储交给命令列表）、流式下的 `uploadBuffer` Discard（1341-1347，同样重命名）和 NoOverwrite（直接 memcpy）。
- `runBeforeUi` 流式分支、阴影 / 效果执行器（`shadow_before_ui.cpp`、`before_ui_effects.h` 里没有任何 wait/submit；已核对 grep）。
- `pollTimeline` 416-436、`eventComplete` 只读 timeline。

---

## 4. “每帧 ~75 次提交”从哪来（精确对账）

提交只有这几个来源（已核对代码）：`flushStream` 里的 `submit(false)`，`submit(true)`（上传/建贴图/Clear/读回/初始化），`present` 的 `submit(false)`。用 host 计数对账：

```
submitted = streamSubmissions + 贴图区域上传 + clears + presents + texturesCreated + readbacks + 1(initialize)
贴图区域上传 = uploads − (noOverwriteUploads + discardUploads)      // 动态缓冲上传都走无提交路径
```

| | LT | EI | 生与死 |
|---|---|---|---|
| streamSubmissions | 55,263 | 62,241 | 24,475 |
| 贴图区域上传 | 754,187−696,914 = **57,273** | 1,096,441−1,033,160 = **63,281** | 2,296,295−2,256,488 = **39,807** |
| clears | 2,119 | 2,250 | 9,648 |
| presents | 2,118 | 2,249 | 9,647 |
| texturesCreated | 2,019 | 1,185 | 15,791 |
| readbacks | 484 | 484 | 484 |
| 初始化 | 1 | 1 | 1 |
| **合计** | **119,277** | **131,691** | **99,853** |
| host `submitted` | 119,277 | 131,691 | 99,853 |

三张图全部精确相等（已核对，`host.log.2` 的 `warvk-d3d9-host/v1`、`stream-host`、`dynamic-buffer-host` 三行；计算脚本思路见上式）。

解读：
- 流式提交 55,263 ≈ 贴图上传 57,273：**几乎每次贴图上传都会把正在录的流式列表截断提交一次**（W1 的 settle → flushStream），再加上自己的拷贝列表一次。所以每次贴图上传 ≈ 2 次提交。（推断，但数量吻合度很高）
- LT 每 Present 56.3 次提交，其中约 54 次来自贴图上传，Clear 1、Present 1、其余不到 1。按 1,600 个 pace 帧算是 74.5 次/帧，就是任务里说的 ~75。
- EI 每 Present 58.6 次，同样结构。生与死的提交被“建贴图 15,791 + Clear/Present 各 9.6K（加载期）”主导。
- 粗算全量 GPU 排空（`vkWaitSemaphores` 到最新 serial）次数上限：LT 约 12.8 万次（≈60/Present），全槽回收扫描约 19 万次（≈90/Present）。（推断：每个 settle 是否真的 vkWait 取决于当时有没有 pending）

---

## 5. 同步调用 195,650 次从哪来（LT）

代理端每次贴图 `UnlockRect` 发：`BeginTextureUpload`（同步，d3d9_proxy.cpp 923-925）+ 每 64 KiB 一个 `TextureUploadChunk`（同步，930-939）+ `CommitTextureUpload`（同步，940）。它们都走 `conn->call` → `ringCall`（connection.h 846-889），在 x86 游戏线程上自旋后挂起等 host 回复。

已知项（已核对计数）：CreateTexture2D 2,019 + Destroy 2,024 + 建缓冲/声明 5 + Present 2,118 + FrameScene 8,275 + StageMarker 4,965（这两个也是同步 `call`，connection.h 486-499）+ Readback 484 + ReadContent 62 = 19,952。
剩余 175,698 = 2×上传 + chunk 数 + FlushDraws（EndScene 同步，connection.h 574-577）+ TextureLod。若每次上传只有 1 个 chunk，贴图上传 = 3×57,273 = 171,819，**占 87.8%**，剩 3,879 给 FlushDraws、TextureLod 和多出来的 chunk。EI 为 88.9%，生与死 65.7%（另有 3.2 万次建/删贴图同步）。（拆分是推断：chunk 数没有单独计数；从字节量看平均每次上传只有几 KB，见 §11）

每次同步调用在 host 端还会顺带触发 §3 的 GPU 排空（Commit、Create、Destroy、Present、LOD），所以 x86 线程等的是“往返 + GPU 排空”。

---

## 6. Present：同步到上屏，而且模式和进程内不同

- host 日志：`Present mode: VK_PRESENT_MODE_FIFO_KHR`（`losttemple-r1-proxy/logs/host_warvk-d3d9-host.log:282,289`）。进程内日志：`VK_PRESENT_MODE_IMMEDIATE_KHR`（`losttemple-r1-inprocess/logs/War3_d3d9.log:305`）。（已核对）
- 原因：进程内 War3 钩子在 `GetD3d9Parameters` 里强制 `IMMEDIATE`（`src/d3d9/war3/hooks/war3_hook_lifecycle.cpp:1989-1990`，开关 `kWar3ForceImmediatePresentEnabled=true`，`war3_internal_test_config.h:395-396`）。代理模式游戏 `CreateDevice` 收到的是 `interval=0x0`（DEFAULT，`game.stderr:24`），host 据此 `setSyncInterval(1)`（d3d9_backend.cpp 1211），`reset-host` 行 `"syncInterval":1`。（现象已核对；“钩子在代理模式装得晚/没生效”是推断）
- FIFO 下 DXVK 的帧线程要 `vkWaitForPresentKHR` 等真正上屏后才 `signal(frameId)`（`src/dxvk/dxvk_presenter.cpp:1415-1436, 1466`）。host 的 `present()` 在 2323 等的正是**本帧**这个信号，所以每个 Present 都要等到本帧显示完才回复 x86：CPU 和 GPU 完全串行，再叠加垂直同步量化。进程内 D3D9 只等 `frameId − 2`（`d3d9_swapchain.cpp:1689-1701`；游戏给 `BackBufferCount=0`，被规范化成 1（`d3d9_swapchain.cpp:1336`），所以 `min(DefaultFrameLatency=3, 1+1)=2`）。（已核对代码）
- 两个 `Sleep(1)` 轮询（678、2318）：host 用 `setFrameRateLimit(0,1)`（1212），DXVK 的 `Sleep::initialize()`（会 `NtSetTimerResolution`，`src/util/util_sleep.cpp:36-59`）只在帧率限制器/延迟睡眠里调用，所以 host 不会提高计时器精度；两个日志里都没有 “Setting timer interval”（已核对 grep 计数 0）。Windows 10 2004 起计时器精度按进程算，没申请的进程 `Sleep(1)` 可能睡到 ~15.6 ms。（推断，需实测）
- LT/EI 代理中位数 28–30 fps、最快窗口 33–38 fps，和“串行 + 垂直同步 + 粗粒度 Sleep”的量化很像。（推断）

**结论：在改任何 GPU 等待之前，A/B 门禁应先让代理用和进程内相同的 present mode，否则测不出 P1 的效果。**

---

## 7. DXVK 自己的 D3D9 是怎么避免这些等待的

### 7.1 贴图上传（对应 W1-W3）
- `LockImage`（`src/d3d9/d3d9_device.cpp:35947-36237`）：MANAGED 贴图只标脏，“Managed textures are uploaded at draw time”（36203-36215）。只有 RT/读回过的资源才 `WaitForResource`（36148-36151）。
- `UnlockImage` → `FlushImage` → `UpdateTextureFromBuffer`（36239-36291、36293-36323、36325-36486）：
  1. `ThrottleAllocation()`（36332）；
  2. `AllocStagingBuffer(dirtySize)`（36410）从 `DxvkStagingBuffer` 切一片；
  3. CPU 把数据打包进去（36413-36415）；
  4. `EmitCs(ctx->copyBufferToImage(...))`（36420-36426）录进唯一的 CS 上下文；
  5. `ConsiderFlush(ImplicitWeakHint)`（36485）。**没有任何 GPU 等待。**
- `DxvkStagingBuffer::alloc`（`src/dxvk/dxvk_staging.cpp:19-54`）：4 MiB 块（`d3d9_device.h:421`）里线性切，满了就**扔掉自己的引用**新建一块；旧块靠命令列表的跟踪活到 GPU 用完（`DxvkContext::acquireResources` 里 `m_cmd->track(e.buffer, access)`，`dxvk_context.cpp:8363`；图像 8405）。大于半块的请求单独建缓冲。
- `ThrottleAllocation`（35706-35742）：在途 staging 上限 x64 = 16×4 MiB = 64 MiB，超过“上限/3”就提交，超过上限才 `waitForFence(m_stagingBufferFence, 已分配−上限)` —— **只等到够用的那个值**，不排空全部。`ExecuteFlush` 每次提交时给这个 fence 记当前累计分配量（37755-37773）。
- 同列表内的顺序由 DXVK 上下文负责：`copyBufferToImageHw`（`dxvk_context.cpp:3419-3462`）先 `prepareOutOfOrderTransfer`，图像本列表没被用过就放进 InitBuffer（在本列表所有绘制之前执行），被读过就留在 ExecBuffer 里按序执行并加屏障（9114-9124）。所以“上传之前的绘制看到旧内容、之后的看到新内容”自动成立。

### 7.2 建贴图（W4）
- `D3D9Initializer::InitDeviceLocalTexture`（`src/d3d9/d3d9_initializer.cpp:95-108`）：`EmitCs(ctx->initImage(...))`，每积累一定数量的初始化命令才 `flushCommandList`（148-151、161-169），不等。

### 7.3 Clear（W6）
- D3D9 `Clear` 只是 `EmitCs` 一条 clear；DXVK 上下文还会把它推迟到 render pass 开始时合并。没有提交、没有等待（推断：没逐行读 `D3D9DeviceEx::Clear`，但它和其它状态命令走同一 `EmitCs` 路径）。

### 7.4 删除资源（W5）
- D3D9 `Release` 只是引用计数归零；`DxvkImage/DxvkBuffer` 被每个用过它的命令列表 `track` 住（8363、8405），列表退休时才真正释放。不等 GPU。（已核对跟踪代码；“D3D9 Release 不等”是 DXVK 通识，推断）

### 7.5 SetLOD（W15）
- `d3d9_texture.h:60-70`：`CreateSampleView(m_lod)` + `MarkTextureBindingDirty`，不等。

### 7.6 动态缓冲（W16）
- `LockBuffer` DISCARD：`DiscardMapSlice` + `EmitCs(invalidateBuffer)`（`d3d9_device.cpp:36579-36616`），旧存储交给命令列表（`dxvk_context.cpp:1352-1356` 的 `m_cmd->track(prevAllocation)`）；NOOVERWRITE 直接跳过等待（36634-36635）。host 的流式路径已经是这样（W16 只在非流式时触发）。

### 7.7 提交时机
- `ConsiderFlush`（`d3d9_device.cpp:37330-37336`）→ `GpuFlushTracker::considerFlush`（`src/util/util_flush.cpp:12-84`）：
  - `ExplicitFlush`（Present、显式 Flush）：总是提交；
  - `ImplicitStrongHint`（EndScene 15756、事件 Query End 39459-39460）：本列表 ≥3 个 chunk 才提交；
  - `ImplicitWeakHint`（上传 36485、36828、换深度 15677 等）：≥6 个 chunk，且按在途提交数提高门槛（最多 20）；
  - `ImplicitSynchronization`：GPU 快空闲（在途 <2）就立刻提交，否则同上。
- `ExecuteFlush`（37746-37788）：录一条 `signal(fence) + flushCommandList(nullptr, nullptr)`，**不等提交结果**（状态指针是 nullptr，只有 9On12 才等）。
- Present（`d3d9_swapchain.cpp:1440-1458`）：blit 录进同一个 CS 上下文，`synchronizeWsi` + `flushCommandList` + `presentImage(..., nullptr)`；然后 `SyncFrameLatency()`（1471、1689-1692）只等 `frameId − 2`。

### 7.8 真正会等的地方（作对照）
- `WaitForResource`（35761-35799）：只等**那一个**资源，且先 `Flush()` 再等；`D3DLOCK_DONOTWAIT` 时只给一个 WeakHint。
- 读回（GetRenderTargetData / LockImage 需要 readback）：拷贝录进去，然后 `WaitForResource(mappedBuffer)`。

### 7.9 RTX Remix bridge（外部参照）
- `bridge.conf`：`presentSemaphoreMaxFrames` 默认 1（客户端最多领先服务端 1 帧）；`sendCreateFunctionServerResponses` 默认 true（Create 等回复）；`sendAllServerResponses` 默认 false（其余调用，包括 Lock/Unlock，不等回复）；`client.asyncQueries` 默认 true。服务端跑完整 DXVK D3D9，提交时机由 DXVK 自己决定。（已核对网页 `github.com/NVIDIAGameWorks/dxvk-remix/blob/main/bridge/bridge.conf`）
- 对我们的启示：**Create 保持同步可以接受；上传/Unlock 必须异步；Present 允许领先 1-2 帧。**

---

## 8. 对照小结

| 操作 | 现在 host | DXVK D3D9 | P1 目标 |
|---|---|---|---|
| 贴图区域上传 | 关流 + 2 次 G + 3 次 R + 2 次提交 + 3 次同步往返 | staging 切片 + 录进同一上下文 + WeakHint | 同 DXVK；代理端改异步 |
| 建贴图 | G+R+提交+G+R | 录 `initImage` | 录进开着的列表；代理仍同步（只为拿 OOM/槽位结果） |
| 删资源 | G+R+P | 引用计数 | 丢 `Rc`，不等 |
| Clear | G+R+提交+G+R | 录 clear | 录进开着的列表 |
| SetLOD | G+R | 换 view | 换 view |
| 读回 | G+R+提交+G+R | 录拷贝 + 等那一个资源 | 录进流 + 提交 + 只等那个 serial |
| Present | G+R+P(上一帧)+提交+S+Sleep 轮询+G+R+P(本帧) | 录 blit + 提交 + 等 frame−2 | 同 DXVK，帧延迟 2 |
| 每次提交 | 等提交线程 | 不等 | 不等（延迟检查状态） |
| 何时提交 | 每个同步点 | GpuFlushTracker | GpuFlushTracker |

---

## 9. P1 设计

### 9.1 必须继续成立的顺序 / 生命周期保证

1. **API 顺序 = GPU 可见顺序**：一个会话的所有命令按环里的序号在 host 唯一 owner 线程上、录进同一个 `DxvkContext`。上传前的绘制看旧内容、之后的看新内容由 DXVK 的跟踪和屏障保证（§7.1 最后一条）。这条现在已经成立，P1 不能引入第二个上下文或第二条录制列表。
2. **读回是读后写**：`Readback` 必须看到之前所有绘制/清屏/上传。做法：录拷贝 → 提交 → 等**这次**的 serial → 再等读回缓冲不在用 → 读映射内存。
3. **销毁在最后一次使用之后**：GPU 对象靠 DXVK `Rc` + 命令列表跟踪（§7.4）。另外要查 host 侧的**裸指针**：阴影施法者的 `source.vertexData/indexData` 指向动态缓冲的映射内存（d3d9_backend.cpp 2034-2042，传到 `shadow_before_ui.cpp:55-56`）。如果 `AppendShadowCaster` 不是当场拷贝，那么在 `addShadowCaster` 和 `runBeforeUi` 之间 Destroy 或 Discard 重命名就会悬空。（推断，需 Fable 读 `src/d3d9/war3/effects` 的 `AppendShadowCaster` 确认；`copiedBytes` 计数暗示是当场拷贝）
4. **staging 字节在 GPU 拷完前不能被改**：每次上传用新切片，旧块靠跟踪续命；总量按 timeline 节流。
5. **逻辑 ID 复用**：host 槽位立即可复用是安全的（新资源是新的 `DxvkImage` 对象），但要改掉 host_main 718-719 / d3d9_proxy.cpp 498-500 的“退休后才 tombstone”承诺，并确认代理端没有别的东西依赖“Destroy 回复 = GPU 已退休”。
6. **Present / 帧延迟**：x86 最多领先 GPU 2 帧（和进程内 D3D9 相同），这也是在途内存（重命名的常量存储、staging）的上界。`acquiredImage` 和 present 状态对象在 present 提交被处理前不能复用。
7. **设备重置 / 会话重置 / 结束**：`reset()`、`finish()` 保留全量排空（很少发生）。`SessionReset`（地图切换）只清 CPU 侧存储和阴影场景（host_main 615-623），不需要等 GPU；效果执行器的每图资源靠 `Rc` 释放。
8. **错误语义**：现在每个同步操作都能把 Vulkan 错误同步报给 x86。异步化后，错误只能在下一次同步点（Present/Create/Readback）报 `D3DERR_DEVICELOST`。代理已经有“异步记录被拒 = 终止”的约定（connection.h 432-433、host_main 976-989），P1 沿用它。
9. **诊断检查点**：`DiagnosticCheckpoint` 要求 `!hasPendingDraws()`（host_main 773）。P1 后流式列表几乎总是开着，检查点前要先提交（或改规则）。

### 9.2 组件

- **唯一录制列表**：把现在的 `streamOpen` 列表变成唯一的列表。`begin()` 只在 `reset/initialize/finish` 用。上传、建贴图、Clear、读回、Present、BeforeUi 都 `beginStream()` 后录进去（BeforeUi 流式分支 2086 已经这么做）。
- **staging 环**：直接用 `dxvk::DxvkStagingBuffer`（host 已链接 `dxvk_lib`），块大小 4 MiB；记录 `allocatedTotal`。每次提交时记下“这个 serial 对应的累计分配量”（小环形数组，或者像 D3D9 那样单独一个 `sync::Fence`）。
- **节流**：照抄 `ThrottleAllocation`：在途 > 上限/3 → 提交；在途 > 上限（64 MiB）→ `vkWaitSemaphores` 等到能把在途降到上限以下的那个 serial（不是最新 serial）。
- **提交状态**：`submit()` 不再等提交线程。两种做法：传 `nullptr`（和 D3D9 一样，错误靠 `getDeviceStatus()`），或者用一个小的状态对象环、在下一次同步点检查。
- **提交策略**：移植 `GpuFlushTracker`（`src/util/util_flush.h`，同在 util 库）。“chunk”用“本列表录进的 host 消息数”（绘制批、上传、Clear 各算 1），`lastCompleteSubmissionId` 用 `vkGetSemaphoreCounterValue(completion)`。映射：`FlushDraws`/EndScene → StrongHint；上传、Clear、建贴图 → WeakHint；Present → Explicit；staging 节流 → Synchronization/Explicit；保留 `MaxStreamDraws=4096` 上限。
- **在途上限**：`MaxStreamLatency=3`（按 serial）在提交变少以后会变成每帧中途的阻塞点。改为按帧（Present 计数）限制为 2 帧，serial 上限放宽到例如 16（推断：需要实测每帧提交数后定）。
- **Present**：blit 录进开着的列表 → `synchronizeWsi` → 提交 → `presentImage`（状态延迟检查）→ `presentSignal->wait(frame − 2)`（`sync::Fence::wait` 是真正的阻塞等待，不用 `Sleep(1)`）。present mode 跟进程内一致（IMMEDIATE）。
- **回收**：删掉热路径上的 `waitRetired()`/`resourcesInUse()` 全槽扫描；“retired”统计改成 `pollTimeline` 那种非阻塞观察。`finish()` 前仍做一次全量排空，保证结束时 `submitted == gpuCompleted == retired` 的检查（2403-2408）。

### 9.3 逐项改法

| 等待 | P1 做法 |
|---|---|
| W1-W3 贴图上传 | `alloc` staging 切片 → memcpy `textureUpload.data()` → `beginStream()` → `copyBufferToImage(slot.image, …, slice.buffer(), slice.offset(), …)` → 记 1 个 chunk、WeakHint。去掉 1417 的 settle、1418-1420 的换大缓冲、1422 的 begin、1426 的 submit(true) |
| W4 建贴图 | 建 image/view 后 `beginStream()` + `initImage`；去掉 1395/1397 |
| W5 删资源 | 不 settle、不等 present；`unbindAuthorResources()` 可保留（便宜）；直接丢 `Rc` |
| W6 Clear | `beginStream()` + `bindTargets()` + clear；去掉 1538/1562 |
| W7-W12 Present | 见 §9.2 |
| W13 读回 | `beginStream()` + 拷贝 → `flushStream` → `vkWaitSemaphores(该 serial)` → `waitForResource(readback)` → 读 |
| W14 提交握手 | 见 §9.2 |
| W15 SetLOD | 去掉 1438 的 settle |
| 代理端上传 | `Begin/Chunk/Commit` 加进 `AsyncOrderedOp`（connection.h 435-437），走 `enqueueOrdered`；或者新增一个单消息 `UploadTextureRegion`（区域 + 数据 ≤ 64 KiB−128 B 时一条消息完成，大的继续分块）。被拒 = 终止。去掉 `AbortTextureUpload` 的同步补救（d3d9_proxy.cpp 941） |
| 代理端 EndScene | `FlushDraws` 不需要回复，可以改成异步有序消息 |

### 9.4 内存影响
- x86 侧：不增加。命令环大小不变（4 MiB 映射），贴图字节本来就要经过环；异步只是不等回复。（已核对：环容量是固定参数，connection.h 87-94）
- x64 侧：staging 在途 ≤ 64 MiB + 2 帧的重命名常量存储。这正是“把内存从 x86 挪到 x64”的方向。

---

## 10. 给计划的结论

**先修门禁，再修等待。** 代理 FIFO、进程内 IMMEDIATE，Present 又同步到上屏——现在的 29 vs 136 fps 不能拿来衡量 P1。

建议顺序（每步单独跑单元测试 + 1 张图 LT 代理 A/B；机器共享，跑轻量门禁）：

| 步 | 内容 | 谁 | 工作量 | 预期 |
|---|---|---|---|---|
| S0 | **测量**：按调用点统计 settle 次数和 ns（GPU 等待 / 回收扫描 / 提交握手 / present 两个 Sleep 循环分开），`WaitTimer` 已有（40-54）；x86 侧按 op 统计 `ringCall` 阻塞时间；启动时记录 `NtQueryTimerResolution`。注意 admission profile 和命令环互斥（connection.h 84-85），要新开一个和环兼容的开关 | Opus（机械） | 0.5 天 | 给后面每步一个基线 |
| S1 | **同条件 present**：代理模式也强制 IMMEDIATE（与 `kWar3ForceImmediatePresentEnabled` 一致）；`present()` 两个 `Sleep(1)` 轮询改成 `presentSignal->wait()` / `waitForSubmissionUntil`（语义不变，只换等待方式） | Opus（机械） | 0.5 天 | 可能单独就让代理 fps 明显变化（推断） |
| S2 | Clear、SetLOD、建贴图录进开着的列表（W4/W6/W15）。先例：流式 `runBeforeUi` | Opus（照抄已有模式） | 1 天 | 每帧少 ~2 次提交、~4 次排空；生与死少 1.6 万次排空 |
| S3 | 贴图上传：`DxvkStagingBuffer` + 64 MiB 节流 + 录进开着的列表（W1-W3）。照抄 `UpdateTextureFromBuffer` | Opus 实现，Fable 复核 | 1.5 天 | LT 每帧少 ~54 次提交、~54 次排空、~80 次全槽扫描（推断） |
| S4 | 提交策略：移植 `GpuFlushTracker`；`FlushDraws` 改 StrongHint；在途上限改按帧 | Opus | 1 天 | 每帧提交降到个位数 |
| S5 | 代理端贴图上传、EndScene 改异步有序 | Opus 写机制；失败语义 Fable 定 | 1.5 天 | 同步调用少约 88%（LT 每帧 ~120 → ~15） |
| F1 | 删资源不等（W5）：改 tombstone 承诺，查阴影裸指针、代理端依赖 | **Fable** | 1 天 | 生与死少 1.6 万次排空 |
| F2 | Present 流水化：帧延迟 2、present 状态延迟检查、`acquiredImage` 所有权、`DEVICELOST` 延迟上报、诊断 presentedReadback 保留同步路径 | **Fable** | 1-1.5 天 | 去掉每帧两次全量排空 + 上屏等待 |
| F3 | 每次提交不等提交线程（W14）：状态对象环或 nullptr；`canCleanWindowFault`（887-909）等依赖“submitted==retired”的故障清理逻辑重审 | **Fable** | 1 天 | 每帧少几十次跨线程握手（S4 后已不多） |

合计：Opus 约 6 天，Fable 约 3-3.5 天。S0-S4 在 Fable 不在的这周就能做，且每一步都可以单独回退。S3 虽然是 Opus 能写的，但它第一次让 staging 字节的生命周期依赖 DXVK 跟踪而不是显式等待，建议 Fable 回来后复核再合入默认路径（先挂在开关后面）。

**模型建议**：S0-S2、S4 用 Opus；S3、S5 Opus 写、留开关；F1-F3 等 Fable。

---

## 11. 还不知道的（以及怎么测）

1. **每类等待到底花了多少毫秒/帧**。推断：每次上传 ≈ 2×GPU 往返 + 3×全槽扫描 + 2×提交握手，约 0.3-0.5 ms；LT 每帧 27 次 ≈ 10-14 ms，加上 120 次同步往返和同步 Present，足以解释 34 ms/帧。→ S0 的分站点计时。
2. **全槽扫描 `resourcesInUse()` 一次多贵**（16,320 个约 330 B 的结构体按步长访问）。→ S0 单独计时；或者离线写个微基准。
3. **IMMEDIATE 下代理 fps 是多少**、显示器刷新率、隔离桌面上 FIFO 的行为。→ S1 后重跑 LT/EI。
4. **host 进程的 `Sleep(1)` 实际睡多久**。→ 启动时打日志 `NtQueryTimerResolution`；S0 统计 present 循环的迭代数和时间。
5. **每帧 ~27 次贴图上传是什么**（字体字形？小地图？战争迷雾？），是整级还是小区域，平均多大。LT 贴图上传字节估计在 1.7-3 亿字节之间（`uploadedBytes` 8.29 亿 − 上传窗口 `requestedBytes` 6.58 亿，后者含少量贴图），平均每次几 KB（推断）。→ host 按（贴图尺寸、级别、区域字节）做每帧直方图；或 x86 侧按调用者统计。如果大多是同一张贴图反复整级更新，可以考虑 x86 侧合并到下一次绘制前（像 DXVK MANAGED 那样“绘制时上传”）。
6. **chunk 数**（多于 1 个 chunk 的上传有多少）。→ host 计数 `TextureUploadChunk`。
7. **FlushDraws 和 TextureLod 各多少次**（现在只能从残差算出合计约 3.9K）。→ host 按 op 计数。
8. **阴影施法者的动态缓冲裸指针**是否在 `AppendShadowCaster` 当场拷贝。→ 读 `src/d3d9/war3/effects` 对应函数（F1 前置）。
9. **流式 Discard 的“带上旧字节”**（`discardCarriedBytes` LT 6.5 亿、生与死 30.6 亿字节，d3d9_backend.cpp 1343-1347）不是 GPU 等待，但是 host CPU 成本；D3D9 语义里 DISCARD 后内容未定义，只为 host 的前缀校验/IB 影子保留。P1 之后再看要不要只带 IB。
10. **生与死为什么命令环满**（38,118 次背压、12.7 s）：host 是瓶颈，但是 CPU（处理 79 亿字节、每次建贴图 `Session::slot()` 线性扫 16K 条目，host_main 268-278）还是 GPU 等待占大头，现在分不清。→ S0 的 host 忙/闲统计（和环兼容）。
11. **DXVK 跨提交的写后读/读后写屏障**：贴图拷贝落进下一列表的 InitBuffer 时，对上一列表里还在读它的绘制是否有执行依赖。进程内 D3D9 每帧都走这条路且正常，所以应当成立（推断），F 阶段可以读 `DxvkCommandList` 提交路径确认。
