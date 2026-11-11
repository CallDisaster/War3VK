# AutoTest 自动化与性能验证套件

本目录包含 War3 自动化测试 / 性能基线 / shadow trace 控制等"发布级"工具。
历史调试脚本（按 phase 分组）已经归档到 `_archive/`，不参与日常验证。

## 评审后的 VA / flight 等待测量（2026-10-29）

当前先做 T1/T2，兼容面扩展暂停，见
[测量与未覆盖](../docs/research/2026-10-29-x64-review-t1-t2-baseline.md)。
`run_native_d3d9_survey.py --memory-review --normal-exit-test` 在 ready 后等待12秒，
使用保留只读进程句柄采完整 x86 VA 表；不暂停/读取目标载荷，不由 MEM_PRIVATE 猜owner。
`--cpu-recorder-review` 只在该模式中授权固定容量CPU事件环的 arm/freeze/discard，
不启用raw inputs/图像历史。无census的构建可通过独立metadata/exit门，census仍为未覆盖。
完整区域输出保存在`full-va-map.json`，终端只读摘要，保留非原子/partial状态。

`run_d3d9_draw_batch_gate.py --wait-profile --event-completion --gpu-groups --draws 1025`
启用开发宿主有界分段计时；可用`--reference-dll`指定同轮本地参照，另跑不带
`--wait-profile`的成本对照与`--validation`的正确性门。测量对象是EVENT区间的CPU墙钟，
不是纯GPU执行或游戏FPS。`analyze_d3d9_reuse_wait.py`拒绝缺区间/丢样/错序与不闭合时间。
新增纯测试为`test_native_game_memory_review.py`、`test_d3d9_reuse_wait.py`；调查器
权限/分类行为纳入已有`test_native_d3d9_survey_static.py`。真实回执与纯测试分列。

## x64 基础兼容接线门（2026-10-29）

`run_d3d9_factory_clear_gate.py --build-receipt <fresh-host-receipt> --output <new-artifact-dir>`
检查预设备 Factory 查询、局部 Clear、紧凑 FVF、FF/SM3 混用、材质/方向光、变换和
POSITIONT、多 mip/局部上传、格式与表面寿命；本地前后与两种远端配置做整图比较。可用 `--validation` 挂载固定身份
VVL365，或 `--reference-dll <fresh-x86-product>` 验证共用生产代码重建后的本地路径。
所有运行使用 restricted-Medium 非输入 Desktop、精确 Job、原始证据保全。
`--optimized-pipelines` 强制两端关闭 GPL，并校验实际日志中的有效配置；
验证层配方不能覆盖这个配置。后续 16 格式/表面与 sampler 两模式回执为
`x64-sampler-gpu-20261029-r3/r4`，每端 225 图 / 3,686,400 通道，48 个真实 IPC
反例覆盖故障不发布以及后续合法恢复。Alpha/fog/scissor/separate blend 随后的
`x64-effects-gpu-20261029-r3/r4` 默认/优化门已通过，各端 269 图 / 4,407,296 通道
完全相等、60 个实际 IPC 反例与恢复；`x64-effects-lifetime-20261029-r1` 96 轮通过。
八纹理 stage / 独立 sampler 的 `x64-multitexture-gpu-20261029-r3/r4` 随后通过，
每端 286 图 / 4,685,824 通道相等、72 个 IPC 反例/恢复；shader token 两宽各 6,560，
wire 两宽各 1,454,388。`x64-multitexture-lifetime-20261029-r1` 的 96 轮亦通过；
不能由这些旧回执追认。

StateBlock / null-texture programmable draw 的 `x64-stateblock-gpu-20261029-r5/r6`
两模式门随后通过：每端 297 图 / 4,866,048 通道相等，新增 StateBlock native 246 /
remote 247 检查与 11 张图；raw 72 个拒绝/恢复不变。两位宽 wire 各 1,454,644，
覆盖所有空引用组合的选择性允许与拒绝时不发布。完整范围、红门和候选身份见
[StateBlock 回执](../docs/research/2026-10-29-x64-stateblock-integration.md)。
该门仍不是 Warcraft 完整接入或新的内存/性能测量。
同身份 `x64-stateblock-lifetime-20261029-r1` 的 96 轮 / 576 资源 / 256 Query 及原 raw
故障恢复亦通过；公开引用归零与最终 host 物理资源归零在报告中分别说明。

该门明确 `memoryMeasured=false`、`fullRendererMigrated=false`。按用户要求，
Storm 与游戏进程内存对照必须等待完整游戏接入，不可用本门或旧 TLS 减量替代。
FF 当前为同步事务；grouped 配置只验证与既有批次有序共存，不证明 FF 成本已优化。
结果与失败原因见 [接线回执](../docs/research/2026-10-29-x64-fixed-function-integration.md)。

## x64 D3D9 开发代理：批次、在途资源与完成通知（2026-10-28）

这组测试为受限 64×64 COM → x64 原 DXSO/DXVK 路径，**不是 Warcraft 全画面迁移或发布门**。
保留 Game.dll 拒绝与玩家 DLL；所有原生测试由唯一 owner 在 restricted-Medium、非输入 Desktop
运行，使用独立 Job、精确 PID/创建时间及原始输出回执。不能把这些测试 DLL 复制到玩家目录。

可复现入口（从仓库根目录执行，每次输出目录必须全新）：

```powershell
python -B AutoTest/run_d3d9_connection_async_cpu.py --output AutoTest/artifacts/<fresh-cpu>
python -B AutoTest/run_d3d9_connection_fault_ipc.py --output AutoTest/artifacts/<fresh-fault> --event-wait
python -B AutoTest/run_d3d9_draw_batch_gate.py --build-receipt AutoTest/artifacts/<frozen-host-build>/receipt.json --output AutoTest/artifacts/<fresh-gpu> --event-completion
python -B AutoTest/run_d3d9_draw_batch_gate.py --build-receipt AutoTest/artifacts/<frozen-host-build>/receipt.json --output AutoTest/artifacts/<fresh-stress> --event-completion --draws 1025
python -B AutoTest/run_d3d9_draw_batch_gate.py --build-receipt AutoTest/artifacts/<frozen-host-build>/receipt.json --output AutoTest/artifacts/<fresh-vvl> --event-completion --draws 1025 --validation
```

CPU 测试直接调用生产 Connection/队列边界；fault 使用真实双进程管道与恶意协议 peer，
不证明 GPU。GPU gate 使用公共 COM 调用、原 shader 和独立像素判据，计时到 EVENT 完成，
并检查批次整组拒绝/恢复、常量跨批寿命、纹理更新、在途 Destroy、代际复用和 Query 换代。
VVL 使用项目已有的固定 1.4.365 层与原始回调，不能拿该模式的时间作性能结论。
后续对照在上述 `--event-completion` 命令追加 `--gpu-groups`，比较四消息合并提交；
再追加 `--state-reuse` 可测默认关闭的批内状态省略实验（尚无稳定性能收益）。
新增真实非索引绘制、相邻深度/混合/写掩码变化、12 个非索引坏批次及恢复判据。
Clear 前必须恢复整面 viewport；当前宿主不覆盖子视口/矩形 Clear。
CPU 时间/周期来自保留的精确进程句柄，范围是含预热和读回的 44 帧，不是单帧 GPU 时间；
短窗口 CPU 时间可能量化为零，不能据此宣称零开销。详见
[本次合并与覆盖回执](../docs/research/2026-10-28-x64-submit-groups-and-nonindexed.md)。
构建 owner 按 BelowNormal / `-j2`，fresh host 来自 `run_render_host_candidate_build.py`。
严格 reader 为 `test_d3d9_draw_batch_reader.py`、`test_d3d9_connection_async_cpu_reader.py`
和 `test_d3d9_connection_fault_ipc_reader.py`。完整版本身份与未覆盖项见
[本轮证据](../docs/research/2026-10-28-x64-draw-batching-and-cost.md)。

## 发布级脚本（合并主线必用）

| 文件 | 作用 |
|---|---|
| `war3_autotest_mcp.py` | MCP 服务核心：启动/进图/采样/截图/读取报告等一体化 API。所有自动化流程的底层依赖。 |
| `autotest_sessions.py` | 多实例纯 Python 基础层：沙盒边界、内容寻址地图、实例布局、会话注册表和 DBWIN PID 分流。 |
| `test_autotest_sessions.py` | 多实例路径、硬链接/复制策略、事件隔离和孤儿回收单元测试。 |
| `ydwe_instance_launcher.py` | YDWE per-instance Win32 helper 的 fail-closed 审计请求层；当前永远不执行。 |
| `ydwe_job_owner.py` | 尚未接线的 Job owner capability；HANDLE 不序列化，失败清理可重试。 |
| `ydwe_runtime_catalog.py` | 晚加载运行时的只读 catalog 审计；不持句柄、不授权启动。 |
| `run_mcp.ps1` | 启动 MCP 服务（PowerShell 入口）。 |
| `dual_perf_baseline.py` | 双图性能基线脚本：高压（光影测试-高压.w3x）+ 低压（光影测试.w3x），各 30s × 多轮，输出 FPS / mainThread / GPU 等核心指标。**合并前性能护栏验收必用**。 |
| `run_light_feature_matrix.py` | 点光/点阴影/体积光特性矩阵：先 dual_perf 护栏，再低压地图 5 组 env 配置（baseline / volumetric / point / point+shadow / vol+point），输出 JSON 与 `latest_light_baseline.json`。 |
| `shadow_pose_full_trace_control.py` | 控制 plane 动态启停 shadow pose full trace（写盘 JSONL）。问题诊断时手动开 15-30s trace。 |
| `requirements.txt` | Python 依赖清单。 |
| `README.txt` | 历史 README（YDWE 启动链路 / DBWIN 监听等说明）。 |
| `ydwe_launch_notes.txt` | YDWE -loadfile 启动链路逆向笔记。 |

## 性能护栏（合并前必跑）

```pwsh
# 在前台环境运行（不要在 isolated desktop 里跑性能基线，否则受 GPU present 阻塞污染）
py AutoTest\dual_perf_baseline.py

# 点光 / 体积光特性矩阵（同样前台）
py AutoTest\run_light_feature_matrix.py
```

护栏标准：
- 高压地图（带桥/斜坡/装饰物）≥ 85 FPS
- 低压地图（光影测试.w3x）≥ 120 FPS
- 特性矩阵相对 baseline：volumetric ≥0.92×、point ≥0.95×、point+shadow ≥0.85×、vol+point ≥0.90×

如果某轮低于护栏，先在前台 / clean machine state 复测一次再判定。
后台 Defender 扫描、热节流、其他游戏运行 都会污染 isolated desktop 的基线。

## 非交互隔离桌面安全边界

隔离桌面只用于后台正确性、稳定性和运行时取证，不用于发布 FPS。它通过
`CreateDesktopW` 与 `CreateProcessW/STARTUPINFO.lpDesktop` 创建进程，但 AutoTest
不申请桌面切换权限，也不调用桌面切换 API。

隔离会话禁止键盘、鼠标、前台窗口和输入计划。场景控制只能使用 named pipe/JASS
内部测试命令，截图只能使用内部 framebuffer 或隔离窗口的 `PrintWindow` 路径。
若地图必须通过点击或按键才能进入，测试会 fail-closed；不会切换用户桌面，也不会
自动回退到可见桌面。默认桌面上的显式人工测试仍可使用现有输入工具。

## 受限 D3D9 / MDLDATA 开发诊断

`run_native_d3d9_survey.py` 是真实游戏**观察**入口，不是发布门或x64游戏渲染器。
它只写全新独立实例，绑定PID+creation time、精确模块/候选身份与受限Medium非输入Desktop；
`--observe-only` 的期限清场不等于正常退出。没有成功打开控制管道时，不宣称地图ready已检查。

MDLDATA探针另需开发构建 `warvk_mdl_allocation_trace_dev=true` 和运行参数
`--mdl-allocation-trace`；默认关闭。不修改Game分支、LAA或分配策略。
`run_mdl_trace_gate.py --build <独立build32_device_owner_目录> --output <全新artifacts子目录>`
执行实际Core/ABI与C++日志→生产reader往返。`analyze_mdl_link_trace.py` 仅供准确所属进程/Job
结束后读文件；`--process-exited` 是调用者的断言，不会自行证明进程已退出。
高位字段可能是合法sentinel；高返回/函数返回也不自动认证存活、分配器路由或结构正确。

当前实现与失败边界见
[MDLDATA checkpoint](../docs/research/2026-10-23-mdldata-allocation-probe.md)。
这些定向门不能替代全库回归、GPU/玩家视觉、前台性能和正常退出验收。

## 多实例稳定性测试

多实例接口强制使用 `E:\Work\War3_AutoTestSandbox`。先调用
`preflight_instance_pool`；它只读检查 1-6 个实例，不会创建目录或启动游戏。
通过后使用 `launch_war3_instance` 或 `launch_war3_batch`。每个会话拥有独立的
`session_id`、游戏根、内容哈希地图、工件目录、Win32 Desktop 和
`KILL_ON_JOB_CLOSE` Job Object。

运行态通过 `list_war3_sessions` 查询；`get_runtime_events(session_id=...)` 会从
系统级 DBWIN 监听器中只返回目标 PID 的事件，`read_runtime_status`、
`wait_for_runtime_status` 和 `wait_for_game_ready` 同样接受 `session_id`。停止和清理由
`stop_war3_batch`、`cleanup_orphan_sessions` 完成，二者不会在未指定
`session_ids_json`/`run_id` 时批量结束进程。

`launch_war3_batch` 启动的是若干彼此隔离的 `-loadfile` 单机进程，只能验收进程、
Desktop、日志和工件隔离，不能作为联机同步通过的证据。联机入口是
`run_multi_instance_suite`；它强制要求专用沙盒里存在经过审计的 `ydhost` 适配器。
先调用 `provision_ydhost_assets`（缺省 `apply=false`）查看配置计划；只有显式
`apply=true` 才会把哈希锁定的 `ydhost.exe`、`msvcp140.dll`、`vcruntime140.dll`
写入两个允许的沙盒目标之一，并生成 `provision-manifest.json`。已存在文件或清单
只要发生漂移就拒绝覆盖。`map.cfg` 和 `ydhost.cfg` 不从 YDWE 示例目录复制：前者必须
由已审计的 YDWE `mapdump` 为当前地图生成，后者必须绑定本次地图路径和客户端数。

`run_multi_instance_suite` 会依次核验 provision 清单、三项运行时 SHA、实际 MPQ 地图和
与地图 SHA-256 绑定的 mapdump 清单。任一条件缺失即返回精确 blocker；即使门禁全部
通过，真实启动仍默认关闭并返回 `YDHOST_LAUNCH_OPT_IN_REQUIRED`。显式传入
`enable_ydhost_launch=true` 后，当前还会返回
`YDHOST_CLIENT_LAUNCH_PROTOCOL_UNRESOLVED`：官方证据使用
`YDWE.exe -war3 -auto`，而现有实例启动器固定为 `war3.exe -loadfile`，两者尚未证明
等价。进一步的源码审计已经确认：原版 YDWE 通过当前用户/机器共享的 Warcraft III
`InstallPath` 选择 War3 根，并没有已证明的 per-process 根参数；隔离 Desktop 不隔离
注册表，因此不能靠并发修改注册表绑定实例根。独立 Win32 helper 已能显式描述每实例根，
但默认编译门关闭，Python 也固定 `launchable=false`；它仍缺 OwnedJob/SessionRegistry
接线、绝对路径依赖核验和 ready 前运行时闭包，详见 `YDWE_INSTANCE_LAUNCHER.md` 与
`Tools/YdweInstanceLauncher/PRODUCTION_READINESS_PLAN.md`。`-auto` 由注入后的
`yd_loader.dll` 解释，且其 `connect` hook 只证明 JOIN 请求，不是 in-game ready。
机器可读身份/命令契约位于 `ydwe_lan_protocol.py`，完整证据与下一步见
`YDWE_LAN_PROTOCOL.md`。不会静默降级为伪联机测试。

地图元数据由 `generate_ydhost_map_metadata` 生成。它校验包含 YDWE Lua、StormLib、
maphash、w3xparser、w3x2lni 全部脚本/数据、共享游戏数据及 runtime 24 JASS 的
434 文件封闭 catalog；随后逐文件复制并复核到沙盒隔离快照，紧贴启动前再次计算
catalog SHA，执行阶段不再从活 YDWE 树加载代码。缺省 `apply=false` 时，`map.cfg` 仅
存在于自动清理的沙盒临时目录；显式 apply 才会原子写入
`.ydhost-metadata/<map-sha256>/`。目标存在且内容不一致时拒绝覆盖。
mapdump Lua 以 `-E` 启动，并使用不继承父进程 `LUA_INIT/LUA_PATH/LUA_CPATH` 的最小
环境；Windows/System32 由 Win32 API 获取并拒绝 reparse，工作目录固定为已哈希快照的
`bin`，避免本机用户配置、父环境或可写 cwd 绕过封闭 catalog。

所有 provision/metadata 写路径同时使用 lexical containment 与逐组件
reparse-point/junction 拒绝；每次原子替换前都会复查，不能通过中间 junction 逃逸
专用沙盒。LAN 状态机本身已用依赖注入 fake 覆盖“host listen → clients start →
逐客户端 joined/ready 证据 → running”、超时全量清理、部分启动失败清理、Job/Desktop
唯一性以及停止单个客户端不影响兄弟客户端/host。准备阶段还会严格校验 run/session 标识符，
逐组件检查 run_root、cfg 与 artifact 路径，并重新计算 cfg/map SHA；清理失败会保留运行记录供
后续重试。依赖注入模拟永远不计入 LAN 验收。真实进程启动器仍按上述协议缺口关闭。

生产绑定还保留三项强制 blocker：mapdump 的 434 个文件需在最终 hash 到 CreateProcess 之间
持有 deny-write 句柄；启动中途失败且清理失败时需登记 FAILED handles；War3/ydhost 启动必须
使用 retained process handle 与 `CREATE_SUSPENDED → AssignJob → Resume`，并补 run/session 锁、
identity fail-closed、固定 System32 工具和 timeout。完成前不得签收真实 2/6 客户端 LAN。

长时间正常波次稳定性测试可在启动参数 `env_overrides_json` 中显式传入
`{"FLK_AUTOTEST_FAILURE_GUARD":"1","FLK_HANDLE_DIAGNOSTICS":"1"}`。前者只让地图的
测试桥给三座基地临时无敌并屏蔽失败动作，后者启用句柄代际诊断；两者都不得写成机器级
持久环境变量，生产启动默认关闭。

## 单实例 YDWE / JAPI 兼容启动

历史地图依赖 YDWE JAPI 时，`launch_war3_test` 与 `run_quick_autotest` 必须显式传入：

```text
launcher_mode="ydwe"
ydwe_root="E:\\Work\\War3\\YDWE1.32.13 - MemoryHack"
war3_dir="E:\\Work\\War3_AutoTestSandbox"
use_isolated_desktop=false
```

该模式启动命令为
`YDWE.exe -war3 -loadfile Maps\Test\WorldEditTestMap.w3x -closew2l`。当前机器的 Win32
Desktop-object 路径曾因黑屏风险被禁用；2026-08-11 的 `b0123bac` 已改为前述
non-interactive-only 合同（不申请 SWITCHDESKTOP，检查输入桌面前后身份）。旧文档的
“显式请求必定启动前失败”不是当前实现。隔离仍须按次验证实际线程 Desktop 身份，不能只查
session 元数据；未通过不得回退到默认/可见桌面。共享显示驱动的黑屏/TDR 风险也不因隔离消失。
2026-10-22 x64 验证先用独立非 GPU 探针，再单独验最小图形路径；隐藏 HWND 本身不是桌面隔离。
`-closew2l` 禁止启动时再次转换候选图。工具先把候选图原子部署到
专用沙盒短路径并复核源/目标 SHA-256；目标被占用或哈希不一致时直接失败，绝不复用旧的
`WorldEditTestMap.w3x`。

启动前还会只读校验 HKCU 的 Warcraft III `InstallPath` 必须精确指向
`E:\Work\War3_AutoTestSandbox`。工具不会修改这个注册表值，也不会创建/复制新的 War3
沙盒。YDWE wrapper 启动后，AutoTest 只接受其进程树中新建、镜像路径精确匹配沙盒
`war3.exe` 的 child PID；DBWIN、`wait_for_game_ready`、截图与 `stop_war3` 全部绑定这个
游戏 PID，不绑定短命的 YDWE wrapper PID。

为证明 JAPI 实际进入目标进程，ready 前还会核验已加载的 `LuaEngine.dll` 与
`yd_jass_api.dll`，要求模块路径和启动前 SHA-256 同时匹配。若用户正在运行同一 YDWE 的
`worldeditydwe.exe`、`YDWEConfig.exe` 或 `YDWE.exe`，共享 wrapper 可能受 mutex 影响而不
产出 War3；此时返回 `USER_YDWE_PROCESS_CONFLICT`，不会关闭或干预用户进程。需要先由用户
自行关闭编辑器再重试；当前工具不会静默降级为不带 JAPI 的 `war3.exe -loadfile`。

YDWE 1.32.13 的 LuaEngine 使用固定的 `logs\war3.log`，并且在 DLL 初始化阶段直接打开该
文件。日志不可写或被另一个会话占用时，LuaEngine 会抛出 C++ 异常，最终表现为 War3
`0xC0000409`，此时地图和 JAPI 尚未开始初始化。为防止把该错误误报成地图崩溃，YDWE 模式
现在会在 CreateProcess 前验证 `war3.log` 可写，并按规范化 YDWE 根获取跨进程互斥锁；锁由
启动一直持有到 `stop_war3` 完成。同一 YDWE 根不能并发运行多个单实例验收。若以后需要并发
JAPI 客户端，必须为每个会话提供独立的 YDWE 可写 `logs` 根，不能共享中央日志目录。

`launcher_mode="direct"` 仍是默认值，保持既有渲染/性能回归行为。以上单实例入口不改变
`run_multi_instance_suite` 的 LAN/ydhost 生产 blocker，也不能作为联机验收。

## 归档目录（`_archive/`）

| 子目录 | 内容 |
|---|---|
| `phase_7xx_history/` | Phase 7.30-7.116 各轮一次性诊断 / 验证脚本（约 100+ 个）。 |
| `analysis/` | trace JSONL 分析脚本（_analyze_*）。 |
| `ida_scripts/` | IDA MCP 调用脚本（_ida_*）。已经回写完命名 / 注释，仅作历史参考。 |
| `probes/` | 一次性探针脚本（_probe_*, _check_*, _find_* 等）。 |
| `logs/` | 历史 perf 报告 / 调试 log。 |

归档原则：
- **不删除**：所有调试历史完整保留，便于 root-cause review。
- **从 PATH 移除**：不再出现在 `AutoTest/` 主目录，避免给新人造成"哪个是当前用的"困惑。
- 任何归档脚本若需要重新启用，直接从 `_archive/` 拉回来即可。

## 本目录文件命名约定

新增脚本规则（写在这里防止后人继续扔垃圾）：

- `_phaseXXX_*.py`：临时调试，写完即用，**不要进 release**。一旦完成 phase 就归档到 `_archive/phase_7xx_history/`。
- 发布级脚本：必须用语义命名（`dual_perf_baseline.py`、`shadow_pose_full_trace_control.py` 等），名字直接说明"做什么"，不带 phase 编号。
- 一次性 IDA 调用：`_ida_*.py` 写完后立即移到 `_archive/ida_scripts/`。

合并前自检：
```pwsh
# AutoTest 主目录里不应该有任何 _phaseXXX_* 文件
Get-ChildItem AutoTest -File -Filter "_phase*"
```
