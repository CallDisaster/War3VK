# War3 语义渲染旧路径冻结与退役清单

Date: 2026-05-06

## 1. 目的

这份文档专门处理“以后可能不再使用的代码”。

它不要求今天就删除所有旧代码，但要求从今天开始明确：

1. 哪些路径已经不再是主线
2. 哪些路径只允许保留为过渡兜底
3. 哪些试验已经被证明错误，必须保持关闭
4. 未来删除它们的条件是什么

## 2. 状态定义

### Active

仍是主线，允许继续演进。

### Transitional

暂时仍被主线调用，但不允许继续长新功能，只允许：

1. crash guard
2. metrics
3. narrow-scope correctness patch

### Frozen

默认关闭或只保留 emergency gate。

不允许：

1. 新功能
2. 扩大适用范围
3. 作为长期主线依赖

### Rejected

已经验证方向不对，必须保持关闭。

## 3. 冻结/退役清单

| 路径/模块 | 当前状态 | 处理原则 | Removal Gate | 主要文件 |
|---|---|---|---|---|
| 旧 VB/IB snapshot/freeze object shadow 主路径 | Transitional -> Deprecated | 不再作为 object shadow 主生产器；只保留 emergency fallback 能力 | Phase 3 canonical shadow cutover 稳定通过 | `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.*` |
| semantic frame + draw-time patch mixed submit | Transitional | 不再扩大复杂度；当前只允许围绕稳定性做窄修复 | Phase 2 canonical format 建成后退出主线 | `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp` |
| broad `War3TryBuildLiveRuntimeGroupPalette(...)` fallback builder | Transitional | 只保留过渡兜底；不再往里塞新的 correctness 分支 | draw-time producer 能稳定提供 palette 后降级或删除 | `src/d3d9/d3d9_device.cpp` |
| `QueryBlendedPaletteBySlotIndex(...)` slot cache fallback | Transitional | 只作为 draw-time snapshot 缺失时的过渡后备，不可继续升级为长期 palette source | draw-time palette snapshot 稳定后删除 | `src/d3d9/war3/model/war3_model_hook.cpp` |
| static hydrate based static-world experiments | Frozen | 维持默认关闭，不允许重新变成“大而全静态补全主路” | canonical rigid/static producer 建成 | `src/d3d9/war3/core/war3_internal_test_config.h`, `src/d3d9/war3/render/war3_visible_renderables.cpp` |
| failed async end-frame bootstrap build default | Rejected | 保持关闭；除非专门重做 bootstrap architecture，否则不再启用 | 未来有独立 bootstrap scheduler 后才可重评 | `src/d3d9/war3/core/war3_semantic_shadow_gate.cpp`, `src/d3d9/d3d9_device.cpp` |
| legacy unit capture bypass gates 的“功能性扩张” | Frozen | 只保留 kill-switch / compatibility，不再作为 correctness 主方案 | canonical producer 覆盖后整体退场 | `src/d3d9/war3/core/war3_internal_test_config.h`, `src/d3d9/d3d9_device.cpp` |
| native backend prepared-frame prototype 直接扩成主渲染器 | Frozen | 在 canonical contract 建成前，不把 native backend 变成结构性主线 | Phase 6 canonical -> native adapter 完成 | `src/d3d9/war3/shadow/war3_shadow_backend_native_d3d9.*`, `src/d3d9/war3/platform/war3_runtime_bootstrap.*` |

## 4. 立即执行的代码纪律

从本文件落地后开始，默认遵守：

1. 不再给 legacy snapshot/freeze 增加新对象类型支持
2. 不再给 mixed semantic submit 路线新增“再试一个 fallback”
3. 不再把 static hydrate 重新拉成默认开
4. 任何 rejected experiment 只能在专项分支里临时复现，不能回主线默认值

## 5. 可以继续修改的范围

即使是冻结/过渡路径，以下修改仍然允许：

1. 崩溃修复
2. 观测计数
3. 明确的 kill-switch
4. 与主线重构接缝相关的最小注释和包装

## 6. 删除顺序建议

建议按下面顺序收尾，而不是一次大扫除。

1. 先停止继续依赖
2. 再把 consumer 从旧路径切走
3. 再把旧路径降成 emergency-only
4. 最后删除代码

优先删除顺序：

1. broad palette fallback branches
2. mixed submit-time patch branches
3. legacy object shadow snapshot/freeze authority
4. static hydrate trial branches

## 7. 与主计划的关系

本文件是：

1. `semantic_render_takeover_master_plan_2026_05_06.md`
   的配套约束

如果主计划发生阶段推进，本文件必须同步更新：

1. 哪些路径从 `Transitional` 变成 `Frozen`
2. 哪些路径从 `Frozen` 变成 `Removed`
3. 哪些 rejected experiment 已彻底退出主仓默认配置
