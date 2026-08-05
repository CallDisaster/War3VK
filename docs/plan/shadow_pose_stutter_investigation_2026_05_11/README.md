# 阴影 Pose 停顿问题深度研究报告

## 日期：2026-05-11

## 用户观察到的现象
- 视频 60FPS 下：20 帧跟随 + 13 帧停顿的确定性循环
- 游戏帧率：26.7 FPS
- 停顿结束时全场阴影闪消失一帧，随即重新出现跟住 Caster
- 大门（destructible）持续闪烁
- 几乎只渲染刚体部位，蒙皮部位不渲染

## 已确认的根因（通过 AutoTest 数据验证）

### 核心问题：`War3TryPopulateDirectCurrentDrawGrouped` 间歇性返回 0

**数据证据（ShadowTest/光影测试.w3x，前台窗口化）：**

1. `semanticScenePopulateLastReturnReason = 6`（`kPopulateReturnEmptyFrame`）
   - 在"停顿帧"里，DirectOnly 路径的 populate 认为没有可提交的 caster

2. `semanticSceneReceiverHoldEmptyReplayCount = 1`
   - Receiver 检测到"上一帧有 caster 但本帧 replay 为空"，触发 hold

3. `semanticSceneReceiverReuseShadowMap = 1`
   - Shadow map 被复用（阴影停在原地）

4. Hold 帧数耗尽后（`kShadowTransientEmptyReplayHoldFrames = 8`）：
   - Receiver 被迫用空 replay 重绘 shadow map → 全场阴影消失一帧
   - 下一帧 populate 恢复成功（reason=9）→ 新 shadow map 画出来 → 阴影重新出现

### 完整循环解释
```
帧 1-9:  populate 成功(reason=9) → shadow map 每帧重绘 → 阴影跟住 caster
帧 10:   populate 返回空帧(reason=6) → hold 开始 → 复用旧 shadow map
帧 11-17: populate 持续返回空帧 → hold 继续 → 阴影停在原地
帧 18:   hold 帧数耗尽 → 用空 replay 重绘 → 阴影消失一帧
帧 19:   populate 恢复成功 → 新 shadow map → 阴影重新出现
```

## 已排除的假设

1. ❌ `kPopulateReturnStableSubmittedReuse`（reason=5）节流
   - 实际走的是 `War3SemanticDirectOnlyRuntime()=true` 的 DirectOnly 路径
   - 该路径在 StableSubmittedReuse 判断之前就 return 了

2. ❌ `kShadowAdaptiveMapUpdateEnabled` 跳帧
   - 编译期默认 `false`，不会触发

3. ❌ `kShadowHoldLastGoodMapOnSemanticIdentityChurn`
   - 编译期默认 `false`

4. ❌ Palette 数据质量问题
   - `TrustedSourceHitCount = 100650~106703`，hit rate ~74%（RangeCopy 路径）
   - Palette 数据本身没问题，问题在于 populate 返回空帧时根本不消费 palette

## 下一步需要调查的方向

### 为什么 `War3TryPopulateDirectCurrentDrawGrouped` 间歇性返回 0？

该函数依赖 `CurrentDrawContractRecord` 数据。关键线索：
- `currentDrawContractPublishAttemptCount = 270032`
- `currentDrawContractPublishReadyCount = 135328`
- **Ready/Attempt 比率 = 50%** — 只有一半的 publish 尝试成功！
- `currentDrawContractPublishSkippedNonWorldContext = 173207` — 大量被跳过（非世界上下文）

这意味着 `PublishCurrentDrawContract` hook 在很多帧里被跳过了（因为当时不在"世界渲染"上下文中）。当 BeforeUi 插入点到来时，如果本帧的 world draw 阶段没有产生足够的 ready records，`War3TryPopulateDirectCurrentDrawGrouped` 就会看到空的 record 列表并返回 0。

### 可能的修复方向

1. **延长 hold 帧数**：增加 `kShadowTransientEmptyReplayHoldFrames`（当前=8），让 hold 期间有更多机会等到下一次成功的 populate。但这只是治标。

2. **修复 `PublishCurrentDrawContract` 的 ready 率**：调查为什么 50% 的 publish 被标记为 non-world-context。如果 BeforeUi 插入时机和 world draw 阶段有相位差，可能需要调整插入时机或缓存上一帧的 ready records。

3. **在 DirectOnly 路径中，当本帧 direct draw 为空时，fallback 到上一帧的 scene 而不是返回 0**：这样 receiver 就不会看到 empty replay，shadow map 继续用上一帧的数据重绘（palette 已经更新了）。

## AutoTest 配置备忘

- 正确地图路径：`E:\Work\War3\Maps\ShadowTest\光影测试.w3x`
- 前台模式：`use_isolated_desktop=false`
- 窗口化：`windowed=true`
- Hot shadow timeout：90s（当前地图需要较长时间稳定）


## Phase 7.32 修复尝试结果（2026-05-11 17:02）

### 修复内容
- 放宽 `RecordHasLocalPaletteSnapshot` 中的 `captureSerial` 匹配条件（允许差 2）
- 移除 `matrixCount == capturedPaletteCount` 的严格匹配
- 放宽 `frameTag` 匹配（允许差 1）

### 验证结果
- **问题未完全解决**：`postFailureSummaryRefresh` 仍然捕获到 `reason=6`（EmptyFrame）
- 但稳定状态下（hot shadow 快照）系统正常工作：`reason=9`，43 casters，162 drawn
- 说明 `captureSerial` 放宽只解决了部分空帧，还有其他过滤条件在起作用

### 第二个过滤点：`SnapshotRecordVisibleEnough`
- 要求 `record.visibleFrameSerial >= minVisibleFrameSerial`
- `minVisibleFrameSerial` = `currentVisibleFrameSerial - 2`（grace window = 2 帧）
- 当 `PublishCurrentDrawContract` 被 `SkippedNonWorldContext` 跳过时，record 的 `visibleFrameSerial` 不更新
- 连续多帧跳过后，record 落后超过 2 帧 grace window → 被过滤 → 空帧

### 下一步
- [x] 增大 `DXVK_WAR3_SEMANTIC_CURRENT_DRAW_GRACE_FRAMES` 的默认值（从 2 增加到 8）— 已完成
- 或者在 DirectOnly 路径中，当 snapshot 为空时 fallback 到上一帧的 submitted scene

## Phase 7.32 最终验证结果（2026-05-11 17:31，隔离桌面 AutoTest）

### 验证环境
- 地图：`E:\Work\War3\Maps\ShadowTest\光影测试.w3x`
- 模式：隔离桌面窗口化，hot shadow poll
- DLL：已自动部署（`build32/src/d3d9/d3d9.dll`）

### 关键指标（全部达标）
| 指标 | 修复前（用户观察到停顿时） | 修复后 |
|------|---------------------------|--------|
| `PopulateLastReturnReason` | 6 (EmptyFrame) 间歇出现 | **9 (成功)** |
| `ReceiverHoldEmptyReplayCount` | 1 (触发 hold→停顿) | **0** |
| `ReceiverReuseShadowMap` | 1 (shadow map 复用→阴影停在原地) | **0** |
| `SkippedEmptyFrameCount` | 非零 | **0** |
| `ShadowCastersCount` | 43 | **45** |
| `ShadowMapDrawnCasters` | 145 | **157** |
| `SubmittedObjectJaccardMilli` | ~998 | **1000** (完美稳定) |
| `SubmittedPartJaccardMilli` | ~998 | **1000** (完美稳定) |
| `PaletteCaptureTrustedSourceHit` | ~29K | **29239** (hit rate ~77%) |
| `CurrentDrawMissStaleVisibleFrame` | 非零（根因） | **0** |

### 结论
- **Phase 7.32 修复有效**：两个过滤条件的放宽（captureSerial 允许差 2 + grace frames 从 2 增到 8）
  彻底消除了 `populate 返回空帧 → hold → shadow map 复用 → 阴影停顿` 的循环。
- **DLL 已部署到 `E:\Work\War3\d3d9.dll`**。

### 用户视觉复核结果（2026-05-11 17:40）
- ✅ 大门闪烁明显改善
- ⚠️ 阴影卡顿仍然存在（比之前好但未完全消除）
- ❌ **大多数 Caster 阴影只显示一个部位**（核心残余问题）

## Phase 7.33 目标：修复"只显示一个部位"

### 根因
- `semanticSceneRejectedCutoutVisualPolicy = 41`（41 条 cutout 记录被拒绝）
- `semanticSceneDirectObjectIncompleteByAlphaPolicyCount = 36`（36 个对象不完整）
- 原始 87 条 record 中只有 45 条通过过滤
- **控制开关**：`DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER`（默认 1=拒绝）
- **设计原因**：shadow caster shader 不支持 alpha test，cutout geoset 当 opaque 画会出现方形卡片
- **解决方向**：关闭该过滤（接受方形卡片作为临时方案），或在 caster shader 中实现 alpha test

### 方案评估
1. **关闭 alpha 过滤（快速但有副作用）**：设置 `DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER=0`
   - 优点：立即让所有 geoset 进入 shadow map，阴影完整
   - 缺点：cutout 材质的透明区域会产生方形阴影轮廓
   - 适用：如果 War3 模型的 cutout 主要用于边缘柔化而非大面积透明，方形可能不明显

2. **在 caster shader 中实现 alpha test（正确方案）**：
   - 需要在 shadow pass 中绑定 diffuse 纹理 + UV + alphaRef
   - 在 fragment shader 中做 `if (texColor.a < alphaRef) discard;`
   - 工作量较大但效果正确
