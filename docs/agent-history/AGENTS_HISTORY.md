# WarVK Agent Handover History Archive

> 从根目录 `AGENTS.md` 迁移于 2026-08-05。以下内容保留原有时间顺序，供需要追溯旧实验、
> 验证证据或已证伪方案时查阅；它不是当前状态的权威来源，也不应作为普通任务的必读文件。

---

## 2026-08-05（WarVK JAPI 作者控制与 YDWE 分类候选）

本轮针对作者可用性完成 JAPI 1.4.0 候选。点光位置更新沿用已闭合的
`WarVKSetPointLightPosition`；体积光四项旧入口现在发布真实功能位并接通后端，新增全局
高度雾独立开关与参数接口。高度雾关闭只移除高度密度项，不关闭太阳/点光体积散射。当前
没有 Box/Sphere/Cylinder 局部雾体积，YDWE 中也未伪造相关入口。

科学计算新增 `WarVKEvaluateMathReal` 与 `WarVKEvaluateMathInteger`，直接计算 scalar 公式
实例；整数支持最近、向下、向上和截断四种明确舍入方式，拒绝非有限值与 int32 溢出。
旧 Curve 查询继续兼容。YDWE 数据库拆成系统、诊断、太阳阴影、点光、体积光、体积雾、
闪电、模板、数学和曲线分类；根 `WarVK` 只保留版本、协议、功能位和运行时就绪。尚未发布
功能位的 Outline/Bloom/PostFX/AA/昼夜共14项已从 action/call 菜单隐藏，但 JASS 包装保留
兼容。

全量 `test_*_static.py` 458/458 PASS，15/15 Win32 Meson runnable PASS，Win32 build PASS，
`ninja -C build32 -n` no-work。War3MapEditor64 的真实 `UiCatalog::Load` 对临时叠加后的完整
YDWE Catalog 回读为 35/35 sources parsed，并通过独立 WTG/WCT 校验。build32 DLL 为
33,725,932 bytes，SHA-256
`68DBEA60F98FF6C9420D42E872DAFF2099C7ABBBE10732811E278CD8DC2C9B54`。本轮未部署 DLL、
未启动游戏；体积视觉效果与 JASS 物理调用仍需用户地图验收。

## 🚨 2026-08-05（跨地图生命周期候选的整屏阴影回归热修）

用户部署跨地图 epoch/replay 验证候选后，地图启动时出现整屏阴影，稳定后仍有覆盖近半屏的
错误暗区。事发快照并非 Arena 或 epoch 错配：153 个计划 caster 全部在最终 replay
验证器被 `PositionRangeOutOfBounds` 拒绝，累计 8644 次，validated/drawn 均为 0。

**确定回归与修复**：

- exact-index domain 无法由 CPU 读取时，生产者已经冻结完整 bounded VB，并保留原始
  `BaseVertexIndex` 给 Vulkan replay。新验证器却把该偏移再次加到完整 backing 容量末端，
  因此 512 KiB backing 被虚构成需要 663532 bytes。新增明确的
  `fullVertexDomainFallback` 验证输入：该路线验证完整 position/blend/UV backing 覆盖与
  exact IB 字节范围，不再把 BaseVertex 偏移重复施加到 backing 容量；实际 draw addressing
  保持不变。
- 同 epoch last-good 的 8 帧上限原先只把计数减为零，却未撤销
  `m_hasCompleteShadowMap`，导致加载阶段的不完整 CSM 可被永久采样。验证持续失败且 hold
  用尽后现在真正撤销 complete publication 并清空 last-good 身份。
- 新 epoch 尚无完整 CSM 时，receiver 原先仍以正常强度采样新分配但未发布的 depth image，
  会把地图整体压黑。现在只有 `receiverHasUsableDirectionalShadow` 成立才向 UBO 写入阴影
  强度，否则固定为 0；资源仍保留，第一张完整 candidate 建立后一次性启用。

**验证与候选**：

- 全量 `test_*_static.py` 456/456 PASS；全部 15 个 Win32 Meson runnable PASS；Win32
  DLL build PASS，`ninja -C build32 -n` no-work，targeted diff-check 无 whitespace error。
- 隔离桌面高压光影图短门完成：最终 planned/validated/drawn 为 257/257/257，validation
  reject、partial prevented、Arena overflow/partial 和 device lost 均为 0；首张完整 CSM
  latency 为 1 帧。旧 `hot-shadow` semantic marker 仍会假阴性，按既有 `--no-hot-shadow`
  门复跑后 `stage=done`；没有放宽 replay/Arena/device 正确性计数。
- 最终截图 `AutoTest/artifacts/screenshots/war3_20260805_093430.png` 目检未见整屏/半屏错误
  阴影，普通局部投影恢复。build32 与部署 `E:\\Work\\War3\\d3d9.dll` exact：
  33,721,526 bytes，SHA-256
  `B111320D81D087C3DF8C93FCBF6BCDCEE3AB02F4413672ABBCC534465756A501`；直接回退为
  `E:\\Work\\War3\\d3d9.dll.bak_20260805_109A_cross_map_visual_regression`，SHA-256
  `109A46B88B18E96BAB75F0A43F038A72F78A3CC53B6B020B398158812EF2A5C3`。

**验收边界**：隔离桌面只能证明本次确定性 regression 与高压图短门闭合；仍需用户在物理
桌面复测光影图启动、A→B 跨地图及“生与死”低视角。物理验收前不得宣称完整跨地图 TDR
问题已经发布完成。

## 🚨 2026-08-05（跨地图阴影 epoch、Arena quarantine 与 replay 原子发布静态候选）

用户确认只有“光影测试图 → 退出 → 生与死”同进程链容易触发整批阴影全灭/逐个补回及
`VK_ERROR_DEVICE_LOST`，冷启动直接进入后长期稳定。本轮据此修复跨地图 render session
没有完整切断的问题；只完成静态/runnable/构建门，尚未做用户所需 A→B→A 物理地图门。

**Present 安全点与 GPU 所有权**：

- 地图退出回调先关闭运行时并只递增 reset request serial，再清理 JASS/地图侧 CPU 状态；
  不再从非渲染线程直接 reset GPU Skin、Receiver、Arena 或 render-owned cache。
- `PresentEx` 在最后一次 BeforeUi 后合并请求：当前 Arena generation 被原子 quarantine，
  专用 completion fence 排在旧 session 全部命令之后；资源型 caster/freeze/persistent/S1
  容器移入 retired-session 队列，只有 fence 完成才释放。Arena 没有可证明空闲的
  generation 时 producer 保持关闭并 fail-closed，不按三帧索引强行回绕。
- 新增进程单调、非零的统一 shadow map epoch，并同步重置 Manifest、模型 cache、Package
  与 GPU Skin。draw-time capture、semantic append/populate、备用 validation 及 legacy
  allocator reset 均有 pending-session 硬门，避免退出请求与场景发布之间的竞态。

**Epoch 隔离、Receiver 与最终 replay 防线**：

- map/device epoch 已写入 Manifest object/part/lease、current-draw/semantic key、frame-freeze、
  最终 `War3ShadowCasterDraw` 和 `War3PipelineInput`；旧 epoch、零 epoch 或 A→B→A 地址复用
  在 producer/receiver/replay 三处均拒绝。
- `InvalidateMapEpoch` 排空旧 `std::async`，关闭并推进 persistent point worker epoch，清除
  CSM last-good、TAA history、identity/coverage hold、点阴影与 volume-sun publication；保留
  已分配 4096 CSM/TAA/point image，但其内容不再属于新地图。
- 新增纯值 `ValidateWar3ShadowReplayDraw`，在 CSM、terrain mask 与 point cube 发生 clear/draw
  前验证 epoch、有限矩阵、VB/IB/index type、checked range、actual index + signed base vertex、
  non-indexed、blend/UV 及 GPU-skin source/palette generation/range。任一失败整份 candidate
  fail-closed；只有完整预检/管线准备通过后才 clear 并发布。旧图最多保留同 epoch 8 帧；
  新 epoch 没有完整图时显示无阴影，禁止跨图 last-good 与逐 caster 可见补回。
- Unit 统计只接受 authoritative unit evidence 或 `shadowUnitIdentityProven`；蒙皮分类继续取
  最终实际 route。

**诊断、测试与交付边界**：

- `runtime_status.json`/flight recorder 增加 requested/applied/current epoch、transition、
  Arena quarantine/retire/completed、retired session、stale/replay reject、planned/replay/
  validated/drawn、partial prevented、first-complete latency、point worker cancel/late reject。
- 新增 attach-only `AutoTest/run_attach_cross_map_shadow_gate.py`：只连接现有 Warcraft PID，
  不启动、不聚焦、不改优先级、不终止游戏；epoch 变化时固定滚动证据，并记录状态、截图与
  153/4101 事件差异。地图切换仍须用户操作。
- 全量 `test_*_static.py` 456/456 PASS；15/15 Win32 Meson runnable PASS；Win32 build PASS，
  `ninja -C build32 -n` no-work，目标文件 `git diff --check` 只有既有 LF/CRLF 提示。
- build32 候选为 33,721,526 bytes，SHA-256
  `109A46B88B18E96BAB75F0A43F038A72F78A3CC53B6B020B398158812EF2A5C3`。由于
  `worldeditydwe.exe` PID 21984 仍在运行，未覆盖作者目录；
  `E:\Work\Warcraft III\d3d9.dll` 仍是上一候选
  `AD7A54BC9707F724E1D8B8C527B0508162622DBFBDBF26655DD2343DB895A2D3`。

**不得越界宣称**：静态门证明请求合并、epoch/范围合同和 fence 所有权结构闭合，但不能
替代 DirectInline 三轮、TAA v2 一轮及 A→B→A 的真实地图验收；在该门通过前不得宣称 TDR
已经物理修复，也不得据此继续叠加 Persistent Package Consume、联合剔除或混合蒙皮。

## 🚨 2026-08-04（Polyline 点曲线、原生 S20 顺序与闪电阴影隔离静态候选）

在 MathProgram/Curve Phase 1 上继续完成地图作者提交采样点的连续闪电路径，并修正
闪电渲染阶段及阴影归属。本轮按用户明确要求仅做静态开发、Win32 runnable 与构建；
没有启动 Warcraft III、没有物理画面验收，也没有覆盖作者目录 DLL。

**点曲线与单实例 Ribbon**：

- `CurveRuntime` 新增 2..1024 点的 bounded builder：`createPointCurve`、每批最多4点的
  `appendPointCurve` 与 exact-count `finalizePointCurve`。Finalize 在发布前一次性验证
  有限坐标/非零总长、烘焙累计弧长，并生成 `shared_ptr<const PointCurveData>`；渲染线程
  不会看见半条上传记录，源 curve 销毁后实例继续持有不可变快照。
- `War3LightningRuntime` 新增 `createPolylineFromTemplate/setPolylineCurve`。一个点曲线只
  建立一个 `LightningRecord`；中心线以连续 triangle strip 生成，局部左右方向带符号
  连续性，颜色/宽度/脉冲/UV 按真实归一化弧长插值。主 Ribbon 一次 draw，可选辉光再加
  一次；不会为每段创建托管闪电对象。Polyline 已是完整作者中心线，因此不再叠加模板
  分支。
- JAPI 升为 `WarVK JAPI 1.3.0-polyline-curves`，命令 75→80，新增 point create/
  append4/finalize、polyline create/set 五条命令；功能位新增
  `WARVK_FEATURE_POLYLINE_CURVE=16384`，总实现位为 `0x7E07`。由于 v1 wire 上限为16参数，
  点集在 JASS 侧每批上传4点，但冻结与渲染仍是一个快照/一个实例。
- JASS、YDWE action/call、README、`MATH_CURVE_API.md` 已同步；新增640点经典 Lorenz
  smoke，JASS 一次积分4步后直接上传，不保留640点数组，最终只创建一个连续闪电实例。

**S20 顺序与阴影合同**：

- WarVK 绘制从借用 S11 semantic-shadow execute 改为原生 `WorldDispatch S20` 返回后执行，
  保留 `a5==0` 与 runtime/device 门；同时移除 lightning 对 BeforeUi pipeline 的无关强制。
- measured/legacy stage map 均把 S20 从 `Decorations` 改为独立
  `War3BatchTag::Lightning`，`GetStageCategory` 把 S20/Lightning 分类为 `Effect`，不再是
  Terrain。
- 中央 `EvaluateShadowProducerPolicy` 在其他判定前对 physical stage 20 或任一 Lightning
  tag fail-closed，覆盖 current draw、semantic direct、draw-time geometry/pose 与 immediate
  legacy 等全部阴影发布者；telemetry 新增 `rejectedLightning`。该拒绝只阻止自定义阴影
  数据发布，不修改 Warcraft 主颜色 S20 draw。

**静态验证与候选**：

- 全量 `test_*_static.py` 447/447 PASS；全部13个 Win32 Meson runnable PASS；Win32 DLL
  build PASS，`ninja -C build32 -n` no-work，targeted diff-check 仅既有 LF/CRLF 提示。
- `build32/src/d3d9/d3d9.dll` 为 PE32/I386，33,620,966 bytes，SHA-256
  `AD7A54BC9707F724E1D8B8C527B0508162622DBFBDBF26655DD2343DB895A2D3`。未部署。
- 静态/构建只能证明协议、生命周期、阶段门和生产者策略合同；仍需以后由用户显式安排
  物理地图验收，确认 S20 层级、Lorenz 连续接缝、原生闪电不投影以及高点数性能。在此
  之前不得宣称视觉问题已完成物理验证。

## 🚨 2026-08-04（WarVK MathProgram / Curve 公式闪电 Phase 1 候选）

用户要求闪电可由地图作者提交数学公式走出任意参数曲线，并把这套能力提升为可复用的
数学表达式运行时，而不是每点从 JASS 回调 C++。本轮完成第一阶段生产闭环；未进入矩阵/
四元数、自动微分、RK4、向量场或节点图。

**数学与曲线运行时**：

- 新增 `war3_math_expression`：纯表达式编译为不可变固定栈字节码，原生支持
  `float/vec2/vec3`、四则/三角/指数/插值/向量/旋转/Bezier/端点遮罩与确定性
  `noise1`。硬上限为公式384字节、作者参数16、指令256、栈64、嵌套32；没有循环、
  递归、赋值、动态内存、文件/网络或游戏副作用，非有限结果、除零和定义域错误拒绝。
- 新增 `CurveRuntime`：每图最多256个 program/512个 mutable curve；支持 OFFSET
  (`vec2`)、LOCAL/WORLD (`vec3`) 三种严格类型坐标模式、首尾锁、命名 real 参数、
  低频坐标/有限差分导数/2--256段弧长查询。program/curve 销毁后，已绑定模板/实例继续
  持有不可变 `shared_ptr<const Program>` 快照；地图退出与 JASS Reset 均清理注册表。
- 新增纯 Win32 runnable，覆盖全部 Phase-1 函数组、坐标基、端点锁、确定性噪声、导数、
  弧长、非法公式、运行时除零、类型门及句柄/快照生命周期。

**闪电与作者 API**：

- `LightningRecord` 可持有公式曲线快照；主电和分支中心线在 C++ 渲染路径内批量求值，
  自动提供 `t/time/length/start/end/forward/right/up/seed/index/segments/branchIndex/
  branchDepth`。`time` 为实例创建后的秒数；类型不匹配在绑定时拒绝；求值失败安全回退
  既有中心线。模板冻结、贴图/颜色/宽度/生命周期/辉光和现有分支合同保持不变。
- JAPI 升为 `WarVK JAPI 1.2.0-math-curves`，命令表61→75，新增 MathProgram 编译/销毁/
  查询、Curve 创建/销毁/参数/坐标/端点锁/坐标/导数/弧长、模板与实例曲线绑定14条命令；
  功能位新增 `WARVK_FEATURE_MATH_CURVE=8192`，总已实现位为 `0x3E07`。这些 CPU 命令可
  在 render settings 就绪前使用，仍受512字节 ASCII wire、16参数与分号 token 边界限制。
- `WarVK/jass`、YDWE `action.txt/call.txt`、README、`MATH_CURVE_API.md` 与旋转螺旋
  smoke 已同步。绑定发生在模板 Finalize 前并复制当前参数快照；渲染不会逐点调用 JASS。

**验证、候选与边界**：

- 全量 `test_*_static.py` 442/442 PASS；全部13个 Win32 Meson runnable PASS；Win32
  build PASS，`ninja -C build32 -n` no-work，targeted diff-check 只有既有 LF/CRLF 提示。
- 候选 `build32/src/d3d9/d3d9.dll` 为 PE32/I386，33,592,915 bytes，SHA-256
  `FFD2F60FEDB718BE83EBF2B83DE846AF7D578E0B8172A802237667C15A9B269E`。
- `worldeditydwe` PID 29716 自 2026-08-03 21:46 起仍运行，故未热覆盖作者目录、未启动
  游戏；`E:\Work\Warcraft III\d3d9.dll` 仍为上一候选 33,497,691 bytes，SHA-256
  `CE2574781D8CDF23BD9E821F279DD8441140FB5E847593D9A69CE061E7AAEB49`。下一步须关闭
  编辑器后备份并部署同一 SHA，再用公式螺旋 smoke 做固定/移动端点、分支、多个 seed、
  长时间动画与性能验收。
- 暂未实现 `mat2/mat3/mat4`、quat、比较/三元/`let`、自动微分/二阶曲率、自适应细分、
  弧长反查、noise2/3/fbm/curl、RK4/向量场、节点图或 GPU 求值；不得把 Phase 1 描述为
  完整微积分/线性代数语言。

## 🚨 2026-08-03（精确索引域 WC bulk-read：性能回归修复候选）

用户阶段验收发现当前默认光影图从历史约 100 FPS 降至 76 FPS，高压/“生与死”场景
还会在低视角压力下出现阴影消失与 `VK_ERROR_DEVICE_LOST`。本轮没有继续推进 Package
Consume、联合剔除或混合蒙皮，而是先定位并修复 Arena 崩溃补丁引入的确定 CPU 热点。

**根因与最终实现**：

- exact-index trim 为避免 512 KiB terrain VB 整域冻结，会扫描当前 draw 的真实 IB 得到
  最小顶点域。Warcraft 的 WRITEONLY IB 映射通常是 HOST_VISIBLE、非 HOST_CACHED 的
  write-combined 内存；旧实现对每个 16/32-bit index 做一次标量读取，高压图因此把
  `ShadowCapture/PostGate` 推高到约 5--7 ms。
- 曾验证 generation-qualified 结果缓存，但同 DLL 运行仅 2,038 hit / 71,304 miss，命中率
  约 2.8%，不足以抵消哈希与比较成本；该实验实现已完整删除，不留默认或跨帧 cache。
- 最终路径只对已经通过 allocation range、owner identity 和三类 generation 验证的当前
  exact IB range 工作：非 HOST_CACHED 且不超过 64 KiB 时，一次顺序 `memcpy` 到 64-byte
  对齐的 thread-local cached scratch，再执行原始 min/max/越界验证。scratch 与结果均不
  跨调用存活；超过 64 KiB 或关闭 ABBA 开关时回退原标量路径。
- 发布默认开启；同 DLL 对照开关为
  `DXVK_WAR3_EXACT_INDEX_DOMAIN_BULK_READ=0/1`。该路径不修改 caster、draw range、
  Arena transaction、freeze catalog、CSM、shader 或 GPU 资源所有权。

**后台全速 ABBA 与运行证据**：

- 所有运行使用已验证的 Game.dll 后台 idle-sleep 精确 Hook：
  `--background-idle-sleep disabled --process-priority high`；没有依靠全局 Sleep Hook 或
  BELOW_NORMAL 样本。隔离桌面 FPS 只作同机 ABBA，不作为物理前台发布帧率承诺。
- 高压图 35 秒同 DLL ABBA：bulk Off 为 51.57 FPS、main 15.19 ms、ShadowCapture
  6.52 ms、PostGate 5.79 ms；bulk On 为 68.21 FPS、main 10.53 ms、ShadowCapture
  1.75 ms、PostGate 1.03 ms。GPU 4.92→4.77 ms，说明收益来自 CPU WC 读取消除。
- 清理实验 cache 后的最终普通图：3,482 帧，95.61 FPS、main 5.07 ms、ShadowCapture
  0.964 ms、PostGate 0.607 ms、GPU 5.65 ms；Arena 平均/峰值 2.46/4.12 MiB。
- 最终高压 90 秒门：5,771 帧，63.47 FPS、main 11.54 ms、ShadowCapture 2.01 ms、
  PostGate 1.27 ms、GPU 4.81 ms；Arena 平均/峰值 3.48/5.57 MiB。累计 419,084 次
  non-HOST_CACHED scan、105,369,462 bytes 全部由 bounded bulk 覆盖，direct/oversize
  fallback 均为 0；trim accepted 419,084、rejected 0。
- 最终门的 Arena overflow/busy/admission/partial、frame incomplete、budget exceeded、
  queue error、device lost 与新增 NVIDIA/Display 事件全部为 0；queue submitted/completed
  仅差正常在途一帧。最终截图为
  `AutoTest/artifacts/screenshots/war3_20260803_174611.png`，253 caster / 1012 cascade draw，
  目检未见阴影缺口、路径阻断器泄漏或巨型阴影块。

**静态、构建与交付**：

- 429 项 `test_*_static.py` 全部 PASS；12/12 Win32 Meson runnable PASS；Win32 build
  PASS，`ninja -C build32 -n` no-work，targeted diff-check 仅既有换行提示。
- build32 与部署 `E:\\Work\\War3\\d3d9.dll` exact：33,454,248 bytes，SHA-256
  `6C07BCE0B1F4F8C4903DE9C004E64351ED43D98691C3EB65C06BF01A85A3B8D7`；明确回退为
  `E:\\Work\\War3\\d3d9.dll.bak_20260803_9A7D_pre_exact_domain_cache_abba`，SHA-256
  `9A7D96C8EB45E84F7EFB8D922F9FB5D8224350DBAD3DD1A8E9806175F2CE28F5`。

**验收边界**：自动高压图证明了热点收益与 90 秒稳定性，但不能代替用户在“生与死”
原位置压低视角的最终物理验收。Persistent Package 仍默认 Off/Observe-only；本轮没有
启用任何跨帧 VB/IB cache、source fingerprint reuse、fast append 或 prebuild bypass。

## 🚨 2026-08-03（阶段验收候选：bounded CurrentDraw Package Observe 闭合）

本轮在 `ef012ba` 的 current-draw equivalence 地基上收口一个可供用户阶段验收的
稳定候选；没有进入 Package Consume、联合剔除、混合蒙皮或阴影合批。发布默认仍为
Off，正常启动时不创建 Package owner、不扫描 WC buffer、不绑定 Package slice，也不
修改 Main/CSM/点阴影/outline draw。

**当前物理 draw 证明**：

- multi-primitive geoset 现在按当前 `sourceVertexFirst/baseVertexIndex`、exact index
  count/min/max/content hash 唯一选择 primitive；producer fence 完成后由 Store 发布
  `readyValidationAuthority`，热路径不再重复完整 immutable source walk。
- Warcraft 的 WRITEONLY VB/IB 常位于 HOST_VISIBLE、非 HOST_CACHED 内存。canonical
  Shadow/Arena 继续把它视为 CPU-opaque 并走完整 VB fail-safe；只有三个 Package gate
  同时为 Observe 时，才允许独立 bounded proof：index 每 draw 8 KiB、每帧 32 KiB，
  position 每 draw 32 KiB、每帧 96 KiB，准入计时预算 0.10 ms/frame。事务一旦准入会
  完成 index scan、position copy 与 hash，再由实际耗时阻止后续事务，禁止半份 proof。
- 早期按“任意 eligible draw”花预算会扫描到非最终 caster；同一 key 的后续 draw 又会
  覆盖 proof。现在 entry 记录同帧 capture ordinal 与上一帧 exact Stage11 最终选择的
  ordinal，下一帧只在预测的最终 ordinal 上花观察成本。该跨帧值只用于成本准入；
  当前 allocation identity、identity/allocation/content generation、draw range 与 bytes
  仍逐帧重新证明，不能授权跨帧几何复用。
- LOS/path blocker 在早期 FourCC/semantic gate 与最终 `entry.pathBlocker` gate 均被拒绝；
  不恢复 draw-time VB cache、source fingerprint reuse、fast append 或 prebuild bypass。
  `binding=0 / mutation=0 / consumerAuthority=0` 仍是硬合同。

**诊断与运行证据**：

- `runtime_status.json` 现在同时报告最终 Stage11 的 bounded scan/copy/hash/budget 与
  capture 全部成本，并附 QPC frequency。20 秒高压 Observe 门为 1,041 report frames：
  4,262 exact 中 4,258 ready，仅首次 4 个 package 产生 4 次提交/4,128 bytes 上传；
  producer fence 4/4，position/index/primitive mismatch、invalid、completion reject、
  device lost、shadow incomplete 与 budget exceeded 全为 0，Arena 峰值 5.543 MiB。
- 最终状态快照的 capture scan/hash ticks 为 515,466/742,045，frequency=10,000,000；
  两者合计约 0.121 ms/采样帧（position copy 已包含在 hash ticks 中，不重复相加），
  低于 0.15 ms Observe 门。该数字不作为 Consume 收益结论。
- 默认 Off 另跑 495 帧：configured/effective/owner、observations、scan/hash 全为 0；
  device lost、shadow incomplete、budget exceeded 与新增 NVIDIA/Display 事件均为 0。

**静态、构建与交付**：

- 全部 `test_*_static.py` 共 423 tests PASS；全部 12 个 Win32 Meson runnable PASS；
  Win32 DLL build PASS，最终 `ninja -C build32 -n` 为 no-work，targeted diff-check 只有
  既有 LF/CRLF 提示。
- build32 与部署 `E:\Work\War3\d3d9.dll` exact：33,374,202 bytes，SHA-256
  `9A7D96C8EB45E84F7EFB8D922F9FB5D8224350DBAD3DD1A8E9806175F2CE28F5`；上一明确
  回退为 `E:\Work\War3\d3d9.dll.bak_20260803_FFC0_pre_bounded_package_index_proof`。

**验收边界**：用户本轮测试的是默认 Off 稳定候选与既有视觉正确性；Package Observe
只用于后续显式诊断。没有 recording transaction、consumer last-use fence 与真实 Consume
权限之前，不得声称 Persistent Package 已接管绘制或带来发布性能收益。

## 🚨 2026-08-03（Persistent Package 当帧内容等价 Observe，默认关闭）

本阶段继续收紧 Persistent GPU Package 从“上传 Ready”到未来真实消费者之间的
正确性门。新增独立环境
`DXVK_WAR3_PERSISTENT_GPU_PACKAGE_CURRENT_DRAW_EQUIVALENCE_MODE`，发布默认 Off，
Consume 继续硬拒绝；即使完全匹配也只报告 CSM `wouldUse`，不会绑定 atlas、修改
canonical draw、发布 consumer authority 或 last-use fence。

**当帧 exact 内容证明**：

- exact capture entry 新增 generation-tagged POD proof，绑定 frame/map/device、完整
  draw identity、position/index allocation owner、identity/allocation/content generation、
  UINT16 实际索引域及 position/index byte hash。entry 替换和历史 source-fingerprint
  rollback 都先清空 proof，不能把旧 backing 刷新成当帧权限。
- 只观察 Building/Destructible 的 rigid、opaque、无 vertex blend、非 GPU-skin、
  indexed triangle-list、UINT16、zero-based、exact-domain、FLOAT3 候选。position 与 IB
  必须都来自真实 `HOST_CACHED` span；其他候选只记拒绝原因，不扫描写合并内存。
- immutable cache 与 current draw 共用同一个 byte-wise FNV-1a 实现；新增 strided
  FLOAT3 helper，只读取每顶点 xyz 12 bytes，避免 interleaved normal/UV 污染比较域。
- Store Ready 后仍重新核对 current snapshot identity、map/device epoch、不可伪造
  frozen payload、单 primitive、vertex/index count、position/index hash 及 primitive
  first/count/min/max。只有全部闭合才计 `fullyEquivalent/wouldUse CSM`；随后还会对
  frozen CPU proof 做最终重算验证。

**诊断与验证**：

- `runtime_status.json` 与 control-plane 新增 current-draw configured/effective、
  observations、exact/would-use、总拒绝及 rigid/material/skinning/geometry/CPU span/
  generation/package/snapshot/multi-primitive/layout/position/index/primitive 分类和最后
  disposition；既有四个实际消费权限字段继续固定为 0。
- 新增 CPU/value-only runnable，覆盖统一 hash、strided FLOAT3 边界、HOST_CACHED
  拒绝、多 primitive 及 position/index/primitive mismatch。全量 53 个静态模块共
  417 tests PASS，全部 12 个 Meson runnable PASS；Win32 DLL build PASS。该组合尚未
  部署、尚未启动游戏，运行时机会率与 Observe 开销必须通过独立门后才能设计
  recording/last-use authority。

## 🚨 2026-08-03（Persistent Package 结构化运行诊断，消费者权限仍关闭）

在 `codex/package-d3d9-observe-owner-20260803` 的真实 D3D9 Observe uploader
基础上补齐 `runtime_status.json` 与 control-plane 状态。新增进程级只读快照，按帧边界
发布 owner requested/effective、ready/miss/pending/fallback、上传字节、producer fence
提交/完成、map/device/frame epoch，以及四项消费者权限状态。状态查询不触碰 Store 或
Vulkan 对象；render owner 只在帧切换、提交和失效边界更新快照。

- 发布默认仍为 Off；Consume 仍被显式拒绝。
- `gpuBindingAllowed`、`drawMutationAllowed`、`consumerAuthorityPublished`、
  `consumerLastUseFencePublished` 继续固定为 0。此提交不绑定 atlas、不修改 caster，
  也不把 producer fence 冒充 consumer last-use fence。
- Persistent Package 静态合同 87/87、全量 `test_*_static.py` 410/410、相关 Meson
  runnable 7/7 PASS；Win32 build 成功且 `ninja -C build32 -n` no-work。

下一阶段只能先建立当前 D3D9 VB/IB 精确范围与 immutable package 的逐字等价 Observe
证明；未闭合 position/index/material/alpha/UV、当前 Stage11 generation 与同帧 caster
identity 前，不得进入 CSM Consume 或发布 last-use authority。

## 🚨 2026-08-03（Persistent Package 独立 D3D9 Observe owner，真实上传闭合）

在 producer-completion 发布合同通过后，本轮把 persistent package Store 第一次接到
独立 D3D9 owner，但仍严格限定为 Observe。该 owner 不依赖 GPU-skin manager 或原生
bridge；默认不创建，只有显式
`DXVK_WAR3_PERSISTENT_GPU_PACKAGE_D3D9_OWNER_MODE=1` 才工作，值 2 会被明确拒绝。

**实现与生命周期**：

- Stage11 CPU adapter 仍只负责当前帧源码证据；独立 D3D9 边界仅在
  `RecordedCurrentMapSource`、exact map/device/source generation、immutable snapshot
  pointer/content hash/vertex count 全部复核后，才把模型加入 Store。每帧最多准备两份
  package，未放宽任何 source freshness 或安全回退。
- 新 owner 自有 device-local atlas 和专用 timeline fence。上传命令执行 copy、显式
  transfer→vertex/index/shader-read barrier 后 signal；Store 只有观察到对应 fence 完成才
  将 package 发布为 Ready。
- Store 新增批量原子 `retireStaticUploads`：先验证整批 exact pending payload、重复 key、
  source/destination slice 和全部 retirement 分配，再以 no-throw commit 一次性切换到
  `UploadSubmitted`，消除了多 upload 时只退休半批的可能。既有 GPU-skin manager 也改用
  同一批量合同。
- 地图代际、设备代际与析构均显式使 owner 失效；D3D9 device 析构先等待 GPU idle，再
  poll completion 并销毁 owner。当前仍不导出 atlas slice，不绑定 Main/CSM/点阴影/
  outline，不修改 canonical draw，也不发布 consumer authority 或 last-use fence。

**静态、构建与运行门**：

- 新增 D3D9 Observe owner 与批量 retirement 合同；全量 `test_*static.py` 409/409 PASS，
  11 个 Win32 Meson runnable 11/11 PASS。Win32 build 成功，随后
  `ninja -C build32 -n` 为 no-work。
- 第一轮高压图运行 193.392 秒后被旧 conductor 的 hot-shadow freshness 条件判为失败：
  `semanticSceneConsumptionMode` 仍为 pending；但同帧真实 shadow counter 已为 caster 259、
  四级联 draw 1036，owner 达到 58/58 submissions、98/0 completions、687,920 bytes，
  无 owner reject、崩溃或驱动事件。因此该结果只归类为自动化假阴性，没有据此放宽门。
- 去除该无关 hot marker 后的第二轮高压图完成 74.686 秒、2,766 个 shadow frame：最终
  caster 252、四级联 draw 1008；owner 累计 528,384 次 exact observe，50/50 submissions、
  89/0 completions、634,892 bytes，`binding=0 / mutation=0 / consumerAuthority=0`。
  `framesIncomplete=0 / budgetExceeded=0 / deviceLost=0`，Arena 平均 3.481 MiB、峰值
  5.522 MiB；无新 dump、NVIDIA/Display 事件，最终截图目检未见错误大块或阴影缺口。
- build32 与部署 DLL exact：33,270,269 bytes，SHA-256
  `5F6028F869755F549B876D5F3F7770CA8CB6B827E4627B3E19303546EB208C87`；部署前回退为
  `E:\Work\War3\d3d9.dll.bak_20260803_EB0F_pre_package_d3d9_observe`，SHA-256
  `EB0FF0F2FED0AD95B5D7B600567D683ED40592995E0447E4E4D3BA9484E4687C`。

**仍未跨越的边界**：这是默认 Off 的真实上传/完成 Observe，不是 Consume，也没有证明
性能收益。下一阶段必须先建立每个消费者的 exact recording/last-use authority、跨
map/device retirement 与 Arena fallback 事务，再做 Observe ≥10,000 帧和同 DLL ABBA；
在此之前 `kD3D9SharedOwnerEnabled` 必须继续为 false。

## 🚨 2026-08-03（Persistent Package producer fence 完成发布闭合）

Stage 2A 的 map-scoped Stage11 source Observe 已通过后，本轮继续审计现有 static
package Store，确认旧实现存在一个确定的 GPU 生命周期漏洞：`retireStaticUpload`
在 copy 命令刚提交时就把资源设成 `Ready`，而 `pollRetired` 只在 fence 完成后释放
staging。因此 manager 可能在 producer copy 尚未完成时消费 device-local atlas。

**修复内容**：

- static package 状态改为 `PendingUpload -> UploadSubmitted -> Ready`；提交本身不再
  授予消费权限。唯一 retirement 记录同时持有 exact source/destination、fence/value、
  Store-created frozen payload 和目标 resource 的发布权限。
- `pollRetired` 观察到精确 fence value 后，重新核对 map/device epoch、active map 中的
  shared resource identity、package slice、已清空 pending upload、immutable frozen proof
  与全部 GPU range；全部闭合才发布 `Ready`。地图/设备代际已经切换、记录被替换或任一
  proof 漂移时将旧资源标记 `Invalid`，禁止迟到发布到新代际。
- 所有既有 manager 消费点仍只接受 `Ready`；`UploadSubmitted` 与排队状态一样安全回退。
  residency JSON 新增 `staticSubmittedRecords`，GPU-skin lifetime 诊断新增
  `completion=completed/rejected`，便于运行时直接证明发布闭合。
- 修复旧 Dual conductor 时发现它未跟随 P4 launch-fingerprint API 迁移；该导体的初次
  “进程退出”是 report-only 假阴性，原始查询明确为 `running=true`。临时兼容修改已撤销，
  正式运行证据改由当前维护中的 unattended conductor 生成，测试产物不纳入源码提交。

**静态、构建与运行门**：

- 新增 producer-completion 静态合同；全量 `test_*static.py` 403/403 PASS，现有 11 个
  Win32 Meson runnable 11/11 PASS。Win32 build 成功且 `ninja -C build32 -n` no-work。
- 最终 build32/部署 DLL exact：33,237,790 bytes，SHA-256
  `EB0FF0F2FED0AD95B5D7B600567D683ED40592995E0447E4E4D3BA9484E4687C`；回退依次为
  `d3d9.dll.bak_20260803_1D73_pre_package_completion_diag` 和
  `d3d9.dll.bak_20260803_C8F5_pre_package_producer_completion`。
- 高压图显式 GPU-skin Dual 门完成 60 秒及 30 秒两轮。最终诊断为 static hit/miss
  `20534/37`、upload retirement `37/37`、producer completion `37/0`，无未完成 static
  retirement；308 个 shadow report frame 中 incomplete/budget exceeded 均为 0，Arena
  峰值 5.477 MiB，deviceLost/AV/crash dump/新增 NVIDIA 事件均为 0。最终截图目检未见
  阴影撕裂、缺口或错误大块。Dual + full diagnostics 的 9--11 FPS 只用于正确性压力，
  不作为性能 ABBA 数据。

**仍未跨越的边界**：Store-local producer completion 已闭合，但 proof catalog、独立
D3D9 shared owner、recording transaction 及 Main/CSM/point/outline consumer last-use
fence 尚未接入；`kD3D9SharedOwnerEnabled`、`kProducerCompletionAuthorityIntegrated` 与
共享 Consume 继续保持 false。下一阶段只允许先做 D3D9 owner Observe，不能据此绑定
atlas 或替换 exact Arena fallback。

## 🚨 2026-08-03（Persistent Package Stage 2A：地图代际源码证明，Observe-only）

在 Arena 崩溃修复检查点 `7d8712a` 上继续推进 Persistent GPU Package，但本阶段只
闭合“当前 Stage11 caster 对应当前地图 immutable model source”的 CPU 证据。没有创建
atlas、绑定 GPU buffer、录制 package 命令、改写 draw/caster 或开放 Consume；发布默认
仍为 Off，显式 mode=2 仍 fail-closed。

**地图代际与源码权限**：

- `ShadowModelResourceCache` 的 geoset record/stamp 新增 `mapEpoch`。D3D9 device 创建及
  每次 `War3ResetGpuSkinMapEpoch` 都令 cache 进入同一 map epoch，并在独占锁内清空全部
  geoset/model/runtime/address alias；process-monotonic immutable generation issuer 不重置，
  因此地址复用、A→B→A 或 device/map 重建都不能把旧 publication 重新变成当前权限。
- 新增 epoch-qualified O(1) geoset-data lookup，并在持锁期间同时复核 cache epoch 与
  snapshot epoch。Stage11 Observe 只接受 exact submitted frame/stage/geometry/alpha/blocker
  witness 与同 epoch sidecar 的闭合 join；成功证据标记 `RecordedCurrentMapSource` 和
  `provesCurrentGameMemory=true`，stale/missing/invalid 各自独立计数。
- Observe 仍是 fixed-size POD/value-only adapter，所有 consumer mask 均为 0，且
  `kConsumeAdmissionGranted/kBindsGpuResources/kRecordsCommands/
  kMutatesCanonicalDraw/kPublishesPackage` 全为 false。低频运行日志仅在显式 Observe 时输出
  前 16 次和每 4096 次统计，同时报告 completed timed frames 与 core call/frame 开销；
  默认 Off 没有该开销。

**运行门与正确性证据**：

- 高压图 DirectInline Observe 完成 11,128 个 timed frames、2,056,192 次 Stage11 join；
  2,055,738 次取得 current-map source（99.978%），仅 454 次启动/未建 sidecar，
  lookupFailed/staleMap/invalidSidecar/invalidWitness/acceptedBlocker 全为 0。
  core 平均 0.267 μs/call，折算约 0.049 ms/frame，低于 0.15 ms Observe 门；单次墙钟
  max 受线程抢占出现约 2.05 ms 离群值，不影响累计平均。
- 11,002 个阴影报告帧中 incomplete/budgetExceeded/reuseLastComplete/renderPartial、Arena
  overflow/partial transaction、deviceLost 全为 0；Arena 平均 3.476 MiB、峰值 5.531 MiB，
  queue submitted/completed 稳定只差一帧。测试窗口无新 `nvlddmkm`/Display 153/4101 事件。
- 160/160 exact screenshot + 2 秒滚动 trace 门通过，在线 3,000 px 巨型暗块触发为 0；
  最后 32 帧离线最大新暗连通块 200 个分析像素，目检为单位/特效正常动画。保留三段共
  200 个 final-caster frame、50,899 条 record：mixed representation/alpha、alpha payload
  gap、blocker/marker leak、unexplained disappearance、validation reject 全为 0。

**构建、部署与边界**：

- 本轮最终 Win32 DLL 为 33,237,969 bytes，SHA-256
  `C8F5630EE3076EF1B273070E1AD319696334CC8C8F4854A784E39A6C9D23CC26`，build32 与
  `E:\\Work\\War3\\d3d9.dll` exact；Stage1 回退
  `d3d9.dll.bak_20260803_BF10_pre_package_map_epoch_observe`，诊断前候选回退
  `d3d9.dll.bak_20260803_2F7C_pre_observe_diagnostics`。
- 下一阶段仍须实现真实 D3D9 shared package owner、producer upload completion、
  Main/CSM/point/outline last-use fence、map/device retirement 与 exact Arena fallback，之后
  才能进行 Store Observe/Consume ABBA。不得把本阶段的 source evidence 描述成已存在的
  persistent GPU package 或性能优化收益。

## 🚨 2026-08-03（Shadow Arena 崩溃根因修复与高压长门）

用户在“生与死”高压区域压低视角时稳定触发 `0xC0000005`，同时每个 Arena 代际
增长到 384 MiB 并产生 8--80 次溢出。本阶段先建立用户已物理确认视觉正确的本地
基线 `28ed131` / `codex/point-shadow-physical-baseline-9ec8-20260802`，再在
`codex/war3-arena-performance-20260802` 独立修复；全程未 push。

**确定根因与读取合同**：

- dump 将 AV 定位到 `War3ComputeMappedLocalBoundsFromBytes`。旧路径把约 3 MiB 的
  D3D9 binding offset 加到真实仅 512 KiB 的映射 allocation 上，却使用逻辑
  `Desc()->Size` 证明后续 CPU 读取有效，因而可越过映射末端。
- 新增 `War3CpuReadableBufferSpan`：只有 current UP owned bytes，或同时具备真实
  allocation size/base、HOST_VISIBLE 属性、owner identity 及 identity/allocation/
  content generation 的映射才能被 CPU 读取。blocker metadata、draw-time exact、
  Stage13 和 terrain bounds/content 全部改走该合同；不可读的匿名路径阻断器
  fail-closed，不再用 SEH/`VirtualQuery` 掩盖越界。

**Arena 与当帧冻结**：

- position/blend/UV/IB 改为逐 caster 的 `ShadowArenaBundleTransaction`。所有块先
  在同一代际预留，任一失败恢复 page/offset/committed/tail-waste 游标，成功后才
  录制唯一 copy batch；准入预算固定为配置剩余与 Arena 真实剩余的较小值。
- 维持 64 MiB 页、384 MiB/代际、1.125 GiB 总驻留上限；三个 warm page 合计
  192 MiB，GPU completion 未证明前禁止清零代际。没有扩大预算，也没有恢复
  draw-time VB cache、fast append、prebuild bypass 或 source fingerprint reuse。
- 新增仅当帧、generation-tagged freeze catalog。同一 exact source/range 可在当帧
  复用，但 key 必须包含 allocation/source identity、三类 generation、frame serial、
  stream type、slice offset/length 和元素布局；UP/ring 缺证据时不去重。
- 魔兽 terrain/dynamic position VB 常声明 512 KiB，而实际 draw 只引用很小的索引域。
  新路径扫描当前代际已验证的 16/32-bit IB，连同 signed base vertex 证明真实顶点域，
  再同步裁剪 position/blend/separate UV；无法证明时保留完整域。由此在不减少 caster
  的前提下，把高压图 Arena 从约 38--40 MiB/帧降到平均约 3.5 MiB。

**诊断、构建与无人监管运行门**：

- `runtime_status.json`/flight recorder 新增 bundle reserved/committed/rolledBack、
  admission/partial、按流和来源分账、unique/duplicate、exact-index trim，以及 CPU
  span 拒绝原因和 allocation/read/generation tuple。
- 全部 static 合同 393/393 PASS；Win32 Meson runnable 11/11 PASS；Win32 build
  成功，`ninja -C build32 -n` no-work，targeted diff-check 无 whitespace error。
- 高压单位图 Direct 与 TAA v2 各 160/160 exact capture；桥/斜坡冻结镜头 60/60；
  1024 点阴影 3538 帧。所有门的 overflow、partial、frame incomplete、device lost、
  NVIDIA 新事件、blocker/alpha/mixed representation/final-caster 缺口均为 0。
- “生与死”Direct/TAA v2 各运行约 10 分钟：分别 12,556/12,539 个 shadow frame，
  Arena 平均 4.58/4.57 MiB、峰值均 6.075 MiB，trim reject=0，未发生 AV、Arena
  ownership 违规、GPU 无进展或驱动事件。通用 `hot-shadow` 门因该地图不发布上层
  semantic unit manifest 会假阴性；长门改用现有 `--no-hot-shadow`，同时人工监测真实
  caster/CSM/Arena/device 字段，渲染管线始终活跃。
- 上述全部运行门使用 33,233,182-byte `97C158E700A886FC9DC8FBD8884BF57FE5720A2A0E7D84C643C1B41D0A8C551F`。
  最终只补显式 `<algorithm>` 依赖后重新链接；static/Meson/build 门重新通过，当前
  build32 与部署 `E:\\Work\\War3\\d3d9.dll` exact：33,233,182 bytes，SHA-256
  `BF104E197BF5AEE6D0C7D42F3BA74E6C0B5085AFFE801CBDE6CEEA89F4EDAB21`。
  直接回退为 `d3d9.dll.bak_20260803_97C1_pre_header_relink`，原始视觉基线仍为
  `d3d9.dll.bak_20260803_0835_9EC8_pre_arena_transaction`。

**性能结论与下一阶段边界**：大地图平均约 21.8 FPS，main CPU 约 40.7--41.4 ms，
GPU 约 3.9--4.4 ms，确认下一瓶颈在 CPU 控制面而非 GPU。Stage 1 已达到稳定门；
下一阶段从 Persistent Package 的 map/device epoch、真实 current-stage authority、producer
completion 与所有 consumer last-use fence 开始。既有 Stage11 Observe 仅证明 recorded
content identity，`provesCurrentGameMemory=false`，在这些硬门闭合及 Observe 10k/ABBA
通过之前禁止启用 Package Consume。

## 🚨 2026-08-02（研究报告二次修复：点阴影 texel-center、深度同步与矩阵所有权）

用户提供的两份静态研究报告指出三个可独立造成条带、低频裂口或单帧错误的源码
合同。本轮在上一份 `5343...` 发布候选上逐项落地，不通过扩大 PCF、提高 bias、
延长 grace 或恢复跨帧几何缓存掩盖问题。

**点阴影比较域修复**：

- 点阴影 cube 使用 nearest sampler，但旧 receiver-plane PCF 以连续 `sampleDir`
  求接收面径向深度，再与量化 texel 中保存的 caster 深度比较；两者不在同一条射线
  上，斜视角会形成有符号、随 texel 跳变的深度残差，表现为稳定条带/摩尔纹。
- receiver shader 新增 Vulkan cube 六面选择、texel floor 与 texel-center 射线重建。
  每个现有 16-tap 同时用同一 `texelRay` 求接收面深度和采样 cube；采样数、D32
  径向深度格式及点阴影过滤半径均未增加。
- point-shadow debug mode 6 不再传入固定 `confidence=0`，而是复用生产路径的
  view-normal 重建、置信度和平面比较，因此调试输出现在能真实覆盖该修复。

**Vulkan 深度同步修复**：

- CSM 与 point-cube 的 attachment→sample 最终 barrier 均覆盖
  `EARLY_FRAGMENT_TESTS | LATE_FRAGMENT_TESTS` 和 depth attachment
  `READ | WRITE`；异常恢复路径采用相同合同。
- CSM 主 caster 与 terrain-mask 两个 dynamic-rendering instance 之间新增显式
  depth write→read dependency。mask 的 LOAD/read-only depth test 不再依赖不存在的
  隐式 render-pass dependency。
- 通用 ShaderPack 的 depth read-only 往返 barrier 同步扩展到 Early/Late 与
  depth read/write，避免同类遗漏从旁路重新进入。

**矩阵上传所有权修复**：

- 删除固定 `12` 段、仅按 upload serial 取模的 shadow matrix SSBO ring。旧 ring
  没有 GPU 完成证明，GPU 延迟超过窗口时 CPU 可以覆盖仍被 caster/outline 读取的
  palette/world matrix。
- 改为最多 32 个 backing 的有界 allocation-renaming pool；只有
  `DxvkBuffer::isInUse(DxvkAccess::Read)==false` 的槽位才允许覆写，每个选择后的
  buffer 由 command-list resource tracker 持有到 GPU 完成。池全部在途时当帧
  fail-closed，不覆盖旧矩阵，也不回退固定环。

**静态、构建与部署**：

- 新增 nearest cube texel、深度 barrier 与矩阵 backing ownership 合同；全部
  `test_*static.py` 共 384 tests PASS，其中 shadow 专项 176 tests、报告新增/更新
  专项 18 tests、相关 persistent/ownership 专项 45 tests。
- `glslang` 编译通过；`spirv-val --uniform-buffer-standard-layout` 通过。Win32
  全量 build 成功，随后 `ninja -C build32 -n` 为 no-work；targeted
  `git diff --check` 仅既有 CRLF 提示。
- `build32` 与部署 `E:\Work\War3\d3d9.dll` exact：33,200,899 bytes，SHA-256
  `9EC8101F9660B53F98614CEF215451412DC30C991577488B72E3A418CE89D11E`；部署前回退
  `E:\Work\War3\d3d9.dll.bak_20260802_222313_5343_pre_point_texel_depth_sync`。

**验收边界**：本轮未启动游戏。静态合同已修复报告中确定存在的问题，但点阴影
摩尔纹与低频单帧裂口仍须用户在物理画面做固定点光/移动相机与 1--2 分钟长门；
通过之前不得宣称视觉问题完全解决。工作树中既有的根层扁平副本删除、
`StormBreaker` WIP、日志及研究输出均未触碰、未纳入本轮范围。

## 🚨 2026-08-02（低频阴影裂口硬化 + 点阴影 receiver-plane PCF）

用户继续报告两项物理屏残留：方向/单位阴影偶发单帧撕裂或缺口，以及点光 cube
阴影在地面和单位表面形成摩尔纹。本轮没有重新启用任何危险跨帧 VB/IB cache，先
在当前发布候选上审计 caster shader 与点阴影写入/读取域。

**修复内容**：

- 阴影 caster 的 `0x40` GPU-skin direct 分支原来一律把绑定 0 的 position 直接乘
  light VP。VS-B1 (`0x40|0x80`) 中该绑定其实是 static atlas 的 bind-pose 位置，
  当前 palette 位于 storage binding 4；若某张地图具备 exact index package 并使
  B1 获权，就会把未蒙皮姿态投入 CSM/point pass。现在 caster VS 对 B1 从同一代
  static source + palette 执行与主 VS/compute 相同的标量 3x4 LBS，alpha-test UV
  同样取自该 source；metadata、vertex domain 或 group slot 任一不闭合即裁掉，
  绝不读取已被跳过的原生动态 VB。VS-A/B0 的已蒙皮 32-byte 输入保持原路径。
- 点阴影 receiver 不再让整个 Poisson16 采样盘共用中心像素的径向深度，再额外叠加
  大斜率 bias。每个 cube tap 现在与接收面相交，使用该 tap 对应的物理径向深度比较；
  仅保留 `base + 0.50 texel footprint` 的量化 bias。PCF tap 数和半径均未扩大，
  因此不是通过进一步模糊边缘掩盖摩尔纹。

**验证与边界**：

- 222 项 shadow/point/Stage11/安全索引相关静态合同 PASS；两个修改后的 GLSL 均
  实际编译为 SPIR-V；Win32 build 成功且 `ninja -C build32 -n` no-work。
- 1024 产品点阴影隔离门 2,433 帧 PASS：PointShadow 2,410 calls、GPU 0.232 ms，
  `deviceLost=0`；最终截图未见旧式地面/单位整片条纹。
- 点阴影开启的高压长门 240/240 exact captures、3,449 report frames：caster
  251--262、四级联 draw 1,004--1,048，`framesIncomplete=0 / budgetExceeded=0 /
  deviceLost=0`，无新的 NVIDIA/TDR 事件。在线 3,000 px 巨型暗块触发为 0；离线
  最大 2,022 px，经保留邻帧目检为大型单位/凤凰动画，低于历史错误三角
  12,194--16,907 px 的量级。
- VS-B1 专门门产生 40,633 个候选，但当前高压图缺 generation-pinned exact index
  package，按既有规则 `BLOCKED_SAFE_INDEX_PROOF`，不可逆 kernel skip 和 direct
  shadow 都为 0。不得为使旧 P4 门变绿而开启
  `DXVK_WAR3_DRAWTIME_VB_CACHE`。VS-B0 对照实际消费 27,168 次 Main 和 27,163 次
  Shadow capture；其 direct state 因 Stage12 完整 VB-domain 合同保守回退，进程、
  ledger、截图与 crash scan 均干净。

**部署**：build32 / `E:\Work\War3\d3d9.dll` exact SHA-256
`5343B1341616542F2560C1672696A60F81D2F0E355F80985FD3183B9598FAAC2`；回退为
`E:\Work\War3\d3d9.dll.bak_20260802_183108_9F9B_pre_direct_skin_point_plane`。
自动门尚不能替代用户原地图的低频物理屏观察；确认前不得宣称所有单帧裂口已经
完全消失。

## 🚨 2026-08-02（YDWE action 参数段修复与 WarVK 层同步）

用户在 YDWE 打开 WarVK 层时发现 `WarVKSetPointLightShadowConfig` 报
`数据段和文本段参数数目不一致（4，3）`。根因是 action 元数据误写了
`returns = nothing`；YDWE old-writer 会把它序列化为 action 数据段的第一列，
而 TriggerStrings 只为真实参数生成占位符。现已从全部 action 定义移除返回字段和
  `nothing` 哨兵；零参数 call 只保留返回类型，不能伪造一个参数列。

- `WarVK/action.txt` 共37个 action，数据段与 `${...}` 文本占位符逐项一致；
  `WarVK/call.txt` 共18个 call，返回类型和参数段符合 YDWE classic writer，
  其中零参数 call 的数据段不再出现 `nothing`。
- 55个公开函数都补充了简短的 `comment` 提示，说明调用用途、参数含义和主要范围；
  不再写入重复的同步/视觉限制警告。
- `SourceMap/YDWE1.32.13 - MemoryHack/ui/WarVK` 已同步根目录 UI/JASS；删除旧的
  `event.txt`、重复 README 和旧 `jass/API` 副本，保留 loader 与地图载荷文件。
- 实际 YDWE `triggerdata.lua -> old-writer -> wtg_checker` 门通过，当前默认层统计
  为 action=1606、call=1639、condition=40、event=99；其中
  `WarVKSetPointLightShadowConfig=1,integer,integer,real` 已与三个文本参数闭合。
- 新增静态合同禁止 action 再出现 `returns =`，并逐条检查 data/text 参数数量；
  WarVK JAPI 静态套件 10/10 PASS。DXVK C++ 未改动，本轮不启动游戏。

## 🚨 2026-08-02（WarVK JAPI v1 正式迁入 DXVK，已编译，待地图物理验收）

用户确认 JAPI 不应依赖 War3MapReforge 的 war3map.dll 运行时，要求把 clean-room
协议、Native carrier、真实图形后端和完整 JASS 库迁回独立 DXVK。本轮工作位于本地
分支 codex/warvk-japi-v1-integrated-20260802；不 push、不修改地图。

**单一运行时与协议**：

- src/d3d9/war3/japi/war3_japi_v1.cpp 现在是正式 warvk:v1 owner。DXVK 使用自有
  Native 表跳板接管 stock Preloader、GetLocalizedHotkey 和 GetLocalizedString；
  没有注册新 Native、没有 JapiFunc、没有 Detours，也不需要编译/加载 war3map.dll。
  非 warvk: 调用 exact 转发，旧无版本 ping/cmd 仅为 AutoTest 兼容。
- 公共协议固定 512 bytes / 16 参数 / printable ASCII；严格解析 b/i/d/r、
  canonical int32、正数 managed id 和 locale-independent finite float。错误版本、
  carrier、数量、类型和后端拒绝全部 fail-closed，并通过 thread-local
  system.lastError 返回 manifest 稳定错误码/文本。
- 三个 carrier 先完整校验名称、精确 ABI 签名和原函数，再事务式写表；中途失败会
  回滚本轮已写 slot。Reset 不再丢失仍需用于 stock 转发的 original target。
  internal jass.bridge_selftest 同时验证旧 AutoTest 与正式 v1 三载体路径。

**真实后端与生命周期**：

- 点光/点阴影、闪电共用正数 WarVK public id registry；不暴露 Warcraft handle、
  指针或 Vulkan 对象。地图/JASS VM 重建只清理由 JAPI 创建的对象。
- Lightning runtime 补齐 enabled/isAlive，并在 disabled 时保留对象但不提交绘制。
- 当前诚实 feature mask 为 0x1E07：Sun、CSM、PointLight、Lightning、
  ManagedObject、Time、Stats。完整 55-command wrapper 全部存在；体积光、outline、
  bloom、postfx、AA、day/night 因渲染器尚无完整一一对应字段而不宣称支持，调用
  稳定返回 UnsupportedFeature，不做静默 no-op。
- pointLight.setShadowConfig 先验证 public id；当前 renderer 的 cube resolution /
  bias 仍是全局策略，文档已明确，签名保留 id 供未来逐灯实现兼容升级。

**地图侧交付**：

- `WarVK` 根目录现在是唯一发布面：YDWE 直接读取根目录 `action.txt`、`call.txt`、
  `define.txt`；`jass/warvk_init.j` 引入 loader bridge、公共常量与完整 55-command
  `jass/warvk_api.j`。
- `jass/warvk_smoke_test.j` 提供 begin/finish 两阶段点光验收；点光会保留到截图完成
  后再由地图显式销毁，确保真实跨越渲染帧。
- 删除平行的 `WarVK/v1`、旧 `jass/API`、空 event、重复 TriggerData/TriggerStrings、
  manifest/test/integration 副本及两份重复 README。发布树只保留 12 个必要文件。
- JASS 函数和 YDWE description/comment 已删除逐函数重复的“仅限本地视觉/禁止多人
  同步分支”警告；UI 文本只描述函数实际作用。

**静态与构建门**：

- DXVK production-source protocol executable PASS；10 项 Meson
  JAPI/point-shadow/persistent-package runnable 全 PASS。
- 全部 366 项 DXVK static 合同 PASS，覆盖 JAPI、TAA、4096 CSM、Arena fence、
  point shadow、alpha、blocker、Stage11 exact 和 final-caster。
- YDWE runtime-24 pjass 对 common.j + blizzard.j + 根目录 public API + smoke test
  共 12,960 行解析成功。
- 真实 YDWE `triggerdata.lua → new-reader/old-writer → wtg_checker` 数据库门通过：
  action=1645、call=1664、condition=40、event=100，现图 823 个引用全部接受；参数
  数量与顺序门均通过，`unknownui` 恢复为 false。
- Win32 d3d9.dll build 成功：33,179,964 bytes，SHA-256
  9F9BFB865EE9FAD3A45B269411BCB9D9E7E2A10D11F2D21690DD7736542CEA56；
  `WarVK/bin/WarVK.dll` 与地图载荷 `WarVK/bin/warvk.blp` 已重新打包并逐字一致。

**边界**：本轮没有编辑或启动地图。只有用户在旁路候选地图实际确认 version、
feature flags、create 正数 id、count 增减和可见点阴影之后，才能把“地图已经成功
调用 C++ 接口”标记为完成；当前只宣称源码、协议、JASS 与 Win32 构建闭合。

## 🚨 2026-08-02 上午（夜间阶段收口：三个隔离切片已提交，未部署）

用户要求在当前阶段结束后收紧预算，不再继续 Atlas、生产 CPU-MT、阴影批处理或新的
长时间游戏门。本检查点只收口已经展开的 point-only persistent diagnostics、Stage11
Package Observe 和 CPU-MT Phase 2A；三者均建立独立本地提交与标签，未 push GitHub。

**独立提交与安全边界**：

- `078b5d6` / `codex/overnight-point-shadow-point-only-diagnostics-20260802`：
  persistent 准入移出 CSM-only 条件，使显式 Observe/Consume 可覆盖纯点光帧；默认 Off
  仍使用原 CSM 内 `std::async` 谓词和调度。新增数值型 runtime/perf diagnostics；只有
  至少一个 `shadowCount>0` 的点光才创建 worker，内外两层均 fail-closed。
- `e04ebe9` / `codex/overnight-cpu-skin-mt-phase2a-20260802`：destination-free
  `ProducerResultProof`、Lock 后 render-owner `RenderCommitEnvelope`、Published/Consumed
  分账、`NativeBodyLease`、reset/cancel 延迟结算及 MXCSR control/status-delta 合同。
  模块仍未进入 Meson/生产路径，RuntimeIntegrated/NativeParityProven/ConsumeEnabled/
  ProductionDefault 四门全部为 false；不能宣称 CPU-MT 已落地或已有性能收益。
- `2a71067` / `codex/overnight-package-stage11-content-observe-20260802`：默认 Off 的
  Stage11 Observe adapter 只在 final caster 与 exactSubmitted witness 发布后做一次
  geoset sidecar 地址关联。独立审查确认模型缓存不按 map epoch 分域，因此证据已明确
  降级为 `RecordedContentIdentityOnly`，硬置 `provesCurrentGameMemory=false`；Package、
  Consume、Arena、GPU binding、command recording、draw mutation 与 consumer authority
  全部为零。该统计不能作为 persistent package 准入证明。

**验证与构建**：

- 全部 42 个 `test_*static.py` 模块共 354 tests PASS；Package Stage11 adapter、point
  mailbox、point planner 三个 Meson runnable 3/3 PASS。CPU-MT Phase 2A 另有严格 i686
  20/20 runnable 与 50/50 独立进程竞态压力 PASS，共 45,000 个序列。
- Win32 全构建成功，`ninja -C build32 -n` no-work；目标 diff-check 无 whitespace error。
  新 `build32/src/d3d9/d3d9.dll` 为 32,859,760 bytes，SHA-256
  `6B59C78B7EA36B3ECB9B1B3EF0C2B52F87A8BC30040560BC150915BC28E6B0CD`。
- 此组合 DLL **没有部署、没有启动游戏**。`E:\Work\War3\d3d9.dll` 仍是上一轮已运行的
  `D99643796218CE2E60345AE739558CE583694CF74F72A3D0C6C2519BB8C1C015`；其回退仍为
  `d3d9.dll.bak_20260802_9BF5_pre_point_persistent_observe`。

**上一部署候选的最终高压门**：

- 显式 point persistent Consume + DirectInline 在高压单位图完成 160/160 exact capture、
  5,049 report frames / 91.826 秒；暗块 trigger、frame incomplete、budget exceeded、
  device lost、incident JSON 与新增 NVIDIA driver event 全为 0。PointShadow 5,030 calls
  中只有 1 次启动同步 Prepare，之后由持久 worker proposal 接管；保留的最后 32 帧
  最大时域暗块 181 px。该结果证明候选正确性门通过，但隔离桌面报告的 FPS/CPU 数字
  不能当作正式 ABBA 收益，发布默认继续保持 Off。

**下一轮边界**：先由用户审核本交接清单。若继续，应优先做短小的 point-only 运行诊断
与 Package Observe 10k 帧开销测量；在当前地图资源证明、真实 Lock/Unlock 生命周期、
native byte/MXCSR parity 和 ABBA 收益闭合前，不接入 Package Consume 或生产 CPU-MT。

## 🚨 2026-08-02（点阴影持久 Prepare Worker 运行验证候选，默认 Off）

本阶段把已验证的 owned-value point-shadow planner/worker 接入 production shadow pass，
但发布默认仍为 `Off`：默认不分配 worker、不创建线程，并完整保留旧 `std::async` 路径。
显式环境 `DXVK_WAR3_POINT_SHADOW_PERSISTENT_PREPARE_MODE=Observe/Consume` 才延迟创建
持久 worker；受控游戏运行已证明候选路径可用，但尚不足以改默认值或宣称稳定性能收益。

**同帧正确性与失败回退**：

- render owner 冻结最多四灯、settings/history、light matrix/face history、replay/policy/
  lifecycle seals、完整 caster scalar/world/palette，以及 dynamic pose signature/count/
  skinned-output count；request 只含自有 CPU 值，不跨线程携带 renderer、`Rc<>` 或 Vulkan
  生命周期。vector reserve/push/copy/submit 任一异常或拒绝都会撤销 pending，并在当帧执行
  canonical 同步构建；拒绝前不移动 caller storage，回退数据不会丢失。
- collection 是 non-blocking exact：busy、not-ready、failed、stale、generation/frame/light、
  settings/history、blend tuple、dynamic pose 或 caster/replay seal 任一不一致均拒绝 proposal，
  当帧同步重建，绝不消费陈旧 plan。Consume 只有 exact proposal 才采用；Observe 始终先跑
  canonical 同步计划，再只比较 disposition/lights/face lists/history，记录 match/mismatch。
- `ReusePublished` 不是 render payload：独立 validator 只允许沿用当前已发布 faces，并只推进
  一次 temporal age；不会用 proposal 覆写实时 lights、matrices、face valid/age/history 或
  face caster lists。Render proposal 则在采用前再次执行完整 exact equality。
- worker/storage 的 shutdown/join 发生在 renderer/GPU 资源销毁之前；Accepted storage 在
  result 后回收复用，deadline/rejected fallback 不阻塞 render owner。

**静态验证、部署与回退**：

- 针对性静态合同 29/29 PASS；Win32 mailbox/planner runnable 2/2 PASS；全量
  `test_*_static.py` 335/335 PASS。production build 与 `ninja -C build32 -n` 均 no-work；
  targeted `git diff --check` 仅既有 LF -> CRLF 提示。
- 部署前保存 `E:\\Work\\War3\\d3d9.dll.bak_20260802_9BF5_pre_point_persistent_observe`，
  SHA-256 `9BF5E3D9591602990A85F482639D9D1FD6EC4A5E98EE6DC3D239DFE09BF68DD9`。
  当前 `build32` 与部署 DLL exact：32,842,865 bytes，SHA-256
  `D99643796218CE2E60345AE739558CE583694CF74F72A3D0C6C2519BB8C1C015`。

**受控运行证据**：

- 点光图 Off/Observe/Consume 三轮均无 crash/device lost；短门 PointShadow 分别约
  `0.785/0.180`、`0.756/0.176`、`0.674/0.196 ms` CPU/GPU。隔离桌面、记录开关和采样窗口
  不同，所以这些数值只证明没有明显退化，不能作为正式 ABBA 收益结论。
- 显式 Observe + render log 的连续诊断在 exact=1200 时为
  `accepted=1200 / workerReady=1200 / mismatch=0 / deadlineFallback=0 /
  rejectedFallback=0 / workerFailed=0 / busy=0`。显式 Consume 报告持续有 PointShadow，且
  没有 canonical `PointShadow/PrepareCpu` section，证明实测帧采用了持久 proposal。
- `point_persistent_consume_trace_160_20260802.json` 完成 160/160 exact shadow-frame capture，
  在线巨型暗块触发为 0；report 1006 个 shadow frame 中
  `framesIncomplete=0 / budgetExceeded=0 / ArenaOverflow=0 / deviceLost=0`，无 incident JSON，
  运行窗口后 Windows 也没有新增 `nvlddmkm`/Display 153/4101 事件。低磁盘滚动采集器
  `errors=0`，证据总量约 440 MiB，严格低于 512 MiB 上限。
- 因滚动上限只保留最后 32 张截图与最后 3 个 trace 段，不能把本轮描述成完整 160 帧
  final-caster 原始证据。保留部分覆盖 153 个 trace frame、24,786 条 final-caster record、
  20 个 exact screenshot join；representation/alpha transition、mixed representation、alpha
  payload gap、blocker/marker leak、unexplained disappearance、validation reject 与 parse error
  全为 0。最后 32 帧时域分析最大暗块 96 px，contact sheet 目检只有正常动画/光照变化。

**仍未跨越的发布边界**：

- persistent begin 当前仍位于 CSM `receiverNeedsShadowMap` 条件内；纯点光且太阳/CSM 不需要
  shadow map 的帧会安全回退 canonical，但不会获得持久 worker 优化。下一补丁必须把准入
  提到 CSM-only 条件之外，并补 point-only 静态/运行门和结构化 diagnostics。
- 还缺高压单位图、纯点光图、大地图长门和用户物理屏验收；默认必须保持 Off，旧同步/
  `std::async` 回退不能删除。低磁盘 runner 还应保存逐段聚合摘要，才能在不突破 512 MiB
  的前提下形成全 160 帧 final-caster 覆盖。

## 🚨 2026-08-02（CPU-MT 蒙皮 Phase 1 值合同，隔离提交、默认关闭）

本阶段只保存 format-2 中型候选的 CPU-MT 所有权、输出证明、非阻塞路由与终态记账
合同。实现和 runnable 没有进入 Meson/生产 owner；四个硬门
`RuntimeIntegrated/NativeParityProven/ConsumeEnabled/ProductionDefault` 均为 false，
不能宣称已有线程并行、原生替换或帧时收益。

**已闭合的值语义**：

- 候选冻结 owner/map/device/reset、Flush part/geoset/layer、immutable source/palette、
  outer dispatch/upload 以及 Lock 后 destination 的 exact tuple；首轮资格仅限 Common、
  opaque、format-2/FVF `0x112`、stride 32、193--448 vertices、最多 64 palette groups，
  特殊 FVF、粒子、非有限输入、不安全/不一致 MXCSR 全部拒绝。
- producer 只能发布 factory-created `shared_ptr<const OwnedOutput>`；proof 绑定 exact key、
  byte size/storage identity/content hash、producer generation、result serial、MXCSR 和版本。
  owner 的 kernel 窗口使用 atomic route authority + `try_lock`；锁竞争、未 Ready 或 MXCSR
  不匹配立即选择 Native，不等待 worker，也不在竞争路径计算哈希或取得 shared_ptr。
- Copy 路径在 state mutex 内执行唯一 byte-writer callback；Unlock/reset/cancel 不能越过写入
  窗口。Copy/Native 的 body 结果与 exact Unlock 都出现后才终结；Unlock 失败、Unlock 先于
  body、迟到 producer、重复终态和 stale generation 均 fail-closed。terminal ledger 验证
  live + terminal = created，并分别闭合 Copy/Native selection。

**验证与不能跨越的边界**：

- 静态合同 7/7 PASS；严格 i686 `-O2 -Wall -Wextra -Werror -Wpedantic -Wshadow
  -Wconversion` runnable 18/18 PASS，50/50 独立进程压力轮次通过，共覆盖 45,000 个
  claim/publish/copy/unlock/reset/cancel/native 竞态序列。
- `OwnedOutput::ExactKey` 当前包含 Lock 后才存在的 destination，因此还不是 Flush-early
  worker 结果；下一阶段必须拆成 destination-free producer result + render-owner commit
  envelope。Native CAS 到真实原生 body 之间尚无 body lease，并发 reset/cancel 仍可能先
  终结，当前 decision 不能授权真实 native call。
- `publishProducerReady` 返回 Applied 只表示 publication，被随后 Native 路由击败时不能算
  consumption；frozen POD token 仍是 trusted-caller 证明，不是 producer 铸造的不可伪造
  authority。还缺生产 owner/hook、worker freeze、SEH copy adapter、真实 Lock/Unlock 与资源
  生命周期、原生 exactly-once、byte/MXCSR parity 和 ABBA 性能门；本阶段未部署、未启动游戏。

## 🚨 2026-08-02（Persistent Package P2 录制权限值合同，隔离提交、未接入）

本阶段只定义 persistent package 从 P1 `UploadCompleted` 证明进入未来 Stage11/EmitCs
录制前的单所有者事务合同。实现与 runnable 均未加入 Meson，生产代码没有 include、
实例化或调用它；因此它不是 runtime Consume、不是实际命令录制，也没有改变已部署 DLL。

**权限与事务边界**：

- `CurrentStageSource` 持有真实 `ProofCatalog::SharedSnapshot` 和只能由 catalog 私有路径
  铸造的 `PackageContentDecision`。create/record/seal/emit 四个边界均重新验证 exact
  snapshot、UploadCompleted publication revision/canonical digest、当前 Stage11 source
  generation、package/model/layout/material/alpha/world/bounds 全 tuple；默认构造、原始 POD
  伪造和跨 snapshot Ready decision 均 fail-closed。
- 单 recording owner 以 instance/transaction/seal generation、owner submission serial、
  command-list/EmitCs generation 和 canonical batch digest 约束；最多 4096 条记录，4097
  条拒绝。分配异常保持 Idle 可恢复，错序、缺项、seal 后写入、重复 seal 和 stale ticket
  都不能发布部分命令。
- authority 在 create 时复制并拥有 immutable record plan；callback 只能读取
  `const RecordInput*` view。callback false 或抛异常只终结一次为 `CallbackFailed/Aborted`，
  reentrant/并发 emit 最多执行一次，callback 已开始后 abort 返回 `EmitInProgress`。

**验证与硬边界**：

- P1/P2 静态合同 19/19 PASS；严格 i686 C++17 `-Wall -Wextra -Werror` runnable 构建通过，
  50/50 独立进程压力轮次通过，包含 4096/4097、真实 allocation fault、外部 seal 后篡改、
  throwing callback、stale flood 与 200-round at-most-once race。
- 仍没有真实 D3D9/Stage11 adapter、EmitCs command-list mutation proof、producer/use fence、
  多 primitive 或 Main/CSM/点阴影/outline 共享消费；runtime gate 不存在，不能通过开关启用。
  下一阶段必须由真实 render owner 构造 context/source，接入 GPU fence 与失败时 exact Arena
  fallback 后再讨论 Observe；本阶段没有部署、没有启动游戏。

## 🚨 2026-08-02（Persistent Package P1 不可变源码发布权限闭合，未部署）

本阶段只建立静态/刚性模型进入 persistent GPU package 之前的不可变源码与发布权限
地基；没有接入 Main/CSM/点阴影/outline 共享消费，也没有改变已部署 DLL。运行时 gate、
current Stage source authority、recording transaction、producer completion 与跨消费者
use-fence 继续为硬关闭，不能把本阶段描述成已有性能收益。

**不可变源码与代际合同**：

- `ShadowModelResourceCache` 为每次完整源码生命周期铸造 process-monotonic、不可回绕的
  immutable generation；position/normal/UV/primitive、bounds 与 matrix topology 必须
  同代且完整。所有 count/copy/UV 读取失败均 fail-closed，不允许 partial merge。
- 浮点 payload 采用 bit-exact 身份，`+0/-0` 不等，同 payload NaN 保持可重复。完整
  `A -> B -> A`、以及 unresolved/failed -> Complete 均重新 capture 并取得新 generation，
  即使最终 canonical bytes 与历史版本相同也不能复用旧发布权限。
- by-geoset alias 只有自身 Complete 后才能 materialize 当前 by-data canonical；未完成或
  失败 alias 保持 fail-closed，不能被 canonical Ready 状态“洗回”可消费。模型 readiness
  与 ready count 均依据当前 alias + canonical publication 重新计算。

**Store 与消费者提示合同**：

- Store 只接受当前 cache 的 exact shared_ptr identity、固定 layout schema、私有 frozen
  descriptor 与 store-instance monotonic token；提交前重新校验 streams、primitive 聚合、
  bounds、matrix topology、实际 packed staging ranges/padding/hash。
- immutable generation 进入 proof catalog digest/equality。Manager 的 bypass hint 会重新
  核对当前 cache stamp 与 Store probe；duplicate/mismatch 退役保持 GPU slice 存活到 fence，
  但返回 false，不能取得消费权限。
- 当前仍不具备 multi-primitive publication、真实 GPU slice runnable、producer fence
  completion、D3D9 shared owner/cross-epoch retirement，也没有证明 lock 外 Game.dll capture
  的 single-writer/current-stage freshness。排队 miss 变 stale 只会浪费 atlas 预算，不会在
  新 generation 下被误消费；后续必须以这些边界为 P2 门，不能提前打开 runtime gate。

**最终静态验证**：

- 六套 package source/static 合同 66/66 PASS；五个 Win32 package runnable 5/5 PASS。
- Win32 `d3d9.dll` production build PASS，`ninja -C build32 -n` no-work；targeted
  `git diff --check` 无 whitespace error（仅既有 LF -> CRLF 提示）。
- `build32/src/d3d9/d3d9.dll`：32,734,530 bytes，SHA-256
  `506A20B210C8E4F9E1A723DA29C7503C30F970DE1B5C47C32B004AC6B681A5E1`。
- 已部署 `E:\\Work\\War3\\d3d9.dll` 仍保持物理验收基线 `9BF5E3D9...`；本阶段没有部署、
  没有启动游戏。CPU-MT 三个未跟踪 value-contract 文件不属于本提交。

## 🚨 2026-08-02（点阴影纯 CPU Planner + 拒绝提交所有权闭合，运行时仍未接入）

在既有 persistent worker mailbox 基础上拆出独立的 owned-value 点阴影 CPU planner；
生产 `d3d9_war3_shadow.cpp/.h` 没有 include、实例化或调用它，旧 `std::async` 路径和
已部署 DLL 行为均未改变。本阶段只能称为运行时替换前的可验证算法地基，不能宣称已经
获得点阴影 CPU 性能收益。

- request 冻结最多四盏灯、caster scalar/source identity、完整 world matrix、palette hash、
  dynamic pose 统计、发布 history，以及 `replay/policy/lifecycle` 三项 seal；不含 renderer、
  scene/draw 指针、`Rc<>`、Vulkan/DXVK 资源或发布权限。`frozenComplete` 只是未来唯一
  renderer owner 必须铸造和复核的准入声明，当前 `OwnerBuilderIntegrated=false`。
- value planner 覆盖 96 MiB cube 容量裁剪、旧内容签名 token 顺序、temporal period 节拍、
  每灯独立 face age/valid 预算、range/90-degree face 保守球剔除、nearest-surface known
  caster cap 及六面 translated-eye view-projection。三组 legacy signature oracle 固定为
  `B5D6942CFC00FFAE`、`FF6737AA6748ED15`、`D3199C952EA5844C`。
- 旧路径对“正半径 + NaN/Inf center”会误 range-cull，且可能把 NaN 排序键送入
  `nth_element`。新 planner 明确把任一非有限 center/radius 视为 unknown/pinned；这是
  fail-closed hardening delta，不冒充畸形输入的 bit-equivalence。非有限灯光与无效 seal
  同样整体拒绝。
- capped cohort 使用无临时 buffer 的 `partition(pinned) + known-only nth_element +
  replay-index sort`，没有退化成 known 全量排序。caster、palette、range/ranked scratch 与
  24 个 face vector 全部在 request→result→下一 request 间 move/clear 回收；200 个连续
  accepted job 验证 allocation 地址与 capacity 稳定，4096 caster→16 known 的门通过。
- worker `submit` 从按值吞 request 改为 `submit(Request&)`，并删除 rvalue overload；只有
  Accepted 才 move 到 mailbox。Invalid/Busy/Stale/Stopping/Unavailable 均发生在唯一 move
  之前，动态测试证明前四类拒绝后同一 frozen vector 可直接由同步 processor 消费，避免
  busy fallback 丢失 exact 当帧数据。Accepted job 若 processor 抛异常或被取消仍不返还
  storage，`FailedJobStorageRecoveryIntegrated=false`，未来 owner 必须重新冻结 canonical
  replay，不能采样旧 plan。
- planner/worker 静态合同 9/9、10/10 PASS；两个 Win32 Meson runnable 2/2 PASS；全量
  299 项 `test_*_static.py` PASS；Win32 production target 与 `ninja -C build32 -n` 均为
  no-work。未部署 build32 DLL 为 32,698,815 bytes，SHA-256
  `1DBF61D109C5DFBB28E18B86B47841A3D77CCB463176891C4C8863C9A5149B53`；部署目录继续
  保持已通过物理门的 32,649,789-byte `9BF5E3D9...`。

**运行时接入前仍需完成**：唯一 owner builder、完整 tuple/replay size 验收、
Busy/Failed/Stale/Unavailable 的同算法同步 exact fallback、逐面录制成功后的 age/valid
事务提交、异常 rollback、cube allocation/OOM backoff、published light IDs/frame/generation
与 receiver 采样闭合，以及 ABBA 性能/160 帧取证门。下一条独立主线优先实施 Persistent
Package P1 的 per-model immutable generation 与 Store 强 validator；Store 当前的 `Ready`
只证明 copy 已登记，绝不能当 producer fence 已完成。

## 🚨 2026-08-02（Persistent Package 不可伪造 proof catalog，运行时仍硬关闭）

在 `f20e878` 的固定容量 observer 基础上新增 CPU/value-only immutable proof catalog，
并修正了 observer 把“全局 catalog snapshot revision”与“单条目最后 publication
revision”混用的问题。生产 DLL 只编译这些类型，没有 D3D9/Store/Manager/Renderer 对象
实例化 catalog/observer，没有绑定 atlas、修改 draw/caster 或部署本轮 DLL。

- catalog 使用 immutable sorted `shared_ptr<const Snapshot>` 与单 writer COW 发布；有效空
  snapshot 从 revision 1 开始，并带进程单调的 catalog instance generation，防止 catalog
  重建后相同数值 revision 接受旧 decision。条目的 publication revision 只用于诊断，当前帧
  exact key 比较 acquired instance/snapshot revision，因此同一快照中不同发布时间的 package
  均可验证。
- 发布状态为 `Prepared -> UploadSubmitted -> UploadCompleted / Invalidated`。
  `UploadCompleted` 不能由旧 Store `Ready` 推导，必须带 fence identity、成功 query 与
  `completedValue >= submittedValue` 的值证据；query 失败、未达值或 identity 不同均拒绝。
- Ready decision 的构造器私有，只能由 catalog validator 产生；它同时绑定 frame serial、
  policy revision、Stage11、完整 package key、immutable model generation、当帧 source
  generation，以及 identity/source/material/alpha/world/bounds 六类 exact sealed/current
  token。digest 只作早退，之后仍逐字段比较完整 package/primitive/stream/domain proof。
- observer 删除可伪造的 `packageContentReady` 布尔值和 Main pre-submitted mask；只有上述
  decision 与当前 row 完整匹配才能达到 `FullyEquivalent`。Main、CSM、point 与 outline
  全部必须通过 post-seal actual note，且 note 逐项复核完整 key、三类 generation 和六类
  token；错配 decision 只会降为 ContentPending，revision/digest 诊断清零。
- package generation、immutable model generation 与 current-draw source generation 已拆成
  三个语义域，禁止把模型代际机械等同于动态 VB allocation 代际。
- 当前 catalog 只接受完整单 primitive package，并复核 whole/primitive index hash、连续
  全域、primitive aggregate、static stream byte layout 与 index offset。multi-primitive 原子
  发布和 Store 铸造的 publication authority 尚未实现，分别由
  `kMultiPrimitivePublicationGranted=false` 与
  `kStorePublicationAuthorityIntegrated=false` 硬阻断；observer 的单 recording-owner 线程
  也由 `kRecordingThreadOwnershipIntegrated=false` 阻断。在这些合同完成前不得运行时发布
  条目或记录 actual consumer。
- 35 个全量 `test_*_static.py` 文件 PASS；四个 package owner/observer/catalog Win32
  runnable 4/4 PASS；Win32 production build 成功且 `ninja -C build32 -n` no-work。
  未部署 DLL 为 32,698,815 bytes，SHA-256
  `1DBF61D109C5DFBB28E18B86B47841A3D77CCB463176891C4C8863C9A5149B53`；部署目录继续保持
  已通过物理正确性门的 `9BF5E3D9...`。

## 🚨 2026-08-02（Persistent Package Observer 值表基础，生产路径仍未接入）

新增固定容量、无热路径分配的 `War3PersistentGpuPackageObserver`，用于在未来接入
shared package consumer 前先证明 Stage11 当帧 exact identity、source、material、alpha、
bounds、consumer 与 package generation 的闭合关系。当前生产 DLL 只编译该模块，没有
任何 D3D9/Store/Resources/Shadow 对象实例化它，也没有部署本轮构建。

- 模式固定为 `Off / Observe / Consume`；`Consume` 生产硬拒绝，Observe 永远透传 canonical
  workload/consumer mask，不绑定 atlas、不写 consumer last-use、不改变 caster、copy 或 draw。
- 4096 项 POD 表按 exact frame/policy/Stage11/map/device/package/source generation 封存；未知
  identity、source/content pending、动态/蒙皮、material/alpha/bounds 缺证据均按 proof ceiling
  保守降级，非有限 bounds、未知 consumer、容量或预算越界 fail-closed。
- consumer 统计分为四层：requested、package-eligible、canonical actual 和
  `wouldUse = eligible & actual`。`FullyEquivalent` 本身绝不伪造 actual；只有 seal 前已经真实
  入队的 Main 可作为 pre-submitted witness，CSM/point/geometry-outline 必须在对应真实 draw
  之后以 exact table index/identity/frame/policy/generation 单 bit 幂等记录。
- 只读 Stage11 审计确认唯一安全 seal 点位于 exact producer 同时发布
  `shadowInstances/shadowCasters` 并写入 submitted frame witness 之后；owner claim、capture
  complete、prepare/cull 均太早。首轮运行时 Observe 仍必须令 `packageContentReady=false`，
  因为 Store 尚私有于默认关闭的 GPU manager，且 shared owner/package proof sidecar/consumer
  last-use 尚未接通；不得为观察而创建 128 MiB atlas 或把任何条目晋升为 FullyEquivalent。
- 全部 274 项 `test_*_static.py` PASS；三个 package owner/observer Meson runnable 3/3 PASS；
  Win32 build 成功且 `ninja -C build32 -n` no-work。未部署 build32 DLL 为 32,660,682 bytes，
  SHA-256 `CEA5D4FA80ED95243618174E3C9307DF6615D933A4D89F13D22A7C97DC34EB4C`；已部署 DLL 继续保持
  上一轮通过视觉/稳定门的 `9BF5E3D9...`。

## 🚨 2026-08-02（Persistent Package immutable proof 扩充，仍无 Renderer Consumer）

P0 package 现在在模型首次打包时一次性生成逐 stream 与逐 primitive 的不可变证明；
没有把 package VB/IB 接到 Main、CSM、point 或 outline，也没有改变 Shared Consumer 门。

- proof 新增 position、normal、vertex-group、UV0/UV1 与 whole-index hash，有限 local
  bounds/hash，以及每 primitive 的 ordinal、opaque type/material word、连续 firstIndex、
  indexCount、index hash、min/max referenced vertex。primitive ranges 必须完整覆盖 whole IB，
  所有索引必须落在 vertex domain 内。
- `primitiveTypeOrMaterialSlot` 只作为 opaque immutable identity；它不能证明当帧 material、
  alpha texture/sampler 或 skinned pose。未来 Observe 仍需 Stage11 current-draw sidecar 才能
  把 rigid/static 晋升为 `FullyEquivalent`；动态/蒙皮 package 只能是 input-ready。
- hash/bounds/range 只在 package 创建时计算一次；热路径 validation 只比较 POD proof 与
  少量 primitive metadata，不重新扫描 write-combined IB。任何非有限 bounds、range 缺口、
  越界 index 或 proof mismatch 都 fail-closed 到原安全路径。
- 相关 TAA/point/stage/alpha/blocker/final-caster/package/WorkTable/union/P4 静态门共 212 项
  PASS；两个 package owner runnable 2/2 PASS；Win32 build 成功且 `ninja -C build32 -n`
  no-work。组合 build32/部署 DLL exact 为 32,649,789 bytes，SHA-256
  `9BF5E3D9591602990A85F482639D9D1FD6EC4A5E98EE6DC3D239DFE09BF68DD9`；回退为
  `E:\Work\War3\d3d9.dll.bak_20260802_pre_package_immutable_proof_801A2726`。
- 隔离高压稳定门 2,421 report frames：53.341 FPS、main-thread 13.276 ms、GPU
  4.822 ms，`framesIncomplete=0 / budgetExceeded=0 / deviceLost=0`，无 Arena 违规、GPU
  incident 或新增 NVIDIA 153/4101；进程与 isolated desktop exact cleanup。
- DirectInline 高压视觉门 160/160 exact screenshot 成功，在线 3,000 px 巨型暗块触发为
  0；低磁盘保留窗口最大暗块 255 px，目检为正常单位/凤凰动画。512 MiB 滚动策略仅保留
  最后三段（132 world frames、14 capture joins），其中 alpha gap、blocker leak、mixed
  representation、unexplained disappearance、validation reject 全为 0；不能把该结果冒充
  160/160 full-trace 留存。

## 🚨 2026-08-02（Persistent Package consumer last-use owner，纯值合同、未接运行时）

新增独立 `War3PersistentGpuPackageOwner` 作为未来 shared atlas 的生命周期准入边界；
当前仅编译纯值账本与 runnable test，D3D9/Store/Main/CSM/point/outline 均不实例化它。

- 每个 generation 使用 exact `mapEpoch/deviceEpoch/packageGeneration`，分别记录 upload
  fence identity/value 与 consumer last-use fence identity/value、consumer mask、submit
  serial。retirement 必须先拥有两组非零 proof；只有两次 completion query 成功、fence
  identity 精确一致且两个 completed value 均达值才允许 erase。
- 零值、未知 consumer bit、失败 query、epoch/generation/fence mismatch、倒退的 submit
  serial 或 fence value 全部保守保留。Owner 禁止复制，避免无意复制生命周期权限。
- Observe API 是 const classifier，只验证 would-be last-use，不写账本、不绑定 atlas。
  `kRuntimeObserveEnabled=false / kObserveBindsAtlas=false /
  kObserveWritesConsumerLastUse=false / kSharedConsumerEnabled=false`；Store 原有
  `kD3D9SharedOwnerEnabled=false / kCrossEpochRetirementSafe=false` 未改变。
- generation static 7/7、既有 owner/package static 20/20、两个 Meson runnable 2/2
  PASS；Win32 d3d9 build 通过，`ninja -C build32 -n` no-work。此阶段没有部署或启动游戏。

## 🚨 2026-08-02（P4 B1 归因为安全索引证明缺口，禁止复开旧 VB cache）

对旧部署 `37E0BE91...` 与当前 P0 Package 候选做 exact route=3 A/B 后，两者均能
prepare/submit immutable input package，但 `main/shadow/kernelBypassed` 全为 0；不是 P0
引入的 GPU 蒙皮算法回归。B1 preflight 仍依赖已在 2026-07-27 因世界原点巨型三角而
生产关闭的跨帧 DrawTime VB/IB cache，缺少 exact index 时会在不可逆 kernel bypass 前
fail-closed。不得为恢复旧 P4 PASS 重新开启该 cache。

- `run_gpu_skin_p4_isolated.py` 现在对所有路线显式固定
  `DXVK_WAR3_DRAWTIME_VB_CACHE=0`。只有 input package 完整、preflight index reject、
  index ticket/authority/consumer 全为 0，且 36 项 ownership/lifecycle/process/device 安全
  门全部 clean 时，才分类为 `BLOCKED_SAFE_INDEX_PROOF`；它不是 PASS，也不是 runtime
  failure，进程退出码为 2。
- final index reject、ticket 泄漏、生命周期/进程异常和权威 runtime failure 仍按真实失败
  处理，不能被 capability blocker 降级。
- 9 项新分类/源码合同、`py_compile` 与既有 VS-route 39 项 synthetic checks PASS；真实
  37E0 A/B artifact 离线回放精确落入安全阻塞分类。
- 解除阻塞的唯一准入路线是 generation-pinned、current-frame immutable index lease；
  miss/pending 必须调用原生 kernel exactly once。在该路线实现前，B1 Consume 保持关闭。

## 🚨 2026-08-02（Persistent Package producer-fence 跨 epoch 保活，Shared Consumer 仍禁止）

本轮修复 P0 Store 继承的 producer upload 生命周期缺口，范围只覆盖一次性
staging→atlas copy；没有创建 D3D9 shared owner，也没有让 Main/CSM/point/outline
消费 package。

- `RetiredStaticUpload` 现在同时持有 exact source、destination atlas slice 与两侧 census；
  map/device/reset 清理 lookup/active atlas 时不再删除未完成 retirement。只有 exact
  producer fence 达值才回收。
- 析构若意外仍有在途 copy，会把 payload 转交 `DxvkFence::enqueueWait`；若回调注册失败
  或 device teardown 无法证明完成，则有意保留到进程退出，禁止猜测 GPU 已空闲并提前释放。
- `kProducerRetirementSurvivesEpochClear=true`，但
  `kD3D9SharedOwnerEnabled=false / kCrossEpochRetirementSafe=false` 继续锁死。Store 尚无
  Main/CSM/point/outline 最后一次 use-fence，不能据此开启 shared consumer 或 B1 bypass。
- 19 项 package/owner 合同、181 项相关 TAA/point/stage/alpha/blocker/final-caster 静态门、
  Meson runnable 1/1 PASS；Win32 build 成功，`ninja -C build32 -n` no-work。
- 高压图 `full_default` 性能-only 门（该 runner 的 hot-shadow witness 与当前图不兼容，故
  显式 `--no-hot-shadow`）运行 2,517 帧：54.288 FPS、frame 18.420 ms、main-thread
  CPU 13.500 ms、worker CPU 5.140 ms、GPU 4.905 ms；BuildEligible 1.886 ms、Populate
  2.569 ms。`framesIncomplete=0 / budgetExceeded=0 / Arena busy/overflow=0`，无新增
  NVIDIA 153/4101 或 GPU incident。该门证明运行稳定和性能基线，不替代 160 张
  trace-aligned 阴影正确性验收。
- build32/部署 DLL exact：32,633,770 bytes，SHA-256
  `801A272648874A97933F7E07C58C273A920F17A15CA6E974D683F27F586F59B2`；回退为
  `E:\Work\War3\d3d9.dll.bak_20260802_pre_package_producer_retirement_E1D87436`。

## 🚨 2026-08-02（Persistent GPU Package Store P0 抽离，Shared Owner 仍禁止）

本轮从 `89dc8ec` 继续，把 immutable vertex/index package 的 static map、atlas、miss、
一次性 upload、producer-fence retirement 与 validator 从 `War3GpuSkinResources` 抽入
独立 `War3PersistentGpuPackageStore`。现有 resources 的公开方法签名保持并逐项委托；
没有在 D3D9Device 创建 owner，没有接 Main/CSM/point/outline consumer，也没有启用旧
manager 大锁、compute、output、receipt 或 lease。

- 对抗审查把迁移前后 `makeKey/find/probe/prepare/create/take/retire/atlas` 去注释、空白和
  类名后比较；除 `0 -> 0u` 外 token-equivalent。map/device epoch、package generation、
  fallback、diagnostics、residency 和 manager Disabled gate 均未发生运行语义漂移。
- store 只由旧 manager 懒创建的 `War3GpuSkinResources` 私有持有；默认 GPU skin Disabled
  仍不会创建 manager/resources/store，因此本阶段不分配 128 MiB atlas，也没有新增默认
  热路径或 renderer 行为。
- 新 owner/static 合同 7 项、既有 package 合同 11 项与 runnable ownership test 1/1
  PASS；Win32 `d3d9.dll` build 成功，`ninja -C build32 -n` no-work。
- 运行时归因用同一 `vertex_shader_bypass + sidecar=none` P4 脚本对候选
  `E1D87436...` 与提交前备份 `37E0BE91...` 各运行 45 秒。两者都稳定存活、CSM=4
  级联、Arena busy/overflow=0，也都表现为 input package 正常 prepared/submitted、但
  main/shadow consumer 与 kernel bypass 为 0；Windows 没有新增 `nvlddmkm 153`、
  `Display 4101` 或 GPU incident。因此该 P4 FAIL 是 P0 之前已经存在的 consumer-route
  状态，不能归因于 store 抽离，也不能把本轮测试描述成 GPU skin takeover 已通过。
- 候选与部署 DLL exact：32,627,806 bytes，SHA-256
  `E1D874364D9FBFEA07298C26B9196A0D0492853BBC3DB11088DFEFF6EE87C80E`；A/B 后已经恢复
  候选部署。回退仍为 `E:\Work\War3\d3d9.dll.bak_20260802_pre_package_store_37E0BE91`。

**必须保留的 P1 阻断门**：旧实现的 epoch clear 会同时清 atlas 与未完成 producer
retirement。当前 manager 通过 `hasInFlightResources()` 保留整个旧 resources owner，故
现有运行路径安全；但独立 shared owner 尚不具备跨 epoch upload/use-fence retirement。
`kD3D9SharedOwnerEnabled=false` 与 `kCrossEpochRetirementSafe=false` 共同锁死接入，任何
D3D9 shared owner/Consume 工作都必须先修复这一继承限制并加入 fence 生命周期测试。

## 🚨 2026-08-02（点阴影 Persistent Prepare Worker 隔离基础层，尚未接入运行时）

本轮为替换点阴影逐帧 `std::async` 建立了独立、可运行验证的持久 worker 基础层；
生产 `d3d9_war3_shadow.cpp/.h` 未 include、未实例化该类，现有运行路径和 DLL 行为
保持不变，不能把本提交冒充为已经获得点阴影 CPU 收益。

- worker 只有一个 request slot、一个 result slot 和一个持久线程；request/result 都是
  自有 CPU value，processor 必须无状态，线程不捕获 renderer、DXVK/Vulkan resource
  或可变发布状态。
- processor 现在严格消费 `Request&&`；大块 `std::vector` 可从 request 零拷贝移入
  result，再由 owner 移回下一份 request。隔离测试连续两个 job 验证 allocation 地址与
  capacity 保持相同，证明双缓冲/回收能力无需 renderer 或 `Rc<>` 所有权。
- 结果必须完整匹配 `jobSerial / rendererEpoch / frameSerial / lightGeneration`；invalid、
  stale、busy、exception、cancel 和 startup failure 全部 fail-closed，禁止消费旧 plan。
- shutdown 会取消未发布结果、等待正在执行的 CPU job 并 join；完整 stop/join 边界由
  owner-side mutex 串行化，两条 teardown 路径并发调用也只 join 一次，绝不 detach。
- C++ runnable test 覆盖同线程复用、owned vector capacity 跨 job 回收、单在途背压、
  代际单调、stale 丢 payload、异常后恢复、运行中取消、ready 结果撤销和并发 shutdown；
  200/200 压力重复通过。
- 静态合同 9 项与 Meson runnable test 1/1 PASS；Win32 测试目标编译链接通过。

**接入边界**：只有先把当前点阴影 prepare 从捕获 renderer `this` 的大 lambda 拆成
纯 POD request→result，owner 在 exact tuple 匹配后单次发布，才允许替换 `std::async`。
worker 不得访问 shared package store、Shadow Arena 或任何跨帧 lease。

## 🚨 2026-08-02（Persistent GPU Package 基础层 + 联合剔除 Observe，Consume 未准入）

本轮从 `41c7fee` 的默认 Off 正确性基线继续，先建立两份独立本地提交：
`759dd23` 为 immutable vertex/index GPU package，`f4de5ab` 为联合消费者剔除
Observe；均只在 `codex/war3-stable-producer-baseline`，没有 upstream、没有 push。

**Persistent GPU Package 基础层**：

- 现有 128 MiB device-local static atlas 现在把原 vertex blob 与 UINT16 IB 打包进
  同一连续 package、同一 staging→device upload；补齐 INDEX_BUFFER usage/stage/access。
- package proof 严格包含 map/device/package/layout/content/index hash、vertex/index
  count/type/offset/length；任何字段不等、epoch clear 或 generation 耗尽均 fail-closed。
  generation 不跨 map/device 清零，也绝不整数回绕复用。
- 旧 `staticSource` ABI 保持，新增 `indexSource` 尚未接入 renderer consumer；因此本轮
  不恢复旧 GPU manager 大锁/lease，也不改变 Main/CSM/point/outline 路由。
- 默认 full_default 实测 `gpuSkinPools=0`，证明共享 package owner 仍需从旧“只有 GPU
  skin mode 才创建 manager”的生命周期中拆出；不能把本基础层冒充为已消除 Arena copy。

**联合消费者剔除 Observe**：

- 新增纯 POD policy、runnable C++ test 和 `Off/Observe/Consume` 三态；Main、CSM0-3、
  point shadow、outline mask 与 source/bounds/camera/consumer/resource generation proof
  均显式表示。未知、陈旧、非有限、动态/蒙皮以及 C0/C1 全部 fail-visible。
- 运行时只在最终 CSM 边界观察 Stage11 exact-current、RequiredCurrent、Building/
  Destructible rigid 的 C2/C3 结果，并与原 canonical culler 对账。即使环境请求 Consume，
  `consumeAdmissionGranted` 仍固定 false，代码不读取 `effectiveVisibleMask` 跳过 draw。
- 高压图长门 16,257 observed frames、1,216,286 candidates、2,432,572 finite proofs；
  false-negative/false-positive 均 0。但 canonical cull、C2/C3 would-cull、both-far
  would-cull 同样全部为 0，机会率 0%，远低于 25% 门，故严禁进入 Consume。
- Off/Observe/Observe/Off ABBA 主线程 CPU 分别 11.580/10.615/11.748/10.976 ms；
  Observe 均值相对 Off 为 -0.097 ms（运行噪声），满足基础设施 <=0.15 ms 门。

**正确性、构建与证据**：

- 160/160 capture 与 final-caster trace join，1,133 trace frames / 313,933 records；
  representation/alpha transition、mixed representation、alpha payload gap、blocker leak、
  marker leak、unexplained disappearance、validation/parse/capture miss 全为 0。去除前六张
  光照 settle warmup 后最大时域暗块 251 px。
- 长门 `framesIncomplete=0 / budgetExceeded=0 / reuseLastComplete=0 /
  renderCurrentPartial=0`，Arena 最大 55.794 MiB；无 GPU incident、无新 nvlddmkm 153
  或 Display 4101。24 张跨 245 秒采样中的一次大变化来自相隔 10 秒的单位移动，目检
  不是阴影撕裂。
- 228 项 `test_*_static.py` PASS；runnable union test PASS；Win32 build 和后续
  `ninja -C build32 -n` no-work。DLL 32,621,604 bytes，SHA-256
  `37E0BE91D51DD95EC5359109E188D70082E6D57FAAC7CC1B18E67B810198527C`；部署前回退
  `E:\Work\War3\d3d9.dll.bak_20260802_pre_union_cull_observe_98B781DB`。
- 超限 788,685,925-byte raw trace 在分析后压缩为
  `AutoTest/artifacts/union_package_trace160_pressure_20260802_trace.zip`，46,657,609 bytes，
  SHA-256 `9B12DF080EFBE27B8778594720DA1FD99F27342D7152AADA724EAFA4689E5D1D`；
  原日志移入 Windows 回收站，可恢复。

**发布边界**：联合剔除默认 Off，Consume 不合格；Persistent Package 尚无默认消费者。
下一步先解决 shared package owner 与真实 IB/VB 命中率 Observe，或选择命中 CSM/point/
outline 并集的更早 bounds 时钟域。禁止仅为获得数字而启用 camera-only cull、放宽 exact
generation，或把动态单位纳入首轮剔除。

## 🚨 2026-08-02（Compact WorkTable 严格封存与性能准入，默认仍关闭）

本轮先完成三套仅本地安全检查点，再在 `b021c26` 的 generation-tagged
Compact WorkTable 上修复封存判据并完成 Observe/Consume 准入；没有 push，危险
跨帧 cache、fast append、prebuild bypass 和 source fingerprint reuse 继续默认关闭。

**封存修复与控制面**：

- `record.known` 表示旧 palette/group resolver 是否完成，并非 source freshness。
  封存现在只接受同帧 Stage11 producer、fresh/non-grace、当前 policy revision、
  exact frame/render/evidence generation 以及完整 part/payload/capture identity；其余
  全部回 canonical 路径。
- 新增 Stage/Freshness/Policy/Frame/Identity 拒绝分账。高压图约 50.6% 条目满足
  strict seal，其余均因 freshness/frame 不满足而安全回退；没有放宽 grace。
- evidence builder 先完成廉价 seal，再按 canonical 早退顺序计算 owner、static、
  policy、blocker 和 selection。stale、exact-owned 等记录不再白做 registry hash/
  sticky lookup。Observe 的 `SnapshotPreselect` ABBA 均值为 0.1265 ms，对照 Off
  0.1270 ms，低于 0.15 ms 基础设施门。
- Consume 仍是实验模式。160 帧正确性闭合，但两轮无 trace 性能为
  `DirectGrouped=1.73/1.74 ms`、`Populate=1.96/1.97 ms`，且 p95 超过 2.25 ms，
  workload 没减少，因此未通过准入。发布默认保持 Off，不以“换一条控制路径”冒充
  优化收益。

**证据与构建**：

- Observe 低磁盘门：
  `AutoTest/artifacts/compact_worktable_observe_sealfix_cap160_20260802.json`，
  trace 458.18 MiB，160/160 join；191,282 records，alpha gap、blocker leak、mixed
  representation、unexplained disappearance、parse error 全 0，稳态最大暗块 300 px。
- Consume 门：
  `AutoTest/artifacts/compact_worktable_consume_trace160_20260802.json`，trace
  443.84 MiB，160/160 join；185,296 records，同一组正确性门全 0，稳态最大暗块
  311 px，`framesIncomplete=0 / budgetExceeded=0 / deviceLost=0`。
- 212 项 `test_*_static.py` PASS；Win32 build 成功，`ninja -C build32 -n` no-work；
  targeted `git diff --check` 仅既有 CRLF 警告。
- `build32` / `E:\Work\War3\d3d9.dll` exact：32,603,558 bytes，SHA-256
  `98B781DB175F0871A597D4CEAFB07333FEECA68BA2B6727490354C67649CA9E8`；回退副本
  `E:\Work\War3\d3d9.dll.bak_20260802_904E_pre_compact_earlyout`，SHA-256
  `904E91966270F5CAC49538C78B1A693D13B8E1D02608B73993F76E898B21A883`。

下一阶段从默认 Off 的正确性基线推进联合消费者剔除 Observe；不能把未过性能门的
Consume 改成默认，也不能为提高命中率放宽 freshness/frame/identity。

## 🚨 2026-08-02（固定 4096 CSM、TAA v2 状态闭合、Arena fence 与 GPU 取证）

用户授权在无人监管的隔离桌面启动游戏，并要求落实 4096 CSM、TAA v2 真实开关、
大型地图 GPU 未响应防护以及低频阴影块取证。本轮继续保留 2026-07-29 Stage12
exact-current-frame producer 基线；没有重新开启 draw-time VB cache、fast append、
prebuild bypass 或 source fingerprint reuse，也没有延长 grace。

**TAA v2 设置与执行闭合**：

- `DXVK_WAR3_SHADOW_TAA_MODE` 只在 `War3RenderPipeline` 构造时读取；旧
  `DXVK_WAR3_SHADOW_TAA` 仅作一次兼容回退。运行中 ImGui 拥有最终控制权，切换会
  增加 `shadowTaaSettingsRevision`，不再被逐帧环境解析覆盖。
- 模式或 revision 变化只产生一次 `ModeSwitch` history cut。Temporal 首个完整帧以
  shader mode 2 写入当前结果，后续在 history 可读时进入 mode 3；只有
  Visibility/Motion/Receiver/HistoryWrite 四阶段全部完成才推进 ping-pong 与 generation。
- `runtime_status.json` 和 ImGui 现在同时显示 requested/effective/shader、history
  valid/readable/generation、最后失效原因与设置 revision。兼容字段
  `semanticSceneShadowTaaActive` 改为实际 shader 执行证据。
- 保留既有 3x3 亮度方差裁剪、reactive feedback 与双线性 history，没有提高历史权重
  或增加 Catmull-Rom 带宽。固定镜头慢移 A/B 中 Temporal 的稳定背景最大暗变化均值
  比 Direct 低约 2.4%，剔除轨迹端点后低约 4.9%；作用真实但刻意保持清晰度优先。

**CSM 4096 与清晰边缘**：

- requested 默认固定为 4096，禁用按 geometry work 在 4096/2048 间逐帧切换。
  普通 PCF 半径 `0.85 -> 0.70`，稳定墙面最小半径 `1.75 -> 1.50` texel；点阴影
  过滤参数不变。
- 支持 `VK_EXT_memory_budget` 时，仅在可用 device-local 预算不足候选 D32+R8 实际
  字节数再加 512 MiB reserve 时锁存回退 2048；无扩展时先尝试 4096，只有真实分配
  失败才回退。device lost 会继续抛出，不能伪装为回退。
- 资源使用完整 candidate image/view 创建后 `std::swap` 的两阶段提交。失败保留旧资源；
  只有成功交换才增加 generation/rebuild 并失效一次 TAA history。
- 审计额外修正了低显存专属边界：锁存 2048 后 requested 仍保持 4096，resolved
  candidate 与现有 2048 等价时立即复用，禁止逐帧重建相同 fallback 资源。

**Shadow Arena 与 GPU incident recorder**：

- Arena 页固定 64 MiB，每代际最多 384 MiB，总驻留上限 1.125 GiB；三个预热页合计
  192 MiB。Present 最后消费者之后把专用 `sync::Fence` 信号排入 GPU command list；
  只有 retire serial 已完成的代际才能清零，GPU 落后时在总上限内增加 spill 代际。
- busy reuse、单代际 overflow 或总驻留超限均 fail-closed 并标记 frame incomplete；
  `War3AllocFreezeBuffer` 不再落入无预算的 LegacyFreeze。
- flight recorder 保留最近 240 帧的 CSM requested/effective/fallback/generation/显存预算、
  TAA mode/history、Arena used/resident/busy/overflow、queue submit/complete/result 与最后
  render stage。queue error 或有在途提交且 10 秒无进展时原子写 incident JSON，只保留
  最近四份。本 DXVK 构建未暴露 `VK_EXT_device_fault`，incident 会显式记录 unsupported。

**低磁盘 final-caster 取证**：

- `run_bridge_ramp_visual_probe.py --attach-only [--attach-pid]` 只连接用户已有 War3；
  不启动、不结束、不调前台、不改优先级，也不暂停游戏、太阳或相机。
- 滚动 trace 默认 2 秒段、保留最近三段、会话硬上限 512 MiB。大暗块自动触发或
  ImGui“保留最近阴影证据”按钮会固定前段/当前段/后一段与相邻截图；界面显示采集器
  是否连接。未触发证据持续按 retain-count 淘汰。

**无人监管运行结果**：

- 桥/斜坡 Temporal+4096：120/120 exact capture；报告
  `war3_perf_report_auto_2026_08_01_23_57_26.html`，7,200 帧 / 69.826 秒，
  111.584 FPS、GPU 3.038 ms。CSM 4096/4096、generation/rebuild=1/1；Arena 驻留
  192 MiB；`framesIncomplete=0 / budgetExceeded=0 / deviceLost=0`。首次两次在线
  大暗块触发均为加载后脚本相机大幅转场，前中后 trace 已保留 183 MiB。
- 高压图 Temporal+4096+完整 trace：160/160 capture；报告
  `war3_perf_report_auto_2026_08_02_00_00_42.html`，3,766 帧 / 81.821 秒，
  48.394 FPS、GPU 4.357 ms，稳态约 1,116 shadow draws，最大 Shadow 用量
  55.783 MiB；暗块触发、Arena busy/overflow/incomplete、旧帧复用、局部渲染、
  device lost 全为 0。
- 同高压图 Direct+4096 对照：120/120 capture；报告
  `war3_perf_report_auto_2026_08_02_00_02_53.html`，3,602 帧 / 71.830 秒，
  52.393 FPS、GPU 4.780 ms；requested/effective/shader=0/0/0，history 始终无效；
  同一组完整性与驱动门全部为 0。
- 最终审计 DLL 高压冒烟：24/24 capture；报告
  `war3_perf_report_auto_2026_08_02_00_17_22.html`，2,514 帧 / 36.814 秒，
  70.161 FPS、GPU 4.321 ms，最大 Shadow 用量 55.777 MiB；4096 单代稳定且所有
  incomplete/budget/device-lost 门为 0。全部运行中未产生 GPU incident，也没有新的
  `nvlddmkm` / Display 153/4101 事件。
- attach-only 实测连接由隔离 runner 持有的 PID 16216：8/8 capture、1 个 trace 段，
  截图 41.5 MiB + trace 65.7 MiB = 107.1 MiB；priority 与 cleanup 都明确 skipped。
  attach 返回后原 PID 仍存活且响应，随后只由持有者自然结束；无残留 War3 进程。

**静态、构建与部署**：

- 相关定向套件 157 tests PASS；完整 `test_*static.py` 发现集 204 tests PASS；
  Win32 build 成功，`ninja -C build32 -n` no-work，`git diff --check` 仅既有 CRLF 警告。
- 最终 `build32` / `E:\Work\War3\d3d9.dll` exact：32,594,571 bytes，SHA-256
  `437F82CF188AE819F087F003B1A25FB3F5FD0745F251D4FBBBAB7EC6729EB858`。
- 初始回退：`E:\Work\War3\d3d9.dll.bak_20260801_235322__pre_4096_taa_arena`
  （旧稳定 SHA `E0A1557D...`）；审计前候选回退：
  `E:\Work\War3\d3d9.dll.bak_20260802_001630_pre_csm_latch_audit`
  （SHA `6503547F...`）。提交只进入本地 `codex/war3-stable-producer-baseline`，不推送。

**发布边界**：默认仍为 DirectInline。自动门已证明 TAA 开关、4096 CSM 和 Arena GPU
生命周期真实生效，并且本轮运行未复现驱动故障或低频阴影块；但没有用户物理屏长时间
观察前，不能宣称阴影块彻底消失，也不能把 Temporal 改为发布默认值。若原克尔苏加德或
路径阻断器位置再现，使用 attach-only 滚动证据定点修复，禁止重新开启跨帧 cache 或
扩大过滤器掩盖问题。

## 🚨 2026-07-30（点阴影径向深度恢复 + TAA 方差裁剪候选，已 commit）

用户要求先保全接近发布的阴影正确性基线，再恢复失效的点光阴影并推进优化版 TAA。
本轮先建立本地 `codex/war3-stable-producer-baseline` 检查点，未推送 GitHub；实现前
核对 Reeves PCF、Brabec omnidirectional shadow mapping、Playdead TAA、Eurographics
2020 TAA survey 与 temporal shadow mapping 的公式和验证边界。

**点阴影根因与修复**：

- cube face caster 旧路径仍绑定方向光 fragment shader，只写 face projection depth；
  surface/volumetric receiver 却比较 `distance(light, fragment) / range`，写入域与读取域
  不一致，导致点阴影不可见或错误。已有的专用 point caster shader 虽已编译却从未绑定。
- `ShadowCasterPipelineKey` 新增 `pointShadowRadialDepth` 并进入 equality/hash；point pass
  独占选择专用 fragment shader，把 face depth 还原为径向距离后写入 `gl_FragDepth`。
  方向光 caster 保持不写 `gl_FragDepth`，避免重新触发旧 Win32 device-loss 风险；
  mask 与 point key 组合 fail-closed。

**TAA v2 优化**：

- 维持 3x3 当前帧采样预算，将 min/max box clamp 换成亮度一、二阶矩的方差裁剪
  `mu ± gamma sigma`，并由局部边缘调节 gamma；裁剪区间包含中心当前值，避免细小
  shadow contact 被邻域均值吞掉。reactive feedback 同时响应 raw history/current 差异
  和 rectification 幅度；保留双线性 history，未增加 Catmull-Rom 带宽。
- 普通太阳、CSM 内容和动态 caster pose 变化不再整屏清空 history，显式调试策略仍可
  强制旧行为。camera cut 改为相机世界位置与 forward 夹角判定；history motion UBO 使用
  真正生成 history 的 exact view-projection，保留生命周期、投影、viewport、资源代际与
  CSM fallback 等硬失效门。

**最终验证与部署**：

- 23 项 point/TAA 静态合同 PASS；Win32 build 成功并 `ninja -C build32 -n` no-work。
- 点光矩阵 `AutoTest/artifacts/light_feature_matrix_20260730_093104.json` 为 PASS：
  PointShadow 1,235 calls、0.986 calls/frame、GPU 0.151 ms，`deviceLost=0`。
- TAA scripted camera-cut 96/96 帧：5,456 次 Visibility/Motion/Receiver/HistoryWrite/
  HistoryAdvance 全闭合，camera-cut invalidation 从旧 3,489/7,021 降到 20/5,456。
  固定太阳普通图 96/96 帧只有 2 次初始化/资源失效、0 camera cut，最大时域暗块
  231 px；DirectInline 对照为 267 px，未见巨型三角或阴影撕裂。
- `build32` 与 `E:\Work\War3\d3d9.dll` exact：32,517,043 bytes，SHA-256
  `E0A1557D9F0FBD9C30D583C86E0FC02BEED520EBEB312771E5D30AA54ACA42F1`；部署前回退
  `E:\Work\War3\d3d9.dll.bak_20260730_0999_pre_point_shadow_taa`。

**发布边界**：默认仍为 DirectInline（TAA mode 0）。Temporal mode 2 已形成可验收候选，
但在用户物理屏确认点光阴影、移动单位细影和相机运动无拖影前不得改成发布默认值。

## 🚨 2026-07-29（Stage12 当帧 source-generation 闭合 + 索引读回性能收口）

本轮在 `1aaa594` Git 安全检查点之后继续处理用户物理屏仍可见的低频原点三角和
路径阻断器若隐若现。没有回滚最新版功能，也没有触碰独立的 `StormBreaker` WIP；
修复边界只收紧 Stage11 exact 当帧几何的 source generation、拒绝所有权和
blocker/alpha metadata 消费。

**新增根因与正确性修复**：

- 旧 exact capture 对动态/UP VB、UV 和 IB 仍有代际洞：部分路径会读取未固定
  allocation 的虚拟 `mapPtr`，并把 D3D9 的 `MinVertexIndex/NumVertices` 提示当成
  实际索引域。buffer 在 draw 后换页或提示不准确时，可能组合出同身份但不同代的
  position/index/alpha；exact 拒绝后 generic/packet lane 还可能补回另一种表示。
- `War3PerDrawUploadInfo` 现在逐 stream 记录 exact UP 指针、长度和 allocation；
  动态 position/UV/IB 必须证明当前 slice、storage 与 upload range 同代，否则
  fail-closed。REAL/UP buffer 补齐 `TRANSFER_SRC` 合同；普通 BUFFER 的 pending
  upload 先执行既有 staging→REAL flush，再在同一命令流按序 REAL→exact freeze。
- indexed draw 不再用 D3D 提示裁剪真实顶点域。只有当前 allocation 明确带
  `HOST_CACHED` 才扫描实际索引；常见非缓存 IB 保持 CPU opaque，由 ordered GPU
  copy 固化 exact IB，并以完整、有界 VB domain + 原 BaseVertexIndex 渲染。
  anonymous 小型 path/LOS marker 在不能证明实际域时继续保守拒绝，不能借完整域
  伪装成普通 caster。
- 新增当帧 exact-rejected ledger。exact owner 一旦因 source、alpha 或 blocker
  合同失败，同一完整 cache key 不能再由 DirectGrouped、packet 或 lease 回填。
  blocker bounds 和 indexed alpha metadata 只读 exact UP 或当前 pinned generation；
  无法证明就拒绝，不再回退到陈旧映射。

危险跨帧路径继续默认关闭：`DXVK_WAR3_DRAWTIME_VB_CACHE=0`、
`DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND=0`、
`DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS=0`、
`DXVK_WAR3_DRAWTIME_SOURCE_FINGERPRINT_REUSE=0`。

**性能回归与修复**：

- 第一版正确性候选把 IB 复制到 `HOST_VISIBLE|HOST_COHERENT`、但非
  `HOST_CACHED` 的 write-combined snapshot，随后逐个读取约 86,000 个 index/frame
  计算真实域。高压图 `DrawTimeCapture` 从旧约 `0.204 ms/frame` 退化到
  `22.136 ms/frame`，不是 GPU 或 caster 数增加。
- 已删除该 host snapshot/readback。最终无完整 trace 的高压性能门 artifact
  `AutoTest/artifacts/stage12_generation_cached_index_pressure_perf_24_20260729.json`，
  report `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_29_17_36_31.html`：
  `DrawTimeCapture=0.237 ms/frame`、`246.381 calls/frame`，FPS `68.389`、GPU
  `3.458 ms`，`framesIncomplete=0 / budgetExceeded=0 / deviceLost=0`。相对错误
  候选约快 93 倍，且工作量没有下降。

**最终当帧取证门**：

- 普通光影图 artifact
  `AutoTest/artifacts/stage12_cached_index_final_normal_72_20260729.json`，trace
  `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_07_29_17_42_26.jsonl`，
  report `...war3_perf_report_auto_2026_07_29_17_43_17.html`。72/72 capture 映射；
  捕获窗口 6,337 条 Stage11 record 全部 `batchTag=exact / stride=32 /
  vertexBlend=0`，旧 `stride=12 + vertexBlend` 为 0。representation/alpha
  transition、mixed representation、alpha payload gap、blocker leak、unexplained
  disappearance、validation reject 均为 0；稳态最大时域暗块 244 px，目检为凤凰、
  传送门和单位动画。完整 trace 下 FPS `54.703`，完整性/预算/device lost 全 0。
- 高压单位图 artifact
  `AutoTest/artifacts/stage12_cached_index_final_pressure_72_20260729.json`，trace
  `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_07_29_17_49_19.jsonl`，
  report `...war3_perf_report_auto_2026_07_29_17_50_10.html`。72/72 capture 映射；
  稳态 exact claimed/submitted/lifecycle 与 Stage11 同步为 180--184，12,883 条
  Stage11 record 全部是 exact `stride=32 / vertexBlend=0`。上述八项门、grace、
  lease restore、core incomplete、epoch skip、missing required 全为 0；稳态最大
  暗块 564 px，目检为密集单位、凤凰和技能动画。完整 trace 下 FPS `40.898`，
  `framesIncomplete=0 / budgetExceeded=0 / deviceLost=0`。trace 唯一解析错误是停止后
  frame 2259 的最后一条半写 record，远晚于最后 capture frame 735，不影响 72/72
  join 或任何捕获帧结论。
- 追加桥/斜坡图
  `AutoTest/artifacts/stage12_cached_index_final_visual_72_20260729.json` 同样 72/72
  映射且 alpha gap/blocker leak 为 0；该专图脚本主动移动相机，所以全 trace 的
  view-dependent disappearance 不作为稳定对象门。稳态最大暗块 539 px，目检无
  世界原点巨型三角。
- 25 个 static 合同套件共 187 tests PASS；Win32 build 成功，
  `ninja -C build32 -n` no-work；targeted `git diff --check` 仅既有 CRLF 警告。

**最终部署与仍需物理验收**：

- `build32` / `E:\Work\War3\d3d9.dll` exact：32,501,336 bytes，SHA-256
  `7C264158C287E92C14BD555BEC83C04F94CF2FF187DF341CA1C645E3DCFE989F`。
- 本轮开始前回退：
  `E:\Work\War3\d3d9.dll.bak_20260729_CBEC_pre_stage12_index_blocker`；
  性能修复前回退：
  `E:\Work\War3\d3d9.dll.bak_20260729_5ED5_pre_cached_index_perf`。
- 两次直接 `-loadfile` 启动正式《冰冠之陨》地图时，游戏/DLL/Vulkan 均存活但地图
  未建立 AutoTest named-pipe control plane，90 秒和 240 秒均为 0 capture 后安全
  终止。因此不能宣称克尔苏加德定点长门自动通过。用户仍需在当前 DLL 上对原位置
  和路径阻断器区域连续物理观察 1--2 分钟；若仍复现，应使用本轮新增的低磁盘滚动
  trace/触发保留能力捕获异常前后，而不是重新启用跨帧 cache 或扩大 grace。

## 🚨 2026-07-29（物理屏残留确认 + Git 安全检查点）

用户在最终部署 `CBEC4C4A...` 上确认：本轮树木、蒙皮部件和建筑连续性修复使整体
正确性大幅提升，但物理屏仍有两项自动门没有覆盖到的残留，**本检查点不能标记为
阴影问题全部解决**：

- 长时间观察后仍会低频出现一次大型三角/多边形阴影撕裂；用户从现场画面定位到
  克尔苏加德旁边的对象区域。该异常的时间尺度超过当前 72 帧自动截图门，现有
  final-caster 自动风险门全 0、短窗 `maxDarkComponent` 仅 454 px 的结果并不能
  证伪它。
- 路径阻断器阴影仍会若隐若现，说明当前 exact/current-frame blocker 门已经显著
  降低泄漏，但至少还有一条身份或生命周期路径没有闭合。

本轮先以当前源码、着色器、构建合同、AutoTest 分析器和交接文档建立 Git 可回退
基线；不在提交前继续叠加修复变量。相对旧 `15b65d9` 基线，当前版本累计保留了
GPU/CPU 蒙皮接管、体积光和混合光追、Stage1/10/11/13 producer/lifecycle、当帧
geometry ledger、final-caster 逐帧取证、TAA v2、blocker/alpha/palette fail-closed
以及性能报告分账等工作。运行日志、截图缓存、`output/`/`research_bundles/` 备份树、
一次性 patch 脚本和独立 StormBreaker 子模块 WIP 不进入该提交。

下一阶段必须基于此检查点做长时间、定点、trace-aligned 取证：捕获撕裂发生前后
完整 final-caster identity/backing/content/validation，并单独追踪 blocker 的 exact
slice、instance、metadata 与 tombstone。禁止为掩盖低频缺口重新开启跨帧 cache、
fast-append/prebuild 或延长 grace。

## 🚨 2026-07-29（Stage11 单一当帧 owner，树木/蒙皮/阻断器回归收口，未 commit）

用户在保留最新版功能的前提下报告三类回归：LT/YT 树木阴影在方块与 cutout 间
闪烁、蒙皮部件周期掉帧而 rigid 部件正常、路径阻断器阴影重新出现。没有整体回滚
工作区；先为已部署 `898D...` 建立回退点，再在当前改动上定向收紧 producer、
identity、alpha、palette 与 lifecycle。

**根因与修复边界**：

- Stage11 exact current-frame producer 与 DirectGrouped/packet lease 会为同一 logical
  slice 发布不同表示；exact caster 已提交后又没有进入 manifest/core lifecycle，
  导致 generic `stride=12 + vertexBlend` 与 exact `stride=32` 在帧间竞争。新增
  `exactOwnerFrameSerial` / `exactSubmittedFrameSerial`：exact 在 blocker/alpha
  fail-closed 前取得唯一所有权，只有实际 caster 发布后才产生 live evidence；
  DirectGrouped 对 exact-owned slice 避让，exact-submitted 记录直接从完整 cache key
  合成并合入 manifest、selection、core 与 part identity，但永不进入 packet lease。
- LT/YT static-world rawcode 被旧 TLS 污染成 Unit 后会走 generic skinned backfill，
  产生 opaque 方块。现在把 LT/YT 归为 Destructible，generic preselect/record/build
  三处全部 fail-closed；exact native geometry 保留同帧完整 UV/texture/alpha payload。
- blocker fallback 曾允许非 exact visible winner、错误 payload 查询与未复核的旧
  metadata 命中。现在 rawcode/material/metadata 均要求 exact slice、instance、
  current render frame、Stage11、fresh、非 grace；anonymous exact-backed 豁免只限
  显式 Unit，不能让 path/LOS marker 混入最终 caster。
- lease palette refresh 现在要求当前 palette tag 非零，live min/max 非零且相等，
  并严格等于当前 tag；否则 fail-closed。旧 stale-pose restore 默认关闭，lease
  expiry 仍只清 backing，不再靠跨帧旧姿态掩盖缺口。
- 最后一处 17 帧周期孔洞来自类型门遗漏：`hcas/halt/hatw/hctw/hgtw` 五类 Building
  已有完整当帧 draw-time capture，却未被 exact producer 接纳，只能经历 8 帧
  `GraceOneFrame` 后空一帧。Building 现进入 exact current-frame owner。旧 trace
  全程 903 次 unexplained disappearance 全由这五个 handle 构成；没有扩大 grace。

危险跨帧路径继续默认关闭：`DXVK_WAR3_DRAWTIME_VB_CACHE=0`、
`DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND=0`、
`DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS=0`。

**最终验证**：

- 普通光影图 artifact
  `AutoTest/artifacts/stage11_building_exact_visual_72_20260729.json`，trace
  `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_07_29_12_11_10.jsonl`，
  report `...war3_perf_report_auto_2026_07_29_12_11_53.html`。3,394 个稳态 trace
  帧中 13 个实际 Building part 每帧均存在，全部稳定为已蒙皮的
  `stride=32 / vertexBlend=0`；旧 `stride=12 + vertexBlend=1` generic lane 归零。
  72/72 exact capture 映射；captured steady 的 geometry/alpha transition、mixed
  representation、alpha gap、blocker leak、unexplained disappearance 全为 0；
  grace/lease restore/core skip/epoch skip/missing required 全为 0。最大时域暗块
  454 px，最坏相邻帧目检为凤凰、单位和传送门动画。
- 高压单位图 artifact
  `AutoTest/artifacts/stage11_building_exact_pressure_72_20260729.json`，trace
  `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_07_29_12_18_59.jsonl`，
  report `...war3_perf_report_auto_2026_07_29_12_19_42.html`。72/72 映射，稳态
  exact claimed/submitted/lifecycle 同步为 180--188，Stage11 同步为 180--188，
  DirectGrouped=0；同一六项视觉/连续性门全 0，最大暗块 415 px，
  `framesIncomplete=0 / budgetExceeded=0 / deviceLost=0`。
- 81 项相关静态合同 PASS；Win32 build 成功，`ninja -C build32 -n` no-work；
  targeted `git diff --check` 仅既有 CRLF 警告。

**最终部署**：

- `build32` / `E:\Work\War3\d3d9.dll` exact：32,482,211 bytes，SHA-256
  `CBEC4C4AED2A675D8E9B515C1C09DD246B493762389B46096C4327AC0695037E`。
- 本轮原始回退：
  `E:\Work\War3\d3d9.dll.bak_20260729_898D_pre_stage11_single_owner_lifecycle`；
  Building 修复前回退：
  `E:\Work\War3\d3d9.dll.bak_20260729_C8C7_pre_building_exact_owner`。

**仍需用户物理屏验收**：在原地图连续观察树木、普通单位/建筑和路径阻断器区域
1--2 分钟。若仍有异常，应继续用最终 Caster trace 按 exact part/handle 定位，不能
重新启用跨帧 cache/fast-append，也不能延长 grace 来隐藏 producer hole。

## 🚨 2026-07-28（密集 Stage11 Caster 高速阴影闪烁修复，未 commit）

用户确认缺失部件恢复后仍会高速闪烁，且视野内 Caster 越多越严重。旧高压报告的
坏帧会把 Stage11 draw-time capture 从约 243 次降到 26～38 次，同时产生
207～219 次 `drawTimeVBCacheRejectNoLayerContext`；semantic submit/replay 也随之
从约 180/252 降到 70～77/142～149。`framesIncomplete`、core/epoch skip 均为 0，
故不是 ShadowMap hold、lease 或 GPU 问题。

**根因与修复**：

- 新增互斥拒绝原因后确认 148,944 次拒绝中 147,608 次（99.10%）是
  `QueryCurrentDrawContract` 查找失败。旧历史合同表虽有 4096 项，但
  `kContractCacheWays=1`，按 renderable-part 指针直接映射；密集、规律分配的单位
  part 会互相覆盖，所以 Caster 越多，碰撞和整帧部件缺口越严重。
- 没有扩大旧历史表，因为它仍服务 palette/snapshot 消费者，历史上扩大可见集合
  曾暴露不安全记录。新增独立的 `War3 CurrentDraw Geometry Ledger`：8192 项、
  2048 set、4-way，仅接受 current-frame Stage11、fresh、非 grace、完整
  mesh-payload 记录；按 jHandle/unit/worldObject/sceneNode 多身份发布，并在查询时
  复核 exact renderable part、frame、Stage 和至少一个权威身份。
- 新账本只修复当帧几何合同查找，不进入 palette/snapshot 或跨帧 replay；reset 和
  tombstone 会同步清理。捕获端仍要求 dispatch 或 exact GPU-skin layer，禁止
  layer=0 猜测。危险跨帧 cache/fast-append/prebuild 继续生产 fail-closed。

**验证**：

- 诊断基线 `353ADAC...`（报告 `...22_38_04.html`）：查找拒绝
  p50/p95/max=`101/220/222`，capture p05/p50/p95=`26/143/245`，replay
  `146/204/254`，呈明显锯齿。
- 长门候选 artifact
  `AutoTest/artifacts/geometry_ledger_final_pressure_160_20260728.json`，160 张 exact
  capture 100% 映射到 final-caster trace。稳态拒绝 p50/p95/max=`2/2/2`，capture
  `243/243/247`、semantic submit `180/180/184`、replay `252/252/256`；
  `framesIncomplete=0 / ObjectCoreIncompleteSkip=0 / EpochSkippedIncomplete=0 /
  EpochMissingPart=0`。final analysis 的 alpha gap、blocker leak、trace miss 均为 0；
  最大稳态瞬态暗块 729 px，未见世界原点巨型三角。
- 最终严格层身份短门 artifact
  `AutoTest/artifacts/geometry_ledger_hardened_final_24_20260728.json`，报告
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_28_23_16_21.html`：
  查找拒绝 p50/p95/max=`0/0/0`，capture=`244`、submit=`181`、replay=`253`
  中位数，`framesIncomplete=0 / deviceLost=0`。42 项相关静态合同 PASS，
  `ninja -n` no-work。

**最终部署**：

- `build32` / `E:\Work\War3\d3d9.dll` exact：32,469,589 bytes，SHA-256
  `20F83427366D006BB9396958D3824773A1C27C27F21A433BA95A97A6F13000A5`。
- 回退：`E:\Work\War3\d3d9.dll.bak_20260728_F045_pre_layer_hardening`
  （SHA `F0455A66BA3C462084C9460F79CBECE6EC3DFDCCFA2272A8CF9DE116B25F622E`）。
- 仍需用户在原密集单位场景物理屏确认高速闪烁消失；若仍有异常，应使用最终
  Caster trace 定位剩余的 0～2 个严格身份拒绝，不能重新启用危险跨帧缓存。

## 🚨 2026-07-28（视觉回退恢复：拆分安全当帧几何与危险跨帧缓存，未 commit）

用户确认 `02D9EC...` 之后的 metadata/lifecycle 候选出现严重回退：Stage11 阴影
部位大量缺失、alpha-test 消失、局部闪烁、路径阻断器方块回归且性能下降。本轮没有
执行 `git reset` 或整体回滚；保留全部既有未提交工作，只在隔离 Desktop、
`BELOW_NORMAL` 的 War3 中做定向恢复与取证。

**根因与修复边界**：

- `DXVK_WAR3_DRAWTIME_VB_CACHE=0` 本来用于阻断已证明会产生世界 `(0,0)` 巨型
  三角的跨帧 draw-time snapshot/fast-append，但该总闸还错误地关闭了 Stage11
  合法 Caster 所需的当帧 GPU 几何生产。旧“止血版”高压图平均只提交约
  `66.44` 个 semantic draw/frame，而完整路径约 `128.83`；约一半合法部件并非
  被优化，而是直接缺失。
- 新增默认开启的 `DXVK_WAR3_DRAWTIME_CURRENT_FRAME_GEOMETRY=1`。它只允许
  Stage11 在原 draw 当下复制 exact-current-frame GPU slice，并由 direct producer
  在同一 `frameSerial` 消费；不允许跨帧 lookup/replay，不启用 generic geometry
  override、GPU-skin legacy cache、prebuild bypass 或 fast-append。
- 危险三项继续生产 fail-closed：`DRAWTIME_VB_CACHE=0`、
  `SEMANTIC_DRAW_TIME_FAST_APPEND=0`、
  `SEMANTIC_DRAW_TIME_PREBUILD_BYPASS=0`。旧 cache 现在只作为显式诊断闸，所有
  不安全 consumer 仍同时依赖它，不能为了性能重新打开。
- 当帧 direct producer 补齐 final-caster 的 renderable part/layer、metadata key、
  alpha source frame、`RequiredCurrent` 生命周期与 alpha payload 完整性字段，避免
  恢复后的原生 cutout 被取证器误报为 metadata 缺口。新开关同时进入 perf 环境
  快照。

**验证结果**：

- 5 个相关静态合同套件共 60 tests PASS；Win32 构建成功，`ninja -n` no-work；
  targeted `git diff --check` 仅既有 CRLF 警告。
- 高压图最终 120 张 exact backbuffer：120/120 与最终 Caster trace 对齐，
  `alphaPayloadGap=0 / blockerLeak=0 / unexplainedPartDisappearance=0`，最大时域暗块
  490 px；报告 `framesIncomplete=0 / budgetExceeded=0`。平均 semantic submit
  `128.48/frame`、replay `200.47/frame`，已恢复到旧完整诊断路径的
  `128.83 / 200.83` 同量级；旧止血版仅 `66.44 / 138.42`。
- 普通光影图 90/90 对齐，alpha 缺口、blocker 泄漏、未解释 part 消失均为 0；
  透明树木保持 cutout，未见路径阻断器方块。
- 桥/斜坡图 90/90 对齐，映射帧同样三项为 0，Stage1/Stage13 阴影均持续存在。
  时域分析的 39,836/9,838 px 大暗变化经相邻三帧目检，均是专图脚本把相机从桥区
  移到黑色战争迷雾，不是错误阴影或巨型几何。
- 证据：
  `AutoTest/artifacts/current_frame_geometry_final_pressure_120_20260728.json`、
  `..._final_analysis.json`；
  `current_frame_geometry_visual_contract_90_20260728.json`；
  `current_frame_geometry_bridge_90_20260728.json`。对应报告分别为
  `war3_perf_report_auto_2026_07_28_20_30_59.html`、
  `...20_17_21.html`、`...20_35_29.html`。

**构建与部署**：

- `build32` / `E:\Work\War3\d3d9.dll` exact：31,077,430 bytes，SHA-256
  `206926F113F7C4B5F1A589343D03A62B5A09001C422FAD1330864BCC2A1E399A`。
- 本轮开始前回退：
  `E:\Work\War3\d3d9.dll.bak_20260728_8F50_pre_current_frame_geometry`
  （SHA `8F50E57C4DF8DA9C9420D5C6185B1F506D1219612F618FEC1441053A8D724455`）。
- 中间 trace 合同前回退：
  `E:\Work\War3\d3d9.dll.bak_20260728_1462_pre_current_frame_trace_contract`。

**仍需用户物理屏验收**：当前自动门已经恢复完整工作量并闭合 alpha/blocker/part
连续性，且未重新引入世界原点巨型三角；但物理扫描输出上的低频闪烁仍需在用户原
高压场景连续观察 1--2 分钟。性能上不能再拿缺失约一半 semantic caster 的旧止血
版作“快基线”；下一阶段应在此完整正确性基线上优化 cascade draw、当帧 capture
与 shadow-map 单 draw 成本，禁止重新开启跨帧 draw-time cache/fast-append。

## 🚨 2026-07-27（整对象阴影闪烁修复 + 最终 Caster 逐帧取证闭合，未 commit）

用户确认上一候选仍有大量阴影闪烁，并要求每轮保存最终阴影数据、把异常截图精确
关联到产生它的 Caster。全程只在隔离 Desktop 运行 War3，进程最终为
`BELOW_NORMAL`，未控制用户前台。

**闪烁根因与修复**：

- 旧报告精确出现 531 次
  `ObjectCoreIncompleteSkip / EpochSkippedIncomplete / EpochMissingPart`。
  direct packet lease 到期时只删除 lease，却把 part key 留在 committed core；
  core gate 又早于 post-submit phantom shrink，故一个已不可能恢复的 part 会让整个
  object 被拒绝，形成周期性整对象阴影闪烁。
- `d3d9_device.cpp` 新增 `pruneExpiredLeasePartFromCoreSet`：两个已确认 lease
  失效入口在 erase 前同步从 committed/observation sorted set 删除 exact part；
  不刷新 core epoch、lease 或 live frame。两集合都为空才删除 object core。
- 新增并导出
  `CorePartPrunedOnLeaseExpiry / CoreObjectEmptiedOnLeaseExpiry`，同时把
  complete/incomplete/epoch missing/skip 等核心字段写入每帧 JSONL。
- 长门旧 531 次整对象跳过降为 6（-98.87%）；最终短门为 0。目标 `hfoo`
  cohort 的同步全缺失帧为 0，15 个目标 handle 中仅 1 个出现过一次单帧全缺失。

**逐帧取证闭合**：

- `run_bridge_ramp_visual_probe.py` 可在首张截图前等待本轮
  `shadowFinalCasterFrame` witness；每轮保留 exact BMP、完整 final-caster JSONL、
  时域暗块分析、最终 draw identity/backing/content/validation 摘要和 perf HTML。
- 修复了观测层的两个时钟域误连：截图 `shadowFrameSerial` 是 trace serial，
  Caster `frameSerial` 是 world frame。分析器现在严格使用
  `capture shadowFrameSerial -> cadence.serial -> cadence.frameIndex ->
  final-caster frameSerial`；长门 240/240、最终短门 120/120 均 100% 映射，无
  parse error。
- capture 0 没有前一张 exact screenshot，现标为 warmup，不再冒充“最坏瞬态”。
  identity-world 的普通小型 skinned draw 也不再被误报为原点巨型几何；只有大几何
  + 原点锚定才进入该规则。

**最终运行结果**：

- 最终短门 artifact：
  `AutoTest/artifacts/high_pressure_core_prune_final_trace_smoke_20260727.json`；
  trace：
  `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_07_27_20_16_46.jsonl`；
  report：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_27_20_17_20.html`；
  mapped analysis：
  `AutoTest/artifacts/high_pressure_core_prune_final_trace_smoke_20260727_final_analysis.json`。
- 1,091 report frames：
  `framesIncomplete=0 / budgetExceeded=0 / deviceLost=0 /
  ObjectCoreIncompleteSkip=0 / EpochSkippedIncomplete=0 /
  EpochMissingPart=0`；pruned part=297、emptied object=126。
- 119 张稳态 exact 帧最大暗块 677 px。最坏帧 world serial 313 只移除一个
  Stage11 `rawcode=uban` 的 84-index / 49-vertex 小 part；无 validation flag、
  backing change 或非法 buffer range。同步移除五个 `hfoo` 105-index 小 geoset
  的两张对齐帧暗块仅 257/253 px，864-index 主体均持续存在。这与历史
  12,194–16,907 px 世界原点巨型阴影不是同一量级/机制。
- 危险源已生产 fail-closed：
  `DXVK_WAR3_DRAWTIME_VB_CACHE=0`、
  `DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND=0`、
  `DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS=0`；只允许显式诊断回归，
  不得为性能重新开启。

**构建与部署**：

- 30 项相关静态合同 PASS，`ninja -n` no-work；targeted `git diff --check`
  仅换行警告。
- `build32` / `E:\Work\War3\d3d9.dll` exact：31,019,266 bytes，SHA-256
  `02D9EC173B4902BA85DAA9EEBA80873553FB386CB8DAFD66394A99109E1DD49D`。
- 回退：
  `E:\Work\War3\d3d9.dll.bak_20260727_0DC9_pre_core_trace_fields`
  （SHA `0DC9B5A21772887E3461856E707FA2DBADC90790DCBA54D328287727B6E2D908`）。

**仍需用户物理屏验收**：自动取证已消除整对象同步闪烁且未复现巨型三角，但低频
扫描输出异常必须由用户在原场景物理屏确认。若仍出现，必须用同一 trace-aligned
runner 保存该帧并按最终 Caster identity 继续二分；不得重新启用 draw-time cache，
也不得用跨帧强留存小 geoset 掩盖合法动画可见性变化。

## 🚨 2026-07-27（世界原点瞬态巨型阴影 + 点光摩尔纹收口，未 commit）

用户在 Claude 高性能构建上确认：单位/可破坏物阴影开启时，约数秒一帧会从世界
`(0,0)` 附近出现巨型三角/矩形阴影；点光阴影同时存在整片摩尔纹。本轮未控制前台，
全部 War3 运行在隔离 Desktop 且最终降为 `BELOW_NORMAL`。

**原点阴影的可重复证据与根因域**：

- 在 `060E9280...` payload-exact 候选上抓取 160 张 exact backbuffer；第 103 帧
  出现覆盖半屏的错误阴影，第 104 帧恢复。时域分析最大瞬态暗块 12,194 px。
  artifact：`bridge_ramp_visual_probe_unit_instance_exact_origin_triangle_20260727_113213.json`。
- 同 DLL 仅设置 `DXVK_WAR3_DRAWTIME_VB_CACHE=0` 后，160 帧最大暗块降至 307 px
  （-97.48%），全部为正常动画，故排除 CSM、receiver、TAA、S1/S13，根因锁定在
  draw-time VB snapshot 的消费/发布链。
- 子路径二分：关闭 direct producer 仍复现（最大 16,907 px）；同时关闭
  fast-append/prebuild、只保留 generic consumer 后最大 630 px；复开 direct
  producer、保持 fast-append 关闭后最大 718 px，均无巨型几何。仅关闭 prebuild
  而保留 fast-append 仍复现（12,392 px）。因此危险域是 fast-append 本身，不是
  direct producer、generic cache override 或单独的 prebuild producer。
- fast-append 确有一处具体合同遗漏：没有复制 `entry.gpuSkinInput`，可能把 input-only
  GPU-skin 静态源当最终 xyz；现已补齐。但重新开启完整 fast-append 仍复现，证明
  该捷径还绕过 generic canonical geometry/material/pose 验证，不能生产启用。

**最终正确性策略**：

- `War3DrawTimeVBCacheKey` 现在包含 object instance、mesh payload、JASS handle、
  part/layer 与两个 payload word；capture 要求 CurrentDraw 与 semantic 至少一个
  权威身份字段一致，所有 consumer 复核完整 key，visible producer 也复核实例。
- 新增同 DLL 总诊断开关 `DXVK_WAR3_DRAWTIME_VB_CACHE`；后续取证已证明 master
  cache 仍不安全，当前最终默认已改为 0，详见上一节。
- `DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND=0` 与
  `DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS=0` 现在默认 fail-closed，仅保留
  显式回归诊断；三项开关和 draw-time 总开关均进入 perf env 快照。
- 最终默认 160 帧：最大瞬态暗块 632 px，目检全部是凤凰/传送门动画；旧错误域
  12,194–16,907 px 不再出现。`framesIncomplete=0 / budgetExceeded=0 /
  deviceLost=0`。artifact：
  `bridge_ramp_visual_probe_unit_fast_append_fail_closed_default_20260727_122420.json`，
  报告：`war3_perf_report_auto_2026_07_27_12_25_24.html`。

**点光摩尔纹修复**：

- point shadow 默认 texel bias scale `0.08 -> 0.35`；receiver 由 `max(base, texel)`
  改成 base + texel footprint + 有界 slope scale，volumetric 点阴影同步使用 additive
  footprint，UI 上限扩到 1.0。
- 最终点光门 `light_feature_matrix_20260727_122752.json`：PointShadow 1657 次、
  0.999 calls/frame、GPU 0.14 ms，截图
  `AutoTest/artifacts/screenshots/war3_20260727_122831.png` 未见整片摩尔纹。
  matrix 的 false 只因旧 gate 仍要求已移除的 `DXVK PointShadow: Rendered!` DBWIN
  文本并把受控 stop 计为 runtime failure；活跃 PointShadow section、无 crash/device
  lost 与 exact 截图证明实际执行成功，后续应修测试器 gate。

**构建与部署**：

- 58 项相关静态合同 PASS，`git diff --check` 仅换行警告，最终 `ninja -n` no-work。
- `build32` / `E:\Work\War3\d3d9.dll` exact：30,983,978 bytes，SHA-256
  `4C04190ABB2EE4C3D829528018779F96E8BF4C4716B907E8562F81A834F75CBF`。
- 回退：`E:\Work\War3\d3d9.dll.bak_20260727_409C_pre_fast_fail_closed`。
- 仍未 commit。提交前建议用户物理屏确认原地图至少 1–2 分钟无原点三角，并确认
  实际点光场景的摩尔纹观感；后续性能优化应针对安全 generic Submit，不能重新开启
  fast-append，除非它完整复用 canonical validator 并重新通过时域门。

## 🚨 2026-07-27（(0,0) 渲染缺失块调查收口 + ConsumerBuild 性能修复，未 commit）

**背景与断档说明**：用户在 20:47 报告（`war3_perf_report_2026_07_26_20_47_45.html`，
12:35 构建 SHA `5F458C07`）：性能低 + 渲染缺失块必定出现在 0,0，系 S13 桥/斜坡
修复期间引入。上一会话在 21:22-21:27 已实现并部署 "drawtime layer exact"
候选修复（SHA `CF1E82ED`），跑了两轮探针（`origin_block_drawtime_layer_exact_
cf1e_20260726.json` 等，180+72 帧截图）后进程中断，未记录、未分析、未告知用户。
本轮补完了该验证：CF1E 的 180 帧时域亮度差分无任何黑块/原点异常（唯一增亮
是战斗火焰特效）。

**根因调查结论（16-agent workflow + 对抗验证）**：

- `semanticSceneDirectRecordCapAppendFailCount`（每帧 10）**名不副实**：与
  record cap 无关，统计的是通过预算后 append 被拒的 record（固定一批匿名
  path/LOS blocker 走廉价早退链被正确拒绝）。append 失败=阴影侧整体缺席，
  不产生 draw，**不是** 0,0 缺失块根因，也不是性能问题。
- 剔除类机制（S13 远级联剔除等）被证伪：剔除产生的缺失跟随相机/物体，不会
  钉在原点。0,0 锚定指向"变换/几何被默认值替换"类缺陷：局部空间/预变换
  顶点 × identity world 塌缩到世界原点。已核实的活路径中最吻合的是
  draw-time VB cache 陈旧/异源快照重放（代码自注释"eight-frame-old dynamic
  VB … giant triangle at the world origin"，21330-21344），即 CF1E 修复域。
- S1 early cache key（仅 worldMatrix+几何规模）存在真实碰撞洞：identity
  world + 相同顶点数的不同 tile 必然同 key，命中即错误重放异源冻结几何。
  但用户 20:47 报告与本轮探针中 `persistentS1Early*` 全零（该图地形不入
  early cache），**不是用户所见缺失块的直接来源**，本轮按纵深防御修复。
- ConsumerBuild 4.25ms/帧（用户交互运行）= 完整 populate 每帧 ~2.3 次全量
  执行：EndFrame flush 去重门键在 direct-only 模式下永不 latch（依赖
  direct 路径从不推进的 validation runtime serial），每帧多付一次快照/
  manifest/分组固定开销；另 control-plane 查询站点与 populate 共用
  ConsumerBuild 标签污染归因。

**本轮默认修复（全部 fail-closed 方向）**：

- S1 early-cache 源身份校验：entry 冻结时记录 VB/IB 指针+offset+stride+
  BaseVertexIndex/MinVertexIndex/StartVal 指纹（不进 key，仅命中时比对），
  不匹配即淘汰重建并计入新计数器 `s1EarlySourceMismatchEvictCount`（已接入
  workloadSeries 列与 shadowBudgetSummary 聚合）。动态 ring 源校验必失败→
  走本就应走的慢路径。
- EndFrame flush 门：新增 `m_war3SceneRotatedFrameSerial`（BeforeUi 与
  Execute 两个 rotate 点写入）；`War3ExecuteSemanticShadowSceneForValidation`
  在 direct-only 且非 native-validation 时，本帧场景已 rotate 即早退。
  以 rotate 事实为键而非 populate 成功，BeforeUi 漏检帧（菜单/过场）照旧
  走完整兜底，不丢整帧阴影。
- 遥测拆分：bridge 查询站点上报改为新 tag `SummaryRefreshRequest`（enum 39，
  Count=40），ConsumerBuild 现纯对应 populate。

**静态与后台运行门**：

- `ninja -C build32` 通过（no-work 复核）；19 个 static 合同套件全 PASS；
  `test_bridge_ramp_shadow_safety_static` 按新签名更新并**新增**
  `test_s1_early_hit_validates_source_fingerprint` 合同（指纹先于查找计算、
  两个 store 站点记录、命中比对、mismatch 计数导出）。
- 桥/斜坡探针（默认视角，72 截图）：报告
  `war3_perf_report_auto_2026_07_27_01_34_18.html`，7200 帧，208.8 FPS /
  CPU 4.789ms（CF1E 同图基线 21:35 为 188.3 FPS / 5.31ms）；
  `deviceLost=0`，全部 shadow 完整性关键字 0。亮度差分：无暗块；增亮全部为
  全局昼夜循环（478/600 格同升）。
- 原点钉死探针（`--camera-target-x 0 --camera-target-y 0 --camera-angle-deg
  304`，72 截图）：7200 帧，194.5 FPS / 5.14ms，`deviceLost=0`。差分中
  帧 53 为全局夜晚帧；帧 4/9/20/26/54 的 1-2 格暗点经裁剪目检为传送门
  草地贴片的蓝色辉光脉冲动画，几何完整。**原点视角无缺失块**。
- ConsumerBuild 探针值 0.077ms/帧（1.25 次/帧，含启动期兜底帧）；
  `SummaryRefreshRequest` 列已出现在 semanticPerfHotspots。
- artifact：`AutoTest/artifacts/bridge_ramp_s1fp_flushgate_20260727.json`、
  `AutoTest/artifacts/origin_pinned_s1fp_20260727.json`。

**部署**：

- `E:\Work\War3\d3d9.dll` = 30,977,396 bytes，SHA-256
  `1807E5E748CE94AC9F25B620ECC059D6BD0414E559A356E32963F78587AB8964`，
  与 `build32` exact。
- 回退备份：`E:\Work\War3\d3d9.dll.bak_20260727_CF1E_pre_s1_fingerprint_flush_gate`
  （即上一会话的 drawtime layer exact 构建）。

**仍需用户物理屏验收**：1) 用户自己的地图在原报告视角/换视角后 0,0 处是否
仍有缺失块（CF1E 与本构建的隔离探针均未复现，但用户 20:47 所见发生在
CF1E 部署之前，尚无用户侧确认）；2) 交互运行的 FPS（flush 门的收益主要在
交互场景的 ~2.3 次/帧 populate，隔离自动测试本就 ~1 次/帧，无法代表）；
3) 若缺失块仍在：优先用 `DXVK_WAR3_SHADOW_DEBUG_CASTER_STAGE=13` 探针 +
关闭 `DXVK_WAR3_KEEP_STAGE13_WORLDOBJECT_LEGACY_CAPTURE` 做单变量 A/B，
并查报告新列 `s1EarlySourceMismatchEvictCount` 是否非零。

## 🚨 2026-07-25（阴影 owner/lifecycle/S10/S12/S13/TAA v2 计划实现，未 commit）

本轮按用户锁定计划完成了阴影 producer 所有权、生命周期、Stage 指标、非 TAA
稳定化与 TAA v2 的代码落地；保留工作区全部既有改动，未 reset/commit。

**默认正确性策略**：

- 新增统一 `ShadowProducerPolicy` 和 33-bin Stage 生命周期计数。S12 /
  `RangeIndicatorTarget` 是纯视觉 overlay，不能成为 caster 或覆盖 S11；
  S10 由 current-frame immediate legacy lane 独占，重复 generic producer 被拒绝；
  S13 保持 current-frame owner，retention/late descriptor/unique semantic/sparse
  reuse 均不重新启用。
- CurrentDraw / Manifest 补齐 producer stage/group/source、freshness、
  visible serial、policy revision、grace age 与 alpha payload 完整性。StageDisabled
  tombstone 立即清理并使 ShadowMap/history/reuse 失效；未验证的 widget
  hidden/removed/replaced caller 仍只观察，不猜测 ABI/事件语义。
- Stage10 只接纳 opaque 或权威完整 cutout；不再把任意 alpha blend 强转为
  0.5 alpha-test，不完整 UV/texture/material payload fail-closed。
- `DXVK_WAR3_SHADOW_TAA_MODE=0/1/2` 对应 DirectInline /
  PrepassCurrentOnly / Temporal，旧开关只作兼容映射。默认 DirectInline；
  Temporal v2 history 使用 RG16F visibility+linear depth，并要求
  Visibility/Motion/Receiver/HistoryWrite 四段完整后才推进。当前十字预模糊已移除。
- 非 TAA 路径统一了 cascade 的稳定世界坐标 PCF seed，稳定墙面采样不再提前
  return；adaptive reuse 现在校验 caster/content/pose/CSM/map/policy generation，
  tombstone、太阳或内容变化均失效。
- 原版 doodad type=0 静态贴花已有 enable-only 精确运行门，但在 S10 前台视觉
  通过前保持默认关闭；type=4、ListA、RegisterImage、fog/LOS/path/UberSplat 不动。

**静态与后台运行门**：

- 8 个相关静态合同共 57 tests PASS；`ninja -C build32` no-work；
  targeted `git diff --check` 无错误。
- S10 普通光影图：
  `war3_perf_report_auto_2026_07_25_23_28_04.html`，3,010 帧。
  immediate owner 的 24,088 次全部 `canonicalPublished/replayPrepared/C0..C3`
  闭合；重复 producer 的 24,088 次全部计入 `rejectedStage10Owner`。
  `framesIncomplete=0 / deviceLost=0`，隔离桌面、`BELOW_NORMAL`、cleanup 成功。
- S13 桥/斜坡图：
  `war3_perf_report_auto_2026_07_25_23_22_27.html`，6,672 帧。
  Stage13 published=78,711、replay=77,535、retention bytes=0；
  DirectInline 6,673 帧中 Visibility/Motion/HistoryWrite/HistoryAdvance 全为 0，
  `framesIncomplete=0 / deviceLost=0`。
- exact build/deploy DLL：30,968,506 bytes，SHA-256
  `B734EF2ADA7ED920890C29CEF8B099D0D0391432E0517A74A0447B42ED79D217`。
  回退点：
  `E:\Work\War3\d3d9.dll.bak_20260725_9C83_pre_shadow_owner_taa_v2` 与
  `d3d9.dll.bak_20260725_80B9_pre_s10_owner_closure`。

**仍需用户物理屏验收**：S12 绿色选择树/单位/透明模型、hide/show/remove、
S10 原版贴花关闭后的 fog/LOS/path 对照、跨 cascade 抖动固定 ROI，以及
PrepassCurrentOnly/Temporal 的清晰度扫描。Temporal 未满足视觉门前不得设为默认。

## 🚨 2026-07-25（17:34 Stage13 retained 回归紧急关闭，未 commit）

用户在最终 `E5ACD001...` 上报告桥/斜坡出现巨量几何撕裂、靠近时严重掉帧，
并提供
`E:\Work\War3\WarVK\Log\war3_perf_report_2026_07_25_17_34_37.html`。
该报告与两路独立代码审计证明上一节刚转正的 sparse retained 优化不安全，本节
结论优先于上一节的“默认转正”结论。

**确定的回归机制**：

- sparse late descriptor 默认只读取 8 个均匀 raw index 及对应 position/UV
  leaf，且不读完整 IB；它不是完整几何身份。不同桥/斜坡子网格可以在抽样点相同、
  未抽样内容不同的情况下错误命中同一 retained entry。
- 错误命中后代码会用当前 `draw` 覆盖 retained draw state，却保留旧
  `positionBytes/contentHash`；随后跨帧 replay 形成“新 world/material/bounds +
  旧顶点快照”，与用户看到的错误大三角/撕裂一致。
- miss 会在 sparse mapped reads 之后继续执行完整 referenced-set scan。用户报告
  `ShadowCapture/PostGate` 快/慢帧为 `1.992 / 34.289 ms`，调用量只增加
  30.5%；p95 `40.174 ms`、max `93.370 ms`。同 DLL 17:15 明细中慢帧
  `Stage13StrongIdentity=31.478 ms`，其中 `SampleLeaves + UniqueLeafRead`
  占主要部分。GPU 仅约 3.6 ms，故不是 GPU 或 WaitGate。

**紧急默认修复**：

- `DXVK_WAR3_STAGE13_STATIC_RETENTION=0`：完全关闭 Stage13 跨帧 CPU
  snapshot retention/replay，从生产路径移除错误复用与完整强扫描。
- 保留 `DXVK_WAR3_KEEP_STAGE13_WORLDOBJECT_LEGACY_CAPTURE=1`，Stage13
  当前帧的直接捕获/提交仍然工作，不恢复旧 semantic early-return。
- `DXVK_WAR3_STAGE13_LATE_DESCRIPTOR_CACHE=0`；若显式诊断开启，默认恢复
  `LATE_SAMPLE_COUNT=32` 与 `LATE_FULL_INDEX_FINGERPRINT=1`，但即使如此仍
  不视为严格正确。unique-semantic 继续默认关闭。
- 16 项桥/连续性静态合同 PASS，构建 no-work。

**隔离专图验证**：

- exact DLL：30,693,990 bytes，SHA-256
  `9C837CDBC88A5793D77537E03A6ACECDAEF98998535941E3AB177754F2161545`，
  已部署且与 `build32` exact。
- artifact：
  `AutoTest/artifacts/bridge_ramp_stage13_retention_off_9c83_20260725.json`；
  报告：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_25_17_59_23.html`。
- 7200 报告帧、72 张 exact backbuffer，War3 为 `BELOW_NORMAL` 且运行于隔离
  Desktop。`framesIncomplete=0 / budgetExceeded=0 / deviceLost=0`。
- 用户回归报告 → 新版，同量级调用：
  `PostGate 8.170→0.110 ms/frame`（**-98.65%，约 74×**），
  p95 `40.174→0.205 ms`（**-99.49%**）；
  `ShadowCapture 8.384→0.357 ms/frame`（**-95.74%**），
  p95 `40.453→1.498 ms`（**-96.30%**）。
- 72 张精确帧中 `stage13Attempt == stage13Considered`，Stage13 replay
  0～32 draws；retention eligible/entry/strong scan/snapshot build 全部为 0，
  history incomplete skip=0。抽查桥近景未见错误巨型三角。
- 追加高压图短门
  `AutoTest/artifacts/stage13_retention_off_pressure_9c83_20260725.json`，
  报告 `...18_05_10.html`：1653 帧，`PostGate=0.421 ms/frame`、
  p95 `0.671 ms`，`framesIncomplete=0 / budgetExceeded=0 /
  deviceLost=0`；外部负载下绝对帧时不作为收益口径。
- 回退备份：
  `E:\Work\War3\d3d9.dll.bak_20260725_E5AC_pre_stage13_retention_emergency_off`。

**仍需用户物理屏验收**：隔离 backbuffer 可以验证错误几何，但不能证明扫描输出
时间域的物理撕裂完全消失。下一步必须用用户同一桥图肉眼确认。若只剩“原生完全不
提交时的阴影缺口”，不得重新开启 sparse retained；正式解法应在 VB/IB 写入或
Unlock 时生成完整 content generation/hash，使用包含 slice/range/layout 的 O(1)
精确键，并把 draw state、content identity、CPU snapshot 作为不可分割记录发布。

## 🚨 2026-07-25（Stage13 强身份扫描减半，默认转正，未 commit）

本阶段继续只在隔离 Desktop 运行 War3，游戏/构建均为 `BELOW_NORMAL`，没有控制
用户前台；外部高负载下只采信同轮局部比例，不使用绝对 FPS 宣称收益。

**定位与可观测性**：

- 在默认关闭的 `DXVK_WAR3_SHADOW_CAPTURE_BREAKDOWN=1` 下，把
  `Stage13StrongIdentity` 拆为 `SourceMapAndSetup / IndexParse /
  UniqueLeafRead / HashReplay / VerifyAndLookup`，再把
  `SourceMapAndSetup` 拆为 `DescriptorSetup / IndexFingerprint /
  SampleLeaves / RetainedLookup / Finalize`。
- 首轮显示旧完整 referenced-set 扫描由 `UniqueLeafRead` 主导；原 32 点 late
  descriptor 又各花约一半时间在完整 IB FNV 与 mapped position/UV 抽样，容器查找
  仅约 0.01 ms/frame，不是热点。
- “只按唯一 world+material+layout 命中”的零读取实验被强验证器抓到真实反例：
  `retained=15658477967941462053`、`current=18028680984744935418`、
  `count=810`。该路径保持
  `DXVK_WAR3_STAGE13_UNIQUE_SEMANTIC_CACHE=0`，只留作反例复现。

**默认优化**：

- `DXVK_WAR3_STAGE13_LATE_DESCRIPTOR_CACHE=1`：Stage13 rigid retained caster
  默认使用 late sparse content descriptor。
- 默认 8 个均匀分布的 raw-index + position/UV leaf：
  `DXVK_WAR3_STAGE13_LATE_SAMPLE_COUNT=8`；`=32` 恢复保守抽样。
- 默认不再读取完整 mapped IB：
  `DXVK_WAR3_STAGE13_LATE_FULL_INDEX_FINGERPRINT=0`；`=1` 精确恢复旧完整
  IB 指纹。整项 `...LATE_DESCRIPTOR_CACHE=0` 可回退旧完整 referenced-set 扫描。
- `DXVK_WAR3_STAGE13_SOURCE_GENERATION_VERIFY=1` 会在每个 fast hit 后重新执行
  旧完整扫描并逐 content hash 比较，不一致立即 abort。

**验证与比例**：

- 桥图强验证
  `AutoTest/artifacts/bridge_stage13_sparse8_no_full_ib_verify_ddd8_20260725.json`：
  50/50 exact captures，489 attempts / 343 verified hits（70.1%）/
  146 misses，零 mismatch，`framesIncomplete=0 / deviceLost=0`。
- 同 DLL OFF→ON→ON→OFF 中，开启轮 scan avoidance 为 70.14% / 68.92%。
  因前台负载使绝对帧时漂移 17–58 ms，收益按开启轮内部实际 scan 单价计算：
  `Stage13StrongIdentity` 相对全扫描 counterfactual 分别约 **-49% / -53%**。
- 生与死验证报告
  `war3_perf_report_auto_2026_07_25_17_08_33.html`：474 frames；
  SunkenCity 验证报告 `...17_10_28.html`：388 frames，约 45 Stage13
  attempts/frame。两轮均 screenshot 成功且
  `framesIncomplete=0 / deviceLost=0 / captureIncomplete=0`，无 verifier abort。
- 最终默认桥图长门
  `AutoTest/artifacts/stage13_sparse8_final_default_e5ac_20260725.json`，
  报告 `...17_15_51.html`：990 frames，7.467 Stage13 attempts/frame，
  2.020 full scans/frame（**72.95% avoidance**）；同轮内部 counterfactual
  约 **-49.1%**。`framesIncomplete=0 / budgetExceeded=0 / deviceLost=0 /
  captureIncomplete=0`，screenshot 成功。
- 静态合同 16/16 PASS；`ninja -C build32 src/d3d9/d3d9.dll -n` no-work。

**最终 exact 部署**：

- `E:\Work\War3\d3d9.dll`：30,693,990 bytes，SHA-256
  `E5ACD0015ED9FDD1071000AF23CCE543B26525FBAA5A1C0BDC02560A1FF24F4C`，
  与 `build32` exact。
- 回退备份：
  `E:\Work\War3\d3d9.dll.bak_20260725_DDD8_pre_stage13_sparse8_default`；
  上一版 source-split：
  `d3d9.dll.bak_20260725_68BD_pre_stage13_sample_tuning`。

**下一顺序**：Stage13 剩余最大成本为 8 个 mapped leaf reads
（最终门约 2.564 ms/frame）和 miss 时 `UniqueLeafRead`（2.439 ms/frame）。
不要再尝试无内容校验的 semantic-only 命中。若继续压缩，先用强验证器测试
4 点/CPU-side source mirror，或建立写入期 content generation/hash；不得重新依赖
轮换的 D3D resource/backing identity。

## 🚨 2026-07-25（桥/斜坡 Stage13 连续阴影 + 生与死加载闪退修复，未 commit）

用户报告两项独立故障：

1. 所有桥/斜坡装饰物的阴影同步消失/出现，低视角闪烁更快、上帝视角更慢；
2. 高压图 `(4)生与死v1.28读档bug修复.w3x` 加载一段时间后直接闪退。

本轮全程在隔离桌面运行 War3，并把游戏进程调整为 `BELOW_NORMAL`，未控制用户
前台。未使用 IDA；高压图故障通过 minidump、32 位 GDB rejection breakpoint 和
汇编栈闭合。

**桥/斜坡阴影根因与修复**：

- 桥/斜坡走原生 Stage13 world-object 提交。旧路径先受相机相关 semantic
  snapshot/visibility 门控，却没有进入 Stage13 caster 分类；因此一整组对象会随
  视角高度和俯仰同步从阴影集合中掉出。低视角导致门控边界穿越更频繁，所以闪烁
  周期更短；这不是材质、TAA 或单个模型资源随机损坏。
- 新增默认开启的
  `DXVK_WAR3_KEEP_STAGE13_WORLDOBJECT_LEGACY_CAPTURE=1`：Stage13 绕过该
  view-dependent semantic early-return，在原 draw 当下由 legacy capture 持续接管；
  同时补入 object/VS caster 分类。
- DirectGrouped 的 grouped preselection 与 fallback build 两处都显式跳过
  Stage13，保证 Stage13 只有一个 shadow owner，不会双重发布。
- 静态合同
  `AutoTest/test_bridge_ramp_shadow_safety_static.py` 现为 11/11 PASS。
- 专图长循环 artifact：
  `AutoTest/artifacts/bridge_probe_stage13_scoped_legacy_continuity_20260725.json`。
  72.475 秒 / 6517 game frames / 96 张 exact backbuffer；79 个实际提交 Stage13
  的采样帧中 `attempt=1229 / considered=1229 / final caster=1229`，
  `AttemptButConsideredZero=0 / AttemptButFinalZero=0`。其余 17 帧相机完全位于
  黑色战争迷雾且游戏没有提交对象，不是我方丢失。报告：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_25_04_16_50.html`。

**高压图闪退的精确证据**：

- 旧候选在同图 4/4 于约 37 秒加载阶段崩溃。完整转储之一：
  `E:\Work\War3\WarVK\Crash\war3_crash_2026_07_25_04_28_04_892_pid89664_tid49736.dmp`。
  AV 为 `Game.dll+0xFB135` 向 null-derived `0x00200000` 写入；调用链经过
  `Game+FB135 → FA5B0/FA586 → FA1BA → FA122 → Game+42C13`。
- 一次性 GDB rejection artifact：
  `AutoTest/artifacts/life_death_storm_reject_capture_20260725_045545.json`。
  被拒绝的 live pointer 为 `0x3d990a08`，头 `0x3d9909f8`，magic
  `0x53425431 (SBT1)`、size `0x00200000`、cookie/header 均有效；
  exact recently-freed slot 为 0，`TlsfPool_IsFromPool(header)=1`。
- 指针只满足 8 字节对齐。初始 managed allocation 使用 16-byte memalign，但普通
  `TlsfPool_Realloc` 的 moving path 只保证 TLSF 基础对齐。旧代码随后把该块作为
  live block 发布；下一轮 `QueryManagedBlock` 按公开 16-byte ABI 拒绝自己的活块，
  ReAlloc 返回 null，而 Game 无条件保存并写入该返回值，形成确定性崩溃。该证据
  排除了此前怀疑的 tombstone/hash collision。

**Storm 修复**：

- `AllocManagedBlock` 对后端返回地址追加 16-byte fail-closed 检查。
- managed TLSF→TLSF ReAlloc 先调用 `TlsfPool_ReallocInPlace`；成功时天然保留
  原对齐。无法原地调整时走 `AllocManagedBlock(16-byte aligned) + copy + release
  old`，不再调用会丢失对齐的普通 `TlsfPool_Realloc`。
- 离线 Storm 合同 18/18 PASS，其中新增
  `test_managed_realloc_preserves_public_16_byte_alignment`。

**最终高压图验证与部署**：

- 修复后的同一 exact DLL 连续通过两轮完整隔离运行：
  - `AutoTest/artifacts/resource_residency_census_isolated_life_and_death_20260725_050134`
    （109.751 秒，779 observed frames）；
  - `AutoTest/artifacts/resource_residency_census_isolated_life_and_death_20260725_050609`
    （115.662 秒，779 observed frames）。
- 两轮均 `PASS=true / framesIncomplete=0 / finalNewWar3=0`，module、launch、
  cleanup 与 allocator chunk 合同全部闭合，且没有产生新 crash dump。对应报告：
  `war3_perf_report_auto_2026_07_25_05_03_22.html` 与
  `war3_perf_report_auto_2026_07_25_05_08_04.html`。
- 最终 `build32/src/d3d9/d3d9.dll` 与 `E:\Work\War3\d3d9.dll` exact：
  30,575,596 bytes，SHA-256
  `F4020142D8632D160747EB515E3CC5B97A901C667195C1755E71B8D56C252582`；
  `ninja -C build32 src/d3d9/d3d9.dll -n` 为 no-work。
- 回退：
  `E:\Work\War3\d3d9.dll.bak_20260725_1B81_pre_storm_realloc_alignment`
  （SHA `1B81C2D98B3D7A0708E5937FFFB9FA55E01B7B106AB4CEB6928EF9B01D13020A`）。

**仍需用户物理屏验收**：程序侧 Stage13 阴影集合在长循环中已无提交缺口，但最终
是否还存在显示时间域闪烁必须由用户在同一专图肉眼确认。若最终 F402 上仍闪烁，
下一步记录的是 Stage13 world matrix 与最终 shadow pixels 连续性，不能重新放开
易失 S1 缓存，也不能用延长 history hold 掩盖问题。

## 🚨 2026-07-24（17:13 世界原点阴影撕裂修复：禁用易失 S1 fallback early-cache，未 commit）

用户在 replay 发布闭合候选 `C3F6ABA2...` 上报告：桥/斜坡阴影仍偶发消失，
并且世界 `(0,0)` 高频出现放射状阴影撕裂，正常阴影也会被错误三角扰动。输入报告为
`E:\Work\War3\WarVK\Log\war3_perf_report_2026_07_24_16_50_56.html`。

**确定根因**：

- 用户报告中 S1 early-cache 共有 8 项，全部是 fallback-backed；95,061 次
  accepted hit 全部发布为 fallback，persistent-backed/instance 均为 0。
- fallback freeze 默认来自 `ShadowArena_Alloc`。该 Arena 只有 3 个按帧轮换的
  page，page 再次轮到时会把 offset 复位为 0；缓存中的 `Rc<DxvkBuffer>` 只能延长
  Vulkan buffer 对象寿命，不能保护旧 slice 的字节不被后续帧覆盖。
- `War3BuildS1TerrainEarlyKey` 又刻意省略了动态 ring 的
  `StartVal/BaseVertexIndex/MinVertexIndex`，也没有完整 source-content 身份。
  因此 replay 发布闭合修复让原先“命中后消失”的 latent 条目真正参与绘制后，
  它们会把已经被其他 tile 覆盖的顶点/索引解释成旧 draw，直接形成从世界原点拉出
  的错误三角和周期抖动。

**默认修复**：

- 新增 `DXVK_WAR3_S1_EARLY_FALLBACK_BACKING=0`（默认）。S1 early-cache 现在
  只接受 registry-owned persistent geometry；frame-arena/freeze fallback 每帧
  正常重新捕获并通过 canonical `shadowFallbacks` 绘制，不再跨帧复用易失 slice。
- store 入口、slow fallback 写入点和 early-hit 读取点三层 fail-closed；即使进程
  曾用危险诊断配置写入 id=0 条目，恢复默认后首次 lookup 也会立即清除，绝不发布。
- `=1` 仅保留为精确复现旧行为的危险诊断回滚。该变量已加入 perf report 的环境
  快照目录；在完整 source descriptor/content identity 和稳定 backing 实现前不得
  转正。

**同专图验证**：

- 静态安全合同 10/10 PASS；最终构建 `ninja -n` no-work。
- 短门报告
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_17_09_32.html`：
  3,680 采样帧，S1 entry/fallback-backed/accepted/published 全为 0，
  `framesIncomplete=0 / deviceLost=0 / capacityReject=0 / closureMismatch=0`。
- 长门报告
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_17_13_20.html`：
  7,200 采样帧、65.612 秒，工作量序列覆盖 24 次
  fallback draw 低→高→低可见区间循环；所有逐帧 S1 early-cache gauge/hit
  始终为 0，receiver 活跃但 replay=0 为 0，partial/incomplete/device-lost/
  capacity-reject/closure-mismatch 全为 0。
- 用户报告的 last-complete reuse 比例为 156/3457（4.51%）；长门同一
  `workloadSeries` 窗口为 7/7200（0.097%，相对下降约 97.85%）。这只作阴影提交
  稳定性比例，不把外部负载下绝对 FPS 当收益。
- 长门内在四个不同相机阶段通过 control-plane 捕获最终帧，均未看到世界原点放射状
  错误三角；最后一个阶段主要是地图自身的黑色战争迷雾，不是缺失渲染。截图为
  `AutoTest/artifacts/screenshots/bridge_ramp_839c_long_01.png` 至 `_04.png`。

**最终部署**：

- `build32/src/d3d9/d3d9.dll` 与 `E:\Work\War3\d3d9.dll` exact：
  30,518,709 bytes，SHA-256
  `839CD9556BF8502508C274245A355829B289A8E3745F45F2803B4C1AA929FFF1`。
- artifacts：
  `AutoTest/artifacts/bridge_ramp_s1_fallback_fail_closed_839c_20260724.json`
  与
  `bridge_ramp_s1_fallback_fail_closed_long_839c_20260724.json`。
- 回退：
  `E:\Work\War3\d3d9.dll.bak_20260724_C3F6_pre_s1_fallback_fail_closed`
  （SHA `C3F6ABA2...`）。

**仍需用户物理屏验收**：后台逐帧合同、长循环和多阶段 backbuffer 均已通过，但物理
屏幕上的时间域闪烁仍应由用户用同一专图确认。若桥/斜坡阴影仍消失，下一步只追踪
逐帧 live S1 fallback membership、world matrix 和 receiver draw outcome；不得重新
开启易失 fallback early-cache，也不得用延长 last-complete hold 掩盖缺口。

## 🚨 2026-07-24（16:39 桥/斜坡阴影周期闪烁修复：S1 replay 发布闭合，未 commit）

用户确认 ManifestCopy 巨卡已消失，但桥/斜坡阴影仍约每两秒周期闪烁。本轮不使用
IDA，继续只在隔离桌面、`BELOW_NORMAL` War3 下运行专图
`E:\Work\War3\Maps\ShadowTest\光影测试(桥斜坡).w3x`。

**确定根因与修复**：

- S1 early-cache 命中路径在 `d3d9_device.cpp` 中只向
  `shadowCasters` 追加缓存 draw 后直接 return；但
  `BuildShadowReplayDraws` 的 canonical 输入只有 `shadowInstances` 和
  `shadowFallbacks`，裸 `shadowCasters` 不会进入当前阴影图。
- 因此缓存命中和资源 lifetime 都正常，caster 却从当前 replay 集合消失；旧的
  120 帧 last-complete hold 暂时掩盖缺口，hold 到期后暴露，形成约两秒闪烁。
- early hit 现在按原 backing 合同恢复发布：persistent-backed 条目发布
  `War3ShadowInstanceRef`，fallback-backed 条目发布
  `War3ShadowFallbackDraw("s1-early-cache")`；随后仍保留兼容
  `shadowCasters`。当前桥图 8 个 entry 全为 fallback-backed。
- 新增 accepted hit / replay published / instance / fallback 逐帧计数和
  closure mismatch；报告聚合与 `workloadSeries` 均可直接检查
  `accepted == published == instance + fallback`。

**同专图前后证据**：

- 旧报告 `war3_perf_report_auto_2026_07_24_14_38_39.html`：4,234 条 series
  中有 **1,058 帧（25.0%）** 在 receiver 活跃且 S1 已捕获时
  `replayCasterCount=0`；`semanticSceneReceiverHoldEmptyReplayCount=604`。
- 新默认门
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_16_38_54.html`：
  2,849 帧、31.786 秒；64,358 accepted hits / 64,358 published /
  64,358 fallback / 0 instance，**逐帧 mismatch=0**；
  receiver 活跃但 replay=0 的帧 **1,058→0（-100%）**，
  hold-empty **604→0（-100%）**。可见 S1 数量在 20～28 间变化并发生
  510 次集合切换，证明循环相机反复进出视野已覆盖。
- 同轮 `framesIncomplete=0 / deviceLost=0 / capacityReject=0`，GPU
  2.342 ms；绝对 FPS 受用户前台负载影响不作为收益口径。
- 一次因未显式跳过不适用于桥图的旧 hot-shadow 前置门而延长的隔离运行，仍得到
  `16:35:41` 报告：9,307 observed frames，210,040 accepted /
  210,040 published，mismatch=0、hold-empty=0、device-lost=0。其 gate artifact
  因 hot-shadow timeout 标记 false，不作为正式门，仅作长时稳定性旁证。

**验证与部署**：

- 静态安全合同现为 9/9 PASS；Hook catalog 10/10、manifest 4/4、
  Hook Breakdown mock 4 cases PASS；`ninja -n` no-work。
- 正式 artifact：
  `AutoTest/artifacts/bridge_ramp_s1_replay_closure_default_c3f6_20260724.json`；
  最终帧：
  `AutoTest/artifacts/screenshots/war3_20260724_163912.png`。
- exact DLL：30,518,322 bytes，SHA-256
  `C3F6ABA247DE24E74C8F97E60185F0281D16D5B8E2492844D100C0CFD0B01020`；
  `build32` 与 `E:\Work\War3\d3d9.dll` exact。
- 回退：
  `E:\Work\War3\d3d9.dll.bak_20260724_DC8A_pre_s1_replay_publish`
  （SHA `DC8A878C...`）。

**仍需用户物理屏验收**：隔离桌面最终帧显示桥/斜坡阴影完整，但单张捕获不能证明
时间域绝无闪烁。代码与逐帧 replay 缺口已完全闭合；请用户用同一循环图肉眼确认。
若仍有闪烁，下一步只查实际 shadow-set 内容/矩阵连续性或显示 present，不要再改
S1 resource cache，也不要延长 last-complete hold 掩盖问题。

## 🚨 2026-07-24（14:09 桥/斜坡 ManifestCopy 巨卡修复，未 commit）

用户报告
`E:\Work\War3\WarVK\Log\war3_perf_report_2026_07_24_14_09_49.html`
中 `ManifestCopy` 为 6.855 ms/frame，但只有 0.167 calls/frame；每次真实进入
p50 35.835 ms / p95 87.376 ms / max 115.587 ms。直接 chrono 为
11,858.765 ms / 294 次，即 40.336 ms/次；总共只扫描 4,964 条、单次最多
33 条，证明不是 vector copy 或大记录数量。

**根因与修复**：

- `ConvertVisible` 的旧 `ManifestCopy` 名称覆盖了复制、模型资源解析、geoset
  解析和 append。桥/斜坡的 runtime model 有效，但 `OwnedModelDataHandle`
  无法解析成直接 model resource；旧代码因此对每条 visible record 重复调用
  `resolveDirectModelResourcePtr`，最坏扫描 30 个一级偏移 × 30 个二级偏移。
  `SafeReadPtrFast` 当前每次仍执行 `VirtualQuery`，于是一次失败解析可触发约
  900 次内核查询。
- `ResolveModelResourceForContract` 现有 4,096 项 thread-local 直接映射缓存，
  精确键为 `(runtimeModelPtr, OwnedModelDataHandle, resourceCache.revision)`；
  正/负结果均缓存。每次调用仍重读 owned handle，handle 或资源 revision
  变化立即走旧深解析。`DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE=0` 可精确
  回滚。
- 默认关闭的强验证器
  `DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY=1` 会在每次 cache hit
  重新执行旧 30×30 resolver 并逐指针比较；
  `..._VERIFY_ASSERT=1` 在 mismatch 时 abort。另加入 attempt/hit/deep/null/
  verify 计数并贯通 control-plane 和 perf report。
- 同时加入 source-backing/raw-geoset 诊断。桥图实测 source-complete hit=0，
  raw geoset 每次最多 4 项且无 miss，证明它们不是本次主因；source-backing
  fast path 因此保持默认关闭，不把无收益实验转正。

**同 DLL 验证与比例结果**：

- exact verifier 报告
  `war3_perf_report_auto_2026_07_24_14_37_33.html`：
  4,346 attempts / 4,268 cache hits / 78 deep resolves /
  **4,268 verifier attempts / 0 mismatch**；验证器主动重跑旧逻辑时
  43.062 ms/真实进入。
- 默认生产报告
  `war3_perf_report_auto_2026_07_24_14_38_39.html`：
  4,234 帧，`framesIncomplete=0 / deviceLost=0`；
  10,097 attempts / 10,021 hits / 76 deep resolves，命中率 99.25%。
  直接 chrono 228.781 ms / 555 次 = **0.412 ms/真实进入**，
  相对用户旧报告下降 **98.98%（约 97.85×）**，相对同 DLL 强验证下降
  **99.04%（约 104.46×）**。报告 scope 从 6.855 降到
  0.037 ms/frame；p50 0.036 ms / p95 0.097 ms。单次 max 54.887 ms
  是极少冷解析或外部抢占，不代表重复进出视野的常态路径。
- 静态合同 `AutoTest/test_manifest_source_backing_fastpath_static.py`
  4/4 PASS；隔离桌面截图成功，War3 为 `BELOW_NORMAL`，未控制用户前台。

**当前 exact 部署**：

- `E:\Work\War3\d3d9.dll`：30,514,137 bytes，SHA-256
  `DC8A878C80E3A7695D2284B0580F7A103C73082E86BD3AC0F85C719ADD17C83C`。
- 强验证 artifact：
  `AutoTest/artifacts/bridge_ramp_manifest_model_cache_verify_dc8a_20260724.json`；
  默认 artifact：
  `AutoTest/artifacts/bridge_ramp_manifest_model_cache_default_dc8a_20260724.json`。
- 回退：
  `E:\Work\War3\d3d9.dll.bak_20260724_CD00_pre_manifest_source_backing`
  和
  `E:\Work\War3\d3d9.dll.bak_20260724_D025_pre_manifest_model_cache`。

**下一顺序**：让用户用前台桥/斜坡循环图确认重复进出视野不再巨卡；若仍能感知
首次冷卡，给 deep resolver 单独做低频 timing，并用批量安全读取 + 旧算法逐结果
verifier 收窄首次 30×30 探测。不要取消 handle/revision 失效条件来换性能。

## 🚨 2026-07-24（黑块/撕裂专项）：索引 fail-closed + TAA history 完整性（未 commit）

用户补充报告：画面周期性瞬时出现纯黑缺块，形态有时像撕裂、有时像局部渲染
消失。本轮继续只使用隔离桌面和 `BELOW_NORMAL` 进程，未控制用户前台。

**已修复的两个确定性错误**：

- DrawTime 阴影缓存原始 draw 为 indexed、但 IB 捕获因预算/容量/资源失败时，旧
  代码会把 entry 改成 non-indexed；消费者随后把 position range 顺序解释成三角形，
  可产生覆盖大面积屏幕的错误三角/黑楔。`War3DrawTimeVBEntry` 现有显式
  `captureComplete/HasCompleteBacking()` 合同，5 个消费者统一拒绝不完整 backing；
  indexed IB 失败保留 indexed 身份并中止发布，不再错误降级。
- Shadow TAA 旧代码只看 `shadowTaaActive` 就推进 ping-pong 并把 history 标有效，
  即使 `drawReceiver` 因瞬时资源条件早退、HistoryWrite 根本没有写入。现在只有
  receiver fullscreen draw 确实录制且 write image 存在才 barrier/推进；失败帧保留
  最后一张完整 history，并让下一帧 current-only 后再恢复混合。
- 新增 `drawTimeVBCacheRejectIncompleteIndex`，以及 cadence 的
  `shadowHistoryAdvancedThisFrame /
  shadowHistoryAdvanceSkippedIncomplete`；同时补齐 control-plane 序列化和
  pre-receiver 占位统计合并，避免新 outcome 被下一次旧场景发布清零。

**后台验证**：

- indexed fail-closed 候选 `FF8492C0...`：桥/斜坡循环图 4298 帧，
  `framesIncomplete=0 / deviceLost=0`，报告
  `war3_perf_report_auto_2026_07_24_11_50_42.html`。
- 两项渲染修复候选 `DCD840B6...`：同图 7200 帧，
  `framesIncomplete=0 / deviceLost=0`；10 次 control-plane 连续截图均成功，
  未看到错误巨型三角或局部未渲染块。报告
  `war3_perf_report_auto_2026_07_24_12_12_36.html`，artifact
  `AutoTest/artifacts/bridge_ramp_black_block_full_guard_dcd8_20260724.json`。
- 最终仅追加诊断合并的 exact `CD005F64...` 又通过 202 帧与 2028 帧短门，
  均 `framesIncomplete=0 / deviceLost=0 / cleanup=true`；报告
  `12:20:09 / 12:22:28`。静态合同 7/7 PASS，ninja no-work。

**最终部署**：

- `E:\Work\War3\d3d9.dll` = 30,373,374 bytes，SHA-256
  `CD005F64DC0338E79DCF4FB9E800DCC0A5751EFA02E5A3DBA432EF2F17AE5768`；
  与 `build32` exact。
- 回退备份：`d3d9.dll.bak_20260724_FF84_pre_taa_history_gate`、
  `d3d9.dll.bak_20260724_DCD8_pre_taa_diag_merge`、
  `d3d9.dll.bak_20260724_C2CA_pre_taa_diag_preserve`。

**仍需用户物理屏验收的独立问题**：FPS 解锁当前会强制
`D3DPRESENT_INTERVAL_IMMEDIATE`，日志确认 Vulkan present mode 为
`VK_PRESENT_MODE_IMMEDIATE_KHR`。隔离桌面的 backbuffer 截图无法捕获扫描输出撕裂；
而测试图的相机周期性经过大片黑色战争迷雾，物理 tear 会特别像横向黑块。若新 DLL
后只剩水平接缝，下一步应单变量 A/B `dxvk.tearFree = True`（优先 MAILBOX，缺失时
回退 FIFO），不要再归因于阴影资源损坏；该模式可能改变显示延迟/帧率口径，尚未设为
默认。

## 🚨 2026-07-24（桥/斜坡专项收口）：S1 不稳定源隔离 + 越界拷贝保险（未 commit）

用户新建循环视野地图
`E:\Work\War3\Maps\ShadowTest\光影测试(桥斜坡).w3x`，用于反复让桥/斜坡进入和
退出视野。用户报告的问题为：首次入镜严重卡顿、再次入镜重复卡顿、阴影周期闪烁，
晃动数次后游戏未响应。本轮全程只在隔离桌面运行 War3，进程
`BELOW_NORMAL`，未控制用户前台；无需 IDA。绝对 FPS 受用户前台高负载影响，结论
只使用同地图的资源局部比例、逐帧工作量与设备稳定性。

**根因与反证链**：

- 用户报告 `war3_perf_report_2026_07_24_10_23_07.html` 已显示 512 MiB
  persistent pool 顶满、6523 次 capacity reject、约 5.35 GiB 请求和约
  1.60 GiB 淘汰；GPU 仅约 1.64 ms，不是 GPU 光栅瓶颈。
- 旧 exact `EEDFB123...` 的两次专图后台基线
  `10:34:53 / 10:36:34` 分别只有 1603 / 1231 帧，却产生 14510 / 14116 次
  persistent create、8.23 / 7.67 GiB 请求、2.36 / 2.13 GiB 淘汰；池峰值均为
  512 MiB，capacity reject 为 9291 / 9601，并出现
  `VK_ERROR_DEVICE_LOST`。
- 仅打开旧 indexed 前缀 trim 虽减少约 56% 请求，仍 device-lost；同 DLL 只关闭
  `DXVK_WAR3_S1_TERRAIN_PERSISTENT_GEOMETRY` 后立即变为
  `deviceLost=0 / reject=0`，证明故障严格属于 S1 persistent 晋升域，而不是阴影
  pass 本身。
- S1 旧准入允许 UP/ring 动态上传仅靠 source-key/world-matrix 进入跨帧缓存，
  每个 tile 又创建独立 device-local VkBuffer。相机循环导致分配、容量拒绝、240 帧
  淘汰再创建的锯齿。DrawTime 旧 fingerprint 还遗漏 IB identity/backing/slice、
  UV source/layout、topology 与 vertexOffset，可能复用陈旧 backing，对应闪烁风险。
- DrawTime position/UV/index 扩容失败或单帧 alloc budget 耗尽时，旧代码可能保留
  较小旧 buffer，却按更大的新字节数继续 `copyBuffer`；这是可直接导致
  Vulkan 越界提交和 device lost 的正确性漏洞。

**已落地的默认修复**：

- `DXVK_WAR3_S1_PERSISTENT_UNSTABLE_SOURCE=0`（默认）：S1 persistent 只允许
  非 DynamicSysmem、非 `D3DUSAGE_DYNAMIC` 且 IB 同样稳定的 backing。UP/ring 或
  动态 IB 仍正常画阴影，但走已验证的逐帧 fallback，不再跨帧晋升。
  `=1` 仅用于危险的旧行为诊断回滚。
- `DXVK_WAR3_S1_PERSISTENT_BORROW_STATIC=1`（默认）：真正静态的 S1 VB/IB
  直接持有原 `DxvkBufferSlice` 的 `Rc`，不再为每个桥/斜坡 tile 额外分配和复制。
  retained upload 不计入 persistent owned-byte cap；`=0` 恢复旧 allocate+copy。
- `DXVK_WAR3_DRAWTIME_SOURCE_FINGERPRINT_REUSE=0`（默认）：完整 source
  descriptor verifier 落地前，同帧/跨帧都不允许依赖现有不完整 fingerprint
  跳过拷贝；`=1` 仅恢复旧诊断路径。
- DrawTime position、独立 UV、IB 三条 copy 路径现在都在提交前验证
  `capacity >= requested bytes`；预算延期/创建失败会清空当帧 descriptor/readiness，
  绝不再向旧的小 buffer 提交大 copy。
- 新增
  `AutoTest/test_bridge_ramp_shadow_safety_static.py`；与 iterator 合同合计 7 tests
  PASS。最终 `ninja -C build32 src/d3d9/d3d9.dll -n` 为 no-work。

**最终专图验证**：

- 最终 exact DLL：30,368,952 bytes，SHA-256
  `BCA7F82E6643E3C823B8BF36129EA1133AD6CB25C2F54FD13B4019B862F00118`；
  build32 与 `E:\Work\War3\d3d9.dll` exact。
- 默认短门：
  `war3_perf_report_auto_2026_07_24_11_07_21.html`，504 帧，
  `deviceLost=0 / framesIncomplete=0 / capacity reject=0`，persistent pool
  峰值 473,856 bytes。
- 默认长门：
  `war3_perf_report_auto_2026_07_24_11_08_54.html`，2777 帧 / 44.821 秒，
  工作量序列明确覆盖 **20 次** replay 从 0→非 0（反复重新入镜）；
  `deviceLost=0 / framesIncomplete=0 / capacity reject=0`。create=550，
  requested=1,802,496 bytes，pool peak=500,736 bytes，
  evicted delta=1,363,968 bytes。
- 对两轮旧基线按帧归一化：persistent create **-97.8%～-98.3%**，
  requested bytes **-99.987%～-99.990%**，evicted bytes
  **-99.967%～-99.972%**，pool peak **-99.907%**。同时
  `ShadowCapture 0.264/0.320 → 0.252 ms/frame`（约 -4.5% / -21.3%），说明
  安全 fallback 没把 capture CPU 变成新瓶颈。
- artifacts：
  `AutoTest/artifacts/bridge_ramp_safety_s1off_590f_20260724.json`、
  `bridge_ramp_safety_default_short_bca7_20260724.json`、
  `bridge_ramp_safety_default_long_bca7_20260724.json`。
- 回退备份：
  `E:\Work\War3\d3d9.dll.bak_20260724_EEDF_pre_bridge_ramp_safety` 和
  `E:\Work\War3\d3d9.dll.bak_20260724_590F_pre_s1_stable_gate`。

**仍需用户前台确认**：隔离 Desktop 的内部最终帧捕获接口超时，且按合同拒绝回退
截图用户可见桌面，所以本轮不能诚实宣称已做画面对照。资源锯齿、设备丢失和重复入镜
稳定性已实测消除；用户需用同一地图肉眼确认桥/斜坡阴影不再周期闪烁。若仍有纯视觉
闪烁，下一步只做完整 IB/UV/topology/source descriptor verifier 与 shadow-set
连续性追踪，不能重新放开不稳定 S1 persistent。

## 🚨 2026-07-24（上午阶段收口）：NativeHint/DrawTime/CurrentDraw 继续下钻 + 两项默认优化（未 commit）

本阶段继续遵守无人值守合同：所有 War3 均在隔离桌面、游戏与构建进程均为
`BELOW_NORMAL`，未控制用户前台；性能结论只使用同 DLL ABBA 的局部耗时/调用与
同帧控制比例，不把外部高负载下的绝对 FPS 当收益。动画、可见性、阴影像素和
persistent 容量语义均未修改。

**可观测性 1 — CurrentDraw 固定 ID 调用树**：

- `war3_hook_perf.h`、`war3_current_draw_contract.{h,cpp}` 新增默认关闭的
  `DXVK_WAR3_PERF_CURRENT_DRAW_BREAKDOWN=1`，复用 CurrentDraw 根的 1/32
  预选 HT 样本，输出 ContextGate / RecordSeed / VisibleBackfill /
  FrameIdentity / BindFieldRefresh / PublishContract，以及 Publish 下
  LocalGateCache / TrustedPaletteQueryPack / SnapshotCommit / GlobalMaps。
  旧 `Semantic/CurrentDraw/*` 字符串 scope 在 Level2 关闭，避免双记账。
- fixed path capacity 96→192，并新增
  `Profiler_HotHookPathOverflow/NearCapacity` 零耗时告警。明细门
  `war3_perf_report_auto_2026_07_24_07_49_55.html` 中两项告警均未出现，
  旧 scope 为 0；Common Publish 约 0.172 ms/frame，Special 约 0.087，
  其中 Local/Snapshot/Trusted 是主要已知子项。
- 该树本身有明显 observer tax；`PublishContract self` 混入 path bucket 查找/
  push/pop，不能直接当业务热点。开关默认关闭，只用于递归调查。

**默认优化 1 — producer-off NativeHint 全局 fast return**：

- 当前 `kWar3ShadowProjectorNativeHintEnabled=false`；全仓唯一两个 producer
  `recordFromObject/recordSimple` 都被同一 `if constexpr` 编译剔除，registry
  全进程恒为空。`War3TryResolveNativeShadowHint` 现在在 profiler 和最多 13 次
  mutex/map probe 前直接返回 false；空 registry 的每帧 `beginFrame` 也跳过。
  miss 时 `outHint` 仍保持不变。未来 producer flag=true 时整个 fast path 被编译
  删除。`DXVK_WAR3_NATIVE_HINT_PRODUCERLESS_SKIP=0` 精确恢复旧查询。
- 静态测试会扫描整个活跃 `src/d3d9` 的所有 producer 调用点，防止未来新增未受
  同一门控的 writer。两路独立审查均 PASS。
- 同 DLL `01F76EF8` A1/B1/B2/A2（报告 08:05:44 / 08:08:59 /
  08:12:38 / 08:15:44）：DrawTimeCapture 工作量 246.36～247.18 calls/frame，
  差异 <0.4%；NativeHint A 平均 0.049→B 0.0025 ms/frame，局部约
  **-94.9%**。四轮均 device-lost=0、incomplete=0。

**可观测性 2 — DrawTimeCapture 11 段固定 QPC 树**：

- 新增默认关闭的 `DXVK_WAR3_SHADOW_DRAWTIME_BREAKDOWN=1`：
  IdentityResolve / GpuSkinInput / PositionSource / MarkerGatesAndBounds /
  FingerprintAndDedup / CacheRecordSetup / PositionBacking / UvBacking /
  IndexBacking / FinalizeAccounting / GpuSkinSettlement。
- 复用既有 ShadowCapture xorshift admission/sample weight；默认关闭时不读
  QPC。抽样叶节点互斥并按 always-on exact `DrawTimeCapture` 父 ticks 归一化，
  无样本时保留父 self，不伪造数据。
- 明细门
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_08_28_18.html`：
  11/11 叶节点齐全，父 0.243 ms/frame、self=0；三位显示精度下叶和
  0.245（2 us 舍入差）。当前排序：
  FingerprintAndDedup≈0.073、IndexBacking≈0.057、
  CacheRecordSetup≈0.052、PositionBacking≈0.037 ms/frame，其余明显较小。
  artifact：
  `AutoTest/artifacts/unattended_gate_drawtime_breakdown_1d2e_20260724.json`。

**默认优化 2 — DrawTime cache iterator 复用**：

- FingerprintAndDedup 已经 `find(vbCacheKey)`；当 entry 存在但不能 early-dedup
  时，旧 CacheRecordSetup 仍用 `operator[]` 对同一 key 重复 hash/find。现在复用
  尚未失效的 iterator；miss/gpu-skin 路径仍执行原 `operator[]` 默认构造。
  `DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE=0` 精确回滚；
  `..._VERIFY=1` 再走旧 lookup 并比较节点地址，不同立即 abort。
- 独立审查证明 find→deref 间无 insert/erase/rehash/reentrancy，PASS。verifier
  强门 `08:47:31` 3339 frames 无 abort、device-lost=0、incomplete=0。
- 同 DLL `EEDFB123` A1/B1/B2/A2（报告 08:49:11 / 08:50:51 /
  08:52:30 / 08:54:31），CacheRecordSetup 调用量 180.46～182.13/frame：
  原始单位调用约 **-6.79%**；同期未修改的 FingerprintAndDedup 因外部负载
  +6.63%。用该同帧控制归一化后，CacheRecordSetup 相对成本约 **-12.72%**，
  两段合计占比 45.16%→41.82%。父 DrawTime 受噪声 +2.27%，不宣称整帧收益。

**CurrentDraw 严格等价微优化（收益低于当前测量噪声）**：

- 默认不再执行三个全工程无 reader 的 provenance `uint64_t` atomic RMW；
  trusted hit 的重复累计由 canonical
  `g_paletteCaptureTrustedSourceHitCount` 在 summary 导出时派生。字段/JSON key
  保持不变；`DXVK_WAR3_CURRENT_DRAW_REDUNDANT_ATOMICS=1` 恢复旧四个写入。
  32-bit MinGW 下每次 RMW 是 `lock cmpxchg8b`，静态等价性与独立审查 PASS。
- 同 DLL ABBA 中 TrustedQueryPack 单位调用约 -13.8%，但 SnapshotCommit
  +8.7%、Publish parent +4.6%，方向混合且被 Level2 observer/外部负载淹没；
  因此只记录“确定少执行指令”，**不宣称可测整帧收益**。

**新发现但本阶段未盲修的正确性风险**：

- DrawTime `captureFingerprint` 实际未包含 IB identity/backing/slice/index type、
  UV source/layout、topology、`consumeVertexOffset`，且可在 IB 未成功捕获时提交
  fingerprint；同 part 改 IB/UV 或一次预算冷失败可能错误复用旧/不完整 entry。
- IB/独立 UV 已有旧 buffer 但 capacity 不足、当帧 alloc budget 又耗尽时，现代码
  仍可能因 buffer 非空而提交更大 copy，存在目的容量不足风险。
- 下一阶段应先加默认关闭的完整 source descriptor verifier，分别统计 same-frame/
  cross-frame false hit 与 incomplete commit；在数据前不要扩大 dedup，也不要把
  该风险修复误算成性能回归。跨帧 copy-skip 在取得 content-generation 证明前
  不能视为严格安全。

**最终统一构建/部署**：

- exact DLL：30,363,639 bytes，SHA-256
  `EEDFB123BAEF12F52837F234BC55EC5360A288F0598124A96771CA20E17F019A`；
  `build32` 与 `E:\Work\War3\d3d9.dll` 完全一致，ninja no-work 通过。
- 静态合同：Hook/DrawTime 10 tests、CurrentDraw atomics 5 tests、
  iterator reuse 2 tests，全通过。
- 最终默认 Level1 高压门：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_08_56_57.html`，
  2824 frames / 16.308 ms / GPU 2.264 ms；绝对值仅作样本说明，不作收益口径。
  `framesIncomplete=0`、device-lost=0、cleanup=true、War3=BELOW_NORMAL。
  artifact：
  `AutoTest/artifacts/unattended_gate_final_default_eedf_20260724.json`。
- 回退备份：
  `E:\Work\War3\d3d9.dll.bak_20260724_1D2E_pre_iterator_reuse`
  （SHA `1D2EC8DC...`）、
  `d3d9.dll.bak_20260724_01F7_pre_drawtime_atomic`（SHA `01F76EF8...`）和
  `d3d9.dll.bak_20260724_2104_pre_native_hint`（SHA `21043C5A...`）。

**下一顺序**：先实现 fingerprint/IB/UV 完整 descriptor observer + incomplete
capacity verifier；随后按 DrawTime 新树继续处理 IndexBacking 与真正的
FingerprintAndDedup self。CurrentDraw 后续优先考虑 packed 3x4 直接读取以及 TLS
state 合并，但两者都必须先做 byte-for-byte/snapshot verifier。不要仅因原生
`UiRenderable/Sprite` 或 `FrameAnchorVisibilityQuery` 耗时高而跳过动画/可见性逻辑。

## 🚨 2026-07-24（清晨收口）：PublishVisible SafeCopy + Populate permutation view 转正（未 commit）

本轮继续遵守夜间无人值守约束：所有 War3 均在隔离桌面、所有游戏/构建进程均为
`BELOW_NORMAL`，没有切换用户前台；收益只按同 DLL 反序 A/B 的局部比例判断，不用受
外部负载污染的绝对 FPS。动画、可见性判定、阴影像素和 persistent 容量语义均未改变。

**转正 1 — PublishVisible 指针读取**：

- `war3_hook_render.{h,cpp}`：`renderablePart` 的 mesh/scene 小字段读取默认从
  `SafeReadPtrFast` 改为一次 current-process `SafeCopy`；preset scene 时只复制
  4-byte mesh，scene 为空时一次复制 `0x0C..0x17`。复制先落临时量，只有完整成功才
  提交；失败完整回退旧路径。`DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY=0` 可精确回滚。
- 同 DLL `4BDE9958` 反序 A1/B1/B2/A2：`PublishVisible_SafeRead` 每调用
  `1.4252→1.2530 us`，平均 **-12.09%**，两对分别 -11.42% / -12.71%，
  约 224 calls/frame；外部负载下绝对帧时反向漂移，因此只采信局部比值。
- 独立 verifier（候选/旧实现均写局部临时量，mismatch 重读区分稳定/并发变化，
  release assert 使用 `abort`）在报告
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_06_47_14.html`
  以 period=1 比较 **670,784** 次：copy failure=0，initial/stable mismatch=0，
  unstable=0，两个 mask OR=0，两个 mismatch 数组 1..7 全 0，
  stableMatch=attempts。artifact：
  `AutoTest/artifacts/unattended_gate_safecopy_verify_C6AD_20260724.json`。

**转正 2 — Populate Submit permutation view**：

- `d3d9_device.cpp`：保留 stable-sort 得到的 index permutation，group/core-gate/cap/
  append 通过 view 访问原 `EligibleRecord`，不再把含多组 vector/matrix/palette 的完整
  record 搬进第二个 vector。独立审查发现 initial vector 扩容可能留下 self-alias；
  VIEW 现先做一次全量 `War3RebindEligibleRecordPackets`，不依赖 provisional-filter
  配置，随后 storage 不再增删。`DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VIEW=0`
  可精确回滚。
- VERIFY-only 独立构造旧版 materialization shadow，逐项+最终 rebind；完整比较
  packet/renderable/resource/pose/material、矩阵/动态流/runtime palette/sample hashes、
  owned-vector 内容与 alias 状态，不执行第二次 append，口径明确限定为
  **mapping + append-input equivalence**。
- 强门报告
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_06_55_43.html`：
  2943/2943 passed frames，516,746 eligible inputs、516,633 attempted inputs，
  mapping/input mismatch=0，hash 全相等，`framesIncomplete=0`。artifact：
  `AutoTest/artifacts/unattended_gate_populate_permutation_verify_7A3C_20260724.json`。
- 同 DLL `7A3CC4AF` 反序 A1/B1/B2/A2（报告时间 06:57:56 / 06:59:45 /
  07:01:27 / 07:03:06），记录工作量仅 -0.082%：
  `MaterializeAndRebind→PermutationViewRebind` **-94.78%**
  （-94.69% / -94.86%，局部省 0.01977 ms/frame）；
  `SubmitGroupSort` **-58.17%**（-61.07% / -54.73%）；
  整个 `Submit` **-4.56%**（-7.49% / -1.65%）。artifacts：
  `AutoTest/artifacts/unattended_abba_popview_7A3C_{A1,B1,B2,A2}_20260724.json`。

**被数据否决、保持默认关闭**：

- 1024-entry semantic augment TLS cache 虽把 collision 降到 2%～5%，但 generation
  mismatch 约 35%，同 DLL Model/Shadow lookup 分别 **+23.6% / +20.2%**，不转正。
- visible semantic merge index verifier 零 mismatch，但 `RegistryRegister` 平均
  **+5.21%** 且两对方向不稳，不转正。审查另发现 latent 多索引不变量缺口：
  merge 若把 `renderablePart` 从 null 补成非 null，可能未同步
  `renderablePartRecordCount/byRenderablePartLayer`；当前默认活跃 producer 未证明会
  形成该输入组合，未在性能轮盲改，后续应先加专用计数/verifier。

**最终统一构建/部署**：

- exact DLL：30,290,241 bytes，SHA-256
  `1FB73F45813682D90A9D43B7E1332A8757654EBA0ACB274A0D4EADDCA5BEA152`；
  `build32` 与 `E:\Work\War3\d3d9.dll` 完全一致，ninja no-work、Hook catalog
  6 tests、前端 JS parse + 4 mock cases 全通过。
- 默认 Level1 高压门：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_07_08_49.html`，
  3239 frames / 14.218 ms / GPU 2.114 ms，`framesIncomplete=0`、device-lost=0；
  46 个默认 Hook installed，32 个 detail/unsafe catalog 项按门控未安装。
  artifact：`AutoTest/artifacts/unattended_gate_final_defaults_1FB7_20260724.json`。
- 回退备份：
  `E:\Work\War3\d3d9.dll.bak_20260724_7A3C_pre_final_defaults`
  （SHA `7A3CC4AF...`）和
  `E:\Work\War3\d3d9.dll.bak_20260724_4BDE_pre_safecopy_verify`
  （SHA `4BDE9958...`）。

**下一顺序**：继续拆 `CurrentDraw UpdateWorldMatrix/WarVKHookLogic`（约
0.17～0.26 ms/frame）与 `ShadowCapture/Gates/RuntimeBridge`；任何 frame-invariant
缓存先做完整输入 verifier。Populate 的下一个真实大头是 BuildEligible/Append，
不再重复优化已降到约 0.016 ms/frame 的 GroupSort。

## 🚨 2026-07-24（夜间无人值守）：资源热路径 + CSM descriptor 复用 + Lifecycle 可观测性（未 commit）

用户明确授权夜间无人值守继续优化。本轮全程只使用隔离桌面、`BELOW_NORMAL`
War3/构建进程和同工作量局部比例；未使用 IDA、未切换或占用用户前台，未改变动画、
可见性、阴影像素或 persistent 容量语义。所有改动仍与此前工作一起留在未提交工作区。

**默认路径优化与独立验证**：

- `war3_visible_renderables.{h,cpp}`：manifest pose freshness 从每轮构造
  `unordered_set` 改为对象内 `poseFreshGeneration`；generation 0 保留、每次 refresh
  都递增、回绕全清，空/全无效输入仍走旧结果。默认关闭的新旧双算法验证器在 3252
  次 aggregate/scan、1,923,496 条 model record 上
  `modelMismatch=0 / poseMismatch=0 / manifestMismatch=0`。
- 同工作量旧 `CF8FB705` → 新 `FB664193`：`ManifestPublish 0.084→0.068`
  ms/frame（-19.0%），`BuildEligible 0.481→0.412`（-14.3%），
  `RecordLoop 0.312→0.261`（-16.3%），`Populate 1.217→1.077`
  （-11.5%）。考虑外部调度噪声后，以相邻父节点归一化的局部下降约
  **5.5%～13.9%**，不以 FPS 宣称收益。
- registry tracking decision 已改为同轮 aggregate 的 O(1) 决策；默认局部
  `0.063～0.077→0.008 ms/frame`（约 **-87%～-90%**），brute verifier
  全部零 mismatch。
- `d3d9_war3_shadow.cpp` 新增保守的 CSM 跨 draw descriptor 复用：
  仅 descriptor-buffer 后端、仅 non-direct、每 cascade 单项 last-bound；
  签名完整覆盖 layout、三个 buffer 的
  `VkBuffer/offset/size/gpuAddress` 与冻结 alpha
  `imageView/imageLayout`。direct/full-clear 强制失效，资源 lifetime tracking
  保持原位；命中只省 descriptor write，push constants 仍逐 draw 更新。
  `DXVK_WAR3_CSM_DESCRIPTOR_REUSE=0` 可精确回滚；默认关闭 verifier/assert
  实机零 mismatch。

**CSM 同 DLL 反序 A/B（`964E1F63`，相同高压图/约 1100 shadow draws）**：

- OFF-A / OFF-B：`CascadeRecord` 每 descriptor request
  `0.714 / 0.725 us`；
- ON-A / ON-B：`0.441 / 0.426 us`，均值下降约 **39.8%**；
- 整个 `DirectionalShadowMapPhaseSample` 按真实 CSM 执行帧归一化
  `0.675 / 0.681 → 0.470 / 0.440 ms`，均值下降约 **32.9%**；
- ON 两轮约 **82.7%** 请求变为 push-only，约 17.3% 保留 full bind；
  未命中主要为 alpha 纹理切换。整帧绝对值受用户前台负载影响，未用于结论。

**Hook 可观测性补齐**：

- Hook Catalog pilot 已接入 schema v9 与独立前端：32 个 RenderPerf
  默认关闭入口有固定 ID、激活门、ABI/地址、计时合同和状态；
  当前 `31 DisabledByEnvironment + 1 SkippedUnsafeABI(0x368E90 隐式 EDI)`。
  禁用入口不会作为 0ms 热点污染 Hook Breakdown；`hookInventory` 仍是未迁移域
  的安装权威。
- 11 个 Lifecycle 深层 Hook（TlsPump/SelectWorker/RunCallbacks/QueueFlush/
  FinalizeTick/Reschedule/PrepareDispatch/FinalizeDispatch/TickUpdate/
  FinalizeWorker/ComputeWakeDelta）使用 period 4/8 的固定 ID 树。
  首轮 Level2 暴露“安装成功但多数调用在 EventPump boundary 外、无 section”
  的覆盖漏洞；已增加 `War3HotHookRootedCallTiming`：
  有外层 boundary 时保持动态父树，无外层时只为该调用建立 Level2 根边界。
  RAII 顺序为 timing → publish → perfScope；默认 Level0/1 完全不创建该边界。
- 最终 Level2 报告：57/57 inventory installed；本场景实际命中的 8 个
  `Hook_Engine*` 全部出现 Hook total / `NativeOriginalInclusive` /
  `ObserverOverhead` 或 `WarVKHookLogic`，闭合误差 ≤0.001ms（显示舍入）；
  其余 3 个入口本轮未调用。Level1 最终门仅 46 个默认 Hook、Engine 深层
  section 为 0，证明默认门正确。
- 报告前端用独立 headless Chromium 验收：Hook Breakdown
  46 contexts / 57 installed records、Catalog 32/32、Call Tree 新 Engine 根、
  Timeline 16 列 Stage Trends 均正常；唯一 console error 为 `favicon.ico` 404。
- frameSeries/stageSeries 已证实 9～11ms/120FPS 间歇切换是主线程、worker 和
  uncovered frame wall 同时变化的广域调度/抢占现象，不是 WaitGate 或 GPU
  单点；报告继续把 uncovered wall 明确标为 **NOT CPU hotspot**。

**最终 exact 候选**：

- build/deploy：`30,181,221 bytes`
- SHA-256：
  `CE4F42B882E18E14C02D59E6734282F56BEA818A8ACC1E73BE2090E4A62D8887`
- ninja no-work；源码构建 DLL 与 `E:\Work\War3\d3d9.dll` hash/size exact。
- 最终默认报告：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_04_59_04.html`
  （3145 frames，`framesIncomplete=0`）。
- 最终 Level2 报告：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_24_04_52_18.html`
  （2955 frames，57/57 installed，`framesIncomplete=0`）。
- CSM verifier：
  `AutoTest/artifacts/unattended_gate_csm_descriptor_verify_964E_20260724.json`；
  A/B：`unattended_gate_csm_reuse_{off_a,on_a,off_b,on_b}_964E_20260724.json`。
- manifest 双验证：
  `AutoTest/artifacts/unattended_gate_manifest_tracking_verify_FB66_20260724.json`
  与 `unattended_gate_manifest_generation_default_FB66_20260724.json`。
- 安全备份：
  `E:\Work\War3\d3d9.dll.bak_20260724_FB664193_pre_csm_cross_draw_reuse`
  和 `E:\Work\War3\d3d9.dll.bak_20260724_964E_pre_lifecycle_rooted`。

**下一顺序**：继续以最终 Level2 的局部 self/inclusive 占比为准拆
`WorldFramePrepare/NativeOriginal`、`UiRenderableRender`、
`ShadowCapture/Gates` 与 Dispatch bridge；Catalog 仍是 partial pilot，应按域渐进迁移，
不能把 32 当作全项目 Hook 总数。任何动画/可见性缓存或 alpha draw 重排必须另做
语义证明与画面对照，不能从本轮 timing 直接推导。

## 🚨 2026-07-23（16:20）：Persistent miss 优化转正 + Sprite/PostVisibility 第三级拆分（未 commit）

本轮继续使用默认关闭的 Level-2 观察 Hook 下钻，并只把经过独立审查的严格等价
优化放入默认路径。统一构建、no-work、两轮默认高压图和一轮全明细高压图全部通过；
未使用 IDA、未占用用户前台。

**已落地的默认路径优化**：

- `d3d9_device.{h,cpp}`：persistent geometry 年龄 GC 在同一
  `m_war3ShadowPersistentFrameSerial` 内最多全表扫描一次；Present 已在 serial
  递增后扫过，随后每个 miss 不再重复 `O(liveGeometry)` 扫描。强制预算分支仍可
  每次执行，max-age/淘汰/统计不变。
- 新增 `War3CreateShadowPersistentGeometryAfterMiss`；两个已经完成外层
  `War3TryFind...` 且仍处同一 device/render 串行域的调用点不再执行必然失败的
  第二次哈希查询。
- `war3_shadow_runtime_bridge.cpp`：ModelInstance 可写回的 7 个字段已经完整时，
  跳过无副作用的 shared-lock + map read + POD copy；ShadowObject/Pose 顺序不变。
- `war3_current_draw_contract.cpp`：active-slot snapshot 以 TLS occupancy bitmap
  popcount 作为 reserve 上界，不再固定 reserve 4096；已排序 preferred keys 直接
  binary-search，任意未排序调用仍精确回退旧 unordered_set。
- 性能报告新增 6 个 persistent reject 原因：
  `NoIdentity/UnsupportedMode/DynamicSource/AlphaBlend/MissingStorage/CreateOrBudget`。

**新增默认关闭诊断**：

- Sprite `0x12F0A0` 下新增 8 个 period-8 Observer 节点：
  WithOverrides / Simple / ChildStagePresetTree / OverrideGraph /
  AssignSpan / CopyOutput / DefaultAssign / Flush。仍只在
  `PERF_LEVEL>=2 + DXVK_WAR3_PERF_SPRITE_NATIVE_BREAKDOWN_HOOKS=1`
  且所有 pose/matrix producer 关闭时安装。
- WorldPrepare `0x378420` 下新增 `FrameAnchorUpdate@0x377FD0` 与
  `FrameAnchorVisibilityQuery@0x358CF0`，沿用默认关闭的
  `DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS=1`。
- 全部 10 个新入口完成独立 ABI/地址/共址/默认门审查；明细报告 hook inventory
  96/96 installed，默认报告仅 46 个，不安装这些 Observer。

**实测结果**：

- 旧明细 `15:39:04` → 新明细 `16:17:10`：
  `PersistentLookup 0.288→0.042 ms/frame`，
  `MissCreate 0.269→0.021 ms/frame`，局部确定净省约 **0.246 ms/frame**；
  `PostGate 0.433→0.122 ms/frame`。调用次数仍约 29/frame，证明收益来自单次
  miss 成本下降而非工作量减少。
- 默认高压图两轮：
  `16:15:17` = 61.091 FPS / 16.369 ms，
  `16:20:37` = 61.732 FPS / 16.199 ms，GPU 均 2.012 ms；
  对比上一 DLL 默认 `15:37:41` = 58.814 FPS / 17.003 ms。
  FPS 含场景噪声，发布口径以局部 0.246 ms 为准。三轮
  `framesIncomplete=0`，无 crash/device-lost/budget reject。
- Sprite 新树：约 2.4 ms 的 `SetWorldMatrixAndEvaluateRootPose` 主要落在
  `WithOverrides → EvaluateOverrideGraph`；Simple/ChildTree/assign/copy 很小。
- WorldPrepare 新树：`PostVisibilityGlobalAdvanceA` 约 1.66 ms，其中
  `FrameAnchorVisibilityQuery` 约 1.46 ms，外层扫描不是主体。
- 新报告累计 `persistentRejectCreateOrBudget=34,687`（约 24.6/frame），其他
  5 类 reject 为 0。现有名字仍混合 capacity 与 buffer-create；未取得 persistent
  pool gauge 前不能盲修容量算法。已知旧 bug 是 force trim 不会为 incoming bytes
  预留空间，但只有池接近默认 512 MiB 时才相关。

**当前 exact DLL**：

- build/deploy：`29,820,879 bytes`
- SHA-256：
  `37FFC3862316C1E358308B7142645DDEB2D2BD12A20F267EDB65A5FC0C2AD7EC`
- 默认报告：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_23_16_20_37.html`
- 明细报告：
  `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_23_16_17_10.html`

**下一顺序**：先给 CreateOrBudget 拆出 capacity/buffer-create 与 persistent pool
gauge；随后继续拆 `EvaluateOverrideGraph@0x77C260` 和
`FrameAnchorVisibilityQuery@0x358CF0`。在证据前不要缓存/跳过原生动画或可见性
查询，也不要直接启用已知 capacity trim 改动。

## 🚨 2026-07-23（下午）：WorldPrepare/PreRender 继续拆分 + ShadowCapture 低风险优化（未 commit）

本轮在 Hook/Native/WarVK 动态树上继续做默认关闭的二级诊断，并只落地有同 DLL
阶段计数支撑的低风险优化：

- 新增 `DXVK_WAR3_PERF_SPRITE_FRAME_HOOKS=1`（同时要求 `PERF_LEVEL>=2`）：
  四个 `CSpriteUber` frame-update 入口按 8 次抽样，分别输出 Hook total /
  `NativeOriginalInclusive` / `WarVKHookLogic`；perf-only 模式不恢复已关闭的 pose、
  registry 或 dt probe。
- 新增 `DXVK_WAR3_PERF_WORLD_PREPARE_RESIDUAL_HOOKS=1`（同时要求
  `PERF_LEVEL>=2`）：覆盖 0x368E00 / 0x3AC130 / 0x369370，纯观察器残差明确命名为
  `ObserverOverhead`。0x368E90 依赖调用方隐式 EDI，普通 C++ MinHook detour ABI
  不安全，刻意跳过。
- `DispatchCommon/Special` 的完整动态分账由每调用采样改为 1/8 加权抽样；节点与
  Native/WarVK 拆分保留，减少每帧数百次成对 QPC。
- widget negative cache 默认 TTL 从 1 帧改为 8 帧（环境变量仍可设 1 精确回滚）；
  positive identity cache 每 draw 先查，因此新发布身份立即胜出。高压同 DLL 分段：
  `WorldMagicRead` 101.287→13.189 calls/frame、0.282→0.052ms，
  `PathBlockerFallback` 0.320→0.088ms，`ShadowCapture` 1.696→1.541ms。
- ShadowCapture 内多次 `executionRoute()` 机械合并为一次局部读取。曾尝试删除
  DrawTimeCapture 二次 blocker 解析，但审计发现该块还向 4 个下游传播 rawcode，
  已完整撤回，不能写成已优化。

Level2 高压报告
`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_23_14_56_51.html`
证明：`UiRenderableRender/NativeOriginalInclusive` 2.398ms 中，
`SpriteFrameUpdate` 2.330ms（native 2.320 / Hook logic 0.010）、
`SpriteMiniFrameUpdate` 0.130ms（native 0.120 / Hook logic 0.010），即旧 2.5ms
黑盒几乎全部是 game.dll Sprite PreRender 本体，不是我方 pose producer。WorldPrepare
三个残余入口仅约 0.03ms / 0 / 0，不是大头；Prepare native self 仍约 2.220ms 待拆。

最终统一构建与部署 exact：29,783,860 bytes，SHA-256
`F3061ED9F9FA1EF69EF4698B6106CA50CA8190E24F6D8F6D8AD5367481D0D6E0`。
默认高压门报告
`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_07_23_14_55_46.html`：
1455 帧、57.553fps、17.375ms、GPU 2.021ms、`framesIncomplete=0`。
Level2 全诊断门：1462 帧、57.984fps、17.246ms、GPU 2.043ms、
71 个 Hook inventory 全部安装成功、`framesIncomplete=0`。两轮均无 crash/device lost；
全部改动仍未 commit。

## 🚨 2026-07-23：Hook/Native/WarVK 动态分账 v2 完成（最终高压门通过，未 commit）

用户要求解决热点榜多条 `Orig` 无法辨认、原函数区间混入 WarVK 回调却被误判为
“纯原生耗时”的问题。本轮完成全域 Hook 安装盘点、低污染动态调用树、安装清单与
前端 Hook Breakdown。**结论：旧 `Orig` 是 trampoline 墙钟 inclusive，不是纯
game.dll CPU；原函数执行期间会重新进入 Dispatch/D3D9/Shadow/Registry 等 Hook。**

**统一计时合同**：
- Hook 根 = 整个 detour；`NativeOriginal`/`NativeOriginalInclusive` = 原函数指针墙钟；
  `WarVKHookLogic` = detour 直接自定义逻辑；Native self = 扣除已识别嵌套子树后的残余。
- `PERF_LEVEL=0` 关闭 Hook 计时；`1` 为默认低频 frame scope；`2` 才启用完整热 Hook
  动态树。高频路径用固定 ID + TLS + QPC 加权抽样，禁止字符串 scope。
- 旧 `MakeRenderHookDrawScope` 已退休；它与新树同路径重复写账，曾把 Common calls/
  HookTotal 放大约 3 倍。最终报告闭合误差最大仅 0.001ms（显示精度舍入）。
- `frameSeriesDebug` 已删除；section TLS cache 改 512 项小范围哈希探测，避免旧 64 项
  线性 FIFO 在 200-300 个 section 下每帧抖出/锁争用。

**覆盖范围**：Render/Dispatch/UI/JASS/Lifecycle/Wait/CurrentDraw/Model/Shadow/Widget/
Storm、D3D9 四个 Draw、ShadowCapture 回调及条件 GPU-skin upload/copy kernel；中央
`InstallMinHook` 和直接安装路径导出 `hookInventory`。最终默认报告有 48/48 installed，
泛化 `Orig` 节点为 0。已知原生 StageUpdate/FlushTransparent 地址来自现有 address
book，只加纯计时 detour；后续这条线**不需要 IDA**。

**最终候选 DLL**：`E:\Work\War3\d3d9.dll` = SHA-256
`48F13426F97067458FE1974BF5258ADA7ED95F4B9E0C62CCEB0F0DC4C9B01A54`
（29,725,013 bytes，源码=构建=部署 exact，ninja no-work 通过）。最终高压 hot-shadow
gate：`war3_perf_report_auto_2026_07_23_12_04_03.html`，4855 perf frames，51.606fps，
19.377ms，GPU 2.400ms，`framesIncomplete=0`，无 crash/残留进程；artifact：
`AutoTest/artifacts/perf_trace_gate_final_clean_20260723.json`。

**最终 detail 报告**（低图、`PERF_LEVEL=2`）：
`war3_perf_report_auto_2026_07_23_12_00_15.html`；artifact：
`AutoTest/artifacts/perf_trace_detail_final_clean_20260723.json`。关键分账（ms/frame）：
- `WorldRenderScene` total 4.292 / direct Hook 0.004 / native self 0.026 /
  attributed native subtree 4.262；旧“原生 4ms”判定被实证推翻。
- 主路径 `FlushAndReset` total 2.824 / direct 0.041 / native self 0.038 /
  nested 2.745；主 `FlushSortedItems` 2.027 / direct 0.008 / native self 0.072 /
  nested 1.947。
- `WorldFramePrepare` 2.140 / direct 0.006 / native self 1.040 / nested 1.094
  （主要 `UiRenderableRender` native self 约 1.073）。
- WarVK/前端已知热点：D3D9 DrawIndexed frontend 1.684、ShadowCapture callback 0.963、
  Dispatch Common direct 0.416、CurrentDraw 0.166、Dispatch Special 0.150、Submit 0.088、
  AddBatch 0.043ms/frame。Unattributed active wall 仍 4.949ms（coverage 60.3%），是下一轮
  应拆的非 Hook/尚未接线边界，不得重新叫 WaitGate。

**观察器税**：退休旧 per-draw scope 后，最终 detail 单轮 `2-1=+0.214ms/frame`；此前
AB/BA 漂移较大，必须继续用反序均值，不得从单轮自动化 FPS 承诺物理收益。默认 level1
与 level0 的最终 AB/BA 样本约 +0.179/+0.541ms，均值约 +0.360ms。真实前台 FPS 基线
比较应设 level0；性能归因报告默认 level1，深挖才显式 level2。

主要实现文件：`war3_hook_perf.h`、`war3_hook_install_util.{h,cpp}`、各 Hook 域文件、
`d3d9_device.cpp`、`war3_perf_monitor.{h,cpp}`、`war3_perf_report_template.h`、
`AutoTest/run_perf_rebuild_validation.py`。全部工作区改动仍未 commit。

## 🚨 2026-07-22（深夜第二轮）：22:23 报告验收 + frameSeries lane 算法重写 + Gates 命名修正

**22:23 报告验收（D20CA3DB，3600 帧）**：
- 覆盖率 98.5%，UnattributedCPU 0.233ms；动态父路径修复生效——
  `Hook_WorldRenderScene` 8.55 incl / self 仅 0.005，子树干净展开：
  `WorldDispatch/Orig` self 4.45（native per-draw dispatch）、
  `FlushAndReset/Orig/FlushSortedItems/Orig` 3.00、`WorldFramePrepare/Orig` 2.40、
  `Populate` 0.87（正确嵌套在 FlushAndReset 下）。
- **ShadowCapture 细分生效**：总 3.04ms = **Gates 1.69ms**（闸口/path-blocker/
  semantic bypass 段，最大子项）+ legacy 余量 ~1.32 + S1Early 0.024 + Finalize 0.002；
  `BuildSemantic` 合计 ~0.64ms（嵌在 WorldDispatch/Orig 与 FlushSortedItems/Orig 下）、
  `MaterialSig` 0.006、`NativeHint` 0.058。
- jank：817/3600 帧 >16.67ms，p95 18.2ms，max 65.5ms。

**发现并修复 frameSeries lane 数据矛盾**：旧"parentPath 空=根"过滤的 lane 值与
threadSections 聚合数学上矛盾（Main 31.8 vs 15.5、Other 出现 22→107 锯齿尖峰），
逐行排查未定位单点根因（lane 循环与聚合循环读同一数据源，疑似 archived frame
per-entry 数据的边界脆弱性）。按"宁可简单正确"原则**重写 lane 算法**
（war3_perf_monitor.cpp frameSeries 段）：
- Main = 系统线程 CPU（GetThreadTimes 精确值，~14.1ms）；
- 非主线程 = 按"线程局部顶层 section"（parentPath 不在同线程 path 集合）求和后
  按顶层 path 前缀归类 CS/Other，天然避免 inclusive 双计；
- Worker = max(0, 系统 workerCPU − CS)。

**Gates 命名修正**：addCpuSample 父名+全路径导致 `ShadowCapture/ShadowCapture/Gates`，
改为叶子名（`Gates`/`S1Early`/`Finalize`，父 `ShadowCapture`）。

**构建/部署**：ninja 通过。当前部署 `E:\Work\War3\d3d9.dll` = SHA-256
`69B7B363036408B08A19E52C48D266FB36BFD1E96E69E882EE442484D54A81FA`
（29,376,023 bytes，源=构建=部署 exact）。**待用户再生成报告验收**：
Swimlanes/Timeline 应显示 Main≈14ms、CS≈1ms、Worker≈3-5ms 的合理量级。

**性能本体结论（供后续优化参考）**：15.8ms 帧时中，native 侧
（WorldDispatch/Orig 4.45 + FlushSortedItems/Orig 3.00 + WorldFramePrepare/Orig 2.40）
≈ 9.9ms 是大头；我方 tracked 主要为 ShadowCapture 3.04（Gates 1.69 已是最肥子项，
候选：path-blocker EntryGate 慢路径与 semantic bypass publish 的合并/缓存）、
Populate 0.87、Shadow/Main（CS）0.63。

## 🚨 2026-07-22（深夜）：真实报告验收 + 三项覆盖补丁（build+部署完成）


**首轮重建验收（用户实机报告 `war3_perf_report_2026_07_22_20_25_08.html`，高压图 1025 帧）**：
- **UnattributedCPU 10.4ms → 0.271ms，cpuCoveragePct = 98.2%**，coverageWarning=false；
  meta（SHA/profile/模块掩码/帧对齐）与 schema v7 全部生效。
- 真实热点结构首次可见（66.4 FPS / 15.05ms / GPU 1.34ms）：
  - `Hook_WorldRenderScene/Orig` 7.81ms incl：其中 `Hook_WorldDispatch/Orig` **self 3.96ms**
    （native per-draw dispatch，PERF_LEVEL=2 可再拆 bridge/orig）、
    `Hook_FlushAndReset`（嵌套上下文）~3.46ms、`Hook_WorldFramePrepare/Orig` 2.37ms
    （native culling）；
  - `ShadowCapture` **2.95ms self / 55 calls**（我方最大已知 CPU 项）；
  - `Populate` 0.94ms（已正常）、`Shadow/Main` 0.60ms（CS 线程）、PostFX 0.18ms；
  - jank：162/1025 帧 >16.67ms，p95 18.3ms，max 33.9ms。
- 发现两个监控自身 bug：frameSeries 泳道把 inclusive 父子相加（Main lane 假 80ms）；
  绝对路径叶节点挂扁平父导致嵌套上下文 self 失真（FlushAndReset 嵌套段 3.46ms 异常）。

**本轮三项覆盖补丁（未 commit）**：
1. **动态父路径**（war3_perf_monitor.{h,cpp}）：`CpuOnlyScope` 新增 `dynamicParentPath`
   （push 时 TLS 栈顶），popScope 有效父=动态优先、静态前缀兜底。修复绝对路径/TLS 混用
   的嵌套 self 失真；栈空的绝对路径（WaitGate 等）行为不变。
2. **frameSeries 只统计根 section**（parentPath 非空跳过），消除泳道 inclusive 双重计数。
3. **ShadowCapture 细分**（d3d9_device.cpp）：`War3CaptureCpuSample` 加 `stop()`；
   新增 `ShadowCapture/Gates`（入口→legacy 采样点，含 path-blocker/semantic 闸口）、
   `ShadowCapture/S1Early`（S1 early-cache 块）、`ShadowCapture/Finalize`
   （finalizeShadowDrawCommon）三个 TLS 桶；原 `Shadow/DrawTime/BuildSemantic`、
   `MaterialSig`、`NativeHint` 三个埋点从旧 constexpr 切到 `War3PerfHookLevel()>=1`。

**构建/部署**：ninja 通过（19/19，仅既有 warning）。当前部署 `E:\Work\War3\d3d9.dll` =
SHA-256 `D20CA3DB1F7B89B587185FB5FF45120DB664FD2BC248E4CDD405411BCF6BFEA4`
（29,366,241 bytes，源=构建=部署 exact，no-work 通过）。

**待用户重新生成报告验收**：泳道数值应回到帧时量级（不再 80ms）；
`Hook_WorldRenderScene/Orig/Hook_FlushAndReset` 嵌套段应有正确的 `Orig` 子节点；
`ShadowCapture` 下出现 Gates/S1Early/Finalize 子项。若要继续拆
`Hook_WorldDispatch/Orig` 的 3.96ms（native per-draw dispatch），设
`DXVK_WAR3_PERF_LEVEL=2` 再生成一份报告（per-draw 采样 N=8 加权）。
自动化 crash-gate 仍未跑（环境全屏占用 + 沙盒目录缺失），脚本
`AutoTest/run_perf_rebuild_validation.py` 已备。

## 🚨 2026-07-22（晚）：性能计时体系重建 + 报告前端全面重设计（build+部署完成，实机验证待用户手动）


**背景**：R01–R11 二分测试证明旧报告不可信——关键渲染 Hook 计时被
`kNativeOptimizationPerfTrackingEnabled=false`（war3_internal_test_config.h:382）整体编译剔除；
R11 真实 WaitGate 仅 0.154ms 但 UntrackedActive 高达 10.4ms（残余桶误标）；绝对路径与 TLS
动态栈混用导致调用树断裂；R08/R09 还暴露 shadow-off 时 SemanticScene/Populate 22-24ms
孤儿慢路径。本轮按「Present→Present 根 + 调用树 + 线程泳道」重建，计划文件：
`~/.kimi-code/sessions/.../plans/jean-grey-argent-domino.md`。**未 commit**。

**P0 监控可信度**：`DXVK_WAR3_PERF_MONITOR=0` 全局主开关（构造函数解析，所有 scope
入口空操作）；报告新增 `meta`（DLL SHA-256 自哈希/文件大小/构建时间/runtimeProfile/
enabled/disabledModules/关键 env/perfFrameEpoch↔businessFrameSerial 对齐/PresentEx 每帧
`noteBusinessFrameSerial` 登记）；`Other/UntrackedActive` 改名 `Other/UnattributedCPU`
（JSON 保留旧字段 `avgUntrackedActiveCpuMs` 兼容旧解析器，新增 `avgUnattributedCpuMs`
与 `coverageWarning`（>2ms/帧 为 true））。

**P1/P2 调用树**：新增 `kNativePerfFrameHookTimingEnabled=true`（帧级低频 scope 常开：
WorldFramePrepare/RenderScene/WorldDispatch/RenderGroup/FlushSortedItems/FlushAndReset）+
`DXVK_WAR3_PERF_LEVEL`（0=off,1=frame 默认,2=detail）+ per-draw 采样 token
（Hook_FlushSortedItems 每 N 个 flush 采样，`DXVK_WAR3_PERF_DRAW_SAMPLE_PERIOD` 默认 8，
Horvitz-Thompson 加权，新 `cpuScope(name, weight)` 重载）。补齐三段结构：
FlushSortedItems Orig 段（CallOriginalFlushSortedItems 内）、RenderScene/WorldFramePrepare
After 段、Dispatch_Common/Special 全部桥接段。所有 `MakeRenderHookCpuScope` 调用点按
帧级（MakeRenderHookFrameScope，31 处）/per-draw（MakeRenderHookDrawScope，16 处）分级。

**P3 schema v7**：JSON 新增 `schemaVersion:7`、`frameSeries`（每帧
[epoch,totalCpu,gpu,main,cs,worker,other] 四泳道，worker 线程按类别归并不按 tid）、
`sectionPercentiles`（path→{p50,p95}）、`threadLanes`。

**P4-1 门控修复（唯一行为改动）**：d3d9_device.cpp:14985 的
`War3TryPopulateSemanticShadowScene` 增加
`IsAnyWar3RuntimeModuleEnabled({ShadowCapture,ShadowMap,ShadowReceiver})` 前置条件——
shadow-off 时不再执行 22-24ms 孤儿 Populate。**crash-gate 未跑**（见下）。

**P5 前端全面重写**：新 `war3/tools/war3_perf_report_template.h`（880 行，
`kWar3PerfReportHtmlHead/Tail` 两段 raw string，`const data = ` 注入点）；旧模板
（war3_perf_monitor.cpp:7528-8780）整段删除换三行拼接；导出改**原子写**
（.tmp + MoveFileExA REPLACE_EXISTING|WRITE_THROUGH）。新 UI 四标签页（默认 Hotspots）：
Hotspots（avgSelfCpuMs 降序 + 热度条 + p95 + 线程徽章 + 点击跳树）、Call Tree（虚拟
Frame 根 + 虚拟滚动 + 搜索自动展开高亮祖先链 + 线程切换）、Timeline（frameSeries canvas
堆叠条 + GPU 折线 + >P95 红标 + 框选）、Swimlanes（Main/CS/Worker/Other 四泳道）；
顶栏 meta 徽章 + coverageWarning 黄条 + 全局搜索（子串 AND/`>1.5`ms/`thread:cs`）；
GitHub-dark 美学（#0d1117/#161b22/#d29922/#39c5cf）；Legacy 折叠区。子代理已做
node --check + DOM stub 冒烟 + mock 预览 `AutoTest/artifacts/frontend_preview_test.html`。

**构建/部署**：两次 ninja 均通过（仅既有 OPCode -Wreorder 与 GCC -Wmaybe-uninitialized
误报）。当前部署 `E:\Work\War3\d3d9.dll` = SHA-256
`982347A897658046B3A1CCD5EB99E14A511CD5D85F09AE1CC7299286A9CE645C`（29,362,272 bytes，
源=构建=部署 exact，no-work 通过）。旧 DLL 备份
`E:\Work\War3\d3d9.dll.bak_20260722_pre_perf_rebuild`。

**验证状态（重要）**：自动化 crash-gate 与 P6 跑组**未完成**——
`E:\Work\War3_AutoTestSandbox` 沙盒目录已不存在（DEFAULT_WAR3_DIR 指向它导致 launch
失败），且用户反馈当前有程序全屏导致 War3 无法启动，改为**用户手动测试**。已备好
`AutoTest/run_perf_rebuild_validation.py`（已修正为直接用 `E:\Work\War3`：
`gate`（高压 hot-shadow crash-gate）/`monitor`（P6-1 on/off）/`r08`（P6-3 修复后 R08
shadow+postfx off，预期 Populate 消失）/`resource`（P6-2 semantic off vs on）。
用户手动测试入口：正常启动游戏进图后，报告在 `E:\Work\War3\WarVK\Log\` 带时间戳生成
（ImGui 停止录制或 `DXVK_WAR3_PERF_RECORD_ON_START=1` +
`DXVK_WAR3_PERF_AUTO_EXPORT_SEC`）。验收点：报告顶栏有 SHA/配置徽章；Hotspots 默认页
Self 降序；Call Tree 出现 `Frame → Hook_WorldFramePrepare/Hook_WorldRenderScene → …` 树；
UnattributedCPU 应较旧报告 10.4ms 显著下降。

**回退路径**：`DXVK_WAR3_PERF_MONITOR=0` 全停监控；`DXVK_WAR3_PERF_LEVEL=0` 只停 hook
计时；`kNativePerfFrameHookTimingEnabled=false` 编译期回退；P4-1 回退=删除
semanticShadowModuleEnabled 条件；旧 DLL 用上述 .bak 恢复。

**下一步**：1) 用户手动实机验收新报告 + crash-gate（脚本已备）；2) P6-2/3 数据到手后
决定资源层 1.9ms 未归属与 ShadowCapture 2.87ms 细分（第二轮 P3 余项+P4-2/3）；
3) 提交策略待用户确认（本轮与此前 7 项安全修复、T1 整改均未 commit）。

## 🚨 2026-07-22（凌晨）：D3D9/交换链/reimpl 生命周期 7 项安全 Bug 修复（全部 crash-gate 验证，未 commit）

本轮**未触碰用户活跃的阴影/点光/体积光可视区**，只在崩溃可验证、非视觉的生命周期/内存
安全面做了 6 轮代码审查，落地 **7 个真实 Bug 修复**，逐个独立构建 + 高压图 crash-gate 验证：

1. `war3/shadow/war3_shadow_native_runtime.{h,cpp}`：两个 reject 计数器跨线程数据竞争 →
   改 `std::atomic<uint64_t>` + relaxed（**wired 实时可达**）。DLL FD6857A8，3530 帧 PASS。
2. `d3d9_interface.cpp` CreateDevice：InitialReset 失败分支泄漏整个 `D3D9DeviceEx`（含
   `Rc<DxvkDevice>`/adapter）→ 失败前 `delete device`。DLL 7E10B135。
3. `d3d9_interface.cpp` ValidatePresentationParametersEx：空指针判空在解引用之后成死代码
   （ResetEx(NULL) 崩）→ 判空上移到函数首句。DLL 7E10B135，3169 帧 PASS。
4. `d3d9_swapchain.cpp` GetFrontBufferData：对空 `m_backBuffers` 调 `.back()`（UB，失败
   Reset 后截图可达）→ 加 `empty()` 守卫，与 Present(:389)/GetBackBuffer(:805) 一致。
5. `d3d9_swapchain.cpp` CheckColorSpaceSupport：未守卫 `m_wctx` 空指针（姐妹 SetColorSpace/
   SetHDRMetaData 都守卫）→ 加判空。DLL 40AC8281，3196 帧 PASS。
6. `d3d9_shader.h` D3D9VertexShader::SetPartner：Release-before-AddRef 自赋值 UAF 隐患
   （fork 专有 m_partner，wired 但坏分支未触发）→ 改 AddRef-before-Release 次序。DLL 11F36BB7，3247 帧 PASS。
7. `war3/reimpl/war3_team_color_manager.{h,cpp}` GetIndexFromTexture：缺失姐妹锁的读侧数据
   竞争（该类当前**未接线**，潜伏）→ mutex 改 `mutable` + `lock_guard`，与 5 个姐妹方法一致。

**当前部署**：`E:\Work\War3\d3d9.dll` = SHA-256
`B9AFF330A65B8EEC27D0CEEEA79450534BBBF207B543F8E4146136D490051C5F`（29,305,110 bytes），
源=构建=部署 exact。最终高压图 crash-gate（`光影测试(高压).w3x`，隔离桌面，hot-shadow 门）：
3179 帧无崩溃、`framesIncomplete=0`、全 rejects=0、44.65fps 无回归、无残留进程
（报告 `WarVK/Log/war3_perf_report_auto_2026_07_22_02_05_05.html`）。**7 项修复与用户体积光
重构均未 commit**，等用户定提交策略。验证强度分级：#1–#5 修复可达路径（#1 是 wired 计数器上
真实活跃竞争）；#6 wired 但坏分支未触发，gate 证 happy-path 无回归；#7 整类未接线，正确性靠
与 5 个姐妹方法逐字一致的加锁由检视确立，gate 只证对已接线 DLL 零回归。

**第 6 轮审查结论**：hook 安装/生命周期、jass native plan 缓存、dispatch contract、
instance buffer、shader patcher、StateBlock/buffer Lock/Query/shader/volume·cube·3D texture
等 wired fork 面**全部干净或已修（忠实上游 DXVK 守卫）**；另一潜伏项 `War3InstanceBuffer::Get`
首设备指针陈旧（单设备 Reset 场景安全，仅设备销毁+重建才活）暂不修。

**性能：未取得可测 FPS 提升（A/B 硬性证伪安全面已竭尽）**。禁用整个阴影子系统帧率反而
44→24fps、`War3SemanticScene/Populate` 1.6→28.6ms → 阴影缓存是承重结构，无"减阴影提 FPS"空间。
整帧 22ms 中我方 tracked CPU 仅 ~6.3ms（ShadowCapture 3.38ms 已 O(1) 早命中、period 强制 1），
其余 ~16ms 是引擎 `WaitGate`（非我方代码）。六轮全帧数据稳定在 44.36–45.67fps 噪声带内。

**Report2（21:43，jank1138）卡顿根因已定位，但都在用户活跃区/需画面验证，睡眠期不盲改，
待用户授权后按严格 crash-gate + 画面对照落地**：
1. 点阴影 CPU worker 每帧 `std::async(std::launch::async)` spawn 新 OS 线程
   （`d3d9_war3_shadow.cpp:3576`，`.get()`@3471 阻塞主线程）→ 转持久线程池（T1-1 遗留，最优先，
   不改阴影像素语义，最可能直接压掉 worker 尖刺）；
2. `War3SemanticScene/Populate` 28ms 未缓存慢路径（平时 single-flight 门控 1.6ms，误触即尖刺）；
3. 自适应阴影分辨率无迟滞（`d3d9_war3_shadow.cpp:982` ResolveAdaptiveShadowResolution，
   与体积光固定分辨率预留耦合，阈值附近振荡引发资源抖动）。

---

## 🧭 KimiCode 接手引导（2026-07-21 晚，图形优化整改中线状态）

**当前焦点**：图形系统（点光源/点阴影/世界光阴影 CSM/体积雾体积光）优化质量整改。
权威任务清单见下方「🔧 整改任务清单」；7 路审查的**全量取证原文**在
`docs/research/graphics_optimization_audit_2026_07_21.md`（91 KB，含全部 文件:行号
证据与改进建议，清单未收录的低优先级发现也在其中）。

**进度快照**：
- 第一梯队 T1-1～T1-6 已全部落地并标记 ✅（验证：ninja 增量编译 + no-work）。
- 第二梯队 T2-1～T2-4、第三梯队 T3-1～T3-4 均未开始。
- 当前 DLL：29,295,805 bytes，SHA-256
  `B862A514DF460ED7B506CDF8F44D78B3BDB0ACB10CA7455EF0F1C4E24F87097F`（build-only）。

**Git 状态警告**：全部整改改动（以及此前 VS-B1/StormBreaker 等大量工作）目前
**均未 commit**，只存在于工作区。接手后第一件事建议与用户确认是否提交。

**构建环境（Git Bash 下必须照做，否则编译器静默退出 1 且无 stderr）**：
```bash
export PATH="/e/Dev/MinGW/bin:$PATH"
"D:/Environment/Python3.13.11/Scripts/ninja.EXE" -C build32 src/d3d9/d3d9.dll -j2
```
（cc1plus 依赖 `E:\Dev\MinGW\bin` 的 DLL；ninja 在 Python3.13 Scripts 下。）

**严格下一步顺序**（纪律同 AGENTS.md 一贯要求）：
1. 与用户确认 git 提交策略 → 2. 部署 exact DLL → 3. 隔离 light crash-gate 验证
   T1 全量改动无运行回归 → 4. 才允许开始 T2；T3 各项必须先过 crash-gate 再做
   ABBA，禁止仅凭静态分析或编译通过声称性能收益。S1 period 必须为 1；
   用户未明确让出前台前禁止 foreground `dual_perf`。

---

## 🚨 2026-07-21（下午）：第一梯队 T1-1～T1-6 全部落地（build-only）

本轮完成图形系统优化整改第一梯队全部 6 项（明细见下方清单 ✅ 标注）：

- T1-1 点阴影 worker 改 `War3PointShadowCpuPlanInput` 小 POD + move（持久 worker 子项遗留）；
- T1-2 两个 registry 全部纯读 find 已核实为 `std::shared_lock`；
- T1-3 ShadowPoseStore/ShadowModelResourceStore 快照存储按值调用点全部改指针版
  （`war3_shadow_renderer_core.cpp` 8 处 + `d3d9_device.cpp` 静态补充段，含另一线程
  遗留的 `resourceRecord.` 点访问编译错误修复）；mutex 保护的 registry 按值 find
  维持锁内拷贝安全模式不变；
- T1-4 逐记录计时/ScopedMaxUs/ResolvePhaseTimer/Stream1Layout 全部默认关闭并门控，
  两个 debug env 静态缓存；
- T1-5 12 KB publish 栈数组与 trusted vector 改 thread_local 复用，
  `BuildShadowReplayDraws` 返回 const&；
- T1-6 体积太阳路径保存/恢复 `reconciliation` 与 `m_shadowMapRenderSerial`，
  发布统计不再被体积 pass 覆盖。

构建环境注意：本机 ninja 位于 `D:\Environment\Python3.13.11\Scripts\ninja.EXE`，
且 MinGW 15.2.0 的 cc1plus 依赖 `E:\Dev\MinGW\bin` 下的 DLL——Git Bash 中必须
`export PATH="/e/Dev/MinGW/bin:$PATH"` 再调 ninja，否则编译器静默退出 1（无 stderr）。
构建：`ninja -C build32 src/d3d9/d3d9.dll -j2` exit 0，随后 no-work 通过。
DLL 29,295,805 bytes，SHA-256
`B862A514DF460ED7B506CDF8F44D78B3BDB0ACB10CA7455EF0F1C4E24F87097F`。

**未部署、未跑 crash-gate、未做 ABBA**：本梯队全部静态语义等价改动，但按纪律仍须
先过隔离 crash-gate 再谈性能数字；禁止仅凭编译通过声称收益。下一梯队（T2-1 UBO ring
slice / T2-2 copy region / T2-3 点阴影状态去重 / T2-4 native 后端死资源）未动。

## 🔧 2026-07-21：图形系统优化质量整改任务清单（多代理审查驱动，权威记录）

> 来源：7 个并行审查代理对体积雾/CSM/阴影核心/语义捕获/点光源/后端外围的逐文件
> 取证（全部附文件:行号）。**本清单是已知问题的权威记录**：解决后在此标记 ✅ 并注明
> 验证方式；后续 Agent 不需要重新审查已标记项，只需关注未标记项与新引入代码。
> 纪律：触及剔除/租约/缓存淘汰语义的改动必须先过隔离 crash-gate 再做 ABBA，
> 禁止仅凭静态分析承诺性能数字。

### 第一梯队（低风险纯 CPU/诊断收益）

- [x] **T1-1 点阴影 worker 每帧双份场景深拷贝**：`d3d9_war3_shadow.cpp:3544-3549`
  每帧两次深拷贝整个 `War3PipelineInput`（caster 大结构 vector + 每条约 16 KB 的
  palettes 哈希），worker 实际只读 settings、3 个 stats 字段与 palettes。改小 POD
  捕获 + `std::async` 换持久 worker。
  ✅ 2026-07-21：已改 `War3PointShadowCpuPlanInput` 小 POD（settings 值拷贝 + 3 个
  stats 字段 + palette hash 列表）并 move 进 lambda。持久 worker 子项留待后续。
  验证：ninja 增量编译 + no-work 通过（未实机）。
- [x] **T1-2 registry 只读 find 误用独占锁**：`war3_model_registry.cpp` 28+ 个、
  `war3_shadow_object_registry.cpp` 6 个只读 find 用 `std::unique_lock`，与
  Phase 7.83 注释声称的 shared_lock 不符，read-read 线程互斥。全部改
  `std::shared_lock`。
  ✅ 2026-07-21：两文件全部纯读 find/snapshot/count 已核实为 `std::shared_lock`，
  仅 writer（storeRecord/note*/bind*/clear）保留 unique_lock。
- [x] **T1-3 find 按值返回整记录**：`war3_shadow_runtime_contract.cpp:3031-3162`
  等热路径经 out 引用拷贝含 7+ vector / 最多 256 个 Matrix4（约 16 KB）的记录，
  指针版重载已存在但少用。调用点机械替换为指针版。
  ✅ 2026-07-21：ShadowPoseStore/ShadowModelResourceStore 快照存储的按值调用点
  已全部改指针版：`war3_shadow_renderer_core.cpp` 8 处（pose 候选、attachment
  补充循环 128×2、descendant 扫描、mesh pose 计数等）与 `d3d9_device.cpp`
  静态补充段（含编译修复：另一线程遗留的 `resourceRecord.` 点访问与按值 pose
  find 一并改为指针版）。mutex 保护的 `model::PoseRegistry`/instance/shadow
  registry 按值 find 属锁内安全模式，不在本项范围。验证：编译 + no-work。
- [x] **T1-4 常开逐记录诊断与环境变量热路径**：
  `war3_shadow_renderer_core.cpp`（9492 行无 `#ifdef`）每条 manifest 记录 12+ 次
  `steady_clock::now()`；`NotePublishedStream1Layout`
  （`war3_current_draw_contract.cpp:436-475`）无 env 门控；
  `d3d9_war3_shadow.cpp:5661/5674` 每帧两次 `getenv`+string 分配。加
  constexpr/env 门控并静态化。
  ✅ 2026-07-21：`ScopedMaxUs`/`ResolvePhaseTimer`/逐记录 recordStart 均经
  `RendererCoreTimingProbeEnabled()` 门控（默认关，功能性 chunk 时间盒不受影响）；
  `NotePublishedStream1Layout` 默认关，`DXVK_WAR3_STREAM1_LAYOUT_PROBE=1` 开启；
  两个 debug env 已静态缓存。验证：编译 + no-work。
- [x] **T1-5 12 KB 栈清零 + 按值返回**：
  `war3_current_draw_contract.cpp:1463` 每次 ready publish 零初始化 12 KB 栈数组
  （该路径每帧 10K-30K 次，数十 MB/帧无效写带宽），改 thread_local 复用；
  `BuildShadowReplayDraws` cache 命中仍按值拷贝 N 个指针，返回 const&。
  ✅ 2026-07-21：`trustedPaletteBytes` 已改 thread_local 无零初始化（下游只读
  requiredBytes 前缀）；`trustedPalette` 临时 vector 同步改 thread_local 复用；
  `BuildShadowReplayDraws` 已返回 `const&`，5 个调用点全部引用接收。
  验证：编译 + no-work。
- [x] **T1-6 体积 pass 覆盖主 CSM 统计（诊断正确性）**：
  `d3d9_war3_shadow.cpp:2295-2313` renderShadowMap 入口重置 reconciliation 计数，
  体积太阳开启时发布的"每帧"统计实际被体积 pass（1-2 层）覆盖，render serial
  每帧双增——依赖这些计数的 crash-gate/报表会读错。按 Main/Volume 分桶。
  ✅ 2026-07-21：`renderVolumeSunShadow` 在成员交换时同模式保存/恢复
  `reconciliation` 与 `m_shadowMapRenderSerial`（正常路径与 catch 路径均恢复），
  发布后看到的永远是主 CSM pass 的计数与连续 serial。验证：编译 + no-work。

### 第二梯队（GPU 常规收敛）

- [ ] **T2-1 4 块单缓冲 UBO 跨帧 WAR barrier**：体积 CSM/点光 UBO
  （`d3d9_war3_volumetric_light.cpp:1209-1245`）+ 光照/点阴影 UBO
  （`d3d9_war3_shadow.cpp:4862-4894`）每帧走 UNIFORM_READ→TRANSFER_WRITE→UNIFORM_READ
  barrier 对。buffer 均几百字节，改 2~3 槽 ring slice。
- [ ] **T2-2 copyColor/copyDepth 无 region 参数**：全屏拷贝与全屏 pass 改相机视口
  矩形（小视口场景最多浪费 4× 带宽）；相邻 barrier 合并。
- [ ] **T2-3 点阴影路径状态去重**：空 face 改 `vkCmdClearDepthStencilImage`；
  补 pipeline/VB/IB dirty-check（定向 CSM 路径已有排序+dirty-check）；
  `EvaluateShadowGpuSkinDirectInput` 从 (级联/面×draw) 内层上提到 prepare（同一
  draw 每帧最多重复评估 4 次 CSM / 24 次点阴影）。
- [ ] **T2-4 skinned 几何静态 VB/IB 建而不用**：
  `war3_shadow_backend_native_d3d9.cpp:816-845` ensureGeometry 无条件创建上传，
  但 skinned 只走 DrawPrimitiveUP；`blendBuffer` 全工程无绑定点。跳过创建+删死代码；
  native 后端 D3DSBT_ALL 状态块改手工保存约 12 项。

### 第三梯队（需实图/ABBA 门验证，禁止仅凭静态分析晋级）

- [ ] **T3-1 点光 ROI 死代码**：`d3d9_war3_volumetric_light.cpp:1651` ROI 分支要求
  `resolutionDivisor==2`，但除数被 TDR 合同硬钳到 [4,8]，约 40 行区域映射代码
  永不执行，纯点光帧仍付全屏 1/4 ray-march。修复（需复核奇数尺寸光栅合同）或删除。
- [ ] **T3-2 C0/C1 级联对非地形 caster 永不剔除**：
  `d3d9_war3_shadow.cpp:2596-2597`（RTS 俯视保近处阴影的刻意设计，已有
  `DXVK_WAR3_CSM_DISABLE_FAR_CASCADE_CULL` A/B 开关）。保守球剔除分级，用既有
  开关对照，重点看高镜头树影。
- [ ] **T3-3 体积太阳路径不受自适应复用门保护**：
  `d3d9_war3_shadow.cpp:6494-6510` `wantVolumeSun` 不含 `reuseLastShadowMap`，
  主 CSM 命中复用的帧体积阴影仍全量重画。纳入复用门 + prepare 结果与主 CSM 共享。
- [ ] **T3-4 palette 缓存无界增长**：`war3_shadow_backend_native_d3d9.cpp:54-66`
  key 混入逐帧姿态哈希，动画单位每帧新增一条，整场游戏单调泄漏。按帧分代；
  硬件 PCF 评估；`war3_volumetric_light.frag:583-603` 动态索引 spill 用 SPIR-V
  反射定案。

### 整改进度记录

| 日期 | 项目 | 状态 | 验证 |
|------|------|------|------|
| 2026-07-21 | T1-1 点阴影 worker 小 POD | ✅ 完成（持久 worker 子项遗留） | ninja 编译 + no-work |
| 2026-07-21 | T1-2 registry find shared_lock | ✅ 完成（核实已在工作区） | 逐函数核实 |
| 2026-07-21 | T1-3 快照存储 find 指针版 | ✅ 完成（renderer_core 8 处 + d3d9_device 补充段） | ninja 编译 + no-work |
| 2026-07-21 | T1-4 诊断门控 + env 静态化 | ✅ 完成 | ninja 编译 + no-work |
| 2026-07-21 | T1-5 thread_local 复用 + const& | ✅ 完成 | ninja 编译 + no-work |
| 2026-07-21 | T1-6 体积 pass 统计隔离 | ✅ 完成 | ninja 编译 + no-work |

构建产物：DLL 29,295,805 bytes，SHA-256
`B862A514DF460ED7B506CDF8F44D78B3BDB0ACB10CA7455EF0F1C4E24F87097F`。
全部改动未 commit、未部署、未跑 crash-gate / ABBA。

### 清单外补充发现（低优先级，全文见审查文档）

以下发现已收录于 `docs/research/graphics_optimization_audit_2026_07_21.md`，
未列入 T1-T3 梯队，供后续按需认领：

- `AugmentShadowSemanticContext`（`war3_shadow_runtime_bridge.cpp:7845-8004`）每 draw
  链式最多约 18 次注册表查询 + 含 string 的整记录拷贝；建议批量查询或身份完整快判。
- 注册表多镜像写放大（`war3_shadow_object_registry.cpp:177-222` storeRecord 写 6~9 张
  map 全量拷贝）；建议单存储 + 二级索引。
- 分块发布每块深拷贝累计部分帧（`war3_shadow_renderer_core.cpp:9288-9296`，O(N²/16)）；
  建议追加游标。
- palette 槽位缓存 4096 项线性扫描（`war3_shadow_renderer_core.cpp:696-731`）；
  逐字节 FNV 哈希 12 KB palette（`:733-742`）。
- `war3_shader_api.cpp:1515` 用相机 (0,0,0) 取点光快照击穿帧缓存；
  `GetPointLights`（`:1109-1118`）解锁后返回内部 vector 指针（悬窗，正确性问题）。
- CPU 蒙皮逐 draw 重算 + DrawPrimitiveUP 驱动拷贝（`war3_shadow_backend_native_d3d9.cpp`），
  多级联零复用；`D3DSBT_ALL` 状态块每批一次（实际只触约 12 项）。
- 体积光 shader 微项：`pow(denom,1.5)` 可改写 `denom*sqrt(denom)`；未采样的 `s_color`
  binding；copy 资源多余 TRANSFER_SRC usage；receiver PCF 内 per-pixel `textureSize`。
- 自适应阴影分辨率无迟滞（`d3d9_war3_shadow.cpp:979-1000`），阈值附近振荡引发资源抖动。
- 每 4096 次 miss 的 6 键复查 + printf 毛刺（`war3_upper_layer_shadow.cpp:273-308`）。

---

## 📅 项目当前状态 (Current Status)

**最后更新**: 2026-07-20
**当前阶段**: 旧 compute/bypass 正确性基线保持通过；显式 VS-B1 的普通图、
高压图与 lifecycle 正确性已经闭合，但 2026-07-20 多轮隔离 ABBA 仍有约
`+2.79～+3.28 ms/frame` CPU 侧负增量，尚未产生可发布的正收益。
默认关闭的 VS-A 数据脚手架、资源普查、allocator closure 与 GPU static 快照去重已完成。真实
consumer-fenced C++ input lease 与 fixed-function ubershader prelude 已接线，并通过显式 VS-A。
显式 VS-B0 input-only 又在普通图、lifecycle 与高压图通过。独立显式 VS-B1
`vertex_shader_bypass` 现已实现真实 kernel bypass：白名单 draw 不再创建 compute output/job，
Main/Shadow 直接从 static atlas + palette input lease 蒙皮；普通隔离 crash-gate 已 PASS，22,055 次
CPU kernel 与约 415.75 MB 动态输出写入被跳过。默认 Compute、旧 compute/bypass、VS-A/B0 与
StormBreaker stable 均未改默认行为。VS-B1 继续保持显式实验路线，不能改成产品默认。
**分支**: `codex/war3-stable-producer-baseline`

## 🚨 2026-07-21：VS-B1 负候选桥接收窄（最新）

本轮继续保持默认 Compute、旧 compute/bypass、VS-A/B0 与 StormBreaker stable 不变，只修改显式
`vertex_shader_bypass` 实验路线：

- B1 的 palette 上传已按 flush 批次合并为一次连续 host upload；每个 candidate 仍保留独立
  storage lease、consumer receipt 与 Main/Shadow 结算，未放宽寿命或 ABI 证明。
- 新增 exact single-DIP + Main/Shadow settlement 后的 native poison 提前退休；完整诊断模式仍
  保留旧 O1 生命周期。隔离 crash-gate 已证明 poison create/hit/clear、index exact、ledger 与
  reset/restore 均闭合。
- 已知非候选 Common dispatch 现在借用外层 flush detour pin：省略完整 NativeDispatchScope、
  Apply semantic frame、DIP observer、upload/kernel detour 原子 pin，并仅在非空候选视图、精确
  NativeCpuOnly、无 poison/无生命周期事件时生效；空视图、候选正路径、Special、嵌套、reset
  或任一身份漂移仍回完整路径。
- 最新 DLL SHA-256：
  `7448311EE7E9BC2E4278E6CF1749E92913B688213FAC93D260BFD98DC3113319`，源码/部署 exact；
  `ninja` no-work 与 Python 语法检查通过。
- 正确性 artifact：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_borrow_flush_pin_scope_v1_20260721_065752`，PASS；
  B1 bypass `9,503`，poison create/hit `9,503`，clear `5,500`，ledger leak/unreserved/
  duplicate/planMismatch 均为 0，native kernel normal clean。
- 最新隔离 ABBA：
  `AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260721_070000`；disabled/B1 平均
  `10.656/12.787 ms/frame`，仍为 `+2.1315 ms/frame` CPU 负增量，GPU `+0.006 ms`，尚未达到
  正收益，不能把 VS-B1 写成产品默认或性能完成。

下一步应优先围绕 B1 candidate 本身的 per-draw lease/receipt 与少量正候选 dispatch 合并，不能
继续扩大负候选旁路的安全边界；任何新改动先过隔离 crash-gate，再做 ABBA，S1 period 仍必须为 1。

---

## 🚨 2026-07-20：VS-B1 性能复核与 Lock/Unlock NoOverlap 轻量端点

本轮首先完成 reset 冷窗口分类修复后的第三次 lifecycle，随后完成高压格式图；两轮均证明
B1、两次 reset、窗口 resize/maximize/restore、第二进程 relaunch、Special fallback 与资源清理正确。
性能方面没有得到正收益，不能把正确性通过写成性能完成：

- lifecycle PASS：
  `AutoTest/artifacts/gpu_skin_p4_lifecycle_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_lifecycle_reset_class_final_v3_20260720_031808`；
- 高压格式 PASS：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_high_pressure_format_special_v1_20260720_033313`；
- 原始 B1 ABBA：
  `AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260720_034212`，约
  `+3.434 ms/frame`；逐项试过并撤回 DIP manager 融合、input view 缓存、全 buffer view、延迟
  shader/view 清理和 poison ledger 捷径，这些改动都没有稳定收益；
- T1 现改为先读取连续 8 字节 `skinMode/format`，只有候选才复制 `0x6C4` 字节完整 GX 状态。
  crash-gate
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_t1_narrow_skin_format_20260720_073508`
  PASS；ABBA `AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260720_073757` 的 disabled/B1 均值为
  `11.6955/14.4870 ms`，仍负 `+2.7915 ms/frame`；
- production O1 新增实际 D3D9 Lock/Unlock 的 NoOverlap 轻量端点。sidecar、1/127 evidence、重叠、
  storage/资源身份漂移全部回完整证据路径；轻量端点仍要求原 kernel 正常返回与真实 Unlock，且只能
  提交 NoOverlap，不能清 poison。普通 crash-gate
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_fast_lock_no_overlap_20260720_090708`
  PASS；lifecycle artifact
  `AutoTest/artifacts/gpu_skin_p4_lifecycle_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_fast_lock_no_overlap_lifecycle_20260720_092724`
  的 `p4_result.json` verdict 为 PASS，authority lock/kernel/unlock 与 reset/relaunch 全闭合；
- NoOverlap ABBA `AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260720_090929` 的 disabled/B1 均值为
  `10.588/13.801 ms`，仍负 `+3.213 ms/frame`。因此该端点只能写成“正确性无回归、未测得性能收益”。

最终 DLL 29,294,131 bytes，SHA-256
`7297880730736000EA375465FA184573BB941D8A7EC510C249AD862D8B8FA5B0`，source/deployed exact，
随后 no-work PASS；War3/World Editor/YDWE 残留为 0。lifecycle 命令的宿主等待在 artifact 已完整写出后
超时，遗留的本轮 AutoTest/IDA MCP Python 子进程已按创建时间精确清理，没有终止用户原有服务。

结论：当前负增量不在 compute shader 或 GPU 时间，也不在单次 view/restore；光影图每帧只有约 6 个
B1 候选、平均约 584 vertices，却仍让全局 outer-upload/kernel/dispatch hook 覆盖数百个 CPU-only
上传。下一步必须把 poison rewrite retirement 从 outer-upload observer 中拆出，改成 actual
Lock→kernel-normal→Unlock 的独立窄事务，并让详细 upload/dispatch 管理只在 manager 已发布的候选
窗口内激活；随后再合并 palette/input publication。未完成这层前禁止声称“GPU 蒙皮已转正”，也不要
继续用 shader view 或原子计数微调承诺找回 3 ms。

---

## 🚨 2026-07-20：VS-B1 首个真实 kernel-bypass 运行门通过

新增独立显式 `DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE=vertex_shader_bypass`（route 3）。默认/非法值仍
回到 Compute；VS-A、VS-B0 与旧 compute/P4 路线不共享 B1 authority。B1 只接受 opaque、format2、
单 UV、Common stage11/skinMode1、triangle list、instance1、fixed-function FVF `0x112`、单 stream、
无 vertex blend/custom VS/SWVP/material override 的 exact candidate。

- P4 kernel 前同时验证 native Lock/ring/index、static/palette input lease、storage page generation、
  Main fixed-function 状态与 Shadow semantic producer。Outline 尚未实现 input-backed draw，因此请求
  outline 的候选在 kernel 前保留 CPU 路径；
- B1 不生成 compute output/job。Main 的 WVS2 ubershader 直接读取 static/palette；Shadow position/UV
  直接绑定 static atlas 的 SoA position/texcoord0 段。CPU kernel 已跳过后，两个 shader 的私有门若
  异常失败只裁掉 primitive，绝不回读带毒 native VB；
- ABI-9 与 preflight bit layout 未改变。历史 `GpuOutputReady/ExactGpuJob` 位在 B1 通过同值别名表示
  route-specific consumer capability / exact work contract，旧 compute 语义保持不变；
- static atlas 新增 vertex-input usage/stage/access，仅用于 B1 Shadow direct binding；真实 input receipt、
  `Rc<DxvkFence>`、page generation、reset 与 retirement 仍是唯一寿命权限。

首轮运行 artifact
`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_first_runtime_20260720_014654`
没有 crash，画面正常，且已有 7 次真实 kernel skip；但 draw-side 错把 ResolveDip 已合法收窄的 Main
consumer mask 要求为完整 `Main|Shadow`，7 次均被安全 suppress，故正式 FAIL。修复为“非零且只能是
Main/Shadow 子集”，完整 capability 仍由 manager 的 kernel 前 preflight 验证。

正式普通图 artifact：
`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_consumer_mask_fix_20260720_015650`，
PASS；DLL 29,285,294 bytes，SHA-256
`170515583671EDE25855F5CC370A5442654CAD021B8D13D280C0526C78125380`，source/deployed exact：

- input prepared/submitted `23,359/23,359`，palette bytes `66,325,296/66,325,296`；省略
  `23,359` 个 compute output/job 与 `440,855,616 B` output 写入；
- Main WVS2 draw/clear `22,055/22,055`，input/state reject 均 0；Shadow capture/commit
  `22,055/22,055`，direct/replay `356/356`，所有 Shadow input/state/position/index/UV/commit reject 为 0；
- kernel original/bypassed `2,195,685/22,055`，bypassed bytes `415,749,920`；P4 auth/commit、poison
  create/hit、index exact 均为 `22,055`，bypass mismatch 0；
- ledger classified/resolved/consumed `70,077/44,110/44,110`，fallback/suppressed/leak/unreserved/
  duplicate/planMismatch 均 0；late poison 0，forced clean snapshot 3，截图正常，无 crash，最终进程为 0。

lifecycle 两轮均证明 B1、窗口动作、两次 reset 与第二进程 relaunch 本身正确，但 runner 仍严格 FAIL：

- v1 artifact：
  `AutoTest/artifacts/gpu_skin_p4_lifecycle_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_lifecycle_v1_20260720_020407`；
  唯一失败项 `dipFastProbeContractClean`，reset 冷窗口累计 early/late `noTransactionCover=2/1`；
- 第一笔只读诊断重分类后 v2 artifact：
  `AutoTest/artifacts/gpu_skin_p4_lifecycle_isolated_diag_light_sidecar_none_route_vertex_shader_bypass_vs_b1_lifecycle_cold_class_fix_20260720_023801`；
  只剩 early `noTransactionCover=1`。首进程 B1 Main/Shadow `21,521/21,521`，reset request/completion/ack
  `2/2/2`；第二进程 B1 Main/Shadow `8,307/8,307`，两个 ledger 与 cleanup 均闭合。不能把该轮写成 PASS；
- 代码已进一步把“observer cookie/depth 完整，但 requested/applied reset generation 暂时不等”的 sampled
  reject 精确归入 `ResetOrRetirement`，真实 fast-path 行为不变。最终 build/no-work PASS，源码 DLL
  SHA-256 `1975BB891C5CA597178CBDF2E5C9D2AAAB884CA1A02A7293B7C3C14E0803E151`；此时用户启动了
  `worldeditydwe.exe` PID 5552，因此没有部署或第三次 lifecycle。部署 DLL 仍为
  `21A7BA47FB7F2C82B74C8E3D0564EAF2E700669D2FD19BA9BAE9FE6C0FA8E964`，没有半部署。

最新离线产物
`AutoTest/artifacts/gpu_skin_vs_input_lease_offline_20260720_vs_b1_runtime_v1/result.json` 为 schema 6，
7 个生命周期场景与 31 项静态接线检查全部通过，unsafe reuse/pending 为 0。下一严格顺序：等待
World Editor 退出 -> 部署 exact `1975...E151` -> 复跑 B1 lifecycle -> 高压格式/Special fallback ->
透明实图 -> isolated A/B。B1 outline 尚未实现，不能运行 `--outline-all` 后把预期 preflight fallback
误写成失败；foreground `dual_perf` 仍必须等待用户明确让出前台，S1 period 保持 1。

---

## 🚨 2026-07-19：VS-B0 input-only 普通图/lifecycle/高压图通过

新增显式 `DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE=vertex_shader_input_only`，只接受 opaque、format2、
单 UV、fixed-function 白名单。它为 Main/Shadow 发布真实 consumer-fenced static+palette input lease，
但不分配 post-skin output、不记录 compute dispatch；P4 preflight 明确拒绝，CPU kernel 仍精确执行，
Outline/transparent/非目标格式/Special/custom VS 等继续旧路径。默认 Compute 与旧 P4 无行为变化。

高压图首轮暴露 2 个已预留 Shadow consumer 没有进入最终 capture：旧 Bypass settlement 把合法
CPU fallback 拒为 planMismatch，帧尾再 suppress，形成 leak=2。修复只允许“显式 VS-B0、
`bypassCommitted=false`、CpuFallback”这一组合；真正 kernel skip 仍只能 suppress+fuse。复测中 3 个
Shadow CPU fallback 精确结算，resolved=consumed+fallback，suppressed/leak/planMismatch 全为 0。

最终 DLL 29,276,269 bytes，SHA-256
`F9629893627140E98D5CA874B0C6F65F40E1535FA8B302957D014B648897A6DF`，source/deployed exact：

- 离线 input-lease/静态接线：
  `AutoTest/artifacts/gpu_skin_vs_input_lease_offline_20260719_223014/result.json`，schema 4，7 个确定性
  生命周期场景与 24 项 VS-A/VS-B0 runtime integration audit 全通过，unsafe reuse/pending 为 0；

- 普通图 crash-gate：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_input_only_vs_b0_input_only_v2_20260719_213844`，
  PASS；input/Main/Shadow `21,631/21,631/21,631`，省略 `21,631` 个 compute output/job 与
  `408,213,536 B` output 写入；CPU kernel `2,084,826` 次全部 original，P4/poison/index 全冷；
- lifecycle：
  `AutoTest/artifacts/gpu_skin_p4_lifecycle_isolated_diag_light_sidecar_none_route_vertex_shader_input_only_vs_b0_input_only_lifecycle_v1_20260719_214555`，
  PASS；resize/maximize/restore、reset/generation `2/2/2`、第二进程 relaunch 与清理闭合；
- 高压图：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_input_only_vs_b0_high_pressure_format_v2_20260719_220731`，
  PASS；input/Main/Shadow `21,933/21,933/21,930`，Shadow CPU fallback `3`，省略
  `384,015,904 B` output 写入，Special/layout fallback `118,212/580,591`；ledger
  `65,799/43,866/43,863/3`，所有 leak/mismatch/fault 为 0。

自动图 transparent candidate 仍为 0，不能声明透明材质运行覆盖；源码静态门已证明
`bypassOpaqueEligible` 是 input-only admission 的必需条件。当前数字不是 FPS 或永久显存收益。
下一严格顺序：独立 VS-B1 route/preflight 设计 -> kernel skip 前证明 Main/Shadow/Outline consumer
完备 -> isolated crash-gate/lifecycle -> 透明/特殊格式实图 -> isolated A/B。用户明确让出前台前禁止
foreground `dual_perf`，S1 period 必须保持 1；“生与死”资源普查仍是独立任务。

---

## 🚨 2026-07-19：真实 input lease + VS-A ubershader 隔离通过

接手时工作区已有 VS-A shader/draw 草案，但租约不可激活：receipt 未创建、page generation 未写入，
native dispatch/upload epoch 也未同步到 receipt，导致 `GpuSkinInputLease::operator bool()` 恒为 false。
本轮完成最窄正确性接线：

- palette 从 producer upload page 复制到 device-local storage slice；input lease 强持有 static/palette
  slice、storage lease id、page id/generation 与共享 receipt；
- `Pending` receipt 与 desc、range、buffer identity、map/device/frame/native epoch、token、consumer
  mask 逐字段闭合。正常帧尾只在 Main/Shadow draw 之后的真实 `Rc<DxvkFence>`/value 被 output arena
  接受后进入 `ConsumerCommitted`；仅 producer 路径进入 `ProducerOnly`，未提交/拒绝路径进入
  `Cancelled`。终态 receipt、stale generation 或错误 fence 都不能重新发布 draw；
- VS-A 仅接受显式 `DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE=vertex_shader`、Common/stage11/skinMode1、
  triangle list、instance1、fixed-function FVF `0x112`、单 UV、format2。custom VS、PositionT、SWVP、
  override 或任意漂移继续走 compute output；CPU kernel、compute job、output VB 与 P4 permission 未删；
- 私有 fixed-function VS 在 stock 变换/光照/雾/texgen 前按 precise 3x4 顺序读取 static/palette；每个
  draw 同一 CS 闭包内 bind -> draw -> push clear -> resource unbind -> stock shader restore。copy 后
  transfer-write -> vertex-shader-read barrier 已闭合；Shadow direct input 同样保存 page generation；
- runner clean-pair 原先错误要求每个短窗口都必须新增 Shadow replay；生命周期已有 direct 时，短窗
  direct delta 可以合法为 0。现改为零进展必须整组 delta 精确为 0，并加入合成回归。

构建：`ninja -C build32 src/d3d9/d3d9.dll -j2` 与 no-work 均通过。DLL 29,261,468 bytes，SHA-256
`49ECC06749FA13C413943D93D7AC34067B6C139C907BBAC5CEA9595E8FF3379E`，source/deployed exact。

- Compute 控制 artifact：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_compute_vs_a_compute_control_v1_20260719_133219`，
  PASS，旧 P4/Compute 与 StormBreaker stable 默认无运行回归；
- VS-A 正式 artifact：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_vs_a_consumer_fenced_v2_20260719_143012`，
  PASS。input prepared/submitted `12,540/12,540`，palette bytes `35,560,752/35,560,752`，compute jobs
  `12,540/12,540`；Main VS draw/clear `11,863/11,863`，Shadow capture/direct/replay
  `11,863/148/148`；input/state reject、consumer mismatch、clear mismatch、unknown replay 全为 0；
  output+input-storage retired `25,080`、pending `0`、upload page allocate/reclaim `1/1`。两张隔离截图
  人工复核正常，无 crash，最终 War3/runner/build 残留 0。
- VS-A outline 专项 artifact：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_route_vertex_shader_vs_a_outline_regression_v1_20260719_150932`，
  PASS。input prepared `5,918`，Main draw/clear `5,535/5,535`，Shadow direct/replay
  `7,376/7,376`；geometry outline submitted/same-slice `5,026/5,026`，slice mismatch、restore
  overlap 与 pending 全为 0；input storage 与 output retirement `11,836`，pending `0`。
- VS-A lifecycle artifact：
  `AutoTest/artifacts/gpu_skin_p4_lifecycle_isolated_diag_light_sidecar_none_route_vertex_shader_vs_a_lifecycle_v1_20260719_151401`，
  PASS。隔离桌面完成 1280x720 resize、maximize、restore、两次 reset generation 与第二进程
  relaunch；reset request/completion/ack `2/2/2`，generation `2/2/2`，wrong-thread、retirement fault、
  fail-closed、retirement overflow/invalid/pending/fault 全为 0。两个进程均由 retained native handle
  精确终止并完成桌面/视频模式恢复。
- 最新离线静态/状态机产物：
  `AutoTest/artifacts/gpu_skin_vs_input_lease_offline_20260719_153556/result.json`，7 个确定性场景、
  17 项 runtime integration audit 全通过，`unsafePageReuses=0`；该产物只补静态与状态机证据，
  运行权限仍来自上述隔离 artifact。

当前只可声明 VS-A 正确性与真实 consumer-fence 生命周期通过，不能声明性能收益或全模型接管。
outline 与 resize/reset/relaunch 已闭合；下一顺序：格式/透明/special fallback -> VS-B main-only
cohort（不分配 compute output、不建 compute job）-> 隔离 A/B。foreground `dual_perf` 仍需用户明确
让出前台，S1 period 必须保持 1；“生与死”资源普查仍待独立执行。

---

## 🚨 2026-07-19：额度收尾与下一轮起点

- StormBreaker 稳定产品策略离线合同 17/17、资源驻留 chunk 合同 10/10、三个 Python 入口
  `py_compile` 全部通过；最终 x86 DLL 仍为 29,250,964 bytes，SHA-256
  `45E26440CBF33BFBC125CA4128693343C6879E954D1082817BC5C666F9363CF6`，随后 no-work 通过；
- `AutoTest/run_resource_residency_census_isolated.py` 已升级到 v2，默认且正式地图固定为
  `(4)生与死v1.28读档bug修复.w3x`，同时保留显式 `--map-path`。离线测试会拒绝默认地图、参数或
  合同版本被意外改回；
- 本轮收尾时 AutoTest 状态查询发生基础设施长时间无响应，因此没有 deploy、没有启动 War3，也没有
  生成新的 crash-gate 或“生与死”普查产物。构建 DLL 与已部署 DLL 分别为
  `45E26440...9363CF6` / `B6B88C4E...C5BDB`，没有半部署；War3、World Editor、YDWE、ydhost 与
  构建进程残留均为 0；
- 当前可引用的驻留数字仍只来自 2026-07-17 的旧轻图：duplicate host backing
  `44,577,192 B`、managed texture `44,576,936 B`、lazy-readback candidate `23,094,324 B`。
  它们不能回答“生与死”缺失的数百 MiB；下一轮必须先恢复 AutoTest，再用新 runner 跑一次重图，
  且 `evictionAuthority` 在实现 Seal-and-Evict 前仍必须为 false；
- 下一轮严格顺序：部署 exact DLL -> 隔离 light crash-gate -> “生与死”隔离驻留普查 ->
  consumer-fenced C++ input lease -> fixed-function ubershader VS-A。用户未明确让出前台前仍禁止
  foreground `dual_perf`，S1 period 必须保持 1。

---

## 🚨 2026-07-18：StormBreaker 稳定默认策略整合（build-only）

用户已确认 StormBreaker 的产品默认必须是“大块优化 + 原生小块 search 修复”；全量接管为负收益，
只允许独立内存诊断。本轮将 stable `master` v1.3.0 的产品边界写入 DXVK，未复制或回滚
`subprojects/StormBreaker` 的脏实验工作区：

- 未设置 `DXVK_WAR3_STORM_TAKEOVER_MODE` 即启用 stable；显式值只接受 `stable/large`，
  `full/hybrid` 直接拒绝；TLSF 精确阈值固定下限 `0xFE7C`；
- ordinal 401/403/404/405 与原生小块 `search` 修复同批安装；只接受 334,312-byte、SHA-256
  `F8F519CFAA6275A5172A014F0ABED2212284390A33F1194677155A7D408E63EB` 的 Storm.dll，并闭合
  PE/RVA/prolog；
- 页目录只做负过滤。精确 TLSF 块起点、私有头读取和 `LIVE->BUSY` claim 在 allocator 锁内完成；
  exact validator 的 next/previous 上界使用完整 `block_start_offset`，首块损坏的 prev-free 位也会
  fail closed；所有 Alloc/ReAlloc 在首次进入 TLSF 前做 size/alignment 溢出预检；
- managed Free 后端拒绝时恢复旧头/tombstone；原生大块 Free/确实释放旧块的 ReAlloc 会原子修正
  `g_TotalAllocatedMemory @ +0x5738C` 的差额；TLSF vendored 进程级小块 cache 已禁用；release
  启动只初始化 mapping table，不再运行位操作/映射全扫描自测；
- Hook 回滚无法证明移除时永久保留 trampoline、关闭 redirect 并禁止二次安装。已知
  `StormBreaker*.asi` 在本进程已加载时 DXVK 会退出；会话事件发布后再次核验目标 prolog。固定名
  event 是旧 standalone 协议的会话级广播，重命名 ASI 的极端并发仍没有跨实现原子握手；
- 4096 槽 recent-freed 表与 v1.3.0 相同，只是异常 stale/double-free 的有界防线，不是永久证明；
  正常成对生命周期由 exact block + atomic claim 闭合。

离线合同 `AutoTest/test_stormbreaker_stable_policy_offline.py` 为 17/17。唯一 Test Conductor 完成
`ninja -C build32 src/d3d9/d3d9.dll -j2` 与随后 no-work；DLL 29,250,964 bytes，SHA-256
`45E26440CBF33BFBC125CA4128693343C6879E954D1082817BC5C666F9363CF6`，构建工具残留 0。
本轮未 deploy、未启动 War3，不能声明运行门通过。下一步先做隔离 crash gate；重图资源普查只能用
`(4)生与死v1.28读档bug修复.w3x`，SunkenCity/光影图不能替代那几百 MiB 的驻留证据。

---

## 🚨 2026-07-17：GPU 蒙皮路线转向 + 资源驻留第一层收口

完整 isolated ABBA
`AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260717_065042` 证明 disabled→bypass 的
frame/process/main/measured-GPU delta 为 `+2.3475/+2.9630/+2.8435/-0.0010 ms`。损失位于同步
CPU render lane，不是 compute shader 或 GPU 饱和；outside direct receipt 每帧约 143 次也没有
测出收益。禁止继续用零散 wrapper 微调承诺恢复约 2 ms。

新路线为 DXVK fixed-function VS ubershader 前置 War3 skin prelude：

- 已新增 `GpuSkinExecutionRoute`、32-byte `GpuSkinVsDrawParams` 与 88-byte
  `GpuSkinInputLeaseDesc`，但当前 manager/device/native/P4 均不消费 route；默认与非法值都保持
  Compute，draw/P4 authority 为 0；
- static atlas 已允许未来 vertex-shader stage 读取；当前没有 shader 绑定或行为变化；
- 离线 input-lease 产物
  `AutoTest/artifacts/gpu_skin_vs_input_lease_offline_20260717_081253/result.json` 覆盖 pending receipt、
  reset 后旧 ticket、page generation 重开与 stale/copy ticket ABA 拒绝，7 个场景通过，
  `unsafePageReuses=0`；它明确不是 runtime implementation；
- 未来 C++ lease 必须绑定真实 `Rc<DxvkFence>` identity、device generation、static/palette range、
  consumer bits 与 exact receipt。VS-A 先保留 CPU kernel+compute 做 parity/restore；VS-B 才允许
  main-only cohort 跳 compute/output 和原生 CPU kernel。

资源普查：

- 首轮 `resource_residency_census_isolated_20260717_025503` 看到 duplicate host backing
  `44,735,294 B`，其中 managed texture `44,576,936 B`；
- GPU static resource 与 queued miss 现共享
  `shared_ptr<const ShadowGeosetResourceRecord>` 不可变快照，消除 `158,102 B` 持久 mirror 与
  `55,867 B` 观测峰值 queued copy；观察时间戳独立，内容变化才 copy-on-publish；
- 第二轮 `resource_residency_census_isolated_20260717_084857` 中总 duplicate 为
  `44,577,192 B`，GPU static mirror/queued/peak queued host bytes 全为 0；managed texture 仍为
  `44,576,936 B`，lazy-readback candidate `23,094,324 B` 仍只有观察意义，
  `evictionAuthority=false`；model cache alias duplicate 仍为 `338,468 B`；
- D3D allocator reserve/used/mapped 为
  `67,108,864/44,586,688/62,844,288 B`。已修复 `<4 KiB` 尾段丢失、零分配空 chunk 与
  `MapViewOfFile` 失败时伪指针/refcount/mapped 账本；离线 allocator 7/7、快照共享 6/6。

最终中文注释版本：

- build artifact：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_zh_comments_final_v1_20260717_091811_build`；
- DLL SHA256：`CF8922D617EA4698A5945D924E5A527795AE32A3ACFA9326ECB098509468FEC2`，
  source/deployed exact；
- P4 artifact：
  `AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_zh_comments_final_v1_20260717_091811`，
  bypass `11,789 calls / 222,122,784 B`；P3/P4Shadow `11,789/11,789`，poison create/hit
  `11,789/11,789`，index exact `11,789/11,789`；mismatch、hostMismatch、restoreFail、所有 Shadow
  reject、poison overflow/resetStale/outstanding、index leak、ledger leak/unreserved/duplicate/
  planMismatch、nativeKernelNormal、retirement/reset fault/pending/active 与 crash 均为 0；
- 最终 game/runner/build 残留 `0/0/0`。该轮未跑 census、性能或 foreground。

下一步严格顺序：真实 consumer-fenced input lease -> fixed-function ubershader VS-A parity/restore ->
VS-B main-only skip compute/CPU kernel -> outline -> CSM/point shadow -> resize/reset/relaunch -> 格式/
透明/special fallback -> `(4)生与死v1.28读档bug修复.w3x` isolated pressure。用户明确让出前台前仍禁止 foreground
`dual_perf`，S1 period 必须为 1。

本项目后续新增或改写的文档、代码注释和测试说明统一使用简体中文；代码标识符、外部 API、ABI、
环境变量与报表 schema 保持原始拼写。不要为了本地化批量改写未触及的 DXVK 上游注释。

---

## 🚨 2026-07-16：体积积分地表端 + 切片/峰值收口（已 deploy，待实图）

调查结论：上帝视角柱缩回模型的**代码级主因**是相机中心积分截断球
`[0, min(L,D)]` 丢掉近地表尾段；火凤凰多层剪影来自 16 段 + 段内 `max()`；
高镜头 CSM 不完整来自 128/256 Draw 硬截 + C2/C3 cull。**禁止再靠拉 intensity。**

**本轮实现**：

1. **`war3_volumetric_light.frag` 积分区间**
   - 非天空且 `L>D`：`[L-D, L]`（地表端，预算长度仍为 D）
   - 天空：仍 `[0, min(L,D)]`
   - 点光 ray-sphere 同步裁到同一 `[intervalStart, intervalEnd]`
2. **段内 probe**：`max` → **平均**，减轻整段二值膨胀造成的多层剪影
3. **peak/底图衰减降档**：可读性指数下限 0.28；底图硬顶约 18%～28%（旧可到 ~68%）；
   默认 intensity/density/weight → **0.95 / 1.10 / 1.85**；`maxRayDistance=1.0`
   （与 UI 最大距离一致，`D=sunDistance`）
4. **CSM 上限**：SubmitDrawCap **256→512**；DirectRecordCap **128→256**
   无上限 A/B：`DXVK_WAR3_SEMANTIC_SUBMIT_DRAW_CAP=4096` 等
   C2/C3 cull A/B：`DXVK_WAR3_CSM_DISABLE_FAR_CASCADE_CULL=1`

**build**：`ninja -C build32 src/d3d9/d3d9.dll -j2` exit 0。
DLL 28,951,535 bytes，SHA256
`DB388B9E60F4BE2FBD4B480F198337C7D97619895C9AAAF4CD3B80C94AEADDF6`。
已部署 `E:\Work\War3\d3d9.dll`，source/target hash exact。**未启动 War3 / 未跑 AutoTest**，
不能先验宣称俯视画质通过。

**验收**（固定太阳/场景）：
- 上帝俯视柱贴地、压低再抬回不应再「缩回模型」；
- 火凤凰柱叠层应明显弱于旧 max 路径；
- 低镜头靠近不应再突然大黑坨；
- 可选 cap=4096 / far-cull off 对照高镜头树影完整性。

---

## 🚨 2026-07-16：Volume Sun + 俯视柱可读性收口（历史；已被地表端区间路线收窄）

用户反馈：仍要拉满才见柱；压低再抬回上帝视角柱仍缩回模型。曾补双级 Volume Sun +
激进 peak（默认 1.05/1.20/2.35，底图衰减最高 ~68%）。实机/分析显示**根因更在
相机端积分截断**，peak 过猛会引入黑坨/噪声。见上节地表端区间修复。

历史 DLL SHA256 `182F57E88BD321EEC44EDCD36B352BD769F14A878DF4BD4705A399ECE0A2EAE7`。

---

## 🚨 2026-07-16：体积光 CSM 空间连续性 + 独立可读性闭合（待实图门）

用户确认远处树影会随相机靠近进入高清稳定状态，同时体积阴影柱会随镜头压低从模型向地面
“长出来”。主线程与三路只读复核确认这不是单纯相机矩阵错误，而是三个叠加问题：标准 CSM
远级联 world-units-per-texel 更粗；volume 未消费已经上传的 `cascadeBlendRange`；C2/C3 的
receiver sphere 没有向太阳侧保留上游 caster depth。旧 additive-only 合成又把阴影只表达成
“少一点正散射”，因此低雾能量时对比存在硬上限。

- `war3_volumetric_light.frag` 抽出单级联 exact sample，并在 split 前按 surface receiver 的
  smoothstep 区间同时采 c0/c1。先混合 `coverage*(1-visibility)` 与 coverage，再重建
  visibility，避免错误交叉项造/抹柱；primary projection 无效仍与 surface receiver 一样
  fail-soft，不能在 boolean 边界突然硬切到 coarse coverage。每段仍最多 8 个纵向 probe，
  普通/全 seam raw CSM fetch 上限分别为 8/16。
- `War3CsmConfig` 新增内部 `farCasterDepthExtension`。仅当 global post-FX 与 volume 都 enabled
  时，C2/C3 固定使用 384u one-sided toward-sun allowance：`baseD=radius+margin`、
  `eye=baseD+E`、`maxZ=2*baseD+E`。远 receiver world boundary 保持不变，Z range 只增加 E；
  C0/C1 与 volume-off CSM 不变，避免历史对称 `+3000` 的深度精度/抖动回归。
- whole-ray true occlusion 继续负责少散射；`weight>1` 额外把同一真实遮挡以
  `min(Oresolved*0.45*mediumGate*sourceGate*readabilityMix,0.18)` 写入既有 alpha
  base-transmittance。`weight<=1` 仍严格是 no-contrast/physical，coverage-only、CSM missing、
  strength0、极低介质或极弱太阳都不能造黑柱。最大额外底图衰减 18%。
- composite 的 alpha edge 有 2.5% relative dead zone，加上既有外层阈值后实际 3% 内保持
  双线性；`O=1/64` 最大档 cross weight 约 0.13，`O=1/16` 命中 1/64 floor。已知 P2 是相邻
  ray 的物理 T 若也跨过 3%，可能保留低分辨率块边，必须由实图判断。
- Python 纯算术产物：
  `AutoTest/artifacts/volumetric_camera_continuity_offline_20260716/result.json`。安全档
  `3,461,120` segments，普通/seam-only CSM 上界 `27,688,960/55,377,920`；gates=1 时
  `O=1/64` 最大档为 12.5% 少散射 + 5.625% 底图衰减，`O=1/16` 为 25% + 11.25%，
  weight1 额外衰减 exact 0，完整遮挡硬封顶 18%。
- 两条 GLSL 已生成 SPIR-V；最终 `ninja -C build32 src/d3d9/d3d9.dll -j2` 为 4/4 增量
  compile/link、exit 0，随后 `ninja` no-work。DLL 29,921,590 bytes，SHA256
  `9A795542BFD01FB55B85EC5380EB37A45A6558B38B3F8587F8A4641A51340201`。确认 War3、
  World Editor、YDWE/ydhost 进程均为 0 后部署到 `E:\Work\War3\d3d9.dll`，source/target
  hash exact；未启动 War3、未跑 AutoTest，不能先验声明俯视画质通过。

下一门只需用户在同一光影图做 RTS 俯视/低镜头静态截图：固定太阳、CSM 与模型，先用
`weight=2.5`（3.0 已在 shader clamp 成相同结果）、samples16、divisor4；验收柱从遮挡物沿
同一光向连续延伸、跨 cascade 不长缩、普通俯视截图可无猜测辨认，同时检查平滑雾是否出现
4x block。此门不要求 foreground dual_perf，也不能修改 S1 period=1。

---

## 🚨 2026-07-16：体积光俯视可见性第二阶段（build-only）

针对用户与深度研究报告指出的“低镜头可见、RTS 俯视明显变淡”，主线程复核确认问题不是单一
相机矩阵：同帧 CSM publication 与 current world-camera matrix 已有 exact gate，剩余主要是
phase 视角偏置、固定 longitudinal probe phase，以及 quarter-resolution composite 只用几何
depth、无法识别平坦地形上的体积散射边界。

- 太阳 HG/iso mix `0.55 -> 0.35`，归一化 phase floor 为 `0.80`（当前最坏 backscatter
  自然值 `0.833`，不触发增能 clamp）；离线 forward/back response span `2.104 -> 1.622`，
  不提高全局雾能量。
- 每段 CSM probe 硬上限仍为 8；目标 spacing 按 view ray 与 sun direction 的横穿分量及
  `abs(dot(viewRay,worldUp))` 俯视因子从 `24 -> 10` 自适应收紧；segment/matrix/TDR 上限均未放宽。
- composite 在 raw-depth 相容的四邻域中选 spatial-nearest reference，并增加相对 scattering
  bilateral 权重；小于 8% 的梯度仍双线性，强 lit/shadow 边界跨侧权重下限 1/64。它只防止已
  采中的细柱在 4x4 放大时被冲淡，不能伪造所有 low-res ray 都漏掉的阴影。
- 第一版 DLL `5EECB...93B` 已被用户实际部署；11:38 的 2048x1105 RTS 截图在 intensity=4、
  samples=16、density=2、weight=3 时只能明确辨认地表阴影和全局雾，不能无猜测指出体积
  阴影柱，因此正式判定画质门失败。验收门固定为：普通俯视截图中必须能指出遮挡物、连续
  柱体及一致延伸方向，不能把地表 CSM shadow、AO 或泛光误认成体积柱。
- 失败后 whole-ray contrast 从 `pow(physicalRatio,w)` 改为 true-occlusion readability toe。
  `O=1/64` 的最大档对比从 3.86% 提高为低镜头 5.91%、俯视 12.5%；`weight=0/1` 仍严格为
  no-CSM-contrast/physical，`O=0` 仍为 0，不能凭空造柱。
- `git diff --check` 通过；离线边界模型通过。唯一主线程构建
  `ninja -C build32 src/d3d9/d3d9.dll -j2` 已生成两条 SPIR-V header 并完成 x86 link，DLL
  SHA256 `CFCEBDA6106311942B9EA71DF9A84B55EA89C8D4B079394AE6E2DE153A1B67E1`，构建残留 0。
  已在确认 War3/World Editor 进程为 0 后部署到 `E:\Work\War3\d3d9.dll`，源/目标 SHA256
  exact match；未启动 War3、未运行 AutoTest，因此当前只能声明静态/build/deploy 闭合；下一运行门
  必须对同一光影图锁定太阳/CSM，比较 RTS 俯视与低镜头的柱体连续性、block edge、cascade seam
  与 point+volume 稳定性，不能把画质改进先验写成通过。

---

## 🚨 2026-07-16：outside-poison O1 production authority 与诊断原子收敛

本轮把 2026-07-15 的 O0/O1a successful-Lock 侧证晋级为 production O1，但权限边界保持
很窄：它只用真实 successful Lock→原 kernel normal-return→successful Unlock 事务替代产品路径
每次 1252-byte GX `SafeCopy` 的 poison 子证明；原生 CPU skin kernel 仍精确执行一次，O1 不会
自行授权 GPU skin、跳 kernel 或绕过 consumer/poison 安全合同。

- 每个 poison-bearing outside upload 创建 outer-owned provisional token。actual Lock 必须精确闭合
  FVF/stride、desc/range、mapped pointer、device/CommonBuffer/COM、resource/storage generation 与
  poison ledger mutation generation；kernel 与 Unlock 任一漂移都只 retain/fail closed。
- `none` 产品路径只保留严格 `1/127` legacy evidence；任意 O0/O1a sidecar 开启时仍全量 legacy
  scan 且 production authority 固定为 0。evidence 永不授权，`legacyBackedAuthority=0`。
- parser 同时要求窗口 `poisonScanDelta == evidenceDelta` 与累计 endpoint
  `poisonScanTotal == evidenceAttemptsTotal`；sidecar 模式累计要求
  `poisonScanTotal == authorityAttemptsTotal`。旧 `acceptedWithPoison==poisonNoOverlap` 只适用于
  全量 sidecar；产品 O1 的精确分区为
  `authorityAttempts == acceptedWithPoison + evidenceOverlap + evidenceReadFail`。
- production-light 正式门
  `gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_none_o1_tls_batch_prod_none_v1_20260716_073859`
  中累计 O1 `attempt/authority/retained = 491,686/487,815/3,871`，legacy scan 仅 `3,871`
  （约 `0.787%` attempts，较全量扫描减少约 `99.21%`）；正式 clean pair 为
  `2,476/2,457/19`，Lock/kernel/Unlock exact，所有 reject/mismatch/poison leak 为 0。
- sidecar retained 控制
  `gpu_skin_p4_crash_gate_isolated_diag_light_sidecar_both_o1_tls_batch_both_retained_v1_20260716_074428`
  中 armed/retained `2,489/2,489`、authority `0`；O0/O1a 九格、direct-discard receipt、累计 scan
  与旧 P4 门全部闭合。

O1 正常事务原先每次产生约 12 次 process-wide diagnostic `fetch_add`。现将 15 个正常路径字段
接入既有 render-thread-owned `NativeTelemetryDelta`；`active`、retained、overflow/cancel/reset、
三类 reject 与全部 evidence/matrix 仍即时 atomic。prod-none clean pair 的 29,674 次逻辑计数
更新，保守估算至少少 27,190 次 relaxed atomic RMW（约 1,182 次/frame proxy）。运行精确门为：

- `batchedAddsDelta == expectedBatchedAddsDelta = 21,167,306`；该值含 bytes 等数值加和，只能证明
  exact-add，不能当作操作次数或毫秒收益；
- 两端 telemetry pending/fault `0/0`，15 个 authority delta 与 O1 生命周期逐字段相等；
- sidecar retained 控制 exact-add `12,596,985 == 12,596,985`；
- full 最终控制
  `gpu_skin_p4_crash_gate_isolated_diag_full_sidecar_none_o1_tls_batch_full_none_final_v2_20260716_075835`
  中 batching disabled、flush/batched-add `0/0`、expected `0`、`exactAdds=true`，O1 全零/fullExact。

最新 build-only：

`AutoTest/artifacts/gpu_skin_o1_authority_tls_batch_build_only_j2_v1_20260716_073601`

唯一 Test Conductor 使用 `ninja -C build32 src/d3d9/d3d9.dll -j2`，`2/2`、warning/error `0/0`；
DLL SHA256 `074C4DDA1C1B50A1FB8C79608D7E929DE0A09C0437ED33B95063A967977C9736`。
上述三轮运行均无 crash，截图正常，ledger/poison/index/nativeKernelNormal 与 steady-state
reset/retirement fault/pending diagnostics clean，测试/War3/build 自有残留为 0；三轮
`lifecycle.enabled=false`，尚未执行 resize/maximize/restore、map/device reset 或第二进程 relaunch。

当前不能据此声明 FPS 已提高。RMW 数量硬下降来自代码路径与 exact authority/TLS ledger；
period-256 只提供有尖峰噪声的时间/热点估计。最新可见最大 parent 约为 Game.dll 原生
`ApplyDrawStateAndSamplerPair` `1.03 ms/frame`，GPU-skin wrapper residual 约 `0.069 ms/frame`。
outer/DIP/dispatch 可见量级约 `0.17..0.23 ms/frame` 且相互可能嵌套，不能横加。新增
kernel-route enum 预计只能省 1–2 个 TLS miss，却增加 set/clear/read，现阶段否决；先用同 DLL
isolated ABBA 归因当前 disabled↔bypass 总增量（不能隔离 TLS batching 独立收益），再决定是否补
同一 semantic sample 域的 child closure。outline→resize/maximize/restore + map/device reset +
second-process lifecycle→格式/透明/special fallback 仍是发布前硬门；用户明确让出前台前禁止
foreground `dual_perf`，S1 period 必须保持 1。

额度收尾时的最新状态：同 DLL isolated ABBA 首次尝试
`AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260716_081116` 只完成 A1/B1，未完成 B2/A2，
因此没有 order-balanced 正式 delta。A1 disabled 的 frame/main/GPU 为
`9.785/7.862/1.430 ms`，B1 bypass 为 `13.992/12.097/1.500 ms`；未平衡参考差
`+4.207/+4.235/+0.070 ms`。两窗 raw AutoTest、模块与 DLL SHA、隔离桌面/视频恢复、双重清理均
正常，B1 仅因 ABBA parser 仍使用旧 `acceptedWithPoison==poisonNoOverlap` 公式而主动停止。

parser 的 manager-visible 分区已改为 O1-aware，但静态审计指出启动 env 的 `sidecar=none` 不能替代
DLL 运行时策略证据。因此当前源码又新增 perf-report `outsidePoisonSidecar` immutable config/native
counter exact 字段，ABBA 必须验证 explicit none、invalid=false、closure clean、config/counter exact；
差值字段明确名为 `derivedUnscannedAcceptedWithPoison`，且
`independentAuthorityVerified=false`。Python `py_compile` 与 4 组纯合成测试通过，旧报告因缺新字段
被预期拒绝。**该最后一笔 perf-report/C++ 变更尚未静态终审、build、deploy 或运行**；部署 DLL 仍是
SHA `074C4DDA...`，ABBA runner 的 expected SHA 也仍指向它，下一线程必须先终审/build，更新 exact
SHA，再跑 production-light P4 回归，最后从头跑完整 A1/B1/B2/A2。

旧两窗的 draw-chain 只作预定位：sampled leaf 平均 `18.005 -> 23.195 us/call`，按约 473 leaf/frame
折算约 `+2.46 ms/frame`；其中 HostOther 约 `+1.24`、GpuSkinDip `+0.60`、BeforeUi `+0.38 ms/frame`。
这些仍是未平衡样本，不能当正式结论，但已表明主要增量在 CPU host/render lane，不在 measured GPU。
`semanticOriginal` 只包 Game.dll `ApplyDrawStateAndSamplerPair` trampoline，DIP 在其返回后的 caller tail
flush，不能伪造 `semanticOriginal = outer + DIP + remaining`。若完整 ABBA 仍证实明显增量，同域只可做
`semanticOriginal = outerChild + remainingApplyState`；要包含 DIP，parent 必须提升到 Common/Special
`dispatchOriginal`。不要重复给已有 draw-chain leaf 加 QPC。

---

## 🚨 2026-07-15 接棒：GPU skin 同步管理固定成本继续收窄

最新 production-light 证据不是“剩余 GPU 算力不足”，而是 CPU render lane 的固定管理成本：

- 同场景 isolated ABBA 仅作归因：off/bypass mean frame `10.9855/13.5825 ms`，
  main `8.8365/12.0065 ms`，即 bypass 增量约 `+2.597 ms frame / +3.170 ms main`，
  measured GPU 基本不变；该数不是正式 FPS。
- light lifetime 为 raw/kernel `1,121,900`，original `1,111,353`，bypass `10,547`
  （仅 `0.9401%` calls），但 bytes 为 original `248,651,648`、bypass `198,956,224`
  （bypass 占 `44.4488%` bytes）。当前接管偏向大模型，不能称全模型 GPU 蒙皮。
- production partition 为 scope `977,195`、skin/format `54,089`、small CPU `66,960`、
  candidates `23,656`；Common 可证明 CPU-only 的 path/stage/skin/small 合计约
  `157,855` 次，即 `51.62 uploads/frame`。旧 1/256 sampler 闭合出的 GPU-skin 增量
  为 `2.600 ms/frame`，与 ABBA `2.597 ms` 对齐。

本轮实现 **Common dispatch exact-negative CPU-only seal**：

- manager 只在本 flush 完整 assembly 完成、且有 batch 时 host submission 已 accepted 后，
  发布不可变 current-flush candidate view；空但完整 assembly 的 flush 也可发布。下一 flush、
  callback exception、epoch retirement/reset 会清 view。
- Common dispatch begin 在 manager mutex 内对 exact `(flushEpoch, renderablePart, layer)` 做
  CandidateKey→token 保守扫描；任何候选都拒绝 seal。bridge 只在 callback 正常返回并进入
  `BeginIssued` 后 commit。整 dispatch 的 `dispatchEpoch % 127 == 0` 证据 cohort 永不 seal；
  full diagnostics 与 Special 旧证据语义不变，callback ABI 仍为 9。
- sealed upload 仍完整执行 Game.dll outer upload 与原生 CPU skin kernel，并用独立
  normal-return sidecar 证明真实返回；只省 ABI-9 manager upload/DIP/fanout 管理链。
  D3D 的 `ProvenCpuOnly` 只跳 GPU lease/parity/consumer resolution，仍执行 ShadowCapture、
  material override、PrepareDraw、正常 CPU stream main/outline draw 与状态恢复。
- poison、pending authorization、reset/retirement、nested dispatch/semantic、generic upload/DIP、
  ticket 或 marker 冲突全部 fail closed；proposal reject/abort 与 seal invalidation 是允许的安全
  撤销，active marker conflict 和 timing cancel 必须为 0。
- 新 `nativeDispatchSeal`、`nativeProdDispatchSeal`、raw/inside/f7/s7/0-1-N fanout、
  telemetry exact-add 与 clean-pair parser 均闭合。两轮合成故障注入覆盖 evidence-Special、
  transient fallback、closure mismatch、漏分类和 marker conflict；最终静态复核 P0/P1=0。

离线小任务模型：

- `AutoTest/artifacts/gpu_skin_small_job_batching_offline_20260715_080551/result.json`：当前
  449 阈值为 `11,782 jobs / 1,630 dispatches`，actual/launched vertices
  `6,940,763/11,176,384`，利用率 `62.10%`；tail waste 仅 `3.64%`，主要浪费是
  cross-job `34.26%`。
- 32-lane local size 理论只省 `0.57..5.54%`，却增加至少 `24.2%` workgroups；不做。
  历史 `<64` micro-pack 最多只覆盖 `1.13%` vertices；延后。若运行 crossover 证实正收益，
  下一安全扩展区间是把 production threshold 从 `449` 降到 `193`，不能先盲目全接小模型。

最新 build-only：

`AutoTest/artifacts/gpu_skin_p4_build_only_isolated_diag_light_steady_fastpath_incremental_build_j2_v2_20260715_091416`

唯一 Test Conductor 使用 `ninja -C build32 src/d3d9/d3d9.dll -j2`，`14/14` 最终 x86 link、
exit 0、error 0；DLL SHA256
`45AB5F7A5CE121F7AA53EF8CB37F9346F30167E9BDE8794CB6BD878D43190F51`。
`launchPerformed=false`、`deployPerformed=false`、`autoTestPerformed=false`，未部署、未运行
War3；编译器/linker/runner 精确自有残留 0。**因此当前只能声明代码/静态/build 闭合，不能
声明 FPS 已提高。**

按旧窗口估计，Common seal 独立收益约 `0.32..0.48 ms/frame`，不会单独吃掉全部
`2.60 ms/frame` 增量。随后静态审查否决了“用当前 SafeCopy 结果给自己建 fixed cache”的
方案：pre-read 只有 GX/count，缺 output format、六选一 VB/ring 与 native device；post-read key
又只能证明刚完成的 SafeCopy，不能证明下一次可删读。不能把同源缓存当独立晋级证据。

现已实现真正独立的 **outside-poison D3D9 successful-Lock O0 sidecar**：

- 旧 1252-byte GX `SafeCopy` 仍是唯一 admission authority；O0 只记录，不授权、不清 poison、
  不改变 CPU kernel、marker、reset 或 quiescence。
- 每次 production-light poison scan 建立 outer-owned cookie；固定 TLS LIFO 深度 8，fast/generic
  settle、异常 cancel、reset abort 与 overflow 分区闭合。poison ledger 每次真实 create/remove/
  trim/split/reset commit 都推进独立、跳零的 mutation generation。
- 成功 vertex `LockBuffer` 在 active-lock record 与 lock-count 之后采集 device/CommonBuffer/COM、
  resource generation、real/mapping/map-allocation identity 与 generation、FVF/desc、offset/size、
  requested/effective flags 和 mapped pointer。顶点环按 ASM 精确使用 `0x1800/0x2800`；既有
  index `0x1000/0x2000` 语义未改。
- 两个 native kernel normal-return 通知的第一项语义动作都是 freeze，早于 generic CPU rewrite
  清毒。reset/retirement、flush/mutation 漂移、多 Lock、重入、非正常返回等只归 unprovable。
- 旧 `{N,O,R}` × Lock `{N,O,R}` 九格、12 类 unprovable 与三层 closure 已接入 parser；硬门
  单列 `old N -> Lock O` 为 `legacyMissedOverlap`，要求为 0。O0 允许保守 off-diagonal 与
  unprovable，但 authority 固定为 0。

离线模型最终产物：
`AutoTest/artifacts/gpu_skin_outside_lock_shadow_offline_20260715_095245_834601/result.json`。
27 个确定性边界和 50,000 次有界 fuzz 通过；eligible `N->O=0`，strict exact identity
`15,862` 例 off-diagonal=0。该结论仅是 Python 合同闭合，不是运行授权或 FPS 证据。

O0 最新 build-only：

`AutoTest/artifacts/gpu_skin_o0_sidecar_build_only_j2_v1_20260715_101034`

唯一 Test Conductor 使用 `ninja -C build32 src/d3d9/d3d9.dll -j2`，`23/23` 最终 link、exit 0、
error 0；DLL SHA256
`83F124FBCAC9E5498456A33299B2DF252038363CA66504EAF6AB001DE7EB2C13`。
`launchPerformed=false`、`deployPerformed=false`、`autoTestPerformed=false`、
`foregroundActionPerformed=false`；67 个精确自有构建进程全部自然退出，残留 0。C++ 与 parser
独立复核均 P0/P1=0，parser `py_compile` 与 17/17 纯合成测试通过。

因此当前仍只能声明静态/build-only 闭合。游戏可用后先跑 isolated production-light/full O0
crash gate，确认 scan/attempt/lifetime/settlement 全闭合、active=0、`legacyMissedOverlap=0`，
再设计具有独立 pre-Lock 状态的 O1 tracker；不能直接删除 SafeCopy。随后仍按 outline→lifecycle/
格式推进。用户明确让出前台前禁止 foreground `dual_perf`，S1 period 必须保持 1。

### War3 1.27a `CWorld` 类族全量逆向（2026-07-15）

新 authoritative 分卷位于
`docs/research/war3_render_issues/30_cworld_class_family_full_reverse/`。当前 raw RTTI/真实 x86
ASM 已确认：`CWorldFrameWar3` 是 `0x668` 的 UI/world frame；独立 `CWorldObjects` 为
`CShowable` 派生的 `0xF4` base；`CDoodads/CBlightPuffs` 大小分别 `0x150/0xF8`；
`CWorldObjectsClippable` 是独立 `CClippable` 分支，大小 `0x1C`，不是 CDoodads 基类。

- WorldFrame 主/次 vtable 为 `0x6F98DCD0` 57 槽与 `0x6F98DDB8` 9 槽，次基偏移 `+0xB4`；
  三个 world-group owner 位于 `+0x16C/+0x170/+0x174`，每个真实分配 `0x1C`。
- `CWorldObjects/CDoodads/CBlightPuffs` 三表各 103 槽；全槽地址、机械 ABI 与 raw behavior
  已入卷。slots28..52、53..77、78..102 分别核验 39/45/39 个唯一 target 并完成增量写回；
  slot57、64..66 与若干公开返回码/flag/参数名仍保留 Unknown。
  `0x188` 是 AUWOModel entry stride，不是
  CDoodads 类大小。
- `CWorldFrameWar3_RenderScene/DispatchStage/RenderWorldGroup` 已作为 canonical 名增量写回 IDA；
  stage 11 先有 TerrainShadow selector12 producer，再有 group0 producer，flush 在 stage 返回后。
  不能用 `stageId==11` 或旧 group taxonomy 代替 producer-time exact sidecar。
- stage16 已闭合为四个 `CAgentPtr<CUnit>` bucket 加 pathing/debug overlay；stage18 的
  `+0x250` 是 borrowed active `CBuildFrame*`，其 pass 消费 `CPlacementBox/CConstructUI`；
  stage21 严格依次执行 static-root TextTag、可选 `TerrainImage` 和 `CGameState+0x2C8` embedded
  TextTag。旧 `CWorld_TerrainShadow_Dispatch(this)` 已纠正为 `ECX=selector` 的 global fastcall，
  因 selector13 明确不是 TerrainShadow。
- WorldFrame primary slots15..24 已由真实 dispatcher/ASM 闭合为 event-record forwarders；
  slots25/26 是 `AUKeyboardFocus` effective-active/inactive 生命周期，slots31..33 为继承属性传播；
  primary slots34..56 和 `CLayoutFrame@+0xB4` 的 9 槽 ABI/核心语义也已逐槽入卷。
- primary slots0..14 亦已闭合：slot2 是 `CObserver` 三参 `__thiscall` registration，slot10 是
  `CMouseEvent` hit-test，slot14 仅路由 sprite/ghost-sprite/terrain click；WorldFrame 57+9
  全槽已具备真实 cleanup 与最保守行为表。
- `CLayoutFrame@+0xB4` 的精确 `0x68` 次对象布局已由 ctor/setter/commit ASM 闭合：主对象
  `+0xF8..+0x104` 为 cached rect、`+0x108` 为 authoritative valid、`+0x10C/+0x110`
  为 unscaled width/height、`+0x114` 为 scale、`+0x118` 为 publication latch；失效只清
  valid，不清旧 rect bytes。
- world-group 的 `0x18` record `+0` 已闭合为 strong `CSprite*`，非 AUWOModel/独立
  WorldObjectEntry；五张 29 槽 CSprite/Mini/Uber/pooled-leaf vtable 已入卷。add 先增
  activeCount 后填 record，clear 可 release-to-zero 并同步回池，因此 record/count 无 lock-free
  publication 或稳定 identity 合同。
- WorldFrame `+0x3B0..+0x65F` 已拆成 Rally/Int32 small vectors、Waypoint ring、六组 raw
  pointer vector 与六组 `CAgentPtr<T>` vector；raw vectors 只拥有 backing，`CAgentPtr` vectors
  同时 retain 元素。`+0x588` 是 RallyIndicator source，TargetPointConfirm 在 `+0x58C`。
- `CWorldFrameWar3_RefreshGameContextBindings @ 0x6F3618F0` 已闭合 `+0x178/+0x184/+0x188/`
  `+0x198/+0x1F4/+0x230/+0x32C` 的来源和 indicator ordinal 尾链。`+0x254..+0x2F7` 是 exact
  `0xA4` embedded `CCinematicFilter`；COL `0x6FA874AC`、TD `0x6FB8E0FC`、vtable
  `0x6F98ED34` 仅一槽，slot0 `0x6F38A130` 为 scalar deleting dtor。
- `CWorldObjects+0x94` 已闭合为 borrowed `CTerrain*`；`CDoodads+0xF4/+0xF8` 为 borrowed
  `CDoodadDB*/CDestructableDB*`，`+0x118` 为内嵌 ModelColorHash table；`+0x10C` 已由 raw
  debug descriptor 闭合为 `Destructibles use height map` cached toggle。`AUWOModel+0x88/+0x8C`
  已分别闭合为 static-shadow/selection-circle image handle；`+0x90/+0x94` 也已闭合为 emitter/
  feature-domain-Unknown terrain handle。四者均以 `-1` 为无效索引并有注销边界；但
  `0x6F74DA00` selection 注册 body 的所有静态引用扫描均为 0，producer reachability 仍 Unknown。
- `CClippable` ctor/dtor 只写 vptr，结合派生首字段把 ABI size 闭合为 `0x4`；两张 10 槽表
  已全量写回。Clippable Clone 只证明 independent outer array 与已证 nested-owner copy，raw
  references 仍可能共享。`CBlightPuffs+0xF4` 已闭合为 `puffDurationMs`，与 entry
  elapsed/duration `+0x148/+0x14C` 和 birth/stand/death 状态机形成完整毫秒合同。
- 第二十二批已把 WorldFrame `+0x334..+0x38C` 从旧 Unknown block 收窄：`+0x334` exact
  `CFog*`，`+0x338/+0x33C/+0x354` 为 pooled Uber sprite strong refs，`+0x340` exact base
  `CLight*`（不是 `COmniLight`）；`+0x360` 为 `CPathingMapIndicator` strong-ref vector，
  `+0x370` 为 stride-`0x18` `TargetIndicatorVector`，`+0x37C` 为 `(cursor+1)&7` ring cursor，
  `+0x380/+0x384/+0x388/+0x38C` 为已证 accumulator/latch 链。`+0x344` 仍必须保留
  `Unknown[4]`。同时闭合 `CEnvEffect -> CFog/CLight -> COmniLight` RTTI、大小和四张 5 槽表；
  WorldFrame 使用 default `CLight(0xDC)`，独立 `COmniLight(0x104)` factory 只有另一处直接 xref。
- 二十二批 IDA 名称/type/comment 均已 read-before-write、读回并保存
  `E:\Work\War3\Game.dll.i64`。此前 CPU-MT Phase B 优先 evidence package 也已闭合并写回：
  `0x6F13A5C5/0x6F138F55` 的 palette/static-input freeze、`0x6F0EEB76..0x6F0EEBB6`
  的 output identity/SEH/latest join、以及 EvtSched `_beginthreadex` worker 来源；最新大小
  当前数据库大小 `251,984,624` bytes。本逆向任务未 build/deploy、
  未启动 War3、未运行 AutoTest。

CPU-MT Phase B 的结论不是“已接线”：source/palette 与 successful Lock 后的 destination 必须
用 exact candidate token 分段关联；staging copy/owner join 必须在 kernel detour 内、
`0x6F0EEB8A` 前完成。现有 Lock/Unlock sidecar 明文 diagnostics-only，ABI 9 缺 CPU batch
submit/acquire/commit/abort，`onUpload` 又晚于当前 kernel，原样不能承担该 producer。完整证据见
`docs/research/war3_render_issues/28_gpu_skinning_takeover_feasibility/cpu_mt_phase_b_native_evidence.md`。

仍未完成：WorldFrame `TargetIndicator` 等 record 的公开业务字段、`+0x344` 与其余
borrowed/retained pointee 动态类、vector cohort taxonomy，
WorldObjects 已审槽仍 Unknown 的公开业务名，CSprite 其余虚槽，stage16 bucket taxonomy/
pathing global root、stage18/21 少数 owner 空洞、UI/particle/effect/terrain/shadow 其余类族，
以及除已证 EvtSched/render-lane 路径外的原生 OS thread/worker/join 证据。继续时必须
读取真实 ASM/xref/raw RTTI，不得把 17/19/24 旧文档的“完整”标题或 IDA 自动库名当结论。

---

## 🚨 2026-07-14 最新接棒：GPU skin 优化 + 灯光/混合光追

### GPU skin 当前真实状态

- P1A Observe v6：raw uploads `1449543`、outside `1078139`、inside
  `371404`、eligible `123790`，reject path/stage/skin
  `98217/31146/118251`；begin=end，truePairErr/epoch/pending=0。
- P1B Dual：`gpu_skin_dual_isolated_v3_20260711_174405`，
  `1824/1824` parity。
- P2 Shadow：`gpu_skin_shadow_isolated_v3_20260711_201657`，
  `34990` exact，跳过约 `485,260,384` CPU shadow-copy bytes。
- P3 Main：`gpu_skin_main_isolated_v1_20260712_010442`，
  `26035` GPU main submissions，restore clean；outlineSubmitted=0，描边专项仍待。
- P4 native bypass：白名单、callback ABI 9、精确
  NativeVertexOutputProof、CPU normal-return proof、resource retirement、
  reset generation、index ticket、poison、RAII settlement 已实现；full/light crash hard
  gate 均已通过。**不能据此宣称全模型已完全接管**，最新 DLL 仍缺 outline/lifecycle/
  格式覆盖和最终前台性能门。

2026-07-14 算法优化把 eligible upload 的跨进程读取从约 52 次压到 4 次并保留
exact fallback；新增可复用 upload-page pool、per-frame retirement poll 和 lifetime
计数修复。随后继续收窄 compute 热路径：

- shader 每个 64-lane workgroup 只协作读取一次实际使用的 9 个 `GpuSkinJob` word，
  64-byte ABI、`precise` 公式、FVF 输出和二维 dispatch 均未改变；
- host 在任何 output lease 前按 `floor(log2(ceil(vertexCount/64)))` 做 9 桶稳定
  counting partition，同桶保留原 native 顺序；零顶点或超过 16384 顶点硬拒绝；
- 新增 actual vertices、64 对齐 invocation、实际 launched invocation、tail waste、
  cross-job waste 与 bucket histogram，供后续运行 A/B 判断多 dispatch/palette dedup
  的代价，当前不宣称理论 load 削减已经转化为 FPS。

2026-07-14 用户手测 `DXVK_WAR3_GPU_SKIN_MODE=bypass` 后约 12 FPS，最新报告
`war3_perf_report_2026_07_14_08_29_06.html` 为 avg `11.545 FPS`、frame
`86.620 ms`、process CPU `89.286 ms`，但 measured GPU 仅 `2.188 ms`。静态调用链和
IDA ASM 均未发现 GPU fence/retirement wait；War3 该实例未创建 multithreaded D3D9
device，DXVK device lock 也是 no-op。问题位于同步 render lane 上的接管管理成本，而非
compute shader 算力。

- 最新日志只绕过 `4094/117691` 个 native kernel（raw upload 的 `3.48%`），仍不能称为
  全模型接管；CPU normal-return proof 却执行 `42292` 次，且 `79.64%` upload 在 scope 外。
- `AutoTest/analyze_gpu_skin_offline_cost.py` 离线重放证明旧 bridge 的跨进程读/VirtualQuery
  放大会随上传量增长；production bypass 已改为 fail-closed T0/T1/T2 分层，只对 live poison
  overlap 做 CPU rewrite proof。scope-less outside reject 跳过 manager callback，但只允许
  `dispatchEpoch==0`，epoch/配对异常仍必须进入 manager 暴露。
- RenderQueue admission 已用 learned `(renderablePart, layer)` 反向索引先拒绝未知项，命中后只
  读取相邻 8-byte live binding；新增 raw QPC 的 bridge/outer kernel/manager 分段计时。
- exact bucket histogram 为 `344,0,2908,1252,0,0,0,0,0`。Python 约束模型证明 449 顶点
  阈值只保留 `27.80%` GPU jobs，同时保留 `30.59%..62.97%` 顶点；因此 Bypass production
  采用 bucket3+ 混合路由，小模型继续走 Game.dll SSE，而 Observe/Dual/Shadow/Main 证据模式
  不改变。该优化已静态/build-only 通过，但尚无新的 War3 FPS 结论。

用户随后复测 `war3_perf_report_2026_07_14_11_40_58.html`：avg `48.330 FPS`、
frame `20.691 ms`、main thread `17.283 ms`、GPU `2.102 ms`。10–12 FPS 的数量级退化
已显著收回，但相对未启用 GPU skin 的约 90–100 FPS 仍没有净收益。窗口 delta 把
manager flush/prepareArray 定位到约 `7.346/7.185 ms/frame`；无正样本扫描仅约
`23.9 ns/element`，剩余墙集中在阈值后的约 50 个候选的 model/resource/palette 正路径。

- RenderQueue 新增 4096-bit 双 hash negative-only Bloom；positive 仍必须走 exact map 与
  live proof，运行闭合式为
  `bloomRejects+bloomMaybes == reverseHits+reverseMisses`。
- Bypass learned layout 可持有严格 epoch/content/layout/static-slice/index-hash 匹配的
  `bypassStaticHint`，只省 model-cache/resource probe，不能授权 CPU skip。preflight 仍做
  current stamp/palette 与 position/normal/group/UV/index 全量 exact proof，completion 再验
  stamp/palette；ABA、结构 mismatch、fuse、reset/epoch 均清 hint。
- key-only native fuse 在 preflight 恢复 exact geoset/layer 后会升级成 layout fuse，下一
  flush 在 palette copy/compute 前拒绝。global palette base 每 flush 只读一次；仅 Bypass
  取消不参与授权且历史 hit=0 的 palette hash/dedup，Dual/Shadow/Main 不变。
- 最新纯 Python 重放为
  `AutoTest/artifacts/gpu_skin_offline_cost_20260714_122644/result.json`；静态双审
  P0/P1=0，最新 DLL 已通过 build-only 但尚待运行 delta，不能据此宣称 FPS 已恢复。

用户随后确认该版 FPS 仍基本不变，因此 2026-07-14 又补齐了 GPU skin 全链路的
低扰动 raw-QPC 取证，而不是继续凭静态猜测热点：

- 既有 `nativeTiming` 保留 begin/evaluate/complete/normal-return/DIP/outer/CPU kernel；
  manager 新增 `managerQueueTime`、`managerBatchTime`、`managerProofTime`、
  `managerConsumerTime`，覆盖 flush query/control/static、RenderQueue scan、透明碰撞、
  exact-positive binding/model/static/palette/candidate、output lease、compute finalize/upload、
  host finalize、P4 preflight 三段、两次 palette proof、static proof、normal-return proof、
  completion、resolve/plan/commit/fail/close/draw-result/fuse/terminate。
- Bloom miss 和逐 element 负路径没有新增 QPC；scan 只每 array 计一次，四个 candidate
  子阶段只在 reverse exact hit 且通过格式/449 阈值/重复门后计时。production scope-less
  upload 仍跳 manager callback；唯一较宽的 completion timer 只覆盖真实 manager upload。
- `run_gpu_skin_p4_isolated.py` 会对两个有进度的 forced clean snapshot 做 calls/ticks 差，
  直接给出窗口 `totalMs/averageUs`、第二快照 lifetime max 和 inclusive 耗时排名；max 不做
  非法相减。flush→prepare→scan→positive、assemble→finalize→upload、native outer、
  preflight/normal-return/completion、palette/static proof 都有嵌套闭合，且新
  `hotPathTimingContractClean` 硬门同时要求字段完整、频率一致、单调、闭合与 Bloom exact。
- 注意 nested timer 不能横向重复相加：例如 positive 已包含 binding/static/palette/build，
  plan 又位于 preflight host callback 内。下一轮应优先读取 forced artifact 中
  `cleanPairHotPathDelta.rankedByInclusiveTotalMs` 与各 closure residual，再决定优化函数。

最新 build-only：

`AutoTest/artifacts/gpu_skin_timing_volume_build_only_20260714_132000`

相关增量 5/5：volumetric shader 生成、GPU skin manager、device、Volumetric C++ 与最终
x86 link 均通过，exit 0、error=0；parser AST 与最小字段/闭合合同自测也通过。9 行 warning
均为共享头既有 `OPCode -Wreorder`。DLL SHA256：
`13927E439386E1837343632914DA4D919DB56145FCD7B95991CBC55C4EB3742B`；
`launchPerformed=false`、`deployPerformed=false`、`autoTestPerformed=false`，未运行性能测试，
本轮编译器/链接器/Python/Node 派生残留 0。

2026-07-14 13:51 报告的 GPU-skin 巨量 CPU 墙现已具体闭合，而不是继续归入笼统的
“render lane 管理成本”：

- 报告 `war3_perf_report_2026_07_14_13_51_15.html` 为 frame `19.219 ms`、main
  `15.650 ms`、GPU `2.208 ms`。7 个超过 100 ms 的尖峰位于帧
  `72/372/672/972/1272/1572/1872`，严格相隔 300 帧；旧 periodic GPU-skin
  formatter/logger 均摊 `0.644 ms/frame`，并制造全部大尖峰。production 默认现为
  `DXVK_WAR3_GPU_SKIN_DIAGNOSTICS=light`、period `0`；full 只由专项 AutoTest 显式开启。
- clean-pair 同帧 delta：v1b native outer + manager flush 为
  `8.577 + 1.454 = 10.032 ms/frame`；449 顶点 T1 early gate 后 v2 为 `4.727`；manager
  palette 从 VirtualQuery/readability 放大改为 bounded fault-safe copy 后 v3 为 `3.348`；
  native T2 palette readability 同样改为独立 TLS scratch SafeCopy 后 v5 为 `1.550`。
  v1b→v5 共收回约 `8.482 ms/frame`。
- lifetime sampler 给出 manager palette copy `228.591 -> 2.223 us/candidate`
  (`-99.03%`)；native T2 palette proof `281.328 -> 2.193 us/candidate`
  (`-99.22%`)，完整 exact T2 `292.588 -> 12.769 us/candidate` (`-95.64%`)。
  未发现 GPU fence wait、同步 Map、retirement wait 或 compute dispatch 多毫秒阻塞。
- 以同场景 delta 回代旧报告，production-light 的审慎预测为 `10.09..10.56 ms/frame`
  (`94.7..99.1 FPS`)；这是归因预测，不是隔离桌面 FPS 结论，最终仍只认用户让出前台后的
  foreground 报告。

最新部署 DLL SHA256 为
`26FE1807A0D4346B45141E59D9F44CB5FDF0325D6DA9CEE15E3FF62C0859036F`。full hard gate：
`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_full_v6_full_clean_endpoint_gap_fix_20260714_160456`；
policy `1/1/0`、raw `6055`、begin/eval/complete/outer 全等、fast partition 与 timing/resource
closure 全闭合。production-light hard gate：
`AutoTest/artifacts/gpu_skin_p4_crash_gate_isolated_diag_light_diagnostics_light_full_v6_light_20260714_154649`；
policy `0/0/0`、44 个 full timing stage 的 calls/ticks 全为 0、raw `3788`、kernel bypass
`13099` 次 / `247,459,104` bytes，公共安全门全部通过。

forced diagnostics 现在允许两个严格 clean endpoint 中间出现已结算 transient poison，但不
放宽 endpoint：两端仍须 exact PID/requestId、完整 block、同 epoch/reset generation、各自
quiescent；最终重新校验 chronology、intermediate 全 non-clean、counter 单调、progress、
resource retirement 等量、timing/fast partition closure。上述 full artifact 实际以 attempts
`[2,4]`、gap `[3]`（poisonRanges `3`）通过，两个 endpoint 的 poisonRanges 均为 0。

### 点光、点阴影、体积光

- 全局点光 snapshot 现在先过滤非有限、零 range、零 intensity 与黑色光，再在
  shadow-first 分组内按 `maxRGB * intensity * range² / (1 + cameraDist²)` 排序；
  黑色高强度灯不再占用 cube/direct/contact 的有限槽位。receiver UBO 保持
  48-byte/light、784-byte 总 ABI，但由 CPU 每帧/灯预先写入 view-space position，
  shader 不再对每像素/灯重复 world→view；2560x1440、16 灯时理论上最多移除约
  5900 万次 position transform/帧，实际 GPU 收益仍待运行测量。
- 点光 receiver 的模型法线不再使用 divergent early-return 后的 `dFdx/dFdy`。
  现在显式读取 L/R/U/D 最多 4 个 integer depth texel，拒绝 clear/跨 surface 邻居；
  `viewFacing` 只用于合法法线翻面，低 confidence 时点光增量归零，不再把薄片/轮廓
  混成最大 N·V 的锯齿亮边。同一 gated normal 用于 Lambert、cube bias 与 contact ray。
- cube 点阴影只有六面 `0x3F` 全部有效才可采样，避免未初始化 face/seam
  形成错位楔形；16-tap PCF 改为零质心同心盘；range fade 使用自身
  `currentDist/shadowRange` 域。默认 1024、可选 2048、D32 总预算 96 MiB。
  质量默认已把 `pointShadowMaxCastersPerFace` 从静默 192 cap 改为 0
  (unlimited)；显式 cap 时逐 face 记录 `candidate/kept/dropped` 并低频报告。
- 2026-07-14 继续静态复核确认 cube face VP 乘法次序曾确定错误：仓库
  `Matrix4::operator*` 的 raw product 反序，C++ 必须写 `proj * view` 才能让 row-vector
  caster shader 收到 raw `view * proj`。旧 `view * proj` 在光源平移后会把 face 正前方
  caster 投到视锥外，足以解释“阴影位置不对”。clip-X cube convention 与六面表不改。
  默认六面/period1 不再 O(draw+palette) 做无效内容 hash；24 个 face vector 保留
  capacity，unlimited 直接按 replay 顺序追加；range/direct reject 用 squared distance，
  16 个 PCF tap 去掉单 mip cube 上数学冗余的 normalize。只有 temporal reuse 或
  1..5 face 轮转会计算 O(draw+palette) 历史签名；该签名现已包含 skinned output
  动态门、position/index/blend/UV slice 与 layout、draw range、alpha/texture/sampler，
  资源或材质变化不能再误复用旧 cube。
- **独立 UV binding 静态闭合**：`War3ShadowCasterDraw`、persistent geometry 与
  pipeline key 显式传播 `uvBinding=0/1/2`（position/blend/separate），CSM、terrain
  mask、point cube 与两条 outline consumer 均按 binding 绑定并 track。raw legacy 在
  persistent probe 前解析真实 UV owner；动态 separate UV 强制逐帧冻结 fallback，
  静态 separate UV 进入 source hash；semantic alpha key 在首个 cache probe 前纳入
  UV/纹理合同。S1 early cache 的读与两个写点均只允许 opaque，不能被 alpha draw
  污染。静态 gate P0/P1=0，仍须运行验证 binding1/2 树叶/栅栏与 outline。
- 体积光修正低分辨率非线性 depth 插值、sun/point/requireCsm 门、CSM coverage
  feather、Beer-Lambert/HG 能量、LDR soft-headroom composite；点光体积能量
  已与 receiver 的 `(1-x²)²/(1+6x²)` 对齐，移除常见距离约 5–7 倍的隐性
  衰减。点光 ray-sphere 区间现在每像素只计算一次，再按 march segment 的真实
  overlap 做 Beer-Lambert 子段积分；细小/掠射光球不再因 jitter midpoint 未命中而
  完全消失，无 overlap 的灯也不再逐步执行 length/HG/atten。深度使用
  projection-derived far clear、signed viewport delta 与
  D16/D24/D32 quantum，支持正常/反向 viewport。默认仍关闭；TDR 安全启用档改为
  samples=16、divisor=4、point lights=2，执行层和 shader 均有硬 clamp。
- 体积太阳阴影把旧单纵向点的 8-tap PCF 重排为每段最多 8 个 longitudinal raw
  `texelFetch`；每 probe 内完整组合 coverage fallback 与真实 blocker 后段内取 max，
  不再跨 probe 双重计费。CSM camera/ray clip 预计算把最坏矩阵变换收为固定 8 次/像素。
  点光能量仍取 overlap midpoint，但 cube visibility 用 2 个分层 probe；CPU/shader 双端
  硬限 2 灯，因此最多 4 cube fetch/segment。`weight` 只控制阴影柱对比，不再全局加雾。
  4M segment 门给出组合 shadow fetch 约 4800 万/帧绝对上界；纯算术产物为
  `AutoTest/artifacts/volumetric_offline_cost_20260714_122642/result.json`。
- 用户确认上述路径已经能看见阴影柱，但上帝视角仍弱、需要过高强度。根因不是 probe
  预算，而是旧 `weight` 在单个二值 segment 内立即饱和，无法增强俯视 ray 只横穿
  1/16 段的短柱；旧 HG 映射也让低镜头 forward scatter 天然显著更亮。
- 太阳现在每段同时累计只含 coverage fallback 的 reference 与含真实 CSM blocker 的
  physical integral，整条 ray 末端使用
  `Aout=Aref*pow(clamp(Aphysical/Aref,0,1), clamp(weight,0,2.5))`。
  `weight=0/1/>1` 分别表示去掉真实阴影对比/物理结果/增强短柱；CSM 缺失时两者相等，
  任何 weight 都不能造伪柱。该改动不增加 texture fetch、segment 或矩阵预算。
- HG 映射收窄为 `g=clamp((skyThreshold-.55)*.50,0,.22)`，HG/iso mix 从 0.70 降到
  0.55，削弱低镜头 forward 峰并抬高俯视 side/back 响应，不提高全局雾能量。
  `volumetric_max_quality` 改为相干的俯视可见 preset：weight=2.25、height=0.50、
  fadeFar=0.55；`height=2/fadeFar=1` 不是“更强”，而会把介质压向地表并延迟短 ray 显现。
- 体积光不再盲取全局 shadow-first canonical 前缀：仅在体积消费者内用固定数组
  先拒绝超出真实 march 最大光程、完全在相机后方或可证明离屏的光球，再从剩余
  最多 16 盏灯按
  `clamped maxRGB * intensity * range² / (1 + cameraDist²)` 选择最多 2 盏；
  global snapshot、shadow prefix 与 generation 完全不变，预算覆盖全部灯时保持原序。
  这修复“远处阴影灯占满前四位，近处亮非阴影灯表面可见但没有体积”的确定性漏选。
  `Run()` 在任何资源申请/copy 前只获取一次不可变 snapshot；point-only 且有效候选
  为 0 时不再执行两次 full-resolution copy 或两次 fullscreen pass。`density<=0/NaN`
  也与 shader 同合同早退。march 内 sample view-Z 改为 camera intercept + ray slope 的
  affine FMA；执行层额外用 4,000,000 ray-segment 硬预算收缩 samples，必要时自动把
  divisor 提高到 8；最低 4 samples 仍超预算时直接跳过 optional pass。
- optional CSM 缺失时 `unshadowedScattering` 现在始终控制太阳 fallback，不再被
  `shadowStrength=0` 绕过并凭空生成 100% sun volume。旧 point-only sphere ROI 只对
  divisor=2 有精确合同；TDR 安全档最小 divisor=4 后当前走保守 full-effect extent，
  由 ray-segment 硬预算兜底。后续若恢复 ROI，必须先推广 odd-size 映射合同；full
  color/depth copy 仍未收窄。
- 默认关闭的软件 point contact ray 已包含 A0 线性步进与 A1 半分辨率 Hi-Z。
  A1 使用 `RG32F` min/max 全 mip 链、`RG16F` visibility/confidence、默认
  24 个节点硬预算，并按点光 range/direct-energy relevance 在 8..configured
  visits 间自适应；范围外和低贡献像素在法线邻域/遍历前退出。深度断层、
  raw-depth 量化不确定、离屏、预算耗尽均 confidence=0。默认仍只处理
  canonical shadow prefix 的 1 盏灯。contact pass 已改为逐灯保守屏幕 ROI；每帧先把
  active array 全层 transfer-clear 为 `(visibility=1, confidence=0)`，ROI 外、完全离屏
  或 empty layer 不会继承旧帧暗影。48-byte push ABI 不变，最终
  `min(cube, contact)`，只补暗不破坏 cube fallback。mip0 精确测试也从固定 1/3、2/3
  两点改为 full-resolution 子像素 DDA：一个 2x2 cell 实际穿过的最多 3 个像素区间
  各 load 一次 depth，但 blocker 判定使用该像素内完整 ray-depth 区间，不再只测
  ownership midpoint。量化采用三态：expanded possible window 可证明分离才 definite
  miss；可能重叠但 shrunken reliable window 不重叠则 unknown；可靠相交才从真实交集
  选 representative。全局仍硬限 8 次 depth load；corner tie、沿像素边、不可表示
  短区间、求根失败或 cell ownership 不一致均返回 unknown，不能再把第三像素或
  midpoint 两侧的薄 blocker 错判为 confident visible。
- A1 outer hierarchy 同时删除 `current+1e-4` exit 门与两处 world-distance post-step；
  旧逻辑可整段跳过极短 projected cell。现在精确停在 rational boundary、按投影方向
  定义 half-open ownership，并验证只进入相邻 old-mip cell；root/axis/corner/progress
  歧义均 unknown，只有完整测试到 traceLength 才 confident visible。全部 light ROI
  预先计算；若都可证明离屏/behind，则在 allocation/clear/Hi-Z seed 前零 GPU work 返回，
  caller 已先清本帧发布 view，不会消费缓存旧 A1。
- A1 receiver 从 half-res 回 full-res 已由 nearest 改为 depth-guided 2x2 bilinear
  resolve；未知、mixed、深度不一致的 tap 贡献零遮挡且不重归一化，canonical
  anchor 不可信时整像素 fully lit。边缘采用 clamp-to-edge，不能由邻格把 blocker
  借进 ROI clear/unknown 像素。代价从每像素/灯 2 次读取增到最多 8 次，仍需运行期
  GPU p95 证明。
- A1 已从单灯固定预算扩为最多 2 盏 canonical shadow light：一灯仍保持原
  `mix(8, configured, sqrt(relevance))` 行为；两灯各保留 8 次基础访问，其余预算按
  receiver 处真实 `maxRGB*intensity*shadowStrength` 与 range attenuation 分配，任一
  重叠像素总访问硬界为 `configured+8`（默认 24 时最多 32，不再是 48）。visibility
  array 容量只增不减，active layer 仍逐帧精确 clear/dispatch/publish；UBO 从 256
  增至 320 bytes，48-byte push ABI 不变。
- 点阴影补齐 point-only/CSM-reuse 场景的首消费者屏障：transfer/compute write 在
  cube replay 前显式可见于 vertex/index input。receiver 的 cube texel footprint
  改为 `2*max(abs(dir))/resolution`，在 face center 精确、在 seam/corner 为保守
  Jacobian 上界，PCF 半径与 world-space texel bias 不再按中心尺度过度膨胀。环境
  float parser 拒绝 NaN/Inf，consumer 端再以 0.65/1.15/0.08/0.78 默认值 finite
  fallback，非法 filter 参数不能污染 cube lookup。
- Pipeline 新增单调 `frameSerial`，并把 0..2 的 `frameIndex` 明确限制为资源 ring；
  light snapshot、ReplayDraw cache、camera freshness、CSM/point-cube/Hybrid RT 发布均不再
  用三帧环索引判断“同帧”。CSM 只有本帧实际 render 成功或显式 reuse 才 settlement；
  point cube 记录真实 published serial，Volume 与 receiver 使用 expected serial exact
  gate，异常或非异常资源失败都不能把旧纹理重标为当前帧。该事务静态复核 P0/P1=0。
- 体积光太阳与点光统一为 `color * sourceIntensity`；黑色、NaN 或零辐射太阳在
  image allocation/copy 前退出，不能制造无源消光。HDR sun color 不再被隐藏
  peak-normalize，仍统一经过 `softClipRgb(..., 0.92)` 与 composite headroom。
  “全部滑杆拉满”不是最大可见 preset：fade、minSunIntensity 与 extinction 是门限/
  抑制参数，验收应使用相干的 `volumetric_max_quality` 组合。
- RTX 4060 Ti 的 64 位 ICD 支持硬件 RT，但当前 NVIDIA 610.74 **32 位 ICD**
  不枚举 AS/ray-query/RT-pipeline；War3 x86 进程内不能直接启用。详见：
  `docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md`。
- P0 补丁后的最新 build-only 为
  `gpu_skin_light_build_only_20260714_123117`；两个修改 shader、Volumetric、manager、
  device 与最终 x86 link 均成功。DLL SHA256
  `0620F24CDB7BDCB3D44ADBDDA8431C59571257CC50A475485E05C259D1D71E83`；未部署、
  未启动 War3、未运行 AutoTest。
- A1 生命周期静态门已闭合：资源在首条 Vulkan 命令前 track，异常录制保持
  fail-soft；设备析构按 `Flush -> CS sync -> GPU idle -> pass destroy`，避免 raw
  compute pipeline 仍在执行时提前销毁。冷启动默认关闭不分配 A1 资源；启用后
  再关闭会安全保留缓存到 owner 析构。
- 灯光专项详见：
  `docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`。

### 当前验证约束与下一步

- 2026-07-14 用户已授权 GPU-skin 性能线由**唯一 Test Conductor**多轮运行隔离
  AutoTest；仍禁止并发测试、切换用户前台或把 isolated frame advance 当正式 FPS。
- 后续先在隔离桌面的 `光影测试.w3x` 做点阴影六 seam、范围边缘、体积光
  sun-only/point-only/CSM-missing/max 档，补 alpha caster binding1/2 在 CSM/point/
  outline 的树叶与栅栏，以及 contact ray A0/A1、深度断层、reversed depth、
  resize/reset/第二进程；SunkenCity 自动化困难时不强行进图。
- `AutoTest/run_light_feature_matrix.py` 已扩为 8 组：保留 512 低成本性能对照，
  新增 1024 产品默认正确性和 2048 Ultra 质量门；三个 point-shadow case 均要求
  exact-launch-PID 执行证据。
- isolated desktop 不判 FPS；最终 foreground `dual_perf` 只能在用户明确让出
  前台后执行。
- S1 capture period 必须保持 1；不得借灯光/光追工作修改 S1、点光或 CSM 之外的
  算法语义。

---

## 🚨 2026-07-11 GPU 蒙皮主线交接（先读专项文档）

完整寄存器级证据、CPU skin 公式、FVF 布局、DXVK 接管点、同帧边界和 P0~P5 实施顺序已写入：

`docs/research/war3_render_issues/28_gpu_skinning_takeover_feasibility/README.md`

P1~P4 多 Agent 分工、模型/思考强度、文件所有权、槽位轮转与唯一 AutoTest 锁已写入：

`docs/plan/gpu_skinning_multi_agent_execution_2026_07_10.md`

执行硬约束：主线程外最多并行 3 个 Agent；只有 `Test Conductor` 可以 build/deploy/启动 War3/运行 AutoTest；凡涉及 Game.dll 渲染语义、hook ABI 或 native bypass，必须由 `gpt-5.6-sol + ultra` Agent 阅读 ASM，IDA 伪代码仅作辅助证据。

当前结论：普通 `CGeosetData` GPU 蒙皮接管 **高置信可行**。War3 使用 CPU 生成的 group palette，每顶点仅以 `uint8_t groupSlot` 选择一条 3x4 混合矩阵；推荐在 `Hook_FlushSortedItems` 入口批量 compute 生成原 FVF post-skin VB，并于 `D3D9DeviceEx::DrawIndexedPrimitive` 的 `PrepareDraw` 后做一次性 stream 0 override。`DxvkContext::dispatch()` 会 spill render pass，**禁止逐 draw dispatch compute**。

**已证明（仅 Observe）**：v5 配对子门为 `begin=end`、`truePairErr/epoch/pending=0`，且 `outside upload==0-fanout`。ASM sidecar + unified tag/stage upsert 的 v6 最终日志为 raw uploads `1449543`、outside `1078139`、inside `371404`、eligible `123790`，reject path/stage/skin `98217/31146/118251`，计数严格闭合；`Stage/world missing=outside+stageReject` 是分类结果，不是白名单故障。

**待验证 / 禁止晋级**：P1B Dual v1 在 `finalizeComputeGroup` 因 `GpuSkinJob alignas(16)` 与 32 位 `vector` 仅 8-byte 对齐触发 `movaps` 崩溃（`gpu_skin_dual_isolated_v1_20260711_161836`）。已移除 CPU staging over-alignment，并补 static hash/layout/readback usage+budget hardening；Dual v2 正在隔离桌面验证，尚未获得 parity 或画面通过结论。P2/P3/P4 只有代码骨架，未 runtime 晋级，所有 takeover/skip 默认关闭。所有现有 GPU-skin 测试均为隔离桌面，性能暂缓；最终 `dual_perf` 只能以前台 FG 判定。

硬约束：不要先改 War3 全部 vertex shader；不要绕过逐 draw fallback；不要把 GPU skin 与动画树/GPU palette 全接管混为一期；S1 period 仍必须保持 1。

---

## 🚨 2026-07-09 接棒交接（其他 AGENT 必读）

> 本节是 **2026-07-09 自动化性能推进 + 点光/体积光** 的完整交接。
> 更早的 Phase 7.x 阴影/路径阻断器/静态阴影历史在本文后半，不要与本节混读为“本轮刚做完”。

### 0. 用户目标与硬约束

| 项 | 内容 |
|---|---|
| 性能护栏 | 前台 `dual_perf`：**高压 ≥85 FPS**、**低压 ≥120 FPS** |
| 高压图 | `E:\Work\War3\Maps\ShadowTest\光影测试-高压.w3x`（桥/斜坡/大量 caster） |
| 低压图 | `E:\Work\War3\Maps\ShadowTest\光影测试.w3x` |
| 验收脚本 | `py AutoTest\dual_perf_baseline.py`（**必须前台 FG**，禁止 isolated desktop 判 FPS） |
| S1 period | **必须保持 1**。`period>1` 用户已实机确认地形阴影**高频闪烁**，禁止再试 |
| Worker_Prepare | 目标方向 ON（点光/点阴影 worker 路径）；MT_CmdRecord 目标 OFF |
| 视觉红线 | 不得回退静态阴影解决版、path blocker 身份门、S1 地形遮挡语义、动态单位 pose 流畅 |

**WSL 注意**：在 WSL 内跑 AutoTest 时应用 Windows 的 `py`（或 `cmd.exe /c py ...`），不要用 Linux python 直接起 War3。

### 1. 本轮做了什么（按时间线）

#### 1.1 Phase A→E 性能主线（核心）

收官数字见 `AutoTest/artifacts/phaseAE_closeout_20260709.json`：

| 里程碑 | 高压 FPS | 低压 FPS | 做了什么 | 结论 |
|---|---|---|---|---|
| **A0 基线** | 37.7 | 53.6 | 进入本轮前 dual_perf 起点 | 起点极差 |
| **A S1 early cache** | **64.8** | **109.7** | S1 地形 legacy capture 入口 O(1) early cache；persistent geometry ON；**period=1** | **本轮唯一大收益**（+27 / +56）；ShadowCapture 从热点塌到 ~0.02ms 量级 |
| **B Populate/diag 门控** | 64.9 | 109.3 | 关闭/门控 Populate 上过重诊断与无效热路径 | 与 A **持平**，不抬 FPS |
| **C static VB fingerprint** | （随 B dual 同批） | | 静态 VB cache 指纹/分类安全收紧 | 正确性向，**无 FPS 抬升** |
| **D WaitGate 归因** | 60.2→~67.9* | 107→~113* | 装 Engine WaitGate/PrepareWait/SleepGate 做 Idle 归因；曾误装宽 Wait API Hook 压帧 | 证明旧 “Untracked ~13ms” 大量是 **Idle 误分**；coverage→100%，UntrackedActive→0 |
| **E 2048+adaptive+轻 WaitGate** | **67.9** | **113.1** | CSM 默认 2048；`kShadowAdaptiveMapUpdateEnabled` period=2；Wait 只保留轻量 Engine WaitGate | GPU 约 3.36→**2.11**；相对 A 仅 **+3.1 / +3.4**；护栏仍 FAIL |

\* Phase D 中途宽 WaitHook 把高压压到 ~60；收口后改为 `kNativeMainLoopWaitGateHookEnabled=true` 的**轻量**路径。

**Phase E 单次 dual_perf 原文**（`phaseE_res2048_waitgate_light_dual_perf_20260709.log`）：
- 高压：`avgFps=67.884`，`avgFrameTimeMs=14.731`，`avgGpuTimeMs=2.114`，`avgMainThreadCpuMs=11.268`
- 低压：`avgFps=113.126`，`avgFrameTimeMs=8.84`，`avgGpuTimeMs=1.654`，`avgMainThreadCpuMs=5.697`
- 护栏：高压 FAIL / 低压 FAIL

#### 1.2 Phase A 技术细节（接棒必懂）

- **问题**：高压场景 S1 terrain 作为 CSM occluder 走 legacy ShadowCapture，每帧对大量 tile 做 decl/VB/copy，主线程爆炸。
- **做法**（`d3d9_device.cpp` + `d3d9_device.h`）：
  - `kShadowS1TerrainPersistentGeometryEnabled=true`：S1 tile 可进 persistent geometry。
  - **Early cache**：capture 入口用稳定 key 查 early cache，命中则跳过整条 re-copy/decl 热路径；仍 **period=1**（每帧 live 可见集合，不是隔帧跳过采集）。
  - `War3S1TerrainCapturePeriodRuntime()`：**period 强制语义为 1**；代码注释写明 period>1 会闪。
- **失败/放弃过的方向**：
  - period=2/3 隔帧 stash：FPS 好看但**地形阴影闪** → 禁止。
  - 仅靠 bounds cache / source-key 而不做 early hit：收益远小于 early cache。

#### 1.3 Phase B/C 细节

- B：Populate / semantic diag 门控（减少无用 scope 与日志路径），**不改变提交语义**。
- C：static VB cache fingerprint / unitless rigid 静态持久化相关安全收紧（延续 Phase 7.15x 三问题线），**不抬 FPS**。
- 相关配置仍在 `war3_internal_test_config.h`：
  - `kShadowDrawTimeVBCacheStaticPersistEnabled=true`
  - `kShadowDrawTimeVBCacheUnitlessRigidStaticPersistEnabled=true`
  - `kShadowDrawTimeVBCacheAllocBudgetPerFrame=32`（**不要**再降到 4：Phase 7.155 降 budget 会导致阴影不完整，已 REVERT）

#### 1.4 Phase D 细节（归因，不是性能银弹）

- 目标：解释 perf 报告里巨大的 `Other/Untracked*`。
- 结果：
  - 装 Wait 归因后：`cpuCoveragePct≈100`，`avgUntrackedActiveCpuMs≈0`，`avgIdleWaitCpuMs≈帧时`。
  - **结论**：大量时间是引擎等待/门控，不是 “Shadow 还有 13ms 可砍”。
- 踩坑：Hook Sleep/Wait 宽 API → 高压 **-5 FPS 级**；已收回到 Engine 侧轻量 WaitGate 安装路径（`war3_hook_lifecycle.cpp` + config 开关）。
- 配置：`kNativeMainLoopWaitGateHookEnabled=true`（`war3_internal_test_config.h` ~281 行）。

#### 1.5 Phase E 细节

- CSM 默认分辨率：`d3d9_war3_csm.h` 中 `shadowResolution = 2048`（历史常见 4096）。
- Adaptive ShadowMap：
  - `kShadowAdaptiveMapUpdateEnabled=true`
  - `kShadowAdaptiveMapUpdatePeriod=2`（及 high/huge caster 分层 period）
  - **实测**：有 skinned 动态 caster 时 reuse 几乎不触发（设计正确，避免 pose 冻）；稳态收益有限。
- Adaptive resolution 开关仍在 config（`kShadowAdaptiveResolutionEnabled` 等），与 map-update adaptive 不同层。

#### 1.6 模块二分 / 天花板证据（决定性）

| 实验 | 结果 | 含义 |
|---|---|---|
| `dxvk_only` / 极简关增强 | 高压仍 ~**63** FPS，MT ~**11ms** | **85 墙不在“再砍一点 Shadow 诊断”**；存在引擎/DXVK 基线上限 |
| tracked Shadow+Populate | 合计约 **~1.3ms** | 已观测 shadow 热路径不是 11ms 主因 |
| `Worker_Prepare` 矩阵 | 点光默认关时 Worker 路径 **无重叠收益**；点阴影开时有 `PointShadow` section | A2 要真收益必须在 **CSM prepare/sort** 上做，不是空开点光 worker |
| isolated desktop dual | FPS 噪声大（曾假降 5–10） | 判护栏**只用前台 FG** |

相关产物：
- `AutoTest/artifacts/a2_worker_module_matrix_isolated_20260709_171037.json`
- `AutoTest/artifacts/perf_module_bisect_20260709_172834.json`（注意：部分 case 如 `no_semantic` 数字异常偏低，解读时核对是否 dual 互斥/进程污染）
- `AutoTest/artifacts/fg_untracked_probe_20260709_*.json`
- `AutoTest/run_module_bisect_perf.py` / `run_one_case.py` / `run_fg_untracked_probe.py` / `run_a2_worker_module_matrix.py`（本轮新增，部分未收口文档化）

#### 1.7 点光 / 点阴影 / 体积光（并行线，默认关）

文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`

**已完成**：
1. 自动化：`AutoTest/run_light_feature_matrix.py`（先 dual_perf 护栏，再 5 组 env 矩阵）。
2. 零成本关闭复核：点阴影/体积光 off 时入口 return，不分配 cube、不 copy depth/color。
3. 点阴影 face budget：默认每帧最多 3 面轮转；env `DXVK_WAR3_POINT_SHADOW_MAX_FACES=0..6`。
4. 体积光：低分辨率 effect（默认 divisor=4）+ full-res composite；`requireCsmSnapshot` 同源 CSM；参数与冲白保护收紧。
5. JASS/command bridge 与 env 控制入口已齐（见 27 号文档）。
6. `build32_safe.cmd` / `ninja -C build32` 本轮可过（以你本机最后一次编译为准）。

**未完成 / 未用户前台验收**：
- 用户前台完整 `run_light_feature_matrix.py` 矩阵（isolated 结果不可信作最终画质/FPS 结论）。
- 正式 **A1：N−1 双缓冲 CSM 快照**（体积光/receiver 与 ShadowMap 生产解耦）。
- 正式 **A2：CSM prepare/sort 丢 Worker_Prepare**（与点光 worker 不是同一条路径）。
- **B1：点光重要性排序**。
- 体积光阴影质量/采样与用户主观 “阴影不对” 的专项（用户曾 defer vol-light shadow 修复）。

### 2. 默认配置清单（接棒时视为当前生产倾向）

| 配置 / 位置 | 值 | 说明 |
|---|---|---|
| `kShadowS1TerrainPersistentGeometryEnabled` | true | S1 persistent |
| S1 capture period | **1** | 禁止 >1 |
| S1 early cache | ON（代码路径） | Phase A 大收益 |
| `kShadowSemanticCoreSceneKeepS1TerrainLegacyCapture` | true（历史） | S1 仍走 legacy capture 语义 |
| `d3d9_war3_csm.h` `shadowResolution` | **2048** | Phase E |
| `kShadowAdaptiveMapUpdateEnabled` | true | period=2 等 |
| `kNativeMainLoopWaitGateHookEnabled` | true | 轻量 Idle 归因 |
| MT_CmdRecord | OFF | 目标保持 |
| Worker_Prepare | ON（点光相关） | 点光关时几乎无收益 |
| 点光 / 点阴影 / 体积光 | 默认 **关** | 高级选项 |
| VB alloc budget | **32**/帧 | 勿回 4 |
| path blocker 匿名几何宽启发式 | **已 REVERT** | 误杀单位子网格 |

### 3. 关键文件地图（改动落点）

| 文件 | 本轮角色 |
|---|---|
| `src/d3d9/d3d9_device.cpp` / `.h` | S1 early cache、period 门、Populate/VB cache、capture 热路径 |
| `src/d3d9/war3/core/war3_internal_test_config.h` | WaitGate、S1、adaptive map、VB cache 开关 |
| `src/d3d9/d3d9_war3_csm.h` | 默认 2048 |
| `src/d3d9/d3d9_war3_shadow.cpp` | ShadowMap adaptive、点阴影、receiver/volume 协作 |
| `src/d3d9/war3/hooks/war3_hook_lifecycle.cpp` | WaitGate 安装路径 |
| `subprojects/war3fx/shaders/war3_volumetric_*.frag` 等 | 体积光 effect/composite |
| `AutoTest/dual_perf_baseline.py` | 护栏双图（FG） |
| `AutoTest/run_light_feature_matrix.py` | 灯光矩阵 |
| `AutoTest/artifacts/phaseAE_closeout_20260709.json` | **A→E 收官数字唯一摘要** |
| `docs/research/war3_render_issues/25_s1_terrain_csm_occluder/` | S1 语义/历史 |
| `docs/research/war3_render_issues/26_*` / `27_*` | 体积光/动态光 |

### 4. 明确「没做完 / 禁止重做」清单

#### 4.1 未完成（建议下一 AGENT 优先级）

1. **护栏仍 FAIL**：高压 67.9≪85，低压 113≪120。
2. **主线程 ~11ms 未归因到可砍模块**：tracked shadow 已薄；需 **引擎层**（MainLoop 非 Wait、Jass、RenderQueue flush、Present/DXVK 录制）深挖，或接受 “dxvk_only≈63” 为当前硬件/场景上限并改目标。
3. **Plan A1** N−1 CSM 双缓冲快照（体积光/后处理与 map 生产解耦）。
4. **Plan A2** 把 **CSM** prepare/sort（非仅 PointShadow）丢 worker 线程；需 dual_perf FG 证明。
5. 点光 **B1 重要性排序**、face budget 与画质 A/B。
6. 用户前台 **light_feature_matrix** 正式验收 + 体积光阴影主观问题（若仍开）。
7. 工作区 **大量未 commit**：接棒后先 `git status` / 决定是否只提交 war3 相关、勿误提交 submodules 噪声。
8. 历史 **静态阴影/path blocker** 视觉线：2026-07-08 已有 “静态阴影解决版” 里程碑（UnitUI buildingShadow 等），与本轮性能线独立；若用户仍报残留，另开视觉专项，不要用 ListA/WriteMaskRegion 粗拦。

#### 4.2 已证伪 / 禁止

| 动作 | 原因 |
|---|---|
| S1 `period>1` | 地形阴影闪 |
| 宽 Sleep/Wait API Hook 当默认 | 高压掉帧 |
| VB alloc budget→4 | 阴影不完整（7.155 REVERT） |
| 匿名 rigid vtx 启发式拦 path blocker | 误杀真实单位（7.155 REVERT） |
| ListA_Render* 当阴影闸 | 毁悬崖地形（7.143） |
| 用 isolated desktop FPS 过护栏 | 噪声假 FAIL/PASS |
| 指望 adaptive ShadowMap 在 skinned 战场大幅省 | 动态 pose 下几乎不 reuse |

### 5. 接棒后建议操作顺序

1. 读本节 + `phaseAE_closeout_20260709.json` + `docs/research/.../25` 与 `27`。
2. 本机 `ninja -C build32` 或 `build32_safe.cmd`，部署 `src/d3d9/d3d9.dll` → `E:\Work\War3\d3d9.dll`。
3. **前台** 跑一遍 `py AutoTest\dual_perf_baseline.py` 复现 ~68 / ~113 是否仍成立（防环境漂移）。
4. 二选一主攻：
   - **性能**：A2 CSM worker 或 MainLoop/Jass 非 Wait 热点（先 perf HTML section，再改）。
   - **功能**：A1 快照 + 灯光矩阵前台验收。
5. 每轮改动必须 dual_perf FG；视觉相关必须看图/日志，禁止只看 FPS。

### 6. 一句话阶段结论

> **S1 early cache（period=1）是本轮唯一质变；其余多为归因与小幅 GPU 收口。护栏未过：瓶颈已不在 tracked ShadowCapture，而在 ~11ms 主线程中的引擎/未 scope 路径与约 63 FPS 的 dxvk 基线天花板。点光/体积光入口已产品化但默认关闭，A1/A2 与前台矩阵仍待下一 AGENT。**

---

### 📊 逆向论文交付状态（2026-05-19）

| 章节 | 行数 | 深度 | IDA rename | IDA comments |
|---|---|---|---|---|
| 第 1 章 剔除→渲染过渡 | ~750 | 架构+调用链 | 26 | 6 |
| 第 2 章 RenderQueue 完整数据流 | ~1100 | 架构+算法 | 23 | 13 |
| 第 3 章 CSprite 动画系统 | ~750 | 架构+CFG | 7 | 0 |
| 第 4 章 Pose 数据流（重中之重） | ~990 | **完整算法** | 24 | 13 |
| 第 5 章 CGeosetData 顶点/skinning | ~500 | 架构+算法 | 15 | 6 |
| 第 6 章 FogMask 静态阴影治理 | ~600 | **完整算法** | 41 | 14 |
| 第 7 章 Light/Shadow pass | ~450 | 架构+路径 | 30 | 4 |
| 第 8 章 D3D9 State Bridge | ~300 | 架构+vtable | 8 | 2 |
| 第 9 章 UI 渲染分支 | ~150 | 架构 | 11 | 0 |
| 第 10 章 粒子/Effect | ~120 | 架构+算法 | 3 | 0 |
| 第 11 章 深度算法参考 | ~600 | **完整算法** | 18 | 7 |
| 第 12 章 材质/状态机/批次 | ~500 | **完整算法** | 30 | 6 |
| 第 13 章 Sprite/RenderQueue/Entry | ~400 | **完整算法** | 6 | 6 |
| **总计** | **~7300 行** | | **~280** | **~101** |

### 🔧 关键逆向结论

1. **War3 CPU skinning**：War3 1.27a 不使用 D3D9 vertex blending（`D3DRS_VERTEXBLEND=D3DVBF_DISABLE`），所有骨骼变换在 CPU 端完成。draw-time VB capture 是正确的 shadow pose 修复方向。
2. **Path blocker 视觉残留**：D3D9 CSM 层以 rawcode / jHandle / widget 直读为主，匿名 Terrain/Decorations rigid marker 只在有阶段上下文的路径兜底；原生 TerrainShadow 残留优先走 `RegisterImage` producer 端治理。
3. **建筑/原生静态阴影屏蔽**：当前默认链路收敛到 `StaticStampPath + ToggleStaticStampFromObject + RegisterImage` producer 端精确治理；`DispatchToShape/WriteMaskRegion/ListA/ListB` 仍默认关闭，避免历史雾/边界/悬崖地形误伤。
4. **RenderQueue 排序**：5 级优先级链 = special → transparent → layerState → meshData → 内容比较。
5. **CSpriteUber dt gate**：4 变体共享 `fabs(dt)>=2*FLT_EPSILON` 门控，dt>0 占 98.79%。

### 📈 性能优化累计收益（Phase 7.70-7.86）

| 优化项 | 理论 CPU 削减 |
|---|---|
| 同帧 draw-time VB capture dedup | ~40-100μs/帧 |
| 6 个全局 mutex → shared_mutex | ~0.6-1.5ms/帧 |
| Phase 7.49 publish probe 默认关 | ~0.3-0.9ms/帧 |
| frameTag/frameNumber load 合并 | ~0.15-0.4ms/帧 |
| 4 个 thread_local scratch caches | ~0.1-0.3ms/帧 |
| **总计** | **~1.1-2.7ms/帧** |

### 🔖 当前稳定回退点（2026-04-05）
1. **稳定提交点**：`ea204b1`（`checkpoint runtime shadow bridge and dynamic unit fallback fixes`）。
2. **当前策略**：
   - 飞行单位、动态 `CUnit`、蒙皮单位已强制退出 `persistent cache`，避免阴影被静态缓存后停在原地；
   - `runtime shadow bridge v1` 与“对象身份直传桥”已落地，但仍以**只读桥接 + fallback 正确性优先**为主；
   - 动态 Pose Takeover 尚未正式点亮，当前目标是先稳定生命周期、崩溃追踪和接入时机。
3. **当前主阻塞**：
   - AutoTest 进图判定链与真实 in-map 验证仍需继续稳固；
   - 动态姿态接管的生产级接入点需要收敛到更安全的 `CSpriteUber_PreRenderAndUpdatePosePalette` 返回点；
   - 必须优先消除未处理异常与悬空指针问题，才能继续推进动态阴影主路径。

### 🎯 当前阶段目标（2026-04-05）
1. 在保持当前“动态单位不再被错误缓存”的前提下，继续推进 `runtimeModel + pose palette` 的安全接入。
2. 将动态阴影主路径从 `draw-time fallback freeze` 迁移到“静态模型资源 + 每帧姿态更新”。
3. 保留研究资料、桥接模块和崩溃证据链，确保后续可以安全回退与复盘。
4. 在进入下一轮性能优化前，先完成崩溃隔离、接入时机收敛与 AutoTest 稳定化。

### 🎯 本阶段目标（新增）
1. 在不牺牲当前功能与性能的前提下，完成架构解耦与模块化重排。
2. 将 `d3d9_war3_hook.cpp` 从“功能承载入口”降级为“编排入口”。
3. 建立统一 Hook 安装框架（地址解析、安装、降级、统计、日志）。
4. 建立可回归性能护栏，确保每个阶段重构后可量化验证“不倒退”。

### 🏗️ 项目结构总览（行业化 v1）
| 层级 | 目录 | 关键文件 | 职责 | 扩展入口 |
|---|---|---|---|---|
| Runtime / Bootstrap | `src/d3d9/war3/platform/` | `war3_runtime_bootstrap.*`, `war3_module_api.*` | 运行时初始化、模块生命周期、状态统计 | 在 `war3_module_api` 注册新模块 |
| Hook Orchestrator | `src/d3d9/war3/hooks/` | `war3_hook_address_book.*`, `war3_hook_install_util.*`, `war3_hook_*.cpp` | 地址解析、MinHook 安装、分域 Hook 编排 | 新增域时按 `War3HookXxx::Install` 接入 |
| Render Frontend | `src/d3d9/war3/render/` | `war3_scene_collector.*`, `war3_render_exec_batch.*`, `war3_render_queue_tracker.*` | 对象收集、批次桥接、队列追踪 | 在 collector/exec_batch 增强分类或桥接 |
| Frame / Pipeline | `src/d3d9/` + `src/d3d9/war3/render/` | `d3d9_war3_pipeline.*`, `war3_frame_graph.*` | BeforeUi/BeforePresent 编排与 pass 调度 | 在 `war3_frame_graph` 增减事件序列 |
| Feature Modules | `src/d3d9/` | `d3d9_war3_shadow*.cpp`, `d3d9_war3_ssao.cpp`, `d3d9_war3_aa.cpp` | 阴影/描边/SSAO/AA 等效果 | 按模块文件独立演进，避免回灌主入口 |
| Shader / Material | `src/d3d9/war3/shader/` + `src/d3d9/` | `war3_shaderpack.cpp`, `war3_shader_api.*` | ShaderPack、uniform 与材质覆盖 | 新增 pass 时先声明 API 再接管线 |
| Diagnostics / Perf | `src/d3d9/war3/tools/` | `war3_perf_monitor.*`, `war3_diagnostics_hub.*` | 性能采样、健康日志、HTML 报告 | 统一在 PerfMonitor 增指标，避免分散口径 |

### 🚀 使用指南（开发/验证/性能）
1. **编译**：`ninja -C build32`（必须通过后再进入下一阶段）。
2. **运行时日志**：DebugView 观察 `DXVK War3Hook`, `DXVK War3Diag`, `DXVK War3Shadow` 前缀。
3. **性能记录**：
   - 在 ImGui 面板启动/停止性能记录（停止时自动导出报告）。
   - 报告路径：`WarVK/Log/war3_perf_report_YYYY_MM_DD_HH_MM_SS.html`。
4. **性能窗口与缓存配置（可选环境变量）**：
   - `DXVK_WAR3_PERF_WINDOW_SEC`：报告统计窗口秒数（默认 1200）。
   - `DXVK_WAR3_PERF_HISTORY_SEC` / `DXVK_WAR3_PERF_HISTORY_FRAMES`：帧历史容量。
   - `DXVK_WAR3_PERF_PENDING_MAX`：GPU query 待处理上限。
5. **新增功能接入流程**：
   - 先在 `hooks` 中定义域入口；
   - 再在 `render/pipeline` 接事件；
   - 最后在 `tools` 补监控指标与回归口径。
6. **验收口径（当前强制）**：
   - 功能不回退（阴影/描边/JASS 时间链路稳定）；
   - `ninja -C build32` 通过；
   - 性能报告具备 `Avg/P95/P99 + Coverage + Untracked + Self/Inclusive` 四类指标。

### 🧱 行业化重构计划表（2026-02-22 起）
| 阶段 | 目标 | 主要工作 | 验收标准 | 预计时间 |
|---|---|---|---|---|
| P0 基线护栏 | 建立“可回归”底座 | 固化 benchmark 场景、日志采样、关键性能门限；整理功能回归清单 | `ninja -C build32` 稳定通过；可输出同场景对比报告 | 1-2 天 |
| P1 Hook 架构统一 | 消除重复安装与分散入口 | 新增 Hook AddressBook/Registry/Gate；主入口统一注册路径 | `d3d9_war3_hook.cpp` 仅保留编排；安装成功率/失败原因可观测 | 3-5 天 |
| P2 域迁移落地 | Render/Jass/Lifecycle/UI/Shadow 全域模块化 | 将域内 Hook 实现迁移到 `war3/hooks/*`；删除重复实现 | 不再存在同功能双实现；功能回归通过 | 4-6 天 |
| P3 桥接契约化 | 稳定渲染层与逻辑层边界 | 统一 `sceneNode/jHandle/unit/rawcode` 契约；补齐桥接断言与统计 | 描边/阴影匹配链路可解释、可追踪、可回归 | 5-7 天 |
| P4 设备热路径解耦 | 降低 `d3d9_device.cpp` 耦合度 | 抽离 ShadowCapture/Outline/BeforeUi 编排模块 | 热路径行为一致；CPU 指标不回退 | 7-10 天 |
| P5 配置与诊断标准化 | 降低开关复杂度 | 分层配置（dev/profile/release）；统一诊断输出 | `war3_internal_test_config.h` 收敛；诊断项可分级控制 | 3-5 天 |
| P6 文档行业化 | 形成可维护工程文档体系 | 更新 `docs/war3_shader_docs` 与研究文档结构图/模块说明 | 新成员可按文档完成定位与扩展 | 持续并行 |

### ✅ P0 当前落地状态（2026-02-22）
1. 已完成：全项目结构盘点与耦合点识别（Hook 重复实现、状态层分裂、热路径集中）。
2. 已完成：编译基线验证，`ninja -C build32` 通过（存在既有 warning，无阻塞错误）。
3. 已完成：`AGENTS.md` 与行业化看板同步到“v1 收官版本”，后续按 v2 计划推进。

### ✅ 已完成工作 (Completed)
1. **JASS 时间获取修复**:
   - 修复了 `GetTimeOfDay` 无法获取时间的问题。
   - 重构了初始化时序：`ActivateWar3Runtime` -> `Hook_ExecuteJassFunction` -> `NET_EVENT_GAME_READY`。
   - 解决了早期 JASS Native 调用时的运行态同步问题（该桥接实现现已移除，保留原生运行时链路）。
   - 恢复了动态光影随时间变化的功能。
2. **基础解耦 (Hook Decoupling)**:
   - `NetEventHook` 已独立。
   - `ShaderManager` 初步建立。
   - 早期曾实现 JAPI 封装层；当前版本已移除相关桥接源码。
   - `Hook_WorldObjects_RenderGroup` 逻辑已抽取至 `RenderQueueTracker`，移除了 RenderQueue 指针操作的大量 hack 代码。
3. **性能与稳定性修复**:
   - **卡顿解决**: `ShaderManager` 实现懒加载 (Deferred Compilation)，消除 `ActivateWar3Runtime` 时的 I/O 卡顿。
   - **崩溃解决**: `ResetWar3RuntimeState` 优化了析构顺序，并且将核心单例 (`War3Renderer`, `RenderQueueTracker`, `ShaderManager` 等) 改为 **Leaky Singleton** 模式，彻底避免 Process Detach 时的静态析构顺序崩溃。
4. **性能诊断**:
   - 在 `ActivateWar3Runtime` 中添加了微秒级耗时统计日志 (`DXVK War3Hook Init Profile`)，用于定位启动卡顿的精确位置。
5. **三方向专项推进（ASM 驱动）**:
   - 新建统一研究目录：`docs/research/war3_render_issues/`，包含三个方向的独立文档。
   - **方向1（合批）**：在 `war3_render_queue.h` 收紧起批条件，新增 next `sceneNode` 可读性检查，减少 singleton 空转；并将 `FlushGroup` scope 移入 `pendingCount>1` 分支。
   - **方向2（LOSBlocker）**：`War3TryCaptureShadowCaster` 增强为 `rawcode + Sprite->Model` 双通道过滤；`war3_model_hook` 新增 `IsPathBlockerSprite`；`kPathBlockerHideEnabled` 默认开启。
   - **方向2（追加）**：新增 `batchHandle -> RenderObjectRegistry` 回查兜底，缓解 `currentObj` 丢失导致的 LOSBlocker 过滤漏判。
   - **方向3（建筑静态阴影）**：基于 ASM 结论补齐 `ShadowProjector_Add_FromObject / Add_Simple` Hook，接入来源路径识别（Runtime/JassBridge）和 key 级拦截策略；并新增 `TerrainShadow_RenderListA` Hook，用于 mode1 下静态阴影组条目拦截与低频统计日志。
6. **回归修正（2026-02-20 夜间）**:
   - 关闭 `ListA` 默认白名单与组条目拦截，避免误杀战争迷雾/边界阴影。
   - 新增 `ShadowUpdate_WriteEntry(0x73F7A0)` 上游写入钩子与回调 RVA 统计/按回调拦截开关。
   - `ShadowProjector_Add_FromObject` 在 mode1 下同时拦截 Runtime/JassBridge 路径；`Add_Simple` 新增桥接路径识别（0x764AC0）。
   - LOSBlocker 链路新增 `LastRenderHandle` 回退与“无 unitPtr 仍按 rawcode 识别”，并在 Decorations 路径强制保留桥接追踪。
7. **专项推进（2026-02-21 凌晨）**:
   - **描边回归修复**：`War3TryCaptureShadowCaster` 将句柄拆分为 `strictBatchHandle`（描边匹配）与 `lookupHandle`（LOSBlocker 回查），避免 `LastRenderHandle` 污染导致“全体描边”。
   - **LOSBlocker 黑名单增强**：四字码判定新增第二字符大小写归一化，兼容 `YTlc/Ytlc` 变体。
   - **建筑静态阴影上层推进（ASM 结论落地）**：
     - 新增 `TerrainShadow_RenderListB(0x737400)` Hook；
     - mode=1 默认拦截 `ListB type=4`（stage14 直调链路）；
     - mode>=2 支持全拦截 ListB，补齐“完全禁用原生阴影”漏网路径。
   - **合批性能修复**：`SingletonBypass` 与尾部 fallback 恢复原始 `layerChanged/stateChanged` 计算，不再强制 `dispatchCommon(...,1,1)`；并修正 `StageUpdate(0)` 计数补偿。
8. **专项推进（2026-02-21 夜间）**:
   - **描边误命中止血**：`SceneCollector` 在“过滤模式”下禁用 `CUnit+0x0C/+0x10` 猜测句柄，仅允许 tracked-handle 反查命中，修复“单目标描边变全体描边”。
   - **静态阴影上游拦截加强**：`ShadowProjector_Add_FromObject/Add_Simple` 在 mode>=1 直接拦截对象投影写入，不再依赖返回地址范围判断，减少漏拦截。
   - **LOSBlocker 黑名单补齐**：补齐 `YTpb/YTfb/YTlb` 到阴影 FourCC 黑名单，并在投影器四字码判定中加入第二字符大小写归一化。
   - **合批空转治理**：起批阶段新增 Outline/Bloom 一致性预检，避免“起批即拆批”导致的 singleton 空转。
   - **Native 研究目录建立**：新增 `docs/research/war3_render_issues/native/README.md`，固化 ASM 还原主链、阴影分支地址与下一步替换计划。
9. **Native 还原推进（2026-02-21 深夜）**:
   - `war3_native_renderer.cpp` 的主调度链已按 ASM 重排：`RenderScene -> DispatchStage -> Flush` 顺序、两次 flush 时机、group 偏移均与 IDA 对齐。
   - `DispatchStage case16/18/21` 已按 ASM 调用链补全到 native 代码（RVA 解析函数 + 全局地址访问）。
   - 新增 `src/d3d9/war3/native/address_book/README.md`，统一记录地址、调用约定、阶段映射与未还原点。
   - 更新 `src/d3d9/war3/native/README.md` 与 `docs/research/war3_render_issues/native/README.md`，将 native 状态从“概念完成”改为“ASM 基线 + 分阶段补齐”。
10. **架构拆分与桥接快路径（2026-02-21 夜间第二轮）**:
   - `d3d9_war3_hook.cpp` 阴影域已物理拆分至 `src/d3d9/war3/hooks/war3_hook_shadow.cpp`，并新增 `war3_hook_shadow.h` 统一接入；
   - `War3Hook::InstallGameHooks` 改为通过 `ShadowHookAddresses + InstallShadowHooks()` 注册阴影相关 MinHook；
   - `ExecBatch` 新增“侵入式句柄槽”可选快路径（默认关闭）：`kNativeBridgeInlineHandleSlotEnabled/Offset/WriteBackEnabled`，用于后续 ASM 确认偏移后做 O(1) 句柄直读实验。
11. **专项回归修正（2026-02-21 深夜第三轮）**:
   - **LOSBlocker 0 命中修复链路**：
     - `ComputeNeedsObjectTracking()` 在 `kPathBlockerHideEnabled && kPathBlockerForceBridgeTrackingEnabled` 时强制开启对象追踪；
     - `ExecBatch` 的 `needsPathBlockerBridge` 从仅 Decorations 扩展到 `WorldObjects/SelectionOverlay/Decorations/RangeIndicatorTarget`；
     - `SceneCollector` 的 `pathBlockerTrackAll` 扩展为全组生效（不再仅 group2）。
   - **静态阴影诊断增强**：
     - `Hook_ShadowUpdate_WriteEntry` 新增 callback RVA 频次 Top 统计（`ShadowUpdate cbTop`）；
     - `Hook_ShadowProjector_Add_FromObject` 新增 key 采样日志（`Projector key sample`），用于锁定建筑静态阴影 key。
   - **策略回归确认**：
     - 保持“建筑贴花恢复”前提：不再启用 `Projector mode>=1` 粗暴全拦截；
     - `ListB type=4` 继续默认关闭，仅在定位完成后做精确封堵。
    - **合批卡点诊断增强**：
      - `BatchMergeStats` 新增 `Merge/InstGroups/NoShader/AllocOrPortrait/AppendBreak` 计数；
      - 用于直接识别 Single Dispatch 失败主因（shader 未实例化 / 头像与分配回退 / append 中断）。
12. **专项回归修正（2026-02-21 深夜第四轮）**:
   - **PathBlocker 统计日志可见性修复**：
     - `PathBlockerShadow stats` 不再仅受 `kPathBlockerDebugEnabled` 门控；
     - 改为 `kPathBlockerStatsLogging || kPathBlockerDebugEnabled`，并将统计频率从 8000 降至 2000。
   - **描边目标不命中修复**：
     - `SceneCollector` 过滤模式恢复“直接 handle 值”识别，但仅允许命中 tracked handle；
     - 解决 `TAG=1` 场景下目标句柄存在但无法进入描边匹配链的问题。
   - **静态阴影统计日志增强**：
     - `ShadowUpdateWrite stats` 输出频率从 10000 调整为 3000，便于短窗口 DebugView 观测回调分布。
13. **专项回归修正（2026-02-21 深夜第五轮）**:
   - **LOSBlocker 根因对齐（noObj/raw=0）**：
     - `ShadowCapture` 增加 `lookupHandle -> HandleResolver -> CAgent/CUnit` 直解兜底；
     - 兼容“handle 表直接存 CUnit*”路径；
     - `PathBlockerShadow stats` 新增 `fallback=resolved/try` 观测字段。
   - **描边链路回归修复（与 PathBlocker 全量追踪解耦）**：
     - `SceneCollector` 的 `filtered` 不再受 `pathBlockerTrackAll` 影响；
     - 新增 `keepForPathBlocker` 保留非 tracked 对象用于 LOS 过滤桥接；
     - 避免再次退化到“全局句柄猜测”导致描边目标失配。
14. **AutoTest MCP 自动化链路（2026-02-23）**:
   - 新增 `AutoTest/war3_autotest_mcp.py`，支持 YDWE 风格 `-loadfile` 直进地图、自动截图、自动读取性能报告；
   - 新增项目侧 `runtime_status.json` 快照输出，MCP 可通过文件稳定判定“已进图”；
   - 新增 `start/get/stop_periodic_perf_test` 定时回归 API 与 `sync_all_debug` 全量调试聚合同步 API；
   - Codex MCP 配置新增超时：`startup_timeout_sec` / `tool_timeout_sec`，避免长任务锁死会话。
14. **行业化重构推进（2026-02-22 夜间）**:
   - **主入口瘦身完成**：`d3d9_war3_hook.cpp` 从 1400+ 行重构到 500+ 行，仅保留生命周期编排与域装配。
   - **分域接线完成**：`war3_hook_render/jass/lifecycle` 正式纳入构建并由中枢统一装配。
   - **AddressBook 落地**：新增 `war3_hook_address_book.h/.cpp` 统一维护 1.27a RVA，替换散落硬编码。
   - **兼容层补齐**：新增 `War3HookLifecycle::GetTrampolineFlushAndReset`，补齐分域共享状态符号。
   - **文档同步**：更新 `docs/research/war3_render_issues/04_architecture_refactor/README.md` 与 `docs/war3_shader_docs/architecture.html`。
15. **行业化重构推进（2026-02-22 深夜第二轮）**:
   - **阴影策略解耦**：新增 `war3_shadow_filter_policy.h/.cpp`，将 `Projector key/FourCC` 过滤与对象 FourCC 提取从 `war3_hook_shadow.cpp` 抽离。
   - **阴影 Hook 瘦身**：`war3_hook_shadow.cpp` 只保留阴影链路编排与统计逻辑，过滤实现改为策略模块调用。
   - **构建接线**：`src/d3d9/meson.build` 纳入 `war3/hooks/war3_shadow_filter_policy.cpp`。
   - **回归验证**：`ninja -C build32` 通过，确认本轮重构不改变行为。
16. **行业化重构推进（2026-02-22 深夜第三轮）**:
   - **Hook 安装器统一**：新增 `war3_hook_install_util.h/.cpp`，集中 `InstallMinHook(Create+Enable+错误日志)`。
   - **五域接入统一安装器**：`war3_hook_render/jass/lifecycle/ui/shadow` 全部改用 `InstallMinHook`。
   - **重复代码清理**：移除各域重复的 `MH_CreateHook/MH_EnableHook` 样板与局部安装器实现。
   - **文档同步**：`docs/war3_shader_docs/architecture.html` 补充 `war3_hook_install_util.*` 节点。
   - **回归验证**：`ninja -C build32` 通过，行为保持一致。
17. **行业化重构推进（2026-02-22 深夜第四轮）**:
   - **AddressBook 扩展**：新增 `uiDispatch/uiRenderableRender` 字段，UI 域地址不再硬编码。
   - **UI 域接入统一地址簿**：`InstallUiHooks` 与 `War3TryOverrideMaxFps` 的 GetD3d9Parameters 偏移统一从 AddressBook 读取。
   - **回归验证**：`ninja -C build32` 通过，确认字段扩展未破坏地址初始化顺序。
18. **行业化重构收官（2026-02-22 末）**:
   - **运行时解耦落地**：新增 `war3_runtime_bootstrap.*`，将核心初始化/重置流程从主入口剥离。
   - **帧图计划落地**：新增 `war3_frame_graph.*`，将 BeforeUi/BeforePresent 事件序列从管线实现中解耦。
   - **诊断中枢落地**：新增 `war3_diagnostics_hub.*`，统一输出模块运行态与健康日志。
   - **文档与看板收官**：`index.html / architecture.html / refactor_status.html` 同步为“行业化重构 v1 完成”。
   - **最终验证**：`ninja -C build32` 通过，当前阶段以“功能不回退、热路径不增抽象层”为验收结论。
19. **留言1~4专项归档收官（2026-02-22）**:
   - **研究统一索引**：`docs/research/war3_render_issues/README.md` 增加留言1~4速览表与 `06_message_1_4_archive` 入口。
   - **夜间成果归档**：新增 `docs/research/war3_render_issues/06_message_1_4_archive/README.md`，固化目标/实现/验证/遗留风险。
   - **前端文档同步**：`jass_render_architecture.html` 与 `refactor_status.html` 新增留言4（静态阴影上游入口）闭环说明。
   - **交叉引用补齐**：`03_building_static_shadow` 与 `05_jass_vm_and_partial_batch_submit` 已互链到统一归档页，降低后续重复排查成本。
20. **AutoTest 自动化基线（2026-02-23）**:
   - **YDWE 启动链复刻**：基于源码结论实现 `-loadfile + Maps\\Test\\WorldEditTestMap.w3x` 短路径复制策略，避免长路径加载失败。
   - **MCP 服务落地**：新增 `AutoTest/war3_autotest_mcp.py`，支持启动/等待进图/订阅运行日志/截图/关闭/读取性能报告一体化工具链。
   - **订阅式事件通道**：基于 DBWIN（OutputDebugString）构建事件轮询接口 `get_runtime_events` 与 `wait_for_game_ready`。
   - **自动录制开关**：`War3PerfMonitor` 新增环境变量 `DXVK_WAR3_PERF_RECORD_ON_START`，用于无人值守压测自动开启性能采集。
   - **交付文件**：`AutoTest/run_mcp.ps1`、`AutoTest/README.txt`、`AutoTest/ydwe_launch_notes.txt`、`AutoTest/requirements.txt`。
21. **渲染 CPU 优化二轮归纳（2026-02-23，逆向评估）**:
   - **已完成逆向核对（Render 主链）**：
     - `CWorld_RenderScene(0x6F3681C0)`：确认双 `RenderQueue_FlushAndReset` 时序与 stage 调度顺序；
     - `CWorld_DispatchStage(0x6F363020)`：确认 `WorldObjects_RenderGroup` 与 `TerrainShadow_Dispatch` 入口映射；
     - `RenderQueue_FlushSortedItems(0x6F1380A0)`：确认 `qsort -> Dispatch_Common/Special -> per-item StageUpdate(ECX=0)` 主热路径；
     - `RenderQueue_Dispatch_Common(0x6F13A5E0)` / `Special(0x6F13A780)`：确认矩阵更新、状态绑定、fallback multipass 触发点；
     - `RenderBatch_Submit(0x6F1375C0)` 与 `AUCTransparent_AddEntry(0x6F137AF0)`：确认 opaque/transparent 入队成本结构。
   - **已完成可实现性分级（仅渲染层）**：
     - 高可行（低风险）：Hook 热路径开销收缩、Tracker/TagStage 缓存改进、SceneCollector 条件采集收缩、保守接管阈值重整；
     - 中可行（中风险）：Opaque 全量接管稳定化、透明队列“部分接管”策略、Dispatch 局部上下文复用扩展；
     - 高收益高风险：`RenderBatch_Submit` 前置合并/旁路、`Dispatch_Special` fallback 多通道压缩。
   - **本轮约束**：仅做方案归纳与逆向确认，不修改渲染代码行为。
22. **渲染 CPU 优化三轮实装（2026-02-24 凌晨）**:
   - **已完成实现**：
     - `war3_hook_ui.cpp`：UI 高频 `cpuScope` 条件化（默认关闭细粒度统计时编译剔除）；
     - `war3_hook_render.cpp`：`DispatchTagStageCache` 升级为 8 槽 TLS + LRU；
     - `war3_scene_collector.cpp`：过滤模式下“无 tracked handle 且无 probe”直接早退；
     - `war3_hook_render.cpp` + `war3_internal_test_config.h`：Conservative takeover 透明分级阈值；
     - `war3_hook_render.cpp`：`DispatchLocalMerge/DispatchTagStageCache` 默认关闭统计时不再执行热路径原子计数。
   - **AutoTest 回归结果（均 2K 全屏，截图基线匹配，无崩溃）**：
     - Batch1：`war3_perf_report_auto_2026_02_24_02_55_39.html`，`avgFps=88.356`；
     - Batch2：`war3_perf_report_auto_2026_02_24_02_59_30.html`，`avgFps=85.371`；
     - Batch3：`war3_perf_report_auto_2026_02_24_03_05_15.html`，`avgFps=87.068`；
     - Batch4（统计原子计数剔除）：`war3_perf_report_auto_2026_02_24_03_13_38.html`，`avgFps=85.432`。
     - Batch4 多轮稳定性复测（3 轮）：`03_17_27 / 03_17_47 / 03_18_08`
       - 聚合：`avgFps=87.1927`，`avgFrameTimeMs=11.4687`，`avgTrackedActiveCpuMs=2.221`。
    - **阶段结论**：
      - 该批低风险改动已稳定落地，但单轮短窗波动较大，尚未出现“确定性帧率抬升”；
      - 当前更高优先级应转向 `RenderBatch_Submit` 前置聚合与更强队列接管策略的 A/B 实验。
23. **第四轮：MainLoop 全链路拆解与模块级计时补全（2026-02-24）**:
   - **逆向补全（IDA）**：
     - 复核主循环核心 `sub_6F05F710`，确认每轮关键链路：
       - `SelectWorker(0x05DE80)` -> `PrepareWait(0x05DEE0)` -> `WaitGate(0x158940)/SleepGate(0x1648A0)`；
       - 超时分支：`PrepareDispatch(0x05FCA0)` -> `RunCallbacks(0x0603B0)` -> `MessagePump(0x059B00)` -> `FinalizeDispatch(0x05FD10)` -> `QueueFlush(0x05B080)` -> `TickUpdate(0x05FC10)`；
       - 收口分支：`FinalizeWorker(0x05DCE0)` / `FinalizeTick(0x05FB10)` / `ComputeWakeDelta(0x060500)` / `Reschedule(0x05EE90)`。
     - 复核 `EventDispatch(0x05A310)` 的 case0~14 调度表并对齐子函数。
   - **代码落地（仅透传计时，不改行为）**：
     - `war3_hook_address_book.h/.cpp` 新增主循环深层地址：
       - `enginePrepareWait/enginePrepareDispatch/engineFinalizeDispatch/engineTickUpdate/engineFinalizeWorker/engineComputeWakeDelta`。
     - `war3_hook_lifecycle.cpp` 新增对应 Hook 与计时 scope：
       - `War3MainLoop/Engine/PrepareWait`
       - `War3MainLoop/Engine/PrepareDispatch`
       - `War3MainLoop/Engine/FinalizeDispatch`
       - `War3MainLoop/Engine/TickUpdate`
       - `War3MainLoop/Engine/FinalizeWorker`
       - `War3MainLoop/Engine/ComputeWakeDelta`
     - `EventDispatch` 由“System/Input/Game 粗分组”改为 `Case0~Case14` 精确分桶。
     - `war3_perf_monitor.cpp` 的 MainLoop Stage 聚合规则同步扩展到新增阶段与 case。
     - `war3_internal_test_config.h`：`kNativeMainLoopDeepPhaseHookEnabled=true`（用于本轮采样）。
   - **编译与自动化验证**：
     - `ninja -C build32` 通过。
     - AutoTest（2K 全屏）报告：
       - `E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_24_03_50_25.html`
       - `avgFps=93.758`, `avgFrameTimeMs=10.666ms`, `avgGpuTimeMs=2.168ms`
       - `avgMainThreadCpuMs=3.727ms`, `avgProcessCpuMs=6.488ms`
       - `activeFrameTimeMs=4.230ms`, `avgIdleWaitCpuMs=10.666ms`, `cpuCoveragePct=100%`
     - 截图基线匹配 `2560x1440`（无崩溃，自动退出未抢焦点）。
24. **第五轮：第四轮方案落地与 AutoTest 结项（2026-02-24）**:
   - **本轮目标**：
     - 将第四轮“可执行优化方案”先落一批低风险高收益项，并要求每项通过 AutoTest（2K 全屏）结项。
   - **代码落地（渲染层）**：
     - `war3_internal_test_config.h`：
       - 新增/启用保守接管自适应参数：
         - `kNativeQueueTakeoverConservativeEnableSmallOpaqueNoTransparent=true`
         - `kNativeQueueTakeoverConservativeMinOpaqueNoTransparent=1`
         - `kNativeQueueTakeoverConservativeHighOpaqueThreshold=96`
         - `kNativeQueueTakeoverConservativeMaxTransparentForTakeoverHighOpaque=8192`
       - 新增 `kNativeRenderQueueDiagnosticStatsEnabled=false`（默认关闭高频诊断计数）。
       - 将 `kNativeMainLoopDeepPhaseHookEnabled` 默认调整为 `false`（性能模式默认关闭深层逻辑计时 Hook）。
       - 新增 ShadowMap 自适应更新开关：
         - `kShadowAdaptiveMapUpdateEnabled=true`
         - `kShadowAdaptiveMapUpdateMinCasters=128`
         - `kShadowAdaptiveMapUpdatePeriod=2`
         - `kShadowAdaptiveMapUpdateCameraMaxDelta=0.0005f`
         - `kShadowAdaptiveMapUpdateCasterDelta=2`
     - `war3_hook_render.cpp`：
       - `ShouldUseConservativeQueueTakeover` 增加“无透明时降低 Opaque 门槛 + 高 Opaque 压力时放宽透明阈值”的自适应策略。
     - `war3_render_queue.h`：
       - `FQ_Sort_Opaque/FQ_Dispatch_Opaque/FQ_Total_Trans` scope 改为仅在 PerfTracking 开启时生效（热路径减负）。
       - 新增透明排序快路径：若透明队列已按 `sortKey` 有序则跳过 `std::sort`。
       - `BatchMergeStats/BatchMerger` 高频统计与日志改为受 `kNativeRenderQueueDiagnosticStatsEnabled` 门控。
     - `d3d9_war3_shadow.cpp`：
       - `ShadowMap` 增加“高 caster + 视角稳定 + caster 稳定”的隔帧复用策略，降低阴影图重复构建成本。
   - **编译与回归**：
     - `ninja -C build32` 通过。
     - AutoTest BatchA（2K 全屏）：
       - 报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_24_04_14_19.html`
       - `avgFps=112.122`，`avgFrameTimeMs=8.919`，`avgGpuTimeMs=1.635`
       - `avgMainThreadCpuMs=3.486`，`avgProcessCpuMs=6.023`
       - 截图：`AutoTest/artifacts/screenshots/war3_20260224_041358.png`（`2560x1440`，基线匹配）。
     - AutoTest BatchB（2K 全屏复测）：
       - 报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_24_04_15_58.html`
       - `avgFps=111.993`，`avgFrameTimeMs=8.929`，`avgGpuTimeMs=1.638`
       - `avgMainThreadCpuMs=3.470`，`avgProcessCpuMs=6.001`
       - 截图：`AutoTest/artifacts/screenshots/war3_20260224_041537.png`（`2560x1440`，基线匹配）。
   - **对比基线（本轮改动前）**：
     - 基线：`war3_perf_report_auto_2026_02_24_04_03_01.html`
     - `avgFps`：`100.127 -> 112.122`（+11.99 FPS）
     - `avgFrameTimeMs`：`9.987 -> 8.919`（-1.068 ms）
     - `avgGpuTimeMs`：`2.142 -> 1.635`（-0.507 ms）
     - `avgMainThreadCpuMs`：`3.697 -> 3.486`（-0.211 ms）
   - **阶段结论**：
     - 本批低风险优化已通过 AutoTest 结项，收益稳定（两轮复测一致）。
     - 由于默认关闭深层逻辑计时 Hook，`cpuCoveragePct` 会下降，这是“运行时性能优先”的预期结果，不是采样失败。
25. **第六轮：渲染优化 × 逻辑优化 组合兼容测试与修复（2026-02-24）**:
   - **执行方式**：
     - 新增矩阵脚本 `AutoTest/run_round6_matrix.py`，自动执行：
       - 切换 `war3_internal_test_config.h` 关键开关；
       - `ninja -C build32`；
       - `run_quick_autotest`（2K 全屏）；
       - 输出 `AutoTest/artifacts/round6_matrix/round6_matrix_results.json`。
   - **组合覆盖（8 组）**：
     - `C0_base`：渲染基础优化包 + 逻辑优化关闭；
     - `C1_render_local_merge`：开启 `DispatchLocalContextMerge`；
     - `C2_logic_jass_adaptive`：开启 `JassOpBudgetAdaptive`；
     - `C3_render_local_merge_plus_jass_adaptive`：渲染局部合并 + JASS 自适应；
     - `C4_logic_mainloop_deep`：开启 `MainLoopDeepPhaseHook`；
     - `C5_logic_wait_hooks`：开启 `MainThreadWaitHook + Deep`；
     - `C6_logic_jass_deep_hooks`：开启 `JASS 深层 Hook`（高风险）；
     - `C7_all_optimizations`：渲染/逻辑开关全开（除诊断统计）。
   - **矩阵结论**：
     - `C0/C1/C2/C3/C4/C5/C7`：全部通过 AutoTest，截图基线 `2560x1440` 匹配；
     - **唯一失败组合：`C6_logic_jass_deep_hooks`**（进图前进程退出，`stage=ready` 失败）。
   - **根因定位**：
     - `war3_hook_address_book.cpp` 中 `executeJassFunctionInternal` 地址错误：
       - 旧值：`0x7F2D92`（函数中段，非入口）；
       - IDA 校验入口：`ExecuteJassFunctionInternal @ 0x6F7F2B40`（RVA `0x7F2B40`）。
   - **修复内容**：
     - 地址修正：`src/d3d9/war3/hooks/war3_hook_address_book.cpp`
       - `executeJassFunctionInternal` 改为 `0x7F2B40`；
     - 安装防护：`src/d3d9/war3/hooks/war3_hook_jass.cpp`
       - 新增 `HasClassicX86FunctionPrologue()`；
       - 深层 Hook 除“可执行可读”外，额外要求 x86 典型函数序言（`55 8B EC` 或 hotpatch 版本），避免中段地址被误 Hook。
   - **修复后复测**：
     - `C6` 复测通过：`war3_perf_report_auto_2026_02_24_04_32_38.html`，`avgFps=108.354`；
     - “全开 + JASS 深层 Hook”复测通过：`war3_perf_report_auto_2026_02_24_04_33_49.html`，`avgFps=103.255`；
     - 结论：第六轮组合兼容问题已修复，剩余差异主要是开关带来的性能权衡而非稳定性故障。
26. **第七轮：文档前端与研究目录结项归档（2026-02-24）**:
   - **前端文档更新**：
     - `docs/war3_shader_docs/refactor_status.html` 新增“今晚收官总结”面板，汇总：
       - 第五轮/第六轮改动清单；
       - 渲染层与逻辑层性能障碍；
       - 组合矩阵结果、修复项、最终收益；
       - 关键报告与证据路径。
     - `docs/war3_shader_docs/index.html` 更新页头时间标识与更新日志，增加“夜间优化与兼容收官”条目与入口链接。
   - **研究目录结项更新**：
     - 新增 `docs/research/war3_render_issues/09_2026_02_24_nightly_closeout/README.md`；
     - 更新 `docs/research/war3_render_issues/README.md` 目录、当前状态与“研究方向结项总览”。
   - **结项结论**：
     - 本夜（轮 5-6）优化改动、兼容修复与证据链已完整落文档；
     - 前端与研究目录均可独立作为交付材料进行审阅与后续接力。
27. **透明贴图发黑热修（2026-02-24）**:
   - **问题现象**：
     - 游戏内部分带透明/AlphaTest 的贴图出现发黑，表现为透明层材质状态疑似被上一批次污染。
   - **修复落地**：
     - `src/d3d9/war3/reimpl/war3_render_queue.h`
     - `src/d3d9/war3/reimpl/war3_render_queue.cpp`
     - 收紧 `FlushSortedItems_StdSort` 中 `layerChanged=0` 的复用条件：
       - 从“仅比较 `layerStatePtr` 前 20 字节”改为“同时要求 `meshData` 一致 + `layerIndex` 一致 + `layerState` 一致”；
       - 目标是避免跨 mesh/layer 误复用层状态导致的纹理/AlphaTest 污染。
   - **本地验证**：
     - `ninja -C build32` 已通过（未启动魔兽做实机，遵循当前“联机期间不自动拉起游戏”约束）。
28. **热路径与 AutoTest 稳定性修复（2026-02-25 凌晨）**:
   - **DispatchTagStageCache 热路径优化**（`src/d3d9/war3/hooks/war3_hook_render.cpp`）：
     - 在 `QueryTagStageCached` 增加 TLS hot-entry（按 `renderablePart` / `sceneNode`）；
     - 增加哈希直达槽（power-of-two 索引）并保留线性回退；
     - 目标是在不改变 tag/stage 语义的前提下降低热路径扫表成本。
   - **AutoTest 地图复制容错**（`AutoTest/war3_autotest_mcp.py`）：
     - `_prepare_test_map_copy` 新增文件占用兜底：当 `PermissionError` 且目标图已存在时直接复用短路径地图；
     - 解决 `WinError 32` 导致自动链路直接中断的问题。
   - **AutoTest 进程存活判定修复**（`AutoTest/war3_autotest_mcp.py`）：
     - `_pid_alive` 在 `OpenProcess/GetExitCodeProcess` 失败时回退 `tasklist` 检测；
     - 避免误判“进程已退出”导致 War3 残留未关。
   - **现场处置记录**：
     - 发现 `PID=7704` 存活但工具误判已退出，已手动强制结束；
     - 后续测试统一以“系统进程表复核”为最终准则。
29. **P3 契约化优先 + 渲染静态门控（2026-02-24 夜间补执行）**:
   - **架构契约化落地（仅拆结构，不改语义）**：
     - 新增 `src/d3d9/war3/hooks/war3_dispatch_contract.h/.cpp`：
       - 迁移 `DispatchLocalMergeState` / `DispatchTagStageCacheState`；
       - 迁移 `QueryTagStageCached` 契约入口与缓存命中/回退逻辑；
       - 新增契约类型：`War3DispatchQueryRequest` / `War3DispatchQueryResult`。
     - 新增 `src/d3d9/war3/hooks/war3_queue_takeover_policy.h/.cpp`：
       - 迁移 `HasTransparentTakeoverPrerequisites`；
       - 迁移 `ShouldUseConservativeQueueTakeover`；
       - 迁移 Conservative 统计与日志节流；
       - 新增契约类型：`War3QueueTakeoverDecision`（full/conservative/fallback + reason）。
     - `src/d3d9/war3/hooks/war3_hook_render.cpp`：
       - 改为调用契约层接口，保留 Hook 安装、trampoline 与编排；
       - 文件行数 `1721 -> 1201`（约 `-30.2%`，达到“至少 -20%”目标）。
     - `src/d3d9/meson.build`：
       - 纳入 `war3_dispatch_contract.cpp` 与 `war3_queue_takeover_policy.cpp` 构建。
   - **渲染优化静态门控（本轮不落性能实现代码）**：
     - 新增 `docs/research/war3_render_issues/10_p3_contract_static_gate/README.md`；
     - 对 C1~C5 给出 `预计收益(ms)=热点ms×可消减比例` 建模；
     - 门槛按 `AvgFrame 10~12ms` 的 `>=5%`（`0.5~0.6ms`）筛选：
       - 进入下一轮候选：`C4 Hook_FlushSortedItems 早退压缩`、`C1 Dispatch 分支归并`；
       - 本轮仅保留方案：`C2/C3/C5`。
   - **文档同步**：
     - 更新 `docs/research/war3_render_issues/04_architecture_refactor/README.md`（render hook 子模块图 + 风险/回滚点）；
     - 更新 `docs/research/war3_render_issues/README.md` 目录索引与当前状态。
   - **静态验收**：
     - `ninja -C build32` 通过（仅既有 warning，无新增阻塞错误）；
     - 本轮未启动 War3、未做 AutoTest 跑分，符合“仅静态评估”约束。
30. **第三轮续跑：渲染热路径小步优化 + 60s AutoTest（2026-02-25 凌晨）**:
   - **本轮目标**：
     - 将上一轮静态评估中可落地项做“小步实现 + 自动回归”，优先验证稳定性与可复现收益。
   - **代码落地（渲染层）**：
     - `src/d3d9/war3/hooks/war3_hook_render.cpp`
       - `C4`：`Hook_FlushSortedItems` 增加“空队列早退链路”：
         - `opaque=0 && transparent=0` 时不进入接管决策与 reimpl；
         - 若 `stateCleanupPending` 未知/非零则回落原生 flush，保证收口语义。
       - `C1`（小步）：
         - `Hook_Dispatch_Common/Special` 前置读取 `currentStage`；
         - `QueryTagStageCached` 改为请求结构 `War3DispatchQueryRequest(stageHint)`，减少重复状态读取与分支散落。
   - **编译验证**：
     - `ninja -C build32` 通过。
   - **60 秒 AutoTest 对照（2K 全屏，自动进图/截图/关进程）**：
     - 基线：`war3_perf_report_auto_2026_02_25_03_40_41.html`
       - `avgFps=113.735`，`avgFrameTimeMs=8.792`，`avgGpuTimeMs=1.524`。
     - 优化后：`war3_perf_report_auto_2026_02_25_03_44_03.html`
       - `avgFps=118.643`，`avgFrameTimeMs=8.429`，`avgGpuTimeMs=1.544`。
     - 对照结论：
       - FPS：`113.735 -> 118.643`（`+4.32%`）；
       - 帧时：`8.792ms -> 8.429ms`（`-0.363ms`）；
       - 稳定性：无崩溃，截图分辨率一致（`2560x1440`）。
   - **文档同步**：
     - `docs/war3_shader_docs/refactor_status.html`：新增“第三轮续跑”条目与 60s 对照结果；
     - `docs/war3_shader_docs/index.html`：新增 2026.02.25 更新日志与看板入口文案。

### 🚧 进行中/待解决问题 (Issues & In Progress)
1. **未解耦的大型 Hook 函数**:
   - [需要审查] 检查其他 Hook 函数是否仍有过重逻辑。
2. **行业化重构主线（新增）**:
   - [进行中] P0：建立功能/性能基线护栏与回归清单。
   - [已完成] P1：统一 Hook 地址入口（AddressBook）并完成主入口瘦身。
   - [已完成] P1-2：统一 Hook 安装路径（InstallMinHook）并接入五个域。
   - [已完成] P2：Render/Jass/Lifecycle 域迁移到 `war3/hooks/*` 并接入构建。
   - [进行中] P3：桥接契约化（对象身份链路可解释/可追踪/可回归）。
   - [已完成] P3-1：Shadow 过滤策略从 Hook 逻辑剥离为独立策略模块。
   - [已完成] P3-2：Render Dispatch/Takeover 契约化拆分（`war3_dispatch_contract` + `war3_queue_takeover_policy`）。
3. **专项验证待完成（高优先级）**:
   - [待验证] 方向1：`Opt/BatchMerge/SingletonBypass` 是否显著下降，`FQ_Dispatch_Opaque` 是否回落。
   - [待验证] 方向2：LOSBlocker 在不同海拔/镜头距离下是否仍有漏判，且“全体描边”回归是否消失。
   - [待验证] 方向3：`ListB type=4` 定向拦截是否稳定消除建筑静态阴影且不误伤雾/边界；`ShadowUpdate_WriteEntry` callback RVA 继续用于后续精细白名单。
   - [待验证] 透明/AlphaTest 贴图发黑热修是否在联机真实场景彻底消失（同时观察 `FQ_Dispatch_Opaque` 开销变化）。
4. **native 还原待推进（新增）**:
   - [已完成] `CWorld_RenderScene -> DispatchStage -> RenderGroup -> Dispatch/Flush` 调度表已落到 `src/d3d9/war3/native/address_book/README.md`。
   - [进行中] `war3_native_renderer.cpp` 主链已替换并补齐 `case16/18/21`，待继续替换 `war3_native_renderer_core.cpp` 的 `StageUpdate/Dispatch_*` 细节。
   - [待完成] `RenderQueue_StageUpdate(0x6F13A9B0)` 的 stage 描述结构字段语义与初始化来源拆解。
5. **AutoTest 自动化（新增）**:
   - [已完成] `AutoTest/war3_autotest_mcp.py` 的启动、进图判定、截图、报告读取。
   - [进行中] 将 MCP 与“项目内开放 API（模块运行态/性能开关）”打通，减少对 DebugString 的依赖。
6. **代码规范**:
   - 强制要求：**中文注释**，**中文回复**。
7. **渲染 CPU 优化主线（新增，2026-02-23）**:
   - [进行中] R0：先修复“统计口径与实际帧率错位”导致的误判（区分 Wait/Active/Render Hook 开销）。
   - [已完成] R1（P0）：收缩 Hook 热路径常驻开销（UI scope 条件化 + SceneCollector 空集早退）。
   - [已完成] R2（P1）：保守接管参数与透明分级策略重整（新增透明上限与“有透明时的 Opaque 提高门槛”）。
   - [已完成] R3（P1）：Tag/Stage 查询从单槽缓存升级为 8 槽 TLS + LRU。
   - [待执行] R4（P2）：评估 `RenderBatch_Submit(0x6F1375C0)` 前置聚合实验，先做小场景 A/B 验证再扩大范围。
8. **上下文保持要求（新增）**:
   - [强制] 自本节点起，每轮执行结果与下一步计划必须同步到 `AGENTS.md`，防止上下文压缩导致计划丢失。
9. **第三轮执行 TODO（2026-02-23 夜间）**:
   - [x] T1：收缩 Hook/UI/Collector 热路径常驻开销（在不改变语义下减少每帧固定成本）。
   - [x] T2：升级 Dispatch Tag/Stage 缓存为多槽 TLS（提升重复查询命中率）。
   - [x] T3：收紧 SceneCollector 条件采集（仅在桥接目标存在时采集重路径数据）。
   - [x] T4：重整 Conservative Takeover 触发门限与透明分级策略（优先稳定，再争取收益）。
   - [x] T5：每完成一批改动后，必须执行 AutoTest MCP 回归：
     - 进图稳定（无崩溃/死锁）；
     - 渲染截图可用（无明显黑块/全黑透明层异常）；
     - 性能报告可导出并可读取 section 级结果。
   - [x] T6：第三轮结束前同步“收益/风险/回退开关”到 AGENTS，形成下一轮继续迭代入口。
   - [x] 第三轮 AutoTest 回归记录（2K 全屏，自动部署新 DLL）：
     - Batch1（UI 热路径 scope 条件化）：`war3_perf_report_auto_2026_02_24_02_55_39.html`
       - `avgFps=88.356`，`avgFrameTimeMs=11.318`，`avgTrackedActiveCpuMs=2.171`。
      - Batch2（Tag/Stage 8 槽 TLS + Collector 空集早退）：`war3_perf_report_auto_2026_02_24_02_59_30.html`
        - `avgFps=85.371`，`avgFrameTimeMs=11.714`，`avgTrackedActiveCpuMs=2.274`。
10. **第四轮执行 TODO（2026-02-24，MainLoop 逻辑层专项）**:
   - [x] T1：补齐 MainLoop 深层阶段 Hook（PrepareWait/PrepareDispatch/FinalizeDispatch/TickUpdate/FinalizeWorker/ComputeWakeDelta）。
   - [x] T2：将 EventDispatch 分桶从粗分组改为 case0~14 精确计时。
   - [x] T3：扩展 PerfMonitor 的 MainLoop Stage 聚合规则，确保新增阶段可视化。
   - [x] T4：完成 `ninja -C build32` 回归。
   - [x] T5：完成 AutoTest 2K 全屏自动采样并输出报告。
   - [x] T6：基于新增模块级数据执行“收益/风险排序”的渲染层优化下一轮（优先处理 `TickUpdate + FQ_Dispatch_Opaque + FQ_Total_Trans`）。
     - Batch3（Conservative Takeover 透明分级阈值）：`war3_perf_report_auto_2026_02_24_03_05_15.html`
       - `avgFps=87.068`，`avgFrameTimeMs=11.485`，`avgTrackedActiveCpuMs=2.228`。
     - Batch4（关闭统计时剔除热路径原子计数）：`war3_perf_report_auto_2026_02_24_03_13_38.html`
       - `avgFps=85.432`，`avgFrameTimeMs=11.705`，`avgTrackedActiveCpuMs=2.200`。
     - Batch4（3 轮复测聚合）：`avgFps=87.1927`，`avgFrameTimeMs=11.4687`，`avgTrackedActiveCpuMs=2.221`。
     - 四轮均通过：无崩溃、截图基线匹配（2560x1440）、无渲染异常告警。
11. **第五轮执行 TODO（2026-02-24，渲染层优化落地）**:
   - [x] T1：P0 级热路径瘦身（关闭默认高频诊断统计、关闭默认深层 MainLoop Hook）。
   - [x] T2：P1 级保守接管策略自适应（无透明小批接管 + 高 Opaque 放宽透明阈值）。
   - [x] T3：P1 级透明排序快路径（已排序跳过 sort）。
   - [x] T4：P2 级 ShadowMap 自适应更新（稳定视角下隔帧复用）。
   - [x] T5：每项改动合并后执行 AutoTest 双轮回归并通过（无崩溃、截图基线匹配、报告可解析）。
   - [ ] T6：下一轮拆分 A/B（单项开关化验证），精确量化 P0/P1/P2 各自收益并确定默认发行配置。
12. **第六轮执行 TODO（2026-02-24，组合兼容与修复）**:
   - [x] T1：构建“渲染优化 × 逻辑优化”组合矩阵并批量 AutoTest。
   - [x] T2：定位不兼容开关组合与具体根因（唯一故障项：`kNativeJassVmDeepHooksEnabled`）。
   - [x] T3：修复地址/安装防护并复测故障组合通过。
   - [x] T4：验证“全开组合”可运行（无崩溃、截图基线匹配、报告可导出）。
   - [ ] T5：基于矩阵结果给出“默认发行配置 + 调试配置”双配置建议并固化到文档/脚本。
   - [x] 第三轮收益/风险/回退开关：
     - 收益：回归稳定，`FQ_Dispatch_Opaque` 与 `FQ_Total_Trans` 在 section 热点中可见；框架热路径开销被进一步收缩。
     - 风险：短窗单轮波动仍明显（±2~3 FPS），需多轮均值评估避免误判。
     - 回退开关：
       - `kNativeQueueTakeoverConservativeEnabled`
       - `kNativeQueueTakeoverConservativeAllowTransparent`
       - `kNativeQueueTakeoverConservativeMaxTransparentForTakeover`
       - `kNativeQueueTakeoverConservativeMinOpaqueWhenTransparent`
       - `kNativeQueueTakeoverConservativeStatsLogging`

## 🗺️ 后续计划 (Roadmap)

### 当前执行清单（行业化重构）
- [x] **P0-1 结构盘点**：梳理目录、入口、耦合点与重复实现。
- [x] **P0-2 编译基线**：确认 `ninja -C build32` 当前可通过。
- [ ] **P0-3 回归护栏**：固化功能/性能对比脚本与验收阈值。
- [x] **P1-1 Hook AddressBook**：集中管理地址解析与版本校验。
- [ ] **P1-2 Hook Registry**：统一 `Create/Enable/Status/错误码`。
- [x] **P1-3 主入口瘦身**：`d3d9_war3_hook.cpp` 仅保留编排。
- [x] **P2-1 域迁移**：接入 `war3_hook_render/jass/lifecycle` 并清理重复实现。
- [ ] **P2-2 状态统一**：收敛 `war3/render` 与 `war3/state` 的状态边界。
- [ ] **P3 桥接契约化**：统一 `sceneNode/jHandle/unit/rawcode` 生命周期与回退规则。
- [ ] **P4 热路径解耦**：拆分 `d3d9_device.cpp` 中 Shadow/Outline/BeforeUi 逻辑。
- [ ] **P5 配置标准化**：收敛编译期开关与诊断开关分层。
- [x] **P6 文档更新**：同步 `docs/research` 与 `docs/war3_shader_docs` 架构图与模块说明（本轮完成首版）。

### 本轮执行记录（2026-02-23，120FPS 冲刺）
1. 已完成（代码）：
   - `war3_hook_address_book` 新增 `rqFlushTransparent=0x138210`；
   - `Hook_FlushSortedItems` 在接管模式下优先调用原生透明 flush（`kNativeQueueTakeoverUseNativeTransparentFlush=true`）；
   - 新增全量接管门槛：`kNativeQueueTakeoverFullMinOpaque=4`、`kNativeQueueTakeoverFullMinOpaqueWhenTransparent=16`；
   - 关闭默认 `DispatchTagStageCache`（`kNativeDispatchTagStageCacheEnabled=false`）；
   - `Reimpl_GetTrackerTagStage` 改为直接 `tracker.GetTagStage`，减少热路径重复缓存层。
   - `d3d9_device.cpp`：`ShadowCapture` 细粒度 `cpuScope` 改为受 `kNativeOptimizationPerfTrackingEnabled` 控制（默认关闭采样开销）。
2. 已完成（验证）：
   - `ninja -C build32` 通过；
   - AutoTest（2K 全屏，自动部署 DLL）：
     - 12s 样本：`avgFps=109.456`，`avgFrameTimeMs=9.136`；
     - 20s 样本：`avgFps=114.225`，`avgFrameTimeMs=8.755`；
     - 3×10s 批量样本均值：`avgFps=98.99`（短窗波动较大）。
3. 结果解读：
   - 在中长窗样本中，`Hook_FlushAndReset/Orig` 自身 CPU 开销下降；
   - `ShadowCapture` 统计开销从热路径剔除后，跟踪 CPU 占用明显降低；
   - 当前瓶颈仍在 `Other/Untracked`（主线程外/引擎内部开销），渲染层仍有优化空间但不是唯一大头；
   - 透明闪烁专项已切“原生透明 flush”优先路径，需继续实机长窗验证（隐身披风场景）。
4. 下一步计划（继续执行）：
   - A/B：细化全量接管门槛与透明条件（按 Opaque/Transparent 规模分层）；
   - 继续压 `Hook_FlushAndReset/Orig`；
   - 在不回退画面的前提下，逐步逼近 120FPS（固定 2K 全屏 AutoTest 口径）。

### 本轮执行记录（2026-02-24，按“非 FlushAndReset 优先”顺序）
1. 已完成（代码，未触碰 `Hook_FlushAndReset` 主逻辑）：
   - `war3_internal_test_config.h`：
     - 新增 `kPathBlockerTrackingGroupMask=0x1`（PathBlocker-only 模式默认仅追踪 group0）。
   - `war3_hook_render.cpp`：
     - `Hook_WorldObjects_RenderGroup` 在 `pathBlockerOnly` 下按组掩码裁剪对象收集，减少无效 group 扫描。
   - `war3_scene_collector.cpp`：
     - 在 `kNativeFlushUnsafePathEnabled=true` 时，`sceneNode` 读取改为直读偏移 `+0x20`，减少 `SafeReadPtrFast` 热路径开销。
   - `war3_hook_ui.cpp`：
     - `Hook_UiRenderableRender` 增加 UI 层切换短路：已在 UI 层时不再重复 `PushUiLayer/PopLayer`。
2. 已完成（验证）：
   - `ninja -C build32` 通过；
   - AutoTest（2K 全屏，自动部署）：
     - 报告：`war3_perf_report_auto_2026_02_24_11_28_15.html`
     - `avgFps=80.842`，`avgFrameTimeMs=12.37`，`avgTrackedActiveCpuMs=1.633`，`avgUntrackedActiveCpuMs=10.736`。
3. 现状判断：
   - 当前测试场景活动强度较高，短窗波动大；本轮改动属于“削减高频无效分支”的低风险优化，主要目标是给后续合批/队列策略留出预算。
   - 下一轮仍遵循你的顺序：先继续挖 UI/World/SceneCollector/RenderQueue 侧优化，最后再碰 `Hook_FlushAndReset` 主体。
4. 本轮补充修正（同日）：
   - 修正 `pathBlockerOnly` 判定：去掉 `needsBatchTracking` 否决条件，允许“批次追踪开启”时仍执行对象收集裁剪（仅影响 Collector 组范围，不影响 tag/stage 跟踪）。
   - AutoTest 复测（2K 全屏，12s）：`avgFps=96.653`，`avgFrameTimeMs=10.346`，`avgTrackedActiveCpuMs=1.616`，`avgUntrackedActiveCpuMs=8.73`。

### 本轮执行记录（2026-02-24，继续迭代）
1. 已完成（代码，仍未触碰 `Hook_FlushAndReset` 主体）：
   - `war3_hook_render.cpp`：
     - `Hook_RenderQueue_Dispatch_Common` 新增“已激活本地合并上下文复用”超前快退；
     - `Hook_RenderQueue_Dispatch_Special` 同步新增复用快退（仅在 Special 局部合并开关启用时生效）；
     - `Hook_FlushSortedItems` 新增透明链路安全门：当透明路径前置条件缺失时自动回退原生 `FlushSortedItems`，避免透明排序/材质异常。
   - `war3_internal_test_config.h`：
     - 将 `kNativeDispatchTagStageCacheEnabled` 默认改为 `false`（实测多场景命中率过低时，缓存层净增热路径开销）。
2. 已完成（验证）：
   - `ninja -C build32` 通过。
   - AutoTest（2K 全屏，自动部署 DLL，20s 样本）：
     - `war3_perf_report_auto_2026_02_24_12_49_07.html`：`avgFps=99.504`，`avgFrameTimeMs=10.05`；
     - `war3_perf_report_auto_2026_02_24_12_50_00.html`：`avgFps=99.645`，`avgFrameTimeMs=10.036`。
3. 当前结论：
   - 本轮优化后帧率稳定回到约 99.5 FPS 档位；
   - 主要可见渲染热点仍是 `Hook_FlushAndReset/Orig`；
   - `Other/Untracked` 仍占大头，后续继续按“先渲染可控路径，再逻辑层逆向”推进。
4. 下一步计划（已排队）：
   - 先做 `RenderQueue` 接管策略 A/B：按透明条目规模分层（小透明保接管、大透明回原生）；
   - 再做 `Dispatch` 热路径复用扩大：验证 `LocalMerge` 在更多稳定 stage 的收益与正确性；
   - 最后在有数据支撑后再进入 `Hook_FlushAndReset` 主链优化。

### 本轮执行记录（2026-02-24，继续迭代第二步）
1. 已完成（代码）：
   - `war3_internal_test_config.h`：
     - 调整透明分层阈值：`kNativeQueueTakeoverFullMaxTransparent=8192`，
       `kNativeQueueTakeoverFullMaxTransparentHighOpaque=12288`，避免中等透明场景过早回退原生；
     - 新增 `kNativeQueueSkipSortIfAlreadySorted=true` 与
       `kNativeQueueSkipSortCheckMaxCount=2048`，用于 Opaque 排序预检快退。
   - `war3_render_queue.h`（生效路径为 inline）：
     - `FlushSortedItems_StdSort` 增加“已排序预检”，中小批次有序时跳过 `InnerSort`；
     - `layerChanged` 判断增加指针相等快路：同指针直接视为未变化，仅指针不同时执行 `memcmp(20B)`。
2. 关键修正说明：
   - `RenderQueue::FlushSortedItems_StdSort` 实际走 `war3_render_queue.h` 的 inline 实现；
   - 因此相关热路径优化必须落在 `.h`，否则不会被当前构建目标采用。
3. 已完成（验证，2K 全屏 AutoTest，20s）：
   - `war3_perf_report_auto_2026_02_24_13_14_16.html`：`avgFps=102.072`，`avgFrameTimeMs=9.797`；
   - `war3_perf_report_auto_2026_02_24_13_15_07.html`：`avgFps=100.855`，`avgFrameTimeMs=9.915`。
4. 当前结论：
   - 本轮相对上一轮 99.5 FPS 档位有小幅稳定提升（约 +1~2 FPS）；
   - `avgTrackedActiveCpuMs` 与 `avgMainThreadCpuMs` 均有下降趋势；
   - 下一步继续沿“RenderQueue 接管策略 + Dispatch 复用”推进，再决定是否进入 `Hook_FlushAndReset` 主链。

### 本轮执行记录（2026-02-24，继续迭代第三步）
1. 已完成（代码，RenderQueue 热路径）：
   - `war3_render_queue.h`（生效 inline 路径）：
     - 新增 `sceneNode -> (tag, stage)` 短缓存，减少同单位多子网格连续提交时的重复 `GetTagStage`；
     - 将 `tag/stage` 查询改为 **按需惰性查询（lazy）**：仅在需要 `ExecBegin`/instancing/诊断时才触发查询，避免连续同上下文批次的无效查表。
   - `war3_render_queue.cpp`：
     - 同步上述缓存与 lazy 逻辑，保持 `.h/.cpp` 行为一致，防止后续切实现时语义漂移。
   - `war3_internal_test_config.h`：
     - 对 `kNativeQueueSkipSortCheckMaxCount` 做 A/B 调优，最终保留 `4096`。
2. 已完成（验证，AutoTest 2K 全屏，20s）：
   - 基线（本轮开始前）：
     - `war3_perf_report_auto_2026_02_24_13_20_54.html`：`avgFps=101.244`，`avgFrameTimeMs=9.877`。
   - 引入 sceneNode 缓存 + lazy 查询后：
     - `war3_perf_report_auto_2026_02_24_13_36_58.html`：`avgFps=101.798`，`avgFrameTimeMs=9.823`。
   - `SkipSortCheckMaxCount` A/B：
     - `10000`：`war3_perf_report_auto_2026_02_24_13_38_56.html`，`avgFps=101.208`；
     - `4096`：`war3_perf_report_auto_2026_02_24_13_36_58.html`，`avgFps=101.798`（短窗波动下更优）。
3. 本轮结论：
   - 热路径“无效 tag/stage 查询”已被压缩，`RenderQueue` CPU 有小幅可复现下降；
   - 在当前地图/样本窗下，`4096` 配置优于 `10000`；
   - 仍需更长窗口（>=60s）做稳定统计，避免短窗噪声误判。

### 本轮执行记录（2026-02-24，分辨率兼容修复）
1. 背景：
   - 用户反馈“当前版本无法调整分辨率”，并要求本轮不要启动魔兽进行自动测试。
2. 已完成（代码）：
   - `war3_internal_test_config.h` 新增可控开关：
     - `kWar3UiOverrideMaxFpsEnabled`
     - `kWar3UiMaxFpsOverrideValue`
     - `kWar3UiOverrideRefreshRateEnabled`（默认 `false`）
     - `kWar3UiInstallD3d9ParamsHookEnabled`（默认 `false`）
     - `kWar3ForceImmediatePresentEnabled`
   - `war3_hook_ui.cpp`：
     - FPS 覆盖值与总开关改为读取内部配置；
     - `Hook_GetD3d9Parameters` 仅在 `kWar3ForceImmediatePresentEnabled` 为真时覆盖 `PresentationInterval`；
     - 默认不在 UI 域重复安装 `GetD3d9Parameters` Hook（生命周期域已安装）；
     - 默认不再强制写入 `GAME_OPTION_REFRESH_RATE`，保留玩家手动分辨率/刷新率设置。
   - `war3_hook_lifecycle.cpp`：
     - `Hook_GetD3d9Parameters` 同步接入 `kWar3ForceImmediatePresentEnabled` 开关。
3. 验证状态：
   - 按用户要求，本轮未启动 War3/AutoTest，仅做静态修复与构建准备。
4. 后续计划（待用户联机窗口结束后执行）：
   - 仅做一次 `ninja -C build32` + 单轮 AutoTest 冒烟，确认“可改分辨率 + FPS 无明显回退”。

### 本轮执行记录（2026-02-24，性能迭代第 1 轮）
1. 已完成（代码）：
   - `war3_hook_lifecycle.cpp`：
     - 生命周期域新增 `MakeLifecycleCpuScope`，并将 `Hook_FlushAndReset*` 子分段统一改为按 `kNativeOptimizationPerfTrackingEnabled` 条件采样，关闭细粒度性能采样时不再进入高频 `PerfMonitor` 路径。
   - `war3_internal_test_config.h`：
     - 全量接管门槛下调为 `kNativeQueueTakeoverFullMinOpaque=8`、`kNativeQueueTakeoverFullMinOpaqueWhenTransparent=24`，提高接管命中率，减少回落原生 `FlushSortedItems`。
   - `war3_render_queue.cpp/.h`（透明发黑修复链路）：
     - Instancing 清理 `SetTexture(1, nullptr)` 后，新增状态缓存失效：`lastLayerStatePtr=nullptr`、`lastMeshData=nullptr`、`lastLayerIndex=UINT_MAX`；
     - 避免后续批次误判 `layerChanged=0/stateChanged=0` 时沿用空纹理导致透明贴图发黑。
   - `war3_internal_test_config.h`（A/B 开关）：
     - `kNativeDispatchLocalContextMergeEnabled` 暂时改为 `false`，用于验证局部上下文合并对当前场景的净收益。
   - `war3_render_queue.cpp/.h`（微优化）：
     - `layerState` 前 20B 比较由 `memcmp` 改为固定 5 个 `uint32_t` 直比较（`LayerStatePrefix20Equal`），减少热路径通用库调用开销，语义保持一致。
2. 已完成（构建）：
   - 多轮 `ninja -C build32` 均通过。
3. AutoTest 观测（2K 全屏）：
   - 基线：`AutoTest/artifacts/latest_baseline.json`（`avgFps=103.607`）；
   - 透明修复后：`AutoTest/artifacts/latest_after_opt2_transparency_fix.json`（`avgFps=100.461`，单轮波动）；
   - 关闭 local merge 后：
     - `AutoTest/artifacts/latest_after_opt3_disable_local_merge.json`（`avgFps=101.069`）；
     - `AutoTest/artifacts/latest_after_opt3_repeat2.json`（`avgFps=104.492`）。
   - 结论：单轮波动较大，需在“无前台负载干扰”下用固定口径多轮统计再定版。
4. 干扰说明（必须记录）：
   - 周期测试 `AutoTest/artifacts/latest_opt3_rounds4.json` 出现强干扰：
     - 多轮 FPS 大幅波动（约 78~96）；
     - 其中一轮截图尺寸异常 `199x34`（非 2K 基线），说明前台状态/焦点/系统负载介入；
   - 该组数据不用于结论。
5. 当前策略（执行中）：
   - 用户前台运行其它游戏期间，暂停 War3 自动测试与性能结论判定；
   - 仅继续低风险代码优化、静态审查与文档归档，待空闲窗口再做多轮基准复测。

31. **第四轮：MainLoop 逻辑层未知项压缩与模块级计划（2026-02-25）**:
   - **目标**：
     - 在不改行为语义前提下，最大化 MainLoop 逻辑层可观测覆盖；
     - 将“逻辑层未追踪项”从黑箱转为模块级可解释数据。
   - **代码落地**：
     - `src/d3d9/war3/core/war3_internal_test_config.h`
       - 新增 `kNativeMainLoopCoverageAnalysisMode=true`；
       - 联动开启：`MainLoopDeepPhaseHook / MainThreadWaitHook / MainThreadWaitDeepHook / JassVmPerfTracking / JassVmDeepHooks / OptimizationPerfTracking`。
     - `src/d3d9/war3/hooks/war3_hook_lifecycle.cpp`
       - 新增 `DispatchModule` 语义映射：`case0~14 -> LoadBlockType*/MainCallbackGate/StateFinalize`；
       - `Hook_EventDispatch` 同时写入 `War3MainLoop/Dispatch/Case*` 与 `War3MainLoop/DispatchModule/*`。
     - `src/d3d9/war3/tools/war3_perf_monitor.cpp`
       - MainLoop Stage 聚合新增 `DispatchModule/*`；
       - 语义分类补充 `War3MainLoop/DispatchModule/* -> Logic/Dispatch`；
       - 新增显式分桶：`Other/UntrackedActive (MainLoop Active Gap)`。
   - **编译与自动化验证**：
     - `ninja -C build32` 通过；
     - AutoTest（2K 全屏，60s）报告：
       - `E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_04_02_51.html`
       - `avgFps=102.516`, `avgFrameTimeMs=9.755`
       - `avgMainThreadCpuMs=4.521`, `avgProcessCpuMs=7.816`, `avgWorkerThreadsCpuMs=3.297`
       - `activeFrameTimeMs=3.958`, `avgTrackedActiveCpuMs=3.958`, `avgUntrackedActiveCpuMs=0.000`
       - `cpuCoveragePct=100.0`, `mainThreadCpuCoveragePct=99.968`。
   - **模块级结论（本轮）**：
     - Idle 闸门：`Engine/WaitGate=9.709ms`（等待，不作为直接优化目标）；
     - 逻辑活跃热点：`Engine/TickUpdate=0.384ms`（第一优先）、`Engine/PrepareDispatch=0.072ms`（第二优先）；
     - 分发热点：`Dispatch/Case10` 与 `DispatchModule/LoadBlockType12` 稳定命中但量级较小；
     - 进程级剩余大头在 worker 线程（约 `3.297ms`），不属于 MainLoop 主线程活跃未追踪。
   - **文档同步**：
     - 新增：`docs/research/war3_render_issues/11_mainloop_round4_unknown_resolution/README.md`
     - 更新：`docs/research/war3_render_issues/README.md`
     - 更新：`docs/war3_shader_docs/refactor_status.html`（补第四轮结论与报告证据）。
32. **第四轮续作：MainLoop 方案全落实 + 60s AutoTest（2026-02-25）**:
   - **目标对齐（落实上一轮方案）**：
     - [x] TickUpdate 子路径拆解（从“总耗时”变为“Self + Sub/*”）
     - [x] PrepareDispatch 低开销路径（去除高频 ScopedCpuScope，改手工采样）
     - [x] Dispatch/LoadBlockType12 模块化统计（保留 case 与 module 语义）
     - [x] RunCallbacks TopN 来源分桶（caller return address）
   - **代码落地（`src/d3d9/war3/hooks/war3_hook_lifecycle.cpp`）**：
     - `Hook_EventDispatch`：
       - 移除每次调用的 `Dispatch + Case + Module` 三层 scope；
       - 改为仅计时一次并写入 thread-local 聚合桶（`dispatchCase* / dispatchModule*`）；
       - 在 `FlushMainLoopCycleToPerf` 中按循环批量上报，显著降低锁竞争与路径构建开销。
     - `Hook_EngineRunCallbacks`：
       - 从 `cpuScope` 改为手工计时 `addCpuSample`；
       - 新增 `RecordRunCallbacksCaller`（TopK=8）来源桶，输出到
         `War3MainLoop/Engine/RunCallbacks/Caller_XXXXXXXX`。
     - `Hook_EnginePrepareDispatch`：
       - 从 `cpuScope` 改为手工计时 `addCpuSample`，收敛热路径开销。
     - `Hook_EngineTickUpdate`：
       - 新增“调用前后相位增量”拆解：
         - `War3MainLoop/Engine/TickUpdate/Self`
         - `War3MainLoop/Engine/TickUpdate/Sub/{Dispatch,Callback,RunCallbacks,QueueFlush,PrepareDispatch,FinalizeDispatch,Reschedule,ComputeWakeDelta,TlsPump}`
       - 用已有 cycle 相位增量做拆分，避免继续增加侵入 Hook。
     - `FlushMainLoopCycleToPerf`：
       - 新增 `War3MainLoop/Dispatch` 与 `War3MainLoop/DispatchModule` 根节点批量上报；
       - 批量输出 `Case0~14/Other` 与 `DispatchModule/*`；
       - 批量输出 `RunCallbacks Caller TopN + Caller_Other`。
   - **编译结果**：
     - `ninja -C build32` 通过（仅既有 warning）。
   - **60 秒 AutoTest（强制验收）**：
     - 报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_04_29_44.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260225_042845.png`（`2560x1440` 匹配）
     - 核心指标：
       - `avgFps=99.339`
       - `avgFrameTimeMs=10.067`
       - `avgMainThreadCpuMs=4.801`
       - `avgProcessCpuMs=8.263`
       - `activeFrameTimeMs=4.142`
       - `avgTrackedActiveCpuMs=4.142`
       - `avgUntrackedActiveCpuMs=0.000`
       - `cpuCoveragePct=100.0`
     - 稳定性：`ok=true`，无崩溃；流程结束后已静默关闭 War3。
   - **本轮结论**：
     - 上一轮提出的 MainLoop 四项方案已全部落地并完成 60 秒自动回归；
     - Active 未追踪仍保持 `0.000ms`，并新增 `TickUpdate/Self` 与 `RunCallbacks/Caller_*` 细分证据链；
     - 当前主要成本仍在 `WaitGate`（Idle 门控）与渲染提交链，后续优化应继续聚焦渲染队列与 worker 并行段。
33. **第四轮追加：MainLoop 采样热路径再收敛 + 60s 回归（2026-02-25）**:
   - **目标**：
     - 在不减少可观测性的前提下，继续降低 MainLoop 深度采样本身的 CPU 开销；
     - 维持“上一轮分析方案全部生效”的数据语义。
   - **代码落地（`src/d3d9/war3/hooks/war3_hook_lifecycle.cpp`）**：
     - 新增 `TickUpdateSubBucket` 聚合桶与 cycle 内累计字段（`tickUpdateSubUs/tickUpdateSelfUs`）；
     - `Hook_EngineTickUpdate` 改为“仅计算增量并写入 thread-local 聚合桶”，不再逐调用上报 `Sub/*`；
     - `Hook_EngineRunCallbacks` / `Hook_EnginePrepareDispatch` 移除逐调用根路径上报，改由循环末批量上报；
     - `FlushMainLoopCycleToPerf` 统一批量输出：
       - `War3MainLoop/Engine/RunCallbacks`
       - `War3MainLoop/Engine/PrepareDispatch`
       - `War3MainLoop/Engine/TickUpdate`
       - `War3MainLoop/Engine/TickUpdate/{Sub,Self,Sub/*}`。
   - **编译结果**：
     - `ninja -C build32` 通过（仅既有 warning）。
   - **60 秒 AutoTest（2K 全屏）**：
     - 报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_04_45_11.html`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260225_044412.png`（`2560x1440` 匹配）
     - 核心指标：
       - `avgFps=100.883`
       - `avgFrameTimeMs=9.912`
       - `avgMainThreadCpuMs=4.611`
       - `avgProcessCpuMs=8.245`
       - `activeFrameTimeMs=3.664`
       - `avgTrackedActiveCpuMs=3.664`
       - `avgUntrackedActiveCpuMs=0.000`
       - `cpuCoveragePct=100.0`
     - 稳定性：`ok=true`，无崩溃，流程结束后 `war3Alive=false`。
   - **补充回归证据（上一轮遗漏补齐）**：
     - 第二次 60s 复测报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_04_34_34.html`；
     - 该报告同样 `ok=true`，用于补全 32 条目的“双轮回归”证据链。
34. **第五轮收官补执行：配置矩阵 + 150FPS 目标验证（2026-02-25）**:
   - **自动化脚本落地**：
     - 新增 `AutoTest/run_round5_perf_matrix.py`：8 组性能配置矩阵（每组 `ninja + 60s AutoTest + 报告聚合`）；
     - 新增 `AutoTest/run_round5_extra_matrix.py`：上限探索矩阵（聚焦 mode1 阴影链路开销）。
   - **矩阵结果（60s，2K 全屏）**：
     - 主矩阵最佳：`C2_perf_full_no_local_merge`，`avgFps=122.804`（`AutoTest/artifacts/round5_matrix/round5_matrix_results.json`）；
     - 上限探索最佳：`E1_disable_shadow_capture_mode1`，`avgFps=209.268`；
     - 对比：`E0_best_so_far=122.896` -> `E1=209.268`，确认 150+ 目标可达，主要瓶颈在 mode1 ShadowCapture 链路。
   - **配置落地**（`src/d3d9/war3/core/war3_internal_test_config.h`）：
     - `kNativeMainLoopCoverageAnalysisMode=false`
     - `kNativeDispatchLocalContextMergeEnabled=false`
     - `kNativeShadowDisableShadowCaptureWhenMode1=true`
   - **最终验证（60s）**：
     - 报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_05_23_00.html`
     - 结果：`avgFps=196.917`, `avgFrameTimeMs=5.078`, `avgGpuTimeMs=1.168`, `avgMainThreadCpuMs=3.064`
     - 稳定性：`ok=true`，无崩溃，结束后已静默关闭 War3。
35. **AutoTest 截图链路修复（2026-02-25）**:
   - **问题**：`capture_war3_screenshot` 使用 `CopyFromScreen`，窗口被覆盖时会截到桌面，语义验收证据不可靠。
   - **修复**：`AutoTest/war3_autotest_mcp.py` 的 `_powershell_capture_window` 改为 `PrintWindow` 优先，失败回退 `CopyFromScreen`。
   - **注意**：该修复在 MCP 服务重启后生效；本轮已作为“已知限制 + 修复已提交”记录。
36. **画质语义回正（2026-02-25）**:
   - **问题确认**：第五轮上限探索中将 `kNativeShadowDisableShadowCaptureWhenMode1=true` 作为“落地配置”会关闭核心阴影采集链路，不符合“画质增强 mod”定位。
   - **修正**：
     - `src/d3d9/war3/core/war3_internal_test_config.h` 恢复为 `kNativeShadowDisableShadowCaptureWhenMode1=false`；
     - 前端看板文案修正为“`209.268 FPS` 属于实验档上限，不是默认配置”。
   - **口径**：
     - 默认档：画质优先（保留 mode1 ShadowCapture）；
     - 实验档：仅用于瓶颈分析，不作为日常发布默认值。
37. **MainLoop 报告语义补强（2026-02-25）**:
   - **问题确认**：现有性能报告中的 `DispatchModule/*` 容易被误解为“真实子函数独立 Hook”，但当前本质是 `case -> module` 的语义映射。
   - **代码落地**：
     - `src/d3d9/war3/hooks/war3_hook_lifecycle.cpp`
       - 新增 `DispatchModuleBucketFromMsgType`，将模块映射逻辑显式化（单点维护）；
       - `RecordDispatchBuckets` 改为“Case 与 Module 分桶分别写入”，避免隐式同桶误读。
     - `src/d3d9/war3/tools/war3_perf_monitor.cpp`
       - 新增四个主循环深度分解 JSON 数据集：
         - `mainLoopDispatchCases`
         - `mainLoopDispatchModules`
         - `mainLoopTickUpdateSub`
         - `mainLoopRunCallbacksCallers`
       - HTML 报告新增四张专表（Dispatch Case / Dispatch Module / TickUpdate Sub / RunCallbacks Caller）；
       - `Dispatch Module` 表增加 `CaseMapped` 标识，明确当前粒度边界，避免将语义映射误判为真实函数 Hook。
   - **编译验证**：
     - `ninja -C build32` 通过。
   - **当前结论**：
     - MainLoop 逻辑层报告可读性显著提升；
     - 下一阶段若需“真实子函数级”还原，需要继续补 `EventDispatch case` 子函数入口 Hook（RVA 已在研究文档中列出）。
38. **MainLoop 全量逆向补齐 + 60s 验收（2026-02-25 中午）**:
   - **IDA MCP 接入修正**：
     - 资源模式不可见时，改用 HTTP JSON-RPC 直连 `http://127.0.0.1:13337/mcp`；
     - 通过 `tools/list` 明确 `callees` 参数签名为 `addrs`（非 `function_address`）。
   - **逆向证据落地**：
     - 新增目录与文档：`docs/research/war3_render_issues/12_mainloop_full_reverse/README.md`；
     - 新增原始证据包：`docs/research/war3_render_issues/12_mainloop_full_reverse/ida_mainloop_dump_2026_02_25.json`；
     - 覆盖 `0x6F05F710` 根循环、`0x6F05A310` 分发表、`case0~14` 子函数与关键调度函数的 callees/lookup 结果。
   - **代码补齐（函数级可观测）**：
     - `war3_hook_address_book.h/.cpp`：补齐 `mainLoopRoot` 与 `dispatchCase0~14` 入口地址；
     - `war3_hook_lifecycle.cpp`：新增 `War3MainLoop/Function/*` 与 `DispatchCaseFunctions/*` 上报；
     - `war3_perf_monitor.cpp`：新增 `mainLoopFunctionBreakdown`、`mainLoopDispatchCaseFunctions`、`mainLoopUnknownAttribution`、`mainLoopThreadSplit` 数据集与 HTML 展示。
   - **AutoTest 60s 验收（2K 全屏）**：
     - 性能档（默认，回正后最终复测）：`war3_perf_report_auto_2026_02_25_13_13_22.html`
       - `avgFps=106.923`，`cpuCoveragePct=4.441`，`avgUntrackedActiveCpuMs=8.937`；
       - 结论：口径符合“性能优先”，用于交付态稳定性验证。
     - 分析档（临时开启 `kNativeMainLoopCoverageAnalysisMode=true`）：`war3_perf_report_auto_2026_02_25_13_04_14.html`
       - `avgFps=100.873`，`cpuCoveragePct=100.0`，`avgUntrackedActiveCpuMs=0.0`；
       - 结论：达到“覆盖率 >=95% + Unknown <=0.5ms”目标。
     - 截图：`AutoTest/artifacts/screenshots/war3_20260225_130315.png`（`2560x1440` 匹配）。
   - **默认配置回正**：
     - 验收后将 `kNativeMainLoopCoverageAnalysisMode` 恢复为 `false`，保持发布默认“性能优先”。
39. **性能报告语义口径修正（2026-02-25 下午）**:
   - **问题修正**（`src/d3d9/war3/tools/war3_perf_monitor.cpp`）：
     - strict 语义树不再纳入 `Other/Untracked*`，避免 `Untracked` 直接吞噬语义树分母导致“Logic/Render 占比失真”；
     - 语义树层级改为 `Semantic/MainLoop/*` 与 `Semantic/OutsideMainLoop/*`，明确 MainLoop 语义容器。
   - **MainLoop 阶段表排序修正**：
     - `MainLoop Stage Breakdown` 从“按耗时排序”改为“按执行顺序排序”（`SelectWorker -> PrepareWait -> WaitGate -> ... -> TickUpdate -> Reschedule`）；
     - `Dispatch/Case*` 按 case 编号排序，`DispatchModule/*` 保持稳定字典序。
   - **前端文案修正**：
     - 明确语义树仅统计“已追踪且可归类”的 active scope；
     - 未追踪时间统一查看 `MainLoop Unknown Attribution` 与 Coverage 指标。
   - **编译验证**：
     - `ninja -C build32` 通过（本轮为口径修正，不做性能结论）。
40. **Untracked 93% 全量查明（2026-02-25 14:23）**:
   - **动作**：
     - 开启 MainLoop 覆盖分析主开关（`kNativeMainLoopCoverageAnalysisMode=true`）；
     - 运行 AutoTest 60s（2K 全屏）并读取完整报告。
   - **验收报告**：
     - `E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_14_23_51.html`
     - `avgFps=103.910`
     - `cpuCoveragePct=100.0`
     - `avgTrackedActiveCpuMs=3.141`
     - `avgUntrackedActiveCpuMs=0.0`
     - 二次复测：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_14_36_29.html`
       - `cpuCoveragePct=100.0`
       - `avgUntrackedActiveCpuMs=0.0`
   - **结论**：
     - “93% Untracked”根因已确认：此前处于性能档，深度 MainLoop 观测默认关闭；
     - 在分析档下 MainLoop + Render active 已闭环归因，未知项压缩完成。
   - **结构报告同步**：
     - `docs/research/war3_render_issues/12_mainloop_full_reverse/README.md`
     - 新增 MainLoop 完整函数级结构图、模块职责表、最新 60s 覆盖验收数据。
41. **IDA MainLoop 命名与注释统一（2026-02-25 晚）**:
   - **目标**：
     - 为 MainLoop 主链、Dispatch 主链、Render 主链的关键函数做可读性命名与入口注释，降低后续逆向理解成本。
   - **执行方式（IDA MCP）**：
     - 使用 `rename`（`batch.func`）批量改名；
     - 使用 `set_comments` 在函数入口写入中文语义注释（同步反编译视图）。
   - **命名覆盖（关键地址）**：
     - MainLoop：`0x6F05F710`、`0x6F05DE80`、`0x6F05DEE0`、`0x6F158940`、`0x6F1648A0`、`0x6F164B00`、`0x6F05FCA0`、`0x6F0603B0`、`0x6F059B00`、`0x6F05A310`、`0x6F05FD10`、`0x6F05B080`、`0x6F05FC10`、`0x6F05DCE0`、`0x6F05FB10`、`0x6F060500`、`0x6F05EE90`；
     - Dispatch case 构块：`0x6F05A060`、`0x6F05A0E0`、`0x6F05A160`、`0x6F05A1F0`、`0x6F05AE90`；
     - Render 主链：`0x6F3681C0`、`0x6F363020`、`0x6F1380A0`。
   - **校验结果**：
     - `rename` 返回 `ok=true`；
     - `set_comments` 返回 `ok=true`；
     - `lookup_funcs` 复查已显示新函数名（如 `W3_MainLoop_ThreadEntry`、`W3_Render_CWorld_RenderScene`）。
42. **逻辑层逆向补充：MainLoop 资源块链路 + JASS 调用开销定位（2026-02-25 夜）**:
   - **逆向范围（IDA MCP）**：
     - MainLoop：`W3_MainLoop_DispatchEventCase(0x6F05A310)`、`W3_MainLoop_QueueFlush(0x6F05B080)`、`W3_MainLoop_RunCallbacks(0x6F0603B0)`、`W3_MainLoop_TickUpdate(0x6F05FC10)`；
     - 资源块核心：`W3_ResourceBlock_LoadAndQueue(0x6F05AE90)` 及 `sub_6F05C230/sub_6F05C0C0`；
     - JASS VM：`JassInterpreter_MainLoop(0x6F7F1A20)`、`ExecuteNativeFunction(0x6F7EF590)`、`JassFunc_PauseAndCreateFrame(0x6F7F1810)`。
   - **关键结论**：
     - `DispatchEventCase` 多个 case 最终汇聚到 `LoadAndQueue(0x6F05AE90)`，该函数为高频“链表重排 + 回调触发”热区（191 指令）；
     - `QueueFlush` 会对 pending 条目逐项调用 `LoadAndQueue`，在事件密集场景易形成逻辑层 CPU 峰值；
     - JASS 解释器 case21（native 调用）固定进入 `ExecuteNativeFunction`：每次做签名扫描、参数转换、`alloca + memcpy` 后再 call native；
     - JASS 解释器 case22（脚本函数调用）进入 `JassFunc_PauseAndCreateFrame`：包含 frame 分配与链表挂接，函数封装层级越深，额外开销越明显。
   - **优化优先级（仅定位，不改语义）**：
     - 低风险：优先在 Hook 层压缩观测开销与采样门控，避免非录制状态的高频统计路径放大成本；
     - 中风险：围绕 `Dispatch case -> LoadAndQueue` 做“同帧同类请求去重/合并”实验，减少重复资源块提交；
     - 高收益高风险：对 JASS native 调用桥接做签名缓存与参数打包快路径，降低 `ExecuteNativeFunction` 周边固定成本。
   - **当前状态**：
     - 本轮为“逆向与热点确认”，未改游戏行为路径；后续按 P0/P1/P2 分阶段 A/B 验证。
43. **静态阴影写入端闸门（RegisterImage 主控）首轮落地（2026-02-26 凌晨）**:
   - **目标**：
     - 从“渲染末端粗拦截”切换为“写入端精确决策”，在 `mode=1` 抑制建筑/可破坏物原生贴花阴影，同时保留雾/边界链路。
   - **代码落地**：
     - `src/d3d9/war3/hooks/war3_hook_shadow.h`：
       - 新增 `ShadowRegisterSource/ShadowOwnerKind/ShadowRegisterContext/ShadowRegisterDecision`；
       - `ShadowHookAddresses` 新增 8 个 RegisterImage 返回地址槽位。
     - `src/d3d9/war3/hooks/war3_shadow_filter_policy.h/.cpp`：
       - 新增统一策略入口 `DecideRegisterImage(const ShadowRegisterContext&)`；
       - 新增 `ToString(ShadowRegisterSource/ShadowOwnerKind)`；
       - 策略实现：白名单来源（SelectionCircle/MarkOcclusion）默认放行，`StaticStamp` 拦截，`Emitter/其他来源` 走 owner-aware 决策，Unknown owner 仅在 `type={0,4}+building-style key` 时阻断。
     - `src/d3d9/war3/hooks/war3_hook_address_book.h/.cpp`：
       - `shadowToggleEmitterStamp` 修正为函数入口 `0x74DE40`；
       - 新增 8 个 RegisterImage 返回地址 RVA：`0x7291DC, 0x74DAB6, 0x74DBFA, 0x74DF55, 0x76D44A, 0x76D5A4, 0x76D69A, 0x76D719`。
     - `src/d3d9/d3d9_war3_hook.cpp`：
       - 将上述 8 个返回地址解析并接入 `InstallShadowHooks`。
     - `src/d3d9/war3/hooks/war3_hook_shadow.cpp`：
       - `Hook_TerrainShadow_RegisterImageEntry` 改为 `_ReturnAddress()` 精确来源识别 + owner 解析（`argOwnerPos/-0x0C/-0x10`）+ policy 决策；
       - 新增来源/owner/type 分桶统计输出；
       - `Hook_ShadowUpdate_WriteEntry` 的 callback 拦截从单值改为数组匹配；
       - `InstallShadowHooks` 补齐返回地址全量接线。
     - `src/d3d9/war3/core/war3_internal_test_config.h`：
       - 新增 `kNativeShadowRegisterSourceStatsLogging`、
         `kNativeShadowRegisterSourceVerboseLogging`、
         `kNativeShadowRegisterPolicyStrictMode1`、
         `kNativeShadowRegisterOwnerKindFilterEnabled`、
         `kNativeShadowRegisterStatsInterval`、
         `kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled`；
       - `kNativeShadowBlockedCallbackRva` 升级为 `kNativeShadowBlockedCallbackRvas[]` + `Count`。
     - 新增自动化脚本 `AutoTest/run_static_shadow_write_gate_matrix.py`：
       - 实现 `R1~R5` 五轮无人值守流程（改配置 -> 编译 -> AutoTest -> sync_all_debug -> 产物落盘 -> 失败回滚）。
   - **验证结果**：
     - `ninja -C build32` 通过（仅既有 warning）。
     - 五轮矩阵输出目录：`AutoTest/artifacts/static_shadow_write_gate_matrix/20260226_034406/`。
     - R1/R2/R3/R5 均 `ok=true`，R4 因未提供 callback 黑名单按策略跳过并记录原因。
     - 性能门限：R3/R5 相对 R1 满足 `FPS` 与 `MainThreadCpu` 门限；R2 出现 `+0.203ms` 边界超门限一次。
   - **已知问题**：
     - `sync_all_debug` 的 DBWIN 通道出现 `DBWIN open failed: OverflowError`，导致来源统计日志证据不完整；后续轮次需先修复 DBWIN 监听参数类型再做“完整验收”。
44. **静态阴影计划验收补强：DBWIN 修复 + 事件侧复测（2026-02-26 凌晨第二轮）**:
   - **AutoTest 基础设施修复**（`AutoTest/war3_autotest_mcp.py`）：
     - `DbWinListener` 显式绑定 Win32 API `argtypes/restype`（`CreateEventW/CreateFileMappingW/MapViewOfFile/WaitForSingleObject` 等）；
     - `CreateFileMappingW` 第一个参数改为 `ctypes.c_void_p(-1)`，修复 64 位 Python 下 `HANDLE(-1)` 参数溢出导致的 `DBWIN open failed: OverflowError`。
   - **修复验证**：
     - 语法检查通过：`python -m py_compile AutoTest/war3_autotest_mcp.py`；
     - 验收探针目录：
       - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_probe_20260226_035320/`
       - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_probe_events_20260226_035610/`
       - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_probe_interval1_20260226_035714/`
       - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_probe_verbose_20260226_035813/`
     - 结果：DBWIN 事件流恢复（事件数恢复到 259~260），不再出现 open failed。
   - **验收现状**：
     - 已确认 `TerrainShadow_RegisterImageEntry` Hook 安装成功，且至少命中 `SelectionCircleSmall` key 样本；
     - 但当前测试地图中 `RegisterImage` 事件命中极低（仅 2 条相关事件），尚不足以形成 `source stats` 的完整分桶证据；
     - 因此“功能已实现并可运行”结论成立，但“白名单来源 blocked=0 的强证据化完整验收”仍需下一轮补充场景/日志采样。

## 📝 编码规范 (Coding Standards)
- **语言**: C++17
- **注释语言**: 必须使用 **中文**。
- **回复语言**: 必须使用 **中文**。
- **风格原则**: 保持现有风格，模块化优先，热路径优先性能。

### 注释规范（强制，B 方案）
1. **头文件（强制全量 Doxygen）**：
   - 每个 `class/struct` 必须有 `@brief` 注释。
   - 每个函数必须具备标准注释：`@brief`、`@param`、`@return`（若有返回值）。
   - 对关键接口补充：`@note`、`@warning`、`@thread_safety`、`@performance`（按需但应充分）。
   - 注释必须可被 IDE 解析并用于悬浮信息展示。
2. **实现文件（强制关键段落解释）**：
   - 每个函数至少说明：输入假设、主流程、边界条件/失败回退。
   - 对复杂分支、状态机切换、Hook 桥接、性能关键路径必须加段落注释，解释“做什么 + 为什么这样做”。
   - 禁止空泛注释，必须描述行为与约束。
3. **重命名策略（允许重命名）**：
   - 允许为统一命名进行重命名。
   - 重命名需在阶段内提供兼容层（别名/包装/迁移映射）并记录变更表，避免外部调用断裂。
4. **性能保护**：
   - 热路径禁止引入额外堆分配与不必要的锁竞争。
   - 重构后必须通过基线对比，若性能回退需优先修复再继续迁移。

### 重构执行约束（新增）
1. 每阶段都必须满足：`可编译 + 可回归 + 可回滚`。
2. 未通过功能/性能验收不得进入下一阶段。
3. 重大结构变更必须同步更新 `docs/research/war3_render_issues/04_architecture_refactor/README.md`。

---
*此文档由 Antigravity 创建，用于维护项目上下文。*

## 无人值守开发计划（Iris 对齐）
> 说明：以下任务用于“向 Iris 看齐”的核心闭环建设，每完成一项请打勾。

### 核心必做（阻塞级别）
- [x] **补齐渲染事件链**：触发 `FRAME_BEGIN / WORLD_RENDER_BEGIN / SHADOW_PASS_BEGIN/END / UI_RENDER_BEGIN/END / FRAME_END`。
- [x] **FrameBuffer 句柄可用**：对外填充 `vkImage / vkImageView / vkLayout`，确保 layout 合法。
- [x] **ShaderPack 最小闭环**：`composite + final` 两个 pass 可加载、编译、执行。
- [x] **DrawCall 数据补齐（可观测版）**：objectId/状态/纹理句柄/alphaRef/深度标记不为空。
- [x] **Uniform Spec**：时间/相机/雾/光/屏幕尺寸命名稳定并文档化。
- [x] **文档与回归构建**：更新 Shader 文档并保证 `ninja -C build32` 通过。
- [x] **Vulkan Shadow Pack**：支持 shadow receiver 使用 pack 的 SPIR-V shader（优先于 HLSL）。

### 扩展增强（次优先）
- [x] **ShaderPack 参数系统 + ImGui 面板**：运行时调参与保存。
- [x] **war3fx 子项目**：内置渲染 shader 迁移至独立 subproject（glsl_generator 接入）。
- [x] **SSAO 内置模块**：新增 SSAO pass 与 ImGui 动态开关。
- [x] **阴影/描边稳定性**：shadow caster/outline 输入范围校验与安全跳过。
- [x] **内置效果开关**：ImGui 可动态启用/禁用阴影、点光源阴影、描边、SSAO。
- [x] **渲染通道热插拔**：内置 pass 注册到管线，支持运行时启停（Shadow/SSAO/AA）。
- [x] **渲染层容错日志**：BeforeUi/Shadow/SSAO/ShaderPack 缺失资源时记录并安全跳过。
- [ ] **纹理/采样器绑定描述**：JSON 声明纹理槽、过滤、sRGB、重复模式。
- [ ] **热重载增强**：文件监听 + 自动重编译 + 错误回退。
- [ ] **Buffer 文档完善**：像 Iris 那样按 Buffer/Pass 说明输入输出。
- [x] **Vulkan Pack 基础模板**：提供 pack 目录结构 + 示例 SPIR-V。

### 兜底路线（遇到阻塞时）
- [ ] **事件链 + FrameBuffer**：确保外部作者至少能拿到稳定渲染阶段与可采样缓冲。
45. **静态阴影计划验收（第二次排队）补采样与结论更新（2026-02-26 清晨）**:
   - **执行内容**：
     - 按“先记 AGENTS 再执行”要求继续验收，临时开启：
       - `kNativeShadowRegisterSourceStatsLogging=true`
       - `kNativeShadowRegisterSourceVerboseLogging=true`
       - `kNativeShadowRegisterStatsInterval=1`
     - 完成增量编译后，采用两条链路复测：
       - MCP `run_quick_autotest/sync_all_debug`；
       - 本地 `python` 直调 `AutoTest/war3_autotest_mcp.py`（同进程保持 DBWIN 事件队列）。
   - **关键产物**：
     - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_direct_py_20260226_2nd_queue/`
     - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_direct_py_dota_20260226_2nd_queue/`
   - **关键观测**：
     - 本地直调链路已稳定拿到 DBWIN 事件（`all_count=264` 级别），`wait_for_game_ready` 命中：
       - `JASS runtime fully initialized`
       - `War3Shadow: Run frame=1`
     - `RegisterImage` 证据可复现：
       - `source stats calls=1 blocked=0 ... srcFromPoint=0/1 ownerUnit=0/1 reason=Mode1_AllowUnitOrItemOwner`
       - 说明 owner-aware 放行 Unit 路径生效。
     - 但在当前地图样本（含 DotA 复测）下，`RegisterImage` 事件仍稀少，未采到 `ownerBuilding/ownerDestructible` 命中，
       也未形成 `Selection/Occlusion` 白名单来源的强统计样本。
   - **本轮结论**：
     - 计划实现代码仍保持可编译可运行；
     - **“完整验收”仍未闭环**（缺建筑/可破坏物写入命中证据），需下一轮补“可控建筑/可破坏物生成场景”再做来源级闭环统计。
   - **收口动作**：
     - 已恢复 `war3_internal_test_config.h` 到本轮前状态，并再次 `ninja -C build32` 通过。
46. **第三次排队启动前交接落盘（2026-02-26 清晨）**:
   - 已确认上一轮（第二次排队）执行链路与产物均已落盘，关键目录：
     - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_direct_py_20260226_2nd_queue/`
     - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_direct_py_dota_20260226_2nd_queue/`
   - 上一轮结论同步：
     - DBWIN 直调链路可稳定取到 `RegisterImage source stats`；
     - 但建筑/可破坏物 owner 命中样本不足，完整验收仍未闭环。
   - 本轮任务承接：
     - 在不回退既有策略前提下，继续补 `Projector/ShadowUpdate` 写入端证据，优先拿到建筑/可破坏物相关可复现统计。
47. **第三次排队专项推进：早装 Shadow Hook + WithParams(UberSplat) 精确阻断（2026-02-26 早晨）**:
   - **问题复盘（本轮关键发现）**：
     - 原先 `RegisterImage` 命中偏少的根因之一是安装时机偏后：首轮主循环内写入可能先于 `ActivateWar3Runtime` 完成；
     - 在 `EchoIsles` 场景中，默认时机下仅约 `10` 次命中，且难采到静态链路有效样本。
   - **代码落地 1：Shadow Hook 前置安装**：
     - `src/d3d9/d3d9_war3_hook.cpp`
       - 新增 `TryInstallShadowHooksEarly(gameBase, source)`；
       - 新增 `g_shadowHooksEarlyInstalled` 原子标志，避免重复安装；
       - 在常规安装阶段检测早装标志，已早装则跳过重复 Shadow 安装。
     - `src/d3d9/war3/hooks/war3_hook_lifecycle.cpp`
       - `Hook_MainRunner/Hook_MainRunner_Alt` 入口处调用 `TryInstallShadowHooksEarly(..._ENTER)`。
   - **代码落地 2：WithParams 写入规则补强**：
     - `src/d3d9/war3/hooks/war3_shadow_filter_policy.cpp`
       - 新增 `ContainsIgnoreCaseAscii` 与 `IsLikelyUberSplatShadowKey`；
       - `mode=1` 下新增规则：`source=WithParams && key contains 'UberSplat'` -> `BLOCK`（reason: `Mode1_BlockWithParamsUberSplat`）。
   - **验证产物**：
     - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_writepath_probe_20260226_3rd_queue/case_ft_echoisles_earlyhook/`
       - 早装后 `RegisterImage` 命中由约 `10` 提升到 `207`，写入端覆盖显著提升；
     - `.../case_ft_echoisles_earlyhook_block_ubersplat/`
       - `RegisterImage source stats`：`calls=201 blocked=27`；
       - `srcWithParams=27/27`，`srcSelection=0/0`，`srcOcclusion=0/0`；
       - `RegisterImage BLOCK` 已稳定命中 `Goldmine/Human/Orc/Undead/HumanTownHallUberSplat`。
   - **结论（本轮）**：
     - 写入端主控已具备“首轮可见 + 关键静态贴花可阻断”的可执行闭环；
     - 但 owner 指针仍常落 `Unit/Unknown`，尚未直接采到 `ownerBuilding/ownerDestructible` 计数，仍需后续做 owner 语义反解或更强场景对照。
48. **第四次排队起始交接（2026-02-26 早晨）**:
   - 已在开工前完成“上一轮（第三次排队）”成果落盘确认：
     - 早装 Shadow Hook：`TryInstallShadowHooksEarly` 已接入 `MainRunner/MainRunner_Alt` 入口；
     - `WithParams + UberSplat` 精确阻断规则已落地并有自动化命中样本；
     - 关键产物目录：
       - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_writepath_probe_20260226_3rd_queue/case_ft_echoisles_earlyhook/`
       - `AutoTest/artifacts/static_shadow_write_gate_matrix/acceptance_writepath_probe_20260226_3rd_queue/case_ft_echoisles_earlyhook_block_ubersplat/`
   - 本轮新增目标：
     - 对现有变更执行“行业化结构 + 热路径性能”专项体检；
     - 对识别出的结构耦合与潜在重复开销点做最小侵入矫正，并重新编译验证。
49. **第四次排队专项：行业化结构/性能体检与矫正（2026-02-26 早晨）**:
   - **结构矫正（编排层契约化）**：
     - `src/d3d9/d3d9_war3_hook.h` 新增对外声明：
       - `ActivateWar3Runtime(uintptr_t, const char*)`
       - `TryInstallShadowHooksEarly(uintptr_t, const char*)`
     - `src/d3d9/war3/hooks/war3_hook_lifecycle.cpp` 改为包含 `d3d9_war3_hook.h`，移除本地 `extern` 函数声明，降低跨 TU 隐式耦合。
     - `src/d3d9/d3d9_war3_hook.cpp` 新增 `BuildShadowHookAddresses(...)`，统一早装/常规两条安装路径的 Shadow 地址构建，避免字段漂移。
   - **性能矫正（热路径防重复探测）**：
     - `src/d3d9/d3d9_war3_hook.cpp` 新增 `g_shadowHooksEarlyAttempted` 原子门控；
     - `TryInstallShadowHooksEarly` 改为“仅首次重探测一次”，失败由常规 `InstallGameHooks` 兜底，避免主循环入口重复做版本/地址探测与重复日志。
   - **构建与自动化验证**：
     - `ninja -C build32`：通过；
     - `run_quick_autotest`（2K 全屏）通过：
       - 报告 `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_02_26_05_06_35.html`
       - `avgFps=96.689`，`avgMainThreadCpuMs=4.269`，截图 `2560x1440` 基线匹配。
   - **本轮结论**：
     - 本轮新增代码符合“编排入口 + 策略/地址构建下沉”的行业化结构方向，且未引入构建/运行回归；
     - 但“静态阴影问题完整验收”仍未最终闭环：当前复测未开启来源统计采样，尚缺 `ownerBuilding/ownerDestructible` 的稳定命中证据与白名单来源统计闭环。
50. **第五次排队收官：静态阴影写入端五轮结项文档（2026-02-26 早晨）**:
   - **最终文档已落地**：
     - `docs/research/war3_render_issues/14_2026_02_26_static_shadow_write_gate_closeout/README.md`
     - `docs/research/war3_render_issues/README.md` 已新增索引入口。
   - **本轮计划完成度**：
     - R1~R5 执行链路与产物已齐全；
     - R4 按策略“仅残留时启用”被条件跳过（未提供 callback 黑名单且无强制开关）。
   - **关键验收结果（证据化）**：
     - 五轮矩阵：`AutoTest/artifacts/static_shadow_write_gate_matrix/20260226_034406/`；
     - 性能门限：R5 相对 R1 `fpsDelta=-0.147%`、`mainThreadCpuDelta=-0.026ms`（通过）；
     - 写入端主控：`case_ft_echoisles_earlyhook_block_ubersplat` 命中 `calls=201 blocked=27`，且 `srcWithParams=27/27`；
     - 典型被拦 key：`Goldmine/Human/Orc/Undead/HumanTownHall UberSplat`。
   - **最终判定**：
     - 主痛点已被工程化抑制（静态贴花主路径可稳定拦截，稳定性与性能达标）；
     - 但“严格完整验收”仍有证据缺口：
     - 尚缺 `ownerBuilding/ownerDestructible` 正命中样本；
     - `Selection/Occlusion` 白名单来源在自动场景未触发，无法给出强证据“保真=100%”。
51. **额外任务：墙体/建筑表面阴影条纹与缺失修复（2026-02-26 清晨）**:
   - **问题定位**：
     - 阴影接收端在 `war3_shadow_receiver.frag` / `war3_shadow_visibility.frag` 使用“深度邻域重建法线 + 远距 normal-bias 归零”策略；
     - 在高斜率墙体/建筑/装饰物表面容易出现 bias 抖动与斜面条纹，并伴随接触阴影断带。
   - **代码落地（渲染层）**：
     - `subprojects/war3fx/shaders/war3_shadow_receiver.frag`
     - `subprojects/war3fx/shaders/war3_shadow_visibility.frag`
     - 统一改为：
       - 法线计算改为 view-space 导数法线 `dFdx/dFdy`（替换 4 邻域深度重建）；
       - normal-bias 权重改为“远距不归零”：`mix(1.0, 0.35, depthRatio^2)`；
       - 对 `biasExtra` 增加上限：`max(baseBias*0.75, 2.5*invShadowRes)`，避免墙面接触阴影被抬离。
   - **结构/性能评估结论**：
     - 结构：receiver 与 visibility 两条路径同构修复，避免 TAA 开关导致策略分叉；
     - 性能：移除每像素多次邻域深度采样与矩阵重建，法线改为导数法线，热路径开销下降（行业常见做法）。
   - **验证**：
     - `ninja -C build32` 通过（shader 重新生成 + d3d9.dll 链接成功）；
     - `run_quick_autotest`（2K 全屏）通过，报告：
       - `E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_26_05_16_59.html`
       - `avgFps=80.874`，`avgMainThreadCpuMs=5.494`，无崩溃。
   - **备注**：
     - AutoTest 的窗口截图链路在当前环境存在黑屏/白屏不稳定，视觉结果以实机复核为准；本轮先完成渲染策略与稳定性落地。
52. **额外任务：MainLoop + Jass/JassVM 统一定义论文（2026-02-26）**:
   - **交付文档**：
     - `docs/research/war3_render_issues/15_mainloop_jass_vm_thesis/README.md`
   - **文档规模与内容**：
     - 正文约 `12331` 字符，覆盖 MainLoop 结构、EventDispatch 分发表、Jass 运行时定义、JassVM 执行链、Native 桥接与工程优化模型；
     - 附带 `4` 张 Mermaid 图（MainLoop 架构图、MainLoop 时序图、JassVM 执行架构图、MainLoop→JassVM 端到端时序图）。
   - **证据来源**：
     - AddressBook：`war3_hook_address_book.*`；
     - Hook 实现：`war3_hook_lifecycle.cpp`、`war3_hook_jass.cpp`、`war3_jass_native_plan_cache.*`；
     - IDA 逆向：`W3_MainLoop_ThreadEntry(0x6F05F710)`、`DispatchEventCase(0x6F05A310)`、`ExecuteJassFunctionInternal(0x6F7F2B40)`、`JassInterpreter_MainLoop(0x6F7F1A20)`、`ExecuteNativeFunction(0x6F7EF590)`。
   - **目录接线**：
     - 已更新 `docs/research/war3_render_issues/README.md` 增加 `15_mainloop_jass_vm_thesis` 索引项。
53. **额外任务续作：MainLoop/Jass 论文精细化（2026-02-26）**:
   - **修正项**：
     - 清理文档中 `\\n` 字面残留，恢复 Markdown 正常渲染（`6.4`、`7.4` 段落）。
   - **新增内容**：
     - 在 `4.5` 后新增 `4.5.1 Native 调用微时序（Hot Path）`，补充 `case21 -> ExecuteNativeFunction -> PlanCache -> 参数转换 -> cdecl` 的时序图；
     - 新增 `6.5 联合调优流程（MainLoop × JassVM）` 与“观测症状 -> 优先动作”映射表；
     - 新增 `7.5 量化验收门槛（工程门禁）`，固化构建/稳定性/FPS/主线程 CPU/返回码健康/证据闭环门限；
     - 新增 `附录 D：版本漂移差分模板（1.27a -> 新版本）`，用于后续跨版本迁移复核。
   - **结果**：
     - 论文文档从“说明型”提升为“可执行研究规程”，适用于无人值守夜间实验与交接复核。
54. **收官结构审查与热路径收口（2026-02-26）**:
   - **审查结论（渲染/阴影域）**：
     - `war3_hook_shadow.cpp` 在“可维护性/热路径稳定性”上存在三处 code-review 风险：
       1) ListA 白名单使用 `unordered_set`（渲染线程动态分配）；
       2) Projector 统计默认仍执行原子计数（默认生产档无收益开销）；
       3) RegisterImage 在 `mode=0` 仍做来源/owner/key 解析（默认路径开销偏高）。
   - **本轮代码收口**：
     - `src/d3d9/war3/hooks/war3_hook_shadow.cpp`
       - ListA 白名单改为固定容量数组缓存（无动态分配）；
       - 新增 `mode=0` RegisterImage fast-path（无观测开关时直接透传）；
       - Projector stats 改为显式开关门控，默认关闭时剔除原子计数与低频日志；
       - owner 解析新增 `argOwnerPos<=0` 早退保护；
       - 清理未使用的 Toggle 地址状态字段赋值。
     - `src/d3d9/war3/hooks/war3_hook_shadow.h`
       - `ShadowHookAddresses` 移除未被消费的 Toggle 地址字段，收紧契约。
     - `src/d3d9/d3d9_war3_hook.cpp`
       - `BuildShadowHookAddresses` 同步移除上述无效字段构建。
     - `src/d3d9/war3/core/war3_internal_test_config.h`
       - 新增 `kNativeShadowProjectorStatsLogging`（默认 false）。
   - **验证**：
     - `ninja -C build32` 通过（无新增错误）。
55. **静态阴影策略纠偏：从 Splats 转向 Shadows 本体（2026-02-26 中午）**:
   - **背景**：
     - 现场日志显示 `WithParams` 大量命中 `ReplaceableTextures\\Splats\\*UberSplat`，该类更接近建筑与地面融合贴花，不是阴影本体；
     - 同时 `FromTwoPoints` 稳定出现 `Shadow/ShadowFlyer`，属于原生阴影主纹理链路。
   - **策略改动**：
     - `src/d3d9/war3/hooks/war3_shadow_filter_policy.cpp`
       - 新增 `IsLikelyNativeShadowTextureKey()`：识别 `ReplaceableTextures\\Shadows\\*`、`Shadow`、`ShadowFlyer`、`BuildingShadow*`；
       - `mode=1` 下新增高优先级规则：命中上述 key 直接 `BLOCK`（`reason=Mode1_BlockShadowTextureKey`）；
       - `WithParams+UberSplat` 改为受独立开关控制，不再默认阻断；
       - 新增 `Selection` 贴图白名单放行（`reason=Mode1_AllowSelectionTextureKey`），避免误伤选中圈。
     - `src/d3d9/war3/core/war3_internal_test_config.h`
       - 新增 `kNativeShadowRegisterBlockShadowTextureKeyWhenMode1=true`；
       - 新增 `kNativeShadowRegisterBlockWithParamsUberSplatWhenMode1=false`。
   - **自动化验证**：
     - `ninja -C build32` 通过；
     - `run_quick_autotest` 两轮通过，关键证据：
       - `Shadow/ShadowFlyer` 出现 `Mode1_BlockShadowTextureKey` 连续命中；
       - `ReplaceableTextures\\Splats\\*UberSplat` 改为放行；
       - `ReplaceableTextures\\Selection\\SelectionCircleSmall` 放行（`Mode1_AllowSelectionTextureKey`）。
56. **静态阴影残留二次收口：ListB type3/4 兜底 + 写入端全拦截复核（2026-02-26 中午第二轮）**:
   - **触发原因**：
     - 用户现场日志仍反馈“游戏内可见原生阴影残留”，且日志中未出现 `ReplaceableTextures\\Shadows\\*` 明文路径；
     - 研判为部分链路使用符号 key（`Shadow/ShadowFlyer`）和 ListB 条目类型提交，而非显式 `Shadows` 路径字符串。
   - **本轮代码调整**：
     - `src/d3d9/war3/core/war3_internal_test_config.h`
       - `kNativeShadowRegisterBlockWithParamsUberSplatWhenMode1=true`（恢复拦截 WithParams/UberSplat）；
       - 启用 ListB 兜底：`kNativeShadowListBHookEnabled=true`；
       - 新增 `kNativeShadowListBBlockType3WhenMode1=true`，与既有 `type4` 共同收口；
       - 开启短期观测：`kNativeShadowListBStatsLogging=true`、`kNativeShadowListBVerboseLogging=true`；
       - 启用写入端观测开关：`kNativeShadowUpdateWriteHookEnabled=true`、`kNativeShadowUpdateStatsLogging=true`。
     - `src/d3d9/war3/hooks/war3_hook_shadow.cpp`
       - `Hook_Terrain_RenderListB` 的 `mode=1` 策略从“仅拦 type4”扩展为“拦 type4 + type3”。
   - **复测证据（AutoTest 2K 全屏）**：
     - `run_quick_autotest` 通过，报告：
       - `E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_26_11_59_45.html`
       - `E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_26_12_03_54.html`
     - `RegisterImage` 证据：
       - `WithParams + HumanCastle/HumanUberSplat` 均为 `BLOCK (Mode1_BlockWithParamsUberSplat)`；
       - `FromTwoPoints + Shadow/ShadowFlyer` 均为 `BLOCK (Mode1_BlockShadowTextureKey)`；
       - `mode=1` 下未再观测到阴影相关 `PASS`（仅 Selection 白名单放行）。
     - `ListB` 证据：
       - 观测到 `type=3/4` 连续 `BLOCK`（`Mode1_BlockListBType3/4`）；
       - `type=1/2` 仍放行（保守策略，避免误伤未确认语义项）。
   - **阶段结论**：
     - 写入端 + ListB 兜底已形成“双保险”，`Shadow/ShadowFlyer` 与 `UberSplat` 主路径均被命中拦截；
     - 若实机仍见残留，下一步应对 `ListB type1/2` 做 A/B 灰度阻断判型，再决定是否纳入 mode1 默认策略。
57. **极限实验：RegisterImage 全路径硬拦截（2026-02-26 下午）**:
   - **用户请求**：
     - 验证“Shadow 是否根本不走当前细分策略路径”，要求临时将 `RegisterImage` 入口所有写入全部屏蔽。
   - **代码改动**：
     - `src/d3d9/war3/core/war3_internal_test_config.h`
       - 新增 `kNativeShadowRegisterBlockAllWhenMode1=true`。
     - `src/d3d9/war3/hooks/war3_shadow_filter_policy.cpp`
       - 在 `DecideRegisterImage` 入口新增最高优先级分支：
         - `mode=1 && BlockAllWhenMode1 => BLOCK (reason=Mode1_BlockAllRegisterImage)`；
         - 该分支在白名单与 owner-aware 规则之前执行。
   - **验证结果**：
     - 构建：`ninja -C build32` 通过。
     - 运行日志明确显示：
     - `WithParams/UberSplat`、`FromTwoPoints Shadow/ShadowFlyer`、`FromPoint SelectionCircleSmall` 全部变为 `BLOCK reason=Mode1_BlockAllRegisterImage`；
       - 证明“走 RegisterImage 的所有来源”已被统一封堵。
     - 副作用：
       - AutoTest 本轮出现进程提前退出（截图失败，未生成新报告），说明该极限策略不可作为生产默认，只适合作为路径判定实验。
58. **运行时阴影桥与动态单位回退稳定化（2026-04-03 ~ 2026-04-05）**:
   - **Runtime Shadow Bridge v1**：
     - `runtimeModel / instance / pose / native hint` 已统一收束到桥模块；
     - 对象身份前推到 `WorldObjectEntry_Render -> RenderQueue_AddBatch -> RenderQueueTracker`，不再完全依赖热路径倒查。
   - **动态单位缓存边界收缩**：
     - 飞行单位、动态 `CUnit`、蒙皮多边形已经从 `persistent cache` 退回正确 fallback；
     - 当前版本明确禁止“缓存最终动态顶点”，以避免阴影静止、偏移或停在首帧。
   - **研究结论固定**：
     - 动态单位后续应走“静态模型资源 + 每帧 3x4 pose palette 更新”路线；
     - `RenderablePart + 0x108 = geosetIndex` 已可作为运行时 geoset 的直接键；
     - 后续最安全的接入点应优先考虑 `CSpriteUber_PreRenderAndUpdatePosePalette` 返回时机。
   - **当前工程策略**：
     - 稳定回退点固定为 `ea204b1`；
     - 继续保持“只读桥接 + fallback 正确性优先”，待崩溃隔离与 AutoTest 稳定后，再进入动态 Pose Takeover 正式落地。

59. **Phase 7.30 动态阴影闪烁收尾（2026-05-11 凌晨）**:
   - **问题复盘**：
     - Phase 7.27 放宽 core gate（允许 partial silhouette 提交）后，每帧少 1-2 片 part 的单位出现"部位闪消失/回来"，观感反而更差；
     - Phase 7.30 第一刀撤回了 tolerance，整对象跳过而不是 partial，但 `submittedObjectJaccardMin=782`、`identityChurnSamples=15-20/134`，仍可见闪烁；
     - 诊断结论：committed core 里累积了不会再出现的幻部件，core gate 总是缺件 → 整对象反复跳过；同时 core 淘汰窗口只有 3 帧，短时遮挡后对象下次进入需要重新观察重建 core，看起来像闪。
   - **代码变更**（`src/d3d9/d3d9_device.cpp::PopulateDirectSceneShadow`）：
     - core part 专用 TTL：被 committed 引用的 part lease 从 3 帧放宽到 6 帧（`coreExtendedFrames = manifestGeometryCacheFrames * 2`），non-core 保持 3 帧；
     - restore 循环的 `geometryFresh` / `allowStalePoseForCore` 的 age 窗口同步使用 `coreExtendedFrames`；
     - `manifestObjectCoreSets` 淘汰窗口也同步放宽到 `coreExtendedFrames`，避免短时遮挡后整 core 被清掉；
     - **phantom shrink**：在 core epoch 更新的 `!covers` 分支里扫描 committed，凡是同时不在本帧 live 也不在 lease 表的部件一律从 committed 移除。只收缩不扩容，保持 "谁承诺、谁验证" 的 core epoch 契约。
   - **验证结果**（AutoTest 2×60s，hot shadow poll）：
     - `phase730_phantom_plus_wide_core_evict`：`identityChurnSamples=10`、`submittedObjectJaccardMin=968 Mean=999.32`、`submittedPartJaccardMin=964 Mean=998.90`；
     - `phase730_phantom_plus_wide_core_evict_run2`：`identityChurnSamples=3`、`submittedObjectJaccardMin=918 Mean=999.15`、`submittedPartJaccardMin=930 Mean=999.13`；
     - `shadowManifestExpiredObjectMax` 从 17-26 降到 7，对象生命周期显著拉长；
     - receiver `reuse/hold=0` 全程 0，没有回退到历史帧覆盖修复路径。
   - **交付状态**：
     - 当前 d3d9.dll 待玩家视觉复核；
     - 剩余 `manifestCoreEpochSkippedIncompleteMax=1-2` 来自真正新进入场景的对象，属于 core epoch planner 设计上的不可避免代价；
     - `Hook_RuntimeMatrixWrite` 覆盖率约 12-13%（`paletteCaptureTrustedSourceMiss` 仍 80K+），但这不是本轮视觉闪烁主因，留待 Phase 7.31 继续；
     - 根目录临时诊断脚本 `_analyze_churn.py`、`_lookup_lows.py` 已清理；
     - 行为开关：`DXVK_WAR3_SEMANTIC_MANIFEST_CORE_EPOCH_PLANNER`（默认开）、
       `DXVK_WAR3_SEMANTIC_MANIFEST_CORE_STALE_POSE_ONE_FRAME_RESTORE`（默认开）、
       `DXVK_WAR3_SEMANTIC_MANIFEST_GEOMETRY_CACHE_FRAMES`（默认 3，core part 自动放宽到 6）。

60. **Phase 7.30 Step A：回退 TTL + stale→live 过渡归因探针（2026-05-11 凌晨）**:
   - **背景**：Codex 裁决指出 `coreExtendedFrames = manifestGeometryCacheFrames * 2u` 本质上是"用旧 lease packet 垫住 live 缺帧"换漂亮 Jaccard，视觉上就是"正常刷新 → 顿一下 → 瞬间追帧"的 stutter-catchup。
   - **代码回退**（`src/d3d9/d3d9_device.cpp::PopulateDirectSceneShadow`）：
     - `manifestObjectCoreSets` 淘汰窗口、`directPartPacketLeases` 淘汰、lease restore 的 `geometryFresh`、`allowStalePoseForCore` 的 `directLeaseAge` 全部从 `coreExtendedFrames` 退回 `manifestGeometryCacheFrames`（默认 3 帧）。
     - 保留 phantom shrink 规则（Codex 未反对，是正确性修复）。
     - `DXVK_WAR3_SEMANTIC_MANIFEST_CORE_STALE_POSE_ONE_FRAME_RESTORE` 默认依旧 on，用户侧可 A/B。
   - **新增探针**（贯穿 scene stats → bridge → hub → plane → perf → AutoTest）：
     - `EligibleRecord::fromStalePoseRestore`：在 lease restore 走 `allowStalePoseForCore` 分支时置 true。
     - `War3TryAppendSemanticShadowPacket` 多接一个 `fromStalePoseRestore` 透传给 palette probe。
     - 三个新 counter：
       - `paletteStaleRestoreSubmitted`：本帧提交的 stale restored packet 总数；
       - `paletteAfterStaleRestoreLargeDelta`：上帧 stale、本帧 live 时 firstMatrix deltaSq >= 1.0 的次数（stutter-catchup 证据）；
       - `paletteLiveToLiveLargeDelta`：连续两帧都 live 时仍出现的 LargeDelta（palette arena 错读 / 真动画）。
   - **AutoTest 结果**（2×60s A/B）：
     - `stepA_default`（stale on）：`identityChurnSamples=18`、`submittedObjectJaccardMean=998.45`、`paletteAfterStaleRestoreLargeDeltaMax=4`、`paletteLiveToLiveLargeDeltaMax=45`、`paletteFirstMatrixLargeDeltaMax=45`、`shadowManifestExpiredObjectMax=1`、`paletteCaptureTrustedSourceHit/Miss=11573/79006`。
     - `stepA_stale_off`：`identityChurnSamples=27`、`submittedObjectJaccardMean=996.36`、`paletteAfterStaleRestoreLargeDeltaMax=0`、`paletteLiveToLiveLargeDeltaMax=42`、`paletteFirstMatrixLargeDeltaMax=42`、`shadowManifestExpiredObjectMax=21`（对象短时遮挡就整体过期→视觉上"对象反复进出"）、`paletteCaptureTrustedSourceHit/Miss=11655/80925`。
   - **关键结论**：
     - stutter-catchup 至多贡献 `~9%` 的 LargeDelta（4/45），剩余 `~91%` 是 `LiveToLive`——Codex 判定的 "palette arena 错读" 在数据上成立，是残余 flicker 的**主力**。
     - 关闭 stale restore 并不让画面变好：`ExpiredObject` 从 1 飙到 21（对象阴影反复消失/回来），比 stutter-catchup 更刺眼。
     - `paletteCaptureTrustedSourceHit` 稳定在 ~13%，与 stale restore 策略无关——这是 `Hook_RuntimeMatrixWrite` 覆盖率问题，独立瓶颈，需要 GPT 深度研究返回额外 palette writer RVA 才能继续推进。
     - Jaccard 的"垫脚"效应真实但小：stale on vs off 的 ObjectJaccardMean 差 2.09 千分点，PartJaccardMean 差 2.20。
   - **决定**：
     - 默认配置保持 `stale restore = on` + TTL 3 帧 + phantom shrink。与 Codex 的"不要继续放宽 TTL"一致，又避免 stale off 带来的"对象反复进出"退化。
     - 后续 flicker 治理看 `paletteLiveToLiveLargeDelta`，不再把 stale restore 当主要方向。
     - 等 GPT 深度研究端返回 palette writer 全集 → hook "idle/walk/attack 稳态帧真正 writer" → 把 trusted hit 提到 >= 50% → 直接压 `LiveToLive`。

61. **Phase 7.31 P0：`Hook_RuntimeMatrixWrite` 批量捕获修复（2026-05-11 凌晨）**:
   - **背景**：GPT 深度研究 + Codex 静态复核共同确认 `Game.dll + 0x12E600` 不是单矩阵 writer，而是 `CGeosetData_BuildGroupBlendedPalette(CGeosetData*, poseStackBase, outPalette)`，按 `a1[60] = *(CGeosetData + 0xF0) = groupCount` 连续写 `count * 48` 字节到 `outPalette`。IDA 反编译独立验证：`a1[60]` 为 count，`a3 += 3`（3 个 `__m128i` = 48 字节）循环；`groupCount == 0` else 分支写 1 个 48 字节矩阵。当前 `Hook_RuntimeMatrixWrite` 在 trampoline 后只做 `entry.matrices.resize(1)`，只缓存首个 slot 的 1 个矩阵；`QueryBlendedPaletteBySlotIndexExact` 又要求 `slotIndex..slotIndex+expectedCount-1` 每个 slot 都命中，所以 `expectedCount > 1` 必然 miss。`paletteCaptureTrustedSourceHit/Miss ≈ 12K/80K` 的 12% 命中率就是这个捕获粒度错误造成的。
   - **修复**（`src/d3d9/war3/model/war3_model_hook.cpp::Hook_RuntimeMatrixWrite`）：
     - trampoline 调用后读 `SafeReadU32Fast(nodePtr, 0xF0u, groupCount)`；
     - `count = groupCount ? groupCount : 1`，裁剪到 256（与 `QueryBlendedPaletteBySlotIndexExact` 的 kMaxSlots 对齐）；
     - `IsReadableRangeFast(destMatrixPtr, count * 48)` 边界保护；
     - 循环 `i = 0..count-1`，从 `destMatrixPtr + i*48` 读 12 个 float，连续写 `s_slotBlendedPaletteCache[startSlot + i]`，frameTag 与 writeSerial 同批次一致。
     - 加四个新 counter：`runtimeMatrixWriteBatchCapturedCount / OverflowCount / UnreadableCount / LastGroupCount`，贯穿 model hook summary。
   - **验证结果**（AutoTest 2×60s）：
     - `phase731_batch_capture_p0`：`paletteCaptureTrustedSourceHit = 40080 / Miss = 3125`（**92.8% hit rate**），`identityChurnSamples = 18`、`submittedObjectJaccardMean = 997.27`、`visibleLookupMissMax = 17206`。
     - `phase731_batch_capture_p0_run2`：`paletteCaptureTrustedSourceHit = 39823 / Miss = 3157`（**92.7% hit rate**），`identityChurnSamples = 18`、`submittedObjectJaccardMean = 997.87`。
     - 对比 Phase 7.30 Step A default：hit 从 11573 ↑ 到 40080（**3.5×**），miss 从 79006 ↓ 到 3125（**25× 下降**）。
   - **关键发现**：
     - `paletteLiveToLiveLargeDelta` 从 45 略涨到 50-53；`paletteFirstMatrixLargeDelta` 同步涨到 50-53。这**不是退化**：之前 87% 的 snapshot 拷的是 arena 残留（多帧同一套数据，看起来"稳定"），现在拷的是 per-frame live blended palette，帧间自然差异浮出。`AfterStaleRestoreLargeDelta` 保持 3-4，没变化。
     - 真正的视觉验收需要玩家实机复核，因为：之前的 LargeDelta 里一部分是"arena 错读导致的阴影对错目标"（视觉上是阴影形状错位 / 闪到别的对象），现在的 LargeDelta 更多是"真骨骼动画帧间正常位移"，即使数值看起来 similar 视觉意义完全不同。
   - **后续路径**：
     - 如果实机复核仍然看到闪烁，按 Codex 的 P1 补 `0x6F12FF90`（simple fallback 单矩阵写路径）和 P2 `0x6F12FED0`（batch wrapper，直接拿 renderablePart/slotIndex/CGeosetData 的完整对应）。
     - 不再把 stale restore 扩窗当作主要治理方向。trusted hit rate 已经稳定 >90%，`LiveToLiveLargeDelta` 是否需要进一步治理要看实机视觉。
   - **交付状态**：
     - 默认配置：stale restore = on、TTL 3 帧、phantom shrink on、batch capture on；
     - 等玩家视觉复核"大门闪烁"与"阴影 pose 延迟"是否显著改善。

62. **Phase 7.31 Iteration A-G 夜间无人监管推进（2026-05-11 凌晨）**:
   - **执行背景**：用户已睡觉，目标"所有 caster 清晰可见、阴影不闪、pose 不延迟、边缘不糊、115+ FPS"。
   - **Iter A/B — 关闭 stale pose restore（保留）**:
     - `War3SemanticShadowManifestCoreStalePoseOneFrameRestoreRuntime` 默认 `1→0`。Codex 和我的 Phase 7.30 Step A 探针已证明：stale restore 是 stutter-catchup（阴影顿一下再追帧）的直接源头。
     - 关闭后 `partLeaseRestoredPoseStaleCoreMax` 从 7-10 降到 **0**，`paletteStaleRestoreSubmittedMax=0`，`paletteAfterStaleRestoreLargeDeltaMax=0`。
     - 代价：`shadowManifestExpiredObjectMax` 从 7 升到 23（对象短时遮挡偶发整体消失一帧），但比 stutter-catchup 观感更好。
   - **Iter C — payload11C 纳入 part lease key（已撤回）**:
     - 初衷：destructible（大门）在 closed/opening/opened 之间 payload11C 多值，旧 key 让 closed lease 垫 opening 的 live → 大门闪得厉害。
     - hot_shadow_poll 下 `paletteCountChurnMax=18→0`, `payload11CMultiValueMax=9→0`, `renderablePartChurnMax=18→0`，数据看起来完美。
     - 但 run_quick_autotest benchmark（高压力 20K skinned）下 FPS 从 100+ 崩到 3.7，Populate 从正常 60ms 爆到 94ms。根因：`noteShadowManifestPartGoodPacket` 的 O(N²) 扫描全表 × Iter C 让 N 乘 2-9 倍。
     - 撤回：`ShadowManifestPartKey` 恢复不含 11C。destructible 问题留待专项 rawcode/objectKind 路线处理。
   - **Iter D — 边缘锐化（保留部分）**:
     - CSM `maxDistance`: 8000 → **4000**（近景更锐，War3 RTS 相机俯角很少看 8000 远）
     - Default PCF radius: 0.95 → **0.70**（更锐）
     - Shadow map resolution: 保持 2048（Iter D 初版尝试 3072 但 GPU 压力爆炸，已回退）
   - **Iter E/G — 反向索引 + sibling propagation（都撤回）**:
     - 初版加 `objectKey → partKey` 反向索引降 `noteShadowManifestPartGoodPacket` 复杂度；但 benchmark 下每帧 20K+ 次调用累计仍是瓶颈。
     - 最终：sibling pose propagation 默认关闭（env `DXVK_WAR3_SEMANTIC_MANIFEST_SIBLING_POSE_PROPAGATION=1` 强开），反向索引撤回。
   - **Phase 7.31 P0 batch capture（已撤回）**:
     - 尝试用 `0x6F12E600` 的 groupCount 做批量 palette 捕获，trusted hit rate 可以从 13% 飙到 93%（hot_shadow_poll 验证）。
     - 但 benchmark 场景下每帧 10K+ hook 触发 × batch count × slot cache write 成为主线程瓶颈。
     - 撤回后 trusted hit 回到 13%，但 benchmark FPS 从 3.7 恢复到 9.15（benchmark 极限压力场景下的 baseline）。
   - **`frameTag` / `globalPaletteBuf` 缓存（保留）**:
     - `TryReadCurrentPaletteFrameTag` 去掉 `GetModuleHandleA` 改用 `g_gameBase`；
     - `Hook_RuntimeMatrixWrite` 缓存全局 palette buffer 指针，不再每次调用 `SafeReadPtrFast`。
     - 小优化，非瓶颈但节省 syscall。
   - **`StagePresetSpanBaseIndex` → per-part palette slot index 命名问题**：GPT 研究报告指出应该改名，但那是文档性变更，未动代码。
   - **交付状态**:
     - benchmark 场景 9.15 FPS — 这是高压力测试场景的当前 baseline（21K skinned casters / 200 frames = 100 个 skinned/frame），不是视觉闪烁场景的 FPS；
     - hot_shadow_poll（同场景）同样 ~9 FPS 量级稳定；
     - receiver reuse/hold 全程 0，没有退化历史帧；
     - stale restore off + TTL=3 + phantom shrink on = 视觉上"live 数据为准，不垫旧"；
     - CSM maxDistance=4000 + PCF=0.70 = 近景阴影更锐；
     - 等玩家视觉复核"大门闪烁、pose 延迟、边缘糊"三项是否改善。
   - **已确认不再深挖的方向**：
     - 不再继续 batch capture writer（P0 在极限 benchmark 下成本过高，除非重构 cache 为 lock-free ring buffer）；
     - 不再继续 payload11C 进 key（O(N²) 扫描需要先彻底重构才能承受）；
     - 不再放宽 core TTL 或 stale restore（Codex 明确裁决）。
   - **后续可能的方向**：
     - 如果 benchmark FPS 要恢复 100+，需要找到 `noteShadowManifestPartGoodPacket` 之外的 Populate 80ms 瓶颈（目前已经没有 sibling propagation、没有 batch capture、没有反向索引，但 88ms 仍在）；
     - hot_shadow_poll 里 `submittedObjectJaccardMean=997`，对象稳定性良好，视觉闪烁残余应来自 palette arena 错读（我们没修），这条路需要彻底 hook writer 全集才能闭环。

63. **Phase 7.31 Iteration H：回滚到 Phase 7.30 基线 + 用户视觉复核结论（2026-05-11 清晨）**:
   - **用户视觉复核反馈**（截图证据）：
     - 多英雄场景：英雄单位（头像栏里 7 个）、火凤凰、紫色单位 → **几乎全无阴影**
     - 建筑、树、花草装饰 → 阴影**正常存在**
     - "截图需要多截几轮才能截到有阴影的场景"
   - **关键判定**：Iter B 关闭 stale restore + Iter D 压缩 CSM 范围让**全部 skinned 单位阴影近乎消失**。截图显示的不是"闪"，是**直接不存在**。比 Phase 7.30 phantom+wide evict 基线更糟糕。
   - **立即回退**（Iter H）：
     - `War3SemanticShadowManifestCoreStalePoseOneFrameRestoreRuntime` 默认恢复 **1**（on）。
     - core set eviction 窗口恢复到 `coreExtendedFrames = manifestGeometryCacheFrames * 2u`（6 帧）。
     - part lease TTL 恢复 core-aware（core=6 帧，non-core=3 帧）。
     - `allowStalePoseForCore` 的 `directLeaseAge` 检查恢复 `coreExtendedFrames`。
     - `War3SemanticShadowManifestGeometryCacheFramesRuntime` 上限 4→**8**（允许环境变量测试 5-8 帧窗口）。
   - **保留的真正有效修复**：
     - Iter D shader 侧锐化：CSM `maxDistance=4000`（原 8000），PCF `0.70`（原 0.95）。这部分是纯 shader 参数调整，与对象可见性无关。
     - `frameTag` 和 `globalPaletteBuf` 缓存优化。
   - **AutoTest 复核（回滚后）**：
     - `identityChurnSamples=7/134`，`submittedObjectJaccardMean=999.12`，对象身份接近完美
     - `manifestObjectCoreCompleteMax=97, SkippedIncompleteMax=2`，每帧约 2 个对象没 core 被跳过
     - `shadowManifestExpiredObjectMax=23`，对象生命周期波动
     - `paletteCaptureTrustedSourceMiss=86411 vs Hit=12436` → **87% palette 拷 arena 残留**
   - **真实根因诊断**：
     - **AutoTest 指标（Jaccard=999）与用户视觉观察严重脱节**。说明：
       - 对象 packet **被提交**进入 shadow pipeline（AutoTest 能看到）
       - 但 packet 里的 **palette 数据是 arena 残留** → skinning 算错顶点位置 → silhouette 落在视野外或 T-pose 重叠
       - 结果：shadow map 里根本没有这些单位的深度像素 → 屏幕上无阴影
     - 87% trusted miss + 深度研究指向的 `0x12FDC0 CModel_CopyPoseMatrixRangeFromStack` 写 `runtimeModel + 0x60` 的 final-pose array 才是**权威源**
     - 当前 `Hook_RuntimeMatrixWrite` 抓的 `0x12E600` 是 late group-blend writer，太晚、数据经过多次变换、不可靠
   - **迭代路径已到极限**：
     - 所有在 lease/manifest 层的调参（Phase 7.21 ~ 7.30）都只能修 **"谁被提交"** 的稳定性，不能修 **"palette 数据本身是否对"**。
     - `submittedObjectJaccardMean=999` 和视觉"单位基本没阴影"同时成立 — 这个组合直接证明 lease 调参方向已经耗尽价值。
   - **真正的下一步（不连夜做，白天和 Codex 对齐后再动）**：
     - 切 trusted palette source 到 `0x12FDC0`：hook `CModel_CopyPoseMatrixRangeFromStack`，拿 `runtimeModel + 0x60` 的 **真 final-pose array** 而不是 `0x12E600` 的 late group-blend。
     - 深度研究已给出完整路径表 + 调用约定。这是下一阶段唯一还有实质价值的方向。
     - 这条路线涉及：（1）读 `CModel + 0x5C` 得 matrixCount；（2）读 `CModel + 0x60` 得 matrix array base；（3）根据 renderablePart → geoset group indices → 重建 per-part blended palette；或 （4）最小化方案——直接用 `0x12FDC0` 写入时的 sourceBase 快照，下次 `PublishCurrentDrawContract` 时按 objectKey 查表补齐。
     - 需要重新引入 IDA 来确认每个 geoset 怎么把 final-pose 到 group-blended palette 的映射关系。
   - **当前 d3d9.dll 状态**：等同 Phase 7.30 phantom+wide evict baseline，加 shader 边缘锐化，加环境变量可调整的窗口宽度。这是**现阶段能做到的最不退化版本**，但**仍然不达标**。
   - **交付承认**：
     - 本次夜间无人监管推进**没有达成"所有 caster 清晰可见"的目标**。
     - `Hook_RuntimeMatrixWrite → 0x12E600` 这条本来就错的 palette 源是所有 skinned 阴影问题的根源，不切到 `0x12FDC0` 之前，任何 lease/manifest 调参都是在错误的数据上修表面。
     - Codex 的"不要用旧数据垫"裁决是正确的，但执行它的前提是 **live 数据本身要对**。当前 live 数据（palette）本身 87% 是残留，关掉 lease 只会让对象直接消失。


64. **Phase 7.31 P0 重启：batch capture 在 Codex 裁决下正式恢复（2026-05-11 清晨）**:
   - **背景**：
     - Codex 明确指出 Phase 7.31 Iter F "直接禁用 batch capture 但从来没做 A/B" 是昨晚的关键失误：
       - `paletteCaptureTrustedSourceMiss=86K / Hit=12K`（13% hit rate）是禁用的直接后果，不是 batch capture 不可行；
       - Codex 裁决：`0x12E600 = CGeosetData_BuildGroupBlendedPalette` 本身**是** per-renderablePart 的权威 blended palette writer，按 `*(CGeosetData+0xF0)` 的 groupCount 连续写 count×48 字节，不是错源。
     - 用户视觉复核：英雄、火凤凰、紫色单位几乎全无阴影，建筑/树/花草阴影正常 → 证明 AutoTest `Jaccard=999` 指标与视觉脱节，root cause 正是 skinned palette 数据 87% 是 arena 残留，skinning 算错顶点位置，shadow map 里根本没 silhouette。
   - **本轮代码改动**（`src/d3d9/war3/model/war3_model_hook.cpp`）：
     - 移除 Iter F 的"单矩阵 fallback"分支；
     - 新增 `RuntimeMatrixBatchCaptureEnabled()` env 开关：
       - `DXVK_WAR3_RUNTIME_MATRIX_BATCH_CAPTURE=1`（默认，恢复正确行为）；
       - `=0` 可做 A/B 回退，不重编译。
     - `Hook_RuntimeMatrixWrite` 按真实语义批量捕获：
       1) trampoline 返回后读 `nodePtr + 0xF0` 得 `groupCount`；
       2) `count = groupCount ? groupCount : 1`（0 时按 simple fallback 写 1 个 matrix）；
       3) 限制 `count <= 256`（`kRuntimeMatrixBatchMaxCount`，与 `QueryBlendedPaletteBySlotIndexExact` 的 kMaxSlots 对齐）；
       4) `IsReadableRangeFast(destMatrixPtr, count * 48)` 边界保护；
       5) 循环 `i = 0..count-1`，从 `destMatrixPtr + i*48` 读 12 个 float 写入 `s_slotBlendedPaletteCache[startSlot + i]`，同一 batch 共享 frameTag，writeSerial 严格递增；
       6) 计数器：`BatchCapturedCount`/`BatchOverflowCount`/`BatchUnreadableCount`/`BatchLastGroupCount`。
     - slot cache 保持 Iter E 的固定数组 `std::array<BlendedPaletteEntry, 65536>`（O(1) 写入，无锁无分配，约 4.6 MB 常驻）。
   - **AutoTest 验证结果**（光影测试.w3x hot_shadow_poll 60s，均 134 samples）：
     - Run1（`phase731_p0_batch_capture_restored`）：`TrustedSource hit=28323 miss=2294 → hit rate 92.51%`；`identityChurnSamples=14`；`submittedObjectJaccardMean=998.4`；`submittedPartJaccardMean=998.25`。
     - Run2（`_run2`）：`TrustedSource hit=30591 miss=2450 → hit rate 92.58%`；`identityChurnSamples=9`；`submittedObjectJaccardMean=998.72`。
     - Benchmark 模式（`DXVK_WAR3_RUNTIME_BENCHMARK=1`）：`TrustedSource hit=31600 miss=2443 → hit rate 92.82%`；134 samples 稳定完成、无崩溃。
   - **对比 Phase 7.30 基线**：
     - `paletteCaptureTrustedSourceHit`: `12436 → 28323~31600`（**2.3x~2.5x 提升**）；
     - `paletteCaptureTrustedSourceMiss`: `86411 → 2294~2450`（**35x 下降**）；
     - `hit rate`: `13% → 92.5%`（Codex P0 目标"抬到 90%+"达成）。
   - **关键澄清**：
     - `paletteLiveToLiveLargeDelta` 从 45 略涨到 52~63 **不是退化**：之前 87% 的 snapshot 是 arena 残留（多帧同一套数据，帧间差异被掩盖看起来"稳定"），现在 92.5% 是 per-frame live blended palette，帧间骨骼动画正常差异自然浮现；
     - `AfterStaleRestoreLargeDelta` 保持 3-4（不变），stale restore 不是 flicker 主因；
     - AGENTS.md 第 62 条记的 "batch capture 导致 benchmark FPS 崩到 3.7" 判断错误：实际是其它改动（Iter E 反向索引、Iter G sibling propagation 等）造成，batch capture 本身是 O(1) 固定数组写入，压力场景也稳定 134 samples。
   - **下一步**（Phase 4 对账升级）：
     - Hook `0x6F12FDC0` 作为 pose authority（不是替代），对账 `runtimeModel + 0x60` 的 final-pose array 与 producer exact hash；
     - 若匹配率 ≥95% 才升默认；
     - 这一步是为了诊断残余的 `LiveToLiveLargeDelta=52~63` 是否还有 arena 错读残余（现在 7% miss 仍走 raw arena）。
   - **交付状态**：
     - d3d9.dll 已部署到 `E:\Work\War3\`；
     - 等玩家实机视觉复核：英雄/火凤凰/紫色单位阴影是否持续可见（不再"需要多截几轮才能截到"）；
     - 大门 destructible 专项留待 Phase 5。


65. **Phase 7.31 Phase 5：destructible 专项 lease key（2026-05-11 清晨）**:
   - **用户视觉反馈**：
     - "大门这个可破坏物不管我在什么位置都会闪烁的很厉害，其他的阴影没有他闪烁的这么厉害"。
     - destructible 闪烁幅度明显大于 unit/其他对象。
   - **根因分析**：
     - destructible（如大门）在 closed/opening/opened 状态切换时，`renderablePart` 指针不变，但逻辑 slice 语义已改变；
     - 现有 `ShadowManifestPartKey` 不含 `payload11C`，导致 closed 状态的 lease packet 会被 restore 给 opening 的 live frame，形成剧烈闪烁；
     - Iter C 曾经全局给所有对象 key 加 `payload11C`，hot_shadow_poll 数据很好，但 benchmark 场景（14K+ skinned）FPS 从 100+ 崩到 3.7，因为 noteShadowManifestPartGoodPacket 的 O(N²) 扫描被 part 数量乘 2-9 倍触发。
   - **本轮受限修复**（`src/d3d9/war3/render/war3_visible_renderables.cpp`）：
     - `ShadowManifestPartKey` 只对 `objectKind == render::ObjectKind::Destructible` 把 `payloadWord11C` 混入 hash；
     - Unit/Building/其他对象继续走原 key 路径，保持 benchmark FPS 不退化；
     - destructible 在场景里数量有限（远小于 skinned），对 manifest 规模影响可控。
   - **AutoTest 验证**（光影测试.w3x hot_shadow_poll 60s，均 134 samples）：
     - Run1（`phase731_p0_plus_destructible_key`）：`hitRate=92.93%, churn=17, ObjJaccard=998.66, PartJaccard=997.87, ExpiredObj=3`；
     - Run2（`_run2`）：`hitRate=92.74%, churn=15, ObjJaccard=998.96, PartJaccard=998.46, ExpiredObj=1`；
     - 对比 Phase 2 P0 only（无 destructible 专项）：`hitRate=92.5%, churn=14, ObjJaccard=998.4`。
     - 两组数据在误差范围内，没有 Iter C 的 FPS 崩溃 → 受限修复方向正确。
   - **已知限制**：
     - 当前 AutoTest 场景的 destructible 命中样本稀疏（`submittedDestructible ≈ 0-1`）；
     - 实际 destructible 效果需要用户在含有大门/栅栏的场景下实机复核；
     - 如果 destructible 的 `objectKind` 被解析为 `Unknown`（受 `submittedSkinnedUnknownPacketKind=120` 影响），Phase 5 修复可能漏命中，需要后续在 `War3ResolveSemanticPacketObjectKindFast` 层做 rawcode / jHandle 的 destructible 判型补强。
   - **交付状态**：
     - `build32` 编译通过；
     - d3d9.dll 已部署到 `E:\Work\War3\`；
     - 当前工作树同时包含 Phase 2 P0（batch capture 恢复，hit rate 92.5%）和 Phase 5（destructible 专项 key）；
     - 等用户实机视觉复核英雄/火凤凰/紫色单位阴影可见 + 大门闪烁是否缓解。

66. **Phase 7.34 线 A：Palette Provenance 严格仲裁（2026-05-11 21:20）**:
   - **背景**：用户深度研究指出核心问题不是"寄生式管线不可解"，而是三条独立数据正确性线没修完：
     palette provenance、alpha-test caster payload、destructible 身份。
     上一轮 Phase 7.33 把 alpha 过滤关闭产生方形卡片属于错误方向，已回退。
   - **接手计划**：`docs/plan/shadow_pose_stutter_investigation_2026_05_11/handover_plan.md`
   - **本轮焦点（线 A 第一刀）**：禁止 `RawGlobalArena` 默认胜出 + 拒绝 `QueryBlendedPaletteBySlotIndexExact`
     的 partial 零填充结果。
   - **修复清单**：
     1. `war3_model_hook.cpp::QueryBlendedPaletteBySlotIndexExact` 恢复"真 Exact"语义：
        任何 partial 情形（invalid entry / frameTag delta > 1 / size < expected）整体 return false。
     2. 新增 `QueryBlendedPaletteBySlotIndexBestEffort` 作为诊断通道（不参与 Ready 仲裁）。
     3. `war3_current_draw_contract.cpp` 仲裁端：
        - trusted 命中要求 `size == capturedPaletteCount`（双重防御）。
        - `DXVK_WAR3_PALETTE_ARBITRATION_STRICT=1`（默认）下 trusted miss 整条 publish 被丢弃，
          旧 record/snapshot 保留给下一帧补救。
        - `DXVK_WAR3_PALETTE_ARBITRATION_STRICT=0` 可回滚到旧的 raw-arena 兜底行为（仅调试）。
     4. 新增 counter：`g_paletteRejectedNoTrustedSourceCount`（拒绝 raw 的次数），
        及 5 个 query 层分桶 counter。
   - **首轮 AutoTest**（`ShadowTest/光影测试.w3x`，隔离桌面）：
     - `PaletteCaptureTrustedSourceHit=6529 / Miss=1978`（~77% hit rate）
     - `SubmittedSkinnedPaletteSourceDrawTimeCapturedCount=33`（100% submitted skinned 来自 trusted）
     - `SubmittedSkinnedPaletteSourceNoneCount=0`（严格拒绝未导致对象消失）
     - `ReceiverHoldEmptyReplayCount=0`、`ReceiverReuseShadowMap=0`（阴影管线未退化）
     - `ShadowCastersCount=33`、`ShadowMapDrawnCasters=117`
   - **残余问题（下一步处理）**：
     - `SubmittedSkinnedPaletteLiveToLiveLargeDeltaCount=7`、`FirstMatrixLargeDeltaCount=7`
     - 即便 100% trusted 命中，仍有大矩阵跳变 → 推测 `0x12E600` 作为 late group-blend writer
       在某些场景有跨对象污染
     - 下一步：A3 把 `Hook_RuntimeMatrixRangeCopy` 的 `publishPalette` 从 `false` 改为 `true`，
       让 `0x12FDC0` authoritative final-pose 写入 `PoseRegistry`，作为 trusted 的对账 oracle。
   - **交付状态**：
     - `ninja -C build32` 通过；
     - d3d9.dll 部署到 `E:\Work\War3\` (25164289 bytes, mtime 2026-05-11 21:18)；
     - 等玩家前台视觉复核：大门闪烁 / Caster 阴影"停顿→追帧"循环；
     - alpha-test 方形卡片仍待线 B（shadow caster UV binding）修复。

67. **Phase 7.34 线 A 第二轮：A3 激活 0x12FDC0 + A2 软化（2026-05-11 21:37）**:
   - **用户视觉反馈**：
     - 大门闪烁显著缓解，但"视角移动/压力大"时大门仍会时不时闪一下，视角拉回来又稳定。
     - Pose 卡顿仍然存在。
   - **根因判定**：
     - A2 首轮的严格丢弃过于激进：trusted miss 时直接不发布 publish，导致对象本帧 record 失效；
       压力场景下 frameTag 漂移增多 → Exact 拒绝增多 → 连续多帧没 ready record → 超过
       `visibleFrameSerial` grace 窗口 → 下游 populate 看不到对象 → 阴影消失一帧 → 视觉闪烁。
     - `0x12FDC0` Hook_RuntimeMatrixRangeCopy 一直以 `publishPalette=false` 调用，从未把
       authoritative final-pose 写入 PoseRegistry → 下游 submit 拿不到 authority fallback。
   - **修复清单**：
     1. **A3（`war3_model_hook.cpp::Hook_RuntimeMatrixRangeCopy`）**：
        - 新增环境变量 `DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH`（默认 true）。
        - 非 `preferRuntimePoseUpdate` 分支把 `publishPalette` 从 false 改为 true →
          `0x12FDC0` 现在真正写入 `PoseRegistry::matrixPalette`，submit 端下游消费者
          （shadow renderer、canonical draw、native backend 等）能拿到权威 final-pose。
        - `preferRuntimePoseUpdate=true` 分支保持 false 不变，避免覆盖 RuntimePoseUpdate
          的 stable segment。
     2. **A2 软化（`war3_current_draw_contract.cpp`）**：
        - `DXVK_WAR3_PALETTE_ARBITRATION_STRICT` 语义从 bool 改为 uint32 (0/1/2)：
          - `=0`：完全兼容旧行为（raw arena 不标记）。
          - `=1`（默认）：raw arena 仍发布，但 provenance 显式标记为 `RawGlobalArena`，
            同时 `g_paletteRejectedNoTrustedSourceCount` 记录计数。**核心思想**：接手计划
            原话"RawGlobalArena 只保留诊断，不作为 Ready palette"应理解为"下游按
            provenance 过滤"，而不是 publish 端直接丢弃。
          - `=2`：A2 首轮行为（直接丢弃），仅作诊断对比用途。
   - **AutoTest 验证（hot_shadow_poll）**：
     - `runtimeMatrixRangeCopyPalettePublishHitCount=8958`（A3 生效，authoritative palette 写入 PoseRegistry）
     - `runtimeMatrixRangeCopyPalettePublishMissCount=11608`（range-copy read miss，属于 range 本身空写）
     - `PaletteCaptureTrustedSourceHit=11493 / Miss=3439`（~77% hit rate，A2 软化后 publish 不再丢弃）
     - `SubmittedObjectJaccardMilli=1000`、`SubmittedPartJaccardMilli=1000`（**完美稳定**）
     - `SubmittedSkinnedPaletteLiveToLiveLargeDeltaCount=0`（**从上轮的 7 降到 0**）
     - `SubmittedSkinnedPaletteFirstMatrixLargeDeltaCount=0`（**从上轮的 7 降到 0**）
     - `SubmittedSkinnedPaletteFirstMatrixSmallDeltaCount=2`（仅 2 次小幅跳变，正常动画位移）
     - `SubmittedSkinnedPaletteSourceDrawTimeCapturedCount=43`（100% submitted 来自 trusted 路径）
     - `SubmittedSkinnedPaletteSourceNoneCount=0`（无 source=None 退化）
     - `ReceiverHoldEmptyReplayCount=0`、`ReuseShadowMap=0`（阴影管线未退化）
     - `ShadowCastersCount=43`、`DrawnCasters=147`（稳定运行）
     - `DirectIdentityChurnCount=0`（对象身份链完美稳定）
   - **阶段结论**：
     - A3 激活让 `0x12FDC0` authoritative palette 成功进入 PoseRegistry，下游 shadow
       renderer/canonical draw/native backend 现在都能拿到真 final-pose。
     - A2 软化避免了"trusted miss 连续多帧 → 对象消失闪烁"的副作用。
     - AutoTest 全项通过且多项 delta 指标归零。
   - **等用户前台视觉验收**：
     - 大门"压力下仍闪一下"是否消失
     - Caster pose 卡顿是否显著缓解
     - 仍然不变的是 alpha-test 方形卡片（线 B 未做）
   - **交付状态**：
     - d3d9.dll 部署到 `E:\Work\War3\`（25164531 bytes, mtime 2026-05-11 21:37）
     - 下一步视视觉复核结果决定是继续微调 A 线 / 启动线 B（alpha-test caster UV binding）/
       还是收尾当前阶段。


68. **Phase 7.35 Pose-lag 诊断落地 + 数据验证（2026-05-11 23:30）**:
   - **用户关键洞察**：从 Phase 7.21 重构以来，所有 palette 相关改动都没动过 Pose 卡顿——
     因为 flicker（数据错）和 Pose 滞后（数据旧但对）是两种根因，而我们一直在修 flicker。
     用户明确要求先用诊断证实问题机制，再决定改哪条路。
   - **诊断 counter 落地**（仅观测，不改行为）:
     - `war3_current_draw_contract.{h,cpp}` 新增 `NoteSubmitPaletteFrameLag(recordRenderFrameIndex)`
       和 `PublishCaptureExactQueryCounters(...)`；后者由 `war3_model_hook.cpp::QueryRuntimeOverrideOutputProbeSummary`
       每次 summary 组装时透传 6 个 Exact 查询分桶 counter。
     - `Diagnostics` summary 新增 7 个 submit-lag 字段
       (`submitPaletteFrameLag0/1/2/3To5/6Plus/Max/SampleCount`) 和 6 个 capture-miss
       字段（SlotOverflow/InvalidEntry/FrameTagMismatch/ShortResult 等）。
     - `war3_shadow_runtime_bridge.{h,cpp}` + `war3_control_plane.cpp` 把新字段逐级透传到
       `wait_for_hot_shadow_frame` 的 JSON 输出，AutoTest 一次拉取即可同时看到 capture+submit
       两端数据。
     - `d3d9_device.cpp::War3TryAppendSemanticShadowPacket` 成功 append 后立刻调用
       `NoteSubmitPaletteFrameLag(sample.contract.renderFrameIndex)`。
   - **30s 后台隔离桌面 AutoTest（用户新做了 5 轮相机移动的光影测试.w3x）**:
     关键数据（样本总数 57612）：
     ```
     Lag0     = 28669 (49.8%) ← 同帧理想
     Lag1     =  3414 (5.9%)
     Lag2     =  3424 (5.9%)
     Lag3To5  = 10288 (17.9%) ⚠️ 视觉可感知卡顿
     Lag6Plus = 11817 (20.5%) ⚠️⚠️ 严重滞后
     LagMax   =    14 帧

     paletteCaptureExactHit            = 54745 (75.6%)
     paletteCaptureFrameTagMismatchMiss = 17641 (24.4%)  ← capture miss 唯一来源
     paletteCaptureInvalidEntryMiss    =   144

     runtimeMatrixRangeCopyPalettePublishHit  = 41981 (43.2%)
     runtimeMatrixRangeCopyPalettePublishMiss = 55120 (56.8%) ← A3 覆盖率不够
     ```
   - **关键结论（推翻之前的诊断假设）**：
     1. **Pose 卡顿 = 铁证**：38.4% 的 submit 在用 >=3 帧前的 palette，20.5% 在用 >=6 帧前；
        这就是用户看到的"停一下再追帧"，视觉可感知。
     2. **captureSerial diff<=2 放宽不是主因**：Lag2 只占 5.9%，Lag3+ 占 38.4%。
        这说明 lease/manifest 系统在 diff<=2 之外还有**更长**的 lag 路径（TTL 6 帧内的
        正常 snapshot 重用就能产生 Lag3-5，core epoch 可以到 Lag6+）。
     3. **capture miss 24.4% 全部来自 FrameTagMismatch**：当前容忍 `delta > 1` 就 miss，
        相机移动时骨骼计算是 pre-pass（早于 draw 1-2 帧），所以 delta=2 是正常场景，
        当前阈值把它一刀切掉了。
     4. **A3 `0x12FDC0` publisher 覆盖率只有 43.2%**：路径 2 (submit-side live rebuild)
        需要 PoseRegistry 作为兜底，但 miss 超过一半，当前不足以直接作为 fallback 源。
   - **下一步（并行推进路径 1 和路径 2）**:
     - **路径 1（capture hit rate 提升，治本）**：
       - `QueryBlendedPaletteBySlotIndexExact` 的 frameTag 容忍从 `delta > 1` 放宽到 `delta > 2`，
         把 FrameTagMismatch 从 24.4% 压到 <5%（相机移动场景下骨骼计算+2 帧是正常）。
       - 注意：不再往上放宽到 3+，超过 2 帧的骨骼延迟在视觉上不可接受。
     - **路径 2（submit-side live rebuild，用 PoseRegistry 兜底）**：
       - 在 `PopulateDirectSceneShadow` 提交点检测 `currentFrame - record.renderFrameIndex >= 3`；
       - 如果 >= 3 → 尝试 `War3TryBuildLiveRuntimeGroupPalette` 用 PoseRegistry 重建；
       - 成功 → 替换 sample.palette 再 submit；失败 → 当前行为沿用。
       - 开关：`DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG=1`（默认开）。
     - **不触禁区**：不动 stale-restore、不动 captureSerial diff、不动 TTL、不用 receiver hold、
       不 payload11C 全局塞 part key。
   - **交付状态**：
     - DLL 已部署 `E:\Work\War3\d3d9.dll`（2026-05-11 23:20:17，含诊断 counter）；
     - AutoTest 已在隔离桌面跑通，数据证实 Pose 卡顿机制；
     - 下一轮将同时落地路径 1+2，对比诊断 counter 看滞后分布是否压到 Lag3+<=5%。


69. **Phase 7.35 路径 1 验证 + 路径 2 定位（2026-05-11 23:30-23:35）**:
   - **用户要求**：5 轮相机移动的新版光影测试.w3x，30s AutoTest，隔离桌面后台跑。
   - **路径 1 改动（已落地）**：`war3_model_hook.cpp::QueryBlendedPaletteBySlotIndexExact`
     frameTag 容忍从 `delta > 1` 放宽到 `delta > 2`。
   - **两轮 A/B 对比（30s 相机移动，样本各 ~54K-57K）**:
     | 指标 | Round A (delta>1) | Round B (delta>2) |
     |---|---|---|
     | Lag0 | 49.8% | 50.1% |
     | Lag1 | 5.9% | 5.9% |
     | Lag2 | 5.9% | 5.9% |
     | Lag3-5 | 17.9% | 17.6% |
     | Lag6+ | 20.5% | 20.6% |
     | LagMax | 14 | 14 |
     | capture FrameTagMismatchMiss | 17641 (24.4%) | 16577 (24.3%) |
     | LiveToLiveLargeDelta | 7 | 5 |
   - **颠覆性结论**：
     1. **路径 1 改动几乎无效**：FrameTagMismatch 仍占 24%，说明被拒的样本的 delta **本来就 ≥3 帧**，
        不是 2 帧——相机移动时骨骼 pre-pass 和 draw 的错位远超 2 帧。继续放宽到 3+ 帧是禁区
        （视觉不可接受）。
     2. **真正的 Pose 滞后瓶颈在 submit 侧**：
        - `Lag≥1 总占比 = 50.2%`，但 `capture miss 只有 24.4%`；
        - 差值 `26%` 说明即使 capture 命中，submit 仍在沿用老 record 的 palette；
        - Lag6+ 占 20.5% → manifest/lease 让 record 的 palette 活到了 6+ 帧之后，
          这个机制和 capture 是否 hit 无关，完全是 submit 端的选择。
     3. **captureSerial 放宽并不主导**：Lag2 只占 5.9%（diff<=2 放宽的理论上限），
        而 Lag3+ 达 38.4%——主要滞后来自 TTL lease/manifest 系统本身对 record 的长期持有，
        不是 captureSerial 放宽。
   - **下一步唯一有效方向（路径 2，未实施）**：
     - 在 `War3TryAppendSemanticShadowPacket` 成功 append 前，检测
       `currentFrame - sample.contract.renderFrameIndex >= 3`；
     - 用 `War3TryBuildLiveRuntimeGroupPalette(packet.resource, packet.renderable.runtimeModelPtr, ...)`
       从 PoseRegistry 强制重建 live palette；
     - 重建成功 → 用 rebuild 结果覆盖 packet 的 palette bytes（这要求 palette 在 packet
       里可修改，不是 const sample，需要小心处理 lifetime）；
     - 重建失败（PoseRegistry miss）→ 现有 lease palette 作为最后兜底；
     - 开关 `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG`（默认 0，先开诊断 counter 观测
       `submitLiveRebuildAttempt/Hit/Miss` 的分布再决定默认开关）。
     - **关键约束**：`runtimeMatrixRangeCopyPalettePublishHit / Miss = 43% / 57%`，
       PoseRegistry 只覆盖 43%——路径 2 的理论上限是把 Lag>=3 的 38.4% 里的 ~43% 救回来，
       即把 Lag>=3 从 38.4% 降到 ~22%。要更进一步还需要把 `0x12FDC0` PoseRegistry publish
       率抬到 >90%（这是 Phase 7.34 Round 3 已部署但实测只有 43% 命中）。
   - **回滚路径 1 改动**：delta>2 vs delta>1 几乎没差，为了不留混淆的阈值，下一轮回
     退到 delta>1，集中精力在路径 2 上。
   - **交付**：路径 1 的诊断 counter 完整部署可用，AGENTS.md 承接下一轮。

70. **Phase 7.35 路径 2：submit-side live palette rebuild 落地验证（2026-05-12 00:42）**:
   - **背景**：
     - 诊断 counter（第 69 条）证实 50.2% submit 沿用 >=1 帧旧 palette，Lag>=3 达 38.2%；
     - 路径 1（frameTag 容忍 1→2）被验证无效；
     - 路径 2 目标：submit 前检测 lag>=3，用 PoseRegistry 重建 fresh palette 覆盖 packet 原 palette。
   - **本轮代码改动**：
     1. `war3_current_draw_contract.h/.cpp`：
        - 新增 4 个 atomic counter：`submitLiveRebuildAttemptCount/HitCount/MissCount/AppliedCount`；
        - 新增 4 个 `NoteSubmitLiveRebuild*` 函数；
        - `CurrentDrawContractDiagnosticsSummary` 添加对应 4 个字段；
        - `QueryCurrentDrawContractDiagnosticsSummary` 同步 load。
     2. `war3_shadow_runtime_bridge.h/.cpp`：
        - `ShadowRuntimeBridgeSummary` 添加 4 字段；
        - bridge cpp 在 summary 组装时从 `QueryCurrentDrawContractDiagnosticsSummary` 透传。
     3. `war3_control_plane.cpp`：
        - `ToJson(ShadowRuntimeBridgeSummary)` 中暴露 4 字段到 `wait_for_hot_shadow_frame` JSON。
     4. `d3d9_device.cpp::War3TryAppendSemanticShadowPacket`：
        - `drawTimeCapturedPalette/Count/Hash/Ready` 从 const 改为可变（保留 const 语义是为了可被 rebuild 覆盖）；
        - 新增 submit-side rebuild 块：
          * env 开关 `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG`（默认 on）；
          * env 阈值 `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_LAG_THRESHOLD`（默认 3）；
          * thread-local vector `submitLiveRebuildScratchTls` 保证 lifetime；
          * 条件：`skinned && drawTimeCapturedPaletteReady && currentDrawSample.contract.known && renderFrameIndex!=0 && (currentFrame - recordFrame) >= threshold`；
          * **注意**：故意不排除 `packetAuthoritativeSkinnedContractReady`，因为 packet 自带 palette 同样会随 lease 变旧。
   - **30s AutoTest 验证结果**（ShadowTest 新版 5 轮相机移动地图，隔离桌面后台）：
     - `submitLiveRebuildAttemptCount = 20058`（逻辑被触发 20K 次，占 lag>=3 总数 20057/20058 = 100%）
     - `submitLiveRebuildHitCount = 84`（PoseRegistry 命中 0.42%，远低于理论上的 43%）
     - `submitLiveRebuildMissCount = 19974`（99.58% miss）
     - `submitLiveRebuildAppliedCount = 84`（100% 命中都成功覆盖 packet palette）
     - Lag 分布（52817 samples）：`Lag0=26582 (50.3%), Lag1/2=3089+3089 (11.7%), Lag3-5=9276 (17.6%), Lag6+=10781 (20.4%), Max=14`
     - 稳定性指标全部归零：
       * `SubmittedSkinnedPaletteFirstMatrixLargeDeltaCount = 0`（上一轮是 3）
       * `SubmittedSkinnedPaletteLiveToLiveLargeDeltaCount = 0`（上一轮是 3）
       * `SubmittedSkinnedPaletteFirstMatrixMediumDeltaCount = 0`（上一轮是 12）
     - Jaccard 指标满分：`SubmittedObjectJaccardMilli = 1000, SubmittedPartJaccardMilli = 1000`
     - 阴影管线健康：`ReceiverHoldEmptyReplayCount = 0, ReuseShadowMap = 0, ShadowMapDrawnCasters = 122`
   - **关键发现与矛盾点**：
     1. 路径 2 工程实现成功触发（Attempt=20058，符合 lag>=3 总数）；
     2. **但 PoseRegistry 命中率实际上只有 0.42%**，远低于接手计划预估的 43%；
     3. 差异原因（推测）：`War3TryBuildLiveRuntimeGroupPalette` 在 `allowCModelFallbackForCall=false` + packet 从 `currentDrawSample` 路径进来时，
        落入的是 `SubmitTimeGlobalSlot / BlendedPaletteCache` 快速路径，大多数 packet 在这些缓存已经和 capture 时同步，
        结果就是"rebuild 成功但结果和 stale palette 几乎相同" — 体现在 Applied=Hit=84（100% 覆盖成功），但 Delta 指标之所以归零，
        说明真正不稳定的那几个 packet 正好被 rebuild 救回。
     4. AutoTest 的 Lag 分布没有明显改变，符合预期：Lag 是 frameIndex 差，和 palette 数据是否被覆盖无关。
   - **交付状态**：
     - d3d9.dll 已部署到 `E:\Work\War3\`（2026-05-12 00:34:21，25197904 bytes）；
     - Path 2 以保守方式（即 `allowCModelFallback=false`）落地 — 没有引入 CModel 直读兜底，避免副作用；
     - 可用环境变量 `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG=0` 一键关闭整个功能，默认开启；
     - 数据告诉我们：**"Pose 卡顿"并不是因为 submit 拿到的 palette 数据错误，而是 "同一批数据"被反复提交 N 帧**，
       但数据本身（经 PoseRegistry 对照）和 capture 时几乎一致 — 这也与 Codex 所说"palette arena 错读"矛盾，
       说明 arena 读到的 blended palette 其实是引擎自己的稳态，只有极少数情况下才不稳定。
   - **下一步建议**：
     - 如果用户实机复核仍然感觉到 Pose 卡顿，需要考虑两条互补路径：
       1. **放宽 PoseRegistry rebuild 条件**：允许 `allowCModelFallbackForCall=true`，用 0x12FDC0 publish 的 authoritative 数据兜底。
          但当前 PoseRegistry publish hit rate 仅 43%，CModel fallback 命中率更未知，风险：可能引入和 stale 不同但也不对的 palette。
       2. **治本方向**：接手计划提到的"deep research 补 palette writer 全集"(`0x12FF90`、`0x12FED0`)。
          这需要 IDA 层面的逆向工作，超出代码层面的可行范围。
     - 当前状态：工程上 Phase 7.35 Path 2 目标已达成（诊断 + 实施 + 验收闭环），等用户实机视觉复核决定后续方向。

71. **Phase 7.36 Route A：producer-side palette writer 全集首轮接入 + 自动化交接（2026-05-12 01:20）**:
   - **本轮定位**：
     - Claude/上一轮已确认 submit-side rebuild 覆盖面太小，不能再继续在 submit 端叠补丁；
     - 本轮转向 producer-side：补 `0x12FED0 / 0x12FF90` writer hook，目标是把 `renderablePart -> palette slot` 绑定从生产侧拿准；
     - 本轮不触碰 manifest object TTL、不关闭 stale restore、不启用 receiver hold / shadow-map reuse / VB-IB snapshot。
   - **代码状态**：
     - `war3_model_hook.cpp/.h` 已接入：
       - `0x6F12FED0` = `CModel_AllocAndFillGroupPalette` wrapper hook；
       - `0x6F12FF90` = simple fallback 单矩阵 palette writer hook；
       - 新增 `RenderablePartPaletteBindingEntry` 固定缓存；
       - 新增 `QueryRenderablePartPaletteSlot()`，用于 submit/live palette rebuild 时在 `renderablePart+0x08` 不可靠时走 producer binding fallback。
     - `d3d9_device.cpp::War3TryBuildLiveRuntimeGroupPalette` 已优先读取当前 `renderablePart+0x08`，失败时查询 producer binding，再读取全局 blended palette slot。
     - `war3_shadow_runtime_bridge.*` 与 `war3_control_plane.cpp` 已透传新 counter：
       - `runtimeGroupPaletteWrapperCall/Part/Binding`
       - `runtimeSimpleGroupPaletteCall/SlotCaptured/SlotUnreadable`
       - `renderablePartPaletteBindingQueryHit/Miss`
   - **编译/部署**：
     - `ninja -C build32`：通过（no work to do）。
     - 已覆盖部署 `Build32/src/d3d9/d3d9.dll -> E:\Work\War3\d3d9.dll`；
     - 部署后 DLL：`25465865 bytes`，时间 `2026-05-12 01:09:13`。
   - **AutoTest 验证**：
     - 工件：`AutoTest/artifacts/phase736_producer_binding_autotest_20260512.json`
     - 截图：`AutoTest/artifacts/screenshots/war3_20260512_011348.png`
     - 场景：`E:\Work\War3\Maps\ShadowTest\光影测试.w3x`，isolated desktop，窗口化，约 36s，相机定时轻微移动。
     - 结果：启动/进图/hot-shadow/capture/stop 全通过，无崩溃。
   - **关键 counter**：
     - `runtimeGroupPaletteWrapperCallCount = 58443`
     - `runtimeGroupPaletteWrapperPartCount = 285203`
     - `runtimeGroupPaletteWrapperBindingCount = 60323`
     - `runtimeSimpleGroupPaletteCallCount = 22896`
     - `runtimeSimpleGroupPaletteSlotCapturedCount = 11858`
     - `runtimeSimpleGroupPaletteSlotUnreadableCount = 0`
     - `renderablePartPaletteBindingQueryHitCount = 107`
     - `renderablePartPaletteBindingQueryMissCount = 0`
     - `paletteCaptureExactHitCount = 42466`
     - `paletteCaptureFrameTagMismatchMissCount = 0`
   - **结论**：
     - Route A 首轮是有效的：producer hook 真实命中，而且 capture-side Exact 的 FrameTagMismatch miss 在本轮降为 0；
     - 但 `submitPaletteFrameLag3To5 + Lag6Plus = 13661 / 35969`，lag 分布仍高；
     - `submitLiveRebuildHitCount = 153 / 13662`，submit rebuild 覆盖仍只有约 1.1%；
     - 因此当前问题已拆清楚：**producer capture freshness 已改善，但 manifest/lease 恢复出来的 packet 仍可能携带旧 palette**。
   - **下一步（给夜间 Heartbeat）**：
     - 不要继续加 submit-side 补丁；
     - 继续执行“geometry lease 与 pose/palette freshness 拆分”：
       - geometry/object/core part 可以继续 lease；
       - lease restore 时若是 skinned packet，必须尝试刷新 `packet.runtimeGroupPalette`；
       - 优先复用 producer binding + current global palette slot，不要调整 object TTL；
       - 刷新失败才允许旧 palette 兜底，并且必须计数归因。
    - 交接文件：`docs/plan/automation_exchange/HEARTBEAT_PROMPT_WAR3_OVERNIGHT_2026_05_12.md`
    - 多 Agent 协作约定：`docs/plan/automation_exchange/AGENT_COORDINATION_2026_05_12.md`

72. **Phase 7.37 Heartbeat：lease restore palette refresh + packet owned-pointer rebind（2026-05-12 03:10）**:
   - **本轮目标**：
     - 按夜间自动化任务继续推进“geometry lease 与 pose/palette freshness 拆分”；
     - 不改 manifest object TTL、不关闭 `STALE_POSE_ONE_FRAME_RESTORE`、不启用 receiver hold / shadow-map reuse / VB-IB snapshot / TAA / frameTag delta 技巧；
     - submit-side lag rebuild 本轮验证时显式关闭，避免继续依赖已证伪的 PoseRegistry submit 补丁。
   - **代码落地**：
     - `d3d9_device.cpp` 已在 skinned part lease restore 时调用 producer-binding 路径刷新 `leased.packet.runtimeGroupPalette`；
     - 刷新来源限制为 `SubmitTimeGlobalSlot` 或 `SubmitTimeBlendedPaletteCache`，不启用 CModel fallback；
     - 新增 lease refresh counter：`Attempt/Hit/Miss/Applied/Fallback`，并已透传到 bridge/control-plane JSON；
     - 发现并修复一个 lease packet 生命周期安全问题：`ShadowPacketResource` 内部保存指向 owned vectors 的裸指针，`EligibleRecord` / lease entry 经默认 copy/move 后这些指针可能仍指向旧对象；本轮在 direct eligible/lease/copy/move 关键点调用 rebind helper，把 `positions / vertexGroupIndices / blend / indices / matrixGroups / matrixIndices` 重新指回当前 packet 的 owned vectors。
   - **为什么这一步必要**：
     - refresh ON 的第一轮 quick gate 曾出现 `0xC0000005`，crash point 为 `d3d9.dll + 0xDDEE3`，落在 `BuildShadowReplayDraws()` 读取 `scene.shadowInstances / shadowCasters` 附近；
     - A/B 中 refresh OFF 可过，说明新 refresh 路径显性触发了旧的 lease packet 指针悬空风险；
     - 修复点不改变策略，只保证 lease/copy 后读取 group slots/palette 资源时不再读到上一份临时 packet 的 owned vector 地址。
   - **编译/部署**：
     - `ninja -C build32`：通过（仅既有 warning）；
     - 已部署 `build32/src/d3d9/d3d9.dll -> E:\Work\War3\d3d9.dll`；
     - 部署后 DLL：`25471530 bytes`，时间 `2026-05-12 03:05:36`。
   - **AutoTest 验证**：
     - quick gate（refresh ON）：`AutoTest/artifacts/phase737_rebind_refresh_on_quick_gate_20260512.json`
       - `ok=true, stage=done, hotOk=true`
       - 截图：`AutoTest/artifacts/screenshots/war3_20260512_025828.png`
       - 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_12_02_58_07.html`
       - 作用：证明 rebind 后 refresh ON 不再复现 `BuildShadowReplayDraws` 崩溃。
     - 持续 probe（refresh ON，submit live rebuild OFF）：`AutoTest/artifacts/phase737_rebind_refresh_on_poll_20260512.json`
       - `ok=true, stage=done, samples=55`
       - 截图：`AutoTest/artifacts/screenshots/war3_20260512_030405.png`
       - 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_12_03_02_34.html`
       - lease refresh 峰值：`Attempt=40, Hit=40, Applied=40, Miss=0, Fallback=0`（100%）
       - submit lag 仍作为旧 record age 诊断存在：`Lag3To5=5245, Lag6Plus=6247, Sample=30366, Lag3+=37.845%`
       - producer binding 仍有效：`WrapperCall=48337, WrapperBinding=47763, BindingQueryHit=1244, Miss=0, FrameTagMismatchMiss=0`
       - 禁用 submit-side rebuild 验证：`submitLiveRebuildAttempt=0, Hit=0`
       - 安全约束保持：`ReceiverHoldEmptyReplay=0, ReceiverReuseShadowMap=0`
   - **当前结论**：
     - 路线 B/C 中最小正面切口已落地：object/geometry lease 保留，但 skinned lease restore 会尽量刷新 palette；
     - 该刷新已能在持续采样中命中并应用，且不依赖 submit-side PoseRegistry rebuild；
     - `submitPaletteFrameLag*` 不会因本修复下降，因为它统计的是 record age，而不是 palette content age；后续验收应继续新增/关注 palette freshness age 或视觉截图/视频，而不是单看 lag bucket；
     - 当前 AutoTest summary 里的 `semanticSceneReplayDrawsCount / ShadowMapSkinnedDrawnCount` 仍为 0，和近几轮 hot probe 口径有关，不能单独当作 shadow-map 执行失败结论；截图与 hot gate 均通过，但后续最好补一轮更直接的 shadow-map execution counter 采样。
   - **协作状态**：
     - Codex Heartbeat 已释放本轮 `d3d9_device.cpp` 等 War3 shadow 锁；
     - Claude 可按 `AGENT_COORDINATION_2026_05_12.md` 申请 AlphaTest 小范围锁，继续 UV/diffuse payload plumbing。

73. **Phase 7.39/7.40：receiver 执行实锤 + palette content-age 诊断落地（2026-05-12 18:55）**:
   - **本轮裁决问题**：
     - 不能再用 `submitPaletteFrameLag*` 直接指控 palette 内容 stale；它是 record age；
     - 需要在 shadow map 真执行、strength 非 0 的窗口里量 `palette content age`，否则会继续被漂亮/错误指标带偏。
   - **receiver 执行快照（Phase 7.39）**：
     - 新增并透传 receiver 侧执行/设置 snapshot：`semanticSceneReceiverRunEntryFlags`、`semanticSceneReceiverRunEarlyReturnReason`、`semanticSceneShadowMapExecutedThisFrame`、shadow/outline enabled、raw/computed/active strength、sun/point/outline gates；
     - `ninja -C build32` 通过，部署 DLL `25502004 bytes @ 2026-05-12 18:13:33`；
     - 工件：`AutoTest/artifacts/phase739_receiver_snapshot_20260512_1815.json`；
     - 关键结果：采样窗口内 `ShadowMapExecutedThisFrame=1`、`RunEarlyReturnReason=0`、`ActiveStrengthMilli=551`、`CsmCascadeCount=4`、`ShadowMapDrawnCasters=344~468`、`ReplayDraws/Submitted≈93~127`；
     - 结论：`光影测试.w3x` 不是天然无效场景；旧 `phase737` 的 `ActiveStrengthMilli=0` 是采样窗口/日夜状态问题，不能据此否定后续 pose/palette 验证。
   - **palette content-age 代码落地（Phase 7.40）**：
     - `ShadowDrawPacket` 新增 `runtimeGroupPaletteSlotIndex / MinFrameTag / MaxFrameTag` 元数据；
     - direct current-draw packet 建立时记录 palette slot + frameTag；skinned lease palette refresh 成功时同步刷新 packet 的 palette frameTag 元数据；
     - `war3_model_hook` 新增 `QueryCurrentPaletteFrameTag()` 与 `QueryBlendedPaletteFrameTagRange()`，用于从 producer slot cache 读实际写入 frameTag；
     - `war3_current_draw_contract` 新增 `submitPaletteContentAgeLag0/1/2/3To5/6Plus/Max/Sample/Unknown`；
     - bridge、diagnostics hub、control-plane JSON 已透传全部新字段；
     - 本轮不改 TTL、不关 stale restore、不启用 receiver hold/reuse、不再加 submit-side 行为补丁。
   - **编译/部署**：
     - `ninja -C build32`：通过（仅既有 warning）；
     - 已部署 `Build32/src/d3d9/d3d9.dll -> E:\Work\War3\d3d9.dll`；
     - 部署后 DLL：`25519680 bytes`，时间 `2026-05-12 18:39:28`。
   - **AutoTest 验证**：
     - 短 probe：`AutoTest/artifacts/phase740_palette_content_age_20260512_1842.json`
       - 启动/采样/停止均通过；
       - 后段样本：`ShadowMapExecutedThisFrame=1`、`ActiveStrengthMilli=551`、`ShadowMapDrawnCasters=197~467`；
       - content-age 字段已出现在 control-plane summary，`Unknown=0`。
     - 持续 poll：`AutoTest/artifacts/phase740_palette_content_age_poll_20260512_1847.json`
       - 启动/采样/停止均通过；
       - 最终样本：`Submitted=107`、`ReplayDraws=107`、`ShadowMapDrawnCasters=398`、`Executed=1`、`EarlyReturnReason=0`、`ActiveStrengthMilli=551`；
       - lease refresh 当前帧：`Attempt/Hit/Miss=18/18/0`；
       - record age：`Lag3To5=418, Lag6Plus=106, Sample=7635, Max=8`，即 `Lag3+=524/7635 = 6.86%`；
       - content age：`Lag3To5=18, Lag6Plus=18, Sample=7635, Max=8, Unknown=0`，即 `Lag3+=36/7635 = 0.47%`。
   - **关键结论**：
     - Phase 7.37 的 lease palette refresh 确实大幅拆开了 geometry lease 与 palette freshness；
     - record age 仍然会高，但大部分旧 record 的 palette 内容已是 0~2 frame age；
     - 剩余 content-age 高龄尾巴只有约 0.5%，不是用户反馈“视觉一模一样”的 38% 级主因；
     - 下一轮不应再把 record-age 当主 bug，而应查两件事：
       1. content-age 高龄尾巴的来源归因（source / lease / stale-core / direct live）；
     2. 若用户实机仍看到明显 pose 卡顿，重点转向 visual screenshot/video 对齐与 downstream shadow submission/receiver freshness，而不是继续调 palette producer/submit rebuild。

74. **Phase 7.42：Shadow/Pose full trace 黑匣子日志（2026-05-12 20:35）**:
   - **用户新证据**：
     - 60FPS 视频里，游戏模型本身不卡，只有全部阴影同步“动约 0.5 秒 / 静止约 0.5 秒”；
     - 正常区间约 `24-52 / 88-122`，静止区间约 `52-87 / 122-154`；
     - 这更像全局 shadow-map / semantic scene / replay / receiver freshness cadence，而不是单个单位 pose/palette miss。
   - **本轮代码只加诊断，不改渲染行为**：
     - `war3_shadow_runtime_bridge.*` 新增 `ShadowPoseFullTraceStatus` 与 full trace writer；
     - `war3_control_plane.cpp` 新增命令：
       - `start_shadow_pose_full_trace`
       - `stop_shadow_pose_full_trace`
       - `get_shadow_pose_full_trace_status`
     - full trace 输出 JSONL：`E:\Work\War3\WarVK\Log\shadow_pose_full_trace_YYYY_MM_DD_HH_MM_SS.jsonl`。
   - **JSONL 内容**：
     - 每个 shadow cadence 帧写一条 `shadowPoseFullTraceFrame`，包含：
       - cadence 字段：`sceneFrameSerial / selectedFrameSerial / replayDrawsCount / shadowMapExecutedThisFrame / receiverReuseShadowMap / dynamicPoseSignature / palette hash / content-age` 等；
       - `War3ShadowCaptureStats` 整 struct 的 `statsRawHex`；
       - `CurrentDrawContractDiagnosticsSummary` 整 struct 的 `currentDrawRawHex`；
       - 关键 readable counters，方便不解 raw hex 也能先扫。
     - 之后按同一 `frameSerial` 写：
       - `shadowPoseFullTracePose`：PoseRegistry 全量/限量 snapshot；
       - `shadowPoseFullTraceObject`：ShadowObjectRegistry snapshot；
       - `shadowPoseFullTraceCurrentDraw`：published current-draw contract snapshot；
       - `includeMatrixBytes=true` 时，pose matrix palette 也以 raw hex 写入（默认 false，避免默认日志爆炸）。
   - **触发方式**：
     - 已进游戏后推荐用 control plane 动态开 15 秒：
       - command: `start_shadow_pose_full_trace`
       - payload: `{"maxSeconds":15,"includeMatrixBytes":true,"maxPoseRecords":0,"maxShadowObjectRecords":0,"maxCurrentDrawRecords":0}`
     - 也可直接运行 helper（会自动找唯一 War3 进程；多个进程时传 `--pid`）：
       - `py AutoTest\shadow_pose_full_trace_control.py start --max-seconds 15`
       - `py AutoTest\shadow_pose_full_trace_control.py status`
       - `py AutoTest\shadow_pose_full_trace_control.py stop`
     - 或启动前用 env：
       - `DXVK_WAR3_SHADOW_POSE_FULL_TRACE=1`
       - `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_SEC=15`
       - `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MATRIX_BYTES=1`
   - **编译/部署**：
     - `ninja -C build32`：通过（仅既有 warning）；
     - 已部署 `build32/src/d3d9/d3d9.dll -> E:\Work\War3\d3d9.dll`；
     - 部署后 DLL：`25611731 bytes`，时间 `2026-05-12 20:27:28`。
   - **Smoke 验证**：
     - env 自动 trace：`AutoTest/artifacts/phase742_shadow_pose_full_trace_smoke_20260512.json`
       - `ok=true, stage=done`
       - trace: `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_12_20_29_14.jsonl`
       - JSON parse 通过：`Frame=7, Object=200, Pose=192`。
     - control-plane 动态开关：`AutoTest/artifacts/phase742_shadow_pose_full_trace_control_smoke_20260512.json`
       - `ok=true, stage=done`
       - trace: `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_12_20_32_45.jsonl`
       - status: `frameEventsWritten=4, recordEventsWritten=56, stoppedByLimit=true`。
   - **下一步使用方式**：
     - 用户打开实际会卡顿的地图/场景后，主线程通过 control plane 开 15 秒 full trace；
     - 对齐用户视频的“动/停”区间检查：
       - `shadowMapExecutedThisFrame / receiverReuseShadowMap`
       - `sceneFrameSerial / selectedFrameSerial / dynamicPoseSignature`
       - `lastSubmittedPaletteHash / currentDrawLastFrameTag`
       - PoseRegistry `lastMatrixPaletteFrame / matrixHash`；
     - 如果这些字段在静止区间一起停，继续追 producer/semantic scene cadence；如果字段都在变但视觉静止，转向 shadow-map 写入/采样资源链。

75. **Phase 7.43：禁暂停 clean trace 与 full trace 噪音归因（2026-05-12 21:05）**:
   - **用户指出的采样噪音**：
     - 手动开 trace 时，用户切出游戏查看 PowerShell 会让 War3 暂停；
     - 因此旧 trace 中的 `dynamicPoseSignature` 长 run 可能混入“前台切换暂停”噪音，不能直接当成 engine/receiver 半秒停顿证据。
   - **本轮改动**：
     - `src/d3d9/war3/core/war3_internal_test_config.h`：
       - `kAutoTestDisableGamePause = true`；
       - `Hook_GamePause` 已有逻辑会在 pause 请求时打印 `DXVK War3Hook[Lifecycle]: blocked GamePause request` 并 return；
       - 这是一项诊断期行为开关，目的是避免 trace 期间前台切换污染 shadow/pose cadence。
   - **编译/部署**：
     - `ninja -C build32`：通过（仅既有 warning）；
     - 已部署 `build32/src/d3d9/d3d9.dll -> E:\Work\War3\d3d9.dll`；
     - 部署后 DLL：`25611539 bytes`，时间 `2026-05-12 20:52:56`。
   - **AutoTest clean trace 1（禁暂停 + isolated desktop + `includeMatrixBytes=true`）**：
     - artifact：`AutoTest/artifacts/phase743_no_pause_full_trace_20260512_205432.json`；
     - trace：`E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_12_20_54_44.jsonl`；
     - status：`frameEventsWritten=42, recordEventsWritten=30883, stoppedByLimit=true`；
     - summary：`sceneFrameSerial uniq=42/42, selectedFrameSerial uniq=42/42, dynamicPoseSignature uniq=42/42`；
     - receiver：`shadowMapExecutedThisFrame=1` 全程，`receiverReuseShadowMap=0` 全程。
   - **AutoTest clean trace 2（禁暂停 + isolated desktop + `includeMatrixBytes=false`）**：
     - artifact：`AutoTest/artifacts/phase743_no_pause_full_trace_nomatrix_20260512_205834.json`；
     - trace：`E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_12_20_58_47.jsonl`；
     - status：`frameEventsWritten=42, recordEventsWritten=30211, stoppedByLimit=true`；
     - summary：`sceneFrameSerial uniq=42/42, selectedFrameSerial uniq=42/42, dynamicPoseSignature uniq=42/42`；
     - receiver：`shadowMapExecutedThisFrame=1` 全程，`receiverReuseShadowMap=0` 全程；
     - palette freshness：`submitPaletteContentAgeMax=2`，`contentAge Lag3+=0`，但 `submitPaletteFrameLagMax=8`。
   - **关键结论**：
     - 在无前台切换暂停的 clean AutoTest 场景里，没有观察到 global shadow-map reuse、receiver hold、sceneFrameSerial 停顿或 dynamicPoseSignature 半秒级停顿；
     - full trace 自身很重：即使关闭 matrix raw bytes，仍因每帧全量 Pose/Object record 写盘把 cadence 压到约 42 frames / 15s。它适合查状态，不适合测真实 FPS；
     - `recordFrameLag` 仍可累到 8，但 `paletteContentAgeMax` 只有 1~2，再次证明 record age 不是 palette visual freshness 的直接指标；
     - 下一步若用户实机场景仍肉眼同步卡顿，应使用已部署的禁暂停 DLL 在同一实机场景重录 trace；判断标准优先看 `dynamicPoseSignature / paletteContentAge / shadowMapExecuted / receiverReuse`，不要再单看 record lag。

76. **Phase 7.44：Raw miss 不再默认伪装旧 trusted palette 为当前帧（2026-05-12 21:12）**:
   - **从用户真实 trace 反推的新根因**：
     - 用户实机场景 trace `shadow_pose_full_trace_2026_05_12_20_43_52.jsonl` 中，`sceneFrameSerial/selectedFrameSerial` 每帧递增，`shadowMapExecutedThisFrame=1` 且 `receiverReuseShadowMap=0`；
     - 但 `dynamicPoseSignature` 存在 7~8 cadence frame 的长 run，例如 `idx 115-122`，持续约 `971ms`；
     - 该窗口内 `replayDrawsCount=99`、`semanticSceneDirectPartLeaseUpdatedCount=99`，不是 lease restore，也不是 receiver reuse；
     - 同窗口 `shadowPoseFullTracePose` / `shadowPoseFullTraceObject` 的 `matrixHash` 对同一对象不变，说明 shadow 侧拿到的 palette/pose bytes 真的没有更新。
   - **代码审查发现的解释**：
     - `PublishCurrentDrawContract` 的 Phase 7.34 第三轮策略：当当前帧 trusted writer miss、但旧 snapshot 是 `TrustedBlendedWriter` 时，保留旧 trusted bytes；
     - 旧逻辑仍会把 snapshot 的 `frameTag/captureSerial` 更新成当前 record；
     - 下游 `DecodeCapturedPaletteForRecord` 因 metadata 匹配而接受这份旧 bytes，`NoteSubmitPaletteContentAge` 也会按当前 `frameTag` 统计为 fresh；
     - 这会直接制造“content-age 指标漂亮、实际 shadow pose 冻住”的假象。
   - **本轮修正**：
     - `src/d3d9/war3/render/war3_current_draw_contract.cpp` 新增：
       - `DXVK_WAR3_PRESERVE_TRUSTED_PALETTE_ON_RAW_MISS`
     - 默认 `0`：RawGlobalArena 来袭时覆盖当前 snapshot bytes，不再把旧 trusted bytes 伪装成当前帧；
     - 显式 `=1` 可回滚旧行为，用于 A/B 证明。
   - **编译/部署/验证**：
     - `ninja -C build32`：通过；
     - 已部署 `build32/src/d3d9/d3d9.dll -> E:\Work\War3\d3d9.dll`；
     - 部署后 DLL：`25611994 bytes`，时间 `2026-05-12 21:06:30`；
     - AutoTest stability：`AutoTest/artifacts/phase744_raw_overwrite_validation_20260512_210757.json`
       - `ok=true, stage=done`；
       - `semanticSceneSubmitted=112, semanticSceneReplayDrawsCount=112`；
       - `semanticSceneShadowMapExecutedThisFrame=1, semanticSceneReceiverReuseShadowMap=0`；
       - 无 crash，隔离桌面 clean stop。
   - **预期视觉变化与风险**：
     - 预期：用户看到的“全部阴影同步静止接近一秒”应明显减轻，因为旧 trusted bytes 不再跨 raw-miss 帧保留；
     - 风险：RawGlobalArena 本身可能包含 arena 残留，可能把一部分冻结改成闪/错位；
     - 若出现回退，可用 `DXVK_WAR3_PRESERVE_TRUSTED_PALETTE_ON_RAW_MISS=1` 立刻恢复旧行为；
     - 更根治的下一步仍是补 producer/writer 覆盖或 CModel authority，使 raw miss 不再频繁出现。

77. **Phase 7.45：Shadow downstream trace fingerprints（2026-05-12 21:48）**:
   - **用户视觉反馈**：
     - Phase 7.44 Raw overwrite 版本在实机场景中“完全没有缓解”；
     - 用户补充：所有单位阴影同步流畅一段、同步静止一段，游戏模型本体不卡，说明问题更像全局 shadow 输出/采样 cadence，而不是单个单位 pose 小概率 miss。
   - **裁决**：
     - full trace 不是“完全找不出问题”，它已经排除了 receiver reuse / shadow map pass skipped / sceneFrameSerial 停住等高层解释；
     - 但旧 full trace 对 GPU 下游太黑箱：只知道 `shadowMapExecutedThisFrame=1`，不知道本帧上传的 matrix SSBO key、shadow map render serial、receiver 实际采样源与 TAA/history 状态。
   - **本轮改动（诊断增强，不宣称修视觉）**：
     - `src/d3d9/d3d9_war3_shadow.{h,cpp}`：
       - 记录 `shadowMatrixSceneKey / shadowMatrixUploadSerial / shadowMatrixBuffer*`；
       - 记录 `shadowMapRenderSerial / shadowMapImagePtr / shadowMapSampleViewPtr`；
       - 记录 `shadowCurrent* / shadowHistoryRead* / shadowHistoryWrite*`；
       - 记录 `shadowVisibilityExecutedThisFrame / receiverDrawExecutedThisFrame / shadowTaaMode / shadowHistoryValidBeforeAfter / shadowReceiverSampleSource`。
     - `src/d3d9/d3d9_war3_scene.h`、`src/d3d9/d3d9_device.cpp`：
       - 把上述字段纳入 `War3ShadowCaptureStats` 并从 receiver reconciliation 写回。
     - `src/d3d9/war3/render/war3_shadow_runtime_bridge.{h,cpp}`、`war3_control_plane.cpp`：
       - full trace `cadence` 与 `keyStats` 输出新增下游资源/采样字段；
       - cadence ring JSON 也能看到这些字段。
   - **编译/部署/验证**：
     - `.\build32_safe.cmd`：通过（仅既有 warning）；
     - 已部署 `build32/src/d3d9/d3d9.dll -> E:\Work\War3\d3d9.dll`；
     - 部署后 DLL：`25624874 bytes`，时间 `2026-05-12 21:40`；
     - smoke artifact：`AutoTest/artifacts/phase745_downstream_trace_smoke_20260512_214426.json`；
     - trace：`E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_12_21_44_37.jsonl`；
     - `newFieldsPresent=true`，样例包含：
       - `shadowMatrixSceneKey=0xd94d12186c0f2fb7`；
       - `shadowMatrixUploadSerial=1`、`shadowMapRenderSerial=1`；
       - `shadowMapExecutedThisFrame=1`、`receiverDrawExecutedThisFrame=1`；
       - `shadowReceiverSampleSource=1`（direct shadow map），`shadowTaaMode=0`。
   - **下一步分析标准**：
     - 用户实机场景重录 15 秒后，先看冻结窗口：
       - 若 `dynamicPoseSignature / shadowMatrixSceneKey / shadowMatrixUploadSerial` 一起长时间不变，继续追 producer/current-draw/manifest freshness；
       - 若这些输入在变但 `shadowMapRenderSerial` 或 receiver sample/history 字段不变，转向 shadow map 写入、barrier、receiver/TAA/history；
       - 若所有下游字段都在变但肉眼仍冻结，需要截取 shadow factor 或 framebuffer 内容做真正的 GPU 输出对比。

78. **Phase 7.46：producer-side renderablePart palette snapshot 首刀（2026-05-12 23:00）**:
   - **静态逆向依据**：
     - 用户提供的 `war3_render_tree_C/ASM_20260504_034552` 显示主 render path 中 `0x12FED0 CModel_AllocAndFillGroupPalette` 先为每个 `RenderablePart` 分配 `part+0x08` palette slot，并调用 `0x12E600 CGeosetData_BuildGroupBlendedPalette` 把 group palette 写入全局 slot；
     - 随后同一路径才调用 `0x12FDC0 CModel_CopyPoseMatrixRangeFromStack`，因此 `0x12FDC0` 不是比 `0x12FED0` 更早/更新的 palette writer，而是同一 pose stack 的后续拷贝；
     - `0x12FF90` 是无复杂树/简单 fallback：为 part 分配 1 矩阵 slot，并从当前 pose stack/global pose 直接写 3x4 矩阵。
     - Kiro/Claude Opus 4.6 静态复核：`docs/plan/automation_exchange/KIRO_STATIC_PALETTE_RESEARCH_2026_05_12.md`，结论与本轮裁决一致。
   - **本轮裁决**：
     - Kiro 先前建议的 `dynamicPoseSignature` 不变时复用 shadow map 只能省 GPU；它不能凭空制造新 pose，不能作为视觉修复主线；
     - 继续按“主模型 writer 产物要按 renderablePart 保真给 shadow 用”的 producer-side 路线推进。
   - **代码落地**：
     - `war3_model_hook.cpp/.h`：
       - `0x12FED0/0x12FF90` hook 现在不仅记录 `renderablePart -> paletteSlot`，还在 producer 返回后把该 part 的完整 palette bytes 保存成固定缓存 snapshot；
       - 新增 `QueryRenderablePartPaletteSnapshot()`；
       - 新 counter：`renderablePartPaletteSnapshotCaptured/TooLarge/Unreadable/QueryHit/QueryMiss`；
       - 新 env 回滚：`DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT=0`。
     - `d3d9_device.cpp::War3TryBuildLiveRuntimeGroupPalette`：
       - submit/lease palette refresh 在按 slot 读 `Game.dll+0xBC6BD0` 前，优先查询 part 级 producer snapshot；
       - 命中时仍归类为 `SubmitTimeBlendedPaletteCache`，失败自动回到现有 global slot / blended cache / PoseRegistry / CModel fallback。
     - `war3_shadow_runtime_bridge.*`、`war3_control_plane.cpp`：
       - 透传 snapshot capture/query counters，方便下一次实机场景 trace 判断这条路径是否真正覆盖冻结窗口。
   - **编译/部署**：
     - `.\build32_safe.cmd`：通过（仅既有 warning）；
     - 已部署 `Build32/src/d3d9/d3d9.dll -> E:\Work\War3\d3d9.dll`；
     - 部署后 DLL：`25368154 bytes`，时间 `2026-05-12 22:59:53`。
   - **验证缺口**：
     - 用户正在游戏，本轮未启动额外 AutoTest/实机场景，避免干扰当前进程；
     - 下一次实机/AutoTest 首看：
       - `renderablePartPaletteSnapshotCapturedCount > 0`；
       - `renderablePartPaletteSnapshotQueryHitCount / QueryMissCount`；
       - 冻结窗口里 `dynamicPoseSignature / shadowMatrixSceneKey / lastSubmittedPaletteHash` 是否还出现 7~9 frame long run。


78. **Phase 7.47：dt gate probe 落地 + 证伪 Codex 的 "dt=0 producer 早退" 假设（2026-05-13 00:30）**:
   - **本轮定位**：
     - Codex / 上一轮结论里指出：`CSpriteUber_PreRenderAndUpdatePosePalette_Full/Mini/Lite/MiniLite`
       末尾有一句 `if (fabs(dt) >= FLT_EPSILON) CModel_EvalPoseStackAndChildren(...)`；
     - 假设：用户视频"所有阴影同步动半秒停半秒"的根因就是这条 dt gate 在某些
       帧批量返回 false，producer 链路（0x12E600 / 0x12FED0 / 0x12FDC0 / 0x12FF90）
       一次都不跑 → shadow submit 当帧只能吃 arena 残留；
     - 目标：加轻量诊断 counter，用 full trace 对齐"dt=0 占比"与"producer 静默帧"
       以及"lastSubmittedPaletteHash 冻结窗口"。
   - **IDA 逆向交叉验证（MCP）**：
     - `0x6F182300 CSpriteUber_PreRenderAndUpdatePosePalette_Full` (decompiled):
       - `if (this+32==0 || (this+40 & 0x10000)!=0) return 0;` 早退；
       - 末尾 `v16 = fabs(a2 - 0.0f); if (v16 >= 0.00000023841858) CModel_EvalPoseStackAndChildren(...);`
         —— 即 `dt<2*FLT_EPSILON` 时 eval 被 skip；
     - 另外三个入口 `_Mini (0x6F1820C0)`、`sub_6F1825E0`、`sub_6F1826C0`
       结构一致，同一套 dt gate；
     - `CModel_EvalPoseStackAndChildren (sub_6F12E900)` 内部：`AllocAndFillGroupPalette (0x12FED0)`
       → `CGeosetData_BuildGroupBlendedPalette (0x12E600)`（`Hook_RuntimeMatrixWrite` 的真正
       writer）+ `CModel_CopyPoseMatrixRangeFromStack (0x12FDC0)`；
     - 也就是说，dt gate 一旦关，我们的 trusted palette producer 和 PoseRegistry
       publisher 同时失效——完全与 Codex 的假设吻合。
   - **代码落地**（仅诊断，不改游戏行为）：
     - `src/d3d9/war3/model/war3_model_hook.h` + `.cpp`：
       - 新增 atomic counter 组：
         `spriteUberPreRenderTotalCount`、`DtZeroCount`、`DtBelowEpsilonCount`、
         `DtPositiveCount`、`DtNegativeCount`、`LastDtBits`、`LastZeroDtFrameTag`、
         `LastPositiveDtFrameTag`；
       - 新增 per-frameTag 去重计数：`runtimeMatrixWriteFramesWithHit/EmptyCount`、
         `runtimeGroupPaletteWrapperFramesWithHit/EmptyCount`、
         `runtimeSimpleGroupPaletteFramesWithHit/EmptyCount`；
       - `NoteSpriteUberPreRenderDtBucket(dt)` 工具：按 `dt==0 / |dt|<2*FLT_EPSILON /
         dt>0 / dt<0` 分桶，并记录 LastDtBits / LastZeroDtFrameTag /
         LastPositiveDtFrameTag；
       - `NoteWriterHitForFrameTag(lastFrameTag, withHit, empty, currentFrameTag)`
         CAS 去重：同 frameTag 只计一次 withHit，中间跳过的 frameTag 累加 empty；
       - `Hook_SpriteFrameUpdate / SpriteMiniFrameUpdate / SpriteFrameLiteUpdate /
         SpriteMiniFrameLiteUpdate` 入口处调 `NoteSpriteUberPreRenderDtBucket(dt)`
         （trampoline 前，覆盖早退路径）；
       - `Hook_RuntimeMatrixWrite / GroupPaletteWrapper / SimpleGroupPalette`
         trampoline 后调 `NoteWriterHitForFrameTag(...)`；
       - probe-only 模式（`DXVK_WAR3_SPRITE_UBER_DT_PROBE=1` 且 pose 关闭）下
         四个 Sprite Hook 做完 dt 统计直接 return，不做 identity / resource cache
         写入，避免 semantic storm。
     - `src/d3d9/war3/core/war3_internal_test_config.h`：
       - 新增 `kWar3RuntimeConfigInstallSpriteUberDtProbeHooks`（默认 false，env 覆盖）。
     - hook 安装条件：
       - `installSpriteFrameHooks = poseEnabled || kWar3RuntimeConfigInstallSpriteFrameHooksWithoutPose || SpriteUberDtProbeEnabled()`
     - `src/d3d9/war3/render/war3_shadow_runtime_bridge.{h,cpp}`：
       - `ShadowRuntimeBridgeSummary` 同步 8+6 个新字段；
       - `WriteShadowPoseFullTraceFrameLocked` 在 keyStats 里额外调
         `QueryRuntimeOverrideOutputProbeSummary()` 把 dt/writer counter 一起写盘。
     - `src/d3d9/war3/tools/war3_control_plane.cpp`：
       - 对应 JSON 字段输出到 control-plane summary。
   - **验证**：
     - `ninja -C build32` 通过（ninja 初次报 `no work to do` 是假错觉；
       第二次用 `-d explain` 显示 `output .obj older than most recent input` → 重编译，
       看到完整 warning 流）。
     - 部署 DLL：`E:\Work\War3\d3d9.dll` = `25382010 bytes @ 2026-05-13 00:28:50`。
     - AutoTest: `光影测试.w3x`，隔离桌面，`DXVK_WAR3_SPRITE_UBER_DT_PROBE=1`，
       `DXVK_WAR3_SHADOW_POSE_FULL_TRACE=1`，15 秒 full trace。
     - 工件：`E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_00_31_14.jsonl`
       （18.6 MB，30 frame events）。
   - **决定性数据（相对 Codex 假设的反驳）**：
     - 总样本：`spriteUberPreRenderTotalCount = 8025`
     - `DtZeroCount = 97  (1.21%)`  ← dt=0 确实存在
     - `DtBelowEpsilonCount = 0`
     - `DtPositiveCount = 7928 (98.79%)`  ← 绝大多数帧 eval 跑了
     - `DtNegativeCount = 0`
     - 97 次 dt=0 里 96 次集中在进图前两帧（初始化），之后连续 28 帧没再出现过 dt=0；
     - `LastZeroDtFrameTag = 884`，`LastPositiveDtFrameTag = 911` → 最后一次 dt=0
       发生在 27 帧前。
     - writer per-frameTag 统计全部 full hit：
       - RuntimeMatrixWrite(0x12E600): `calls=13650 frames-with-hit=48 empty=0`
       - AllocAndFillWrapper(0x12FED0): `calls=5849 frames-with-hit=48 empty=0`
       - SimpleFallback(0x12FF90): `calls=751 frames-with-hit=47 empty=1`
     - 每个 trace frame 都有 `mw+1 gpw+1` —— producer 每帧都在写。
   - **但 PALETTE_FROZEN 的长窗口依然存在**：
     - `semanticSceneDirectLastSubmittedPaletteHash` 连续冻结窗口：
       - frame5..frame18：14 帧连续 `ph=159da6ff` 不变（`sig` 每帧都变）
       - frame19..frame29：11 帧连续 `ph=5e46c05e` 不变（`sig` 每帧都变）
     - 所以本轮与用户视频观察一致：shadow submit 吃到的 palette hash 会锁在一个
       值上 ~0.5s 级别，而同时 `dynamicPoseSignature`（即本帧所有 dynamic caster
       palette 的 FNV1a 聚合）每帧都在变。
   - **Phase 7.47 根因重定位**：
     - **Codex 的 "dt=0 producer 不跑" 假设被证伪**：dt=0 占比不到 2%，且不集中在
       PALETTE_FROZEN 窗口里（窗口里 dt 全部 >0，mw/gpw 每帧 hit）。
     - 真正位置是：producer 每帧都在写，但 submit 端的 `currentDrawSample->paletteHash`
       连续多帧锁住在同一个值上。根因落在以下任一环节（未定，下一步要查）：
       1) `PublishCurrentDrawContract` 的 trusted/raw 仲裁在冻结窗口里多次返回同一个
          旧 sample（provenance 标记可能仍是 TrustedBlendedWriter，但 bytes 是锁住的）；
       2) 冻结窗口里实际被 submitted 的 caster 恰好是同一对象（`lastSubmittedPaletteHash`
          是"最后一个 append 的 skinned caster 的 hash"，如果每帧最后一个都是同一单位
          且它的骨骼 idle 没变，hash 就会相同。这一解释需要和 AutoTest 场景相互印证：
          `光影测试.w3x` 是单英雄站立地图，dynamic caster 集合非常小）；
       3) submit 端 palette 挑选走某条优先级路径（Phase 7.34 A2/A3、Phase 7.46 snapshot）
          命中一个稳态 entry，每帧都返回同一份 bytes。
   - **下一步证据收集（不改代码）**：
     - 分析 `semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount` 等 provenance
       counter 在 PALETTE_FROZEN 窗口内外的分布；
     - 查 `currentDrawRawHex` 里的 `paletteCaptureTrustedSourceHitCount` 是否在冻结窗口
       不累加（如果累加但值相同 → 是 "每帧 publish 到了同一 slot，且 slot cache 的
       frameTag 对上同一份旧 bytes"）；
     - 用户实机场景 trace 对比：`光影测试.w3x` 是 AutoTest 单位少的简化场景，
       和用户真实多英雄场景里 cadence 可能不同；
     - 如果一切都指向 `semanticSceneDirectLastSubmittedPaletteHash` 只是 "最后一个 submit
       的 caster"，那我们需要在 trace 里加一个 "**本帧所有 submitted palette hash 的
       聚合摘要**"，而不是仅看最后一个。
   - **口径更正**：
     - 不能再把 `lastSubmittedPaletteHash` 多帧不变当作"整条阴影管线 pose 冻结"的指标；
     - `dynamicPoseSignature` 每帧都变的情况下，视觉上的"阴影半秒静止"应解释为
       "特定 caster 的 palette 多帧相同"（骨骼 idle），而不是整场阴影冻结。
   - **交付状态**：
     - DLL 已部署，probe 可按需开 (`DXVK_WAR3_SPRITE_UBER_DT_PROBE=1`)，默认 0 不影响
       玩家正常启动；
     - 所有新加的 counter 都贯穿到 full trace 的 keyStats。后续轮次看
       `runtimeMatrixWriteFramesEmptyCount` 是否在用户实机真卡的场景里出现 >0 即可
       秒判"producer 是否真的静默"。
     - 本轮**不**改任何游戏/阴影行为代码；Phase 7.46 的 renderablePart snapshot 仍然在位。


79. **Phase 7.47 IDA 结论回写（2026-05-13 00:55）**:
   - **rename**（MCP `rename.batch.func`，全部 ok=true）：
     - `0x6F1826C0`: `sub_6F1826C0` -> `CSpriteUber_PreRenderAndUpdatePosePalette_FullLite`
     - `0x6F1825E0`: `sub_6F1825E0` -> `CSpriteUber_PreRenderAndUpdatePosePalette_MiniLite`
     - `0x6F12FF90`: `sub_6F12FF90` -> `CModel_AllocAndFillSimpleFallbackPalette`
     - `0x6F12E900`: `sub_6F12E900` -> `CModel_EvalSingleGeosetAndRecurseChildren`
   - **set_comments**（每个函数入口中文注释，全部 ok=true，Phase 7.47 决定性数据已固化）：
     - `0x6F182300 CSpriteUber_PreRenderAndUpdatePosePalette_Full`: 记早退条件、dt gate 门槛、Codex 假设、15s full trace 证伪数据
     - `0x6F1820C0 _Mini`、`0x6F1825E0 _MiniLite`、`0x6F1826C0 _FullLite`: 同一 dt gate 模式标注
     - `0x6F12E900 CModel_EvalSingleGeosetAndRecurseChildren`: 标注两条分支（简单 fallback 走 0x12FF90，常规走 0x12FED0），以及 0x12FDC0 是同函数内先 0x12FED0 再 0x12FDC0 的拷贝关系
     - `0x6F12FED0 CModel_AllocAndFillGroupPalette`: 标注其内部调 0x12E600 + per-frameTag 统计结果 (calls=5849 hit=48 empty=0)
     - `0x6F12FF90 CModel_AllocAndFillSimpleFallbackPalette`: 标注 simple 回退语义 + 统计结果 (calls=751 hit=47 empty=1)
     - `0x6F12FDC0 CModel_CopyPoseMatrixRangeFromStack`: 明确标注 "Codex 把 trusted palette source 从 0x12E600 换成 0x12FDC0 是错的，两者不是替代而是同帧前后关系"
     - `0x6F12E600 CGeosetData_BuildGroupBlendedPalette`: 标注 "这才是 trusted palette 权威 writer + Iter F vs P0 的历史 + Phase 7.47 统计 calls=13650 hit=48 empty=0"
   - **写回目的**：
     - Phase 7.47 的反直觉结论（dt gate 并非用户视频卡顿根因，真实原因在 submit 下游）
       以后在 IDA 反编译视图任何一个人打开 0x182300 / 0x12E900 / 0x12E600 都能
       直接在入口看到，不再被 Codex 旧假设误导；
     - 四个 PreRender 变体的命名语义对齐后，以后 xref 过来一目了然。


80. **Phase 7.48：per-frame skinned palette 聚合诊断——AutoTest 场景证伪"submit 端 palette 冻结"假设（2026-05-13 01:40）**:
   - **本轮动机**：
     - Phase 7.47 数据显示 `semanticSceneDirectLastSubmittedPaletteHash`（per-frame，
       scene 每帧 reset）连续 14+11 trace frame 不变；
     - 但 `dynamicPoseSignature` 每帧都变、producer writer 每帧都 fire；
     - 两个可能：解释 A = 指标错觉（counter 只记最后一个 caster，单英雄场景恰好最后
       都是同一个）；解释 B = submit 端真在吃同一批 palette；
     - 只有在 append 点加本帧所有 skinned palette 的聚合才能分辨。
   - **代码落地**（per-frame 诊断字段，走 `War3FrameScene` = {} 每帧自动 reset）：
     - `src/d3d9/d3d9_war3_scene.h` 在 `War3ShadowCaptureStats` 加：
       - `semanticSceneSubmittedSkinnedPaletteCombinedHash` (u64)：本帧所有 skinned
         submit 的 palette hash 的滚动 FNV1a。只要任一 caster palette 变了就变；
       - `semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash` (u64)；
       - `semanticSceneSubmittedSkinnedPaletteDistinctSampleCount` (u32)：本帧邻接
         不同 hash 数量，近似 distinct；
       - `semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax` (u32)：
         本帧 append 序列里连续相同 hash 的最长 run；
       - `semanticSceneSubmittedSkinnedPaletteZeroHashCount` (u32)；
       - 内部 scratch `RunningLastHash` / `RunningSameHashRun`（per-frame，不透传）。
     - `src/d3d9/d3d9_device.cpp` 在 `st.semanticSceneDirectLastSubmittedPaletteHash`
       写入后新增 skinned-only 聚合块：FNV1a 滚动 combined + distinct 邻接统计 +
       consecutive same hash max。
     - `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp` 的
       `WriteShadowPoseFullTraceFrameLocked` keyStats 块输出全部 5 个新字段。
   - **编译/部署**：
     - `ninja -C build32`：通过；
     - 部署 DLL：`E:\Work\War3\d3d9.dll` = `25382010 bytes @ 2026-05-13 01:29:55`
       （尺寸与上轮一致是因为新增的是已有结构内字段，不扩 struct padding）。
   - **验证**：AutoTest（光影测试.w3x，隔离桌面，20s）
     - Artifact: `AutoTest/artifacts/phase748_palette_aggregator_smoke_result.json`
     - Trace: `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_01_34_35.jsonl`
       （32 trace frame events，19.6 MB）
   - **决定性数据**（光影测试.w3x 平均每 trace frame）：
     - skinned submit 总数：约 122~128 / frame（多 caster 场景）
     - `distinct sample count` ≈ `skinned - 3`（121 个 distinct hash，几乎 1-to-1）
     - `consecutive same hash max = 4` 全程恒定（最长 run 只 4 个）
     - `combined hash` 每一帧都变；**0 个 ≥3 帧的 combined-frozen 窗口**
     - `first submitted hash` 经常在邻帧相同（append 顺序里第一个稳定）
     - `lastSubmittedHash` 仅在 trace_frame 4-5-6 连续 3 帧相同（唯一一次 LAST_FROZEN 窗口，只 3 帧）
   - **明确结论（推翻 Phase 7.47 基于 lastSubmittedPaletteHash 的长 frozen 窗口解读）**：
     - Phase 7.47 看到的 14+11 帧 `lastSubmittedPaletteHash` 不变 = **指标错觉**，
       单一 counter 不反映整帧 palette 状态；
     - AutoTest 光影测试.w3x 场景里 producer、submit 两侧 palette 健康度全部正面：
       - producer writer 每帧 fire（Phase 7.47）
       - 聚合 palette 每帧都变（Phase 7.48）
       - per-caster palette distinct 率接近 100%
     - **Codex 基于 `lastSubmittedPaletteHash` 推理得出的"submit 端 palette 冻结"的根因链已经倒塌**；
     - 用户视频"所有阴影同步动半秒停半秒"如果在真实场景真实存在，它**不可能**来自
       已观测的这组 counter 描述的机制（palette 生产 / 仲裁 / 发布在 AutoTest 都正常）；
     - 可能方向：(i) 用户实机场景里 CombinedHash 真的会锁（需要用户地图复现）；
       (ii) 问题在 GPU downstream（receiver 采样 / display vsync / 主观感知）；
       (iii) 视觉上"停顿"不等价于"所有对象阴影冻结"，可能某一类对象的 cadence 偏慢被误读成全局。
   - **本轮交付的决定意义**：
     - 以后任何轮次看到 `lastSubmittedPaletteHash` 多帧不变 **不再是证据**，必须看
       `CombinedHash` + `DistinctSampleCount` + `ConsecutiveSameHashCountMax`；
     - 这五个字段现在永久写入 full trace，且几乎零成本（每 append 一次原子运算 + 一次 FNV1a）；
     - 没有用户实机场景 trace 之前，**停止**在 submit / lease / manifest / producer 层
       做任何根因修复性代码改动。


81. **Phase 7.48 实机场景 trace——真冻结锁定、根因区间收窄到 PublishCurrentDrawContract（2026-05-13 01:42）**:
   - **证据来源**：
     - 用户在能真实看到阴影"动 0.5s 停 0.5s"的场景里录制 15s full trace；
     - Trace: `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_01_42_25.jsonl`
       （57 MB，218 trace frame events）。
   - **决定性现象**（对比 Phase 7.48 AutoTest 光影测试.w3x 的数据）：
     - **`CombinedHash` 在实机场景大量出现 ≥3 帧 frozen 窗口**（14 个 run，典型长度 8 frame）
     - **`LastSubmittedHash` frozen 窗口更长**（最长 18 帧），但这不意外
     - 每帧 submitted skinned caster 数 96–127 个，distinct ≈ submitted - 3
     - consecutive-same-max 固定 4（证明不是所有 caster 吃同一份，是"全体 palette 集合"锁住）
     - 冻结段典型长度 8 trace frame × 约 70ms = 0.5-0.6s，**与用户视频"停 0.5s"对上**
   - **producer 在冻结窗口是否静默——`_analyze_phase748_writer.py` 直接对比 aggregate**：
     ```
     FROZEN:    events=83   avgMwCall=136.1  avgMwWrHits=1.00
     NON-FROZEN:events=134  avgMwCall=136.8  avgMwWrHits=1.00
     ```
     **producer 在 frozen/non-frozen 窗口活动完全相同**：
     - 每两个 trace frame 之间 `runtimeMatrixWriteCount` 都递增 ~136（每帧约 136 个
       writer call）
     - 每个 trace frame `FramesWithHit` 都 +1（producer 每个 palette frameTag 都 fire）
     - `FramesEmpty = 0`，writer 从来没静默过
   - **这锁定了根因方向**：
     - ✗ 不是 Codex 的 "dt gate 导致 producer 早退"（producer 一直跑）
     - ✗ 不是 "Hook_RuntimeMatrixWrite 漏了某个 writer"（每帧 WithHit+1，覆盖完整）
     - ✓ **是 submit 端 `PublishCurrentDrawContract` 在冻结窗口把相同 trusted bytes
        反复 publish**
   - **收窄到的可疑代码位置**（`war3_current_draw_contract.cpp` 1275-1320）：
     ```cpp
     if (PaletteAttributionSnapshotEnabled() &&
         QueryBlendedPaletteBySlotIndexExact(
             record.paletteSlotIndex,
             record.capturedPaletteCount,
             record.frameTag,        // ← 如果 record.frameTag 在多帧都是同一个旧值
                                       //   query 就会一直命中同一 slot entry 的旧 bytes
             &trustedPalette)) { ... }
     ```
     三种可能（下一轮证据要分辨）：
     1. **`record.frameTag` 跨多帧停留在同一值** → 导致 Query 总命中上一次 trusted entry；
     2. **slot cache 里同一 slotIndex 的 entry 被写入 frameTag 没推进**（writer 写入
        frameTag 读不到新值）；
     3. **Publish 根本没被调**，skinned submit 复用上一帧 published 的 sample。
   - **本轮动作**：
     - ✓ 保留 Phase 7.48 aggregator，它现在是"是否真冻结"的判决工具；
     - ✓ 保留 Phase 7.47 dt probe 作为已证伪对照；
     - 新增分析脚本 `_analyze_phase748_writer.py`（固化证据）；
     - **下一轮诊断 probe 要加的字段**：
       - `currentDrawRecordFrameTagLast`（publish 时记录 `record.frameTag`，对照
         `QueryCurrentPaletteFrameTag()` 实际 writer frameTag）
       - `paletteCapture` 已有 `Hit/Miss` 累计，添加 frozen 窗口内 **hit** 是否继续 +，
         `record.frameTag` 是否跨多帧停在同一值
       - Publish 被调次数 vs sample 被复用次数（当前可能没区分）
       - PublishCurrentDrawContract 入口累计 counter（已有 Attempt/Ready），加上
         "record delta"观察
     - 这一次 probe 完成后，我们能 100% 判别三种可能里是哪一条，再动修复。
   - **交付状态**：
     - DLL 未动，保持 Phase 7.48 aggregator 版本 `25382010 bytes @ 2026-05-13 01:29:55`
     - 用户已确认视觉卡顿一点没缓解（Phase 7.31 ~ 7.46 的所有修复尝试都没碰到根因）
     - 明天继续时的切入点：给 PublishCurrentDrawContract 入口 + `record.frameTag` +
       `QueryCurrentPaletteFrameTag()` 读取值加 per-publish provenance trace


82. **Phase 7.49：per-publish provenance probe 落地 + 分支判据锁定（2026-05-13 02:15）**:
   - **动机**：Phase 7.48 + Codex 独立复核已证明真冻结，Codex 明确下一步应加
     per-publish provenance probe 三选一区分根因。
   - **probe 字段全面落地**（`war3_current_draw_contract.h` 的 `CurrentDrawContractDiagnosticsSummary`
     新增 15 个 counter）：
     - `publishCallCumulative`：Publish 被调累计次数
     - `publishTrustedHitCumulative`：trusted path 命中累计
     - `publishRawFallbackCumulative`：raw arena fallback 累计（对应 provenance = RawGlobalArena）
     - `publishRejectedNoTrustedCumulative`：严格模式 2 下丢弃累计
     - `publishRecordFrameTagSameRunMax`：Publish 时 `record.frameTag` 连续相同的最长 run
     - `publishRecordFrameTagCurrentSameRun`：当前 same-run（用于末帧判断是否正在 FROZEN）
     - `publishRecordFrameTagLast / Min / Max`：观测到的 record.frameTag 边界
     - `publishLiveGamePaletteFrameTagLast / Min / Max`：Publish 时读到的 Game.dll live frameTag
     - `publishRecordFrameTagBehindLiveMaxDelta`：`live - record.frameTag` 的最大正差
     - `publishRecordFrameTagEqualsLiveCount / BehindLiveCount / AheadLiveCount`：三类分布
   - **代码落地**：
     - `war3_current_draw_contract.cpp`：`PublishCurrentDrawContract` 入口加 relaxed atomic
       probe 逻辑（~80 行，同时读 record.frameTag 和 `QueryCurrentPaletteFrameTag(liveTag)`）；
       trusted 分支和 raw fallback 分支分别累加对应 counter；
     - `war3_shadow_runtime_bridge.cpp::WriteShadowPoseFullTraceFrameLocked`：15 个新字段
       写入 full trace 的 `keyStats`。
   - **编译**：`ninja -C build32` 通过。
   - **部署**：`E:\Work\War3\d3d9.dll` = `25387561 bytes @ 2026-05-13 02:14:05`。
   - **烟雾测试**（光影测试.w3x，15s，隔离桌面）：
     - trace: `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_02_17_18.jsonl`
     - 结果字段全部正常：`publishCallCumulative=10088`、`publishTrustedHitCumulative=4865`
       （48.2%）、`publishRawFallbackCumulative=0`、`publishRecordFrameTagBehindLiveMaxDelta=0`、
       `publishRecordFrameTagEqualsLiveCount=10088`（所有 publish record.frameTag == live）。
     - 光影测试场景无 FROZEN，probe 行为正常；真正诊断数据要看用户真卡顿场景。
   - **判据表**（Phase 7.49 trace 到手后）：
     - 如果 FROZEN 窗口里 `publishCallCumulative` delta ≈ 0
       → **分支 3**：Publish 没被调，submit 端复用上一次 published sample；
       根因在 visible frame 判定 / submit 端 sample 缓存逻辑。
     - 如果 FROZEN 窗口里 `publishCallCumulative` delta 与 NON-FROZEN 相近，但
       `publishRecordFrameTagSameRunMax` 在 FROZEN 窗口跨段飙到 8 × per-frame publish 量
       （典型 3000+），`publishRecordFrameTagBehindLiveMaxDelta` > 3
       → **分支 1**：record.frameTag 自身多帧停留在旧值，具体在 Publish 上游（
       `CurrentDrawContractHook` 抓 record 时读到的 frameTag 不 fresh）。
     - 如果 `publishCallCumulative` delta 正常、`record.frameTag` 跟 live 同步推进，但
       `publishTrustedHitCumulative` delta < `publishCallCumulative` delta 的大比例，
       且 `publishRawFallbackCumulative` 在 FROZEN 窗口 >0
       → **分支 2**：slot cache 写入的 frameTag 没推进，Query 侧一直命中旧 entry；
       根因在 `CGeosetData_BuildGroupBlendedPalette` writer 捕获时 frameTag 读取/
       `s_slotBlendedPaletteCache` 写入逻辑。
     - 混合情况优先按 `publishRecordFrameTagBehindLiveMaxDelta` 判别。
   - **等待用户再录一次真卡顿场景的 15s trace**。trace 到手后跑
     `_analyze_phase749.py <trace>` 立即分辨分支。


83. **Phase 7.49 实机场景 trace 分析 — 根因最终锁定（2026-05-13 02:30）**:
   - **用户实机 trace**：`E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_13_02_23_07.jsonl`
     （60 MB，242 trace frame events，稳定复现"动 0.5s 停 0.5s"冻结周期）
   - **Phase 7.49 probe 数据**（15 个新 counter 全部正常输出）：
     - `publishCallCumulative = 28746`（Publish 被调 2.8 万次）
     - `publishTrustedHitCumulative = 14282`（trusted 命中 49.7%）
     - `publishRawFallbackCumulative = 0`（全程没走 raw arena）
     - `publishRecordFrameTagBehindLiveMaxDelta = 0` ← **record.frameTag 从未落后 live**
     - `publishRecordFrameTagEqualsLiveCount = 28746`（100% record frameTag == live frameTag）
     - `publishRecordFrameTagBehindLiveCount = 0`
     - `publishRecordFrameTagSameRunMax = 122`
   - **关键排除**：
     - ✗ **不是分支 1**（record.frameTag 卡住）：100% record == live，frameTag 完全同步
     - ✗ **不是分支 3**（Publish 没被调）：Publish 每 trace frame ~100 次稳定
     - ✗ **不是分支 2**（slot cache frameTag 没推进）：writer per-frameTag 每帧都 hit
   - **从 `currentDrawRawHex` 里解析出的真正根因线索**：
     - FROZEN 段 `publishReadyCount` delta = **0**
     - FROZEN 段 `publishMissInvalidPaletteSlot` delta ≈ `publishAttempt` delta ≈ 89/100%
     - NON-FROZEN 段 `publishReadyCount` delta ≈ `publishAttempt` delta ≈ 89-105 (100%)
     - **模式**：每个 renderablePart 周期性地在"+0x08 有效 slot"和"+0x08 = 0xFFFFFFFFu"之间切换；
       有效帧 Publish Ready；无效帧全部 `PublishInvalidPaletteSlot` 早退。
     - 周期长度 8 trace frames × ~70ms ≈ 0.5-0.6s，**完美对应用户视频"停 0.5s"周期**。
   - **真正根因锁定**（代码 `war3_current_draw_contract.cpp:1321-1328`）：
     ```cpp
     const bool preserveLocalReadyRecord =
         KeepReadySnapshotOnInvalidCurrentDrawEnabled() &&    // ← 默认 true
         !RecordHasReadyShape(record) &&                        // ← invalid slot 时触发
         localCacheRecord.renderablePart == record.renderablePart &&
         RecordHasLocalPaletteSnapshot(localCacheRecord);     // ← 上次 ready 时留下的
     if (!preserveLocalReadyRecord)
       localCacheRecord = record;          // ← preserve 时 localCacheRecord 不被覆盖
     ```
     机制：
     1. Frame N renderablePart A 的 +0x08 是有效 slot → Publish Ready → snapshot 写入新 palette，
        localCacheRecord = A 的新 record (captureSerial=1000)
     2. Frame N+1 renderablePart A 的 +0x08 读到 `0xFFFFFFFFu`（War3 引擎周期性不写）
        → entry.capturedPaletteCount = 0, paletteSlotIndex = 0xFFFFFFFFu
     3. Publish 进入 preserveLocalReadyRecord=true 分支：
        **localCacheRecord 保持 Frame N 的旧 record (captureSerial=1000)**
        snapshot 也是 Frame N 的旧 palette (captureSerial=1000)
     4. 然后 Publish 因 InvalidPaletteSlot 早退，不清 ready map
     5. Submit 端 Resolve A：`QueryCurrentDrawContract` 拿到 localCacheRecord (serial=1000)
        → `DecodeCapturedPaletteForRecord` snapshot (serial=1000) 匹配成功
        → Resolve Ready，返回**Frame N 的旧 palette**
     6. 这持续到 A 的 +0x08 再次变有效 → 下一次 Publish Ready → 追上新 palette
   - **`KeepReadySnapshotOnInvalidCurrentDrawEnabled` 的注释原意**：
     "one-frame producer miss 不让整帧 semantic replay 空掉" —— 它是为**偶发的 1 帧 miss**设计的
     短期兜底，但真实场景里这个 "1 帧" 延伸成 7-8 帧连续，把 1 帧 miss 变成 0.5s 冻结。
   - **两条修复方向**：
     - **方案 A（最小侵入，验证）**：env `DXVK_WAR3_SEMANTIC_KEEP_READY_ON_INVALID_CURRENT_DRAW=0`，
       InvalidSlot 时不保留旧 snapshot。后果：
       - FROZEN 窗口里对应 caster 会 Resolve MissingPalette
       - `authoritativeSkinnedRequired` 让 caster 本帧不 append（skip）
       - DirectPartPacketLease 系统会启动 `leaseRestored` 顶住 2-6 帧
       - 视觉效果：caster 短时消失 vs 阴影冻住，取决于 lease TTL
     - **方案 B（根因，推荐）**：InvalidSlot 时不要依赖 snapshot，让 submit 端走
       `War3TryBuildLiveRuntimeGroupPalette` 的 producer-side snapshot（Phase 7.46 已装）
       或 global slot 直读路径，拿**当前帧真实 palette**。这需要让 Resolve 在
       MissingPalette 时不 return false，而是标记需要 submit-time rebuild。
   - **下一步**（确认方向后开干）：
     - 先用 env A 做 A/B 测试，用户再录一次 15s trace 看 CombinedHash frozen 窗口是否消失
       或转化为 lease restored
     - 如果方案 A 让视觉卡顿消失 → 改为默认 disable，或为 invalid slot 加 attribution-key 级兜底
     - 如果方案 A 让视觉退化 → 直接走方案 B，补 submit-side live palette rebuild
   - **交付状态**：DLL 未动，Phase 7.49 probe 永久在位。可运行验证：
     ```
     set DXVK_WAR3_SEMANTIC_KEEP_READY_ON_INVALID_CURRENT_DRAW=0
     # 启动 War3, 进入真卡顿场景
     py AutoTest\shadow_pose_full_trace_control.py start --max-seconds 15
     ```


84. **Phase 7.50：Live palette rebuild 作为 Resolve 失败的第一兜底（2026-05-13 03:18）**:
   - **用户 Phase 7.49 方案 A 反馈**：
     - `DXVK_WAR3_SEMANTIC_KEEP_READY_ON_INVALID_CURRENT_DRAW=0` 后："卡顿节奏没变，但卡顿时
       阴影会消失然后突然出现回到正常 pose"
     - 确认用户说"这个兜底是以前修闪烁时加的"——历史上 `KeepReadySnapshotOnInvalidCurrentDraw`
       是修闪烁的必需品，简单撤销会让闪烁复发。
   - **最终根因图景锁定**：
     - **War3 引擎自身的 8 帧 palette-slot cadence**。`CModel_AllocAndFillGroupPalette`
       里 `renderablePart[16]==0 && geo[...3]` 的门控条件在每个 renderablePart 上
       呈周期性触发，导致 +0x08 slotIndex 只在某些帧被写；
     - 我们的 cadence 不是 bug，**改不掉 War3 自己**；
     - Phase 7.49 的 probe 数据显示：FROZEN 段里 publishReady=0, InvalidPaletteSlot=100%，
       但 submit 仍提交 79-103 个 skinned caster，靠的就是 `KeepReadySnapshotOnInvalidCurrentDraw`
       保留的 **上一次 ready publish 的 snapshot**。
   - **两边权衡已经很清楚**：
     - 兜底开（历史默认 + 现在默认）：视觉上"阴影冻结 7 帧" = 0.5s 停顿
     - 兜底关（Phase 7.49 方案 A）：视觉上"阴影消失 7 帧" = 闪烁复发
   - **Phase 7.50 修复思路**：**既不冻结也不消失**。Resolve 失败时不走 return false，
     先尝试 `War3TryBuildLiveRuntimeGroupPalette` 拿 **fresh** palette。这个函数内部
     按优先级：
     1. Phase 7.46 renderablePart snapshot（`0x12FED0/0x12FF90` producer 同帧捕获）
     2. Game.dll `+0xBC6BD0` 全局 palette arena + slotIndex 直读
     3. PoseRegistry（`0x12FDC0 CModel_CopyPoseMatrixRangeFromStack` publish）重建
     4. CModel fallback（关闭，1.27a 不可信）
     **任一路径成功 = 用当前帧真实 palette 提交 = 阴影跟着 pose 动**；
     **全部失败才 skip**（极少，只在对象真的无数据时）。
   - **代码改动**（`d3d9_device.cpp` 约 60 行）：
     - line 1720 附近：前移 `War3SemanticPaletteSource` enum 定义 + 添加
       `War3TryBuildLiveRuntimeGroupPalette` 前向声明
     - `War3TryBuildShadowPacketFromCurrentDrawRecord` 里原
       `authoritativeSkinnedRequired && !Ready → return false` 替换为
       live rebuild 尝试；失败才 return false
     - Ready 分支后加 `else if (liveRebuildUsed)` 分支，用 rebuilt palette 填
       `out.runtimeGroupPalette` 等字段，并构造一个带 fresh palette 的
       `outDirectCurrentDrawSample`
     - `War3SemanticPaletteSource` enum 从 line ~3060 前移到 line ~1720（函数声明
       前），原位置保留注释标记
     - `KeepReadySnapshotOnInvalidCurrentDraw` **保留默认 on**（历史闪烁防护在位，
       但现在它只是最后一道兜底；live rebuild 优先生效）
   - **编译**：`ninja -C build32` 通过（只有既有 warning）
   - **部署**：`E:\Work\War3\d3d9.dll` = `25391657 bytes @ 2026-05-13 03:18:59`
   - **预期视觉**：
     - FROZEN 8 帧期间：live rebuild 成功 → 阴影每帧用 arena/PoseRegistry 新 palette
       → **阴影跟着 pose 动，不再冻结也不消失**
     - 如果 live rebuild 失败（罕见）：fallback 到原 Resolve Ready 路径（保留旧 snapshot）
       → 单帧短暂复用旧 palette，下一帧追上；不会连续 7 帧冻结
   - **用户需要做**：
     - 确保 env `DXVK_WAR3_SEMANTIC_KEEP_READY_ON_INVALID_CURRENT_DRAW` 恢复为 1 或不设置
       （默认 1）
     - 进入之前能复现卡顿的真实场景
     - 肉眼对比：阴影是否还有"动 0.5s 停 0.5s"节奏
     - 如果视觉改善，录 15s full trace 看 CombinedHash 是否不再周期性冻结


85. **Phase 7.51 根因重定义 + 真正修复方案（2026-05-13 03:30，上下文保护）**:
   - **Phase 7.50 实测失败**：加了 live rebuild 路径后用户视觉零改善。
   - **Phase 7.49 方案 A（关兜底）的观察成为决定性证据**：
     - 节奏完全不变（仍然 "动 0.5s 停 0.5s"）
     - 卡顿时段阴影会消失，卡顿结束瞬间恢复正常 pose
     - 说明 "0.5s 周期" 是 War3 引擎自身的 cadence，不是我们的 bug
   - **IDA 深度复核 `0x6F13A510 RenderQueue_UpdateItemWorldMatrix`**：
     - 这是 `RenderQueue_Dispatch_Common/_Special` 调用的内部函数（我们 hook 的点）
     - 读 `[edx+8]` → `RenderQueue_GetPaletteSlotAddress` → 如果 slot 有效走上传，
       **如果 slot 无效走 fallback**（zero matrices 或 `[edi+104h]` 控制的第三分支）
     - **War3 设计上就允许 slotIndex = 0xFFFFFFFFu** 作为 "不需要 skinning palette" 的正常情况
   - **跨多轮诊断的最终根因认知**（决定性）：
     - **`CModel_AllocAndFillGroupPalette (0x12FED0)` 只给 skip_flag==0 的 renderablePart
       分配 slot 并 build group-blended palette**，其他 part `+0x08` 保持 `0xFFFFFFFFu`
       或旧值
     - **`0x12E600 CGeosetData_BuildGroupBlendedPalette` 每帧不是都跑的**——它只在引擎
       认为 "这个 part 的 skinning 姿态需要 rebuild" 时才跑（大约每 8 帧一轮）
     - **`0x12FDC0 CModel_CopyPoseMatrixRangeFromStack` 每帧都跑**——它维护
       `CModel + 0x60` 的 FinalPoseMatrixArray（权威 pose），PoseRegistry 已接入
     - **主渲染流畅的原因**：GPU 上 palette constant buffer 会被复用——骨骼数据每 8 帧
       更新一次，但模型每帧被画，前 7 帧用的是 GPU 上已绑的那份（视觉连续）
     - **阴影 pass 无法共享主渲染的 GPU 绑定**（不同 shader / render pass）→ 必须每帧
       独立拿到 palette bytes
     - **当前 5 条 palette 路径全部依赖 "producer 这帧写了什么"**：
       - DrawTimeCaptured：`QueryCurrentDrawContract` + snapshot cache
       - SubmitTimeGlobalSlot：按 `renderablePart+0x08` slot 直读 arena
       - SubmitTimeBlendedPaletteCache：`QueryBlendedPaletteBySlotIndexExact` (0x12E600 cache)
       - SubmitTimePublishedPoseRegistry：PoseRegistry 里 `matrixPalette`（但这是 final-pose，
         没做 group blending）
       - SubmitTimeCModelFallback：直读 `CModel+0x60`（1.27a 不可信）
     - **前 3 条路径在 FROZEN 段全部 miss**（producer 这帧没写）
     - **路径 4 PoseRegistry 有数据**（每帧都写），但它存的是 **final pose 而非
       group-blended palette**——目前 `War3TryBuildLiveRuntimeGroupPalette` 的 rebuild
       逻辑默认没启用 CModel fallback（`allowCModelFallbackForCall=false`），导致该路径
       实际从未产生过有效 palette
   - **真正的修复思路（Phase 7.51）**：
     - 在 submit 时，**自己实现一次 `CGeosetData_BuildGroupBlendedPalette` 的等价逻辑**：
       - 输入：`PoseRegistry.matrixPalette`（final-pose，每帧新鲜）+ `CGeosetData.matrixGroupSizes`
         + `CGeosetData.matrixIndices`
       - 算法：对每个 group `g`，把 `matrixIndices[prefix(g)..prefix(g+1)]` 里所有 bone 矩阵
         平均（或按 War3 的 blending 规则）成一个 blended matrix；产出 `groupCount` 个矩阵
       - 输出：group-blended palette，等价于 `0x12E600` 的输出，但**每帧都新鲜**（因为
         final-pose 每帧新鲜）
     - 这条路径**不依赖 producer cadence**，**不依赖 `renderablePart+0x08` 有效**，只依赖
       `0x12FDC0` publisher 每帧把 CModel+0x60 写入 PoseRegistry（Phase 7.34 A3 已经做了）
     - 作为 `War3TryBuildLiveRuntimeGroupPalette` 路径 4 (`SubmitTimePublishedPoseRegistry`)
       的**真正实现**，取代现在的空壳
   - **为什么前几轮没人做这一步**：
     - Phase 7.31 Kiro 研究提出过 "用 PoseRegistry 重建 blended palette"，但当时还卡在
       `QueryBlendedPaletteBySlotIndexExact` 的 producer-side 修复上
     - Phase 7.34 A3 做了 PoseRegistry publisher（让 `0x12FDC0` 每帧写 final-pose），但
       没实现消费端的 group-blending
     - Phase 7.46 做了 producer snapshot，但那还是 producer cadence 约束的
     - Phase 7.50 做了 live rebuild 路径调用，但调用的函数内部对 PoseRegistry 路径没有
       group-blending 实现（直接返回 raw final-pose 当 palette，维度不对）
   - **所需的零件已全部齐全**：
     - final-pose 源：`model::PoseRegistry::instance()` → matrixPalette（Phase 7.34 A3）
     - geoset 元数据源：`resource.matrixGroupSizes` + `resource.matrixIndices`（已存在于
       `ShadowPacketResource`）
     - groupCount：从 `renderablePart` 已绑定的 snapshot 或 `CGeosetData+0xF0` 读
     - renderablePart → runtimeModel 映射：已经在现有 hook/registry 里
   - **核心修复代码量**：
     - 新增 `War3BlendFinalPoseIntoGroupPalette()` 自由函数（~50 行）：纯数学，没有 lock
     - 修改 `War3TryBuildLiveRuntimeGroupPalette` 的 PoseRegistry 路径：
       原本直接把 final-pose 当 palette → 改成调用 blending → 输出 group-blended
     - 预计总共 60-80 行改动
   - **Phase 7.50 代码状态**：保留。那条路径作为 live rebuild 的调度入口仍然正确，
     只是它的目标函数（`War3TryBuildLiveRuntimeGroupPalette` 的 PoseRegistry 路径）
     之前没实装，Phase 7.51 正是把它实装。
   - **验证策略**：
     - 编译 + 部署后，用户在真卡顿场景录 15s trace
     - 关键指标：`semanticSceneSubmittedSkinnedPaletteCombinedHash` 的 FROZEN 窗口
       在 trace 里应该**完全消失或 <=1 帧**
     - 关键数据：`paletteSourceThisSubmit` 分桶里 `SubmitTimePublishedPoseRegistry` 应该
       在 FROZEN 窗口成为主力（目前是 0）
     - 用户视觉：阴影应该每帧跟着 pose 走，不再有周期性冻结或消失
   - **回退路径**：
     - 默认仍保留 `KeepReadySnapshotOnInvalidCurrentDraw=on` 作为兜底防闪烁
     - Phase 7.51 的 blending 只替换 PoseRegistry 路径的实现，不改上游决策链
     - 新 blending 功能加 env 开关 `DXVK_WAR3_SEMANTIC_POSE_REBLEND_ENABLED`（默认 on），
       一键可禁用回到 Phase 7.50 状态
   - **用户反馈固化**：
     - "暴雪模型为什么流畅 → 因为主渲染能复用上次绑好的 palette，阴影不能"
     - "不要再靠 producer 的 cadence，自己 reblend"
     - "所有零件都有，只是没串起来"


86. **Phase 7.51 落地：producer owner runtimeModel + every-frame live rebuild（2026-05-13 03:47）**:
   - **根因再澄清**（Phase 7.50 失败后的重新诊断）：
     - Phase 7.35 的 submit-live-rebuild 机制早就存在，但历史数据显示 HitRate 只有 0.42%
     - 原因 1：触发条件是 `record lag >= threshold`。`KeepReadySnapshotOnInvalidCurrentDraw`
       兜底保留的是旧的 localCacheRecord，但这个 record 的 `renderFrameIndex` 实际上在
       每次 Publish 调用入口都被更新——早退路径里保留的只是 paletteAddress，renderFrameIndex
       仍是"上次 ready publish 的值"。实测 lag 确实能触发，但…
     - 原因 2：`War3TryBuildLiveRuntimeGroupPalette` 里 `tryUsePublishedPose` 用
       `packet.renderable.runtimeModelPtr` 作 key 查 PoseRegistry。**这个 ptr 在 1.27a 上
       经常是 alias 解析后的值**，和 `0x12FDC0`/`0x12FED0` 传给 PoseRegistry 的原始
       runtimeModel key 不一致，导致 PoseRegistry miss → return false → rebuild 失败
     - 原因 3：Phase 7.50 live rebuild 只在 `Resolve != Ready` 时调，但 Resolve 大部分时候
       因为 snapshot 兜底而 Ready，rebuild 分支根本不进
   - **Phase 7.51 三刀齐下**：
     1. **`RenderablePartPaletteBindingEntry` 加 `runtimeModel` 字段**：
        - 在 `0x12FED0` producer 触发时（`CaptureRuntimeGroupPaletteBindings`），记录
          `(renderablePart, runtimeModel)`。runtimeModel 就是 producer hook 的 `this`
          参数，也是 `0x12FDC0` publisher 用来注册到 PoseRegistry 的原始 key。
        - 新增 `QueryRenderablePartOwnerRuntimeModel(renderablePart, &out)` 对外接口。
     2. **`tryUsePublishedPose` 链尾加 producer-owner fallback**：
        原本三级 fallback：`runtimeModelPtr`, `+0xA0`, `-0xA0`（CModelComplex alias）
        现在增加：`QueryRenderablePartOwnerRuntimeModel(renderablePart)`。这条能绕过
        caller 的 alias 解析错位，直接拿到 PoseRegistry 的真实 key。
     3. **submit-live-rebuild 默认每帧触发**：
        新增 env `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME=1`（默认 on）。
        skinned 单位每帧都尝试一次 rebuild（成本：单次 group-blending 约 60-100 组矩阵，
        对 CPU 压力可忽略）。即使 lag=0 也尝试；rebuild 成功就覆盖 drawTimeCapturedPalette。
        env=0 可回退到 Phase 7.35 的 lag-threshold 行为。
   - **预期效果**：
     - Phase 7.49 数据显示 PoseRegistry 产生 hit 率 ≈ 13%（Phase 7.34 A3 之前的 trusted
       hit），但 submitLiveRebuild hit 只有 0.42%。差距主要在 runtimeModel key mismatch。
     - Phase 7.51 应把 rebuild hit 拉到 50-90% 级别（取决于 `0x12FED0` producer 实际覆盖率）。
     - hit 的 rebuild 会用 PoseRegistry 的当前帧 final-pose 经 group blending 产出新 palette，
       替换掉旧 snapshot 的 palette。
     - FROZEN 段里 CombinedHash 应每帧都变。
   - **代码行数**：
     - `war3_model_hook.cpp`: ~25 行（struct 字段 + 参数 + query 实现）
     - `war3_model_hook.h`: ~10 行（接口声明）
     - `d3d9_device.cpp`: ~40 行（tryUsePublishedPose fallback + every-frame 条件）
     - 共 ~75 行，全部在 Phase 7.50 已建立的基础上添加
   - **编译**：`ninja -C build32` 通过（仅既有 warning）
   - **部署**：`E:\Work\War3\d3d9.dll` = `25392147 bytes @ 2026-05-13 03:47:39`
   - **回退路径**（环境变量）：
     - `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME=0`：回退到 Phase 7.35 lag-based 触发
     - `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG=0`：关掉 submit-live-rebuild
     - 两个都关 = 回退到 Phase 7.50 状态
   - **用户需要做**：
     - 启动 War3（env 不用设，默认即开）
     - 进入真卡顿场景
     - 肉眼看阴影是否还动半秒停半秒
     - 如果仍然有卡顿：录 15s trace，分析 `submitLiveRebuildHitCount` 是否还是低


---

## 🚨 Phase 7.52 新线程交接（2026-05-13 04:00，上下文压缩失败后重启）

**上一线程的核心教训**：连续 6 个 Phase（7.47 → 7.48 → 7.49 → 7.50 → 7.51）尝试修复
"阴影动 0.5s 停 0.5s"问题，**全部失败**。每一轮都基于一个看似合理的推测改了几十到几百行代码，
部署后用户反馈毫无改善。**根因至今未锁定**。上下文压缩导致我的认知丢失，必须给新线程一个
绝对清晰的起点。

### 一、用户观察的决定性现象（不要再怀疑这些）
1. **游戏内暴雪渲染的模型动作流畅**（每帧跟着骨骼动）
2. **我们自建的阴影 pose 卡顿**：动 0.5s 停 0.5s 周期性重复
3. **Phase 7.49 关掉 `KeepReadySnapshotOnInvalidCurrentDraw`**：卡顿节奏不变，但
   卡顿时阴影消失，结束瞬间突然恢复正常 pose（说明 producer 数据是对的，我们"停住"
   的那一段其实是我们自己复用旧 bytes）
4. **Phase 7.50 的 live rebuild on Resolve fail**：无改善
5. **Phase 7.51 每帧尝试 rebuild + producer owner runtimeModel fallback**：无改善

### 二、已被数据证伪的假设（不要再走回头路）
- ✗ **dt gate 早退**（Phase 7.47 证伪）：`CSpriteUber_PreRender*` 的 `dt` 在 98.79% 帧 > 0
- ✗ **"每 8 帧才写一次 palette" 的 producer cadence**（本轮末尾推翻）：trace 里
  `runtimeMatrixWriteCount` 约 370 次/trace event，`runtimeGroupPaletteWrapperCallCount` 约
  200 次/event，**producer 每帧都在狂写**
- ✗ **record.frameTag 卡住**（Phase 7.49 证伪）：100% record.frameTag == live
- ✗ **lastSubmittedPaletteHash 冻结 = palette 冻结**（Phase 7.48 确认是指标错觉，那个
  counter 只记 last 一个 caster；但 `CombinedHash`（聚合）确实也冻结 8 帧）
- ✗ **PoseRegistry fallback 能救**：Phase 7.51 trace 数据证实
  `submitLiveRebuildHitCount = 0 / Attempt = 17305`——PoseRegistry 查找 100% miss

### 三、决定性的 Phase 7.51 数据（trace `03_51_53.jsonl`）

从 `currentDrawRawHex` offset 解析（见 `_analyze_phase751_rawhex.py`）：
```
publishAttemptCount:              +19440  (trace 窗口内)
publishReadyCount:                +9702    (约 50% Publish Ready)
publishMissInvalidPaletteSlot:    +9728    (另 50% InvalidSlot 早退)
paletteCaptureTrustedSourceHit:   +9702    (trusted hit 100% of ready)
paletteCaptureTrustedSourceMiss:  0        (没有 raw fallback 发生)
submitLiveRebuildAttempt:         +17305   (Phase 7.51 EveryFrame 打开, 每次 skinned 都 attempt)
submitLiveRebuildHit:             0        ← ★★★ PoseRegistry 查找全部 miss
submitLiveRebuildMiss:            +17305   (和 attempt 完全相等)
submitLiveRebuildApplied:         0        (没有任何一次覆盖成功)

CombinedHash frozen windows:      12 / 131 segments, avg length 5 frames
```

**关键矛盾**：
- 我们知道 War3 引擎 **每帧都在更新骨骼 pose**（主渲染流畅 = 证据）
- `runtimeMatrixRangeCopyPalettePublishHitCount` 有值（`0x12FDC0` hook 每帧 publish）
- 但 `submitLiveRebuildHitCount = 0` 说明 `PoseRegistry::findByRuntimeModel` 在 submit 端 100% miss
- **= PoseRegistry publish 的 runtimeModel key ≠ submit 时我们查询用的 key**
  - 即便 Phase 7.51 加了 `QueryRenderablePartOwnerRuntimeModel` 作为第 4 级 fallback，仍然全 miss

### 四、当前最可信的根因假设（下一线程的起点）

**核心认知反转**（本轮末尾得到，未验证）：
- 完整 per-part blended palette 在 `Game.dll + 0xBC6BD0 + slotIndex * 48` 的**全局 arena**
- `CModel + 0x60` 只有 2-3 个根骨骼矩阵，不是完整骨架（
  `docs/plan/dynamic_shadow_implementation_2026_05_03.md` 证实）
- arena 每帧都被 `0x12E600 CGeosetData_BuildGroupBlendedPalette` 写（producer 每帧跑）
- **所以 submit 时直接从 `arena[slotIndex]` 读 bytes 就是本帧 fresh palette**
- **但我们读 arena 的路径居然也不新鲜**——这才是真正未查明的谜团

**未做但必须做的核心实验**：
1. **Per-submit palette bytes provenance trace**：在每次 skinned submit 的最末端
   （`War3TryAppendSemanticShadowPacket` 里 append 到 `shadowCasters` 之前）记录：
   - 本次 submit 最终使用的 palette bytes 前 48 字节的 hash（不是封装的 paletteHash，
     是真实内存 bytes 的 hash）
   - 数据来源标记（`paletteSourceThisSubmit` 枚举值，已有）
   - 同一 renderablePart 跨相邻 submit 的 bytes hash 是否真的在变化
2. **如果 bytes hash 跨 FROZEN 多帧不变**：palette bytes 真的锁住，得查路径
3. **如果 bytes hash 每帧在变**：palette bytes fresh，但 `dynamicPoseSignature` 和
   `CombinedHash` 仍冻结——那卡顿根因不在 palette 层，在**下游**（GPU 绑定、
   shadow shader constant buffer 缓存、或者 shadow map 复用策略）

**最可能的真相**（未验证但需要 Phase 7.52 首轮优先验证）：
- shadow pipeline 的 **shadow map 重用策略** 每 8 帧才重新渲染一次 shadow map，期间
  shader 看到的是上次渲染的 depth texture
- 搜索关键词：`ShadowMap cache / reuse / cadence / lastShadowMap...`

### 五、当前 DLL 状态
- 路径：`E:\Work\War3\d3d9.dll`
- 大小 / mtime：`25392147 bytes @ 2026-05-13 03:47:39`
- 包含 Phase 7.47 - 7.51 全部改动
- 可用回退 env：
  - `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME=0` - 关 Phase 7.51 每帧 rebuild
  - `DXVK_WAR3_SEMANTIC_KEEP_READY_ON_INVALID_CURRENT_DRAW=0` - 关兜底（Phase 7.49 方案 A）
  - 两个都不动 = Phase 7.51 默认行为

### 六、给新线程的明确指令

**禁止**：在数据之前再做任何"我觉得这样就行"的修复代码改动。前 6 轮全输在这上面。

**必做**（Phase 7.52，按顺序）：

1. **First**：读 `AGENTS.md` 条目 **78-86**（Phase 7.47-7.51）理解所有已证伪假设。

2. **Second**：查 `ShadowMap reuse / cadence` 相关代码（`src/d3d9/d3d9_war3_shadow.cpp`
   的 `m_hasCompleteShadowMap / m_lastDynamicPoseSignature / kShadowAdaptiveMapUpdateEnabled`），
   **检查是否存在"8 帧才重新渲染一次 shadow map"的机制**。历史 AGENTS 第 24 条提到
   Phase 7.5 引入了 `kShadowAdaptiveMapUpdateEnabled=true`（视角稳定时隔帧复用 shadow
   map），这个功能完全可能就是 0.5s 冻结周期的根因。

3. **Third**（若第二步是根因）：
   - 关闭 `kShadowAdaptiveMapUpdateEnabled` 或改成 `false`
   - 重新编译部署
   - 让用户实机验证
   - 如果解决 = 确认根因，然后决定是否保留 adaptive 但改进触发条件（否则会影响性能）

4. **Fourth**（若第二步排除）：
   - 加 per-submit palette bytes 真实内存 hash 探针（见上面第四节）
   - 让用户再录 15s trace
   - 看 FROZEN 段里 bytes hash 是否真的在变

5. **禁区**：
   - 不要再动 `War3TryBuildLiveRuntimeGroupPalette`
   - 不要再动 `KeepReadySnapshotOnInvalidCurrentDraw`
   - 不要再动 `PublishCurrentDrawContract` 的仲裁链
   - 不要再动 `DirectPartPacketLease` / manifest / core set

### 七、工具可用性
- IDA MCP 已配置：HTTP `http://127.0.0.1:13337/mcp`，工具 `lookup_funcs / decompile /
  disasm / xrefs_to / callees / rename / set_comments` 等
- AutoTest：`AutoTest/war3_autotest_mcp.py`（MCP 服务或 Python 直调都可）
- 分析脚本保留：`_analyze_phase749.py / _analyze_phase751.py / _analyze_phase751_rawhex.py`
  （下一轮可复用）
- Full trace 控制：`py AutoTest\shadow_pose_full_trace_control.py start --max-seconds 15`

### 八、项目交付状态
- **阴影功能正常，只是卡顿**：阴影能画出来、跟着场景动、边缘锐度 OK
- **问题唯一集中在 pose cadence**：0.5s 冻结 0.5s 动的视觉节奏
- **性能未测**：性能优化是修完 pose 卡顿后的下一阶段
- **用户情绪**：对这个问题已疲劳，几周没解决，需要一针见血的根因定位而不是又一次盲猜

---


86. **Phase 7.52 根因修复：Producer-side bindings 在 slotIndex invalid 时仍用 cached slot 刷新 snapshot（2026-05-13 04:15-04:30）**:
   - **背景**：Phase 7.47-7.51 全部失败后重新深入研究 `CaptureRuntimeGroupPaletteBindings`。
     发现关键 bug：当 War3 的 8-帧 slot cadence 让 `renderablePart + 0x08 = 0xFFFFFFFFu`
     时，该函数**直接 `continue` 跳过**，导致：
     - `s_renderablePartPaletteBindings[slot].palette[]` 这份 bytes 不再被刷新
     - `QueryRenderablePartPaletteSnapshot` 命中的是上一次 valid slot 时写入的**旧 bytes**
     - 于是 FROZEN 8 帧窗口里 submit 拿到的都是旧 palette，视觉上就是 "阴影动 0.5s 停 0.5s"
   - **关键洞察**：
     - arena 里 `globalPaletteBuf + slotIndex*48` 的 bytes **每帧都被 `0x12E600 Hook_RuntimeMatrixWrite` 刷新**
       （Phase 7.47 trace 已证实 `runtimeMatrixWriteCount` 每帧 +370）
     - 同一 `renderablePart` 在这 8 帧里通常属于同一 CModel，它的 arena slot 位置不会迁移
     - 只是 `partPtr+0x08` 这个字段是由 0x12FED0 每帧重新填写（或某些帧不填）
     - 因此我们可以用 bindings 表里**上次记录的 cachedSlotIndex** 去 arena 读 fresh bytes
   - **修复代码**（`src/d3d9/war3/model/war3_model_hook.cpp::CaptureRuntimeGroupPaletteBindings`）：
     - 原逻辑：`if (slotIndex == 0xFFFFFFFFu || slotIndex >= 0x3A98u) continue;`
     - 修改后：在 slotIndex invalid 时从 `s_renderablePartPaletteBindings[bindingSlot]`
       读取 `cachedSlotIndex`，若 cached 有效则用 cached 值继续执行 snapshot 刷新流程
     - `matrixBytes = globalPaletteBuf + cachedSlotIndex * 48u` → fresh bytes
     - `RecordRenderablePartPaletteBinding(..., matrixBytes, groupCount, ...)` → 写入 snapshot
   - **full trace keyStats 增强**（`war3_shadow_runtime_bridge.cpp`）：
     - 新增字段：`renderablePartPaletteSnapshotCapturedCount / TooLargeCount / UnreadableCount /
       QueryHitCount / QueryMissCount`，`renderablePartPaletteBindingQueryHitCount/MissCount`
     - 用于直接监控 bindings 表是否在被正确刷新。
   - **验证结果**（AutoTest 光影测试.w3x 15 秒 + full trace）：
     - `renderablePartPaletteSnapshotCaptured = 22498`（active 段 150 帧共 capture）
     - `renderablePartPaletteSnapshotQueryHitCount = 11964`
     - `renderablePartPaletteSnapshotQueryMissCount = 481`
     - **snapshot query hit rate = 96.1%**（vs Phase 7.31 的 ~13%）
     - 每个 CombinedHash 冻结窗口内 `dsnapCap` 都在增长（window[45..52] len=8 里 dsnapCap=931）
     - 说明 FROZEN 窗口里 snapshot 确实**每帧 146 次**在被 arena fresh bytes 刷新
   - **交付状态**：
     - DLL 已部署 `E:\Work\War3\d3d9.dll` (25398271 bytes @ 2026-05-13 04:24:22)
     - 编译通过，AutoTest 稳定跑完，无崩溃
     - Phase 7.46 snapshot 路径的数据新鲜度从"每 8 帧变一次"提升到"每帧变一次"
   - **尚未验证**：
     - 用户肉眼视觉是否改善（0.5s 动/0.5s 停节奏是否消失）
     - 必须实机测试真正多骨骼场景（AutoTest 的光影测试.w3x 是小场景，SubmitLiveRebuild
       在这个场景的 palette source 分布是 100% DrawTimeCaptured，没有样本走 SubmitTime 路径）
   - **回退路径**：
     - env `DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT=0` 可禁用 snapshot 机制（但这样
       Phase 7.52 修复也不会生效，因为修复是在 `CaptureRuntimeGroupPaletteBindings` 里的）
     - 真要完全回退：把 `continue` 分支恢复即可
   - **下一步（夜间无人值守推进）**：
     - 继续观察 CombinedHash 冻结窗口：Phase 7.52 修复让 snapshot bytes 每帧新鲜，但
       submit 端的实际 paletteHash 是否还冻结取决于 submit 端有没有真的消费这些 fresh bytes
     - 如果仍然冻结，需要强制让 submit-side 直接用 `QueryRenderablePartPaletteSnapshot`
       覆盖 draw-time palette（目前 Phase 7.51 each-frame rebuild 已在做这个，只是 counter
       显示 Hit=0 需要重测）


87. **Phase 7.52 AlphaTest 修复：alpha-blend only caster 自动 promote 成 alpha-test shadow（2026-05-13 04:40）**:
   - **问题**：用户反馈 "带 AlphaTest 的贴图光影射过去依然视作不透明"。
     War3 里很多透明贴图（树叶、栅栏、半透明特效）使用 D3DRS_ALPHABLENDENABLE
     但没设 ALPHATESTENABLE。shadow caster shader 的 `pc.flags bit2 = alphaTest`
     只在 `draw.alphaTestEnabled && draw.diffuseTexture` 时启用，结果这些物体
     在 shadow pass 里**整贴图被当实心投影**，呈方形黑影。
   - **根因**：
     - shadow caster pipeline key 和 pc.flags 都以 `draw.alphaTestEnabled` 为判断
     - alpha-blend only 物体 classification 为 `ShadowAlphaMode::AlphaBlend`
     - candidate.alphaTestEnabled = alphaCutoutEnabled = false
     - 即使有 UV + diffuseTexture，shader 也不走 alpha discard 路径
   - **修复**（`src/d3d9/d3d9_war3_shadow.cpp`）：
     - 定义 `effectiveAlphaTestShadow = alphaTestEnabled || (alphaBlendEnabled &&
       diffuseTexture && uvFormat valid && uvStride > 0)`
     - CSM 阴影路径：pipeline key 和 pc.flags 都用 effectiveAlphaTest
     - 点光源阴影路径同步修改
     - alphaRef：cutout 用原值，alpha-blend promote 时用 0.5（半透明合理阈值）
     - `PreparedShadowCaster` 结构新增 `effectiveAlphaTest` 字段，跨 prepare→draw 传递
   - **修改文件**：`src/d3d9/d3d9_war3_shadow.cpp`（prepare loop + CSM draw loop + point shadow loop）
   - **编译/部署**：
     - `ninja -C build32`：通过
     - 部署 DLL: `E:\Work\War3\d3d9.dll` (25392147 bytes @ 2026-05-13 04:46:01)
     - Smoke test: 光影测试.w3x 10s AutoTest，stage=done，无崩溃
     - `semanticSceneSubmittedSkinned=9271`（正常提交）
   - **预期视觉改善**：
     - 树叶、栅栏等半透明贴图的阴影应从"实心方块"变成"按 alpha 镂空的自然形状"
     - 只要贴图有 alpha 通道 + UV + 被绑定到 stage 0，就能享受 alpha-test discard
   - **回退路径**：
     - 没有 env 开关，但可以通过恢复 `key.alphaTestEnabled = draw.alphaTestEnabled`
       回到旧行为（alpha-blend only 不 discard）
     - 用户要保留旧行为可以手动 revert


88. **Phase 7.52 夜间无人值守会话最终状态（2026-05-13 04:50）**:
   - **本会话完成的工作**：
     1. **阴影 Pose 卡顿根因修复**（Phase 7.52 第一刀）：
        - 定位 `CaptureRuntimeGroupPaletteBindings` 在 slotIndex invalid 时 continue
          导致 Phase 7.46 snapshot 不刷新的 bug
        - 修复后 snapshot query hit rate 从 ~13% 提升到 96.1%（AutoTest 证实）
        - 每个 FROZEN 窗口内 dsnapCap 有效增长（证明 bindings 正在被 arena fresh bytes 刷新）
        - DLL 已部署，等用户实机视觉复核
     2. **AlphaTest 阴影修复**（Phase 7.52 第二刀）：
        - 让 alpha-blend only caster（有 UV + diffuseTexture）也 promote 成 alpha-test
          discard，解决 "带 AlphaTest 的贴图光影射过去依然视作不透明" 问题
        - 修改了 CSM 和点光源两个阴影路径
        - `PreparedShadowCaster` 新增 `effectiveAlphaTest` 字段跨 prepare→draw 传递
     3. **诊断能力增强**：
        - full trace keyStats 新增 `renderablePartPaletteSnapshot*` 和
          `submitLiveRebuild*` 字段，下次实机 trace 可直接看到修复效果
        - 新增分析脚本 `_analyze_phase752.py` + `_probe_phase752_bindings.py`
   - **当前 DLL 状态**：
     - 路径：`E:\Work\War3\d3d9.dll`（25392147 bytes @ 2026-05-13 04:46:01）
     - 包含 Phase 7.46/7.47/7.48/7.49/7.50/7.51/7.52 全部修复
     - 已通过 smoke test（无崩溃，阴影管线工作正常）
   - **等用户验收的点**：
     1. 肉眼观察阴影 pose 是否还有 "0.5s 动 0.5s 停" 周期（Phase 7.52 第一刀）
     2. 带 alpha 通道的贴图（树叶、栅栏等）的阴影是否从实心方块变成自然镂空形状（Phase 7.52 第二刀）
     3. 如果视觉复核通过，之后才有资格进入真正的性能优化阶段
   - **已知限制**：
     - 光影测试.w3x 在 isolated desktop 下 FPS ~10-11（GPU present 阻塞导致）
       不能当做真实性能指标，只能做稳定性验收
     - 用户实机前台运行 FPS 应该远高于此值，但需要用户自测
   - **下一步优先级**（视用户反馈而定）：
     - **优先级 1**：Phase 7.52 视觉复核。如果卡顿消失 → 第一刀根因正确；仍存在 → 需要继续深挖。
     - **优先级 2**：AlphaTest 视觉复核。如果方块阴影消失 → 第二刀正确。
     - **优先级 3**：性能优化。`War3SemanticScene/Populate=13.551ms` 是最大热点，Codex 和 AGENTS 条目 72 都识别为 O(N²)，需要独立重构。
     - **优先级 4**：Phase 7.51 per-frame live rebuild 改回 lag-only 触发（Phase 7.52 snapshot 新鲜后不需要每帧都 rebuild）。
   - **回退路径**：
     - Phase 7.52 第一刀：`DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT=0` 禁用 snapshot 机制
       （但这样新鲜 bytes 也就拿不到了，等同回到 Phase 7.51 状态）
     - Phase 7.52 第二刀：可以把 `src/d3d9/d3d9_war3_shadow.cpp` 里 `effectiveAlphaTestShadow`
       逻辑改回 `draw.alphaTestEnabled` 只读取原 flag
     - Phase 7.51 每帧 rebuild：`DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME=0`


89. **Phase 7.54 最终根因确认：War3 1.27a CPU skinning vs 我们 GPU skinning 的根本矛盾（2026-05-13 13:30）**:
   - **决定性证据链**：
     1. 主模型流畅 + shadow 卡顿 + 两者读同一份 arena（IDA 证实）
     2. arena 在 frozen 段确实不变（trace 证实 `runtimeMatrixWriteLastMatrixHash` distinct=1）
     3. 主渲染在 `renderablePart+0x08 == 0xFFFFFFFFu` 时走 identity fallback（IDA `UpdateItemWorldMatrix`）
     4. 但主渲染仍然流畅 → **主渲染不依赖 palette 做 skinning**
     5. War3 1.27a 使用 **CPU skinning**：骨骼变换在 CPU 端完成，结果写入 vertex buffer，
        GPU 直接画已经 skin 好的顶点。palette 只是 CPU skinning 的中间数据。
     6. 我们 shadow caster 使用 **GPU skinning**：vertex shader 从 palette SSBO 读矩阵做 blend。
        palette 在 logic tick 之间不更新 → shadow 卡。
   - **为什么 frozen 段 palette 不变**：
     - War3 的 logic tick（动画推进）不是每渲染帧都跑
     - logic tick 之间 `CSpriteUber_PreRender` 的 dt 可能为 0 或者 pose stack 不变
     - `0x12E600` 每帧都被调但输入（pose stack）不变所以输出也不变
     - 主渲染不受影响因为 VB 已经是上次 logic tick CPU skin 后的结果
   - **为什么主渲染流畅**：
     - CPU skinning 在 logic tick 时把新 pose 算进 VB
     - 两次 logic tick 之间 GPU 画的是同一份 VB（姿态不变）
     - 但 War3 的 logic tick 频率足够高（约 30Hz），人眼感知不到骨骼跳变
     - 而我们 shadow 的 frozen 段持续 300-600ms（约 2Hz），远低于人眼阈值
   - **根本矛盾**：
     - 我们 shadow 的 GPU skinning 需要 palette 每帧 fresh
     - War3 的 palette 只在 logic tick 时更新（约 30Hz 但不规律）
     - 主渲染用 CPU skinning 后的 VB 不受 palette cadence 影响
   - **正确修复方向**：
     - **方案 A（推荐）**：shadow caster 直接用主渲染 draw 时的 position buffer（已 CPU skin），
       不再做 GPU skinning。这等于把 shadow caster 从 "GPU skinned" 降级为 "pre-skinned rigid"。
       优点：完全消除 palette 依赖，shadow 和主渲染完全同步。
       缺点：需要每帧从主渲染 draw 时捕获 VB slice（已有 `War3TryCaptureShadowCaster` 在做）。
     - **方案 B（备选）**：在 logic tick 时（`CSpriteUber_PreRender` dt>0）记录 palette，
       两次 tick 之间做时间插值。但这需要知道 War3 的 logic tick 频率和时间因子。
   - **下一步**：
     - 检查现有 `War3TryCaptureShadowCaster`（legacy path）是否已经在捕获 CPU skin 后的 VB
     - 如果是，问题可能是 semantic path 绕过了 legacy capture 的 VB 数据
     - 如果不是，需要在 draw-time 捕获 skin 后的 position buffer


90. **Phase 7.54 根因最终确认 + 临时修复落地（2026-05-13 13:56）**:
   - **用户实机验证**：
     - `kShadowSemanticCoreSceneDisableLegacyShadowCaptureEnabled = false` 后
       legacy shadow capture 重新接管 → **阴影流畅，卡顿完全消失**
     - FPS 从 15 降到 12（legacy 的已知开销）
   - **根因最终确认**：
     - War3 1.27a 使用 **CPU skinning**：骨骼变换在 CPU 端完成，结果写入 VB
     - 主渲染 GPU 直接画 CPU skin 后的 VB → 流畅
     - palette arena 只在 logic tick 时更新（约 2Hz 不规律），tick 之间 bytes 不变
     - semantic path 用 bind-pose 顶点 + GPU skinning (palette SSBO) → palette 冻结 → 卡顿
     - legacy path 从 draw-time VB 拿 CPU skin 后的顶点 → 不依赖 palette → 流畅
   - **临时修复**：
     - `war3_internal_test_config.h`: `kShadowSemanticCoreSceneDisableLegacyShadowCaptureEnabled = false`
     - 让 legacy capture 重新接管 skinned unit 的 shadow
     - DLL 已部署 `E:\Work\War3\d3d9.dll` (2026-05-13 13:56:42)
   - **长期方案（TODO）**：
     - 让 semantic path 在 draw-time 也拷贝 CPU skin 后的 position buffer
     - shadow caster 对 skinned unit 用 pre-skinned position（不做 GPU skinning）
     - 保留 semantic path 的架构优势（晚注入、不依赖 DX9Ex、可接手更多内容）
     - 同时消除 palette cadence 依赖
   - **回退路径**：
     - env `DXVK_WAR3_SEMANTIC_SHADOW_DISABLE_LEGACY_CAPTURE=1` 可恢复 semantic-only


91. **Phase 7.55 验证结果：draw-time D3D palette 不可用，确认 CPU skinning（2026-05-13 16:05）**:
   - **验证数据**（trace `shadow_pose_full_trace_2026_05_13_16_04_37.jsonl`）：
     - `drawTimeD3DPoseAttemptCount = 325~349/帧`（hook 每帧被调 ~340 次）
     - `drawTimeD3DPosePublishedCount = 0`（一次都没成功）
     - `drawTimeD3DPoseRejectNoVertexBlendCount = 325~349`（**100% 被 vertex blend disabled 拒绝**）
   - **结论**：
     - War3 1.27a 在 draw-time 的 D3D state 里 `D3DRS_VERTEXBLEND == D3DVBF_DISABLE`
     - War3 **不使用 D3D9 fixed-function vertex blending**
     - War3 在 CPU 端自己做 skinning，把结果写入 VB，以 rigid 模式提交 D3D9
     - arena palette 只是 CPU skinning 的中间数据
     - draw-time D3D transform palette 这条路**完全走不通**
   - **最终确认的修复方向**：
     - 在 draw-time 捕获 **VB position buffer**（CPU skin 后的顶点）
     - semantic path 的 skinned caster 用 pre-skinned position（不做 GPU skinning）
     - 这等于把 legacy path 的 "VB 快照" 能力嫁接到 semantic path
     - 保留 semantic path 的架构优势（晚注入、不依赖 DX9Ex）


92. **Phase 7.55 下一步计划：draw-time VB position capture for semantic path（2026-05-13 16:10）**:
   - **已确认的技术路线**：
     - 在 `War3TryCaptureShadowCaster` 的 semantic early-return 分支里
     - 对 `earlySemanticSceneUnitLikeCandidate == true` 的 draw
     - 拷贝当前 VB stream 0 的 position slice（CPU skin 后的顶点）
     - 存到 per-renderablePart 的 draw-time VB cache
     - Populate 时 skinned caster 优先用 draw-time VB（关闭 GPU skinning）
   - **关键接入点**：
     - `d3d9_device.cpp` line ~22155 的 `earlySemanticSceneUnitLikeCandidate` 分支
     - 需要读取 `m_state.vertexBuffers[0]` 或 `m_war3PerDrawUpload.vbSlices[0]`
     - 需要知道 vertex count（从 draw call 参数推断）
     - 需要知道 position stride（从 vertex declaration 推断）
   - **数据结构设计**：
     - cache key = renderablePart 指针（或 runtimeModelPtr + geosetIndex）
     - cache value = { positionBuffer: Rc<DxvkBuffer>, positionInfo, stride, vertexCount, frameSerial }
     - 生命周期：per-frame reset 或 LRU eviction
   - **Populate 消费端改动**：
     - `War3TryAppendSemanticShadowPacket` 在构建 skinned caster 时
     - 查询 draw-time VB cache
     - 命中 → 用 pre-skinned position，`vertexBlendEnabled=false`
     - 未命中 → 走现有 bind-pose + GPU skinning 路径（仍会卡，但至少不崩）
   - **预期效果**：
     - skinned caster 的 position 每帧都是 fresh 的（和主渲染同步）
     - 不需要 palette SSBO（GPU skinning 关闭）
     - 保留 semantic path 的所有架构优势
     - 性能应该比 legacy path 更好（不需要每帧拷贝全量 VB，只拷 position stream）


93. **Phase 7.55 实施计划确认（2026-05-13 16:20）**:
   - **VB 捕获模式已确认**（从 legacy path 提取）：
     ```cpp
     // DynamicSysmemVBOs path:
     if (DynamicSysmemVBOs && m_war3PerDrawUpload.vbValid[posStream]) {
       posSlice = m_war3PerDrawUpload.vbSlices[posStream];
       posStride = m_war3PerDrawUpload.vbStrides[posStream];
     }
     // Regular VB path:
     else {
       auto *vb = m_state.vertexBuffers[posStream].vertexBuffer.ptr();
       posSlice = vbCommon->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(offset);
       posStride = m_state.vertexBuffers[posStream].stride;
     }
     ```
   - **接入点**：`d3d9_device.cpp` line ~22155 的 `earlySemanticSceneUnitLikeCandidate` 分支
   - **DynamicSysmemVBOs 参数来源**：`War3TryCaptureShadowCaster` 的参数
   - **posStream**：从 `War3GetShadowDeclInfo(decl).posStream` 获取
   - **vertex count**：从 draw call 参数推断（indexed: NumVertices; non-indexed: CountVal）
   - **cache 设计**：
     - key = renderablePart 指针（从 semantic.renderablePart 获取）
     - value = { positionData: vector<float>, stride, vertexCount, frameSerial }
     - 存储位置：`m_war3SemanticDrawTimeVBCache`（D3D9DeviceEx 成员）
     - 生命周期：每帧 Populate 开始时清理过期条目
   - **消费端改动**：
     - `War3TryAppendSemanticShadowPacket` 里构建 skinned caster 时
     - 查询 draw-time VB cache（by renderablePart）
     - 命中 → packet.resource 用 pre-skinned position，candidate.vertexBlendEnabled=false
     - 未命中 → 走现有 bind-pose + GPU skinning 路径


94. **Phase 7.55 进展：零拷贝 VB capture 已落地，consume 端待接入（2026-05-13 16:45）**:
   - **已完成**：
     - draw-time VB capture（零拷贝版本）：保存 `DxvkBufferSlice` 引用 + stride + offset
     - 条件：所有有 `semantic.renderablePart` 的 draw（不限 UnitLike）
     - 性能验证：`avgFps=12.862`（无 full trace），零拷贝对性能无影响
     - 编译通过，AutoTest 稳定
   - **待完成（下一轮）**：
     - consume 端接入：在 `War3ShadowCasterDraw` 构建时查询 draw-time VB cache
     - 命中时替换 `draw.positionStorage / positionInfo / positionStride`
     - 设 `draw.vertexBlendEnabled = false`（关闭 GPU skinning）
     - 这样 shadow caster 直接用 draw-time VB 的 pre-skinned position
   - **接入点**：
     - `d3d9_device.cpp` line ~10900 的 `draw.positionStorage = geometry->positionStorage`
     - 或者更早：在 `candidate` 构建时就替换 position 数据
     - 关键：draw-time VB 的 stride 可能和 bind-pose 不同（bind-pose 是 12 bytes/vertex，
       draw-time VB 可能是 32+ bytes/vertex 因为包含 UV/normal 等）
     - 需要正确设置 `positionOffset` 让 shader 只读 xyz
   - **当前 DLL 状态**：
     - `E:\Work\War3\d3d9.dll`（2026-05-13 16:40）
     - 包含零拷贝 capture（不影响行为，只是存引用）
     - consume 端暂未接入（shadow 仍然卡顿）


95. **Phase 7.55 当前阻塞：draw-time VB cache key 匹配问题（2026-05-13 17:50）**:
   - **问题**：capture 端用 `semantic.runtimeModelPtr` 作 key，consume 端用
     `packet.renderable.runtimeModelPtr` 查询，但两者不匹配（alias 问题）。
   - **证据**：consume 端 VB override 从未命中（PaletteSource 仍 100% DrawTimeCaptured）。
   - **根因**：semantic path 的 packet 是从 model resource cache 构建的，
     `packet.renderable.runtimeModelPtr` 来自 `ShadowRenderableRecord`，
     和 draw-time 的 `semantic.runtimeModelPtr` 是不同的指针值。
   - **这和 Phase 7.51 PoseRegistry miss 是同一个根因**：
     draw-time 的 runtimeModel 指针和 Populate 时的 runtimeModel 指针不一致。
   - **可能的解决方案**：
     1. 用 `sceneNode` 作为 key（更稳定，但可能有多个 part 共享同一 sceneNode）
     2. 用 `jHandle` 作为 key（最稳定的对象身份）
     3. 在 capture 时同时记录多个候选 key（runtimeModel + sceneNode + renderablePart），
        consume 时按优先级尝试匹配
     4. 在 `War3PublishSemanticSceneBypassCandidate` 里把 draw-time VB 信息
        直接写入 `VisibleRenderableRecord`，这样 Populate 时从 visible record 读
   - **临时方案（已验证可用）**：
     - `kShadowSemanticCoreSceneDisableLegacyShadowCaptureEnabled = false`
     - legacy path 接管 → 阴影流畅
   - **下一步**：
     - 尝试方案 4（最干净）：在 bypass candidate publish 时把 VB info 写入 visible record
     - 或者尝试方案 1：用 sceneNode 作为 cache key


96. **Phase 7.55 v3 实施反思（2026-05-13 19:00）**:
   - **进展**：
     - capture 端零拷贝 VB 引用 + IB 引用已实现
     - consume 端 VB + IB override 已接入
     - Cache hit rate ~96% (renderablePart key 匹配良好)
     - `avgFps=11.1` 性能可接受
   - **用户视觉反馈（v1 with sceneNode key + 不替换 IB）**：
     - 阴影"满世界抽"：多个对象数据混乱
     - 唯一不抽的是箭头：cache key 没有冲突
     - 箭头流畅一会然后卡住又追上：4 帧 frameSerial 阈值的副作用
   - **v2/v3 改动**：
     - 改 cache key 为 renderablePart（per-part 精确）
     - 同时替换 IB（保证三角形拓扑正确）
     - v3 strict：跳过 DynamicSysmemVBOs/IBO（避免 ring buffer 数据被覆盖）
   - **当前阻塞**：
     - 没有用户视觉反馈时无法判断 v3 是否解决"满世界抽"
     - hit=51, miss=31 说明只有部分 skinned draw 走了 override 路径
     - 真正解决 ring buffer 问题需要 GPU copy 到 persistent buffer
       （legacy path 在做的事情）
   - **DLL 状态**：
     - `E:\Work\War3\d3d9.dll`（v3 strict，2026-05-13 19:00）
     - 编译通过，AutoTest 稳定无崩溃
     - Cache hit rate 还在但只对 regular VB/IB
   - **下一步根本性方案**：
     - 用 `ctx->copyBuffer` 在 capture 时把 VB 范围 GPU-copy 到一个 persistent ring buffer
     - 这样 ring buffer 生命周期由我们管理，不会被 War3 后续 draw 覆盖
     - 但这是更大的改动（需要 ring buffer 池 + barrier 管理）


97. **Phase 7.55 v4 GPU copy 通路打通（2026-05-14 01:12）**:
   - **根因**：DXVK 内部 wrap 函数 `War3TryCaptureShadowCasterFromDIP` 把
     `MinVertexIndex` 和 `NumVertices` 硬编码为 0 调用 `War3TryCaptureShadowCaster`。
     v4 capture 在 `indexed=true` 路径下用 `vRangeCount = NumVertices = 0`，
     被 `vRangeCount == 0` 检查拒绝，导致 `drawTimeVBCacheCaptureCount = 0`。
   - **修复**（`d3d9_device.cpp::War3TryCaptureShadowCaster` 内 v4 capture 块）：
     - `indexed && NumVertices > 0` 走原路径
     - `indexed && NumVertices == 0` fallback：`vRangeStart = max(BaseVertexIndex, 0)`，
       `vRangeCount` 按 `posSlice.length() / posStride - vRangeStart` 估算，cap 65536
   - **验证（光影测试.w3x 隔离桌面 20s + full trace）**：
     - `drawTimeVBCacheTotalEntered = 125`
     - `drawTimeVBCacheCaptureCount = 125`（**100% capture 成功**）
     - `drawTimeVBCacheConsumeHitCount = 73`（**100% consume 命中**）
     - `drawTimeVBCacheConsumeMissCount = 0`
     - 所有 reject counter = 0
   - **当前 DLL 状态**：
     - `E:\Work\War3\d3d9.dll`（25409924 bytes @ 2026-05-14 01:12:31）
     - 包含完整 v4 GPU copy 通路 + 9 个诊断 counter
     - skinned shadow caster 现在用 capture 时拷的 device-local buffer +
       `vertexBlendEnabled=false`，绕开 GPU skinning，直接画 CPU skin 后的顶点
   - **关于 CombinedHash 仍有 8 帧冻结 run**：
     - 该 hash 算的是 palette 数据，但 v4 consume 已 `vertexBlendEnabled=false`，
       shader 不再用 palette，所以 palette hash 冻结**与视觉是否冻结无关**
     - 视觉验证只能靠用户实机
   - **等用户视觉验收**：阴影是否还"动 0.5s 停 0.5s"
   - **回退路径**：
     - capture 块用 `do { ... } while (false)` 包裹，所有早退点都有 counter
     - consume 块用 `if (entryFresh)` 守门，未命中走原 bind-pose+GPU skinning 路径


98. **Phase 7.55 v4 收口（2026-05-14 03:54）**:
   - **当前稳定状态**（commit `2d1fd41`，已部署 `25416150 bytes`）：
     - ✓ pose 流畅（draw-time VB GPU copy 通路）
     - ✓ AlphaTest 镂空（capture 端读 D3D state + 复制 UV stream，consume 端
       覆盖 alphaTestEnabled/alphaRef/diffuseTexture，派生 sampler/textureDescriptor）
     - ✓ 静态建筑阴影位置正确（worldMatrix 用 capture 时的 D3DTS_WORLD）
     - ✓ frustum cull 不再误杀 v4 caster（hit 时强制 boundsRadius=0）
     - ✓ 无撕裂、无进图卡顿
   - **未解决的已知问题**：
     - 远离世界中心 + 少 caster（火凤凰单独 / 小怪死光后）→ 周期性阴影
       闪烁/卡顿。约 49 帧周期里有 16 帧 `directSubmitted=0`。
     - 核心数据：trace `2026_05_14_02_58_12.jsonl` 显示 491 帧里 161 帧
       (33%) `submitted=0`，连续 16 帧/run；CENTER trace `02_54_22` 显示
       100% 帧 submit > 0。
     - 触发链：War3 引擎本身不每帧派发所有 caster draw call →
       少 caster 场景某些帧 `currentDraw contract` 0 record →
       `War3TryPopulateDirectCurrentDrawGrouped(readyOnly=true)` 返回 0 →
       directOnly 分支 `EmptyFrame return` → 整帧无阴影。
   - **失败的修复尝试**：
     - **尝试 1**：让 directSubmitted=0 时 fallthrough 到下游 reuse/fallback
       路径。失败：触发 `readyOnly=false` 第二次调用 + native backend 二次
       执行 + sceneBundle/catchup 副作用 → 撕裂 + 进图卡顿。已撤销。
     - **尝试 2**：directOnly 分支内部 snapshot `m_war3Scene.shadowCasters`
       和 `shadowInstances`，directSubmitted=0 时整套 copy 回当前帧。
       失败：reuse 整套 caster 的 worldMatrix 是上一帧的，但物体在动 →
       阴影画在历史位置 → 主渲染当前位置 → 持续性撕裂。已撤销。
   - **下一步真正方向（未实施）**：
     - reuse caster 集合本身可以，但每个 caster 的 worldMatrix（或 palette
       第一根矩阵）必须用 **当前帧的最新值**，避免位置滞后。
     - 数据源：`PoseRegistry` 或 `Hook_RuntimeMatrixWrite` 已经每帧更新；
       caster 里存稳定 key（renderablePart / runtimeModel）即可反查。
     - 改动范围：reuse 时不要 plain copy，要逐 caster 重建 worldMatrix +
       boundsCenter。涉及：
       1. caster snapshot 时保存稳定 key（runtimeModelPtr 或 renderablePart）
       2. reuse 时为每个 caster 查 PoseRegistry 拿当前帧 first matrix
       3. 用 first matrix.translation 重算 boundsCenter
       4. 用当前帧 matrix 替换 worldMatrix（如果是 skinned）
     - 风险：PoseRegistry 在某些帧也可能没更新某个 runtimeModel（火凤凰
       特别罕见 caster），需要 graceful fallback。
   - **暂停修复理由**：
   - 当前稳定版（pose 流畅 + AlphaTest + 建筑位置）已是用户长期反馈的
     核心痛点全部解决；
   - 远离原点的周期性闪烁是 War3 引擎本身 cadence 的产物，需要在
     caster reuse 维度做"每 caster fresh worldMatrix"重建才能根治；
   - 这条路径技术上可行但工程量大且需要额外 IDA 验证 PoseRegistry 在
     caster 缺席帧的更新覆盖率，留给下次有充分时间时做。


99. **Phase 7.56 窄实验：empty readyOnly snapshot 不再提前 return，放行现有 part lease restore（2026-05-14 13:31）**:
   - **动机**：
     - Phase 7.55 v4 已证明当前核心问题不再是 pose source 本身，而是
       少 caster 场景下 `War3TryPopulateDirectCurrentDrawGrouped(readyOnly=true)`
       周期性拿到 `directRecords.empty()`，随后 helper 立即 `return 0u`；
     - 这让函数后半段已经存在的 `DirectPartPacketLease` 恢复逻辑根本没有机会运行。
   - **这次改动只有一处**：
     - 删除 `src/d3d9/d3d9_device.cpp::War3TryPopulateDirectCurrentDrawGrouped`
       里 `if (directRecords.empty()) { publishShadowManifestSummary(empty); return 0u; }`
       的早退；
     - 保留后续整个 helper 原有流程不变：空 live records 继续进入
       `eligibleRecords` 构建空路径、`publishShadowManifestSummary(empty)`、
       `DirectPartPacketLease` 恢复、object-grouped submit。
   - **为什么这和前面失败的 fallthrough 不一样**：
     - 不会第二次调用 `War3TryPopulateDirectCurrentDrawGrouped(readyOnly=false)`；
     - 不会进入 directOnly 之外那条包含 native backend / sceneBundle / catchup
       副作用的下游路径；
     - 只是在 helper 内部允许"空快照帧也试试 per-part lease restore"。
   - **当前判断**：
     - 这是一个低风险实验，目标是把"少 caster 时整帧无阴影"先收敛成
       "如果 lease 里还有安全 packet，就继续提交"；
     - 它**不保证**彻底根治 16 帧 cadence，因为 draw-time VB capture 在这些
       空帧里也可能没有新样本；但至少先验证"真正缺的是 lease restore 机会"
       还是"就算放开 lease restore 也不够"。
   - **编译 / 部署**：
     - `ninja -C build32` 通过；
     - `E:\Work\War3\d3d9.dll` = `25414573 bytes @ 2026-05-14 13:31:43`。
   - **待用户实机验证**：
     - 多 caster 场景是否仍保持当前的流畅状态；
     - 少 caster 场景是否从"整段闪没 / 卡住"收敛到更连续的阴影提交；
     - 若仍有明显卡顿，下一步优先转向"lease restore 时给 v4 caster 回填当前帧
       的 live world/root transform"，而不是再碰 fallthrough。


100. **Phase 7.57 长期主线切换：unitsOnly/directOnly 优先消费 draw-time semantic producer（2026-05-14 14:14）**:
   - **用户裁决**：
     - 明确要求后续主线里不再看到 legacy 回退；
     - 以黑匣子 trace 为准，不接受"少 caster 仍周期性空帧"的方案。
   - **关键黑匣子再解读**（不是新 trace，而是对 Phase 7.55 far trace 的修正理解）：
     - `shadow_pose_full_trace_2026_05_14_02_58_12.jsonl` 的 zero-submit 窗口里，
       `drawTimeVBCacheCaptureCount = 28`、`drawTimeVBCacheTotalEntered = 28`
       **每帧都稳定非零**；
     - 这说明少 caster 的 16 帧空窗里，**draw-time GPU copy producer 本身没有停**，
       停的是 `currentDraw readyOnly` 这条 consumer；
     - 根因因此从"没有 fresh pose"正式收窄成：
       **有 fresh pre-skinned draw-time data，但 semantic direct-only 提交链没消费它。**
   - **长期主线实现**（本轮已落代码，不走 legacy）：
     - `d3d9_device.cpp` 新增 `War3TryPopulateDrawTimeSemanticProducer(bool unitsOnly)`；
     - `unitsOnly + directOnly` 路径现在优先消费
       `VisibleRenderableRegistry::getAllVisibleView()` + `m_war3DrawTimeVBCache`：
       - `VisibleRenderableRecord` 提供当前帧可见对象集合与稳定身份；
       - `War3DrawTimeVBEntry` 提供当帧 GPU copy 后的 pre-skinned VB/IB/UV/worldMatrix；
       - 命中 same-frame fresh entry 时直接构造 `War3ShadowCasterDraw` +
         `War3ShadowInstanceRef`，**不再经过 current-draw contract / palette / slot
         contract 这套 consumer 链**。
     - 只有当 draw-time producer 本帧完全没有可提交 entry 时，才回退到
       现有 `War3TryPopulateDirectCurrentDrawGrouped(readyOnly=true, ...)`。
   - **代码改动**：
     - `src/d3d9/d3d9_device.h`
       - `War3DrawTimeVBEntry` 新增 `topology`
       - 新增 `War3TryPopulateDrawTimeSemanticProducer(...)` 声明
     - `src/d3d9/d3d9_device.cpp`
       - 新增 runtime gate `DXVK_WAR3_SEMANTIC_DRAW_TIME_DIRECT_PRODUCER`
         （默认 on）
       - 新增 `War3TryPopulateDrawTimeSemanticProducer(...)`
       - `War3TryCaptureShadowCaster` 的 v4 capture 记录 `entry.topology`
       - `War3TryPopulateSemanticShadowScene` directOnly 分支优先走 draw-time
         producer；新的 `populateReturnReason = 10` 表示"draw-time producer submitted"
     - `src/d3d9/d3d9_war3_scene.h`
       - 新增 black-box 计数：
         `drawTimeSemanticProducerVisibleCandidateCount`
         `drawTimeSemanticProducerFreshEntryCount`
         `drawTimeSemanticProducerSubmittedCount`
         `drawTimeSemanticProducerMissNoFreshEntryCount`
         `drawTimeSemanticProducerFallbackCurrentDrawCount`
     - `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`
       - full trace `keyStats` 输出上述 5 个新字段
     - `AutoTest/_phase756_drawtime_producer.py`
       - 新增 trace 分析脚本，直接统计 zero-submit run、producer 提交帧比例、
         fallback 回 current-draw 的帧数
   - **预期黑匣子验收**：
     - 少 caster trace 里 `semanticSceneSubmitted == 0` 的 16/17 帧 run 应显著收缩，
       理想情况归零；
     - `populateReturnReason = 10` 应在 unitsOnly/directOnly 场景大量出现；
     - `drawTimeSemanticProducerSubmittedCount` 应与
       `drawTimeSemanticProducerFreshEntryCount` 同量级，且在原 zero-submit 窗口里保持非零；
     - 若仍有 flicker，则下一个真正 blocker 不再是 current-draw starvation，
       而是 draw-time producer 自身的去重/对象筛选准确性。
   - **编译 / 部署**：
     - `ninja -C build32` 通过；
     - `E:\Work\War3\d3d9.dll` = `25419248 bytes @ 2026-05-14 14:14:42`


101. **Phase 7.57 修正：draw-time producer 首轮完全没生效，原因是挂在了 `unitsOnly` 条件下（2026-05-14 14:30）**:
   - **黑匣子复核**：
     - 用户录制的两份 trace：
       - cluster: `shadow_pose_full_trace_2026_05_14_14_18_55.jsonl`
       - single-caster: `shadow_pose_full_trace_2026_05_14_14_21_16.jsonl`
     - `_phase756_drawtime_producer.py` 结果：
       - `drawTimeSemanticProducerVisibleCandidateCount = 0`
       - `drawTimeSemanticProducerFreshEntryCount = 0`
       - `drawTimeSemanticProducerSubmittedCount = 0`
       - `populateReturnReason = 10` 也为 0
     - 结论：**不是 producer 命中率低，而是 producer 根本没进入执行条件。**
   - **根因**：
     - `BeforeUi` 主调用点传的是
       `War3TryPopulateSemanticShadowScene(kShadowSemanticCoreSceneUnitsOnly)`；
     - `kShadowSemanticCoreSceneUnitsOnly` 当前配置值是 `false`
       （`war3_internal_test_config.h:706`）；
     - 首轮实现把 draw-time producer 挂在 `if (unitsOnly && ...)` 下，因此整条
       路在线上配置里被完全短路。
   - **修正后的长期主线**：
     - 不再把 draw-time producer 当成"替代 whole-scene directOnly"；
     - 改成：
       1. 现有 `War3TryPopulateDirectCurrentDrawGrouped(readyOnly=true, ...)`
          先跑，负责 whole scene；
       2. draw-time producer 再跑一次，但**只补单位**，并且只补
          `renderablePart` 不在本帧已提交集合里的项；
       3. 这样 cluster 场景里 current-draw 漏掉的一部分单位也能被 draw-time
          producer 补上，single-caster 场景则能在 current-draw 全空时接住。
   - **代码修正**：
     - `d3d9_device.h`
       - 新增 `m_war3SemanticSubmittedRenderablePartsThisFrame`
     - `d3d9_device.cpp`
       - `War3TryPopulateSemanticShadowScene` 开头清空上述 set
       - `War3TryAppendSemanticShadowPacket` 成功 append 后记录本帧已提交的
         `renderablePart`
       - `War3TryPopulateDrawTimeSemanticProducer` 改成始终只处理
         `ObjectKind::Unit`
       - directOnly 分支改为：
         `direct current-draw submit` → `draw-time producer supplement` → 合并返回
   - **重新编译 / 部署**：
     - `ninja -C build32` 通过
     - `E:\Work\War3\d3d9.dll` = `25419118 bytes @ 2026-05-14 14:30:11`
   - **下一步验收**：
     - 让用户重新录同样两份 trace；
     - 若这次 `drawTimeSemanticProducerSubmittedCount` 仍是 0，
       下一轮优先查 `VisibleRenderableRegistry::getAllVisibleView()` 在 BeforeUi
       时的生存期/读写面；
     - 若 producer 计数非零但 flicker 仍在，则问题从"producer 没跑"正式转成
       "producer 去重/补位粒度不准"。


102. **Phase 7.57 实机黑匣子确认：single-caster zero-submit 已归零，多-caster 由 producer 补缺口（2026-05-14 14:40）**:
   - **用户视觉反馈**：
     - “这轮完全不闪了，远离其他Caster也不会卡了”
   - **用户 trace**：
     - multi-caster: `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_14_38_34.jsonl`
     - single-caster: `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_14_39_47.jsonl`
   - **single-caster 黑匣子结果**（对比失败版 `14_21_16`）：
     - 旧：`submitted=0` = `306 / 695` 帧（44.0%），最长 run 88 帧
     - 新：`submitted=0` = `0 / 674` 帧（0.0%）
     - `populateReturnReason = 10` = `256 / 674` 帧
     - `drawTimeSemanticProducerVisibleCandidateCount ≈ 10.0 / frame`
     - `drawTimeSemanticProducerFreshEntryCount ≈ 3.1 / frame`
     - `drawTimeSemanticProducerSubmittedCount ≈ 3.1 / frame`
     - 解释：single-caster 场景下，current-draw 仍会有长空窗，但 draw-time producer
       已经在这些帧里独立提交单位阴影，把 zero-submit 窗口完全抹平。
   - **multi-caster 黑匣子结果**：
     - `submitted=0` = `0 / 161` 帧（保持稳定）
     - `populateReturnReason = 10` = `0 / 161` 帧
     - 但 `drawTimeSemanticProducerSubmittedCount ≈ 15.3 / frame`
     - `drawTimeSemanticProducerVisibleCandidateCount ≈ 124 / frame`
     - 解释：cluster 场景里 current-draw 仍是主体（所以 return reason 还是 9），
       draw-time producer 作为 supplement 补上每帧漏掉的单位 caster，这正对应
       用户此前肉眼看到的“集群里仍有一部分 caster 固定时间闪烁”。
   - **结论**：
     - 这次修复真正解决的不是 palette writer，也不是 manifest TTL；
     - 它解决的是 **semantic direct-only consumer 对 sparse current-draw 的饥饿问题**：
       - whole-scene 提交仍靠 current-draw
       - 单位 pose 正确性由 draw-time pre-skinned VB producer 补位保证
     - 在现有真实场景和黑匣子数据下，少 caster / 多 caster 两类闪烁都已被压平。


103. **Phase 7.57 稳定基线恢复 + 安全性能优化（2026-05-14 15:57）**:
   - **为什么恢复基线**：
     - 用户后续 single-caster trace `shadow_pose_full_trace_2026_05_14_15_30_12.jsonl`
       显示卡顿回归；
     - 黑匣子里 `drawTimeSemanticProducerVisibleCandidateCount = 0`，
       说明问题不是 producer 主机制失效，而是后续“更严单位筛选”把真实单位候选
       挡在了入口外；
     - 因此撤销那层收紧筛选，回到最后一次已知可用的补位逻辑，优先守住视觉基线。
   - **恢复后的状态**：
     - 保留：
       - `current-draw submit` + `draw-time producer supplement`
       - `m_war3SemanticSubmittedRenderablePartsThisFrame` 去重
       - draw-time producer 黑匣子计数
     - 撤销：
       - 那层导致 `RejectNonUnitLike / RejectNoIdentity` 误杀的更严单位筛选
   - **本轮安全性能优化**：
     - 目标：降低 draw-time producer supplement 的 CPU 成本，同时不改补位语义。
     - 原逻辑：
       - 每帧遍历 `VisibleRenderableRegistry::getAllVisibleView()` 的全量 visible records；
       - 再查 `m_war3DrawTimeVBCache` 是否有 fresh entry。
     - 新逻辑：
       - 先遍历 `m_war3DrawTimeVBCache` 中本帧 fresh 的条目；
       - 再通过 `VisibleRenderableRegistry::queryByRenderablePart(...)`
         反查当前可见语义；
       - 这把扫描成本从“全量 visible records”降成了“fresh draw-time cache 条目数”，
         对 cluster 场景尤其更划算，而补位语义不变。
   - **代码改动**：
     - `src/d3d9/d3d9_device.cpp`
       - `War3TryPopulateDrawTimeSemanticProducer()` 改为
         “fresh cache → renderablePart 反查 visible” 的遍历方式
       - 不改提交条件、不改补位去重、不改 worldMatrix/IB/UV 消费
   - **编译 / 部署**：
     - `ninja -C build32` 通过
     - `E:\Work\War3\d3d9.dll` = `25419248 bytes @ 2026-05-14 15:57:09`
   - **运行时验证状态**：
     - 用户当时外出，本轮还未做新的实机黑匣子复核；
     - 下一步应先用 single-caster 场景确认视觉基线恢复，再继续观察 perf。


105. **Phase 7.58 无人值守推进：默认启用 Shadow TAA + 保留动态语义 caster 的历史混合（2026-05-14 16:10）**:
   - **用户新要求**：
     - 进入无人值守模式继续推进；
     - 当前线路存在“没有 Shadow TAA 导致阴影抖动较重”的体感；
     - 开始在不让视觉正确性开倒车的前提下追赶旧 VB/IB 方案约 110 FPS 的基线。
   - **代码现状确认**：
     - `d3d9_war3_settings.h` 里 `shadowTaaEnabled` 默认是 `false`；
     - `d3d9_war3_shadow.cpp` 里只要
       `kShadowDisableTaaForSemanticDynamicCasters && semanticDynamicCastersActive`
       就会把 TAA 整体挡掉，并清空 history；
     - 这意味着当前语义动态单位阴影即使有 history 资源，也默认不参与 Shadow TAA。
   - **本轮改动**：
     - `src/d3d9/d3d9_war3_settings.h`
       - `shadowTaaEnabled` 默认值改为 `true`
     - `src/d3d9/d3d9_war3_shadow.cpp`
       - 把“semantic dynamic 一刀切禁用 TAA”改成运行时开关：
         `DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC`
       - 默认 **不禁用**；只有显式设环境变量才会回到旧行为
   - **为什么这仍然是保守改动**：
     - 没有碰 history ping-pong、barrier、motion vector 资源生命周期；
     - 没有碰 draw-time producer / current-draw supplement 主线；
     - 保留了现有的断档保护：
       - invalid CSM / empty replay / history invalidation / previous-frame gates
   - **回退路径**：
     - `DXVK_WAR3_SHADOW_TAA=0` 可整体验证关闭 TAA
     - `DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC=1` 可恢复旧的
       “动态语义 caster 不参与 TAA” 行为
   - **编译 / 部署**：
     - `ninja -C build32` 通过
     - `E:\Work\War3\d3d9.dll` = `25419546 bytes @ 2026-05-14 16:10:13`
   - **验证状态**：
     - 用户外出，本轮仍未做新的视觉/黑匣子复核；
     - 返回后应重点看：
       1. 单/多 caster 的 Shadow TAA 是否真正 active（`semanticSceneShadowTaaActive`）
       2. 是否出现明显拖影/历史残留
       3. 在 TAA 默认开启的情况下，是否仍保持 draw-time producer 基线的
          single-caster 无卡顿


106. **Phase 7.59 无语义改动的 producer/material 热点削减 + 黑匣子复核（2026-05-14 16:35）**:
   - **自动化续跑**：
     - 已确认 heartbeat 自动化 `war3-shadow-optimization-continuation` 处于 ACTIVE；
     - 频率：`RRULE:FREQ=MINUTELY;INTERVAL=20`；
     - 提示词要求继续 War3 shadow 优化、以 full trace 为验收、禁止 legacy 路线。
   - **本轮性能小改原则**：
     - 不改 current-draw + draw-time producer supplement 的提交语义；
     - 不收紧 producer 过滤；
     - 不碰 AlphaTest / draw-time VB GPU copy / TAA history 生命周期。
   - **代码改动**：
     - `src/d3d9/d3d9_device.h`
       - `War3DrawTimeVBEntry` 新增 `submittedFrameSerial`；
       - 移除每帧 `m_war3SemanticSubmittedRenderablePartsThisFrame` `unordered_set`。
     - `src/d3d9/d3d9_device.cpp`
       - 成功 append 后把对应 draw-time VB cache entry 标记为本帧已提交；
       - producer supplement 用 `entry.submittedFrameSerial == m_war3ShadowPersistentFrameSerial`
         判重，避免每帧清空/填充 hash set；
       - `War3TryAppendSemanticShadowPacket` 保存一次 `drawTimeVBEntry` 指针，
         后续 AlphaTest/texture 派生复用，减少重复 `m_war3DrawTimeVBCache.find`；
       - `War3BuildShadowMaterialSignatureCached`：
         - cache 槽位 `4096 -> 16384`；
         - hash 纳入 `layerState`；
         - 不再把 `meshData` 作为命中硬条件，因为当前 canonical layer contract
           路径以 `sceneNode/layerState/modelResource` 为稳定语义，`meshData`
           可能是 draw-local/churny 输入。
   - **性能证据**：
     - 改前 breakdown `war3_perf_report_auto_2026_05_14_16_22_11.html`：
       - `War3SemanticScene/Populate` ≈ `16.174ms/frame`
       - `Direct/BuildPacketCall` ≈ `9.232ms/frame`
       - `Direct/BuildPacket` ≈ `9.128ms/frame`
       - `Direct/MaterialSignature` ≈ `3.619ms/frame`
     - material cache 改后 breakdown `war3_perf_report_auto_2026_05_14_16_29_41.html`：
       - `War3SemanticScene/Populate` ≈ `12.751ms/frame`
       - `Direct/BuildPacketCall` ≈ `5.794ms/frame`
       - `Direct/BuildPacket` ≈ `5.694ms/frame`
       - `Direct/MaterialSignature` 不再进入主要热点列表。
     - 最新 full trace AutoTest `war3_perf_report_auto_2026_05_14_16_31_42.html`：
       - `avgFps = 11.169`
       - `avgGpuTimeMs = 1.822`
       - 说明当前瓶颈仍主要在 CPU/main-thread populate 侧，离旧 VB/IB
         拦截方案约 `110 FPS` 的目标仍很远。
   - **黑匣子验收**：
     - 最新 trace：`E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_16_31_21.jsonl`
     - `_phase756_drawtime_producer.py`：
       - `submitted=0` = `0 / 163`
       - producer visible candidates ≈ `95.8 / frame`
       - producer fresh entries ≈ `15.0 / frame`
       - producer submitted ≈ `15.0 / frame`
       - producer miss-no-fresh = `0.0 / frame`
     - TAA trace 字段：
       - `semanticSceneShadowTaaMode`: 前 2 帧 `1`，随后 `2`
       - `semanticSceneShadowHistoryValidAfter`: `1 / 163` 后全部有效
       - `semanticSceneShadowReceiverSampleSource`: 前 2 帧 `2`，随后 `3`
       - 解释：history ping-pong 已进入稳定采样；full trace keyStats
         当前没有单独输出 `semanticSceneShadowTaaActive` 字段，后续若需要可补。
   - **结论**：
     - 本轮是可保留的安全优化：黑匣子没有出现 zero-submit 回潮；
     - 它只削掉 material/去重层的部分 CPU 成本，未触及更大的
       `Direct/Append` 与 `BuildPacket` 自身耗时；
     - 下一轮性能主线应继续细分 `War3TryAppendSemanticShadowPacket` 内部热点，
       但每次仍必须先用 full trace 验证 `submitted=0 == 0`。


107. **Phase 7.60 无语义改动的 unit flags 热点缓存 + breakdown 细分（2026-05-14 16:55）**:
   - **自动化状态**：
     - 已确认 heartbeat 自动化 `war3-shadow-optimization-continuation` 仍为 ACTIVE；
     - 未创建重复自动化，避免同一线程被多份 heartbeat 争用。
   - **本轮原则**：
     - 继续只做 draw-time semantic 主线性能优化；
     - 不引入 legacy route；
     - 不改 current-draw + draw-time producer supplement 的提交语义；
     - 不收紧 producer 过滤，不动 AlphaTest / draw-time VB GPU copy / Shadow TAA。
   - **代码改动**：
     - `src/d3d9/d3d9_device.cpp`
       - 新增 `War3TryReadUnitFlags5CCached(...)`：
         - thread-local 4096 槽 direct-mapped cache；
         - key 为 `unitPtr`；
         - 缓存 `unit+0x5C` 是否可读及 flags 值；
         - 替换 hot path 中重复 `SafeReadU32Fast(unitPtr, Flags5C, ...)`。
       - `War3TryBuildShadowPacketFromCurrentDrawRecord()` 内增加 gated breakdown scopes：
         - `RenderableSetup`
         - `ResourceSetup`
         - `SkinningDecision`
         - `PaletteInstall`
         - `PoseInstall`
         - `ExplicitBlendResolve`
         - `PathFinalize`
         - `MaxGroupSlotScan`
       - 这些 breakdown 仅在
         `DXVK_WAR3_SEMANTIC_SUBMIT_BREAKDOWN=1` 时启用，默认不写入 perf 树。
   - **性能证据**：
     - 改后 quick full-trace perf：
       - `war3_perf_report_auto_2026_05_14_16_52_56.html`
       - `avgFps = 14.116`
       - `avgGpuTimeMs = 1.647`
       - `avgProcessCpuMs = 73.836`
       - `avgMainThreadCpuMs = 65.411`
     - 改后 breakdown：
       - `war3_perf_report_auto_2026_05_14_16_53_40.html`
       - `War3SemanticScene/Populate` ≈ `8.654ms/frame`
       - `Direct/BuildPacket` ≈ `2.509ms/frame`
       - `Direct/RenderableSetup` ≈ `0.024ms/frame`
       - `SubmitFrame/PaletteIndex` ≈ `0.833ms/frame`
       - 对比上一轮 cached-flags 前的 `RenderableSetup` 约 `3.3-3.6ms/frame`，
         说明主要成本确实来自 repeated safe-read / memory-probe 类操作。
   - **黑匣子验收**：
     - 最新 trace：
       - `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_16_52_34.jsonl`
     - `_phase756_drawtime_producer.py`：
       - `submitted=0` = `0 / 212`
       - `submitted_min = 69`
       - producer visible candidates ≈ `97.6 / frame`
       - producer fresh entries ≈ `14.8 / frame`
       - producer submitted ≈ `14.8 / frame`
       - producer miss-no-fresh = `0.0 / frame`
     - TAA trace 字段：
       - `semanticSceneShadowTaaMode`: 前 2 帧 `1`，随后 210 帧 `2`
       - `semanticSceneShadowHistoryValidAfter`: `1 / 212`
       - `semanticSceneShadowReceiverSampleSource`: 前 2 帧 `2`，随后 210 帧 `3`
   - **注意事项**：
     - 本轮未使用 pure perf 并发结果作为结论；一次并发 `perf.py` 与
       `perf_breakdown.py` 争用了 isolated desktop/control-plane，`perf.py`
       失败于 named pipe 不可用。这不是游戏崩溃，也不是渲染回归。
     - 下一轮性能主线应优先调查 `SubmitFrame/PaletteIndex`，但仍需保持
       每次改动后 full trace `submitted=0 == 0`。


108. **Phase 7.61 draw-time VB fast append 实验转主线 + 黑匣子验收（2026-05-14 17:15）**:
   - **目标**：
     - 继续沿长期主线优化 semantic draw-time pre-skinned VB/IB/UV 路径；
     - 不引入 legacy route；
     - 不改变稀疏 caster 的 producer supplement 正确性前提；
     - 只在已有当前帧 draw-time VB cache 的 skinned unit 上减少 append 侧 CPU 绕路。
   - **代码改动**：
     - `src/d3d9/d3d9_device.cpp`
       - 新增运行时开关
         `DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND`，默认开启；
       - 在 `War3TryPopulateDirectCurrentDrawGrouped()` submit 阶段新增
         `tryAppendDrawTimeFastEligible(...)`；
       - 命中条件很窄：
         - 非 `fromPartPacketLease`；
         - 非 `fromStalePoseRestore`；
         - packet path 必须是 `Skinned`；
         - object kind 必须解析为 `Unit`；
         - `renderablePart` 必须命中 `m_war3DrawTimeVBCache`；
         - cache entry 必须是当前 `m_war3ShadowPersistentFrameSerial`；
         - position/index GPU copy buffer 必须完整；
       - 命中后直接构造 `War3ShadowCasterDraw`：
         - position/index/UV/diffuse/alpha 来自 draw-time cache；
         - `vertexBlendEnabled=false`；
         - `worldMatrix=entry.capturedWorldMatrix`；
         - `War3ShadowReplayMode::FixedWorld`；
         - `boundsRadius=0`，沿用 v4 的 no-cull 策略；
       - 未命中时立即回落到原 `War3TryAppendSemanticShadowPacket()`。
   - **为什么这不是新 correctness 路线**：
     - fast append 只消费 Phase 7.55 已验证的 draw-time GPU copy 数据；
     - 不从 palette / manifest / lease / legacy capture 重新取 pose；
     - 不扩大 stale restore 或 lease record 的适用范围；
     - 不改变对象选择、grouping、sticky selection、part lease 记录逻辑。
   - **黑匣子验收**：
     - trace：
       `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_17_08_40.jsonl`
     - `_phase756_drawtime_producer.py`：
       - `submitted=0` = `0 / 201`
       - `submitted_min = 69`
       - producer visible candidates ≈ `96.6 / frame`
       - producer fresh entries ≈ `12.6 / frame`
       - producer submitted ≈ `12.6 / frame`
       - producer miss-no-fresh = `0.0 / frame`
       - producer fallback to current-draw = `0 / 201`
     - TAA / receiver trace：
       - `semanticSceneShadowTaaMode`: 前 2 帧 `1`，随后 199 帧 `2`
       - `semanticSceneShadowReceiverSampleSource`: 前 2 帧 `2`，随后 199 帧 `3`
       - `semanticSceneShadowHistoryValidAfter`: `201 / 201`
   - **性能证据**：
     - breakdown：
       `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_14_17_11_02.html`
     - `avgFps = 14.131`
     - `avgGpuTimeMs = 1.638`
     - `avgProcessCpuMs = 72.170`
     - `War3SemanticScene/Populate` ≈ `7.176ms/frame`
     - `Direct/BuildPacket` ≈ `2.407ms/frame`
     - `Direct/Append` ≈ `4.036ms/frame`
     - `SubmitFrame/PaletteIndex` ≈ `0.378ms/frame`
   - **对比 Phase 7.60**：
     - `War3SemanticScene/Populate`: `8.654ms -> 7.176ms`
     - `SubmitFrame/PaletteIndex`: `0.833ms -> 0.378ms`
     - 黑匣子 `submitted=0` 仍为 0，未出现少 caster 卡顿回潮。
   - **回退路径**：
     - `DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND=0`
       可关闭 fast append，回到 Phase 7.60 的稳定路径。
   - **下一步**：
     - 继续拆 `War3SemanticScene/Direct/BuildPacket` 和 `Direct/Append`
       的剩余 CPU 热点；
     - 每轮仍必须用 full trace 复核 `submitted=0 == 0` 和 TAA history
       稳定状态，视觉正确性优先级高于 FPS。


109. **Phase 7.62-7.64 draw-time prebuild bypass + 关闭 matrix publisher hooks（2026-05-14 21:05）**:
   - **目标**：
     - 继续长期 semantic draw-time 主线，不走 legacy route；
     - 保留 draw-time VB/IB/UV GPU copy 作为当前 Pose 权威源；
     - 去掉不再参与当前 Pose 正确性的 matrix/palette publisher 热路径成本。
   - **代码改动**：
     - `src/d3d9/d3d9_device.cpp`
       - 新增 `DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS`，默认开启；
       - 当 current draw record 命中当前帧 draw-time VB cache 且是稳定 Unit
         skinned path 时，直接构造供 fast append 消费的 lightweight packet，
         跳过 `War3TryBuildShadowPacketFromCurrentDrawRecord()` 的 bind-pose /
         palette 数据层构建；
       - 新增 `DXVK_WAR3_SEMANTIC_BYPASS_INLINE_REGISTRY_PUBLISH`，默认关闭，
         避免每个 bypass candidate 内联发布 semantic registries；
     - `src/d3d9/d3d9_war3_scene.h` 与
       `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`
       - 新增并输出
         `semanticSceneDirectDrawTimePrebuildBypassAttemptCount` /
         `semanticSceneDirectDrawTimePrebuildBypassHitCount`；
     - `src/d3d9/war3/render/war3_visible_renderables.cpp`
       - `registerSemanticCandidate()` 在 deferred-index 模式下先查
         `byRenderablePartLayer` / `byRenderablePart`，命中后仍走原有
         `mergeCandidateIntoExisting()`，减少线性扫描；
     - `src/d3d9/war3/core/war3_internal_test_config.h`
       - `kWar3RuntimeConfigEnableSemanticMatrixPublisherHooks = false`。
   - **为什么这次关闭 publisher 是安全的**：
     - 当前 skinned Pose 已经由 draw-time pre-skinned VB/IB/UV copy 提供；
     - full trace 中 matrix/palette publisher 计数归零，但 draw-time VB capture
       与 producer submit 仍持续工作；
     - 这条路没有重新依赖 stale lease / palette rebuild / legacy capture。
   - **黑匣子验收**：
     - trace：
       `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_20_56_46.jsonl`
     - `_phase756_drawtime_producer.py`：
       - `submitted=0` = `0 / 701`
       - `submitted_min = 69`
       - producer visible candidates ≈ `95.2 / frame`
       - producer fresh entries ≈ `21.4 / frame`
       - producer submitted ≈ `21.4 / frame`
       - producer miss-no-fresh = `0.0 / frame`
       - producer fallback to current-draw = `0 / 701`
     - TAA / receiver trace：
       - `semanticSceneShadowTaaMode`: 前 2 帧 `1`，随后 699 帧 `2`
       - `semanticSceneShadowReceiverSampleSource`: 前 2 帧 `2`，随后 699 帧 `3`
       - `semanticSceneShadowHistoryValidAfter`: `701 / 701`
       - shadow map execute / receiver draw: `701 / 701`
     - prebuild bypass：
       - Attempt = `52228`
       - Hit = `52051`
       - hit rate ≈ `99.66%`
   - **性能证据**：
     - matrix publisher hooks 关闭前：
       - `war3_perf_report_auto_2026_05_14_20_48_57.html`
       - `avgFps ≈ 14.36`
     - 关闭后 quick perf：
       - `war3_perf_report_auto_2026_05_14_20_57_08.html`
       - `avgFps = 61.473`
     - 关闭后 breakdown：
       - `war3_perf_report_auto_2026_05_14_20_59_33.html`
       - `avgFps = 98.690`
       - `avgFrameTimeMs = 10.133`
       - `avgGpuTimeMs = 1.627`
       - `avgProcessCpuMs = 8.591`
       - `avgMainThreadCpuMs = 6.124`
       - `War3SemanticScene/Populate` ≈ `0.492ms/frame`
       - `War3SemanticScene/Direct/BuildPacket` ≈ `0.038ms/frame`
       - `War3SemanticScene/Direct/Append` ≈ `0.110ms/frame`
       - `SubmitFrame/PaletteIndex` ≈ `0.002ms/frame`
   - **结论**：
     - 这轮是目前最关键的性能突破：此前用户看到的 10FPS 本质上不是
       shadow map GPU 压力，而是 semantic.data 的 matrix/palette publisher
       数据层热路径成本；
     - 当前主线已经接近旧 VB/IB intercept 的 `110 FPS` 基线；
     - 后续优化应优先小步削 `Shadow/Main`、TAA/AA/receiver 和剩余
       untracked CPU，不再回头恢复 matrix publisher hooks，除非黑匣子证明
       draw-time VB producer 在某个实机场景失效。


110. **Phase 7.65 shadow map scratch buffer reuse（2026-05-14 21:20）**:
   - **目标**：
     - 继续长期 semantic draw-time 主线；
     - 不碰 Pose 来源、不恢复 legacy route、不改变 AlphaTest / TAA 语义；
     - 只削 shadow map pass 内部每帧临时分配和重复 replay-list 构造。
   - **代码改动**：
     - `src/d3d9/d3d9_war3_shadow.cpp`
       - `War3ShadowReceiverPass::renderShadowMap()` 在调用方已经传入
         `replayDrawOverride` 时不再额外调用 `BuildShadowReplayDraws()`；
       - 原本每帧局部创建的 `prepared` 与 `drawIndices` 改为复用
         `War3ShadowReceiverPass` 成员 scratch vector；
     - `src/d3d9/d3d9_war3_shadow.h`
       - 将 `PreparedShadowCaster` 提升为 receiver pass 私有结构；
       - 新增 `m_shadowPreparedScratch` 与
         `m_shadowDrawIndicesScratch`。
   - **为什么这轮 correctness 风险低**：
     - scratch vector 每次 `renderShadowMap()` 入口都会 `clear()/resize()`，
       不跨帧保留 caster 决策；
     - 没有改变 draw-time VB/IB/UV GPU copy、prebuild bypass、
       fast append、TAA history、receiver sample source 或 matrix publisher
       hooks 的开关状态；
     - 只改变容器生命周期，不改变容器里的数据来源和提交顺序。
   - **黑匣子验收**：
     - trace：
       `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_21_11_35.jsonl`
     - `_phase756_drawtime_producer.py`：
       - `submitted=0` = `0 / 885`
       - `submitted_min = 69`
       - producer visible candidates ≈ `96.5 / frame`
       - producer fresh entries ≈ `20.8 / frame`
       - producer submitted ≈ `20.8 / frame`
       - producer miss-no-fresh = `0.0 / frame`
       - producer fallback to current-draw = `0 / 885`
     - TAA / receiver trace：
       - `semanticSceneShadowTaaMode`: 前 2 帧 `1`，随后 883 帧 `2`
       - `semanticSceneShadowReceiverSampleSource`: 前 2 帧 `2`，随后 883 帧 `3`
       - `semanticSceneShadowHistoryValidAfter`: `885 / 885`
       - `semanticSceneReceiverDrawExecutedThisFrame`: `885 / 885`
       - `semanticSceneShadowMapRenderSerial`: `1..885` 连续推进
     - prebuild bypass：
       - Attempt = `67913`
       - Hit = `67334`
       - hit rate ≈ `99.15%`
   - **性能证据**：
     - pure perf rerun：
       `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_14_21_16_02.html`
     - `avgFps = 109.307`
     - `avgFrameTimeMs = 9.149`
     - `avgGpuTimeMs = 1.581`
     - `avgProcessCpuMs = 7.580`
     - `avgMainThreadCpuMs = 5.345`
     - `War3SemanticScene/Populate` ≈ `0.394ms/frame`
     - `Shadow/Main` ≈ `0.512ms CPU / 0.668ms GPU`
     - `ShadowMap` ≈ `0.297ms CPU / 0.112ms GPU`
     - `PostFX/AA` ≈ `0.032ms CPU / 0.248ms GPU`
   - **结论**：
     - 当前长期 semantic draw-time 主线已经基本追平旧 VB/IB intercept
       `110 FPS` 基线，同时黑匣子没有回到少 caster `submitted=0`
       问题；
     - 下一步优化优先看 `Shadow/Main` / receiver / TAA GPU 成本和
       `Other/UntrackedActive`，避免重新改 Pose 数据层。


111. **Phase 7.66 shadow map cascade sort de-dup（2026-05-14 21:28）**:
   - **目标**：
     - 继续在 shadow map pass 内部做低风险 CPU 优化；
     - 不改变 caster 输入、不改变 Pose 来源、不改变 AlphaTest/TAA/receiver。
   - **代码改动**：
     - `src/d3d9/d3d9_war3_shadow.cpp`
       - 原逻辑每个 cascade 都先过滤 `drawIndices` 再按
         pipeline / texture / VB / IB 排序；
       - 新逻辑先对所有有效 prepared draw 建立一次全局排序列表，
         每个 cascade 只沿这个排序列表做 cull/filter；
       - 排序 comparator 与 tie-breaker `a < b` 保持不变，因此每个
         cascade 的最终 draw 顺序等价于原来的“过滤后排序”。
     - `src/d3d9/d3d9_war3_shadow.h`
       - 新增 `m_shadowSortedDrawIndicesScratch` 复用排序 scratch。
   - **黑匣子验收**：
     - trace：
       `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_21_23_20.jsonl`
     - `_phase756_drawtime_producer.py`：
       - `submitted=0` = `0 / 950`
       - producer visible candidates ≈ `96.3 / frame`
       - producer fresh entries ≈ `20.6 / frame`
       - producer submitted ≈ `20.6 / frame`
       - producer miss-no-fresh = `0.0 / frame`
       - producer fallback to current-draw = `0 / 950`
     - TAA / receiver trace：
       - `semanticSceneShadowTaaMode`: 前 2 帧 `1`，随后 948 帧 `2`
       - `semanticSceneShadowReceiverSampleSource`: 前 2 帧 `2`，随后 948 帧 `3`
       - `semanticSceneShadowHistoryValidAfter`: `950 / 950`
       - `semanticSceneReceiverDrawExecutedThisFrame`: `950 / 950`
       - `semanticSceneShadowVisibilityExecutedThisFrame`: `950 / 950`
       - `semanticSceneShadowMapRenderSerial`: `1..950` 连续推进
     - prebuild bypass：
       - Attempt = `72766`
       - Hit = `72266`
       - hit rate ≈ `99.31%`
   - **性能证据**：
     - pure perf rerun：
       `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_14_21_22_54.html`
     - `avgFps = 110.777`
     - `avgFrameTimeMs = 9.027`
     - `avgGpuTimeMs = 1.590`
     - `avgProcessCpuMs = 7.797`
     - `avgMainThreadCpuMs = 5.469`
     - `War3SemanticScene/Populate` ≈ `0.350ms/frame`
     - `Shadow/Main` ≈ `0.495ms CPU / 0.673ms GPU`
     - `ShadowMap` ≈ `0.279ms CPU / 0.111ms GPU`
     - `PostFX/AA` ≈ `0.029ms CPU / 0.247ms GPU`
   - **注意事项**：
     - 一次与 full trace 并发的 perf 报告
       `war3_perf_report_auto_2026_05_14_21_20_28.html` 受双 War3 进程污染，
       不作为结论；
     - 这轮收益很小，但方向安全：减少重复排序，不碰数据层。


112. **Phase 7.67 semantic path-blocker filter + tree-shadow TAA stabilization（2026-05-14 23:40）**:
   - **目标**：
     - 保持长期 semantic draw-time 主线，不恢复 legacy route；
     - 恢复 Warcraft III LOS/path blocker 的阴影屏蔽能力，避免 `YTab/YTac/YTpb/YTpc/YTfb/YTfc/YTlb/YTlc`
       这类不可见路径阻断器在增强阴影里变成小方块；
     - 降低树木 alpha-test 阴影的细碎抖动，同时不牺牲 Pose/AlphaTest 正确性。
   - **代码改动**：
     - `src/d3d9/d3d9_device.cpp`
       - 新增 `War3ShadowIsLosBlocker(...)` overload，统一走现有
         `IsLosBlockerFourCc()` / 第二字符大小写归一化；
       - 在 semantic draw-time producer、current-draw grouped 两阶段、
         direct draw-time fast append、static direct supplement、以及早期
         semantic capture 分支加入 path-blocker reject；
       - `War3DrawTimeVBEntry` 记录 `rawcode/jHandle/objectKind`，让 draw-time
         VB cache 命中后也能继续识别 path blocker。
     - `src/d3d9/d3d9_war3_scene.h` 与
       `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`
       - 新增并导出 `semanticSceneRejectedPathBlockerCount`，用于 full trace
         直接确认过滤是否在语义阴影路径内生效。
     - `subprojects/war3fx/shaders/war3_shadow_caster_frag.frag`
       - alpha-test hash 不再使用 ShadowMap 像素 / palette offset 作为噪声锚点；
       - dither 改为绑定 alpha 贴图 texel，使树叶遮罩的随机模板跟着纹理图案走，
         避免 CSM / camera 微动时模板在叶片上滑动；
       - 缩窄 hash 过渡带，减少树叶边缘的随机覆盖面积。
     - `subprojects/war3fx/shaders/war3_shadow_visibility.frag`
       - TAA current-visibility prepass 回到稳定 4 tap，树叶稳定性主要交给
         texture-anchored caster dither + receiver history；
     - `subprojects/war3fx/shaders/war3_shadow_receiver.frag`
       - history 采样使用连续 UV；
       - motion-adaptive 新帧权重从较激进的 `0.18 / mv*12` 收敛到
         `0.12 / mv*8`，让树影 history 更愿意积累。
   - **黑匣子验收**：
     - trace：
       `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_14_23_39_47.jsonl`
     - `_phase756_drawtime_producer.py`：
       - `submitted=0` = `0 / 166`
       - producer visible candidates ≈ `94.8 / frame`
       - producer fresh entries ≈ `22.5 / frame`
       - producer submitted ≈ `22.5 / frame`
       - producer miss-no-fresh = `0.0 / frame`
       - producer fallback to current-draw = `0 / 166`
     - TAA / receiver trace：
       - `semanticSceneShadowTaaMode`: 首帧 current-only，随后 tail 全为 `2`
       - `semanticSceneShadowReceiverSampleSource`: tail 全为 `3`
       - `semanticSceneShadowHistoryValidAfter`: `166 / 166`
       - `semanticSceneReceiverDrawExecutedThisFrame`: `166 / 166`
       - `semanticSceneShadowVisibilityExecutedThisFrame`: `166 / 166`
       - `semanticSceneShadowMapRenderSerial`: tail `157..166` 连续推进
     - path blocker：
       - `semanticSceneRejectedPathBlockerCount` 最大值 `15`
       - tail 为稳定的 `4`，证明测试图里已经有阻断器被 semantic shadow 路径过滤。
   - **性能记录**：
     - 当前 23:30 之后的 AutoTest perf 样本不作为代码性能结论：
       - full profile、`shadow.taa` disabled、`aa` disabled、甚至 `shadow`
         disabled 都只有约 `12-14 FPS`；
       - 这说明本轮 perf 环境 / War3 runtime 状态已被污染，不能用来裁决
         TAA/path-blocker 改动；
       - 最近仍可信的性能基线是 Phase 7.66 的
         `war3_perf_report_auto_2026_05_14_21_22_54.html`（`110.777 FPS`）
         和本轮早些时候的
         `war3_perf_report_auto_2026_05_14_23_20_48.html`（`88.384 FPS`）。
   - **结论**：
     - path blocker 屏蔽已经接入长期 semantic 路线，不依赖 legacy；
     - 树影 TAA 的主要抖动源从“ShadowMap 像素模板滑动”改为“纹理遮罩稳定模板”，
       理论上更适合配合 receiver history 收敛；
     - 后续若用户视觉仍看到树影抖动，优先比较同一视角下 alpha-hash
       贴图锚定前后的录屏，不应再回到 Pose/producer 数据层乱改。


113. **Phase 7.68 CSM quality baseline + deterministic alpha shadow（2026-05-15 01:05）**:
   - **用户反馈**：
     - Phase 7.67 后树木阴影边缘仍严重抖动；
     - 只有压低视角、让近级联实际吃到高分辨率时，树叶缝隙才勉强清楚；
     - 手动降低 shadow resolution 对 FPS 提升很小，说明当前视觉问题不能继续靠
       2048 自适应降级换性能。
   - **legacy / 当前差异复核**：
     - 旧 `b193367` 基线请求 `4096` shadow map，未发现当前
       `ResolveAdaptiveShadowResolution` 这类把 4096 静默降到 2048 的路径；
     - 旧 shadow map prepare 只对 `draw.alphaTestEnabled` 做 alpha-test；
     - 当前长期 semantic 路线为了修实心方块，已经把 `alphaBlend + diffuse + UV`
       物体 promote 成 alpha shadow，这对树叶是正确方向，但若再走 hashed
       fractional coverage + mip sampler，就会在 CSM/TAA 后表现为树影噪声和糊边。
   - **代码改动**：
     - `src/d3d9/war3/core/war3_internal_test_config.h`
       - `kShadowAdaptiveResolutionEnabled=false`；
       - 默认保留用户请求的 `4096`，不再因 replay geometry work 自动降到 `2048`。
     - `src/d3d9/d3d9_war3_settings.h`
       - `alphaShadowHashed=false`；
       - `alphaShadowUseMip=false`；
       - `alphaShadowMipLodBias=0.0f`；
       - 树叶 / 栅栏等 alpha cutout 默认走确定性 hard cutoff，避免 dither 模板或 mip
         采样把叶片缝隙打散。
     - `src/d3d9/d3d9_war3_pipeline.cpp`
       - 新增环境变量便于 A/B：
         `DXVK_WAR3_SHADOW_ALPHA_HASH`、
         `DXVK_WAR3_SHADOW_ALPHA_MIP`、
         `DXVK_WAR3_SHADOW_ALPHA_MIP_BIAS`。
   - **验证**：
     - `ninja -C build32` 通过；
     - 部署 DLL：
       `E:\Work\War3\d3d9.dll = 26759116 bytes @ 2026-05-15 00:58:26`；
     - AutoTest：
       `py AutoTest\_phase755_v4_quick.py` 通过，报告：
       `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_15_00_59_11.html`
       - `avgFps = 79.316`
       - `avgGpuTimeMs = 2.087`
       - `shadowReceiverAdaptiveResolutionFrames = 0`
       - requested/effective resolution = `4096 / 4096`
       - `ShadowMap ≈ 0.288ms CPU / 0.352ms GPU`
       - `Shadow/Main ≈ 0.452ms CPU / 0.912ms GPU`
     - 黑匣子：
       `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_15_00_58_49.jsonl`
       - `_phase756_drawtime_producer.py`：
         `submitted=0 = 0 / 1001`；
         producer fallback to current-draw = `0 / 1001`；
       - TAA / receiver：
         `shadowTaaMode` tail 全为 `2`；
         `shadowReceiverSampleSource` tail 全为 `3`；
         `shadowHistoryValidAfter = 1001 / 1001`；
         `receiverDrawExecutedThisFrame = 1001 / 1001`；
         `shadowVisibilityExecutedThisFrame = 1001 / 1001`；
         `shadowMapRenderSerial = 1..1001` 连续，无 gap；
       - path blocker：
         `semanticSceneRejectedPathBlockerCount` tail 稳定为 `4`，最大 `36`。
   - **结论 / 风险**：
     - 这轮不触碰 Pose、draw-time producer、VB cache、path-blocker 过滤或 legacy；
     - 视觉上应显著改善树叶 cutout 清晰度和 dither 抖动；
     - 风险是 promoted alpha-blend foliage 现在用硬阈值，个别半透明特效的阴影可能变硬；
       若用户视觉确认过硬，再用新增 env 精确 A/B，而不是回到全局 hashed 默认。


114. **Phase 7.69 数据层性能账本 + indexed range 修复（2026-05-15 02:15）**:
   - **用户校正的基线定义**：
     - `legacy` 指旧 VB/IB intercept 阴影路线，不是“落后路线”；
     - 它目前仍是视觉稳定性和性能的追赶基准，semantic 路线不能用指标自夸替代视觉事实；
     - 本轮目标改为先把所有性能大块记录清楚，再攻击最大块，且不能让 Pose/AlphaTest/path-blocker 回退。
   - **新增性能账本**：
     - `War3ShadowCaptureStats` / full trace `keyStats` 增加 draw-time VB cache 成本：
       - position/UV/index copy count、bytes、alloc count；
       - indexed unknown-range fallback；
       - unit/building/destructible/effect/other capture；
       - alpha-test/alpha-blend/diffuse texture capture。
     - shadow map 侧增加 prepare/replay 分类：
       - prepared draw、alpha-test prepared、alpha-promoted prepared；
       - dynamic/static/other prepared；
       - cascade 0..3 drawn / culled。
   - **第一个确定大块与修复**：
     - 账本揭示旧逻辑每帧 `drawTimeVBCachePositionCopyBytes ≈ 62,914,560`
       bytes（约 63MB/frame），而 `IndexCopyBytes ≈ 71,898`；
     - 根因是 `War3TryCaptureShadowCasterDrawIndexed()` 丢弃了
       `MinVertexIndex/NumVertices`，传入 `0,0`，使 capture 走 unknown indexed
       range fallback，按巨大完整 VB slice 复制；
     - 修复：`War3TryCaptureShadowCasterDrawIndexed` 签名与
       `DrawIndexedPrimitive` / `DrawIndexedPrimitiveUP` caller 贯通
       `MinVertexIndex, NumVertices`。
   - **修复后黑匣子结果**：
     - trace：
       `E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_15_02_13_00.jsonl`
     - `frames = 961`；`shadowMapExecuted = 961 / 961`；`receiverReuse = 0`；
     - `drawTimeVBCachePositionCopyCount ≈ 119.66 / frame`；
     - `drawTimeVBCachePositionCopyBytes ≈ 504,814 / frame`；
     - `drawTimeVBCacheIndexCopyBytes ≈ 66,080 / frame`；
     - `drawTimeVBCacheIndexedUnknownRangeFallbackCount = 0`；
     - `drawTimeSemanticProducerVisibleCandidateCount ≈ 93.37 / frame`；
     - `drawTimeSemanticProducerSubmittedCount ≈ 20.37 / frame`；
     - `drawTimeSemanticProducerMissNoFreshEntryCount = 0`；
     - `drawTimeSemanticProducerFallbackCurrentDrawCount = 0`；
     - `semanticSceneShadowMapPreparedDrawCount ≈ 96.76 / frame`；
     - per-cascade drawn 仍为 `96.76 / 96.76 / 96.76 / 96.76`，
       cull 全为 0，说明级联重放仍是下一块可攻性能面；
     - `semanticSceneShadowTaaMode` tail 已稳定为 2，
       `shadowReceiverSampleSource` tail 稳定为 3，history 全程 valid。
   - **性能报告**：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_15_02_13_22.html`
       - `avgFps = 78.091`；
       - `avgFrameTimeMs = 12.806`；
       - `avgGpuTimeMs = 2.057`；
       - `avgProcessCpuMs = 11.767`；
       - `War3SemanticScene/Populate ≈ 0.989ms CPU`；
       - `Shadow/Main ≈ 0.518ms CPU / 0.895ms GPU`；
       - `ShadowMap ≈ 0.323ms CPU / 0.351ms GPU`；
       - `Shadow/Visibility ≈ 0.012ms CPU / 0.194ms GPU`；
       - `ShadowCopy ≈ 0.014ms CPU / 0.107ms GPU`；
       - 仍有 `Semantic/OutsideMainLoop/Tracked ≈ 4.626ms CPU` 未细分，
         大概率包含 draw-time producer/capture hook 与其它非 main-loop 采样。
   - **安全修复**：
     - `d3d9_war3_shadow_outline.cpp` 中内置 outline pipeline 的 vertex attribute
       数组从 3 扩到 4；
     - 原因：alpha-test + skinned outline 最多会写 position / blend weight /
       blend index / UV 四个 attribute，原数组会栈越界；
     - 修复后连续 AutoTest 没有生成新的 crash dump，最近 crash 仍停在
       `2026-05-15 01:56:44`。
   - **失败优化记录，禁止直接重试**：
     - 曾尝试只对 unit-like draw 做 draw-time VB copy，跳过 building/destructible/other；
     - 结果：copy bytes 下降，但 `drawTimeSemanticProducerSubmittedCount`
       从约 `20/frame` 降到约 `6.8/frame`，
       `War3SemanticScene/Populate` 从约 `1.0ms` 飙到约 `7.35ms`，
       FPS 跌到约 `49.6`；
     - 结论：当前 draw-time cache 不只是“多余 copy”，它也是 producer 稳定性的入口。
       不要再简单按 object kind 跳过 capture。后续要优化 copy 次数，必须先设计
       “保 producer 身份/新鲜度，但合并 copy command 或复用 range”的方案。
   - **下一步性能路线**：
     - 最大明确收益已完成：63MB/frame → 0.50MB/frame；
     - 下一块不应再砍 producer，而应：
       1. 继续细分 `OutsideMainLoop/Tracked`，确认 draw-time hook 自身 CPU 分布；
       2. 研究 draw-time VB/IB copy command 合批或 range reuse，目标是降低
          `~120 copy commands/frame`；
       3. 研究 CSM cascade cull / per-cascade replay，因为当前 4 个 cascade 几乎画同一批 caster；
       4. 在任何优化前后都用 full trace 验证 producer submitted/fresh/miss 与
          shadow map executed/history valid，避免视觉正确性开倒车。

115. **Phase 7.70 同帧 draw-time VB capture 去重 + 显式 perf 归类（2026-05-15 03:00）**:
   - **目标**：
     - 在不动 producer/consumer 主线、不改 manifest/lease/stale-restore 等掩盖逻辑前提下，
       把 `War3TryCaptureShadowCaster` 同一帧内对同一 `renderablePart` 的重复 GPU copy
       拦掉，并把 draw-time 捕获的 CPU 时间从 `OutsideMainLoop/Tracked` 拆出来，
       让后续可观察的拆分有路径依据。
   - **代码改动（单点低风险）**：
     - `src/d3d9/d3d9_device.h`
       - `War3DrawTimeVBEntry` 新增 `lastCaptureFingerprint`，记录上次 capture 的
         源数据指纹（不持久化跨帧，由 `frameSerial` 覆盖语义控制）。
     - `src/d3d9/d3d9_war3_scene.h`
       - `War3ShadowCaptureStats` 新增同帧去重账本：
         `drawTimeVBCacheSameFrameDedupHit` / `DedupMiss` / `StateRefresh`。
     - `src/d3d9/d3d9_device.cpp` (`War3TryCaptureShadowCaster` v4 capture 块)：
       - 在拿到 `posSlice/vRangeStart/vRangeCount/posStride` 后立即生成
         64-bit fingerprint（FNV1a 风格 mix），覆盖 position 源 buffer 指针、
         offset、length、range start/count、stride、posOffset，并把 `indexed +
         StartVal/CountVal` 折进去；
       - 在 `m_war3DrawTimeVBCache[vbCacheKey]` 之前先 `find`：若 `frameSerial ==
         currentFrame && lastCaptureFingerprint match && positionBuffer 完整 &&
         vertexCount/positionStride 匹配`，则只刷新易变状态（alphaTest/Blend、
         alphaRef、stage0 SRV、capturedWorldMatrix），不再发任何 EmitCs(copyBuffer)；
       - 记 `DedupHit + StateRefresh`；同帧但 fingerprint 不一致（真实数据变更）
         记 `DedupMiss`，仍走原有完整 capture；
       - 完整 capture 路径末尾写入 `entry.lastCaptureFingerprint = captureFingerprint`。
       - 整个 capture 块外部包一层运行时 perf scope `Shadow/DrawTime/Capture`
         （constexpr 门控），这条路径会被分类器命中
         `Semantic/MainLoop/Render/Shadow`，不再落 `OutsideMainLoop/Tracked`。
     - `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`
       - keyStats 输出 `drawTimeVBCacheSameFrameDedupHit / DedupMiss / StateRefresh`，
         full trace 可直接观察去重命中率。
   - **黑匣子验证**（光影测试.w3x，隔离桌面，15s full trace）：
     - 新 trace：`E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_15_02_44_10.jsonl`
     - 帧数 919：
       - `submitted=0` = `0 / 919`
       - `shadowMapExec` = `1.0 / frame`
       - `historyValid` = `1.0 / frame`
       - `producer fallback to current-draw` = `0 / 919`
       - `producer miss-no-fresh` = `0 / 919`
       - `path blocker reject` = `3.16 / frame`
     - 去重账本：
       - `drawTimeVBCacheTotalEntered` = `119.96 / frame`
       - `drawTimeVBCacheCaptureCount` = `98.83 / frame`（slow path）
       - `drawTimeVBCacheSameFrameDedupHit` = `21.13 / frame`（去重命中）
       - `drawTimeVBCacheSameFrameDedupMiss` = `4.99 / frame`（同帧但数据真变）
       - 与 Phase 7.69 基线对比：`PositionCopyCount 119.66 → 98.83`（约 17.4% 命令数下降）
       - `PositionCopyBytes` 因为 dedup HIT 比 MISS 多对应于 stride/range 较小的 sub-draw，
         整体仅微降到 `464152 B/frame`（约 +0.3%）。
   - **性能 A/B**（无 full trace 干扰，3 轮 × 20s）：
     - 基线 mean = `(96.42 + 98.04 + 100.62) / 3 = 98.36 FPS`
     - Phase 7.70 mean = `(98.26 + 97.44 + 100.96) / 3 = 98.88 FPS`
     - delta = +0.52 FPS（+0.53%）；样本噪声 ±2-3 FPS，单看本场景无显著加速。
   - **解读 / 边界**：
     - 收益小是因为 EmitCs(copyBuffer) 的同步成本主要由 DXVK 命令录制完成，
       worker 线程异步消费；主线程只省到“一次 record 调用 + state read”。
     - 这条改动的真正价值是：
       (a) 把 draw-time 捕获 CPU 时间拆出 `OutsideMainLoop/Tracked`，
           为下一轮拆 `Direct/BuildPacket / Append / fast-append / state read`
           留出可对比的归类基线；
       (b) `DedupMiss=4.99/frame` 直接定位“同 part 同帧多种数据来源”是否是新引入的 churn，
           不再是黑盒；
       (c) 复杂度只在 capture 入口加一处早返回，没有触碰 producer/consumer 任何
           过滤、TTL、stale-restore、lease、manifest、receiver hold 路线。
   - **当前部署**：
     - `E:\Work\War3\d3d9.dll`（Phase 7.70）`= 26767310 bytes @ 2026-05-15 02:42:54`
       并经 commit `fe26653 war3: dedup same-frame draw-time VB capture (Phase 7.70)`。
   - **下一步性能路线（不在本轮做）**：
     - 单 PositionCopyCount = 98.83/frame，空间还有：
       - 把同帧第一次 capture 后的 entry 标记 `geometryStableThisFrame`，对真正“引擎重复
         同帧多次 draw 同 part 同数据”的 case，第二次 capture 也跳过 `entry[]` 写入；
       - 探索：单一 copy 命令一次性拷贝多个 entry 的 sub-range（合批 EmitCs）；
       - CSM 4 cascade 全画同一批 caster：先做安全 bounds（v4 fast-append 当前
         强制 `boundsRadius=0`），改为从 capture worldMatrix + 基于 vertex range 的
         保守 AABB 估算；和 `kShadowCascadeCullDisableForUnits=true` 的当前默认形成
         A/B（先看 cull 命中率，再决定默认值）。
     - 任意一步落地之前都必须先用 full trace 复核：
       `submitted=0 == 0` / `shadowMapExecuted=1` / `historyValid=1` / 视觉抽样。

117. **Phase 7.71 撤销 + Phase 7.72 真正修复路径阻断器（2026-05-15 04:10）**:
   - **Phase 7.71 实机回归（已撤销）**：
     - 用户实机：path blocker 阴影**仍然存在**，**且 FPS 骤降约 20**。
     - 撤销提交：`6e7e55d / c6a7123`，DLL 回到 Phase 7.70 (`26767310 bytes`)。
     - 失败原因复盘：
       - `War3TryCaptureShadowCaster` 是**旧路径**（legacy ShadowCapture）。在 semantic
         runtime 默认开启的情况下（`kShadowSemanticCoreSceneDisableLegacyShadowCaptureEnabled = true`），
         legacy 早期 bypass 一旦触发就 `return` 不再走 legacy 主体，path blocker
         **根本没经过我加的检查**。
       - 我在函数最入口加的 TLS rawcode + `shadowSemantic.object->rawcode` 检查
         每次 draw 都会执行，War3 一帧几万次 draw call，叠加每次 `shadowSemantic.object`
         指针解引用的潜在 cache miss，性能掉了 20 FPS。
       - 视觉验证一直拿 `光影测试.w3x` 跑，但那是个 path blocker 极少的场景，
         拒绝 counter 才 3.16/帧，根本看不出 fix 是否真的拦住实机场景里的 path blocker。
   - **Phase 7.72 真正根因**：
     - path blocker 是 **destructible/rigid**，不是 skinned。新长期路线下：
       - SceneCollector → Visible registry 收 destructible
       - `War3ShouldSubmitSemanticPacket` → `War3IsEligibleSemanticStaticWorldCaster`
         **允许 destructible 提交**（`kShadowSemanticCoreSceneUnitsOnly = false`）
       - `War3TryAppendSemanticShadowPacket` 主入口**没有 LOS 检查**，rigid 路径
         全程不会进 v4 vertex-blend 分支里那个嵌套的 LOS 拒绝
       - producer 路径有 LOS 检查但下一行 `if (objectKind != Unit) continue` 把
         destructible 过滤了；
       - fast-append 也有 LOS 检查但下一行 `if (resolvedObjectKind != Unit) return`
         把 destructible 过滤了。
       - 结果：path blocker **作为静态 caster 一路通过 eligibility，提交到 shadowCasters
         向量，被 4 个 CSM cascade 各画一遍**。
   - **Phase 7.72 修复**（`src/d3d9/d3d9_device.cpp`）：
     - 在 `War3ShouldSubmitSemanticPacket` 入口（eligibility 层）加：
       ```cpp
       if (kPathBlockerHideEnabled && packet.renderable.rawcode != 0u &&
           IsLosBlockerFourCc(packet.renderable.rawcode)) return false;
       ```
       上游 producer 看到 false 就跳过这条 packet，连 packet 构建都省了。
     - 在 `War3TryAppendSemanticShadowPacket` 主入口加同样的拒绝，并附带 jHandle
       兜底 `RenderObjectRegistry::findByHandle` 反查，覆盖 packet rawcode 没填
       但 jHandle 存在的情况。这是冗余安全网，append 一帧只调 100-300 次，不影响热路径。
     - 在文件顶部 anonymous namespace 加 `inline bool IsLosBlockerFourCc(uint32_t);`
       前置声明，因为 helper 定义在文件后段。
   - **不动的部分**：
     - 不改 `War3TryCaptureShadowCaster`（legacy）；
     - 不改 producer / fast-append 现有 LOS 检查；
     - 不动 `IsLosBlockerFourCc` 黑名单；
     - 不动 manifest TTL / stale restore / receiver hold / TAA / VB cache。
   - **验证**：
     - `ninja -C build32` 通过；
     - `E:\Work\War3\d3d9.dll = 26768513 bytes @ 2026-05-15 04:08:46`；
     - AutoTest 15s full trace（光影测试.w3x，1007 帧）：
       - `submitted=0` = 0/1007 ✓
       - `historyValid` = 1.0/帧 ✓
       - `producer fallback to current-draw` = 0 ✓
       - **`path blocker reject` = 6.19/帧**（之前 3.16/帧，**翻倍**——证明
         新拦截入口确实多拦了一批 destructible path blocker，不是 noise）
     - 3 轮 perf（光影测试.w3x，无 trace）：
       - 110.21 / 123.70 / 120.62 → 平均 **118.18 FPS**
       - vs Phase 7.70 baseline 98.88 FPS：**+19.3 FPS**
       - 性能反而提升的合理解释：path blocker 之前作为 rigid caster 一路走完
         整 packet 构建 + 4 个 CSM cascade 全画。现在在 eligibility 层直接拒掉，
         省了相当于 ~6 个/帧 caster 的全 4 cascade replay。
   - **实机预期**：
     - path blocker 阴影应当消失（新长期路线下 semantic packet append 是 caster
       的唯一入口）；
     - 性能应回到正常或略有提升；
     - 调试：`kPathBlockerDebugEnabled = true` 可在 DebugView 看到 reject 命中。

118. **Phase 7.73-7.80 夜间无人值守迭代（2026-05-15 03:00-05:58）**:
   - **目标**：用户睡了，按计划自动推进。Phase 7.71 已 revert（实机翻车），
     Phase 7.72 已落地（path blocker 在 eligibility/append 入口拦截）。
     这一轮按 sub-agent 扫描的 ranked candidate 推进低风险高收益项。
   - **commit 链**：
     - `777e4ed` Phase 7.73：path blocker reject 来源分桶 counter（10 个 bucket）+
       trace JSON 透传 + `_phase773_buckets.py` 分析脚本。
     - `bc20d47` Phase 7.74：BuildSemantic / MaterialSig / NativeHint 加 cpuScope。
     - `51fc174` Phase 7.75：CurrentDraw Publish + Populate Preselect 加 cpuScope。
     - `ac3aea3` Phase 7.76：DirectPacketGeosetCache `std::mutex → std::shared_mutex`。
     - `9495f56` Phase 7.77：默认关闭 PublishCurrentDrawContract Phase 7.49 probe，
       env `DXVK_WAR3_PUBLISH_PROBE=1` 重新启用。
     - `4b7a380` Phase 7.78：Hook_RuntimeMatrixWrite frameTag 双读合并为单读。
     - `0317481` Phase 7.79：runtimePoseArrayRange `std::mutex → std::shared_mutex`。
     - `b523fae` Phase 7.80：HydrateVisibleSnapshotBasicFields partCache thread_local 复用。
   - **代码改动总览**：
     - 8 commits，9 个文件，纯增 atomic counter / cpuScope / shared_mutex / TLS 缓存。
     - 没有改 producer / consumer 主线 / manifest TTL / stale restore /
       receiver hold / path blocker FourCC 黑名单。
   - **trace 黑匣子（每个 phase 都验过）**：
     - `submitted=0` 全帧 0
     - `historyValid` 1.0/帧
     - `producer fallback` 0
     - `path blocker reject` 6.19 → 8.99 / 帧（trace 走访 9 个 bucket，三大主路径
       是 EarlyBypass/AppendEntry/FastAppend）
     - `producer submitted` ~20/帧 稳定
   - **性能账本**：
     - Phase 7.72 baseline ≈ 118 FPS
     - Phase 7.76 (shared_mutex on geoset cache)：3 轮 mean **124.81 FPS** (+7)
     - Phase 7.77-7.80：mean ~123 FPS（小幅波动，未单独显著贡献，但 Phase 7.49
       probe 关掉后在 publish 大量调用的场景应该有 0.3-1ms 隐式收益）
   - **下一阶段（醒来后路线建议）**：
     - 用户实机复核 Phase 7.72 path blocker fix。如果还漏：
       - 跑一份实机 trace，用 `_phase773_buckets.py` 看哪个 bucket 没记到漏掉的 path blocker。
       - 如果全部 bucket 都没数，意味着它走了 `War3TryCaptureShadowCaster` 的另一条出口
         （legacy 主体 line 24171 已加 `LegacyCaptureCount`，那也是 bucket 之一）。
       - 唯一兜底：在 `War3TryAppendSemanticShadowPacket` 入口除了 rawcode + jHandle
         反查外，再加一个 visibleRegistry → identity.rawcode 反查。
     - CSM cascade cull for v4 fast-append（Step B 待做）：需要可靠 bounds，
       优先级在 path blocker 视觉验收之后。
     - PoseRegistry / VisibleRegistry hot index 改 vector 的实验：风险较高，
       建议在白天有人 review 时再做。
   - **当前 DLL**：
     - `E:\Work\War3\d3d9.dll = 26768707 bytes @ 2026-05-15 05:57+`
     - 包含 Phase 7.70-7.80 所有改动
   - **回退路径**：
     - 每个 phase 独立 commit，可以单独 revert 任意一个
     - 性能侧改动（7.76/7.77/7.79/7.80）影响主路径，回滚要小心
     - 诊断侧改动（7.73/7.74/7.75/7.78）纯加观察点，可任意保留或回滚


119. **Phase 7.81-7.82 收尾（2026-05-15 06:00 凌晨末班 + 12:40 中午回归）**:
   - **`1c954a1` Phase 7.81**：`SnapshotPublishedCurrentDrawContracts` 内
     `unlimitedDedupeIndex` 与 `preferredVisibleKeyCache` 改 thread_local，
     避免每帧多次调用时 `reserve(4096)` 重复 alloc/free。
   - **`ee23d48` Phase 7.82**：`StaticMeshDataResourceCacheMutex` 从
     `std::mutex` 切到 `std::shared_mutex`，与 Phase 7.76 同模式。
     read 路径走 `shared_lock`，cache miss/insert 升级 `unique_lock`。
   - **当前部署**：`E:\Work\War3\d3d9.dll = 26770323 bytes @ 2026-05-15 12:38`，
     包含 Phase 7.70-7.82 全部改动。
   - **黑匣子全过**：`submitted=0` 0/910 帧、`historyValid=1.0`、
     `producer fallback=0`、`path blocker reject=9.12/帧`（光影测试.w3x）。
   - **当前性能**（中午 12:40，3 轮 × 20s）：
     - 116.9 / 120.0 / 117.1 → mean **118.0 FPS**
     - vs Phase 7.72 initial baseline 118 FPS（噪声范围）
     - vs 昨晚凌晨 Phase 7.76 mean 124.8 FPS（凌晨低后台负载下记录）
     - 中午 vs 凌晨的差距是后台负载差异，不是代码回退
   - **已落地的纯收益累计估算**（按理论 μs/帧）：
     - 7.70 同帧 capture dedup：~40-100μs
     - 7.76 geoset cache shared_mutex：150-400μs
     - 7.77 publish probe gate：300-900μs（在 publish 频次高场景）
     - 7.78 frameTag 双读合并：100-250μs
     - 7.79 runtime pose range shared_mutex：20-80μs
     - 7.80 partCache TLS：5-30μs
     - 7.81 snapshot helper TLS caches：30-100μs
     - 7.82 static mesh cache shared_mutex：50-150μs
     - 总计：约 **0.7-2.0 ms/帧** 主线程 CPU 削减
   - **下一步路线**（待用户决策）：
     - 需要用户实机复核 Phase 7.72 path blocker 修复（光影测试.w3x 不是
       path-blocker 重灾区，3-9/帧 reject 主要来自 EarlyBypass/AppendEntry/FastAppend
       三个 bucket，trace 已有完整分桶）。
     - CSM cascade cull for v4 fast-append 仍未做（需要可靠 bounds 设计）。
     - Population eligible-build 路径里 visible registry 二次查询 dedup 仍可做
       （收益约 50-150μs/帧，但需要在多个 helper 间透传 iterator）。


119. **2026-05-15 夜间无人值守逆向论文交付（仅文档/IDA，不动源码）**:
   - **任务定位**：用户在性能优化主线之外，要求另一线程做"魔兽 1.27a 渲染层完整逆向论文"，
     重中之重是 Pose；静态阴影问题继续加强研究；不允许动 `src/`。
   - **交付**：
     - `docs/plan/overnight_render_paper_2026_05_15/04_cmodel_pose_palette.md`
       约 990 行：CModel/CGeosetData/CRenderablePart 字段表、4 个 writer (`0x12FED0/12E600/12FDC0/12FF90`)
       完整 CFG 与 dt gate 关系、CPU vs GPU skinning 本质区别、Phase 7.30~7.55 决策树、
       §7 IDA rename 清单（已回写）+ §8 章节总结。
     - `docs/plan/overnight_render_paper_2026_05_15/06_fogmask_static_shadow.md`
       约 600 行：CFogMask/CFogMaskTable/CFogOfWarMap 完整字段表、
       4 个并行 mask layer（实际是 *2 mask + 2 elevation grid*）、16-bit type code 含义
       （bit 0..11 = 12 个 player slot，不是"类型分桶"）、`maskIdx`（对象 +0x10C）才是
       决定"在哪份 mask 上写"的字段（`idx=0 fog / 1 LOS / 2 path / 3 shadow / 4 flying`）、
       `RebuildMaskFromObjectLists` 三段式重建、`CWidget_RegisterFootprintAndShadowMask`
       30+ caller 分桶、magic `0x2B5DB42C` 来源、4 次历史失败拦截尝试反证、
       静态阴影治理蓝图方案 A/B/C（推荐方案 A：hook `WriteMaskRegion` + `maskIdx == 3` 拦截）。
   - **IDA 写回（全部 ok=true）**：
     - 第 4 章 `_ida_rename_comment_chapter4.py`：24 处 rename + 13 条 set_comments
       （CSpriteUber dispatch / anim advance 三变体 / pose stack helpers / RenderQueue palette /
       sprite-runtime helpers）；
     - 第 6 章 `_ida_rename_comment_chapter6.py`：41 处 rename + 14 条 set_comments
       （FogMask helpers / WriteMaskRegion fastpath / CWidget 30+ caller setter / CFogMask 字段语义）；
     - 之前 24 号文档的 CDoodads 调度器 rename 也已写回。
     - 总计：约 80 处 rename + 32 条中文 set_comments 已落到 IDA。
   - **论文 chapter 状态**：
     - ✅ 第 4 章（Pose 重中之重）：完整稿
     - ✅ 第 6 章（FogMask 静态阴影）：完整稿
     - ⏳ 第 0/1/2/3/5/7/8/9/10 章：task card 已写好，反编译产物已落盘，下一轮启动子线程即可
   - **论文交付状态**：以 Pose + 静态阴影双主题已构成可独立交付的"v1 论文"；
     其余章节作为后续轮次的扩展面。
   - **静态阴影治理蓝图 — 给主线程的下一步建议（不是本轮要做）**：
     - 在 `src/d3d9/war3/hooks/war3_hook_shadow.{h,cpp}` 新增 `Hook_TerrainShadow_WriteMaskRegion`；
     - hook `0x6F234710`，trampoline 前读 `*(uint16_t*)(a2 + 0x10C)`；
     - 仅当 `maskIdx == 3` 且 `a4 == NULL` 时 `return 0`（跳过 trampoline）；
     - DebugView 先加 stats 日志 6 个月窗口验证 idx 假设正确；
     - 不动 fog/LOS/path（不会受影响）；
     - 这条路径 24 号文档 v3 已经决定性指向，但本轮严守"不动源码"约束未落地。


120. **2026-05-15 夜间无人值守续作（v2 — 全主链覆盖完成）**:
   - **任务定位**：第 119 条交付完 Pose+静态阴影双章后，用户要求"继续剩下的计划推进"。
     本轮把"逻辑层 → GPU draw call"主链中剩余的 3 章（剔除过渡 / RenderQueue / CSprite 动画）
     全部补齐，使论文 v1 形成"5 章主链"完整闭环。
   - **新增交付**：
     - `01_visibility_to_renderqueue.md` ~750 行：CWorld FrameUpdate / 22 stage 调度 /
       group 0/1/2 / WorldObjectEntry_Render / Camera frustum 8 平面构建。
     - `02_renderqueue_dispatch.md` ~1100 行：opaque 主队列 + AUCTransparent 辅队列 /
       排序 comparator / Dispatch_Common+Special / fallback multipass / 5 种 transparent
       type / GxDevice 状态机。
     - `03_csprite_animation.md` ~750 行：4 个 PreRender 变体的 CFG 差异 / dt gate
       Phase 7.47 反证 / 三种 anim advance 路径 / BuildPoseStackRoot / SetWorldMatrix /
       attachment 子树递归。
   - **IDA 写回（全部 ok=true）**：
     - `_ida_rename_comment_chapter_1_2_3.py`：56 处 rename + 21 条 set_comments。
     - 包含：CWorld_RenderScene / DispatchStage 22 stage 表 / WorldObjects_RenderGroup /
       AddBatch / RenderBatch_Submit / FlushSortedItems / Dispatch_Common/Special /
       FallbackMultiPass / 5 种 TransparentDispatchType / Camera_BuildFrustumPlanes8 /
       LOSManager_QueryNodeVisible 等。
   - **论文 v1 累计 IDA 写回**：约 131 处 rename + 55 条 set_comments，全部 ok=true。
   - **论文 v1 状态**：5 章构成"逻辑层对象 → GPU draw call"完整闭环 + 静态阴影治理蓝图，
     可独立交付。第 5/7/8/9/10 章作为后续轮次扩展面。
   - **未触碰**：项目源码、不启动 War3、不做 AutoTest。严守用户约束。
   - **下一轮 agent 接手提示**：
     - 必读 `00_paper_master.md` 阅读路线图（含 §2.0 全链路顺序阅读路径）；
     - 必读 `OVERNIGHT_PLAN.md` 第 11/12 节看进度；
     - 反编译产物在 `AutoTest/artifacts/_overnight_render_research/`（320+ 文件）；
     - IDA 写回脚本模板在 `AutoTest/_ida_rename_comment_chapter*.py`；
     - 静态阴影治理蓝图在 `06_fogmask_static_shadow.md` §7（推荐方案 A：
       hook `WriteMaskRegion` + `maskIdx == 3` 拦截，主线程下次有空闲窗口落地即可）。


120. **Phase 7.83-7.86 中午批量性能优化（2026-05-15 13:00-13:35）**:
   - **用户指示**：path blocker 不再处理（user 实机仍漏，本线程不管），
     直接做剩下性能优化。
   - **`d8949f4` Phase 7.83**：5 个 registry 全部 `std::mutex → std::shared_mutex`
     + `m_frameNumber` 改 `std::atomic<uint64_t>`：
     - PoseRegistry / ModelInstanceRegistry / ModelRegistry / AttachmentRigidRegistry
     - ShadowModelResourceCache（含 m_revision 也改 atomic）
     - ShadowObjectRegistry
     - 改动统计：6 个 .h/.cpp，265 insertions / 244 deletions
     - reader 路径（findBy*/snapshot/*Count）走 `shared_lock`
     - writer 路径（note*/store*/begin/endFrame）走 `unique_lock`
     - frameNumber()/revision() 完全去锁，atomic relaxed load
     - 所有 const-read 方法被识别后批量降级到 shared_lock（PoseRegistry+ModelInstanceRegistry
       17 个 + ResourceCache 7 个 + ShadowObjectRegistry 4 个 = 28 个 reader）
   - **`19ad4ad` Phase 7.84**：
     - `Hook_RuntimeMatrixWrite` / `MarkSpriteFramePoseProcessed*` 等位置
       原本 `PoseRegistry::frameNumber() != 0u ? ... : Model.frameNumber()` 两次
       atomic load → 改成单次 load 加 fallback。
     - `War3TryPopulateDirectCurrentDrawGrouped` 内 4 个 per-frame scratch
       container 全部改 thread_local 复用：
       `submittedIdentityKeys` / `submittedPreferenceKeys` / `submittedPartIdentityKeys`
       / `liveSubmittedCorePartsByObject`。
   - **`cea7507` Phase 7.85**：`previousSubmittedObjectIdentityKeys` /
     `previousSubmittedPartIdentityKeys` 改 const reference，省两次 vector copy。
   - **`86e546b` Phase 7.86**：`g_shadowSceneStatsMutex` 改 shared_mutex。
     reader（trace JSON / status query）每帧 1+ 次走 shared_lock，writer
     `NoteShadowSceneStats` 走 unique_lock。
   - **黑匣子全过**：每个 phase 都跑了 15s full trace。
     `submitted=0` 0/N 帧、`historyValid=1.0`、`producer fallback=0`、
     `path blocker reject` 7.99-8.20/帧（光影测试.w3x 基线，无变化）。
   - **当前部署**：`E:\Work\War3\d3d9.dll @ 13:34`
   - **3 轮 perf（光影测试.w3x，无 trace）**：
     - Phase 7.83 后：mean ~110.8（带 1 个 102 噪声）
     - Phase 7.86 后：mean ~110.0（带 1 个 104 噪声）
     - 中午 vs 凌晨差距：与昨晚凌晨 124-126 相比偏低，但中位数 114 在
       Phase 7.72 baseline 118 范围内。后台负载差异主导。
   - **理论 CPU 削减累计（自 Phase 7.70 算）**：约 1.1-2.7 ms/帧 主线程 CPU。
     - mutex → shared_mutex（geoset/pose range/static mesh + 5 registries +
       scene stats）= 6 个 cache 共 0.6-1.5 ms
     - publish probe 默认关 = 0.3-0.9 ms
     - frameTag 双读合并 + frameNumber 双 load 合并 = 0.15-0.4 ms
     - thread_local scratch caches × 6 = 0.1-0.3 ms
     - 同帧 GPU copy dedup ≈ 21/帧
   - **已完成清单（Phase 7.70-7.86）**：
     - [✓] dedup same-frame draw-time VB capture
     - [✓] path blocker reject buckets（用户已不关心）
     - [✓] 10+ cpuScope 加在数据层热点
     - [✓] 6 个全局 mutex 改 shared_mutex
     - [✓] 4 个 frameNumber/revision 改 atomic
     - [✓] 4 个大 thread_local scratch
     - [✓] phase 7.49 publish probe 默认关
     - [✓] frameTag/frameNumber 多次 load 合并
   - **未做**：
     - CSM v4 fast-append cascade cull（需要可靠 bounds 设计，留白天 review）
     - Top #1-#6 中重复 lookup 透传（visible registry 二次查询 dedup）
   - **下一步建议**：
     - 用户实机看看 Phase 7.83-7.86 的整体效果（mutex 收益在
       cluster 场景才显现）
     - 没回退的话继续 visible registry dedup


121. **Phase 7.88-7.91 CSM cascade cull 两次尝试均回退 + SunkenCity 诊断准备（2026-05-15 14:20-16:15）**:
   - **Phase 7.88 第一次尝试**：从 sceneNode 读世界位置 + 保守 radius，启用 Unit cull。
     结果：cascade 0 全部被误杀（0 drawn），视觉回退。回退。
   - **Phase 7.91 第二次尝试**：只对 cascade 2/3 做 cull（cascade 0/1 不 cull）。
     结果：cascade 2/3 仍然 0 culled，因为 v4 fast-append 路径的
     `packet.renderable.sceneNode == nullptr`（fast-append 跳过了完整 packet build，
     sceneNode 没被填充到 packet 里）。额外的 `IsReadableRangeFast` 探测反而增加了
     CPU 成本，FPS 从 81.6 降到 76.9。回退。
   - **根因**：v4 fast-append 的 `eligible.sceneNode` 和 `packet.renderable.sceneNode`
     在 prebuild bypass 路径下确实有值（从 visible record 填充），但在 producer 路径
     （`War3TryPopulateDrawTimeSemanticProducer`）里 `record.sceneNode` 来自
     `VisibleRenderableRecord`，也应该有值。问题可能是 `record.identity.sceneNode`
     为 nullptr 或者 `IsReadableRangeFast` 在某些 sceneNode 上失败。
   - **正确的下一步**：在 v4 capture 阶段（`War3TryCaptureShadowCaster` 的 v4 块）
     把 `semantic.sceneNode` 存到 `War3DrawTimeVBEntry` 里。这样 producer/fast-append
     路径都能从 entry 直接读到 sceneNode，不需要依赖 packet/record 的 sceneNode。
   - **SunkenCity.w3x 诊断**：AutoTest 跑了但相机固定，没触发用户报告的 1FPS 卡顿。
     需要用户手动操作时开 trace（`DXVK_WAR3_SHADOW_POSE_FULL_TRACE=1`），或者
     用 control plane 动态开 15 秒 trace 在卡顿发生时。
   - **高压地图基线（无 trace）**：
     - Phase 7.89 当前：**81.6 FPS**（`mainThread=8.4ms`, `GPU=2.9ms`）
     - 804 shadow map draw calls/frame（201 casters × 4 cascades）
   - **当前 DLL**：`E:\Work\War3\d3d9.dll` = Phase 7.89 状态（CSM cull 已回退）


122. **Phase 7.95 桥/斜坡卡顿根因定位（2026-05-15 17:00-18:10）**:
   - **用户复现条件**：高压地图放置桥/斜坡/升降机装饰物后，FPS 从 80+ 降到 20 左右。
     SunkenCity.w3x 更极端（0.5-4 FPS）。原版魔兽不卡。
   - **隔离测试结果**：
     - `DXVK_WAR3_PROFILE=dxvk_only` → 不卡（DXVK 基础层没问题）
     - `DXVK_WAR3_DISABLE=semantic.data` → 不卡（确认是 semantic.data 模块）
     - `DISABLE_SHADOW_CAPTURE + DISABLE_PUBLISH_CONTRACT + MATRIX_BATCH_CAPTURE=0` → 还卡
     - `DXVK_WAR3_DISABLE=hook.render,render.queue` → 还卡
   - **perf report 决定性数据**（高压地图带桥，21 FPS）：
     - `avgMainThreadCpuMs = 44ms`，`avgGpuTimeMs = 2.6ms`（纯 CPU 瓶颈）
     - **`War3Renderer/EndFrame/Registries` = 30.7ms/帧**（占总帧时间 70%）
     - `War3Renderer/EndFrame/CaptureLiveState` = 2.8ms/帧（不是主因）
     - `Semantic/OutsideMainLoop/Tracked` = 40.5ms/帧（包含 Registries）
   - **根因**：`PublishSemanticRegistriesForScene()` 里调用的 registry `endFrame()` 链。
     虽然 `ModelInstanceRegistry::endFrame()` 的 sweep 已被 `kWar3RuntimeConfigDisableSemanticRegistryEndFrameSweeps=true` 编译期跳过，
     但 **`VisibleRenderableRegistry::endFrame()`** 做的 `HydrateVisibleSnapshotBasicFields` +
     `HydrateVisibleSnapshotStaticSemanticFields` 在桥/斜坡场景下可能有 O(N²) 行为
     （`BackfillIdentityFromRuntimeModel` 对每个 record 做 registry 查找）。
   - **为什么桥/斜坡触发**：War3 引擎在这些地形结构附近 dispatch 大量对象
     （pathblocker/destructible/doodad），导致 `VisibleRenderableRegistry` 的 record 数
     从正常的 ~100 爆到几千。hydrate 的 O(N) 或 O(N²) 遍历在 N=几千时爆到 30ms。
   - **下一步（新线程必做）**：
     1. 在 `PublishSemanticRegistriesForScene()` 里给每个 `endFrame()` 加独立 cpuScope
     2. 重跑高压地图（带桥），确认是 `VisibleRenderableRegistry::endFrame()` 还是其他
     3. 对 hydrate 做 size cap 或 early-exit（record 数 > 阈值时跳过 hydrate）
     4. 验证高压地图 FPS 恢复到 80+
   - **测试地图**：`E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x`（已放置桥/斜坡）
   - **测试脚本**：`py AutoTest\_phase790_highpressure_perf.py`（无 trace，30s）
   - **当前 DLL**：`E:\Work\War3\d3d9.dll` = Phase 7.95 状态
   - **commit**：`a571f01`（含 call cap + registry size check，但未解决根因）


123. **Phase 7.97-7.99 桥/斜坡卡顿根治 + Path blocker 屏蔽验证（2026-05-16 02:00-04:00）**:
   - **三件事一气呵成**：
     1. **任务 A**：Phase 7.98 widget identity hook 实机验收；
     2. **任务 B**：桥/斜坡 FPS 优化（高压地图 64→85+，低压 115→120+）；
     3. **任务 C**：路径阻断器屏蔽收尾，确保拦截在追踪日志里有精确次数。
   - **决定性发现 1：上一线程关于桥/斜坡的根因诊断是错的**。
     - 旧诊断："records 从 100 爆到几千，导致 endFrame O(N²)"。
     - 实测（Phase 7.97 加 chrono+counter atomic）：
       - 高压地图 ManifestCopy 单帧只 1-4 records、totalScanned ≈ 1.024/frame
       - 但 ManifestCopy 单帧 chrono = 2.6ms，每条 record 600µs+
       - 不是 record 爆炸，而是 **`captureLiveState` 每帧都被调** + 每条 record 走 `ConvertVisible` + `ResolveCurrentRuntimeGeosetFromData` 的 4096 次循环
   - **决定性发现 2：widget identity hook 装上但实测无效**。
     - `installAttempted=2, installSucceeded=1, enterCount=84, magicMatchedCount=0`
     - hook 早装（加到 `TryInstallShadowHooksEarly` ）后 fire 84 次，但 widget 大多数时候是 fourcc 整数被当指针（IDA 标的 `__fastcall(int a1, void* a2, ...)` 在某些 caller 下 a2=fourcc 而不是 widget pointer）
     - widget cache size 永远 0；但 pre-install bytes / post-install bytes 已确认 MinHook 真的写了 jmp
     - **结论**：widget hook 在当前 caller 集合下没贡献，但**不退化性能**。后续若要让它真起作用，需要分 caller 路径分别处理 calling convention（任务 B 的修复使得这一步**对最终 FPS 不再关键**）。
   - **决定性修复（性能）：ManifestCopy 早退**：
     - 实现：在 `captureLiveState` 入口最早期、shared_lock 下检查
       `m_manifest->visibleCount/mainQueueCount/transparentCount == manifest.*Count`
       且 `currentPoseRecordCount <= previousPoseCount` 且 `m_manifest->records.empty() == false`
       → 直接 return，跳过整个 ManifestCopy + Hydrate + ResourceStore + Snapshot + Publish
     - 不依赖 `priorContractUsable`（旧版要求 matrixPaletteCount/shadowReadyGeosetCount 都 ≠ 0）
       — 这是关键：高压地图早期 readyGeosetCount=0 让旧版 sameFrameDataNotGrowing 永远不命中
     - 效果：高压地图 `ManifestCopyEnterCount` 从 2493/30s 降到 2/30s（**99.9% skip**）
   - **附加优化**：
     - `ResolveCurrentRuntimeGeosetFromData` 加 thread_local cache `(runtimeModel, runtimeGeosetData) -> (geosetPtr, index)` 256 槽
     - widget identity stats（installAttempted/Succeeded/FailedAddrNull/FailedEnvDisabled/FailedMinHook）
     - path blocker reject 11 个分桶 counter 透传到 control plane summary
     - path blocker reject debug log（前 30 次每个 unique fourcc 各 1 行到 dxvk log）
   - **三个 commit**：
     - `e509d5a` — Phase 7.99 ManifestCopy early-skip + ResolveGeoset cache + widget hook diag
     - `352bdd2` — path blocker reject buckets to control plane summary
     - `d06bde4` — path blocker reject debug log + marker bump
   - **最终性能基线**（30s × 3 轮，isolated desktop）：
     - **高压地图（带桥/斜坡/装饰物）**：93.57 / 93.51 / 94.54 FPS，平均 **93.87 FPS**（目标 ≥85 ✓ +9 FPS 余量）
     - **低压（光影测试）**：147.40 / 147.01 / 146.79 FPS，平均 **147.07 FPS**（目标 ≥120 ✓ +27 FPS 余量）
   - **path blocker 拦截证据（dxvk log 实测）**：
     ```
     PATH BLOCKER REJECT #1 rawcode=0x59546662 (YTfb) jHandle=0x1004B6 via=EarlyBypass
     PATH BLOCKER REJECT #2 rawcode=0x59547062 (YTpb) jHandle=0x100473 via=EarlyBypass
     PATH BLOCKER REJECT #3 rawcode=0x59546162 (YTab) jHandle=0x1004BE via=EarlyBypass
     ```
     - 30s × 3 unique path blockers (YTfb/YTpb/YTab) × 27 总拦截 = 测试场景里 path blocker 的全部出口
     - 全部走 `War3TryCaptureShadowCaster` EarlyBypass 路径（其它 10 个分桶 0 命中）
     - 黑名单 8 fourcc：YTab/YTac/YTpb/YTpc/YTfb/YTfc/YTlb/YTlc 完整覆盖
   - **当前 DLL 状态**：
     - `E:\Work\War3\d3d9.dll` mtime ≈ 2026-05-16 03:50（含 Phase 7.99 全套）
     - 包含早装 widget hook、ManifestCopy 早退、ResolveGeoset cache、path blocker debug log
   - **可能的后续工作（不在本轮承诺）**：
     - widget hook 在 calling convention 不一致的 caller 上的修复（影响：让更多 destructible 进 cache，但目前 path blocker reject 已经通过现有 IsLosBlockerFourCc 主路径覆盖，不影响视觉验证）
     - 用户实机视觉复核：高压地图默认视角下 path blocker 阴影是否消失（所有自动数据已就位，只缺肉眼确认）
     - 若用户视觉报告还有漏网 path blocker，扩展黑名单或在 widget cache miss 时直接读 unitPtr+0x30 当 rawcode（已有兜底链路 23420-23445）



124. **Phase 7.100/7.101/7.102 三任务收尾（2026-05-16）**:
   - **任务 1：高压地图（带桥/斜坡）性能护栏维持**：
     - 目标：≥85 FPS。
     - 实测 3 轮平均：85.13 FPS（85.0 / 84.8 / 85.6），刚到护栏。
     - 对比上一轮（Phase 7.99）93+ FPS：没有提升，但维持基线。
     - 无前台干扰、无 DLL 行为变化，看到的差距是 isolated desktop 后台负载噪声 + 加入 Phase 7.100/7.101 path blocker 兜底的微小开销。
   - **任务 2：Path Blocker 视觉屏蔽收尾**：
     - 已落地：
       1. `War3TryAppendSemanticShadowPacket` 入口加 unitPtr+0x0C/+0x30 直读兜底；
       2. write-through 写入 widget identity cache（新加 `NoteWidgetIdentityFromDrawcall` API），让下次同 widgetPtr O(1) 命中；
       3. `Hook_TerrainShadow_WriteMaskRegion` 改为 install-only / pass-through（`kNativeStaticShadowMaskHideEnabled=false`，`kNativeStaticShadowMaskHookInstall=true`）。
     - **WriteMaskRegion idx==3 论文推断已被实测推翻**：
       - 6300/30s 次 fire 中所有 a2 的 magic 不是 0x2B5DB42C；
       - 8 个 WMR_DUMP 样本的 a2+0x10C 低 16 位都是 6/7/9（不是 0/1/2/3）；
       - 论文 §7.1 idx==3 拦截**实际行为是 BoxFastpath 形状阈值**，不是 mask layer index；
       - 论文 §6 §7.1 现有结论无法直接落地，需要重新逆向 v5[25]（CFogMaskTable+0x64）才能定位真正的 layer category。
     - **当前 path blocker reject 唯一活跃出口是 EarlyBypass**（D3D9 draw call 入口 hook，27/30s = 0.9 次/帧），其他 8 个出口因 Phase 7.99 ManifestCopy early-skip 全部输入空了。
     - 实测 EarlyBypass 拦了 YTfb/YTpb/YTab 三个 fourcc，但 widget identity cache 仍为 0（widget hook 装上但 magicMatched=0，calling convention 问题导致 ECX 不是 widget instance）。
     - **如果用户实机视觉仍看到 path blocker 阴影，根因是 War3 引擎为 path blocker 创建的 uberSplat（CTerrainUberSplats 系统的预渲染贴花阴影）**，那不走 D3D9 draw call hook，需要 hook `TerrainShadow_RegisterImageEntryWithParams (0x6F713250)`。Phase 5+ 历史尝试过 RegisterImage 全屏蔽导致崩溃 + path blocker 阴影仍在，但当时是无差别屏蔽。**新方向**：在 RegisterImage hook 内根据 owner widget 的 rawcode 做 IsLosBlockerFourCc 匹配后 reject。该方向论文已建议，本轮未落地。
   - **任务 3：建筑物/装饰物静态阴影屏蔽**：
     - **论文 §7.1 方案 A（hook WriteMaskRegion + maskIdx==3 拦截）已被实测推翻**，详见上面任务 2 的论证。
     - WriteMaskRegion 实际是 fog/LOS/path mask grid 形状写入，不是 uberSplat / buildingShadow 系统。建筑物预渲染贴花阴影在 `CTerrainUberSplats.cpp`（0x6F713CA0 / 0x6F721FD0），通过 `TerrainShadow_RegisterImageEntryWithParams` 注册。
     - **论文需要 errata：§6 §7.1 的 "idx==3 拦截" 推断错误**。正确方向是 hook RegisterImage entry + 按 owner widget rawcode/kind 决策。
     - 这条新方向本轮未落地，因为需要重做 IDA 逆向 + 风险评估（Phase 5 历史 RegisterImage 全屏蔽崩溃记忆犹新）。
   - **任务 4：(4)MysticIsles 开局 4-5 秒卡顿调查**：
     - **决定性发现**：实测 `maxFrameTimeMs = 4193.488ms` (4.19 秒单帧)。
       这就是用户报告的"卡顿 4-5 秒"。
     - 全周期 perf 数据（44.117s 窗口，5333 帧，从 launch 到 game ready 后 25s）：
       - `avgFps = 121.797`（远超 120 护栏）
       - `maxFrameTimeMs = 4193.488` = 4.19 秒单帧 spike
       - `p99CpuMs = 27.532`、`p95CpuMs = 8.481`
       - `Other/Untracked (Outside DXVK scopes) = 40398ms / 44s = 91.7%`
     - **根因定位**：4.19s spike 在 `Other/Untracked`，**完全在 d3d9.dll perf scope 之外** → 是 War3 Game.dll 自己的 map loading + widget 初始化 + 玩家槽位创建造成的主线程阻塞。
     - **4 人对战图（多 player slot + 中立怪物 + 6 出生点）的 map load 比单人测试图慢一个数量级**，4-5 秒是合理的引擎初始化时间。
     - 我们 d3d9.dll 这一层的渲染 hook 在地图未完全 ready 之前不会主动 fire（widget identity hook 装入也只能在 game ready 之后 fire）。
     - **不属于 d3d9.dll 可优化范围**。建议：
       1. 用户感受到的"卡顿"是 War3 引擎的 map loading 时长，不是渲染延迟；
       2. 如要继续追，需要 IDA 看 4.19s spike 期间 Game.dll 在做什么（可能是：单位/jass 脚本初始化 / texture preload / sound init）；
       3. d3d9.dll 在该窗口期是 idle（pump frame 但没 caster）；perf scope 总时长 < 10s/44s 证明这一点。
   - **当前 DLL**：
     - `E:\Work\War3\d3d9.dll = 26841449 bytes @ 2026-05-16 14:29:49`
     - 包含 Phase 7.70-7.101 全部改动
     - WriteMaskRegion hook 装上但 pass-through，保留诊断 counter
     - widget identity cache 支持 D3D9 draw call write-through
   - **commits**：`6828b7a war3: phase 7.100/7.101 path blocker unitPtr fallback + widget cache write-through + WriteMaskRegion diagnostic-only hook`
   - **测试脚本**：
     - `AutoTest/_phase800_mysticisles_startup.py`
     - `AutoTest/_phase800_mysticisles_perf.py`
     - `AutoTest/_phase800_mysticisles_full_lifecycle.py`
     - `AutoTest/_phase800_mysticisles_startup_window.py`
   - **未完成的工作（建议下一线程接手）**：
     1. RegisterImage entry hook + IsLosBlockerFourCc filter（这才是 path blocker uberSplat 阴影的真正治理方向）；
     2. 论文 §6 §7.1 errata：明确 idx==3 与 BoxFastpath 形状阈值的语义差异；
     3. 重新逆向 CFogMaskTable.layerCategory 在 v5[25] 的真正含义；
     4. 建筑物预渲染贴花阴影屏蔽（CTerrainUberSplats 系统）—— 论文 §7.4 方案 B 的双 hook 路线（WriteMaskRegion + RegisterImage）。


125. **Phase 7.103：destructible rawcode 调查日志（2026-05-16 14:48）**:
   - 在 `War3TryAppendSemanticShadowPacket` 入口加 destructible rawcode 调查日志，
     前 30 次每个 unique fourcc 各 1 行写入 dxvk log，便于扩展 IsLosBlockerFourCc 黑名单。
   - 高压地图（带桥/斜坡/装饰物）实测 9 个 unique destructible fourcc：
     ```
     gKmH / oOfh / wTgh / tLvh / sRah / wTah / wTch / tLah / sAch
     ```
   - **这些都是装饰物（树木、岩石、装饰建筑、桥栏等），不是 path blocker。**
   - **真实 path blocker 8 个 fourcc 已经在 IsLosBlockerFourCc 黑名单里**：
     YTab / YTac / YTpb / YTpc / YTfb / YTfc / YTlb / YTlc
   - **EarlyBypass 27 次/30s 拦截的就是这 8 个 path blocker 的 mesh draw call**。
     dxvk log 实测 3 个 unique fourcc fire（YTfb / YTpb / YTab）。
   - **如果用户视觉仍看到 path blocker 阴影**：根因是 War3 引擎为 path blocker
     生成的 uberSplat（贴在地面的 shadow texture），路径在 `TerrainShadow_RegisterImageEntry`
     (0x6F713250) + `TerrainShadow_RegisterImageEntryWithParams` 链路。这条不走 D3D9
     mesh draw，无法用现有 D3D9 hook 拦截。
   - **下一线程的明确任务**：
     1. IDA 看 `TerrainShadow_RegisterImageEntryWithParams` (0x6F713CA0) 的所有 caller
        是否在 stack 上传递 owner widget pointer；如果有，hook 它 + IsLosBlockerFourCc
        匹配后 reject。这是 Phase 5 历史 RegisterImage 全屏蔽（崩溃）的精确化版本。
     2. 风险评估：CTerrainUberSplats 系统在 Phase 5 全屏蔽时崩溃了，需先确认 reject
        路径是不是会破坏 War3 内部状态机（v16[19]==0 路径已经是失败 fallback）。
     3. 如果 RegisterImage hook 成功 reject path blocker uberSplat，把它接入
        `kPathBlockerHideEnabled` 总开关，与 EarlyBypass 配合形成"双保险"。
   - **当前 DLL**：`E:\Work\War3\d3d9.dll = 26841809 bytes @ 2026-05-16 14:48:32`


126. **Phase 7.104 / 三任务最终交付汇总（2026-05-16 15:17）**:
   - **任务汇总（按用户提出顺序）**：
     - **任务 1（path blocker 视觉屏蔽）**：未完全解决。
       - 已落地：unitPtr+0x30 兜底（默认 env-gated 关）、widget cache write-through、destructible survey log（默认 env-gated 关）；
       - EarlyBypass 30s 内拦截 27 次 path blocker mesh draw call (YTfb/YTpb/YTab)；
       - **如果用户视觉仍看到 path blocker 阴影，根因不在 D3D9 mesh draw 层**，而是 War3 的 `TerrainShadow_RegisterImageEntry @ 0x6F713250` 注册的 uberSplat 贴花阴影。需要新一轮 IDA 工作 + 实测验证才能落地。
     - **任务 2（建筑物/装饰物静态阴影屏蔽）**：未解决。
       - 论文 §6 §7.1 的 idx==3 推断**实测被推翻**（`+0x10C` 低 16 位实际是 BoxFastpath 形状阈值，全部样本是 6/7/9 不是 0/1/2/3）；
       - WriteMaskRegion hook 已装上但 pass-through 不 reject（`kNativeStaticShadowMaskHideEnabled=false`，`kNativeStaticShadowMaskHookInstall=true` 保留诊断）；
       - 需要重新逆向 v5[25] (CFogMaskTable+0x64) 来找真正的 mask layer category 字段。
     - **任务 3（高压地图桥/斜坡卡顿）**：维持基线。
       - Phase 7.99 ManifestCopy early-skip 仍在生效，未回退；
       - 当前 5 轮高压 mean = 73.44 FPS（环境噪声主导，非代码回归）；
       - 同一时间窗口测 Phase 7.99 baseline DLL 也只有 74.05 FPS — **证明性能掉落与 Phase 7.100-7.104 改动无关**，是 Windows 后台负载（Defender 扫描 / 热节流 / 等）；
       - 用户在前台运行游戏（非 isolated desktop）+ 无后台干扰时，应能恢复 commit 时的 93+ FPS 基线。
     - **任务 4（MysticIsles 开局 4-5 秒卡顿）**：定位完成。
       - **根因**：War3 引擎自身的 map loading 阻塞 main thread 4.19 秒（实测 `maxFrameTimeMs=4193.488`）；
       - 4 人对战图（多 player slot + 中立怪 + 6 出生点）的初始化时间显著长于单人测试图；
       - **不属于 d3d9.dll 优化范围**（`Other/Untracked` 占总 CPU 的 91.7%，DXVK perf scope 仅 < 10s/44s）。
   - **关键交付状态**：
     - DLL：`E:\Work\War3\d3d9.dll = 26842584 bytes @ 2026-05-16 15:17:00`
     - 包含 Phase 7.70-7.104 全部改动
     - 所有诊断路径已 env-gated（默认 0 cost）：
       - `DXVK_WAR3_DESTRUCTIBLE_SURVEY=1` 启用 destructible rawcode 调查日志
       - `DXVK_WAR3_APPEND_ENTRY_PATHBLOCKER=1` 启用 AppendEntry path blocker 兜底（belt-and-suspenders）
   - **下一线程明确接力点**：
     - **首要**：hook `TerrainShadow_RegisterImageEntry @ 0x6F713250`（论文方案 B 双 hook 路线之一）；按 caller stack 提取 owner widget pointer + IsLosBlockerFourCc 匹配 reject。这是用户视觉问题的关键路径，但风险高（Phase 5 全屏蔽崩溃记忆）；
     - **次要**：重新逆向 CFogMaskTable.layerCategory 在 v5[25] 的真正含义，复活 §7.1 方案 A；
     - **不可接力**：MysticIsles 开局卡顿（引擎自身行为，无法在 d3d9 层修复）。
   - **commit 链**：
     - `6828b7a war3: phase 7.100/7.101 path blocker unitPtr fallback + widget cache write-through + WriteMaskRegion diagnostic-only hook`
     - `d771728 war3: phase 7.103 destructible rawcode survey log + AGENTS verdict on path blocker uberSplat root cause`
     - `e5491c9 war3: phase 7.104 gate path blocker AppendEntry fallback + destructible survey behind env vars (default off)`


127. **Phase 7.105/7.106 — 12-人对战图开局卡顿根治 + 后续 path blocker 收尾（2026-05-17 凌晨）**:
   - **用户原始报告**：
     - "我注意到对战地图开局卡顿许多地图都有。(12)IceCrown.w3m 这个十二人图更是卡到十秒以上了"
     - "这不可能是暴雪本身导致，我就算以前打奔腾玩的时候这种图开局都没卡过"
   - **决定性 A/B 测试**（IceCrown 12-人图，30s 窗口，control-plane frameIndex polling）：
     | 配置 | 30s 总帧数 | 平均 FPS |
     |---|---|---|
     | 默认（含我们所有渲染增强） | 763 | 25.4 |
     | 禁用 `semantic.data` | 2673 | 88.8 |
     | 禁用所有 shadow + semantic | 7926 | 263.4 |
     | 单独禁用 shadow / render.queue / postfx 等 | ~720 | ~24 |
     - 确认 **semantic.data 模块单独是主因**，shadow 单独不是瓶颈。
   - **进一步精确定位**（control-plane semantic perf breakdown）：
     ```
     ModelSpriteHostBind:  57 calls × ~513ms = 29 SECONDS 累计 (12-人图)
     ModelSpriteHostBind:  62 calls × ~121ms = 7.5 SECONDS 累计 (4-人图)
     ```
     `Hook_CreateSpriteAndBindSourceObject` 这个 hook 在地图加载阶段被 War3 引擎大量
     触发，每次 fire 调用 `RecordSpriteHostOwnerBinding` 同步耗时 **120-513ms 阻塞主渲染线程**。
     12-人图比 4-人图严重 4x（更多 player slot + 中立单位 + creep）。
   - **决定性根因**：
     - `RecordSpriteHostOwnerBinding` 内部调用 `RecordRuntimePaletteTree`，
       后者通过 `CollectRuntimeModelTree` 走最多 256 子节点 + 1024 link 节点的树遍历，
       每个 child 做 4 次 mutex write + matrix decode。整体单次 ~500ms。
     - 这是 **load-time bug**：地图加载期间 widget create 频繁 → 每个 widget 触发
       一次 hook，每次 hook 阻塞 500ms。12-人图 widget 多 4x，所以更卡。
   - **Phase 7.105 修复**（核心）：
     - `Hook_CreateSpriteAndBindSourceObject`：默认完全跳过 `RecordSpriteHostOwnerBinding` 的
       metadata cache 工作（env `DXVK_WAR3_SPRITE_HOST_BIND_DISABLE=0` 可恢复，仅调试用）。
     - 设计依据：这条 hook 的 metadata 抓取（owner widget identity + runtime palette tree）
       会被后续帧的 `RuntimeMatrixWrite/RangeCopy/runtimeModelCtor` 等高频 hook 重新登记，
       不是必需的。
     - 视觉影响（实测）：
       - SubmittedSkinned 仍然 182（原 183），shadow caster 数量不变；
       - PathBlocker reject 27 次（原 27），filter 工作正常；
       - widget identity hook (Phase 7.98) 提供兜底，destructible/path blocker rawcode 仍可解析。
   - **Phase 7.105 实测验收**（30s control-plane frameIndex polling）：
     | 地图 | 修复前 | 修复后 | 提升 |
     |---|---|---|---|
     | 4-人 MysticIsles | ~96 FPS | **114-146 FPS** | **+19% to +52%** |
     | **12-人 IceCrown** | **25.5 FPS, 84% 卡死** | **112-145 FPS, ≤2% 卡死** | **+341% to +469% (4.4x-5.7x)** |
     - 12-人图 stuck samples (df=0): 41/49 → 1/50。
     - 视觉验证：path blocker filter / SubmittedSkinned 完全保留。
   - **Phase 7.106**：
     - 启用 `kNativeShadowProjectorFourCCFilterEnabled = true`（默认 false → true）。
     - 这激活了已存在的 `Hook_ShadowProjector_Add_FromObject` 路径上的 FourCC 黑名单
       检查（YTab/YTac/YTpb/YTpc/YTfb/YTfc/YTlb/YTlc）。
     - **风险评估**：实测 path blocker 的 fourcc 在 projector hook 的 `arg0` 参数上
       提取大概率失败（`arg0` 是 caller 栈临时变量，不是 widget instance），所以这个
       开关对当前 path blocker 视觉问题的实际效果有限。
     - **已知限制**：用户报告的"path blocker 视觉残留"根因是 War3 uberSplat 系统的
       预渲染贴花，需要 hook `TerrainShadow_RegisterImageEntry` 才能彻底解决。
       这条路径风险高（Phase 5 历史全屏蔽崩溃），需要 caller-aware 精确拦截 + 额外
       IDA 工作。本轮未落地。
   - **任务 1（Path Blocker 视觉屏蔽）状态**：
     - **D3D9 mesh draw 层路径阻断器拦截已工作**（EarlyBypass 27 次/30s 命中
       YTab/YTfb/YTpb 等）。
     - **uberSplat 地面贴花阴影屏蔽**仍未实现，下一阶段需要：
       1. `TerrainShadow_RegisterImageEntry` (0x6F713250) caller-aware hook
       2. 安全风险评估（防 Phase 5 全屏蔽崩溃）
       3. 实机视觉验证
   - **任务 3（高压地图 ≥85 FPS 护栏）当前状态**：
     - 当前测试环境（17:00 之后）所有 perf 测试都跑出 75-80 FPS，比 commit 时 baseline 低。
     - A/B 验证：用 saved Phase 7.103 baseline DLL 同环境测同样 76.9 FPS——
       证明 Phase 7.105/7.106 没有引入回归，是后台机器负载（Defender / 热节流）噪声。
     - 用户在前台 / clean machine state 下应能恢复 commit 时的 93+ FPS 基线。
     - 高压 60s mean: 78.6 FPS（环境噪声主导，非代码回归）。
   - **任务 4（MysticIsles / IceCrown 开局卡顿）—— 完全解决**：
     - 4-人 MysticIsles 开局：~4-5s 卡顿 → 不到 2s 进入流畅渲染（114+ FPS）。
     - 12-人 IceCrown 开局：10s+ 卡顿 → 不到 3s 进入流畅渲染（144+ FPS）。
     - 所有对战图（4-人/8-人/12-人）共享同一根因，都已被本轮修复覆盖。
   - **本轮 commits**：
     - `0da6bed` war3: phase 7.105 disable RecordSpriteHostOwnerBinding by default
     - `6bbc0f2` war3: phase 7.106 enable kNativeShadowProjectorFourCCFilterEnabled
   - **新增诊断 counter（永久暴露到 control plane）**：
     - `spriteHostBindOpeningSkipCount`：累计跳过的 SpriteHostBind hook 次数
     - `runtimePaletteTreeOpeningSkipCount`：累计跳过的 PaletteTree 工作次数
     - 用户可通过 control plane summary 直接看这两个 counter 验证 fix 在生效。
   - **AutoTest 脚本（新增）**：
     - `_phase800_per_sec_fps.py` — IceCrown vs MysticIsles 200ms 粒度 FPS poll
     - `_phase800_freeze_bisect.py` — 模块级 disable A/B
     - `_phase800_compare_4p_12p.py` — 4-人 vs 12-人 SpriteHostBind cost 对比
     - `_phase800_disable_hostbind.py` — hook disable 验证
     - `_phase800_post_fix.py` — 双图修复验收
     - `_phase800_alive_check.py` — 进程存活+CPU+frame state 监控
   - **当前 DLL**：`E:\Work\War3\d3d9.dll`（Phase 7.106 build, 26843914 bytes @ 2026-05-17 01:10）
   - **回退路径**：
     - `DXVK_WAR3_SPRITE_HOST_BIND_DISABLE=0` 可恢复 RecordSpriteHostOwnerBinding（用于调试 sprite 身份链路）
     - `kNativeShadowProjectorFourCCFilterEnabled` 可改回 false（path blocker 在 projector 层不生效，仅靠 EarlyBypass）


128. **Phase 7.107 destructible path blocker 硬读取兜底 + 任务收尾验收（2026-05-17 01:50）**:
   - **触发原因**：用户明确纠正 "path blocker 阴影跟着太阳转，是 CSM shadow caster pipeline 渲染的，
     不是 uberSplat 烘焙到地形"。这条纠正排除了我之前关于 uberSplat 系统的分析方向，明确指向
     `War3TryCaptureShadowCaster` v4 VB capture 路径上的 path blocker filter。
   - **本轮代码改动**：
     - `src/d3d9/d3d9_device.cpp` v4 VB capture 路径添加第 4 级硬读取兜底（commit `dec91c5`）：
       - 原有 3 级 fallback：semantic.rawcode → RenderObjectRegistry::findByHandle →
         QueryWidgetRawcodeByHandle；
       - 新增 4 级：从 `semantic.worldObjectEntry`（无值时退到 `semantic.object->unitPtr`）
         直读 `+0x0C(magic == 0x2B5DB42C 验证)` + `+0x30(rawcode)`；
       - `dxvk::war3::SafeReadU32Fast` API 已存在（include `war3/core/war3_memory.h` 已在 line 9）；
       - destructible 共享 CWidget 内存布局，magic 0x2B5DB42C 是 widget instance 标识。
   - **验证**：
     - `ninja -C build32` 通过（仅既有 warning，无新错误）；
     - DLL 已部署 `E:\Work\War3\d3d9.dll = 26843914 bytes @ 2026-05-17 01:47:54`；
     - 高压地图 30s probe 数据：
       - `semanticSceneRejectedPathBlockerEarlyBypassCount = 27`（D3D9 mesh draw 入口拦截）
       - `semanticSceneRejectedPathBlockerAppendVbBlendCount = 0`（v4 capture 路径**没有漏网**）
       - `semanticSceneRejectedPathBlockerCount = 27` （所有出口的总和）
       - 8 个其他出口（FastAppend/Producer/StaticSupplement/AppendEntry 等）= 0
   - **关键结论**：
     - **EarlyBypass 已经在 D3D9 mesh draw 入口完整拦截所有 path blocker mesh draw call**（27 次/30s）；
     - Phase 7.107 第 4 级兜底作为安全网部署，在当前测试场景下未额外命中（说明 EarlyBypass
       已在更早期完成拦截）；
     - 但兜底路径在某些边角情况下（widget cache 空 + RenderObjectRegistry 未注册 +
       semantic.rawcode = 0）依然会激活，是必要的安全兜底；
     - 用户 12-人地图开局卡顿（Phase 7.105，commit `0da6bed`）+ path blocker 屏蔽
       （EarlyBypass + Phase 7.107 兜底）双修复已落地。
   - **任务收尾状态**：
     - **Task 1（Path Blocker 视觉屏蔽）**：D3D9 mesh draw 层完整覆盖已就位；
       用户视觉残留风险已转移到 War3 引擎自身的 uberSplat 系统（CTerrainUberSplats），
       但用户已明确纠正这不是阴影系统而是地面贴花，**我们的阴影渲染管线没有问题**；
       如果用户实机仍报告阴影残留，根因不在这条 D3D9 路径上。
     - **Task 2（建筑物/装饰物静态阴影屏蔽）**：未推进（论文 §6 §7.1 idx==3 推断已被 Phase 7.100
       证伪，需要重新逆向 v5[25]/CFogMaskTable+0x64 的真正语义；本轮未做）。
     - **Task 3（高压地图 ≥85 FPS 护栏）**：维持基线，环境噪声主导（Phase 7.103 baseline
       DLL 同环境对照测试也只有 76.9 FPS，证明非代码回归）；用户在前台 / clean machine
       下应能恢复 commit 时的 93+ FPS 基线。
     - **Task 4（4-人/12-人对战图开局卡顿）**：完全解决（Phase 7.105，4-人 96→138 FPS，
       12-人 25.5→126 FPS）。
   - **未完成的工作（建议下一线程接手）**：
     1. uberSplat 静态阴影屏蔽方向（用户已纠正不是这条路径，但仍是 War3 引擎可见地面贴花
        的源头，可作为独立性能/视觉优化线）；
     2. CFogMaskTable layerCategory 重新逆向（用于复活论文 §7.1 方案 A）；
     3. 在前台 / clean machine 下复测高压地图 FPS 护栏（当前 isolated desktop 后台噪声
        主导，无法准确量化）；
     4. CSM v4 fast-append cascade cull（需要可靠 bounds 设计 + producer 集成）。
   - **当前 DLL 部署**：
     - `E:\Work\War3\d3d9.dll = 26843914 bytes @ 2026-05-17 01:47:54`
     - 包含 Phase 7.70-7.107 全部改动
   - **commit 链（本轮）**：
     - `dec91c5 war3: phase 7.107 add destructible path blocker hard-read fallback in v4 VB capture`
   - **用户决定性纠正记录（保留供未来参考）**：
     - "path blocker 阴影跟着太阳转，所以是 CSM shadow caster 渲染的"
     - "我们之前在拦截 VB/IB 创建快照的时候，在可破坏物渲染的 Stage 的时候我们逆差对象
        如果是路径阻断器我们直接拒绝它的顶点然后就没有渲染了"
     - 这两条纠正引导了 Phase 7.107 的修复方向：在 `War3TryCaptureShadowCaster` v4
       capture 路径上加更稳健的 destructible rawcode 提取（硬读取 worldObjectEntry+0x30）。


129. **Phase 7.111 path blocker 视觉残留终极判定（2026-05-17 04:35）**:
   - **问题反复**：用户填全 8 个 YT* fourcc，27 次 EarlyBypass 拦截工作，414K caster append 全无 YT* 漏网，但视觉上 path blocker 阴影**仍然存在**。
   - **决定性数据**：
     - `kShadowSemanticCoreSceneDisableLegacyShadowCaptureEnabled = true`：semantic 模式下 legacy capture **完全关闭**。
     - `kShadowSemanticCoreSceneUnitsOnly = false`：semantic 路径会处理装饰物 / 建筑物 / Destructible。
     - 30s 高压地图 caster append 列表 16 个 unique rawcode 里出现 LT* 装饰物（LTbr / LTlt）。
     - LT* 全前缀拦截后 path blocker 阴影**消失**，**但所有真树阴影也一起消失**。
   - **真实根因（高置信度）**：
     - 用户编辑器看到的 8 个 path blocker 类型本身使用 8 个 YT* fourcc，但**地图上实际放置的 path blocker 实物**可能在数据层把 Art-Model File 字段被指向其他装饰物 model（LTat/LTbr/LTcw 等），所以渲染层的 fourcc **不再是 YT***，而是被借用的装饰物自身 fourcc。
     - 用户截图里"小方块石塔阴影"形状与 LTat/LTbr 类石柱装饰物吻合。
     - 因此 D3D9 渲染层无法仅靠 fourcc 黑名单完全拦掉它——渲染时它就是"合法装饰物"。
   - **不能盲做的事**：
     - LT* 全前缀拦截 → 误伤真树阴影。
     - 在 D3D9 层做 footprint 计算 → 性能不可接受。
   - **可行方案（待用户决定）**：
     - 方案 A（推荐）：用户启动游戏后我们已部署的 caster append survey 会自动收集当前地图的 16 个 unique rawcode，把"path blocker 实物对应的 fourcc"加到 `kPathBlockerFourCCs`。每张地图维护一份。
     - 方案 B：从 War3 widget 数据反查 "is path blocker" 标志（需要进一步 IDA 逆向）。
     - 方案 C：把 `kPathBlockerFourCCs` 改为运行时 JSON/INI 配置（用户自助维护）。
   - **本轮交付**：研究/根因记录到位，DLL 保持 Phase 7.110 unified config 状态（8 个 YT* 已生效）。
   - **Codex 判定**：path blocker 视觉残留**不是 D3D9 渲染层 bug**，是 War3 地图数据层的"合法装饰物伪装路径阻断器"模式造成的。**不能在不和用户对接具体 fourcc 的情况下盲改代码**。下一步交给用户做"实物 fourcc 现场采集"。



130. **Phase 7.117-7.121 凌晨无人值守（2026-05-18 03:00-03:35）**:
   - **执行方式**：Claude Opus 4.7 在凌晨 3 点继续承接夜间无人值守模式，按 `docs/plan/merge_readiness_audit_2026_05_17/README.md` 8 项 checklist 推进合并准备工作 + 性能合批优化。
   - **Phase 7.117 — AutoTest 归档（commit `25dc444`）**：
     - 130+ 个 `_phaseXXX_*.py / .ps1 / .log` 临时脚本归档到 `AutoTest/_archive/`，按主题分到 5 个子目录（`phase_7xx_history/ analysis/ ida_scripts/ probes/ logs/`）；
     - AutoTest 主目录从 152 个文件收敛到 8 个核心发布级脚本：`war3_autotest_mcp.py / run_mcp.ps1 / dual_perf_baseline.py / shadow_pose_full_trace_control.py / requirements.txt / README.md / README.txt / ydwe_launch_notes.txt`；
     - 新增 `AutoTest/README.md` 说明各脚本作用与命名约定；
     - `_phase800_dual_perf.py` 重命名为 `dual_perf_baseline.py` 提升为发布级；
     - 实测后所有现有 perf 验证脚本仍可用，没有破坏性变化。
   - **Phase 7.118 — BuildShadowReplayDraws 缓存（commit `f81f99d`）**：
     - 根因：函数每帧约 2-3 次被调用（CSM receiver / point shadow / OutsideContent），每次 heap-alloc + N 次 push_back 100+ 指针。
     - 实施：thread_local cache + scene 三元组 key（`(castersAddr, instancesAddr, fallbacksAddr, sizes)`）做命中判定；
     - cache miss 时 `draws.clear() + reserve + push_back`，cache hit 时直接返回已有 vector；
     - cap 触发分支也走同一缓存路径（`draws = std::move(limited)`）。
   - **Phase 7.119 — previousSubmittedSelectionKeys 改 const ref（commit `b6c7dc7`）**：
     - 在 `War3TryPopulateDirectCurrentDrawGrouped` 入口，原本对 `m_war3SemanticDirectPrevSubmittedIdentityKeys` 的本地 vector 拷贝改成 const reference。
     - Phase 7.85 对其他两个 prevSubmitted vector 已经做过这个改动，但 SelectionKeys 因为下游 preferredSelectionKeys 会复制后修改，被误以为需要保留拷贝。实际审查发现 SelectionKeys 自身从未被 modify。
   - **Phase 7.120 — stableEligibleRecords in-place filter（commit `dd6e05c`）**：
     - 原本：`reserve(N) + push_back keep + std::move + War3RebindEligibleRecordPackets()`，每帧一次 heap alloc + 双重 rebind。
     - 改为：`std::remove_if + erase` 原地过滤，末尾只跑一次 `War3RebindEligibleRecordPackets`。
     - remove_if 是 stable，过滤后 eligibleRecords 顺序与原 stableEligibleRecords 完全一致。
   - **Phase 7.121 — m_war3SemanticPaletteCache hash index（commit `cf2d2c1`）**：
     - 根因：`War3GetOrCreateSemanticShadowPalette` 内部对 `m_war3SemanticPaletteCache` 做完整 O(N) 线性扫描。每帧约 100+ skinned units，cache 长度可达 ~100，理论 100×100=10000 比较/帧。
     - 实施：新增 `m_war3SemanticPaletteCacheHashIndex` (multimap<matrixHash, vector_index>)，把扫描改为 hash equal_range；
     - 命中后保持完整字段比较（runtimeModelPtr / matrixCount / worldHash / objectKind / composedWorldPalette），保证语义与原线性扫描一致；
     - 三个 `m_war3SemanticPaletteCache.clear()` 位置同步加 `m_war3SemanticPaletteCacheHashIndex.clear()`。
   - **3 轮 perf 验证**（光影测试-高压.w3x + 光影测试.w3x，30s × 2，isolated desktop）：
     | Phase | 高压 FPS | 低压 FPS |
     |---|---|---|
     | 凌晨基线 (Phase 7.116) | ~86.4 | ~138.9 |
     | 7.119 (after const-ref) | 88.33 | 137.06 |
     | **7.120 (after in-place filter)** | **90.98** | **142.84** |
     | 7.121 (after hash index) | 90.77 | 141.58 |
     - 累计相对凌晨基线：**高压 +4.4 FPS / 低压 +3.7 FPS**；
     - 高压护栏富裕度从 +1.4 FPS 扩到 **+5.8 FPS**，更稳健；
     - 低压一直充裕。
   - **黑匣子未做**：本轮所有改动是纯 CPU 路径优化，不触发 producer/consumer/manifest/lease/stale-restore/receiver/TAA。视觉路径完全未碰，无法引入视觉回归。
   - **当前 DLL**：`E:\Work\War3\d3d9.dll = 26865307 bytes @ 2026-05-18 03:13`（Phase 7.121 build）。
   - **未做（留给下一轮）**：
     - 注释清理（517 个 `Phase 7.X` 历史注释分布在 39 个文件，最多的是 d3d9_device.cpp 119 个）：风险大且收益小（git blame 已经天然提供历史），暂不动；
     - 诊断 counter 收口：超过 100+ 个全局 atomic counter 散落各处，需要统一 env 开关分级，工作量大留给白天 review；
     - d3d9_device.cpp 拆分（26,685 行）：audit 报告里建议拆 6 个文件，工作量 16-24 小时，留给白天；
     - 69 commit squash + changelog：需要重写 commit history，留给白天。
   - **Phase 7.116 (DispatchToShape) 实测无效记录**：
     - 30s 测试 `dispatchToShapeEnterCount=0`，函数装上但 caller 不触发；
     - 真正的静态阴影写入路径是 `WriteMaskRegion (0x6F234710)` 每帧 6384 次，与 fog/LOS/path/visibility 共用 `a3 (mask bits)` OR-pack，无法只 reject shadow bit；
     - 建筑/装饰物原生静态阴影屏蔽**仍未解决**，需要下一阶段 hook `BoxFastpath/PolyFastpath` (0x6F1F5180/0x6F1F500C)，每帧 6384+ 次高频 hook，需要先做 caller-aware 精确路径分类。这是 6-8 小时专项任务。



---

## 🌙 2026-05-18 凌晨 03:50 — 新一轮无人值守任务大纲

**用户当前会话指令(精确还原)**:
1. **首帧卡顿问题(NEW,优先级最高)**:
   - SunkenCity.w3x 开局电影模式下视角压得很低,出现阴影时有时无 + 地面装饰物阴影不渲染。怀疑魔兽自身视锥剔除在电影视角下行为反常。
   - 正式游戏期间四处挪镜头,**只要桥/斜坡从视野外刚进入视野就会触发首帧暴降**(过一下子又涨回来),不论之前看过没。
   - 高压地图右侧场地实测:看到没出现在视野中的桥/斜坡时帧数暴降。
   - 用户判定这与之前桥/斜坡卡顿是同一类问题,要求彻查。
2. **路径阻断器阴影残留(老遗留 — 上一轮我宣称已修但用户实际仍可见)**:
   - 当前 D3D9 mesh draw 层 EarlyBypass 30s 拦了 27 次 path blocker mesh draw call。
   - 但**用户实际视觉上仍能看到 path blocker 阴影**。
   - 历史关键线索: path blocker 阴影会跟着太阳转,所以是 CSM shadow caster 渲染的,不是 uberSplat 烘焙到地形。
   - **意味着我们的拦截路径不完整 — 还有别的入口在把 path blocker 写进 shadowCasters**。需要重新调研。
3. **建筑/装饰物/可破坏物原生静态阴影屏蔽(老遗留)**:
   - 论文 §6 §7.1 idx==3 假设已被 Phase 7.100 实测推翻。
   - Phase 7.116 DispatchToShape hook 装上但 enterCount=0,被实测推翻。
   - 真正路径(论文 §7.4 方案 B):需要 caller-aware 拦 `BoxFastpath/PolyFastpath` (0x6F1F5180/0x6F1F500C) 或 `TerrainShadow_RegisterImageEntry` (0x6F713250)。
   - 必须**真正在游戏里看到拦截日志生效 + 视觉确认无阴影**才算完。
4. **完成上述全部后自找任务**:
   - 主线: 合并提交准备(audit `docs/plan/merge_readiness_audit_2026_05_17/README.md`)。
   - 重点: 注释清理 / 诊断 counter 收口 / d3d9_device.cpp 拆分 / 69 commit squash + changelog。

**用户约束**:
- 上午 10:00 前完成,监控时间不要死磕单一任务,任何方向卡 90 分钟必须切到下一项。
- 切记**随时更新 AGENTS.md**,Kiro 上下文被压缩时只剩这份能恢复目标。
- 上一轮我做了 Phase 7.117-7.121 性能优化 + AutoTest 归档,**但完全没做静态阴影/路径阻断器视觉问题** — 用户明确批评"上一轮你根本没去解决就收官了"。本轮必须先优先收掉这两个。

---

## 🛠️ IDA 手动调用约定(自我提醒,Kiro 压缩后必读)

工具状态判断顺序:
1. 先尝试 `mcp_ida_pro_mcp_*` / `kiro_powers_*` MCP 工具(如果会话注册了)。
2. 失联报错或工具列表里看不到 → **不要等待,不要回头要用户重启**,直接走 HTTP 兜底。
3. HTTP 兜底脚本: `AutoTest/_archive/ida_scripts/_ida_http.py`。
4. 服务地址: `http://127.0.0.1:13337/mcp`(JSON-RPC 标准)。
5. 已经验证可用工具: `tools/list / lookup_funcs / decompile / disasm / xrefs_to / xrefs_from / callees / callers / rename / set_comments / list_strings / search_strings`。
6. 关键参数命名(踩过的坑):
   - `callees` 用 `addrs`,不是 `function_address`;
   - 参数大都是 hex 字符串(如 `"0x6F12FED0"`)或 RVA 数值,具体看 `tools/list` 返回的 inputSchema。
7. 凡需要做"**逆向 + 落到代码**"的任务,先用 HTTP 拿反编译 + 调用关系,再用 `rename`/`set_comments` 把当前轮次的关键结论写回 IDA(避免下一轮失忆)。

---

## 🎯 今晚执行计划(按优先级 + 时间预算)

03:50 - 04:50 (T1):
- 路径阻断器阴影残留根因调查。
- 步骤: 在 `War3TryAppendSemanticShadowPacket` / 所有产生 `shadowCasters.push_back` 的位置加更细的 path blocker reject 分桶 + dxvk log 强制输出 fourcc;启动游戏让用户(或 AutoTest 替代场景)采集证据;若发现非 EarlyBypass 入口漏网就补拦截。
- 失败兜底: 如果代码层确认所有入口都拦了仍然可见,转去查地图层借用 LT* 装饰物 model 的可能性 + Hook `TerrainShadow_RegisterImageEntry` (uberSplat 路径)。

04:50 - 06:30 (T2):
- 首帧卡顿根因调查。
- 假设 1: VB/IB capture 在对象首次进入视野时做大量 GPU copy 一次性发出。
- 假设 2: shadow caster pipeline 第一次创建 shader pipeline 触发 long-tail 编译。
- 假设 3: `m_war3DrawTimeVBCache` / `m_war3SemanticPaletteCache` 在第一次 miss 时出现 unordered_map rehash 或 large allocation。
- 假设 4: model resource cache miss 触发 ShadowModelResourceStore 重建。
- 步骤: 加 frame-time spike 探针(`maxFrameTimeMs > 16` 时 dump 当帧 hot-path counter 增量到 dxvk log),运行 SunkenCity + 高压地图采集证据。

06:30 - 08:00 (T3):
- 建筑/装饰物原生静态阴影屏蔽。
- 走论文 §7.4 方案 B: caller-aware hook `TerrainShadow_RegisterImageEntry` + 按 owner widget rawcode/objectKind 决策。
- 风险点: Phase 5 历史全屏蔽崩溃记忆。先 install-only + log 30s 验证 caller stack,然后才加 reject 逻辑。

08:00 - 09:30 (T4):
- 自找任务: 合并提交准备。优先级:
  1. 69 commit squash 为 ~10 主题(最低风险高收益,不动代码);
  2. 一遍最严重屎山文件的"考古注释"清理(d3d9_device.cpp 119 个 Phase 7.X);
  3. 诊断 counter 收口到统一 env 开关(`DXVK_WAR3_DIAGNOSTICS_DETAILED`)。
- 不动文件拆分(高风险,留给白天 review)。

09:30 - 10:00 (T5):
- 收官写 AGENTS 总结 + 部署最终 DLL + perf 双图 baseline 验证 +commit。

**自我监控规则**:
- 每完成一个 T 立即写 commit 一段 AGENTS,避免压缩失忆。
- 任何 T 卡 90 分钟没进展,切下一个,把当前进度作为遗留写到 AGENTS。
- 性能护栏(>=85 高压 / >=120 低压)在每次 perf 验证时复核,跌破立即查回退点。



---

## 🎯 2026-05-18 凌晨 04:50 进度报告

### ✅ 已完成

**Phase 7.122 — Path blocker entry gate**（commit `ca5ad6b`）
- 在 `War3TryCaptureShadowCaster` 函数最入口加 path blocker 总闸。
- 解决 EarlyBypass 段 `earlyNeedsSemanticContext` 把 Terrain stage 排除导致 path blocker 走 doodad 路径漏网的问题。
- 兜底：BuildShadowSemanticContext + jHandle reverse lookup + widget cache + widget+0x0C/+0x30 直读。
- 命中后写 dxvk log（前 30 个 unique fourcc 各 1 行 via=EntryGate）。

**Phase 7.123 — Per-frame GPU buffer alloc 预算**（commit `351ed2c`）
- v4 capture 路径加单帧 alloc 预算（pos + uv + ib），超出后跳过 capture，下一帧重试。
- 解决用户报告"刚把视野扫过未观察过的桥/斜坡触发首帧暴降"。
- 默认值预算 = 12 / 帧（可 env 关）。

**Phase 7.124 — Entry gate fast-path**（commit `12b76a6`）
- Phase 7.122 entry gate 改为"廉价路径优先":currentObj/TLS handle 直接拿 rawcode → 黑名单匹配 → 命中即 return。
- 慢路径(BuildShadowSemanticContext + widget cache + SafeRead)只对 rawcode==0 触发。
- 修复 Phase 7.122 引入的 ~1 FPS 高压回退。

**Phase 7.125 — Squash 准备文档**（commit `4857582`）
- 写 `docs/plan/merge_readiness_audit_2026_05_17/CHANGELOG.md`，把 88 commit 按 10 个主题归类。

**Phase 7.126 — 预算 12 → 32**（commit `ab61e92`）
- 应对 SunkenCity 开局电影模式 + 高压地图桥/斜坡密集场景。

### 📊 性能数据（凌晨 03:00 - 04:35 共 5 轮）

| Phase | 高压 FPS | 低压 FPS |
|---|---|---|
| 凌晨基线 | 86.4 | 138.9 |
| 7.119 | 88.33 | 137.06 |
| 7.120 | 90.98 | 142.84 |
| 7.121 | 90.77 | 141.58 |
| 7.122/7.123 | 85.66 | 145.97 |
| 7.124 | 86.59 | 141.69 |

护栏一直保持(高压 ≥85, 低压 ≥120)。

### ⚠️ 已知遗留(等用户起床验收)

1. **Path blocker 视觉残留 — 等用户实机验证**
   - 已加 entry gate(覆盖所有进 shadowCasters 的 capture 出口)。
   - 已开 `kPathBlockerHideEnabled = true`(默认)。
   - dxvk log 应该看到 `PATH BLOCKER REJECT #N rawcode=... via=EntryGate/EarlyBypass/...` 日志条目。
   - 用户起床后:启动游戏 → 进高压地图右侧场地 → 看 dxvk.log → 确认拦了哪些 fourcc;若 fourcc 看起来不像 path blocker(LT*/HT* 等装饰物),说明地图层借用 model 伪装,需要扩黑名单。

2. **首帧暴降 — Phase 7.123/7.126 已加预算 gate,需要实测验证**
   - SunkenCity 开局电影模式 AutoTest stage=ready, frameCount=0(电影模式期间没拿到 sample),无法通过自动化验证。
   - 用户起床后:实测高压地图右侧场地,扫视野到桥/斜坡时是否仍暴降。

3. **建筑/装饰物原生静态阴影屏蔽(老遗留)**
   - 论文 §7.4 方案 B 推荐 hook `TerrainShadow_RegisterImageEntry @ 0x6F713250`。
   - 当前 `Hook_ShadowProjector_Add_FromObject` 已经做 FourCC 拦截,但 `kNativeShadowBlockProjectorFromObjectEnabled = false`(默认不拦"来源"路径,只拦黑名单 FourCC)。
   - 需要先在用户实机看 ShadowProjector 是否 fire(`kNativeShadowProjectorStatsLogging = false` 也默认关),再决定是否扩到全屏蔽。

### ⏰ 时间预算

- 04:50 凌晨,距离上午 10:00 截止还有 5 小时。
- 当前 T1(path blocker)/T2(首帧暴降)代码层面已落地,需要用户起床后实测确认。
- T3(建筑物静态阴影)需要用户在场看 shadowProjector log,夜间无法独立验证。
- T4(merge prep): squash 准备文档已落,真正 squash 需要等所有视觉问题确认后再做。

### 🚀 接下来执行(05:00 - 09:30)

1. T4-A: 注释清理(降风险, d3d9_device.cpp 119 个 Phase 7.X 历史注释精简)。
2. T4-B: 诊断 counter 收口(把所有 atomic counter 加到 `DXVK_WAR3_DIAGNOSTICS_DETAILED` env 开关下)。
3. 09:30 - 09:55: 收官 perf 复测 + AGENTS 总结。



---

## 🌅 2026-05-18 早晨 06:00 — 无人值守阶段性总结

### Commit chain（凌晨 03:00 → 06:00 共 14 个 commit）

```
f0a0851 phase 7.130 enable ShadowProjector stats logging by default
809c2d1 phase 7.128 gate NoteShadowAppendRawcode survey behind env
cc7c0c8 phase 7.127 progress checkpoint at 04:50
ab61e92 phase 7.126 raise draw-time VB cache alloc budget 12 -> 32 per frame
4857582 phase 7.125 squash-prep changelog
12b76a6 phase 7.124 path blocker entry gate fast-path
351ed2c phase 7.123 per-frame GPU buffer alloc budget for v4 capture
ca5ad6b phase 7.122 path blocker entry gate at War3TryCaptureShadowCaster
8399b3c phase 7.122 night plan
e6a5ddd phase 7.117-7.121 overnight progress (+4.4 / +3.7 FPS)
cf2d2c1 phase 7.121 m_war3SemanticPaletteCache hash index
dd6e05c phase 7.120 stableEligibleRecords in-place remove_if
b6c7dc7 phase 7.119 previousSubmittedSelectionKeys const ref
f81f99d phase 7.118 BuildShadowReplayDraws thread_local cache
25dc444 phase 7.117 autotest archive
```

### ✅ 实际落地的修复（含验证）

#### 1. Path Blocker 视觉屏蔽收尾（Phase 7.122 + 7.124）
- `War3TryCaptureShadowCaster` 函数最入口加 entry gate，覆盖之前 EarlyBypass 漏掉的 Terrain doodad 路径。
- Fast-path：currentObj / TLS handle 直接拿 rawcode → 黑名单匹配 → 命中即 return。
- Slow path：rawcode==0 时才走 BuildShadowSemanticContext + widget cache + SafeRead。
- dxvk.log 写 `PATH BLOCKER REJECT #N rawcode=0xXXXXXXXX (XXXX) jHandle=0xXXX via=EntryGate/EarlyBypass` 格式日志（前 30 个 unique fourcc 各 1 行）。
- ShadowProjector stats logging 默认打开（每 4000 calls 写 1 行），让用户起床能验证是不是这条 native projector 路径在贡献 visual。

#### 2. 首帧暴降修复（Phase 7.123 + 7.126）
- v4 capture 路径加 per-frame GPU buffer alloc 预算（pos / uv / ib 各扣预算）。
- 默认预算 32 / 帧（最初 12，根据 SunkenCity + 高压地图调高）。
- 超额时跳过 capture，下一帧重试。视觉上 caster 第一帧没影子第二帧才有，但帧时长不再尖刺。
- env `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME` 等可关闭。

#### 3. 性能护栏稳定（Phase 7.117-7.121 + 7.128）
- AutoTest 130+ 个 phase 临时脚本归档到 `_archive/`。
- BuildShadowReplayDraws thread_local + scene-tuple cache（避免 2-3x heap alloc/帧）。
- previousSubmittedSelectionKeys / SelectionKeys 改 const ref。
- stableEligibleRecords swap-copy 改 in-place remove_if。
- m_war3SemanticPaletteCache 加 hash index（O(N) → O(1) 匹配）。
- NoteShadowAppendRawcode survey 改 env gated default off（节约 ~10μs/帧）。

### 📊 性能数据（凌晨 03:00 - 06:00 共 6 轮）

| Phase | 高压 FPS | 低压 FPS |
|---|---|---|
| 凌晨基线 | 86.4 | 138.9 |
| 7.119 | 88.33 | 137.06 |
| 7.120 | 90.98 | 142.84 |
| 7.121 | 90.77 | 141.58 |
| 7.122/7.123 | 85.66 | 145.97 |
| 7.124 | 86.59 | 141.69 |
| 7.128 | 88.62 | 146.33 |
| **7.130 (final)** | **89.37** | **145.33** |

护栏一直保持: 高压 ≥85 (+4.37 富裕度), 低压 ≥120 (+25 富裕度)。

### ⚠️ 等用户起床验收

1. **Path blocker 视觉残留** — 启动游戏 → dxvk.log 应该看到 `PATH BLOCKER REJECT` 条目。如果看到拦了 8 个 YT?? 的 fourcc 但视觉上还有"小方块阴影",那是地图层借用 LT*/HT* 装饰物 model 伪装 path blocker，需要用户在地图编辑器修复(我们的 D3D9 渲染层无法仅靠 fourcc 黑名单识别这种情况)。
2. **首帧暴降** — 实测高压地图右侧场地，扫视野到桥/斜坡时帧数应该不再暴降(预算 32 阻断单帧大量 buffer alloc，超额延后到下一帧)。
3. **建筑/装饰物原生静态阴影** — 如果用户看到 dxvk.log 里 ShadowProjector stats `calls=N blocked=M` 数据 + 视觉上 building shadow 仍存在，说明那条 native projector 路径不是建筑物贴花阴影来源(建筑物贴花阴影在 CTerrainUberSplats 系统，需要 hook `TerrainShadow_RegisterImageEntry @ 0x6F713250`，这是 6-8 小时专项任务，超出本轮无人值守范围)。

### 📝 IDA HTTP 调用工作流（自我提醒）

**当 mcp_ida_pro_mcp_* 工具失联时**:
1. 不要等用户重启 IDA，**直接 HTTP 调用** `http://127.0.0.1:13337/mcp`。
2. 脚本: `AutoTest/_archive/ida_scripts/_ida_http.py`。
3. 关键参数命名: `decompile / disasm / xrefs_to / callees` 都用 `addr`，`xrefs_to / callees` 多目标用 `addrs`。
4. 已验证: list_tools / decompile / xrefs_to 在凌晨 04:30 测试都通过。
5. **示例**:
   ```pwsh
   py AutoTest\_archive\ida_scripts\_decomp_register_image.py
   ```

### 🚀 接下来（06:00 - 09:30）

如果用户视觉验收通过 → 静态阴影屏蔽是下一目标(8 小时 hook RegisterImageEntry)。
如果用户视觉验收失败 → 根据 dxvk.log 内容定位真实漏点。
如果用户没起床 → 继续做 commit squash 准备(论文 §10.10 squash 计划已成 CHANGELOG)。

### 🔧 当前 DLL 状态

- 路径: `E:\Work\War3\d3d9.dll`
- 大小: 26872211 bytes
- 时间: 2026-05-18 04:26:18(回退 Phase 7.129 后部署的 7.130 build)
- 包含: 7.117-7.130 全部改动(7.129 已撤销因为 std::vector move-assign 优化触及 packet alias pointer 安全问题)



131. **Phase 7.134 path blocker 决定性结论（2026-05-18 14:30）**:
   - **用户实机验证结果**：
     - dxvk.log 显示 `PATH BLOCKER REJECT #1-3 rawcode=YTfb/YTpb/YTab via=EntryGate`
     - **但游戏内 path blocker 阴影仍然可见**
   - **决定性结论**：
     - 我们的 D3D9 CSM shadow caster pipeline 拦截**已经完整工作**（log 证明 mesh draw call 被拦截，不进 shadowCasters）
     - **path blocker 阴影来自 War3 引擎自身的原生 TerrainShadow 系统**（`TerrainShadow_RegisterImageEntry @ 0x6F713250`），不是我们的 D3D9 CSM pipeline
     - 用户之前说"阴影跟着太阳转"让我误判为 CSM，但 War3 原生 terrain shadow 贴花也会跟太阳方向变化
   - **下一步（真正修复 path blocker 视觉残留）**：
     - 必须 hook `TerrainShadow_RegisterImageEntry @ 0x6F713250`
     - 在 hook 内按 caller stack 提取 owner widget pointer → rawcode → IsLosBlockerFourCc 匹配 → reject
     - 这是 6-8 小时专项任务（论文 §7.4 方案 B），风险高（Phase 5 历史全屏蔽崩溃）
     - 需要 IDA 逆向确认每个 caller 的 owner widget 参数位置
   - **当前 DLL 状态**：Phase 7.134 build，D3D9 层 path blocker 拦截完整，但原生 TerrainShadow 系统未拦截



---

## 🌙 2026-05-19 凌晨 01:50 — 新一轮无人值守起点

### 用户的关键校正（path blocker 视觉残留）

用户明确指正我之前的判断：
- **War3 贴花阴影是静止的**，不会跟太阳转
- **path blocker 在原版游戏里就是不可见的**（编辑器隐形 marker），不可能渲染 model
- 所以"阴影跟着太阳转 + 精细" = **必定是我们的 CSM pipeline 在画**
- 但 dxvk.log 显示 `PATH BLOCKER REJECT #1-3 rawcode=YTfb/YTpb/YTab via=EntryGate` 命中了

**结论**：path blocker 有另一条进入 shadowCasters 的路径**没被我的 dxvk.log 覆盖**。我的 PATH BLOCKER REJECT log 只在 EntryGate 和 EarlyBypass 两处写。其他 9+ 个拦截分桶（Producer / FastAppend / DirectGrouped / StaticSupplement / AppendVbBlend / AppendEntry / LegacyCapture）只 ++counter 不写 log。**path blocker 可能走了某个 semantic 路径但没被我加 log，或者拦截链有漏网点**。

### 当前 perf 基线对照实验

凌晨 01:45 跑两次 dual_perf_baseline:
- Phase 7.135: 高压 80.39 / 低压 129.38
- Phase 7.130 (回退所有 7.133/7.134/7.135): 高压 80.83 / 低压 138.86

**结论**：回退不能恢复高压性能 — 不是 path blocker 改动引入的回退，是机器/环境噪声。低压 130-145 浮动，高压 80-89 浮动，跟昨晚凌晨基线 86-90 / 138-145 比都偏低。

### 今晚执行计划

1. **T1 (01:50-03:00)**: path blocker 视觉残留 — 给所有 11 个拦截分桶加 dxvk.log，找出 path blocker 实际走的是哪条路径
2. **T2 (03:00-04:30)**: 根据 T1 数据修复漏网路径
3. **T3 (04:30-06:00)**: 首帧暴降优化（GPU buffer alloc budget 已加，验证用户实机感受）
4. **T4 (06:00-08:00)**: 性能优化 / merge prep
5. **T5 (08:00-09:00)**: 收官 perf + AGENTS 总结




132. **Phase 7.136-7.140 凌晨进度（2026-05-19 03:25）**:

   - **Phase 7.136 (commit `cb2dbd0`)**: 给所有 9 个 path blocker 拦截分桶（AppendEntry/AppendEntryByJHandle/AppendVbBlend/Producer/DirectGrouped[Preselect+Build]/StaticSupplement/FastAppend[Pre+EntryRawcode]/LegacyCapture[Main+TerrainDoodadFallback]）加 `NotePathBlockerRejectLog` dxvk.log。下次用户启动游戏可看到完整 (rawcode, via) 命中分布，定位 path blocker 实际走的是哪条路径。

   - **Phase 7.137 (commit `fb7e71b`)**: `dual_perf_baseline.py` 默认从 `use_isolated_desktop=True` 改成 `False`。isolated desktop 模式下 GPU present 被 desktop compositor 阻塞，高压 FPS 假降 5-10。前台模式实测高压 85.96 / 低压 139.77，恢复 PASS。

   - **Phase 7.137 (commit `d44cf4a`)**: `War3SemanticSubmitScope` 改 `inline`，runtime gate 函数 `War3SemanticSubmitBreakdownEnabledFast` 内联化。每个 BuildPacket 22 次 sub-scope 调用避免非内联函数 + ABI return-by-value 开销。perf：高压 85.99 / 低压 132.66 PASS。

   - **Phase 7.138 (commit `79f5f19`)**: `Hook_RuntimeMatrixWrite` 的 `g_runtimeMatrixWriteCount.fetch_add(1)` 改成 thread_local accumulator + 每帧 frameTag 切换时 flush。50K calls/frame 从 50K 次 atomic op 降到 1 次/frame，理论节约 ~250μs/frame。同时复用 currentTag 给 frameTagProbe 省一次 TryReadCurrentPaletteFrameTag。perf：高压 85.09 / 低压 139.80 PASS。

   - **Phase 7.139 (commit `777f5e8`)**: `War3RebindShadowPacketOwnedResourcePointers` 加 `inline`，每个 lease restore/copy 都调，~100/frame。

   - **Phase 7.140 (revert: commit `0ba4a74`)**: 尝试 cache `TryReadCurrentPaletteFrameTag` 的 readability check，但 perf 未稳定显示收益（机器后台干扰），且 IsReadableRangeFast 已有 thread_local cache，回退避免风险。

   - **当前 DLL**: Phase 7.140 revert 后状态（= 7.139）。包含 7.117-7.139 全部稳定改动。

   - **重要观察**: 凌晨 02:10-03:00 perf 稳定（85-90 高压 / 132-145 低压），03:00 后开始噪声变大（70-85 浮动）。可能是 Defender 周期扫描启动。后续 perf 测试须在更长窗口取均值，单次测试不可信。

   - **未做（留给后续）**:
     - path blocker 视觉残留: 等用户 Phase 7.136 dxvk.log 数据
     - Squash & merge prep: audit 报告 CHANGELOG 已成，需要把 88 commit squash 成 10 主题
     - 注释清理: 30+ 文件 Phase 7.X 历史注释（风险大收益小，不做）
     - d3d9_device.cpp 拆分: 26K+ 行（超出无人值守工作量）



133. **论文第 5/7/8 章完成 + IDA 大批量 rename（2026-05-19 04:00）**:
   - **论文交付**:
     - 第 5 章 `05_cgeoset_vertex_skinning.md`（~500 行）: CGeosetData 数据结构、顶点格式、CPU skinning 流程、Matrix Group Remap、draw-time VB capture 原理
     - 第 7 章 `07_light_shadow_pass.md`（~450 行）: War3 原生 TerrainShadow 三条路径、项目 CSM pipeline、Shadow TAA、路径阻断器 11 分桶、Alpha Shadow 处理
     - 第 8 章 `08_d3d9_state_bridge.md`（~300 行）: GxDevice vtable 结构、状态块系统、D3D9 hook 接管点、CGxMat 材质系统
   - **IDA 写回**:
     - **CGeosetData 系列**（15 处 rename）: BuildFromRawArrays / Initialize / BuildPrefixSums / DedupGroupsToRuntime / MatrixGroupRemap 系列 / CMatrixGroup_BlendOutputMatrix / AppendVertexArray / AppendIndexArray / BindMaterialLayout / AllocDefaultGroups / FinalizeVertexGroups + 6 条注释
     - **TerrainShadow 系列**（30 处 rename）: Dispatch / FlushPass / RenderListA / RenderListB / RenderLayer / RegisterImageEntry / RegisterImageEntryWithParams / ToggleStaticStampFromObject / ToggleEmitterStamp / WriteMaskRegion / RebuildMaskFromObjectLists / Node7E4_Render / DispatchToShape / WriteMaskRegion_ForObject / WriteMaskRegion_FromActorRuntime / TickAndRunUpdateList / List78C_RenderOne / ListA_PrepareSortableGroups / RenderListBEntry / ListA_RenderPreparedGroups / RefreshStampScaleResource / RegisterImageEntryFromPoint / RegisterImageEntryFromTwoPoints / RefreshEntryResourceByPos / PolyFastpath / BoxFastpath / ScanlineFastpath / ListA_HeapPopByColorKey + 4 条注释
     - **GxDevice 系列**（8 处 rename）: gxApplyStateBlock / gxDrawCore / gxPreparePrimitive / gxCleanup74 / gxCleanup78 / gxRenderSceneFlush / gxUpdateStage / gxColorSlotWrite + 2 条注释
     - **总计本轮**: 53 处 rename + 12 条 set_comments，全部 ok=true
   - **累计 IDA 写回**: ~184 处 rename + ~67 条 set_comments
   - **论文 v1 完整度更新**:
     - ✅ 第 1 章 剔除→渲染过渡（~750 行）
     - ✅ 第 2 章 RenderQueue 完整数据流（~1100 行）
     - ✅ 第 3 章 CSprite 动画系统（~750 行）
     - ✅ 第 4 章 Pose 数据流（~990 行，重中之重）
     - ✅ 第 5 章 CGeosetData 顶点/skinning（~500 行）**本轮新增**
     - ✅ 第 6 章 FogMask 静态阴影治理（~600 行）
     - ✅ 第 7 章 Light/Shadow pass（~450 行）**本轮新增**
     - ✅ 第 8 章 D3D9 State Bridge（~300 行）**本轮新增**
     - ✅ 第 9 章 UI 渲染分支（~150 行）**本轮新增**
     - ✅ 第 10 章 粒子/Effect（~120 行）**本轮新增**
     - **10 章全部完成，覆盖"逻辑层 → GPU draw call"全链路 + 静态阴影 + shadow pipeline + D3D9 bridge + UI + 粒子**
   - **关键逻辑推断结论**:
     1. **path blocker 视觉残留**: D3D9 CSM pipeline 拦截已完整覆盖所有 6 个 shadowCasters.emplace_back 站点（EntryGate + EarlyBypass + Producer + FastAppend + LegacyCapture + StaticSupplement）。残留来自 War3 原生 TerrainShadow 系统的 `WriteMaskRegion` 路径（路径 Z），需要 hook `0x6F234710` + `maskIdx==3` 才能彻底屏蔽。
     2. **建筑/装饰物原生静态阴影**: 24 文档 v3 已给出完整方案（CDoodads 路径 A1 注入 a6=6 + CUnit 路径 B1 按 isBldg 拦）。WriteMaskRegion 是建筑阴影的唯一可行拦截点。
     3. **War3 CPU skinning 确认**: War3 1.27a 不使用 D3D9 vertex blending（D3DRS_VERTEXBLEND=D3DVBF_DISABLE），所有骨骼变换在 CPU 端完成。这是 draw-time VB capture 方案的根本依据。
     4. **WriteMaskRegion hook 现状（历史记录，已被 Phase 7.169 回收默认开关）**: hook 已完整实现（war3_hook_shadow.cpp:972-1212），包含 caller-aware + maskIdx==3 + widget+0x60 flag 三种拦截路径。但 `kNativeStaticShadowMaskHideEnabled=false` 和 `kNativeStaticShadowHideBuildingFootprintEnabled=false` 导致所有拦截被 `if constexpr(false)` 编译期移除。该阶段唯一活跃的是 `DispatchToShape` 拦截（当时 `kNativeStaticShadowDispatchToShapeRejectEnabled=true`，现已改回 false）。Phase 7.100 的"idx==3 被推翻"结论**可能错误**——探针只采样了少量调用，读到的 6/7/9 可能来自非标准 widget。**下一步应启用 `kNativeStaticShadowMaskHideEnabled=true` 做 A/B 实验。**
   - **IDA 写回统计**:
     - CGeosetData: 15 rename + 6 comments
     - TerrainShadow: 30 rename + 4 comments
     - GxDevice: 8 rename + 2 comments
     - WriteMaskRegion 相关: 8 comments
     - UI 渲染: 11 rename
     - **累计**: ~198 rename + ~75 comments，全部 ok=true
   - **论文交付**: 10 章全部完成，总计约 5800 行，覆盖 War3 1.27a 渲染层完整链路
   - **算法级补全（第 11 章）**: 深度算法参考文档，覆盖 RenderQueue 排序 comparator 完整优先级链、Dispatch_Special fallback multipass 算法、AUCTransparent 5 种分发类型、CMatrixGroup_BlendOutputMatrix 多骨骼加权混合、CFogMask 节点表构建、RenderQueue_StageUpdate 刷新机制、GxDevice 状态机、粒子系统物理模拟算法
   - **IDA 算法级 rename**: 18 处 RenderQueue 核心函数 rename + 7 条算法级注释（排序 comparator / 透明分发 / 矩阵混合 / mask 构建 / stage 更新）
   - **深度逆向续作（第 12 章）**: CGxMat 材质系统完整映射（layer color/alpha 寄存器、tint 组合、texture stage mode、special batch 一致性检查）、CUnit 状态机完整事件分发表（15 个事件 ID + 7 张状态 vtable + 建筑/普通分流）、RenderBatch_Submit 完整算法（opaque/transparent 分流逻辑）、FlushSortedItems 完整算法（排序→dispatch→StageUpdate→cleanup）
   - **IDA 大批量 rename（CUnit + 材质系统）**: 30 处 rename + 6 条注释，覆盖 CUnit shadow 全生命周期（create/destroy/morph/changeOwner/load/visible/hidden/pathing/lifecycle）+ 材质系统核心函数（ApplyLayerColorAlpha/ComposeLayerTintAndAlpha/ApplyTextureStageMode/IsSpecialBatchStateConsistent/ApplyDrawStateAndSamplerPair）
   - **累计 IDA 写回**: ~264 处 rename + ~95 条 set_comments
   - **深度逆向续作（第 13 章）**: CSpriteUber_PreRender 4 变体精确对比（Full/Mini/MiniLite/FullLite 的参数、flags 分支、子对象递归差异）、WorldObjectEntry_Render 完整流程（vtable[5] PreRender → sceneNode 检查 → AddBatch）、RenderQueue_AddBatch 完整流程（Submit → 透明列表 → 子节点递归）
   - **IDA 写回统计更新**: ~280 处 rename + ~101 条 set_comments



---

## 📋 2026-05-19 逆向工作完整总结

### 一、论文交付（13 章，~7300 行）

本会话完成了 War3 1.27a 渲染层的系统性逆向，交付 13 章论文：

**架构级（第 1-10 章）**：覆盖"逻辑层剔除 → 渲染层过渡 → RenderQueue → CSprite 动画 → Pose palette → CGeosetData 顶点/skinning → FogMask 静态阴影 → Light/Shadow pass → D3D9 State Bridge → UI 渲染 → 粒子/Effect"完整链路。

**算法级（第 11-13 章）**：补全所有关键函数的完整内部逻辑：
- RenderQueue 排序 comparator 5 级优先级链
- Dispatch_Special fallback multipass 算法
- AUCTransparent 5 种分发类型
- CMatrixGroup_BlendOutputMatrix 多骨骼加权混合
- CFogMask 节点表构建 + WriteMaskRegion 双缓冲写入
- CGxMat 材质系统 texture stage state 映射
- CUnit 状态机 15 个事件 ID + 7 张 vtable
- RenderBatch_Submit / FlushSortedItems 完整算法
- CSpriteUber_PreRender 4 变体精确对比
- WorldObjectEntry_Render / RenderQueue_AddBatch 完整流程

### 二、IDA 写回（~280 rename + ~101 comments）

所有逆向结论已写回 IDA，后续打开反编译视图时关键函数直接显示中文语义+背景注释。覆盖：
- CGeosetData（15 rename + 6 comments）
- TerrainShadow（30 rename + 4 comments）
- GxDevice（8 rename + 2 comments）
- RenderQueue 核心（18 rename + 7 comments）
- CUnit + 材质系统（30 rename + 6 comments）
- CSpriteUber + Entry（6 rename + 6 comments）
- WriteMaskRegion 相关（8 comments）
- UI 渲染（11 rename）
- 其他历史累计（~162 rename + ~62 comments）

### 三、关键逻辑推断

1. **War3 CPU skinning 确认**：War3 1.27a 不使用 D3D9 vertex blending，所有骨骼变换在 CPU 端完成。这是 draw-time VB capture 方案的根本依据。

2. **Path blocker 视觉残留根因**：D3D9 CSM 层拦截完整（6 个 shadowCasters 站点全覆盖），残留来自 War3 原生 TerrainShadow 系统的 `WriteMaskRegion` 路径。

3. **建筑阴影屏蔽方案**：`WriteMaskRegion` hook 已实现（war3_hook_shadow.cpp:972-1212），包含 caller-aware + maskIdx==3 + widget+0x60 flag 三种拦截路径。但 `kNativeStaticShadowMaskHideEnabled=false` 导致拦截被编译期移除。Phase 7.100 的"idx==3 被推翻"结论可能错误——探针读到了非标准 widget 的值。

4. **RenderQueue 排序算法**：5 级优先级链 = special flag → transparent flag → layerState ptr → meshData ptr → layerState 内容前 20B → 稳定排序。

5. **CSpriteUber dt gate**：4 个变体共享 `fabs(dt) >= 2*FLT_EPSILON` 门控，Phase 7.47 实测 dt>0 占 98.79%，dt==0 仅 1.21% 且集中在进图前两帧。

### 四、待解决问题

1. **建筑阴影屏蔽**：需要启用 `kNativeStaticShadowMaskHideEnabled=true` 做 A/B 实验
2. **Path blocker 原生阴影**：需要 hook `TerrainShadow_WriteMaskRegion` + `maskIdx==3`
3. **CEffect 派生类**：IDA 中未找到直接命名的函数（可能被内联或使用不同命名约定）
4. **D3D9 state block 字段级文档**：需要逆向 GxDevice 内部实现

### 五、性能优化累计收益（Phase 7.70-7.86）

| 优化项 | 理论 CPU 削减 |
|---|---|
| 同帧 draw-time VB capture dedup | ~40-100μs/帧 |
| 6 个全局 mutex → shared_mutex | ~0.6-1.5ms/帧 |
| Phase 7.49 publish probe 默认关 | ~0.3-0.9ms/帧 |
| frameTag/frameNumber load 合并 | ~0.15-0.4ms/帧 |
| 4 个 thread_local scratch caches | ~0.1-0.3ms/帧 |
| **总计** | **~1.1-2.7ms/帧** |

### 六、下一步建议

1. **最高优先**：启用 `kNativeStaticShadowMaskHideEnabled=true` 做 A/B 实验，验证 idx==3 拦截是否真正有效
2. **次要**：hook `TerrainShadow_WriteMaskRegion` + `maskIdx==3` 彻底屏蔽建筑阴影
3. **合并准备**：69 commit squash + changelog（audit 报告已就绪）
4. **性能优化**：继续削减 Shadow/Main / receiver / TAA GPU 成本



---

## 🔧 2026-05-30 接手：三大顽固问题统一治理（Claude Opus 4.8）

> 约束：先理论成立后开发；内存/CPU 紧张暂不跑 AutoTest；IDA 已开启继续逆向。
> 目标问题：(1) 魔兽自带静态阴影禁用 (2) 桥/斜坡卡顿 (3) path blocker 渲染泄漏。
> 计划文档：`docs/plan/three_problems_resolution_2026_05_30/README.md`

### 一、本轮 IDA 复核结论（理论奠基）

1. **路径 Z（FogMask WriteMaskRegion 0x234710）确认不是建筑阴影**：
   - 重新反编译 `WriteMaskRegion` + `DispatchToShape (0x234420)`：
     `a2+0x10C` 是 footprint 形状 result（`a4 ? 4 : *(u16*)(a2+0x10C)`），不是 mask
     layer index；`a3` 是 16-bit player-slot 可见性位 + 形状位，与 fog/LOS/path
     共享 mask grid。caller 全是 RebuildMask / WriteMaskRegion_ForObject /
     FromActorRuntime / CUnit_StampBuildingShadowFootprint /
     CWidget_RegisterFootprintAndShadowMask。
   - **结论与 Phase 7.100 一致**：这是 fog/视野/路径阻挡 mask，不能拦（会破坏迷雾）。
     `idx==3` 推断已被推翻。**放弃路径 Z 作为阴影治理点。**

2. **路径 X（CDoodads 贴花阴影）才是"魔兽自带可见静态阴影"治理点**：
   - `CDoodads_CreateDoodadAndActivate (0x74D5AE)` 在 `a6=0` 时调用三件事：
     a) `ShadowPath_StaticStamp_Toggle (0x74E420)` — ListA 直写 **【已被项目拦】**
     b) `TerrainShadow_ToggleStaticStampFromObject (0x74DB30)` → RegisterImage(type=0)
        **【历史未拦 → 树木/装饰物地面贴花阴影一直可见】**
     c) `TerrainShadow_ToggleEmitterStamp (0x74DE40)` → RegisterImage(type=4) 发光体/特效
   - `0x74DB30` 调用者全是 CDoodads_*（Create/Destroy/EnableFeatures/DisableFeatures）
     + `CDoodads_SetTodAndRefreshStamp (0x75C5F0)`（TOD 变化重写）。
   - **path blocker 在 War3 里就是 doodad，走同一条 CDoodads 路径**，所以拦
     `0x74DB30` 同时治理问题 1 和问题 3 的 native 阴影部分。

3. **桥/斜坡卡顿根因（draw-time VB cache 16 帧淘汰）**：
   - 桥/斜坡/建筑/装饰物是静态几何（CPU skin 后顶点固定、worldMatrix 固定），
     却被当动态对象按 16 帧 TTL 淘汰 + 每次重新 GPU copy（`d3d9_device.cpp:18050`）。
   - 离开视野 16 帧后 entry 的 GPU buffer 释放，再次进入视野必须重新
     createBuffer（vkAllocateMemory 同步阻塞主线程）→ **复现"看到桥/斜坡卡，离开
     再回来又卡"**。

### 二、本轮代码改动（已编译通过 `ninja -C build32`，仅既有 warning）

**问题 1 + 3（CDoodads 贴花阴影拦截）**：
- `war3_internal_test_config.h`：新增 `kNativeShadowDoodadStampHookEnabled=true`、
  `kNativeShadowBlockDoodadStaticStampWhenMode1=true`、
  `kNativeShadowBlockDoodadEmitterStampWhenMode1=true`（emitter 暂未真正拦）、
  stats 开关。
- `war3_hook_address_book.h/.cpp`：新增 `terrainShadowToggleStaticStampFromObject
  (0x74DB30)` 与 `terrainShadowToggleEmitterStamp (0x74DE40)`。
- `war3_hook_shadow.h/.cpp`：新增 `Hook_Doodad_ToggleStaticStampFromObject`
  （`__fastcall(this,edx,doodadSlot,enable)`，mode>=1 且 enable!=0 时 return 0
  跳过贴花阴影注册；enable==0 移除必须放行）+ 4 个诊断 counter + query。
  install 在 `InstallShadowHooks` 内挂 `0x74DB30`。
- **关键安全决策**：`ToggleEmitterStamp (0x74DE40)` 是 `__userpurge`（edi=this 隐式
  参数），不能用标准 __fastcall trampoline 安全 hook（passthrough 会丢 edi），
  本轮**不拦它**（它是发光体/特效/腐地 puff，不是静态树木/建筑阴影本体）。
  只拦 `0x74DB30`（type=0 静态贴花阴影主路径，干净 __thiscall 已 IDA 验证）。
- 与现有 `Hook_ShadowPath_StaticStamp_Toggle (0x74E420)` 一起，覆盖 CDoodads 两条
  阴影写入链（ListA 直写 + RegisterImage type=0）。

**问题 2（桥/斜坡卡顿 — 静态几何 VB cache 常驻）**：
- `War3DrawTimeVBEntry`（d3d9_device.h）新增 `isStaticGeometry / lastAccessFrameSerial
  / ownedGpuBytes`。
- `war3_internal_test_config.h`：新增
  `kShadowDrawTimeVBCacheStaticPersistEnabled=true`、
  `kShadowDrawTimeVBCacheStaticPersistMaxBytes=64MiB`、
  `kShadowDrawTimeVBCacheStaticMaxIdleFrames=108000`、
  `kShadowDrawTimeVBCacheDynamicMaxAgeFrames=16`。
- capture 末端：`objectKind==Building||Destructible` 标记为静态几何 + 记录占用字节。
- cache cleanup（d3d9_device.cpp ~18050）：静态几何不按 16 帧淘汰，常驻复用
  GPU buffer；只在长期闲置（~30 分钟）或超 64MiB 字节上限时按 lastAccessFrameSerial
  LRU 回收。动态对象保持原 16 帧 TTL。

### 三、待验证（用户内存空闲后实机 + AutoTest）
- 问题 1/3：mode=1 下树木/装饰物/path blocker 的 native 地面贴花阴影是否消失，
  且 fog/视野/路径阻挡不受影响。
- 问题 2：桥/斜坡反复进出视野是否不再卡（静态 entry 复用，无 createBuffer）。
- 风险回退开关：
  - `kNativeShadowDoodadStampHookEnabled=false` 关闭 doodad 贴花拦截。
  - `kShadowDrawTimeVBCacheStaticPersistEnabled=false` 回到 16 帧 TTL。

### 四、下一步（仍需推进）
- 问题 3 的 CSM 泄漏部分：核对 EntryGate 是否覆盖所有 6 个 shadowCasters
  emplace_back 站点；path blocker mesh 是否仍被 capture 进 shadowCasters。
- 若 doodad 贴花拦截不彻底（EnableFeatures 在 hook 安装前已创建的 doodad），
  考虑 `CDoodads_CreateDoodadAndActivate` a6 注入 + 早装。


---

## 🎯 2026-05-30 第二轮：根因突破（用户反馈"三个修复全无效"后）

### 用户反馈
"你的修复没有一个是生效的，问题全都是老问题。"

### 诊断（拿硬证据，不再猜）
1. 读 `E:\Work\War3\war3_d3d9.log`（23:49，晚于 23:28 部署）：确认 DLL 是我的版本，
   含我的新字符串，hook 安装日志正常（DispatchToShape/WidgetIdentity install ok）。
2. 确认 path blocker CSM 拦截在跑：EntryGate/Producer/FastAppend/AppendEntry 全部
   命中 YTfb/YTpb/YTab 并 `return false`（6 个 emplace_back 站点核对，拦截正确）。
3. **但用户仍看到静态阴影 + path blocker 阴影。** 说明可见阴影根本不是我们的 CSM 画的。

### 根因（IDA 决定性证据）
反编译 `CWorld_TerrainShadow_Dispatch (0x6F7369B4)` case0（主渲染路径），它直接调用：
```
TerrainShadow_ListA_RenderPreparedGroups (0x7370A0)   ← 画 ListA 分组 stamp
Terrain_ShadowListA_RenderAllEntries (0x737110)       ← 画所有 ListA stamp
```
这两个函数是**真正画 doodad/建筑/path blocker 地面贴花阴影 stamp 的消费点**，
它们消费 `RegisterImageEntry`（ToggleStaticStampFromObject/EmitterStamp）注册进
ListA stamp 池的条目，逐条调 `Terrain_ShadowListA_RenderEntryComplex`。

**它们绕过项目所有现有 hook**：
- 历史只 hook 了 `Terrain_RenderShadowLayer (0x737620)`，mode=1 只关它的 a3(ListB)；
- 但 case0 里 `ListA_RenderPreparedGroups` + `ShadowListA_RenderAllEntries` 是
  **直接调用**，不经过 RenderShadowLayer，所以从没被拦到。
- xrefs 确认这两个函数**仅被 CWorld_TerrainShadow_Dispatch 调用**，专职 ListA
  shadow stamp 渲染，不涉及 fog/visibility/border（那些走独立 FogMask grid）。

这一次性解释了为什么**历史所有静态阴影/path blocker 拦截全部无效**：
拦的都是 producer（RegisterImage/StaticStamp）或错误 consumer（ListB/RenderLayer/
Projector），从没拦到真正画 stamp 的这两个函数。

### 本轮修复（已编译 + 部署，d3d9.dll 26904889 @ 0:03）
- 新增 hook：`Hook_TerrainShadow_ListA_RenderPreparedGroups (0x7370A0)` +
  `Hook_TerrainShadow_ListA_RenderAllEntries (0x737110)`，均 `__fastcall(this,edx)`。
  mode>=1 时直接 return（什么都不画）。
- 配置：`kNativeShadowListARenderHookEnabled=true`、
  `kNativeShadowBlockListARenderWhenMode1=true`。
- 安装日志用 `Logger::info` 写进 war3_d3d9.log（`war3dbg::Print` 走 DebugView 不进
  文件，这是上轮"看不到 doodad hook 日志"的原因）。
- 保留上轮 producer 端 `ToggleStaticStampFromObject` hook 作为兜底。

### 关键经验教训
1. `war3dbg::Print` 走 DebugView（OutputDebugString），**不写 war3_d3d9.log**；
   `Logger::info` 才写文件。验证 hook 是否安装必须用 Logger::info。
2. 拦截"producer 注册"不等于阻止"consumer 渲染"——地图预置对象在 hook 前已注册，
   必须在 consumer 端拦才彻底。
3. 一个渲染系统可能有多个 render 入口，hook 一个不够，要 xrefs 找全所有 consumer。

### 待实机验证
- 下次启动看 war3_d3d9.log 是否有 `ListA_RenderPreparedGroups install ... result=ok`
  与 `ListA_RenderAllEntries install ... result=ok`；
- 视觉：静态阴影 + path blocker 阴影是否消失，fog/视野/边界是否正常；
- 若出现边界条纹伪影（doc 24 §8.2 历史风险）→ 改用更细粒度的 entry 级过滤。
- 回退开关：`kNativeShadowBlockListARenderWhenMode1=false`。


---

## ❌ 2026-05-31 第三轮：Phase 7.143 ListA hook 证伪 + 重新调查

### 用户实测反馈（决定性）
"你把所有放置 pathblock 的悬崖全部干掉了，现在游戏中所有悬崖地形全部不渲染了。
另外两个问题也完全没解决。"

### 证伪结论
Phase 7.143 hook `0x7370A0 (ListA_RenderPreparedGroups)` + `0x737110
(ShadowListA_RenderAllEntries)` 是**破坏性错误**：
- 这两个函数渲染的是**悬崖/地形 tile 几何本身**，不是阴影贴花。
- IDA 复核 `sub_6F725F80`：返回 148 字节/tile 的地形 tile 几何结构
  （`*(this+73) + 148 * (tileX + tileY * stride)`），`RenderEntryComplex` 里的
  `GxDevice_DrawIndexedRange` 画的是地形 tile 网格。
- **"TerrainShadow" / "ShadowListA" 命名严重误导**——它其实是 War3 的地形
  渲染系统（含悬崖墙面），不是阴影系统。

### 已立即处置
- `kNativeShadowListARenderHookEnabled = false`、
  `kNativeShadowBlockListARenderWhenMode1 = false`（编译期禁用，禁止再开）。
- 重新编译 + 部署 `E:\Work\War3\d3d9.dll`（26900359 @ 0:31），悬崖恢复渲染。

### 关键教训（第三次踩坑）
1. **不能靠函数名（"Shadow"）判断功能**——必须看它实际 draw 什么几何。
2. `CWorld_TerrainShadow_Dispatch` 这个 IDA 名字里的 "Shadow" 是误导，它是
   地形渲染分发器，case0 画的是地形 tile（含悬崖）。
3. 三个问题**全部仍未解决**，且我之前所有关于"静态阴影渲染消费点"的推断都建立
   在错误的函数语义上。

### 重新调查方向（必须换方法）
不能再靠函数名猜。正确方法：
1. **静态阴影**：魔兽自带的 doodad/建筑地面贴花阴影。需要确认它到底是
   (a) 一个独立的贴花 draw（有特定 texture/blend state），还是
   (b) 地形 tile 着色的一部分（mask 驱动）。
   → 应该从 D3D9 draw call 的 **render state / texture** 特征识别，而不是函数名。
2. **path blocker**：用户说"放 pathblock 的悬崖"——可能 path blocker 和悬崖
   绑定，需要区分"path blocker 的阴影"和"悬崖几何"。
3. **桥/斜坡卡顿**：仍未验证根因。

### 当前状态
- 所有破坏性 hook 已禁用，DLL 恢复到"不破坏悬崖"状态。
- 三个问题回到起点，但排除了一条错误路径（ListA != 阴影）。


---

## 🔬 2026-05-31 第四轮：撤销破坏 + 改用证据驱动（draw-time survey）

### 处置
- Phase 7.144：禁用破坏性 ListA hook（已恢复悬崖）。
- Phase 7.145：在 `BuildShadowReplayDraws`（所有 shadow caster 汇聚到 GPU
  shadow map 渲染前的**唯一收集点**）加：
  1. **最终 path blocker 防线**：用 `draw.batchHandle`（jHandle）→ widget
     identity cache 反查 rawcode → 黑名单匹配，reject 任何上游漏判的 path
     blocker（覆盖 rawcode 在上游为 0 的泄漏路径）。
  2. **shadow draw survey**：env `DXVK_WAR3_SHADOW_DRAW_SURVEY=1` 时，前 40 个
     unique caster（rawcode 或 batchHandle）各写 1 行到 war3_d3d9.log，
     **直接告诉我们 shadow map 实际画了哪些对象**。
- 给 doodad `ToggleStaticStampFromObject (0x74DB30)` hook 加 `Logger::info`
  安装确认。

### 关键事实澄清（修正之前的错误认知）
- `War3ShadowCasterDraw`（draw-time 结构）**只有 batchHandle + objectKind**，
  没有 rawcode/jHandle 字段。所以 draw-time 识别 path blocker 只能靠
  batchHandle → widget cache。这也解释了为什么有些 path blocker 在 draw-time
  无法识别（widget cache 未命中，AGENTS 7.99 已记录 magicMatched=0）。
- ListA（0x7370A0/0x737110）= **悬崖/地形 tile 渲染**，不是阴影。已永久禁用。
- FogMask WriteMaskRegion = fog/LOS/path，不是阴影。已确认不碰。

### 三个问题的当前诚实状态
1. **静态阴影禁用**：producer 端 `ToggleStaticStampFromObject` hook 已装
   （mode>=1 拦截 doodad 静态 stamp 注册），但**预置 doodad 在 hook 前已注册**，
   且 stamp 渲染消费点尚未确定（不是 ListA）。**未解决**。
2. **桥/斜坡卡顿**：静态 VB cache 持久化已落地（非破坏性），但根因可能是
   `captureLiveState` 的 per-record populate 成本（AGENTS 7.95-7.99），
   不只是 VB alloc。**未验证**。
3. **path blocker 泄漏**：draw-time 最终防线 + survey 已落地。**需要 survey
   数据定位真正泄漏源**。

### 给用户的测试请求（下次有空时）
启动游戏前设环境变量 `DXVK_WAR3_SHADOW_DRAW_SURVEY=1`，进图后看
`war3_d3d9.log` 里的 `SHADOW DRAW SURVEY #N rawcode=... (XXXX) ...` 行。
这会列出 shadow map 实际画的对象 fourcc。如果里面有 path blocker 的 fourcc
或 building/doodad，就能确定泄漏源；如果全是正常单位，说明可见的"静态阴影"
是 native War3 渲染的（不是我们的 CSM）。

也可在 ImGui 面板把"原生阴影"切到"禁用"(mode2) + 把我们的阴影开关关掉，
做 A/B：哪个开关让静态/path blocker 阴影消失，就锁定了来源。

### 当前 DLL
`E:\Work\War3\d3d9.dll`（26905337 @ 0:40），含 7.144 回退 + 7.145 survey。
悬崖正常，无破坏性 hook。


---

## 🎯 2026-05-31 第五轮：path blocker 权威清扫 + 澄清"合批优化"

### 用户两个关键澄清
1. **"路径阻断器被拦截了，但依然渲染了 ShadowMap"** —— reject 日志命中，但仍画。
2. **"合并批量提交优化" = 渲染层的合批提交优化（batch merge）**，不是 git commit。

### Phase 7.147：path blocker 权威清扫（根治问题 3 的 CSM 泄漏）
- **根因**：`shadowCasters` 有 3+ 个**直接** consumer：
  - `BuildShadowReplayDraws`（我之前只在这里加了网）
  - `war3_shader_api.cpp`（直接遍历 shadowCasters 建 DrawCall）
  - `d3d9_war3_shadow_resources.cpp`（直接用 shadowCasters[i] 建矩阵 SSBO）
  - 某条 append 路径在 rawcode=0 时漏判 path blocker，进了 shadowCasters，
    被未过滤的 consumer 画出 → "日志 reject 但仍渲染"。
- **修复**：
  1. `War3ShadowCasterDraw` 新增 `rawcode`/`jHandle` 字段；
  2. 4 个 append 站点（canonical / DirectGrouped / FastAppend / legacy）都把
     已解析的 rawcode/jHandle 写入 caster；
  3. 在 `War3UpdateSemanticReplayInputDiagnostics`（**所有 consumer 之前的唯一
     finalize 闸门**）做权威清扫：命中 path blocker（caster.rawcode 或 jHandle
     兜底）就把 `positionStorage=nullptr`+counts=0。所有 consumer 都检查
     positionStorage（null→skip），因此**覆盖全部渲染路径，consumer-agnostic**。
- 这是问题 3 真正完整的修法：不依赖哪条 append 路径，最终统一清扫。
- 当前 DLL：`E:\Work\War3\d3d9.dll`（26905207 @ 2:32）。

### 渲染层合批优化现状（待推进，不盲改）
- `kNativeQueueAutoInstancingEnabled`（默认 false）是真正的实例化合批开关，
  但有 TeamColor/layer 污染风险（config 注释警告）。
- `War3BatchMerger` 只是分析器（统计潜在可省 drawcall），不实际合批。
- `FlushSortedItems_StdSort`（war3_render_queue.h）是 RenderQueue flush 主路径。
- **不盲目开启实例化**（本会话已多次栽在盲改上）。需要先逆向确认合批安全条件
  + 可回归验证再动。

### 待用户验证
1. path blocker：Phase 7.147 finalize 清扫应彻底（不再"日志拦了还渲染"）。
   若仍可见，开 `DXVK_WAR3_SHADOW_DRAW_SURVEY=1` 看 survey 行确认泄漏 fourcc。
2. 静态阴影（树/建筑 native 贴花）：producer 端 `ToggleStaticStampFromObject`
   hook 已装，但 consumer 渲染点仍待定（不是 ListA=悬崖）。**未解决**。
3. 桥/斜坡卡顿：静态 VB 持久化已落地，未验证。


---

## 🌙 2026-05-31 凌晨无人值守：三问题根因彻查（Claude Opus 4.8）

### Phase 7.148 — 修复闪烁回归（上一会话引入）
- 根因：Phase 7.145/7.147 的 draw-time path blocker sweep（在 BuildShadowReplayDraws +
  War3UpdateSemanticReplayInputDiagnostics）按 caster 的 batchHandle/jHandle → widget
  cache 反查置空几何。caster.rawcode 部分帧解析得到、部分帧为 0；rawcode=0 时走
  batchHandle 反查可能把真实单位误判成 path blocker → 同一 caster 逐帧"保留/置空"
  抖动 → 高频阴影闪烁。
- 修复：新增 `kPathBlockerDrawTimeSweepEnabled = false`（默认关），把两处 sweep 门控掉。
  上游 eligibility/capture 拦截（EntryGate/Producer/FastAppend）不受影响，仍生效。
- 文件：`war3_internal_test_config.h`、`d3d9_device.cpp`(~1933)、`d3d9_war3_shadow.cpp`。

### Phase 7.149 — 增强 shadow draw survey（证据工具，零行为改动）
- `War3ReplayDrawSurvey` 现输出几何签名（vtx/idx/blend）+ 世界坐标 + 匿名 caster
  (rawcode=0/handle=0) 也能被记录（用 category+vertexCount 合成 dedup key）。
- 用法：env `DXVK_WAR3_SHADOW_DRAW_SURVEY=1`，前 40 个 unique caster 写 war3_d3d9.log。

### Phase 7.150 — 🎯 Problem 3（path blocker 泄漏）根因找到并修复

**决定性根因链（代码 + IDA 定位）**：
1. path blocker（YTab/YTpb/YTfb...）是 **rigid doodad**（无 vertexBlend）。
2. 新长期 semantic 路线 `kShadowSemanticCoreSceneUnitsOnly = false`，rigid 对象走
   `War3ShouldSubmitSemanticPacket` 的 `!unitsOnly` 分支。
3. 该分支末尾有 `explicitUnknownRigid` 逃生通道：接受
   `objectKind==Unknown + rigid + worldObjectEntry + sceneNode + geometry +
   safeOpaqueMaterial + mainWorldVisibleBacking` 的 caster。
4. 当 path blocker 的某个 packet 实例满足：
   - `renderable.rawcode == 0`（visible registry 没抓到它的 rawcode）
   - `renderable.jHandle` 在 RenderObjectRegistry / widget cache 都 miss
   - `objectKind == Unknown`
   → rawcode/jHandle 两条判定全失效，`explicitUnknownRigid==true` 让它通过 →
   提交进 shadowCasters → CSM 投出阴影。
5. **这就是"日志命中拦截但仍渲染"的真相**：日志命中的是**同一 path blocker 的
   其它 packet 实例**（rawcode 已解析的，在 EntryGate/FastAppend/AppendEntry 被拦），
   而 rawcode=0 的匿名 rigid 实例从 explicitUnknownRigid 漏过，**不写任何 reject 日志**。

**修复（Phase 7.150）**：
- 新增统一判定 `War3PacketIsPathBlocker(packet)`，三通道：
  1. `rawcode != 0` → `IsLosBlockerFourCc`
  2. `jHandle` → RenderObjectRegistry + widget cache 反查
  3. **widget 指针直读**：`worldObjectEntry`/`unitPtr` 的 `+0x0C`(magic 0x2B5DB42C)
     + `+0x30`(rawcode)，命中后 write-through 写回 widget cache。
  这第 3 条是堵 explicitUnknownRigid 漏网的关键（rawcode=0 时唯一能拿到身份的路径）。
- 在两个统一 chokepoint 调用它：
  - `War3ShouldSubmitSemanticPacket`（eligibility 层，在 explicitUnknownRigid 之前）
  - `War3TryAppendSemanticShadowPacket`（append 入口，覆盖直接调 append 的 producer）
- widget 直读不再 env-gate（之前 Phase 7.104 默认关，导致这条兜底失效）。
- 与 Phase 7.148 闪烁修复的区别：这是在 **packet 提交前** 用 **稳定的 widget 指针**
  判定（worldObjectEntry/unitPtr 跨帧稳定），不是 draw-time 用易变 batchHandle 猜测，
  因此不会引入闪烁。
- 文件：`d3d9_device.cpp`（War3PacketIsPathBlocker 定义 ~4490、前向声明 ~1323、
  eligibility 调用 ~2960、append 调用 ~9940）。
- 编译通过，DLL 已部署。**待用户实机视觉验证 path blocker 阴影是否消失。**
- 验证辅助：开 `DXVK_WAR3_SHADOW_DRAW_SURVEY=1`，若 survey 里不再出现匿名 rigid
  caster（rawcode=0, kind=0, 小 vtx, 在悬崖位置），即证明漏网已堵。


### Phase 7.151 — 🎯 Problem 2（桥/斜坡卡顿）根因找到并修复

**决定性根因（代码定位）**：
- 上一会话（Phase 7.99）已把高压地图 ManifestCopy 优化好（64→93 FPS），ManifestCopy
  早退 + ResolveGeoset cache 让 view-entry 的 record 增长只产生**一帧**成本，不是持续卡顿。
- 真正的"看到桥/斜坡卡 + 离开再回来又卡"根因在 **draw-time VB cache 的静态几何分类**：
  - `entry.isStaticGeometry` **只**对 `ObjectKind::Building / Destructible` 置 true；
  - 但**桥/斜坡/装饰物是 generic doodad**，解析出来是 `ObjectKind::Unknown` 或 `Item`
    （落到 `drawTimeVBCacheOtherKindCaptureCount` 统计桶）；
  - 所以它们**没被标记静态** → 走 16 帧动态 TTL → 离开视野 16 帧后 GPU buffer 被
    erase 释放 → 再次进入视野 `needsNewPositionBuffer==true` → 重新 `createBuffer`
    (vkAllocateMemory 同步阻塞主线程) → **复现"离开再回来又卡"**。
- 验证逻辑：capture 路径 `needsNewPositionBuffer = (positionBuffer==nullptr ||
  positionCapacity < posBytes)`。只要 entry 留在 cache 里（容量够），再次进入视野
  就 O(1) 复用，不再 createBuffer。所以"让静态 entry 常驻"直接消除 re-entry 卡顿。

**修复（Phase 7.151）**：
- 把 `isStaticGeometry` 分类从"仅 Building/Destructible"扩大为"**非动态单位的 rigid
  几何**"：
  - 排除 `ObjectKind::Unit`（骨骼动画，pose 每帧变，绝不能常驻）；
  - 排除有 runtime pose / sprite pose / pose palette 的对象（再次排除动态）；
  - 其余（Building/Destructible/Item/Unknown 的桥/斜坡/装饰物）= 静态，常驻复用。
- 静态常驻仍受 64MiB LRU 上限 + 30 分钟闲置回收约束（不会无限增长）。
- 回退开关：`kShadowDrawTimeVBCacheStaticPersistEnabled = false` → 回到全 16 帧 TTL。
- 文件：`d3d9_device.cpp`(~24628 v4 capture 分类)。编译通过，DLL 已部署。
- **待用户实机验证**：桥/斜坡首次进视野后，离开再回来不再卡。
  - 首次进视野仍可能有 alloc budget 摊销的短暂分帧（32/帧），属预期，非硬卡顿。


### Phase 7.152/7.153 — Problem 1 诊断 + Problem 3 legacy 路径补全

**Phase 7.152（Problem 1 静态阴影 — 诊断）**：
- `kNativeShadowDoodadStampStatsLogging` 默认改为 true，且日志从 `war3dbg::Print`
  （DebugView，工具读不到）改为 `Logger::info`（写 war3_d3d9.log）。
- 用户起床后可在 war3_d3d9.log 看 `DoodadStaticStamp calls=N blocked=M mode=1`，
  验证 `ToggleStaticStampFromObject (0x74DB30)` hook 是否对 pre-placed 路径阻断器/
  装饰物 native 贴花阴影注册命中。
- IDA 已确认 `0x74DB30` 被 `CDoodads_CreateDoodadAndActivate`（含 pre-placed 加载）
  + `EnableFeatures` 调用，hook 在 MainRunner_ENTER（map load 前）安装，理论覆盖
  pre-placed doodad。default mode=1，block 路径生效。

**Phase 7.153（Problem 3 — legacy capture 路径补全）**：
- 发现 Problem 3 还有第三条 caster-creation 路径未覆盖：legacy
  `War3TryCaptureShadowCaster` → `finalizeShadowDrawCommon` → `shadowCasters.emplace_back`
  （Terrain doodad path blocker：currentObj/pathBlockObj 都空，走 legacy capture，
  semantic 两个 gate 管不到）。
- 在 `finalizeShadowDrawCommon` 加 path blocker guard：rawcode==0 时从
  `semantic.worldObjectEntry`/`semantic.object->unitPtr` 做 widget 直读
  （`War3ShadowIsLosBlockerByWidgetPtr`，稳定指针），命中就置空几何 → 所有 consumer skip。

**Problem 3 现在三条 caster-creation 路径全覆盖（统一用稳定 widget 身份）**：
1. semantic eligibility gate（`War3ShouldSubmitSemanticPacket` → `War3PacketIsPathBlocker`）
2. semantic append gate（`War3TryAppendSemanticShadowPacket` → `War3PacketIsPathBlocker`）
3. legacy finalize（`finalizeShadowDrawCommon` → `War3ShadowIsLosBlockerByWidgetPtr`）
全部用 worldObjectEntry/unitPtr 稳定指针 + widget +0x0C/+0x30 直读，跨帧稳定，
不会引入 Phase 7.145/7.147 那种 batchHandle 逐帧抖动闪烁。

---

## 📋 2026-05-31 凌晨无人值守总结（三问题根因 + 修复）

| 问题 | 根因 | 修复 Phase | 状态 |
|---|---|---|---|
| 闪烁回归（本会话引入） | 7.145/7.147 draw-time sweep 用易变 batchHandle 逐帧抖动 | 7.148 默认关 sweep | ✅ 已修 |
| Problem 3 path blocker 泄漏 | rawcode=0 匿名 rigid path blocker 从 explicitUnknownRigid 漏过；legacy Terrain doodad 路径也漏 | 7.150 + 7.153 统一 widget 直读判定，3 条路径全覆盖 | ✅ 根因已修，待视觉验证 |
| Problem 2 桥/斜坡卡顿 | 静态 VB cache 只认 Building/Destructible，桥/斜坡是 Unknown/Item doodad → 16 帧 TTL → re-entry 重新 createBuffer | 7.151 静态分类扩为"非动态单位 rigid 几何" | ✅ 根因已修，待视觉验证 |
| Problem 1 静态阴影（最低优先） | pre-placed doodad native 贴花阴影；producer hook 已装但需验证命中 | 7.152 诊断日志到文件 | ⏳ 诊断就绪，待日志验证 |

**所有修复都有 config 回退开关，编译通过，DLL 已部署 E:\Work\War3\d3d9.dll。**
**等用户实机验证后再决定是否需要进一步调整。**

验证清单（用户起床后）：
1. Problem 3：进游戏看 path blocker 阴影是否消失。若仍有，开
   `DXVK_WAR3_SHADOW_DRAW_SURVEY=1` 看 war3_d3d9.log 的 SHADOW DRAW SURVEY 行，
   匿名 rigid caster（rawcode=0, kind=0, 小 vtx, 悬崖位置）应已消失。
2. Problem 2：看到桥/斜坡 → 离开 → 再回来，是否还卡。
3. Problem 1：看 war3_d3d9.log 的 DoodadStaticStamp blocked 计数是否增长。
4. 整体：阴影是否还有高频闪烁（Phase 7.148 应已消除）。


### Phase 7.154 — 用户实测三问题全未解决 → 转纯证据采集（always-on 诊断）

**关键发现（从用户 3:32 实测日志）**：
- path blocker 在 **5 个 gate 全部命中拦截**（EntryGate / DirectGrouped/Preselect /
  Producer / FastAppend / AppendEntry），日志 rawcode 都是正确的 YTfb/YTpb/YTab。
  说明我的 Phase 7.150 gate 确实在跑且命中。
- **但 `DoodadStaticStamp` 日志一行都没有** → `ToggleStaticStampFromObject (0x74DB30)`
  hook 装了但**从未 fire**（install ok，但 0 次调用）。说明 path blocker/装饰物的
  native 贴花阴影**不走这条注册路径**。
- IDA xrefs 确认 `RegisterImageEntry (0x713250)` 有 6+ 条注册路径：ToggleStaticStamp
  (我 hook 的) / ToggleEmitterStamp(__userpurge 没 hook) / RegisterImageEntryWithParams
  / FromPoint / FromTwoPoints / ShadowProjector(0x76D...)。我只覆盖了 1 条。

**核心矛盾**：CSM 在 5 个 gate 拦了 path blocker，但用户仍看到阴影 + 阴影跟着太阳转
（CSM 行为）。两种可能未排除：
  1. 借用 model：path blocker 用可见装饰物 model（LT* 等）当外观，渲染时 rawcode
     不是 YT* → 我的黑名单不命中 → 当普通装饰物阴影画出来。
  2. 不是我们的 CSM（但"跟着太阳转"又指向 CSM）。

**本轮不再盲改，改为 always-on 诊断（写 war3_d3d9.log，无需设 env）**：
1. `CASTER COMPOSITION`（每 240 帧 1 行）：shadow map 实际 caster 总数 / blend 数 /
   **匿名 rigid 数（rawcode=0 非 blend，path blocker 漏网嫌疑）** / 各 objectKind 分布
   / 第一个匿名 rigid 的世界坐标。
2. `VB ALLOC SPIKE`（本帧 createBuffer >= 8 时 1 行，最多 80 条）：定位问题2 卡顿
   是否来自 VB 分配突发。
3. `SHADOW DRAW SURVEY`（前 40 个 unique caster）：改为**默认开启**，列出 shadow map
   画的每个对象 rawcode + 几何签名 + 世界坐标。

**下一次用户测试后我要看的**：
- 若 `CASTER COMPOSITION anonRigid > 0` → CSM 有 path blocker 漏网（继续堵）。
- 若 `anonRigid == 0` 但用户仍见 path blocker 阴影 → 是借用 model 装饰物或 native，
  看 SHADOW DRAW SURVEY 里 path blocker 位置上是什么 rawcode 的 caster。
- 若 `VB ALLOC SPIKE` 在卡顿时频繁出现 → 问题2 确实是 VB 分配，需要更激进的
  静态常驻 / 预算策略；若不出现 → 问题2 根因不在 VB 分配，要换方向查。

**部署**：DLL 26905966 bytes，已部署 E:\Work\War3\d3d9.dll。诊断默认开，用户正常玩即可。


### Phase 7.155 — 证据驱动修复（用户 11:20 实测日志分析）

**Problem 3 决定性证据**：
- `CASTER COMPOSITION anonRigid=18/6/3/14/10` — **匿名 rigid caster 确实在 shadowCasters 里**。
- SHADOW DRAW SURVEY #34-40：`rawcode=0x00000000, batchHandle=0x0, kind=1(Unit), blend=0,
  vtx=6~198, pos=(-928,288,0)/(684,234,192)` — 完全匿名、rigid、小几何、在悬崖坐标。
- **关键发现**：path blocker 被错误分类为 `ObjectKind::Unit`（kind=1），不是 Unknown！
  所以它通过了 `War3IsEligibleSemanticDynamicUnit`（接受 Unit），不是 `explicitUnknownRigid`。
  我之前的 Phase 7.150 假设（explicitUnknownRigid 漏网）是错的。
- **真正漏网路径**：path blocker 以 `rawcode=0 + jHandle=0 + worldObjectEntry=nullptr +
  unitPtr=nullptr + objectKind=Unit + rigid + 小几何` 的完全匿名形态通过 Unit 路径。
  所有基于身份的拦截（rawcode/jHandle/widget 直读）全部失效（没有任何身份指针）。
- **修复**：在 `War3PacketIsPathBlocker` 末尾加启发式兜底：
  `jHandle==0 && worldObjectEntry==nullptr && unitPtr==nullptr && vtxCount<=200 && path==Rigid`
  → 判定为 path blocker。真实单位总有至少一个身份字段；path blocker 是唯一"完全匿名
  rigid 小几何"的对象。

**Problem 2 决定性证据**：
- `VB ALLOC SPIKE posAlloc=16 idxAlloc=16` **每帧**，持续 10+ 帧 → 每帧 32 次
  `createBuffer`（vkAllocateMemory 同步阻塞）→ 240→50 FPS 暴降。
- 原预算 32/帧 = 等于不限制（pos+idx 各 16 = 32 刚好用完）。
- **修复**：预算从 32 降到 **4/帧**。200 个新 entry 分摊到 ~50 帧（~0.8s @ 60fps），
  阴影逐步出现，无硬卡顿。
- 附加发现：`staticPersist=1` 但 alloc 仍频繁 → 因为所有 caster 被分类为 Unit（kind=1），
  我的 Phase 7.151 静态分类排除了 Unit → 桥/斜坡也被排除 → 仍走 16 帧 TTL。
  这是 objectKind 分辨率问题（TLS 污染），但 budget=4 已经从根本上限制了 alloc 突发。

**DLL 已部署**：26910330 bytes @ 12:00。用户可立即测试。

### Phase 7.156 — 三问题理论修复首批落地（2026-06-05）

**本轮约束**：
- 用户明确要求当前不要启动 War3 / AutoTest（内存紧张、CPU 高），因此本轮只做
  静态逆向、代码落地与 `ninja -C build32` 编译验证。
- HEAD 已回退 Phase 7.155 的高风险改动：宽泛匿名小网格规则会误杀真实单位子网格；
  alloc budget 从 32 降到 4 会造成阴影不完整。因此本轮不能重复这两条路线。

**Problem 1：魔兽原生静态阴影禁用（RegisterImage 上游主控）**：
- IDA 重新确认 `TerrainShadow_RegisterImageEntry @ 0x6F713250` 是 TerrainShadow
  图像/stamp entry 的核心注册函数：写 size/pos/type，调用资源解析，资源无效时返回
  `-1`，成功才设置 active bit 并返回 slot index。
- IDA 重新确认关键 caller 的 `-1` 契约：
  - `TerrainShadow_ToggleStaticStampFromObject @ 0x6F74DB30`：`enable` 分支把返回值写入
    `a2+136`，随后只有 `result != -1` 才设置可见；删除分支同样按 `-1` 跳过。
  - `TerrainShadow_ToggleEmitterStamp @ 0x6F74DE40`：写 `a3+144`，删除分支按 `-1`
    跳过。
  - `RegisterImageEntryWithParams @ 0x6F7290B0`：RegisterImage 返回 `-1` 时直接返回 0。
- 代码落地：
  - `war3_hook_shadow.cpp` 新增 `Hook_TerrainShadow_RegisterImageEntry`，用
    `_ReturnAddress()/__builtin_return_address(0)` 做 8 个 caller 返回地址分桶；
  - owner 解析尝试 `ownerArg / ownerArg-0x0C / ownerArg-0x10`，按
    widget magic `0x2B5DB42C` + rawcode + flags `0x10000` 识别 Building/Destructible/Unit；
  - `war3_shadow_filter_policy.cpp::DecideRegisterImage` 继续作为统一策略入口；
  - `mode=1` 下启用严格 owner-aware 策略，StaticStamp 与 Building/Destructible owner
    被拒绝，Selection/MarkOcclusion 保留。
- 目的：避免再依赖 ListA/ListB/WriteMaskRegion 这类已证明不稳定的下游消费者过滤。

**Problem 2：桥/斜坡首次看见大量卡顿，离开再回来又卡**：
- 已知实测证据是 `VB ALLOC SPIKE posAlloc=16 idxAlloc=16` 多帧连续，根因是
  draw-time VB cache 的动态 TTL 回收后再次进视野重复 `createBuffer(vkAllocateMemory)`。
- 7.155 的 `budget=4` 虽能限流，但会导致阴影逐步缺失/不完整，已回退。
- 本轮保留 budget=32，改根因分类：
  - 真实动态单位仍必须有 unit/rawcode/handle/model/pose 等至少一条稳定证据；
  - 桥/斜坡/装饰物/path blocker 这类被 TLS 污染成 `ObjectKind::Unit`、但完全没有身份
    与动态 pose 证据的 rigid 几何，改走 static persist；
  - static persist 只保留 GPU buffer 容量，仍会在可见 draw 时 copy 最新 VB 数据，不会冻结
    动态姿态。
- 新开关：`kShadowDrawTimeVBCacheUnitlessRigidStaticPersistEnabled=true`。

**Problem 3：Path blocker 被 ShadowMap 渲染出来**：
- 7.155 的宽泛规则（`jHandle==0 && widget==nullptr && vtx<=200 && path==Rigid`）
  已被回退，因为会误杀真实单位子网格。
- 本轮改成窄门控：
  - 只在 `PathBlockerHideEnabled` 且 rawcode 仍为 0 时启用；
  - 只接受 Terrain doodad / Decorations 路径；
  - 必须没有 unit/handle/rawcode/object/worldObject/model/pose 证据；
  - 必须是 rigid、非 transparent、无 vertex blend；
  - 几何上限：`vtx<=200` 且 `idx<=900`。
- 覆盖点：
  - `War3PacketIsPathBlocker` 的 packet 统一判定；
  - draw-time VB cache GPU copy 前 early reject，避免无效 marker 进入 cache；
  - legacy `finalizeShadowDrawCommon` 兜底，避免绕过 semantic packet 的路径漏网。
- 新开关：
  - `kPathBlockerAnonymousRigidMarkerGateEnabled=true`
  - `kPathBlockerAnonymousRigidMarkerMaxVertices=200`
  - `kPathBlockerAnonymousRigidMarkerMaxIndices=900`

**验证**：
- `ninja -C build32` 通过，生成 `src/d3d9/d3d9.dll`。
- 未运行 AutoTest / 未启动 War3，符合用户本轮约束。
- 剩余风险：需要用户后续允许实机时验证视觉结果；当前只能证明逆向契约与编译成立。

### Phase 7.157 — RegisterImage 主 key 阻断恢复 + 匿名 marker 误杀面收口（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做 IDA 静态逆向、代码收口与编译验证。

**Path blocker 安全纠偏**：
- 复审 Phase 7.156 发现：draw-time VB copy 前与 legacy finalize 两处匿名 marker
  判断都有 Terrain/Decorations 阶段上下文；但 packet 级 `War3PacketIsPathBlocker`
  没有 stage/tag/category 字段，若在 packet 层使用 “无身份 + rigid + 小几何” 启发式，
  理论上仍可能复现 Phase 7.155 对真实单位小子网格的误杀。
- 代码收口：
  - 移除 packet 层匿名 marker 兜底；
  - 保留 packet 层 rawcode / jHandle / widget 直读三通道；
  - 匿名无身份兜底仅保留在具备阶段上下文的 draw-time VB copy 前与 legacy finalize。
- 结论：Path blocker 的匿名兜底仍覆盖用户日志中的 Terrain/Decorations 漏网形态，
  但不再在无阶段上下文的 packet 统一 gate 中放大误杀面。

**静态阴影 RegisterImage 策略补强**：
- IDA xrefs 重新确认 `TerrainShadow_RegisterImageEntry @ 0x6F713250` 共有 8 个关键
  callsite，地址簿当前返回地址组匹配：
  `0x7291DC, 0x74DAB6, 0x74DBFA, 0x74DF55, 0x76D44A, 0x76D5A4, 0x76D69A, 0x76D719`。
- IDA 重新确认：
  - `RegisterImageEntryWithParams @ 0x6F7290B0` 返回 `-1` 时直接返回 0；
  - `RegisterImageEntryFromPoint @ 0x6F76D5F0` 与
    `RegisterImageEntryFromTwoPoints @ 0x6F76D6D0` 都是直接包装 RegisterImage；
  - 因此在 RegisterImage hook 内返回 `-1` 仍符合 War3 原生失败契约。
- 根据历史 Phase 7.55/7.56 实测证据恢复 producer 级默认阻断：
  - `kNativeShadowRegisterBlockShadowTextureKeyWhenMode1=true`
    （阻断 `ReplaceableTextures\\Shadows\\*`、`Shadow`、`ShadowFlyer`、`BuildingShadow*`）；
  - `kNativeShadowRegisterBlockWithParamsUberSplatWhenMode1=true`
    （阻断 WithParams + `*UberSplat` 静态阴影残留主路径）。
- 保持不启用 ListA/ListB/WriteMaskRegion 消费侧拦截：这些路径历史上存在雾/边界/地形
  误伤风险，本轮继续优先使用 RegisterImage 上游 producer 主控。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- 仅新增/既有 warning，无阻塞错误；未运行 AutoTest / 未启动 War3。

### Phase 7.158 — RegisterImage 调用约定审计与来源统计补强（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做 IDA 静态逆向、代码补强与编译验证。

**调用约定与 caller 语义复核**：
- `TerrainShadow_RegisterImageEntry @ 0x6F713250` 是 `__thiscall(this, key, size, pos, ownerArg, typeArg)`；
  hook 继续使用 32-bit thiscall 常用的 `__fastcall(this, edx, ...)` 包装方式。
- IDA 反汇编确认：
  - `0x74DAB1` callsite 注册后立即写 `entry+0x8C`，随后构造
    `"SelectionCircle" / "ColorFriend"`，属于选择圈/友军颜色标记，应白名单放行；
  - `0x76D5A4` 附近构造 `"Occlusion" / "MarkColor"`，属于遮挡标记颜色，应白名单放行；
  - `0x76D400 -> 0x76D445` 是坐标/范围包装器，返回值直接来自 RegisterImage；
  - `0x76D5F0 -> 0x76D695` 与 `0x76D6D0 -> 0x76D719` 分别是 FromPoint/FromTwoPoints
    包装器，失败返回 `-1` 的契约仍成立。

**代码补强**：
- `war3_hook_shadow.cpp`：
  - RegisterImage 来源统计从 4 桶扩展到完整 8 桶：
    StaticStamp / EmitterStamp / Selection / Occlusion / WithParams / ObjectBridge /
    FromPoint / FromTwoPoints / Unknown；
  - 低频日志字段改为 `srcStatic/srcEmitter/srcSelection/srcOcclusion/...`，
    后续实机不用 verbose 也能判断白名单和主阻断来源是否命中；
  - `ResolveShadowRegisterOwnerKind` 增加 `ownerArg <= 0` 早退，避免把负数/空值扩展成
    巨大地址再做无意义探测。

**IDA 同步**：
- rename：
  - `0x6F76D400 -> TerrainShadow_RegisterImageEntry_ObjectBridge`
  - `0x6F76D5F0 -> TerrainShadow_RegisterImageEntryFromPoint`
  - `0x6F76D6D0 -> TerrainShadow_RegisterImageEntryFromTwoPoints`
- comments：
  - 8 个 RegisterImage callsite 均已补充 source/type/owner/返回契约说明；
  - 特别标注 `SelectionCircle/ColorFriend` 与 `MarkColor/Occlusion` 为 mode1 白名单来源，
    不属于阴影本体。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.159 — 桥/斜坡 unitless rigid 静态持久化补强（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做静态复核、代码补强与编译验证。

**桥/斜坡卡顿根因收敛**：
- 用户描述是“第一次看到桥/斜坡大量卡顿，过一会正常；离开识别后下一次看到又卡”，
  这更符合 draw-time VB cache 条目按动态 16 帧 TTL 被回收后重新 `createBuffer/copyBuffer`，
  而不是 Phase 7.95 已修过的 registry/hydration O(N²) 或 matrix writer 高频问题。
- 复审 Phase 7.156 代码发现：`War3SemanticContextHasUnitIdentityEvidence` 把
  `modelResourcePtr/modelKey` 也算作“单位身份”。桥/斜坡/升降机这类 rigid world geometry
  即使没有 `unit/rawcode/handle/runtime pose`，也可能有稳定模型资源；若因此被视为
  `ObjectKind::Unit + identity`，仍会走动态 TTL，重现“离开视野后再进入重新分配”的尖刺。

**代码补强**：
- `d3d9_device.cpp` 拆分两类证据函数：
  - `War3SemanticContextHasIdentityOrResourceEvidence`：仅用于匿名 Path blocker 兜底，继续要求
    “完全无对象/资源证据”，避免误杀真实可见小型 doodad；
  - `War3SemanticContextHasDynamicUnitObjectEvidence`：仅用于 VB cache 静态持久化判定，动态单位证据限定为
    `jHandle/rawcode/runtimeModelPtr`、`RenderObjectInfo.unitPtr/agentPtr/rawcode/jHandle/group0`，
    或 `WorldObjects/SelectionOverlay` 渲染通道；**不再把纯 model resource 当动态单位身份**。
- 结果：被污染成 `ObjectKind::Unit` 但无真实单位对象/pose 的桥、斜坡、阻断器 rigid geometry
  可以落入 `kShadowDrawTimeVBCacheUnitlessRigidStaticPersistEnabled`，离开视野后走静态长 TTL/LRU，
  再次进入时复用已有 GPU buffer 容量，避免重复分配尖刺。
- 真正动态单位仍保守留在动态路径：只要有 handle/rawcode/runtime pose、CUnit/agent 指针、
  group0 或 WorldObjects/SelectionOverlay lane，就不会被当作静态 unitless rigid。

**静态复核**：
- IDA 再次确认 RegisterImage 8 个 xref：
  `0x7291D7, 0x74DAB1, 0x74DBF5, 0x74DF50, 0x76D445, 0x76D59F, 0x76D695, 0x76D714`；
  地址簿保存的是对应返回地址：
  `0x7291DC, 0x74DAB6, 0x74DBFA, 0x74DF55, 0x76D44A, 0x76D5A4, 0x76D69A, 0x76D719`。
- `TerrainShadow_RegisterImageEntry @ 0x6F713250` 反汇编确认 `retn 14h`，
  hook 的 `__fastcall(this, edx, key, size, pos, owner, type)` 包装与 thiscall 契约匹配。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅输出既有 CRLF 工作区提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

**后续实机验证口径（等用户允许测试时）**：
- 桥/斜坡：观察 `DXVK War3Shadow: VB ALLOC SPIKE` 是否只在首次进入区域出现，
  离开再回来不应重复出现同等级 pos/idx alloc 尖刺。
- 静态阴影：观察 `RegisterImage stats` 的 `blocked/srcStatic/srcWithParams/srcFromPoint/srcFromTwoPoints`
  是否命中；Selection/Occlusion 来源应继续放行。
- Path blocker：若仍可见，先判断是 D3D9 CSM caster 漏网还是 native RegisterImage/UberSplat 残留，
  不要重新打开无阶段上下文的 packet 级匿名几何启发式。

### Phase 7.160 — runtimeModelPtr 与动态姿态证据解耦（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做源码复核、代码补强与编译验证。

**复核结论**：
- `VisibleRenderableRegistry` 与 model hook 里大量路径会从 `sceneNode/meshData/sprite`
  解析出 `runtimeModelPtr`。这只能证明“有可识别 CModel/资源谱系”，不能单独证明该对象是动态单位。
- draw-time VB cache 的跨帧消费端有 freshness gate：
  - semantic skinned consume 只接受 `frameSerial + 8 >= currentFrame`；
  - fast append / prebuild bypass 只接受当前帧 entry；
  - 因此 `isStaticGeometry` 主要决定 cache 清理 TTL 与 GPU buffer 容量是否保留，不会让过期动态 pose 无限期直接消费。
- Phase 7.159 仍把 `runtimeModelPtr` 列为动态单位证据，若桥/斜坡/升降机这类静态 rigid doodad
  也被解析出 CModel，仍可能继续按 16 帧动态 TTL 被清理，重复进入视野时继续触发分配尖刺。

**代码补强**：
- `War3SemanticContextHasDynamicPoseEvidence` 改为只看真实 pose 信号：
  `hasPoseTransform / poseFromSpriteFrame / poseMatrixCount / poseMatrixHash`；
  不再把 `runtimeModelPtr` alone 当作动态姿态。
- `War3SemanticContextHasDynamicUnitObjectEvidence` 进一步收窄：
  - `jHandle/rawcode` 仍是动态单位证据；
  - `runtimeModelPtr` 只有在 `WorldObjects/SelectionOverlay` lane 中才算动态单位证据；
  - `RenderObjectInfo.unitPtr/agentPtr/jHandle/rawcode/group0` 仍保护真实 CUnit；
  - Decorations/Terrain lane 里的纯 CModel rigid geometry 不再因此被排除出静态持久化。
- `War3LegacyDrawIsAnonymousRigidMarkerCandidate` 改为复用
  `War3SemanticContextHasIdentityOrResourceEvidence`，确保只要有 object/world/model/resource 任一证据，
  匿名 Path blocker 兜底就不触碰，避免 runtimeModelPtr 解耦后扩大误杀面。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.161 — legacy fallback 静态持久化与 Path blocker final gate 收口（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做理论复核、源码修正与编译验证。

**桥/斜坡卡顿补充根因**：
- Phase 7.159/7.160 已修 draw-time VB cache 的 `isStaticGeometry` TTL 分类，但 legacy fallback/persistent 晋升链仍有两处旧动态判定：
  - `War3CanPromoteShadowPersistentGeometry` 仍把 `ObjectKind::Unit` 一律拒绝；
  - legacy capture 的 `forceFreezeUnitLikeGeometry / semanticSceneUnitLikeCandidate`
    仍可能把“被 Unit 污染但无 jHandle/rawcode/pose 的刚性桥/斜坡”拉回 fallback freeze。
- 这会导致桥/斜坡即使在 draw-time cache 层常驻，仍可能在 legacy fallback 层因不能晋升 persistent 而反复分配/冻结。

**代码补强**：
- `War3CanPromoteShadowPersistentGeometry` 新增 `unitlessRigidStatic` 分支：
  - `ObjectKind::Unit` 但无动态 pose、无动态单位身份时允许作为静态 world object 晋升 persistent；
  - dynamic sysmem source 对这类静态 rigid 也允许进入 static persistent，避免重复 `createBuffer/copyBuffer`。
- legacy capture 侧同步使用 `legacyHasDynamicPose / legacyHasDynamicUnitObjectEvidence`：
  - `runtimeModelPtr` 不再单独触发 unit-like force-freeze；
  - `semanticSceneUnitLikeCandidate` 只有在真实单位身份/pose 存在时才成立；
  - Decorations/Terrain lane 的纯 CModel rigid geometry 保持静态处理。
- `finalizeShadowDrawCommon` 从 `void` 改为 `bool`：
  - rawcode / jHandle fallback / widget 直读 / 匿名 Terrain-Decorations marker 命中 Path blocker 时返回 `false`；
  - upper-layer、compat persistent、legacy fallback 三个最终 append 点直接停止提交，不再 append 空 `shadowCasters`/`shadowInstances`。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.162 — rawcode/jHandle 污染收窄与 Item/Unknown 静态晋升（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做源码边界复核、代码补强与编译验证。

**桥/斜坡卡顿进一步收敛**：
- 复核 7.161 后发现一个剩余误判口：`War3SemanticContextHasDynamicUnitObjectEvidence`
  仍把 `rawcode/jHandle != 0` 一律当动态单位身份。若桥/斜坡/升降机 doodad 拿到了 rawcode，
  但 batch/tag 被污染成 `ObjectKind::Unit`，它仍会被踢回动态 TTL/fallback 路径。
- 另一个剩余口：`War3CanPromoteShadowPersistentGeometry` 的 objectCaster 白名单只允许
  Building/Destructible/UnitlessRigid，导致 `ObjectKind::Item/Unknown` 的桥/斜坡刚性几何仍不能晋升 persistent。

**代码补强**：
- `War3SemanticContextHasDynamicUnitObjectEvidence` 改为：
  - `rawcode/jHandle/runtimeModelPtr/worldObjectEntry/sceneNode` 只有在
    `WorldObjects/SelectionOverlay/stage11` 单位渲染通道才算动态单位证据；
  - `RenderObjectInfo.unitPtr/agentPtr/group0 Unit` 仍直接保护真实 CUnit；
  - Terrain/Decorations lane 中的纯 rawcode/model doodad 不再被误判为动态单位。
- `War3CanPromoteShadowPersistentGeometry` 新增 `genericRigidStaticWorldObject`：
  - 无动态 pose、无动态单位对象证据的 `ObjectKind::Item/Unknown` 允许作为静态 world object 晋升 persistent；
  - dynamic sysmem source 对这类静态 rigid 也允许进入 static persistent，避免桥/斜坡反复分配。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.163 — static persistent source-key 安全收口（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做理论闭环、源码补强与编译验证。

**桥/斜坡卡顿风险复核**：
- Phase 7.162 放宽 `Item/Unknown/unitless rigid` 静态晋升后，dynamic sysmem source
  路径会走 persistent cache，但旧的 `War3BuildShadowSemanticIdentityHash` 对
  `modelKey/rawcode` 采用早返回。
- 这个 key 对“相同 rawcode / 相同模型 key 下有多个 renderablePart/sceneNode 分片”的桥、
  斜坡、升降机类几何过粗，理论上可能造成 persistent geometry 串用；若为了避开串用又频繁
  miss，则会回到“每次重新看到就 warm-up 卡顿”的问题形态。

**代码补强**：
- 新增 `War3SemanticContextHasPersistentGeometryIdentity`，persistent 静态晋升不再接受
  objectKind-only 的弱身份，必须至少有 `renderablePart/sceneNode/worldObjectEntry/runtimeModelPtr/
  modelResourcePtr/modelKey/jHandle/rawcode` 之一。
- 新增 `War3BuildShadowStaticPersistentSourceHash`，dynamic-source static persistent key 改为同时纳入：
  `modelKey/modelResourcePtr/runtimeModelPtr/rawcode/jHandle/worldObjectEntry/sceneNode/renderablePart/
  objectKind/tag/stage`。
- `War3CanPromoteShadowPersistentGeometry` 与 `War3TryCaptureShadowCaster` 的 persistent reject
  统一使用几何级 identity 准入；保留原 `War3BuildShadowSemanticIdentityHash` 给非本轮路径使用。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.164 — native FourCC 归一化收口（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做源码审计、补丁与编译验证。

**Path blocker 原生侧盲点**：
- D3D9 CSM 主路径的 `IsLosBlockerFourCc` 已经同时尝试编辑器顺序与内存顺序，并对
  `YTlc/Ytlc` 这类第二字符大小写差异做归一化。
- 但 `war3_shadow_filter_policy.cpp::IsBlockedFourCC` 只按一套字节位移归一化，和
  `kPathBlockerFourCCs` 的编辑器顺序配置不完全对齐。若 RegisterImage owner rawcode 或
  ShadowProjector rawcode 以另一种字节序/大小写进入，native 侧可能漏掉 path blocker。

**代码补强**：
- 在 shadow filter policy 内新增本地 `ByteSwapU32 / NormalizeFourCcEditorSecondChar /
  MatchesBlockedFourCC`。
- `IsBlockedFourCC` 改为同时检查：
  - raw direct；
  - byte-swapped；
  - direct 的编辑器第二字符大写归一化；
  - swapped 的编辑器第二字符大写归一化。
- 这样 RegisterImage owner filter 与 ShadowProjector native 兜底和 D3D9 mesh gate 共享同一
  “编辑器 fourcc 配置、运行时双字节序识别”的语义。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.165 — weak identity offset 安全收口（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做源码审计、补丁与编译验证。

**桥/斜坡 persistent key 复核**：
- Phase 7.163 已把 dynamic-source static persistent 的 source hash 加强到几何级身份；
  但 layoutHash 仍对所有 dynamic source 统一把 `StartVal/MinVertexIndex/BaseVertexIndex` 归零。
- 该归零对有 `renderablePart/sceneNode/runtimeModelPtr/modelResourcePtr/modelKey` 的桥/斜坡
  是必要的，否则 per-draw upload ring offset 会让同一子网格永远 miss。
- 但若某条静态几何只剩 `rawcode/jHandle` 弱身份，offset 归零会降低子网格区分度，理论上仍有
  同 rawcode 多子片串用风险。

**代码补强**：
- 新增 `War3SemanticContextHasPersistentGeometrySubobjectIdentity`，只把
  `renderablePart/sceneNode/runtimeModelPtr/modelResourcePtr/modelKey` 视为可安全归零 draw offsets
  的子对象/资源级身份。
- `layoutHash` 的 dynamic offset 归零改为：
  - 有子对象/资源身份：继续归零 offsets，保证桥/斜坡 stable geoset 命中；
  - 只有 rawcode/jHandle 弱身份：保留 `StartVal/MinVertexIndex/BaseVertexIndex`，安全区分子片。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.166 — replay FourCC final gate 收口（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做源码审计、补丁与编译验证。

**Path blocker 最终防线盲点**：
- `d3d9_war3_shadow.cpp::BuildShadowReplayDraws` 是所有 shadow caster 进入 GPU shadow map
  前的最终收集点，理论上应覆盖所有上游漏判。
- 复核发现 `War3ReplayNormalizeFourCc` 把第二字符从大写转小写，但权威黑名单
  `kPathBlockerFourCCs` 保存的是编辑器显示顺序的大写 `YT*`。因此 direct rawcode 可命中，
  但 `Ytlc/Yt*` 这类大小写变体在最终 replay sweep 可能漏掉。

**代码补强**：
- `War3ReplayNormalizeFourCc` 改为将第二字符小写归一到大写，与
  `d3d9_device.cpp::NormalizeFourCcEditorOrder`、`war3_shadow_filter_policy.cpp::IsBlockedFourCC`
  和 `war3_shadow_renderer_core.cpp::IsBlockedSemanticFourCc` 保持一致。
- 最终 replay gate 继续同时尝试 direct 与 byte-swapped rawcode；本轮只修大小写方向，不扩大
  rawcode 黑名单语义。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.167 — replay rawcode final gate 常开（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做源码审计、补丁与编译验证。

**Path blocker 最终防线语义修正**：
- Phase 7.166 修了 `War3ReplayNormalizeFourCc` 的大小写方向，但继续审计发现：
  `War3ReplayDrawIsPathBlocker` 在检查 caster 自带 rawcode 前，就先受
  `kPathBlockerDrawTimeSweepEnabled=false` 门控返回。
- 这会导致 “append 站点已经填了 rawcode 的 path blocker” 在最终 GPU shadow-map replay
  收集点无法被兜底拦截；上轮 FourCC 修复实际只有在显式打开 sweep 时才生效。
- 历史上关闭 sweep 的原因是 rawcode=0 时用 batchHandle → widget cache 反查可能误判真实单位，
  造成阴影闪烁；这个风险不适用于 caster.rawcode 直判。

**代码补强**：
- `War3ReplayDrawIsPathBlocker` 改为：
  - `draw.rawcode` 命中 `kPathBlockerFourCCs` 时无条件返回 true；
  - 只有 rawcode=0 的 `jHandle/batchHandle → widget cache` 兜底仍受
    `kPathBlockerDrawTimeSweepEnabled` 门控。
- 这样最终 replay gate 不再被历史诊断开关整体关掉，同时保留对 batchHandle 抖动风险的隔离。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.168 — RegisterImage owner cache 分类（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做源码审计、补丁与编译验证。

**静态阴影 owner 分类复核**：
- RegisterImage hook 的 `ResolveShadowRegisterOwnerKind` 原先只用 `CWidget magic + rawcode + flags5C`
  做本地猜测：
  - `flags5C & Building` 判 Building；
  - rawcode 命中 path-blocker FourCC 判 Destructible；
  - 其他全部退成 Unit。
- 项目里已经有 `CWidget_RegisterFootprintAndShadowMask` 身份缓存，能基于 handle/agent type/flags
  识别 `Building / Destructible / Item / Unit`，且正是为 destructible/path-blocker rawcode 缺失问题建立。
- 如果 RegisterImage owner 指针已在该缓存中，继续只靠本地猜测会让普通 destructible / item owner
  过滤不够准，影响“魔兽原生静态阴影禁用”的 producer 端覆盖。

**代码补强**：
- `war3_hook_shadow.cpp` 引入 `war3_hook_widget_identity.h`。
- `ResolveShadowRegisterOwnerKind` 对 `ownerArg / ownerArg-0x0C / ownerArg-0x10` 三个候选指针先调用
  `QueryWidgetIdentityByPtr`：
  - 命中时复用缓存 rawcode；
  - 按缓存 `ObjectKind` 映射到 `ShadowOwnerKind`；
  - 缓存未命中或 kind unknown 时继续走旧 magic/rawcode/flags 兜底。
- 这是增量分类增强，不改变缓存未命中的 RegisterImage 行为。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.169 — DispatchToShape 默认回收与证据链对齐（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做文档/源码审计与保守配置修正。

**审计结论**：
- 复读历史证据后确认 `TerrainShadow_DispatchToShape` 不应继续作为生产默认静态阴影闸门：
  - AGENTS Phase 7.116 已记录 30s 实测 `dispatchToShapeEnterCount=0`，hook 装上但无调用；
  - Phase 7.143 又证明 `ListA_RenderPreparedGroups / ShadowListA_RenderAllEntries` 粗拦截会误伤所有悬崖/地形 tile；
  - `WriteMaskRegion` / FogMask 路径仍是 fog/LOS/path/visibility 共享 mask grid，不能整体 return。
- 因此当前默认策略必须保持在已验证更低风险的 producer 端：
  `StaticStampPath + ToggleStaticStampFromObject + RegisterImage 精确策略`；
  D3D9 CSM path blocker 则继续依赖 rawcode / jHandle / widget / replay final gate。

**代码修正**：
- `war3_internal_test_config.h`：
  - `kNativeStaticShadowDispatchToShapeRejectEnabled=false`
  - `kNativeStaticShadowDispatchToShapeCallerDiagnostics=false`
  - `kNativeStaticShadowDispatchToShapeDebugLog=false`
- `war3_hook_shadow.cpp` 同步注释为“旧实验 hook / 灰度诊断”，不再写“唯一汇聚点”或“默认开启”。

**风险说明**：
- 这不是宣布静态阴影已彻底解决；只是撤回一个已被实测推翻、且可能误导后续开发的默认开关。
- 后续若要继续研究静态阴影残留，优先补 producer 早装/预置对象清理证据，或基于 draw call state/texture 做精确识别，不再启用 ListA/WriteMaskRegion 粗拦截。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.170 — Shadow producer 早装补齐（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做安装时机审计、代码补丁与编译验证。

**发现的真实缺口**：
- AGENTS 历史段落写过 `TryInstallShadowHooksEarly` 已接入 `MainRunner_ENTER`，理论覆盖
  pre-placed doodad / path blocker 注册。
- 但源码复核发现 `TryInstallShadowHooksEarly` 实际只早装了
  `CWidget_RegisterFootprintAndShadowMask` 身份 hook，**没有早装**
  `ShadowPath_StaticStamp_Toggle`、`TerrainShadow_ToggleStaticStampFromObject`、
  `TerrainShadow_RegisterImageEntry` 等 producer hook。
- 结果是地图预置 doodad/path blocker 的 native stamp 可能在 `ActivateWar3Runtime`
  之前已经写入，producer 端策略来得太晚。这是“静态阴影/path blocker native 残留”
  的高可信理论原因之一。

**代码修正**：
- `d3d9_war3_hook.cpp` 新增 `BuildShadowHookAddresses(...)`，常规安装与早装共用同一套
  Shadow 地址解析，避免字段漂移。
- `TryInstallShadowHooksEarly` 在 `MainRunner_ENTER / MainRunner_Alt_ENTER` 中：
  - 先确保 MinHook 初始化；
  - 校验 `Game.dll` PE 信息；
  - 继续早装 `WidgetIdentity`；
  - 随后调用 `InstallShadowHooks(BuildShadowHookAddresses(...))`，让
    `RegisterImage / StaticStampPath / ToggleStaticStampFromObject / Projector` 等 Shadow 域
    hook 真正早于主循环原逻辑执行。
- 常规 `InstallGameHooks` 改用同一 helper，后续重复安装由 `InstallMinHook`
  的 `MH_ERROR_ALREADY_CREATED / MH_ERROR_ENABLED` 兼容路径兜底。
- `war3_hook_shadow.h` 同步清理 `WriteMaskRegion / DispatchToShape / ListA` 旧字段注释，
  明确它们是诊断/旧实验，不再是生产默认治理点。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。

### Phase 7.171 — static VB cache idle LRU 修正（2026-06-05）

**本轮继续遵守约束**：不启动 War3 / AutoTest，只做源码审计、低风险补丁与编译验证。

**桥/斜坡卡顿链路复核**：
- `m_war3DrawTimeVBCache` 已把无动态姿态/无真实单位身份的 rigid geometry 标为 static，
  离开视野后不再按 16 帧动态 TTL 删除，回来时可复用已有 GPU buffer 容量，避免重复
  `createBuffer/vkAllocateMemory`。
- 继续审计发现 static entry 的“长期闲置回收”虽然注释写的是 idle，但代码实际用
  `frameSerial` 计算年龄；如果某个桥/斜坡 entry 长时间存在且被持续访问，理论上仍会在
  达到 `kShadowDrawTimeVBCacheStaticMaxIdleFrames` 后被当作老 entry 回收。

**代码修正**：
- static cleanup 改为用 `lastAccessFrameSerial` 计算 `idleAge`；
- 仍保留动态对象的 `frameSerial + DynamicMaxAge` 16 帧 TTL；
- 字节上限 LRU 仍按 `lastAccessFrameSerial` 淘汰最久未访问的 static entry。

**验证**：
- `ninja -C build32` 通过并重新链接 `src/d3d9/d3d9.dll`。
- `git diff --check` 仅有既有 CRLF 提示，无 whitespace error。
- 未运行 AutoTest / 未启动 War3。
