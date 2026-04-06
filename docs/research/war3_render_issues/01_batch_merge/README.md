# 研究方向一：渲染对象批次合并（Batch Merge）

## 问题定义
当前 Auto-Instancing 仍存在“每个对象都触发一次完整 `dispatchCommon`”的问题，导致合批收益被 CPU 调度成本抵消，`FQ_Dispatch_Opaque` 仍是热点。

## 关键路径
- 代码入口：`src/d3d9/war3/reimpl/war3_render_queue.h`
- 关键函数：`FlushSortedItems_StdSort()`
- 关键指标：
  - `Opt/BatchMerge/FlushGroup`
  - `Opt/BatchMerge/SingletonBypass`
  - `FQ_Dispatch_Opaque`

## 已实现优化（本轮）
1. 收紧“起批”条件，新增下一项 `sceneNode` 可读性检查，避免“起批后下一项无效 -> singleton 空转”。
2. 将 `Opt/BatchMerge/FlushGroup` scope 移到 `pendingCount > 1` 分支，避免单实例 flush 也计入 FlushGroup 热点。
3. 修复 fallback 状态位回归：
   - `SingletonBypass` 和 `dispatchRangeDirect` 不再强制 `dispatchCommon(..., layerChanged=1, stateChanged=1)`；
   - 恢复为与主循环一致的 `layerChanged/stateChanged` 判定，避免无谓状态切换开销。
4. `flushPendingMergedBatch` 内部新增 `stageUpdatesIssued` 计数，保证 `StageUpdate(0)` 总调用数与原语义一致，同时避免 fallback 路径重复调用。
5. 保持 group 规则不变：
   - 同 `meshData`、同 `layerIndex`、同 `layerStatePtr`；
   - 非 `Special(type3)`；
   - `FastStateGen` 一致。
6. 新增“起批前 Outline/Bloom 一致性预检”：
   - 当描边/Bloom 句柄存在时，先比较当前项与下一项的可视状态，再决定是否起批；
   - 避免“起批后立刻拆批 -> SingletonBypass 空转”。

## 本轮结论
- 这轮属于“空转路径清理 + 合批起批纠偏”，不是最终形态。
- 目标是先把 `SingletonBypass` 占比压下来，再继续推进真正的大组实例提交比例。
- 架构侧已完成阴影域 Hook 物理拆分，并补充了 Bridge 侵入式句柄槽快路径框架（默认关闭）。
  详见：`docs/research/war3_render_issues/01_batch_merge/2026-02-21_hook_split_and_bridge_fastpath.md`

## 当前卡点（2026-02-21 夜间）
1. 仍大量落入非单发路径：
   - `canInstance=false`（典型是 VS 为空或未打 instancing patch）会回退到逐对象 `dispatchCommon`。
   - 回退路径仍需要每对象完整引擎调用链，CPU 开销接近未合批。
2. “单发批次”前后的状态一致性检查仍偏重：
   - 当前实现仍有 `Viewport/View/Proj + memcmp` 的合并守卫；
   - 在高调用场景里会放大 `FQ_Dispatch_Opaque` 固定成本。
3. 统计已证明“合批框架生效”，但“真实单发占比”仍不足：
   - 有 `FlushGroup` 计数不等于真正的硬件 instancing 主路径占比。

## 与用户反馈对齐
- 你看到“性能没有提升”是合理现象：当前主要收益被回退路径和状态检查吞掉，尚未达到目标的 `Per-Batch Single Dispatch` 覆盖率。
- 当前不是“没做合批”，而是“合批命中率和单发占比不足”。

## 新增诊断（2026-02-21 深夜）
`[BatchMergeStats]` 已新增关键分原因计数：
- `Merge=accepted/candidates`
- `InstGroups`
- `NoShader`
- `AllocOrPortrait`
- `AppendBreak`

用于直接回答“卡在哪里”：是 shader 未实例化、分配/头像路径回退，还是 append 提前中断。

## 下一步
1. 增加回退分原因统计（`NoVS/VSNotInstanced/AllocFail/StateMismatch/AppendFail`）。
2. 把合批一致性检查切换到 `FastStateGen` 路径，减少每批 `GetViewport/GetTransform/memcmp`。
3. 推进“首实例 dispatch + 其余实例仅写 InstBuf + 单次 FlushBatch”覆盖率，压低 `dispatchCommon` 总调用数。

## 验证建议
1. 开启 `kNativeOptimizationPerfTrackingEnabled`。
2. 固定同一压测地图，记录 20~30 秒窗口。
3. 对比以下指标：
   - `Opt/BatchMerge/SingletonBypass` 总调用与 CPU 占比；
   - `Opt/BatchMerge/FlushGroup` 平均耗时；
   - `FQ_Dispatch_Opaque` CPU 占比与 FPS。
