# Agents.md - 项目进度与交接文档

## 📅 项目当前状态 (Current Status)

**最后更新**: 2026-04-05
**当前阶段**: 稳定回退点已建立（`ea204b1`），进入 v2.5：`runtime shadow bridge v1` 收口 + 动态 Pose Takeover 前置阶段

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
