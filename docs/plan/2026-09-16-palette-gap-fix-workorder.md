# 2026-09-16 — P0 工单：palette 两处缝隙的"路由式"修复

> 用户裁定：两处缝隙优先处理，但**不能只改成返回失败**。验收必须证明
> "错误矩阵不再被使用" **且** "合法对象仍有正确的替代路径"，
> 不得只凭撕裂减少 / 拒绝计数增加宣布通过。

## 0. 已核实的关键结构（主线程读码结论）

### 0.1 合同状态决定可达性
- `War3TryBuildLiveRuntimeGroupPalette`（d3d9_device.cpp:8501）在
  `render::skin::ContractEnabled()` 为真时走**严格路径**（8564-8581）：
  只用 `QueryOwnedRenderablePartPaletteSnapshot`，失败即 return false，
  注释明确"绝不把记忆槽位当作今天 arena 字节的所有权，无 legacy/raw/Pose 回退"。
  → **B-S1 只在合同关闭时可达**，即普通/Release 构建的生产路径。
- 合同关闭时，slot 路径之后仍有**非 slot 替代路径**：
  `PoseFallback`（8822+）经 `PoseRegistry` 已发布姿态矩阵，含 runtimeModelPtr、
  ±0xA0 偏移、`QueryRenderablePartOwnerRuntimeModel` 反查、pose alias 解析；
  仍失败且不允许 CModel 回退才 return false。

### 0.2 两处缝隙的现状
- **Gap A / B-S1**（d3d9_device.cpp:8633-8644 `useCachedEntry`）：
  第 8635-8638 行 `+0x08` 合法时直接供出并覆盖 entry；8639-8642 producer 命中则采用；
  **8644 行 producer miss 时 `return entry.paletteSlotIndex`（记忆槽位）**。
  后续 8694-8818 是三个**按 slot 键**的读者：producer part snapshot（part 键，安全）、
  Game.dll 全局 arena（8757-8801）、`QueryBlendedPaletteBySlotIndex`（8805-8818）。
  记忆槽位一旦陈旧，后两者会读到**别的对象今天的字节**。
- **Gap B / B-S2**（war3_shadow_renderer_core.cpp:760-805 `FindOrUpdatePaletteSlotCache`）：
  当前槽位非法时（778 行 else，784-788）**完全不问 producer、无帧校验**，
  直接返回 `s_paletteSlotCache[i].paletteSlotIndex`；调用方
  `tryEngineDirectPosePalette`（6524-6573）再 `×48` 读 Game.dll+0xBC6BD0 arena。
  该函数失败后，6591+ 有基于 `resource.matrixIndices` / `pose.matrixPalette` 的 CPU 构建路径。

### 0.3 可用的独立校验源（producer 侧，非被质疑字段）
- `QueryRenderablePartPaletteSlot(part, &slot, &groupCount, &frameTag)`
  （war3_model_hook.cpp:9240）：独立绑定表 `s_renderablePartPaletteBindings`，
  part 身份校验 + cellBusy guard，返回 slot/groupCount/frameTag。
- `QueryRenderablePartPaletteSnapshot(part, expectedCount, &vec, &hash, &frameTag)`
  （war3_model_hook.cpp:9285）：**该 part 本帧的调色板快照**，开关
  `DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT` **默认 true**（2413-2417）。
  → 这就是"合法对象的正确替代路径"的首选。
- 现状：`war3_shadow_renderer_core.cpp` **当前无任何 producer API 依赖**（grep 为空），
  需引入薄声明头（避免把 war3_model_hook.h 整头拉进 shadow core）。

## 1. 修复设计（路由式，不是拒绝式）

### Gap A（B-S1）
1. entry 增加 `groupCount` / `frameTag` 字段，写入时由已有的
   `QueryRenderablePartPaletteSlot` 调用一并填充。
2. `useCachedEntry` 供出缓存前复核：producer 必须确认同一 part **同一 slot**、
   groupCount 兼容、frameTag 为当前帧；通过才供出。
3. producer miss 或复核不通过 → **返回 0xFFFFFFFF**，不再供出记忆槽位。
   → 跳过两个 slot 键的危险读者，落到 8822+ `PoseFallback`（独立已发布姿态源）。
4. 保留：producer part snapshot（8726，part 键）仍在 slot 路径内且优先级最高。

### Gap B（B-S2）
1. 当前槽位非法时，先 `QueryRenderablePartPaletteSlot` 复核；
   确认同一 part 同一 slot 且 frameTag 当前 → 可继续用缓存（**合法对象的快路径保留**）。
2. 复核不通过 → 返回 0xFFFFFFFF，`tryEngineDirectPosePalette` 返回 false。
3. 在 CPU 构建路径（6591）**之前**插入 producer part snapshot 备选：
   `QueryRenderablePartPaletteSnapshot` 成功则采用（默认开关已开）。
4. 顺序变为：producer 确认的 slot → producer part snapshot → CPU resource/pose 构建 → 省略。

### 计数器：优先复用 B 既有来源分类（已核实，勿建平行体系）
B 已有 palette 来源taxonomy，直接作为度量仪表
（`d3d9_war3_scene.h:1436-1442` + `1468`）：
- `semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount`（最稳定）
- `…SourceSubmitTimeGlobalSlotCount`（危险读者①：Game.dll arena 按 slot）
- `…SourceSubmitTimeBlendedCacheCount`（危险读者②：QueryBlendedPaletteBySlotIndex）
- `…SourceOwnedPartSnapshotCount` / `…SourceSubmitTimePublishedRegistryCount`
  （替代路径：producer 快照 / 已发布姿态）
- `…SourceSubmitTimeCModelFallbackCount` / `…SourceNoneCount`（兜底/未取到）
- `semanticSceneSubmittedSkinnedPaletteSourceChurnCount`（**同 key 本帧 source 与上一帧
  不一致**——这正是陈旧槽位导致的撕裂指纹）
- 另有 `semanticSceneLivePaletteRefresh*` / `…MotionRawChanged/Stable`、
  `…MotionGroupChanged/Stable`（bridge.h:696-714）可作交叉验证。

**只新增缺失的两类**，与既有字段同风格、同出口：
- `…PaletteSlotCacheServedAfterProducerConfirmCount`（合法快路径仍命中，证明未误杀）
- `…PaletteSlotCacheRejectedStaleCount`（复核不通过被拒，且必须伴随上面的
  `OwnedPartSnapshot`/`PublishedRegistry` 增长，而不是 `SourceNone` 增长）

## 2. 验收标准（用户指定，缺一不可）

1. **错误矩阵不再被使用**：陈旧槽位不可能在未经 producer 确认的情况下被读为
   arena 字节——用静态断言钉死代码路径（两个危险读者只能在 producer 确认后到达），
   而非只看计数。
2. **合法对象仍有正确替代路径**：实机/隔离数据需显示被拒对象中有相当比例经
   `snapshotFallbackUsed` 或 `poseFallbackUsed` 成功投影；
   **不得**用"拒绝计数增加"代替本项。
3. **失败后可恢复**：第 N 帧被拒的对象在 N+k 帧绑定恢复后必须能重新投影；
   不得引入永久黑名单/负缓存。
4. 反例门：必须有一张"应当投影"的高压低视角 + 往返移动画面，证明正常单位/
   建筑/桥梁/装饰物未被本修复误杀。

## 3. 排期与纪律

- 与 P2 批次 1（doodad 默认翻转）**解耦**；与批次 6（Stage13 常驻几何）**解耦**。
- 本工单属用户指定的 **P0**；实现需在 B 树 build32 空闲时进行（一次只跑一个构建）。
- 合同 ON 路径已是严格 fail-closed，本修复**不改变**合同 ON 行为。
- 报告须按交付项描述，不使用百分比概括。

## 4. 必须保住的两项既有合同（已核实现有测试钉死）

1. **跨地图会话隔离**（`AutoTest/test_issue6_render_identity_cache_reset_static.py`）：
   `FindOrUpdatePaletteSlotCache` 内必须保留
   `g_paletteSlotCacheSessionGeneration` 的 TLS generation 比对与
   "生成号变化即清空 s_paletteSlotCache + 归零 s_paletteSlotCacheIndex" 逻辑。
   本次加固不得削弱地图 epoch 失效语义；该测试若因行锚点漂移只能更新锚点，
   不得改动断言语义。
2. **直映射加速器合同**（`AutoTest/test_live_palette_slot_cache_accelerator_static.py`）：
   `useCachedEntry` 的直映射条目（`s_paletteSlotCacheLookup`）与
   `s_paletteSlotCacheCursor++` 环形分配必须保留；该加速器只是**碰撞回退扫描**的
   快捷路径，命中后仍要按原合同复核 `renderablePart` 身份与 `mapEpoch`
   （见源码 8621-8625 注释）。加固只增加 producer 绑定复核，不得取消身份/epoch 复核。
   该测试可能需要更新锚点（新增字段/语句会移动位置）——只更新锚点。

## 5. 与既有诊断的衔接

- 新增的四类计数器应并入既有的 palette 诊断出口（与 B 的 skin-selection / v1 证据事件
  同风格），**不要**新建平行的事件体系；命令名与 JSON 键沿用 B 现约定。
- 合同 ON 路径（8564-8581）已有严格 fail-closed 语义，本修复不改变其行为；
  报告中需明确"合同 ON 未受影响"的证据（该分支代码未改）。

## 6. 三项证明的度量方案（用既有计数器，不靠"拒绝数增加"）

| 用户要求 | 度量（修复前 vs 修复后，同一场景） |
| --- | --- |
| ① 错误矩阵不再被使用 | `SubmitTimeGlobalSlot` 与 `SubmitTimeBlendedCache` 中**无 producer 确认**的占比归零；`SourceChurnCount` 显著下降（陈旧槽位翻转指纹消失） |
| ② 合法对象仍有替代路径 | `OwnedPartSnapshot` / `SubmitTimePublishedRegistry` / `DrawTimeCaptured` **上升并接住**被拒的量；`SourceNoneCount` **不得**同幅上升 |
| ③ 失败后可恢复 | 同一对象跨帧：被拒帧之后出现 `…ServedAfterProducerConfirm` / `…DrawTimeCaptured` / `…OwnedPartSnapshot` 重新命中；`SourceNoneCount` 不得逐帧累积 |

补充反例门（必须）：高压低视角 + 往返移动画面下，正常单位/建筑/桥梁/装饰物
	（对应 `DrawTimeCaptured` / `OwnedPartSnapshot` 路径）投影数量不得低于修复前基线。

## 7. 实施进度（2026-09-16 深夜）

### Gap B（shadow-core）— **主线程已实施**
- 文件：`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`
- 改动：① 槽位非法分支先 `QueryRenderablePartPaletteSlot` 复核同 part 同槽位；
  不通过返回 `0xFFFFFFFF`；② `tryEngineDirectPosePalette` 增加 producer part 快照优先
  （`requiredCount` 数量校验）；③ 三个本地计数器（含"经确认仍命中"与"陈旧被拒"）；
  ④ CPU 构建路径保留为下游替代来源；⑤ 跨地图会话隔离语义未动。
- 测试：`AutoTest/test_palette_slot_cache_producer_confirmation_static.py`
- 门禁：构建 exit 0、no-work、meson 72/72、静态 **238/238**；
  DLL 35,946,589 / `1C314090821487505D66298B7CF003B1D5F87E036800EE1E9D177296B6C955C6`
- 未完成：三个计数器**尚未导出**到报告（后续接线）；三项画面证明待实机。

### Gap A（device 侧 `useCachedEntry`）— **kimi-k3 已完成（2026-09-16 深夜）**
- 文件：`src/d3d9/d3d9_device.cpp`，原 `useCachedEntry` 在 8635-8647，
  fail-open 在 **8646 `return entry.paletteSlotIndex;`**。
- 改动：① `queryProducerBindingSlot` 扩展为同时取回 groupCount/frameTag；
  ② `PaletteSlotCacheEntry` 新增 `paletteGroupCount`/`paletteFrameTag`；
  ③ `useCachedEntry` 供出记忆槽位前复核：producer 绑定命中 + 槽位等于记忆槽位 +
  groupCount ≥ requiredPaletteCount，三要素全满足才供出（并刷新 entry），
  任一不满足返回 `0xFFFFFFFFu`；④ 冷路径插入时一并填充 groupCount/frameTag；
  ⑤ 两个本地计数器 `g_devicePaletteSlotCacheServedAfterConfirmCount` /
  `g_devicePaletteSlotCacheRejectedStaleCount`（与 Gap B 同风格，尚未导出到报告）。
  身份/mapEpoch 复核、直映射加速器与环形分配未动；合同 ON 分支未动。
- 替代路径保持可达：slotIndex 非法时不进 slot 块，落到 **PoseFallback**
  （PoseRegistry 已发布姿态 + ±0xA0 偏移 + producer-owner 反查）；无负缓存。
- 测试：新增 `AutoTest/test_device_palette_slot_cache_producer_confirmation_static.py`；
  `test_live_palette_slot_cache_accelerator_static.py` 仅更新一处锚点（语义不变）。
- 门禁：构建 exit 0、no-work、meson 72/72、静态 **241/241**；
  DLL 35,979,538 / `F73F49B0295A46DF230BC83DCED85CB9F5D40114BAB23335E94A2621C293C039`
  （注意：build32 为 F855 诊断配置）。
- 未完成：两个计数器**尚未导出**到报告（后续接线）；三项画面证明待实机。
