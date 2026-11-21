# x64 迁移阶段 0：对齐基线

2026-11-13，Claude（Opus 5.5）执行，按 [整体路线](../plan/2026-11-13-x64-migration-roadmap.md) 阶段 0。
10-31 的 S2 第一组之后，主线合入了 1.22.02 与 1.22.03。本阶段确认开发路径没有被破坏，并把 1.22.03 新增的
x86 → 设备事件登记进传输合同的阶段标记清单。没有接游戏，没有部署玩家 DLL，没有提交、推送或发布。

## 1. 现场

- 源码：main `a7e1cba`（= origin/main，含 v1.22.03 标签 `c224a8c` 与发布回执）。工作树只多了路线文档与本回执；
  `subprojects/StormBreaker` 的用户未提交修改不进入构建。
- **玩家目录的 `d3d9.dll` 已不存在。** 1.22.03 发布回执最后一次复核（11-12 15:09 前后）时它还是
  `1FFE0BD1…35CF`；`war3_d3d9.log` 显示 15:13 游戏以 `DXVK: 1.22.01` 字样的 DLL 跑过一次，之后 15:10–15:23
  目录里新装了 `War3Tools`（`War3Tools1.11.mix`、`War3Tools/`、快捷方式），目录最后修改 15:23:42，
  此后不再有 `d3d9.dll`。本会话没有动过该目录；本阶段不恢复、不部署。
- 因此构建脚本与 GPU 参照门的保护清单改为“缺失也记录”：`guarded_identity()` 对不存在的受保护文件记
  `{"path", "exists": false}`，结束时再比一次，**缺失必须仍然缺失**，出现任何文件都算改动；存在的文件照旧比
  字节数与 SHA-256。改了 `run_device_owned_build.py`、`run_render_host_candidate_build.py` 与
  `run_d3d9_factory_clear_gate.py` 的冻结输入两处，没有测试固定这些行。
- **系统 Vulkan 加载器换过。** 10-23 钉住的是 `C:\Windows\System32\vulkan-1.dll` 1.4.341（1,730,096 B，
  `236BA2BB…D0E9`），10-31 的门仍用它通过；现在是 1.4.357（1,695,280 B，`542D7CB4…985F`，文件时间戳保留安装包的
  10-21，推测随显卡驱动更新替换）。这是系统组件，不还原。

## 2. 结果

全部按顺序运行，BelowNormal、`-j2`，同一时刻只有一个构建或门。

| 回执 | 结果 |
|---|---|
| `x86-phase0-build-20261113-r1` | `build32_device_owner_phase0_20261113`，配置 14 s、ninja 446 s，精确目标再跑为 no-work。DLL `128191FE…F212`，24,563,712 B（比 10-31 的 `67DD5D8F…` 多 143,360 B，即 1.22.02/03 的新代码）。保护清单：`build32_weather_20261010` 不变，玩家 DLL 记为缺失且构建后仍缺失。回执 `C1E80BDE…4F9E` |
| `x86-data-layer-cut-20261113-r1` | **ratchet 失败，保留**（`EB6053B2…2A72`）："forward cut gained symbols"，正向 104 → 106 个符号、46 → 48 条边，删掉 0 |
| `x86-data-layer-cut-20261113-r2` | 同一构建、同一 10-31 基线、不带 `--ratchet`，作为**新基线**（`C9E2C209…0848`）。delta 与 r1 逐项相同。数据层对象 114 → 118；反向 477 → 485 个符号、105 → 111 条边 |
| `x64-phase0-host-build-20261113-r1` | `build64_render_host_phase0_20261113`，配置 11 s、ninja 148 s；宿主 `350FFF77…BC5C`，5,910,528 B；源码与保护清单前后一致。回执 `C54C9111…7FB5` |
| `x64-phase0-reference-gpu-vvl-20261113-r1` | **失败，保留**（`72CFEBC3…8847`）：冻结输入里的玩家 DLL 不存在，未启动任何用例 |
| `…-r2` | **失败，保留**（`74D74F39…D64B`）："pinned VVL identity loader"，未启动任何用例 |
| `…-r3` | **失败，保留**（`E5DB9E2C…23A7`）：加了 `--repin-loader` 后跑完 wire/着色器记号/特化/采样器单测与前两个像素用例（均与 10-31 相同，VVL 0 错误），在“加载器版本不在冻结诊断合同内”处停下 |
| `…-r4` | **通过**（`5E9045F1…5B7A`）。主路径 6 例、resize 7 例，13 个像素哈希与 10-31 逐个相同：主路径 `03f7265ab853783c`；resize `b3ae0d54a133b4b5`、`c31a6277983e7c78`，呈现帧 `f476c78366fbd725`、`be1ad3ff9b018725`。7 个远端用例 VVL 0 错误、0 个非状态警告；故障用例通过；32/64 位 wire 各 1,455,523 项、着色器记号 6,560、特化与采样器键各 200,000 项。冻结输入与原始产物前后一致 |

静态与单元测试：S2 时的 `test_native_d3d9_survey_static.py`、`test_shadow_non_taa_continuity_static.py` 仍是 45 项通过；
改过的门所涉及的 `test_d3d9_draw_batch_reader.py`、`test_d3d9_stream_metrics.py`、`test_render_lab_layer_source.py`、
`test_render_host_validation_gate.py` 共 86 项通过。

### 加载器换钉

`run_d3d9_factory_clear_gate.py` 新增 `--repin-loader`（必须与 `--validation` 一起用）：

- 只放开系统加载器一项。VVL 层清单与层 DLL 仍必须与 10-23 钉住的身份逐字节相同。
- 回执 `loaderRepin` 同时记下旧、新两个身份和 `changed`；新身份进入冻结输入，跑完再比一次。
- 加载器诊断合同按 v1.4.341 的隐式层发现日志写成。1.4.357 的实际日志里四处
  “Searching for implicit layer manifest files / In following locations / Found no files”结构不变，空源检查在版本检查
  之前已经通过（r3 即停在其后）。换钉时期望版本改为读冻结加载器文件自身的 `VS_FIXEDFILEINFO`（1.4.357），
  不信日志自报。`check_loader` 新增可选参数 `loader_version`，不传时仍是 1.4.341，其他门行为不变。
- `run_d3d9_draw_batch_gate.py` 也引用同一钉住清单，这次没有改它；下次用它时会同样被拒，需要照此处理。

### 正向切口新增的两个符号

| 符号 | 边 | 来源 | 归类 |
|---|---|---|---|
| `D3D9DeviceEx::War3SetWorldLightingScope(bool)` | `war3_hook_render.cpp` → `d3d9_device.cpp` | 1.22.03 世界光照作用域 | 设备内部入口；代理模式由 WorldSceneEnter/Exit 标记提供（§3 E1/E2） |
| `startup_report::snapshot()` | `war3_startup_error.cpp` → `dxvk_startup_report.cpp` | 1.22.02 启动诊断 | 启动失败报告读 DXVK 的适配器与设备创建记录。代理模式下设备在宿主创建，报告内容要由宿主回传（握手或故障消息里带上），x86 只负责弹窗与写文件。归“设备内部入口” |

反向新增 8 个：1.22.02 的 `ReportStartupFailure`、`InitVramPreset`、`CurrentVramPreset`、`VramTierName`（启动诊断与显存档位），
1.22.03 的 `IsWorldRenderSceneActive`（§3 E3/E4）、`NoteFrameEvent`（逐帧阶段序列诊断）、`player::ApplyAtStartup` 与
`SetNativeShadowMode`（特效管线启动时套用 `settings.ini`）。后两项在代理模式下应改为 x86 读设置后经设置通道推给宿主。

## 3. 1.22.03 新增的 x86 → 设备事件

对照 `v1.22.02..a7e1cba` 的 `src/d3d9` 差异。1.22.03 的三项修复都让设备在每次 draw 或 Present 时直接读
x86 侧的静态状态，或经 `RunWithActiveDevice` 直接调设备。代理模式下宿主读不到 x86 的静态量，也拿不到
`D3D9DeviceEx&`，这些都要变成**与 draw 流同序**的消息。现有阶段标记只有
`WorldBegin/BeforeUi/UiBegin/FrameEnd`（`tools/render_host/d3d9_annotation_wire.h`），产品里还没有调用方。

| # | 事件 | x86 侧来源 | 进程内现状 | 代理模式下的形态 |
|---|---|---|---|---|
| E1 | 世界 `RenderScene` 进入 | `Hook_WorldRenderScene`（`war3_hook_render.cpp`）调原函数之前：`OnWorldRenderSceneEnter`、`native_light::BeginWorld`、`SetDeviceWorldLightingScope(true)` | `RunWithActiveDevice` → `D3D9DeviceEx::War3SetWorldLightingScope(true)`：作用域 Outside→World，按需重套光 0/环境光/材质 | 新阶段标记 **WorldSceneEnter**。必须在本场景第一个世界 draw 之前到达宿主；标记按合同先冲刷暂存批，顺序由调用顺序保证。宿主收到后切作用域并重套 |
| E2 | 世界 `RenderScene` 返回 | 原函数返回后：`OnWorldRenderSceneExit`（记 `g_lastWorldRenderSceneFrameStamp`）、`native_light::EndWorld`、`SetDeviceWorldLightingScope(false)` | 同上，World→Outside，头像等之后的模型用游戏原值 | 新阶段标记 **WorldSceneExit**。同时是 E3/E4 的输入 |
| E3 | BeforeUi 第 10 层分界 | 无独立调用 | 设备每次 draw 读 `War3RenderState::HasCompletedWorldRenderSceneThisFrame()`、`IsWorldRenderSceneActive()`（x86 静态量），再与设备自己的 `haveWorldInput`、`HasInsertedBeforeUi()` 合成 `War3IsWorldSceneEndBoundary` | **不需要新消息**：宿主由“本帧收到 WorldSceneExit 且未再 Enter”得到前两项，后两项本来就是执行端状态。分界在 WorldSceneExit 之后的第一个 draw 之前触发 |
| E4 | 帧末兜底守卫 | 无独立调用 | Present 时 `War3FrameEndFallbackAllowed(sceneCompleted, m_war3DrawsAfterWorldSceneThisFrame, override)`；计数在设备里按 draw 累加，但累加条件又读 x86 的 `IsWorldRenderSceneActive()` | **不需要新消息**：宿主在 WorldSceneExit 之后自己数 draw，FrameEnd/Present 时判定。A/B 开关 `DXVK_WAR3_BEFOREUI_FRAME_END_AFTER_UI` 要随设置进宿主 |
| E5 | 换图信号 | `ObserveWar3MainRunnerEnter`（`d3d9_war3_hook.cpp`）：`IsWorldRenderSceneLive()` 过滤后经 `War3MapTransitionGate`；另有 Native 表重建、退出对局两条 | `RequestActiveDeviceShadowMapResetOrCpuFallback()` → `War3RequestShadowMapEpochReset()`（设备内部入口）；无设备时走 CPU 语义图回退 | **不是阶段标记**，是会话事件：可能来自非渲染线程，进程内也是由 Present 合并后应用。x86 记下请求，在下一次 Present 消息之前发一条有序的“会话重置请求”（带换图代次）。`worldLive` 过滤所需的两个帧序号都在 x86（`g_presentedFrameSerial`、`g_lastWorldRenderSceneFrameStamp`），过滤留在 x86 |

说明：

- 1.22.03 新增的正向链接符号只有 E1/E2 的 `D3D9DeviceEx::War3SetWorldLightingScope`（数据层 → 设备内部入口）。
  E3/E4 是设备读数据层，属于反向面；E5 的 `War3RequestShadowMapEpochReset` 在 1.22.02 就已存在（已在基线里）。
- 钩子从未触发时设备保持 Unscoped（全程改写，旧行为）；宿主同样以 Unscoped 起步，直到第一个 WorldSceneEnter。
- 1.22.03 的另外两项不产生设备事件：面板设置保存只在 x86 读写 `settings.ini`（设置本身走已有的设置邮箱）；
  逐帧阶段序列 `DXVK_WAR3_FRAME_SEQUENCE_TRACE` 是设备写 x86 的诊断（`NoteFrameEvent`），归“调试”类，随 S2 调试组处理。
- 体积光的分辨率、近层比例、引导图分档与默认值都是执行端参数，随阶段 4d 迁移，不涉及新事件。

## 4. 对后续阶段的输入

- 阶段 1（传输层）：`Stage` 枚举加 WorldSceneEnter/WorldSceneExit；加一条会话重置请求消息（不是 StageMarker，
  因为它的语义是“下一次 Present 前合并应用”，不是 draw 流中的位置）。
- 阶段 2（设备接口）：`War3SetWorldLightingScope` 与 `War3RequestShadowMapEpochReset` 都属于“设备内部入口”，
  进程内由现有实现提供，代理模式由上面两种消息提供。
- 阶段 4a（BeforeUi 模板）：第 10 层分界与帧末兜底的判定搬到宿主时，输入只剩 WorldSceneEnter/Exit 两个标记
  和宿主自己的 draw 计数；`render_scope_contracts` 里的 32 行分界表可以原样用作宿主侧单元测试。
