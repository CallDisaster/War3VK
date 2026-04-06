# DXVK-War3 保守合并提交与桥接优化状态（2026-02-14）

## 1. 执行窗口
- 开始时间：`2026-02-14 03:53:28 +08:00`
- 本轮整理时间：`2026-02-14 04:10:59 +08:00`

## 2. 已落实项

### 2.1 保守合并提交（Bridge 侧）
- 文件：`src/d3d9/war3/render/war3_render_exec_batch.cpp`
- 开关：`src/d3d9/war3/core/war3_internal_test_config.h`
  - `kNativeConservativeMergedSubmitEnabled = true`
  - `kNativeConservativeMergedSubmitCacheEntries = 1024`
- 机制：
  - 增加 TLS 级 `SceneNode + Tag -> (resolvedHandle, RenderObjectInfo*)` 直映缓存。
  - 每帧通过 `epoch` 失效，避免跨帧脏数据。
  - 仅复用桥接解析结果，不改变任何 Draw 顺序，不跨 Stage，不跨透明路径。
- 追踪项：
  - `Opt/ConservativeMerge/SceneCacheProbe`
  - `Opt/ConservativeMerge/SceneCacheHit`

### 2.2 Dispatch Common/Special 桥接成本细分
- 文件：`src/d3d9/d3d9_war3_hook.cpp`
- 将单项桥接耗时拆分为三段：
  - Common：
    - `Opt/DispatchCommon/BridgeBegin`
    - `Opt/DispatchCommon/OrigCall`
    - `Opt/DispatchCommon/BridgeEnd`
  - Special：
    - `Opt/DispatchSpecial/BridgeBegin`
    - `Opt/DispatchSpecial/OrigCall`
    - `Opt/DispatchSpecial/BridgeEnd`
- 目的：分离“桥接成本”与“原函数成本”，便于继续定点优化。

### 2.3 Dispatch 半快速路径增强（本轮新增）
- 文件：`src/d3d9/d3d9_war3_hook.cpp`
- 新增开关：`kNativeHookFastPathSkipBridgeForNonWorldTag = true`
- 策略：
  - 当满足 `仅对象追踪` 且 `Tag 已知且非世界对象` 时，直接透传 trampoline。
  - `Tag == Unknown` 不触发该快路，避免误跳过世界对象桥接。
- 新追踪项：
  - `Opt/DispatchCommon/FastNonWorldTagPassThrough`
  - `Opt/DispatchSpecial/FastNonWorldTagPassThrough`

### 2.4 高风险 patch 默认回退（稳定性优先）
- 文件：`src/d3d9/war3/core/war3_internal_test_config.h`
- 默认关闭：
  - `kNativePatchDynamicBypassDispatchHookCalls = false`
  - `kNativePatchSkipOpaqueStageUpdatePerItem = false`
  - `kNativePatchSkipTransparentStageUpdatePerItem = false`
  - `kNativePatchSkipFlushWhenQueueEmpty` 保持 `false`
- 原因：
  - 已验证会触发“暂停时鼠标消失 / 画面错位”等异常，收益不稳定。

### 2.5 Stage 分类一致性修正（和当前实测映射对齐）
- 文件：`src/d3d9/war3/render/war3_render_state.cpp`
- 修改点：
  - `GetStageCategory()` 改为“优先 BatchTag、再回退 Stage”。
  - 与当前实测对齐：`S10=Decorations`、`S11=CUnit`、`S12=范围染色`、`S21=漂浮文字链`。
  - 去除旧逻辑中 `S21 -> PostProcess` 的误导性分类。

## 3. 当前建议测试开关组合（第一轮）
- 建议先保持：
  - `kNativeConservativeMergedSubmitEnabled = true`
  - `kNativeHookFastPathEnabled = true`
  - `kNativeHookFastPathSkipBridgeWhenNoTracking = true`
  - `kNativeHookFastPathSkipBridgeForNonWorldTag = true`
- 建议保持关闭：
  - `kNativePatchDynamicBypassDispatchHookCalls`
  - `kNativePatchSkipOpaqueStageUpdatePerItem`
  - `kNativePatchSkipTransparentStageUpdatePerItem`
  - `kNativePatchSkipFlushWhenQueueEmpty`

## 4. 预期收益与风险
- 预期收益来源：
  - 降低 `ExecBatchProcessor::Begin/End` 在“非世界对象批次”上的无效桥接成本。
  - 降低多层对象连续提交中的重复 `SceneNode -> Registry` 查询成本。
- 风险等级：低到中
  - 保守合并缓存为“同帧复用 + 按 Tag 隔离 + 不改排序”，风险可控。
  - 快路依赖 Tag 精度，`Unknown` 情况已强制回退慢路。

## 5. 构建结果
- 命令：`ninja -C build32`
- 结果：通过（存在历史 warning，但无新增编译错误）。

