# Semantic Shadow Control Plane Status

Date: 2026-04-17

## 1. 本轮已经落地并实机验证的内容

### 1.1 DLL named pipe control plane 已上线

当前 DLL 已提供 `War3ControlPlane`，pipe 名称固定为：

`\\.\pipe\War3ControlPlane_<pid>`

已实现并实机验证的命令：

1. `ping`
2. `get_runtime_status`
3. `wait_until`
4. `get_shadow_runtime_summary`
5. `get_frame_manifest_summary`
6. `capture_final_frame`
7. `invoke_test_command`
8. `shutdown_session`

实机验证点：

1. DBWIN 已观察到 `DXVK War3Control: pipe=\\\\.\\pipe\\War3ControlPlane_<pid> online`
2. Python 直接通过 `CreateFileW/ReadFile/WriteFile` 请求 `ping/get_runtime_status/...` 成功
3. `capture_final_frame` 已成功导出最终帧截图

### 1.2 AutoTest 默认部署路径已修正

`AutoTest/war3_autotest_mcp.py` 现在会把默认的：

`build32/src/d3d9/d3d9.dll`

自动解析到 repo 根目录下的绝对路径，不再依赖 MCP 服务自身的工作目录。

这修复了命名场景第一次启动时出现的：

`构建产物不存在: build32\\src\\d3d9\\d3d9.dll`

### 1.3 runtime status / ready 判定已在游戏内跑通

DLL 当前会稳定写出：

`WarVK/Temp/runtime_status.json`

同时 control plane 的 `get_runtime_status` 已能返回下面这组 ready 条件：

1. `module.state = Running`
2. `runtime.jassReady = true`
3. `runtime.runtimeReady = true`
4. `runtime.gameStarted = true`
5. `render.inGameRenderReady = true`

实机验证时，以上字段全部返回真值，并且最终帧截图确认已真实进图。

## 2. Shadow contract / semantic core 当前状态

### 2.1 contract cache 已独立成型

当前已独立出以下权威 contract：

1. `ShadowFrameManifest`
2. `ShadowModelResourceStore`
3. `ShadowPoseStore`
4. `ShadowFrameStats`

这些结构位于：

`src/d3d9/war3/shadow/war3_shadow_runtime_contract.*`

当前数据来源：

1. `VisibleRenderableRegistry`
2. `ShadowModelResourceCache`
3. `PoseRegistry`
4. `QueryShadowRuntimeBridgeSummary()`

### 2.2 ShadowRendererCore 已按 observe 模式每帧真实运行

当前 `War3Renderer::EndFrame()` 在发布 contract 后，会驱动：

1. `ShadowRendererCore`
2. `DxvkValidationBackend`

这里仍然是 observe/validation 模式，不直接替换现有 object shadow draw。

### 2.3 空帧覆盖问题已修复

本轮发现一个关键时序问题：

1. 有些 frame 会发布“空 manifest”
2. 它会把上一帧完整 contract 冲掉
3. 导致 control plane 与 semantic core 在异步读取时看到 `visible=0`

当前修复策略：

1. `BeginFrame()` 不再清空最后一帧已发布 contract
2. `captureLiveState()` 遇到空帧时，不覆盖上一帧有效 contract

修复后连续采样结果已经稳定，不再出现“语义核心隔帧变 0”的抖动。

## 3. 2026-04-17 实机验证结果

### 3.1 Control plane 直接请求结果

对进图后的 War3 进程直接发 pipe 请求，已验证：

1. `ping` 返回 `protocolVersion=1`
2. `get_runtime_status` 返回 ready 态
3. `get_frame_manifest_summary` 返回非零 visible manifest
4. `get_shadow_runtime_summary` 返回非零 semantic core 统计

### 3.2 Semantic core 解析结果

在 `光影测试.w3x / full_analysis / isolated desktop` 的实机采样中：

早期版本：

1. `semanticCoreResolved = 6`
2. `semanticCoreSubmittedDrawCount = 6`

在补上：

1. 空帧不覆盖 contract
2. multi-group palette 的 `direct matrix remap / direct pose palette` fallback

之后，先提升到：

1. `semanticCoreConsidered ≈ 950`
2. `semanticCoreResolved = 49`
3. `semanticCoreSkinnedResolved = 49`
4. `semanticCoreSubmittedDrawCount = 49`

继续补上：

1. `runtimeModel` 侧 geoset alias 全量纳入 contract
2. 稀疏 `vertex_group_indices` 的 direct/sparse remap fallback
3. `UpperLayerShadowRegistry` 的去重 authoritative resolved item 固化
4. validation runtime 对这批 resolved item 的补充消费能力

后，最新实机采样继续提升到：

1. `semanticCoreResolved = 93~95`
2. `semanticCoreCoreDrawPacketCount = 93~95`
3. `semanticCoreSubmittedDrawCount = 93~95`

并且当前已经能稳定拿到：

1. `semanticCoreUpperLayerResolvedItems ≈ 39~40`（当前帧去重后的 upper-layer authoritative item）
2. `upperLayerResolvedAuthoritativeItems > 1000`（累计计数，说明 live 语义链一直在工作）

这说明：

1. 新的 host-independent semantic core 已经不是“只编译不运行”
2. 它已经能在真实游戏帧上稳定吃到 manifest/resource/pose
3. 并且已经能独立构建并提交一批 skinned draw packet
4. 当前 observe 路线的可消费对象数已经明显高于早期版本的 `6 -> 49 -> 95`

### 3.3 semantic packet -> DXVK scene 提交已落地（units-first）

本轮又向前推进了一步：

1. `War3MaybeInsertBeforeUi()` 现在会在提交 `War3PipelineInput` 之前，直接消费 `ShadowValidationRuntime` 里缓存的 `ShadowSubmissionFrame`
2. 当前接管范围先锁定为 `units-first`
3. 语义包会被直接落成：
   - `m_war3Scene.shadowInstances`
   - `m_war3Scene.shadowCasters`
4. 运行时新增计数：
   - `semanticSceneSubmitted`
   - `semanticSceneSubmittedUnit`
   - `semanticSceneSubmittedSkinned`

这一步的意义是：

1. 新链路已经不只是 observe 统计
2. 它已经开始把上层语义 packet 变成 DXVK 宿主里的真实 shadow replay draw
3. 当前 object shadow 的单位路径，已经开始具备“完全不依赖 VB/IB snapshot 本身来提供 geometry”的宿主消费能力

### 3.4 单位 legacy capture 已前置旁路，fallback 数量明显下降

为了避免“单位同时走 semantic scene 和 legacy freeze/capture”形成双份成本，本轮新增了单位优先旁路：

1. 对 `explicit Unit`
2. 以及 `Unknown + runtimeModel + skinned` 的单位候选

当前会优先跳过旧的 unit-like legacy capture，并把同类 legacy skinned fallback 从最终 scene 里清掉。

最新几轮自动回归报告显示：

1. `semanticBridgeBypassed` 已经稳定升到 `3900+`
2. `fallbackDrawCount` 已从早期 `7334` 明显降到 `1783`
3. `semanticSceneSubmitted = semanticSceneSubmittedUnit = semanticSceneSubmittedSkinned = 144`（7 帧窗口）

这说明：

1. “单位仍完整走旧 fallback”这一条已经不成立
2. units-first semantic scene submission 已经真正替掉了一大段旧 object capture 热路径
3. 但它还没有把最终视觉和性能一起收稳

### 3.5 AutoTest 端到端链路已再次跑通

本轮还重新跑通了一次：

`run_quick_autotest`

结果：

1. 能成功启动、等待 ready、截图、停服、导出性能报告
2. 说明当前项目内的自动化主链已经恢复到“可执行”状态
3. 即使 MCP 服务进程本轮返回的 ready mode 仍显示为旧的 `debug-events`，DLL 侧 control plane / pipe 请求在实机中已经单独验证通过，说明代码路径本身可用；服务重启后应切到 pipe-first 实现

## 4. 当前仍未完成的目标

以下目标仍未完成，不能误判为已 fully cutover：

1. object shadow 默认路径仍未切成 `ShadowRendererCore` authoritative
2. 旧 `VB/IB snapshot/freeze` 仍存在于默认 object shadow fallback
3. `NativeD3D9Backend` 目前只有宿主接口骨架，还没有真正的阴影绘制实现
4. 虽然 `semanticSceneSubmitted*` 已经非零且 legacy unit capture 已被大幅旁路，但“普通单位阴影在视觉上稳定可见”这条还没有被最终确认
5. 当前 `avgFps` 仍停留在 `0.x ~ 1.x` 的不可用区间，说明 units-first semantic submit 还没有解决当前分支的主性能问题
6. `wait_for_game_ready` 的 MCP 服务进程是否已热重载到 pipe-first 逻辑，还需在服务重启后再确认

## 5. 当前最可靠的结论

1. AutoTest 的“游戏侧控制面”现在已经具备 named pipe 方案，不再只能依赖 DBWIN / 文件轮询
2. semantic-only shadow 路线现在已经有独立 contract 与独立 core，并且已在 DXVK 宿主里做了真实帧验证
3. 新链路现在已经进一步走到“可以把 semantic packet 真正落成宿主 shadow draw”的阶段，不再只是 validation
4. 当前最大的剩余硬工作，已经转成两件事：
   - 继续把 `units-first` 这条 semantic scene 消费链收成视觉正确
   - 找出当前 `0.x ~ 1.x FPS` 的主热源，把 legacy 残留与无效重复工作继续剥掉
5. 本轮代码已经为后续 native D3D9 / 晚注入宿主保留了干净的接口边界，但 native backend 仍未到可用态
