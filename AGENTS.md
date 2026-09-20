# WarVK / DXVK Agent Guide

> 最后整理：2026-08-09。本文是后续 agent 的快速入口，不记录逐轮实验、历史改动或路线图；
> 需要追溯时按需检索 [历史归档](docs/agent-history/README.md)。

## 项目是什么

WarVK 是一个面向 **Warcraft III 1.27a** 的 Windows 图形增强项目。它从 DXVK 派生，
以 32 位 `d3d9.dll` 将游戏的 D3D9 路径接到 Vulkan，并增加阴影、光照/体积效果、
后处理、运行时诊断及供地图作者使用的 WarVK JAPI。

项目只改变渲染和诊断，不应改变地图或游戏玩法。长期架构方向是从 Warcraft 上层运行时取得
对象身份、模型、姿态、材质和 draw-time 几何来构建阴影场景；旧 D3D9 draw capture/replay
仍是兼容与诊断路径，不可把它误认为唯一真相。

## 代码地图

| 位置 | 负责内容 |
| --- | --- |
| `src/d3d9/` | D3D9 设备、交换链、Present、WarVK 主运行时和游戏接入层。 |
| `src/d3d9/war3/render/`、`shadow/` | ShadowMap/receiver、最终 replay 验证、渲染阶段与资源发布。 |
| `src/d3d9/war3/hooks/`、`bridge/`、`native/` | Game.dll/JASS Hook、逻辑层到渲染层的对象语义桥及原生接口。 |
| `src/d3d9/war3/gpu_skin/`、`memory/`、`model/` | GPU skin、Arena/静态包、模型与资源生命周期。 |
| `src/d3d9/war3/japi/`、`math/` | 受限的 WarVK JAPI v1、数学表达式和曲线运行时。 |
| `WarVK/` | YDWE catalog、JASS 包装、地图作者文档与打包脚本。 |
| `AutoTest/` | 发布级自动化、性能基线、运行时取证；历史一次性脚本位于其 `_archive/`。 |
| `docs/` | 生命周期、逆向证据和专项设计资料；带日期的 plan/research 文档不是当前状态本身。 |

基础生命周期见 [docs/WAR3_LIFECYCLE.md](docs/WAR3_LIFECYCLE.md)，自动化边界见
[AutoTest/README.md](AutoTest/README.md)。这两份资料用于定位入口；涉及跨地图资源或阴影时，
仍须以当前代码和测试为准。

## 渐进式架构收敛（长期约束）

- 修改相关模块时，必须按[渐进式架构收敛计划](docs/plan/2026-09-15-incremental-architecture-convergence.md)
  识别本次数据链、决策所有者及可顺带闭合的边界；只处理与本问题直接相关、能独立验证的债务，禁止借机全库重构。
- 优先减少决策入口与旁路，而非仅拆文件或新增 Manager。实际几何、姿态、材质与其来源、坐标空间、
  身份和有效期必须一起交接；下游不能换数据却沿用旧证明。逐步让消费者只接收验证入口签发的受限凭据。
- 明确模型、帧姿态、地图/设备代际和 GPU 最后使用的不同寿命；不可用一个 generation 或 producer fence 替代全部证明。
  保留所有者线程、Present 安全点及现有 DXVK 资源跟踪，不另建未经证明的平行生命周期系统。
- 校验失败继续 fail-closed，但必须同时验证合法对象持续绘制及故障后的恢复；不绘制、漏投影或全关特性不算修复。
  旧兼容路径仅在覆盖与等价证据闭合后逐项退役，不能一刀切删除或无责任边界地永久叠加旁路。
- 验证须包含实际生产代码的交接测试，并分别标明静态/模型、CPU、GPU、玩家视觉证据；
  热路径改动检查帧耗时、尾延迟、分配/拷贝/锁与内存增长，诊断关闭开销和开启预算分别说明。
- 每个 checkpoint 在开发日志说明解决的问题、收紧的边界、验证与剩余风险；不能安全顺带处理的债务记录原因和再处理条件。
  本规则不扩张构建、部署、实机或发布授权；文档目标不等于功能已实现，适用细节以链接计划为准。

## 当前状态

- 2026-09-20 Stage11三流寿命分页FAC候选已有本次玩家移动/各区域阴影恢复正向反馈，默认384MiB与GPU退休保留；正式配置、组合长时/性能及发布包尚未完成，不外推全面修复。现进入[发布收尾](docs/plan/2026-09-20-v122-release-closeout-checklist.md)，[离线证据](docs/plan/2026-09-20-stage11-production-lifetime-recovery.md)与旧失败分别保留。

- 2026-09-20 玩家 E42 回归仍约90%记录帧生产不完整、快照页池384MiB，末459帧阴影图停更；UV小修没有解决全屏失影。本轮没有新逐页census，长期M3/M4/跨Pass及生产分页仍未完成，见[玩家复核与计划偏差](docs/research/2026-09-20-e42-player-shadow-loss-and-night-progress-review.md)。

- 2026-09-20 夜班已闭合 S2 局部成本、快照页账户、UV 引用寿命与离线门禁；两次完整隔离路线仍压力恢复失败，P2a 无命中、P3 仅纯组件未接生产。新 DLL 仅离线诊断候选，禁止冒充已修复/发布；详见[夜班收口](docs/agent-history/2026-09-20-night-shift-closeout.md)。

- 2026-09-19 预算 P1b 轻量页账户与 P2a 上传索引范围候选已实现、默认关闭并通过离线门禁；尚无实机减量/阴影恢复证明。不得提高预算或减少投影对象代替修复；P3 回收策略等待真实账户，合并/发布仍阻塞。见[本批边界](docs/plan/2026-09-19-stage11-budget-p1b-p2a-implementation.md)。

- 2026-09-19 P0/P1a 首批已完成离线候选：报告重复 emitter、profile 只缩小、失败捕获寿命/占用账及 backing 解除已修正；87 项 Meson / 265 个静态脚本通过。随后一次隔离 canary 未通过地图 ready，已恢复原现场并清场；页 census、范围放大和压力后阴影恢复仍未闭合，禁止据此合并/发布。见[离线边界](docs/agent-history/2026-09-19-memory-recovery-p0-p1a-checkpoint.md)与[本次实机/恢复记录](docs/agent-history/2026-09-19-pressure-canary-and-merge-hold.md)。
  同日已按用户要求交付桌面测试包（未自动部署）；玩家自行测试期间不主动覆盖现场或并发编译/实机，等待反馈。包身份、关闭重型取证的测试配方和回退说明见开发日志。
  同日玩家回归已确认 BF938 仍会全场景丢影、移动后恢复并再次消失；九份报告严格解析与去重通过，池容量拒绝/完整性早退/阴影图停更相符，压力恢复门未通过。页持有者与范围放大仍待收口，禁止合并/发布；见[调查](docs/research/2026-09-19-bf938-player-shadow-loss-report.md)。

- 2026-09-19 当前长期重构的统一进度、基础收尾及验收缺口见
  [进度基线](docs/plan/2026-09-19-longterm-progress-and-foundation-closeout.md)；它区分实现与验收，
  下列日期条目属于历史 checkpoint，不应拼成当前全量通过。资源压力后的阴影恢复仍未闭合，不能晋升稳定。
  Pro 完整审核后的执行优先级见[修订计划](docs/plan/2026-09-19-post-pro-review-memory-recovery-plan.md)：
  先修可信账户/失败所有权与范围放大，再决定回收策略。用户已明确 **v1.22 不推出 64 位更新**：
  CPU 取证外置和完整渲染宿主均延期，保留研究、不接产品；当前只做基础收尾与阴影恢复。

- 2026-09-17 长线收敛 S1 checkpoint（**内部诊断候选，未部署、未实机、未提交、非稳定版**）：
  ① 四项活跃路径纯计数（append 入口/canonical 拒绝/两条 draw-time 提交分母）已落地，**未改任何准入**；
  ② **线程关系证明推翻了 A5 的「同线程」前提**——控制面 drain 在命名管道分离线程上推进构建，
  而 A5 的槽位区间读是无锁普通读，故此前 A5 的通过结论不能作为证据；**线程修复两部分均已落地**：
  所有者门设在两处推进入口（非所有者/所有者未建立一律只发请求不推进，防直接调用绕过），
  且构建进度改为锁内发布的**不可变摘要**（reader 不再读 live 字段），并有 9 例生产边界测试与变异验证门禁；
  槽位缓存写者审计显示写者均属主循环/Present 所有者线程族。
  **但按上级口径只能写「已观察到所有者线程唯一推进 + 进度发布一致」，不得写「已消除竞争」**；
  ③ S2 补上**可复现差分**（SplitMix64 固定种子、旧实现副本 SHA-256、core 30 万+upper 30 万输入 0 mismatch）
  与成本实测：core 槽位扫描 **2→4 趟**、两侧适配层新建并 move 输出 vector **丢掉调用方容量复用**
  （core +2 分配/次、upper +1）；**数值等价不构成热路径成本等价**；
  ④ 新增对象级证据工单（有界记录 + 后随观察表 + 槽位所有权路线 A/B）并完成 Step0 回溯：
  **既有实机三轮未挂采集器，故不存在可回溯的对象级证据**。
  冻结身份：`build32/src/d3d9/d3d9.dll` **36,253,553 B / `D1C3EBBDFF184AEB908F9A97DB4F9E8EBBA39BCCE96B357ED0CC69502391AAF6`**；
  门禁：`ninja`（Below Normal + `-j2`）exit 0、`ninja -n` no work、meson **80/80**、静态 **247/247**、
  内核 T1-T17、线程边界 9/9、进度生命周期 86/86、生产共用生命周期 **187/187**、对象级证据记录器 **19/19**、
  R 具名原因 POD 生产函数 **6/6**、解析/缺链静态 **44/44**、**生产往返测试**（记录器→转换器→环→导出→解析器）
  A/B/C/D 四场景全部达标、采集点门禁 EXIT 0；记录器子门默认关。
  **未实机、未部署、未提交；措辞上限「源码加固完成、纯判定测试通过，生产运行时覆盖待验证」**。
  **仍未证明**：错误矩阵不再被使用、合法对象正确回退、压力解除后恢复、槽位所有权；
  Water 与原版模型自动点光仍属主动延期，不得为凑清单重新混入发布。详见
  `docs/plan/2026-09-17-candidate-freeze-record.md`、`2026-09-17-p0-thread-relationship-proof.md`、
  `2026-09-17-thread-fix-implementation-design.md`、`2026-09-17-p0-object-level-evidence-workorder.md`。

- 2026-09-17 P0 palette 复核实机与两项待验证候选：合同 OFF 轮证明 device 侧记忆槽位复核在生产
  路径真实工作（陈旧拒绝累计 31,881→91,039），但三项画面证明仍为"观察/未覆盖"，且确认
  shadow-core 侧 Gap B 与 palette 来源分类仪表当前**不可度量**（语义 core 构建完成但
  四键资源查找 0 resolved；分类块在合同 ON 不可达路径上）。Gap B 补强（groupCount + 绑定帧/
  逐槽区间帧见证 + 细分拒绝，DLL 35,996,016 / `F03A84E3…`）与 S2 共享纯计算内核
  （T1-T13、meson 73/73）已入库并通过门禁，**均未部署、未实机**；实机验证过的
  `8CECC495…` 已从测试现场撤下并按备份恢复原 DLL。详见
  `docs/plan/2026-09-17-p0-real-machine-execution-record.md`、
  `docs/agent-history/2026-09-17-overnight-session-summary.md`。
- 2026-09-16 CPU取证历史外置E1/E2已完成核心及真实32→64实验：64位保存完整98MiB历史、
  32位有界入口，协议故障/慢消费/退出及独立字节校验通过；尚未接产品或证明游戏内存/闪退收益。
  下一步E3须独立异步owner合同，禁止直接接入阻塞lab析构，详见
  [E2收口](docs/agent-history/2026-09-16-recorder-offload-e2-verification.md)。

- 2026-09-16 夜间架构工作先收紧录制会话交接，并建立独立 `tools/render_host/` CPU协议实验室；
  真实32↔64位CPU管道、固定小共享槽、受控helper退出与有界故障测试已验证；原型不链接产品、不接游戏/GPU，
  尚未迁移内存或渲染。64位方向须先遵守[协议与架构图](docs/plan/2026-09-16-render-host-ipc-v0-contract.md)，
  不把CPU回执当GPU完成；阶段和截止见[夜间工作表](docs/plan/2026-09-16-overnight-architecture-workplan.md)。

- 2026-09-16 玩家2D4B在NVIDIA线程捕获到空指针读取，伴随内存不足状态；根因尚未完全闭合。
  新取证进程VA/commit准入及部分初始化防护仅通过离线/CPU检查，未重建DLL或实机；
  暂停调试现场保留，先解决闪退再继续闪烁。见 `docs/research/2026-09-16-player-crash-pid13216-memory-admission.md`。

- 2026-09-16 内部诊断构建新增 `warvk_internal_frame_recorder=true`：普通入口默认启用取证/HUD，
  不再要求启动器设变量；显式0可关闭，项目/正式构建默认仍false。隔离1440p无取证变量/默认目录门已通过。
  本build32为内部配置，发布前必须关闭该选项重新构建验证；不联动其他渲染实验。
  预算与操作见 `docs/INTERNAL_RECORDER_CANDIDATE_README.md`，不能把内部取证构建当作稳定/性能基线。

- 2026-09-16 DLL 自主取证及快照页 CPU 原子发布组合已通过零 watcher 的隔离 1440p 移动视角门，
  启动器默认选择内置调度；热键邮箱、冻结导出及发布失败/恢复已有生产 CPU 测试。
  保留外部显式模式，每进程一个事件；未部署玩家目录，未证明高压阴影/崩溃修复，
  退出中断及正向 semantic palette 覆盖仍待验。见开发日志 R4 和 `docs/research/2026-09-15-recorder-owner-and-snapshot-publication.md`。

- 2026-09-15 蒙皮矩阵来源保护候选已完成定向离线测试、DLL构建与no-work，
  默认off、独立启动器启用。玩家已测试候选 DLL，但该次进程的实际开关状态仍需核实；
  高压地图仍反馈裂缝、变换俯仰后阴影消失/闪烁，崩溃也未稳定复现，均未闭合。
  不得声称随机裂缝已修复，见 `docs/research/2026-09-15-skin-palette-publication-contract.md`。

- 2026-09-15上午玩家热键无效已定位为未安装窗口回调的错误接线，现改Ctrl+Shift+C并增加HUD。
  移动相机一秒前窗/实际窗口测试通过；显存上限和完整取证缺项仍需明确，撕裂未声称修复。
  见`docs/agent-history/2026-09-15-shortcut-and-one-second-fix.md`。

- 2026-09-15后半夜已完成初版GPU彩色历史/触发导出与实际方向draw参数关联，68帧限定
  隔离门通过，玩家DLL未动。中间GPU图像与完整输入仍缺，撕裂未修复，发布只做准备不推送；
  自动模型灯已默认关闭。见`docs/agent-history/2026-09-15-frame-history-overnight.md`。

- 2026-09-15逐帧记录器CPU环/控制、阶段事件与单张截图关联已构建、离线测试通过；
  GPU环形图像、完整输入和热键/视频尚未完成，实机因用户编辑器运行未开始，玩家DLL未改。
  详见`docs/agent-history/2026-09-15-frame-evidence-foundation.md`，不能视为裂缝修复或交付接受。

- 2026-09-14夜用户搁置原版模型自动点光/点阴影，移出本轮计划发布；保留源码与证据，
  不自动替换玩家DLL。两组短暂阴影异常已完成像素定位但根因未闭合；按要求先汇报，
  逐帧关联采集尚未实现。见`docs/research/2026-09-14-player-fissure-frame-pairs.md`。

- 2026-09-14 用户批准从公开 `v1.21.00` 建立本独立 `v1.22`（完整编号 `1.22.00`）集成树。
  当前处于移植与原生点光接入阶段，已有首批构建但未发布。保留全部已发布能力；明确排除未完成 Water
  分支水体改造，不覆盖旧性能树或玩家 DLL。当前阶段以
  `docs/plan/2026-09-14-v1.22-integration-and-release-gates.md` 及开发日志为准；下列8月状态为继承历史。

- 2026-09-14 已修正太阳关闭语义和 1.27a typed JASS carrier 精确签名；隔离 2560×1440 的真实
  JASS 地图完成 83 项断言及三形局部雾/Froxel 提交取证。仅覆盖 49 个公开函数的限定用例，
  不等于全量 API、动态 Guide 画质、前台性能或 v1.22 发布接受；玩家 DLL 未覆盖。详见
  `docs/agent-history/2026-09-14-v122-jass-local-fog-functional-checkpoint.md`。

- 2026-09-14 同步空读回/异步截图已移植，修正自动console及两处退出期线程/TLS问题；
  开/关录制隔离1440p截图与退出码0通过。模型点光现有默认开启消费者及按路径注册/阴影开关
  JASS候选；新双灯测试候选已修复显式注册优先级和world尾段过度回退，混合场景隔离1440p
  双灯持续提交/开关通过。原版灯遮暗与全阴影视觉、长期门仍未闭合，不是稳定发布。
  详见开发日志及two-native-lights-runtime-checkpoint；未包含Water改造。

- 2026-08-14 已把 Froxel/局部体积雾线与阴影生产者 CPU 优化线合并，并修复扩大后的逐帧性能诊断在
  Warcraft III 32 位地址空间中因超大历史环导致的退出；同图 ABBA 显示主线程 -0.357 ms、
  DirectGrouped -87.4%、BuildEligible -96.4%，体积光图/Lost Temple 120 秒及“生与死”603 秒
  隔离长门均无 device lost、GPU 事件或阴影完整性失败。DLL `7305E21A...B9DBDA3` 尚待用户前台
  视觉与绝对 FPS 验收，详见 `docs/agent-history/2026-08-14-stable-volumetric-performance-integration.md`。

- 当前分支：`codex/stable-optimization-integration-20260814`。原工作树另有用户未提交的外部子模块、
  PlayerCrash 与构建日志；
  保留它们，避免 reset、checkout 或覆盖式操作。
- 最近已提交的阴影基础已将地图/设备 epoch、Arena quarantine、fence retirement 与最终 replay
  验证接入生命周期。跨地图时旧资源不得发布给新地图；新地图在没有完整 CSM 前应安全退化为
  **无阴影**，不能采样未发布的深度图。
- 当前工作树含 **WarVK 1.21.00 Release 审核候选**：产品与 JAPI 显示版本已更新，外部 Shader API
  数字版本仍为 1.2.0、JASS 线协议仍为 `warvk:v1`。该线补齐点光位置、体积光和全局高度雾
  的作者接口，新增 scalar 数学求值，并重整 YDWE 分类。2026-08-12 的局部雾候选已进一步接通
  最多 8 个 Sphere/Box/Cylinder、解析射线区间、独立全局介质开关、受预算约束的局部 half-res ROI，
  并以视角无关 CSM 光学证据替代阴影柱俯仰特判；77/77 静态、21/21 Win32 runnable、DLL 构建及
  no-work 通过，但尚未部署、真实 Catalog 回读或玩家前台物理验收，详见
  `docs/agent-history/2026-08-12-local-volumetric-fog-candidate.md`。
- 2026-08-13 的 Froxel Medium/High 候选使用统一 `[20,10000]` 对数 Z、受限三维网格、时域重建、
  全分辨率 depth 引导和 compare-first 2×2 Caster 可见度；当前候选默认 Froxel High，并保留显式
  Legacy 回退。该基础已合入稳定生产者性能线并通过隔离长门，详见
  `docs/agent-history/2026-08-13-froxel-dynamic-shadow-comparison-pcf-candidate.md`。
- 后续研究确认方向阴影没有进入 3D temporal，且 `1/4`/`1/8` effect、raw-depth-only
  upsample 与低分辨率可读性共同放大移动阶梯。当前未部署候选新增独立 R16F base/边缘自适应
  `1/2` directional guide，以 full-resolution receiver plane 联合重建，并把最大 24% 可读性
  移到重建后。首轮玩家回归发现 clear-air 提前返回会丢弃 guide 且路径平均遮挡过弱；当前修订
  已让早退同时检查 guide，并恢复受双重光学证据约束的峰值遮挡；29/29 定向、78/78 静态、
  21/21 Win32 runnable、DLL 构建及 no-work 通过，尚待玩家前台视觉/性能 A/B，详见
  `docs/agent-history/2026-08-14-directional-volumetric-shadow-guide-candidate.md`。
- 产品、DLL 资源与 JAPI 显示版本已统一为 `1.21.00`；外部 Shader API ABI 仍为 `1.2.0`。
  GitHub 源码、玩家包、地图作者包及明确排除项见 `docs/RELEASE_1.21.00.md`。
- 当前候选验证为：474/474 静态测试、15/15 Win32 runnable、真实 YDWE Catalog 35/35 回读与
  WTG/WCT 校验、Win32 DLL 构建及 `ninja -C build32 -n` no-work。DLL 为 33,745,880 bytes，
  SHA-256 `84112587871BD421A3B927C65119258851A3E471734633220167AA81877FFF80`；未部署或启动游戏。
- 2026-08-06 的玩家 dump 证明 1.2.0 崩溃处理器把可恢复的 InputHost/CoreMessaging 首机会
  异常同步写成约 59 MiB dump；旧 `g_dumpInProgress` 还会永久遮蔽之后真正的 fatal。当前
  诊断热修默认不再注册 VEH；显式 `DXVK_WAR3_CRASH_FIRST_CHANCE_TRACE=1` 时也只做锁自由
  内存计数。完整 dump 仅由 UEF 为未处理异常生成，`latest_crash.json` 只表示 fatal，且
  首机会状态与 fatal 一次性门已经分离。
- 首个诊断候选暴露出既有增量构建污染：`d3d9_device.cpp.obj` 的 Ninja 依赖表为 0，调用方
  按旧布局只为 `War3RenderPipeline` 分配 656 bytes，而新构造函数按 736-byte 布局写到
  `this+0x2D8`，因此启动即在 d3d9.dll 内越界。现在 pipeline 创建/销毁移入构造函数同一
  翻译单元，factory 符号编码 pipeline/settings 的 size+alignment；旧布局调用方会链接失败，
  不再生成混合对象 DLL。全新目录 clean build 后为 33,750,533 bytes，SHA-256
  `36AC64802D36BEC4327F85E7B3B0CB2878B12BDEAFF54CA2223063C609B90DAC`；483/483 静态测试、
  16/16 Win32 runnable 通过。高压光影图 AutoTest 两轮分别完成 969/1636 采样帧，均进入
  地图且 device lost、frame incomplete、budget exceeded 和新增 dump 为 0。clean DLL 已由
  AutoTest 部署到 `E:\Work\War3\d3d9.dll`；仍不能据此排除 ReShade/InputHost 自身问题。
- 2026-08-07 的单地图高压候选已区分 receiver 终态与 pre-receiver 占位发布，并为 direct geoset
  快取补齐 map/immutable generation 门。默认可见桌面下“生与死”完成 DirectInline 三轮及 TAA v2
  一轮各 10 分钟巡航，receiver 全零、Arena/replay 异常、incident 和 GPU 事件均为 0；504/504 静态、
  16/16 runnable 通过。部署 DLL SHA-256 为
  `9FE2F6132015D6BF5413B844915187F80D9F53E14F204564890CD9E71E12AED3`。详细证据见
  `docs/agent-history/2026-08-07-life-and-death-night-gate.md`。
- 2026-08-09 的对象 bounds 候选已由模型缓存的 map epoch 与 process-monotonic immutable
  generation 派生精确局部 geoset bounds，并以独立 identity proof 贯穿到最终 CSM；skinned/
  动态附件保持 fail-visible。对象 C2/C3 消费默认关闭，只允许先收集 Observe 证据，详见
  `docs/agent-history/2026-08-09-generation-backed-object-bounds-observe.md`。
- 2026-08-09 已确认最终 CSM 曾允许非地形 C2/C3 使用未证明的猜测包围球剔除；当前本地候选统一使用
  bounds provenance 授权，Unknown/Generic/Animated/Skinned 一律 fail-visible，并保留 would-cull 统计。
  该候选会增加远级联工作，尚未部署或通过低视角物理 A/B，详见
  `docs/agent-history/2026-08-09-object-bounds-fail-visible.md`。
- 2026-08-11 的夜间高压门捕获到真实 NVIDIA `READ_INVALID` device fault；当前仅新增一个默认关闭、
  编译期开发专用的 `VK_EXT_device_address_binding_report` 有界环，用于下一次复现时把 fault 地址关联到
  Vulkan 对象。它尚未部署或实机取证，不能描述为 TDR 修复；详见
  `docs/agent-history/2026-08-11-device-address-binding-fault-correlation.md`。
- 2026-08-09 已修复 Transparent Type0 建造附件在 Stage11 被错误拒绝的问题：Type0 现在拥有常驻的
  exact CurrentDraw 边界，使用子部件身份和同帧 VB/IB/UV/完整矩阵调色板，并正确支持
  `D3DVBF_0WEIGHTS` 索引蒙皮。用户已确认不死族 UBirth 建造阴影不再闪烁；该路径没有重新开启
  全局跨帧 VB/IB cache。详细证据见
  `docs/agent-history/2026-08-09-stage11-type0-correctness-baseline.md`。
- 2026-08-09 的本地 `1.2003` Hotfix3 组合候选已合入用户确认过的 Type0/UBirth 建造阴影、点阴影
  receiver-bias，以及编译期关闭 legacy `warvk:cmd`、JASS CPU-only reset、异步 settings mailbox、
  active-device 发布、Reset device epoch 和 SceneCollector 早退身份清理。76 个静态脚本/581 个用例、
  20/20 Win32 runnable、32 位 clean build 与 no-work 通过；DLL SHA-256 为
  `A36253BC63854B4B9F620DE6303B076C5361B8511A3A732D8768459F42A4147F`。前两项视觉修复已有用户
  前台确认，新增运行时安全组合仍未部署或完成 Reset/A→B→A 物理门。详细证据见
  `docs/agent-history/2026-08-09-runtime-safety-and-shadow-edge-research.md`。
- 2026-08-10 的 TDR terminal-drain 候选让 D3D9 CS 在终态设备丢失时仅等待已派发序号，并让
  submission/finish/presenter 只做 CPU 收尾、停止新增 GPU/WSI wait；仅通过离线合同和构建，尚无
  实际 device-lost 注入或玩家前台证据，详见
  `docs/agent-history/2026-08-10-tdr-terminal-drain.md`。
- 2026-08-10 的 TDR P2 候选在已锁存的设备丢失后拒绝新的 D3D9 shader、PipelineManager 与
  compiler-worker 工作，并 CPU 排空既有 worker entry；它不把 pipeline 编译失败解释为 device loss，
  仅完成离线合同/构建，详见 `docs/agent-history/2026-08-10-tdr-p2-pipeline-compiler-drain.md`。
- 2026-08-10 的 TDR P3 候选将同一不可逆 fail-stop 语义补到 D3D9Ex `ResetEx` 和额外 swapchain
  创建入口，终态时不再重建原 Vulkan device；仅完成离线合同/构建，详见
  `docs/agent-history/2026-08-10-tdr-p3-d3d9ex-reset-fail-stop.md`。
- 2026-08-10 的 TDR P4 候选在支持 `VK_EXT_device_fault` 时只对真实 `VK_ERROR_DEVICE_LOST`
  采集一次有界、按值拥有的文本/地址诊断；不启用或写入 vendor binary，且不改变终态 fail-stop。
  它仅有 fake/离线验证，真实 device-loss 注入仍未验收，详见
  `docs/agent-history/2026-08-10-tdr-p4-device-fault-text-capture.md`。
- 同一调查确认现有 3794 帧取证全为 4096 DirectInline，阴影边缘持续爬动并非 Temporal 或自适应
  降档所致；首要源码嫌疑是 CSM 先线性过滤原始深度再比较，以及默认周期性世界坐标 Poisson 旋转。
  compare-first PCF、固定对称核和关闭周期旋转已在候选中实现；2026-08-10 又统一了 Direct/Prepass
  的 PCSS、级联失效回退、receiver-plane 数值合同、non-uniform 控制流前的导数和 UBO fail-soft。
  仍未获得玩家物理复审，不能描述为视觉问题已修复；
  详见 `docs/agent-history/2026-08-10-issue4-receiver-prepass-numeric-contract.md`。
- 2026-08-10 的 Issue #4 alpha-cascade parity 候选将 Release 默认远级联 cutout bias 设为零，并让
  depth/mask 共用有限值 helper；它仅消除确定的跨级联 silhouette 差异，仍等待玩家前台物理复测。
- Issue #5 的地形级联剔除现为默认关闭的 `Off / Observe / Consume` 合同；只有同帧、同代且来自
  已验证 position span 的精确 bounds 才能授权 C2/C3 剔除，猜测或陈旧 bounds 一律 fail-visible。
  当前仅完成离线验证，未部署且未通过实机 A/B；详见
  `docs/agent-history/2026-08-09-issue5-terrain-bounds-observer.md`。
- 2026-08-10 的 Issue #5 observer build policy 候选保持正式 DLL 的 terrain/union observer 永远 Off；
  仅独立 `warvk_shadow_observers_dev=true` 开发构建可用环境值 `1` 收集 Observe，`2` 仍为 Off，不能授权
  Consume 或部署发布。
- 2026-08-11 的本地候选补齐 S1 persistent/early-hit 与 Stage10 的 exact terrain bounds 来源：indexed
  draw 只接受当前 IB 扫描域，persistent local bounds 由不可变 geometry generation 拥有；Release 仍 Off，
  但实机开发 Observe 的 proof accepted 仍为零，因为 indexed-domain 计算受历史禁用的 exact-trim 门
  阻断，不能据此宣称剔除可用；详见 `docs/agent-history/2026-08-11-issue5-exact-terrain-bounds-provenance.md`。
- 2026-08-11 的高压低视角门捕获到真实 `VK_ERROR_DEVICE_LOST`：device fault 报告
  `READ_INVALID`，点阴影为零且 Arena 无 ownership/overflow 违规；TDR 前同时存在连续
  `ProducerIncomplete` 与四级联重复重放。本地诊断候选已把独立 producer/cache/unknown-index 原因
  贯通到 runtime、flight、incident 和 perf，仍不等于 TDR 已修复；详见
  `docs/agent-history/2026-08-11-night-tdr-invalid-read-evidence.md`。
- 2026-08-11 最新报告证明高压全体阴影闪烁主要来自必需 Caster 被软优先级预算拒绝，而非 Arena
  硬容量耗尽；本地候选改为低于 384 MiB 硬上限时全部准入并增加独立原因诊断，尚待前台物理 A/B。
- 2026-08-12 的开发专用 coherent REAL index-trim 候选只对同一 draw 内可同步冻结的刚体不透明
  地形收紧 position/index 域；已删除会产生多毫秒回归的中间 scratch copy，303 秒门的 Arena
  p95 为 63.929 MiB，producer-incomplete/TDR 均为零，但瞬时峰值仍为 369.132 MiB。Release 默认
  仍不可达，前台视觉验收尚未完成；详见
  `docs/research/2026-08-12-coherent-real-index-trim.md`。
- Issue #6 已先闭合两个确定的跨地图 CPU 身份泄漏：caster tombstone 现在按 map epoch 隔离，且
  旧相机、per-draw upload、RT/DS fallback 会在 Present 安全点重置；显式禁用 producer stage 的
  进程级策略仍跨地图保留。该阶段仅通过离线合同和 Win32 runnable，尚未部署或完成 A→B→A
  物理验收，详见 `docs/agent-history/2026-08-09-issue6-map-identity-isolation.md`。
- 同一 Issue 的后续审计确认 `ModelRegistry`、实例、Pose、附件、ShadowObject 与已发布 semantic
  contract 原先都不会在地图切换时清空；现在由 `War3Renderer::ResetMapSession` 在 Present 安全点
  发布空快照并清除纯 CPU 指针/姿态表，旧 contract 仍由 `shared_ptr` 保护在途读者。该候选同样尚未
  部署或物理验收，详见 `docs/agent-history/2026-08-09-issue6-semantic-registry-reset.md`。
- Issue #6 的 retired-session census 现可分别报告在途缓存条目、allocator chunk、逻辑 GPU 引用和
  CPU backing，并在 completion serial 回收后递增 collected 计数；它不改变 fence 或释放时机，仍需
  冷启动 B / A→B / A→B→A 物理数据确认，详见
  `docs/agent-history/2026-08-09-issue6-retired-session-census.md`。
- Issue #6 的 Direct geoset、unit flags、材质、palette slot、terrain bounds、index slice、static
  mesh-data、runtime-geoset 及 render-hook CUnit 身份热缓存现已按 map epoch 失效；进程全局别名在 Present reset 清除，
  固定 TLS 容器拒绝旧条目。模型 Hook 也已将地图会话 reset 与进程 Shutdown 分离，换图只清理
  palette/pose/attachment 裸指针状态而不伪装卸载 Hook。该候选不改变 GPU 退役，仍未部署或完成
  跨地图物理门，详见 `docs/agent-history/2026-08-09-issue6-map-scoped-hot-caches.md`、
  `docs/agent-history/2026-08-09-issue6-shadow-core-cache-isolation.md` 与
  `docs/agent-history/2026-08-09-issue6-model-hook-map-session-reset.md`；render-hook 补充见
  `docs/agent-history/2026-08-09-issue6-render-hook-unit-cache.md`。后续还清除了 widget/rawcode/handle
  映射及 SceneCollector 的 CUnit→handle TLS，并让 runtime-model 正验证和 palette-slot TLS 随地图
  会话失效；细节见 `docs/agent-history/2026-08-09-issue6-render-identity-cache-reset.md` 与
  `docs/agent-history/2026-08-09-issue6-widget-scene-identity-reset.md`。
- DirectGrouped 的 Producer Claim 目前只有默认关闭的同帧 Observe 预测器；Consume 请求会被明确
  拒绝且不改变 caster。旧性能报告显示 BuildEligible 是主要 CPU 热点，但 reduced key 尚缺已证明的
  source/material/alpha 身份，必须先完成至少 10,000 帧零误判实机门，详见
  `docs/agent-history/2026-08-09-producer-claim-observe.md`。
- 当前 RTX 4060 Ti 的主 15.73 GiB device-local heap 不可 host-visible；唯一同时
  `DEVICE_LOCAL | HOST_VISIBLE` 的 heap 只有 214 MiB。因此 ReBAR direct-upload 实验不满足既定
  准入条件，保持未实现/默认关闭，不能占用小 BAR heap 冒充完整 ReBAR 收益。
- 1.21.00 的发布范围仍限定为“新启动进程只进入一张地图”。同进程退出地图后再进入其他地图仍可能
  造成性能下降、阴影异常或其他生命周期问题，已由用户决定延期到下一版本；README/CHANGELOG
  必须保留该已知问题。点阴影 receiver-bias 修复已由用户前台确认，不再把旧摩尔纹列为当前已知问题。
- 当前 JAPI/体积效果仍需用户地图物理验收；可见桌面 AutoTest 的低视角稳定门不能代替玩家前台
  视觉判断，也不能外推为跨地图生命周期已经修复。

## 不可破坏的工程约束

- GPU 资源的 reset、Arena 回收、receiver/skin 状态切换必须由渲染所有者在 `PresentEx` 安全点
  合并执行。地图退出或 JASS/Hook 线程只能提出请求和清理 CPU 侧状态，不能直接释放仍可能被 GPU
  使用的资源。
- Shadow 生产、接收和 replay 都必须验证 map/device epoch、资源身份、范围、索引/顶点来源及有限
  数据。任何证明失败都 fail-closed：整份 candidate 不发布，必要时显示无阴影，绝不跨地图复用。
- Arena、冻结几何、persistent package 与 GPU-skin 的 fence/所有权不能互相冒充。producer 完成 fence
  不等于消费者的 last-use 权限。
- WarVK JAPI 是有界的外部输入面：保留 wire 长度、参数数、句柄、数值有限性、溢出和生命周期检查；
  不能为方便而让渲染线程回调 JASS 或暴露未实现 feature bit。
- 修改阴影、滤波、抗锯齿、资源同步或其他图形学算法时，只要实现者不能从现有合同中完整证明公式与
  Vulkan 行为，就必须先查阅一手资料：原始/同行评审论文、Microsoft DirectX 指南、Khronos Vulkan
  规范或 GPU 厂商研究资料；在 `docs/research/` 记录公式、适用条件、来源链接及其到 WarVK 的映射。
  博客和二手总结只能作为检索线索，不能单独授权 Release 默认改动。
- Unreal Engine 官方源码只允许从 EULA 授权的 `EpicGames/UnrealEngine` 仓库检出到项目外部的
  `E:\Mycode\Source\References\UnrealEngine` 并作为只读架构参考。不得把 UE 源码、片段或资产复制、
  改写进 WarVK/DXVK 开源树或提交历史；WarVK 的实现、测试和注释必须保持独立表达与可追溯的一手依据。
- 不要将 `AutoTest` 的 isolated desktop 数据宣称为玩家前台性能；涉及性能时按
  `AutoTest/README.md` 的前台基线和特性矩阵规则执行。
- 修改阴影、过滤、抗锯齿、同步或其他图形学算法时，只要结论不是十足确定，必须先查阅原始或同行评审
  论文、Microsoft DirectX 指南、Khronos Vulkan 规范或 GPU 厂商研究资料，并在 `docs/research/`
  记录公式、适用边界、来源与本项目映射。博客只能作为检索线索，不能单独授权 Release 默认值。
- Unreal Engine 官方源码只允许从已获 EULA 权限的 `EpicGames/UnrealEngine` 检出到项目外部
  `E:\Mycode\Source\References\UnrealEngine`，作为只读架构参考。不得复制、改写或提交 UE 源码、
  Shader、资产到 WarVK/DXVK 开源树；实现必须独立完成并优先引用公开论文/规范。
- 构建或测试不会自动授权部署 DLL、覆盖 YDWE/Warcraft 文件、启动/关闭编辑器或游戏。此类操作需有
  用户明确请求，并先检查目标进程与精确备份/哈希。

## 常用开发与验证入口

```powershell
# 在仓库根目录构建 32 位 DLL；该 helper 会处理 Windows 扩展路径和 imgui Meson shim。
.\build32_safe.cmd src/d3d9/d3d9.dll -j8

# 确认增量构建没有遗留工作。
ninja -C build32 -n

# 按改动范围运行对应静态检查；全量检查仅在需要时执行。
Get-ChildItem AutoTest -File -Filter 'test_*_static.py' | ForEach-Object { py $_.FullName }
```

主产物为 `build32/src/d3d9/d3d9.dll`。提交前至少运行相关静态测试、相关 Win32 runnable 和 DLL
构建；阴影/生命周期/性能改动还应执行与风险相称的 AutoTest 门。不要把当前工作树已有的测试
记录当作新改动的验证结果。

## 文档维护规则

- 根目录 `AGENTS.md` 保持为短入口：项目目的、代码地图、当前状态、硬约束、构建入口。不要添加逐轮
  日志、旧 SHA、完整 benchmark、实验细节或路线图。
- 长篇变更、旧方案、性能数据、回退信息和未来计划放入
  `docs/agent-history/`、`docs/research/` 或 `docs/plan/`，并用日期和主题命名。
- 完成一项会改变“当前状态”或硬约束的工作时，只在本文更新一条浓缩事实和验收边界；详细证据写入
  独立文档。普通任务不应要求全文读取历史归档。
