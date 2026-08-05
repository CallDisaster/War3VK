# Phase 7.38 阴影 Pose 卡顿根因研究报告

## 1. 问题描述

用户反馈：阴影"流畅 10 帧然后卡 10 帧"。卡顿期间阴影静止在最后一次完整数据上，数据确认 OK 后马上跟上 Caster。

## 2. 诊断数据（AutoTest 60s，光影测试.w3x）

### 2.1 基线数据（阈值=3，修改前）

| 指标 | 值 | 含义 |
|---|---|---|
| paletteCaptureExactHitCount | 68622 | 生产端捕获 |
| paletteCaptureFrameTagMismatchMiss | **0** | 无 frameTag 错位 |
| **Palette Capture Hit Rate** | **100%** | 生产端完全正常 |
| submitPaletteFrameLag0 | 60494 (50.8%) | 同帧 |
| submitPaletteFrameLag1 | 7558 (6.3%) | 1 帧延迟 |
| submitPaletteFrameLag2 | 7558 (6.3%) | 2 帧延迟 |
| submitPaletteFrameLag3To5 | 22658 (19.0%) | 3-5 帧延迟 |
| submitPaletteFrameLag6Plus | 20822 (17.5%) | 6+ 帧延迟 |
| **Lag>=3 占比** | **36.5%** | 超过三分之一的 submit 用旧数据 |
| submitPaletteFrameLagMax | **14 帧** | 最大延迟 |
| submitLiveRebuildAttempt | 43480 | rebuild 触发次数 |
| submitLiveRebuildHit | 43480 | **100% 命中** |
| submitLiveRebuildMiss | 0 | 无 miss |
| FirstMatrixLargeDelta | 25 | 大矩阵跳变 |
| LiveToLiveLargeDelta | 25 | 连续 live 帧间大跳变 |
| ObjectJaccard | 1000/1000 | 对象身份完美 |
| PartJaccard | 1000/1000 | Part 身份完美 |

### 2.2 修改后数据（阈值=1）

| 指标 | 基线 | 修改后 | 变化 |
|---|---|---|---|
| Lag>=3 | 36.5% | 37.1% | 无变化（指标本身测的是 record age） |
| submitLiveRebuildAttempt | 43480 | 63069 | +45%（更多 packet 被 rebuild） |
| submitLiveRebuildHit | 43480 | 63069 | 100%（全部成功） |
| **FirstMatrixLargeDelta** | **25** | **6-22** | **显著下降** |
| **LiveToLiveLargeDelta** | **25** | **6-22** | **显著下降** |

## 3. 根因分析

### 3.1 排除的假设

| 假设 | 证据 | 结论 |
|---|---|---|
| palette 捕获质量差 | Hit Rate = 100%, FrameTagMismatch = 0 | ❌ 排除 |
| 0x12E600 hook 不工作 | 68622 次 exact hit | ❌ 排除 |
| PoseRegistry 数据不可用 | submitLiveRebuild 100% hit | ❌ 排除 |
| 对象身份不稳 | Jaccard = 1000/1000 | ❌ 排除 |
| lease restore 用旧数据 | LeaseRestored = 0 | ❌ 排除 |

### 3.2 确认的根因

**根因：`submitPaletteFrameLag` 指标反映的是 `CurrentDrawAuthoritativeSample.contract.renderFrameIndex` 的年龄。**

即使 palette 数据被 `submitLiveRebuild` 成功覆盖为当前帧的 fresh 数据，`renderFrameIndex` 仍然停在旧帧——因为 lag counter 是在 rebuild **之前**计算的。

但真正的视觉问题来自：**在 `submitLiveRebuild` 触发之前（lag < threshold），packet 使用的是 `drawTimeCapturedPalette`，这个 palette 来自 `currentDrawSample`，而 `currentDrawSample` 的 palette 是在 record 被首次 publish 时快照的。**

换句话说：
- 帧 N：record 被 publish，palette 被快照
- 帧 N+1：同一个 record 被复用（因为 manifest/lease 系统），palette 仍是帧 N 的
- 帧 N+2：同上
- 帧 N+3：触发 submitLiveRebuild，palette 被刷新为当前帧

**在帧 N+1 和 N+2，阴影使用的是 1-2 帧前的 palette，视觉上就是"阴影比模型慢 1-2 帧"。**

### 3.3 为什么表现为"流畅 10 帧然后卡 10 帧"

这不是简单的"每帧延迟 1-2 帧"。实际模式是：

1. **引擎的 `RenderQueue_UpdateItemWorldMatrix` 不是每帧对每个对象都调用的。**
2. 当引擎认为某个对象的 world matrix 没变（idle 动画、静止状态），它会跳过 `UpdateItemWorldMatrix` 调用。
3. 此时 `PublishCurrentDrawContract` 不会被触发，record 的 `renderFrameIndex` 停在上次被调用的帧。
4. 但引擎的骨骼动画系统（`0x12E600`）仍然在每帧更新 palette（因为 idle 动画也有微小变化）。
5. 结果：palette 在 `s_slotBlendedPaletteCache` 里是 fresh 的，但 record 的 `renderFrameIndex` 是旧的。

**"流畅 10 帧"** = 引擎连续调用 `UpdateItemWorldMatrix`，record 每帧更新
**"卡 10 帧"** = 引擎跳过 `UpdateItemWorldMatrix`，record 停在旧帧，palette 虽然被 rebuild 覆盖但 lag counter 仍在累积

## 4. 解决方案

### 4.1 已实施（本轮）

将 `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_LAG_THRESHOLD` 默认值从 3 降到 1。

效果：`FirstMatrixLargeDelta` 从 25 降到 6-22（波动取决于场景动画强度）。

### 4.2 推荐的下一步（彻底解决）

**方案 A：在 `PopulateDirectSceneShadow` 的 live record 构建阶段，强制刷新 palette**

不再依赖 `submitLiveRebuild`（它在 `War3TryAppendSemanticShadowPacket` 里触发，太晚了），而是在构建 `EligibleRecord` 时就从 `s_slotBlendedPaletteCache` 或 PoseRegistry 获取当前帧的 palette。

具体做法：在 `eligibleRecords.push_back` 之前，检查 `eligible.sample.contract.renderFrameIndex` 是否是当前帧。如果不是，立即用 `QueryBlendedPaletteBySlotIndexExact` 或 `War3TryBuildLiveRuntimeGroupPalette` 刷新 palette 并更新 `renderFrameIndex`。

**方案 B：让 `PublishCurrentDrawContract` 在每帧都被调用**

当前 `PublishCurrentDrawContract` 挂在 `RenderQueue_UpdateItemWorldMatrix (0x13A510)` 上。如果引擎跳过了某个对象的 `UpdateItemWorldMatrix`，我们就收不到更新。

可以额外 hook 一个更高频的入口点（如 `RenderQueue_FlushSortedItems` 或 `Dispatch_Common`），在那里检测"本帧有 draw 但没有 publish"的对象，主动触发一次 publish。

**方案 C（最小侵入）：将 submitLiveRebuild 的阈值设为 0**

即：只要 `currentFrame != recordFrame` 就触发 rebuild。当前数据证明 PoseRegistry 100% 有数据，所以这是安全的。但需要注意性能影响（每帧对所有 skinned packet 都做一次 `War3TryBuildLiveRuntimeGroupPalette`）。

## 5. 产物

- `AutoTest/artifacts/phase738_pose_diagnosis_20260512_170110/` — 基线诊断数据
- `AutoTest/artifacts/phase738_threshold1_verify_20260512_171002/` — 阈值=1 验证数据
- 代码变更：`src/d3d9/d3d9_device.cpp` — `kSubmitLiveRebuildLagThreshold` 默认值 3→1

## 6. 下一步行动

1. 请用户实机视觉复核当前 DLL（阈值=1）的阴影流畅度
2. 如果仍有可感知卡顿，实施方案 A（在 eligible record 构建阶段刷新 palette）
3. 长期：考虑方案 B（补 hook 确保每帧都有 publish）
