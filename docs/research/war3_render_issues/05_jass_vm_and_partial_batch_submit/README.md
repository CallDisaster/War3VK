# 05 - JASS VM 与局部合并提交专项

## 背景
当前性能报告显示 `Other/Untracked` 占比高，且 `Hook_Dispatch_Common/Special` 仍是渲染热路径。
本专项聚焦两件事：
1. 先把 JASS VM 主循环路径“看清楚”；
2. 在不强制开启全量队列接管的前提下，落地一个可实际生效的局部合并提交优化。

## 目标
- 给出 JASS VM 的可观测链路：`executeJassFunction -> ExecuteJassFunctionInternal -> JassInterpreter_MainLoop`。
- 落地“低风险、可回滚”的渲染局部合并：复用同 `renderablePart` 连续 Dispatch 的桥接上下文，减少 `ExecBatch Begin/End` 频次。
- 维持渲染语义不变（不改 Draw 顺序、不改 Layer/State 判定）。

## 已落地（2026-02-23）

### A. JASS VM 追踪
- 新增地址簿字段与 1.27a RVA：
  - `executeJassFunctionInternal = 0x7F2D92`
  - `jassInterpreterMainLoop = 0x7F1A20`
- 在 JASS Hook 中新增：
  - `JassVM/ExecuteJassFunction`
  - `JassVM/ExecuteFunctionInternal`
  - `JassVM/MainLoop`
- 新增返回码统计与低频汇总日志（completed/timeout/pause/error）。
- 新增预算策略开关：固定覆盖（override）与自适应（adaptive，默认关闭）。

### B. 渲染局部合并提交（本轮核心）
- 新增开关：
  - `kNativeDispatchLocalContextMergeEnabled`（默认开启）
  - `kNativeDispatchLocalContextMergeStatsLogging`（默认开启）
  - `kNativeDispatchLocalContextMergeStatsInterval`（默认 50000 calls）
- 在 `Hook_RenderQueue_Dispatch_Common/Special` 增加局部上下文复用：
  - 对“世界标签 + 需要对象追踪”的路径，若连续命中同 `renderablePart`，复用上一个 `ExecBatch` 上下文；
  - 不满足复用条件时再切换上下文；
  - 非复用路径自动收口，避免状态泄漏。
- 在 `Hook_FlushAndReset` 帧尾增加强制收口：
  - 调用 `War3HookRender::ResetDispatchMergeContext()`，确保不跨帧残留。

## 代码落点
- `src/d3d9/war3/core/war3_internal_test_config.h`
- `src/d3d9/war3/hooks/war3_hook_render.cpp`
- `src/d3d9/war3/hooks/war3_hook_render.h`
- `src/d3d9/war3/hooks/war3_hook_lifecycle.cpp`

## 验证方法
1. 编译：`ninja -C build32`
2. 运行后观察日志：
   - `DXVK War3Hook: DispatchLocalMerge calls=... begin=... reuse=... end=... reusePct=...`
   - `DXVK War3Hook[Jass]: MainLoop stats calls=... timeout=...`
3. Perf 报表重点看：
   - `Hook_Dispatch_Common` / `Hook_Dispatch_Special` 的 Avg CPU 是否下降；
   - `Other/Untracked` 是否继续收敛（结合 MainLoop 与 Pump/Dispatch 分段）。

## 下一步
- 若 `reusePct` 稳定且无画面回归，再推进“按 SceneNode 聚合”的更大粒度局部合并。
- 若局部合并收益稳定，再评估开启更激进的 `QueueTakeover + AutoInstancing` 进行 A/B。
- 在 Perf HTML 增加 JASS 专题面板，避免 JASS 与渲染开销混在 `Other/Untracked`。

## 交叉索引
- 留言1~4统一归档：`../06_message_1_4_archive/README.md`
- 静态阴影专项：`../03_building_static_shadow/README.md`
