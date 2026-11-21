# x64 迁移阶段 1 回执（一）：共享内存命令环、宿主故障语义、场景标记与会话重置

2026-11-13，Claude（Fable 5.2）。对应[整体路线](../plan/2026-11-13-x64-migration-roadmap.md)阶段 1 的第 1、4、5 项，
设计见[传输层设计](../plan/2026-11-13-x64-phase1-transport-design.md)。第 2、3 项（内容载荷协议、有界上传窗口）
写本回执时还没做（§8），已于 11-15 完成，见[回执（二）](2026-11-15-x64-phase1-content-window.md)。全部改动未提交；没有碰玩家目录（`E:/Work/Warcraft III/d3d9.dll` 仍不存在，门禁记为“不存在且必须保持不存在”）。

## 1. 结论

- D9C2 消息现在可以走**共享内存命令环**（`WARVK_D3D9_COMMAND_RING=1`，默认关）。握手仍在管道上，`AttachRing`
  回复之后双方切到环；不开时管道路径不变。
- 环模式下像素与管道模式、与进程内参照**逐个相同**（16 个用例同一哈希 `03f7265ab853783c`），VVL 0 错误 0 警告。
- 宿主故障有了明确的六类终止原因和一个策略挂钩；真实宿主上 11 种会话（1 个健康 + 7 种坏记录 + 异步拒绝 +
  杀宿主 + 挂起宿主）全部按预期终止，x86 侧每种都在期限内 fail-closed 并报出正确类别。
- 环给 x86 增加的地址空间**恰好两个映射**：`0x401000`（4 MiB 环 + 控制页）和 `0x11000`（64 KiB 回复槽 + 控制页），合计 4.07 MiB。
- 场景标记 `WorldSceneEnter/WorldSceneExit` 与会话重置消息 `SessionReset` 已在协议、宿主、代理导出和测试里落地（阶段 0 回执 §4 的输入）。
- 同一轮内的准入测量（§5）：1,025 draw 固定管线帧的完成中位，管道 5.98 ms、环 4.09 ms（−32%）；环上不再出现队列满。
  与进程内参照（1.23 ms）的差距还在，但已经不在传输上。

## 2. 改动

| 文件 | 内容 |
|---|---|
| `tools/render_host/d3d9_command_ring.h`（新） | 可移植的环核心：生产者区/消费者区布局、记录格式、`Producer`、`Consumer`、故障码。两种位宽布局相同 |
| `tools/render_host/d3d9_ring_channel.h`（新） | Windows 对象：按连接 nonce 命名的两个映射和两个事件，`OwnerChannel`（x86）与 `PeerChannel`（宿主），各自只读映射对方的区 |
| `tools/render_host/d3d9_wire.h` | op `AttachRing`、`SessionReset`；`EncodeHeader`；`ValidSessionReset` |
| `tools/render_host/d3d9_annotation_wire.h` | `Stage::WorldSceneEnter/WorldSceneExit`；`SceneTracker`；记录器折入会话重置 |
| `tools/render_host/d3d9_connection.h` | 环模式的准入、同步调用、背压、看护线程；`TerminalReport` 与 `setTerminalHook`；`requestSessionReset` |
| `tools/render_host/dxvk_core/d3d9_host_main.cpp` | 主循环在挂接后从环读；`AttachRing` 握手；故障发布后自停；场景标记配对与会话重置 |
| `tools/render_host/d3d9_proxy.cpp`、`.def` | 导出 `WarVKD3D9RequestSessionReset@16` |
| `tools/render_host/win32_security.{h,cpp}` | `SecurityObject::Event` |
| `AutoTest/test_render_host_command_ring.cpp`（新） | 环单元测试 |
| `AutoTest/test_render_host_ring_faults.cpp`（新） | 真实 x86 `Connection` 对真实 x64 宿主的故障注入夹具 |
| `AutoTest/test_d3d9_connection_async_cpu.cpp` | 环模式 10 个场景（进程内假宿主）、会话重置 |
| `AutoTest/test_render_host_{d3d9_wire.cpp,annotation.h,fixed_rejection.cpp,d3d9_factory_clear.cpp}` | 新 op、新阶段、`SceneTracker`、会话重置的正反例 |
| `AutoTest/run_d3d9_factory_clear_gate.py` | `--command-ring`：环单元测试、每个管道臂旁一个环臂、raw 故障的环臂、真实宿主环故障表 |
| `AutoTest/run_d3d9_draw_batch_gate.py` | `--command-ring`：同一轮内 管道 F1/F2 包住 环 R1/R2；玩家 DLL 不存在时按“不存在”冻结 |
| `AutoTest/run_d3d9_connection_async_cpu.py` | 新增 worker 用例名；链接传输与安全辅助 |

## 3. 门禁结果

| 工件 | 内容 | 结果 |
|---|---|---|
| `x64-phase1-connection-cpu-20261113-r8` | 连接层 CPU 门（x86 与 x64 各一遍）：原有用例 + 会话重置 + 环 10 场景；线格式测试 | 通过。连接层每种位宽 214,165 项检查，线格式每种位宽 1,455,573 项 |
| `x64-phase1-ring-host-build-20261113-r3` | x64 宿主（含环与场景/会话事件） | 通过。`warvk-d3d9-host-dev.exe` `54A4BAE5…96E0`，构建目录 `build64_render_host_phase1_ring_20261113_c` |
| `x64-phase1-ring-gpu-vvl-20261113-r5` | GPU 参照门，`--validation --repin-loader --stream --fixed-batch --annotations --command-ring` | 通过。16 个用例同一像素哈希；VVL 0 错误 0 警告；7 个 raw 故障用例；环单元测试每种位宽 267,078 项；11 个真实宿主环故障会话 |
| `x64-phase1-ring-perf-1025-20261113-r2` | 1,025 draw 固定管线帧，管道对环，同一轮 | 通过（像素与覆盖哈希一致）。数字见 §5 |

GPU 门里的环臂（每个都与紧邻的管道臂用同一夹具、同一配置，只多一个环境变量）：

| 用例 | 环记录数 | 其中同步 | 字节 | 像素哈希 | VVL |
|---|---|---|---|---|---|
| `remote-sync-cmdring` | 1,887 | 1,596 | 2,335,392 | `03f7265ab853783c` | 0 / 0 |
| `remote-grouped-cmdring` | 2,184 | 1,700 | 2,378,160 | 同上 | 0 / 0 |
| `remote-stream-cmdring` | 2,184 | 1,700 | 2,378,160 | 同上 | 0 / 0 |
| `remote-stream-fixed-cmdring` | 2,181 | 1,406 | 2,377,728 | 同上 | 0 / 0 |
| `remote-stream-fixed-annotate-cmdring` | 2,197 | 1,417 | 2,406,480 | 同上 | 0 / 0 |

每个环臂都核对了：x86 发布的记录数 = 宿主读到的记录数，同步记录数两边相等，`已发布 = 已执行 = 挂接序号 + 记录数`，
Finish 已执行，终止原因为 none，命令总数与宿主的 `accepted + rejected` 相等。raw 故障夹具的三个环臂
（普通、stream、annotate）同样通过：环上的同步拒绝仍是可恢复的。

失败的轮次都保留着：

- 连接层 CPU 门：r1（门禁的构建没链接传输与安全辅助）、r2（线格式测试钉着旧的 op 上界）、r3（少登记一个 worker 用例名）、
  r5 与 r6（**测试夹具自己**的两处生命周期错误，见 §7）。r4、r7 通过，之后因后续改动重跑到 r8。
- GPU 门：r1（门禁读单元测试输出的正则）、r2（改了 `d3d9_connection.h` 没重建宿主，门禁按源漂移拒绝）、
  r3（门禁把挂接序号写死成 2，而夹具在 Initialize 之前有 12 次工厂查询）。r4 全过，之后为 §7 的记账修正重建宿主并重跑 r5。
- 准入门：r1（§7 的记账）。

## 4. 故障注入

**进程内假宿主**（`test_d3d9_connection_async_cpu.cpp`，x86 与 x64 各跑一遍）：真实的环模式 `Connection`
（挂接、准入、同步调用、看护线程、终止原因）对一个按宿主方式打开映射的线程；一个事件代替宿主进程句柄。

| 场景 | x86 报告 | 说明 |
|---|---|---|
| 健康 | 无 | 5 条异步 + 同步回复；宿主停止消费时 3,000 条 Clear 填满 256 KiB 环，生产者等待、不丢、不乱序；Finish |
| 宿主在一条未回复的同步调用上退出 | HostExited | 调用在 5 s 内返回 `DEVICELOST` |
| 宿主空闲时退出 | HostExited | 看护线程发现，下一次调用即拒绝 |
| 宿主拒绝一条异步记录 | HostRejected + HRESULT | 它之前的记录已执行，之后的不执行 |
| 宿主回复 `DEVICELOST` | HostDeviceLost | 是已确认的远端终止，不算传输失败 |
| 宿主发布不可能的 `head` | RingCorrupt: head-invalid | x86 在使用之前校验 |
| 宿主发布不可能的 `applied` | RingCorrupt: applied-invalid | 同上 |
| 宿主的回复属于另一条消息 | RingCorrupt: reply-invalid | 同上 |
| x86 内存里一条记录被写坏 | RingCorrupt: record-header | 宿主拒绝、说明原因、不执行其后的记录 |
| 宿主停止消费（环满） | HostHung: no progress | 滚动无进展期限到期；失败的会话对宿主是流结束 |

每个场景都另外核对：挂钩只被调用一次；此后 `call` 与 `enqueueClear` 都返回 `DEVICELOST`；第一原因不被后来的覆盖。

**真实 x64 宿主**（`test_render_host_ring_faults.cpp`，GPU 门 `--command-ring` 里逐个起会话）：

| 用例 | x86 报告 | 宿主自述 | 宿主已执行记录 | 宿主退出码 | 发现用时 |
|---|---|---|---|---|---|
| `healthy` | 无 | — | 2,003（全部） | 0 | — |
| `record-header` | RingCorrupt: record-header | record-header | 1 | 4 | <16 ms |
| `truncated` | RingCorrupt: truncated | truncated | 1 | 4 | <16 ms |
| `tail-invalid` | RingCorrupt: tail-invalid | tail-invalid | 1 | 4 | <16 ms |
| `checksum` | RingCorrupt: checksum | checksum | 1 | 4 | <16 ms |
| `message-header` | RingCorrupt: message-header | message-header | 1 | 4 | <16 ms |
| `scope` | RingCorrupt: scope | scope | 1 | 4 | <16 ms |
| `sequence` | RingCorrupt: sequence | sequence | 1 | 4 | <16 ms |
| `async-refused` | HostRejected: async-refused, `D3DERR_INVALIDCALL` | async-refused | 2 | 4 | <16 ms |
| `host-killed` | HostExited | — | — | 77（夹具 `TerminateProcess`） | 62 ms |
| `host-suspended` | HostHung: no progress | — | — | 4（恢复后读到流结束） | 1,516 ms（期限设为 1,500） |

“已执行记录”是坏记录之前那条同步屏障（`async-refused` 多一条被拒绝的记录本身）；“发现用时”是故障就位后下一次同步调用
返回 `DEVICELOST` 的耗时（`GetTickCount64`，分辨率约 16 ms）。

宿主对每种坏记录都以对应故障码自停（退出码 4），故障行里的 `records` 证明它没有执行坏记录及其后的任何东西。

## 5. 准入测量

`x64-phase1-ring-perf-1025-20261113-r2`：1,025 draw 的固定管线帧（44 帧，前 4 帧预热，取 40 帧），stream + 固定批次 + 异步事件，
同一个宿主、同一个代理 DLL，顺序 参照 → 管道 F1 → 环 R1 → 环 R2 → 管道 F2 → 参照。这台机器同时有别的会话在跑重负载
（系统占用 32–50%），**只在本轮内比较**，不要和 10-29 的数字对比。

| 臂 | 准入 中位 / p95 / p99（ms） | 完成 中位 / p95 / p99（ms） | x86 周期（M） | 宿主周期（M） | 队列满观察 / 背压调用 / 背压 ms |
|---|---|---|---|---|---|
| 进程内参照（前） | 1.18 / 1.67 / 2.11 | 1.28 / 1.87 / 2.24 | 707 | — | — |
| 管道 F1 | 5.88 / 7.53 / 8.22 | 6.11 / 7.74 / 8.42 | 685 | 2,018 | 3,935 / 1,968 / 63 |
| 环 R1 | 4.15 / 6.36 / 8.24 | 4.37 / 6.55 / 8.44 | 511 | 842 | 0 / 0 / 0 |
| 环 R2 | 3.65 / 4.64 / 6.14 | 3.82 / 4.88 / 6.32 | 449 | 713 | 0 / 0 / 0 |
| 管道 F2 | 5.65 / 6.73 / 7.41 | 5.85 / 6.93 / 7.60 | 652 | 852 | 3,951 / 1,976 / 110 |
| 进程内参照（后） | 1.08 / 1.42 / 1.59 | 1.17 / 1.55 / 1.68 | 443 | — | — |

- 完成中位：管道两臂平均 5.98 ms，环两臂平均 4.09 ms，**少 1.89 ms（−32%）**；x86 侧 CPU 周期少约 28%。
  p99 没有明显改善（8.4/6.3 对 8.4/7.6），尾部主要是机器噪声。
- 管道臂每次运行有约 3,940 次“队列满”观察、约 1,970 次背压调用；环臂为 0，最大在途 55 条记录（环从未填满）。
  也就是说 10-29 准入拆解里“75% 的准入时间在等队列空位”这一项已经消失。
- 相对进程内参照（1.23 ms）环臂仍是 3.3 倍，绝对差约 2.9 ms/帧。这部分不是传输：每个固定管线 draw 带 3,180 字节完整状态，
  一帧 52 个批次、3.28 MB 进环（44 帧共 144.5 MB），x86 逐 draw 编码、宿主逐 draw 解码校验。环模式与准入采样互斥，
  这一轮没有把 x86 编码和每帧约 4 次同步等待进一步拆开。要继续缩小差距，题目是**状态去重/增量编码**，不属于传输层。
- 同步调用的等待还有一处已知的多余唤醒：宿主每执行完一条记录、只要生产者在等，就发一次 `progress`，所以生产者等一条同步回复
  平均被唤醒 5–6 次（R1：178 次同步调用、1,018 次睡眠）。生产者此时本来就在等，不在关键路径上；要省这些唤醒，
  可以让 `waiting` 区分“等空间”和“等回复”，宿主只在同步记录执行完时唤醒。没有做，记在这里。
- 按路线“待作者确认”的性能门（1.5×/2×）衡量：这个合成帧目前是 3.3×。它是 1,025 个互不相同状态的固定管线 draw，
  比真实对局的一帧苛刻；真实流量要到阶段 5 才能量。

## 6. x86 地址空间

`ring-fault-healthy` 用例在挂接前后各枚举一次本进程全部 `MEM_MAPPED` 视图：挂接恰好新增 2 个，
大小 `0x11000` 与 `0x401000`，合计 `0x412000`（4.07 MiB），与设计 §4 列的“4 MiB 环 + 64 KiB 回复槽 + 两页控制头”一致。
环容量可由 `WARVK_D3D9_COMMAND_RING_KIB` 调（256 KiB–64 MiB，2 的幂）。相对地，管道路径的 4 槽队列
（每槽一个 64 KiB 包）在环模式下挂接之后不再使用，但对象还在；`Connection` 成为产品所有者时（阶段 5）一并去掉。

## 7. 实现中补上的点

- **失败的会话要让宿主知道流已结束。** 管道路径里 x86 失败会关管道，宿主读到 EOF 退出。环路径最初没有对应动作：
  空闲的宿主停在“等 `work` 或父进程”上，x86 随后等宿主退出要白等 5 秒。现在看护线程因任何失败退出时置生产者区的
  `closed` 并唤醒宿主（Finish 正常结束时不置）。“宿主挂起”用例验证了：恢复运行后宿主读到流结束并退出。
- **环模式的“远端成功”记账。** 管道逐条回复，可以逐条计数；环上异步记录没有回复。现在同步记录按自己的回复计数，
  异步记录只在 Finish 已执行且会话未失败时整体计为成功（宿主在第一条被拒绝的异步记录处就停，所以 Finish 被执行
  即证明之前的都成功）。第一版把同步发布的批次漏算了，准入门的拒绝用例查了出来。
- **无进展期限做成成员**（默认 10 s），测试可以缩短；产品值不变。
- 测试夹具自己的两处错误（不是产品代码）：worker 线程通过夹具的 `unique_ptr` 访问连接，而 `reset()` 先置空再析构；
  以及析构之后还读连接的字段。用 lldb 在隔离桌面上定位到前者（`noteTerminal` 写空指针偏移）。

## 8. 没做的与已知问题

- **阶段 1 第 2 项（内容载荷协议）、第 3 项（有界上传窗口与默认 x86 预算）未开始。** 设计 §7 只给了接口约束。
  （11-15 补记：两项已完成，见[回执（二）](2026-11-15-x64-phase1-content-window.md)，设计 §7 已改写为定稿。）
- `AutoTest/run_d3d9_connection_fault_ipc.py` 在本阶段之前就已失效：它的读方期待 5 行 stderr，而 10-29/30 的
  有序异步与固定批次已经让 HEAD 输出 7 行。与本次改动无关，没有修；它的对抗性 x64 对端只会说管道协议，
  环的对抗性用例已由 §4 的两套测试覆盖。修不修由作者定。
- `Connection` 的序号仍是 32 位（设计 §9）。
- 环默认关闭。把它设为默认之前还需要：真实游戏流量下的准入测量（阶段 5 的事），以及 §5 里说明的同步调用占比问题。
- 同步调用在环上是“自旋 2000 次再睡”。GPU 门里的夹具几乎全是同步读回，生产者每次同步调用都睡一次
  （`parks` ≈ `syncCalls`），这是预期的；它对同步密集的流量不比管道快，§5 有数。

## 9. 作者待确认（仍按建议值推进）

宿主崩溃/卡死时 x86 的行为：本实现只提供事实（终止原因 + 挂钩），策略未接入。建议值“安全退出并写报告、不做热回退”
在产品接入时（阶段 5）落在挂钩里。
