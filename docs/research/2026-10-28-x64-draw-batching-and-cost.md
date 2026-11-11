# 2026-10-28：x64 实际 draw 批次、通知等待与成本

本轮目标是让实际渲染工作由 x64 原 DXSO/DXVK 执行，并减少跨进程迁移成本。
玩家目录未部署，Game.dll 保护仍在；本报告的 GPU 客户端是独立受限 COM 场景，
不能据此宣布 Warcraft 整幅画面已外置或玩家 FPS 已提升。

收口状态：32-draw 批次、原通知等待、四组在途常量 backing、异步批次和一次性 EVENT
通知均已实现并取得独立 CPU / IPC / 受限真实 GPU 回执；最新 FE6ECB 身份见第五阶段。
图像与生命周期门通过，完整游戏 API 覆盖和性能接受门仍开放；后者既有明显绝对开销，
也缺独占环境与父/宿主 CPU 时间的完整测量。各阶段的时间不能混算成总收益。

## 决策与数据所有者

- x86 COM 调用者拥有按值 draw packet。每次提交时冻结全部资源 ID/代际、viewport、
  render state、VS/PS 常量和 index 参数；不传 x86 指针。固定 staging 容量为 32 draw，
  37,248 bytes，不增加现有四槽队列的容量。
- 资源创建、上传、销毁、Query、Readback、Present 与 Finish 是有序边界；先 flush
  之前的 draw，资源远端销毁确认后才允许 ID 复用。EndScene 发送不足 32 的尾批。
  后来的拒绝或传输歧义会终止该会话，不丢 draw 后继续，也不跨会话重放。
- x64 host 首先核验整批所有六元资源引用，再由原 backend 对所有 draw 完成完整
  preflight。任一项失败都不能留下已记录的前缀；合法批次才分配/写常量及记录命令。
- 每个 draw 的 VS/PS 数据使用独立、按实际设备 uniform alignment 对齐的切片，
  原 shader-defined constant 覆盖规则保持。原 DXVK 拥有资源、管线、提交与退休。
- `D9C2/D9R2` 的既有字段布局不变，新增 `DrawBatch` opcode；旧读方拒绝新操作。
  `WARVK_D3D9_BATCH_DRAWS=1` 是此实验代理的显式开发模式，默认仍单 draw，非产品开关。

## 已完成的第一、二阶段验证

1. `draw-batch-cpu-20261028-r1/receipt.json`：x86/x64 实际 Connection CPU 测试。
   覆盖 32+3 按值复制、读回屏障、返回值归属、迟到拒绝、关闭后不得重放；不是 GPU 测试。
2. `x64-draw-batch-build-20261028-r1/receipt.json`、
   `x64-draw-batch-gpu-20261028-r5/receipt.json`：第一版批处理真实 GPU 门通过。
3. `x64-notify-batch-build-20261028-r1/receipt.json`、
   `x64-notify-batch-gpu-20261028-r1/receipt.json`：改为原队列通知后重新构建及实际 GPU 门通过。
   两种位宽的生产 `condition_variable` 各 1,014 检查通过。
4. `x64-notify-batch-vvl-20261028-r1/receipt.json`：同候选三个 VVL365 场景通过。
   使用固定 Khronos 1.4.365、独立 LOG_MSG 回调和私有空 implicit-layer 来源。
   Core/Synchronization/Stateless/Object lifetime/Thread Safety/Handle Wrapping 激活，
   非状态 warning/error 为 0。未认证所有设置运行时生效，不把 VVL 时间当性能数据。

所有路径均相对于 `AutoTest/artifacts/`。各 receipt 自带执行文件/源码身份、原始输出、
restricted-Medium 非输入 Desktop/原子 Job、实际 PID+creation、正常退出和保全核对。
没有全局输入、桌面切换、系统设置修改或玩家 DLL 覆盖。

GPU 场景每次 44 帧（4 warm + 40 measured），每帧 97 draw，每个 draw 改 VS/PS 常量。
每次 12,672 个独立采样像素检查；本地与远端采样 hash 相同。原 depth/blend 场景仍通过。
29 组真实协议/backend 负例涵盖错误位置 0/15/31、资源代际/类型、几何范围、NaN/Inf、
viewport 和包长度；每次检查拒绝前后整幅 64×64 图不变，再验证合法绘制可恢复。

| 版本/模式 | 97 draw 完成中位数 | p95 | 口径 |
| --- | ---: | ---: | --- |
| 初始逐 draw，同步 IPC + 固定轮询 | 169.22–171.01 ms | 174.79–175.67 ms | 第一版 ABBA |
| 第一版 32 draw 批次 | 8.63–8.71 ms | 10.34–11.36 ms | 第一版 ABBA |
| 原队列通知，仍逐 draw | 10.33–10.99 ms | 12.29–13.28 ms | 第二版 ABBA |
| 原队列通知 + 批次 | 0.755–0.877 ms | 0.938–1.186 ms | 第二版 ABBA |
| 同轮本地参考 | 0.127–0.174 ms | 0.192–0.391 ms | 第二版前后夹测 |

计时从 Clear 到最后 Event Query 的 GPU 完成；Readback 和像素核对在计时窗之外。
这是低分辨率、有限资源、隔离桌面的提交与完成微基准，**不能换算玩家 FPS**。
第一版前后参考 warmup 差别较大，不拼接为精确发行版开销百分比。
第二版仍有约 0.6–0.7 ms 的绝对额外成本；尚不能据此认证完整游戏迁移得益。

## 生产共用等待边界

宿主曾每提交/退休固定 `Sleep(1)`。现在等待原 `DxvkSubmissionQueue` 的提交和 finish
通知，保留原五秒边界。顺带修复 Win32 SRW condition variable 的三处真实问题：
`wait_until` 剩余时长符号反向、谓词等待在虚假唤醒后提前返回、毫秒向下截断/无界转换。
谓词始终是事实来源；通知不认证 GPU 完成。timeout 只结束观察，不撤销提交、不授权
复用 status 或释放不确定的 Owner。共享修改必须另跑 x86 产品全量配置回归和真实游戏入口。

## 第三、四阶段：四批在途、异步批次与实际退休

开发模式 `WARVK_D3D9_GPU_FLIGHTS=4` 已完成下述限定验证。最多四批使用各自常量 backing；
只有该槽位 CPU submit result、原 GPU timeline 和原 DXVK resource-use retirement
全部满足才复用。上传、读回、销毁仍等待全部原有持有者退休。
GPU 先完成而 CPU 结果尚未发布时，只观察完成值，不能回收 status 存储。

`WARVK_D3D9_ASYNC_BATCH=1` 必须同时启用批处理，默认关闭。它复用原四槽 SPSC 队列，
在整批按值复制后返回；Full 仍在原五秒准入期限内背压。独立 worker 按序收取每批回复，
错误或非预期载荷使会话终止。资源更新、销毁、Query、Readback 等仍等待对应有序回复，
不把本地入队当成远端成功或 GPU 完成。没有增加 IPC 槽位，没有跨会话重放。

冷诊断有本地未提交 draw 时返回 INVALIDCALL，保持序号和目标内存不变；调用者显式
EndScene 后可观察有序前缀。诊断本身不再隐式触发 draw 提交；实际代理导出也已测试。

本批原始回执均在 `AutoTest/artifacts/`：

| 回执 | 结果与范围 |
| --- | --- |
| `x64-flight-build-20261028-r3/receipt.json` | x64 原核心构建、no-work、源码/保护文件保全通过 |
| `x64-flight-gpu-20261028-r1/receipt.json` | 四批在途首轮，97 draw，13 场景通过 |
| `x64-flight-stress-20261028-r1/receipt.json` | 四批在途首轮，1,025 draw，13 场景通过 |
| `draw-batch-async-cpu-20261028-r2/receipt.json` | 实际 Connection/SPSC，x86/x64 各 190,493 检查；纯 CPU，非 GPU |
| `x64-async-flight-gpu-20261028-r1/receipt.json` | 异步组合，97 draw，16 场景通过 |
| `x64-async-flight-stress-20261028-r1/receipt.json` | 异步组合，1,025 draw，16 场景通过 |
| `x64-async-flight-vvl-20261028-r1/receipt.json` | 1,025 draw 异步组合、legacy、拒绝/恢复三个 VVL365 场景通过；六类激活，独立回调 warning/error=0 |
| `draw-batch-async-fault-ipc-20261028-r1/receipt.json` | 真实 x86 Connection→x64 对抗进程，S_FALSE、额外载荷、错误 nonce、peer exit、partial header 五场景通过；无 GPU |

每次真实绘制补查 193 draw / 576 像素：129 draw 的 128 个独立可见区域让早期批次在
四槽绕回后仍可观察；另 64 draw 中途上传纹理，前半保留白纹理、后半使用灰纹理。
每个 raw 场景原 29 组整批拒绝之外，再将五个批次入队后立即销毁全部六类资源并复用 ID，
逐类拒绝旧代际，验证旧画面未变及新资源正常绘制。每个 raw 场景共 35 拒绝，
118,784 个整图不变像素检查及 119,808 个恢复/复用像素检查。

1,025 draw 异步组合的每次实际 x64 工作为 **45,293 draw / 1,459 draw 提交**，
1,277 次槽位复用、1,459 次槽位退休，最大占用四槽；本地和各远端模式的采样像素哈希
一致。四份常量 backing 在本 GPU 合计 **1 MiB**，均由原 DXVK 退休条件保护。
`constantCapacityBytes` 是该 Owner 曾分配的容量记录，结束后不清零，不能读成退出后仍驻留。
`maxOccupiedFlightSlots` 是状态槽占用，不能解释为四批 GPU 指令确实同时执行。

CPU 还覆盖无人消费时四整批可按值入队、原始 packet 改写不污染排队数据、入队成功
不代表 completed/retired，以及迟到 INVALIDCALL/DEVICELOST/异常载荷/S_FALSE 不得丢失。
读方测试为批次 5、CPU 回执 21、真实管道回执 31；这些纯测试不增加 GPU 覆盖数量。

### 本批成本结论：明显收敛，仍不满足完整游戏性能验收

在最新 1,025-draw 同轮夹测（40 个测量帧，保留全部样本）中：

| 模式 | 完成中位数范围 | p95 范围 |
| --- | ---: | ---: |
| 原逐 draw 同步请求 | 886–1,141 ms | 1,425–2,028 ms |
| 32 draw 批次，逐批等待 | 42.65–44.36 ms | 61.09–68.73 ms |
| 批次＋四批在途 | 33.92–36.37 ms | 45.53–62.98 ms |
| 异步批次＋四批在途 | 24.64–26.64 ms | 36.15–37.55 ms |
| 同轮本地参考 | 2.06–3.31 ms | 12.81–15.73 ms |

异步组合比同轮逐批等待的中位数减少约 38–44%，但远端绝对额外成本仍明显过高。
本地参考自身也有很大的尾延迟，早先 97-draw 通知候选约 0.76–0.88 ms 的结果不能
与本轮高波动时间直接拼接。**不认证稳定百分比、玩家 FPS 或“迁移不会得不偿失”。**
VVL 场景不加入性能表。97 draw 新组合也已验证，独立原始样本保留在上述 r1 回执。

计数还暴露了下一成本入口：97 draw 的新异步场景 44 次 EVENT Issue 对应 2,048 /
2,738 次实际 Poll IPC，受限客户端紧密轮询会产生额外往返；这只是实测关联，未完成
各层耗时归因。下一步先缩减重复完成查询/帧级往返，再用真实调用分布与稳定负载复测，
不能通过不等待 GPU 或把诊断时间剔除后冒称完整帧加速。

冷检查中的 x86 独立客户端 private 约 2.64–2.65 MiB，x64 宿主约 176–177 MiB，
Connection inline storage 431,168 B。它证明这个小场景的工作/资源宿主位置，**不是
魔兽游戏节省了 176 MiB**，也不是离散显卡 VRAM 驻留测量。

### 构建和产物身份

- x64：`build64_render_host_flights_20261028_r3/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe`，
  5,554,176 B，`64D069FB1E0B7FC410D70EC3C982CAD49C0386C918000380C0BD0FB2F4F14CC3`。
- 97 draw 客户端：`x64-async-flight-gpu-20261028-r1/proxy.dll`，987,648 B，
  `271D751677A825DFB2095DAD612012F89E7D738DD17305D59B08C5A9BAA32DFA`。
- 1,025 draw 客户端：`x64-async-flight-stress-20261028-r1/proxy.dll`，987,648 B，
  `B6B164FC1CA7A5BC28724F202CB2E81A6DCB078FC3B6A2973053AD3313519938`。
- VVL 客户端：`x64-async-flight-vvl-20261028-r1/proxy.dll`，987,648 B，
  `FA045186A9552B9136180CF3EB20A996DDE8A850D9805795F7B930E8767732FC`。

各门单独编译/记录客户端身份，不把其中一份 DLL 说成接受了其它份的全部门。
后续 Connection 头仅用于 x86 客户端；runner 对此唯一差异做显式记录，核验 x64 的
实际 Ninja 依赖中不含该头、构建系统身份未变且目标 no-work，才复用不可变宿主。
其余源码漂移一律拒绝。原始构建回执未重写，新的 x86 编译输入与产物另行冻结。

## 共用修改的 x86 回归

独立 `build32_device_owner_notify_20261028/src/d3d9/d3d9.dll` 为 24,473,600 bytes，
SHA-256 `F6589D0F5BC9EA7440764104ACAB3D9B1279FA53D4CE8842DE52D07D468F9C4F`。
`x86-notify-build-20261028-r1` 构建/no-work/保全通过；
`x86-notify-regressions-20261028-r1` 在 restricted-Medium Desktop 完成 127/127 配置测试。

实际 Lost Temple 对照使用相同 35 秒总时限：新候选 r1 未到 DLL/ready；旧 AA4C 参考
`x86-notify-native-control-20261028-r1` 载图、截图、退图、自然退出通过；新候选 r2
已 ready/截图/原 EndGame/退图，但退出观察超过总时限；新候选
`x86-notify-native-game-20261028-r3` 完整链及自然 exit 0 通过。前两次不能由 r3 追认，
启动耗时波动原因未证实，单轮通过也不认证稳定冷启动或长期游戏性能。

视觉审查：仅查看 `x86-notify-native-game-20261028-r2/game-frame.bmp`，1902×963，
F6589D 候选；实际 Lost Temple 地形、红顶大厅、工人、金矿、树木、UI 与阴影可见，
不是空白画面。未重复开图；该图不证明全部视觉等价，也不是 x64 游戏画面。
可选 startup-window 诊断在 r2 因 `GetThreadDesktop: Win32=5` 拒绝，未取得该诊断图，
没有切换桌面或使用全局输入绕过；正常 GPU 截图与此独立。

## 第五阶段：一次性 EVENT 完成通知（新身份独立验证）

新增 `WaitEvent=19`，沿用既有 header/reply 布局；旧 peer 拒绝新 opcode。
`WARVK_D3D9_ASYNC_EVENTS=1` 显式要求异步批次，默认关闭。
Query 的资源 ID/代际、成功 Issue revision 与 GPU serial 仍由同一验证入口决定。
Connection 只允许一个未结算的 WaitEvent 在原四槽队列中；GetData 返回 S_FALSE 时
不继续制造 Poll IPC。宿主用原 Vulkan timeline 等待已签发的 serial，成功回复后 worker
才 release 发布 nonce 内的单调 GPU 水位，x86 acquire 读取它。没有异步 Query 裸指针，
没有复制一套资源释放机制；CPU ack、GPU 完成和资源 retirement 继续分开。

同一 Query 新 revision 可能复用相同 GPU serial；不同 Query 可以观察同一个 GPU 前缀，
但必须各自先完成真实 Issue。新 Connection 水位从零开始；终态故障优先于缓存完成。
Destroy / 下一次 Issue / Readback 等有序命令仍排在待处理的完成通知之后。
等待只依赖已提交前缀，不依赖队列中未来命令；原 GPU/IPC deadline 不变。

新 host：`build64_render_host_events_20261028_r1/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe`，
5,558,272 B，SHA-256 `FE6ECBD1F340789A34402BF81EBB992FEAA32F71DFE94EE409A2D560C89857D6`。
`x64-event-build-20261028-r1/receipt.json` 的独立构建/no-work/源和受保护文件保全通过。
`x64-event-async-cpu-20261028-r2/receipt.json`：实际 x86/x64 各 **210,589** 检查通过，
包含 20,000 次未完成重复查询只产生一条请求、不同 Query 等待、更晚 serial、重复 Issue、
故障不能发布水位、陈旧会话不得复用；实际 wire x86/x64 各 **1,319** 检查通过。
之前 r1 的 210,589 检查也通过，r2 增加相同隔离 owner 下的 wire 门。该 CPU 回执不证明 GPU。

新身份回执均在 `AutoTest/artifacts/`，不继承前面候选：

| 回执 | 结果 |
|---|---|
| `x64-event-gpu-20261028-r2/receipt.json` | 97 draw，8 场景通过，参考/远端采样像素与跨批覆盖哈希一致 |
| `x64-event-stress-20261028-r1/receipt.json` | 1,025 draw，8 场景通过，同样通过独立像素判据 |
| `x64-event-vvl-20261028-r1/receipt.json` | 三场景通过，六类验证实际激活，独立原始回调 warning/error=0 |
| `x64-event-fault-ipc-20261028-r1/receipt.json` | 五种真实管道故障通过；pending EVENT 不能被伪成功/错身份/断管升级为 GPU 完成 |
| `x64-event-async-cpu-20261028-r2/receipt.json` | 两种位宽 210,589 + 1,319；原始输出、输入身份与隔离清场通过 |

纯 reader **59/59** 通过。GPU raw 场景新增 **12** 个错误 EVENT 等待与 **3** 个后续合法恢复，
覆盖未 Issue、旧 revision、零/错 serial、类型、代际、保留参数、载荷、销毁与 ID 换代。
COM 在每个计时场景外再完成 **5** 项 Query 转换；图像仍覆盖 44 帧、每帧 288 采样点及
193-draw / 576 采样点的常量和纹理寿命判据。压力 E1 的真实宿主为 **45,293 draw / 1,459 批**，
1,277 次槽复用、1,459 次原规则退休，submitted=GPU completed=retired=1,563。

97 draw 的两个对照累计 Poll IPC 为 6,261 / 5,723，新模式均为 **0 Poll / 46 WaitEvent**；
49 次 Issue 中有重复 serial，故不应要求每次 Issue 都新增等待。Query 在 x86 本地的 pending
观察为 446,667 / 518,967 次，这是紧密调用 GetData 的测试器产生的真实循环；**没有测得
进程 CPU 时间，不将少 IPC 直接宣称为主线程 CPU 降低**。

| 同一新候选、同一轮 | 完成中位数 | p95 |
|---|---:|---:|
| 97 draw：异步批次＋四批在途，旧 Poll | 4.995–5.647 ms | 31.137–46.052 ms |
| 97 draw：再加一次性完成通知 | 2.465–4.164 ms | 13.797–16.082 ms |
| 97 draw：本地参考 | 0.419–0.807 ms | 10.868–12.234 ms |
| 1,025 draw：旧 Poll | 43.390–56.868 ms | 91.370–253.142 ms |
| 1,025 draw：一次性完成通知 | 32.375–40.696 ms | 65.846–86.013 ms |
| 1,025 draw：本地参考 | 9.428–10.046 ms | 56.577–56.867 ms |

以上为 40 个计时样本的中位数/p95，包含真正 EVENT 完成，readback oracle 位于窗口外。
新模式高 draw E1 的 p99=158.287 ms，旧 D1 为 129.349 ms；不能宣称尾延迟全面改善。
三个阶段的数字来自不同时间窗口，禁止拼成一个倍率。GPU 正确性门已过，**性能接受门仍未过**。

VVL 后只读进程计数快照看到 `fellowship-Win64-Shipping` PID 3164 的
`PercentProcessorTime=528`（以一个逻辑核为 100%）。这是其他应用负载的直接证据，
不是覆盖全部早先计时区间的独占监测，也没有测其 GPU 竞争；不以此单独认证抖动根因。
没有停止/改优先级/注入该进程。原值与限制记录在
`AutoTest/artifacts/x64-event-environment-20261028-r1.json`。

本阶段三个独立 GPU gate 的 x86 `proxy.dll` 身份分别为：

| gate | SHA-256 |
|---|---|
| 97 draw r2 | `A1C873739195618F200A85657CCBF3677D44ABB64F74DC9E3AFB7B250E8164FB` |
| 1,025 draw r1 | `9DFD1DE3AB8D56E0FB2FCA3E1C32FD228EE5A93CB96FE260985B0FDEF15933C6` |
| VVL r1 | `3CD63DAE1EBC8FE90E2F25CE270FC105029D433051CA69EDFCFF26DFAE4CBFC4` |

均绑定同一 FE6ECB x64 宿主，但属于各自新编译的代理，不能把三门归到其中任意单一 DLL。
不是发布候选，玩家 DLL 仍为 `1FFE0BD1…35CF`。宿主配置/构建/no-work 分别为
84.13 / 534.45 / 0.30 秒，源清单与受保护产物均保持。

## 本轮失败记录

- 首次 gate 缺 `station` 参数，第二次缺 child witness：测试器接线错误，GPU 尚未执行。
- 第三次 raw 负例后的恢复场景用了错误 ClearColor：不变前缀检查通过，独立恢复颜色判据失败。
- 第四次既有 legacy fixture 未传 `--present`，严格读方拒绝缺失的明确 unsupported 证据。
- 第一版最终 r5 完整通过。前四份失败 receipt 保留，不由后来的通过追认。
- 四批在途 fresh 构建 r1/r2 在 Meson 配置探测耗尽 180 秒；r3 只允许构建工具配置
  watchdog=600 秒，实际配置 233.51 秒、编译 668.84 秒/no-work 通过。游戏、IPC、
  GPU、退出的原期限不变；无依据把配置变慢归因于渲染器或图片上下文。
- CPU 读方仍写死旧 worker 集合、fault 读方仍按旧四行 stderr/第一处 `#else` 拆源码，
  新增 batch 后纯测试暴露不一致；现按完整新集合、五行精确诊断及准确父进程分界收紧。
  未删负例或忽略额外行，修后 21/31 测试通过。
- 第五阶段 `x64-event-gpu-20261028-r1` 的 raw 拒绝与 legacy 场景通过，但本地参考的
  新 Query 复用用例超时：测试等待循环用 flags=0，没有要求本地 DXVK 提交 Clear。
  修为 D3DGETDATA_FLUSH 后重新验收；未延长 5 秒 Query deadline。旧失败回执保留。
- 新增纯 reader 用例首次写错 helper 名 `stderr`，修为既有 `stderr_for`；随后 59/59 通过。
  最初从仓库根以模块模式运行时该脚本的 sibling import 失败，改在 AutoTest 工作目录
  运行正式脚本入口；这两次测试器错误不是产品通过，也不作成功回执。

## 未覆盖主线

实际游戏仍大量使用 FVF/Transform/Material/Light/TextureStageState、StateBlock、
DrawPrimitive、StretchRect 和多 mip 纹理。当前小型 SM3 profile 没有这些完整语义，
也未迁移 WarVK 阴影、后处理、UI 全部 pass。下一功能批按原 Factory 能力 → FF 完整
状态/原 shader → 纹理多子资源/RT/DS → 同游戏场景等价与成本推进，禁止以缺画面降低开销。
