# 实机门跑代理模式：实现回执（2026-11-17）

依据：`2026-11-17-x64-proxy-real-game-gates.md`（研究笔记，下称"笔记"）。本文记录 gates 这一路改了什么、跑了什么、结果如何。

标注说明：
- **已核对**：本次读代码或实际跑出来看到的。
- **推断**：没有直接证据。

玩家目录的 `E:/Work/Warcraft III/d3d9.dll` 全程不存在，也没有被创建。每个门前后都查过一次，见各门产物里的 `playerDllBefore/After`、`player_dll_absent=True`（已核对）。

用到的二进制（已核对，sha256）：

| 件 | 哈希 | 用在哪 |
|---|---|---|
| 产品 x86 `d3d9.dll`（`gates_bin`） | `B8BB8C98B84AE1B97490A0ABE9BFCD385F0EF572D1AEA0784CC71E5C9DC3BD72` | 五个门、影子扫描、换图探针、描边 r1 |
| 同上，`outline.pick` 打开全对象跟踪（`gates_bin2`） | `ACEFE8B17B86D5873DE345B3ACF2BEF52E1F00FA6C2171F069FADDE9B57B70AA` | 描边 r2 |
| 同上，加描边"句柄段"挑选（`gates_bin3`） | `4A5146B0CF05C85EF02CFA12BA7243A79203AADE8CBBB5B7E0210F270D58C62F` | 描边 r3 |
| 代理前端 `front.dll` | `142EE6D3D62A1E7FFF6340192204D973CBBBCB51FFCDB1EC3CDCB63434DAF850` | 所有代理臂 |
| 冻结宿主 `warvk-d3d9-host-dev.exe`（`build64_render_host_phase52_20261117_a`） | `802220DD5AA9EECA93E3C86E373EB68F21E12DE98824D118DB12058FE7E563BB` | 所有代理臂 |

两个 DLL 都含 overlay agent 的代码：链接顺序问题按 lead 的说法重跑了一次 ninja，属预期。

## 0 结论

| 门 / 探针 | 进程内 | 代理 |
|---|---|---|
| 启动诊断 4 场景 | **4/4 过**（已核对） | **4 × 不适用**：fail-* 要 N6，ok-* 要 N6b。照跑，证据留下（已核对） |
| 天气 | **过**（r3）：23/26，和 11-12 发版终跑同样只有三项"不作为验收"的为 false（已核对） | **过**（r3）：测得的 10 项、代理验收 11 项全 true，14 项不适用。r1 的宿主 rejected 2 是改窗口尺寸时设计内的跳过 Present，判据已改（§3.3，已核对） |
| 换图 A→B→A | **exit 2**：`resetSerialAdvanced`、`runtimeStateGate` 为 false。和 11-11、11-12 两次发版终跑完全同样（旧问题，见 §2），换图本身（同进程、mapEpoch 1→2→3、三张图）是好的（已核对） | r1：传输、会话重置、世界帧全过，只有两项判据写错，已改（§3.2）；**r2 全过**，`transitionCanaryAccepted=true`（已核对） |
| Walk | **过**（已核对） | **过**：Walk 断言 + 代理验收全 true，世界帧 1→611（已核对） |
| 压力 生与死 | **过**，183 s（已核对） | r1 有两项采样漏了（已改）；**r2 全过**，188 s，世界帧 11→2560（已核对） |
| 探针：描边 | 按句柄登记后画出描边（1334 像素），清掉后归零（已核对） | **能画**：同样的句柄登记后画出描边（8962 像素），清掉后归零。但描到的对象比进程内多，IoU 0.07；描到的是树不是单位（§4.1） |
| 探针：视野外影子（相机扫描） | — | **不比进程内少**：22 步 × 4 个量，代理/进程内比值都在 1.02–2.01（已核对） |
| 探针：`--extra-map` 换图 | A→B→A 走完 | **A→B→A 走完**，会话代号 0→2→4，宿主收到（r2、r3，已核对）。r1 卡在 B 的载入一次；r3 已排除暂停覆盖这个原因，剩下的原因未定（§4.3） |

## 1 改了什么

### 1.1 公共部分

| 文件 | 改动 |
|---|---|
| `AutoTest/x64_proxy_gate_mode.py`（新） | 所有门共用的代理模式支持，详见下方列表 |
| `AutoTest/war3_autotest_mcp.py` | N8 和自然退出，详见下方列表 |
| `src/d3d9/war3/platform/war3_proxy_casters.{h,cpp}` | N7b：每帧计数 `considered/sent/terrain/doodads/worldObjects/units/withHandle/unitsWithHandle/outlineMatched`，累计和"上一帧"各一份 |
| `src/d3d9/war3/platform/war3_proxy_frame.{h,cpp}`、`war3_proxy_status.h`（新） | N1 和 N2，详见下方列表 |
| `src/d3d9/war3/tools/war3_control_plane.cpp`、`war3_diagnostics_hub.cpp` | 代理模式下，`get_runtime_status` 和状态文件多一个 `proxyMode` 段。进程内 `StatusJson()` 返回 null，不加任何字段 |
| `src/d3d9/war3/tools/war3_internal_test_api.cpp` | N7a 描边命令，详见下方列表 |
| `AutoTest/run_x64_game_proxy_probe.py` | 探针动作和参数扩展，详见下方列表 |
| `AutoTest/run_x64_proxy_t4_probes.py`（新） | 三个探针的动作表和分析：描边黄像素掩码和 IoU、影子扫描曲线、换图采样 |

**`x64_proxy_gate_mode.py`**
- `--mode inprocess|proxy`，默认 inprocess。
- `--front`；`--host` 默认是冻结宿主。
- `require_player_dll_absent()`：前后各查一次，存在就抛错，从不创建。
- `ProxyArm`：
  - 把前端和宿主复制进本次产物目录，并核对哈希；
  - 给出代理模式的环境变量：`WARVK_D3D9_FRONT/HOST_EXE/HOST_LOG/GAME=sandbox-isolated-v1/PROFILE=presenter-isolated-v1/ANNOTATIONS=1`；
  - 产品传输档：STREAM + FIXED_BATCH + COMMAND_RING + `X86_BACKING_KIB=32768` + `UPLOAD_WINDOW_KIB=1024`；
  - `DXVK_WAR3_INTERNAL_EXIT_TEST=1`。
- `collect_evidence()` 读这几样：
  - `game.stderr` 里的前端账；
  - `host.log*` 里的宿主账和 exit-intent；
  - 数据层日志里的 `session reset generation=` 等行；
  - host-lost 报告。
- `proxy_acceptance()` 判这些：
  - stdio 接到了；
  - 游戏自己退出；
  - 终端干净；
  - 宿主 normal-return；
  - 宿主 rejected 0。窗口改尺寸时被跳过的 Present 除外：它们按设计返回 `d9::PresentSkipped`，宿主却把它记进 rejected。只扣掉 `reset-host.presentsSkipped` 这么多，其余任何一次 rejected 都算失败（§3.3）；
  - insertions/casters/sessionResets 都是 refused 0；
  - x86 进程没加载 Vulkan；
  - 没有 host-lost 报告；
  - 世界帧有推进。
- `world_progress()`：**按证据判断是否在玩**，回应 lead 的"投递不等于消费"。要求同时满足：
  - 至少 3 个样本；
  - 数据层 `worldScenes` 和 `insertions` 首尾都涨；
  - 最后三分之一的样本里 `worldScenes` 还在涨。

  卡在没有世界画面的屏幕上（例如"按任意键继续"），在这里会失败（已核对，单测）。

**`war3_autotest_mcp.py`**
- N8：`game_stdio_capture(dir)` 上下文。在它里面起的游戏用 `STARTUPINFOEX` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`，只继承三个句柄：`game.stdout`、`game.stderr`、`NUL`。不在上下文里时走原来的 `CreateProcessW`，字节不变。
- 自然退出 `natural_exit_owned_game(pid, ...)`，按顺序：
  1. 调 `game.end_for_exit_test`；
  2. 等到连续两次 `isInGame=false`；
  3. 在游戏自己的桌面上（工作线程 `SetThreadDesktop`），给唯一的 "Warcraft III" 窗口 `PostMessage(WM_CLOSE)`；
  4. 等进程退出。

  前端的账只在设备释放时打印，强杀就拿不到账，所以代理臂必须走这条路。
- `run_life_and_death_tdr_scenario` 加了 `natural_exit_before_stop`，默认 False，进程内行为不变。
- `_compact_life_and_death_status` 在原状态里有 `proxyMode` 时，才保留 `{mode, deviceConnected, deviceLost, terminalKind, presents, frame}`。进程内的压缩结果不变（已核对：进程内状态没有这个段）。

**`war3_proxy_frame` / `war3_proxy_status.h`**
- N1：`proxy::StatusJson()`，schema 为 `warvk-proxy-mode-status/v1`。内容有：mode、前端和设备、终端状态、draws/presents/尺寸、`frame{frames, worldScenes, insertions, sessionGeneration, refused…}`、`casters{…, lastFrame{…}}`、`outline{handleCount}`。
- N2：代理模式连上之后，每隔至少 1 s 写一次 `runtime_status.json`，来源为 `ExportRuntimeStatusSnapshot("proxy-periodic")`。
- 每 900 帧的那行日志加了句柄计数。

**`war3_internal_test_api.cpp`**（N7a，用法见 §4.1）
- `outline.pick {kind, index, handle, rawcodes[], showVisible, showOccluded, widthPx}`。
- 句柄段形式 `outline.pick {rangeLo, rangeCount, …}`：登记 `rangeLo .. rangeLo+rangeCount-1`，最多 1024 个，对应 `OutlineHandlesMax`。
- `outline.clear`：清掉句柄，恢复设置和"全对象跟踪"开关。
- `outline.status`。

**`run_x64_game_proxy_probe.py`**
- 动作 `test=命令:k=v;...`：带负载的测试命令。
- 动作 `status=名字`：存成 `status-名字.json`。
- 动作 `camrel=dx;dy`：相对开局相机做 `camera.apply`。
- 白名单加了 `camera.apply/fixed/world_bounds`、`shadow.lock_sun`、`outline.*`。
- 新参数：`--actions-file`、`--extra-map SRC=REL`。

### 1.2 逐门

**进程内臂**：每个门只改了"玩家 `d3d9.dll`"这一项，把哈希钉换成"前后都不存在"。代理分支一律用 `PROXY_ARM is None` / `arm is None` 隔开（§2）。

| 门 | 入口 | 进程内额外改动 |
|---|---|---|
| Walk | `run_v12202_final_gate.py walk` → `run_unit_walk_probe.py` | 无 |
| 换图 A→B→A | `run_v12202_final_gate.py crossmap` → `run_cross_map_ab_a_canary_20260925.py` | 无 |
| 压力 生与死 | `run_v12202_final_gate.py pressure` | 无 |
| 天气 | `run_weather_canary_20261010.py --run` | 保护列表、`--artifact-root`；断言不变 |
| 启动诊断 4 场景 | `run_startup_diag_gate.py`（新，从 scratchpad 补回仓库） | 按笔记 §1.2 的判据判 |

**代理臂**

- **Walk**：
  - 同一套 Walk 断言；
  - 每轮控制面采样 `proxyMode`；
  - 自然退出；
  - `proxy_acceptance`，含世界帧。
- **换图 A→B→A**：
  - 状态文件不新鲜时，回退到控制面；
  - 等待上限 300 s（进程内仍是 125 s）；
  - `proxy_verdict`：
    - 同进程；
    - 分辨率；
    - **每段进入时的**会话代号严格递增；
    - 数据层 reset 行 ≥2；
    - `sessionResetsReachedHost`：前端 refused 0、sent ≥1，并且宿主 `annotation-host` 最后的 `sessionGeneration` ≥"之后还有世界帧的最大代号"；
    - runtimeStateGate：帧和 insertions 推进、castersSent>0、refused 0；
    - 三张图：当前代号等于本段进入代号，且大于前面各段的进入代号；本段 insertions 比进段时多 30；进段 +5 s。
  - retirementCoverage、mapEpochExact、queueSerials 记不适用，要 N3。
- **压力 生与死**：
  - 路线本身的结果；
  - 控制面 `proxyMode` 采样；
  - 世界帧，含后三分之一；
  - 无 deviceLost 样本；
  - `proxy_acceptance`。
  - 状态文件新鲜度、gpu_incident 文件记不适用：前者进程内也不判，后者由进程内渲染器写，代理的对应物是 host-lost 报告和宿主退出账。
- **天气**：
  - 能测的 10 项照常判：JASS 各阶段、JASS 检查、两种时间查询、暂停时钟和关闭时钟、销毁、截图尺寸、天气图和基线不同、改尺寸到达前端；
  - 14 项 `frozenInput/render/volumetric` 类记不适用，要 N5/N4；
  - 改窗口尺寸看前端 `window-proxy` 的 `resizes≥1 refused 0`；
  - `proxy_acceptance`。
- **启动诊断**：4 个场景都记不适用。
  - fail-quiet、fail-dialog 要 N6：代理启动失败时不调 `ReportStartupFailure`。
  - ok-auto、ok-low 要 N6b：代理模式不套显存档位，没有 `WarVK GPU:` 行。
  - 跑还是照跑，留证据。

## 2 进程内臂没变的证据

**代码**（已核对，`git diff --ignore-cr-at-eol`）。四个门脚本删掉的行只有三类：
- 玩家 DLL 哈希钉：`PLAYER_DLL_SHA256`、`PINNED[player_dll]`、`player DLL changed` 两处检查；
- 文档串；
- 被搬进 `if PROXY_ARM is None` / `if arm is None` 分支、原样保留的启动和判定代码，包括 `launch_war3_instance(...)`、`deadline = began + 125.0`、截图条件、`ready_to_capture`。

进程内能走到的判定表达式没有一行改动。

**静态测试**（已核对）：用 `run_static_tests.py` 跑了读到这些文件的模块：`war3_autotest_mcp`、`run_cross_map_ab_a_canary`、`x64_proxy_gate_mode`、`war3_internal_test_api`、`run_x64_proxy_t4_probes`、`run_v12202_final_gate`、`run_weather_canary_20261010`、`run_startup_diag_gate`。
- 结果：24 个模块，332 个测试，只有 `test_war3_autotest_mcp_ydwe` 的 2 条失败。
- 这 2 条在 11-15 的 `static-4c-r1` 和 11-16 的 `static-4c2-compare` 里就是同样的失败 id：一条是临时目录短名 `ADMINI~1` 和长名 `Administrator` 比较不等，另一条是 mock 的 `OpenProcess failed: 87`。跟这次改动无关。
- 静态测试之后又改了 `x64_proxy_gate_mode.py`（`presentsSkipped` 扣除）和 `run_x64_proxy_t4_probes.py`（句柄段起点）。没有测试模块读这两个文件，所以只做了 `py_compile`，并用已有产物离线复算了验收（已核对）。

**实跑**（已核对）：
- 启动诊断 4/4 过。
- Walk 的 `classification` 和 11-12 发版终跑 `v12202-v12203-final-20261112-b-walk` 逐项相同。
- 压力路线过。
- 天气 r3 和 11-12 发版终跑逐项相同（§3.3）。天气 r1 的进程内臂没起来：我中途停掉了排队，重排时 `--prepare` 遇到已存在的目录报错。r2 撞上实例名重复。这两次都是测试脚本的问题，没有进游戏。
- 换图门 exit 2，但和发版终跑一模一样：

| 运行 | 判定 | 各段 (producerReadyTail, uniqueFrameCount, resetRequestedMax, resetAppliedMax) |
|---|---|---|
| `v12202-final-20261111-b-crossmap` | resetSerialAdvanced F，runtimeStateGate F，mapEpochAdvanced T，retirementCoverage T | A (1,4,2,2)　B (1,4,4,4)　A' (2,4,4,4) |
| `v12202-final2-20261111-crossmap` | 同上 | 同上 |
| `v12202-v12203-final-20261112-b-crossmap` | 同上 | 同上 |
| 本次 `x64-proxy-gates-20261117-crossmap-inprocess-r1` | 同上 | A (1,**3**,2,2)　B (1,4,4,4)　A' (2,4,4,4) |

唯一的差别是 A 段少采到 1 个不同帧，属采样抖动。`resetSerialAdvanced` 为 false，是因为 B 和 A' 的 reset 序号都停在 4，不是严格递增；这是这个判据对现有产品的老问题，不是这次改出来的。

## 3 代理臂结果

### 3.1 一览

| 门 | 结果 | 关键数（已核对） | 产物 `AutoTest/artifacts/` |
|---|---|---|---|
| 启动诊断 | 4 × 不适用 | 见下方说明 | `x64-proxy-gates-20261117-startup-proxy-r1` |
| Walk | **过** | 见下方说明 | `…-walk-proxy-r1` |
| 压力 | r1 判据漏采；**r2 过** | 见下方说明 | `…-pressure-proxy-r1`、`…-pressure-proxy-r2` |
| 换图 | r1 判据错两项；**r2 过** | 见 §3.2 | `…-crossmap-proxy-r1`、`…-crossmap-proxy-r2` |
| 天气 | r1 宿主 rejected 判据过严（已改）；r2 没起来（测试脚本的实例名重复）；**r3 过** | 见 §3.3 | `…-weather-proxy-r1`、`…-weather-proxy-r3` |

**启动诊断**
- fail-quiet、fail-dialog：只出现游戏自己的 DirectX 失败框（标题 "Warcraft III"），没有 `startup-error.txt`，也没有 WarVK 对话框。
- ok-auto、ok-low：没有 `WarVK GPU:` 行，自然退出正常。

**Walk**
- Walk 的 5 项和 `pixelOrAnimationVisualProof`，跟进程内逐项相同。
- 世界帧 1→611；后三分之一 418→611；24 个样本。
- 自然退出 4.47 s。
- 宿主 accepted 926114，rejected 0，normalTeardown。

**压力**
- r1：路线 `routeOk` 过，传输各项都过；`proxyStatusSampled` 和 `worldFramesAdvanced` 为 false，原因是 `_compact_life_and_death_status` 把 `proxyMode` 段压掉了，已改。
- r2：16 项全 true。
  - 世界帧 11→2560；后三分之一 1657→2560；337 个样本。
  - 宿主 accepted 5885077，rejected 0。
  - 数据层收尾行 `frames=6839 worldScenes=2589 … refused=0`。
  - 自然退出 4.11 s。

**两种模式下压力路线的共同现象**（已核对，截图）：难度对话框到最后都还开着，世界在后面照常跑、相机在动。进程内也是这样，所以不算代理的问题。

**启动诊断的一个附带观察**（已核对）：ok-auto、ok-low 在"就绪"（`isInGame=true`）那一刻，代理状态是 `frames 106/109, worldScenes 0`，到退出时也只有 `worldScenes=2`。也就是说，"就绪"本身不代表已经在出世界帧。这就是各门要另加 `world_progress()` 的原因。

### 3.2 换图门（代理）

**r1**（已核对）：
- 三段开始时间：1.0 / 26.5 / 51.3 s（进程内 1.0 / 24.6 / 49.1 s）。
- 传输、`sessionResetLogged`、`sessionResetsReachedHost`、runtimeStateGate、世界帧全过。
- 宿主 `annotation-host`：sessionResets 3，generation 6；前端 requested 6、sent 3、refused 0。
- 失败的两项，都是判据写错，不是产品问题：
  1. `sessionGenerationAdvanced`：原先比各段**尾部**代号 [2,4,4]。JASS 段标记晚于会话重置，所以一段的尾巴可能已经属于下一张图。改成比各段**进入**代号，r1 的进入代号是 [0,2,4]，严格递增。
  2. `screenshots`：同一个原因，改成按进入代号判"新鲜"。

**会话重置是合并发送的，不是丢**（已核对，读代码）：
- `d3d9_connection.h:565-576` 写明"后来的代号替换还没发出的那个"；发送发生在下一次 Present 之前（`:700-717`）。
- 所以 `sent < requested` 是设计如此；退出时那次 reset 之后没有 Present，也发不出去。
- 笔记里写的 `sessionResetsSent == Requested` 因此不能当判据，已换成 §1.2 的 `sessionResetsReachedHost`。

**r2：全过**（已核对，`x64-proxy-gates-20261117-crossmap-proxy-r2/v12202map/cross-map-result.json`）
- 22 项检查全 true，`transitionCanaryAccepted=true`，退出码 0。
- 三段开始时间：1.0 / 25.2 / 49.5 s。
- 进入代号 [0,2,4]，严格递增；尾部代号仍是 [2,4,4]，和 r1 一样，说明 r1 不是产品问题。
- 宿主 `annotation-host`：sessionResets 3，sessionGeneration 6；"之后还有世界帧的最大代号"是 4，宿主的 6 ≥ 4。
- 三张图都按进入代号判为新鲜：代号 0 / 2 / 4，insertions 161 / 631 / …。图上地图自己打的字分别是 `WARVK CROSS MAP A FIRST`、`… B`、`… A RETURN`（已核对，联系表 `t4-analysis/crossmap-proxy-r2-sheet.png`）。
- 自然退出正常；宿主 rejected 0（设备宿主在这一轮有 1 次 windowReset，跳过的 Present 为 0）。

### 3.3 天气（两种模式）

**进程内 r3：过**（已核对，`x64-proxy-gates-20261117-weather-inprocess-r3/wi3/run.json`）
- `runtimeAcceptance=true`，26 项里 23 项 true。
- false 的三项是 `captureExactWeatherFrameCertified`、`gpuCompletionCertified`、`pixelOrArtisticAcceptance`，都属于"不作为验收"。
- 和 `weather-night-20261010/v12203-final-20261112`、`v12202-final-20261111-b/-c` 逐项相同。

**代理 r1：exit 2，只有 `hostRejectedZero` 一项失败**（已核对）
- 测得的 10 项全 true，其余代理验收项全 true。
- 设备宿主 `rejected 2`，同一个宿主的 `reset-host` 是 `windowResets 1, presentsSkipped 2`，两个数正好相等。
- 读代码（已核对）：
  - 窗口客户区和后备缓冲不一致时，Present 返回 `d9::PresentSkipped`（`d3d9_backend.cpp:2662-2667`）。
  - 线协议注明这种情况下"传输保持健康，x86 端报告成功的跳过帧"（`d3d9_wire.h:131-134`）。
  - 但宿主主循环把所有非 SUCCEEDED 一律算进 rejected（`d3d9_host_main.cpp:1219`）。
- 所以这 2 次是改窗口尺寸的设计行为，不是故障。
- 判据改为 `rejected - presentsSkipped == 0`（`x64_proxy_gate_mode.py`），其他任何拒绝照样判失败。用已有产物离线复算（已核对）：walk / pressure r2 / crossmap r2 仍是 rejected 0；weather r1 变成 true。宿主侧的根本修正记为 N9（§5）。

**r2：两种模式都没起来**（已核对）
- 启动报"实例目录非空，拒绝复用"：天气脚本按 `--name` 给沙盒实例起名，r1 和 r2 都用了 `w1`。
- 这是测试脚本的问题，不是产品问题。之后每次运行用不同的名字（`wi3`/`wp3`）。

**代理 r3：过**（已核对，`x64-proxy-gates-20261117-weather-proxy-r3/wp3/run.json`）
- 测得的 10 项、代理验收 11 项全 true，退出码 0。
- 设备宿主 accepted 2067137；`rejected 3 = presentsSkipped 3`，全部是改尺寸那一刻跳过的 Present。
- 前端 `window-proxy`：`resizes 1, refused 0`。
- 14 项不适用，理由一致：天气的 render / frozenInput / volumetric 证据来自进程内渲染器的诊断（`ProcessServices().QueryWar3Weather*Diagnostics`）。代理模式下天气由宿主画，逐帧证据还没写出来，要 x86 侧 N5 和宿主侧 N4。

## 4 三个探针（T4）

共同做法：
- `run_x64_game_proxy_probe.py` 跑同一个产品 DLL，进程内（`--in-process`）一遍、代理模式一遍，`--profile product`。
- 动作表由 `run_x64_proxy_t4_probes.py actions <名字>` 生成。
- 分析：`run_x64_proxy_t4_probes.py analyze <名字> <进程内> <代理> <输出>`，输出在 `AutoTest/artifacts/x64-proxy-gates-20261117-t4-analysis/`。
- 描边和扫描要 `game.pause` 真停，所以加了 `DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE=0`。换图 r2 不加；r1 误加了，r3 是故意加的（见 §4.3）。

### 4.1 单位描边

**r1、r2：按对象挑选，挑不到**（已核对）
- 用 `outline.pick {kind=building|unit, rawcodes=…}` 从 `RenderObjectRegistry` 里挑对象，两种模式都返回 `no outline candidate`。打开全对象跟踪（`SetOutlineDebugAllObjectsEnabled(true)`）也一样：产品配置下这个登记表是空的。
- r2 的副产品，两种模式差别明显（已核对，`outline-r2/outline-analysis.json`，掩码 `outline-r2/outline-range-*.png`）：
  - **进程内**：全对象调试开关自己就会画描边。暂停画面里多出 **8755 个黄像素**，沿着建筑和树丛的轮廓；`outline.clear` 之后回到 0。
  - **代理**：黄像素 0；宿主影子 runLog 256 条里 `outline` 位全是 0。原因已核对，读代码：前端只有在 `War3RenderState::HasOutlineHandles()` 为真时才发 Outline 记录（`war3_proxy_frame.cpp:238-240`、`:289-304`），而全对象开关不往句柄集里加句柄。所以"全对象调试描边"是进程内独有的功能，代理没有对应物。
- 句柄覆盖率（N7b，代理，已核对）：

  | 时刻 | 单位 caster 带句柄的比例 | 说明 |
  |---|---|---|
  | s0（25 s，未开全对象跟踪） | 0%（0/3598 累计，上一帧 0/103） | 产品默认下，单位 caster 一个句柄都不带，描边根本无从匹配 |
  | s1（39 s，开了之后） | 上一帧 103/103 = 100%（累计 9579/24924） | 全对象跟踪会让 caster 带上句柄 |
  | s2（52 s，`outline.clear` 之后） | 上一帧仍是 103/103 | 恢复后仍带句柄（推断：句柄来源已建立，恢复开关不撤回已发出的 batch 句柄） |

  三个时刻的 `outlineMatched` 都是 0，因为句柄集是空的。

**r3：句柄段**
- 做法：用 `outline.pick {rangeLo=0x100000, rangeCount=1024, showVisible, widthPx=3}` 直接登记 War3 句柄段的开头 1024 个，并且同样打开全对象跟踪，让 caster 带句柄。
- 动作时间：30 s 拍 off1 → 32 s 挑选 → 36 s 拍 on1、存 s1 → 39 s `outline.status` → 41 s `outline.clear` → 44 s 拍 off2。
- 比较：on1−off1 是"描边"，off2−off1 是"清掉之后"。两种模式的掩码求 IoU。

**r3 结果**（已核对，`t4-analysis/outline-r3/outline-analysis.json`；掩码图 `outline-r3/outline-range-{inprocess,proxy}.png`，黄色就是新出现的描边像素）：

| | 进程内 | 代理 |
|---|---|---|
| 登记的句柄 | 1023 | 1023 |
| 描边像素（on1 比 off1 多出的黄像素） | **1334**，外框 [0,559]–[296,701]：左下一片树丛的轮廓 | **8962**，外框 [0,40]–[1901,716]：左下同一片树丛，加上右边几片树丛 |
| 清掉之后（off2 比 off1） | 0 | 2（噪声） |
| 两种模式掩码的 IoU | 描边 0.07，清掉之后 0 | |
| 单位类 caster 带句柄 / 命中句柄集（代理，上一帧） | — | s0 0/102；s1 **51/102**，命中 51；s2（clear 之后）回到 0/102 |
| 宿主影子 runLog 的 `outline` 位 | — | 256 条全 0 |

关于"登记 1023 个"：
- 挑选返回了 `outline handle range registration failed`，`handleCount` 1023 < 1024。
- 原因已核对，读代码：`0x100000` 的句柄 id 是 0，被 `NormalizeHandle`（`war3_render_state.cpp:88-93`）拒收。
- 设置在检查之前就已经生效，所以描边照样画了。动作表已改成从 `0x100001` 开始；r3 用的还是旧的 `0x100000`。

结论：
- **代理模式能画描边**（已核对）：用 JASS 的方式登记句柄以后，宿主画出了黄色描边，`outline.clear` 后消失。前端看到有 51 个 caster 命中句柄集。
- **两种模式描出来的对象不一样**（已核对现象，原因未查）。同一个暂停画面、同一组句柄：进程内只描了左下一片树丛，代理除了这一片，右边还描了好几片。一个可能是代理按 caster batch 的句柄匹配，一个 batch 里的多棵树会一起被描（推断）。哪边是"对的"，没有定论。
- **描到的是树，不是单位**（推断，看图）：最前面那 1023 个句柄大多是地图预放的树木；农民和主基地的描边在两种模式下都没出现。真正的"单位描边"要拿到单位句柄才能测，见 §6。
- 宿主 runLog 只留最后 256 次；暂停画面约 15 fps，`clear` 之后到退出还有约 20 s，所以这 256 条全在 clear 之后，`outline` 位全 0 不说明问题（推断）。要直接数，还是得靠 N7c。

### 4.2 视野外单位缺影子：相机扫描

**做法**（已核对）：
- 地图 Lost Temple 开局，红方人族主基地。
- 25 s 后依次 `game.pause` → `visibility.full_map` → `shadow.lock_sun {time01=0.3}`。
- `camera.snapshot` 取开局相机目标 (1616.8, −7147.4)，两种模式一样。
- 然后沿 x 方向每步 250 世界单位：先 +x 走 0..2500，再 −x 走 0..−2500。每一步：拍"影子开"图 → `status` → `api=csm.setEnabled;b:0` → 拍"影子关"图 → `b:1`。
- "影子像素"：开图比关图暗 >12（亮度）的像素。只看世界区域，去掉上栏和下方面板，隔列采样 ×2。
- "出屏侧边缘条带"：主基地出屏那一侧 20% 宽的条带。+x 时看左条，−x 时看右条。

**结果**（已核对，`sweep-analysis.json`、`sweep-curve.png`、`sweep-sheet-p0-p4.png`）：

| | +x 全画面 代理/进程内 | +x 左条 代理/进程内 | −x 全画面 代理/进程内 | −x 右条 代理/进程内 |
|---|---|---|---|---|
| 22 步里的比值范围 | 1.05–1.17 | 1.02–2.01（第 10 步两边都是 0） | 1.07–1.22 | 1.04–1.39 |

- **代理模式没有比进程内少影子**：每一步、每个量，代理都 ≥ 进程内。代理整体多 5–20%，曲线形状一致，主基地出屏后影子消失的位置两边相同。多出来的部分是画法差别（宿主的影子更深或更软），不是缺失（推断：没有逐对象核对）。
- 投射体数量随步数的变化，两边形状相同：代理 `casters.lastFrame.units` 103→123→7（+x）；进程内 `semanticSceneShadowCastersCount` 174→226→0。两个数口径不同（代理只算单位类，进程内含树），只能比形状。
- 局限：
  - 这个差分包含树和地形装饰的影子，量的是"整个画面的影子"，不是某一栋建筑；"建筑已出屏、影子还应该在"的那一小段没有单独分离出来。
  - 笔记 §5.3 的"太阳反向再各扫一遍"没做。

### 4.3 换到另一张图（--extra-map）

**做法**（已核对）：
- `--map` 是换图门的 A.w3x（`410436ED…`）。
- `--extra-map` 把 A 和 B（`B7302655…`）放到 A 的脚本 ChangeLevel 用的 `Maps/AutoTest/WarVKCrossMap20260925/` 下。
- 15–114 s 每 3 s 一次 `status`，每 10 s 拍一张图，118 s 关窗。

**r2：两种模式都走完 A→B→A**（已核对，`t4-analysis/extramap-r2/extramap-analysis.json`，联系表 `extramap-r2-sheet.png`）：

| | 进程内 | 代理 |
|---|---|---|
| 段标记（探针秒） | A 10.4，B 33.7，A' 57.1 | A 13.3，B 39.3，A' 65.0 |
| 帧号 15 s→114 s | 979→14429（约 136 fps） | 194→2304（约 21 fps） |
| 会话代号（`proxyMode.frame.sessionGeneration`） | — | 0→2→4；数据层 `session reset generation=1..5` 五行 |
| 前端 `annotation-proxy` | — | requested 5，sent 2，refused 0 |
| 宿主 `annotation-host` | — | sessionResets 2，sessionGeneration 4，worldScene 进出各 2140 |
| 判定 | 11 项全 true | 18 项全 true（含宿主 normal-return、会话干净结束、x86 无 Vulkan） |

**r1：代理卡在 B 的载入**（已核对现象）：
- 只有 A 第一段；到 114 s 还停在 B 的载入画面。
- 退出时设备宿主 5 s 内没退：`primary failure=timeout stage=setup`，终端 `local-contract hostExit=259`。
- 同一轮进程内也慢一倍：B 段 41.2 → A' 87.6 s，r2 只要 23 s。

**r1 卡住的原因：还没查实。** r1 和 r2 的 DLL、前端、宿主、地图、动作表都相同（已核对），差别有两处：

1. **暂停覆盖**：r1 带了 `DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE=0`，r2 没带，用默认的 1。
   - 这个变量只管 `Hook_GamePause`（`war3_hook_lifecycle.cpp:2728-2734`）：为 1 时拦下 `isPause` 请求，并记一行 `blocked GamePause request`。
   - r2 两种模式各只拦到 1 次，都在退出时（紧接着 `map transition signal count=3`），B 载入期间没有任何暂停请求（已核对，`logs/War3_d3d9.log`）。
   - 所以"暂停覆盖让 B 载入停住"缺乏证据。我中途的判断正是这一条，r3 已直接否定（见下）。
2. **机器负载**：r1 跑的时候机器明显更忙。
   - 代理 A 图约 7 fps，r2 约 21 fps。
   - 进程内的 B 段 46 s，r2 只要 23 s。
   - r1 代理前端 `command-ring-proxy` 的 syncCalls 49890、parks 134187；按运行时长折算，和 r2（232605 / 896299）同一量级，看不出卡在同步上。

结论：
- 这是一次没复现的卡住，原因未定。
- 上一版回执把它定为"代理换图载入极慢"交给性能 agent，证据不够，已降级为待复查。
- 已排了 r3：代理臂，故意带上 `=0`，直接检验第 1 条。

**r3（代理，故意带 `DISABLE_GAME_PAUSE=0`）：A→B→A 走完**（已核对，`t4-analysis/extramap-r3/extramap-analysis.json`）：
- 段标记 A 16.7 s，B 41.3 s，A' 67.6 s。
- 帧号 113→1928（约 18 fps）。
- 会话代号 0→2→4；前端 requested 5、sent 2、refused 0；宿主 sessionResets 2、generation 4。
- 18 项全 true。

所以第 1 条（暂停覆盖）**排除了**（已核对）。三次代理换图探针里有一次卡在 B 载入，原因仍未定，最可能和 r1 当时机器负载重有关（推断）。性能 agent 那边记为"待复查"（§5）。

## 5 交给性能 agent 的宿主侧项（`tools/render_host/**`，我没改）

| 编号 | 要什么 | 放哪（已核对行号，2026-11-17 工作树） | 谁用 |
|---|---|---|---|
| N3 | 宿主按会话代号记账：每次 SessionReset 和退出前各写一行 `{generation, runs, completeMapRuns, lastCompleteMap, maxDrawnCasters, deviceUsed, deviceAllocated}` | `tools/render_host/dxvk_core/d3d9_host_main.cpp:693-701`（`case d9::Op::SessionReset`，在 `annotationRecorder.reset` 之前记上一代）；`:1274`（`LogExitIntent("normal-return",…)` 之前记最后一代） | 换图门 retirementCoverage、mapEpochExact 的代理版；runtimeStateGate 的"每代都有完整影子图" |
| N4 | 宿主效果边沿行：BeforeUi 的效果集合（snow/volumetric/fog…）和上一次不同时写一行，带 `effectFrames` | `d3d9_host_main.cpp:684-685`（`shadowRunLog.push_back` 之后；同一分支 `:682` 记 `lastShadow`）；打印附近 `:1216-1217` | 天气门 14 项"不适用"里的 compositeSubmitted / volume 类 |
| N7c | 描边 pass 实际画了几次：`ShadowRunFacts` 加 `outlineDraws`，宿主 runLog 打出来。现有的 `outline` 位（`war3_shadow_scene_input.h:759-760,833` = `receiverNeedOutlinePass`）只说明"开着且有句柄" | 计数在 `src/d3d9/d3d9_war3_shadow_outline.cpp:228`、`:817`（`IsOutlineHandle` 匹配处）；打印在 `d3d9_host_main.cpp:1183-1199` `printRun` | 描边探针的宿主侧证据 |
| 观察（不是日志项） | **代理帧率明显低于进程内**：换图 A 图约 21 vs 136 fps（§4.3 r2）；Lost Temple 暂停画面约 15 vs 168 fps（描边 r2 的 s0→s1 段）。机器上同时有别的会话在跑 ML，绝对数有噪声 | — | 性能 agent 判断代理臂门的超时余量；换图门代理等待上限已放到 300 s |
| N9（新） | 宿主记账：设计内的 `PresentSkipped`（`d3d9_wire.h:131-134`，"transport stays healthy"）不要算进 `rejected`，或者另起一个计数。现在 `d3d9_host_main.cpp:1219` 把所有非 SUCCEEDED 都算 rejected | `d3d9_host_main.cpp:1219` | 天气门等改窗口尺寸的门；门这边目前先按 `reset-host.presentsSkipped` 扣除 |
| 观察（不是日志项） | 每次代理运行都有一个短命宿主（`host.log.1`，accepted 101、rejected 3、没有 draw），五个门都一样。我的验收只看最后那个设备宿主，所以不受影响。这 3 次被拒的大概是游戏启动时探测设备能力的调用（推断，没查） | 例如 `x64-proxy-gates-20261117-walk-proxy-r1/proxy/host.log.1` | 确认是否属正常 |
| 待复查（不是日志项） | 换图探针 r1 代理卡在 B 的载入：80 s 没载完，退出时设备宿主 5 s 没退（`primary failure=timeout stage=setup`，终端 `local-contract hostExit=259`）。r2、r3 都没复现（r3 带着同样的暂停覆盖），原因未定，可能和当时机器负载有关（推断，§4.3） | 产物 `AutoTest/artifacts/x64-proxy-gates-20261117-t4extramap-proxy-r1/`（`game.stderr`、`host.log*`、`logs/`、`status-e*.json`） | 3 次里卡 1 次；要不要在空闲机器上多跑几次，由性能 agent 决定 |

## 6 剩下的

- **要宿主侧日志的项**（§5）：
  - N3：换图门 retirementCoverage / mapEpochExact / queueSerials 的代理版；
  - N4：天气 14 项；
  - N7c：描边实际画次数。
- **要前端改的项**（不归我，记在这里）：
  - N5：天气 frozenInput 类；
  - N6：代理启动失败调 `ReportStartupFailure`；
  - N6b：代理模式套显存档位、写 `WarVK GPU:` 行。
- **换图门进程内**：`resetSerialAdvanced`、`runtimeStateGate` 两项，对 11-11 以来的产品一直是 false（§2）。要么修判据，要么查产品；不在这次范围内。
- **压力路线**：难度对话框两种模式都没被关掉。路线照样能过，但"压力"可能没压到预期强度（推断）。
- **描边**：
  - 产品默认下单位 caster 不带句柄（0%）；按对象挑选用的登记表是空的，所以按对象挑选这条路走不通；
  - r3 证明了"像 JASS 那样按句柄登记 → caster 带上句柄 → 代理能画描边"，但两种模式描出的对象集合不同（IoU 0.07），原因没查；
  - 句柄段只覆盖到地图预放的树，单位描边要先拿到单位句柄（例如加一个"列出选中单位句柄"的测试命令），再按那几个句柄登记；
  - `outline.pick` 句柄段的 API 默认起点仍是 `0x100000`（句柄 id 0，无效），动作表已经绕开了；要改源码默认值，得重新编译；
  - 宿主侧要 N7c 才能直接数。
- **影子扫描**：没有单独分离单栋建筑出屏的那一小段；没做太阳反向扫描。
