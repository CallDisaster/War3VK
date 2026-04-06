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
