# Semantic Shadow Control Plane Status

Date: 2026-04-19

## 1. 本轮已落地

### 1.1 AutoTest 外部控制面已收口为 single-entry

当前 `AutoTest/war3_autotest_mcp.py` 对外只再走 DLL named pipe control plane：

1. `wait_for_game_ready -> wait_until`
2. `read_runtime_status -> get_runtime_status`
3. `invoke_internal_test_api -> invoke_test_command`
4. `capture_war3_screenshot -> capture_final_frame`

本轮已明确移除 AutoTest 对下列 legacy 主动入口的依赖：

1. `WarVK/Temp/internal_test_request.json`
2. `WarVK/Temp/internal_test_result.json`
3. `WarVK/Temp/frame_capture_request.json`
4. `WarVK/Temp/frame_capture_result.json`

这些文件名现在只保留给：

1. 清理旧工件
2. 离线诊断
3. 兼容旧日志/旧文档

也就是说，从这轮开始，项目内不再存在“外部控制面同时维护 named pipe + JSON 文件双入口”的状态。

### 1.2 `War3RuntimeStatusSnapshot` 已扩成真正的控制面总快照

当前 `get_runtime_status` 与 `WarVK/Temp/runtime_status.json` 除了原有：

1. `module`
2. `runtime`
3. `render`

之外，已经同步携带：

1. `frame`
2. `shadow`

其中新增的关键字段包括：

1. `frame.visibleCount`
2. `frame.recordsWithStableIdentity`
3. `frame.recordsWithResolvedGeoset`
4. `frame.recordsWithRuntimeModel`
5. `frame.unitCount`
6. `shadow.matrixPaletteCount`
7. `shadow.shadowReadyGeosetCount`
8. `shadow.shadowRuntimeModelCount`
9. `shadow.upperLayerResolveAuthoritativeSkinned`
10. `shadow.upperLayerEmitted`
11. `shadow.semanticCoreResolved`
12. `shadow.semanticCoreSkinnedResolved`
13. `shadow.semanticCoreSubmittedDrawCount`
14. `shadow.semanticCoreFrameFresh`

这让控制面不再必须额外拉：

1. `get_frame_manifest_summary`
2. `get_shadow_runtime_summary`

才能得到最基础的语义热度概览。

### 1.3 当前默认边界仍保持 observe-first

本轮没有改变现有的止血边界，默认仍然是：

1. `kUpperLayerShadowConsumerEnabled = false`
2. `kUpperLayerShadowConsumerObserveOnly = true`
3. `kShadowSemanticCoreSceneSubmissionEnabled = true`

因此当前仍是：

1. 上层 consumer 本体默认不直接 authoritative draw
2. 新语义核心继续真实运行并向 scene submission 提交单位优先 packet
3. 旧 object fallback 仍未被彻底删除

## 2. 本轮解决的问题

### 2.1 去掉了 AutoTest 的双协议歧义

此前仓库虽然已经有 control plane，但 AutoTest 对测试命令/最终帧截图仍保留了：

1. pipe-first
2. JSON 文件 fallback

这会让“控制面已切换完成”这个结论不够干净，也会继续把问题空间扩散成两套协议。

本轮之后：

1. 测试命令如果 pipe 不可用，会直接返回 control-plane unavailable
2. 最终帧截图如果 pipe 不可用，会直接返回 control-plane capture unavailable
3. 是否再退到窗口抓图，交给外层 screenshot 逻辑单独处理

### 2.2 runtime status 对自动化更可用了

当前 `runtime_status.json` 虽然仍是兼容输出，但内容已经升级成：

1. ready 判定基础字段
2. manifest 热度字段
3. shadow semantic 热度字段

这对后续三类自动化都有直接价值：

1. ready 后的热帧判断
2. 后台/隔离桌面下的摘要采样
3. 失败时的离线复盘

## 3. 仍未完成

下面这些点本轮没有误报成完成：

1. object shadow 默认 authoritative 路径仍未完全切到 `ShadowRendererCore`
2. 旧 `VB/IB snapshot/freeze` 仍未从 object shadow 仓库代码里删除
3. `NativeD3D9Backend` 仍未达到可绘制 object shadow 的可用态
4. 当前单位阴影形状错误与高 CPU 主阻塞仍未解除

## 4. 下一步最直接的推进面

基于本轮收口后，后续继续推进时可以直接依赖：

1. pipe-only 的测试命令控制面
2. 带 `frame/shadow` 摘要的 `get_runtime_status`
3. observe-first 的上层默认边界

下一步建议继续做：

1. 用新的 `runtime status shadow/frame` 快照把三套基线场景的验收脚本固化
2. 继续清理 object shadow 主路径里残留的 legacy snapshot/freeze authoritative 入口
3. 把 `NativeD3D9Backend` 从骨架推进到最小可画 object shadow 的真实后端

## 5. 2026-04-21 实机更新：Phase 4A 已签收，Phase 4B 热帧验证已恢复

### 5.1 本轮真正完成的事

1. `wait_for_game_ready(...)`
   - 现在在 `timeout_sec` 窗口内等待 pipe 出现
   - `require_control_plane_ready=true` 时仍只认 control-plane 成功，但不再在 pipe 尚未创建时瞬时误判失败
2. control-plane hot-frame 链
   - 已在三套命名场景上重新跑通：
     - `model_runtime_probe`
     - `low_pressure_static_reuse`
     - `dynamic_shadow_pressure`
3. `semanticCore` 饥饿态
   - 去掉了 build 进行中对更新 manifest 的中途 supersede
   - 当前 build 会继续推进，最新 pending contract 只留待下一轮消费
4. runtime lineage
   - `shadowRuntimeModelCount` 已从 `0` 拉到 `179`
   - 说明 `runtimeModel` 次级索引不再整条断掉

### 5.2 Phase completion timestamps

1. `Phase 4A`
   - 完成时间：`2026-04-21 13:52:27 +08:00`
   - 证据：
     - `AutoTest/artifacts/model_runtime_probe_latest.json`
       - `ready.mode=control-plane`
       - `hotShadow.ok=true`
     - `AutoTest/artifacts/model_runtime_probe_normal_ready_latest.json`
       - `ready.ok=true`
       - `ready.mode=control-plane`
2. `Phase 4B`
   - 完成时间：`2026-04-21 13:50:45 +08:00`
   - 证据：
     - `AutoTest/artifacts/model_runtime_probe_latest.json`
     - `AutoTest/artifacts/low_pressure_static_reuse_latest.json`
     - `AutoTest/artifacts/dynamic_shadow_pressure_latest.json`

### 5.3 当前 control-plane 侧已经成立的事实

1. AutoTest 的 ready acceptance 已真正只认 named pipe `wait_until`
2. 隔离桌面和普通桌面都已验证 `model_runtime_probe` 可 stable ready
3. `semanticCoreFrameFresh`
   - 已从“严格 `publishRevisionLag == 0`”调整到“至多落后一帧，且同帧 publish lag 受控”
   - 这是为了去掉同一 `frameSerial` 内重复 publish 放大的假陈旧
4. 三套场景当前都能通过新的 hot-frame 验证链，而不是只靠旧 fallback ready

### 5.4 仍未完成

1. `DxvkValidationBackend`
   - 仍未完成真正的 DXVK host backend cutover
   - object shadow 主路径依然主要挂在 `D3D9DeviceEx::War3TryAppendSemanticShadowPacket / War3TryPopulateSemanticShadowScene`
2. `shadowModelResourceCount`
   - 当前仍然是 `0`
   - 说明 direct `modelResource` 根对象还没有成为稳定事实源
3. `NativeD3D9Backend`
   - 仍未达到最小可画 object shadow 的真实后端
4. 晚注入 / native D3D9
   - 本轮没有推进到可独立运行的阶段

## 6. 2026-04-21 实机更新：Phase 4C 已前推到 backend 驱动提交，但还未签收

### 6.1 这轮前推了什么

1. `War3TryPopulateSemanticShadowScene(...)`
   - 不再自己遍历 semantic frame 再逐包 `War3TryAppendSemanticShadowPacket(...)`
   - 当前改成由：
     - `ShadowRendererCore::submitFrame(...)`
     - `DxvkValidationBackend`
     - DXVK host adapter
     共同驱动
2. `DxvkValidationBackend`
   - 现在会记录成功提交后的：
     - `normalized handle`
     - `worldObjectEntry`
     - `sceneNode`
     - `runtimeModelPtr`
   - fallback prune 改成消费 backend 的 submitted identity set
3. prune/perf 口径
   - `semanticFallbackPruned*` 现在会在实际 prune 路径上累计
   - 后续可以直接用 report 看 semantic 提交是否真的在清 legacy fallback

### 6.2 本轮验证结果

1. `ninja -C build32`
   - 通过
2. 三套命名场景重新通过：
   - `model_runtime_probe`
     - `hotShadowOk=true`
     - `semanticCoreFrameFresh=true`
     - `shadowRuntimeModelCount=179`
     - `matrixPaletteCount=182`
     - `semanticCoreResolved=122`
     - `semanticCoreSkinnedResolved=122`
   - `low_pressure_static_reuse`
     - `hotShadowOk=true`
     - `semanticCoreFrameFresh=true`
     - `shadowRuntimeModelCount=59`
     - `matrixPaletteCount=41`
     - `semanticCoreResolved=32`
     - `semanticCoreSkinnedResolved=32`
  - `dynamic_shadow_pressure`
     - `hotShadowOk=true`
     - `semanticCoreFrameFresh=true`
     - `shadowRuntimeModelCount=179`
     - `matrixPaletteCount=182`
     - `semanticCoreResolved=120`
     - `semanticCoreSkinnedResolved=120`
3. prune 计数补丁后追加 sanity run：
   - `model_runtime_probe`
     - `ok=true`
     - `readyOk=true`
     - `hotShadowOk=true`
     - `semanticCoreFrameFresh=true`
     - `shadowRuntimeModelCount=179`
     - `semanticCoreResolved=120`
     - `semanticCoreSkinnedResolved=120`

### 6.3 为什么这还不能算 4C 完成

1. `War3TryAppendSemanticShadowPacket(...)`
   - 仍然承载真实 DXVK 资源上传和 scene submission
2. `War3AllocFreezeBuffer`
   - 仍然出现在新的 object shadow 提交链里
3. 这说明：
   - 编排层已经切到 backend
   - 但宿主层还没收缩到“纯后端适配”

## 7. 2026-04-21 实机更新：Phase 4D 已前推到“默认主线禁用 frame-local upload + 报告口径补齐”

### 7.1 这轮前推了什么

1. report / control-plane / diagnostics 现在都会直接输出：
   - `semanticSceneSubmitted`
   - `semanticSceneSubmittedFrameLocal`
   - `semanticSceneSubmittedPersistent`
2. semantic scene 默认不再允许 frame-local dynamic mesh upload：
   - `kShadowSemanticCoreAllowFrameLocalDynamicGeometry = false`
   - 只有显式诊断时才允许放开

### 7.2 本轮验证结果

1. `ninja -C build32`
   - 通过
2. `python -m py_compile AutoTest/war3_autotest_mcp.py`
   - 通过
3. 三套命名场景重新通过，并且 report 全部显示：
   - `semanticSceneSubmittedFrameLocal=0`
   - `semanticSceneSubmittedPersistent>0`
4. 具体结果：
   - `model_runtime_probe`
     - `semanticSceneSubmitted=1300`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=1300`
     - `fallbackDrawCount=611`
   - `low_pressure_static_reuse`
     - `semanticSceneSubmitted=868`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=868`
     - `fallbackDrawCount=1041`
   - `dynamic_shadow_pressure`
     - `semanticSceneSubmitted=1760`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=1760`
     - `fallbackDrawCount=705`

### 7.3 当前准确状态

1. acceptance 主线上，semantic object shadow 提交已经不再走 frame-local upload
2. 但 `fallbackDrawCount` 仍然明显不为 `0`
3. 所以：
   - per-draw host upload 的默认主线问题已前推
   - mixed-mode / legacy fallback 清退还没有完成

## 8. 2026-04-21 实机更新：object fallback 已从总 fallback 中拆口径，当前 object fallback 为 0

### 8.1 这轮前推了什么

1. control-plane / diagnostics / perf report / AutoTest
   - 现在都会直接输出：
     - `fallbackDrawCountTerrain`
     - `fallbackDrawCountWorldObject`
     - `fallbackDrawCountUnitObject`
     - `objectFallbackDrawCount`
2. `objectFallbackDrawCount`
   - 固定定义为：
     - `fallbackDrawCountWorldObject + fallbackDrawCountUnitObject`

### 8.2 本轮验证结果

1. `model_runtime_probe`
   - report:
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_21_15_44_12.html`
   - 结果：
     - `fallbackDrawCount=658`
     - `fallbackDrawCountTerrain=546`
     - `fallbackDrawCountWorldObject=0`
     - `fallbackDrawCountUnitObject=0`
     - `objectFallbackDrawCount=0`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSubmittedPersistent=1421`

### 8.3 当前准确状态

1. object shadow 主线当前已经没有 object fallback
2. `fallbackDrawCount` 的剩余大头当前是 terrain fallback
3. 因此后续 `Phase 4D` 的 mixed-mode 清退重点不该再笼统写成“object fallback 还很多”，而应继续拆成：
   - object shadow 主线
   - terrain / 其他 legacy fallback

## 9. 2026-04-21 实机更新：`NativeD3D9Backend` 已前推到真实 native resource/cache backend，但运行时执行链尚未接通

### 9.1 这轮前推了什么

1. `NativeD3D9Backend`
   - 不再只是：
     - `key -> handle`
     - `submitDraw()` 只做非零检查
2. 当前实现已经会真实创建并持有：
   - native `IDirect3DVertexBuffer9`
   - native `IDirect3DIndexBuffer9`
   - skinned blend vertex stream
   - palette cache
   - material cache
   - submission record queue
3. 这轮保持的硬边界：
   - 只依赖 `IDirect3DDevice9`
   - 不引入：
     - `IDirect3DDevice9Ex`
     - `D3D9DeviceEx`
     - `War3AllocFreezeBuffer`

### 9.2 本轮验证结果

1. `ninja -C build32`
   - 通过
2. 代码搜索确认：
   - `war3_shadow_backend_native_d3d9.cpp`
     - 已出现真实 native 资源创建：
       - `CreateVertexBuffer`
       - `CreateIndexBuffer`
   - `war3_shadow_backend_native_d3d9.h/.cpp`
     - 未出现：
       - `IDirect3DDevice9Ex`
       - `D3D9DeviceEx`
       - `War3AllocFreezeBuffer`

### 9.3 当前准确状态

1. `Phase 5A`
   - 已从“只有接口壳”推进到“有真实 native resource/cache 语义的 backend”
2. 但这还不是 native 路线签收，因为：
   - late-inject/bootstrap 还没把 backend 真正驱动起来
   - 当前也还没有 native D3D9 object shadow 的运行时出图验收
3. 因此下一步最直接的推进面已经更清晰：
   - 把 `NativeD3D9Backend` 接到晚注入 bootstrap / native device 获取链
   - 再做最小 object shadow 出图闭环，而不是继续停留在空骨架阶段

## 10. 2026-04-21 实机更新：native backend 已接上 runtime driver 与 control-plane summary

### 10.1 这轮前推了什么

1. 新增 `NativeD3D9BackendRuntime`
   - 不再只停留在单个 backend 类
   - 当前已经有一层 runtime driver，会消费现有 `ShadowSubmissionFrame`
2. `ShadowRuntimeBridgeSummary`
   - 现在直接带出：
     - `nativeD3D9BackendFrameSerial`
     - `nativeD3D9BackendSourcePublishRevision`
     - `nativeD3D9BackendSubmittedDrawCount`
     - `nativeD3D9BackendGeometryCount`
     - `nativeD3D9BackendPaletteCount`
     - `nativeD3D9BackendMaterialCount`
     - `nativeD3D9BackendHasDevice`
3. control-plane
   - `get_runtime_status` / `get_shadow_runtime_summary`
   - 现在已经能直接输出 native backend summary 字段

### 10.2 本轮验证结果

1. `ninja -C build32`
   - 通过
2. 代码搜索确认：
   - native runtime 驱动已经接到：
     - `war3_shadow_runtime_bridge.cpp`
     - `war3_control_plane.cpp`

### 10.3 当前准确状态

1. native 路线当前已经同时具备：
   - real resource/cache backend
   - runtime driver entry
   - control-plane summary surface
2. 因此当前 `Phase 5A` 的剩余 blocker 已进一步收缩为：
   - 真实 `IDirect3DDevice9` 绑定
   - 晚注入帧循环中的 native draw 执行
3. 换句话说，native 路线已经不再缺“后端 contract 和 runtime 胶水”，而是开始缺真正的宿主接入与出图

## 11. 2026-04-21 实机更新：runtime bootstrap 已暴露 native backend bind/drive 入口

### 11.1 这轮前推了什么

1. `war3_runtime_bootstrap.h/.cpp`
   - 新增：
     - `BindNativeShadowDevice(IDirect3DDevice9* device)`
     - `DriveNativeShadowBackend()`
2. 这意味着晚注入侧现在已经有明确的公开挂点，可以：
   - 绑定真实 native device
   - 驱动 native backend 消费 semantic frame

### 11.2 本轮验证结果

1. `ninja -C build32`
   - 通过

### 11.3 当前准确状态

1. native 路线当前已经不缺：
   - backend
   - runtime driver
   - control-plane summary
   - bootstrap bind/drive API
2. 剩余真正没做的就是：
   - 晚注入宿主拿设备
   - 在真实帧时机驱动 native draw

## 12. 2026-04-21 实机更新：当前宿主已经能绑定 native backend device，并在 render thread 驱动 native runtime submission

### 12.1 这轮前推了什么

1. `d3d9_war3_hook.cpp`
   - `War3Hook::InstallHooks(IDirect3DDevice9* device)`
   - 现在会直接调用：
     - `BindNativeShadowDevice(device)`
2. `d3d9_device.cpp`
   - `War3TryPopulateSemanticShadowScene(...)`
   - 现在会在 DXVK backend 提交后继续调用：
     - `DriveNativeShadowBackend()`
3. 这意味着当前宿主已经不只是“有 bind/drive API”
   - 而是已经真的把 native backend 接到了实际 host device 和 render thread 时机上

### 12.2 本轮验证结果

1. `ninja -C build32`
   - 通过
2. 三套命名场景实机验证：
   - `model_runtime_probe`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=119`
     - `semanticCoreSubmittedDrawCount=119`
   - `low_pressure_static_reuse`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=33`
     - `semanticCoreSubmittedDrawCount=33`
   - `dynamic_shadow_pressure`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=119`
     - `semanticCoreSubmittedDrawCount=119`

### 12.3 当前准确状态

1. 当前宿主内：
   - native backend 已经拿到真实 `IDirect3DDevice9*`
   - native runtime submission 已经在 render thread 上被驱动起来
2. 这说明 `Phase 5A` 已经进一步前推到：
   - current host native submission path alive
3. 剩余未完成点已经更集中：
   - 晚注入独立宿主拿设备
   - native backend 真正执行出图

## 13. 2026-04-21 实机更新：native backend 已具备 prepared-frame execute 能力，但当前宿主仍未在真实 shadow-pass 时机执行

### 13.1 这轮前推了什么

1. `NativeD3D9Backend`
   - 新增：
     - `executePreparedDraws()`
     - `executedDrawCount()`
     - `executedFrameSerial()`
2. `NativeD3D9BackendRuntime`
   - 新增：
     - `executePreparedFrame()`
3. `war3_runtime_bootstrap.h/.cpp`
   - 新增：
     - `ExecuteNativeShadowBackendPreparedFrame()`
4. control-plane / summary
   - 现在直接暴露：
     - `nativeD3D9BackendExecutedFrameSerial`
     - `nativeD3D9BackendExecutedDrawCount`

### 13.2 本轮验证结果

1. `ninja -C build32`
   - 通过
2. 三套命名场景回归：
   - `model_runtime_probe`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=119`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticCoreSubmittedDrawCount=119`
   - `low_pressure_static_reuse`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=33`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticCoreSubmittedDrawCount=33`
   - `dynamic_shadow_pressure`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=117`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticCoreSubmittedDrawCount=117`

### 13.3 当前准确状态

1. native 路线现在已经具备：
   - submission build
   - prepared-frame execute
   - execute counters
2. 但当前宿主仍然只调用：
   - `DriveNativeShadowBackend()`
3. 还没有在真正 native shadow-pass timing 下调用：
   - `ExecuteNativeShadowBackendPreparedFrame()`
4. 因此这轮推进后最准确的 blocker 变成：
   - 晚注入独立宿主接管 shadow-pass timing
   - native execute 真正出图并做 acceptance

## 14. 2026-04-21 实机更新：native renderer hook 路线已接上真实 shadow-pass execute timing，但默认仍关闭

### 14.1 这轮前推了什么

1. `war3_native_renderer.cpp`
   - 已在 native shadow-pass window 内接上：
     - `DriveNativeShadowBackend()`
     - `ExecuteNativeShadowBackendPreparedFrame()`
2. execute 时机固定为：
   - `CWorld_ToggleGroup1ShadowPass(world, 1)`
   - `Stage12_Group1`
   - `Stage11_TerrainShadow12_Group0`
   - 之后、`RenderQueue_FlushAndReset()` 之前

### 14.2 本轮验证结果

1. `ninja -C build32`
   - 通过
2. 当前主线追加回归：
   - `model_runtime_probe`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendSubmittedDrawCount=117`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticCoreSubmittedDrawCount=117`

### 14.3 当前准确状态

1. execute timing 现在已经有了
2. 当前 `executedDrawCount` 仍然为 `0`
   - 根因不是 timing 还没接
   - 而是 `kNativeRendererHookTakeoverEnabled=false`
3. 所以下一阶段最直接的工作已经变成：
   - 在独立宿主/晚注入场景真正点亮 native hook takeover
   - 做第一次 native execute 出图签收

## 15. 2026-04-21 实机更新：runtime-bridge warm gate 已经真实装上 native takeover，但 render-thread semantic scene 仍然只看到空 submission frame

### 15.1 本轮前推了什么

1. `War3Hook::MaybeInstallNativeRendererTakeover(...)`
   - 已从“只挂在 DXVK semantic host path 的旁路点”推进到：
   - `war3_shadow_runtime_bridge.cpp` 的 runtime-bridge warm gate
2. 新 gate 条件固定为：
   - `runtimeChainWarm=true`
   - `semanticCoreFrameFresh=true`
   - `nativeD3D9BackendHasDevice=true`
   - `nativeD3D9BackendSubmittedDrawCount>0`
3. `war3_native_hooks.cpp`
   - `WriteJump/WriteCall/WriteNop` 已补 `FlushInstructionCache(...)`
4. `d3d9_device.cpp`
   - 新增宿主内 validation plumbing 与 scene-submit 诊断日志

### 15.2 本轮验证结果

1. `ninja -C build32`
   - 通过
2. isolated desktop / control-plane 实测已经首次看到：
   - `DXVK War3Hook: Installing native renderer hook takeover after semantic warmup reason=runtime-bridge-warm ...`
   - `DXVK War3: [INFO] [War3Hook] Hooked CWorldFrameWar3::RenderScene @ ...`
3. 同时也首次坐实：
   - render-thread 侧会打印
     - `DXVK War3Shadow: semantic scene skipped empty frame unitsOnly=1 frameSerial=882 drawCount=0 ...`
   - 而 control-plane/hot-frame 侧已经能看到
     - `semanticCoreSubmittedDrawCount=119`
     - `nativeD3D9BackendSubmittedDrawCount=119`

### 15.3 当前准确状态

1. native takeover 安装这一层已经不是 blocker
   - 它已经真实触发并安装成功
2. 当前 blocker 也已经从“hook 有没有装上”缩小到：
   - render-thread scene submit 时拿到的 `snapshotFrameShared()` 还是空帧
3. 所以当前 `executedDrawCount=0` 的更准确解释是：
   - native backend execute 还没有真正获得一张 render-thread 可消费的非空 submission frame
4. 下一步应继续优先追：
   - semantic frame publish / render-thread consume timing
   - 而不是再重复验证 native takeover 安装本身

## 16. 2026-04-21 实机更新：takeover-only native execute 已在当前 DXVK 宿主内跑通，控制面与 AutoTest 口径已同步升级

### 16.1 这轮前推了什么

1. `ShadowValidationRuntime`
   - 已新增 `lastRenderableFrame`
   - 当 latest semantic submission 暂时为空时：
     - DXVK semantic scene submit
     - native backend prepare
     - 都会回退到上一张非空 frame
2. `war3_native_renderer.cpp`
   - 已新增低频 `INFO` 级 runtime 证据：
     - `RenderScene entry`
     - `RenderScene shadow gate`
   - `DriveNativeShadowBackend()` 已不再被
     - `shadowModeOrStage21ListBEntryIndex != -1`
     - 绑死
3. `war3_internal_test_config.h`
   - `kNativeRendererHostExecuteValidationEnabled=false`
   - 当前验收已切到真正 takeover-only 路线
4. `war3_control_plane.cpp` + `war3_autotest_mcp.py`
   - hot-frame 验收现改成两段式：
     - 先等 semantic hot frame
     - 再单独等 `nativeD3D9BackendExecutedDrawCount`

### 16.2 本轮验证结果

1. `ninja -C build32`
   - 多次通过
2. runtime proof
   - `Hooked CWorldFrameWar3::RenderScene @ ...`
   - `RenderScene entry ... shadowStageEntry=0 shadowModeOrStage21=-1 ...`
   - `RenderScene shadow gate ... prepared=1`
3. 三套命名场景 takeover-only 回归现已全部通过：
   - `model_runtime_probe`
     - `nativeD3D9BackendExecutedDrawCount=119`
     - `semanticSceneSubmitted=119`
   - `low_pressure_static_reuse`
     - `nativeD3D9BackendExecutedDrawCount=122`
     - `semanticSceneSubmitted=122`
   - `dynamic_shadow_pressure`
     - `nativeD3D9BackendExecutedDrawCount=119`
     - `semanticSceneSubmitted=119`
4. 三套场景当前仍保持：
   - `objectFallbackDrawCount=0`
   - `runtimeChainWarm=true`

### 16.3 当前准确状态

1. native takeover 在 current DXVK host 内已经不再停留在“已安装但不执行”
2. 当前 `executedDrawCount` 已经在 takeover-only 模式下真实>0
3. 当前最准确的剩余工程 blocker 已切到：
   - late-inject/native-only 独立宿主
   - 与后续 perf closeout
4. 因此后续不应再把：
   - `snapshotFrameShared()` 空帧
   - `executedDrawCount=0`
   - 当成 primary blocker

## 17. 2026-04-22 实机修正：默认路径已回退到安全 DXVK-host native execute，full-scene native takeover 不再作为当前可用态

### 17.1 为什么这轮要止损

1. 最新用户实机截图已确认：
   - 默认打开 native renderer takeover 时
   - 主模型会出现白模 / 纯白贴图 / 主场景损坏
2. 当前 `RenderScene` hook 的代码形态也已对齐这个结论：
   - 它接管的是完整 RenderScene 主链
   - 不是单独的 shadow-only 注入
3. 因此 takeover-only execute 虽然此前已证明“能执行”
   - 但它不能直接等价成“当前默认可用态”

### 17.2 本轮代码策略调整

1. `war3_internal_test_config.h`
   - `kNativeRendererHookTakeoverEnabled=false`
   - `kNativeRendererHostExecuteValidationEnabled=true`
2. 当前默认语义阴影路径重新固定为：
   - 主场景不走 experimental full-scene takeover
   - shadow 继续走 semantic contract + native backend host-side execute

### 17.3 本轮验证结果

1. `ninja -C build32`
   - 通过
2. 三套命名场景均重新通过 hot-frame 验收：
   - `model_runtime_probe`
   - `low_pressure_static_reuse`
   - `dynamic_shadow_pressure`
3. 三套场景共同保持：
   - `nativeD3D9BackendExecutedDrawCount > 0`
   - `objectFallbackDrawCount = 0`
4. 最新截图已确认：
   - 默认路径下主模型贴图恢复
   - 白模问题已从当前默认路径移除

### 17.4 当前准确状态

1. takeover-only native execute 仍然是“已证明可运行”的实验路径
2. 但当前仓库默认可用态已经切回：
   - 安全 DXVK host
   - semantic shadow execute
   - native backend host-side execute
3. 后续 native takeover 应继续作为：
   - native-only / late-inject correctness 分支
   - 而不是再直接作为默认路径

## 18. 2026-04-22 实机修正：semantic 动态阴影可见性已从“提交/执行成立”推进到“receiver 中实际可见”

### 18.1 本轮收口前的真实状态

1. control-plane / hot-frame 已稳定证明：
   - `semanticCoreSubmittedDrawCount > 0`
   - `semanticSceneSubmitted > 0`
   - `nativeD3D9BackendExecutedDrawCount > 0`
2. 但 `ShadowFactor` 仍显示大面积发白
3. 因此本轮 primary blocker 已不再是：
   - pipe / ready
   - semantic build
   - native host execute
4. 而是：
   - semantic dynamic caster 的 bounds / cascade visibility 仍然不够保守

### 18.2 本轮代码调整

1. `d3d9_device.cpp`
   - semantic bounds center 改为：
     - `basisMatrix * localBoundsCenter`
   - 不再默认退化成只看 matrix translation
2. `d3d9_war3_shadow.cpp`
   - `ObjectKind::Unit` 当前默认 bypass cascade cull
3. `war3_internal_test_config.h`
   - 新增 `kShadowCascadeCullDisableForUnits = true`
   - 明确作为当前语义动态阴影收口期的 correctness-first 临时策略

### 18.3 本轮验证

1. `dynamic_shadow_pressure` + `DXVK_WAR3_SHADOW_DEBUG=2`
   - `war3_20260422_005514.png`
   - `war3_perf_report_auto_2026_04_22_00_55_12.html`
   - `semanticSceneSubmitted=117`
   - `nativeD3D9BackendExecutedDrawCount=117`
   - `objectFallbackDrawCount=0`
2. 与修复前 `war3_20260422_004656.png` 对比：
   - 中央场景区域暗像素占比 `11.803% -> 15.493%`
   - very-dark 占比 `10.427% -> 13.524%`
3. `dynamic_shadow_pressure` normal：
   - `war3_20260422_005631.png`
   - `war3_perf_report_auto_2026_04_22_00_56_30.html`
4. `model_runtime_probe`：
   - `war3_20260422_005801.png`
   - `war3_perf_report_auto_2026_04_22_00_58_00.html`

### 18.4 当前准确状态

1. semantic 动态阴影现在已经不只是“后台链条通了”
2. 更准确的表述是：
   - safe DXVK host 路线下
   - semantic/new-route dynamic shadow 已在 receiver 中实际可见
3. 仍未完成：
   - 单位 cascade no-cull 还是临时策略
   - perf 仍然极差
   - native-only / late-inject 路线仍未独立收口

## 19. 2026-04-22 上游语义数据 round：安全回退已重建，`no-pose` 已开始下降，但真正主 blocker 仍钉在 child-runtime palette linkage

### 19.1 本轮前确认的真实现象

1. 安全基线下，`dynamic_shadow_pressure` 的 hot-frame 摘要稳定是：
   - `semanticCoreResolved=376`
   - `semanticCoreSkippedNoPose=82`
   - `semanticCoreSkippedNoRuntimeGroupPalette=442`
2. fresh live miss 显示了两类不同问题：
   - `no-pose`
     - 一类是匿名 scene-only subpart：没有 unit/runtime/handle 上下文
     - 一类是 `YTpb/YTfb/YTlc/...` 这类 blocker/path-blocker 风格对象混进来，污染 `no-pose`
   - `no-runtime-group-palette`
     - 典型日志固定是 `phase=no-descendant-runtime`
     - `poseCount=1/2/3`
     - 但 `groupCount/maxSlot` 需要 `21/25/79`

### 19.2 本轮代码推进

1. 完整回退了 `war3_shadow_renderer_core.cpp` 里最后一处 over-strict runtime-model validator，重新回到安全基线。
2. 保留 geoset-owner recovery：
   - `runtimeGeosetPtr`
   - `runtimeGeosetDataPtr`
   - `geosetIndex`
   - 继续作为 runtime/model resource 回补的安全主线
3. `TryResolveChildRuntimeModelByModelResource(...)` 新增 direct-resource 归一化比较，避免 child runtime 的 owned-handle 与 normalized model-resource 混比。
4. semantic consumer 继续收缩 upstream noise：
   - visible/model-instance 侧会清理 `runtimeModelPtr == sceneNode/worldObjectEntry/sprite/renderablePart/meshData` 这种明显假值
   - `no-pose` 统计前会把 blocker-style FourCC 从计数里剔掉，不再让它们继续污染主指标

### 19.3 本轮验证

1. `ninja -C build32`
   - 多次通过
2. 安全基线重建：
   - `AutoTest/artifacts/war3_upstream_quick6.json`
   - `war3_perf_report_auto_2026_04_22_02_23_49.html`
3. 本轮最佳 quick 结果：
   - `AutoTest/artifacts/war3_upstream_quick9.json`
   - `war3_perf_report_auto_2026_04_22_02_37_45.html`
   - `recordsWithModelResource=816`
   - `recordsWithRuntimeModel=886`
   - `semanticCoreResolved=378`
   - `semanticCoreSkippedNoPose=71`
   - `semanticCoreSkippedNoPoseLookupMiss=59`
   - `semanticCoreSkippedNoRuntimeGroupPalette=441`
   - `nativeD3D9BackendExecutedDrawCount=378`
4. live 复核：
   - `AutoTest/artifacts/war3_upstream_live7.json`
   - `semanticCoreSkippedNoPose=71`
   - `semanticCoreSkippedNoRuntimeGroupPalette=442`
   - 说明本轮改善真实存在，但主要先发生在 `no-pose` 侧

### 19.4 当前更准确的 blocker 表述

1. 当前已经不能再说“上游什么都没有”。
2. 当前更准确的说法是：
   - `no-pose` 噪声已经开始下降
   - geoset-owner recovery 也已经证明是安全的
   - 但 child runtime palette linkage 仍然没有真正对上 geoset group 需求
3. 因此当前 primary blocker 进一步收敛为：
   - `phase=no-descendant-runtime`
   - root pose palette 太小
   - child runtime / descendant runtime 的 pose palette 没有被正确 capture 或正确绑定到 consumer
4. 下一轮不该再做的事：
   - 不要再扩大 runtime-model validator
   - 不要再回头把 sceneNode 当 runtimeModel 的推断路径放宽
5. 下一轮应继续直接做的事：
   - 查 child runtime palette capture / publish / bind
   - 查为什么 descendant runtime 仍然没有提供更大的 pose palette

## 20. 2026-04-22 上游语义数据 round：已确认当前 primary blocker 不是 stale lookup，而是 descendant runtime 根本没有可用 pose/palette

### 20.1 本轮代码推进

1. consumer 侧去掉了两处无失效静态缓存：
   - `CollectRuntimeModelTree(...)`
   - `TryResolveChildRuntimeModelByModelResource(...)`
2. descendant runtime -> model resource 命中改成优先信任：
   - `ShadowModelResourceCache::findRuntimeModelResource(...)`
   - `ModelRegistry::findByRuntimeModel(...)`
   - 最后才回退到 live `OwnedModelDataHandle`
3. `VisibleRenderableRecord` 在进入 geoset binding 前新增 runtimeModel 清洗：
   - 若 `runtimeModelPtr` 明显等于 `sceneNode/worldObjectEntry/sprite/renderablePart/meshData`
   - 直接清空并重新走 metadata recovery
4. `ShadowRendererCore` 新增：
   - `no-descendant-runtime detail` 日志
   - descendant runtime 的 on-demand live matrix-palette read
5. `war3_model_hook.cpp` 的 palette capture 又往前推了一轮：
   - `Hook_RuntimePoseUpdate(...)`
   - `Hook_RuntimeMatrixRangeCopy(...)`
   - `Hook_RuntimeMatrixFlush(...)`
   现在都会走 `RecordRuntimePaletteTree(runtimeModel)`。

### 20.2 本轮验证

1. `ninja -C build32`
   - 多次通过
2. fresh live artifacts：
   - `AutoTest/artifacts/war3_upstream_live10.json`
   - `AutoTest/artifacts/war3_upstream_live12.json`
   - `AutoTest/artifacts/war3_upstream_live13.json`
3. 当前 authoritative quick：
   - `AutoTest/artifacts/war3_upstream_quick14.json`
   - `war3_perf_report_auto_2026_04_22_03_04_06.html`
4. 当前稳定计数：
   - `semanticCoreResolved=376~378`
   - `semanticCoreSkippedNoPose=71`
   - `semanticCoreSkippedNoRuntimeGroupPalette=442`
   - `nativeD3D9BackendExecutedDrawCount=376~378`

### 20.3 本轮得到的新硬证据

1. 当前 miss family 里，consumer 已经能看到 descendant runtime tree：
   - 典型样本：
     - `runtime=1bb5af40 model=2d1a05c0 descRuntimeCount=2`
2. 但这些 descendant runtime 没有任何可用 pose：
   - 同一条 detail 日志里固定是：
     - `descPoseHitCount=0`
     - `bestDescPoseCount=0`
     - `matchingModelDescCount=0`
3. 并且当前轮新增的 on-demand live read 也没有改变这个结果。

### 20.4 当前更准确的 blocker 表述

1. 当前 primary blocker 已经不能再表述成“consumer 没找到 descendant runtime”。
2. 更准确的说法是：
   - descendant runtime tree 已经可见
   - 但 descendant runtime 自己没有 snapshot pose，也没有 live-readable matrix palette
3. 因此当前下一步已经收敛为：
   - 找新的 descendant pose capture / publish 来源
   - 而不是继续扩大 visible/runtimeModel 推断或继续折腾 stale cache

## 21. 2026-04-22 上游语义数据 round：wrapper-level pose 路径已补齐，descendant runtime owner 记录已扩张，但 descendant pose publication 仍未进入 stable contract

### 21.1 本轮代码推进

1. `war3_model_hook.cpp`
   - 补挂了此前未接的两条 sprite pose/update 路径：
     - `0x6F1820C0`
     - `0x6F1825E0`
   - 现在四条 wrapper-level path：
     - `0x6F1820C0 / 0x6F182300 / 0x6F1825E0 / 0x6F1826C0`
     都会在返回时走 `RecordSpriteFramePoseFromSprite(...)`
2. wrapper-level path 不再只记 world transform；
   现在会在同一返回点顺手对 runtime root 调 `RecordRuntimePaletteTree(...)`
3. `RecordRuntimePaletteTree(...)` 前新增 tree-level binding pass：
   - 遍历整棵 runtime tree
   - 对每个 runtimeModel 都调用 `ShadowModelResourceCache::noteRuntimeModelBinding(...)`
   - 使 descendant runtime 即使当前无 pose，也先进入 `runtimeModelOwner` 查找面
4. `ShadowRendererCore`
   - 新增 runtime-owner root 直试路径：
     - 如果 `CollectRenderableRuntimeModelRoots(...)` 已通过 `runtimeGeosetPtr/runtimeGeosetDataPtr/geosetIndex` 找到更可信 runtime root
     - 先直接对这个 root 尝试 canonical palette build
     - 再回退到旧的 `TryResolveChildRuntimeModelByModelResource(...)`

### 21.2 本轮验证

1. `ninja -C build32`
   - 多次通过
2. fresh artifacts：
   - `AutoTest/artifacts/war3_upstream_quick15.json`
   - `AutoTest/artifacts/war3_upstream_live14.json`
   - `AutoTest/artifacts/war3_upstream_quick16.json`
   - `AutoTest/artifacts/war3_upstream_live15.json`
   - `AutoTest/artifacts/war3_upstream_quick17.json`
   - `AutoTest/artifacts/war3_upstream_live16.json`
3. 当前本轮最稳定的 counter 变化：
   - `shadowRuntimeModelCount`
     - `218 -> 309~310`
   - `semanticCoreResolved`
     - `376~379`
   - `semanticCoreSkippedNoRuntimeGroupPalette`
     - live 最低 `437~438`
   - `nativeD3D9BackendExecutedDrawCount`
     - `376~379`
4. 本轮 quick 同时出现了新的 hot-frame wait 抖动：
   - `war3_upstream_quick16/17`
   - `hotShadow.ok=false`
   - `error=wait_until timeout`
   但 hot summary 本体仍然成功返回 semantic counters。

### 21.3 当前得到的新结论

1. 当前 blocker 已经不能再说成“descendant runtime 还没进 cache / owner 面”：
   - `shadowRuntimeModelCount` 已明显抬高到 `309~310`
2. 但 primary blocker 仍然不是 owner lineage：
   - `war3_upstream_live15/16`
   - `no-descendant-runtime detail`
   仍然稳定为：
   - `descPoseHitCount=0`
   - `matchingModelDescCount=0`
   - `bestDescPoseCount=0`
   - `bestMatchingModelPoseCount=0`
3. 这说明：
   - runtime owner / runtime tree bookkeeping 已经前推了一步
   - 但 descendant pose publication 本身依然没进 snapshot/live contract

### 21.4 当前下一步

1. 不要再继续把工作重点放在：
   - 扩 wrapper-level pose hook 数量
   - 或继续假设“更多 runtime owner record 会自动解决 descendant pose”
2. 下一轮主线应继续收敛到：
   - `0x6F12EC90 -> sub_6F77C280(...)`
   - child/attachment runtime 是否根本不把最终姿态留在 `CModel + 0x5C/+0x60`
3. 当前更准确的 blocker 表述：
   - descendant runtime owner lineage 已开始补齐
   - 但 descendant pose publication 仍未稳定暴露给 semantic consumer

## 22. 2026-04-22 上游语义数据 round：`post 0x6F12EC90` 仍抓不到 descendant palette，IDA 也开始支持这条链更像 controller/local-point propagation

### 22.1 本轮代码推进

1. `war3_model_hook.cpp`
   - 新增 `post 0x6F12EC90` hook
   - 即 `CModelComplex__RecurseChildRuntimeTree`
2. 这条 hook 当前只做最小被动采样：
   - 对当前递归到的 `runtimeModel`
   - 直接调用 `RecordRuntimeMatrixPalette(runtimeModel)`
   - 用来验证 descendant runtime 的 `+0x5C/+0x60` palette 是否只在 recursion 窗口短暂可见

### 22.2 本轮验证

1. `ninja -C build32`
   - 通过
2. fresh artifact：
   - `AutoTest/artifacts/war3_upstream_live17.json`
3. live17 关键结果：
   - `shadowRuntimeModelCount=310`
   - `semanticCoreResolved=378`
   - `semanticCoreSkippedNoRuntimeGroupPalette=440`
   - `nativeD3D9BackendExecutedDrawCount=378`
4. 但 `no-descendant-runtime detail` 聚合仍完全不动：
   - `descPoseHitPositive=0`
   - `matchingModelPositive=0`
   - `max descPoseHitCount=0`
   - `max matchingModelDescCount=0`
   - `max bestDescPoseCount=0`
   - `max bestMatchingModelPoseCount=0`

### 22.3 本轮新增的 IDA 结论

1. `0x6F12EC90`
   - 先对 controller 调 `0x6F77C280(controller, modelData + 92)`
   - 再沿 child link 递归
2. `0x6F77C280`
   - `sub_6F77BFE0(...)`
   - 遍历一组 `220B` records
   - 每条 record 调 `0x6F77CDD0`
   - 最后 `sub_6F77CF40(a1)`
3. `0x6F77CDD0`
   - 走 `sub_6F789EB0 / sub_6F789C60`
   - 尾部是 controller callback：
     - `(*(v3 + 204))(v12 + 22, &v9, *(v3 + 208))`
4. 这开始支持一个更强的新判断：
   - 这条链更像 controller/local-point/attachment output propagation
   - 而不是最终 3x4 palette publish

### 22.4 当前更准确的 blocker 表述

1. 现在不能再把希望放在：
   - “只要把 hook 时机继续往前推到 `post 0x6F12EC90` 就能拿到 descendant palette”
2. 当前更准确的说法是：
   - descendant runtime owner/bookkeeping 已经比前几轮更完整
   - 但 `CModel + 0x5C/+0x60` 这条 contract 对 child/attachment runtime 很可能根本就不是 publish 面
3. 因此下一轮该查的已经不是更多 wrapper-level hook，而是：
   - `0x6F77BFE0 / 0x6F77CF40 / 0x6F789C60`
   - controller/output block 到底长什么样
   - 这些输出能否作为 child/attachment runtime 的真正 transform/pose contract

## 23. 2026-04-22 上游语义数据 round：controller-output probe 已确认 local-point 写回活跃，当前场景里 primary/shared preset 仍为 0

### 23.1 本轮代码推进

1. `war3_model_hook.cpp`
   - 新增三条 output-write probe：
     - `0x6F77DA20` local-point output
     - `0x6F77DAA0` shared preset output
     - `0x6F77DF10` primary preset output
2. `war3_model_hook.h`
   - 新增 `RuntimeOverrideOutputProbeSummary`
3. `war3_shadow_runtime_bridge.*`
   - 把 probe 摘要接进 `ShadowRuntimeBridgeSummary`
4. `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 现在直接回传 override-output counters / latest sample

### 23.2 本轮验证

1. `ninja -C build32`
   - 通过
2. fresh artifacts：
   - `AutoTest/artifacts/war3_upstream_live18.json`
   - `AutoTest/artifacts/war3_upstream_quick18.json`
3. `war3_upstream_live18.json`
   - `hotShadow.ok=false`
   - `error=wait_until timeout`
   - 但 hot summary 已明确返回：
     - `overrideOutputSampleFrame=927`
     - `overrideOutputLastActiveFrame=927`
     - `overridePrimaryPresetWriteCount=0`
     - `overrideSharedPresetWriteCount=0`
     - `overrideLocalPointWriteCount=42543`
     - `overrideLocalPointNonZeroWriteCount=39165`
     - `overrideMaxLocalPointSlotIndex=10`
     - `overrideLastRuntimeModelPtr=290791012`
     - `overrideLastLocalPointSlotIndex=0`
     - `overrideLastLocalPointX/Y/Z=-1.7233 / 0.8925 / 0.0`
   - 同一轮 semantic 计数仍是：
     - `semanticCoreResolved=393`
     - `semanticCoreSkippedNoRuntimeGroupPalette=420`
     - `nativeD3D9BackendExecutedDrawCount=393`
     - `semanticCoreFrameFresh=false`
4. `war3_upstream_quick18.json`
   - 放宽 `require_semantic_frame_fresh=false` 后：
     - `hotShadow.ok=true`
   - 关键结果：
     - `overridePrimaryPresetWriteCount=0`
     - `overrideSharedPresetWriteCount=0`
     - `overrideLocalPointWriteCount=4805`
     - `overrideLocalPointNonZeroWriteCount=4433`
     - `overrideMaxLocalPointSlotIndex=10`
     - `semanticCoreResolved=378`
     - `semanticCoreSkippedNoRuntimeGroupPalette=442`
     - `nativeD3D9BackendExecutedDrawCount=378`
     - `semanticCoreFrameFresh=false`

### 23.3 当前得到的新结论

1. controller/output block 已不再是“推测存在”：
   - local-point output 在热帧里已经被实测确认是活跃、非零、持续写回的
2. 当前 `model_runtime_probe` 场景里：
   - primary preset output 没亮
   - shared preset output 也没亮
3. 因此当前更准确的 blocker 已经不是：
   - “controller output 还没被生产”
4. 而是：
   - semantic path 完全没有消费这批 local-point output
   - 同时 remaining miss family 仍在 `semanticCoreSkippedNoRuntimeGroupPalette`
5. 这意味着下一轮主线应收敛到：
   - `node_type=7 / local-point output slot`
   - `child link tag / outputSlotIndex`
   - 它们和 missing descendant runtime/geoset 的具体映射

### 23.4 当前下一步

1. 继续追：
   - `sub_6F77DB20 / sub_6F77DA20`
   - `runtime + 0xB4`
   - `child link + 0x0C`
2. 目标是确认：
   - local-point output 能不能直接给 child/attachment 提供 rigid transform contract
3. 不再优先做的事：
   - 扩 `+0x60` palette hook
   - 或默认把 primary/shared preset 当成当前 main missing route

### 24. 2026-04-22 上游语义数据 round：local-point mapping probe 已确认当前 override write context 既不直接带 child links，也在近邻 context 窗口里找不到 owner/root runtime

#### 24.1 本轮改动

1. `war3_model_hook.cpp/.h`
   - 在现有 local-point probe 上继续补了：
     - `sourceRecordIndex -> child link +0x0C`
     - `当前 runtime 是否带 direct child links`
     - `contextPtr` 前 `0x40` 范围内的 runtime-owner 扫描
2. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - 新增 control-plane summary 字段：
     - `overrideLocalPointObservedChildLinkWriteCount`
     - `overrideLocalPointMatchedChildLinkBySourceRecordWriteCount`
     - `overrideLocalPointContextRuntimeWithChildLinksWriteCount`
     - `overrideLastContextRuntimeWithChildLinksOffset/Ptr/Count/MaxTag`

#### 24.2 本轮验证

1. `ninja -C build32`
   - 通过
2. fresh artifacts：
   - `AutoTest/artifacts/war3_upstream_quick19.json`
   - `AutoTest/artifacts/war3_upstream_quick20.json`
   - `AutoTest/artifacts/war3_upstream_quick21.json`
3. `quick19`
   - `overrideLocalPointMatchedChildLinkWriteCount=0`
   - `overrideLocalPointMatchedChildLinkBySourceRecordWriteCount=0`
4. `quick20`
   - `overrideLocalPointObservedChildLinkWriteCount=0`
   - `overrideMaxObservedChildLinkCount=0`
   - `overrideLastObservedChildLinkCount=0`
5. `quick21`
   - `overrideLocalPointContextRuntimeWithChildLinksWriteCount=0`
   - `overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount=0`
   - `overrideLastContextRuntimeWithChildLinksPtr=0`
   - `overrideLastContextRuntimeWithChildLinksOffset=0`
   - 同轮 `semanticCoreResolved=381`
   - 同轮 `semanticCoreSkippedNoRuntimeGroupPalette=442`

#### 24.3 当前新结论

1. 这轮已经可以排除：
   - “当前问题只是 `slotIndex -> child tag` 没对上”
   - “或只要改成 `sourceRecordIndex -> child tag` 就会命中”
2. 当前更准确的 blocker 是：
   - local-point write context 没暴露真正的 child-link owner/root runtime
   - 这也是为什么当前 probe 即使继续换映射键，也始终是 0 命中

#### 24.4 当前下一步

1. 下一轮主线应改成：
   - 直接追 `override eval context / controller / scratch root`
   - 找回 `owner/root runtimeModel`
2. 在拿到 owner/root runtime 之前：
   - 不应继续把时间花在 `slot/tag/sourceRecord` 的组合枚举上

### 25. 2026-04-22 attachment rigid contract live-fallback 已接进 control-plane，但 attachment identity 仍然是 0

#### 25.1 本轮 control-plane / contract 面改动

1. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 新增：
     - `contractAttachmentRigidCount`
     - `contractAttachmentRigidCountWithAnyIdentity`
     - `contractAttachmentRigidCountWithWorldObjectEntry`
     - `contractAttachmentRigidCountWithSceneNode`
     - `contractAttachmentRigidCountWithUnitPtr`
2. `war3_shadow_runtime_contract.cpp`
   - 当已发布 contract 的 attachment store 为空、但 live attachment registry 已经有记录时，
     `snapshotAttachmentsShared()/snapshotBundleShared()` 会即时合成 live attachment store，
     避免 control-plane / semantic-core 再被“capture 当帧吃不到 attachment”卡住
3. attachment rigid repair 现在额外尝试：
   - `ownerRuntimeModelPtr`
   - `runtimeModel -> modelResource/modelKey -> 唯一 ShadowObject/manifest 候选`

#### 25.2 本轮验证

1. `ninja -C build32`
   - 通过
2. 连续 `run_named_scenario(model_runtime_probe)` fresh 验证
   - 最新完成时间点：`2026-04-22 12:43:33 +08:00`
3. 当前已经稳定坐实：
   - `attachmentRigidCount = 3`
   - `contractAttachmentRigidCount = 3`
   - 这说明 control-plane 现在已经能看到 live contract attachment，而不再只是 raw registry 计数
4. 但同轮也继续稳定为：
   - `attachmentRigidCountWithAnyIdentity = 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `contractAttachmentRigidCountWithSceneNode = 0`
   - `contractAttachmentRigidCountWithUnitPtr = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`
5. safety gate 行为保持正确：
   - `requestedSemanticFrameBuild = false`
   - `render.isInGame = false`
   - `wait_until stalled`
   - 没有再复发 CPU storm

#### 25.3 当前更硬的新结论

1. 现在可以确认：
   - blocker 已经不是“attachment rigid 没被 publish 到 contract”
2. 当前更准确的 blocker 是：
   - attachment rigid 已经进了 live contract
   - 但它们仍然完全匿名
   - semantic-core 依然拿不到任何 `sceneNode/worldObjectEntry/unitPtr`
3. 这也意味着：
   - 继续只在 control-plane / contract / renderer 下游补 join，收益已经明显变小
   - 下一轮必须把主线切回 local-point producer/controller 链本身的 identity 发布

#### 25.4 当前下一步

1. 下一轮应直接验证：
   - local-point producer 所在上游对象，谁才是真正的 identity owner
   - 它和当前 `argBlockRuntime/rootRuntime/childRuntime` 的 handoff 关系是什么
2. 在拿到这条上游 identity contract 之前：
   - 不应继续把时间花在更多 downstream repair/join 枚举上

### 27. 2026-04-22 control-plane 已证明 `AttachedEffectInit` 是热且可解 identity，但 contract attachment 仍然匿名

#### 27.1 本轮 control-plane / probe 面改动

1. `war3_model_hook.cpp/.h`
   - 新增 `attachedEffectInit*` 计数：
     - `attachedEffectInitBindCount`
     - `attachedEffectInitResolvedIdentityCount`
     - `attachedEffectInitResolvedUnitCount`
     - `attachedEffectInitResolvedHandleCount`
     - `attachedEffectInitResolvedRawcodeCount`
   - 新增 `attachedEffectDirect*` 计数：
     - `attachedEffectDirectBindCount`
     - `attachedEffectDirectResolvedIdentityCount`
   - 目的：把 `0x6F6BB2C0` 和 `0x6F6B9FF0` 从现有 `AttachModelToPoint` 统计里拆开签收。
2. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 同步新增：
     - `lastAttachedEffectInitOwnerWidgetPtr`
     - `lastAttachedEffectInitChildSpritePtr`
     - `lastAttachedEffectInitChildRuntimeModelPtr`
     - `lastAttachedEffectInitUnitPtr`
     - `lastAttachedEffectInitJHandle`
     - `lastAttachedEffectInitRawcode`
     - 以及对应的 `attachedEffectDirect*` latest sample
3. `war3_shadow_runtime_contract.cpp`
   - `snapshotAttachments()/snapshotAttachmentsShared()/snapshotBundleShared()`
     新增 live-attachment prefer：
     - 当 live attachment store 的 identity 完整度更高时，优先返回 live 修补版

#### 27.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. fresh 验证工件 A
   - `AutoTest/artifacts/codex_model_runtime_probe_20260422_171401.json`
   - 关键结果：
     - `attachedEffectInitBindCount = 17`
     - `attachedEffectInitResolvedIdentityCount = 16`
     - `attachedEffectDirectBindCount = 0`
     - `attachModelToPointBindCount = 17`
     - `attachModelToPointResolvedIdentityCount = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
   - 同轮 latest sample 还确认：
     - `lastAttachedEffectInitChildSpritePtr == lastAttachModelToPointChildSpritePtr`
     - `lastAttachedEffectInitChildRuntimeModelPtr == lastAttachModelToPointChildRuntimeModelPtr`
3. fresh 验证工件 B（加入 live-attachment prefer 后）
   - `AutoTest/artifacts/codex_model_runtime_probe_20260422_171928.json`
   - 关键结果：
     - `attachedEffectInitBindCount = 17`
     - `attachedEffectInitResolvedIdentityCount = 17`
     - `attachedEffectDirectBindCount = 0`
     - `attachModelToPointBindCount = 17`
     - `attachModelToPointResolvedIdentityCount = 0`
     - `attachmentRigidCount = 2`
     - `contractAttachmentRigidCount = 2`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`

#### 27.3 这轮 control-plane 真正证明了什么

1. `AttachedEffectInit` 这条 owner-widget 发布链在当前场景里是真热的
   - 而且它不是“空命中”：
     - 大部分样本已经能解析出 `unitPtr / jHandle / rawcode`
2. `AttachedEffectDirect (0x6F6B9FF0)` 不是当前场景的 primary caller
   - fresh 两轮都保持：
     - `attachedEffectDirectBindCount = 0`
3. 当前问题已经不能再定义成“上游 identity 没出来”
   - control-plane 现在能更准确地描述 blocker：
     - identity 至少已经在 `AttachedEffectInit` 层出来了
     - 但 current anonymous attachment rigid family 仍然没有在 contract / semantic-core 末端体现出来
4. `live attachment prefer` 修补没有改变 `contractAttachmentRigidCountWithAnyIdentity`
   - 所以“仅仅是旧匿名快照挡住了 live 修补”不是唯一 primary blocker

#### 27.4 当前下一步

1. 下一轮不再继续围绕 `0x6F6B9FF0` 加 probe
2. 下一轮应直接对齐：
   - `AttachedEffectInit / AttachModelToPoint` 当前热命中的 `childRuntimeModelPtr`
   - `attachmentRigid raw/contract` 当前匿名那 2 条 record 的 `root/owner/child runtime`
3. control-plane 最值得补的一刀已经收敛为：
   - 直接把 latest attachment rigid raw/contract runtime pointer sample 打出来
   - 先确认这 2 条匿名 attachment 到底是不是当前已经证实“有 identity”的那批 child runtime

### 28. 2026-04-22 control-plane 已证实匿名 attachment rigid 与 `AttachedEffectInit/AttachModelToPoint` 不是同一族 runtime

#### 28.1 本轮 control-plane 面改动

1. `war3_shadow_runtime_bridge.*`
   - 新增 raw / contract attachment 的 runtime 统计：
     - `attachmentRigidChildRuntimeOwnerIdentityCount`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount`
     - `attachmentRigidRootRuntimeOwnerIdentityCount`
     - `contractAttachmentRigidChildRuntimeOwnerIdentityCount`
     - `contractAttachmentRigidOwnerRuntimeOwnerIdentityCount`
     - `contractAttachmentRigidRootRuntimeOwnerIdentityCount`
   - 新增 raw / contract attachment runtime sample：
     - `attachmentRigidSample0/1{Root,Owner,Child}RuntimeModelPtr`
     - `contractAttachmentRigidSample0/1{Root,Owner,Child}RuntimeModelPtr`
   - 新增“是否命中当前 attached-effect family”的硬计数：
     - `attachmentRigidChildRuntimeMatchesAttachedEffectInitCount`
     - `attachmentRigidChildRuntimeMatchesAttachModelToPointCount`
     - `contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount`
     - `contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount`
2. `war3_control_plane.cpp`
   - 将上述字段全部并入 `get_shadow_runtime_summary`

#### 28.2 本轮验证

1. `ninja -C build32 -j1`
   - 两轮通过
2. first fresh probe
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_223445.json`
   - 关键结果：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `attachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidSample0ChildRuntimeModelPtr = 437458652`
     - `attachmentRigidSample1ChildRuntimeModelPtr = 437456388`
     - `lastAttachedEffectInitChildRuntimeModelPtr = 759095252`
     - `lastAttachModelToPointChildRuntimeModelPtr = 759095252`
3. second fresh probe（用于把“是否同一批 runtime”彻底钉死）
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_223931.json`
   - 完成时间点：`2026-04-22 22:39:31.157 +08:00`
   - 关键结果：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `attachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidChildRuntimeMatchesAttachedEffectInitCount = 0`
     - `attachmentRigidChildRuntimeMatchesAttachModelToPointCount = 0`
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount = 0`
     - `contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount = 0`
     - `lastAttachedEffectInitChildRuntimeModelPtr = 781639636`
     - `lastAttachModelToPointChildRuntimeModelPtr = 781639636`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`
     - `semanticCoreFrameFresh = true`

#### 28.3 control-plane 层面的新结论

1. 当前匿名 attachment rigid 已经可以明确排除 `AttachedEffectInit/AttachModelToPoint` 这条链
   - 因为两轮 fresh probe 都表明：
     - attachment raw / contract 的 child runtime 不命中当前 attached-effect family
2. 当前匿名 attachment rigid 也不是“identity 已存在但 contract 丢了”
   - 因为 raw / contract 两侧都同时表现为：
     - `*RuntimeOwnerIdentityCount = 0`
     - `*CountWithAnyIdentity = 0`
3. 所以 current blocker 应改写为：
   - `local-point / controller` 这条主路径当前生成了一族完全匿名的 attachment runtime tree
   - 这族 tree 还没有对回任何 `unitPtr / jHandle / rawcode`

#### 28.4 当前下一步

1. control-plane 下一轮不再继续补 `AttachedEffectInit` 相关对照字段
2. 下一轮 probe 应直接围绕：
   - `NoteAttachmentRigidRecord(...)` 当前这条 local-point 主路径
   - 以及其 `rootRuntime / ownerRuntime` 上游 owner/source
3. 目标已经收敛成一句话：
   - 不再问“这批匿名 attachment 是否来自 attached-effect family”
   - 而是直接问“这批匿名 attachment 自己的 owner/source 到底在哪里发布”

### 29. 2026-04-22 control-plane 已证明匿名 attachment rigid 既没有 arg-block identity，也没有 sprite binding

#### 29.1 本轮 control-plane / probe 面改动

1. `war3_model_hook.cpp/.h`
   - 先补了 `argBlock / arg4Block` identity-hint 观测字段：
     - `localPointArgBlockIdentityHintWriteCount`
     - `localPointArg4BlockIdentityHintWriteCount`
     - `lastArgBlockIdentityHintPtr/Offset`
     - `lastArg4BlockIdentityHintPtr/Offset`
   - 再补了 `parentSprite` owner-hint 字段：
     - `localPointParentSpriteIdentityHintWriteCount`
     - `lastParentSpriteIdentityHintSpritePtr`
     - `lastParentSpriteIdentityHintRuntimeModelPtr`
   - 最后新增一层只读观测：
     - `localPointSpriteBoundCandidateWriteCount`
     - `lastSpriteBoundCandidateSpritePtr`
     - `lastSpriteBoundCandidateRuntimeModelPtr`
2. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - 已把上述字段全部接进 pipe summary。

#### 29.2 本轮验证

1. `ninja -C build32 -j1`
   - 两轮通过。
2. first fresh probe
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_232247.json`
   - 关键结果：
     - `attachmentRigidCount = 2`
     - `contractAttachmentRigidCount = 2`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `overrideLocalPointArgBlockIdentityHintWriteCount = 0`
     - `overrideLocalPointArg4BlockIdentityHintWriteCount = 0`
     - `overrideLocalPointParentSpriteIdentityHintWriteCount = 0`
3. second fresh probe
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260422_232806.json`
   - 完成时间点：`2026-04-22 23:28:06 +08:00`
   - 关键结果：
     - `attachmentRigidCount = 3`
     - `contractAttachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `overrideLocalPointArgBlockIdentityHintWriteCount = 0`
     - `overrideLocalPointArg4BlockIdentityHintWriteCount = 0`
     - `overrideLocalPointSpriteBoundCandidateWriteCount = 0`
     - `overrideLocalPointParentSpriteIdentityHintWriteCount = 0`
     - `overrideLastSpriteBoundCandidateSpritePtr = 0`
     - `overrideLastSpriteBoundCandidateRuntimeModelPtr = 0`
     - `overrideLastParentSpriteIdentityHintSpritePtr = 0`
     - `overrideLastParentSpriteIdentityHintRuntimeModelPtr = 0`

#### 29.3 这轮 control-plane 真正证明了什么

1. 当前 anonymous attachment rigid 并不是“identity 已经挂在某个 sprite 上，只是 contract 没补回来”。
2. 现在 control-plane 已经能更硬地表述 blocker：
   - `argBlock / arg4Block` 没有直接 identity
   - runtime 候选没有 `spritePtr` 绑定
   - `parentSprite` repair 不命中
3. 这意味着当前 anonymous attachment 的 owner/source 不在当前 sprite repair 面里。
4. 更准确的 current blocker 应改写成：
   - `local-point / controller` 主路径正在发布一族完全匿名、且不在 sprite registry 里的 runtime tree
   - 必须到 sprite 之上的 source/object 发布层，才能重新拿到 `unitPtr / jHandle / rawcode`

#### 29.4 当前下一步

1. control-plane 下一轮不再继续扩 sprite-related repair 计数。
2. 下一轮 probe 应上抬到：
   - `0x6F12E900 / 0x6F12EB70` caller 参数来源
   - 或能同时看到 `source object + root runtime` 的更高层入口
3. 当前一句话目标已经收敛为：
   - 不再问“这批匿名 attachment 归哪个 sprite”
   - 而是直接问“这批匿名 attachment 在变成 runtime tree 之前归哪个 source object”

### 30. 2026-04-23 control-plane 已证明 `0x6F182300` 的 `a3` 外部上下文是真热，但 direct resolve 仍为 0

#### 30.1 本轮 control-plane / probe 面改动

1. `war3_model_hook.cpp/.h`
   - 新增 `sprite-frame source probe`，直接记录：
     - `spriteFrameSourceHintCount`
     - `spriteFrameSourceResolvedIdentityCount`
     - `lastSpriteFrameSourceObjectPtr`
     - `lastSpriteFrameSourceRuntimeModelPtr`
     - `lastSpriteFrameSourceUnitPtr / JHandle / Rawcode`
   - probe 来源固定为：
     - `Hook_SpriteFrameUpdate`
     - `Hook_SpriteMiniFrameUpdate`
   - 也就是 `CSpriteUber__PreRenderAndUpdatePosePalette` / `CSpriteMini_` 同层的 `a3` 外部上下文。
2. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 已新增上述 JSON 字段。

#### 30.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过。
2. fresh runtime 验证
   - 工件：`AutoTest/artifacts/codex_model_runtime_probe_20260423_000432.json`
   - 完成时间点：`2026-04-23 00:04:32 +08:00`
3. 关键结果：
   - `attachmentRigidCount = 2`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `spriteFrameSourceHintCount = 6`
   - `spriteFrameSourceResolvedIdentityCount = 0`
   - `lastSpriteFrameSourceObjectPtr = 447086720`
   - `lastSpriteFrameSourceRuntimeModelPtr = 745430608`

#### 30.3 这轮 control-plane 真正证明了什么

1. `0x6F182300` 的 `a3` 不是“根本没有命中”的死参数。
2. 现在 control-plane 已经能更硬地表述 blocker：
   - `a3` 在当前 `model_runtime_probe` 下是真热的外部上下文。
   - 但把它直接当 source object 去解，仍然拿不到任何 `unitPtr / jHandle / rawcode`。
3. 这意味着当前 anonymous attachment rigid 的 owner/source 仍然不在 `0x6F182300` 这一层可直接消费。
4. 更准确的 current blocker 应改写成：
   - `CSpriteUber__PreRenderAndUpdatePosePalette` 已经处在“接收外部 context”的阶段
   - 但 logical owner 还在更上游 caller 里，必须继续上抬才能重新拿到 object identity

#### 30.4 当前下一步

1. control-plane 下一轮不再继续把 `sprite-frame a3` 当 owner 本体扩 probe。
2. 下一轮 probe 应上抬到：
   - `0x6F182300` 的 caller
   - 或能同时看到 `source object + sprite/root runtime + a3 context` 的更高层入口
3. 当前一句话目标已经收敛为：
   - 不再问“`a3` 是不是 owner”
   - 而是直接问“谁在更上面把这块 `a3` context 传进来，并且当时手里还拿着真正的 source object”

### 26. 2026-04-22 `AttachModelToPoint` 已接进 control-plane summary，并确认这层 parent sprite 仍然匿名

#### 26.1 本轮 control-plane / probe 面改动

1. `war3_model_hook.cpp/.h`
   - 新增 `0x6F184E50` (`CSprite_AttachModelToPoint`) hook；
   - 该 hook 直接记录：
     - `parentSprite`
     - `childSprite`
     - `childRuntimeModel`
   - 并尝试从 parent sprite / parent runtime / parent sprite chain 回收 owner identity
2. `war3_shadow_runtime_contract.cpp`
   - attachment rigid repair 继续补了一层 `runtimeModel -> sprite -> parentSprite` 族谱恢复
3. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 新增：
     - `attachModelToPointBindCount`
     - `attachModelToPointResolvedIdentityCount`
     - `attachModelToPointResolvedUnitCount`
     - `attachModelToPointResolvedHandleCount`
     - `attachModelToPointResolvedRawcodeCount`
     - `lastAttachModelToPointParentSpritePtr`
     - `lastAttachModelToPointChildSpritePtr`
     - `lastAttachModelToPointChildRuntimeModelPtr`

#### 26.2 本轮验证

1. `ninja -C build32`
   - 通过
2. fresh runtime 验证
   - 使用 `run_named_scenario(model_runtime_probe)` 做 pipe-first 验证
   - 最新完成时间点：`2026-04-22 14:54:25 +08:00`
3. 当前新结果已经很硬：
   - `attachModelToPointBindCount = 18`
   - `attachModelToPointResolvedIdentityCount = 0`
   - `attachModelToPointResolvedUnitCount = 0`
   - `attachModelToPointResolvedHandleCount = 0`
   - `attachModelToPointResolvedRawcodeCount = 0`
   - `lastAttachModelToPointParentSpritePtr != 0`
   - `lastAttachModelToPointChildSpritePtr != 0`
   - `lastAttachModelToPointChildRuntimeModelPtr != 0`
4. 但 remaining attachment 计数仍然不变：
   - `attachmentRigidCount = 2`
   - `contractAttachmentRigidCount = 2`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`

#### 26.3 这轮控制面真正证明了什么

1. `AttachModelToPoint` 这层确实在跑
   - 所以“是不是函数根本没命中”现在可以从 blocker 列表里删掉
2. 但 `AttachModelToPoint` 这层没有 logical identity
   - 它能给我们 parent sprite / child sprite / child runtime
   - 却给不出 `unitPtr / jHandle / rawcode`
3. 这说明 control-plane 现在已经可以更准确地表述 blocker：
   - 不是 attachment contract 没 publish
   - 也不是 `AttachModelToPoint` 没命中
   - 而是 logical owner 仍然在更上游的 `object/widget -> sprite` 发布层

#### 26.4 当前下一步

1. 下一轮 probe 主线应继续上抬到：
   - `CreateAttachedEffect`
   - `CWidget_CreateAndAttachEffect`
   - `CWidget_CreateAndAttachEffectInternal`
2. 目标不再是继续围绕 `AttachModelToPoint` 做同层 repair，而是直接拿到：
   - logical object / widget
   - parent/root sprite
   - 以及两者之间的稳定 join key

### 31. 2026-04-23 control-plane 已证明 `WorldObjectEntry_Render` 不是 `sceneNode` 的写入点

#### 31.1 本轮 control-plane / summary 面改动

1. `war3_hook_render_identity.cpp/.h`
   - 新增 `RenderIdentityLifecycleProbeSummary`
   - `Hook_WorldObjectEntry_Render` 现直接统计：
     - `worldObjectEntryRenderCallCount`
     - `worldObjectEntryRenderSceneNodeReadyBeforeCount`
     - `worldObjectEntryRenderSceneNodeReadyAfterCount`
     - `worldObjectEntryRenderSceneNodeFilledByCallCount`
     - `worldObjectEntryRenderSceneNodeChangedCount`
     - latest `entry / sceneNode before / sceneNode after`
2. `war3_shadow_runtime_bridge.*`
   - `QueryShadowRuntimeBridgeSummary()` 已聚合这些 render-lifecycle probe 字段
3. `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 已直接暴露上述新字段
   - 外部不再需要去猜 `sceneNode` 是在哪层才变热

#### 31.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. fresh probe 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_20260423_render_identity_lifecycle.json`
   - 完成时间点：`2026-04-23 00:17:02 +08:00`
3. control-plane 关键输出：
   - `worldObjectEntryRenderCallCount = 59`
   - `worldObjectEntryRenderSceneNodeReadyBeforeCount = 59`
   - `worldObjectEntryRenderSceneNodeReadyAfterCount = 59`
   - `worldObjectEntryRenderSceneNodeFilledByCallCount = 0`
   - `worldObjectEntryRenderSceneNodeChangedCount = 0`

#### 31.3 这轮 control-plane 真正证明了什么

1. `entry + 0x20(sceneNode)` 在 `WorldObjectEntry_Render` 进来之前就已经是热的
2. 这层函数不会把 `sceneNode` 从空写成非空
3. 这层函数也不会把已有 `sceneNode` 改成别的值
4. 所以 render-layer 当前用来追回逻辑对象的 key：
   - 不是在 `WorldObjectEntry_Render` 才被写进去
   - 而是在更早的 world/list/source 发布层就已经准备好了

#### 31.4 control-plane 下一步

1. 下一轮 summary/probe 不应再继续围绕 `WorldObjectEntry_Render` 猜“是不是这里写 sceneNode”
2. 下一轮应继续上抬到：
   - `WorldObjectListEntry` 构建层
   - 或能同时看到 `source object + sprite/root runtime` 的更高层入口
3. 当前一句话目标已改写为：
   - 不是再问“`WorldObjectEntry_Render` 什么时候把对象认出来”
   - 而是直接问“更上面是谁先把 `worldObjectEntry + sceneNode + source object` 这组关系写好的”

### 32. 2026-04-23 control-plane 已证明“render 真消费到的 entry 全是 zero-owner”，且 `0x6F182300` 的 `a3/sourceObject` 不是 render-layer object

#### 32.1 本轮 control-plane / summary 面改动

1. `war3_hook_render_identity.cpp/.h`
   - 在 `Hook_WorldObjectListEntry_Write(0x6F0CB110)` 记 recent write
   - 在 `Hook_WorldObjectEntry_Render` 直接对回“当前 entry 最近一次 list write 的 ownerHint”
   - 新增 summary 字段：
     - `worldObjectEntryRenderKnownListOwnerHintZeroCount`
     - `worldObjectEntryRenderKnownListOwnerHintNonzeroCount`
     - `worldObjectEntryRenderUnknownListOwnerHintCount`
     - `lastWorldObjectEntryRenderResolvedListOwnerHintValue`
2. `war3_model_hook.cpp/.h`
   - 在 `TryResolveSourceObjectIdentity(...)` 新增 render-layer fallback：
     - `sourceObjectPtr -> worldObjectEntry`
     - `sourceObjectPtr + 0x20 -> sceneNode`
   - 新增 summary 字段：
     - `sourceObjectRenderBridgeResolvedByEntryCount`
     - `sourceObjectRenderBridgeResolvedBySceneNodeCount`
3. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - 已把上述新字段全部接入 `get_shadow_runtime_summary`

#### 32.2 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. refined list-writer 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_list_write_refine_20260423_023030.json`
   - 完成时间点：`2026-04-23 02:30:30.770 +08:00`
   - 关键输出：
     - `worldObjectListEntryWriteCallCount = 59`
     - `worldObjectListEntryWriteOwnerHintZeroCount = 59`
     - `worldObjectListEntryWriteOwnerHintNonzeroCount = 0`
     - `worldObjectEntryRenderCallCount = 59`
     - `worldObjectEntryRenderKnownListOwnerHintZeroCount = 59`
     - `worldObjectEntryRenderKnownListOwnerHintNonzeroCount = 0`
     - `worldObjectEntryRenderUnknownListOwnerHintCount = 0`
3. source-object render-bridge 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_source_render_bridge_20260423_023835.json`
   - 完成时间点：`2026-04-23 02:38:35.842 +08:00`
   - 关键输出：
     - `worldObjectListEntryWriteCallCount = 76`
     - `worldObjectListEntryWriteOwnerHintZeroCount = 59`
     - `worldObjectListEntryWriteOwnerHintNonzeroCount = 17`
     - `worldObjectEntryRenderCallCount = 59`
     - `worldObjectEntryRenderKnownListOwnerHintZeroCount = 59`
     - `worldObjectEntryRenderKnownListOwnerHintNonzeroCount = 0`
     - `worldObjectEntryRenderUnknownListOwnerHintCount = 0`
     - `sourceObjectRenderBridgeResolvedByEntryCount = 0`
     - `sourceObjectRenderBridgeResolvedBySceneNodeCount = 0`
     - `spriteFrameSourceHintCount = 29`
     - `spriteFrameSourceResolvedIdentityCount = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`

#### 32.3 这轮 control-plane 真正证明了什么

1. 当前 `model_runtime_probe` 下真正进入 render 的 `59` 条 entry，最近一次 list write 全都可回溯，而且全部是 `ownerHint = 0`
2. list build 整体并非“完全没有 nonzero owner”
   - 第二轮 fresh 里仍有 `17` 条 nonzero ownerHint write
   - 但这些 nonzero owner entry 并没有进入当前这批真正被 render 消费的 `59` 条 entry
3. 把 render-layer recover 方法直接套到 `0x6F182300` 的 `a3/sourceObject` 上，本轮也已经实证失败：
   - `sourceObjectRenderBridgeResolvedByEntryCount = 0`
   - `sourceObjectRenderBridgeResolvedBySceneNodeCount = 0`
   - `spriteFrameSourceHintCount = 29`，但 resolved 仍是 `0`
4. 所以 control-plane 现在可以更准确地表述 blocker：
   - render-layer object 这条“胸牌链”是真的存在
   - 但 `CSpriteUber__PreRenderAndUpdatePosePalette` 的 `a3` 并不是那张胸牌
   - 真正 owner 还在构造这块 `a3` context 的 caller 之上

#### 32.4 当前下一步

1. 下一轮不再继续做：
   - “把 `a3/sourceObject` 当 render object 去试”
   - “把 `WorldObjectEntry + 0x14` 当作当前 render entry 的稳定 owner”
2. 下一轮应固定上抬到：
   - `0x6F182300 / 0x6F1820C0` 的 caller
   - 查“谁在 caller 层同时拿着 logical/source object、sprite/root runtime、以及传入 pre-render 的 `a3` context”
3. 当前一句话目标已更新为：
   - 不是再问“能不能在当前层把 `a3` 认成对象”
   - 而是直接问“谁在更上面生成并传下这块 context，同时还持有真正 owner”

#### 33.1 本轮实现

1. `war3_model_hook.cpp/.h`
   - 新增 render-side owner fallback helper：
     - `TryResolveCurrentRenderOwnerHint(...)`
     - `MergeAttachmentIdentityFromRender(...)`
   - 已尝试从以下现成 TLS/context 直接补 owner：
     - `War3Renderer::GetCurrentWorldObjectEntry/GetCurrentSceneNode`
     - `War3RenderState::GetTlsShadowSemanticState()`
     - `render::GetCurrentBatchObject()`
   - 这些 fallback 已接入：
     - `RecordSpriteHostOwnerBinding(...)`
     - `RecordAttachedEffectInitOwnerBinding(...)`
     - `RecordAttachedEffectDirectOwnerBinding(...)`
     - `TryResolveAttachModelToPointOwner(...)`
     - `MaybeRecordSpriteFrameSourceIdentity(...)`
     - `NoteAttachmentRigidRecord(...)`
   - 新增 summary 字段：
     - `currentRenderIdentityHintCount`
     - `currentRenderIdentityResolvedCount`
     - `lastCurrentRenderIdentityWorldObjectEntryPtr`
     - `lastCurrentRenderIdentitySceneNodePtr`
     - `lastCurrentRenderIdentityUnitPtr`
2. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 已暴露上述字段

#### 33.2 本轮验证

1. `ninja -C build32 -j1`
   - 两轮均通过
2. render world-object TLS 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_tls_owner_bridge_20260423_0310.json`
   - 关键输出：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `runtimeOwnerIdentityCount = 21`
     - `completeIdentityCount = 21`
     - `currentRenderIdentityHintCount = 68`
     - `currentRenderIdentityResolvedCount = 0`
3. dispatch semantic/current-batch TLS 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_tls_semantic_owner_bridge_20260423_0318.json`
   - 关键输出：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `runtimeOwnerIdentityCount = 21`
     - `completeIdentityCount = 21`
     - `currentRenderIdentityHintCount = 69`
     - `currentRenderIdentityResolvedCount = 0`
     - `lastCurrentRenderIdentityWorldObjectEntryPtr = 0`
     - `lastCurrentRenderIdentitySceneNodePtr = 0`
     - `lastCurrentRenderIdentityUnitPtr = 0`

#### 33.3 这轮 control-plane 真正证明了什么

1. 现在可以明确排掉两条 render-side backfill 假设：
   - `WorldObjectEntry_Render` 那根 current world-object TLS 太短，吃不到当前 model hooks
   - `ExecBatch` 的 semantic/current-batch TLS 在这些 model hooks 时机里同样不在场
2. 因为 `currentRenderIdentityHintCount > 0` 但 `currentRenderIdentityResolvedCount = 0`，所以这不是“接线漏了”，而是 live timing 上确实拿不到这些 context
3. 当前 blocker 因而继续收紧为：
   - anonymous attachment 的 owner 身份不在当前 render hot path 的现成 TLS/context 里
   - 必须继续从 `0x6F182300 / 0x6F1820C0` 之上的 producer/caller 链拿

#### 33.4 当前下一步

1. 停止继续做：
   - render-side TLS backfill
   - current-batch/current-world-object 当前层实验
2. 下一轮固定主线：
   - 继续上抬 `CreateSpriteAndBindSourceObject(0x6F6BD110)` / `CreateSpriteRuntimeFromSourceObject(0x6F185250)` / `CSpriteUber_PreRenderAndUpdatePosePalette(0x6F182300)` 的 caller / producer 链
   - 目标仍是找到第一次同时持有 `logical/source object + sprite/root runtime + prerender context` 的 authoritative producer

#### 33.5 本轮实现

1. `war3_model_registry.*`
   - `ModelInstanceRecord` / `AttachmentRigidRecord` 新增：
     - `sourceObjectPtr`
     - `sourceSpriteObjectPtr`
   - `ModelInstanceRegistry` 新增：
     - `noteRuntimeSourceObject(...)`
     - `runtimeSourceObjectCount()`
2. `war3_model_hook.*`
   - source-object runtime publication 已接入：
     - `RecordSpriteHostOwnerBinding(...)`
     - `RecordAttachedEffectInitOwnerBinding(...)`
     - `RecordAttachedEffectDirectOwnerBinding(...)`
     - `RecordAttachModelToPointOwnerBinding(...)`
     - `MaybeRecordSpriteFrameSourceIdentity(...)`
   - `NoteAttachmentRigidRecord(...)` 现在会先从 `child/owner/root runtime` 回收 source-object bloodline，再尝试补做一次 source-object identity resolve
   - 新增 summary 字段：
     - `runtimeSourceObjectPublishCount`
     - `attachmentRigidPublishedWithSourceObjectCount`
     - `attachmentRigidSourceObjectFromChild/Owner/RootRuntimeCount`
     - `lastRuntimeSourceObjectPtr`
     - `lastRuntimeSourceRuntimeModelPtr`
     - `lastAttachmentRigidSourceObjectPtr`
3. `war3_shadow_runtime_contract.*`
   - `ShadowAttachmentRigidRecord` 也已携带：
     - `sourceObjectPtr`
     - `sourceSpriteObjectPtr`
4. `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
   - `get_shadow_runtime_summary` 新增：
     - `runtimeSourceObjectCount`
     - `attachmentRigidCountWithSourceObject`
     - `contractAttachmentRigidCountWithSourceObject`
     - attachment sample 的 `SourceObjectPtr`

#### 33.6 本轮验证

1. `ninja -C build32 -j1`
   - 通过
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_source_object_producer_20260423_033623.json`
3. 关键输出
   - source-object producer side：
     - `runtimeSourceObjectCount = 43`
     - `runtimeSourceObjectPublishCount = 49`
     - `lastRuntimeSourceObjectPtr = 455868544`
     - `lastRuntimeSourceRuntimeModelPtr = 787416552`
   - anonymous attachment side：
     - `attachmentRigidCount = 3`
     - `attachmentRigidCountWithSourceObject = 0`
     - `attachmentRigidPublishedWithSourceObjectCount = 0`
     - `attachmentRigidSourceObjectFromChildRuntimeCount = 0`
     - `attachmentRigidSourceObjectFromOwnerRuntimeCount = 0`
     - `attachmentRigidSourceObjectFromRootRuntimeCount = 0`
   - contract side：
     - `contractAttachmentRigidCount = 3`
     - `contractAttachmentRigidCountWithSourceObject = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - correctness：
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 29`

#### 33.7 这轮 control-plane 真正证明了什么

1. 现在已经能证明“source-object contract 没接上”这条假设不成立：
   - producer runtime family 自己是热的，control-plane 能稳定看到 `runtimeSourceObjectCount / runtimeSourceObjectPublishCount`
2. 但 fresh probe 同时也证明：
   - 当前 `3` 条 anonymous attachment rigid record 完全没有 source-object bloodline
   - raw attachment 和 contract attachment 两侧都一样是 `0`
3. 因而最新 hard conclusion 是：
   - 当前 anonymous attachment 不是 `CreateSpriteAndBindSourceObject / CSpriteUber_PreRenderAndUpdatePosePalette` 这条 producer family 的后代
   - 真正缺失 owner 的，是另一族 runtime producer

#### 33.8 当前下一步

1. 下一轮不再继续围着当前 source-object producer family 猜 owner
2. 下一轮固定改为：
   - 直接围绕当前 anonymous attachment 的 sample runtime trio
     - `rootRuntimeModelPtr`
     - `ownerRuntimeModelPtr`
     - `childRuntimeModelPtr`
     去追“哪条 producer/caller 在生成这棵 runtime tree”
3. 也就是说，当前问题已经不再是：
   - “source-object contract 有没有接好”
   而是：
   - “当前 anonymous attachment 根本属于哪一族 runtime family”

#### 33.9 2026-04-23 control-plane 已进一步证明：既不是 source bloodline 没沿 runtime tree 传播，也不是漏了 direct `CreateSpriteRuntime(0x6F185250)` publication

1. 本轮代码
   - `war3_model_registry.*`
     - 新增 `propagateRuntimeSourceObjectLocked(...)`
     - `noteRuntimeSourceObject(...)` 现在会沿 runtime tree 传播
       - `sourceObjectPtr`
       - `sourceSpriteObjectPtr`
   - `war3_model_hook.cpp`
     - `RecordRuntimeModelBinding(...)` / `Hook_CreateSpriteRuntime(0x6F185250)` 新增：
       - `PublishRuntimeSourceObject(runtimeModelPtr, spritePtr, sourcePtr, TryReadSourceSpriteObjectPtr(sourcePtr))`
2. 验证工件
   - 中间证伪工件：
     - `AutoTest/artifacts/codex_model_runtime_probe_source_object_propagation_20260423_034542.json`
   - 最终 fresh 工件：
     - `AutoTest/artifacts/codex_model_runtime_probe_create_runtime_source_20260423_034828.json`
3. 最终 fresh 输出
   - `runtimeSourceObjectCount = 42`
   - `runtimeSourceObjectPublishCount = 48`
   - `attachmentRigidCount = 3`
   - `attachmentRigidCountWithSourceObject = 0`
   - `attachmentRigidPublishedWithSourceObjectCount = 0`
   - `attachmentRigidSourceObjectFromChildRuntimeCount = 0`
   - `attachmentRigidSourceObjectFromOwnerRuntimeCount = 0`
   - `attachmentRigidSourceObjectFromRootRuntimeCount = 0`
   - `contractAttachmentRigidCount = 3`
   - `contractAttachmentRigidCountWithSourceObject = 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
   - `attachmentRigidSample0SourceObjectPtr = 0`
   - `contractAttachmentRigidSample0SourceObjectPtr = 0`
4. 当前 control-plane 能正式回答：
   - 不是“source 只挂在 root runtime，没传到 child”
   - 也不是“匿名 attachment 只是 direct `0x6F185250` 路径，但我们没发 source”
   - 因为这两条现在都已经 live-tested 过，结果 attachment 仍然 `0 sourceObject`
5. 所以 current hard conclusion 已升级为：
   - 当前 anonymous attachment rigid 的 sample runtime trio 来自另一族 producer/caller
   - 这族 producer 当前不在我们已经接线的：
     - `CreateSpriteAndBindSourceObject(0x6F6BD110)`
     - `CreateSpriteRuntimeFromSourceObject(0x6F185250)`
     这组 source-object publication 面里

#### 33.10 当前下一步

1. 下一轮不再继续补同类 source propagation / source publication
2. 下一轮固定围绕 fresh sample runtime trio：
   - `rootRuntimeModelPtr = 457945124`
   - `ownerRuntimeModelPtr = 457944896`
   - `childRuntimeModelPtr = 427038428`
3. 主线改为：
   - 直接往 local-point/controller 所在 runtime family 的 producer/caller 链上抬
   - 找“第一次同时拥有 anonymous runtime tree 与 logical owner/source”的位置

#### 33.11 2026-04-23 control-plane 已进一步证明：chosen child-link 的 `+0x0C source meta` 也不是当前 anonymous attachment 的 direct owner/source

1. 本轮代码
   - `war3_model_hook.*`
     - `RuntimeChildLinkProbeRecord` 新增 `sourceMeta`
     - `CollectDirectChildRuntimeLinks(...)` 现在会读取 `linkNode + 0x0C`
     - 新增一个高置信 probe：
       - 仅当 `linkNode + 0x0C` 可读，且 `TryResolveSourceObjectIdentity(...)` 成功时
       - 才会记 `overrideLocalPointChildSourceMetaIdentityHintWriteCount`
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
     - `get_shadow_runtime_summary` 新增：
       - `overrideLocalPointChildSourceMetaIdentityHintWriteCount`
       - `overrideLastChildSourceMetaPtr`
       - `overrideLastChildSourceMetaRuntimeModelPtr`
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_child_source_meta_20260423_035849.json`
3. 关键输出
   - `overrideLocalPointChildSourceMetaIdentityHintWriteCount = 0`
   - `overrideLastChildSourceMetaPtr = 0`
   - `overrideLastChildSourceMetaRuntimeModelPtr = 0`
   - `attachmentRigidCount = 3`
   - `attachmentRigidCountWithSourceObject = 0`
   - `contractAttachmentRigidCountWithSourceObject = 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
4. 当前 control-plane 可以更明确地说：
   - chosen child-link 自己的 `+0x0C` 槽，在当前场景下并没有直接给出一个可消费的 owner/source object
   - 所以当前 anonymous attachment 的 owner 缺口也不在这里
5. 因而当前 blocker 再次收紧：
   - 不能再继续靠 child-link slot 内联 recover
   - 必须继续上抬到 local-point/controller 所在 runtime family 的 producer/caller

#### 33.12 当前下一步

1. 下一轮不再继续做 child-link 就地 recover
2. 下一轮固定围绕 fresh sample runtime trio：
   - `rootRuntimeModelPtr = 463319076`
   - `ownerRuntimeModelPtr = 463318848`
   - `childRuntimeModelPtr = 432412380`
3. 目标：
   - 直接找“谁创建/发布了这组三元 runtime”
   - 找“谁第一次同时持有 logical owner/source + root runtime + controller/local-point context”

#### 33.13 2026-04-23 control-plane 已新增 runtime create provenance：`0x6F12A5C0` 是热的，但当前 anonymous attachment trio 仍然完全没有 create caller

1. 本轮代码
   - `war3_model_registry.*`
     - `ModelInstanceRecord` 新增：
       - `runtimeCreatorModelDataPtr`
       - `runtimeCreatorCallerRva`
     - `ModelInstanceRegistry` 新增：
       - `noteRuntimeCreationProvenance(...)`
       - `runtimeCreationProvenanceCount()`
   - `war3_model_hook.*`
     - 新增 `Hook_PromoteRuntimeModel(0x6F12A5C0)`
     - 新增 runtime create 统计：
       - `runtimeModelCreateCount`
       - `runtimeModelCreateCallerBuildChildLinksCount`
       - `runtimeModelCreateCallerCreateSpriteRuntimeCount`
       - `runtimeModelCreateCallerOtherCount`
       - `lastRuntimeModelCreateCallerRva`
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
     - 新增 attachment raw/contract create provenance 输出：
       - `attachmentRigid*CreateCallerKnownCount`
       - `attachmentRigidSample{0,1}*CreateCallerRva`
       - `contractAttachmentRigidSample{0,1}*CreateCallerRva`
2. IDA 本轮确认
   - `0x6F12A3C0 -> ModelHandle_TryResolveRuntimeModel`
   - `0x6F12A5C0 -> CModelData_PromoteToRuntimeModel`
   - 当前 hot create 的 `lastRuntimeModelCreateCallerRva = 0x12A3E8` 正好落在 `ModelHandle_TryResolveRuntimeModel` 内
3. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_runtime_create_provenance_20260423_044138.json`
4. 关键输出
   - `runtimeModelCreateCount = 18`
   - `runtimeModelCreateCallerBuildChildLinksCount = 0`
   - `runtimeModelCreateCallerCreateSpriteRuntimeCount = 0`
   - `runtimeModelCreateCallerOtherCount = 18`
   - `runtimeCreationProvenanceCount = 18`
   - `lastRuntimeModelCreateCallerRva = 1221608 (0x12A3E8)`
   - `attachmentRigidChild/Owner/RootRuntimeCreateCallerKnownCount = 0 / 0 / 0`
   - `contractAttachmentRigidChild/Owner/RootRuntimeCreateCallerKnownCount = 0 / 0 / 0`
   - `attachmentRigidSample0*CreateCallerRva = 0 / 0 / 0`
   - `contractAttachmentRigidSample0*CreateCallerRva = 0 / 0 / 0`
5. 当前 control-plane 可以更明确地说：
   - `0x6F12A5C0` 这条 create family 不是空的
   - 而且当前 probe 里的 hot create 全都来自 `0x6F12A3C0` wrapper family
   - 但 anonymous attachment trio 根本不在这条 create family 里

#### 33.14 2026-04-23 control-plane 已进一步证明：即便补上 `0x6F130D90` direct-init fallback，anonymous attachment trio 仍不在当前 hot init family

1. 本轮代码
   - `war3_model_hook.*`
     - 新增 `Hook_RuntimeInitFromModelData(0x6F130D90)`
     - 只在该 runtime 还没有 create provenance 时才补发
     - 新增：
       - `runtimeModelInitCopyCount`
       - `runtimeModelInitCopyPublishedFallbackCount`
       - `lastRuntimeModelInitCallerRva`
       - `lastRuntimeModelInitModelDataPtr`
       - `lastRuntimeModelInitRuntimeModelPtr`
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_runtime_init_fallback_20260423_044711.json`
3. 关键输出
   - `runtimeModelInitCopyCount = 0`
   - `runtimeModelInitCopyPublishedFallbackCount = 0`
   - `lastRuntimeModelInitCallerRva = 0`
   - `attachmentRigidCount = 3`
   - `attachmentRigidSample0 = (456306724 / 456306496 / 425400028)`
   - `attachmentRigidSample0*CreateCallerRva = 0 / 0 / 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
4. 当前 control-plane 的更硬结论
   - 当前 anonymous attachment trio 不只是“不在 `0x6F12A5C0` hot create family”
   - 它们也不在当前 hot `0x6F130D90` direct-init fallback family
   - 同时：
     - `runtimeChildLinkBuildCount = 0`
     - `attachmentAncestorIdentityHintWriteCount = 0`
   - 所以 current blocker 已进一步收紧为：
     - 这批 trio 来自另一族 producer/construction family
     - 或者来自当前 hot-shadow 观察窗之前的更早创建阶段

#### 33.15 当前下一步

1. 下一轮不再继续扩 `0x6F12A3C0 / 0x6F12A5C0 / 0x6F130D90 / 0x6F131F60`
2. 下一轮固定围绕 fresh sample runtime trio：
   - `rootRuntimeModelPtr = 456306724`
   - `ownerRuntimeModelPtr = 456306496`
   - `childRuntimeModelPtr = 425400028`
3. 目标：
   - 找“这棵 anonymous runtime tree 在第一次进入 local-point/controller 语义面之前，是由谁更早创建/发布的”
   - 优先怀疑：
     - 更早创建阶段
     - 或另一族不走当前 hot `PromoteRuntimeModel / CopyFromModelData` family 的 constructor/publisher

#### 33.16 2026-04-23 control-plane 已把 probe 上抬到 `CModel/CModelComplex` constructor family，并确认 anonymous attachment trio 也不在当前 hot ctor 面里

1. 本轮代码
   - `war3_model_hook.*`
     - 新增：
       - `Hook_RuntimeModelPlainCtor(0x6F121880)`
       - `Hook_RuntimeModelComplexCtor(0x6F1219C0)`
     - 新增 summary 字段：
       - `runtimeModelCtorCount`
       - `runtimeModelComplexCtorCount`
       - `runtimeModelPlainCtorCount`
       - `runtimeModelCtorCallerPromoteCount`
       - `runtimeModelCtorCallerOtherCount`
       - `lastRuntimeModelCtorRuntimeModelPtr`
       - `lastRuntimeModelCtorCallerRva`
       - `lastRuntimeModelCtorKind`
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
     - ctor counters / last sample 已进入 `get_shadow_runtime_summary`
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_runtime_ctor_family_20260423_0500.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_runtime_ctor_family_retry_20260423_0506.json`
3. 关键输出（两轮一致）
   - `runtimeModelCtorCount = 513`
   - `runtimeModelComplexCtorCount = 0`
   - `runtimeModelPlainCtorCount = 513`
   - `runtimeModelCtorCallerPromoteCount = 18`
   - `runtimeModelCtorCallerOtherCount = 495`
   - `lastRuntimeModelCtorCallerRva = 1222186 (0x12A62A)`
   - `lastRuntimeModelCtorKind = 1`
   - `attachmentRigidCount = 3`
   - `attachmentRigidSample0Root/Owner/ChildRuntimeCreateCallerRva = 0 / 0 / 0`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
4. 当前 control-plane 的更硬结论
   - 当前 hot constructor 面不是空白：
     - plain `CModel_Ctor` 是热的
     - 而且不仅有 `PromoteRuntimeModel` 家族，还存在大量 `other ctor caller family`
   - 但 anonymous attachment trio 仍然完全不在这个 hot ctor 面里
   - 所以 blocker 已再次收紧为：
     - trio 更像创建于当前 hook 观察窗之前的更早生命周期阶段
     - 或发生在 hook 安装前，而不是当前窗口里漏掉了某个 ctor wrapper

#### 33.17 当前下一步

1. 下一轮不再继续细挖 `0x6F121880 / 0x6F1219C0 / 0x6F128140 / 0x6F128220 / 0x6F12A400`
2. 下一轮固定目标：
   - 对齐 `war3_model_hook` 的真实安装时机
   - 继续验证 anonymous trio 是否在 hook 安装之前就已完成创建/发布
   - 若证据成立，就把 provenance probe 前移到更早 bootstrap/lifecycle 时机

#### 33.18 2026-04-23 control-plane 已验证：把 model hook 前移到 bootstrap 后，anonymous attachment trio 首次恢复了 owner-runtime create provenance

1. 本轮代码
   - `d3d9_war3_hook.cpp`
     - bootstrap 阶段新增：
       - `war3::model::Init(gameInfo.base);`
   - 目的不是改 consumer，而是验证 late-install 是否真在吃掉 trio 的 create provenance
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_bootstrap_model_init_20260423_0512.json`
3. 关键输出
   - `attachmentRigidOwnerRuntimeCreateCallerKnownCount = 3`
   - `contractAttachmentRigidOwnerRuntimeCreateCallerKnownCount = 3`
   - `attachmentRigidSample0OwnerRuntimeCreateCallerRva = 1221608 (0x12A3E8)`
   - `contractAttachmentRigidSample0OwnerRuntimeCreateCallerRva = 1221608 (0x12A3E8)`
   - `attachmentRigidSample0RootRuntimeCreateCallerRva = 0`
   - `attachmentRigidSample0ChildRuntimeCreateCallerRva = 0`
   - `runtimeModelCtorCount = 1863`
   - `runtimeModelComplexCtorCount = 561`
   - `runtimeModelPlainCtorCount = 1302`
   - `runtimeModelCtorCallerPromoteCount = 685`
   - `runtimeModelCtorCallerOtherCount = 1178`
   - `runtimeModelCreateCount = 685`
   - `contractAttachmentRigidCountWithAnyIdentity = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
4. 当前 control-plane 的更硬结论
   - late-install 不是猜测，而是真 blocker 之一：
     - 只把 model hook 安装前移
     - anonymous trio 的 owner runtime provenance 就恢复了
   - 当前 owner runtime 已经不再是“完全无历史”，而是明确落在
     - `ModelHandle_TryResolveRuntimeModel(0x6F12A3C0)` family
   - 但 root runtime / child runtime 仍未恢复 provenance
   - semantic identity 仍然没有闭环

#### 33.19 当前下一步

1. 下一轮不再泛化地继续挖 ctor family 本体
2. 下一轮固定围绕：
   - `ownerRuntimeModelPtr`
   - `ownerRuntimeCreateCallerRva = 0x12A3E8`
3. 目标：
   - 从 `ModelHandle_TryResolveRuntimeModel` 往上追到 `handle/modelData/source object` 的 identity 发布层
   - 判断 root/child runtime 是否仍属于另一条更早、仍未覆盖的发布链

#### 33.20 2026-04-23 control-plane 已验证：当前 anonymous attachment sample child runtime 不在 runtime parent-link 图里

1. 本轮代码
   - `war3_model_hook.h/.cpp`
     - 新增只读查询：
       - `RuntimeParentLinkQueryResult`
       - `QueryRuntimeParentLink(childRuntimeModelPtr, ...)`
   - `war3_shadow_runtime_bridge.h/.cpp`
     - 新增 raw/contract attachment sample parent-link 对账字段：
       - `attachmentRigidChildRuntimeParentLinkKnownCount`
       - `contractAttachmentRigidChildRuntimeParentLinkKnownCount`
       - `attachmentRigidSample0/1ChildRuntimeParentRuntimeModelPtr`
       - `attachmentRigidSample0/1ChildRuntimeParentLinkSourceMeta`
       - `attachmentRigidSample0/1ChildRuntimeParentLinkLastSeenFrame`
       - contract mirror 字段
   - `war3_control_plane.cpp`
     - 将这些字段并入 `get_shadow_runtime_summary`
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_parent_link_20260423_134238.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_parent_link_retry_20260423_134414.json`
3. 关键输出（两轮一致）
   - `useIsolatedDesktop = true`
   - `windowed = true`
   - `attachmentRigidCount = 2`
   - `attachmentRigidChildRuntimeParentLinkKnownCount = 0`
   - `contractAttachmentRigidChildRuntimeParentLinkKnownCount = 0`
   - `attachmentRigidSample0ChildRuntimeParentRuntimeModelPtr = 0`
   - `attachmentRigidSample1ChildRuntimeParentRuntimeModelPtr = 0`
   - `runtimeChildLinkBuildCount = 505`
   - `runtimeChildLinkBuiltChildCount = 2489`
   - `attachmentAncestorIdentityHintWriteCount = 0`
   - `semanticCoreAttachmentRigidResolved = 0`
4. control-plane 新结论
   - 当前 `BuildChildRuntimeModelLinks` family 是热的，不存在“child-link hook 没装上”的问题
   - 但 hot-shadow summary 里的 anonymous attachment sample child runtime 完全没有 parent-link 记录
   - 所以当前 miss family 已不该再解释为“ancestor merge 没把已有 parent-link 身份合并下来”
   - 更准确的结论是：
     - sample child runtime 根本不属于当前捕到的 `BuildChildRuntimeModelLinks` parent-link family
5. 当前下一步
   - 不再继续在 `MergeAttachmentHintsFromAncestorRuntimes(...)` 层 blind-fix
   - 直接继续上抬 sample trio 的更早 producer/construction family
   - 优先查它们是否来自：
     - `ModelHandle_TryResolveRuntimeModel` 之外的另一族 runtime create/publish family
     - 或 hook 安装前的更早 publish 窗口

#### 33.21 2026-04-23 control-plane 已验证：最新 anonymous attachment sample 已经进了 parent-link 图，但仍完全不在 runtime/model/resource/pose/source-object registry 里

1. 本轮代码
   - `war3_shadow_runtime_bridge.h/.cpp`
     - 新增 raw/contract attachment sample 的 runtime-registry 对账字段：
       - `attachmentRigidChildRuntimeRecordKnownCount`
       - `attachmentRigidChildRuntimeModelResourceKnownCount`
       - `attachmentRigidChildRuntimePoseKnownCount`
       - contract mirror 字段
     - 新增 sample 级 child runtime 细节：
       - `CreateModelDataPtr`
       - `SourceObjectPtr`
       - `ModelResourcePtr`
       - `ModelKey`
       - `PoseMatrixCount`
   - `war3_control_plane.cpp`
     - 将上述字段并入 `get_shadow_runtime_summary`
   - `war3_model_hook.cpp`
     - `PublishRuntimeSourceObject(...)` 增加 source publish -> owner identity 的窄 fallback
   - `war3_shadow_runtime_contract.cpp`
     - `TryResolveRuntimeModelSemanticKey(...)` 增加 `OwnedModelDataHandle -> direct modelResourcePtr` fallback
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_child_registry_20260423_145545.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_owner_identity_from_source_20260423_151209.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_direct_semantic_key_20260423_151823.json`
3. 关键输出
   - 所有 fresh run：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - `child_registry`：
     - `attachmentRigidChildRuntimeRecordKnownCount = 0`
     - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `attachmentRigidChildRuntimePoseKnownCount = 0`
     - `contractAttachmentRigidChildRuntimeRecordKnownCount = 0`
     - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `contractAttachmentRigidChildRuntimePoseKnownCount = 0`
     - 但同时：
       - `attachmentRigidChildRuntimeParentLinkKnownCount = 2`
       - `contractAttachmentRigidChildRuntimeParentLinkKnownCount = 2`
   - `owner_identity_from_source`：
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidRootRuntimeOwnerIdentityCount = 0`
     - `attachmentRigidChildRuntimeOwnerIdentityCount = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
   - `direct_semantic_key`：
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
4. control-plane 新结论
   - 需要修正上一轮 `33.20` 的结论：当前最新 anonymous attachment sample 已经进了 parent-link 图，不再是“完全没有 parent-link”
   - 但当前 anonymous attachment 仍然完全不在：
     - `ModelInstanceRegistry`
     - `ShadowModelResourceCache`
     - `PoseRegistry`
     - `source-object` 发布链
   - 因此当前 blocker 已收敛为：
     - `parent-link family 已有`
     - `registry family 仍缺席`
   - 两条本轮窄 fallback 都没有把 identity 抬起来：
     - source publish -> owner identity
     - direct handle -> semantic key
5. 当前下一步
   - 不再继续在 `ancestor merge`、`source publish auto owner identity`、`direct semantic key fallback` 这三层 blind-fix
   - 直接围绕 fresh sample 的：
     - `childRuntimeModelPtr`
     - `parentRuntimeModelPtr`
     - `ownerRuntimeModelPtr`
     - `sourceMeta = 3 / 10`
   - 上抬去找这条 parent-link family 所属的更早 builder/publisher 契约

#### 33.22 2026-04-23 control-plane 已验证：`RecordObservedRuntimeChildLink` direct bootstrap 与 `BuildChildRuntimeModelLinks` child-group pairing 都打不中当前 anonymous attachment sample，因此当前 sample parent-link family 不是当前 hot build family

1. 本轮代码
   - `war3_model_hook.cpp`
     - 先补齐 `BootstrapRuntimeModelResourceLineage(...)` 的前置声明，修复上一轮纯编译问题；
     - 保留 `RecordObservedRuntimeChildLink(...)` 里的 runtime direct bootstrap；
     - 新增资源侧 child-group pairing：
       - `ModelDataChildLinkProbeRecord`
       - `CollectModelDataChildRuntimeLinks(...)`
       - `BootstrapRuntimeChildLineageFromModelData(...)`
     - 直接使用 `CModelDataOffsets::ChildRuntimeGroupRecords(0xC8)` / `ChildRuntimeGroupCount(0xD0)` 读取资源侧 child group，并把 `child modelData -> child runtime` 结果回灌到：
       - `ModelInstanceRegistry::noteRuntimeCreationProvenance(...)`
       - `ShadowModelResourceCache::noteRuntimeModelBinding(...)`
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_child_link_bootstrap_20260423_155157.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_child_group_bridge_20260423_160930.json`
3. 关键输出
   - 两轮 fresh run 都继续保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - `child_link_bootstrap`：
     - `attachmentRigidCount = 2`
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `attachmentRigidChildRuntimeRecordKnownCount = 0`
     - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `attachmentRigidChildRuntimePoseKnownCount = 0`
     - `attachmentRigidChildRuntimeParentLinkKnownCount = 2`
   - `child_group_bridge`：
     - `attachmentRigidChildRuntimeRecordKnownCount = 0`
     - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `attachmentRigidChildRuntimePoseKnownCount = 0`
     - sample child：
       - `CreateModelDataPtr = 0`
       - `ModelResourcePtr = 0`
   - 本轮最关键的 family 对照：
     - `attachmentRigidSample0RootRuntimeModelPtr = 466385192`
     - `attachmentRigidSample1RootRuntimeModelPtr = 466387124`
     - `lastRuntimeChildLinkBuildParentRuntimeModelPtr = 757641296`
     - `lastRuntimeChildLinkBuildChildRuntimeModelPtr = 0`
     - `lastRuntimeChildLinkBuildSourceMeta = 0`
4. control-plane 新结论
   - 当前 anonymous attachment sample 的 parent-link **确实可见**，这一点不变；
   - 但把 direct runtime bootstrap 放在 `RecordObservedRuntimeChildLink(...)` 上，仍然不能把 sample child 拉进 runtime/model/resource/pose contract；
   - 把资源侧 child-group pairing 接入当前 hot `BuildChildRuntimeModelLinks` 样本后，也仍然完全打不中当前 sample child；
   - 再结合：
     - sample root runtime 与 `lastRuntimeChildLinkBuildParentRuntimeModelPtr` 不同
     - sample child 仍无 `CreateModelDataPtr`
   - 可以更硬地收敛成：
     - 当前 hot-shadow 里看到的 anonymous attachment sample parent-link family
     - 不是当前 hot `0x6F131F60 BuildChildRuntimeModelLinks` build family 直接产出的那批 runtime
5. 当前下一步
   - 停止继续在 `0x6F131F60` 本层 blind-fix
   - 直接把逆向主线抬到：
     - `0x6F12EC90`
     - `0x6F12E900`
     - `0x6F182300 / 0x6F1820C0`
   - 的 caller/producer family
   - 目标改为：定位“当前 hot-shadow 观察窗里，第一次同时持有 anonymous runtime tree + owner/source context 的函数”

#### 33.23 2026-04-23 control-plane 已验证：anonymous attachment family 的 owner runtime 会进入 sprite-frame full update，但进入时 `a3/context` 仍为空，因此 reverse gate 继续锁定 `0x6F182300 / 0x6F1820C0` caller 链

1. 本轮代码
   - `war3_model_registry.h/.cpp`
     - `AttachmentRigidRegistry` 新增：
       - `findByOwnerRuntimeModel(...)`
       - `findByRootRuntimeModel(...)`
       - `findByAnyRuntimeModel(...)`
   - `war3_model_hook.h/.cpp`
     - 新增 sprite-frame attachment hit probe；
     - 第二轮再补 full/lite/context 区分：
       - `spriteFrameAttachmentContextHintCount`
       - `spriteFrameAttachmentFullUpdateHitCount`
       - `spriteFrameAttachmentLiteUpdateHitCount`
       - `lastSpriteFrameAttachmentContextPtr`
       - `lastSpriteFrameAttachmentUpdateKind`
   - `war3_shadow_runtime_bridge.h/.cpp`
     - 同步 bridge summary
   - `war3_control_plane.cpp`
     - 输出上述新字段
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_sprite_frame_attachment_20260423_171521.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_sprite_frame_kind_20260423_174025.json`
3. 关键输出
   - 两轮都保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - 第一轮：
     - `spriteFrameAttachmentOwnerRuntimeHitCount = 2`
     - `spriteFrameAttachmentRootRuntimeHitCount = 0`
     - `spriteFrameAttachmentChildRuntimeHitCount = 0`
     - `lastSpriteFrameAttachmentRoleMask = 2`
   - 第二轮：
     - `spriteFrameAttachmentOwnerRuntimeHitCount = 2`
     - `spriteFrameAttachmentContextHintCount = 0`
     - `spriteFrameAttachmentFullUpdateHitCount = 2`
     - `spriteFrameAttachmentLiteUpdateHitCount = 0`
     - `lastSpriteFrameAttachmentUpdateKind = 1`
     - `lastSpriteFrameAttachmentContextPtr = 0`
   - 同轮旧指标仍保持：
     - `attachmentRigidCountWithAnyIdentity = 0`
     - `contractAttachmentRigidCountWithAnyIdentity = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
   - 收尾 `current_state()`：
     - `war3Alive = false`
4. control-plane 新结论
   - 当前 anonymous attachment family 并不是完全绕开 sprite-frame 层；
   - 进入 sprite-frame 的是 owner runtime，不是 root/child runtime；
   - 进入路径不是 lite update，而是 full update；
   - 但这一批 owner-runtime hit 进入 full update 时，当前 `a3/context` 已经为空；
   - 因此不能再把 blocker 归因成：
     - “owner runtime 不进 `0x6F182300 / 0x6F1820C0`”
     - 或“命中发生在 lite 路线所以 `a3` 天生不存在”
   - 现在更准确的结论是：
     - owner runtime 已经进入 `0x6F182300 / 0x6F1820C0`
     - 但需要沿 caller 链往上抬，去找它在更上游哪一层仍然持有非空 context / owner 发布信息
5. 当前下一步
   - reverse gate 主线继续固定在：
     - `0x6F182300 / 0x6F1820C0` caller chain
   - 目标：
     - 找“这批 owner runtime 在 full update 之前哪一层最后一次仍然带着非空 `a3/context`”
     - 再看该层是否同时带着 `logical/source owner`
   - `0x6F12EC90 / 0x6F12E900` 继续保留为交叉验证线，但不再是当前第一主线

#### 33.24 2026-04-23 control-plane 已继续收紧：owner-runtime hit 的稳定 caller 是 `0x184ED3`，落在 `0x184E50 AttachModelToPoint` family 内，因此 reverse gate 具体收敛到 AttachModelToPoint family

1. 本轮代码
   - `war3_model_hook.h/.cpp`
     - 新增：
       - `spriteFrameAttachmentCallerKnownCount`
       - `spriteFrameAttachmentCallerChangedCount`
       - `lastSpriteFrameAttachmentCallerRva`
     - full/lite/context probe 保持不变并继续输出
   - `war3_shadow_runtime_bridge.h/.cpp`
     - 同步 caller 字段
   - `war3_control_plane.cpp`
     - 输出 caller 字段
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_sprite_frame_kind_20260423_174025.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_sprite_frame_caller_20260423_180513.json`
3. 关键输出
   - 仍保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - kind probe：
     - `spriteFrameAttachmentOwnerRuntimeHitCount = 2`
     - `spriteFrameAttachmentContextHintCount = 0`
     - `spriteFrameAttachmentFullUpdateHitCount = 2`
     - `spriteFrameAttachmentLiteUpdateHitCount = 0`
     - `lastSpriteFrameAttachmentUpdateKind = 1`
     - `lastSpriteFrameAttachmentContextPtr = 0`
   - caller probe：
     - `spriteFrameAttachmentCallerKnownCount = 2`
     - `spriteFrameAttachmentCallerChangedCount = 0`
     - `lastSpriteFrameAttachmentCallerRva = 0x184ED3`
     - `lastSpriteFrameAttachmentRoleMask = 2`
   - 收尾：
     - `war3Alive = false`
4. control-plane 新结论
   - 当前 anonymous attachment family 的 owner-runtime hit：
     - 会进入 full sprite-frame update
     - 不会进入 lite update
     - 进入时 current context 为空
   - 更关键的是：
     - 这批命中的 full-update caller 已稳定收敛到 `0x184ED3`
     - 且没有 caller 抖动（`CallerChangedCount = 0`）
   - 因为 `0x184ED3` 落在 `0x184E50 AttachModelToPoint` family 内：
     - 当前 reverse gate 可以正式从“泛化的 sprite-frame caller 链”
     - 收紧成“直接沿 AttachModelToPoint family 往上抬”
5. 当前下一步
   - 下一轮不再泛化搜索 `0x6F182300 / 0x6F1820C0` 所有 caller
   - 直接以：
     - `0x184E50 AttachModelToPoint`
     - `0x184ED3` 命中点
   - 为锚点，查更上游 producer / caller
   - 目标是找：
     - 这条 AttachModelToPoint family 在 `0x184ED3` 之前哪一层最后一次还带着非空 context
     - 该层是否同时持有 logical/source owner

#### 33.25 2026-04-23 control-plane 已确认：anonymous attachment 的 owner identity/source 已通过 `CAttachedEffect_Init -> AttachModelToPoint parent runtime` 回灌到 raw attachment registry 与 contract，当前 blocker 已从“身份不完整”切换为“child runtime resource/pose 不完整”

1. 本轮代码
   - `war3_model_hook.h/.cpp`
     - 为 `Hook_AttachedEffectInit` 新增 TLS scope
     - 新增 `MaybePublishAttachedEffectInitParentRuntimeOwnerIdentity(...)`
     - 命中 attachment owner runtime 且 attach-scope parent runtime match 时：
       - 把 `ownerWidgetPtr` 直接发布到 `parentRuntimeModelPtr`
       - 同步写入 runtime source-object / runtime owner identity / instance identity / runtime tree owner identity
     - 新增 summary 字段：
       - `attachedEffectInitParentRuntimeOwnerPublishCount`
       - `lastAttachedEffectInitParentRuntimeModelPtr`
   - `war3_model_registry.h/.cpp`
     - `AttachmentRigidRegistry` 新增 `noteRuntimeIdentity(...)`
     - 在 owner runtime 身份晚于 raw attachment 发布的情况下，对已有 attachment record 本体做按 runtime 回灌
   - `war3_shadow_runtime_bridge.h/.cpp`
     - 同步 bridge summary
   - `war3_control_plane.cpp`
     - 输出上述新增字段
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_attach_scope_20260423_182640.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_attach_owner_publish_registry_20260423_1846.json`
   - `AutoTest/artifacts/codex_model_runtime_probe_attach_owner_publish_registry_state_20260423_1847.json`
3. 关键输出
   - 继续保持：
     - `useIsolatedDesktop = true`
     - `windowed = true`
   - attach-scope 证据：
     - `spriteFrameAttachmentAttachScopeHitCount = 2`
     - `spriteFrameAttachmentAttachScopeOwnerHitCount = 2`
     - `spriteFrameAttachmentAttachScopeParentRuntimeMatchCount = 2`
     - `lastAttachScopeCallerRva = 0x6BB384`
   - owner publish 结果：
     - `attachedEffectInitParentRuntimeOwnerPublishCount = 2`
     - `attachmentRigidOwnerRuntimeOwnerIdentityCount = 2`
     - `attachmentRigidCountWithAnyIdentity = 2`
     - `contractAttachmentRigidCountWithAnyIdentity = 2`
     - `attachmentRigidCountWithSourceObject = 2`
     - `contractAttachmentRigidCountWithSourceObject = 2`
     - `attachmentRigidSample0SourceObjectPtr = attachmentRigidSample0OwnerRuntimeSourceObjectPtr`
     - `lastAttachedEffectInitParentRuntimeModelPtr = lastAttachScopeParentRuntimeModelPtr`
   - 当前仍未抬起：
     - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
     - `attachmentRigidChildRuntimePoseKnownCount = 0`
     - `contractAttachmentRigidChildRuntimePoseKnownCount = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 2`
   - 收尾：
     - `war3Alive = false`
4. control-plane 新结论
   - 当前 anonymous attachment family 的上层 owner/source 数据已经不再缺失：
     - `CAttachedEffect_Init` 就是当前 owner runtime 的 authoritative owner producer
     - 关键缺口是 parent runtime 命中时没有及时发布 ownerWidget identity
   - 该缺口现已闭环：
     - raw attachment registry 与 contract 都已拿到 identity/source
     - `Phase 1 / Phase 2` 里关于 owner identity publish 的 reverse gate 已通过
   - 当前 primary blocker 已切换：
     - 不再是 identity/source
     - 而是 child runtime 自身的 resource/pose 仍没进可消费 contract
5. 当前下一步
   - 不再继续上抬 `AttachModelToPoint / CAttachedEffect_Init` 的 owner producer
  - 下一轮直接围绕：
    - child runtime 的 `modelResource`
    - child runtime 的 `pose / worldTransform`
  - 去补 attachment rigid 的真正可绘制 contract
  - 目标指标：
    - `semanticCoreAttachmentRigidResolved > 0`

#### 34.02 2026-04-24 control-plane 已确认：semantic core 已能用 resource-owner world pose 产出 2 个 semantic-only rigid draw packet；当前仍缺 skinned matrix palette / attachment rigid 正式消费

1. 本轮代码
   - `war3_shadow_renderer_core.*`
     - attachment geoset match 增加 `childSprite->Model` alias，并新增 `attachmentRigidMatchByChildSpriteRuntimeGeoset` 统计。
     - anonymous renderable recovery 增加 `findRuntimeModelOwner(...)` 回填，使 resource owner 成为 semantic runtime context。
     - pose lookup 增加 `CModel base <-> CModelComplex +0xA0` alias。
     - skinned no-palette 情况下，如果 resource-owner alias 有 sprite-frame world transform，则先产出 semantic-only rigid packet。
   - `war3_shadow_runtime_bridge.*` / `war3_control_plane.cpp`
     - 新增 `semanticCoreAttachmentRigidMatchByChildSpriteRuntimeGeoset` 输出。
2. fresh 工件
   - `codex_model_runtime_probe_world_pose_rigid_fallback_20260424_0302.json`
   - `codex_dynamic_shadow_pressure_world_pose_rigid_fallback_20260424_0307.json`
   - `codex_low_pressure_static_reuse_world_pose_rigid_fallback_20260424_0312.json`
3. 关键输出
   - 所有运行均为 `useIsolatedDesktop=true` / `windowed=true`，收尾无残留 War3 进程。
   - 三套场景一致：
     - `semanticCoreResolved = 2`
     - `semanticCoreRigidResolved = 2`
     - `semanticCoreDrawPacketCount = 2`
     - `semanticCoreSubmittedDrawCount = 2`
     - `semanticCoreSkippedNoPose = 0`
     - `semanticCoreSkippedNoPoseAnonymousSubpart = 0`
     - `semanticCoreSkippedNoPoseLookupMiss = 0`
     - `objectFallbackDrawCount = 0`
   - 未达最终 gate：
     - `semanticCoreAttachmentRigidResolved = 0`
     - `semanticCoreSkinnedResolved = 0`
     - `upperLayerEmitted = 0`
     - `upperLayerResolveAuthoritativeSkinned = 0`
4. control-plane 新结论
   - 现在不是“完全拿不到可绘制数据”：resource/geoset + world pose 已足够让 core 生成 draw packet。
   - 但这只是 rigid fallback，不是最终 canonical skinned / attachment rigid 结果。
   - 关键新证据是 resource runtime owner 与 sprite-frame source runtime 存在稳定 `0xA0` 分层；下一步要把这层关系转成正式 pose/source contract，而不是继续盲猜 attachment child runtime。
5. 当前下一步
   - 补 `resourceRuntimeOwner ±0xA0` 诊断字段。
   - 在 producer 侧把 sprite-frame source runtime 的 world pose/source identity 规范发布到 CModel base runtime owner。
   - 继续追 matrix palette 发布点，让当前 2 个 rigid fallback packet 升级为 skinned 或 attachment rigid packet。

#### 34.03 2026-04-24 control-plane 证伪：sprite-frame source/context 不是可发布逻辑 owner，弱身份已改为 observe-only

1. 本轮 control-plane 扩展
   - 新增 sprite-frame source-object 诊断字段：
     - `spriteFrameSourceDeepIdentityResolvedCount`
     - `spriteFrameSourceObjectRuntimeFieldCandidateCount`
     - `spriteFrameSourceObjectRegistryFieldHitCount`
     - `lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr`
     - `lastSpriteFrameSourceObjectRuntimeFieldOffset`
     - `lastSpriteFrameSourceObjectRegistryFieldCandidatePtr`
     - `lastSpriteFrameSourceObjectRegistryFieldOffset`
     - `lastSpriteFrameSourceWorldObjectEntryPtr`
     - `lastSpriteFrameSourceSceneNodePtr`
   - sprite-frame source identity publish 改为必须有 `unitPtr / jHandle / rawcode`，否则仅观测。
2. fresh 工件
   - `codex_model_runtime_probe_source_object_deep_probe_20260424_0330.json`
   - `codex_model_runtime_probe_deep_identity_merge_20260424_0338.json`
   - `codex_model_runtime_probe_source_field_merge_20260424_0348.json`
   - `codex_model_runtime_probe_source_logical_gate_20260424_0400.json`
3. 关键输出
   - 最终 fresh run：
     - `semanticCoreRigidResolved = 2`
     - `semanticCoreSubmittedDrawCount = 2`
     - `objectFallbackDrawCount = 0`
     - `semanticCoreAttachmentRigidResolved = 0`
     - `spriteFrameSourceResolvedIdentityCount = 0`
     - `spriteFrameSourceObjectRuntimeFieldCandidateCount = 102`
     - `spriteFrameSourceObjectRegistryFieldHitCount = 96`
     - `lastSpriteFrameSourceObjectRuntimeFieldOffset = 0x100`
     - `lastSpriteFrameSourceObjectRegistryFieldOffset = 0x100`
     - `lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0HasWorldTransform = 1`
     - `lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0PoseMatrixCount = 0`
4. control-plane 新结论
   - `resourceRuntimeOwner + 0xA0` 是有效 world-pose/source-context 观察面，但不是逻辑 owner 发布面。
   - `sourceObject+0x100` 的 registry/runtime 候选是内部 context/矩阵块误命中，不能进入 owner contract。
   - 之前 `spriteFrameSourceResolvedIdentityCount > 0` 的含义不可靠；现在已收紧为真实逻辑身份才计数。
5. 当前下一步
   - 不再沿 sprite-frame `a3/sourceObject` 继续找 owner。
   - 直接追调用 `CSpriteUber_PreRenderAndUpdatePosePalette_*` 的更高层 producer。
   - 并行追 `0x6F12F7E0` 后的 matrix palette 发布点，把 current rigid fallback 升级为 canonical skinned packet。

#### 35.01 2026-04-24 control-plane 更新：semantic rigid-only 阶段可观测，attachment child lineage failure 已收敛到 noUniqueChild

1. 新增 control-plane 字段
   - `semanticCoreExplicitResourceOwnerRigidResolved`
   - `semanticCoreExplicitResourceOwnerRigidWorldTransformResolved`
   - `semanticCoreExplicitResourceOwnerRigidNoMatrixPalette`
   - `attachmentChildLineageBootstrapAttemptCount`
   - `attachmentChildLineageBootstrapSuccessCount`
   - `attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount`
   - `attachmentChildLineageBootstrapMissNoModelDataLinksCount`
   - `attachmentChildLineageBootstrapMissNoUniqueChildCount`
   - `lastAttachmentChildLineageBootstrapParentRuntimeModelPtr`
   - `lastAttachmentChildLineageBootstrapChildRuntimeModelPtr`
   - `lastAttachmentChildLineageBootstrapParentModelDataPtr`
   - `lastAttachmentChildLineageBootstrapChildModelDataPtr`
   - `lastAttachmentChildLineageBootstrapChildModelResourcePtr`
   - `lastAttachmentChildLineageBootstrapSourceMeta`
   - `lastAttachmentChildLineageBootstrapBucketIndex`
2. fresh 工件
   - `codex_model_runtime_probe_explicit_resource_owner_rigid_20260424_0455.json`
   - `codex_model_runtime_probe_child_lineage_bootstrap_diag_20260424_0520.json`
   - `codex_model_runtime_probe_child_lineage_observed_links_20260424_0550.json`
3. 关键输出
   - `semanticCoreExplicitResourceOwnerRigidResolved = 2`
   - `semanticCoreExplicitResourceOwnerRigidWorldTransformResolved = 2`
   - `semanticCoreExplicitResourceOwnerRigidNoMatrixPalette = 2`
   - `semanticCoreSubmittedDrawCount = 2`
   - `objectFallbackDrawCount = 0`
   - `attachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 0`
   - `attachmentChildLineageBootstrapAttemptCount = 4`
   - `attachmentChildLineageBootstrapSuccessCount = 0`
   - `attachmentChildLineageBootstrapMissNoModelDataLinksCount = 0`
   - `attachmentChildLineageBootstrapMissNoUniqueChildCount = 4`
4. control-plane 新结论
   - 当前 2 个 draw packet 不是旧 fallback，而是 semantic core 的 explicit resource-owner rigid path。
   - attachment child lineage 已经证明 parent modelData 可达；失败点集中在 child modelData 选择不唯一，而不是 owner identity 或 no modelData。
   - `model_runtime_probe` 允许 `semanticRigidOnlyAccepted=true`，用于阶段性观测；最终 dynamic_shadow_pressure 仍要求 canonical skinned/attachment 或 visible shadow gate。
5. 下一轮 control-plane 目标
   - 给 `0x6F131F60` build-time child runtime creation 直接发布 `childRuntimeModelPtr -> childModelDataPtr/modelResourcePtr`。
   - 若该 producer 命中，预期 `attachmentRigidChildRuntimeModelResourceKnownCount` 和 `contractAttachmentRigidChildRuntimeModelResourceKnownCount` 必须从 0 抬升。
   - 若仍不命中，下一轮只 hook `0x6F12FDC0 / 0x6F12FF50`，验证 final matrix array copy/flush 是否是 child pose publisher。

#### 35.02 2026-04-24 control-plane 更新：attachment rigid supplemental path 已可观测并通过 Phase 3 runtime gate

1. 新增 control-plane 字段
   - `semanticCoreAttachmentRigidSupplementalAttachmentCount`
   - `semanticCoreAttachmentRigidSupplementalResourceCandidateCount`
   - `semanticCoreAttachmentRigidSupplementalResolvedCount`
   - `semanticCoreAttachmentRigidSupplementalResourceMissCount`
   - `attachModelToPointPromotedAttachmentChildRuntimeCount`
   - `attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount`
   - `lastAttachModelToPointPromotedOwnerRuntimeModelPtr`
   - `lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr`
   - `lastAttachModelToPointPromotedChildRuntimeModelPtr`
   - `lastAttachModelToPointPromotedChildModelResourcePtr`
   - `lastAttachModelToPointAttachPointIndex`
2. fresh 工件
   - `codex_model_runtime_probe_attach_promoted_child_runtime_20260424_0710.json`
   - `codex_model_runtime_probe_attachment_rebuild_freshness_20260424_0850.json`
   - `codex_model_runtime_probe_attachment_stats_fixed_20260424_0905.json`
   - `codex_dynamic_shadow_pressure_attachment_rigid_smoke_20260424_0915.json`
   - `codex_low_pressure_static_reuse_attachment_rigid_smoke_20260424_0925.json`
   - `codex_dynamic_shadow_pressure_strict_gate_contrast_20260424_0940.json`
3. 关键输出
   - `attachModelToPointPromotedAttachmentChildRuntimeCount = 2`
   - `attachmentRigidChildRuntimeModelResourceKnownCount = 2`
   - `contractAttachmentRigidChildRuntimeModelResourceKnownCount = 2`
   - `semanticCoreAttachmentRigidResolved = 96`
   - `semanticCoreAttachmentRigidSupplementalAttachmentCount = 1`
   - `semanticCoreAttachmentRigidSupplementalResourceCandidateCount = 96`
   - `semanticCoreAttachmentRigidSupplementalResolvedCount = 96`
   - `semanticCoreResolved = 98`
   - `semanticCoreRigidResolved = 98`
   - `semanticCoreSubmittedDrawCount = 98`
   - `objectFallbackDrawCount = 0`
4. control-plane 新结论
   - attachment owner identity、child resource、root/world pose 与 core 消费链已经闭环；这条路径现在不是 probe-only，而是真正进入 `ShadowRendererCore` submission。
   - `semanticCoreResolved` 之前低报的问题已修正；attachment rigid 成功会同步计入 resolved/rigidResolved。
   - `dynamic_shadow_pressure` smoke 下 attachment rigid 仍稳定，且 `semanticCoreSkippedNoIdentity=0`、`semanticCoreSkippedNoPose=0`。
   - strict `dynamic_shadow_pressure` gate 仍失败在 skinned/native 阶段；returned summary 未等到 attachment supplemental rebuild，因此该 artifact 只作为 gate 对照，不作为 attachment rigid 回归依据。
5. 下一轮 control-plane 目标
   - 正式 gate 需要新增阶段性区分：attachment rigid semantic 正常但 skinned 未完成，不应等同于“无阴影”。
   - 继续补 `semanticCoreSkinnedResolved` / `upperLayerEmitted` 的 canonical skinned path。
   - 对 `semanticCoreAttachmentRigidSupplementalResourceCandidateCount=96` 做性能观察；若 CPU 抬升，优先做 geoset 可见性收窄或 geometry cache/dedup，而不是回退旧 snapshot/freeze。

#### 35.03 2026-04-24 control-plane/AutoTest 更新：hot-shadow gate 支持 attachment rigid 阶段分类

1. 新增/调整 AutoTest gate 字段
   - `semanticAttachmentRigidOnlyAccepted`
   - `semanticAttachmentRigidGateSatisfied`
   - `semanticShadowPhase`
   - `semanticShadowPhaseReason`
   - `mode = control-plane-hot-frame-summary-poll`
2. fresh 工件
   - `codex_dynamic_shadow_pressure_ready_poll_attachment_20260424_0718.json`
   - `codex_dynamic_shadow_pressure_summary_only_gate_20260424_0805.json`
   - `codex_model_runtime_probe_attachment_gate_after_autotest_fix_20260424_0818.json`
3. 关键输出
   - `dynamic_shadow_pressure` strict gate 不再返回笼统 `hot-shadow`：
     - `stage = hot-shadow-skinned-pending`
     - `semanticShadowPhase = attachment-rigid-ok-skinned-pending`
     - `semanticCoreAttachmentRigidResolved = 96`
     - `semanticCoreSubmittedDrawCount = 98`
     - `semanticCoreSkinnedResolved = 0`
     - `upperLayerEmitted = 0`
     - `objectFallbackDrawCount = 0`
   - `model_runtime_probe` hot gate 不再用 2 个 explicit rigid packet 提前通过，当前会等到：
     - `semanticAttachmentRigidOnlyAccepted = true`
     - `semanticCoreAttachmentRigidResolved = 96`
     - `semanticCoreAttachmentRigidSupplementalResolvedCount = 96`
4. control-plane 新结论
   - 隔离桌面 + 窗口模式下，`wait_until` 的高频 hot-frame 条件可能停在 pre-attachment summary；低频 `get_shadow_runtime_summary(refreshSemanticFrameIfStale=true)` 更适合当前 Phase 4 的 semantic contract 验收。
   - 当前应把 `semanticCoreAttachmentRigidResolved=96 && objectFallbackDrawCount=0` 视为 attachment rigid runtime gate 已通过，而不是被 `semanticCoreSkinnedResolved=0` 覆盖为“无阴影”。
5. 下一轮 control-plane 目标
   - 继续围绕 `semanticCoreSkinnedResolved`、`upperLayerEmitted`、`nativeD3D9BackendExecutedDrawCount` 增强 summary，区分 skinned contract 缺 pose/palette、缺 resource、还是被 emission gate 拦住。
   - 若后续 strict visual gate 仍失败，报告必须明确是 skinned/native 未签收，而不是 attachment rigid 数据链失败。

#### 35.04 2026-04-24 control-plane 更新：新增 skinned candidate 分层计数

1. 新增 control-plane 字段
   - `semanticCoreRigidCandidateCount`
   - `semanticCoreSkinnedCandidateCount`
   - `semanticCoreSkinnedCandidatePoseReadyCount`
   - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount`
   - `semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount`
2. fresh 工件
   - `codex_model_runtime_probe_skinned_candidate_counters_20260424_0725.json`
3. 关键输出
   - `semanticCoreSkinnedCandidateCount = 98`
   - `semanticCoreSkinnedCandidatePoseReadyCount = 0`
   - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount = 0`
   - `semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount = 96`
   - `semanticCoreSkinnedResolved = 0`
   - `semanticCoreAttachmentRigidResolved = 96`
   - `matrixPaletteCount = 44`
   - `shadowRuntimeModelCount = 226`
4. control-plane 新结论
   - skinned=0 不是因为资源都不是 skinned；当前资源候选全是 skinned resource。
   - skinned=0 也不是因为全局 pose/palette 彻底消失；pose/palette 总量仍在。
   - 当前 96 个 skinned resource candidate 被 attachment rigid contract 接走，说明它们是 attachment rigid 验证对象，不是 canonical root/unit skinned path。
5. 下一轮 control-plane 目标
   - 增加 root/unit manifest 发布诊断：当前 frame manifest 的 `unitCount`、`recordsWithRuntimeModel`、`recordsWithModelResource` 为什么仍为 0。
   - 若 unit-like draw 被 `kShadowSemanticCoreSceneBypassLegacyUnitCaptureEnabled` 旁路，必须确认是否有等价 semantic manifest record；没有则补发布点，而不是恢复 VB/IB capture。

#### 35.05 2026-04-24 control-plane/contract 更新：root/unit supplement 与 skinned path 首次抬起

1. 新增 control-plane 字段
   - `rootUnitSupplementSeedCount`
   - `rootUnitSupplementUnitSeedCount`
   - `rootUnitSupplementSkippedNoIdentity`
   - `rootUnitSupplementSkippedAttachmentChild`
   - `rootUnitSupplementSkippedNoPose`
   - `rootUnitSupplementSkippedNoResource`
   - `rootUnitSupplementSkippedNoGeoset`
   - `rootUnitSupplementSkippedDuplicate`
   - `rootUnitSupplementAppended`
2. fresh 工件
   - `codex_model_runtime_probe_root_unit_supplement_20260424_0919.json`
3. 关键输出
   - `get_frame_manifest_summary` 已能看到 supplemented root/unit records：
     - `unitCount = 96`
     - `recordsWithRuntimeModel = 96`
     - `recordsWithModelResource = 96`
     - `recordsWithResolvedGeoset = 98`
     - `rootUnitSupplementAppended = 96`
   - `get_shadow_runtime_summary` 已能看到 skinned path 首次通过：
     - `semanticCoreSourceUnitCount = 61`
     - `semanticCoreSkinnedResolved = 31`
     - `semanticCoreSkinnedCandidatePoseReadyCount = 59`
     - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount = 31`
     - `semanticCoreAttachmentRigidResolved = 213`
     - `semanticCoreSubmittedDrawCount = 39`
     - `semanticCoreSkippedNoRuntimeGroupPalette = 0`
     - `objectFallbackDrawCount = 0`
4. control-plane 新结论
   - root/unit data chain 已经不是 0：contract bundle 能合成 `runtimeModel + modelResource + pose/palette` 的单位记录。
   - skinned=0 的 blocker 已突破为 skinned>0；当前新 blocker 是 build freshness/perf，不是继续找 owner。
   - `semanticCoreBuildInProgress` 在隔离桌面尾帧可能长时间为 true；observe-step 已放宽到 in-progress build，但 attachment supplemental workload 仍需要去重/限流。
5. 下一轮 control-plane 目标
   - 增加/调整 semantic build 去重 counters，确认 603 条 build workload 的来源。
   - 将 attachment supplemental 与 root/unit supplemental 去重或分阶段处理，避免 `get_shadow_runtime_summary(refreshSemanticFrameIfStale=true)` 每次只推进很少记录。
   - freshness gate 目标：`semanticCoreFrameFresh=true`、`semanticCoreSourceUnitCount` 接近 manifest `unitCount`、`semanticCoreSkinnedResolved > 0`、`objectFallbackDrawCount=0`。

#### 35.06 2026-04-24 control-plane/semantic-core 更新：attachment supplemental 阶段感知限流

1. 调整内容
   - `ShadowRendererCore::buildFrameChunk` 中 attachment supplemental 增加 root/unit-aware cap。
   - 若 manifest 已有 root/unit semantic records，attachment supplemental resolved cap 降为 16。
   - 若 manifest 还没有 root/unit semantic records，cap 保持 128，避免 attachment-only gate 回退。
   - per-child geoset probe cap 从 512 降为 128。
2. fresh 工件
   - `codex_model_runtime_probe_semantic_build_limited_20260424_0930.json`
3. 关键输出
   - 限流前：`buildRecordCount=603`、`semanticCoreSkinnedResolved=31`、`semanticCoreAttachmentRigidResolved=213`。
   - 限流后：
     - `buildRecordCount=26`，后续 root/unit build 为 `98`
     - `semanticCoreAttachmentRigidSupplementalResolvedCount=16`
     - `semanticCoreSkinnedResolved=4`
     - `semanticCoreSkinnedCandidatePoseReadyCount=4`
     - `semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount=4`
     - `semanticCoreSubmittedDrawCount=42`
     - `objectFallbackDrawCount=0`
4. control-plane 新结论
   - 603 条 build workload 确实来自 root/unit supplement 与 attachment supplemental 同时扩展后的尾帧负载；阶段感知 cap 能有效把 workload 拉回可控范围。
   - 当前 root/unit 数据链、pose/palette、skinned resolve 均已证明可用；下一步不需要继续扩大逆向范围。
   - 当前 `semanticCoreFrameFresh=false` 主要是 chunk 调度没追完，不是缺 resource/pose/palette。
5. 下一轮 control-plane 目标
   - 对小于等于 128 records 的 semantic build，允许一次 refresh 推进多个 observe chunks，或直接优先 root/unit records。
   - 目标是让 `semanticCoreFrameFresh=true`，再继续观察 `upperLayerEmitted` 为什么仍为 0。

#### 35.07 2026-04-24 control-plane/semantic-core 更新：root/unit 小闭环 fresh 已通过

1. 调整内容
   - `get_shadow_runtime_summary(refreshSemanticFrameIfStale=true)` 的 observe completion 支持小型 build 多 chunk 推进，当前 ceiling=128、extra chunks=8、budget=6000ms。
   - root/unit semantic records 在 supplemented manifest 内前置，避免旧 unknown records 阻塞单位补记录。
   - root/unit supplement 暂时限流为 16 records / 2 geosets per runtime，用于先签收 freshness/correctness。
   - semantic scene catchup 现在会处理“已有旧 frame 但 revision 落后”的情况。
2. fresh 工件
   - `codex_model_runtime_probe_root_cap16_samples_20260424_0956.json`
   - `codex_model_runtime_probe_scene_catchup_samples_20260424_1002.json`
   - `codex_dynamic_shadow_pressure_scene_catchup_smoke_20260424_1005.json`
3. 关键输出
   - `model_runtime_probe` 已出现：
     - `semanticCoreFrameFresh=true`
     - `semanticCoreSourceUnitCount=16`
     - `semanticCoreSkinnedResolved=2`
     - `semanticCoreAttachmentRigidResolved=20`
     - `semanticCoreAttachmentRigidSupplementalResolvedCount=16`
     - `semanticCoreSubmittedDrawCount=24`
     - `objectFallbackDrawCount=0`
   - 后续 smoke 中 core/native staging 已到：
     - `semanticCoreSubmittedDrawCount=31/32`
     - `nativeD3D9BackendSubmittedDrawCount=31/32`
     - `nativeD3D9BackendHasDevice=true`
     - `nativeD3D9BackendExecutedDrawCount=0`
     - `semanticSceneSubmitted=4`
4. control-plane 新结论
   - freshness gate 已突破；control-plane 能把 root/unit semantic contract build 完整推完。
   - pipe refresh 只能推进 core/native staging，不能替代真实 render scene pass。
   - 当前 `semanticSceneSubmitted=4` 是旧 render tick 的 scene submission 结果，不代表 fresh core frame 没有 draw packet。
5. 下一轮 control-plane 目标
   - 区分 `semanticCoreSubmittedDrawCount` 与 `semanticSceneSubmitted` 的阶段含义，在 hot-shadow gate 中避免把“尾帧无 render tick”误判成 semantic core 失败。
   - 新增或复用 frame-advance 检测，只有真实 render frame advance 后再判断 DXVK scene submission 是否消费 fresh frame。
   - 若有真实 frame advance 但 scene submission 仍不抬升，再查 unitsOnly/objectKind filter 与 `War3ShouldSubmitSemanticPacket`。

#### 35.08 2026-04-24 control-plane/scene-consumption 更新：新增 scene consumed gate

1. 调整内容
   - `get_shadow_runtime_summary` 新增 scene-consumption 诊断字段：
     - `semanticSceneStatsPublishCount`
     - `semanticScenePopulateAttemptCount`
     - `semanticSceneLastFrameSerial`
     - `semanticSceneLastSourcePublishRevision`
     - `semanticSceneLastTargetPublishRevision`
     - `semanticSceneLastInputDrawCount`
     - `semanticSceneLastSubmittedDrawCount`
     - `semanticSceneLastUnitsOnlyFilteredCount`
     - `semanticSceneCatchupAttemptCount`
     - `semanticSceneCatchupSuccessCount`
     - `semanticSceneSkippedEmptyFrameCount`
     - `semanticSceneZeroSubmitCount`
     - `semanticScenePublishRevisionLag`
   - `wait_until` 新增 `requireSemanticSceneConsumed`，用于要求 DXVK scene pass 已消费最新 semantic publish revision。
   - AutoTest hot-shadow 分类新增 `core-fresh-waiting-render-scene`，用于区分“semantic core 已产包”和“真实 render scene pass 未推进”。
2. fresh 工件
   - `codex_scene_consumption_poll_20260424_1020.json`
   - `codex_scene_consumed_gate_20260424_1029.json`
3. control-plane 证据
   - `semanticCoreSubmittedDrawCount=2`
   - `nativeD3D9BackendSubmittedDrawCount=2`
   - `semanticSceneLastSubmittedDrawCount=4`
   - `semanticSceneLastSourcePublishRevision=37`
   - `semanticCoreSourcePublishRevision=38`
   - `semanticScenePublishRevisionLag=1`
   - `semanticSceneStatsPublishCount=1`
   - `semanticScenePopulateAttemptCount=1`
   - `frameIndex` 在采样期间不再推进，`render.isInGame=false`
4. control-plane 新结论
   - 当前 blocker 已从“semantic core 是否能 build”转移到“测试尾帧是否有真实 render scene pass 消费 fresh frame”。
   - `semanticSceneSubmitted=4` 是旧 scene pass 的结果，不再能单独代表当前 fresh core frame 是否可绘制。
   - `requireSemanticSceneConsumed=true` 可以正确挡住 stale scene，并返回可诊断 summary。
5. 下一轮 control-plane 目标
   - 调整 AutoTest ready/hot-shadow 语义：`inGameRenderReady=true` 但 `isInGame=false` 且 frame stalled 时，不应继续用视觉 gate 误判 core。
   - 对 visual gate 使用 `requireSemanticSceneConsumed=true`；对 `model_runtime_probe` 保留 core-contract 诊断通过路径。

#### 35.09 2026-04-24 control-plane/AutoTest 更新：scene pending 阶段可直接签出

1. 调整内容
   - AutoTest summary-poll 改为调用 `get_hot_shadow_probe`，避免 runtime/manifest/summary 分三次读导致尾帧诊断不同步。
   - 新增 frame-tail 诊断：
     - `runtimeFrameAdvanceStalled`
     - `runtimeRenderTailStalled`
     - `runtimeRenderIsInGame`
     - `runtimeFrameStallSec`
   - `wait_for_hot_shadow_frame` 现在在 semantic strict gate 未满足时，也会优先识别 scene consumption blocker：
     - `core-fresh-waiting-render-scene`
     - `core-packets-waiting-render-scene`
   - `run_quick_autotest` 将这两类 phase 映射成 `stage=hot-shadow-render-scene-pending`。
   - DLL `wait_until stalled` 增加 `frameStalled / semanticBuildStalled / semanticSceneWaitingForRenderPass` 等字段。
2. fresh 工件
   - `codex_hot_gate_direct_probe_20260424_1044.json`
   - `codex_hot_gate_dynamic_visual_probe_20260424_1058.json`
   - `codex_run_quick_dynamic_scene_pending_20260424_1102.json`
3. control-plane 证据
   - direct `model_runtime_probe`：
     - `hotOk=true`
     - `semanticCoreSubmittedDrawCount=5`
     - `semanticSceneConsumptionFresh=true`
   - strict `dynamic_shadow_pressure`：
     - `stage=hot-shadow-render-scene-pending`
     - `semanticShadowPhase=core-packets-waiting-render-scene`
     - `runtimeRenderTailStalled=true`
     - `semanticScenePublishRevisionLag=1`
     - `semanticCoreSubmittedDrawCount=1`
     - `semanticSceneLastSubmittedDrawCount=4`
4. control-plane 新结论
   - 当前失败口径已从“hot shadow 泛失败”收敛为“core 有 packet，但 render scene pass 没消费最新 revision”。
   - `runtimeRenderTailStalled=true` 证明这轮卡点是 isolated desktop/windowed 尾帧不再推进真实 render tick，而不是 control-plane 无法读 summary。
   - 后续视觉验收必须先解决 frame advance / scene consumption；否则继续扩大 semantic resource/pose 只会堆更多 packet，scene 仍不会吃。
5. 下一轮 control-plane 目标
   - 给 visual gate 增加安全的 frame-advance 等待/触发策略，仍保持 isolated desktop + windowed + `enforceVideoBaseline=false`。
   - 若 frame advance 后 scene lag 仍存在，再回到 `War3ShouldSubmitSemanticPacket` 的 `unitsOnly/objectKind` 过滤诊断。

#### 35.10 2026-04-24 control-plane/native validation 更新：native execute 成功计数已接入

1. 调整内容
   - `War3Renderer::EndFrame()` 现在会在 render-thread capture 后显式请求 latest semantic frame build，并对小型 supplemented manifest 做最多 3 个 observe chunk。
   - `NativeD3D9BackendRuntime` 新增稳定 execute 计数，避免后续 control-plane `buildLatestFrame()` 重置 current executed count 后误报“native 从未执行”。
   - `get_shadow_runtime_summary` 新增：
     - `nativeD3D9BackendExecuteAttemptCount`
     - `nativeD3D9BackendExecuteSuccessCount`
     - `nativeD3D9BackendLastSuccessfulExecutedFrameSerial`
     - `nativeD3D9BackendLastSuccessfulExecutedDrawCount`
     - `nativeD3D9BackendExecuteSkippedNoDeviceCount`
     - `nativeD3D9BackendExecuteSkippedNoDrawsCount`
     - `nativeD3D9BackendLastExecuteSubmittedDrawCount`
     - `nativeD3D9BackendLastExecuteFailedDrawCount`
2. fresh 工件
   - `AutoTest/artifacts/codex_model_runtime_probe_native_execute_counters_20260424_1157.summary.json`
3. control-plane 证据
   - `semanticCoreFrameFresh=true`
   - `semanticCoreSubmittedDrawCount=22`
   - `semanticCoreAttachmentRigidResolved=20`
   - `objectFallbackDrawCount=0`
   - `nativeD3D9BackendHasDevice=true`
   - `nativeD3D9BackendSubmittedDrawCount=22`
   - `nativeD3D9BackendExecuteAttemptCount=58`
   - `nativeD3D9BackendExecuteSuccessCount=58`
   - `nativeD3D9BackendExecuteSkippedNoDeviceCount=0`
   - `nativeD3D9BackendExecuteSkippedNoDrawsCount=0`
   - `nativeD3D9BackendLastExecuteFailedDrawCount=0`
   - `semanticScenePopulateAttemptCount=1`
   - `semanticSceneLastSourcePublishRevision=37`
   - `semanticScenePublishRevisionLag=4294967301`
4. control-plane 新结论
   - native D3D9 backend 已确认能执行 semantic draw，不再只是 prepared/submitted。
   - 当前 `nativeD3D9BackendExecutedDrawCount=0` 不能再单独解读为 native 未执行；它会被后续 frame prepare 重置，必须看 stable execute counters。
   - DXVK validation host 的 visual scene 仍停在旧 publish revision，这是独立于 native execute 的验证宿主 tail-frame 问题。
5. 下一轮 control-plane 目标
   - 将 native path gate 从 current `executedDrawCount` 改成 stable `lastSuccessfulExecutedDrawCount / executeSuccessCount`。
   - 将 `hot-shadow-render-scene-pending` 与 native successful execute 分开展示：前者说明 DXVK validation scene 没吃 fresh frame，后者说明 native D3D9 backend 已实际画过 semantic packets。
   - 对 ready 阶段 pipe timeout 补诊断：区分未初始化、启动慢、进程已退出、控制面未安装。

#### 35.11 2026-04-24 AutoTest gate 更新：effective native execute draw count

1. 调整内容
   - AutoTest 新增 stable native execute 口径：
     - `nativeD3D9BackendEffectiveExecutedDrawCount`
   - 计算优先级：
     - `nativeD3D9BackendExecutedDrawCount`
     - `nativeD3D9BackendLastSuccessfulExecutedDrawCount`
     - `nativeD3D9BackendExecuteSuccessCount > 0` 时用 `LastExecuteSubmittedDrawCount - LastExecuteFailedDrawCount` 兜底。
   - 当 native 已执行但 DXVK scene revision 仍 stale 时，hot-shadow phase 标记为：
     - `native-semantic-executed-scene-pending`
2. 验证
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
3. control-plane 新结论
   - AutoTest gate 现在能表达“native path 已成功 draw，但 DXVK validation host 视觉 scene 还没吃 fresh frame”。
   - 这和当前项目目标更一致：DXVK 只是验证宿主，native D3D9 path 的执行成功应单独进入验收视野。
4. 下一轮 control-plane 目标
   - 补 ready pipe timeout 分类，避免偶发 `named pipe 不可用: 2` 被误判成 runtime semantic 失败。
   - 下一次 runtime smoke 优先检查 `nativeD3D9BackendEffectiveExecutedDrawCount` 与 `semanticShadowPhase` 是否按预期出现。

#### 35.12 2026-04-24 frame path 计数与 skinned/rigid 消费断层

1. 调整内容
   - control-plane summary 新增 `semanticCoreLastFrameSourcePublishRevision`、`semanticCoreLastFrameDrawCount`、`semanticCoreLastFrameSkinnedDrawCount`。
   - control-plane summary 新增 `semanticCoreRenderableFrameSourcePublishRevision`、`semanticCoreRenderableFrameDrawCount`、`semanticCoreRenderableFrameSkinnedDrawCount`。
   - `ShadowValidationRuntime` 不再允许 partial build 覆盖 canonical `m_lastFrame`，避免 stats 和 frame draw 内容错位。
   - scene/native frame selection 增加 skinned-aware score，避免较新的 rigid-only partial frame 抢占较完整的动态 frame。
2. 验证
   - `ninja -C build32 -j1` 通过。
   - fresh artifact: `AutoTest/artifacts/codex_dynamic_semantic_rigid_floor_probe_20260424_181613.json`。
3. control-plane 新结论
   - 当前 summary 已能直接区分“core resolve 统计有 skinned”和“最终 `ShadowSubmissionFrame.draws` 是否真的含 skinned draw”。
   - 当前稳定底线是 semantic rigid/native execute：`semanticSceneLastSubmittedDrawCount=4`、`nativeD3D9BackendLastSuccessfulExecutedDrawCount=4`、`objectFallbackDrawCount=0`。
   - skinned visual gate 仍未签收：短窗口仍会出现 `semanticCoreLastFrameSkinnedDrawCount=0`。
4. 下一轮 control-plane 目标
   - 将 hot-shadow gate 文案继续区分：
     - semantic rigid/native executed 可用。
     - canonical skinned draw 未进入 final frame。
   - 下一轮优先围绕 `TryResolveBestPoseForRenderable()` 的 no-pose detail 补稳定 pose key，而不是继续扩 owner probe。

#### 35.13 2026-04-24 control-plane/AutoTest 更新：semantic skinned 三场景签收

1. 新增/调整的观测面
   - runtime group palette miss counters 已进入 `get_shadow_runtime_summary`：
     - `semanticCoreRuntimeGroupPaletteMissNoSkinningData`
     - `semanticCoreRuntimeGroupPaletteMissNoPosePalette`
     - `semanticCoreRuntimeGroupPaletteMissNoVertexGroups`
     - `semanticCoreRuntimeGroupPaletteMissInvalidGroupTable`
     - `semanticCoreRuntimeGroupPaletteMissMatrixIndexOutOfRange`
     - `semanticCoreRuntimeGroupPaletteMissVertexGroupOutOfRange`
     - `semanticCoreRuntimeGroupPaletteMissFallbacksFailed`
     - `semanticCoreRuntimeGroupPaletteMissLastPoseCount`
     - `semanticCoreRuntimeGroupPaletteMissLastGroupCount`
     - `semanticCoreRuntimeGroupPaletteMissLastMaxVertexGroupSlot`
     - `semanticCoreRuntimeGroupPaletteMissLastMatrixIndexCount`
     - `semanticCoreRuntimeGroupPaletteMissLastMatrixIndex`
   - AutoTest hot-shadow gate 新增 `semanticTailFrameAccepted`，用于表达：
     - semantic frame 已 fresh。
     - DXVK scene summary 已消费。
     - native D3D9 backend 已成功执行。
     - isolated desktop 尾帧不再推进，所以不再用 `requireFrameAdvance` 单独判失败。
2. 关键证据
   - `codex_dynamic_palette_miss_reason_probe_20260424_184021.json`：
     - `semanticCoreRuntimeGroupPaletteMissFallbacksFailed=83`
     - `LastPoseCount=2`
     - `LastGroupCount=92`
     - 结论：当时不是 resource 缺失，而是 pose matrix 数不足以覆盖 group table。
   - `codex_dynamic_uniform_palette_probe_20260424_185320.json`：
     - uniform-palette fallback 后 `semanticCoreSkinnedResolved=58`
     - `semanticSceneSubmittedSkinned=57`
     - `nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount=29`
     - `objectFallbackDrawCount=0`
   - `codex_dynamic_shadow_pressure_final_semantic_skinned_20260424_191212.json`：
     - `ok=true`
     - `semanticCoreSkinnedResolved=54`
     - `semanticCoreRenderableFrameSkinnedDrawCount=53`
     - `semanticSceneSubmittedSkinned=53`
     - `nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount=27`
     - `nativeD3D9BackendLastSuccessfulExecutedDrawCount=32`
     - `objectFallbackDrawCount=0`
3. 三场景 control-plane 结果
   - `model_runtime_probe`：
     - `semanticCoreSkinnedResolved=56`
     - `semanticSceneSubmittedSkinned=37`
     - `nativeD3D9BackendLastSuccessfulExecutedDrawCount=32`
   - `low_pressure_static_reuse`：
     - `semanticTailFrameAccepted=true`
     - `semanticCoreSkinnedResolved=28`
     - `semanticSceneSubmittedSkinned=45`
     - `nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount=28`
   - `dynamic_shadow_pressure`：
     - `semanticCoreSkinnedResolved=54`
     - `semanticSceneSubmittedSkinned=53`
     - `nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount=27`
4. control-plane 新结论
   - 本轮已经跨过 `semanticCoreLastFrameSkinnedDrawCount > 0`、`semanticSceneSubmittedSkinned > 0`、`nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount > 0` 的 gate。
   - `upperLayerEmitted` 仍为 0 不再代表“没有动态阴影”；当前主线已经转成 semantic core + native backend counters。
   - 低压场景的尾帧 stall 是 isolated desktop/windowed 验收口径问题，已通过 `semanticTailFrameAccepted` 单独标识。
5. 下一轮 control-plane 目标
   - 修复 perf report 在 isolated desktop + windowed 下 `frameCount=0 / avgFps=0` 的采样缺口。
   - 将 FPS 验收与 semantic correctness gate 拆开：semantic gate 继续看 summary/native counters，perf gate 必须拿到真实 frame timing。
   - 若 perf gate 证明 CPU 抬升，优先新增 palette/build dedupe counters，不允许把旧 snapshot/freeze 恢复为主路径。

#### 35.14 2026-04-25 control-plane/配置更新：semantic.data 分块关闭

1. 新增配置面
   - runtime profile 新增 `semantic.data` module。
   - control-plane / perf report 的 `runtimeProfile.disabledModules` 会显示 `semantic.data`。
   - AutoTest `RUNTIME_MODULE_ORDER` 与 profile matrix 已同步该模块。
2. 语义数据链关闭语义
   - `DXVK_WAR3_DISABLE=semantic.data` 会关闭上层模型/pose/manifest/contract 数据采集和消费热路径。
   - 该开关不等价于关闭全部 hook；它用于单独判断“上层数据层是否造成卡顿”。
3. 渲染层关闭语义
   - `DXVK_WAR3_DISABLE=render` 是新别名，关闭：
     - `hook.render`
     - `render.queue`
     - `shadow.capture`
     - `shadow.map`
     - `shadow.receiver`
     - `shadow.taa`
     - `postfx`
     - `ssao`
     - `aa`
   - `DXVK_WAR3_DISABLE=shadow` 仍只关闭 shadow capture/map/receiver/taa。
4. 验证
   - `ninja -C build32 -j4` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
5. 下一轮 control-plane 目标
   - 用同一地图按 `dxvk_only -> render,semantic.data -> render -> shadow -> semantic.data -> full_default` 顺序跑分块测试。
   - 把卡顿来源明确归类为地图/JASS、上层 semantic data、shadow 渲染层或 postfx/AA/SSAO。

#### 35.15 2026-04-25 semantic data 子模块排查更新

1. 新硬结论
   - 用户已用 `war3_internal_test_config.h` 实测确认：
     - 关闭 `kWar3RuntimeConfigDisableSemanticData` 后，旧渲染层仍可约 `100 FPS`。
     - 渲染层也关闭后，原版/DXVK 基线约 `250 FPS`。
   - 因此当前性能 blocker 已收敛到上层 semantic data 链，不再优先怀疑旧渲染层、receiver、AA、SSAO、postfx。
2. 新增二分面
   - 语义上游 hook 段：
     - `kWar3RuntimeConfigDisableSemanticModelHooks`
     - `kWar3RuntimeConfigDisableSemanticPoseHooks`
     - `kWar3RuntimeConfigDisableSemanticAttachmentHooks`
   - 语义消费/缓存段：
     - `kWar3RuntimeConfigDisableSemanticFrameRegistries`
     - `kWar3RuntimeConfigDisableSemanticContractCapture`
     - `kWar3RuntimeConfigDisableSemanticConsumer`
3. 当前 control-plane 解释
   - 当前 build 故意只关 `SemanticModelHooks`，不关 semantic 总开关。
   - 目标不是恢复最终功能，而是判断 runtime model / pose / attachment hook 包是不是造成一分钟 ready、白屏、未响应的主因。
4. 后续验收读法
   - 若当前 build 明显恢复帧率：下一步在 model hook 包内部继续拆 pose/attachment/resource。
   - 若当前 build 仍卡：下一步改关 `SemanticFrameRegistries`、`SemanticContractCapture`、`SemanticConsumer`，定位是 frame registry 积累、contract capture，还是 consumer 构建造成。
   - 不允许用旧 snapshot/freeze/VB/IB 路线作为解决方案；本轮只是性能定位。

#### 35.16 2026-04-25 semantic half-enabled guard + summary perf counters

1. control-plane 语义
   - 当前 summary 不再只报告原始 config，而是报告有效态：
     - model producer 关闭时，pose/attachment/frame registry/contract/consumer 均视为 disabled。
     - frame registry 关闭时，contract/consumer 不会继续尝试构建缺 manifest 的 semantic frame。
   - 新字段 `semanticBuildSkippedReason` 当前取值：
     - `0` = no skip
     - `1` = semantic.data module disabled
     - `2` = model producer disabled
     - `3` = frame registries disabled
     - `4` = contract capture disabled
     - `5` = consumer disabled
     - `6` = scene submission disabled
2. perf block
   - 新增 `semanticModelHookCalls/us`、`semanticPoseHookCalls/us`、`semanticAttachmentHookCalls/us`。
   - 新增 `semanticFrameRegistryPublishCalls/us`、`semanticContractCaptureCalls/us`、`semanticConsumerBuildCalls/us`。
   - 新增 `semanticLastHotFunctionTag/us`，便于 AutoTest 在矩阵里直接判断最后热点落在哪段。
   - 当前 calls 的解释：
     - model/pose/attachment calls 来自已有 hook counter 汇总。
     - registry/contract/consumer us 来自新加轻量计时。
3. fresh 验证
   - `ninja -C build32 -j4` 通过。
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - `AutoTest/artifacts/codex_semantic_dependency_guard_smoke_20260425.json`：
     - ready 阶段 `elapsedSec=1.447`。
     - hot-shadow 阶段预期失败，因为 model producer 被关闭。
     - 运行 60 秒期间 `frameIndex=4020`、`render.isInGame=true`，没有复现“两帧后未响应”。
     - summary 中 `semanticBuildSkippedReason=2`、`semanticConsumerEnabled=0`、`semanticConsumerBuildCalls=0`。
4. 当前 blocker
   - half-enabled build storm 已先止血。
   - 下一步要进入真正性能矩阵：从 “只开 model/resource 基础 hook” 开始测，逐步放开 pose、attachment、frame registries、contract capture、consumer。
   - 如果下一轮打开基础 model hook 后 FPS 立刻崩，才需要回到 `war3_model_hook.cpp` 内继续做 hook 内 O(1) 去重/dirty gate；否则继续向 pose/attachment/registry 消费侧推进。

#### 35.17 2026-04-25 semantic consumer perf counters -> 第一轮热点修复

1. control-plane 新增字段
   - `semanticCoreSlowestPoseDirectLookupUs`
   - `semanticCoreSlowestPoseOwnerLookupUs`
   - `semanticCoreSlowestPoseSpriteLookupUs`
   - `semanticCoreSlowestPoseInstanceRegistryUs`
   - `semanticCoreSlowestPoseShadowRegistryUs`
   - `semanticCoreSlowestPoseRenderRegistryUs`
   - `semanticCoreSlowestPoseRuntimeRootsUs`
   - `semanticCoreSlowestPoseMeshPoseContextUs`
   - `semanticCoreSlowestPoseMissDiagnosticUs`
2. 本轮定位结果
   - `model_runtime_probe` 初始细分后显示：
     - `semanticCoreSlowestPoseOwnerLookupUs=67229`
     - `semanticCoreSlowestPoseRuntimeRootsUs=70407`
   - 这证明慢点不是 direct pose，也不是 instance/shadow/render registry，而是 resource owner 全表扫描与 runtimeRoots 泛化兜底。
3. 本轮修复结果
   - `ShadowModelResourceCache` 增加 runtime owner O(1) 索引后：
     - `semanticCoreBuildDurationUs` 从约 `676ms` 降至约 `7ms`。
     - `semanticCoreSlowestPoseOwnerLookupUs` 从约 `67ms` 降至 `3us`。
   - attachment rigid cheap precheck 后，低压图：
     - `semanticCoreBuildDurationUs=53266`
     - `semanticConsumerBuildUs=311318`
     - `semanticCoreSkinnedResolved=132`
     - `semanticCoreSubmittedDrawCount=146`
     - `objectFallbackDrawCount=0`
   - `dynamic_shadow_pressure` 复测通过：
     - `AutoTest/artifacts/codex_dynamic_shadow_pressure_after_attachment_precheck_20260425.json`
     - `ok=true`
     - `semanticCoreBuildDurationUs=7067`
     - `semanticCoreSkinnedResolved=30`
     - `objectFallbackDrawCount=0`
   - trusted runtime roots 追加复测：
     - `AutoTest/artifacts/codex_dynamic_shadow_pressure_trusted_runtime_roots_20260425.json`
     - `ok=true`
     - `semanticCoreBuildDurationUs=143`
     - `semanticCoreSlowestRecordResolveUs=25`
     - `semanticCoreSlowestPoseRuntimeRootsUs=0`
     - `semanticCoreSkinnedResolved=30`
     - `objectFallbackDrawCount=0`
4. 下一步读法
   - 如果后续又看到低压图 `hot-shadow-render-scene-pending`，先看 `semanticCoreSubmittedDrawCount / semanticSceneSubmitted`：
     - core 有 draw 且 fallback=0，说明数据链和 core build 已完成；
     - 问题在 isolated desktop 尾帧 scene consumption 或 AutoTest gate，不要误判成上游数据层失败。
   - 下一轮优先补 `semantic build single-flight / tail-frame drain` 与 FPS report 采样，不再重开 owner/resource/palette 逆向。

#### 35.18 2026-04-25 bounded control-plane drain + report reliability

1. control-plane 行为变更
   - `get_shadow_runtime_summary` / `get_hot_shadow_probe` 在 `refreshSemanticFrameIfStale=true` 时仍先 `requestLatestFrameBuild()`。
   - 若 semantic preview scene submission 已显式启用，则额外执行 bounded drain：
     - 最多 6 个 chunk。
     - 最多 12ms。
     - 只允许记录数不超过 2048 的 build。
   - 这只用于处理 isolated desktop 尾帧不再进入 render-thread build 的情况，不是把 semantic build 主路径改回 control-plane 同步执行。

2. runtime summary 读法
   - `semanticCoreBuildInProgress=false` + `semanticCoreSubmittedDrawCount>0` 现在是 tail-frame drain 成功的主信号。
   - `semanticSceneSubmittedSkinned>0` + `semanticSceneLastSubmittedDrawCount>0` 是 DXVK validation scene 实际消费过 semantic packets 的主信号。
   - `objectFallbackDrawCount=0` 仍是 object shadow 不走旧 fallback 的主信号；perf HTML 里的旧 `shadowRuntimeV2Summary` 可能混入历史帧聚合，优先看 control-plane hot-shadow summary。

3. fresh 验证结果
   - `model_runtime_probe`
     - `semanticCoreBuildDurationUs=135`
     - `semanticCoreSkinnedResolved=30`
     - `semanticSceneSubmittedSkinned=30`
     - `objectFallbackDrawCount=0`
   - `low_pressure_static_reuse / 光影测试低压.w3x`
     - ready 时 build 曾停在 `46/138`。
     - hot-shadow summary 后 `semanticCoreBuildInProgress=false`。
     - `semanticCoreBuildDurationUs=54293`
     - `semanticCoreSkinnedResolved=137`
     - `semanticSceneSubmittedSkinned=157`
     - `objectFallbackDrawCount=0`
   - `dynamic_shadow_pressure`
     - `semanticCoreBuildDurationUs=115`
     - `semanticCoreSkinnedResolved=30`
     - `semanticSceneSubmittedSkinned=30`
     - `objectFallbackDrawCount=0`

4. AutoTest 字段
   - 新增 `fpsSampleReliable` 与相关解释字段。
   - 若 report `frameCount < 10` 或 `windowSec < 1.0`，AutoTest 会标记 FPS 样本不可靠。
   - 本轮三份 HTML report 在 isolated desktop + windowed 下仅采到 1-2 个 Present frame，因此不能用 `avgFps≈0` 判断真实游戏帧率。

5. 下一轮建议
   - correctness 已恢复，下一步继续做性能热点：
     - manifest revision single-flight。
     - contract capture skip / same-frame no-op counter。
     - 低压图 `semanticConsumerBuildUs` 和 `semanticContractCaptureUs` 的累计成本压缩。
   - 若要验证真实可玩 FPS，单独跑可见桌面/前台 perf，不要用当前 isolated desktop windowed report 的 `avgFps`。

#### 35.19 2026-04-25 stale pending clear + contract capture skip counters

1. 新增 control-plane 字段
   - `semanticCoreStalePendingBuildClearedCount`
   - `semanticContractCaptureSkippedStableSameFrame`
   - `semanticContractCaptureSkippedEmpty`
   - `semanticContractCaptureSkippedDuplicateSameFrame`

2. 读法
   - `semanticCoreStalePendingBuildClearedCount > 0` 表示 core 已经发现并清掉“不比当前 completed frame 更新”的 pending build snapshot。
   - `semanticContractCaptureSkippedStableSameFrame > 0` 表示同一帧内 pose/attachment 计数没有增长，contract capture 直接跳过完整 snapshot。
   - `semanticContractCaptureSkippedDuplicateSameFrame > 0` 表示完整 snapshot 后仍被判定为同帧 duplicate/regression，被丢弃但已经计数。
   - 这三个字段用于区分“数据真的没来”和“重复消费同一帧”。

3. fresh 观测
   - 低压图最新 summary：
     - `semanticCoreBuildInProgress=false`
     - `semanticCoreBuildRequestPending=false`
     - `semanticCoreStalePendingBuildClearedCount=5`
     - `semanticContractCaptureSkippedStableSameFrame=8`
     - `semanticContractCaptureSkippedDuplicateSameFrame=17`
     - `semanticContractCaptureUs=69915`
     - `semanticConsumerBuildUs=207979`
     - `semanticCoreSkinnedResolved=144`
     - `semanticCoreAttachmentRigidResolved=6`
     - `semanticSceneSubmittedSkinned=139`
     - `objectFallbackDrawCount=0`
   - dynamic_shadow_pressure hot-shadow summary：
     - `semanticCoreBuildDurationUs=136`
     - `semanticCoreSkinnedResolved=30`
     - `semanticSceneSubmittedSkinned=30`
     - `semanticContractCaptureSkippedStableSameFrame=3`
     - `semanticContractCaptureSkippedDuplicateSameFrame=17`
     - `objectFallbackDrawCount=0`

4. AutoTest 状态
   - `AutoTest/war3_autotest_mcp.py` 已补 `semanticTailSceneNearLatestAccepted`。
   - 但当前 MCP 进程如果未重载，仍会按旧 gate 把低压图 `core frame=149 / scene frame=148` 判成 `hot-shadow-render-scene-pending`。
   - 下轮先重载 MCP，再复测低压图，避免把 validation-host tail-frame 限制误报为 semantic data failure。

5. 当前 blocker
   - correctness 侧：semantic skinned draw + fallback=0 已保持。
   - 性能侧：低压图 `semanticConsumerBuildUs≈208ms` 还偏高，下一刀继续拆 consumer/build 的累计成本。
   - FPS 侧：isolated desktop report 仍可能只有 1 个 Present frame，必须用 `fpsSampleReliable=false` 屏蔽，不作为真实 FPS 结论。

#### 35.20 2026-04-25 semantic validation env gate

1. 新增 AutoTest 行为
   - named scenario 预设现在显式携带 semantic validation env：
     - `DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_BOOTSTRAP_CATCHUP=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_FLUSH=1`
     - `DXVK_WAR3_SEMANTIC_SHADOW_TAIL_FALLBACK=1`
   - 覆盖范围：`model_runtime_probe`、`low_pressure_static_reuse`、`dynamic_shadow_pressure`、`semantic_cost_probe`。
   - 调用方传入的 `env_overrides_json` 仍有最终覆盖权。

2. 为什么要这样做
   - C++ 运行期已经故意要求 `DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW=1` 才允许 semantic scene submission。
   - 这是为了保护普通手动进图不误触实验 consumer。
   - 因此 AutoTest 作为验证宿主必须显式打开 preview；否则 summary 会显示 `semanticBuildSkippedReason=6`，这不是数据链失败。

3. 显式 env 验证读数
   - `model_runtime_probe`
     - `semanticBuildSkippedReason=0`
     - `semanticCoreBuildDurationUs=135`
     - `semanticCoreSkinnedResolved=30`
     - `semanticSceneSubmittedSkinned=30`
     - `semanticCoreSubmittedDrawCount=32`
     - `objectFallbackDrawCount=0`
   - `low_pressure_static_reuse`
     - `semanticBuildSkippedReason=0`
     - `semanticCoreBuildDurationUs=46958`
     - `semanticCoreSkinnedResolved=138`
     - `semanticSceneSubmittedSkinned=146`
     - `semanticSceneSubmitted=146`
     - `objectFallbackDrawCount=0`
     - `semanticContractCaptureUs=58862`
     - `semanticConsumerBuildUs=164037`

4. 当前工具层限制
   - 当前 MCP server 如果没有重载，仍不会自动使用新的 preset env，也不会使用新补的 `semanticTailSceneNearLatestAccepted` gate。
   - 因此本轮显式通过 `env_overrides_json` 验证；下一轮先重载 MCP，再跑 preset。
   - 截图/report 后处理仍可能让总 `ok=false`，但 hot-shadow summary 需要优先看 core/scene/fallback 字段。

#### 35.21 2026-04-25 same-frame semantic scene acceptance

1. AutoTest gate 更新
   - 第二条 control-plane hot-shadow path 已补 same-frame semantic scene acceptance。
   - `scene_contract_ok` 现在要求：
     - `semanticSceneSubmitted > 0`
     - `objectFallbackDrawCount == 0`
     - manifest fresh / latest 条件满足
     - 若要求 scene consumption，则 `semanticSceneConsumptionFresh=true`
   - 当上述条件成立且 attachment gate 通过时，AutoTest 会设置：
     - `semanticSceneOnlyAccepted=true`
     - `originalError=<旧 wait_until error>`
     - `error=""`

2. 为什么这是正确 gate
   - 当前 DXVK validation 阶段的关键验收不是 native backend counter，而是 semantic scene 真的提交并消费了 draw packets。
   - 低压图 9.11.32 读数已经满足：
     - `semanticCoreSkinnedResolved=138`
     - `semanticSceneSubmittedSkinned=146`
     - `semanticSceneConsumptionFresh=true`
     - `objectFallbackDrawCount=0`
   - 因此旧 `hot-shadow-skinned-pending` 是工具 gate 滞后，不是 runtime contract 失败。

3. 验证状态
   - `python -m py_compile AutoTest/war3_autotest_mcp.py` 通过。
   - `ninja -C build32 -j4` 通过。
   - 当前 MCP server 需要重载后才会实际使用新 gate；不要用旧 MCP 结果覆盖本结论。

4. 下一步 control-plane 观察重点
   - 重载 MCP 后复测 named scenario，确认返回里出现 `semanticSceneOnlyAccepted=true` 或 tail scene accepted 标记。
   - 继续看性能字段：
     - `semanticConsumerBuildUs`
     - `semanticContractCaptureUs`
     - `semanticLastHotFunctionTag`
     - `semanticLastHotFunctionUs`
   - correctness 字段仍优先看：
     - `semanticCoreSkinnedResolved`
     - `semanticSceneSubmittedSkinned`
     - `objectFallbackDrawCount`

#### 35.22 2026-04-25 root supplement resource/no-geoset diagnostics

1. Control-plane 字段新增
   - `get_frame_manifest_summary` 现在能区分 root supplement resource miss：
     - `rootUnitSupplementResourceCacheMiss`
     - `rootUnitSupplementResourceCacheNotReady`
     - `rootUnitSupplementResourceSemanticKeyResolved`
     - `rootUnitSupplementResourceSemanticKeyReady`
   - no-geoset 现在进一步拆成：
     - `rootUnitSupplementSkippedNoGeosetZeroCount`
     - `rootUnitSupplementSkippedNoGeosetStoreMiss`
     - `rootUnitSupplementSkippedNoGeosetNotReady`

2. Fresh 验证读数
   - `model_runtime_probe` 小图验证：
     - `rootUnitSupplementResourceSemanticKeyResolved=18`
     - `rootUnitSupplementResourceSemanticKeyReady=18`
     - `rootUnitSupplementAppended=2`
     - `semanticCoreSubmittedDrawCount=2`
     - `semanticCoreSkinnedResolved=0`
   - `low_pressure_static_reuse` 预算版验证：
     - `rootUnitSupplementSkippedNoResource≈46~54`
     - `rootUnitSupplementSkippedNoGeoset≈849~857`
     - `rootUnitSupplementSkippedNoGeosetStoreMiss≈848`
     - `rootUnitSupplementSkippedNoGeosetZeroCount=0`
     - `rootUnitSupplementSkippedNoGeosetNotReady=0`
     - `semanticSceneSubmitted=4`
     - `semanticSceneSubmittedSkinned=0`
     - `objectFallbackDrawCount=0`

3. 证伪路线
   - “resource 完全找不到”已不是主 blocker；semantic key 反查能把 no-resource 从 735 级别降到 50 左右。
   - “geoset 本身为空或 not ready”也不是主 blocker；zero/notReady 基本为 0。
   - “热帧全量 rebuild ShadowModelResourceStore”不可用；低压图会直接 pipe timeout。

4. 下一步 control-plane 观察重点
   - 继续观察：
     - `rootUnitSupplementSkippedNoGeosetStoreMiss`
     - `rootUnitSupplementAppended`
     - `semanticCoreSkinnedResolved`
     - `semanticSceneSubmittedSkinned`
   - 下一轮实现应补“增量 resource store alias/add”，目标是让 `StoreMiss` 明显下降，而不是继续加大 semantic key resolve 预算。

#### 35.23 2026-04-25 root supplement geoset overlay / core cache fallback

1. Control-plane 字段新增
   - `get_frame_manifest_summary` 新增：
     - `rootUnitSupplementGeosetCacheFallback`
   - 用途：统计 root/unit supplement 因 `ShadowModelResourceStore` miss 后，直接从 `ShadowModelResourceCache` 取到单条 geoset 的次数。

2. Fresh 验证读数
   - `codex_core_cache_fallback_low_pressure.json`
     - `rootUnitSupplementSkippedNoGeosetStoreMiss=18`
     - `rootUnitSupplementGeosetCacheFallback=60`
     - `rootUnitSupplementAppended=32`
     - `recordsWithResolvedGeoset=138`
   - `codex_core_cache_fallback_dynamic_pressure.json`
     - `rootUnitSupplementSkippedNoGeosetStoreMiss=2`
     - `rootUnitSupplementGeosetCacheFallback=24`
     - `semanticCoreSubmittedDrawCount=6`
     - `semanticCoreSkinnedResolved=2`
     - `objectFallbackDrawCount=0`

3. 证伪路线
   - `StoreMiss≈848` 已被证伪为“几何本身缺失”；实际是 store/alias 快照缺单条 geoset 索引。
   - control-plane 同步 drain pending build 仍不可用；一次 6 chunk / 12ms 上限的尝试仍导致 `等待 control-plane 响应超时`，已从代码中撤回。

4. 下一步 control-plane 观察重点
   - 当前重点从 geoset store-miss 转为 latest semantic build 消费时机：
     - `semanticCoreBuildRequestPending`
     - `semanticCoreSourcePublishRevision`
     - `semanticCoreManifestPublishRevision`
     - `semanticSceneSubmittedSkinned`
     - `objectFallbackDrawCount`
   - 下一轮不要回到 full rebuild 或扩大 semantic key budget；应在 render thread/scene submission 侧解决 pending build 更早消费。

#### 35.24 2026-04-25 indexed owner hydrate / direct-pose coverage

1. Control-plane 路线更新
   - 新增 `ShadowModelResourceCache::findRuntimeModelOwnerIndexed(...)`。
   - 该接口只查既有 `runtimeGeoset/runtimeGeosetData -> runtimeModel` 索引，不进入全量 runtime owner 扫描。
   - `ShadowRuntimeContractCache::captureLiveState()` 在 build 前用该索引对 manifest 做 runtime-owner hydrate，补 `runtimeModelPtr / modelResourcePtr / modelKey`。

2. Fresh 验证读数
   - `AutoTest/artifacts/codex_low_pressure_indexed_owner_hydrate_20260425.json`
     - `semanticCoreResolved=84`
     - `semanticCoreSkinnedResolved=84`
     - `semanticCoreSubmittedDrawCount=84`
     - `semanticCoreSkippedNoPose=44`
     - `semanticCoreBuildDurationUs=11400`
     - `semanticCoreSlowestRecordResolveUs=347`
     - `semanticCoreSlowestResourceLookupUs=0`
     - `semanticCoreSlowestPoseResolveUs=2`
     - `objectFallbackDrawCount=0`
     - `nativeD3D9BackendSubmittedSkinnedDrawCount=84`
     - `nativeD3D9BackendExecuteAttemptCount=0`

3. 结论
   - 相对 strict direct-pose 样本，skinned direct coverage 从 `66` 提升到 `84`，no-pose 从 `62` 降到 `44`。
   - 计算量没有反弹，resource/pose fallback 仍保持在 O(1) direct path；这证明下一步应继续在 producer/contract 侧补 key，而不是恢复 consumer-side fallback 扫描。
   - 当前不是最终视觉通过：截图偏暗，isolated desktop perf report `frameCount=3` 不适合作为 FPS 结论，且 native backend execute counter 仍为 0。

4. 下一步 control-plane 观察重点
   - 对剩余 `semanticCoreSkippedNoPose=44` 做 direct pose producer 侧补齐。
   - 追 `nativeD3D9BackendExecuteAttemptCount=0`，确认 submitted semantic skinned packets 未实际 execute 的时机问题。
   - scene execute 稳定后再排查亮度/色温与真实 FPS。

#### 35.25 2026-04-25 semantic.data visible manifest 性能定位

1. Control-plane 路线更新
   - 新增 visible manifest 热路径二分开关，用于区分 frame registry 生命周期、visible record 写入、以及 `FinalizeVisibleRecord()` 的 per-record 补全成本。
   - 当前临时性能配置保留 `semantic.data` 与 `FrameRegistries`，但启用 lightweight visible writes，跳过 hot-path finalizer。

2. Fresh 验证读数
   - `codex_manual_data_only_shadow_off_20260425.json`
     - shadow/postfx/AA/semantic consumer 全关，semantic.data 保留：`avgFps=8.592`。
     - `semanticModelHookCalls=486`，`semanticModelGeosetResourceUs=81673`，但后续证明 geoset capture 不是主因。
   - `codex_manual_no_shadow_no_semantic_20260425.json`
     - semantic.data 全关：`avgFps=217.986`。
   - `codex_manual_frame_reg_off_model_on_shadow_off_20260425.json`
     - frame registries 关、model hooks 保留：`avgFps=212.37`。
   - `codex_visible_manifest_writes_off_frame_reg_on_shadow_off_20260425.json`
     - frame registries 开、visible manifest 写入关：`avgFps=215.108`，图内截图 FPS 约 295。
   - `codex_visible_manifest_lightwrite_frame_reg_on_shadow_off_20260425.json`
     - visible manifest 轻量写入 109 条、跳过 finalizer：`avgFps=177.637`。
   - `codex_visible_finalize_no_modelmetadata_shadow_off_20260425.json`
     - 只关 `ResolveModelMetadata()`：`avgFps=14.171`。
   - `codex_visible_finalize_all_heavy_off_shadow_off_20260425.json`
     - 关 identity/model/geoset/sibling heavy stages，但仍进入 finalizer 的基础读取：`avgFps=22.625`。

3. 结论
   - 卡顿主因不是 Pose producer、不是 geoset resource copy、不是 shadow render consumer。
   - 主因已经收敛到 `VisibleRenderableRegistry::appendRecord -> FinalizeVisibleRecord()`，尤其 per-record `TryReadSceneNodeFromRenderablePart / TryReadMeshDataFromRenderablePart` 与后续多级恢复链。
   - 代码层面确认 `SafeRead*Fast` 当前每次都会走 `VirtualQuery`，因此 visible manifest finalizer 会制造 VirtualQuery storm。

4. 下一步 control-plane 观察重点
   - 保持 hot path 只做轻量 visible record append，禁止恢复 per-record finalizer。
   - 新的 contract hydrate 应按 unique key 去重后再补：
     - `renderablePart -> sceneNode/meshData`
     - `meshData/runtimeGeosetData -> geoset`
     - `runtimeGeosetData -> runtimeModel owner`
     - `sceneNode/runtimeModel -> identity/pose`
   - 需要新增 hydrate counters，而不是用 `FrameRegistryPublishUs` 推断热路径：
     - visibleHydrateAttempts / uniqueKeys / safeReadCalls / virtualQueryAvoided
     - visibleHydrateRuntimeModelResolved / geosetResolved / identityResolved
   - 下一轮 correctness 目标：在 lightweight visible writes 下恢复 `semanticCoreSkinnedResolved > 0` 和 `semanticSceneSubmittedSkinned > 0`，同时保持 FPS 不回到 5~20 FPS。

#### 35.26 2026-04-25 visible manifest 帧末基础 hydrate

1. Control-plane 路线更新
   - 当前实现不再把 `FinalizeVisibleRecord()` 放回 visible append 热路径。
   - 新增帧末基础 hydrate：只对 `renderablePart` 去重读取 `sceneNode/meshData`，再重建 visible indexes。
   - 新增受控直读开关 `kWar3RuntimeConfigTrustVisibleRenderablePartPointers`，专门用于本帧 render queue 的 `renderablePart` 固定槽位，避免恢复全局 `SafeReadFast` region cache。

2. Fresh 验证状态
   - `ninja -C build32 -j4` 通过。
   - 本轮隔离桌面 data-only 验证未进图，`runtime_status.json` 显示模块已 Running、`jassReady=true`，但 `gameStarted=false`、`inGameRenderReady=false`；没有可采信 FPS。

3. 结论
   - 已把 visible manifest 的第一刀修复落成“热路径轻写入 + 帧末基础补全”的结构。
   - 这不是最终 semantic-only 修复，只是把性能 blocker 和 correctness hydrate 解耦。
   - 当前 control-plane 仍缺 visible hydrate 专用 counters，下一轮应补：
     - `visibleHydrateRecordCount`
     - `visibleHydrateUniqueRenderablePartCount`
     - `visibleHydrateSceneNodeResolved`
     - `visibleHydrateMeshDataResolved`
     - `visibleHydrateDirectReadCount`
     - `visibleHydrateSafeReadCount`

4. 下一步 control-plane 观察重点
   - 低压图隔离桌面 + windowed 复测，先确认 `FrameRegistries=true + visible basic hydrate` 不回到 5~22 FPS。
   - 如果性能成立，再把 contract hydrate 建在 `bySceneNode/byMeshData` 已恢复的 visible records 上，继续追 `semanticCoreSkinnedResolved / semanticSceneSubmittedSkinned`。
   - 不允许把 identity/model/geoset/sibling recovery 重新放回 `appendRecord()`。

#### 35.27 2026-04-25 render-off semantic.data 稳定窗口

1. Control-plane 路线更新
   - geoset resource capture 关闭时，`CreateGeosetFromRawArrays` hook 也不再安装。
   - `render` runtime group 关闭时，visible manifest / frame registry publish 已保持 fail-fast，不再因为 semantic.data 打开而发布空帧。

2. Fresh 验证状态
   - `ninja -C build32 -j4` 通过。
   - `codex_render_off_semantic_on_no_geoset_hook_perfonly2_20260425.json`：
     - `semanticModelHookCalls=486`
     - `semanticModelHookUs=2658`
     - `semanticModelGeosetResourceCalls=0`
     - `semanticFrameRegistryPublishCalls=0`
   - `codex_render_off_semantic_on_stable_20260425.json`：
     - stable perf window `avgFps=253.767` 后继续到 `avgFps=260.970`
     - `semanticModelHookCalls=486`
     - `semanticModelHookUs=2799`
     - `semanticModelGeosetResourceCalls=0`
     - `semanticFrameRegistryPublishCalls=0`

3. 结论
   - `semantic.data` 在“渲染层关闭”口径下已经不再造成 5 FPS/未响应级卡顿，稳定窗口达到 `250 FPS` 目标线。
   - 当前剩余 hook 成本是启动/加载期间 486 次 promote runtime 级别，总计约 2.8ms，不是逐帧热点。
   - render-off 场景不要用 `wait_until ready` 做验收；该模式故意关闭 `HookRender/RenderQueue/Shadow`，所以 `gameStarted/inGameRenderReady` 不会按正常阴影链路置位。

4. 下一步 control-plane 观察重点
   - 回到 full render/semantic shadow 场景时，继续区分：
     - semantic data producer 是否仍保持 O(1)
     - semantic consumer build 是否 single-flight
     - shadow source distribution 中 `legacyCaptureFallback` 是否仍参与
     - native/DXVK backend execute 是否实际发生
   - 性能优先从消费和绘制层查，不再重开上层 visible finalizer / geoset capture 空转路径。

#### 35.28 2026-04-25 full render 低压图：single-flight / runtime geoset seed / default bypass

1. Control-plane 路线更新
   - `War3TryPopulateSemanticShadowScene()` 现在对 same-frame duplicate submit 做 skip。
   - `ShadowRuntimeContractCache::captureLiveState()` 在 resource cache 为空时，会用 `ModelRegistry` 先补 `runtimeModel/modelResource`，并进一步补 `runtimeGeoset/runtimeGeosetData`。
   - `kShadowSemanticCoreSceneBypassLegacyUnitCaptureEnabled` 默认打开，当前不再依赖临时 env 才旁路 legacy unit fallback。
   - 在 perf report 中新增：
     - `War3SemanticScene/CaptureContract`
     - `War3SemanticScene/EnsureLatestFrameBuilt`
     - `War3SemanticScene/BootstrapCatchup`
     - `War3SemanticScene/SubmitFrame`

2. Fresh 验证状态
   - 初始 full render baseline：
     - `codex_low_pressure_manual_full_20260425.json`
     - `avgFps=63.771`
     - `semanticConsumerBuildCalls=4614`
     - `semanticConsumerBuildUs=5594799`
     - `semanticContractCaptureCalls=4634`
     - `semanticContractCaptureUs=1185203`
   - single-flight 后：
     - `codex_low_pressure_manual_full_after_singleflight_20260425.json`
     - `avgFps=84.961`
     - `semanticConsumerBuildCalls=1533`
     - `semanticContractCaptureCalls=1551`
   - runtime geoset seed 后：
     - `codex_low_pressure_manual_full_after_runtime_geoset_seed_20260425.json`
     - `shadowReadyGeosetCount=326`
     - `matrixPaletteCount=34`
     - `semanticCoreSkinnedResolved=37`
     - `semanticSceneSubmittedSkinned=37`
   - 默认 bypass + skip endframe duplicate flush 后：
     - `codex_low_pressure_after_endframe_flush_skip_20260425.json`
     - `avgFps=89.036`
     - `semanticSceneSubmittedSkinned=32`
     - `objectFallbackDrawCount=0`

3. 结论
   - 当前 low pressure full render 已不再是“旧 fallback 画得快、语义链没用上”的状态，而是：
     - `semanticSceneSubmittedSkinned > 0`
     - `objectFallbackDrawCount = 0`
   - 本轮把项目从“语义阴影能画但 FPS 63”推进到了“语义阴影默认在画且 FPS 89”。
   - 最新 perf scope 已证明剩余热点主要在 semantic scene 消费壳，而不是 shadow core 本体：
     - `CaptureContract ≈ 1.18ms/frame`
     - `SubmitFrame ≈ 0.29ms/frame`
     - `BootstrapCatchup ≈ 0.23ms/frame`
     - `EnsureLatestFrameBuilt ≈ 0.23ms/frame`
     - `Other/UntrackedActive ≈ 8.3ms/frame`

4. 下一步 control-plane 观察重点
   - 继续压 `CaptureContract`：优先跨帧稳定 contract 的复用/去重。
   - 继续压 `EnsureLatestFrameBuilt/BootstrapCatchup`：steady-state 下尽量避免每帧追最新。
   - 当前 correctness 已切到语义主路，下一轮不要为了追 100 FPS 把 legacy fallback 重新拉回来。

#### 35.29 2026-04-26 低压图语义主路 100+ FPS 签收，下一步转 GPU/ShadowMap

1. Control-plane 路线更新
   - semantic scene submission 打开时，旧 `ShadowCapture` 不再作为 object shadow 主路径参与：
     - `War3TryCaptureShadowCaster()` 由 `IsSemanticSceneDisableLegacyShadowCaptureRuntimeEnabled()` 早退；
     - `War3RenderPipeline::OnFrameStart()` 使用 `effectiveModuleShadowCapture`，避免仅靠 device 侧早退仍保留 frame-graph capture 需求。
   - semantic validation preset 关闭 `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD`，让 scene submit 单飞消费 semantic frame。
   - AutoTest DXVK-only gate 不再要求 native backend executed draw；native counter 只在 `DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW=1` 时参与验收。
   - direct CModel pose path 小优化：
     - store reserve；
     - matrix hash 改为 hash 原生 48-byte palette。

2. Fresh 验证状态
   - `ninja -C build32 -j4` 通过。
   - 最佳低压短窗：
     - `AutoTest/artifacts/codex_low_pressure_semantic_optimized_gate_20260426.json`
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_26_03_01_58.html`
     - `avgFps=127.120`
     - `avgGpuTimeMs=1.727`
     - `CaptureContract=0.606ms`
     - `SubmitFrame=0.216ms`
     - `semanticSceneSubmittedSkinned=80320`
     - `objectFallbackDrawCount=0`
   - 正式 low-pressure preset：
     - `AutoTest/artifacts/codex_low_pressure_static_reuse_semantic_optimized_20260426.json`
     - `avgFps=123.572`
     - `hotShadowOk=true`
     - `semanticSceneSubmittedSkinned=93728`
     - `objectFallbackDrawCount=0`
   - 正式 dynamic pressure preset：
     - `AutoTest/artifacts/codex_dynamic_shadow_pressure_semantic_optimized_20260426.json`
     - `avgFps=40.447`
     - `avgGpuTimeMs=14.851`
     - `semanticSceneSubmittedSkinned=33504`
     - `objectFallbackDrawCount=0`

3. 结论
   - 低压图已经超过旧 VB/IB snapshot 路线的 100 FPS 级门槛，同时 `objectFallbackDrawCount=0`，说明不是回退路径带来的假性能。
   - `DXVK_WAR3_DISABLE=shadow.capture` 探针曾到 `125.148 FPS`；本轮正式 gate 已把这条收益收进 semantic scene 默认路径。
   - EndFrame build 关闭后 semantic scene 仍能稳定提交 skinned packet，因此后续默认不再让 EndFrame 与 scene submit 双路构建同一帧。
   - high-pressure 已不是 semantic data 取数卡顿，而是 GPU/ShadowMap 绘制压力：`Shadow/Main GPU≈7.34ms`、`ShadowMap GPU≈6.94ms`。

4. 下一步 control-plane 观察重点
   - 低压继续追 `130 FPS+`：优先看 `CaptureContract≈0.6ms` 能否通过 pose copy/hash cache 降到 `<0.5ms`。
   - 高压转向 shadow renderer：对 `renderShadowMap` 做 skinned caster batch / cascade cull / shadow map resolution A/B，所有 A/B 必须保持 `objectFallbackDrawCount=0`。
   - War3VK 剥离继续等待性能稳定签收；当前先不复制 DXVK 私有 renderer 复杂度。

#### 35.30 2026-04-26 Control-plane 更新：低压 130 FPS+ 已签收，迁移前门槛满足

1. Control-plane 路线更新
   - semantic skinned caster 不再在 ShadowMap 阶段无条件绕过 cascade culling。
   - 当前 cull 策略：
     - skinned caster 使用扩大后的 `center/radius` 与额外 NDC guard；
     - 既避免低 Z/远镜头底部阴影被裁掉，又避免所有 skinned caster 重复打满所有 cascade。
   - AutoTest semantic scene consumption 新增 `near-latest` 模式：
     - `sceneLag <= 2` 且 `coreFrameSerial - sceneFrameSerial <= 2` 时视为 render-scene tail 已消费；
     - 仅用于解决隔离桌面 summary tail lag，不改变 runtime 语义链。

2. Fresh 验证状态
   - `ninja -C build32 -j4` 通过。
   - 新工件：
     - `AutoTest/artifacts/codex_semantic_skinned_cascade_cull_20260426.json`
   - 低压图三轮：
     - `139.723 FPS` / `150.982 FPS` / `149.187 FPS`
     - `median=149.187 FPS`
     - `worst=139.723 FPS`
     - 每轮均 `objectFallbackDrawCount=0`、`semanticSceneSubmittedSkinned > 0`、`semanticCoreSkippedNoPose=0`
   - dynamic pressure：
     - `avgFps=114.928`
     - `avgGpuTimeMs=6.382`
     - `ShadowMap GPU=2.878ms`
     - `Shadow/Main GPU=3.136ms`
     - `objectFallbackDrawCount=0`

3. 结论
   - 迁移前性能门槛已满足：低压 `130 FPS+` 不再只是短窗尖峰，三轮 worst 也超过 130。
   - 当前可见阴影仍由 semantic skinned path 提供，不是 legacy capture/freeze 或 VB/IB snapshot fallback。
   - 高压从上一轮 `40.447 FPS / ShadowMap GPU≈6.94ms` 改善到 `114.928 FPS / ShadowMap GPU=2.878ms`，证明 skinned all-cascade replay 是主要高压瓶颈之一。
   - 低压 `CaptureContract≈0.51ms` 和 `SubmitFrame≈0.20ms` 已进入可接受范围；继续优化还有空间，但不再阻塞 ASI 骨架迁移。

4. 下一步 control-plane 观察重点
   - War3VK/ASI 骨架：
     - 只抽 `BindDevice / PrepareFrame / ExecutePreparedFrame / Reset / GetStats` ABI 与 `DllMain` late-load bootstrap；
     - 只验证 ASI 自动加载、Game.dll 等待、load marker 和稳定退出；
     - 不复制 DXVK renderer 大对象，不改变当前 DXVK 主验证基座。
   - DXVK 后续优化：
     - high pressure 的 cascade draw 去重和 ShadowMap 复用；
     - direct pose cache 尾巴；
     - 若继续补 GPU 利用率，先看 `ShadowMap/ShadowMain`，不要恢复旧 capture。

#### 35.31 2026-04-26 War3VK ASI bootstrap control-plane proof

1. Control-plane 路线更新
   - DXVK semantic shadow 已达到迁移前低压性能门槛后，创建 War3VK 独立 late-inject 骨架。
   - War3VK 当前只作为 bootstrap/ABI proof，不参与渲染主路径。
   - ASI 加载验证已独立于 DXVK semantic performance 数据，不污染低压 130 FPS 结论。

2. Fresh 验证状态
   - `E:\Mycode\Source\Repos\War3VK\War3VK.vcxproj`
     - `Release|Win32` 构建通过，`0 warning / 0 error`
     - 静态 CRT `/MT`
     - DLL 依赖只剩 `KERNEL32.dll`
   - `War3VKLoadSmoke.exe`
     - `LoadLibraryA` 成功；
     - `War3VK_GetAbiVersion` 返回 `1`；
     - `War3VK_GetStats` 成功。
   - `War3VK.asi`
     - 已复制到 `E:\Work\War3\War3VK.asi`
     - 删除临时 `SchattenBoost.dll` 后，War3 隔离桌面窗口化启动仍生成 load marker：
       - `DllMain attached`
       - `Bootstrap worker started`
       - `Game.dll detected`
       - `Bootstrap ready`
   - 工件：
     - `AutoTest/artifacts/codex_war3vk_asi_bootstrap_20260426.json`

3. 结论
   - 当前环境支持根目录 `.asi` 自动加载；War3VK 不需要借 `SchattenBoost.dll` 固定名。
   - late-inject bootstrap 可在地图加载链路中看到 `Game.dll`，但尚未绑定 D3D9 device。
   - 后续迁移应先做 DXVK-host-to-War3VK ABI bridge，而不是直接搬 renderer。

4. 下一步 control-plane 观察重点
   - DXVK 侧可选桥：
     - 默认关闭；
     - `LoadLibraryA("War3VK.asi")` 或检测已加载模块；
     - device ready 后调用 `BindDevice`；
     - 每帧调用 `PrepareFrame/GetStats` 做 lifecycle proof。
   - War3VK 侧：
     - 扩展 stats；
     - 准备最小 frame packet；
     - 仍禁止 OpenGL / DX9Ex 依赖。

#### 35.32 2026-04-30 static hydrate 证伪与 units-only 基线恢复

1. Control-plane 路线更新
   - 当前正式场景提交仍锁定 units-only semantic skinned path。
   - 静态对象不再允许通过 `VisibleRenderableRegistry` 主 snapshot 帧末就地 hydrate 来试探；本轮实测证明这会污染当前 skinned scene 选择并可触发崩溃。
   - 静态对象后续必须走只读 observer 或独立 static shadow candidate store，先把候选/reject reason 暴露到 summary，再考虑提交。

2. Fresh 验证状态
   - 恢复后低压图：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_02_42_45.html`
     - `avgFps=127.891`
     - `semanticSceneSubmittedSkinned=48504`
     - `semanticCoreSubmittedDrawCount=40`
     - `semanticCoreSkippedNoPose=0`
     - `semanticCoreSkippedNoRuntimeGroupPalette=0`
     - `objectFallbackDrawCount=0`
   - 视觉截图：
     - `AutoTest/artifacts/screenshots/war3_20260430_024246.png`
     - skinned unit shadow 可见，未出现灰色全屏 veil 或 construction/scaffold 闪烁。
   - 失败静态 hydrate 证据：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_02_40_06.html`
     - `E:\Work\War3\WarVK\Crash\war3_crash_2026_04_30_02_40_06_678_pid11528_tid27112.dmp`
     - `semanticSceneSubmittedSkinned=0`、`shadowReadyGeosetCount=25`，因此该路径已证伪。

3. 结论
   - semantic-only unit shadow 当前不是 fallback 假象：恢复后 `objectFallbackDrawCount=0` 且 `semanticSceneSubmittedSkinned > 0`。
   - 静态对象缺阴影的原因还不能归结为简单开关未开；主清单静态 hydrate 会改变/破坏 semantic frame readiness。
   - 下一步要把静态对象作为独立候选域观察，不再让静态补全写回 unit 主路径的数据结构。

4. 下一步 control-plane 观察重点
   - 增加静态候选 observer counters：
     - `semanticStaticCandidateCount`
     - `semanticStaticCandidateWithMeshData`
     - `semanticStaticCandidateWithResolvedGeoset`
     - `semanticStaticCandidateRejectedNoResource`
     - `semanticStaticCandidateRejectedHiddenOrNonCanonical`
   - 所有 counters 必须只读采样，不得在主 visible snapshot 上 mutate。
   - 继续保持 `objectFallbackDrawCount=0` 和 `semanticSceneSubmittedSkinned > 0` 作为 baseline gate。

#### 35.33 2026-04-30 stale-frame shadow fix and periodic semantic scene submission

1. Control-plane 路线更新
   - 用户观察到“非当前 caster 的阴影被套到多个 caster 上”后，本轮将 primary suspect 收敛到 semantic completed frame 长期停滞。
   - scene submission runtime 现在采用：
     - periodic `CaptureContract`
     - periodic `SupplementedBuild`
     - 每帧 `SubmitFrame`
     - submit-time live `CModel` runtime group palette refresh
   - control-plane 不再把 `semanticCoreSubmittedDrawCount=0` 误解为无提交；当 scene submission 复用 last completed semantic frame 且本帧有 skinned semantic scene draw 时，summary 回填 reusable packet count。

2. Fresh 验证状态
   - 低压 quick 2K：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_05_50.html`
     - `avgFps=135.540`
     - `semanticSceneSubmittedSkinned=59936`
     - `objectFallbackDrawCount=0`
     - `semanticCoreSkippedNoPose=0`
   - 低压 quick summary fix：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_08_51.html`
     - `avgFps=135.009`
     - `semanticCoreSubmittedDrawCount=40`
   - `low_pressure_static_reuse`：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_12_41.html`
     - `avgFps=155.758`
     - `semanticSceneSubmittedSkinned=112576`
     - `semanticCoreSubmittedDrawCount=40`
     - `objectFallbackDrawCount=0`
   - `dynamic_shadow_pressure`：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_14_08.html`
     - `avgFps=40.689`
     - `avgGpuTimeMs=12.227`
     - `ShadowMap GPU≈4.257ms`
     - `semanticSceneSubmittedSkinned=33728`
     - `semanticCoreSubmittedDrawCount=36`
     - `objectFallbackDrawCount=0`
   - `model_runtime_probe`：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_17_06.html`
     - `shadowRuntimeModelCount=185`
     - `shadowReadyGeosetCount=1394`
     - `semanticCoreSubmittedDrawCount=36`
     - `objectFallbackDrawCount=0`

3. 结论
   - 当前低压 semantic-only skinned path 已重新超过 130 FPS 门槛，且没有恢复 legacy VB/IB snapshot/freeze。
   - `semanticCoreFrameLag` 在当前实现中不再等价于 pose stale；因为 submit path 会从 live CModel 重新生成 skinned palette。需要在下一轮补 `live palette refreshed/missed` counters，以便 control-plane 把 topology lag 与 animation freshness 分开。
   - 高压场景已确认不是 semantic.data 卡死，下一轮应转向 ShadowMap/cascade/GPU 优化。

4. 下一步 control-plane 观察重点
   - 增加 live palette refresh counters 并输出到 `get_shadow_runtime_summary`。
   - 继续保持 `objectFallbackDrawCount=0`、`semanticSceneSubmittedSkinned>0`、`semanticCoreSkippedNoPose=0`。
   - 高压优化优先级：
     - skinned caster cascade culling；
     - ShadowMap draw 合并；
     - shadow map 更新复用；
     - 必要时做动态分辨率，但低压视觉不先牺牲。
   - 静态对象仍走只读 observer，不允许再让 static hydrate 写主 manifest。

#### 35.34 2026-04-30 submit-time live palette counters verified and next perf boundary

1. Control-plane 路线更新
   - `semanticSceneLivePaletteRefreshAttempt/Hit/Miss` 已经接入 summary/control-plane/perf report，并在低压图中证明 submit-time live CModel palette 每次都命中。
   - `semanticSceneSkinnedFullIndexFallbackCount=0` 与 `semanticSceneSkinnedDynamicIndexSliceCount>0` 继续作为 no-VB/IB snapshot/freeze 的主 gate。
   - 当前不再把 `semanticCoreFrameLag` 单独作为动画 freshness 判据；动画 freshness 以 live palette refresh hit/miss 为准，topology freshness 由 periodic capture/build 单独观察。

2. Fresh 验证状态
   - 低压图 live palette 按需解码首轮：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_20_02_16.html`
     - `avgFps=94.386`
     - `semanticSceneSubmittedSkinned=81583`
     - `semanticSceneLivePaletteRefreshHitCount=81583`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 当前最终复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_20_09_11.html`
     - `avgFps=87.795`
     - `semanticSceneSubmittedSkinned=68230`
     - `semanticSceneLivePaletteRefreshHitCount=68230`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticCoreSubmittedDrawCount=50`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`

3. 结论
   - 上层数据链已经不是当前卡顿主因：semantic.data render-off 历史基线已到 253~261 FPS；本轮 full render 下 live palette/identity 直读均稳定。
   - 当前 control-plane 应把 `semanticSceneLivePaletteRefreshMissCount == 0` 纳入 correctness gate，避免再误判 `frameLag`。
   - 当前 low-pressure FPS 没有稳定回到凌晨 4 点的 130+ 档位，说明还有一个 runtime/profile/scene 条件差异或未计入 hot path。需要继续对比 `20:02` 与 `04:12/04:05` artifact 的 map/runtime overlay 与 perf scope。

4. 下一步 control-plane 观察重点
   - 新增或临时启用 `SubmitFrame` 子阶段 counters：
     - `semanticSubmitLivePaletteBuildUs`
     - `semanticSubmitPaletteIndexUs`
     - `semanticSubmitGeometryLookupCreateUs`
     - `semanticSubmitBoundsUs`
     - `semanticSubmitInstancePushUs`
   - 继续记录 `semanticSceneSkinnedFullIndexFallbackCount=0`、`objectFallbackDrawCount=0`、`semanticSceneSubmittedSkinned>0`。
   - 静态对象进入下一阶段时只允许观察和独立候选 store；禁止 static hydrate 写主 manifest。

#### 22.14 2026-04-30 submit cap runtime override and limited-submit verification

1. Control-plane 路线更新
   - `semantic submit cap` 现在可以通过 `DXVK_WAR3_SEMANTIC_SUBMIT_DRAW_CAP` runtime override 调整；默认仍为 64。
   - `ShadowRendererCore::submitFrameLimited` 已替代旧的临时 `cappedFrame.draws` 拷贝，后续 cap 调优不需要复制 `ShadowDrawPacket` 列表。
   - 低压 correctness gate 继续固定为：
     - `semanticSceneSubmittedSkinned > 0`
     - `semanticSceneLivePaletteRefreshMissCount == 0`
     - `semanticSceneSkinnedFullIndexFallbackCount == 0`
     - `objectFallbackDrawCount == 0`

2. Fresh 验证状态
   - 低压 cap=64 + limited submit：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_22_05_19.html`
     - `avgFps=108.835`
     - `semanticSceneSubmittedSkinned=105205`
     - `semanticSceneLivePaletteRefreshHitCount=105205`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticCoreSubmittedDrawCount=49`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 高压 cap=64 + limited submit：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_22_06_36.html`
     - `avgFps=29.134`
     - `semanticSceneSubmittedSkinned=22896`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 高压 cap=32 试验：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_22_13_29.html`
     - `avgFps=31.028`
     - `semanticSceneSubmittedSkinned=11744`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`

3. 结论
   - `owned dynamic index slice` + submit-time live palette 组合仍是当前 no-fallback skinned 主路径；本轮没有恢复 VB/IB snapshot/freeze。
   - 高压 FrameSelect/FrameStats 已从 blocker 中退出；当前 control-plane 应把关注点切到 `Other/UntrackedActive` 与 SubmitFrame self。
   - `DXVK_WAR3_SEMANTIC_SUBMIT_DRAW_CAP=32` 可作为压力图临时性能档，但默认暂不下调，避免正常视觉中 caster 数量被过度裁剪。
   - 当前 no-shadow perf-only runner 会在 `inGameRenderReady=false` 状态产出报告，必须修正后才能作为 baseline。

4. 下一步 control-plane 观察重点
   - 增加有效的 in-map baseline marker：即使 shadow 模块关闭，也要能确认 map loaded / world render ready。
   - 如果继续追低压 130+，优先解释 `Other/UntrackedActive` 从 `04:12` 的 `5.861ms` 到当前 `8.580ms` 的差值。
   - SubmitFrame 后续 micro-scope 重点：append packet self、palette cache lookup、persistent geometry lookup、shadow caster vector push。

5. AutoTest baseline 补充
   - `run_quick_autotest` 已支持 perf-only ready timeout 返回，用于后续 shadow-off/no-semantic smoke 矩阵不再卡死。
   - 当前 `shadow,semantic.data` disabled 报告 `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_22_21_14.html` 显示 `avgFps=276.395`，但 runtime status 仍为 `inGameRenderReady=false / visibleCount=0`。
   - 因此 control-plane 不能把该值当成 in-map baseline；下一步需要独立 world-ready/on-map marker，避免把菜单/非进图 FPS 混入性能结论。

#### 02.30 2026-05-01 dynamic-pose gate update

1. Correctness gate 更新
   - 旧 gate `semanticSceneLivePaletteRefreshMissCount == 0` 已废弃为 correctness 条件。
   - 原因：submit-time `CModel +0x5C/+0x60` live palette 在低压图同一 runtime 上 raw hash 长期稳定，命中不代表动画姿态有效。
   - 新 gate：默认 `semanticSceneLivePaletteRefreshAttemptCount == 0`，并要求 `semanticSceneSubmittedSkinned > 0`、`semanticSceneSkinnedFullIndexFallbackCount == 0`、`objectFallbackDrawCount == 0`。

2. Fresh counters / artifacts
   - 静态 CModel 证据：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_01_52_50.html`
     - `semanticSceneLivePaletteMotionRawChangedCount=0`
     - `semanticSceneLivePaletteMotionRawStableCount=66481`
   - alias 证伪：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_01_58_27.html`
     - `semanticSceneLivePaletteMotionRawChangedCount=0`
     - `semanticSceneLivePaletteMotionRawStableCount=69557`
   - 正式路径复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_02_27_04.html`
     - `semanticSceneLivePaletteRefreshAttemptCount=0`
     - `semanticSceneSubmittedSkinned=73350`
     - `semanticSceneSubmittedPersistent=73350`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 动作压力图复测：
     - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_02_33_41.html`
     - `semanticSceneLivePaletteRefreshAttemptCount=0`
     - `semanticSceneSubmittedSkinned=19408`
     - `semanticSceneSubmittedFrameLocal=0`
     - `semanticSceneSkinnedFullIndexFallbackCount=0`
     - `objectFallbackDrawCount=0`
   - 动态视觉证据：
     - `AutoTest/artifacts/screenshots/dynamic_pose_check_20260501_022220/pose_01.png`
     - `AutoTest/artifacts/screenshots/dynamic_pose_check_20260501_022220/pose_04.png`

3. Control-plane 口径
   - `semanticSceneLivePaletteMotion*` 只用于证明 submit-time CModel 源是否动态，不再作为主路径成功条件。
   - 如果后续有人显式打开 `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH=1`，必须同时观察 `semanticSceneLivePaletteMotionRawChangedCount > 0`，否则该刷新源应被视为静态污染源。
   - frame-local dynamic mesh 仍是诊断路径；正式低压主路应保持 `semanticSceneSubmittedFrameLocal == 0` 且 `semanticSceneSubmittedPersistent > 0`。

4. 下一步观测重点
   - 给 packet runtimeGroupPalette 增加独立 motion counter，避免再用 CModel raw hash 误判 packet 动态性。
   - 继续检查低 Z / caster bounds / cascade culling 下的视觉对齐，尤其是单位脚下阴影和模型姿态是否一致。
   - 性能优化只能基于 packet palette 正式路径进行，不允许默认恢复 submit-time live CModel refresh。

#### 04.43 2026-05-01 control-plane correction: source-range palette producer

1. Gate 口径修正
   - `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH` 重新允许默认开启，但成功条件不再是“读 CModel+0x60 命中”。
   - 新语义：live refresh 必须优先消费 `PoseRegistry` 中由 `0x6F12FDC0` source-range copy 发布的 palette；只有该发布缺失时才回退 `CModel+0x60`。
   - `0x6F12FF50` flush/reset 只能作为诊断 publisher counter，禁止写入 PoseRegistry palette。

2. Fresh counters
   - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_04_40_56.html`
     - `semanticCoreFrameFresh=true`
     - `semanticCoreSubmittedDrawCount=47`
     - `semanticSceneSubmittedSkinned=58904`
     - `semanticSceneLivePaletteRefreshAttemptCount=58904`
     - `semanticSceneLivePaletteRefreshHitCount=58904`
     - `semanticSceneLivePaletteRefreshMissCount=0`
     - `semanticSceneLivePaletteRefreshLastMatrixCount=9`
     - `semanticSceneSubmittedPaletteMotionChangedCount=58887`
     - `objectFallbackDrawCount=0`

3. 新增防回退规则
   - 如果后续看到 `semanticCoreFrameFresh=false` 伴随周期性 zero submit，不要先调高 build passes；先确认 pose-only publish 是否误 bump manifest revision。
   - 如果后续看到阴影回到初始化姿态，不要重新启用 flush palette 或旧 CModel-only live refresh；先检查 `RecordRuntimeMatrixPaletteFromRangeCopy` 是否命中。
   - `capturePoseOnlyLiveState()` 的 contract 只允许更新 poses/stats，不允许修改 manifest topology/publishRevision，不允许请求 `ShadowValidationRuntime::requestLatestFrameBuild()`。

4. 下一步 counter 建议
   - 增加 `runtimeMatrixRangeCopyPalettePublishHit/Miss` 与 `runtimeMatrixFlushPaletteSuppressed`，让 control-plane 直接区分 source-range producer 和 flush/reset 诊断。
   - 增加 live PoseRegistry refresh cache hit/miss 与 remap us，用于把 correctness 版本从约 83 FPS 继续推回 100+。

## 2026-05-01 06:42 +08:00 - control-plane fresh gate 更新

1. Gate 口径修正
   - `publishRevision` 只代表 manifest topology/resource 版本；pose-only contract 的动画新鲜度必须看 `frameSerial`。
   - `semanticCoreFrameFresh` 和人工判断都必须同时看：
     - `semanticCoreFrameSerial`
     - `semanticCoreManifestFrameSerial`
     - `semanticCoreFrameLag`
     - `semanticSceneSubmittedPaletteMotionChangedCount`
   - `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH` 保持 diagnostic 默认关闭；submit-time `CModel+0x60` refresh 不能作为生产 gate。

2. Fresh counters
   - `AutoTest/artifacts/codex_pose_only_rebuild_20260501/result.json`
     - 修复前：`semanticCoreFrameSerial=145` 固定，`semanticCoreManifestFrameSerial` 持续推进，证明 core 没有消费 pose-only frame。
   - `AutoTest/artifacts/codex_pose_frame_freshness_20260501/result.json`
     - 修复后：`semanticCoreFrameSerial` 持续推进，`semanticCoreFrameLag` 多数为 1-4。
     - `runtimeMatrixRangeCopyPalettePublishHitCount` 持续增长，`runtimeMatrixRangeCopyPaletteFallbackCModelCount=0`。
     - 非零提交帧 `semanticSceneSubmittedSkinned=34`、`objectFallbackDrawCount=0`。
   - `AutoTest/artifacts/codex_pose_visual_probe_20260501/result.json`
     - 三张隔离桌面截图产出成功，作为视觉复核 artifact。

3. 新增防回退规则
   - 如果阴影再次锁初始姿态，不要先恢复 `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH=1`；该路径已证实容易读到静态/初始 CModel palette。
   - 如果 `semanticCoreFrameSerial` 不动而 `semanticCoreManifestFrameSerial` 增长，优先检查 frameSerial fresh gate 和 `ShadowValidationRuntime` pending/build 状态。
   - 如果 `semanticCoreFrameSerial` 动但视觉仍静态，下一层只查 packet palette hash、palette upload/cache 和 shadow map draw，不重开 owner identity 逆向。

4. 下一步 counter 建议
   - 新增 `semanticSceneReusableFrameForcedCount` / `semanticSceneSelectedFrameEligibleZeroCount`。
   - 新增 packet 层 `semanticPacketRuntimeGroupPaletteChanged/Stable/LastHash/LastRuntime`，不要只依赖 scene submitted aggregate。

## 2026-05-01 07:12 +08:00 - ShadowMap dynamic skinned reuse gate

1. Gate 口径修正
   - `semanticSceneSubmittedPaletteMotionChangedCount > 0` 只能证明 packet palette 动；若 ShadowMap 被自适应复用，视觉仍会停在旧 depth map。
   - 动态 skinned caster 的 ShadowMap 更新判定不能依赖 `ObjectKind == Unit`。
   - 任何 `skinned` semantic packet 都必须参与 `dynamicPoseSignature`，并且在 `semanticSceneSubmittedSkinned > 0` 时默认禁止上一帧 ShadowMap 复用。

2. Fresh counters
   - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_07_21.html`
     - `avgFps=84.032`
     - `dynamicSkinnedOutputCount=66296`
     - `semanticSceneSubmittedSkinned=66296`
     - `semanticSceneSubmittedPaletteMotionSampleCount=66296`
     - `semanticSceneSubmittedPaletteMotionChangedCount=66279`
     - `semanticSceneSubmittedPaletteMotionStableCount=0`
     - `objectFallbackDrawCount=0`
     - `ShadowMap callsPerFrame=0.999`

3. 视觉证据
   - `AutoTest/artifacts/codex_dynamic_pose_sequence_20260501/pose_seq_01.png`
   - `AutoTest/artifacts/codex_dynamic_pose_sequence_20260501/pose_seq_06.png`
   - 同一隔离桌面窗口化场景中，单位动作和地面阴影 silhouette 均变化；本轮没有复现“约 3 秒消失”。

4. 防回退规则
   - 如果未来为了性能恢复 ShadowMap 自适应复用，必须使用 `dynamicPoseSignature` 或 packet palette hash 判断动态内容是否真的稳定。
   - 禁止再用“caster count 稳定 + camera 稳定”直接复用含 skinned semantic caster 的 ShadowMap。
   - `objectFallbackDrawCount == 0` 仍是主路径验收条件；当前修复没有恢复旧 VB/IB snapshot/freeze。

## 2026-05-01 07:26 +08:00 - dynamic signature covers world placement

1. Gate 口径修正
   - `dynamicPoseSignature` 现在必须被视为 “pose/palette + world placement” 签名，而不只是 animation palette 签名。
   - 后续任何 ShadowMap reuse 逻辑只有在该 signature 稳定时才允许复用；禁止回退到 caster count / camera delta-only 判定。

2. Fresh counters
   - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_20_48.html`
     - `semanticSceneSubmittedSkinned=30732`
     - `dynamicSkinnedOutputCount=30732`
     - `objectFallbackDrawCount=0`
   - `AutoTest/artifacts/codex_dynamic_pose_sequence_after_worldhash_20260501/result.json`
     - 6 次 capture 均 `ok=true`
     - 每次 control-plane summary：`semanticSceneSubmittedSkinned=34`
     - 每次 control-plane summary：`semanticSceneSubmittedPaletteMotionChangedCount=34`
     - 每次 control-plane summary：`objectFallbackDrawCount=0`

3. 新增防回退规则
   - 如果未来恢复 adaptive ShadowMap reuse，只能使用已纳入 `draw.worldMatrix/bounds` 的 `dynamicPoseSignature`。
   - 如果用户反馈“单位移动后阴影几秒才跟上”，第一检查点是 `D3D9DeviceEx::War3TryAppendSemanticShadowPacket` 的 dynamic hash 是否仍包含 world matrix。
   - 不允许为 FPS 回退到“有 skinned caster 但只看 camera/caster count”的 ShadowMap 复用。

## 2026-05-01 07:38 +08:00 - ShadowMap safe reuse gate

1. Gate 口径修正
   - `hasDynamicSkinnedCasters` 不再直接禁止 ShadowMap reuse；它现在只要求 `dynamicPoseSignature` 必须非零且稳定。
   - 静态场景仍可按 camera/caster delta 复用；动态 skinned 场景必须通过 signature gate。

2. Fresh counters
   - `E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_32_41.html`
     - `semanticSceneSubmittedSkinned=35560`
     - `dynamicSkinnedOutputCount=35560`
     - `objectFallbackDrawCount=0`
     - `ShadowMap callsPerFrame=0.583`
   - `AutoTest/artifacts/codex_dynamic_pose_sequence_safe_reuse_20260501/result.json`
     - 6 次 capture 均 `ok=true`
     - 每次 summary：`semanticSceneSubmittedSkinned=34`
     - 每次 summary：`semanticSceneSubmittedPaletteMotionChangedCount=34`
     - 每次 summary：`objectFallbackDrawCount=0`

3. 新增防回退规则
   - 禁止删除 `dynamicPoseSignature != 0` 条件；否则一旦未来某条 dynamic skinned 路径没写 signature，会重新把动态 caster 当静态复用。
   - 如果后续做更激进的 ShadowMap reuse，必须先提供 reuse-hit/reuse-miss reason counters，不能只看 FPS。
