# 同一进程 A→B→A 换图门：2026-09-25 实机未通过

结论：地图确实在同一 War3 PID 内执行 A→B→A，三段小图阴影有持续绘制；但 WarVK 的地图 Reset/retirement 生产链**未被这条 `ChangeLevel` 路径触发**。这不是 M3 资源退休修复通过的证据，不能据此发布“跨地图已修复”。

## 用例与身份

- 两张已保存地图只读来源：`E:/Work/Warcraft III/Maps/Test/WorldEditTestMap.w3x` / SHA-256 `692E92F43BAC99EF8ECED7833E137B22475914F592DA7F78FCC24B54C221E95F`；`ShadowPressure.w3x` / `926822FC233C41431397499F6A922AB988C052BA0CA2E7EB14CB2B0581F5F4D8`。
- 用已有 32 位 StormLib 从原 MPQ 抽出 JASS，只对隔离副本加入 `ChangeLevel` 定时器、地图标记和 A 侧 `GameCache` 阶段；PJASS 两份成功，MPQ 再抽取与候选 JASS SHA 完全一致。候选 A 为 5,882,086 B / `410436ED826DEC572FC7F0E51FE214DFA75F975A11530593000D214AC6681A02`；B 为 5,879,426 B / `B730265584CB9B91F211112EB2535D2AD7989A9C24A1E685D38D0F3D7A911DDB`。源地图未回写。
- AutoTest 从 `E:/Work/Warcraft III` 原字节创建唯一实例，在实例目录内放 A/B、部署同 SHA DLL；没有覆盖玩家根目录、前台 YDWE 或原地图。DLL：36,618,365 B / `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`；Game.dll：`E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A`。独立 Win32 desktop、零全局输入，实例窗口设为 borderless **2560×1440** 并由实际截图尺寸核验。
- A 第一次约 20 秒后 `ChangeLevel(B)`；B 约 20 秒后 `ChangeLevel(A)`；A 从该实例私有 GameCache 读到 stage 1 并停留。三个互异 marker 文件各自保留哈希；这不是三次新进程，也不是只返回菜单。生产测试脚本：[runner](../../AutoTest/run_cross_map_ab_a_canary_20260925.py)。

## 运行过程与有效样本

- a：同 PID 与三张图成功，但 runner 错把 `get_runtime_status` 的直接 `result` 当作嵌套 `result.runtimeStatus`，采样为 0；该次“accepted=true”立即作废，保留失败回执，不追认。
- b：修正管道读取后，管道状态只含部分渲染指标，地图 epoch/retired 列不在其中；强门失败，不拿空值推断资源回收。
- c：打开启动即性能记录后，进程停在约第 101 帧，无法覆盖换图；仅停止经进程路径/创建时间核对的 AutoTest 实例，保留本次失败记录。它不能证明 `ChangeLevel` 有问题，也不能把性能日志模式诊断为确定根因。
- d（有效换图样本）：使用每约 10 秒新发布的 `runtime_status.json`，逐条记录文件年龄与实际 frameIndex，不把重复读取当独立帧。PID **58416** 自 A 到 B 到 A 未变；marker 到达时间约 **1.004 / 25.140 / 48.698 秒**。A-first/B/A-return 各有 **3/2/3** 个独立状态帧，尾部两帧各有完整接收与 producer ready；`shadowReplayDrawnCasterCount` 最大 **146/147/148**。三张 2560×1440 截图存在，但过曝与单帧取样不构成所有阴影的视觉验收。无新增 GPU 事件/incident；实例退出已结算；源 DLL、Game.dll、两张源地图 SHA 未变。

| 指标（有效 run d） | A-first | B | A-return |
| --- | ---: | ---: | ---: |
| `shadowMapEpoch` 最大值 | 1 | 1 | 1 |
| `shadowMapResetRequestedSerial` 最大值 | 0 | 0 | 0 |
| `shadowMapResetAppliedSerial` 最大值 | 0 | 0 | 0 |
| `shadowRetiredSessionCount` 最大值 | 0 | 0 | 0 |
| `shadowRetiredSessionCollectedCount` 最大值 | 0 | 0 | 0 |

原始回执：[run d result](../../AutoTest/artifacts/crossmap-20260925-d/maptest/cross-map-result.json) 19,504 B / SHA-256 `57CC1EB24FF4C3E50A9C5B61C67A90C7E04E637B8F78CF22C22CCDCCF377E02C`；[逐状态样本](../../AutoTest/artifacts/crossmap-20260925-d/maptest/runtime-samples.jsonl) 90,376 B / `27FCE9C28451CDE63E336200AF2DDB60EBEA0188A806199D96C27866CD387B2F`。独立 strict JSON 分析：[analysis](../../AutoTest/artifacts/crossmap-20260925-d/maptest/cross-map-analysis.json) 1,115 B / `1189E7614C1613DFC2A488E3A3CBBB1BF1B27C05F2C8E2E2C9C642B9DBFEEA88`，`ok=false`、`releaseAuthorized=false`，失败项正是 epoch 未推进、Reset 请求/应用未闭合、retirement 未覆盖。分析器和 6 个正反例在 `AutoTest/analyze_cross_map_ab_a_canary.py` 与 `AutoTest/test_analyze_cross_map_ab_a_canary.py`，均通过；缺失 B、跨 PID、单帧重复、冻结 epoch、无 retired 和 GPU 事件不会误通过。

## 源码解释与后续门

当前 `ResetWar3RuntimeState()` 通过 `War3Events::registerOnGameExit` 注册，实际 `fireOnGameExit()` 仅见 `NetEventHook::Handler` 的 `NET_EVENT_GAME_LEAVE` 分支。run d 的早期 DBWIN 又显示 `NetEventHook ... ready=0`，整次只有一次 `ActivateWar3Runtime`。因此在这个单机 `ChangeLevel` 路径里，Reset 请求未到达设备这一事实成立；究竟是网络事件 Hook 未安装、原生换图本就不发 GAME_LEAVE，还是两者兼有，仍需单独验证，不能只凭这轮定死单一原因。

下一步先接到**真实世界卸载/新建生命周期边界**，使生产者在旧图退出前关闭、Present 安全点申请并执行 Reset；不要直接在 JASS `ChangeLevel` 回调里释放 GPU/缓存，也不能用扩大预算或清零账冒充回收。新的唯一候选必须复跑同一 PID A→B→A，证明 epoch **1→2→3**（或其它严格递增值）、requested/applied 逐次闭合、旧 owners 在 completion serial 前保活、retired/collected 与 live/retired charge 按账回落，且每段合法 caster/接收/截图保持、无 GPU incident/尾延迟回归。还需单独测游戏菜单换图和 D3D9 Reset；本轮无 charge/perf 原始报告，因此不声称物理显存或性能收益。

当前测试**不授权发布**，保留已发布 v1.22.01 的“每张地图重启进程”建议。技能的实际作用体现在：测试使用原地图的编辑器保存基线、真实 PJASS 和 MPQ 读回，并严格区分进图、脚本切图、CPU 模型、GPU/画面与发布证据；没有把 Icefall 地图/JAPI 链的直接 `-loadfile` 结果误转用到该项目。
