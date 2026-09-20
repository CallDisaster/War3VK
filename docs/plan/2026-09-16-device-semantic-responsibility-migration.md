# 2026-09-16 — 把 War3 语义选择职责迁出 d3d9_device.cpp：现状清册与迁移方案

> 目标项之一（用户裁定为长期重构核心）。本文是**清册与方案**，不代表任何代码已迁移。
> 判定标准：device.cpp 只保留 **D3D9 资源生命周期与 Present 安全点编排**；
> "哪个对象/部件/调色板/几何是权威"的判定都属于语义选择职责，应迁出。

## 1. 现状规模（主线程实测）

`src/d3d9/d3d9_device.cpp` 共 **52,387 行**。按主题行数：

| 主题 | 行数 |
| --- | --- |
| 语义/contract（`War3Semantic`/`semanticScene`/`ShadowSemantic`） | 1,454 |
| palette 选择 | 1,329 |
| Stage13/S1 | 790 |
| CurrentDraw | 409 |
| Arena/budget | 344 |
| pathBlocker | 328 |
| draw-time cache | 227 |
| persistent geometry | 101 |
| objectCaster | 37 |

语义选择类函数定义共 **36 个**，代表项：

| 类别 | 代表符号（行号） |
| --- | --- |
| 当前 draw 绑定 | `War3ResolveCurrentDrawCaptureBinding` (293) |
| sticky/grouped 选择运行时配置与填充 | `War3SemanticObjectGroupedSelectionRuntime` (2345)、`War3SemanticSticky*Runtime` (2477-2658) |
| 选择键构造 | `War3SemanticDirectSelectionKey` (2677)、`War3SemanticDirectRecordSelectionKey` (2757) |
| 对象类型裁决与提交闸 | `War3ResolveSemanticPacketObjectKind` (5007/7942)、`War3ShouldSubmitSemanticPacket` (5010/7989)、`…Fast` (7747/7760) |
| 帧优先策略 | `War3ShouldPreferSemanticSceneFrame` (8074) |
| pose alias 解析 | `War3ResolveLivePoseRuntimeAlias` (8430) |
| **调色板构建** | `War3TryBuildLiveRuntimeGroupPalette` (6656 声明 / 8501 实现) |
| 阴影 replay 分类 | `War3ClassifyShadowReplayMode` (21148) |
| persistent geometry 准入/查找 | `War3CanPromoteShadowPersistentGeometry` (21284)、`War3TryFindShadowPersistentGeometry` (21743) |
| draw-time producer 填充 | `War3TryPopulateDrawTimeSemanticProducer` (25413) |

## 2. 为什么不能一次性搬

这些职责与两类 device 状态**交织**：
1. **GPU 资源生命周期**：Arena 提交/检疫、persistent geometry 表、budget 字节账
   —— 受硬约束"reset/Arena 回收只能在 PresentEx 安全点"约束；
2. **Present 安全点编排**：map epoch 失效、session reset、fence 退休。
因此迁移必须**先分离"数据与判定"和"资源操作"**，再移动判定侧。

## 3. 分阶段迁移（由易到难）

| 步 | 内容 | 判据 | 风险 |
| --- | --- | --- | --- |
| M1 | **纯判定与常量**迁出：对象类型裁决、提交闸、选择键构造、帧优先策略、replay 分类、运行时配置 getter（`War3SemanticSticky*Runtime` 一类） | 无 device 成员依赖；迁出后行为逐点等价 | 低 |
| M2 | **调色板选择**迁出到统一入口（与 `docs/plan/2026-09-16-unified-data-selection-entry-design.md` 的 S2/S3 合并推进） | 同一 `Selection` 裁决；来源 taxonomy 计数对照不变 | 中 |
| M3 | **persistent geometry 准入/查找**迁出，**保留资源操作在 device** | device 只提供资源原语，判定在语义层 | 中高 |
| M4 | **CurrentDraw 绑定与 draw-time producer 填充**迁出 | 需保证与 Present 安全点的先后关系不变 | 高 |

**M4 最后**：它直接参与 Present 路径的编排，最易引入时序缺陷。

## 4. 防止回退的固定门禁（与目标项"建立防止回退的固定门禁"对齐）

1. **符号白名单门禁**：新增静态测试 `test_device_semantic_responsibility_budget_static.py`，
   钉住 `d3d9_device.cpp` 中允许出现的语义选择符号集合与数量上限；
   迁移后**只允许减少**，新增（或改名绕过）即失败。这使"职责回流"无法悄悄发生。
2. **迁移等价门禁**：每步迁移必须同时给出
   - 静态合同（新模块的单测 + 原路径断言的等价改写）；
   - 计数器对照（`semanticSceneSubmittedSkinnedPaletteSource*`、`SourceChurnCount`、
     `budgetExceeded`、persistent 字节账）在迁移前后**同场景不劣化**。
3. **安全点门禁**：断言 GPU 资源 reset / Arena 回收 / receiver-skin 切换仍只经 PresentEx 安全点
   （已有相关静态测试，迁移不得绕过）。
4. 每步独立可回退；不得把语义变更夹带进迁移。

## 5. 明确不做

- 不在迁移中改变任何判定的**结果**（迁移不是修 bug；修 bug 走独立候选）；
- 不把 GPU 资源操作搬进语义模块；
- 不用"device.cpp 行数下降"当作完成度证明——必须同时证明画面与计数未回退。
