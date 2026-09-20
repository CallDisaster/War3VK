# AutoTest MCP 驱动实机取证的可行性与昨夜失败根因（2026-09-18 12:05）

## 0. 一句话结论

**MCP 是本任务正确的入口，我昨夜没用它是错的判断**；本轮把它跑通并定位了两个真实拦路条件：**①`ydwe` 启动模式被硬性限制在专用沙箱**；**②地图路径含 CJK 时 `direct` 模式会静默失败**（游戏在创建设备前退出）。改用 **ASCII 地图路径 + `direct` 模式**后，**已成功进入地图且控制面可用**。

## 1. 昨夜失败的确切根因（已复现并解释）

| 现象 | 根因（本轮实测） |
| --- | --- |
| `preflight_instance_pool` 返回 `ok=false` | `DEFAULT_WAR3_DIR = DEFAULT_SANDBOX_ROOT = E:\Work\War3_AutoTestSandbox`，**该沙箱里没有游戏**（无 `war3.exe`） |
| 手搓 `war3.exe -window -loadfile <相对路径>` 停在标题 200 s | **地图路径含 CJK**（`生与死`）：war3.exe（ANSI）解析失败的命令行后**在 DXVK 创建设备前就退出** |
| `YDWE.exe -war3 -loadfile` 打开 WorldEdit | 未走 MCP 的 YDWE 预检/注入链；且该模式**当前被沙箱门禁拒绝**（见 §2） |

### CJK 路径失败的证据

- `war3_d3d9.log`（11:56:55，328 B）：DLL **确实加载**（`DXVK: 1.21.00`、`Build: x86 gcc 15.2.0`、找到 Vulkan），但日志**只到扩展提供商列表（10 行）就停** ⇒ 进程在**创建 Vulkan 设备之前**退出；
- **无 WER 崩溃报告**（近 20 分钟）⇒ **非崩溃，是正常退出**；
- 换成 ASCII 路径 `Maps\Test\WorldEditTestMap.w3x`（与生与死**同尺寸 62,290,145 B**，应为 ASCII 名副本）后**立即进图**（见 §3）。

## 2. MCP 的沙箱门禁（设计如此，不是 bug）

```
launch_war3_test(war3_dir="E:\Work\Warcraft III", launcher_mode="ydwe", ydwe_root="E:\Work\Warcraft III")
→ {"ok": false, "code": "YDWE_SANDBOX_ROOT_MISMATCH",
   "error": "真实机只能使用专用沙箱 E:\Work\War3_AutoTestSandbox，拒绝路径: E:\Work\Warcraft III"}
```

- `launcher_mode="ydwe"`（即 MCP 文档推荐的 `YDWE.exe -war3 -loadfile <相对路径>` 链）**只允许沙箱根**；
- ⇒ **沙箱没有游戏** + **YDWE 链只允许沙箱** ⇒ 这条自动化路当前**不可用**，除非按 `option-b-sandbox-provisioning-checklist.md` 供给沙箱；
- `launcher_mode="direct"` **允许**真实机（本轮已验证），但它不注入 YDWE/JAPI，且对 CJK 路径敏感。

## 3. 本轮成功的证据（direct + ASCII 路径）

```
launch_war3_test(war3_dir="E:\Work\Warcraft III",
                 map_path="E:\Work\Warcraft III\Maps\Test\WorldEditTestMap.w3x",
                 deploy_d3d9_before_launch=False,      # 保留我们的候选 DLL（36,283,128 / 519AFA69…）
                 enforce_video_baseline=False,         # 未改动用户视频设置
                 windowed=True,
                 env_overrides_json={"DXVK_WAR3_FRAME_EVIDENCE":"1",
                                     "DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT":"1"})
→ ok=true, pid=6880, args=[war3.exe, -window, -loadfile, "Maps\Test\WorldEditTestMap.w3x"],
  mapLaunchMode="inplace-relative", 地图 SHA 源/目标一致
```

- **控制面已建立**：`read_runtime_status(pid=6880)` → `{"ok": true, "mode": "control-plane", "pipeName": "\\.\pipe\War3ControlPlane_6880", "data": {"frame": {...}}}`；
- ⇒ **确认进了地图**（`InitializeRuntimeCore` 已运行）；
- **两点重要确认**：①注入的两个子门环境变量**确实生效**（`effectiveWar3Environment` 列出）；②`deploy: null` ⇒ **没有覆盖我们已部署的候选 DLL**。

## 4. 尚未完成的自动化步骤（下一步）

1. 进程存活期只有约 3 分钟（11:59→12:01:56，期间每 10 s 自动导出 perf 报告），我**还未来得及 arm 录制器**游戏就自行退出了；
2. `frame_evidence_control.py --pid <pid> arm|status|trigger|freeze|export` **可用**（控制面已在），但需要**在进程存活窗口内**发出；
3. 本轮 perf 报告显示 `semanticSceneShadowCastersCount=0`、`drawTimeSemanticProducerSubmittedCount=0` ⇒ 该会话**阴影/语义管线未跑**（测试图可能停在起始状态、无单位）⇒ 需用会真正出单位的场景（如 `run_life_and_death_tdr_scenario` / `run_named_scenario`）或注入输入让游戏开打；
4. 目标顺序：`launch(direct, ASCII 地图)` → `wait_for_game_ready` → **立即 `arm(capacity=65536)`** → `status` 读实时 `paletteObject`（这一步即可**判定"已 arm 且零发射"还是"未 arm"**）→ 驱动战斗 → 再次 `status` → `trigger/freeze/export`。

## 5. 边界

- 本轮**未覆盖**对象级证据（仍未取到）；
- 未改动用户视频设置（`videoBaseline: skipped`）、未覆盖已部署 DLL（`deploy: null`）；
- 启动游戏属于用户明确请求（"用 MCP 制定地图然后测试"）；
- 不得把本轮 CPU 侧观察当作 GPU 提交或像素证据。

---

## 6. 第二轮（隔离桌面）实测：控制面给出决定性事实（12:07）

### 6.1 隔离桌面启动成功

```
launch_war3_test(war3_dir="E:\Work\Warcraft III", map_path="...\Maps\Test\WorldEditTestMap.w3x",
                 launcher_mode="direct", use_isolated_desktop=True, desktop_name="WarVK-P0",
                 windowed=True, deploy_d3d9_before_launch=False, enforce_video_baseline=False,
                 env_overrides_json={DXVK_WAR3_FRAME_EVIDENCE:1, ..._PALETTE_OBJECT:1})
→ ok=true, pid=46344, useIsolatedDesktop=true, args=[war3.exe, -window, -loadfile, "Maps\Test\WorldEditTestMap.w3x"]
```

- 控制面建立：`read_runtime_status(46344)` → `mode: control-plane`、`pipe: \\.\pipe\War3ControlPlane_46344`；
- 用户明确要求隔离桌面（避免影响前台），且本轮**只采集数据、不是性能测试**。

### 6.2 **决定性事实：未触发时录制器根本不存在**

```
frame_evidence arm(capacity=65536) → {"ok": false, "error": "recorder control ownership mismatch"}
frame_evidence status            → {"ok": false, "error": "no session"}
```

对照 `war3_frame_evidence.cpp:272-294`：

- `"no session"` 来自 `if(!s && action!="arm") return {"no session"}`（`:289`）⇒ **`store == nullptr`**：
  **证据录制器在本会话从未被 arm（Store 根本没创建）**；
- `"recorder control ownership mismatch"` 来自 `:285` 的 `controlAuthority.allows(localLease, ...)` ⇒
  **本地（in-process）录制器持有控制权**，外部控制面无法 arm。

### 6.3 这一事实对"对象级零输出"的意义

- `ActiveSession()` = `s->active`，且 `s` 为 **null 时返回 0**（`war3_frame_evidence.cpp:202-206`）；
- ⇒ 在**未 arm** 的会话里，`d3d9_device.cpp:22121-22124` 的 `if (paletteObjectSession != 0u)` **恒假**，
  对象级入队**整段被跳过** —— 每次 caster 追加都静默落空；
- ⇒ 这**至少**解释了"无触发会话必定零输出"；对用户那两轮（有触发、`accepted` 很大）**仍不能完全解释**，
  因为那两轮的 Store 显然存在（否则不会有导出）。

### 6.4 若无触发会怎样（已核实）

- **不存在"启动即 arm"的环境变量**（全量检索 `src` 内的 `DXVK_WAR3_FRAME*`：仅 `EVIDENCE`、`_CASTERS`、`_DRAWS`、`_INPUTS`、`_OUTPUT`、`_PALETTE_OBJECT`、`HISTORY_SELF_CONTAINED`、`TIMELINE`）⇒ **录制只能靠显式 arm（控制面或游戏内历史触发）**。

### 6.5 进程寿命（实测）

- 非隔离：约 3 分钟（11:59→12:01:56，每 10 s 自动导出 perf 报告）；
- 隔离桌面：约 1–2 分钟（12:05:58→≤12:07:01）；
- ⇒ 自动化必须在**进程存活窗口内**完成 arm/status/trigger，且窗口很短。

### 6.6 尚未闭合的两种可能（需最小诊断才能定案）

| 可能 | 内容 | 需要的证据 |
| --- | --- | --- |
| A | 用户那两轮 **确实 armed**，但 `d3d9_device.cpp:22113` 块**在下游诸多门之后、实际不可达** | 在 `:22113` 处加一个"块被执行"计数 |
| B | 用户那两轮 armed，但块执行时 `ActiveSession()==0`（环已 Frozen / `active` 已清） | 在块内记录 `ActiveSession()` |

⇒ **两者都只能靠最小诊断构建区分**（在导出头块暴露 `armed` / `activeSession` / `enqueueBlockReached`）。
**静态分析已到极限**，继续读码不会有新结论。

---

## 7. 沙箱根改指向 + 自动化阻塞点（12:20，用户授权免确认）

### 7.1 已改代码（用户明确指示）

`AutoTest/autotest_sessions.py:25`：

```python
- DEFAULT_SANDBOX_ROOT = Path(r"E:\Work\War3_AutoTestSandbox")
+ DEFAULT_SANDBOX_ROOT = Path(r"E:\Work\Warcraft III")
```

- 这是**唯一真源**，8 个文件（含 `war3_autotest_mcp.py` 14 处引用）随之生效；
- 用户说明：**沙箱早已被删**；验证：`DEFAULT_SANDBOX_ROOT = E:\Work\Warcraft III`、`MCP DEFAULT_WAR3_DIR` 同步；
- 现场必需文件齐全：`war3.exe` / `Game.dll` / `war3.mpq` / `war3x.mpq` / `war3patch.mpq` / `Storm.dll` / `YDWE.exe` / `bin\LuaEngine.dll` / `plugin\warcraft3\yd_jass_api.dll` 全部存在；
- 注册表 `HKCU\...\Warcraft III\InstallPath` = **`E:\Work\Warcraft III`**（与新的沙箱根**精确一致**）⇒ 原先的 `YDWE_INSTALL_PATH_MISMATCH` 前置条件天然满足。

### 7.2 为使 ydwe 预检通过而补的现场文件

- 新建 `E:\Work\Warcraft III\logs\war3.log`（0 B）—— `_probe_ydwe_log_writable` 要求该 YDWE 日志存在；
- 补后 `YDWE_LOG_MISSING` 消失（这是**用户磁盘上新增的一个空日志文件**，需登记）。

### 7.3 仍存在的阻塞（实测）

| 阻塞 | 证据 | 性质 |
| --- | --- | --- |
| `ydwe` 模式：`YDWE_WAR3_CHILD_NOT_FOUND` | 超时未见 YDWE 派生的 war3.exe 子进程 | 环境/启动器差异（用户手动用的是 `bin\YDWEConfig.exe -launchwar3`，而 MCP 固定调用 `YDWE.exe -war3 -loadfile <rel> -closew2l`） |
| 外部 `arm` 被拒 | `{"error":"recorder control ownership mismatch"}` | **设计如此**：本地 in-process 录制器持有控制权 |
| `status` 无会话 | `{"error":"no session"}` ⇒ `store == nullptr` | 同上：**未 arm 就没有 Store** |
| 比赛未开始 | `wait_for_game_ready` 超时 170 s；`wait_for_hot_shadow_frame` 超时 120 s；期间 `frameNumber` **恒为 1534** | 该图停在初始状态（帧号不前进）⇒ 无场景、无 caster |

### 7.4 由此确定的两条自动化结论

1. **对象级证据只能由"游戏内 Ctrl+Shift+C"这条路径 arm**（控制面被设计性拒绝）⇒ 自动化应改为：启动进图后**用 MCP 输入注入发 Ctrl+Shift+C**（`send_war3_input_plan` / `control_war3_window`），而不是发控制面 `arm`；
2. **必须用会立即开始比赛的图**（melee 图），否则帧号不前进、无 caster/阴影。候选：`Maps\ShadowTest\SunkenCity.wx3`、`(2)ConcealedHill.w3x`、`(6)BlizzardTD.w3x`（后者为 TD，开局即有大量单位）。

### 7.5 尚未做（下一步）

- 用 melee 图 + 输入注入 Ctrl+Shift+C 的端到端自动化；
- 仍待定案的 A/B 两种可能（见 §6.6）需要最小诊断构建。

## 8. **决定性实验：外部 arm 成功，对象级仍为 0（真零，非未 arm）**（12:40）

### 8.1 解锁外部 arm 的开关

`DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=0` ⇒ `effectiveConfiguration.localRecorderOwner: false` ⇒ 控制面 arm 成功。

### 8.2 闭环实测（隔离桌面，pid 26908，ConcealedHill）

```
ARM    ok= True
INPUT  ok= True, actionCount=8, mode=isolated-window-message-plan     ← 输入注入可达窗口
PAL    {"emitted":"0", "watchCount":0, "accepted":"8580428"}
FRAME  frameNumber=2682 恒定（6 次轮询不变）, unitCount=0, buildingCount=0
TRIG/FREEZE/EXPORT ok=False（环状态不匹配，未导出）
```

### 8.3 因此**确定**的事实

1. **armed = true**（arm 返回 ok；`QueryPaletteObjectEvidenceHeader` 在 armed 时返回**真实计数**）
   ⇒ `emitted=0 / watchCount=0` 是**真实的零**，**不是**"未 arm"掩码 ⇒ **假设"未 arm"被排除**；
2. 录制器录到 **8,580,428** 条 CPU 边界事件 ⇒ 渲染在跑、录制器在工作；
3. 但 `unitCount=0 / buildingCount=0`、`frameNumber` 恒定 ⇒ **比赛从未真正开始**（无场景 ⇒ 无 caster ⇒ 那块入队代码永不执行）。

### 8.4 新确定的阻塞与性质

| 项 | 结论 |
| --- | --- |
| 控制面 arm | **已解决**（`SELF_CONTAINED=0`） |
| 实时读数 | **已解决**（`status` 返回完整 `paletteObject`） |
| 输入注入 | **可达窗口**（`ok=true`），但**未使比赛开始** |
| 比赛开始 | **未解决**：`frameNumber` 冻结、无单位。两种环境的图（WorldEditTestMap / SunkenCity / ConcealedHill）**都**如此 |
| 进程寿命 | 隔离桌面下进程随启动脚本结束而被 job object 杀掉 ⇒ **全流程必须单脚本长驻** |

### 8.5 下一步（未做）

1. 查明比赛为何不开始：检查同会话 perf 报告的 map/epoch/frame 语义；
2. 若隔离桌面本身导致（DirectInput/消息不吃、桌面不合成），需评估改在**普通桌面**跑一次对照；
3. 场景一旦起来，即可 arm → 等 caster → trigger → freeze → export → 读头块。

---

## 9. **崩溃根因：NVIDIA 32 位着色器编译器（不是候选 DLL）**（12:45）

### 9.1 事实

用设计场景 `dynamic_shadow_pressure` 的图（`光影测试.w3x`，5,859,935 B，源 `E:\Work\War3\Maps\ShadowTest\`）两次启动**都立刻崩溃**：

| 运行 | 进程 | 图路径 |
| --- | --- | --- |
| 12:34 | pid 44992 | `Maps\光影测试.w3x`（CJK） |
| 12:37 | pid 25104 | `Maps\Test\ShadowPressure.w3x`（ASCII 副本，排除 CJK 因素） |

`latest_crash.json`（schemaVersion 2）：

```json
{"exceptionCode":"0xC0000005",
 "exceptionAddress":"0x56538C3E",
 "crashPoint":{"moduleBase":"0x55950000","moduleOffset":"0xBE8C3E",
   "modulePath":"C:\Windows\System32\DriverStore\FileRepository\nvmdi.inf_amd64_21e6b42b376b80b2\nvgpucomp32.dll"}}
```

- `nvgpucomp32.dll` = **NVIDIA GPU（32 位）着色器编译器**；
- `war3_d3d9.log` 显示崩溃前**已进图**：`[FileManager Test] OnGameStart triggered`、阴影 hook 全部 `result=ok`、`semantic bypass keeps S1 terrain legacy capture`、`PATH BLOCKER REJECT #1`、随后 `ShaderManager: Global Uniform missing register: Time` 后崩；
- 日志另有 `[FileManager Test] FAILED: Could not load war3mapImported\Disaster.blp from MPQ`（该图资源缺失）。

### 9.2 结论

- **崩溃点在我们的模块之外**（NVIDIA 驱动内部），**不能**据此指控候选 DLL 越界；
- 但该图在本机（RTX 4060 Ti + 当前驱动）**确定性触发驱动编译器崩溃** ⇒ **此图当前不可用于取证**；
- 与项目历史一致：既有 `nvlddmkm153` 与玩家闪退记录都指向该驱动族。

### 9.3 对取证路线的影响

- 设计场景 `dynamic_shadow_pressure` 对应的图**当前不可用**；
- 其余已成功进图的图（`WorldEditTestMap.w3x` 62 MB / `SunkenCity.w3x` / `ConcealedHill.w3x`）**不崩**，但控制面 `frame.unitCount` 恒为 0、`frameNumber` 恒定 —— **尚不清楚这是"比赛未开始"还是"该字段在此模式不填充"**（下一轮须先核实字段语义，不能默认）。

### 9.4 下一步（未做）

1. 核实控制面 `frame` 块各字段语义（哪些字段真正表示"已开局"），再判断比赛是否已开始；
2. 若确为未开局，改用 `run_life_and_death_tdr_scenario` 等**专门驱动**路径（它会 pan 相机、注输入）；
3. 取证时不使用 `光影测试.w3x`（驱动崩溃）。

---

## 10. **推翻上一轮假设：比赛已开始**（12:55）

### 10.1 权威字段（控制面 `data.runtime` / `data.render`）

```json
runtime: {"gameStarted": true, "jassReady": true, "runtimeReady": true}
render : {"inGameRenderReady": true, "isInGame": true, "isLoading": false, "worldPtr": 780271752}
module : {"state": "Running", "loaded": 1, "registered": 1, "dispatchCalls": 14244, "callbackErrors": 0}
```

⇒ **游戏确实已进入对战并处于 in-game**。§8.4 里"比赛从未开始"的判断**错误**，予以更正。

### 10.2 我误判的原因（重要，避免再犯）

- 我用了 `data.frame.frameNumber`（在 90 s 内恒定）当作"帧冻结"证据；
- 但同一次会话里 **`data.frameIndex` 是前进的**（另一轮采样为 2655）⇒ `frame.frameNumber` **不是**游戏帧计数器，
  **不能**用它推断"比赛未开始"。**教训：判断状态必须用 `runtime`/`render` 的权威字段，不得用推测字段。**

### 10.3 这些会话里真正为零的是"语义 caster / 调色板查询"路径

| 计数（`data.shadow`） | 值 |
| --- | --- |
| `currentDrawContractPublishAttemptCount` | **117,495** |
| `currentDrawContractPublishReadyCount` | **58,841** |
| `currentDrawContractPublishSkippedNonWorldContext` | 27,119 |
| `currentDrawContractPublishSkippedSmallViewport` | 0 |
| `currentDrawCapturedPaletteQueryAttemptCount` | **0** |
| `currentDrawContractQueryAttemptCount` | **0** |

- CurrentDraw 契约**发布**极活跃（11.7 万次尝试、5.9 万次 ready），但**查询**路径恒 0；
- `frame` 块多为 0，仅 `recordsWithResolvedGeoset/RuntimeModel/StableIdentity = 4`（极少量记录）；
- 与用户的实机会话（169 caster、`drawTimeSemanticProducerSubmittedCount = 109,877`）**量级完全不同**。

### 10.4 当前最可信的差异假设（待验证，未定论）

- 用户会话 = **普通桌面 + 全屏 2560x1440**；我方会话 = **隔离桌面 + windowed 1902x963**；
- ⇒ 差异可能来自窗口尺寸/是否真正 present/隔离桌面合成；**尚无证据**支持任一具体机制；
- 验证方式（下一轮）：同图在**普通桌面全屏**跑一次，比对 `shadow.*` 与 perf 报告的 `semanticSceneShadowCastersCount`；
- 若确认是隔离桌面所致，则需在"不影响用户前台"与"能产生 caster"之间取折中（例如正常桌面 + 立即最小化/移出屏幕，或直接询问用户可否短暂占用前台）。

### 10.5 不变的边界

- 仍**未取到**对象级证据（判定维持**未覆盖**）；
- 未改动用户视频设置；未覆盖候选 DLL（现场仍 `519AFA69…`）；
- 隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

### 10.6 画面证据（ASCII 还原，因当前模型不支持图像输入）

- 采集：`capture_war3_screenshot`（走控制面 `capture_final_frame`）→ `AutoTest/artifacts/screenshots/war3_20260918_1246{24,39}.png`，均 **1902×963**；
- 分析：两图 **SHA-256 不同** ⇒ 画面确实变化过；
  · Shot1 均值亮度 **49**、最大 175、64×36 唯一色 **1937**，ASCII 还原显示**有纹理的地图视图 + 底部成带状 UI** ⇒ **渲染正常、不是黑屏/纯菜单**；
  · Shot2 均值亮度 **17**、最大 192、唯一色 **1061**，ASCII 还原**几乎全黑**，仅顶部残留静态标记；
- ⇒ 注入「点击 + 空格」之后画面转入近黑（可能淡出/暂停覆盖/退回菜单），**尚未定论**；
- **注意**：两图前两行 ASCII 完全一致（同一组静态标记），提示存在固定覆盖元素（loading/提示条之类），待查。

### 10.7 新假设（未验证）

- 地图已渲染（Shot1 有地形纹理）但 `frame` 场景计数恒 0、`unitCount=0` ⇒
  可能 **`-loadfile` 单机启动未给玩家分配单位/玩家槽**（有 worldPtr 但无己方单位）⇒ 无 caster；
- 也可能主循环确实停滞。**两者需下一轮用"单位拥有者/玩家槽"类字段或相机移动后计数变化来区分**。

---

## 11. **定案实验：armed + 210 caster + 719k 语义提交 ⇒ 对象级仍为全零**（13:05）

### 11.1 先纠正一个我自己的错误：`frame` 块不是场景指标

- 我一直用控制面 `data.frame.unitCount/buildingCount`（恒 0）判断"无 caster" ⇒ **错误**；
- **同会话的 perf 报告显示**：`semanticSceneShadowCastersCount = 210`、`drawTimeSemanticProducerSubmittedCount = 719,419`；
- ⇒ **语义阴影管线一直在跑**；判场景必须读 **perf 报告**，不得用控制面 `frame` 块。

### 11.2 决定性的单次测量（隔离桌面，ConcealedHill，pid 7016）

```
ARM ok = True
status（每 16 s 一次，共 5 次）：
  accepted : 782,872 -> 1,625,711 -> 2,389,276 -> 3,514,208 -> 4,933,280
  emitted  : 0 0 0 0 0
  terminalEmitted : 0
  watchCount      : 0
  droppedPerFrame : 0   （以及全部其它 dropped/closed 计数均为 0）
perf: semanticSceneShadowCastersCount = 210 ; drawTimeSemanticProducerSubmittedCount = 719,419
```

### 11.3 由该测量**排除**的假设

| 假设 | 状态 |
| --- | --- |
| 录制器未 arm | **排除**（`arm` 返回 ok；armed 时头块返回真实计数） |
| 没有 caster / 阴影管线没跑 | **排除**（210 caster、719,419 语义提交） |
| 录制器故障 / 环未工作 | **排除**（493 万条 CPU 边界事件被接受） |
| 事件被丢弃（表满/探针上限等） | **排除**（**全部** dropped/closed 计数精确为 0） |

⇒ **只剩一种解释：`d3d9_device.cpp:22113` 那段对象级入队块从未被执行**（或其 `ActiveSession()==0` 分支恒走）——
且因为**连一个 drop 计数都没有**，"执行了但被拒"的可能性极低。**倾向假设 A（块不可达）**。

### 11.4 下一步（已按目标授权执行）

- 目标明确授权："若仍为零，则做最小诊断构建（导出头块暴露 `armed` / `activeSession` / `enqueueBlockReached`）一次定案"；
- 现状已满足该条件 ⇒ **下一步做最小诊断构建**：
  · 在 `paletteObject` 头块加 `armed`、`activeSession`、`enqueueBlockReached`（在 `:22113` 处自增的计数）；
  · 改动极小、纯只读、不改行为；重建（Below Normal + `-j2`）+ 重新部署（先备份）+ 跑相关静态门禁。

### 11.5 附：`trigger/freeze` 需带 `session`

- `Control()` 对 `trigger/freeze/export` 要求 payload 含 `session` 且等于 `ring.session()`（`war3_frame_evidence.cpp:293`）；
- 我未传 ⇒ 返回 `session mismatch`。**导出流程须从 `status` 结果取 session 再回传。**

---

## 12. **最小诊断构建完成并定案：入队块不可达（假设 A 确认）**（13:20）

### 12.1 诊断改动（4 处，全部只读、不改行为）

| 文件 | 改动 |
| --- | --- |
| `war3_palette_object_evidence_sink.h` | 新增声明 `NotePaletteObjectEnqueueBlockReached()` / `PaletteObjectEvidenceArmed()` / `PaletteObjectEnqueueBlockReachedCount()` |
| `war3_palette_object_evidence_sink.cpp` | 新增计数器 `g_paletteObjectEnqueueBlockReached` 与三个实现（`Note*` 在未 arm 时早退） |
| `d3d9_device.cpp`（`:22113` 入队块入口） | 在 `if (paletteObjectEvidenceOn) {` 内首行调用 `NotePaletteObjectEnqueueBlockReached()` |
| `war3_frame_evidence.cpp`（`PaletteObjectHeaderJson`） | 头块新增 `armed` / `activeSession` / `enqueueBlockReached` 三个只读字段 |

- 构建：`build32_safe.cmd src/d3d9/d3d9.dll -j2`（Below Normal）**成功**（`[6/6] Linking`）；
- 新 DLL：**36,287,561 B / `EFF2F45A278215E293F0E284C6CF45BCF541EC15AC4BFC6484AFFBE609643526`**；
- 旧候选已备份：`E:\Work\Warcraft III\d3d9.dll.519AFA69_backup_20260918`（36,283,128 B / `519AFA69…`）；
- ⚠️ `ninja -C build32 -n` 仍列 **20 个隐藏测试目标**待重建（`-j2` 只建了 d3d9.dll 目标）⇒ **全门禁复跑尚未做**，不得声称门禁通过。

### 12.2 决定性读数（隔离桌面 ConcealedHill，pid 19044）

```
PRE-ARM : ok=False（no session，符合预期）
AFTER   : armed = True
          activeSession = 1            <-- 会话非 0
          enqueueBlockReached = 0      <-- 块从未被到达
          emitted = 0 ; watchCount = 0
          accepted: 727,438 -> 1,504,824 -> 2,273,496 -> 3,021,932
PERF    : semanticSceneShadowCastersCount = 209
          drawTimeSemanticProducerSubmittedCount = 420,578
```

### 12.3 结论

- **假设 A 确认、假设 B 排除**：`armed=true` 且 **`activeSession=1`**（非 0）⇒ 不是因为"会话为 0"；
- 在 **209 caster / 420,578 次语义提交 / 3,021,932 条已录事件** 下 `enqueueBlockReached` 仍 **0**；
- ⇒ **`d3d9_device.cpp:22113` 那段对象级入队块在实践中不可达**：`War3TryAppendSemanticShadowPacket` 在该点之前就已返回；
- ⇒ **这不是"配置没开"，而是"该采集点被上游门完全遮蔽"** —— 现有候选**无法产出对象级证据**，与用户两轮实测为零完全一致。

### 12.4 下一步（下一轮）

1. 读 `War3TryAppendSemanticShadowPacket`（`:19862` → `:22113`）之间**所有 early return**，找出实际遮蔽点
   （已知线索：`:19868` session-ready/epoch 门、`:19885` `ShadowProducerPolicyAllows(SemanticDirectGrouped)`、`:19901` 所有权预过滤）；
2. 核对"语义生产者提交"（`drawTimeSemanticProducerSubmittedCount`）**是否走同一函数** —— 若走别的 append 路径，则本块从一开始就挂错了函数（**真正的缺陷**）；
3. 定位后按等价迁移纪律修复 + 全门禁复跑，再重跑本诊断实验验证 `enqueueBlockReached > 0` 且 `emitted > 0`。

### 12.5 边界

- **未**取得非零 `paletteObject` 导出（目标主判据**未达成**）；
- **未**跑全门禁（20 个测试目标待重建）；**未**提交、**未**部署到稳定通道；
- 现场 DLL 现为诊断构建 `EFF2F45A…`（**已备份前一个候选**，可回退）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 13. 遮蔽点定位（第一轮）：canonical 门拒掉全部 skinned 记录（13:35）

### 13.1 结构事实（读码）

- `:21871` 之后到 `:22113` **没有任何 `return`**（全量检索 `return`，区间内最后一个在 **21871**）
  ⇒ **只要控制流越过 21871，就必然到达对象级入队块**；
- 因此遮蔽点必在 **21871 之前**；区间内共 46 个在 `shadowStats.*` 上自增的诊断门计数器。

### 13.2 经验定位（同一会话 perf 报告：caster=209）

区间 19910..22113 内**非零**的计数器（其余 33 个全为 0）：

```
semanticSceneAppendEntrySkinnedCount            = 48
semanticSceneSkinnedDynamicIndexSliceCount      = 48
semanticSceneCurrentDrawContractKnownCount      = 48
semanticSceneCurrentDrawPaletteReadyCount       = 48
semanticSceneCurrentDrawGroupSlotReadyCount     = 48
semanticSceneCurrentDrawResolveReadyCount       = 48
semanticSceneLivePaletteRefreshAttemptCount     = 48
semanticSceneLivePaletteRefreshHitCount         = 48
semanticSceneMaterialObservedCutoutCount        = 48
semanticSceneCanonicalRejectNoWorldTransform    = 48
semanticSceneCanonicalGateRejectSkinnedCount    = 48   <-- 全部被 canonical 门拒绝
dynamicPoseCount                                = 42,098
dynamicSkinnedOutputCount                       = 42,098
```

对照上游：`drawTimeSemanticProducerSubmittedCount = 420,578`、`OwnedDirectGroupedSkipCount = 231,045`
（⇒ 越过所有权门的约 **189,533** 次）。

### 13.3 由此得到的结论（含不确定处，如实标注）

- **确定**：进入该阶段的 **skinned** 记录（本窗口 48 条）**全部**在 canonical 门被拒
  （`AppendEntrySkinned` 与 `CanonicalGateRejectSkinned` 数值相同 ⇒ 命中率 0）；
- **确定**：对象级入队块位于该门**下游** ⇒ **被拒的记录永远到不了块**；
- **不确定**：48 vs 189,533 的巨大差额说明**多数调用在更早处即被分流**，但这些分流点未计入我提取的 46 个计数器
  （可能使用了别的 stats 别名或按帧窗口计数）。**在语义未确认前，不得断言"某一门吃掉了全部"**。
- **待确认**：这些 `semanticScene*Count` 是**累计**还是**按窗口/按帧**（48 太小，疑为窗口量）——
  需在下一轮核对计数器语义后再定论。

### 13.4 下一轮（按性价比排序）

1. **核对计数器语义**（累计 vs 窗口）：读这些 `semanticScene*Count` 的定义与写入点；
2. 若确为窗口量 ⇒ 在 `:21001..:21057`（canonical 段）与 `:21871` 附近**加到达计数**（第二次最小诊断构建），
   直接测出"229k 次调用各自停在哪一行"；
3. 同时核对 `drawTimeSemanticProducerSubmittedCount` 的写入点：**它是否真的调用 `War3TryAppendSemanticShadowPacket`**
   —— 若否，则对象级入队块**挂错了函数**（真正的缺陷）。

### 13.5 边界

- 主判据**仍未达成**；全门禁**仍未复跑**；未提交、未部署到稳定通道；
- 现场 DLL 仍为诊断构建 `EFF2F45A…`（前一候选已备份）。

---

## 14. **根因确认：对象级入队块挂在了生产不走的那条路径上**（13:50）

### 14.1 决定性证据（读码 + 计数器归属）

| 事实 | 证据 |
| --- | --- |
| 生产提交计数 `drawTimeSemanticProducerSubmittedCount++` 位于 **`:23495`** | 该行所属函数为 **`D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer`**（定义于 **`:23027`**） |
| 该函数内的 append 是 **`appendExactSubmittedManifestRecord(entry)`**（`:23497`） | **其中没有任何 palette-object 采集调用** |
| 对象级入队块位于 **另一函数** `War3TryAppendSemanticShadowPacket`（`:19862`，块在 `:22113`） | 本会话 `enqueueBlockReached = 0` |
| 但该函数**确实被调用** | `OwnedDirectGroupedSkipCount = 231,045`（自增点在 `:19907/:19919/:19929`，均在该函数内） |

### 14.2 结论

1. **两个函数是不同路径**：`War3TryPopulateDrawTimeSemanticProducer`（生产实际提交，420,578 次）**vs** `War3TryAppendSemanticShadowPacket`（对象级证据所在，231,045 次全部被 exact-owner 预过滤拒绝）；
2. ⇒ **对象级 palette 证据被插桩在"这些 caster 在生产中不走的那条路径"上**；
3. ⇒ 这解释了：用户两轮实测为零、我方隔离桌面实测为零、以及 `enqueueBlockReached = 0`；
4. ⇒ **不是配置问题，是插桩点选择问题**（该缺陷与"未 arm""无 caster""记录器故障"均无关——三者均已排除）。

### 14.3 修复方向（下一轮实施，须按等价迁移纪律）

- 在**生产路径** `War3TryPopulateDrawTimeSemanticProducer` 的 exact-submit 处（`:23495`–`:23497` 附近）加入等价的
  `NoteServed` / `NoteEnqueued` 采集调用（键、帧域、来源参数需与既有语义一致，**不得**自行发明语义）；
- 或按上级裁定把两路径统一后再插桩；**两种都需：legacy 参考 + 宿主差分 + ≥2 条变异真跑 + 全门禁复跑**；
- 修复后重跑本诊断实验，验收判据：**`enqueueBlockReached > 0` 且 `emitted > 0`**，并产出 `paletteObject` 非零的 `cpu-*.json`。

### 14.4 边界

- 主判据**仍未达成**（尚未修复、尚无非零导出）；
- **全门禁未复跑**（`ninja -n` 仍列 20 个隐藏测试目标）；未提交、未部署到稳定通道；
- 现场 DLL 为诊断构建 `EFF2F45A…`（前一候选 `519AFA69…` 已备份）；未改用户视频设置。

---

## 15. **完全定案（第二次诊断构建）：48 次进入 append，48 次全被 canonical 门拒**（14:10）

### 15.1 新增仪器

- sink 新增 `NotePaletteObjectAppendEntered()` / `PaletteObjectAppendEnteredCount()`；
- `d3d9_device.cpp` `War3TryAppendSemanticShadowPacket` **函数体第一行**调用之（在任何门之前）；
- 头块新增 `appendEntered` 字段；
- 构建成功；新 DLL **36,287,928 B / `6EC8DF73C59F3371D07E3109…`**（已部署；前一诊断构建 `EFF2F45A…` 可重建）。

### 15.2 决定性读数（隔离桌面 ConcealedHill，pid 38768）

```
armed = True ; activeSession = 1
appendEntered       = 16 -> 32 -> 40 -> 48     <-- 函数仅被进入 48 次
enqueueBlockReached = 0                        <-- 48 次全部在到达块之前被拒
emitted = 0 ; watchCount = 0
accepted            = 1,270,555 -> 2,157,573 -> 2,867,248 -> 3,780,895
PERF: semanticSceneShadowCastersCount = 209
      drawTimeSemanticProducerSubmittedCount = 514,979
      drawTimeSemanticProducerOwnedDirectGroupedSkipCount = 280,636
```

### 15.3 与上一轮计数器交叉验证（完全吻合）

```
semanticSceneAppendEntrySkinnedCount          = 48
semanticSceneCanonicalGateRejectSkinnedCount  = 48   <-- 100% 被拒
semanticSceneCanonicalRejectNoWorldTransform  = 48   <-- 拒因：skinned 且无世界变换
semanticSceneCurrentDrawPaletteReadyCount     = 48
```

### 15.4 **对 §14 的更正（重要）**

- §14 据 `OwnedDirectGroupedSkipCount = 231,045` 推断"该 append 函数被调用约 23 万次" ⇒ **错误**；
- 该计数器在 **`:19907/:19919/:19929`（append 函数内）** 与 **`:24527/:24828/:25625/:26392/:26399/:26410`（其它函数内）** 多处自增，**是聚合量**；
- 入口计数证明：**该函数本会话仅被进入 48 次**。**教训：聚合计数器不能归因到单一调用点。**

### 15.5 最终根因（两段，均有证据）

1. **生产提交不走该函数**：514,979 次生产提交由 `War3TryPopulateDrawTimeSemanticProducer`（`:23027`）完成，其 append 为 `appendExactSubmittedManifestRecord(entry)`（`:23497`），**不含任何 palette-object 采集**；
2. **该函数仅有的 48 次调用全部在 canonical 门被拒**（skinned 且无 world transform），而对象级入队块在该门**下游** ⇒ `enqueueBlockReached = 0`。

⇒ **对象级证据被插桩在一条"既不承载生产流量、仅有的少量调用又全被拒"的路径上** —— 因此**无论怎么配置都不可能产出非零证据**。

### 15.6 修复方向（明确，下一轮实施）

- **主**：在**生产路径** `War3TryPopulateDrawTimeSemanticProducer` 的 exact-submit 处（`:23495`–`:23497`）挂等价 `NoteServed`/`NoteEnqueued`；
  需要的键/帧域/来源参数须从该处已有的 `entry` 与既有语义推导，**不得自行发明语义**；
- **备**：把对象级入队块从 canonical 门**下游**上移到该门之前（但该路径仅 48 次调用，**单独做此项不足以达到目标**）；
- 无论哪种，均须：legacy 参考 + 宿主差分 + ≥2 条变异真跑 + **全门禁复跑**；验收判据 **`appendEntered>0 且 emitted>0`** 并产出 `paletteObject` 非零的 `cpu-*.json`。

### 15.7 边界

- 主判据**仍未达成**；**全门禁仍未复跑**（`ninja -n` 仍列待重建测试目标）；
- 未提交、未部署到稳定通道；现场 DLL 为诊断构建 `6EC8DF73…`（前两版均可重建/回退）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 16. 修复实施方案（已做可行性核对，下一轮实施）（14:25）

### 16.1 生产路径是否具备等价输入 —— **具备**

对 `War3TryPopulateDrawTimeSemanticProducer`（`:23027`–`~23520`）内 `entry.*` 字段使用做全量统计，可用字段包括：

```
renderablePart, jHandle, rawcode, objectKind, unitPtr, unitIdentityProven,
gpuSkinLeaseBacked, sceneNode, meshPayloadPtr, contractJHandle, producerStage,
frameSerial, submittedFrameSerial, exactOwnerFrameSerial, exactSubmittedFrameSerial,
capturedWorldMatrix, positionInfo/payloadWord*, HasCompleteBacking, MatchesKey, ...
```

⇒ 与 `MakePaletteObjectKey(renderablePart, runtimeModelPtr, jHandle, rawcode, session, mapEpoch)` 所需输入**一致可用**
（`runtimeModelPtr` 位置可用 `entry.unitPtr`；若语义不成立则记 0，**不得臆造**）。

### 16.2 拟实施改动（生产路径，`:23495`–`:23497` 处）

1. 在 `entry` 完成 exact-submit（紧邻 `drawTimeSemanticProducerSubmittedCount++`）之后，
   按同一 `if (PaletteObjectEvidenceEnabled() && ActiveSession()!=0)` 短路条件调用：
   - `MakePaletteObjectKey(entry.renderablePart, /*runtimeModelPtr 待定*/0, entry.jHandle, entry.rawcode, ActiveSession(), <mapEpoch>)`；
   - `MakePaletteObjectFrames(RenderState::instance().getFrameIndex(), entry.submittedFrameSerial, 0, false)`
     （native 帧未知 ⇒ 记 unknown，**不猜**）；
   - `source = PaletteObjectSource::Unknown`、`hitKey = 0`（该处**无** `selectedPalette` ⇒ **诚实记 Unknown**，不得映射成 ArenaSlot/DrawTimeCaptured）；
   - `NoteServed(...)` + `NoteEnqueued(..., selectionCleared=false)`；
2. **不改** `War3TryAppendSemanticShadowPacket` 内既有块（避免破坏等价迁移基线）；本次只**新增**生产路径采集点；
3. 保留本次三项诊断字段（`appendEntered` 可留作回归观测），但**须在提交前评估是否移除**。

### 16.3 验收（严格）

- 判据：**`emitted > 0` 且 `watchCount > 0`**，并产出 `paletteObject` 非零的 `cpu-*.json`（可解析）；
- 须做：legacy 参考 + 宿主差分 + ≥2 条变异真跑 + **全门禁复跑**（`ninja -n` no work、静态全量、meson、预算门禁、域、生命周期、线程闸门、taxonomy、往返、成本）；
- 若 `emitted>0` 但 `source` 全为 Unknown ⇒ **如实报告**"来源未证明"，不得宣称链完整。

### 16.4 本轮结束时的树/现场状态（供下一轮续接）

| 项 | 值 |
| --- | --- |
| 已改源码 | 4 文件：`war3_palette_object_evidence_sink.h/.cpp`（诊断 API）、`d3d9_device.cpp`（入口计数 + 块到达计数）、`war3_frame_evidence.cpp`（头块 4 字段）；**均未提交** |
| 现场 DLL | 诊断构建 **36,287,928 B / `6EC8DF73C59F3371D07E3109…`** |
| 可回退 | `d3d9.dll.519AFA69_backup_20260918`（上一候选）；`EFF2F45A…` 可由当前源码重建前态 |
| 门禁 | **未复跑**；`ninja -C build32 -n` 仍列待重建测试目标 |

### 16.5 边界

- **主判据未达成**（仍无非零 `paletteObject` 导出）——本目标**尚不能标记完成**；
- 全门禁未复跑；未提交；未部署到稳定通道；未改用户视频设置；
- 隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 17. **修复策略修正：对象级采集点共有三个，不止一个**（14:40）

### 17.1 新事实

`MakePaletteObjectKey` / `MakePaletteObjectFrames` 在 `src` 内的**生产调用点**共 **3 处**：

| # | 文件:行 | 说明 |
| --- | --- | --- |
| 1 | `d3d9_device.cpp:22129` / `:22137` | 位于 `War3TryAppendSemanticShadowPacket` —— **已证本会话 48 次进入、48 次全被 canonical 门拒，块不可达** |
| 2 | `d3d9_war3_shadow.cpp:5234` / `:5241` | **尚未仪器化验证** |
| 3 | `war3_shadow_renderer_core.cpp:6817` / `:6822` | **尚未仪器化验证** |

旁证：`war3_palette_object_evidence.h:601` 注释自述"**三个生产采集点**由 `MakePaletteObjectKey()` 一律…"。

### 17.2 对 §16 方案的修正

- §16 拟在 `War3TryPopulateDrawTimeSemanticProducer` **新增第 4 个采集点** ⇒ **应先验证既有第 2、3 点**，
  否则可能重复插桩、或在错误的层次再挂一次（这与 §15 的教训同类：**先确定既有链为何不通，再谈新增**）；
- ⇒ **下一步改为**：给 `d3d9_war3_shadow.cpp:5234` 与 `war3_shadow_renderer_core.cpp:6817` **各加一个到达计数**（沿用本轮已有的
  `NotePaletteObjectAppendEntered()` 同款最小仪器模式），一次运行即可判定这两点是否被到达；
- 判读：
  · 若两点**都未到达** ⇒ 三个采集点全部不可达 ⇒ 修复应是**在生产实际路径新增**（§16 方向成立），
    但**必须先解释为何既有的三点都不执行**（否则新点同样可能不可达）；
  · 若某点**被到达**但计数全零 ⇒ 问题在 `Note*` 语义/短路（例如 `ActiveSession()==0` 或子门短路），
    修复方向完全不同（改采集点内部的调用条件，而非新增点）。

### 17.3 已确认的输入（供下一轮直接写）

- `PaletteObjectKey{ renderablePart, runtimeModelPtr, jHandle, rawcode, sessionGeneration, mapEpoch,
  deviceEpoch, lifecycleIdentity, identityWeak=true, epochUnknown=true }`；
- `PaletteObjectFrames{ renderFrame, manifestFrameSerial, manifestPublishRevision, recordFrameSerial,
  nativeFrameTag, manifestUnknown=true, nativeUnknown=true }`；
- `PaletteObjectSource{ None=0, ArenaSlot=1, ProducerSnapshot=2, PoseKernel=3, DrawTimeCaptured=4,
  PublishedRegistry=5, OwnedPartSnapshot=6, Unknown=7 }`；
- 身份证明种类 `PaletteObjectIdentityProofKind` **只允许**由 `PaletteObjectEvidence::DeriveIdentityProofKind()` 推导，
  **任何采集点不得直接写入**（注释明令，**不得**加 setter 后门）。

### 17.4 边界

- 主判据**仍未达成**；全门禁**未复跑**；未提交；现场 DLL 为诊断构建 `6EC8DF73…`（`519AFA69…` 已备份）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 18. **最终综合：三个采集点全在"被旁路的语义阴影路径"上**（14:55）

### 18.1 生产侧 `Note*` 调用点（全量检索，仅 3 处）

| # | 位置 | 调用 | 是否执行（本会话实测） |
| --- | --- | --- | --- |
| 1 | `d3d9_device.cpp:22220` / `:22224` | `NoteServed` / `NoteEnqueued` | **否**（已证块不可达） |
| 2 | `d3d9_war3_shadow.cpp:5252` | `NoteDrawn` | **否** |
| 3 | `war3_shadow_renderer_core.cpp:6828` | `NoteReject` | **否** |

判据：`NoteReject` 会**建立条目**（见 `war3_palette_object_evidence_test.cpp:349` 注释 "NoteReject // 建条目"）
⇒ 若点 3 执行过，`watchCount` 必然 > 0；实测 `watchCount = 0` ⇒ **点 3 未执行**。
（同时全部 `dropped*`/`closed*` 为 0，进一步排除"执行后被丢弃"。）

### 18.2 为什么三点都不执行 —— 旁路证据

`war3_d3d9.log` 中存在：

```
info: DXVK War3Shadow: semantic bypass keeps S1 terrain legacy capture   （出现多次）
```

- 三个采集点**全部位于语义（semantic）阴影路径**上；
- 本会话实际走的是 **draw-time persistent caster producer** 路径（`drawTimeSemanticProducerSubmittedCount = 514,979`）；
- ⇒ **语义路径被旁路 ⇒ 三个采集点整体不执行 ⇒ 对象级证据恒为零**。

### 18.3 最终结论（本项目 P0 取证阻塞的根因）

**对象级 palette 证据的全部插桩点都挂在语义阴影路径上，而取证会话实际运行在语义被旁路、改走 draw-time persistent caster 的模式下**；
因此**无论怎么配置（子门、arm、会话、地图、隔离与否）都不可能产出非零证据**。
这与以下全部实测一致：用户两轮为零、我方多轮隔离桌面为零、`emitted/watchCount/all-dropped` 全零、
`appendEntered=48 但 blockReached=0`、以及 `semanticSceneCanonicalGateRejectSkinned=48`。

### 18.4 修复的两条正路（择一，均须按等价迁移纪律）

1. **在活的路径上插桩**（推荐）：在 `War3TryPopulateDrawTimeSemanticProducer` 的 exact-submit 处
   （`:23495`–`:23497`）调用等价的 `NoteServed`/`NoteEnqueued`（`source=Unknown`、`hitKey=0`、native 帧记 unknown，**诚实记未证明**）；
   所需输入 `entry.renderablePart/jHandle/rawcode/unitPtr/submittedFrameSerial` **已核实齐备**（§16.1）；
2. **改为让语义路径生效**：查清 `semantic bypass` 的判定条件并使其在取证会话中不旁路（若该旁路是既定正确行为，则此路不可取）。

两条路都必须：legacy 参考 + 宿主差分 + ≥2 条变异真跑 + **全门禁复跑**；
验收判据：**`emitted > 0` 且 `watchCount > 0`**，并产出可解析的非零 `paletteObject` 导出。

### 18.5 边界（不变）

- **主判据未达成**：本目标**不可标记完成**；
- **全门禁未复跑**（`ninja -n` 仍列待重建测试目标）；未提交；未部署到稳定通道；
- 现场 DLL = 诊断构建 `6EC8DF73…`（前一候选 `519AFA69…` 已备份，可回退）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 19. **对 §18 的更正：那条日志是"S1 地形专用"，不是全局语义旁路**（15:05）

### 19.1 更正内容

- §18 据 `war3_d3d9.log` 的 `DXVK War3Shadow: semantic bypass keeps S1 terrain legacy capture`
  断言"语义阴影路径被旁路" ⇒ **该解读过宽、不成立**；
- 读码证明该日志属 **S1 地形专用分支**：其上下文为
  `:43982 War3S1TerrainCaptureDueRuntime(...)`、`:43987 m_war3S1TerrainCasterStash.clear()`、
  `:43990 m_war3S1TerrainStashBuiltFrameSerial` ⇒ 语义仅针对 **S1 地形保留 legacy 采集**；
  `:43994-43999` 还只是**最多打印 5 次**的提示日志。
- 另有 `:2530 War3SemanticBypassInlineRegistryPublishRuntime()`（env `DXVK_WAR3_SEMANTIC_BYPASS_INLINE_REGISTRY_PUBLISH`，默认 0）
  —— 这是**另一个**、与采集点无关的旁路（registry publish），**不是** §18 所指。

### 19.2 §18 中仍然成立的部分

- 三个生产 `Note*` 调用点**确实全部未执行**（这是**实测**：`watchCount=0`，且 `NoteReject` 若执行会建条目）；
- `appendEntered=48` 而 `blockReached=0`（**实测**）；
- `CanonicalGateRejectSkinned=48`、`CanonicalRejectNoWorldTransform=48`（**实测**）；
- 209 caster、514,979 次 draw-time 提交（**实测**）。

### 19.3 因此仍**未解释**的问题（下一轮）

- 既然语义单位路径在跑（209 caster），为何 `d3d9_war3_shadow.cpp:5252` 的 `NoteDrawn` 与
  `war3_shadow_renderer_core.cpp:6828` 的 `NoteReject` **一次都没执行**？
- 需要读这两处的**调用条件**（很可能各自带 `PaletteObjectEvidenceEnabled()` + 额外前提，或只对特定 stage/path 生效），
  以及 `NoteDrawn`/`NoteReject` 是否需要先有 `NoteServed`/`NoteEnqueued` 建立的条目；
- **在查明之前不得再下"某条路径被整体旁路"的结论**（本轮已因此更正一次）。

### 19.4 边界

- 主判据**仍未达成**；全门禁**未复跑**；未提交；现场 DLL 为诊断构建 `6EC8DF73…`；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 20. D 点（`NoteDrawn`）恒不执行的具体机制已查明（15:20）

### 20.1 代码（`d3d9_war3_shadow.cpp:4795-4807` + `:5229`）

```cpp
const bool paletteObjectEvidenceOn = PaletteObjectEvidenceEnabled();
uint32_t paletteObjectDrawnCascade = 4u;                 // 默认 4
if (paletteObjectEvidenceOn) {
  for (uint32_t paletteObjectCascade = 0u;
       paletteObjectCascade < cascadeCount && paletteObjectCascade < 4u;
       ++paletteObjectCascade) {
    if (m_shadowMapLayerViews[paletteObjectCascade]) {   // 第一个有 layer view 的级联
      paletteObjectDrawnCascade = paletteObjectCascade; break;
    }
  }
}
for (uint32_t c = 0; c < cascadeCount; c++) { ...
  if (paletteObjectEvidenceOn && c == paletteObjectDrawnCascade) { ... NoteDrawn(...) ... }
```

### 20.2 机制

- 若级联 0..3 中**至少一个**有 `m_shadowMapLayerViews[c]` ⇒ `paletteObjectDrawnCascade ∈ [0,3]` ⇒ 该级联迭代时**可命中**；
- 若 0..3 **全为空** ⇒ 保持默认 **4**；而 `c` 的取值范围是 `0..cascadeCount-1`（常规 `cascadeCount = 4`）
  ⇒ **`c == 4` 永不成立 ⇒ `NoteDrawn` 恒不执行**（与 `c` 的取值域无关地"安全地少报"）。
- 代码注释亦自述这是**刻意的少报策略**（"宁可少报，不得把没记录命令的级联当成已绘制"）—— **设计如此，不是 bug**。

### 20.3 含义

- D 点未执行**可能完全正常**（若本会话 0..3 级联无 layer view）⇒ **不能**据此断言路径被旁路；
- **待测**：本会话 `m_shadowMapLayerViews[0..3]` 是否有值（旁证指标：perf 报告的
  `semanticSceneReceiverHasCompleteShadowMap` / `...UsableDirectionalShadow`）。
  用户会话该指标为 **1**；我方隔离桌面会话**尚未读该指标**。

### 20.4 下一轮（明确、低成本）

1. 读我方会话 perf 报告的 `semanticSceneReceiverHasCompleteShadowMap`、`semanticSceneReceiverNoCompleteShadowMapCount`、
   `semanticSceneReceiverHasUsableDirectionalShadow`；
2. 若为 0/缺失 ⇒ **本会话阴影图未完整建立** ⇒ D 点不执行属预期；
   此时对象级证据的真正阻塞仍是 §15 的"生产路径不含采集点"，修复方向为 §16（在活路径插桩）；
3. 读 `war3_shadow_renderer_core.cpp:6828`（`NoteReject`）的调用条件（本轮未读，**尚未解释**）。

### 20.5 边界

- 主判据**仍未达成**；全门禁**未复跑**；未提交；现场 DLL 为诊断构建 `6EC8DF73…`；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 21. receiver 指标正常却仍无 D 事件 ⇒ 强化 §16 判断（15:35）

### 21.1 实测（我方会话最新 perf 报告 `war3_perf_report_auto_2026_09_18_13_01_55.html`）

```
semanticSceneReceiverHasCompleteShadowMap        = 1
semanticSceneReceiverHasUsableDirectionalShadow  = 1
semanticSceneReceiverNeedShadowMap               = 1
semanticSceneReceiverNoCompleteShadowMapCount   = 0
semanticSceneReceiverNoShadowMapImageCount       = 0
semanticSceneReceiverNoShadowMapSampleViewCount  = 0
semanticSceneReceiverReuseShadowMap              = 0
semanticSceneShadowCastersCount                  = 209
```

- **排除了** §20.3 提出的可能（"阴影图未完整建立 ⇒ D 点不执行属预期"）：阴影图**完整可用**；
- ⇒ §20.4 第 2 条的替代分支成立：D 点前置**应已满足**（级联 0..3 有 layer view ⇒
  `paletteObjectDrawnCascade ∈ [0,3]` ⇒ `c == paletteObjectDrawnCascade` 可命中），
  **但 `watchCount` 仍为 0 ⇒ `NoteDrawn` 实际未执行**；
- ⇒ 因此该 legacy 级联 caster 循环（`d3d9_war3_shadow.cpp:4809` 起）**很可能不是渲染这 209 个 caster 的路径**
  —— 与 §15/§16 的结论（活的路径是 draw-time persistent caster producer）**相互印证**。

### 21.2 当前证据链（逐条均为实测/读码，不含推测）

| # | 事实 | 来源 |
| --- | --- | --- |
| 1 | 记录器可被外部 arm；`armed=true`、`activeSession=1` | 控制面 status |
| 2 | `emitted/watchCount/全部 dropped+closed` 恒为 0 | 控制面 status（多轮） |
| 3 | append 函数仅进入 48 次，全部被 canonical 门拒（`blockReached=0`） | 入口计数 + perf |
| 4 | 生产提交 514,979 次走 `War3TryPopulateDrawTimeSemanticProducer`，其 append 无采集调用 | 读码 `:23495`–`:23497` |
| 5 | 生产侧 `Note*` 调用点仅 3 处，**全部未执行** | 全量检索 + `watchCount=0` |
| 6 | 阴影图完整可用、209 caster 在渲染 | perf 报告 |

⇒ 由 4+5+6 得到**当前最可信结论**：**对象级采集点所在的 legacy 语义路径，与这 209 个 caster 的实际渲染路径不是同一条**；
修复方向仍为 §16：**在生产实际路径（draw-time producer exact-submit）插桩**。

### 21.3 仍未解释（诚实保留）

- `war3_shadow_renderer_core.cpp:6828` 的 `NoteReject` 调用条件**尚未读**（下一轮）；
- "legacy 级联循环为何不渲染这些 caster" 的**直接证据**尚无（我没有该循环的执行计数）；
  若需铁证，应在该循环入口加到达计数（第三次最小诊断构建）—— 但**优先级低于 §16 的修复**。

### 21.4 边界

- 主判据**仍未达成**；全门禁**未复跑**；未提交；现场 DLL 为诊断构建 `6EC8DF73…`（`519AFA69…` 已备份）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 22. 最后一处采集点（`NoteReject`）门查明 —— §21.3 全部关闭（15:50）

### 22.1 代码（`war3_shadow_renderer_core.cpp:6805-6831`）

```cpp
PaletteObjectRejectReason namedRejectReason = PaletteObjectRejectReason::NotChecked;
if (ShouldNotifyPaletteSlotReject(recheckEvidence, namedRejectReason)) {   // 1) 需"值得上报的拒绝"
  const uint64_t evidenceSession = ActiveSession();
  if (evidenceSession != 0u) {                                            // 2) 已 arm
    ... MakePaletteObjectKey/MakePaletteObjectFrames ...
    PaletteObjectRecorder().NoteReject(key, namedRejectReason, frames);
  }
}
```

- 该点属于**拒绝通知**路径（仅当发生 palette-slot 拒绝**且**被 `ShouldNotifyPaletteSlotReject` 判为值得上报时触发）；
- ⇒ **它自身不产生"正向/服务"证据**；在"未发生此类拒绝"的会话中静默**属预期**，**不构成异常**。

### 22.2 三点职责最终厘清（全部实测/读码）

| 点 | 位置 | 职责 | 本会话未执行的原因 |
| --- | --- | --- | --- |
| S/E | `d3d9_device.cpp:22220/22224` | `NoteServed`/`NoteEnqueued`（**正向证据主源**） | 块不可达：48 次进入全被 canonical 门拒 |
| D | `d3d9_war3_shadow.cpp:5252` | `NoteDrawn`（绘制命令已记录） | 需 `c == paletteObjectDrawnCascade`；实测阴影图完整却仍无事件 ⇒ **该 legacy 级联循环很可能不渲染这些 caster**（§21） |
| Reject | `war3_shadow_renderer_core.cpp:6828` | 拒绝通知（**非正向证据**） | 本会话无此类拒绝 ⇒ **静默属预期** |

### 22.3 结论（稳定）

**要产出非零 `paletteObject` 导出，必须有 S/E 或 D 点执行**；而这两点在取证会话中对这 209 个 caster 都不执行。
因此 **§16 的修复方向（在生产实际路径 `:23495`–`:23497` 插桩）是当前唯一有依据的路径**。

### 22.4 边界（不变）

- 主判据**仍未达成** ⇒ 本目标**不可标记完成**；
- **全门禁未复跑**（`ninja -n` 仍列待重建测试目标）；未提交；未部署到稳定通道；
- 现场 DLL = 诊断构建 `6EC8DF73…`（`519AFA69…` 已备份，可回退）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。
---

## 23. 诊断改动的受影响门禁验证（16:05）

- 全目标构建 20/20 成功；`ninja -C build32 -n` = **no work**；
- `war3_palette_object_wire_roundtrip_test` → **`checks=116 failures=0 PASS`**（需两个 env；实测 arm 后 `emitted=5 / terminalEmitted=1`）；
- `war3_palette_object_evidence_cost_test` → **`COST_VERDICT=PASS checks=38 failures=0`**；
- `war3_frame_evidence_runtime_test` → **`runtime checks=723 PASS`**；
- `war3_frame_recorder_memory_control_test` → **`87 PASS`**；
- `war3_frame_recorder_defaults_0/1_test` → 输出 `argc==3` ⇒ 用法参数缺失（非失败）；
- ⇒ **诊断改动未回归证据子系统**；但**这不等于全门禁复跑**（静态全量/meson/预算门禁/域/生命周期/线程闸门/taxonomy 等尚未复跑）。

---

## 24. §16 修复补丁（**已核对全部局部名，可直接照抄**）（16:20）

### 24.1 已核对的可用局部（`War3TryPopulateDrawTimeSemanticProducer`）

| 局部 | 定义处 | 说明 |
| --- | --- | --- |
| `manifestFrame`（`uint64_t`） | `:23047` `visibleRegistry.getFrameNumber()` | **manifest 帧序号** |
| `currentRenderFrameIndex`（`uint32_t`） | `:23048` `RenderState::instance().getFrameIndex()` | **render 帧** |
| `m_war3GpuSkinMapEpoch` | `:23298`（`draw.mapEpoch = ...`） | **map epoch** |
| `entry.renderablePart` / `.jHandle` / `.rawcode` / `.unitPtr` / `.submittedFrameSerial` | 该函数内 | 键与帧域输入 |
| `war3::tools::evidence::` 命名空间 | `:23301` 已在本函数使用 | 可直接调用 |

### 24.2 补丁（插入到 `:23495` `drawTimeSemanticProducerSubmittedCount++` **之后**、`:23497` `appendExactSubmittedManifestRecord(entry);` 之前）

```cpp
    // 2026-09-18：对象级 palette 证据（挂到**生产实际路径**）。
    // 纪律：子门短路是第一层；未 arm（ActiveSession==0）不发事件，避免污染观察表。
    // 语义诚实性：本处**无** selectedPalette ⇒ source 一律 Unknown、hitKey=0、
    // native 帧不可得 ⇒ nativeFrameTag=0 且 nativeUnknown=true（不猜、不冒充证明）。
    if (dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled()) {
      const uint64_t evidenceSession =
          dxvk::war3::tools::evidence::ActiveSession();
      if (evidenceSession != 0u) {
        const auto paletteObjectKey =
            dxvk::war3::tools::evidence::MakePaletteObjectKey(
                reinterpret_cast<uint64_t>(entry.renderablePart),
                reinterpret_cast<uint64_t>(entry.unitPtr),
                reinterpret_cast<uint64_t>(entry.jHandle),
                reinterpret_cast<uint64_t>(entry.rawcode), evidenceSession,
                uint64_t(m_war3GpuSkinMapEpoch));
        const auto paletteObjectFrames =
            dxvk::war3::tools::evidence::MakePaletteObjectFrames(
                uint64_t(currentRenderFrameIndex), uint64_t(manifestFrame),
                0u, false);
        auto& paletteObjectRecorder =
            dxvk::war3::tools::evidence::PaletteObjectRecorder();
        paletteObjectRecorder.NoteServed(
            paletteObjectKey,
            dxvk::war3::tools::evidence::PaletteObjectSource::Unknown, 0u,
            paletteObjectFrames);
        paletteObjectRecorder.NoteEnqueued(
            paletteObjectKey,
            dxvk::war3::tools::evidence::PaletteObjectSource::Unknown, 0u,
            paletteObjectFrames, false);
      }
    }
```

### 24.3 待下一轮先核对的两点（**不要盲插**）

1. `entry.renderablePart` / `.unitPtr` / `.jHandle` / `.rawcode` 的**实际类型**（指针 or 整型）——
   若已是整型则去掉 `reinterpret_cast`；`unitPtr` 作为 `runtimeModelPtr` 是否语义成立**需确认**，
   不成立就传 `0u`（宁可少报）；
2. `entry.submittedFrameSerial` 与 `manifestFrame` 谁是"manifest 帧域"的正解 ——
   若不确定则 `manifestUnknown=true`（把 `MakePaletteObjectFrames` 的第 4 个布尔参数改为 true）。

### 24.4 插入后的验收（严格按目标）

- 构建（`-j2` Below Normal）→ 部署（先备份）→ 跑本诊断实验；
- 判据：**`emitted > 0` 且 `watchCount > 0`**，并产出可解析的非零 `paletteObject` 导出；
- 若 `source` 全为 `Unknown` ⇒ **如实报告"来源未证明"**，不得宣称链完整；
- 随后按等价迁移纪律复跑**全门禁**（静态全量、meson、预算门禁、域、生命周期、线程闸门、taxonomy、往返、成本）；
- 提交前评估是否移除本轮的三项诊断字段。

### 24.5 边界

- 补丁**尚未应用**（本轮只做核对与成文）；主判据**未达成**；目标**不可标记完成**；
- 全门禁未复跑；未提交；现场 DLL 为诊断构建 `6EC8DF73…`（`519AFA69…` 已备份）。

---

## 25. **重要更正：§24 的补丁字段假设不成立，禁止照抄**（16:35）

### 25.1 发现

核对 `War3DrawTimeVBEntry`（`d3d9_device.h:2635` 起）实际字段：

```cpp
uint64_t mapEpoch;      void* renderablePart;  uint32_t layerIndex;
uint32_t payloadWord108; uint32_t payloadWord11C; void* instanceIdentity;
void* meshPayloadPtr;   uint32_t contractJHandle;  void* sceneNode;
void* unitPtr;          void* worldObjectEntry;    int16_t producerStage;
... (position/index 域) ...
bool MatchesKey(const War3DrawTimeVBCacheKey& key) const { ... contractJHandle == key.jHandle ... }
```

⇒ 该结构**没有** `jHandle`、**没有** `rawcode`、**没有** `submittedFrameSerial`（只有 `contractJHandle`）。

但在 `:23480`–`:23496` 处代码使用 `entry.alphaTestEnabled`、`entry.objectKind`、`entry.jHandle`、
`entry.exactSubmittedFrameSerial`、`entry.packageLastSubmittedCaptureOrdinal`、`entry.packageCaptureOrdinal`
—— **均不属于 `War3DrawTimeVBEntry`** ⇒ **该函数内存在多个不同类型的 `entry`（多个 lambda）**。

### 25.2 结论（对 §24 的更正）

- §24.2 给出的补丁**假设** `entry.renderablePart / .unitPtr / .jHandle / .rawcode / .submittedFrameSerial` 同时存在于 `:23495` 处的 `entry`；
- **该假设不成立/未证实** ⇒ **补丁禁止照抄应用**（否则编译失败或语义错误）；
- §24.3 的核对点 1 因此**升级为阻塞项**：必须先确定 `:23495` 处 `entry` 的**确切类型**，再按其真实字段写补丁。

### 25.3 已确定可用的类型事实（仍然有效）

- `renderablePart`、`unitPtr`、`sceneNode` 均为 **`void*`** ⇒ 需 `reinterpret_cast<uint64_t>`；
- `mapEpoch` 为 `uint64_t`；`contractJHandle` 为 `uint32_t`；
- 局部 `manifestFrame`（`:23047`）、`currentRenderFrameIndex`（`:23048`）在函数作用域内（若插入点在同一 lambda 之外则可用）。

### 25.4 下一轮第一件事（**必须先做，不得跳过**）

1. 定位包含 `:23495` 的**那个 lambda** 的形参类型（读 `:23350`–`:23460` 之间的 lambda 头），
   列出其 `entry` 的可用字段（尤其：playable part 指针、jHandle、rawcode、帧序号）；
2. 若该处**没有** `rawcode`/`jHandle` 等价字段 ⇒ 按 §16 的原则"**宁可少报、不臆造**"，
   以可用字段构造键（缺失项传 0 **并接受 `identityWeak` 语义**），或改在拥有完整字段的采集点插桩；
3. 只有第 1、2 步都完成，才允许写补丁并构建。

### 25.5 边界

- 补丁**未应用**；主判据**未达成**；目标**不可标记完成**；
- 全门禁未复跑；未提交；现场 DLL 为诊断构建 `6EC8DF73…`（`519AFA69…` 已备份）；
- 本轮无残留进程、未改用户视频设置、未改源码。

---

## 26. **再次更正：§25 的"字段缺失"结论错误；§24 补丁重新生效**（16:50）

### 26.1 事实

`War3DrawTimeVBEntry` 的**完整**字段表（`d3d9_device.h:2635` 起，此前我只读到 `:2727` 就下了结论）：

```cpp
2637: uint64_t mapEpoch;              2638: void* renderablePart;
2651: void* unitPtr;                 2652: void* worldObjectEntry;
2777: bool alphaTestEnabled;         2788: uint32_t packageCaptureOrdinal;
2789: uint32_t packageLastSubmittedCaptureOrdinal;
2790: uint64_t submittedFrameSerial;
2799: uint64_t exactSubmittedFrameSerial;
2800: uint32_t rawcode;              2801: uint32_t jHandle;
2806: bool unitIdentityProven;       2809: bool gpuSkinLeaseBacked;
2813: ObjectKind objectKind;
```

⇒ `:23499`（原 `:23497`，因我在 `:19868` 加了 2 行而整体下移）处的 `entry` **就是 `War3DrawTimeVBEntry`**；
⇒ **§24 的补丁字段假设成立**；**§25 的"字段缺失"结论错误，予以更正**。

### 26.2 我错在哪里（方法教训）

- 我只读了该结构的**前 ~90 行**就断言"没有 `jHandle`/`rawcode`/`submittedFrameSerial`"；
- 实际这些字段在 `:2777`–`:2814`；**"局部读取未见" ≠ "不存在"**；
- 与今夜早先那次教训同类（聚合计数器不可归因单一调用点、子串检索会漏中间层）：
  **结论必须建立在"已划定边界并穷尽该边界"之上**，否则应标注为"未验证"而非"不存在"。

### 26.3 由此确定的补丁字段类型（§24.2 以此为准）

| 字段 | 类型 | 补丁中的用法 |
| --- | --- | --- |
| `entry.renderablePart` | `void*` | `reinterpret_cast<uint64_t>(...)` |
| `entry.unitPtr` | `void*` | `reinterpret_cast<uint64_t>(...)`；**语义是否等于 runtimeModelPtr 仍需确认**，不成立传 `0u` |
| `entry.jHandle` | `uint32_t` | 直接传（隐式转 `uint64_t`） |
| `entry.rawcode` | `uint32_t` | 直接传 |
| `entry.submittedFrameSerial` | `uint64_t` | 可作 manifest 帧域；若不确定则置 `manifestUnknown=true` |
| `m_war3GpuSkinMapEpoch` | （`draw.mapEpoch` 源） | `uint64_t(...)` |

### 26.4 仍**唯一**未决的一点

- `entry.unitPtr` 作为 `MakePaletteObjectKey` 的 `runtimeModelPtr` **是否语义成立** ⇒ **未确认**；
- 纪律：**无法证明就传 `0u`**（宁可少报），不得把"某个指针"冒充"runtime model 指针"。

### 26.5 边界

- 补丁**仍未应用**（本轮只做核对与更正）；主判据**未达成**；目标**不可标记完成**；
- 全门禁未复跑；未提交；现场 DLL 为诊断构建 `6EC8DF73…`（`519AFA69…` 已备份）；未改源码。

---

## 27. 补丁已应用/构建/部署，但**测量仍为零** ⇒ 补丁不充分，需再次仪器化（17:10）

### 27.1 本轮实际做了什么（可复现）

1. 编辑 `d3d9_device.cpp`：在生产路径 `:23498`（`entry.exactSubmittedFrameSerial = ...`）与 `:23499`（`appendExactSubmittedManifestRecord(entry);`）之间插入采集块；
2. **首次构建失败**（宝贵的签名证据）—— 编译器给出真实签名：
   `MakePaletteObjectKey(void* renderablePart, void* runtimeModelPtr, uint32_t jHandle, uint32_t rawcode, uint64_t session, uint64_t mapEpoch)`；
   我原按 `uint64_t` 传前两个参数 ⇒ 修正为 `entry.renderablePart, nullptr, entry.jHandle, entry.rawcode`；
3. 修正后构建成功（`[2/2] Linking`）；部署后现场 DLL = **36,287,928 B / `4A8F1484C543BE86665FE76F3ABD207294B83C24B79C4B3763BFA52B035EAAE1`**（与上一诊断构建 `6EC8DF73…` 不同 ⇒ 确为新构建）；
4. 运行（隔离桌面 ConcealedHill，pid 45460）：`arm ok=True`、`armed=True`、`sess=1`，
   `accepted` 1,311,705 → 3,580,653，`appendEntered` 22→46，**但 `emitted=0`、`watchCount=0`、`weakIdentityRecords=0`**。

### 27.2 结论（诚实）

- **补丁不充分**：插入点在 `drawTimeSemanticProducerSubmittedCount++` 之后（该计数确实在自增），但对象级计数仍全零；
- ⇒ **新块本身没有产生任何记录**；两种可能：
  · (a) 新块**未执行**（`PaletteObjectEvidenceEnabled()` 为假，或 `ActiveSession()==0` 在该时刻）；
  · (b) 新块执行了，但 `NoteServed`/`NoteEnqueued` 的效果**被随后的地图/会话转换清空**
    （`ClearPaletteObjectWatchlist` 清观察表 ⇒ `watchCount` 归零；但**无法解释 `emitted` 也为 0**）。
- **未定论**，不得声称修复有效。

### 27.3 下一轮（明确）

1. 在新块内**第一行**加到达计数（复用本轮已有的 `NotePaletteObjectEnqueueBlockReached()` 模式或新增 `NotePaletteObjectProductionInsertReached()`），
   并在头块暴露 ⇒ **一次运行即可判定 (a) 还是 (b)**；
2. 若 (a)：检查该处 `PaletteObjectEvidenceEnabled()` 与 `ActiveSession()` 的**实际取值**（同法加两个只读字段）；
3. 若 (b)：核对是否有地图/会话转换在采集之后清表（`d3d9_device.cpp:8699/13580/13607`），并据此调整插桩位置或重置语义；
4. **在 (a)/(b) 判定之前，不得再改语义**。

### 27.4 边界

- 主判据**未达成**；目标**不可标记完成**；
- **全门禁未复跑**；**未提交**；源码现含：诊断字段 + 本次生产路径补丁（均未提交）；
- 现场 DLL 为补丁构建 `4A8F1484…`（前两版可重建/回退；`519AFA69…` 备份在位）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 28. **关键收窄：生产插入点确实执行了 45 万次，但内层条件/Note 仍无效果**（17:30）

### 28.1 本轮改动

- 新增到达计数 `NotePaletteObjectProductionInsertReached()` / `PaletteObjectProductionInsertReachedCount()`；
- 调用点放在新块的**最前面、子门判断之前** ⇒ 用于分离"插入点未执行"与"子门/会话不满足"；
- 头块新增 `productionInsertReached`；构建成功；现场 DLL = **36,288,181 B / `F195AC23826FD412E72C…`**。

### 28.2 决定性读数（隔离桌面 ConcealedHill，pid 3372）

```
insertReached = 191,145 -> 292,420 -> 450,765   （持续增长，与 ~514,979 次生产提交同量级）
appendEntered = 16 -> 24
armed = True ; activeSession = 1 ; emitted = 0 ; watchCount = 0
accepted = 811,373 -> 1,628,740 -> 2,906,907
```

### 28.3 由此排除与剩余

**排除**：
- (a-1) "插入点未执行" ⇒ **排除**（45 万次执行）；
- "生产路径不承载流量" ⇒ **进一步排除**（该路径确实高频执行）。

**仅剩两种**：
- **(a-2)** 块内 `if (PaletteObjectEvidenceEnabled())` 或 `if (ActiveSession()!=0)` 在**该瞬间为假**
  —— 注意 `activeSession=1` 是**status 时刻**读到的值，**不能**代替绘制线程该瞬间的值；
- **(b)** `NoteServed`/`NoteEnqueued` **被调用但无效果**：可能因记录器被反复 `Reset`
  （`ArmPaletteObjectEvidence` 内部会 `Reset` **清计数**）、或地图/会话转换清表
  （`d3d9_device.cpp:8699/13580/13607`）。

### 28.4 下一轮（可一次定案）

在新块内**再加两个只读观测**（不改语义）：
1. 记录"内层两个条件同时成立"的次数（即 `NoteServed/NoteEnqueued` **实际被调用**的次数）；
2. 在 `PaletteObjectEvidence` 侧记录"被 Post/Reset 清表"的次数（或在 `Arm/Reset` 处计数）；
⇒ 若 ① 为 0 ⇒ 是 (a-2)（条件不成立）；若 ① > 0 而 `emitted=0` ⇒ 是 (b)（被清空/无发射），
   此时再看 ② 即可知清空来源。

### 28.5 边界

- 主判据**未达成**；目标**不可标记完成**；
- **全门禁未复跑**；**未提交**；源码含诊断计数 + 生产路径采集块（均未提交）；
- 现场 DLL 为 `F195AC23…`（前几版可重建/回退；`519AFA69…` 备份在位）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 29. **定案为 (b)：`Note*` 确实被调用 37.9 万次却零效果 ⇒ 记录被清空/未保留**（17:50）

### 29.1 决定性读数（隔离桌面 ConcealedHill，pid 46820；DLL `97B1D617…`）

```
insertReached = 264,733 -> 373,098 -> 505,092
noteCalled    = 139,092 -> 247,457 -> 379,451   <-- NoteServed/NoteEnqueued 实际被调用
appendEntered = 9 -> 25 -> 41
armed = True ; activeSession = 1 ; emitted = 0 ; watchCount = 0
```

### 29.2 判读

- `noteCalled` 的计数点位于**两层 `if` 之内**（`PaletteObjectEvidenceEnabled()` **且** `ActiveSession()!=0`），
  ⇒ **两个条件都通过**、`NoteServed`/`NoteEnqueued` **确实被调用**；
- ⇒ **排除 (a-2)**（"内层条件不成立"）；
- ⇒ **判定为 (b)**：**调用发生但记录器零效果**（`emitted`/`watchCount` 均为 0）。

### 29.3 最可能机制（下一步验证）

1. **记录器被反复 `Reset`/清表**：
   - `ArmPaletteObjectEvidence()` 内部会 `Reset(...)` ⇒ **清计数**（`war3_palette_object_evidence_sink.cpp:152`）；
   - `ResetPaletteObjectEvidence()` / `ClearPaletteObjectWatchlist()` 被 `d3d9_device.cpp:8699`、`:13580`、`:13607` 调用
     （地图/会话/设备转换点）⇒ **清观察表**；
   - 若这些调用**每帧或高频**发生，则"刚记入即被清"，采样时必然全零 —— **与观测完全吻合**（含 `emitted=0`：计数也被清）。
2. 次可能：`NoteServed`/`NoteEnqueued` 内部还有未察觉的前置（如窗口分段/`PrepareStage`），未满足则静默。

### 29.4 下一轮（一次定案）

- 在 `ArmPaletteObjectEvidence()`、`ResetPaletteObjectEvidence()`、`ClearPaletteObjectWatchlist()` **各加到达计数**并暴露；
- 判读：
  · 若 `Reset/Clear` 计数与 `noteCalled` **同量级或更高** ⇒ **确认为"高频清空"**，修复方向是**查明并收紧清空条件**
    （或让对象级记录器不被这些转换清空）——**这是真正的缺陷**；
  · 若 `Reset/Clear` 计数很小 ⇒ 转向 2（`Note*` 内部前置），逐层加计数。

### 29.5 意义

本目标至此**首次把失败点从"采集点不可达"推进到"记录被保留失败"**：
- §15/§27：采集点不可达（已修，见 `insertReached`/`noteCalled` 非零）；
- §29：**修好采集点后暴露出第二层缺陷 —— 记录写入后被清空**；
- ⇒ 两个缺陷**独立**，须分别处理；**均未修复完成**。

### 29.6 边界

- 主判据**未达成**；目标**不可标记完成**；
- **全门禁未复跑**；**未提交**；源码含 4 类诊断计数 + 生产路径采集块（均未提交）；
- 现场 DLL 为 `97B1D617…`（前几版可重建/回退；`519AFA69…` 备份在位）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 30. **§29 假设被否定：`resetOrClear` 仅 2 次 ⇒ `Note*` 自身静默无效**（18:10）

### 30.1 决定性读数（隔离桌面 ConcealedHill，pid 46268；DLL `F275545BAA65A0153804…`）

```
insertReached = 234,066 -> 360,215 -> 480,078
noteCalled    = 121,564 -> 247,713 -> 367,576   （NoteServed/NoteEnqueued 实际被调用）
resetOrClear  = 2 -> 2 -> 2                     （Arm/Reset/Clear 总计仅 2 次）
emitted = 0 ; watchCount = 0 ; weakIdentityRecords = 0
```

（`resetOrClear` 计数点在三处函数体首行，**先于**任何 guard ⇒ 计的是**调用次数**，故"仅 2 次"是**上界**。）

### 30.2 判读

- **§29 的"记录器被反复 Reset/清空"假设 ⇒ 否定**（2 次不可能是清空来源）；
- `noteCalled = 367,576`、`resetOrClear = 2` ⇒ **只剩一种解释**：
  **`NoteServed` / `NoteEnqueued` 被调用后静默无效**（其内部存在未满足的准入条件）；
- 结合 `emitted=0`、`watchCount=0`、`weakIdentityRecords=0` ⇒ 进入记录器的键/帧**未被接受**。

### 30.3 最可疑的具体原因（下一步查）

1. **键参数可疑值**：我在生产路径传的是
   `MakePaletteObjectKey(entry.renderablePart, nullptr, entry.jHandle, entry.rawcode, evidenceSession, uint64_t(m_war3GpuSkinMapEpoch))`
   —— 其中 `m_war3GpuSkinMapEpoch` **可能为 0**（若该时刻 map epoch 尚未建立或语义不同），
   或 `entry.renderablePart` 在 exact-submit 处可能为 null；
   ⇒ 若 `Note*` 要求 `mapEpoch != 0`/有效 part，则全部静默丢弃；
2. `MakePaletteObjectFrames(..., 0u, true)` —— 我把 `nativeUnknown=true` 且 `manifestFrame` 可能为 0；
   若 `Note*` 要求至少一个**已知**帧域，则同样会被丢弃。

### 30.4 下一轮（两条并行、均为只读）

1. **读 `NoteServed`/`NoteEnqueued` 实现**（`war3_palette_object_evidence.h:346` / `:392`），列出其**全部 early-return 条件**；
2. **把该处实际键值暴露出来**（在头块加 `productionLastMapEpoch` / `productionLastSession` / `productionLastPartNonNull` 三个只读字段），
   ⇒ 一次运行即可判定是"参数无效被拒"还是"另有条件"。

### 30.5 边界

- 主判据**未达成**；目标**不可标记完成**；
- **全门禁未复跑**；**未提交**；源码现含 5 类诊断计数 + 生产路径采集块（均未提交）；
- 现场 DLL 为 `F275545B…`（前几版可重建/回退；`519AFA69…` 备份在位）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 31. **根因彻底查明（代码级）：`NoteServed`/`NoteEnqueued` 不创建条目，条目只由 `NoteReject` 创建**（18:30）

### 31.1 决定性代码（`war3_palette_object_evidence.h:346-354`）

```cpp
void NoteServed(const PaletteObjectKey& key, PaletteObjectSource source,
                uint64_t hitKey, const PaletteObjectFrames& frames) {
  StateLock guard(*this);
  if (m_windowClosed) return;
  BeginFrame(frames.renderFrame);
  Entry* e = Find(key);
  if (e == nullptr)
    return;                     // <-- 键不存在 ⇒ 静默返回（不计 dropped、不发事件）
  ...
}
```

`NoteEnqueued`（`:392-401`）**同构**：同样 `Find(key)`；`e == nullptr` ⇒ 静默返回。

### 31.2 与全部观测逐项吻合

| 观测 | 由该机制解释 |
| --- | --- |
| `noteCalled = 367,576` 但 `emitted = 0` | 每次调用都在 `Find` 处静默返回 |
| `watchCount = 0` | 从未建立条目，观察表为空 |
| **全部 `dropped*` / `closed*` 精确为 0** | 静默返回**早于**任何计数点（连 `droppedDuplicatePerFrame`/`droppedPayloadConflict` 都不计） |
| `weakIdentityRecords = 0` | 无事件发出 |
| 用户两轮手动采集同样为零 | 其会话同样**未发生 A 链拒绝** ⇒ 同样无条目 |
| 三个采集点（S/E、D、Reject）中 Reject 也未执行 | 本会话无拒绝发生 ⇒ 无条目、也无 Reject 事件 |

### 31.3 因此完整的因果链

1. 对象级证据的**条目只在 A 链拒绝时创建**（`NoteReject`）；
2. 本会话（以及用户两轮）**没有发生该类拒绝** ⇒ **观察表始终为空**；
3. ⇒ 其余三个采集点（`NoteServed`/`NoteEnqueued`/`NoteDrawn`）**全部静默无效**；
4. ⇒ **无论子门、arm、会话、地图、隔离与否，都不可能产出非零证据**。

**这是设计层面的缺口**：采集点假设"条目已由拒绝路径建立"，但生产路径并不保证这一点。

### 31.4 修复方向（下一轮定夺，需语义决策）

- **(i) 让入队事件具备"创建条目"语义**：在生产路径为首次见到的键**显式建立条目**（等价于把"候选入队"作为该对象的**首次观测**），
  再发 `NoteEnqueued`。这符合"入队是真实发生的事实"，**不是**伪造拒绝；
- **(ii) 若记录器已有 ensure/首次观测 API**（需读 `Find`/`NoteReject`/`PrepareStage` 确认），则调用它；
- **(iii) 若设计上**要求**必须有拒绝才能登记，则应把采集点上移到 A 链拒绝处（即改变取证目标）**——
  但那时"对象级调色板链"的证据含义不同，须上级裁定。

### 31.5 下一步（具体）

1. 读 `Find(...)` 与 `NoteReject(...)`（`war3_palette_object_evidence.h` 内），确认**条目创建的唯一入口**；
2. 若有 ensure 语义 ⇒ 采用 (ii)；否则采用 (i) 并**明确注释**"首次观测建立条目"的语义；
3. 改完后一次运行验收：**`emitted > 0` 且 `watchCount > 0`**，并产出非零 `paletteObject` 导出；
4. 随后按等价迁移纪律复跑**全门禁**。

### 31.6 边界

- 主判据**未达成**（尚未修复）；目标**不可标记完成**；
- **全门禁未复跑**；**未提交**；源码含 5 类诊断计数 + 生产路径采集块（均未提交）；
- 现场 DLL 为 `F275545B…`（前几版可重建/回退；`519AFA69…` 备份在位）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 32. **确证：条目创建唯一入口是 `Insert`，且只被 `NoteReject` 调用；不得伪造拒绝**（18:45）

### 32.1 代码（`war3_palette_object_evidence.h:274-314`）

```cpp
void NoteReject(const PaletteObjectKey& key, PaletteObjectRejectReason reason,
                const PaletteObjectFrames& frames) {
  StateLock guard(*this);
  if (m_windowClosed) return;
  BeginFrame(frames.renderFrame);
  Entry* e = Find(key);
  if (e == nullptr) {
    e = Insert(key, frames.renderFrame, reason, frames);   // <-- 唯一创建点
    if (e == nullptr) { ... m_counters.droppedTableFull++; ... return; }
  }
  ...
}
```

⇒ **条目只能由一次"拒绝"建立**；`NoteServed`/`NoteEnqueued`/`NoteDrawn` 都不创建条目。

### 32.2 因此不能走的两条路

- ❌ 用 `NoteReject(key, PaletteObjectRejectReason::NotChecked, frames)` **去凑登记** ——
  头文件注释**明令禁止**：`NotChecked = 该检查未执行（**不得**用 R0 冒充）`；
  且这会把"未检查"伪装成"已拒绝"，直接污染证据语义；
- ❌ 用 `Unknown` 拒绝原因同理（伪造一个并不存在的拒绝事实）。

### 32.3 因此唯一正路

**新增一个"首次观测即登记"的公开 API**（语义诚实、可判读），例如
`NoteFirstSight(key, frames)` 或在 `NoteEnqueued` 内于 `Find` 失败时 `Insert`，并**明确其语义**：
"该对象作为 CPU caster 候选**首次进入**观察"（`PaletteObjectStage::Enqueued` 作为**首次观测阶段**，
而不是伪造 `Rejected`）。

具体落法需与既有解析器契约对齐（`AutoTest/analyze_palette_object_evidence.py` 对阶段的推断顺序），
⇒ **下一轮先读解析器的阶段/链推断规则，再定 API 形态**（避免"改完仍被判为不可判读"）。

### 32.4 完整根因（最终形态）

```
对象从未被拒绝  →  没有条目  →  NoteServed/NoteEnqueued/NoteDrawn 全部静默 return
                →  emitted/watchCount/全部 dropped 精确为 0
                →  与用户两轮 + 我方全部实测完全一致
```

关键：**这不是配置问题，也不是采集点位置问题（那已修好），而是"登记前提"的设计缺口**。

### 32.5 边界

- 主判据**仍未达成**；目标**不可标记完成**；
- **全门禁未复跑**；**未提交**；
- 现场 DLL 为 `F275545B…`（前几版可重建/回退；`519AFA69…` 备份在位）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。

---

## 33. **解析器契约禁止"首次观测即登记" ⇒ 修复需改证据链契约（须上级裁定）**（19:00）

### 33.1 决定性证据（`AutoTest/analyze_palette_object_evidence.py`）

```
 10: 2. is each object's chain complete in the export (Rejected -> ServedCandidate ->
 11:    Enqueued -> Drawn -> terminal), rebuilt from the event sequence?
463: i_reject=stage_index(events,'Rejected')
464: i_served=stage_index(events,'ServedCandidate')
465: i_enqueued=stage_index(events,'Enqueued')
474: issues.append('servedCandidateBeforeRejected')
476: issues.append('enqueuedBeforeServedCandidate')
478: issues.append('drawnBeforeEnqueued')
496: if saw_submit and i_enqueued is None: truncation.append('submitFlagWithoutEnqueuedEvent')
```

⇒ 解析器**要求链以 `Rejected` 开头**、阶段必须严格有序；
⇒ 若按 §32.3 让"首次观测"直接产生 `Enqueued`，则：
  · 缺 `Rejected` 头 ⇒ 链不完整；
  · 触发 `enqueuedBeforeServedCandidate`（无 ServedCandidate 前置）⇒ **判为 orderViolation**。

### 33.2 因此 §32.3 的"唯一正路"**不成立**（被契约否决）

- 采集点侧的登记**无法**在既有契约下产生"可判读的正向证据"；
- 三条路全部关闭：
  · 伪造 `NoteReject` ⇒ **头文件明令禁止**（`NotChecked` 不得冒充 R0）；
  · 首次观测直接发 `Enqueued` ⇒ **解析器判 orderViolation**（本轮新增证据）；
  · 保持现状 ⇒ **恒为零**（§31 已证）。

### 33.3 因此这是**契约层缺陷**，不是实现层缺陷

- 现有"对象级调色板证据"模型的**隐含前提**是：**每个被观察对象都先经历过一次 A 链拒绝**；
- 该前提在"阴影链健康、无拒绝"的会话中**不成立** ⇒ 证据模型对"一切正常"的场景**天然失明**；
- 修复它必须**扩展证据链的首阶段语义**（例如新增"首次观测"阶段并同步修改解析器的阶段顺序与完整性判定），
  这属于**证据 wire 契约变更**；
- 而本项目纪律明确规定：**证据语义不得自行发明**（`war3_palette_object_evidence.h` 多处注释引自"上级裁定"），
  wire 载体 words32 的位分配亦受既有登记表约束（`IDENTITY_PROOF_KINDS` 等）。

### 33.4 结论：**需上级裁定后才能继续**

需裁定的事项（明确、可一次答复）：
1. 是否为证据链新增"**首次观测**"阶段（或允许 `Enqueued` 作为链首），并接受解析器契约的相应变更？
2. 若不允许 ⇒ 是否改变取证目标（例如以 **A 链拒绝**为观测对象，而非"正常 caster 的对象级链"）？
3. 若允许 ⇒ 新阶段的**位/枚举/顺序**如何登记（须与读方登记表逐项一致）？

在取得裁定前，**不得**改动证据 wire 语义或解析器契约。

### 33.5 边界

- 主判据**未达成**；目标**不可标记完成**；
- **全门禁未复跑**；**未提交**；源码含 5 类诊断计数 + 生产路径采集块（均未提交）；
- 现场 DLL 为 `F275545B…`（前几版可重建/回退；`519AFA69…` 备份在位）；
- 未改用户视频设置；隔离桌面数据不作前台性能；CPU 侧数据不作 GPU 提交/像素证据。
