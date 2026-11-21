# x64 代理线 P2：把每帧的同步往返改成有序异步

日期：2026-11-18。对应 `docs/plan/2026-11-17-x64-proxy-performance-plan.md` 第 10 节第 3 项。
同步调用的清单与次数见 `2026-11-17-x64-sync-call-inventory.md`。

## 1. 做了什么

x86 前端新增四个 profile 开关。全部默认关，取值只能是 `1`，并且要求已经开了异步绘制批（`WARVK_D3D9_ASYNC_BATCH`）；后两个还要求用命令环（`WARVK_D3D9_COMMAND_RING`）。条件不满足时连接在构造阶段直接报错，不会悄悄退回同步。

| 开关 | 改成异步的记录 | 每个游戏帧省掉的同步往返（Lost Temple） |
|---|---|---|
| `WARVK_D3D9_ASYNC_UPLOADS` | 纹理的 Begin / Chunk / Commit，以及 `SetLOD` | 约 96 |
| `WARVK_D3D9_ASYNC_FLUSH` | `EndScene` 的 `FlushDraws`、局部清屏 `ClearRegions` | 2 |
| `WARVK_D3D9_ASYNC_SCENE` | `FrameScene`、`StageMarker` | 8 |
| `WARVK_D3D9_PRESENT_AHEAD` | `Present` | 1（改为最多领先一帧） |

四个都开之后，稳定游戏帧里 x86 不再有“发一条、等一条”的往返，只剩 `Present` 里等上一帧。

这四项会改变同步与异步记录的数量，实验夹具里有按数量写死的断言，所以做成 profile 开关而不是直接改默认行为。验证通过后加进探针的 `product` profile（`AutoTest/run_x64_game_proxy_probe.py` 的 `PROFILES["product"]`）。

## 2. 共同的规则：有序准入

异步记录走已有的 `enqueueOrdered`：先把本地攒着的绘制批发出去，再发这条记录，不等回复。宿主按环上的顺序逐条执行，所以相对前后的绘制、上传、状态，顺序与同步时完全一样。

宿主对异步记录只接受成功。结果不是 `D3D_OK` 时，宿主发布 `AsyncRefused` 故障并停止，x86 在下一次观察时 `failClosed`，游戏在下一次 `Present` 看到终止报告。也就是说，**原来能靠返回值软失败的调用，改成异步后失败即终止**。所以每一类记录改异步之前，都要确认 x86 这边能提前挡住宿主会拒绝的情况，或者像下面第 4、5 节那样给宿主留一个“不算拒绝”的出口。

账目：原有的 `warvk-d3d9-ordered-async/v1` 一行不变（`remoteSucceeded == uploadsPublished + statesPublished` 仍然成立，门禁在断言它）。新增记录单独记在 `warvk-d3d9-ordered-extra/v1` 一行里：`texturePublished`、`flushPublished`、`scenePublished`、`presentsAhead`、`sceneRefusalsObserved`、`presentAheadWaits`、`remoteSucceeded`。分析脚本的环记录恒等式已加上这几项。

## 3. 纹理更新与刷新（ASYNC_UPLOADS / ASYNC_FLUSH）

`Texture::send` 在开关开时调 `enqueueOrdered`，失败则 `failClosed` 并返回 `D3DERR_DEVICELOST`。`UnlockRect` 里的 Begin、每个 Chunk、Commit 都走它；`SetLOD` 也是。Abort 只在同步路径上存在（异步路径失败就是终止，没有可中止的事务）。

x86 在发送前已经做了宿主的全部准入检查（级别、矩形、格式、字节数），所以正常运行时宿主不会拒绝。

这一项建立在宿主的 staging slice 上（`WARVK_D3D9_UPLOAD_SLICES`，现在默认开）：宿主把上传记录进当前流列表，不再每次上传都等 GPU。两项合起来，纹理更新在两端都不再打断一帧。

`FlushDraws` 与 `ClearRegions` 没有返回数据，改异步只是不等。

## 4. 场景记录（ASYNC_SCENE）

`FrameScene` 和 `StageMarker` 是数据层（`war3_proxy_frame.cpp`）发给宿主的每帧记录。同步时宿主一旦拒绝其中一条，数据层的 `Sent()` 就永久停止发送（“两端对这一帧的理解不再一致，不猜”），游戏自己的画面不受影响，只是特效和阴影停掉。这是软降级，不能因为改异步变成终止。

做法：

1. 宿主执行异步的 `FrameScene` / `StageMarker` 得到非成功结果（设备丢失除外）时，**不发布故障**。它把环控制块里的 `sceneRefused` 加一，把会话标成 `sceneStopped`，然后照常发布这条记录“已执行”。
2. 从这一刻起，宿主忽略之后收到的所有场景记录（计入 `sceneRecordsIgnored`）。这与同步时“x86 不再发送”等价：那些记录等于没发过。
3. x86 每次要发场景记录前先读 `sceneRefused`。非零就不发，直接返回 `D3DERR_INVALIDCALL`，数据层据此停止，和收到同步拒绝时一样。

x86 发现拒绝会晚几条记录（最多到下一帧的第一条），这段时间里多发的记录由第 2 步吃掉。停止在一个宿主会话内是永久的；数据层只在新设备连接（新的宿主会话）时恢复发送，所以宿主这边不需要“恢复”。

`sceneRefused` 用的是 `ConsumerBlock` 偏移 140 处原来的保留字节，布局大小不变。

宿主会拒绝的情况有三种：阶段顺序不对（`sceneTracker.mark`）、不能在此处插入特效（`canInsertBeforeUi`）、特效执行器拒绝（`ProfileRejected`）。前两种 x86 用同一个 `SceneTracker` 自己先判过，正常不会发生。

**对实验夹具的影响。** 夹具直接调前端 DLL，不经过数据层，而且会故意发“只有宿主才判得出”的非法阶段标记，要求当场拿到 `D3DERR_INVALIDCALL`，然后继续发合法记录。开了这个开关后，这样的标记不会当场被拒，而且宿主从此不再读场景记录，后面每一帧的特效都会停掉。产品不会出现这种序列（第一次被拒就永久停发），所以夹具这样处理：

- 特效门夹具（`test_render_host_d3d9_scene_effects.cpp`）在开关打开时跳过这 3 处探测，并在回执里记 `refusalsSkipped=3`；门按臂核对“拒绝数 = 10 − 跳过数”和宿主的 `insertionsRefused = 3 − 跳过数`。
- GPU 参照门的注解用例把两次宿主拒绝按数量钉在两端的账目里，所以它的 P2 对照组不带 `ASYNC_SCENE`（只带纹理、刷新、Present 三项）。
- “告知但不终止”本身由环故障夹具的新用例 `scene-refused` 验证（`test_render_host_ring_faults.cpp`）：发一条未协商注解的异步阶段标记，等宿主在环控制块里报出拒绝，再发一条，然后照常清屏、同步屏障、`Finish`；要求会话一直健康、宿主以 0 退出、宿主账目是 `refused=2 ignored=0`。原来的 `async-refused` 用例改用未协商的绘制注解（`AnnotateDraws`），它仍然必须终止会话。

## 5. Present 提前一帧（PRESENT_AHEAD）

原来 `Present` 是同步调用：x86 等宿主把这一帧画完、交给窗口系统、等 GPU 完成之后才返回。x86 在这段时间里什么都不做。

现在：x86 把 `Present N` 当普通有序记录发出去，然后只等 `Present N−1` 被宿主执行完就返回。宿主在画第 N 帧的时候，x86 已经在准备第 N+1 帧。x86 最多领先宿主一帧，长期帧率仍由较慢的一端决定，输入到画面的延迟最多增加一帧的排队时间。

细节：

- **跳帧**：窗口先变了大小、`Reset` 还没来时，宿主丢掉这一帧并返回 `PresentSkipped`。x86 的 `presentFrame` 本来就把它当成功。宿主的环循环里，异步 `Present` 得到 `PresentSkipped` 不算拒绝。
- **其他失败**：设备丢失照旧走 `DeviceLost` 故障；别的失败按异步规则终止。
- **排空**：不需要专门的排空点。环是先进先出的，`Reset`、`Finish`、销毁、任何读回都是同步调用，它们返回时前面的 `Present` 一定已经执行完。
- **不适用的情况**：带“已呈现画面读回”的实验 profile 需要回复里的数据，仍然同步（判断条件是调用方没有要结果缓冲）。
- **离屏会话**：不出帧的会话里，宿主对 `Present` 回答 `E_NOTIMPL`。这是要等的回复，不是可以领先的一帧，所以连接构造时如果不是出帧会话，这个开关不生效（`presentAhead = presentAhead && presenter`）。冻结轮的阴影门发现了这一点：第一版里离屏夹具的 `Present` 被当成异步拒绝，会话终止。游戏里宿主总有交换链，不受影响。画面门的夹具都是离屏或带读回的，所以这个开关只在游戏实跑里被真正用到。
- **统计**：`Present` 在同步剖面里仍记一次，时间是 x86 在 `Present` 里实际阻塞的时间（等上一帧）；帧时间序列的含义不变。

## 6. 这一轮顺带改的传输细节

- 环满时，生产者把 `waiting` 写成 2（等空间）。宿主读走一条记录、空间归还之后立刻发信号，不再等这条记录执行完。纹理改异步后环上的字节量变大，这条路径比以前更常走。

## 7. 验证

- 上传顺序夹具：六个臂像素哈希相同（`2026-11-18-x64-switch-review.md` 第 5 节）。
- 游戏实跑（同一会话，世界帧 fps）：Lost Temple 154 → 195–200，EchoIsles 124 → 147，生与死 26 → 55；账目恒等式成立，终止原因 `none`，`sceneRefusalsObserved` 为 0，宿主约 94 ms 退出。
- 冻结轮（宿主 `x64-f5-host-build-20261118-r1`，x86 `x86-f4-build-20261118-r1`）的 P2 对照组：阴影门（四个开关，该臂异步发出 500 条场景记录、75 次刷新）、特效门（四个开关，407 条场景记录）、GPU 参照门（三个开关，5 个用例带开关）全部通过；帧哈希与 5.2 冻结轮逐项相同（352 / 77 / 52 项，0 差异）。产物：`AutoTest/artifacts/x64-f5-{shadow,scene-effects,reference-gpu-vvl}-p2-20261118-r1/`。
- 宿主在终止性的异步拒绝时多写一行 stderr：`D3D9 host asynchronous record refused op=… sequence=… result=…`（原来只有不带操作码的故障行，查离屏 `Present` 那次就卡在这里）。
- 11-18 后半夜补上的覆盖（窗口生命周期门，`run_d3d9_factory_clear_gate.py --window-life`，夹具由 Astra 6.2 写）：`PRESENT_AHEAD` 下的 `PresentSkipped`（`resize-p2` 臂，恰好跳过 1 帧）、宿主“忽略后续场景记录”的计数分支（`scene-stopped-reset`）。产物：`AutoTest/artifacts/x64-f6-window-life-20261118-r1/`。

## 8. 之后可以做的

- **宿主 Present 不等 GPU**：四个开关都开后，x86 不再被宿主卡住，瓶颈转到宿主自己。宿主每帧在 `Present` 里两次等 GPU 完成（提交前一次、交给窗口系统后一次），CPU 记录与 GPU 执行完全不重叠。改成“只等上一帧完成”可以让两者重叠。这要动故障清理所依赖的“静止”前提，单独立项。
- **同步等待的唤醒风暴**：x86 等一条同步记录时，宿主每执行完一条排在它前面的记录都会唤醒它一次。让生产者把等待的序号写进控制块、宿主只在到达该序号时发信号即可消除。四个开关都开后同步等待已经很少，这项收益有限。
- **创建与销毁改异步**（清单里的 P2-7）：需要 x86 这边镜像宿主的句柄准入规则。只影响加载阶段，不影响稳定帧。

## 9. 第二次独立审查（Codex，11-18）与处置

原文：`2026-11-18-x64-p2-review-codex.md`。逐条核对过代码，结论如下。

| # | 发现 | 核对 | 处置 |
|---|---|---|---|
| 1 | 窗口缩小后 `StateBlock::Apply` 恢复了旧的、超出新目标的视口；之后的 `Clear` 生成越界区域，宿主拒绝；开了 `ASYNC_FLUSH` 时这是终止 | 成立。设置面板每帧“存状态、画、恢复”正好是这个序列，用户拖窗口时可能撞上。异步绘制批本来也会因同一个旧视口被拒 | **已修**（前端）：`Device::apply` 只在捕获的视口仍然装得进当前目标时才恢复，否则保留缩放后设好的整目标视口；`Clear` 在发出前把每个区域与目标求交。夹具：窗口生命周期门的 `stale-viewport` 两臂（缩窗 → Apply 旧块 → 带矩形与不带矩形的 Clear → 两帧），同步与 P2 都过 |
| 2 | `PRESENT_AHEAD`：`Present` 返回后窗口立刻被销毁，宿主执行这一帧时窗口校验失败，正常退出变成宿主故障 | 成立。同步时这一帧在返回前已经执行完。实跑里退出都干净，但时间窗确实存在（最多一帧） | **已修**（前端）：设备窗口的 `WM_DESTROY`（此时窗口句柄与会话标记都还有效）里等最后一个提前发出的 `Present` 被执行完（`Connection::settlePresentAhead`，只读宿主已公布的进度，最多等 5 秒），再让窗口销毁。宿主的窗口身份校验没有放宽。夹具：`destroy-ahead` 两臂；P2 臂里窗口销毁实际等了 387 ms（`presentAheadSettles` = 1），同步臂 1 ms |
| 3 | 新旧两端没有能力识别：环版本没变，语义变了 | 成立。两端总是同一次构建出来的，但配错时应该在挂接时失败 | **已修**：环版本 1 → 2 |
| 4 | 软拒绝发生在已进入的世界场景里时，后面的 Exit 被忽略，`sceneTracker.active` 一直为真，之后的 `SessionReset` 永远被拒 | 成立。产品数据层自己先判阶段顺序，正常不会发出这种序列，但宿主承诺的是“降级而不是卡死” | **已修**（宿主，Astra 6.2 实现）：首次软拒绝时清掉半帧的场景状态（场景包、阴影包、阴影帧，`sceneTracker` 的 active 与本帧标志；代际和累计计数保留）。夹具 `scene-stopped-reset`（进入、嵌套进入被拒、退出被忽略、`SessionReset` 成功）在 GPU 参照门里通过，同时覆盖了“忽略后续记录”的计数分支 |
| 5 | 资源表扩容失败发生在 GPU 资源建好之后，宿主直接终止 | 成立，但要 64 位宿主里一次不到 1 MiB 的分配失败 | **已修**：宿主在会话开始时一次预留整张资源表（65,537 项，不到 1 MiB），之后的创建提交不再分配；预留失败时会话在任何资源存在之前就不启动。单元测试用全局 `operator new` 计数钉住“预留之后提交不分配” |
| 6 | 帧时间直方图不是一致快照；直接调 `PresentEx` 的路径不计数 | 成立。A/B 门用两次快照的桶差值算分位数，不依赖 `frames` 与桶之和相等；War3 只走 `Present` | 记下；输出里注明覆盖范围 |
| 7 | `profileAhead` 记了等待时间但没有样本数，全是提前 `Present` 的区间里看不到按 API 配对的明细 | 成立，只影响诊断 | 记下，以后加独立的样本计数 |

另外两点说明采纳：“最多领先一帧”的口径是 **`Present` 返回时** 最多剩一帧未执行（等待期间环上可以有两个）；“宿主的拒绝 x86 都能先挡住”只对输入准入成立，GPU 执行期的失败仍然是终止。

纹理上传、`FlushDraws`、两种环等待的切换、资源表扩容后的槽位地址、`ConsumerBlock` 布局、退出账目、分段提交的帧内顺序：审查都查过，没有发现问题。
