# x64 代理线：六个开关的审查与默认值

日期：2026-11-18。对应 `docs/plan/2026-11-17-x64-proxy-performance-plan.md` 第 10 节第 1 项。

审查由 Claude Fable 逐行读代码完成，并请 Codex Astra 在只读沙箱里做了一份独立的对抗式审查（原文见 `2026-11-18-x64-switch-review-codex.md`）。两份结论不一致的地方在第 3 节逐条说明。

## 1. 结论

六个开关全部改为**默认开**。每个都保留一个“关”的取值，供对照臂使用。

| 开关 | 所在 | 默认 | 关的取值 | 其他取值 |
|---|---|---|---|---|
| `WARVK_D3D9_RING_WAKE_FIX` | x86 前端 `d3d9_connection.h` | 开 | `0` | `alternate:N` |
| `WARVK_D3D9_UPLOAD_SLICES` | 宿主 `d3d9_backend.cpp` | 开 | `0` | `alternate:N` |
| `WARVK_D3D9_PRESENT_WAIT` | 宿主 `d3d9_backend.cpp` | 开（阻塞等待） | `poll` | `alternate:N` |
| `WARVK_D3D9_DYNAMIC_CPU_COPY` | 宿主 `d3d9_backend.cpp` | 开 | `0` | `alternate:N` |
| `WARVK_D3D9_HOST_EXIT_BUFFERED` | 宿主 `d3d9_host_main.cpp` | 开 | `0` | — |
| `WARVK_D3D9_HOST_EXIT_GL_WATCHER` | 宿主 `d3d9_host_main.cpp` | 开（关闭 watcher） | `keep` | — |

未设置或取值不认识时都按“开”处理。

这六项都只改等待方式或宿主内部实现，不改线协议，也不改同步与异步记录的数量，所以直接在代码里翻默认值。会改变记录数量的新行为（纹理更新异步等）另用 profile 开关，见 `2026-11-18-x64-p2-async-design.md`。

## 2. 逐项依据

### 2.1 RING_WAKE_FIX

问题：`ringAwait` 第一次调用时才读基线，而调用方在此之前已经检查过条件。宿主在这两步之间发布的进度不算“相对基线的变化”，生产者就会睡满 `RecheckMs`（1 秒），因为宿主只给它看见正在等待的生产者发信号。

修法：第一次调用只取基线就返回，调用方重新检查条件；之后的调用比较的基线早于那次检查。

为什么正确：
- 基线单调，且早于调用方最近一次条件检查，所以“条件已成立却去睡”不会再发生。
- `setWaiting(true)` 之后再观察一次进度，与宿主“发布 `applied` 后读 `waiting`”都是 `seq_cst`，两边至少有一边看见对方。
- 首次返回不重置 `wait.moved`，无进度时限（10 秒滚动）与调用截止时间（30 秒）的计时不变。

### 2.2 UPLOAD_SLICES

改法见 `2026-11-17-x64-p1-staging-slices.md`。审查确认：
- **保序**：切片拷贝记录在当前流列表里，与前后 draw 共用一个上下文。DXVK 的 `copyBufferToImageHw` 只在目标图像在当前列表里还没被用过时，才把拷贝提前到 InitBuffer；否则留在 ExecBuffer 里按序执行。
- **寿命**：`DxvkStagingBuffer` 不复用内存；换块只是放掉分配器自己的引用，命令列表仍持有缓冲。
- **读者**：`initializedLevels` / `uploaded` 在“记录成功”后发布。draw 用它判断能否按序引用；`ReadContent` 读的是宿主保留的 CPU 副本；真正的 GPU 读回另走 `settle()`。没有读者把“已记录”当成“GPU 已完成”。
- **失败**：额度查询、等待、分配、记录、提交任一步抛异常，整个 Owner 被隔离，不留半个事务。

本轮修掉一个缺陷：流里只有上传切片时 `hasPendingDraws()` 也返回真，实验夹具的 `DiagnosticCheckpoint` 因此被拒。现在用 `streamHoldsWork` 区分“流里有绘制”和“流里只有上传”。

### 2.3 PRESENT_WAIT

阻塞等待与轮询检查的是同一批条件（真实的 `presentSignal->value()`、提交状态），期限也相同；事件只是唤醒线索，陈旧事件不算退休成功。`CallbackFence::setCallback` 在值已达到时立刻执行回调，注册窗口内完成不会漏。

### 2.4 DYNAMIC_CPU_COPY

所有动态顶点缓冲的写入都经过同一个 `uploadBuffer()`，写到映射的字节同时写进镜像；DISCARD 先搬旧的已初始化前缀再覆盖。对**已初始化的字节**，镜像与映射逐字节相同。投射体几何在 `AppendShadowCaster` 里当场拷走，不会长期借用镜像指针。

### 2.5 HOST_EXIT_BUFFERED

正常路径下输出的字节与顺序不变，写出发生在 `session.reset()` 之前。

### 2.6 HOST_EXIT_GL_WATCHER

opengl32 在给别的进程的窗口设像素格式时会起一个 “Palette Watcher” 线程。它若活到 `ExitProcess`，会先被杀掉，随后加载器的卸载回调轮询它，最多睡 100 × 50 ms，宿主退出就可能超过父进程 5 秒的等待。修法是在正常收尾和账本之后，按 opengl32 自己的方式（给它的窗口发 `WM_CLOSE`）请它结束，最多等 500 ms。窗口不在或线程没结束时，退出过程与原来相同。

这依赖 opengl32 未公开的行为。长期做法是让宿主自己拥有呈现窗口，届时这个开关可以删掉。

## 3. 对 Codex 发现的处置

Codex 的建议是：`RING_WAKE_FIX` 开；`UPLOAD_SLICES`、`PRESENT_WAIT`、`DYNAMIC_CPU_COPY`、`HOST_EXIT_BUFFERED` 先修再开；`HOST_EXIT_GL_WATCHER` 关。它的每条发现都对照代码核实过，处置如下。

| # | Codex 的发现 | 核实 | 处置 |
|---|---|---|---|
| 1.1 | 环满等待时，宿主读走记录（空间已归还）后要等这条记录执行完才通知生产者 | 成立 | **已改**：生产者等空间时把 `waiting` 写成 2；宿主读到记录后若见到 2 就立刻发信号。执行完之后的通知不变 |
| 1.2 | 同步调用的 30 秒期限不覆盖环满准入；首次返回早于健康与期限检查 | 成立，是原有结构 | 不改。环满准入由 10 秒滚动无进度时限约束；“结果已到达则优先于刚好到期”是可接受的语义 |
| 2.1 | 16 MiB 额度记的是上传字节，不是 staging 实际占用 | 成立 | 不改实现，改说法：这是**流量额度**，不是物理内存上限。最坏情况是每个 4 MiB 块只放一个略大于 2 MiB 的切片，实际占用约为额度的两倍，仍有界；占的是 x64 宿主内存，不是 x86 |
| 2.2 | 额度等待在健康检查之前就查询和等待 Vulkan；每个标记各有一段 5 秒 | 成立 | **已改**：查询前、每次等待前都先 `healthy()`；整次额度等待共用一个 5 秒期限 |
| 3.1 | `waitForSubmissionUntil` 先无期限地拿提交队列的锁 | 成立，但不是这个开关引入的 | 不改。这个函数在 E0 之前就被其他等待使用；锁只被短暂持有。上游在提交失败（非设备丢失）分支里持锁重入的问题记为已知风险，外层由 x86 的 10 秒无进度时限兜底 |
| 3.2 | `quarantine()` 路径不归还 `timeBeginPeriod` | 成立 | **已改**：隔离 Owner 前先归还 |
| 4.1 | 投射体拷贝可能读到已初始化前缀之外，镜像那里是零，映射那里是未定义内容 | 成立 | 不改。前缀之外的字节本来就未定义，主绘制不会读到（`prepareDraw` 已校验）；镜像给出的是确定的零，比原路径更稳定。文档不再写“全部字节相同”，改写为“已初始化字节相同” |
| 4.2 | 内存账本没把顶点镜像算进去 | 成立 | **已改**：`diagnosticMemory()` 同时统计 `indexBytes` 与 `vertexBytes` |
| 5.1 | 整块缓冲使退出期崩溃时丢失全部账本 | 成立 | **已改**：攒满 32 KiB 且到行尾就写出；析构时也写出（异常展开不丢） |
| 5.2 | `flush` 不检查短写 | 成立 | **已改**：循环写完；写不全时置 `complete=false`，并另写一行 `exit-accounts-incomplete` 里程碑 |
| 6.1 | 枚举到发消息之间窗口身份可能变化；多个匹配时只取第一个 | 部分成立 | **已改**：要求恰好一个匹配、先打开线程句柄、发消息前复核 PID / TID / 类名。HWND 的高位带代数计数，同一句柄值在这几微秒内被复用实际上不会发生 |
| 6.2 | 500 ms 等待不计入父进程的退出预算 | 成立 | 不改。不关 watcher 时退出可能多花 5 秒，这 500 ms 上限是净收益 |

与 Codex 的分歧只有一处：它建议 `HOST_EXIT_GL_WATCHER` 保持关。这里保留为开，理由是收紧校验后误发的条件实际不成立，而关掉它的代价是宿主退出超时、走强制结束路径。这一项依赖未公开行为的事实不变，已在 2.6 写明。

## 4. 还缺的测试

以下分支目前没有夹具强制命中，默认值翻转靠的是静态审查加正常路径的运行验证：

- `UPLOAD_SLICES` 的额度等待分支。上传顺序夹具里 `creditWaits` 一直是 0：提前刷新加上 GPU 够快，额度从未真正等过。需要一个诊断用的小额度开关来强制命中。
- 额度等待中的设备丢失、等待超时、提交失败。
- LRU 逐出后经 `ReadContent` 恢复，且切片还只在记录状态时立刻读。
- 环满时宿主“读后即通知”的时序（需要能在 `apply()` 里停住宿主）。
- 退出账本的短写与写失败注入。

## 5. 验证记录

- 上传顺序夹具，六个臂（参考、切片关、默认、纹理异步、纹理与刷新都异步、无 stream 的环）：全部通过自带的像素与纹素校验，像素哈希相同（`13c9a69ab106d325`），宿主都以 0 退出。产物：`AutoTest/artifacts/x64-f1-async-uploads-20261118-order-r1/`。
- 冻结轮（11-18）：宿主冻结构建 `x64-f5-host-build-20261118-r1`，x86 冻结构建 `x86-f4-build-20261118-r1`（之后 `src/` 没有再改）。阴影门、特效门、GPU 参照门各跑三组：默认（六个开关都开）、六个开关全关（`--remote-env`，对照）、四个 P2 开关（`--ring-env`，只作用于“流式 + 命令环”臂）。九次全部通过；帧哈希与 5.2 冻结轮（`x64-phase52-*-20261117`）逐项相同：阴影门 352 项、特效门 77 帧、GPU 参照门 52 项，三组都是 0 差异。切口门正向 19 符号、24 边，与基线相同（反向 487 → 519）。
- 第一次冻结轮（f4 宿主）里 GPU 参照门三组都失败在环故障夹具的 `async-refused` 一例：它用“未协商注解的异步阶段标记”来制造终止，而场景记录的拒绝在 11-18 改成了“告知但不终止”。这是锚点跟着语义改，不是放宽：该例改用未协商的绘制注解（仍然终止），并新增 `scene-refused` 一例钉住新语义。详见 `2026-11-18-x64-p2-async-design.md` 第 4、7 节。
- 静态测试：读到今晚改动文件的 31 个模块、335 条，失败的仍是原来 5 个（`test_dxso_shader_module_cache_contract`、`test_host_surface_integration`、`test_render_host_initialization`、`test_render_host_surface_owner`、`test_surface_owner_continuation`）；294 个 `test_*_static.py` 脚本只有已知的 `test_shadow_receiver_ab_env_static` 失败。没有新增失败。
- **第二次冻结轮（f6，11-18 后半夜）**。宿主冻结构建 `x64-f6-host-build-20261118-r1`，包含 f5 之后的全部改动：环版本 2、旧 StateBlock 视口与 `Clear` 求交、P4 的 `DrawFixedDeltaBatch`、首次场景软拒绝时清半帧状态、`WM_DESTROY` 时等最后一个提前的 `Present`、宿主一次预留整张资源表。`src/` 没有再改，x86 仍是 f4，切口门的输入没变，没有重跑。
  - 阴影门、特效门、GPU 参照门 ×（默认、六开关全关、P2、P2 + 固定批 + 差量 + 逐条校验）十二次全部通过；帧哈希与 5.2 冻结轮逐项相同（352 / 77 / 52 项，四组都是 0 差异）。产物：`AutoTest/artifacts/x64-f6-{shadow,scene-effects,reference-gpu-vvl}[-off|-p2|-delta]-20261118-r*`。
  - 窗口生命周期门（新，`--window-life`）5 个臂通过；反证：去掉两处前端修法另做冻结构建（`x64-f6n-host-build-20261118-r1`），`destroy-ahead-p2` 与 `stale-viewport` 两臂如预期失败（宿主在窗口校验处自我终止，退出码 4），原样恢复后再次 5 / 5 通过。产物：`x64-f6-window-life-20261118-{r1,n1,r2}`。
  - 连接层 CPU 门通过（`x64-f6-connection-cpu-20261118-r1`）。
  - 途中保留的失败回执，都是锚点或对照组环境的问题，不是画面：GPU 参照门默认组 r1（环的单元测试把“版本 2”写死为错误版本，改成“当前版本 ±1 都被拒”）；差量组 r1（差量开关要求先开固定批，三个门的环臂没开，设备创建失败）；GPU 参照门差量组 r2（该门钉死了哪些臂开固定批，于是加了 `--fixed-env`，只对本来就开固定批的臂加差量开关）。
  - 静态测试：读到今晚改动文件的 25 个模块、530 条，失败的是原来 5 个，外加 `test_render_host_memory_destroy_mutation`（旧问题：10 月的两个测试脚本序号表不一致，文件与提交版相同，上一轮的选取范围没有包含它）；294 个 `test_*_static.py` 脚本只有已知的 `test_shadow_receiver_ab_env_static` 失败。`test_scene_effects_static` 的锚点跟着改了一处（`clearScene()` 的调用点 4 → 5，并钉住新增的那一处）。
- **第三次冻结轮（f7，11-18 清晨）**。f6 之后只改了 `d3d9_ff_delta_wire.h` 里的差量编码器（输出字节不变，去掉了每个字一次的数据相关分支；起因和证据见 `2026-11-18-x64-p4-format-decision.md` 第 7 节）和线格式测试。这个头文件是宿主构建的输入，所以宿主重新冻结：`x64-f7-host-build-20261118-r1`。x86 仍是 f4。
  - 阴影门、特效门、GPU 参照门 ×（默认、六开关全关、P2、P2 + 固定批 + 差量 + 逐条校验）十二次全部通过，一次重跑都没有；帧哈希与 5.2 冻结轮逐项相同（352 / 77 / 52 项，四组都是 0 差异）。产物：`AutoTest/artifacts/x64-f7-{shadow,scene-effects,reference-gpu-vvl}[-off|-p2|-delta]-20261118-r1`。
  - 窗口生命周期门 5 / 5（`x64-f7-window-life-20261118-r1`）；连接层 CPU 门通过（`x64-f7-connection-cpu-20261118-r1`）。
  - 冻结的 f6、f7 宿主都在游戏里跑过了：三图 A/B `x64-phase52-ab-delta-20261118-r2`（f6）与 `x64-phase52-ab-delta-f7-20261118-r1`（f7），全部检查通过。
- 游戏内结果与 5.2 第二轮见计划第 12 节。
