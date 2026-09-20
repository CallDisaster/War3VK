# 2026-09-17 — 活跃路径 palette 来源仪表化工单（方案稿，**未落地**）

> **用途**：把"合同 ON（编译默认）下活跃生产路径上没有任何 palette 来源仪表"这一结构性缺口，
> 转成一份可执行、可裁定、可静态门禁的仪表化方案。
> **状态**：本文**只出方案，不落地**。文中所有"建议计数器名"均为**建议**，不是既有事实。
> **纪律**：
> - **接线 ≠ 已度量**。"已接线"只表示字段能出现在报告出口；"已度量"必须由实机**非零样本** +
>   对象级关联共同证明。
> - 不确定一律写"待验证"；不得把"建议接线"写成"已实现"或"已观测"。
> - **行号会漂移**：本文一律以**函数名或唯一子串**为锚；行号仅为写作时快照，用于辅助定位，
>   不得作为唯一依据引用。
> - 本文不改变任何判定、阈值、门控或发布语义；只描述"观测"。

---

## 0. 范围与禁止项

### 0.1 范围

1. 本文档**新增且仅新增本文件自身**：
   `docs/plan/2026-09-17-active-path-palette-instrumentation-workorder.md`。
2. 本文定义"在**活跃、可达**路径上补齐 palette 来源 / 漏斗（funnel）仪表"的点位、接线、验收口径、
   风险与静态门禁。
3. 覆盖对象：`D3D9DeviceEx` 的 semantic append 主链（策略门 → 索引切片 → current-draw resolve →
   canonical 就绪门 → 分类块）与两条 **draw-time** 真实 skinned 提交点。

### 0.2 禁止项（本工单执行阶段同样适用，除非上级另行裁定）

- 禁止修改任何源码 / 测试 / meson（本阶段**连仪表本身都不落地**）。
- 禁止构建、部署、启动游戏、覆盖现场 DLL、执行任何 git 写操作。
- 禁止改变任何判定分支：不得把 `return false` 改为可继续、不得新增/放宽/收紧门控、
  不得改变 caster 选择、剔除、材质策略、发布次序或帧语义。
- 禁止把"只加计数"当成"已修复"或"已排除错误矩阵"。
- 禁止以"拒绝计数上升"作为"合法对象仍有替代路径/失败后可恢复"的证明（见 §4）。
- 禁止把单帧采样值 0 或分段 Δ 当作整轮累计事实（见 §4.4 口径）。

### 0.3 术语与既有事实边界

| 术语 | 含义 | 本文状态 |
| --- | --- | --- |
| 合同 ON | `dxvk::war3::render::skin::ContractEnabled() == true` | 见 §8 锚点 A1；某候选构建的**编译默认** |
| canonical 就绪门 | `if (!canonicalItem.readyForShadowConsumer()) { ...; return false; }` | 见 §8 锚点 A8 |
| 分类块（8 桶） | `semanticSceneSubmittedSkinnedPaletteSource*` 的 7 个来源桶 + 1 个来源 churn 桶 | 见 §8 锚点 A9/A9b |
| 当帧值 | `m_war3Scene.shadowStats` 每帧整体重建，快照标量是该帧值 | 见 §8 锚点 A16/A17/A18 |

---

## 1. 结论摘要：为什么现有仪表无法度量这两项证明

### 1.1 现有来源仪表全部挂在"合同 ON 下结构性不可达"的代码之后

append 主链在 `D3D9DeviceEx::War3TryAppendSemanticShadowPacket`（四参重载，
唯一子串 `bool currentFrameExactOwnerPrefiltered`）里顺序如下（函数名/唯一子串为锚）：

1. `ShadowProducerPolicyAllows(...)` 策略门 → 不通过即 `return false`。
2. path blocker 门 → 不通过即 `return false`。
3. 几何/切片/current-draw resolve 各阶段。
4. `noteCanonicalReadiness()` → `semanticSceneCanonicalReadyCount` / 各 `RejectReason` 计数。
5. **canonical 就绪门**：`if (!canonicalItem.readyForShadowConsumer()) { ...; return false; }`。
6. **分类块**：`if (skinned && War3SemanticPaletteDiagnosticsRuntime()) { ... }`，
   内含 7 个来源桶 + 1 个来源 churn 桶。
7. 提交计数：`m_war3Scene.shadowStats.semanticSceneSubmittedSkinned++`。

即：**来源仪表（第 6 步）与 semantic 提交计数（第 7 步）都排在第 5 步的硬门之后**。
在"合同 ON"这一编译默认配置下，第 5 步对**全部** packet 返回 false（就绪计数 0），
因此第 6、7 步整段在 ON 生产路径上是**结构性死代码**：它们不是"观测到 0"，
而是"根本没被执行"。

### 1.2 真实 skinned 提交在 draw-time 路径，而两条 draw-time 路径完全没有来源归属

`semanticSceneSubmittedSkinned` 在树内只有 3 个自增点：

| # | 位置（函数名 + 唯一子串） | 是否在 canonical 就绪门之后 | 本工单归类 |
| --- | --- | --- | --- |
| 1 | `War3TryAppendSemanticShadowPacket`（第 7 步） | 是（ON 下不可达） | 死代码 |
| 2 | `War3TryPopulateDrawTimeSemanticProducer`（`NoteShadowAppendRawcode(entry.rawcode)` 之后） | 否（draw-time 独立路径） | **活跃** |
| 3 | `War3TryPopulateDirectCurrentDrawGrouped` 的 fast-append 发布段（`finishFastAppend(FastAppendOutcome::Success, true)` 之前） | 否（draw-time 独立路径） | **活跃** |

**锚点校正（必须随引用声明）**：`War3ActivateDrawTimeCacheEntry`（唯一子串
`m_war3DrawTimeActiveLedger.activate(`）**本身只做 ledger 激活，不自增任何提交计数**；
它被 `War3TryCaptureShadowCaster` 在 capture 段调用两次。因此"两条 draw-time 提交点"的准确函数名
是上表 #2 `War3TryPopulateDrawTimeSemanticProducer` 与 #3 `War3TryPopulateDirectCurrentDrawGrouped`
（fast-append 发布段），**不是** `War3ActivateDrawTimeCacheEntry`。
本文以函数名锚点为准；旧记录中的函数名指认需按本节校正。

结论：**在合同 ON 的生产路径上，"合法对象仍有替代路径"与"失败后可恢复"没有任何可采样的仪表**。
这不是事件没发生，而是计数器不在可达路径上。这解释了实机三轮中
`semanticSceneSubmittedSkinned` 在 ON 轮仍非零（max 52 / 336）而全部来源桶恒 0 的现象：
非零来自上表 #2/#3 的 draw-time 提交，来源桶的死代码在 #1 之后。

### 1.3 即便门打开，来源桶还额外被一个默认关闭的运行时门挡住

分类块自身还有一层 `War3SemanticPaletteDiagnosticsRuntime()`
（环境变量 `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS`，**默认 0**）。
即"合同 OFF"只是必要条件，还需显式打开诊断开关，分类桶才会出数。
**因此新增仪表不得再挂在同一个默认关闭的运行时门之后**，否则本工单解决的缺口会原样复现（见 §2.5、§4.5）。

### 1.4 三项证明的"度量缺口"落点

| 证明 | 现状缺口 | 本工单补什么 | 本工单**不**能补什么 |
| --- | --- | --- | --- |
| ① 错误矩阵不再被使用 | 无对象级关联；ON 轮无对照仪表 | 在活跃路径补来源分布（替代来源 vs 危险来源） | 不能单独证明"被拒的那次读取原本会读到别的对象矩阵" |
| ② 合法对象仍有替代路径 | 来源桶结构性恒 0 | draw-time 提交点的来源正向计数 | 不能替代同对象关联 |
| ③ 失败后可恢复 | 只有趋势（拒绝增量下降） | 拒绝（死在门）与替代来源提交的当帧共现 | 不能证明"同一被拒对象在后续帧重新命中" |

---

## 2. 精确点位清单（函数名 + 唯一子串锚定）

**通用约定**（适用于本节全部点位）：

- **类型**：除特别注明外，一律为 `uint32_t`，放在 `m_war3Scene.shadowStats`
  （即 `d3d9_war3_scene.h` 中 `War3ShadowCaptureStats`，先例 `semanticSceneSubmittedSkinned` 字段）
  ⇒ **当帧值**，非进程累计。理由与先例一致（§1.3、§8 锚点 A9/A10）。
- **自增形式**：普通 `++`（与先例 `m_war3Scene.shadowStats.semanticSceneSubmittedSkinned++` 一致）；
  这些点位当前已在**无锁**地写 `m_war3Scene.shadowStats`，新计数沿用同一写入模式。
  若实施时实机证实某点会跨线程并发写同一帧统计，再降级为 `std::memory_order_relaxed` 原子
  （**待验证**，见 §5.1）。
- **不改变控制流**：所有自增必须插在**既有分支之外/already-taken 的路径上**，不得引入新的
  `if` 影响 `return` 值或后续判定。

### 2.1 点位 ①：策略门之后（append 入口漏斗分母）

- **建议计数器名**：`semanticSceneAppendEntrySkinnedCount`
- **自增位置**：`D3D9DeviceEx::War3TryAppendSemanticShadowPacket`
  （四参重载，唯一子串 `bool currentFrameExactOwnerPrefiltered`），
  紧接 `const bool skinned = packet.path == dxvk::war3::shadow::ShadowDrawPath::Skinned;`
  之后（唯一子串 `ShadowDrawPath::Skinned;`）。
- **条件**：`if (skinned)`。该点已在 `ShadowProducerPolicyAllows(...)` 策略门与
  `War3PacketIsPathBlocker(packet)` 之后，因此是"策略门之后"的 skinned 入口分母。
- **类型**：`uint32_t` 当帧值。
- **回答的问题**：本帧到底有多少 skinned packet 进入了 append 主链？（后续所有漏斗计数的分母）
- **备注**：放在 `skinned` 计算处而不是更早，是为了让 ① ≥ ② ≥ ③ ≥ ④ 单调可比。
  更早的点位（策略门之前）**建议不要加**：它会把被策略门拒绝的包混进分母，破坏漏斗含义。

### 2.2 点位 ②：skinned 索引切片之后

- **建议计数器名**：`semanticSceneAppendAfterIndexSliceSkinnedCount`
- **自增位置**：`War3TryAppendSemanticShadowPacket` 内，
  `if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneRequireVisibleIndexSliceForSkinned)`
  块与其后的 `if (skinned) { ... semanticSceneSkinnedDynamicIndexSliceCount++ ... }` 块**之后**
  （唯一子串 `semanticSceneSkinnedFullIndexFallbackLastIndexCount`）。
- **条件**：`if (skinned)`，无其他附加门（**不得**放在 `War3SemanticRequireVisibleIndexSliceForSkinnedRuntime()`
  为真的分支里；该门默认 1，但显式置 0 的对照轮仍需有分母）。
- **类型**：`uint32_t` 当帧值。
- **回答的问题**：有多少 skinned 包通过了索引切片门？①−② 即"死在切片/几何"的规模，
  可与既有 `semanticSceneSkinnedMissingVisibleIndexSliceRejectCount` 对账。

### 2.3 点位 ③：current-draw resolve 之后

- **建议计数器名**：`semanticSceneAppendAfterCurrentDrawResolveSkinnedCount`
- **自增位置**：`War3TryAppendSemanticShadowPacket` 内，
  `switch (currentDrawResolveStatus) { ... }`（其 `Ready` 分支自增
  `semanticSceneCurrentDrawResolveReadyCount`）所在 `if (skinned)` 块**之后**，
  紧接 `const bool authoritativeGroupSlotsReady =` 之前
  （唯一子串 `authoritativeGroupSlotsReady`）。
- **条件**：`if (skinned)`。
- **类型**：`uint32_t` 当帧值。
- **回答的问题**：有多少 skinned 包在 current-draw resolve 之后仍然存活？
  ②−③ 即"死在 resolve/契约"的规模，可与既有
  `semanticSceneCurrentDrawMissNoContract` / `MissNoPalette` / `MissNoGroupSlots` /
  `MissStaleVisibleFrame` 四个既有 miss 桶对账。
- **备注**：`semanticSceneCurrentDrawResolveReadyCount` 与
  `semanticSceneCurrentDrawResolveReadyRejectedCount` 已存在（后者在 canonical 门内自增），
  本点位**不重复**它们，只补"过门后仍存活"的当帧分母。

### 2.4 点位 ④：canonical 就绪门（就绪分支 + 拒绝分支，拒绝侧 **skinned-only**）

#### ④-a 就绪分支

- **建议计数器名**：`semanticSceneCanonicalReadySkinnedCount`
- **自增位置**：`noteCanonicalReadiness` lambda 内，
  `if (canonicalItem.readyForShadowConsumer()) {` 块（唯一子串 `semanticSceneCanonicalReadyCount++`）
  内、紧随既有 `semanticSceneCanonicalReadyCount++`。
- **条件**：`if (skinned)`（与既有就绪计数区分）。
- **类型**：`uint32_t` 当帧值。
- **回答的问题**：canonical 门对 skinned 包是否**打开过**？（合同 ON 下预期恒 0；
  这是"门是否已经打开"的判决位）

#### ④-b 拒绝分支（**skinned-only**）

- **建议计数器名**：`semanticSceneCanonicalGateRejectSkinnedCount`
- **自增位置**：`if (!canonicalItem.readyForShadowConsumer()) { ...; return false; }`
  （先例 `semanticSceneCurrentDrawResolveReadyRejectedCount++`）块内，在既有
  `if (currentDrawResolveStatus == ...::Ready)` 判断**之前或之后**均可
  （建议紧随 `{` 之后，确保所有拒绝都被计入）。
- **条件**：`if (skinned)`，**与 `currentDrawResolveStatus` 无关**。
- **类型**：`uint32_t` 当帧值。
- **回答的问题**：有多少 skinned 包**死在 canonical 门**上？
  这是区分"内容死在门"（④-b > 0）与"内容没到门"（① > 0 但 ④-a + ④-b == 0）的关键判据。

#### ④-c 拒绝原因（skinned-only，主因）

- **建议计数器名**：`semanticSceneCanonicalGateRejectSkinnedNoWorldTransformCount`
- **自增位置**：`noteCanonicalReadiness` lambda 的
  `case CanonicalShadowReadinessReason::NoWorldTransform:` 分支
  （唯一子串 `semanticSceneCanonicalRejectNoWorldTransform++`）内。
- **条件**：`if (skinned)`，与既有全局 `semanticSceneCanonicalRejectNoWorldTransform` 并行自增
  （**不改**后者语义与数值）。
- **类型**：`uint32_t` 当帧值。
- **回答的问题**：skinned 拒绝里有多少是 `NoWorldTransform`（实机四轮观测到的同一序列主因）？
- **备注**：其余 `readinessReason` 分支是否需要 skinned-only 细分，**待验证**——
  建议第一阶段只补 `NoWorldTransform` 一项；若 ④-b ≫ ④-c，再按需要追加。

### 2.5 点位 ⑤：分类块入口

- **建议计数器名**：`semanticScenePaletteClassifyEntrySkinnedCount`
- **自增位置**：`War3TryAppendSemanticShadowPacket` 内，
  **在** `if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {`（分类块，先例
  `semanticSceneSubmittedSkinnedPaletteSourceNoneCount++`）**之前**，紧邻插入一句独立的
  `if (skinned) m_war3Scene.shadowStats.semanticScenePaletteClassifyEntrySkinnedCount++;`。
- **条件**：`if (skinned)`；**禁止**附加 `War3SemanticPaletteDiagnosticsRuntime()`，
  否则该计数器在该 env 关闭时同样恒 0（§1.3）。
- **类型**：`uint32_t` 当帧值。
- **回答的问题**：分类块**入口**是否可达？
  ④-a + ④-b 与 ⑤ 的差 = "过门后但在分类块前被丢弃"（例如 alpha/材质/几何策略拒绝，
  既有 `semanticSceneRejectedAlphaBlendVisualPolicy` 等可对账）；
  ⑤ > 0 而所有 8 桶为 0 = "到了分类块但桶没记"（仪表缺陷，非路径缺失）。

### 2.6 点位 ⑥：draw-time 两个提交点的来源归属（镜像分类 8 桶）

两个活跃提交点（§1.2 表 #2/#3）：

| 站点 | 函数 | 唯一子串（自增位置附近） |
| --- | --- | --- |
| ⑥-a | `War3TryPopulateDrawTimeSemanticProducer` | `NoteShadowAppendRawcode(entry.rawcode);` → `semanticSceneSubmitted++` → `semanticSceneSubmittedSkinned++` |
| ⑥-b | `War3TryPopulateDirectCurrentDrawGrouped`（fast-append 发布段） | `Phase 7.108b：survey rawcode（fast-append 路径）。` / `finishFastAppend(FastAppendOutcome::Success, true)` |

#### ⑥-1 每站点分母

- `semanticSceneDrawTimeProducerSubmittedSkinnedCount`（站点 ⑥-a）
- `semanticSceneDrawTimeFastAppendSubmittedSkinnedCount`（站点 ⑥-b）
- 自增位置：各自在既有 `m_war3Scene.shadowStats.semanticSceneSubmittedSkinned++;` 的**同一分支内**、
  紧邻其后。
- 条件：站点自身既有条件（⑥-a：`objectKind == ObjectKind::Unit`；
  ⑥-b：fast-append 成功发布路径）。
- 类型：`uint32_t` 当帧值。
- 回答的问题：真实 skinned 提交分别由哪条 draw-time 路径产生？（代替目前两个站点合并计数）

#### ⑥-2 镜像来源桶（8 桶，两站点共用同一族）

建议名（与既有 `semanticSceneSubmittedSkinnedPaletteSource*` 命名同族，前缀
`semanticSceneDrawTimeSubmittedSkinnedPaletteSource`）：

1. `...SourceNoneCount`
2. `...SourceDrawTimeCapturedCount`
3. `...SourceSubmitTimeGlobalSlotCount`
4. `...SourceSubmitTimeBlendedCacheCount`
5. `...SourceSubmitTimePublishedRegistryCount`
6. `...SourceSubmitTimeCModelFallbackCount`
7. `...SourceOwnedPartSnapshotCount`
8. `...SourceUnknownOrOtherCount` ← **残差桶（新增）**

- 自增位置：两个站点各自在 ⑥-1 分母自增之后。
- 条件：站点自身的来源判定结果（见下方**待验证**）。
- 类型：`uint32_t` 当帧值。
- 回答的问题：**真实提交的 skinned caster 用的是哪条来源？**
  这是"合法对象仍有替代路径"唯一的**正向**证据位。
- **与语义侧 8 桶的差异（必须写明）**：语义侧第 8 个是
  `semanticSceneSubmittedSkinnedPaletteSourceChurnCount`（依赖 8192 项 `thread_local`
  开窗查找表的帧间稳定性探针）。该探针**明确不镜像**到 draw-time（§5.3 成本），
  改以**零成本残差桶** `...SourceUnknownOrOtherCount` 占第 8 位。
  因此两族第 8 桶**语义不同**，报告解读时不得直接对等。
- **待验证（关键设计缺口，不得写成已实现）**：`War3DrawTimeVBEntry`
  （`d3d9_device.h`，唯一子串 `struct War3DrawTimeVBEntry`）当前**没有任何 palette/来源字段**，
  也没有矩阵 palette 字节——它的蒙皮语义来自已捕获的（可能 GPU-skin lease 背书的）顶点数据。
  因此"来源桶"必须由 draw-time 既有证据**派生**，候选映射（**全部待验证**）：
  - `entry.gpuSkinLeaseBacked == true` → `SourceOwnedPartSnapshotCount`（GPU-skin lease 归入
    owner 发布一侧；是否单列 lease 桶由上级裁定，见 §7 Q6）；
  - fast-append `consume hit` 且走 generation-backed 复用证明 → `SourceDrawTimeCapturedCount`；
  - 其余 → `SourceUnknownOrOtherCount`。
  实施前必须先只读核实：这两个站点在自增处**能否拿到**上述证据字段，以及
  `semantic` fallback 分支（若存在）是否会把包再送回 §2.1–§2.5 的主链
  （合同 ON 下会死在 canonical 门 ⇒ 不应把 fallback 记为"替代来源"）。

### 2.7 点位总览（漏斗单调性）

```
① AppendEntrySkinned                     ← 策略门之后
   └─ ② AppendAfterIndexSliceSkinned      ← 切片门之后
        └─ ③ AppendAfterCurrentDrawResolveSkinned   ← resolve 之后
             ├─ ④-a CanonicalReadySkinned     （门打开）
             └─ ④-b CanonicalGateRejectSkinned（门拒绝；④-c 细分 NoWorldTransform）
                  └─ ⑤ PaletteClassifyEntrySkinned   ← 分类块入口（不挂诊断 env）
                       └─ [既有 8 桶 + 既有 semanticSceneSubmittedSkinned]

draw-time 独立支路（不经过上面任何门）：
  ⑥-a DrawTimeProducerSubmittedSkinned ─┐
  ⑥-b DrawTimeFastAppendSubmittedSkinned ┴→ 共享 8 个 DrawTimeSubmitted…Source*Count
```

---

## 3. 完整接线清单（按 `PaletteSourceChurnCount` 先例）

### 3.1 先例核实：每个 `shadowStats` 字段的完整出口

用 `PaletteSourceChurnCount`（全名 `semanticSceneSubmittedSkinnedPaletteSourceChurnCount`）
在树内 grep，共命中 19 行，落在 **8 个导出目的地 / 11 个文字编辑位置**：

| # | 导出目的地（文件） | 先例位置 | 内容 |
| --- | --- | --- | --- |
| L1 | `src/d3d9/d3d9_war3_scene.h` | `:1468`（声明区 `:1436-1471`） | 当帧字段 `uint32_t ... = 0;` |
| L2 | `src/d3d9/war3/render/war3_shadow_runtime_bridge.h` | `:882`（同族 `:873-882`） | bridge summary `uint64_t ... = 0;` |
| L3 | `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp` | `:6746-6748` | `summary.X = g_shadowSceneStats.X;` |
| L4 | `src/d3d9/war3/tools/war3_diagnostics_hub.h` | `:544`（同族 `:535-544`） | hub summary `uint64_t ... = 0;` |
| L5 | `src/d3d9/war3/tools/war3_diagnostics_hub.cpp` | `:2032-2033`（同族 `:2008-2033`） | `summary.X = bridgeSummary.X;` |
| L6 | `src/d3d9/war3/tools/war3_diagnostics_hub.cpp` | `:3241-3243`（同族 `:3222-3249`） | JSON 键值对 `{"X", snapshot.shadow.X}` |
| L7 | `src/d3d9/war3/tools/war3_control_plane.cpp` | `:2322-2323`（同族 `:2300-2327`） | JSON 键值对 `{"X", summary.X}` |
| L8 | `src/d3d9/war3/tools/war3_perf_monitor.h` | `:1015`（同族 `:1006-1020`） | agg 结构 `uint64_t ... = 0;` |
| L9 | `src/d3d9/war3/tools/war3_perf_monitor.cpp` | `:1914-1915`（同族 `:1895-1921`） | `agg.X += stats.X;` |
| L10 | `src/d3d9/war3/tools/war3_perf_monitor.cpp` | `:6917-6919`（写手 A） | JSON `"...X": shadowAgg.X` |
| L11 | `src/d3d9/war3/tools/war3_perf_monitor.cpp` | `:8473-8475`（写手 B） | JSON `"...X": shadowAgg.X` |

说明：
- **L1 与 L2/L4/L8 的整型宽度不同**（当帧 `uint32_t` vs 导出 `uint64_t`）是先例既有形态，
  新字段照抄，不要"统一"。
- L6/L7/L10/L11 是 **JSON 键名**；键名必须与字段名逐字一致（静态门禁会钉死，见 §6）。
- 每个字段 **11 个编辑位置**。用户口径中的"8 处"= 上表 8 个目的地（L8–L11 同属 perf monitor 一族）。

### 3.2 全量字段规模

| 分组 | 字段数 | 编辑位置 |
| --- | ---: | ---: |
| §2.1–§2.5 漏斗（①/②/③/④-a/④-b/④-c/⑤） | 7 | 77 |
| §2.6 站点分母（⑥-1 ×2） | 2 | 22 |
| §2.6 镜像来源桶（⑥-2 ×8） | 8 | 88 |
| **合计（全量）** | **17** | **187** |

⇒ 全量 17 × 11 = 187 处文字编辑，跨 7 个文件。**不建议一次性落地**。

### 3.3 最小可行子集（MVP，建议先行）

**MVP-1（4 个字段 × 11 = 44 处编辑）——本次工单建议的唯一批准范围**：

| 字段 | 点位 | 为什么最划算 |
| --- | --- | --- |
| `semanticSceneAppendEntrySkinnedCount` | ① | 唯一能给出"ON 生产路径 skinned 漏斗分母"的字段；没有它，所有下游计数无法解释 |
| `semanticSceneCanonicalGateRejectSkinnedCount` | ④-b | 唯一能区分"内容死在门"与"内容没到门"的字段；直接量化死代码规模 |
| `semanticSceneDrawTimeProducerSubmittedSkinnedCount` | ⑥-a | 第一条真实提交路径的分母 |
| `semanticSceneDrawTimeSubmittedSkinnedPaletteSourceDrawTimeCapturedCount` | ⑥-2 之一 | **唯一的正向证明位**：合法对象经 draw-time 替代来源投影。缺它则证明②仍然未覆盖 |

MVP-1 能一次性回答四个问题：
1. ON 下 skinned 包到达 append 入口的数量（分母非零即"路径活跃，只是没仪表"）；
2. ON 下死在 canonical 门的 skinned 数量（预期非零，量化 §1.1 的死代码）；
3. 真实 draw-time 提交量（与既有 `semanticSceneSubmittedSkinned` 对账）；
4. 其中经 `DrawTimeCaptured` 来源提交的量（**证明②的第一个正向证据**）。

**MVP-2（在 MVP-1 通过实机后追加，+4 字段）**：
`semanticSceneAppendAfterIndexSliceSkinnedCount`（②）、
`semanticSceneAppendAfterCurrentDrawResolveSkinnedCount`（③）、
`semanticScenePaletteClassifyEntrySkinnedCount`（⑤）、
`semanticSceneDrawTimeFastAppendSubmittedSkinnedCount`（⑥-b）。

**Phase 3（视裁定再定）**：④-a、④-c，以及 ⑥-2 的其余 7 个来源桶。

> **注意**：MVP-1 中的 `...SourceDrawTimeCapturedCount` 依赖 §2.6 的**来源映射（待验证）**。
> 若实施前只读核实发现 draw-time 站点拿不到任何可分辨来源证据，
> 则该字段应退回为 `semanticSceneDrawTimeSubmittedSkinnedCount`（纯分母），
> 并在文档中明确"来源归属未覆盖"，**不得**用分母冒充正向证据。

---

## 4. 验收口径

### 4.1 总则

- 新仪表"接线完成" ≠ "已度量"。判定分三级：**接线完成 / 静态合同通过 / 实机非零样本 + 关联**。
- 每次判定必须同时声明：DLL 哈希、合同 ON/OFF、`DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS` 取值、
  分辨率、地图、段边界快照路径。
- **拒绝计数上升不算通过。** 任何"①～④-b 上升"的表述都不构成"合法对象仍有替代路径"或
  "失败后可恢复"的证据。

### 4.2 三项证明的判定口径

| 证明 | 通过条件（全部满足） | 反例/否决 |
| --- | --- | --- |
| ② 合法对象仍有替代路径 | 同一实机段内：`semanticSceneSubmittedSkinned` 非零 **且** `semanticSceneDrawTimeSubmittedSkinnedPaletteSourceDrawTimeCapturedCount`（或 `OwnedPartSnapshot`）**逐帧非零比例显著**；`...SourceNoneCount`（若已接线）**不**与替代来源同比上升 | 只有 ④-b 拒绝上升；或 `SourceNone` 同幅上升（把拒绝当修复）；或替代来源计数全 0 |
| ③ 失败后可恢复 | 存在**同 part/同对象**的"帧 N 死在门（④-b）→ 帧 N+k 经 draw-time 替代来源重新提交"的关联；且 ④-b **不**逐帧单调累积到永久遗漏 | 无对象级关联（记"未覆盖"）；或"拒绝增量下降"被单独当证明 |
| ① 错误矩阵不再被使用 | Gap A `...DeviceRejectedStaleCount` 增量有效 **且** 能按对象关联读到"被拒读取的原目标对象"；危险来源（`SubmitTimeGlobalSlot` / `SubmitTimeBlendedCache`）增量为 0 或可归因 | 仅 `RejectedStale` 上升而来源分布无改善；或无法做同对象关联（记"未覆盖"） |

### 4.3 必须能区分"内容没到门"与"内容死在门"

判据（全部为**当帧值**比较，取采样序列的 max 与"非零样本占比"，**不做 Δ**——见 §4.4）：

| 现象 | 判据 | 结论写法 |
| --- | --- | --- |
| 内容死在门 | ① > 0 且 ④-b > 0 | "在 canonical 门被拒绝 N 个 skinned 包/帧（max）" |
| 内容没到门 | ① > 0 且 ④-a == 0 且 ④-b == 0 | "包在门之前即被丢弃"；用 ②/③ 定位丢失阶段 |
| 门已打开 | ④-a > 0 | 门的可开性成立（MVP-1 未含 ④-a，需 MVP-2） |
| 到了分类块却无桶数 | ⑤ > 0 且 8 桶全 0 | **仪表缺陷**，不是路径缺失 |
| 替代来源存在 | ⑥-2 任一正向桶 > 0 且 §4.2 条件满足 | 可写"观察到替代来源投影" |

### 4.4 采样口径（当帧值）

- `m_war3Scene.shadowStats` 每帧整体重建（先例 `m_war3Scene = War3FrameScene{}`），
  经 `NoteShadowSceneStats` 发布为 `g_shadowSceneStats`（桥内 `merged = stats;` 复制），
  ⇒ 报告快照里的标量是**当帧值**。
- **因此**：
  1. **分段增量（Δ）对新计数器无意义**：两个边界快照相减得到的是两个随机帧的差，
     不代表区间累计。判定只使用**每帧最大值**与**非零样本占比**。
  2. 单帧 0 **不**等于整轮未发生（需看 0.5s 采样序列的 max / 占比）。
  3. 与既有 **进程累计原子**（Gap A 的 `...DeviceRejectedStaleCount` 等）口径**不同**，
     两族数字**不得混算**；报告中必须分别标注"当帧值"与"进程累计"。
- 新计数器**应放 `m_war3Scene.shadowStats`（与分类桶同源）而不是进程全局原子**：
  这样它们与 8 桶同帧、同发布路径、同 JSON 出口，可直接做"同一帧内 ①/④-b/⑥-2 的联合判读"。
  换成进程全局原子会丢失"同帧"语义，且需要额外接线到 frame 边界。

### 4.5 轮次要求（**已按上级 Q-D 裁定改写**）

> **上级裁定（codex 线程 01a02e0b，Q-D）**：ON/OFF 必须**除合同值外完全一致**；
> 本工单原稿"ON 下 `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS`=0、OFF 下 =1，
> 却同时声称两轮只差合同"**自相矛盾，须修正**。以下按"ON/OFF 只有 `ContractEnabled()`
> 一个差异"重写；**不接受**"这是唯一能产生证明的矩阵"的辩解。

1. **同一对照对（pair）内，`DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS` 必须两轮取值相同**，
   且除合同值（`ContractEnabled()`，即编译期 `WARVK_SKIN_PALETTE_CONTRACT_DEFAULT` 的产物）外，
   地图、分辨率、段边界、DLL 哈希、环境与驱动状态全部一致。
2. 允许的对照对只有两种，**各自独立成对、不得混算**：
   - **pair-0（主对照，必做）**：两轮均 `..._PALETTE_DIAGNOSTICS=0`。用于验证新增四项计数
     **不依赖**该 env 即可出数。
   - **pair-1（交叉对账，可选）**：两轮均 `..._PALETTE_DIAGNOSTICS=1`。用于既有 8 桶与新增计数的
     同轮交叉解释；它**不是** ON/OFF 合同对照的替代，其性能与画面数据单列。
3. **禁止**出现"ON 取 0、OFF 取 1"（或反向）的组合：那会同时改变合同与热路径诊断成本，
   使任何差异都无法归因到合同值。
4. 每段边界取快照 + 0.5s 采样序列。
5. 若 pair-0 的 R-ON 下 ① == 0，则"ON 生产路径无 skinned append"成立，
   本工单前提需重新裁定（见 §7 Q7）。

---

## 5. 风险与代价

### 5.1 每提交一次自增的量级

- 形式：对 `m_war3Scene.shadowStats`（每帧重建的 device 私有结构）做普通 `++` ⇒ 一条
  load-add-store，热点在 L1 cache，约 **1–3 cycles**；不产生 `lock` 前缀。
- 频率上限估计：
  - ①（append 入口）≈ 每帧 skinned packet 数。实机 `semanticSceneSubmitted` max 178–340/帧，
    故 ① ≤ 数百/帧。
  - ④-b（canonical 门）与 ① 同量级。
  - ⑥-1/⑥-2 每个提交点各 1–2 次自增；实机 `semanticSceneSubmittedSkinned` max 52/336（ON），
    故 ⑥ ≤ 数百/帧。
- 合计新增自增 **≈ 1×10³/帧**。按最坏 3 cycles/次 ≈ 3×10³ cycles ≈ **1 µs/帧**（3 GHz 量级），
  相对 16.7 ms 帧预算 < **0.01%**。
- 若实施时被迫用 `std::memory_order_relaxed` 原子（跨线程场景），单次约 20–40 cycles
  ⇒ 仍是 ≈ 20–40×10³ cycles ≈ **7–13 µs/帧**，< **0.1%**。**待验证**：以实机 ABBA 为准。
- 额外的固定成本：结构体增大约 17×4 = 68 B（当帧）+ 17×8 = 136 B（导出），
  可忽略；但会轻微改变 `m_war3Scene.shadowStats` 的 cache line 布局（相邻字段可能跨线）——
  建议把新字段**集中追加在既有 palette 探针字段块尾部**，不要插到热字段中间。

### 5.2 对 90.7% shadow map 执行率与帧时间的影响上限

- "shadow map 90.7% 采样执行"是既有实机观测（来源见 §8 锚点 B5），由 receiver 侧门决定，
  与 append 计数**无因果关系**；新仪表不改变 `semanticSceneShadowMapExecutedThisFrame` 的任何输入。
- 影响上限估计：**帧时间 < 0.1%**（见 §5.1），**执行率无变化**（同一判定链）。
  该数字是**估算**，必须由实机 ABBA（同 DLL 内 ON/仪表开关对照）确认；
  在确认前只能写"预期可忽略"，**不得**写成"已证明无影响"。
- 反例门：若实机 ABBA 显示帧时间上升 > 1%，应回退到只保留 MVP-1 的 4 个字段。

### 5.3 为什么"只加计数不改判定"不改变画面语义

1. 所有新增自增都**不参与**任何返回值、门控、选择、剔除、材质、发布或生命周期判定；
   它们只写 device 私有的当帧统计结构。
2. 既有先例已确立该模式：`War3SemanticPaletteDiagnosticsRuntime()` 的注释明确声明这些探针
   **不参与** palette 选择/重放验证/发布（`d3d9_device.cpp`，唯一子串
   `These historical motion/churn probes do not participate in`）。
3. **唯一需要警惕的语义风险是"插入位置"**：若把计数插进一个原本不会被求值、或求值有副作用的
   表达式（例如放进短路条件、宏、或 `if constexpr` 的错误一侧），会改变行为。本工单要求：
   每个自增都是**独立语句**，插在既有语句之间的语句位置，不进入任何条件表达式。
4. 与 §2.6 相关的风险：**不得**把 7.28 的 8192 项 `thread_local` 开窗探针镜像到 draw-time。
   那是真实热路径成本（既有注释已说明其为大查找表并因此默认关闭）。
   镜像只允许**常数时间**的来源分类（枚举/布尔判定），第 8 位用残差桶。

### 5.4 不改变的既有合同（实施时必须保住）

- `WARVK_SKIN_PALETTE_CONTRACT_DEFAULT` 源码 fallback 必须保持 **0**；
  候选构建默认 1 只能来自 meson 选项（既有静态测试钉死，见 §8 锚点 A22）。
- Gap A/B 计数器（`semanticSceneSkinnedPaletteSlotCache*`）的语义与数值不得被新仪表"顺带"改动。
- 既有 8 桶（`semanticSceneSubmittedSkinnedPaletteSource*`）的位置、条件、env 门保持原样；
  新仪表是**新增**，不是替换或搬迁。
- 不得因为新计数而新增日志、`SafeRead`、指针解引用或异常路径
  （尤其 §2.2/§2.3 附近已有大量 `IsReadableRange` 调用，新计数不得与之耦合）。

---

## 6. 静态门禁草案

### 6.1 目标

新增一个 AutoTest 静态测试，**钉死新计数器名与自增位置**，防止：
(a) 字段名/JSON 键名漂移；(b) 11 个出口任一断线；
(c) 计数器被挪到 canonical 门之前/之后而改变"死代码"结论；
(d) 计数器被重新挂回 `War3SemanticPaletteDiagnosticsRuntime()` 而再次恒 0。

### 6.2 建议文件

`AutoTest/test_active_path_palette_instrumentation_export_static.py`
（形态照抄 `AutoTest/test_palette_slot_cache_counters_export_static.py`：
`pathlib` 读文件 + `assert`，纯文本锚点，不依赖构建产物）。

### 6.3 断言要点

**分组 A — 命名与 11 个出口（对 MVP-1 每个字段循环）**

```python
FIELDS = [
  "semanticSceneAppendEntrySkinnedCount",
  "semanticSceneCanonicalGateRejectSkinnedCount",
  "semanticSceneDrawTimeProducerSubmittedSkinnedCount",
  "semanticSceneDrawTimeSubmittedSkinnedPaletteSourceDrawTimeCapturedCount",
]
# A1 当帧字段：uint32_t ... = 0;  出现在 d3d9_war3_scene.h
# A2 bridge 字段：uint64_t <f> = 0; 出现在 war3_shadow_runtime_bridge.h
# A3 bridge 搬运：<f> 出现在 war3_shadow_runtime_bridge.cpp，且同文件出现 "g_shadowSceneStats"
# A4 hub 字段：uint64_t <f> = 0; 出现在 war3_diagnostics_hub.h
# A5 hub 搬运：<f> 出现在 war3_diagnostics_hub.cpp，且同文件出现 "bridgeSummary"
# A6 hub JSON：f'{{"{f}"' in war3_diagnostics_hub.cpp
# A7 control-plane JSON：f'{{"{f}"' in war3_control_plane.cpp
# A8 perf agg 字段：uint64_t <f> = 0; 出现在 war3_perf_monitor.h
# A9 perf agg 累加：f"agg.{f} +=" 出现在 war3_perf_monitor.cpp
# A10/A11 perf JSON 写手 ×2：f'\\"{f}\\"' 在 war3_perf_monitor.cpp 中至少出现 2 次
```

**分组 B — 自增位置（关键，禁止只钉名字）**

1. **① 必须在策略门之后、几何门之前**：
   - `DEVICE.index("m_war3Scene.shadowStats.semanticSceneAppendEntrySkinnedCount++")`
     必须 **>** `DEVICE.index("ShadowProducerPolicyAllows(")`；
   - 且必须 **<** `DEVICE.index("BuildCanonicalShadowDrawItem(")`（保证在 canonical 构建之前，
     即属于"入口漏斗"而非门内）。
2. **① 必须紧跟 skinned 判定**：存在字面量
   `"ShadowDrawPath::Skinned;"` 与计数自增同处一个窗口（例如断言二者之间的字符数小于 N，
   或断言 `if (skinned)` 紧邻）。
3. **④-b 必须在 canonical 拒绝分支内且为 skinned-only**：
   - 存在字面量 `"if (!canonicalItem.readyForShadowConsumer()) {"`，
     且该子串与 `semanticSceneCanonicalGateRejectSkinnedCount++` 同处一个窗口；
   - 该计数自增**不得**出现在 `"currentDrawResolveStatus =="` 的条件表达式中
     （即拒绝侧计数是无条件的 skinned-only，不依赖 resolve 状态）。
4. **⑤（若 MVP-2）不得挂在诊断 env 上**：
   - `semanticScenePaletteClassifyEntrySkinnedCount++` 必须出现在
     `"War3SemanticPaletteDiagnosticsRuntime()"` 字符串**之前**的独立语句里；
   - 断言 `"if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {"` 仍然存在（既有 8 桶未被搬迁）。
5. **⑥ 两个站点各自正确**：
   - `DEVICE.count("semanticSceneDrawTimeProducerSubmittedSkinnedCount++") == 1`，
     且落在 `War3TryPopulateDrawTimeSemanticProducer` 函数体内
     （用函数签名子串与下一个函数签名的偏移做区间判定）；
   - ⑥-b 的计数（`semanticSceneDrawTimeFastAppendSubmittedSkinnedCount++` / 或 MVP-1 中
     `...SourceDrawTimeCapturedCount++` 的第二站点）落在
     `War3TryPopulateDirectCurrentDrawGrouped` 区间内，且位于
     `"finishFastAppend(FastAppendOutcome::Success, true)"` **之前**。
6. **禁止进程全局原子**：
   - 断言 `d3d9_device.cpp` 中**不出现** `std::atomic<uint32_t>` 与任一新字段名同处一个声明窗口；
   - 断言 `d3d9_war3_scene.h` 中新字段声明为 `uint32_t`（与分类桶同源，§4.4）。
7. **既有合同未动（回归护栏）**：
   - `"#define WARVK_SKIN_PALETTE_CONTRACT_DEFAULT 0"` 仍在
     `war3_skin_palette_selection.h`（与既有测试重复，作为本测试的显式前置）；
   - 既有 8 桶名（`...PaletteSourceNoneCount` … `...SourceChurnCount`）在
     `d3d9_war3_scene.h` 中仍在原位附近且数量不变（可用计数 `== 8`）；
   - **新增镜像桶名与既有桶名不得互相包含导致 grep 歧义**：断言
     `"semanticSceneSubmittedSkinnedPaletteSource"` 与
     `"semanticSceneDrawTimeSubmittedSkinnedPaletteSource"` 是两个不同前缀。

**分组 C — 出口数量守卫**

- 对每个字段断言 `war3_perf_monitor.cpp` 中该键的 JSON 出现次数 `== 2`
  （两个写手，禁止只接一个）。
- 断言 `"runtimeSummary"` 仍在 `war3_perf_monitor.cpp` 中（先例测试同款守卫）。

### 6.4 明确不做

- 不做数值/语义断言（静态测不可能验证"非零"）——非零必须由实机证明。
- 不验证 §2.6 的来源映射正确性（属实机/代码审阅范畴）。
- 不新增/修改既有测试文件（本工单只**建议**新测试内容，不落地）。

---

## 7. 需要上级裁定的问题清单

| # | 问题 | 选项 / 建议 |
| --- | --- | --- |
| **Q1** | 是否允许把本工单的仪表**直接落地**到当前 B 树？ | (a) 直接落地并重算 DLL 哈希；(b) 先做 dev 门控候选（新 meson 选项，默认关）再评估。**建议 (a)**：全部为只加计数、不改判定，且 dev 门控会让"ON 生产路径无仪表"的缺口在默认构建里继续存在 |
| **Q2** | 批准范围 | (a) MVP-1（4 字段 / 44 处编辑）；(b) MVP-1+2（8 字段 / 88 处）；(c) 全量 17 字段 / 187 处。**建议 (a)**，实机验证后再追加 |
| **Q3** | 如果 Q1 选直接落地，是否接受冻结候选哈希变化并重新走一次实机冻结流程？ | **建议接受**，但必须重新记录 DLL 字节数/SHA-256 并重跑 R-ON/R-OFF |
| **Q4** | §2.6 的来源映射是**待验证**的：是否要求实施前先做一次只读代码核实（draw-time 两个站点能否拿到来源证据），还是允许先落"纯分母"版本？ | **建议先只读核实**；若不可得，MVP-1 退回纯分母并在结论中写"来源归属未覆盖" |
| **Q5** | 是否需要为新仪表保留一条**独立编译开关**（默认 ON），还是无条件编译？ | **建议无条件编译**（成本 < 0.1%），避免重演"默认关 ⇒ 恒 0"；若上级要求可回退，则开关默认必须为 ON |
| **Q6** | ⑥-2 中 GPU-skin lease 背书是否单列一个桶（第 9 桶），还是归入 `OwnedPartSnapshot`？ | **建议归入并单列**（来源语义不同）；若为控制字段数则归入，并在文档注明 |
| **Q7** | 若 R-ON 下 ① == 0（ON 路径确实没有 skinned append），本工单前提是否作废？ | **建议**：届时改为裁定"仪表应挂在哪个真正活跃的生产者"，并重新出工单，不得强行把 0 解释为缺仪表 |
| **Q8** | 是否允许为验证镜像桶而临时打开 `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1`（改变热路径成本）？ | **上级 Q-D 已裁定**：ON/OFF 除合同值外必须完全一致；该 env 只能作为**成对**的独立矩阵（pair-1：两轮同为 1），**不得** ON=0/OFF=1。主对照 pair-0 两轮均为 0，性能数据单列（详见 §4.5） |
| **Q9** | §4.2 证明③要求的"同 part/同对象关联"是否必须本轮完成，还是允许先记"未覆盖"？ | **建议允许记"未覆盖"**：新仪表提供"拒绝存在 + 替代来源存在"的必要条件，充分条件仍需 evidence recorder 的对象级事件（`skin-selection/v1`）join |

---

## 8. 附录：一手位置索引

> 全部锚点以**函数名 / 唯一子串**为准；行号为写作时（本文件创建时）在
> `dxvk-v1.22-integration-20260914` 树内的快照，会漂移。

### A. 代码锚点（本工单引用的事实来源）

| ID | 主题 | 文件 | 锚点（唯一子串 / 函数名） | 快照行 |
| --- | --- | --- | --- | --- |
| A1 | 合同开关与编译默认 | `src/d3d9/war3/render/war3_skin_palette_selection.h` | `#define WARVK_SKIN_PALETTE_CONTRACT_DEFAULT 0`；`inline bool ContractEnabled()`；`DXVK_WAR3_SKIN_PALETTE_CONTRACT` | 7-9, 29-33 |
| A1b | 候选构建把默认改为 1 | `src/d3d9/meson.build`；`meson_options.txt` | `warvk_skin_palette_contract_candidate` → `-DWARVK_SKIN_PALETTE_CONTRACT_DEFAULT=1` | 318-321；11 |
| A2 | append 入口 4 参重载 | `src/d3d9/d3d9_device.cpp` | `bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(` + `bool currentFrameExactOwnerPrefiltered` | 22044 |
| A2b | 3 个转发重载 | 同上 | `return War3TryAppendSemanticShadowPacket(packet, directCurrentDrawSample,` | 21991/21996/22004 |
| A3 | 策略门①的锚 | 同上 | `if (!dxvk::war3::render::ShadowProducerPolicyAllows(` | 22067 |
| A4 | path blocker 门 | 同上 | `War3PacketIsPathBlocker(packet)` | 22134 |
| A5 | 索引切片门与计数 | 同上 | `kShadowSemanticCoreSceneRequireVisibleIndexSliceForSkinned`；`semanticSceneSkinnedMissingVisibleIndexSliceRejectCount++`；`semanticSceneSkinnedDynamicIndexSliceCount++` | 22312-22350 |
| A5b | 切片运行时门（默认 1） | 同上 | `DXVK_WAR3_SEMANTIC_REQUIRE_VISIBLE_INDEX_SLICE` | 1182-1186 |
| A6 | current-draw resolve | 同上 | `const auto currentDrawResolveStatus =`；`ResolveCurrentDrawAuthoritativeSample(` | 22433-22447 |
| A6b | resolve 状态桶（`Ready` = 脉冲 ③） | 同上 | `semanticSceneCurrentDrawResolveReadyCount++`；`semanticSceneCurrentDrawMissNoContract++` 等 | 22677-22693 |
| A7 | canonical 就绪计数 + 原因 | 同上 | `noteCanonicalReadiness`；`semanticSceneCanonicalReadyCount++`；`semanticSceneCanonicalRejectNoWorldTransform++` | 23096-23139 |
| A8 | canonical 硬门（死代码分界） | 同上 | `if (!canonicalItem.readyForShadowConsumer()) {`；`semanticSceneCurrentDrawResolveReadyRejectedCount++`；`return false;` | 23166-23172 |
| A9 | 分类块（8 桶）入口 | 同上 | `if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {`；`switch (paletteSourceThisSubmit)` | 23318-23372 |
| A9b | 来源 churn 桶 | 同上 | `stats.semanticSceneSubmittedSkinnedPaletteSourceChurnCount++;` | 23448-23453 |
| A10 | 提交计数（semantic，门后） | 同上 | `m_war3Scene.shadowStats.semanticSceneSubmittedSkinned++;`（#1） | 24882 |
| A11 | 分布诊断运行时门（默认 0） | 同上 | `inline bool War3SemanticPaletteDiagnosticsRuntime()`；`DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS` | 2590-2598 |
| A12 | 来源枚举 | 同上 | `enum class War3SemanticPaletteSource : uint32_t {`（7 值：None…OwnedPartSnapshot） | 6597-6605 |
| A13 | draw-time 提交点 ⑥-a | 同上 | `uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer(`；`NoteShadowAppendRawcode(entry.rawcode);`；`semanticSceneSubmittedSkinned++;`（#2） | 25469；25913-25917 |
| A14 | draw-time 提交点 ⑥-b | 同上 | `uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(`；`Phase 7.108b：survey rawcode（fast-append 路径）`；`finishFastAppend(FastAppendOutcome::Success, true)`；`semanticSceneSubmittedSkinned++;`（#3） | 26636；29758-29779 |
| A14b | `War3ActivateDrawTimeCacheEntry`（只做 ledger activate） | 同上 | `void D3D9DeviceEx::War3ActivateDrawTimeCacheEntry(`；`m_war3DrawTimeActiveLedger.activate(` | 25463-25467 |
| A14c | 其 capture 段调用点 | 同上 | `War3ActivateDrawTimeCacheEntry(vbCacheKey, cached);` / `War3ActivateDrawTimeCacheEntry(vbCacheKey, entry);`（在 `War3TryCaptureShadowCaster` 内） | 45147 / 45260（函数 42804） |
| A15 | `War3DrawTimeVBEntry`（**无来源字段**） | `src/d3d9/d3d9_device.h` | `struct War3DrawTimeVBEntry {` | 2598-… |
| A16 | 当帧结构 | `src/d3d9/d3d9_war3_scene.h` | `struct War3FrameScene {`；`War3ShadowCaptureStats shadowStats;`；8 桶声明区；churn 桶 | 1940-1949；1436-1471（churn 1468） |
| A17 | 当帧重建点 | `src/d3d9/d3d9_device.cpp` | `m_war3Scene = War3FrameScene{};` | 20159 / 26344 / 33044 / 36013 |
| A18 | 快照发布与"当帧值"语义 | `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp` | `void NoteShadowSceneStatsImpl(`；`War3ShadowCaptureStats merged = stats;`；`g_shadowSceneStats = merged;` | 4217-4568（4220 / 4565） |
| A19 | 发布调用点 | `src/d3d9/d3d9_device.cpp` | `dxvk::war3::render::NoteShadowSceneStats(m_war3Scene.shadowStats);` | 20126 等 8 处 |
| A20 | 导出先例（8 目的地 / 11 位置） | 见 §3.1 表 | `PaletteSourceChurnCount` 全名 `semanticSceneSubmittedSkinnedPaletteSourceChurnCount` | 19 行命中 |
| A21 | 静态门禁先例 | `AutoTest/test_palette_slot_cache_counters_export_static.py` | 全文 83 行；`FIELDS = [...]` + 分组断言 | 1-83 |
| A22 | 编译默认类静态守卫先例 | `AutoTest/test_diagnostic_build_options_default_off_static.py` | `WARVK_SKIN_PALETTE_CONTRACT_DEFAULT` fallback 必须为 0 | 59-68 |

### B. 实机证据锚点（上级记录，本文引用其口径，不重新判定其数据）

| ID | 主题 | 位置 | 内容 |
| --- | --- | --- | --- |
| B1 | 三轮胎事实 | `docs/plan/2026-09-17-p0-real-machine-execution-record.md` §1–§2 | round1 M1(ON) / round2 M2(ON) / round3 M2(OFF)；ON 下 P0 目标计数结构性恒 0；OFF 下 Gap A 复活 |
| B2 | 当帧值口径警告 | 同上 §2.2 与 §2.4 | "这些字段位于 `m_war3Scene.shadowStats`，每帧被整体替换（bridge.cpp:4217-4568）⇒ 边界快照是当帧值" |
| B3 | ON 下门 100% 拒绝 ⇒ 死代码 | 同上 §2.4 | "canonical 就绪门在 `d3d9_device.cpp:23166-23172` 对全部包返回 false（`CanonicalReadyCount` 全程 0），使 23172 之后整段…成为结构性死代码" |
| B4 | 需在活跃路径补仪表的三点 | 同上 §4 第 2 项 | ① 门内 skinned-only 拒绝计数；② 阶段脉冲；③ **最关键**：draw-time 两处提交点镜像 8 桶 |
| B5 | 90.7% shadow map 执行率 | `docs/plan/2026-09-16-merge-execution-ledger.md` | "真正活跃的是 direct/draw-time 生产者那半边（replay/casters 400–870/帧，shadow map 90.7% 采样执行；PopulateLastReturnReason=10）" |
| B6 | 分类桶需诊断 env | `docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md` §1.1 | "palette 来源分类 7 个计数器结构性恒 0…该运行时门默认 0" |
| B7 | 三项证明与拒绝不可替代 | 同上 §4；`docs/plan/2026-09-16-palette-gap-fix-workorder.md` §6 | 证明口径与"不靠拒绝数增加" |

### C. 待验证清单（实施前必须只读核实）

1. `War3DrawTimeVBEntry` 在 ⑥-a/⑥-b 自增处能否提供**可分辨**的来源证据
   （`gpuSkinLeaseBacked` / generation-backed 复用证明 / `persistentPackageCurrentDrawProof`
   的可用性与语义）。
2. ⑥-a/⑥-b 是否存在把包送回 §2.1 主链的 fallback；若存在，合同 ON 下会死在 canonical 门，
   不得把该 fallback 记为"替代来源"。
3. 新增字段的自增点是否全部在**单线程**提交上下文中（决定 `uint32_t` vs relaxed 原子）。
4. 结构体扩容后是否触碰既有 `static_assert` / 尺寸/对齐合同（`d3d9_device.h` 与 factory
   符号编码的先例）。
5. `semanticSceneSubmittedSkinned` 的 3 个自增点在新增分母后是否仍能逐帧对账
   （#1 + #2 + #3 == 既有 `semanticSceneSubmittedSkinned`）。

### D. 无代码变更声明

本工单**未创建、未修改、未删除**任何源码、测试、构建脚本或数据文件；
未执行构建、部署、游戏启动或 git 写操作。唯一新增物即本 Markdown 文件。
