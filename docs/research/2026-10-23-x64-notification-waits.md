# X1b：事件唤醒、实际等待成本及完整六例 GPU 开发门

日期：2026-10-23。结论：**Connection 的三处 Sleep 轮询已由固定事件唤醒替换；
双架构 CPU、真实 IPC 故障及两轮完整六例 GPU 开发子集通过。**
不是完整 D3D9、产品异步 owner、游戏渲染迁移、玩家内存/FPS 或发布验收。
`Game.dll` 仍被开发代理拒绝。旧失败保持失败，不由新结果追认。

## 1. 这次收紧的边界

链路仍是实际 x86 COM/Connection → 四槽 SPSC → 一个 worker 私有 Batch →
认证管道 → 既有 x64 DXSO/DXVK。只有 Clear 可在有界接纳后返回；其他调用仍是同步屏障。

- 每 Connection 两个匿名、不可继承的 auto-reset event，没有全局锁、逐调用 event 分配、
  `ResetEvent`、系统计时器精度改动或增容。
- 工作通知在真实发布、producer close、故障处发出；进展通知在真实 dequeue 释放槽位、
  reply 验证/CPU ACK/done 发布后及故障处发出。槽位释放不等于 GPU 完成。
- 事件只催促重新检查；实际 queue 状态和 `done.acquire` 才授权推进。过期/合并通知不能
  完成调用。消息到达早于等待也不依赖下一次轮询。
- 共享 `tryTakeBatch/waitForWorkerBatch/closeProducer` 用于实际 worker 和 CPU 夹具，
  不在测试里重写另一套队列或等待算法。
- 原 admission 5 秒、call 总计 30 秒、transport 5 秒、primary exit 5 秒和 done-first 保留。
  空闲 worker 以有限 5 秒分块复核，不把健康空闲判为错误。事件在 worker join 后才销毁。
- `call` 异常先 `failClosed()`，再输出诊断；两个等待方都被唤醒。实验析构 join/IO drain
  仍可能阻塞，外层 watchdog 仍必需，不能直接搬进游戏回调或 DllMain。

正式源 `tools/render_host/d3d9_connection.h` SHA-256：
`9D833BB46BB9392A969DE3DA91ABDC9E3903568B500BB5E7F142B2D02C460555`。
proxy 源未改：`1B7F31740D02DCF92B7DFD2C00B7447759D9D791628DB449F4AA8ADEF4A1C157`。
Sleep 前像保存在 `AutoTest/artifacts/x1b-notification-preimage-20261023/`，
本轮固定源副本在 `x1b-notification-source-20261023/`。

## 2. CPU 与真实 IPC 验证

均由主线程独占构建/native lease，BelowNormal、串行、restricted-Medium 非输入 Win32
Desktop 执行。没有全局输入、桌面切换或游戏启动；全部正常清场、原始日志/输入保全。

| 验证 | 本轮结果 | 明确不能外推 |
|---|---|---|
| 实际 Connection CPU，x86/x64 | 各 463 断言，读方 21/21；各有 owner+8 个实际 worker 见证 | 同一覆盖跑两位宽，不是 926 种情况；回包为 CPU 合成 |
| 实际 x86→x64 管道故障 | 5/5；读方 30/30 | 对端为 CPU adversary，不是 GPU 故障注入 |
| 等待成本读方 | 11/11；旧/新两个实际 Connection 臂都完成 | 只有一个 CPU AB，不是重复 ABBA 或 COM/IPC/GPU 基准 |

CPU 反例包括 signal-before-wait、合并/过期 hint、跨 Connection 隔离、不可继承句柄、
dequeue 释放槽但尚未 ACK、空队列 close/fault 唤醒、无效 event fail-closed、精确 done+fault
握手和 done-first。测试断言计数改为 atomic，避免测试自身 owner/worker 竞态。

IPC 五例为 Clear 返回 S_FALSE、错误载荷、错误 nonce、对端退出、只发一个字节的 reply header。
Readback 的 32 字节活栈/输出 guard 均未变化，后续准入被拒。前四例 GetTickCount64 报 0ms，
半包 5016ms；0ms 仅表示该时钟分辨率下未跨 tick，绝不表示零成本。
secondary drain 为 0/0/16/16/16ms，仅作故障清场，不代替 primary 正常退出证明。

回执身份：

- CPU：`x1b-notification-cpu-20261023-r1/receipt.json`，
  `E7B5DA11721C169A483E9FDA7AE74564200D720390561119F526BF9BBE541243`。
- IPC：`x1b-notification-fault-ipc-20261023-r1/receipt.json`，
  `DF4E539261EC29583318AF592426349CAF0444FAA5D6536B540313DA7721335A`。

## 3. 实际等待成本，不是游戏倍速

同一实际 Connection/queue/call/acceptReply，96 轮、2244 命令、352 个结果 guard，
旧/新原输入及可逆观测变换分别冻结；原 45 秒外层上限不变。

| CPU-only 测量 | 旧 Sleep | 新 events |
|---|---:|---:|
| 整体 wall | 38550.9463 ms | 16.7088 ms |
| call p50 | 15.5842 ms | 0.0080 ms |
| call p95 | 31.2393 ms | 0.0112 ms |
| worker 空队列等待 p50 | 15.4926 ms | 0.0049 ms |
| owner reply 等待 p50 | 15.5005 ms | 0.0005 ms |

两个线程等待会重叠，不能相加成独占节省量。这里没有真实 IPC、COM 或 GPU；没有据此给出
帧率倍数、游戏内存节省或 GPU 超时的完整归因。它证明轮询在此本机受控负载中有实质等待成本。
QPC 包围跨线程事件的下界可能为负；上下界与线程 CPU 时间保留，不截零伪装精度。

回执 `x1b-wait-cost-20261023-r1/receipt.json`：
`83FEBE7E3D59191AD00C0522C36F330307CC140E7460F4E2EFF8624AFD9BDF73`。

## 4. 本轮先暴露并修正的测试缺陷

初次 notification GPU run `x1b-notification-gpu-20261023-r1` 保留 **FAIL**：
cap5 真实绘制、38 条回执与 62ms 正常退出成功，但读方错误要求只发生一个背压事务。
新调度下 Clear7 重试，随后 Readback8 也可能 Full；同一事务可多次 Full。旧采集只记录前两条，
缺少 Full #3/#4 身份，不能补造日志或把这轮追认为通过。
失败 receipt：`5FFA4F3C02CE8A3E6C46BAD4099AE1C491C15CE068E6F029EB2AA4771CD17A71`。

新门禁在独立目录 `AutoTest/artifacts/x1b-notification-gpu-contract-20261023/`：

1. 固定 128 行，记录每个 Full；溢出即失败。最初两条仍严格是被控制暂停的 Clear7，
   不消耗序号、不变 packet。后续只允许 Clear7 或 Readback8 的实际不可变重试。
2. Readback8 记录真实发布、取出、完整收到回包，以及 `acceptReply` 成功后的 done8。
   “收到 reply”不冒充验证/ACK 完成；独立构造 D9C2 hash。
3. 实际 accountWait 导出每事务 elapsed，严格核对 Full 总数、不同阻塞事务数、sum/max。
   两个阻塞事务不是把 `==1` 随意放成 `>=1`。
4. 独审发现并封堵五项纯读方误通过：Full 之前发布、worker FIFO 乱序、未验证的原场景
   第七条 Clear、Readback 前缀跳跃、Full 非零 result。七条 Clear 完整 tuple、同线程
   前缀单调、实际顺序全部约束。源推导 scene Clear 为 seq22/flags3，draw 模式为 seq15。
5. 不强制跨线程 published 日志早于 dequeued 日志；真正队列 release 早于观测 hook。

最终纯测 **56/56** 主线程独立复跑；独审实际复算五反例均拒、合法额外 Full 均通过，
Connection 12 项插入及 parent 8 项变换可逆还原。正式 queue/worker/ACK/负载没有改变。
reader SHA `FA284BBB315D033B8D055F54235A50B2D62C0E3DEFEC090C6934A798B1350AE6`；
test SHA `B3CCB2FB236B3E1E608EDE294CB4BAAEDD5C7651D2A713845565C6DB79A59F0A`。

## 5. 新六例 GPU 组合，两轮实际通过

同一正式源、门禁、VVL365 和 x64 host，各自 fresh 构建 x86 proxy/fixtures 后执行两轮。
不是同一 DLL 二进制重放；r2 proxy SHA `70A368422FCDE5572A695C446BDCA0D7E73ED2202402F3AD540C5617616D8E93`，
r3 为 `57199DDCA2EF05CC0A99A056AADA6570F5D45DED30D6F4878D2752CE3879EF3F`。
x64 host 固定 `9BCFF0F730A468DCE23F83626A2D2BEFBB4A21A8992F8FB46A0F6EC06BEEF715`。

| case | r2 / r3 | exchanges r2 / r3 | GPU submitted=completed=retired |
|---|---|---:|---:|
| cap5 | PASS / PASS | 38 / 38 | 21 |
| draw 自身回包屏障 | PASS / PASS | 31 / 31 | 14 |
| normal | PASS / PASS | 32 / 31 | 14 |
| invalid 拒绝后恢复 | PASS / PASS | 37 / 37 | 15 |
| COM 96 轮 | PASS / PASS | 2244 / 2244 | 673 |
| raw 资源/代际反例 | PASS / PASS | 795 / 795 | 162 |

normal 的一次额外 Query poll 是原有允许行为，按实际计数核对，不硬凑相同 exchanges。
每轮 COM 都有 96 draw/readback、319488 个核对像素零错、576 创建/释放的公开 COM 引用、
256 次 Query；host 最终 live resources/logical bytes 为零，CPU ACK 不替代 GPU 退休。
cap5 两轮均实测 4 Full、2 个阻塞事务、5 项在途，完整 4096 像素符合第六颜色；
两事务 elapsed 均报 0ms，只是时钟粒度，不是无等待。

COM 从 begin 到 closed 分别 **3719ms / 1844ms**，原 45 秒外层门没有放宽。
六例原 5 秒 primary exit 最大分别 78ms / 63ms。相对旧 45 秒超时，这两轮在原限制内完成；
环境/缓存/初始化耗时未做完整 ABBA，不把两轮差异当优化增益，亦不追认旧超时原因已全面排除。
原 workload 数量、raw 反例、像素判据、容量和期限均保留。

两轮各 183 个冻结输入保持一致，各六例实际 Vulkan validation 的 warning/error 为零，
精确 owned 子进程正常结算，无 watchdog 强杀，Job 空、Desktop 关闭、输入仍 Default。
这里是离屏 D3D9 GPU 子集，不是完整 Presenter/C2、compositor/scanout 或游戏视觉通过。

- r2 receipt：`F440A2D79AF53182CC6831CD542CE15A9FDA8CEE474FD3C1920D317ADA4ECE7C`。
- r3 receipt：`1888890882CE4726B829AB6C2FD9D2298713FBF66058B3FA1B7CB9F719756C80`。
- 文件分别在 `AutoTest/artifacts/x1b-notification-gpu-20261023-r{2,3}/receipt.json`。

## 6. 剩余门与下一步

1. 下一独立边界是 host-owned HWND：父 resize/close、owner WM_QUIT 后保持窗口线程，
   仅在真实 Presenter/surface 退休后销毁。CPU fixture 即使通过也不能冒充 GPU 寿命。
2. 正式产品异步 owner、可取消的非阻塞退出仍未完成；Draw/资源/query 等仍同步 RPC。
3. 此六例 GPU 组合脚本仍为冻结 artifact 试验入口；后续须整理为维护入口及可复现依赖，
   不能以 artifact 通过代替完整项目统一回归。
4. D3D9 FF/stateblock/Lock/Reset/完整资源与 WarVK 全 pass 覆盖、游戏同进程跨图、
   x86 VA/private/commit 与 x64/总成本/玩家视觉 A/B 均未验收。

玩家 `E:/Work/Warcraft III/d3d9.dll` 仍为
`1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`，没有替换。
没有 commit、push、release、系统配置改动或共享核心修改。
