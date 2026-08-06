# AutoTest 自动化与性能验证套件

本目录包含 War3 自动化测试 / 性能基线 / shadow trace 控制等"发布级"工具。
历史调试脚本（按 phase 分组）已经归档到 `_archive/`，不参与日常验证。

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
Desktop-object 路径已被安全隔离：实测显示栈可能令交互桌面黑屏，同时把 War3 留在非输入
桌面，因此显式请求 `use_isolated_desktop=true` 会在启动前失败。使用可见桌面或 attach-only；
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
