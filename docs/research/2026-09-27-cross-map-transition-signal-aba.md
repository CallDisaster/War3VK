# 同进程换图生命周期信号：A→B→A 首次闭合（2026-09-27）

## 结论与边界

09-25 的同 PID `ChangeLevel` A→B→A [反例](2026-09-25-cross-map-aba-runtime-gate.md)确实没有触发 WarVK 的地图 Reset。新候选把**已完成一次 MainRunner 之后的下一次 JASS Native 表重建**作为早期会话边界；同一张地图加载时连续重建只请求一次。若未见该重建，下一次 MainRunner 入口兜底请求。请求立即关闭旧阴影生产准入，GPU 资源仍只由 Present 安全点退休；没有在 JASS Hook 内释放 GPU/Arena。

同一隔离实例的实机 A→B→A 已观察到请求、应用、epoch 与旧会话收集逐次推进，三段均有合法阴影绘制及完整 receiver，且无新增 GPU 事件。这**闭合了该 `ChangeLevel` 路径的触发缺口**，但不等于菜单换图、设备 Reset、高压长时、所有 caster 像素、物理显存和正式发布验收。

## 改动与一对多门控

- 生产与单元测试共用 `war3_map_transition_gate.h`：进程首轮未完成 MainRunner 前不发 Reset；首轮完成后 armed。`Hook_InitJassNatives` 在调用原函数**之前**消费 armed 并请求 Reset。B/A-return 每次各有连续 3 次 Native 初始化，只允许第一次消费；MainRunner 入口只在没有该信号时兜底，退出后重新 armed。
- 显式 `NET_EVENT_GAME_LEAVE` 原 Reset 路径继续保留并 disarm；没有移走已有运行时清理职责。Native 表重建代表一个 JASS VM 会话边界，不以 Game.dll 地址或地图路径相同/不同作判断；若游戏在同图中重建该表，仍可能发生一次保守的阴影会话 Reset，属于后续误触发观察项。
- 真实设备的 `War3RequestShadowMapEpochReset()` 只发布请求并关 producer gate；`War3ApplyShadowMapEpochResetAtPresent()` 在既有 Present 安全点应用、quarantine、提交 GPU 完成序号及收集。这次没有扩大预算、减少 caster、改剔除或清零账户来制造阴影恢复。

## 输入、构建与隔离执行

- 同一组只读 A/B 地图副本：A 5,882,086 B / `410436ED826DEC572FC7F0E51FE214DFA75F975A11530593000D214AC6681A02`，B 5,879,426 B / `B730265584CB9B91F211112EB2535D2AD7989A9C24A1E685D38D0F3D7A911DDB`。原 `E:/Work/Warcraft III` Game.dll / 地图 / 玩家 DLL 的开工 SHA 与 09-25 反例合同一致，收尾 `sourceUnchanged=true`。
- 构建只执行 exact `src/d3d9/d3d9.dll`，Ninja 父进程 Below Normal、`-j2`，实际 14 edge（13 个对象 + DLL link），exit 0，随后 exact target no-work。现有 build32 的 `warvk_data_collection_tree_dev=true`，**不是正式发布配置**。DLL 为 PE `pei-i386/i386`，**36,618,668 B / `83C1B7BE9E293A94C778FFEEE633C4A5F4089DD0C7AF6C136721FE866050B3E9`**。新增生产共用门控 C++ 测试通过；`test_cross_map_transition_signal_static.py` 4/4、既有 `test_shadow_cross_map_lifecycle_static.py` 24/24、分析器反例 6/6，py_compile、diff whitespace 均通过。
- [唯一 runner](../../AutoTest/run_cross_map_transition_gate_20260927.py)只改 run 身份与候选 DLL pin，复用旧地图/隔离实例/强门。单一 War3 PID **12980**，独立 Win32 desktop，客户端 **2560×1440**，零全局输入；A-first/B/A-return marker 约 1.004/25.056/49.053 秒。每段拍一张 2560×1440 截图。进程在样本尾仍活着，后由所属 AutoTest Job Object 强制停止并结算；不是自然游戏退出证明。当前游戏/编译相关进程为 0，玩家根 `d3d9.dll` 仍为 36,618,365 B / `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`。

| 指标 | A-first | B | A-return |
| --- | ---: | ---: | ---: |
| 独立状态帧 | 3 | 3 | 4 |
| `shadowMapEpoch` 最大值 | 1 | 2 | 3 |
| Reset requested / applied 最大值 | 0 / 0 | 1 / 1 | 2 / 2 |
| retired session 当前样本 | 0 | 0 | 0 |
| collected 累计最大值 | 0 | 1 | 2 |
| drawn caster 最大值 | 145 | 147 | 147 |
| 尾部两独立状态帧 receiver complete / producer ready | 2 / 2 | 2 / 2 | 2 / 2 |

DBWIN 的初始 A Native 初始化为 2 次、无请求；B 和 A-return 各 3 次，各只有首个 `Hook_InitJassNatives ENTER` 后出现 1 条 `JASS native-table rebuild requests shadow map reset`，且都早于对应 `Hook_MainRunner_Alt ENTER`。没有 MainRunner fallback 重复请求。周期状态是约 10 秒一次：B 和 A-return 各自首条仍可能是前图的旧 `runtime_status.json` 帧，表中用后续独立帧与最大值，不把 marker 到达时刻误作已应用 Reset 的时刻。`retiredSessionCount=0` 是粗采样的当前值，`collected=1/2` 证明收集路径发生过；不能从当前值 0 推断不存在瞬时 retired owner。

## 原始回执与未覆盖门

- [严格分析](../../AutoTest/artifacts/crossmap-20260927-transition-gate/mapgate/cross-map-analysis.json) 1,004 B / `783DF40D063F7C90FC7A954A66F2A6383714231180C3FBE11A7BAF3CA7855499`：`ok=true`、`failedChecks=[]`，但明确 `persistentChargeAccountVerified=false`、`fullVisualAcceptance=false`、`releaseAuthorized=false`。
- [运行回执](../../AutoTest/artifacts/crossmap-20260927-transition-gate/mapgate/cross-map-result.json) 19,509 B / `24A399551B3ECF78F5FA10B2CE01CA63DD239340826AA7357ED55802FFCC13B8`；[逐状态样本](../../AutoTest/artifacts/crossmap-20260927-transition-gate/mapgate/runtime-samples.jsonl) 188,269 B / `323DC166C816E04786F6B2D580FB0B5D944BD0BF9DE5D4253AFE7F99E5E148AE`。原始 `dbwin.jsonl`、三图及 source identity 均在同一 artifact 目录。
- 本轮只查看 `A-return.png` 这一张关键视图：地图标记和场景可见，地面存在投影，但画面局部过曝；不能凭一张图声明所有对象阴影均正确或无瞬闪。B 与 A-first 截图已保存、未逐像素审查。阶段脚本自身的 `transitionCanaryAccepted=true` 仅表示本合同强门，不是产品稳定版接受。
- 仍需菜单返回/再次进图、真正 D3D9 Reset、较大地图连续切换、在途 GPU 慢完成、live/retired charge、长时帧耗时和玩家视觉复核。这个候选不得因为一次 A→B→A 通过就推送、部署玩家现场或替代已发布 DLL。
