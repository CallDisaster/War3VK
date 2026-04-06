# 研究方向四：框架重构（Hook 架构与桥接层）

## 背景与目标
当前专项状态为“方向1/3继续推进、方向2（LOSBlocker）进入维护”，但公共框架层（Hook 安装、地址管理、生命周期、日志与降级）耦合偏高，导致：
1. 变更成本高：同一问题常跨 `d3d9_war3_hook.cpp`、`war3/render/*`、`war3/hooks/*` 同时改。
2. 回归风险高：某条链路修复容易影响其它链路（典型是句柄兜底与描边污染）。
3. 诊断成本高：运行时统计与开关分散，难快速定位“是地址错、时序错、还是数据错”。

本方向目标：在不打断专项推进的前提下，逐步把“能跑”演进为“可维护、可验证、可扩展”的框架。

## 现状证据（2026-02-22）
1. 主入口已显著瘦身：`src/d3d9/d3d9_war3_hook.cpp` 从约 1400+ 行降至约 540+ 行（以编排为主）。
2. 分域 Hook 已进入可用态：`render/lifecycle/jass/ui/shadow` 全部纳入编译并由中枢装配。
3. 多域调用链已从“同文件内混排”收敛为：
    - 渲染调度（RenderQueue/WorldDispatch）
    - 阴影策略（Projector/ListA/ListB/WriteEntry）
    - 对象桥接（SceneCollector/ExecBatch/HandleResolver）
    - UI/FPS/JASS 初始化
4. 诊断与保护策略仍有分散问题，下一步需要统一“故障分级 + 开关矩阵”。

## IDA MCP 对框架重构的约束
1. 阴影写入入口必须以 `Add_Simple/Add_FromObject -> sub_6F713CA0` 为主轴，不能只依赖末端 List 过滤。
2. `CWorld_TerrainShadow_Dispatch(stage14)` 存在 `RenderListB(type=4)` 直调，决定了“只改 RenderLayer 参数”不可作为唯一方案。
3. `RenderQueue_Dispatch_Common` 固定包含世界矩阵更新，意味着性能优化优先级应为“减少调用次数”，不是微调单次开销。

## 重构原则
1. 先分层，再迁移：先建立统一边界，再搬迁逻辑，避免边改边乱。
2. 保持默认行为稳定：所有新路径均提供安全回退开关。
3. 统计先行：每次迁移必须带最小可观测指标（调用次数/命中率/回退原因）。
4. 单次只收敛一个问题域：避免“合批+阴影+桥接”一次性大改。

## 目标架构（建议）
1. `war3/hooks/framework/`（新增）
   - `hook_address_book.h/.cpp`：按版本集中管理 RVA 与地址有效性校验。
   - `hook_registry.h/.cpp`：统一 Create/Enable/Disable/Status，统一错误日志格式。
   - `hook_feature_gate.h`：集中管理开关与运行时降级策略。
2. `war3/hooks/domain/`（分域）
   - `hook_domain_render.*`
   - `hook_domain_shadow.*`
   - `hook_domain_ui.*`
   - `hook_domain_jass.*`
3. `war3/runtime/`
   - `runtime_lifecycle.*`：Bootstrap、GameReady、GameExit、Reset。
   - `runtime_diagnostics.*`：统一统计输出与采样窗口。
4. `war3/bridge/`
   - 把 `SceneCollector/ExecBatch/RenderObjectRegistry` 的契约（输入/输出/所有权）文档化并类型化。

## 分阶段计划表
| 阶段 | 时间窗 | 目标 | 主要产出 | 验收标准 |
|---|---|---|---|---|
| P0 稳定性止血 | 立即-1天 | 降低崩溃与误伤风险 | WorldObjects 组范围保护、列表可读性校验、关键地址探测校验 | 进图见单位不崩；`ninja -C build32` 稳定通过 |
| P1 Hook 框架底座 | 1-3天 | 统一地址与安装接口 | `hook_address_book`、`hook_registry` 初版 | 新增域可不改主入口直接注册 Hook |
| P2 域内解耦 | 3-7天 | 把渲染/阴影/UI/JASS 安装彻底分域 | 主入口只保留编排与生命周期 | `d3d9_war3_hook.cpp` 行数显著下降 |
| P3 桥接契约化 | 1周+ | 稳定 `SceneCollector -> ExecBatch -> ShadowCapture` | 桥接结构体与状态机文档 + 断言/统计 | `noObj/raw=0` 可解释、可追踪、可回归 |
| P4 性能专项并行化 | 持续 | 以框架为底推进合批与阴影策略 | 回退原因报表、对照压测脚本 | `FQ_Dispatch_Opaque` 与阴影误渲染指标同步改善 |

## 首批落地任务（开始执行）
1. 统一 Hook 地址来源：
   - 新增 `address_book`，把 JASS/UI/Shadow/Render 的关键 RVA 全部集中。
2. 统一 Hook 安装返回值与日志：
   - 记录 `target/detour/createStatus/enableStatus`，支持统计“安装成功率”。
3. 收敛主入口职责：
   - `d3d9_war3_hook.cpp` 只负责生命周期编排，不再承载域内策略细节。
4. 桥接链路最小契约文档：
   - 明确 `jHandle/unitPtr/sceneNode/batchHandle` 的字段语义、来源、回退顺序。

## 2026-02-22 本轮已落地（P1/P2 落地）
1. 已落地：主入口“薄中枢化”
   - `d3d9_war3_hook.cpp` 仅保留生命周期编排、版本门禁、地址探测、分域装配；
   - 移除渲染/JASS/生命周期域内重复 Hook 实现。
2. 已落地：分域 Hook 接线
   - `war3_hook_render.cpp`、`war3_hook_lifecycle.cpp`、`war3_hook_jass.cpp` 纳入正式构建；
   - `InstallGameHooks` 通过分域模块统一安装 Render/UI/Shadow；
   - `InstallHooks` 通过分域模块统一安装 JASS/Lifecycle bootstrap。
3. 已落地：AddressBook 标准化
   - 新增 `src/d3d9/war3/hooks/war3_hook_address_book.h/.cpp`；
   - 集中维护 1.27a RVA（Bootstrap/Render/Shadow/RenderQueue/Data 区）；
   - 各域模块与中枢改为从 AddressBook 读取偏移，减少散落硬编码。
4. 已落地：兼容层补齐
   - 补齐 `War3HookLifecycle::GetTrampolineFlushAndReset()`；
   - 补齐 render/lifecycle 依赖的内部测试开关兼容常量；
   - 统一了 `stage_tag_map` 与现有 `War3BatchTag` 枚举的兼容映射。
5. 已落地：编译回归
   - `ninja -C build32` 通过。

## 2026-02-22 早前已落地（P0 启动）
1. 已落地：渲染层 FourCC 桥接验证通路（低侵入）
   - `RenderObjectRegistry` 增加 `jHandle -> rawcode` 缓存桥接；
   - 解析失败句柄负缓存，避免每帧重复慢路径；
   - 每种 FourCC 仅打印一次，日志含来源字段（`unitPtr/handleCache/handleResolve`）。
2. 已落地：验证期全量收集开关
   - 通过 `kBridgeRawcodeForceTrackAllEnabled` 在验证期强制关闭过滤。
3. 已落地：编译验证
   - `ninja -C build32` 通过。

## 分阶段重构执行清单（性能优先、避免过度抽象）
1. S0（进行中）桥接可观测化
   - 目标：先把“数据是否可信”做实，不动重型架构。
   - 产出：FourCC 一次性日志、桥接来源统计、样本覆盖验证。
2. S1（已完成）Hook 注册统一化
   - 目标：仅收敛“地址解析 + Create/Enable + 失败降级”，不改域逻辑。
   - 产出：AddressBook + 分域 Install 装配，替换主入口重复安装代码。
3. S2（已完成）按域拆分入口
   - 目标：`d3d9_war3_hook.cpp` 只保留编排与生命周期。
   - 产出：`render/shadow/ui/jass/lifecycle` 分域安装函数；主入口只调用。
4. S3（后续）桥接契约固化
   - 目标：明确 `SceneCollector -> ExecBatch -> Registry -> ShadowCapture` 数据契约。
   - 产出：字段语义文档 + 断言 + 统计，减少“猜句柄”回归。

以上拆分策略保持“薄中枢 + 厚域内”，避免为了解耦引入额外虚层和运行时分派开销。

## 2026-02-24：P3 契约化拆分（render hook 子模块图）
本轮按“只拆结构、不改语义”推进 render 域契约化，目标是把 `war3_hook_render.cpp` 收敛为编排层。

### 子模块图（落地后）
```text
war3_hook_render.cpp  (Hook 编排层)
  ├─ war3_dispatch_contract.*         (Dispatch 查询/缓存契约)
  │   ├─ DispatchLocalMergeState
  │   ├─ DispatchTagStageCacheState
  │   └─ QueryTagStageCached(...)
  ├─ war3_queue_takeover_policy.*     (接管策略契约)
  │   ├─ War3QueueTakeoverDecision
  │   ├─ HasTransparentTakeoverPrerequisites(...)
  │   └─ ShouldUseConservativeQueueTakeover(...)
  └─ war3_render_queue.* / RenderQueueTracker / ExecBatchProcessor
```

### 拆分结果
1. `war3_hook_render.cpp` 行数：`1721 -> 1201`（约 `-30.2%`）。
2. Hook 安装、trampoline、调用编排留在 `war3_hook_render.cpp`。
3. 策略/缓存细节迁移到契约模块，降低主文件耦合度。

### 风险清单（本轮）
1. include 依赖环风险：通过“契约头仅暴露必要类型 + 实现放 cpp”规避。
2. 状态双份维护风险：`Dispatch*State` 仅保留契约层单实例，render 侧只访问接口。
3. 语义回归风险：保留原有调用顺序与返回值语义，不调整默认阈值。

### 回滚点
1. `war3_dispatch_contract.*` 可独立回滚，不影响 Hook 安装层。
2. `war3_queue_takeover_policy.*` 可独立回滚，恢复 `war3_hook_render.cpp` 内嵌策略。
3. 失败时按模块粒度回滚，不需要整体回灌 render 域。

## 风险与回退
1. 风险：迁移中可能出现“同一 Hook 重复安装”。
   - 回退：`hook_registry` 强制幂等检查（地址级唯一）。
2. 风险：时序变化导致 JASS/HandleResolver 初始化早晚不一致。
   - 回退：保留懒初始化 + 一次性重试路径。
3. 风险：统计开关过多导致调试复杂。
   - 回退：统一到 `runtime_diagnostics` 做级别管理。

## 与三方向专项的关系
1. 对方向1（合批）：提供稳定的性能统计与回退原因归因框架。
2. 对方向2（LOSBlocker，维护态）：保持桥接字段语义稳定，避免回归导致阻断器阴影复现或描边链路污染。
3. 对方向3（静态阴影）：统一投影入口与 ListB 分支策略，减少误伤回归。
