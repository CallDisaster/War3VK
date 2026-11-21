# x64 六个开关的独立审查（Codex Astra，只读）

> 2026-11-18。由 Claude Fable 发起的只读审查：Codex 新开只读沙箱会话（`exec -s read-only`），未改文件、未构建、未运行。
> 行号对应审查时刻的工作树，之后的修改见 `2026-11-18-x64-switch-review.md` 的“对 Codex 发现的处置”。
> 以下为报告原文。

本次建议：**`RING_WAKE_FIX` 开；`UPLOAD_SLICES`、`PRESENT_WAIT`、`DYNAMIC_CPU_COPY`、`HOST_EXIT_BUFFERED` 需先修；`HOST_EXIT_GL_WATCHER` 关。** 其中，允许打开 `RING_WAKE_FIX` 只表示现有“首次取基线后返回复查”的补丁有明确收益且未发现新的唤醒安全缺陷，不代表整条等待链已无问题。

审查依据为本轮读取的工作树源码，包括六个开关、代理调用方、DXVK 提交队列、staging 分配器及阴影投射体复制入口。没有修改或新建文件，没有运行构建、测试、游戏、宿主或门禁，没有访问玩家目录，没有提交、部署或操作进程。下文“确定”表示可从控制流或数据范围直接证明，不表示已做运行复现；“还缺的测试”均未在本轮执行。行号对应本轮读取版本，路径相对仓库根目录。

---

**一、WARVK_D3D9_RING_WAKE_FIX**

**发现**

1. **环满等待仍有“空间已经释放，但生产者继续睡眠”的窗口；首次返回补丁没有覆盖通知时机。把握程度：确定。**

   位置：[tools/render_host/d3d9_command_ring.h:295](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_command_ring.h:295)、[tools/render_host/dxvk_core/d3d9_host_main.cpp:1117](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:1117)、同文件 `1207、1232–1237`；`tools/render_host/d3d9_connection.h:899–909、938–949`。

   触发条件：生产者因环满进入等待，已经执行 `setWaiting(true)` 和最后一次进度复查；随后宿主 `tryRead()` 读走一个命令，在 `295–296` 行发布新 `head`，释放了足以容纳待写记录的空间，但该命令的 `session->apply()` 较慢或阻塞。

   后果：宿主在发布 `head` 后没有立即通知生产者，而要等 `apply()` 返回、发布 `applied` 后才检查 `producerWaiting()` 并发事件。生产者可以在条件已经满足的情况下继续睡到执行结束或 `RecheckMs=1000` 到期。这个问题属于**空间归还通知延迟**，不是首次基线覆盖进度的那个窗口，也不等同于永久死锁。

   具体修法：把“归还环空间”和“命令执行完成”视为两个可能满足不同等待条件的发布点。宿主读走记录、发布 `head` 后，如生产者正在等待，应通知一次；执行完成后继续保留当前通知。只改生产者的首次返回或条件复查，不能解决条件在其睡眠之后成立、宿主却迟迟不通知的情况。

2. **同步调用的绝对 deadline 没有完整覆盖环满准入；进度早退还会跳过 deadline 检查。把握程度：确定。**

   位置：[tools/render_host/d3d9_connection.h:865](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:865)，尤其 `868–880、886–889、933–949、973–984、1188–1190`。

   触发条件：同步调用进入时建立了 30 秒 `until`，但环满，宿主持续缓慢消费；或者等待期间每次进入 `ringAwait()` 都观察到新的 `head/applied`。

   后果有两层：

   - `ringCall()` 在准入前建立了 deadline，但 `ringPublish()` 调的是 `ringAwait(wait, 0)`。只要宿主每次在 10 秒无进度期限内推进一点，准入可以超过整个调用的 30 秒期限。
   - `ringAwait()` 在发现进度变化时于 `871` 行直接返回，deadline 检查在其后。绝对 deadline 因而不是所有等待步骤都必须经过的检查。

   这两点主要是现有等待结构的问题，不能归因于新开关才引入。新开关另有一个很窄但真实的行为变化：**首次调用会在健康和 deadline 检查之前返回**；如果调用已经到期，而目标回复恰好在调用方条件检查与首次基线读取之间到达，开关打开后可能接受该回复，关闭时则可能在 `886–889` 行报超时。需要明确这是允许“已到达结果优先”，还是要求硬性到期拒绝。

   具体修法：把同一绝对 deadline 传入同步调用的准入阶段；决定并统一“目标已经满足”与“deadline 已到”的优先级。滚动无进度计时只因真实进度更新，不能代替绝对调用期限。

**已核对、未发现的问题**

- 对原先“调用方检查未完成 → 宿主完成且不发信号 → 首次基线直接读到已完成值 → 错误睡眠”的窗口，当前首次返回补丁有效。返回后，`ringCall` 重查 `applied`，`ringPublish` 重试写入。
- `setWaiting(true)` 后再次观察进度，与宿主 `publishApplied()` 后再读 `producerWaiting()` 的 `seq_cst` 顺序相配合；未发现这条完成通知链还存在同一种漏唤醒。
- `wait.polled` 的自旋在登记等待前，之后仍有登记和复查，顺序正确。它在观察到进度后不重新置为 `false`，意味着每个 `RingWait` 只给一次自旋预算；这是性能策略，不是漏唤醒缺陷。
- 首次返回不会再次初始化 `wait.moved`，下一次无进度调用仍使用原基线时间；它没有把 `ringNoProgressMs` 重置成另一整段时间。调用方原来的 `until` 也没有重新计算。
- Present 没有独立绕过这套同步等待的回复路径。ring 模式 worker 在 `watchRing()` 中观察宿主进程，设备丢失或失败通过 `producerProgress` 等现有通知传递；没有发现 worker 抢走宿主 `progress` 事件的路径。

**建议默认值：开。**

现有补丁值得保留，关闭它会重新暴露已经能从控制流证明的首次基线窗口。但更稳的长期实现是让 `ringAwait` 接收调用方实际谓词：同步调用检查目标 `applied`，准入检查经过验证的剩余空间；在 `setWaiting(true)` 后重查该谓词，再进入等待，并用作用域清理保证所有返回和异常路径都撤销 waiting。这样无需依赖“基线必须早于调用方上一次条件检查”的隐含约束。空间归还的宿主通知仍需同时补齐。

**还缺的测试**

- 用可控交错覆盖：完成发生在首次条件检查前、检查与基线之间、登记 waiting 前、登记后复查前、复查后进入等待前。分别测同步回复、Present 回复、环满准入。
- 宿主发布 `head` 后暂停 `apply()`，确认生产者能够立即利用已归还空间，而不是等 1 秒安全复查。
- 环满但每隔数秒持续推进，持续超过 30 秒；验证绝对调用期限不被滚动进度延长。
- 在 deadline 已到的首次基线窗口交付回复，固定期望结果，防止开关两相出现未经约定的超时语义差异。
- worker 发布宿主退出或设备丢失时，分别让生产者处于自旋、waiting 登记、内核等待状态；检查唤醒与终态原因。

---

**二、WARVK_D3D9_UPLOAD_SLICES**

**发现**

1. **16 MiB 额度与峰值计数约束的是对齐后的上传字节，不是实际 staging backing；二者可以明显不一致。把握程度：确定。**

   位置：[tools/render_host/dxvk_core/d3d9_backend.cpp:392](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:392)，以及 `171–173、380–385、1771–1786`；[src/dxvk/dxvk_staging.cpp:19](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/dxvk/dxvk_staging.cpp:19)，尤其 `32–52`。

   触发条件：连续上传不能紧密填满 4 MiB chunk 的区域。例如每次上传 1.5 MiB，小于独立分配阈值，每个 4 MiB chunk 只能放两个切片。假设这些列表尚未退休，十个上传会计为 15 MiB，却持有五个 4 MiB chunk，即 20 MiB backing，尚未触发 16 MiB 字节额度。

   另一个差别是：`retireUploadMarks()` 按 GPU timeline 释放额度，而命令列表引用由 finish 线程稍后在 `src/dxvk/dxvk_queue.cpp:380–394` 释放。GPU 完成但 CPU 回收滞后时，已释放的额度不等于 backing 已释放。

   后果：`peakInFlightBytes` 不能证明实际暂存内存没有超过 16 MiB，`peakInFlightSlices` 也不是实际块数。这里未发现由此导致的提前释放，但不能用这些计数验收物理内存上限。

   具体修法：明确区分上传字节额度、实际 chunk/独立 backing 字节和 CPU 尚持有的退休 backing。若默认开启的准入要求是实际内存上限，就需要按 backing 分配、持有和释放记账；若仅想限制上传流量，则必须修正计数名称及验收口径，不能把它描述成物理内存硬上限。

2. **额度压力路径在设备健康检查之前调用 Vulkan 查询和等待，遗漏了已知设备终态的前置门。把握程度：确定。**

   位置：`tools/render_host/dxvk_core/d3d9_backend.cpp:392–425、559–562、873、1271–1279、1772–1773`。

   触发条件：提交/finish 线程已经锁存设备错误，下一次上传又恰好超过额度。`uploadTextureRegion()` 先调用 `throttleUploads()`，之后才通过 `beginStream()` 执行 `healthy()`；`active()` 本身没有检查设备状态。

   后果：已知终态下仍可进入 `vkGetSemaphoreCounterValue()`，并在满足后续分支时进入 `vkWaitSemaphores()`。这与 `settle()` 中“先检查健康，再开始新等待”的边界不一致，可能增加终态收尾延迟。每个 mark 的等待还单独获得一段 `GpuWaitNs`，并没有该次额度等待的统一绝对 deadline。

   具体修法：在额度查询前检查终态，在每次等待前复查，并让所有额度等待共享同一操作 deadline。失败继续隔离 Owner，不应把失败降级成允许本次上传继续记录。

**已核对、未发现的问题**

- **draw 与上传保序：** `Connection::call()` 在非 draw 命令前会先 `flushDraws()`，见 `d3d9_connection.h:720–724`；宿主切片复制与后续 draw 使用同一个 stream context。`copyBufferToImageHw()` 经过资源访问跟踪和屏障；已使用过的目标图像不能随意把后续写移到 InitBuffer 前面，见 `src/dxvk/dxvk_context.cpp:3439–3461、9030–9050、9114–9125`。没有发现同纹理重复上传、重叠部分更新绕过此顺序的入口。
- **clear、其他同步上传、SetLOD、销毁、Reset、GPU 读回：** 当前这些路径仍调用 `begin()` 或 `settle()`；`settle()` 先 flush stream。对应位置为 `d3d9_backend.cpp:692–697、1694–1700、1824、1928、2541–2544、1555–1556、2826–2827`。没有发现未提交切片被这些操作直接越过。
- **`initializedLevels/uploaded`：** 当前确实在“记录成功”后发布，不能称 GPU 已完成。但已检查的读者中，draw 用它判断可按序引用资源，`ReadContent` 读取的是宿主 retained CPU 副本，真正的 GPU 读回另有 settle。未发现当前读者仅凭这两个字段就读取尚未完成的 GPU 内容。
- **LockRect READONLY：** 当前 managed 纹理读取代理 CPU 副本；被逐出的副本由宿主 `ReadContent` 恢复，不是 GPU 读回。Commit 在后端记录返回后更新 retained 内容，再 reset 上传事务，见 `d3d9_host_main.cpp:652–682`。因此“尚未提交 GPU”本身不使 READONLY 返回旧数据。
- **mip 生成与 StretchRect：** 当前代理没有实现这两项。`CreateTexture()` 拒绝非零 usage，`GenerateMipSubLevels()` 仍是 unsupported 实现，`StretchRect()` 返回未实现。这里应写“不支持，未进入该路径”，不能写“已验证保序”。
- **失败事务及寿命：** 额度查询、等待、分配、复制记录或提交抛异常后，`uploadTextureRegion()` 隔离整个 Owner；宿主不会执行后面的 retained patch、初始化发布和 `textureUpload.reset()` 成功路径。残留事务属于终止会话，不会成为可继续使用的半提交事务。DXVK 命令列表跟踪 staging buffer 与目标 image；换 chunk 只释放分配器的当前引用，未发现正常路径的悬空 slice。失败时保留 Owner 是进程结束前的隔离，不是已经回收的证明。
- **非 stream 回退：** `uploadSlicesNow()` 首先检查 `streamMode`，初始化又排除了 stream 与 flight/group 的非法组合；当前支持范围内的回退条件完整。

**建议默认值：需先修。**

至少先补齐额度压力路径的终态检查和等待预算，并明确实际 backing 与上传额度的记账合同。正常保序链静态上成立，不能替代目前缺失的压力失败验收。

**还缺的测试**

- 强制命中 `creditWaits > 0`，分别由字节额度和切片数额度触发；不能只用“总共上传超过额度”证明等待分支被执行。
- 等待前已设备丢失、查询后丢失、等待返回错误、等待超时、flush 提交失败；检查 Commit 不成功、CPU 副本不提前更新、会话不能继续接受 draw。
- 用 1.5 MiB 等不整除 chunk 的尺寸制造尾部浪费，同时延迟 GPU 完成及 finish 线程回收，比较逻辑额度和实际 backing 峰值。
- 切片仍只在录制状态时立即做 READONLY；再强制 LRU 逐出，走 `ReadContent` 恢复。现有夹具里的 READONLY 在 `Expect()` 的读回之后，不能单独证明这个窗口。
- 在 pending upload 后分别触发 SetLOD、销毁、Reset、另一种上传及读回，并核对旧内容、新内容和保留区域。
- 当前 `AutoTest/test_render_host_d3d9_upload_order.cpp` 已包含 full、partial、mip、slices、bytes、readonly、reset 用例；仍缺上述强制故障和等待交错，不应把“已有相应用例名称”当成分支已覆盖。

---

**三、WARVK_D3D9_PRESENT_WAIT**

**发现**

1. **“10 ms 一片、5 秒期限”的约束没有覆盖新等待入口的互斥锁获取。把握程度：推断。**

   位置：[tools/render_host/dxvk_core/d3d9_backend.cpp:2771](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:2771)、[src/dxvk/dxvk_queue.cpp:99](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/dxvk/dxvk_queue.cpp:99)；退休回调注册另见 `d3d9_backend.cpp:942–943` 和 `src/util/sync/sync_signal.h:174–189`。

   触发条件：Present 状态仍为 `VK_NOT_READY`，新阻塞路径进入 `waitForSubmissionUntil()`，而提交队列的 `m_mutex` 被持续占有。该函数先执行无期限的 `unique_lock`，拿到锁之后才进入带 deadline 的条件变量等待。`setCallback()` 同样先获取普通 mutex，没有把本次退休 deadline 传入。

   后果：这些情况下不能保证在一片等待之后返回 `healthy()`，也不能保证本地 5 秒检查及时执行。旧的该段 `Sleep(1)` 轮询不需要获取提交队列 mutex，因此“检查、deadline 完全相同”的注释比实际保证强。

   这不是已复现的 Present 卡死。源码中另有需要警惕的公共底层路径：`dxvk_queue.cpp:259–286` 在持有 `m_mutex` 时，非设备丢失的提交错误分支调用 `m_device->waitForIdle()`，而 `dxvk_device.cpp:697–698` 又进入提交队列的 `waitForIdle()` 获取同一个 mutex。这是原有底层问题；本轮没有证明它发生在该 Present 等待期间，不能据此解释此前性能样本。

   具体修法：若要承诺此处的有界健康复查，等待通知不应要求先取得可能无限等待的共享锁，或必须把锁获取也纳入同一 deadline；同时检查上述错误收尾的持锁重入。若只保留外部宿主 watchdog 作为最终边界，则应明确本地期限不是全路径硬期限。

2. **`timeBeginPeriod(1)` 正常收尾配对，但 quarantine 路径不配对。把握程度：确定。**

   位置：[tools/render_host/dxvk_core/d3d9_backend.cpp:179](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:179)，以及 `200、1282–1291、2672–2674、2813–2817、2885–2886`。

   触发条件：阻塞相位成功申请了 timer resolution，随后 Present、提交或其他后端调用失败，执行 `quarantine()`。

   后果：`quarantine()` 使用 `m_owner.release()` 保留 Owner，`TimerResolution` 析构不会执行，因而没有对应的 `timeEndPeriod(1)`。不能说所有出口都已配对。当前宿主终态通常继续走进程退出，这不等于已经证明存在长期系统泄漏，但在进程退出之前请求仍被持有。

   正常 `finish()` 的 `m_owner.reset()` 会触发析构；`alternate` 切回 poll 时也会主动释放，这两条路径未发现配对错误。

   具体修法：把 timer-resolution 释放作为不依赖 GPU 资源销毁的 CPU 清理，在隔离 Owner 之前显式执行；不能为了保留不确定的 GPU 引用，同时跳过这一独立清理。

**已核对、未发现的问题**

- `CallbackFence::setCallback()` 在加锁前后都复查 signal 值；完成发生在注册窗口内不会漏掉回调。回调捕获 `shared_ptr` 事件所有权，未发现晚到回调使用已关闭句柄。
- 退休等待始终检查真实 `presentSignal->value()`；事件只是唤醒线索，陈旧事件不构成退休成功。
- 队列结果仍从 owner 常驻的 `presentSubmission` 读取，未发现状态指针指向栈变量或在队列完成前复用。
- 正常情况下，block 模式把设备健康复查间隔从 `Sleep(1)` 轮询变为约 10 ms 一片；退休事件等待表达式另有 `+1`，参数上限实际可到 11 ms。未改变“只有成功结果才能返回成功”的判断。
- 两个模式都没有在等待循环内检查 HWND 身份；窗口验证仍在 Present 前后。不能把 block 模式描述成提供了新增窗口取消能力。
- 后端受 owner-thread 约束，宿主顺序消费 Reset。合法调用模型下未发现 Present 与 Reset 并发修改 `presentSignal`；其他线程直接调用 Reset 本来就不受支持。

**建议默认值：需先修。**

先补 timer 的终态清理，并解决或明确锁获取不受本地 deadline 约束的等待合同。回调事件自身的注册和寿命设计可以保留。

**还缺的测试**

- 完成发生在回调注册前、注册中、注册后但内核等待前；确认没有丢失通知，也没有将陈旧事件误作成功。
- 人为延迟提交队列 mutex 持有者，验证调用是否仍能按约定检查设备终态和超时；普通 GPU 慢帧不能覆盖这个入口。
- 在 queue wait、retire wait 中分别触发设备丢失、窗口销毁、窗口 resize，并核对错误分类及前后模式的响应时间。
- 记录成功的 `timeBeginPeriod` 与 `timeEndPeriod` 次数：正常 Finish、alternate 切换、Present 异常、初始化后失败、quarantine 都应有明确结果。
- Reset 紧接成功 Present、紧接失败 Present，以及等待期间窗口线程发生变化的场景；检查 signal、回调和状态存储不会跨代复用。

---

**四、WARVK_D3D9_DYNAMIC_CPU_COPY**

**发现**

1. **镜像对已初始化前缀可以建立等价性，但阴影复制可能读取前缀之外；因此当前“读到的全部字节相同”不成立。把握程度：确定。**

   位置：[tools/render_host/dxvk_core/d3d9_backend.cpp:1633](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:1633)，以及 `1652–1654、1672、1687–1704、2052–2065、2430–2435`；[src/d3d9/war3/effects/war3_shadow_scene_input.h:347](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/d3d9/war3/effects/war3_shadow_scene_input.h:347)，尤其 `352–355、370–371`。

   触发条件：动态 VB 只初始化了足以满足实际 draw 属性读取的前缀，但 caster 复制的完整 stride 范围更大。

   一个具体例子：VB 容量至少 48 字节，stride 为 16，声明/实际 shader 只读取每顶点 12 字节位置，前三个顶点的初始化前缀为 44 字节。主 draw 的末次读取止于 44，能够通过 `prepareDraw()`；caster 却复制 `3 × 16 = 48` 字节。indexed draw 还可能声明比实际引用索引更大的 `numVertices`，进一步扩大复制范围。

   `vertexBytes.resize(size)` 将 CPU 镜像初始化为零，而映射存储没有在此处作同样初始化；`dxvk.zeroMappedMemory` 默认也不是开启。未上传区域及 DISCARD 新 backing 的尾部没有逐字节等价保证。

   后果：切换开关可能改变冻结到 caster pool 的额外字节。**确定的是范围及字节等价证明缺失，不是已经证明这些额外字节导致像素变化。** 未被索引引用的顶点或 padding 可能不影响最终画面，不能把两者混为一谈。

   具体修法：把初始化范围传入 `ShadowCasterSource/Geometry`，验证实际复制区间被它覆盖；或者缩小冻结范围，同时保持索引重定位正确。仅检查 `buffer->info().size` 不足以授权“镜像与映射相同”。

2. **正式内存 census 漏记新增 VB 镜像，可能把实际存在的 CPU backing 报成零。把握程度：确定。**

   位置：[tools/render_host/dxvk_core/d3d9_backend.cpp:2927](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:2927)，尤其 `2929–2932`；镜像分配见 `1633–1636`。

   触发条件：开关开启并存在动态 VB，调用 `diagnosticMemory()`。

   后果：遍历每个 Buffer 时，无论 VB/IB 都传入 `b.indexBytes.size()/capacity()`；VB 的 `vertexBytes` 没有进入 CPU 账本。`dynamicRead.copyBytes` 另有记录，所以不能说完全没有任何计数，但这两套账本会不一致，内存 checkpoint 不能完整反映该优化增加的宿主 CPU 内存。

   具体修法：VB 统计 `vertexBytes`，IB 统计 `indexBytes`，并在销毁后的空槽核对 size/capacity，保留当前检测遗留容量的意图。

**已核对、未发现的问题**

- 当前所有动态 VB 上传都经过同一 `uploadBuffer()`；实际上传范围同时复制到映射和 `vertexBytes`。
- Ordered 写先 settle；NOOVERWRITE 写相同 offset/count；stream DISCARD 先复制旧初始化前缀，再覆盖本次上传区域。对已初始化范围，未发现漏更新镜像的分支。
- 代理把大 Lock 拆为多个消息时，只有第一个 chunk 使用 Discard，后续使用 NoOverwrite，见 `d3d9_proxy.cpp:800–805`；镜像随每个 chunk 更新。
- `alternate` 即使当前相位读取映射，也继续更新镜像，未发现相位切换读取陈旧镜像。
- 非 stream DISCARD 不换 backing，按既有 settle 路径更新两份内容。真正的 Reset 要求 DEFAULT buffer 释放；窗口 resize 的 keepResources 路径保留原 Buffer。销毁后整个 slot 被清空，新建时重新建立镜像和初始化前缀。
- 未发现 caster 将 `vertexBytes.data()` 长期借用到后续上传之后：`ShadowBeforeUi::addCaster()` 当场进入 `ShadowCasterPool::copy()` 完成复制。
- 缓冲 Lock 当前只接受已列举的 flags；READONLY 等未支持 flag 被拒绝，不能将未实现写路径算作已覆盖。

**建议默认值：需先修。**

先闭合 caster 复制范围的初始化证明，并补内存 census。对已定义字节的镜像更新实现本身，未发现需要重写的缺陷。

**还缺的测试**

- 上述“stride 16、初始化 44、复制 48”的边界，以及 indexed draw 声明范围大于实际索引覆盖范围；检查复制范围和逐字节结果，而不只比较最终画面。
- 首次部分写、前缀追加、前缀内覆盖、非零 offset DISCARD、跨消息分块 DISCARD、NOOVERWRITE，逐次核对镜像与映射的已初始化范围。
- DISCARD 前后各捕获一个 caster，随后继续写同一 VB；核对已冻结几何保持不变。
- slot 销毁重建、窗口 keepResources resize、真正 Reset 后重建，使用不同内容图案检查旧代数据残留。
- 打开镜像后的阴影像素对照，以及内存 checkpoint 中 `vertexBytes.size/capacity` 与 `dynamicRead.copyBytes` 的一致性。

---

**五、WARVK_D3D9_HOST_EXIT_BUFFERED**

**发现**

1. **整块缓冲扩大了退出期崩溃时的账本丢失范围；不是原输出的故障等价替换。把握程度：确定。**

   位置：[tools/render_host/dxvk_core/d3d9_host_main.cpp:82](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:82)，以及 `1294–1433`。

   触发条件：在创建 `ExitText` 后、最终 `exitOut.flush()` 前，宿主崩溃或被外部终止；若发生异常展开，`ExitText` 也没有析构 flush。

   后果：draw、stream、dynamic buffer、ring、annotation、shadow、content、admission 等已格式化的账本全部仍在 `std::string` 中，可能整体消失。原实现逐次输出，通常至少已经留下先前记录或记录前缀。`RawExitMilestone` 和先前写出的 profile 仍可保留阶段信息，但不能代替丢失的全部账本。

   OOM fallback 的代码会先 flush 已积累内容，再改为直接输出，这条路径未发现主动丢弃已积累文字；它不覆盖进程级崩溃。

   具体修法：按完整账本记录分批输出，至少使已经完成的前几条记录不依赖最后一条巨大 shadow 记录才能落出；为正常异常展开补作用域清理。析构 flush 无法解决强制终止或原生崩溃，不能据此宣称完全故障等价。

2. **flush 无条件清空缓冲，不检查短写或刷新失败，随后仍可输出正常成功报告。把握程度：确定。**

   位置：`tools/render_host/dxvk_core/d3d9_host_main.cpp:100–103、1433–1454`。

   触发条件：`fwrite()` 只写出部分字节，或 `fflush()` 失败。

   后果：剩余账本没有重试、没有失败状态，`text.clear()` 直接丢弃它；后续 `normalTeardown/ok` 与 `normal-return` 路径不受影响。原先多个 `fprintf` 同样缺乏完整 I/O 错误处理，但集中到一次大写入后，不能再把“调用过 flush”视为“整份账本已写出”。

   具体修法：检查实际写入长度和 flush 结果，保留未写出后缀或明确报告 `exitAccountsComplete=false`。GPU 正常收尾与账本完整写出应分别记录，不能混为一个成功标志。

**已核对、未发现的问题**

- 正常无错误路径中，各次 `add()` 的字节顺序保持不变，且 flush 在 `session.reset()` 前执行。
- 正常账户生成时已经取得 stats 快照并检查 `normalTeardown`；开关没有提前释放 GPU 资源，也没有替代原有 Finish/retirement。
- 该开关没有直接改变宿主返回码，但日志 I/O 失败目前也不会反映到返回码或单独完成状态。

**建议默认值：需先修。**

先改为记录级批量写出并检查写入结果。退出账本承担故障取证用途时，不建议直接接受“正常退出更快，但退出期间出错会整块丢失”的默认行为。

**还缺的测试**

- 在账本前段、shadow 大记录中段、最终 flush 前后分别注入异常或异常退出，核对可保留的完整记录和最后阶段。
- 注入格式化缓冲分配失败，验证 fallback 没有重复、缺字节或改变顺序。
- 注入短写与 flush 失败，验证不会宣称账本完整。
- 正常路径逐字节比较开关两相的账本内容，并单独核对账本、session reset、成功报告和 exit-intent 的阶段顺序。

---

**六、WARVK_D3D9_HOST_EXIT_GL_WATCHER**

**发现**

1. **PID/类名校验与 WM_CLOSE 发送之间存在身份空窗；当前代码不能保证消息发送时仍命中原窗口。把握程度：推断。**

   位置：[tools/render_host/dxvk_core/d3d9_host_main.cpp:113](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:113)，尤其 `119–130`。

   触发条件：枚举找到本进程、类名为 `Palette Watcher` 的 HWND 后，窗口在 `OpenThread()` 或 `PostMessageW()` 前被销毁、替换；或者本进程存在同名但并非预期 watcher 的窗口。

   后果：

   - PID 只在枚举回调时检查。发送前没有重新核对 HWND 的 PID、TID 和类名。
   - `OpenThread(SYNCHRONIZE, …)` 成功不固定 HWND 身份，且代码在 OpenThread 失败时仍会发送 `WM_CLOSE`。
   - 若 HWND 在空窗内失效并被复用，存在消息命中另一窗口、乃至另一进程窗口的风险；本轮未做运行复现。
   - 类名加 PID 也不是 opengl32 watcher 的充分来源证明。代码统计了多个匹配窗口，却始终只选择第一个，没有唯一性要求。

   具体修法：至少要求唯一匹配、成功取得并校验线程身份、发送前复核窗口身份；这些只能缩小竞态，无法把枚举得到的裸 HWND 变成稳定所有权。缺少可证明的 watcher 所有权时，应保持诊断开关关闭，不把类名探测升级成默认退出协议。

2. **500 ms watcher 等待没有纳入父进程已开始的退出预算，且最终成功报告早于该动作。把握程度：确定。**

   位置：`tools/render_host/dxvk_core/d3d9_host_main.cpp:130、1258、1436–1454`；[tools/render_host/d3d9_connection.h:163](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:163)，尤其 `167–179`。

   触发条件：宿主已经回复 Finish，父 worker 开始最多 5 秒的 primary exit wait；账本和其他收尾已消耗部分预算，而 watcher 不及时退出。

   后果：这里无条件再获得最多 500 ms 等待，未使用父进程剩余预算。例如其余退出工作已接近 5 秒时，新增等待本身就可能使父进程从“自然退出”变为 primary wait 超时。

   `ClosePaletteWatcher()` 不传播 `PostMessage` 失败或 `WAIT_TIMEOUT/WAIT_FAILED`，正常路径仍意图返回 0。因此**预期返回码没有被主动改写，但父进程实际观察到的退出阶段和成功分类可以改变**。此前输出的 `ok=true/normalTeardown=true` 只证明已有会话收尾；它在 watcher 动作和最终进程退出之前生成，不能单独证明进程自然退出。

   具体修法：若继续保留实验入口，给它明确的剩余退出预算与独立完成状态，并以父进程实际观测的退出结果作为最终收据。不能把 `posted=true` 当成 watcher 已结束。

**已核对、未发现的问题**

- 当前枚举不是全局按类名直接发送；它确实在枚举时限制为 `GetCurrentProcessId()`。因此“必然向玩家窗口发 WM_CLOSE”不成立。
- 调用发生在 `session.reset()` 之后，正常 GPU/WSI 收尾之前没有调用这一 helper。
- helper 使用 `PostMessageW` 和有界线程等待，没有在这里直接终止线程或进程，也没有直接替换正常返回码。
- 未找到验证该窗口类确由预期模块/过程拥有的代码；对未公开 watcher 行为的依赖仍然存在。

**建议默认值：关。**

当前代码不足以把这一针对内部窗口的诊断干预作为通用默认收尾。进程退出问题应优先通过已有可控的宿主窗口所有权路径闭合；在此之前，仅凭同名窗口和几次正常返回记录，不足以授权默认发送 `WM_CLOSE`。

**还缺的测试**

- 同进程同名假窗口、其他进程同名窗口、零匹配、多匹配，验证拒绝规则及唯一性。
- 枚举后窗口销毁、线程退出、发送前窗口身份变化；确认不会向不再属于原目标的窗口发送消息。
- `OpenThread` 失败、`PostMessage` 失败、线程等待超时，检查日志明确区分各阶段，且不把已投递当成已完成。
- 在剩余退出预算少于 500 ms 时进入 helper，检查父进程 primary exit wait 的结果。
- 分别核对正常返回、GPU/窗口错误返回、异常 fail-stop 路径是否执行 helper，以及父进程实际观察的退出码和阶段；不能只读取 helper 之前生成的正常收尾 JSON。