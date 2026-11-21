# 实机门搬到代理模式：现状、缺口、改法（2026-11-17 研究笔记）

只读研究。没有构建，没有跑任何脚本、游戏、宿主或探针。
"已核对"＝读过源码或运行留下的文件；"推断"＝由代码推出来、没在运行里看到。

行号按 2026-11-17 上午的工作区。`tools/render_host/dxvk_core/d3d9_host_main.cpp`、
`src/d3d9/war3/platform/war3_proxy_mode.cpp`、`tools/render_host/d3d9_proxy.cpp`
当天正被别的会话改（10:43–10:46 有新改动），这三个文件的行号会漂，动手前重新搜一下。

---

## 0 一句话结论

- 五个实机门现在**两种模式都跑不起来**：都钉着玩家目录的 `E:/Work/Warcraft III/d3d9.dll` 的哈希，而这个文件已经不在了（已核对，`ls` 找不到；`Game.dll`、`war3.exe`、三张测试图都还在）。所以先要决定：进程内那一臂是恢复玩家 DLL，还是改钉法——这不是代理模式的问题，但挡在它前面。
- 五个门里，**Walk 和压力门的路线部分本来就与模式无关**；**换图门、天气门、启动诊断门**的断言大半读的是进程内才有的诊断，代理模式下要么是 0，要么根本没有这个文件。
- 最省事的路：门脚本加 `--mode {inprocess,proxy}`，默认 inprocess，进程内分支一字不动；代理分支换环境、换钉法、换证据来源。证据来源有一部分现成（宿主日志、前端 stderr、数据层日志），另一部分要加 8 处小的计数器/诊断（N1–N8，第 2 节）。
- 描边在游戏里**不是选中/悬停，也不是队伍色**：只有地图脚本把句柄登记进描边集合才画，默认黄色、只画被挡住的部分。普通对局里看不到描边。代理模式的数据层日志里，单位影子投射体的句柄是 0，描边很可能匹配不上单位——要先查这个。
- 一轮代理模式实机门按顺序约 12–13 分钟；两臂都跑约 25 分钟；再加描边、视野外影子、换图三个探针约 11 分钟；宿主改了源码还得重跑 A/B 门约 20 分钟。

---

## 1 五个实机门是哪五个

五个门的名单来自发版回执：`docs/RELEASE_1.22.02.md:60-66,76`、`docs/RELEASE_1.22.03.md:56-62`、`DEVELOPMENT_CHANGELOG.md:79-81`（已核对）。
运行日志在 `E:/WarVK-Builds/v1.22.03-release-20261112/gate-*.log`（已核对）。

注意：`docs/research/2026-11-17-x64-phase5-1-proxy-mode.md` §7 写的是另一份清单（"run_native_d3d9_survey.py、跨图 A/B、压力门、快照压力、GPU 蒙皮"）。两份清单只在"跨图、压力"上重合。下面按发版回执的五个讲，另外三个在 1.6 单列。

### 1.1 总表

| 门 | 脚本 | 怎么启动 | 动作 | 进程内专属的断言 | 钉玩家 DLL | 1.22.03 用时 |
|---|---|---|---|---|---|---|
| 启动诊断 4 场景 | 仓库里**没有脚本**，只有回执 `AutoTest/artifacts/v12203-final-startup-20261112/startup-diag.json` | 每个场景起一次游戏，用环境变量制造失败或选档 | 等报告文件/对话框/日志行，退出 | 全部（见 1.2） | 不清楚（脚本缺） | 约 50 s（14:33:22–14:34:12） |
| 天气（2560×1440） | `AutoTest/run_weather_canary_20261010.py` | 隔离实例，`:828-837` | 冻结输入、跑天气时间线、拍关/开两张图 | render、frozenInput、volumetric 三段 | 是，`:802` | 约 2.5 min（15:03:32–15:06:02） |
| 同进程换图 A→B→A | `AutoTest/run_cross_map_ab_a_canary_20260925.py` | `war3.launch_war3_instance` profile `full_default`，`:167-183` | 地图脚本 20 s 后 `ChangeLevel` 到 B，再 20 s 回 A；全程读状态文件、按纪元拍三张图 | 几乎全部（见 1.4） | 是，`:34`、`:39` | 约 1.6 min（启动到 14:55:47 出结果） |
| 单位 Walk（Hamg） | `AutoTest/run_unit_walk_probe.py` | `:73-86` | 39 s 内反复调 `unit.walk.snapshot`，拍 2 张图 | 无 | 是，`:30`，包装器又钉一次 `run_v12202_final_gate.py:158` | 约 1 min（14:58:40–14:59:12 加载入） |
| 高压路线"生与死" | `AutoTest/run_v12202_final_gate.py` 的 `pressure()` → `AutoTest/war3_autotest_mcp.py:10194 run_life_and_death_tdr_scenario` | `_launch_suite_map_until_ready`（`war3_autotest_mcp.py:3095`） | 180 s 路线、3×3 网格、出生停留 25 s、严格外部钩子 | 状态文件新鲜度、影子证据、gpu_incident 文件 | 是，`run_v12202_final_gate.py:57`（期望清单）、`:134-136`、`:165-166` | 约 3.7 min（14:59:21–15:03:03） |

（各行用时由 1.22.03 运行目录的文件时间推出，已核对；"约"是因为启动前的准备时间没有单独记录。）

包装器 `run_v12202_final_gate.py` 只串起换图、Walk、压力三个门（`:138-149` 换图覆盖、`:150-161` Walk 覆盖、`:98-106` 压力），并在 `:22-24` 钉 `PLAYER_DLL_SHA256 = 1FFE0BD1…`（已核对）。天气门和启动诊断门是单独跑的（天气已核对；启动诊断是推断，脚本找不到）。

### 1.2 启动诊断 4 场景（回执已核对，脚本缺）

| 场景 | 制造方式 | 断言 |
|---|---|---|
| fail-quiet | `DXVK_FILTER_DEVICE_NAME=__warvk_no_such_gpu__` | `startup-error.txt` 里有 `site=Direct3DCreate9 kind=NoCompatibleDevice`，0.5 s 内出现；游戏弹自己的 DirectX 框后退出 |
| fail-dialog | 同上 + `DXVK_WAR3_STARTUP_DIALOG=force` | 出 WarVK 自己的对话框 |
| ok-auto | 默认 | `log/gpu.txt` 有 `WarVK GPU: … tier=high source=auto csmResolutionCap=4096` |
| ok-low | `DXVK_WAR3_VRAM_PRESET=low` | `tier=low csmResolutionCap=2048` |

进程内路径：`src/d3d9/d3d9_main.cpp:40-45` 失败时调 `ReportStartupFailure`（`src/d3d9/war3/startup/war3_startup_error.h:11-29`，只有 Interface、Device 两种站点）；显存档位日志在 `war3_vram_preset.cpp:81-103`（已核对）。

### 1.3 天气门

- 协议检查：`weather-status` 必须有 runtime、render、volumetric、frozenInput 四段，`run_weather_canary_20261010.py:884-889`（已核对）。
- "关"图就绪：`:910`；"开"图匹配：`active_capture_matches` `:493-514`，要求 `render.sameFrozenFrame`、`volumetric.compositeSubmitted`、阶段为 `composite-submitted`（已核对）。
- 分类：`:516-683`；健康：`collect_health` 找 gpu_incident 文件和事件关键字 `:685-721`；最终 `runtimeAcceptance` `:948`（已核对）。
- 窗口改成 2560×1440：`resize_owned_isolated_client` `:140`（已核对）。
- 钉：`:802` 对源地图、玩家 DLL、玩家 Game.dll 求 sha256，玩家 DLL 不在就失败（已核对）。

`weather-status` 的来源（`src/d3d9/war3/tools/war3_internal_test_api.cpp:293-340`，已核对）：
- runtime ＝ 数据层的 `WeatherRuntime`，两种模式都有（推断代理模式下有值）。
- render、frozenInput ＝ `ProcessServices().QueryWar3WeatherDiagnostics()`（`:295`）；volumetric ＝ `QueryWar3WeatherVolumeDiagnostics()`（`:334`）。进程服务在 DLL 加载时总是注册成进程内实现（`src/d3d9/d3d9_war3_execution_services.cpp:106-111`，已核对），转去读进程内渲染器的全局量（同文件 `:80-86`，已核对）。代理模式下进程内渲染器不画天气，这些量应当一直是 0（推断）。

### 1.4 换图门 A→B→A

- 钉：`EXPECTED` `:32-40`（玩家 d3d9 `:34`、DLL `:39`）；启动后再比 DLL 哈希 `:192-193`（已核对）。
- 地图：`AutoTest/artifacts/v122_visual_api_20260914/cross-map-20260925/{A,B}.w3x` 复制到实例 `Maps/AutoTest/WarVKCrossMap20260925/`。A 的脚本 2 s 起计时、20 s 后 `ChangeLevel("Maps\\AutoTest\\WarVKCrossMap20260925\\B.w3x")`（`A-candidate.j:1661,1675,1703`），并用 `PreloadGenEnd("WarVK\\Temp\\crossmap_A_stage_<n>.txt")` 写标记（`:1670`）；B 写 `crossmap_B.txt`，20 s 后换回 A（`B-candidate.j` 约 `:1586-1597`）（已核对）。
- 窗口：`borderless_1440` `:202`（已核对）。
- **遥测只来自** `WarVK/Temp/runtime_status.json`，年龄 ≤12 s `:228-241`，停更 >15 s 直接抛错 `:274-275`（已核对）。这个文件只有进程内流水线在写：`d3d9_war3_pipeline.cpp:832` → 每 1200 帧 `LogRuntimeHealthPeriodic`（`war3_diagnostics_hub.cpp:4474-4515`），另在开局、重置时写（`war3_events.h:96,166`）（已核对）。
- 监视字段 `MONITOR_KEYS` `:41-51`：shadowMapEpoch、ResetRequested/AppliedSerial、TransitionState、ProducerReady、各种 retire serial、退役会话数/字节、queueSubmitted/Completed、shadowReplayCasterCount、shadowReplayDrawnCasterCount、semanticSceneShadowMapDrawnCasters、semanticSceneReceiverHasCompleteShadowMap、semanticSceneShadowMapExecutedThisFrame（已核对）。
- 拍图条件：纪元对、producerReady、receiverComplete、drawnCasters>0，`:276-304`（已核对）。
- 判定 `:370-399`：mapEpochAdvanced（纪元序列 [1,2,3] 且每段 ≥5 个样本）、resetSerialAdvanced、retirementCoverage、runtimeStateGate（尾部 receiverComplete==2、producerReady==2、drawnCasterMax>0）、telemetryErrors<20、没有新 GPU 事件、没有 gpu_incident 文件、3 张图纪元对得上（已核对）。
- 1.22.03 的这次运行（`v12202-v12203-final-20261112-b-crossmap`）：三段开始于 1.0 s、24.6 s、47.7 s；drawnCasterMax 145；resetRequested/Applied 从 2 到 4（已核对）。发版日志里换图门 exit=2、`resetSerialAdvanced false / runtimeStateGate false`，回执说"严格分析器 exit 2，每次换图重置序号 +2"（已核对）——也就是说这个门在 1.22.03 已经是"带解释通过"，不是干净通过。

### 1.5 Walk 门与压力门

Walk（已核对）：
- `PINNED` `:26-32`；循环 39 s 轮询测试命令 `unit.walk.snapshot`（`war3_internal_test_api.cpp:865-871`，经 `game::QueryUnitWalkDiagnostics`，是 CPU 钩子计数）并拍 2 张图 `:108-136`。
- 分类字段 jassRequestReachedDll / Accepted / autoWalkRequested / Suppressed / restored `:138-145`。
- **`main` 无条件返回 0**（`:147`）：只有异常才算失败，分类结果不影响退出码。

压力门（已核对）：
- `run_life_and_death_tdr_scenario`（duration 180、grid 3、birth_hold 25、ack difficulty、strict hooks），包装器断言 `ok`、无新 GPU 事件、无新 GPU incident（`run_v12202_final_gate.py:114-115`）。
- 场景内部：严格外部钩子 `war3_autotest_mcp.py:10402-10406`；采样 = 控制面 `get_runtime_status` + 新鲜度 ≤5 s 的状态文件（`:10517-10545`、`:10668-10699`）；设备丢失判定 `_runtime_status_device_lost` `:7823`；`device_lost/route_ok` `:10757-10765` ＝ 没有失败原因、没有新 Windows GPU 事件、没有 gpu_incident 文件、没有 deviceLost 关键字。
- `_runtime_log_paths` `:3848` 找的是实例下的 `war3_d3d9.log`、`dxvk.log`，但 `DXVK_LOG_PATH` 指向产物目录，所以这项在两种模式下大概都是瞎的（推断）。

### 1.6 另一份清单里的三个

- `run_native_d3d9_survey.py`：进程内编译期普查（`WARVK_D3D9_MIGRATION_CENSUS`），量的是进程内 DXVK 路径的迁移情况，`:754` 玩家 DLL 不在就报 "player DLL preservation witness missing"，分类 `:274-312`（已核对）。代理模式下没有对应物，也不需要——它问的问题在代理模式下不存在（推断）。
- `run_snapshot_pressure_canary.py`：往玩家目录 `E:/Work/Warcraft III` 里写东西（已核对），越界，搬之前要先改成隔离实例。
- GPU 蒙皮：发版里冻结为关（路线图，已核对），不用搬。

---

## 2 逐条断言：代理模式下拿什么替

代理模式现成的证据（`AutoTest/artifacts/x64-phase52-ab-20261117-r1/losttemple-r1-proxy/`，已核对）：

- `game.stderr`：前端在**退出时**写一串 JSON：async-clear、draw-batch-proxy、ordered-async、fixed-batch-proxy、annotation-proxy（annotatedDraws 298964、records 564828、frameScenes 8275、sessionResetsRequested 1、**sessionResetsSent 0**）、content-proxy、command-ring-proxy、backing-proxy、upload-window-proxy、fvf-proxy、window-proxy（resizes、refused）、terminal（kind none、published==applied 1164365）。
- `logs/War3_d3d9.log`：数据层行，`caster class=…`（每类前 6 个）、`first BeforeUi frame=464`、`casters frame=900/1800/2118 considered/sent/alpha/terrain/doodads/worldObjects/units/dynamicBytes/omittedFrames/skipped[]`、`map change, session reset generation=1`、`frames=2118 worldScenes=1655 insertions=1655 … refused=0`、`device released`。
- `host.log*`：`scene-effects-host`（beforeUiRuns 1655、ssao/snow/aa/bloom 记录数、insertionsRefused 0）、`shadow-host/v2`（casterRecords 298964、runs 1655、mapRenders、receiverDraws、castersUsed/Refused、replayRejected 0、peakResourceBytes、last{casters 181, drawnCasters 724, completeMap 1, outline 0, volumetric…}、前 256 次 runLog、deviceMemoryLog）、`annotation-host`（sessionResets 0、sessionGeneration 0）、最后的 `warvk-d3d9-host/v1`（accepted、rejected 0、normalTeardown true、liveResources 0）、`exit-intent normal-return`。

一个能直接用的对时钟：x86 的 `FrameStatus.insertions`（`war3_proxy_frame.cpp:394`）和宿主的 `effectFrames/beforeUiRuns` 是同一个序号（这次都是 1655，已核对）。不改线上协议，就能把宿主的逐次记录和 x86 侧的阶段对上。

两个现成证据的限制：
- 前端 JSON 只在退出时写一次；宿主日志也大多是退出时汇总，runLog 只留前 256 次（`d3d9_host_main.cpp:234`）。门要的"每一段一份"拿不到。
- 用 `war3_autotest_mcp.py` 启动的游戏（压力门、Walk、换图门都是）不接 stdout/stderr：`_launch_process_on_desktop` `:1601`，`CREATE_NEW_CONSOLE`、没有标准句柄 `:1640-1660`（已核对）。所以前端 stderr 那串 JSON 在这些门里**根本收不到**。探针能收到是因为它用 `isolated_render_desktop.py:359-368` 的 HANDLE_LIST（已核对）。

### 2.1 逐门对照

**启动诊断**

| 断言 | 进程内来源 | 代理模式现状 | 缺口 |
|---|---|---|---|
| startup-error.txt 写站点和原因 | `d3d9_main.cpp:40-45` | 代理分支 `d3d9_main.cpp:217-225` 不调 `ReportStartupFailure`；`CreateFactory` 失败直接返回空（`war3_proxy_mode.cpp:287-299`），`CreateDevice` 包装 `:251-255` 也不报（已核对） | N6 |
| 设备过滤真的生效 | x86 自己枚举适配器 | 枚举在宿主里；宿主是否继承 `DXVK_FILTER_DEVICE_NAME` 没查（推断会继承，前端用当前环境起宿主） | 跑一次看 |
| WarVK 对话框 | 同上 | 同上 | N6 |
| `WarVK GPU: tier=… csmResolutionCap=…` | `war3_vram_preset.cpp:81-103` | `war3_proxy_frame.cpp:83-84` 明写代理模式不套显存档位，所以没有这行（已核对） | N6b |

**天气**

| 断言 | 代理模式现状 | 缺口 |
|---|---|---|
| runtime 段（阶段、时钟） | 数据层，有值（推断） | 无 |
| render.sameFrozenFrame、frozenInput | 走进程内渲染器，恒 0（推断，见 1.3） | N5 |
| volumetric.compositeSubmitted | 同上；宿主 `shadow-host last.volumetric`、`scene-effects-host snowRecorded` 只在退出时有 | N4 |
| 关/开两张图 | `capture_final_frame` 在代理模式下走宿主目标回读（5.1 文档 §5，已核对） | 无 |
| gpu_incident 文件、事件关键字 | 进程内诊断写的；代理模式不写 | 换成宿主日志 + terminal 行 + Windows 事件 |
| 窗口改到 2560×1440 | 前端 `window-proxy {resizes, refused}`（退出时） | 门里收不到 stderr → N8 |

**换图 A→B→A**

| 断言 | 代理模式现状 | 缺口 |
|---|---|---|
| 状态文件 ≤12 s 新鲜 | 代理模式没人写这个文件（推断：`d3d9_war3_pipeline.cpp:832` 只在进程内流水线里） | N2，或代理模式改轮询控制面 + N1 |
| shadowMapEpoch [1,2,3]、ProducerReady、retire serial、退役字节 | `QueryWar3ShadowLifecycleDiagnostics` 在代理模式返回 `{}`（`war3_proxy_services.cpp:69-71`，已核对），全是 0 | 换成会话代号（下一行）+ N3 |
| ResetRequested/AppliedSerial | 每次请求都 `FrameSessionReset` + `RequestSessionReset` 并记一行 `map change, session reset generation=N`（`war3_proxy_services.cpp:48-62`，已核对）；不按 Present 合并，所以一次换图代号可能 +2（推断，和进程内"每次 +2"一致） | 断言改成"严格递增"，不要"恰好 +1" |
| 每段 receiverComplete、drawnCasters>0 | 宿主只有退出时的 `last{completeMap, drawnCasters}` 和前 256 次 runLog | N3（每代一行）或 N1（实时） |
| queueSubmitted/Completed | 宿主侧的事；前端 terminal `published==applied` 可作整体替代 | 无 |
| 纪元对上的三张图 | 拍图本身可用；"纪元对上"要改成"会话代号 + insertions 已越过换图点" | N1 |
| 没有 gpu_incident | 同天气 | 同天气 |

另一个要先弄清的现象：LT 这次 `sessionResetsRequested 1 / Sent 0`，宿主 `sessionResets 0`（已核对）。这次的重置是退出时卸图触发的，之后没有下一帧，所以没发出去（推断）。换图后新图会继续出帧，应当会发出去，但没跑过，见"还不知道的"。

**Walk**：全部是数据层 CPU 钩子计数，两种模式一样（已核对来源；推断代理模式下有值）。只要换环境和钉法。

**压力**

| 断言 | 代理模式现状 | 缺口 |
|---|---|---|
| 路线跑完（相机、视野、没崩） | 与模式无关；宿主掉了游戏会以 `0xE0574B01` 退出，路线自然判失败（5.1 文档 §5，已核对） | 无 |
| 状态文件新鲜 ≤5 s | 代理模式没有 | N2，或代理分支跳过文件只用控制面 |
| 设备丢失 | `QueryWar3DeviceStatus` 在终止时返回 -4（`war3_proxy_services.cpp:73-76`，已核对），`_runtime_status_device_lost` 能否认出要看它读哪个字段（没查） | 查 `war3_autotest_mcp.py:7823`；加 terminal 行（N8） |
| gpu_incident 文件 | 同上 | 宿主日志 + Windows 事件 |
| 期望清单里有 PLAYER/d3d9.dll | `run_v12202_final_gate.py:57` | 代理分支换钉法 |

### 2.2 新加的计数器/诊断（N1–N8）

| 编号 | 内容 | 放哪 | 碰不碰宿主源码 |
|---|---|---|---|
| N1 | `get_runtime_status` 里加 `proxy` 段：mode、会话代号、frames、worldScenes、insertions、refused、上一帧的 considered/sent/units/terrain/alpha、累计数、描边匹配数（N7b）。探针已经在找 `result.get("proxyMode")`（`run_x64_game_proxy_probe.py:455-457`，现在永远找不到），字段名就用它 | `src/d3d9/war3/tools/war3_control_plane.cpp:224`（ToJson）/ `:301`（shadow 段附近）；数据来源 `war3_proxy_mode.cpp:329 QueryStatus`、`war3_proxy_frame.cpp:621-624 QueryFrameStatus`（目前没有调用方，已核对）、`war3_proxy_casters.cpp:553 CastersFrameAccount` | 否 |
| N2 | 代理模式也定期写 `runtime_status.json`（比如每 120 帧），内容至少带 N1 的段；同时让诊断中心的文件 JSON（`war3_diagnostics_hub.cpp:2647` 附近）带上它 | `war3_proxy_frame.cpp:582-606 FramePresent` 里，挨着 `:604-605` 每 900 帧的 `LogCasters` | 否 |
| N3 | 宿主按会话代号记账：每次 SessionReset 和退出时各写一行 `{generation, runs, completeMapRuns, lastCompleteMap, maxDrawnCasters, deviceUsed, deviceAllocated}` | `d3d9_host_main.cpp:681`（SessionReset 分支）和 `:1260`（normal-return 前） | 是 |
| N4 | 宿主效果边沿行：某次 BeforeUi 的效果集合（snow、volumetric、fog…）和上一次不同时写一行，带 `effectFrames` | `d3d9_host_main.cpp:672-673`（runLog push 之后） | 是 |
| N5 | x86 记下"天气记录已发出"的快照（冻结输入、最后一帧发出的 Snow/Fog/Volumetric 记录），在代理模式下填进 `weather-status` 的新段 | `war3_proxy_frame.cpp` Snow 记录发出处（约 `:380`）；`war3_internal_test_api.cpp:293-340` | 否 |
| N6 | 代理模式的启动失败也调 `ReportStartupFailure`，带宿主给的原因；N6b：代理模式下用代理过来的 `GetAdapterIdentifier`/`GetAvailableTextureMem` 套显存档位并记 `WarVK GPU:` 行（推断可行） | `war3_proxy_mode.cpp:251-255`、`:287-299`；`war3_proxy_frame.cpp:83-84` | 否（但改启动生命周期，见第 3 节分工） |
| N7 | 描边：a) 测试命令 `outline.pick {kind, index}` / `outline.clear`，用 `RenderObjectRegistry` 选对象登记句柄，并可临时打开 showVisible；b) x86 计数"本帧发出、句柄在描边集合里的投射体数"；c) 宿主描边 pass 实际画了几次（ShadowRunFacts 里加 `outlineDraws`） | a) `war3_internal_test_api.cpp:722` 旁边；b) `war3_proxy_casters.cpp:472` 之后；c) `d3d9_war3_shadow_outline.cpp:228`、`:817` | c 是 |
| N8 | `war3_autotest_mcp._launch_process_on_desktop` 加可选参数接 stderr（照 `isolated_render_desktop.py:359-368` 的 HANDLE_LIST），或前端支持一个把这串 JSON 写到文件的环境变量 | `war3_autotest_mcp.py:1601`、`:1640-1660`；或 `d3d9_proxy.cpp` | 否 |

想要"运行中实时问宿主"（像 `DiagnosticCheckpoint` 那样，`d3d9_host_main.cpp:838`，前端导出 `WarVKD3D9DiagnosticCheckpoint@20` 在 `d3d9_proxy.cpp:1964`）就是改协议，按路线图归 Fable。上面 N3/N4 只写日志、不改线上格式，故意避开了这条。

---

## 3 给 Opus 的改法清单

总原则：每个门脚本加 `--mode {inprocess,proxy}`，默认 `inprocess`。所有新代码都在 `if mode == "proxy"` 里；进程内分支的环境、钉、断言、输出文件名一字不改。

### 3.1 公共部分（新建一个小模块，或直接从探针导入）

1. 环境：照探针 `run_x64_game_proxy_probe.py:676-692` 设 `WARVK_D3D9_FRONT`（前端完整路径）、`WARVK_D3D9_HOST_EXE`、`WARVK_D3D9_HOST_LOG`、`WARVK_D3D9_GAME=sandbox-isolated-v1`、`WARVK_D3D9_PROFILE=presenter-isolated-v1`、产品传输档（STREAM + FIXED_BATCH + COMMAND_RING + `X86_BACKING_KIB=32768` + `UPLOAD_WINDOW_KIB=1024`）、`ANNOTATIONS=1`。前端只认 `sandbox-isolated-v1` 加非输入 WinSta0 桌面（`d3d9_window_identity.h:39-51`，已核对）——用 `war3_autotest_mcp` 启动的门要确认它起游戏的桌面满足这条（推断满足，它也用隔离桌面，没逐行核）。
2. 把 front.dll、host.exe 复制进产物目录，记 sha256，把它们当新的钉。
3. 玩家 DLL 钉：代理分支改成探针的"本来不在、跑完也不在；在的话哈希不变"（`run_x64_game_proxy_probe.py:403-405`）。Game.dll、war3.exe、地图的钉不动。
4. 证据收集：复用探针的 `summarize`（`:787-878`）解析 `game.stderr` 的 `D3D9 game:`/`D3D9 terminal:` 行和 `host.log*` 的 JSON；war3_autotest_mcp 起的门先要 N8 才有 stderr。
5. 通用代理验收：terminal `kind=none` 且 `published==applied`；宿主最后 JSON `rejected 0`、`normalTeardown true`、`liveResources 0`；`insertionsRefused 0`、`castersRefused 0`、`replayRejected 0`、`sessionResetsRefused 0`；产物目录没有 render-host-lost 文件；游戏退出码不是 `0xE0574B01`。

### 3.2 逐门

**启动诊断**
1. 先按回执 `startup-diag.json` 把脚本补回仓库（只做进程内，和回执逐项对齐）。
2. 代理分支：N6 之前，fail-quiet/fail-dialog 记成"代理模式不适用"，**不能记通过**；ok-auto/ok-low 在 N6b 之前同样记不适用。
3. N6 之后：fail 场景断言站点名可能不同（宿主报的原因），用新字段名对，不改进程内的期望字符串。

**天气**
1. 协议检查 `:884-889`：代理分支允许 render/volumetric 为 0，但要求有 N5 的新段。
2. `active_capture_matches` `:493-514`：代理分支换成 runtime 阶段到 `composite-submitted` + N5 快照里 Snow/Volumetric 记录已发 + N4 宿主边沿行的 `effectFrames` ≥ 该阶段的 insertions + 关/开两张图有像素差。
3. 健康 `:685-721`：代理分支换 3.1 第 5 条。
4. 改窗口 `:140`：代理分支再要求 `window-proxy.resizes ≥ 1 且 refused 0`。
5. 钉 `:802`：代理分支换钉法。

**换图 A→B→A**
1. 遥测来源 `:228-241`：代理分支改为每秒调一次控制面 `get_runtime_status` 读 N1（或等 N2 后继续读文件，年龄阈值不变）。
2. `PHASE_EPOCH`：代理分支用会话代号，只要求三段严格递增（0 → ≥1 → 更大），不要求等于 1/2/3。
3. resetSerialAdvanced：代理分支 = 数据层日志的 `session reset generation=` 行数 ≥2，且退出时 `sessionResetsSent == sessionResetsRequested`（需要 N8 或 N1 带上前端计数）。
4. retirementCoverage：代理分支 = N3 每代的 `deviceUsed` 回到第一代的 ±某个余量（余量要先跑一次定）。
5. runtimeStateGate：代理分支 = N3 每代 `completeMapRuns > 0` 且 `maxDrawnCasters > 0`。
6. 拍图条件 `:276-304`：代理分支 = 代号到了这一段、且 insertions 比进段时多 ≥ 30。
7. 钉 `:34`、`:39`、`:192-193`：代理分支换钉法。

**Walk**
1. 只加环境和钉法（`:26-32`；包装器 `:150-161`，`:158`）。
2. 别顺手把 `main` 改成按分类失败——那会改进程内行为。在计划里单独记一条。

**压力**
1. `run_life_and_death_tdr_scenario` 要能收环境覆盖。`launch` 路径有 `os.environ.copy()+overrides`（`war3_autotest_mcp.py:6389-6419`），但 `_launch_suite_map_until_ready`（`:3095`）是否把覆盖传到底没查；没有就加一个可选参数，默认空。
2. 状态采样 `:10517-10545`、`:10668-10699`：代理分支不要求文件新鲜（N2 之前），只用控制面。
3. 设备丢失 `:7823`、`:10757-10765`：代理分支加"terminal kind 不是 none 或退出码 0xE0574B01 即丢失"。
4. 包装器期望清单 `:57`、钉 `:134-136`、`:165-166`：代理分支换钉法；`--mode` 从包装器往下传。

### 3.3 分工

- 第 3 节脚本改动、N1、N2、N5、N7a/b、N8：Opus。
- N3、N4、N7c 改宿主源码：宿主源码冻结后要重编并重跑 3 个宿主门（路线图规则，已核对）。改动只是写日志，Opus 可以做。
- N6 碰启动生命周期，路线图把"传输/生命周期"归 Fable；如果只是在失败点多调一次 `ReportStartupFailure`、不改握手，可以先让 Opus 做，出问题再换（推断）。

---

## 4 描边怎么在游戏里验

### 4.1 描边是什么样

- 不是选中、不是悬停、不是队伍色（已核对）。默认设置 `d3d9_war3_settings.h:422-433`：开启、Silhouette、屏幕空间、3 px、`showVisible=false`、`showOccluded=true`、颜色 RGBA(1,1,0,0.8) 黄色。也就是**只画被挡住的部分**。
- 画谁由句柄集合决定：地图 JASS 通过导出函数登记（`war3_shader_api.cpp:346-366`：AddOutlineHandle、AddOutlineLastRenderHandle、AddOutlineHandleByIndex，经 `RenderObjectRegistry`）。老的 `cmd:outline-add-handle`（`war3_jass_command_bridge.cpp:332-355`）在发版里被关了（`:1155-1160`、`war3_jass_legacy_command_policy.h:17-22`，只给一个测试 exe 打开，`src/d3d9/meson.build:843`）。公开 v1 只有 `outline.setEnabled/setColor/setParameters`（`war3_japi_v1.cpp:276-278`、`WarVK/jass/warvk_api.j:475-486`），没有加句柄（已核对）。
- 所以普通地图、普通对局里看不到描边（推断，除非地图自己调了导出函数）。
- pass 怎么认对象：拿投射体的 `batchHandle` 去问 `IsOutlineHandle`（`d3d9_war3_shadow_outline.cpp:228`、`:817`），匹配不上会记 `DXVK_Outline: no draw match`（`:234-249`）（已核对）。宿主 runLog 里的 `outline` 只说明"设置开着且有句柄"，不说明真匹配上了（4d-1 §3，已核对）。

### 4.2 代理模式的风险，先查这个

代理模式数据层的 `batchHandle` 依次取 TLS 批次句柄 → 当前对象 jHandle → 语义桥 jHandle，再规整成 `0x100000|低20位`（`war3_proxy_casters.cpp:259-270`，已核对）。LT 代理运行的日志里，单位类的前 6 个投射体（`class=3 … unit=1`）全是 `handle=0x0 rawcode=0x0`（`logs/War3_d3d9.log:27-32`，已核对；只记了前 6 个，后面的推断同样多半为 0）。地形类也是 0，这正常。

如果单位投射体的句柄普遍是 0，代理模式下单位描边一个也匹配不上。所以第一步是 N7b（或者把"按类统计句柄非 0 的投射体数"加进现有的每 900 帧那行），跑一局看比例。比例低就要先修句柄来源，再谈画面验证。

### 4.3 探针现在缺什么

- `test=` 只发空负载（`run_x64_game_proxy_probe.py:349`），白名单只有 7 个命令（`:324-329`：game.end_for_exit_test、game.restart_for_map_test、visual.snapshot、weather.status、visibility.full_map、camera.snapshot、game.pause）。`api=` 只认 warvk:v1 公开 API（`war3_internal_test_api.cpp:894-906` / `war3_jass_command_bridge.cpp:1728-1757`）（已核对）。
- 要加：`test=名字:JSON负载`；白名单加 `camera.apply`（`war3_internal_test_api.cpp:528-592`）、`camera.fixed`（`:668`）、`shadow.lock_sun`（`:477-492`）、N7a 的 `outline.pick/clear`。
- `gpu_skin.outline_test_mode`（`:722`）是"所有对象 + 强制"的调试模式，不走 Outline 记录，不能拿它代替。

### 4.4 验法

1. 暂停：设 `DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE=0` 再发 `game.pause`（`:493`）（推断这样能真暂停；5.1 文档说探针做过"真暂停"）。
2. 固定相机：`camera.apply` 到一个已知建筑（静止，比单位稳）。
3. 拍"描边关"图 → `outline.pick` 选这个建筑（可同时开 showVisible，免得要靠遮挡）→ 等 3 帧 → 拍"描边开"图。
4. 两图相减取黄色像素作掩码。两种模式在同一相机下各做一遍，比掩码：像素数差、IoU。
5. 计数：进程内看 `DXVK_Outline` 日志有无 no draw match；代理看 N7b（x86 匹配数）和 N7c（宿主 outlineDraws）。三者都 >0 且掩码对得上才算过。
6. 单位再做一遍（4.2 修好之后）。

`GetMutableSettings` 走当前的设置邮箱（`war3.cpp:291`），代理模式发布 `FrameSettingsMailbox`，所以 `shadow.lock_sun`、showVisible 这类设置改动应当能到宿主（推断）。

---

## 5 视野外单位缺影子（5.1-d）怎么量

### 5.1 两种模式的差别

- 代理模式：影子投射体就是游戏自己的绘制（`war3_proxy_casters.cpp`，`kMaxCastersPerFrame 8192` 在 `:36`）。游戏剔掉、不画的单位就没有影子（计划 §6.1 已列为已知缺陷，已核对）。
- 进程内：单位影子来自语义场景，语义场景读的是 `VisibleRenderableRegistry`，而它也是从游戏的渲染队列登记来的（`war3_renderer.cpp:488-509 OnVisibleRenderables/OnTransparentRenderable`），另有租约/TTL，过期后还会补画 3–6 帧（已核对来源）。所以进程内**多半也缺**，区别可能只是刚出屏那几帧（推断）。必须量，不能假定进程内是好的。

### 5.2 量法（相机扫描）

1. 选一个静止建筑（如主基地），`visibility.full_map`（`:629`），低太阳拉长影子：测试命令 `shadow.lock_sun {enabled, time01}`，或公开 API `sun.setDirection`（`war3_japi_v1.cpp` 有 `sun.setDirection`、`csm.setEnabled`，探针 `api=` 已能调，已核对）。
2. `camera.world_bounds`（`:613`）拿地图范围，`camera.apply` 先把建筑放在屏幕中央，然后每步把相机往外推屏宽的 5%，直到建筑完全出屏、再多走几步直到影子也该出屏。
3. 每步：等 5 帧、拍图、记计数。进程内记 `get_runtime_status` 里的 `semanticSceneSubmittedUnit/Building`、`shadowReplayCasterCount`；代理记 N1 上一帧的 units/considered/sent。
4. 另拍一组 `csm.setEnabled false` 的同位置图，相减得"影子掩码"，只看靠建筑那一侧的屏幕边缘条带。
5. 结果：每种模式画一条"影子像素数 vs 相机步"的曲线。建筑出屏那一步之后，影子还该有、实际没有的步数，就是缺影的范围。两条曲线对比，就知道代理模式比进程内多缺多少。

### 5.3 什么算"该有"

按太阳方向和建筑高度算影子在屏幕上的长度太麻烦。省事的办法：把太阳方向反过来（影子朝屏内 vs 朝屏外）各扫一遍。影子朝屏内那组，建筑出屏后影子还应该留一段；朝屏外那组是对照。

---

## 6 换到另一张图，探针要什么

现状：探针只部署一张图（`run_x64_game_proxy_probe.py:650`），换图只做过 `game.restart_for_map_test`（同图重开；该命令要 `DXVK_WAR3_INTERNAL_EXIT_TEST=1`，`war3_internal_test_api.cpp:1067-1075`，已核对）。`--seconds ≤240`、`timeout ≤300`（`:604`）。

要加：
1. `--extra-map SRC=REL`：把换图门那两张图（哈希钉 MAP_A `4104…`、MAP_B `B730…`，见 `run_cross_map_ab_a_canary_20260925.py:32-40`）复制到实例的 `Maps/AutoTest/WarVKCrossMap20260925/`。路径必须一模一样，A/B 脚本里写死了。
2. 启动 A；A 脚本自己 20 s 后换 B、B 再 20 s 换回 A，载入后约 70 s 走完，放得进 240 s。
3. 用实例 `WarVK/Temp/crossmap_*` 标记文件定位每段开始时间；每段开始后拍一张图（相机用 `camera.apply` 定住，两种模式可比）。
4. 断言：数据层 `session reset generation=` 递增；宿主 `annotation-host sessionResets` ≥2 且 `sessionGeneration` 跟上；退出时 `sessionResetsSent == Requested`、`Refused 0`；宿主 `rejected 0`；x86 私有字节和宿主显存（N3）每代基本持平。
5. 再加一组"换到完全不同的地图"（比如从 LT 换到 ShadowPressure）：需要一张会 `ChangeLevel` 的小图，或者给 `game.restart_for_map_test` 加一个地图路径参数（推断后者更省事，但它在测试命令里，要看实现）。

---

## 7 用时

| 门 | 1.22.03 进程内（已核对，由文件时间推） | 代理模式（推断） | 依据 |
|---|---|---|---|
| 启动诊断 4 场景 | 约 50 s | 约 60–70 s | 每场景多起一次宿主 |
| 换图 A→B→A | 约 1.6 min | 约 2 min | 载入 LT 8.6 s → 12.7 s（A/B 门实测），三次载入 |
| Walk | 约 1 min | 约 1.1 min | 39 s 固定循环 + 载入 |
| 压力"生与死" | 约 3.7 min | 约 4 min | 路线 180 s 固定；生与死载入两种模式都约 75 s |
| 天气 | 约 2.5 min | 约 2.7 min | |
| **一轮代理模式（顺序）** | | **约 11–12 min，加清场约 12–13 min** | |
| 两臂都跑 | | 约 25 min | |
| 新探针：描边、视野外扫描、换图，各两种模式 | | 约 11 min | 每次 1.5–2 min |
| 宿主改源码后的 A/B 门 | | 约 20 min | 12 次运行，01:57–02:17（已核对） |

机器是共享的，别的会话在跑大模型，计时会抖；以上只用来排计划。

---

## 给计划的结论

1. **先解玩家 DLL 钉。** 五个门现在两种模式都会在钉上失败（玩家 `d3d9.dll` 已不在）。进程内要么恢复文件，要么由用户决定改钉；代理模式直接用探针的"本来不在、跑完也不在"。
2. **按 `--mode` 分支改，默认进程内。** 进程内分支不动，所有新逻辑在代理分支里。
3. **先做不碰宿主的几项：** N8（门里收得到前端 stderr）、N1（控制面 `proxy` 段）、N2（代理模式写状态文件）。有了这三项，Walk、压力、换图三个门就能在代理模式下判。
4. **天气和启动诊断要多做一步：** 天气要 N5 + N4；启动诊断要先补回脚本，再做 N6/N6b。之前一律记"代理模式不适用"，不记通过。
5. **宿主日志（N3/N4/N7c）攒成一次改：** 改完重编、重跑 3 个宿主门和 A/B 门。
6. **描边先查句柄：** LT 代理日志里单位投射体句柄是 0。先统计句柄非 0 的比例，低就先修句柄来源，再做画面验证。
7. **视野外影子两种模式都量。** 进程内的单位影子也来自游戏渲染队列，不要默认进程内是对的。
8. **换图探针复用换图门那两张图，** 加 `--extra-map`，约 70 s 走完 A→B→A。
9. 一轮代理模式实机门约 12–13 分钟；全套（两臂 + 三个新探针 + A/B 门）约 55 分钟，全部顺序跑。

## 还不知道的

- 启动诊断门的脚本在哪、钉了什么——仓库里没找到，只有回执。
- 代理模式下宿主是否真的继承 `DXVK_FILTER_DEVICE_NAME` 等过滤变量；失败时前端/游戏的表现（静默退出？弹 DirectX 框？）。
- 换图后 `sessionResetsSent` 是否等于 `Requested`。LT 那次是 1/0，推断是因为重置发生在退出时；换图场景没跑过。
- 每次换图的会话代号加几（推断 +2，同进程内）。
- 单位投射体句柄为 0 的比例。日志只记每类前 6 个。
- `_runtime_status_device_lost`（`war3_autotest_mcp.py:7823`）读哪个字段，代理模式的 -4 能不能被它认出来。
- `_launch_suite_map_until_ready` 会不会把环境覆盖传到游戏进程。
- `war3_autotest_mcp` 起游戏用的桌面，是否满足前端的"非输入 WinSta0 桌面"条件。
- 进程内 `weather-status` 的 render/volumetric 在代理模式下是不是真的全 0（由代码推出，没在运行里看）。
- 进程内的租约/TTL 在视野外到底补多少帧影子；两种模式的差是几帧还是一整段。
- 换图门在 1.22.03 就是"带解释通过"（exit 2）。代理模式下用"严格递增"替换"+1"后，进程内那一臂要不要一起放宽，需要计划决定（本笔记建议不动进程内）。
