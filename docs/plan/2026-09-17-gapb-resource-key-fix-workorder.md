> **上级裁定更正（2026-09-17 08:2x，codex 线程 01a02e0b）**：
> ① 本文中"纯 env trace **零风险**"的表述**作废**——`DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD=1`
> 会改变**抓取频率与时序**，不只是多打印日志；它需要**独立冻结预算**，不得直接追加进下一轮实机。
> ② **不批准**为让 `resolved>0` 直接放宽 `IsContractUnitCandidate`；正确顺序是：先记录同一对象的
> 实际查询键/写入键/资源代际/发布时机/拒绝原因 → 确认缺失来源 → 再形成**默认关闭的 dev 候选**；
> dev 门不是安全检查的替代品。本文 §P1 只能作为"诊断方案"继续细化，不得据以落地生产准入。

# 2026-09-17 — Gap B 资源键失配修复工单（P0 先验 trace + P1 一处谓词 + P2/P3 硬化）

> **文档性质（先读）**
> 1. 本文是**工单 / 实施设计文档**，**本轮只出方案，不落地**：不改 `src/`（含注释）、不构建、
>    不部署、不启动游戏、不做任何 git 写操作。
> 2. 本文依据的一手事实截至 **2026-09-17** 读码 / 交接记录。**行号会漂移**：所有点位一律以
>    **函数名 / 唯一子串**为锚；括号里的行号只是当日快照，实施前必须重读当前文件复核。
> 3. 结论分级一律显式标注：
>    `[已核实-静态]`（源码可回查）/ `[已核实-实机记录]`（有落盘产物）/ `[待验证]` / `[未覆盖]`。
>    **禁止**把"接线/静态"写成"已修复"；**禁止**把 P1 的"能解析"写成"阴影已修复"。
> 4. 本文**不改变**任何默认值 / env 默认 / 验收口径；需要改默认行为的地方一律进入 §5 裁定清单。
> 5. 规范锚点：`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`（下称 **core.cpp**）、
>    `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`（下称 **contract.cpp**）、
>    `src/d3d9/war3/render/war3_visible_renderables.cpp`（下称 **visible.cpp**）、
>    `src/d3d9/war3/model/war3_model_resource_cache.cpp`（下称 **cache.cpp**）、
>    `src/d3d9/war3/core/war3_internal_test_config.h`（下称 **config.h**）、
>    `src/d3d9/d3d9_device.cpp`（下称 **device.cpp**）。

---

## 0. 范围与禁止项

### 0.1 范围内（本工单定义的三步）

| 步骤 | 内容 | 是否改源码 | 是否改渲染语义 |
| --- | --- | --- | --- |
| **P0** | 纯运行时 trace 先验：把 manifest 记录的**四键实际取值**与资源缓存**键集合可达性**定死 | **否**（只用既有 env 开关） | **否** |
| **P1** | 主修复：放宽 `IsContractUnitCandidate` 一处谓词，使 DemandFill 对"已解析 geoset 的场景记录"可达 | 是（1 个函数 + 注释） | **是**（按需几何抓取变活跃、可能新增 caster） |
| **P2/P3** | 可选硬化：key3 精确等值 alias；上游 starvation 规模修复 | 是（独立候选） | P2 是；P3 待定 |

### 0.2 范围外（本轮明确禁止）

1. **不改**任何源码文件、不改任何注释、不碰 `src/`；只写本文件。
2. **不改**默认值：不动 `war3_internal_test_config.h` 的任何 `inline constexpr`，不动
   `kShadowSemanticCoreSceneUnitsOnly`、`kShadowSemanticCoreSceneRootUnitSupplementEnabled`、
   `kShadowSemanticCoreScenePoseResourcePreviewSeedsEnabled`、四个 visible hydrate/finalize 常量。
3. **不加 / 不改** env 默认：不动 `DXVK_WAR3_SEMANTIC_*` / `DXVK_WAR3_MANIFEST_*` 的读取点与默认值。
4. **不改**验收口径：`semanticCoreResolved>0`、`skippedResourceMiss→0`、`submittedDrawCount>0`、
   `shadowArenaFrameIncomplete=0`、`deviceLost=0`、ABBA 交替——见 §3.6。**不得**把
   `semanticSceneSubmitted`（设备侧，实机修复前已非零）替换成通过判据。
5. **不构建、不部署、不启动游戏、不覆盖现场 DLL、不做 git 写**。任何实机执行必须走 §5 的裁定流程。
6. **不混验收**：P1 与 P2/P3 各自独立验收；manifest 条数（上游 starvation）与四键失配**无因果关系**
   （§1.5），不得用 P1 的通过去掩盖 starvation，也不得用 starvation 的修复冒充 P1 通过。

### 0.3 纪律

- 本文所有"实机数字"都注明来源文件；**只有交接记录（非一手）的数字会显式标注**，可回查的写"可从
  `C:\Windows\Temp\warvk_p0\round*_\` 回查"。
- 任何"我改了 X 就能修好"的推理必须给出**次序证明或写入方可达性证明**；给不出的一律写 `[待验证]`。
- 本文**不宣称**任何修复已完成。

---

## 1. 结论摘要与证据链

### 1.1 现象（实机）

`[已核实-实机记录]` P0 组合候选实机（合同 ON 的 M2 轮与合同 OFF 的 M2 轮，矩阵仅合同不同）：

- 语义 core **确实构建完成**：`semanticCoreFrameSerial == semanticCoreManifestFrameSerial`、
  `semanticCorePublishRevisionLag == 0`、`buildInProgress/Pending == 0`。
- 但解析阶段：`semanticCoreConsidered = 2`、`semanticCoreSkippedNoGeoset = 2`、
  `semanticCoreSkippedResourceMiss = 2`、`semanticCoreResolved = 0`、
  `semanticCoreSubmittedDrawCount = 0` ⇒ core 的 draws 为空。
- 一手出处：`docs/plan/2026-09-17-p0-real-machine-execution-record.md` §2.3（M2 轮具体数值）
  与 §2.4（`semanticCoreResolved` 在 round1/2/3 = 0/0/0）。
- **待复核**：该记录 §2.3 的原文只给出 M2 轮的具体数值；OFF 轮的 `considered` /
  `skippedResourceMiss` 逐值请在 `C:\Windows\Temp\warvk_p0\round3_off_m2\` 的分段快照中回查后再引用
  "ON/OFF 两轮同值"。

关键 JSON 键名（可用于回查）：

| 键 | 定义点（以函数名为锚） |
| --- | --- |
| `semanticCoreConsidered` / `Resolved` / `SubmittedDrawCount` | `war3_shadow_runtime_bridge.cpp` 的 bridge summary 填充块（`summary.semanticCoreConsidered = semanticCore.resolve.considered;`） |
| `semanticCoreSkippedNoGeoset` / `SkippedResourceMiss` / `SkippedResourceNotReady` | 同文件，紧随其后（`summary.semanticCoreSkippedResourceMiss = semanticCore.resolve.skippedResourceMiss;`） |
| `semanticManifestResolveRawScanMissCount` 等 | `war3_control_plane.cpp` 的 `ToJson(...)` 诊断块 |
| `recordsWithRuntimeModel` / `recordsWithModelResource` / `unknownCount` / `unitCount` | `war3_control_plane.cpp` 的 `ToJson(const War3FrameManifestSummary&)`（逐 record 统计 `manifest->records`） |
| `shadowGeosetResourceCount` / `shadowReadyGeosetCount` / `shadowModelResourceCount` / `shadowRuntimeModelCount` | 来源 `war3_shadow_runtime_bridge.cpp` 的 `ShadowModelResourceCache::instance().geosetRecordCount()/readyGeosetCount()/modelResourceCount()/runtimeModelRecordCount()` |
| `shadowArenaFrameIncomplete` | `war3_diagnostics_hub.cpp`（`summary.shadowArenaFrameIncomplete = arenaDiagnostics.frameIncomplete;` 与对应 JSON 键） |

### 1.2 代数结论：第一处失败**就是**四键 miss

`[已核实-静态]` `ShadowRendererCore::resolveRecord`（core.cpp）：

```
blocked-fourcc               -> 7512  (rawcode 命中 IsBlockedSemanticFourCc)
no-geometry-hint             -> 7518  (meshData/runtimeGeosetPtr/runtimeGeosetDataPtr/geosetIndex 全空)
anonymous-scene-resource     -> 7531  (sceneSubmissionRuntime && !hasStableIdentity) -> skippedNoIdentity++
no-resolved-geoset           -> 7536  -> skippedNoGeoset++ . skippedNoResolvedGeoset++   <-- 唯一自增点
四键查找                     -> 7547-7580
resource-miss                -> 7581  -> skippedNoGeoset++ . skippedResourceMiss++      <-- 唯一自增点
resource-not-ready           -> 7592  -> skippedNoGeoset++ . skippedResourceNotReady++
resource-scene-sanity-reject -> 7603  -> skippedNoGeoset++ . skippedResourceNotReady++
```

代数推理（用实机计数反推，不依赖日志）：

1. `skippedNoResolvedGeoset == 0`（实机）而该计数**只在** `no-resolved-geoset` 分支自增 ⇒
   两条记录**都**满足 `hasResolvedGeoset()`（`geosetIndex != kInvalid...` 或 `runtimeGeosetPtr != nullptr`
   或 `runtimeGeosetDataPtr != nullptr`，定义在 `war3_shadow_runtime_contract.h` 的
   `struct ShadowRenderableRecord::hasResolvedGeoset()`）。
2. `skippedNoIdentity == 0`（实机）而该计数在 `7532/7538/7583/7594` 等处自增 ⇒ 两条记录
   **都**满足 `hasStableIdentity()`（`worldObjectEntry/sceneNode/unitPtr/runtimeModelPtr/modelResourcePtr/modelKey/jHandle/rawcode`
   任一非空）。
3. ⇒ 两条记录越过 `7536-7545`，在 `7581-7589` 处 `resource == nullptr` ⇒
   `skippedResourceMiss` 自增。**第一处失败就是资源查找失败**；`skippedNoGeoset` 与
   `skippedResourceMiss` 同时 +2 就是这个分支的指纹。

**一处必须写清的边界**：`resource == nullptr` 之后还有一段 legacy 兜底
（`7547-7580` 内 `TryFindRenderableResourceFromCache`），它被
`!IsSemanticSceneSubmissionRuntimeEnabled()` 守卫。实机扫描路径默认开启
（`config.h` 的 `kShadowSemanticCoreSceneSubmissionEnabled = true`；`war3_semantic_shadow_gate.cpp` 的
`IsSemanticSceneSubmissionRuntimeEnabled()` = `kShadowSemanticCoreSceneSubmissionEnabled && IsSemanticShadowPreviewEnabled() && EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION", true)`，
默认 true）⇒ **该兜底未参与**，四键 miss 即完整失败。
若后续实机确认该门为 false，结论须改述为"四键 + legacy cache 兜底全 miss"。
**P0 必须记录 effective env 与 `semanticCoreFrameFresh`，以排除这一分支。**

### 1.3 四键的写入方清单（逐键可达性）

`[已核实-静态]` 四键与写入方（`ShadowModelResourceStore`，
contract.cpp 的 `void ShadowModelResourceStore::add(` / `void ShadowModelResourceStore::bindRuntimeModelAlias(`）：

| 键 | 查找点 | 写入方 | 唯一性 |
| --- | --- | --- | --- |
| **key1** `(runtimeGeosetPtr)` | `findByRuntimeGeoset` | `add()`：`if (stored.runtimeGeosetPtr != nullptr) m_byRuntimeGeoset[...]` | 仅 `add()` |
| **key2** `(runtimeGeosetDataPtr)` | `findByRuntimeGeosetData` | `add()`：`if (stored.runtimeGeosetDataPtr != nullptr) m_byRuntimeGeosetData[...]` | 仅 `add()` |
| **key3** `(runtimeModelPtr, geosetIndex)` | `findByRuntimeModel` | `bindRuntimeModelAlias()`（**唯一**写入者） | 仅此一处 |
| **key4** `(modelResourcePtr, geosetIndex)` | `findByModelResource` | `add()`（`stored.modelResourcePtr != nullptr && geosetIndex != invalid` 时写 `m_byModelResource`） | 仅 `add()` |

`add()` 的输入是 `buildResourceStore` lambda 里的
`resourceCache.forEachGeosetContractSource(...)`（contract.cpp 的
`auto buildResourceStore = [&resourceCache, &manifest]()`），即**缓存里已有的 geoset 记录**。

`bindRuntimeModelAlias()` 的输入是同一 lambda 里的
`resourceCache.forEachResourceStoreAlias(...)`，而该 API（`war3_model_resource_cache.h` 的
`void forEachResourceStoreAlias(Fn&& fn) const`）**只遍历缓存自身的 `m_byModelResource` 与
`m_byRuntimeModel`**。实机 `shadowModelResourceCount == shadowRuntimeModelCount == 0`
（`[已核实-实机记录]`）⇒ 两个循环体都不执行 ⇒ `bindRuntimeModelAlias` 一次都不被调用
⇒ **key3 在结构上永远为空**（`[已核实-静态 + 实机]`）。

**key4 需要更小心的表述**：`m_byModelResource` 也由 `add()` 从 geoset 记录派生，而 geoset 记录确实
可以携带 `modelResourcePtr`（`ConvertGeoset` 在 contract.cpp 中从
`alias->modelResourcePtr` / `src.modelResourcePtr` 传递；cache.cpp 的
`if (modelResourcePtr != nullptr) geosetRecord.modelResourcePtr = modelResourcePtr;` 写入）。
因此 **`shadowModelResourceCount == 0` 本身不能单独证明 key4 为空**；它只证明 key4 的
*manifest 侧*取值与 *geoset 侧*取值不构成匹配（否则 record 就不会 miss）。
⇒ key4 的精确状态列入 **P0 必答项**（§2.4）。

### 1.4 为什么 key1/key2 也 miss：三个写入方全被编译期常量关掉

`[已核实-静态]` `noteRuntimeGeosetBinding`（cache.cpp，
`void ShadowModelResourceCache::noteRuntimeGeosetBinding(`）是**唯一**能把 `geosetIndex` +
`modelResourcePtr` 一起写进 geoset 记录的函数。全库调用者**只有 3 个**：

| # | 调用点（以函数名为锚） | 守卫 | 实机可达性 |
| --- | --- | --- | --- |
| ① | contract.cpp 的 `TryPublishMissingVisibleUnitGeosetBinding` 内 | 函数首行 `if (!IsContractUnitCandidate(record)) return false;` + 随后的 `runtimeModelPtr/runtimeGeosetDataPtr/geosetIndex` 三重判空 | **不可达**：谓词要求 `ObjectKind::Unit`，实机记录是 `Unknown`（§1.4.1） |
| ② | contract.cpp 的 resource bootstrap 块（`const bool needsResourceBootstrap =` 之后遍历 `modelRegistry.snapshot()`） | `resourceCache.runtimeModelRecordCount() == 0u && modelRegistry.recordCount() != 0u && resourceCache.readyGeosetCount() == 0u` | **待验证**：`runtimeModelRecordCount()==0` 成立，但需 `modelRegistry.recordCount() != 0` 且 `readyGeosetCount()==0`；实机 `shadowRuntimeModelCount==0` 说明本块若跑过也没留下 runtime model 记录（或 `modelRegistry.recordCount()==0` 使其被跳过） |
| ③ | visible.cpp 的 `ResolveGeosetMetadata`（`resourceCache.noteRuntimeGeosetBinding(`） | 见下三个编译期常量 | **不可达** |

**③ 的三个调用者全被编译期常量关掉**（`[已核实-静态]`）：

- `ResolveGeosetMetadata` 只被 `FinalizeVisibleRecord` 与两个 hydrate 函数调用；
- `FinalizeVisibleRecord` 的唯一调用点在 `VisibleRenderableRegistry::appendRecord` 的
  `if constexpr (!dxvk::war3::internal::kWar3RuntimeConfigLightweightSemanticVisibleRenderableWrites)`
  分支内；`config.h` 的
  `kWar3RuntimeConfigLightweightSemanticVisibleRenderableWrites = true;`
  ⇒ 走 `else if (record.payload == nullptr) ...` 分支 ⇒ **`FinalizeVisibleRecord` 整体是死代码**，
  `ResolveGeosetMetadata` 的两处调用（finalize 内 1586/1627）永不执行；
- `HydrateVisibleSnapshotBasicFields` 的 unit geoset hydrate 段由
  `if constexpr (dxvk::war3::internal::kWar3RuntimeConfigSemanticVisibleEndFrameUnitGeosetHydrate)`
  守卫；`config.h` 置 **false** ⇒ 1930/1933 两处调用死代码；
- `HydrateVisibleSnapshotStaticSemanticFields` 的第一句是
  `ShouldHydrateStaticSemanticRecord(...)`，其内
  `if constexpr (!...kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate) return false;`；
  `config.h` 置 **false** ⇒ 2007 处调用死代码。

**另外两条"补 identity"的通路同样被关掉**：

- `RootUnitSupplement`：contract.cpp 的 `if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneRootUnitSupplementEnabled)`
  （两处：`buildResourceStore` 后的补充块、`snapshotBundleShared()` 内的补充分支）；
  `config.h` 置 **false** ⇒ 整段死代码。
- `PoseResourcePreviewSeeds`：core.cpp 的 `AppendPoseResourcePreviewSeeds(...)` 两处调用由
  `kShadowSemanticCoreScenePoseResourcePreviewSeedsEnabled` 守卫；`config.h` 置 **false** ⇒ 死代码。

#### 1.4.1 实机记录的字段形态

`[已核实-实机记录]` 两条记录都是 `ObjectKind::Unknown` 且 `unitPtr == nullptr`
（`unitPtr` 非空时 `ConvertVisible` 在 contract.cpp 的
`if (dst.objectKind == render::ObjectKind::Unknown && dst.unitPtr != nullptr && (dst.unitFlags5C & Building) == 0u)`
处会把 kind 提升为 `Unit`，实机没有发生）。
⇒ `IsContractUnitCandidate` 的**第一个**条件就不成立，①②③ 的入口全部关闭。

`[已核实-静态]` `noteModelResourceBinding`（cache.cpp）**从没完成过一次绑定**：
该函数在成功时打印 `"DXVK ModelCache: model bound ..."`，在"非空指针但解析失败"时打印
`"DXVK ModelCache: model bind rejected ..."`（同一 `g_modelBindFailLogCount` 限流块）；
传入 `nullptr` 时在 `if (modelResourcePtr == nullptr)` 处**静默返回、不打印**。
实机日志既无 `model bound` 也无 `bind rejected` ⇒ **要么从未被调用，要么只被以 `nullptr` 调用**
（`[已核实-实机记录]`，交接记录：日志无该两行）。

### 1.5 manifest 条数少是上游 starvation，与四键失配**无因果关系**

`[已核实-静态]`

- manifest 来源：contract.cpp 的 `const auto& visibleRecords = visibleRegistry.getAllVisibleView();`
  然后 `for (const auto& record : visibleRecords) manifest.records.push_back(ConvertVisible(...))`。
- `getAllVisibleView()`（visible.cpp）返回 `snapshotForThread().records`，而
  `snapshotForThread()` 的实现是：
  `if (std::this_thread::get_id() == m_renderThreadId) return m_snapshots[m_writeIndex]; return readSnapshot();`
  ⇒ **渲染线程读到的是本帧写快照**。
- `VisibleRenderableRegistry::beginFrame()` 会 `snap.records.clear()` 并推进 `m_writeIndex`；
  `VisibleRenderableRegistry::endFrame()` 才 `m_publishedIndex.store(m_writeIndex, ...)`。
  ⇒ 抓取点若落在 `beginFrame` 之后、本帧 record 尚未追满之前，看到的就是**近乎空的写快照**。
- 交接记录称"同一时刻已发布快照有 236 条"（`docs/plan/2026-09-16-merge-execution-ledger.md` 的
  2026-09-17 段）。**这是一手性未确认的数字**：原始证据应在
  `C:\Windows\Temp\warvk_p0\boundary_0N_*.json` / `shadow_summary_0N_*.json` 的
  `visibleRenderableCount` 与 frame summary 的 `visibleCount` 中回查 ⇒ `[待复核]`。

**结论**：manifest 条数少 = 上游（visible 采集 / 抓取时机）starvation；四键失配 = 键匹配问题。
两者独立，**P1 不会改变 manifest 条数**，也不应用 manifest 条数来判 P1 成败。

### 1.6 `DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE=1` 已生效但不产生任何资源键

`[已核实-静态]`

- 读取点：device.cpp 的 `bool War3SemanticPublishRegistriesBeforeSceneRuntime()`
  （`War3GetEnvU32("DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE", 0u) != 0u`，默认 **0**）。
- 使用点：device.cpp 的 capture 块
  `if (War3SemanticPublishRegistriesBeforeSceneRuntime()) War3PublishSemanticRegistriesForScene();`。
- 实现：`War3Renderer::PublishSemanticRegistriesForScene()`（`war3_renderer.cpp`）会依次
  `VisibleRenderableRegistry::endFrame()`、…、`model::ShadowModelResourceCache::instance().endFrame()`、
  `ModelRegistry::endFrame()`、`PoseRegistry::endFrame()` 等。
- 但 `cache.cpp` 的 `void ShadowModelResourceCache::endFrame()` **函数体只有一个
  `std::unique_lock<std::shared_mutex> lock(m_mutex);`**（空函数）⇒ **不会派生任何资源键**。
- 且 `getAllVisibleView()` 在 render 线程仍返回写快照（`snapshotForThread()` 的线程判定），
  所以"提前 endFrame"也不会改变抓取点看到的记录集。

⇒ 该 env 确实是"已生效"的接线，但**对四键失配零贡献**；`[已核实-静态]` 无 env 能增加 key1/key2。

### 1.7 无纯 env / 矩阵解

`[已核实-静态]` §1.4 的全部 gate 都是 `inline constexpr`（`war3_internal_test_config.h`），
在编译期固化；§1.4 的①②③与两条 identity 补充通路都**没有**运行时 env 覆盖。
⇒ **不存在纯 env / 纯矩阵解法**；必须改源码（P1）或新增运行时 trace 证据（P0）。

---

## 2. P0 零风险先验步骤（纯运行时 trace）

> 目标：用**既有 env 开关 + 既有日志/JSON**，把 manifest 记录的**四键实际取值**与
> 资源缓存键集合的**可达性/基数**钉死，从而决定 P1 是"一处谓词够不够"。
> 本步骤**不改源码、不改默认值、不改渲染语义**。

### 2.1 精确开关名 / 默认值 / 读取点

| 开关 | 默认 | 读取点（以函数名为锚） | 作用 |
| --- | --- | --- | --- |
| `DXVK_WAR3_SEMANTIC_SHADOW_TRACE` | 未设置 = 关 | core.cpp 的 `bool SemanticCoreTraceEnabled()`（字符串精确比较 `"1" / "true" / "TRUE"`，注意**不接受 `on`**） | 打开 core 的 skip / build / build-progress 日志 |
| `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD` | **240**（`std::max(1u, ...)`） | device.cpp 的 `uint64_t War3SemanticContractCapturePeriodRuntime()` | 稳态下两次 full capture 的帧间隔；置 **1** ⇒ 每帧都重抓 ⇒ 抓取点尽量看到完整写快照 |
| `DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE` | **0** | device.cpp 的 `bool War3SemanticPublishRegistriesBeforeSceneRuntime()` | 已在 §1.6 论证：对资源键无贡献，**P0 不需要打开**（可保持现状以缩小变量） |
| `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS` | 0 | device.cpp（`War3GetEnvU32("DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS", 0u)`） | 与四键无关；沿用 P0 矩阵即可，**不要新增** |

**建议的 P0 最小增量**（相对已跑过的 M2）：只加
`DXVK_WAR3_SEMANTIC_SHADOW_TRACE=1` 与 `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD=1`。
其余保持与 M2 完全一致，以保持可对比性。

### 2.2 期望日志行（逐字模板 + 字段意义）

**(a) 资源 miss 行**——core.cpp 的 `auto logSkippedRecord = [&](const char* reason)`
（调用点 `logSkippedRecord(hasStableIdentity ? "resource-miss" : "resource-miss-no-id")`）：

```
DXVK SemanticCore: skip resource-miss payload=%p part=%p scene=%p mesh=%p layer=%p runtime=%p modelRes=%p runtimeGeo=%p runtimeGeoData=%p geoIdx=%u meshIdx=%u handle=%u raw=%08X kind=%u q=%u layerIdx=%u
```

字段与四键的映射（**这就是"manifest 四键"的取值来源**）：

| 日志字段 | 结构字段 | 对应键 |
| --- | --- | --- |
| `runtime=` | `runtimeModelPtr` | key3 第一分量 |
| `modelRes=` | `modelResourcePtr` | key4 第一分量 |
| `runtimeGeo=` | `runtimeGeosetPtr` | key1 |
| `runtimeGeoData=` | `runtimeGeosetDataPtr` | key2 |
| `geoIdx=` | `geosetIndex` | key3/key4 第二分量（`4294967295` = `kInvalidShadowContractGeosetIndex`） |
| `kind=` | `objectKind`（`Unknown`=0） | §1.4.1 的直接证据 |

> **限流警告**：该 lambda 是"前 32 条全打，其后每 512 条打 1 条"（`s_skipLogCount`）。
> `considered=2` 的场景只有 2 条，一定能看到全部 2 行；但**不要**用它的条数去推断总记录数。

**(b) build-progress 行**——core.cpp 的 `"DXVK SemanticCore: build-progress manifest=%llu rev=%llu idx=%llu/%llu resolved=%llu skipNoId=%llu skipNoGeo=%llu skipNoPose=%llu chunks=%llu\n"`
给出 `idx/records`，用于确认 manifest 记录数（与 §1.5 的 starvation 观察对齐）。

**(c) build 完成行**——core.cpp 的 `"DXVK SemanticCore: build manifest=%llu rev=%llu resolved=%llu skinned=%llu skipNoId=%llu skipNoGeo=%llu skipNoPose=%llu skipNoGrp=%llu explicit=%llu/%llu draws=%llu chunks=%llu\n"`。
**注意：该行不含 `skippedResourceMiss`**，`skippedResourceMiss` / `skippedResourceNotReady` 只能从
JSON（`semanticCoreSkippedResourceMiss` / `...NotReady`）读。

**(d) 缓存侧唯一的两条正/负日志**——cache.cpp 的
`"DXVK ModelCache: model bound ptr=%p key=0x%llX geosets=%u ready=%u\n"`
与 `"DXVK ModelCache: model bind rejected ...\n"`（各限流 16/24 条）。用于判定
`noteModelResourceBinding` 是否曾以非空指针调用（§1.4.1）。

### 2.3 如何把 manifest 四键与缓存键集合对拍

> **能力边界（必须先声明）**：缓存侧**没有**任何逐键枚举出口。全库只有三个打印
> （都在 `noteModelResourceBinding`），`snapshotGeosets()` 虽存在但**无调用者**（`[已核实-静态]`）。
> 因此**纯 env 只能做"集合基数 + 写入方可达性"层面的对拍**；逐键枚举对拍需要一次性诊断输出
> （属改代码，不能算 P0）。

**对拍表**（每条 manifest 记录填一行，A/B 为两侧证据）：

| 判定项 | A：manifest 侧（来自 §2.2(a) 日志） | B：缓存侧（来自 JSON，键名见 §1.1） | 判读 |
| --- | --- | --- | --- |
| key1 是否被尝试 | `runtimeGeo != 0` | — | 非 0 则 key1 是该记录的**第一个**候选键 |
| key2 是否被尝试 | `runtimeGeoData != 0` | — | key1 为 0 或 key1 未命中时才是第一候选 |
| key3 是否被尝试 | `runtime != 0 && geoIdx != 4294967295` | `shadowRuntimeModelCount`（缓存 `m_byRuntimeModel.size()`） | B == 0 ⇒ `forEachResourceStoreAlias` 第二循环为空 ⇒ **key3 结构性不可命中** |
| key4 是否被尝试 | `modelRes != 0 && geoIdx != 4294967295` | `recordsWithModelResource`（manifest 侧）、`shadowModelResourceCount`（缓存 `m_byModelResource.size()`） | 两者都为 0 ⇒ key4 不可能命中；否则需逐键证据 ⇒ `[待验证]` |
| 记录是否有可用的 geoset 指针 | `runtimeGeo` / `runtimeGeoData` | `semanticManifestResolveRawScanMissCount` | **决定性**：>0 ⇒ 存在"只有 dataPtr、geosetIndex/geosetPtr 无效"的记录（见 §2.4-R2） |
| 记录是否有 runtime model | `runtime != 0` | `recordsWithRuntimeModel` | B 与 A 应一致 |
| 缓存里可被 `add()` 取用的 geoset 源规模 | — | `shadowGeosetResourceCount`、`shadowReadyGeosetCount`、`semanticManifestResolveRawScanCount`、`semanticManifestResolveLegacyCacheHitCount`、`semanticManifestResolveSourceCompleteSkipCount` | 用于证明"缓存确实非空，但仍无一条与 manifest 键相等" |

**对拍的正确读法**（避免误判）：
`shadowGeosetResourceCount > 0` **并不**说明 key1/key2 应该命中——缓存里的 geoset 记录是**别组的**
不变几何（来自 model hook 路径），与 manifest 从**活体内存**解析出来的 runtime geoset 指针不是同一批。
`semanticManifestResolveRawScanCount + semanticManifestResolveLegacyCacheHitCount` 与
ManifestCopy 的 scanned 总量（`m_manifestCopyTotalScanned` 的导出键）在四轮里精确相等
（交接记录 `LegacyCacheHit+RawScan==CopyTotalScanned`），说明 manifest 的 runtime geoset
**每帧从活体内存解析、从不来自资源缓存** ⇒ key1/key2 的取值来源与缓存键集合**天然不交集**，
除非有人调用 `noteRuntimeGeosetBinding` 把它们写进去。
（注意：该等式**不能**排除 `rawScanMiss`——miss 也先计一次 `rawScanCount`，所以必须单看
`semanticManifestResolveRawScanMissCount`。）

### 2.4 P0 判读与分流（决定 P1 的形态）

| ID | 观察 | 结论 | 下一步 |
| --- | --- | --- | --- |
| **R1** | `skip resource-miss` 行显示 `runtimeGeo != 0` 且 `geoIdx != 4294967295` | 记录的 key1 是第一个候选键，且 P1 的 DemandFill 三重前置（`runtimeModelPtr`/`runtimeGeosetDataPtr`/`geosetIndex`）**全部满足** | ⇒ **P1 可以只改谓词**（§3.2 的 A 方案） |
| **R2** | `semanticManifestResolveRawScanMissCount > 0`，或日志显示 `runtimeGeo == 0` / `geoIdx == 4294967295` | 记录只有 `runtimeGeosetDataPtr`，**geosetIndex 无效** ⇒ DemandFill 的 `geosetIndex == kInvalidShadowContractGeosetIndex` 前置会挡住，**一处谓词不够** | ⇒ 需要**第二处最小放宽**（DemandFill 前置改为"只需 `runtimeGeosetDataPtr`"）或先修 geoset 索引解析；进 §5-Q2 裁定 |
| **R3** | `runtime == 0`（`recordsWithRuntimeModel == 0`） | `noteRuntimeGeosetBinding` 仍能写 key1/key2（cache.cpp 中 geoset 记录写入不依赖 `runtimeModelPtr`），但 **key3 永远不可用**；`TryPublishMissingVisibleUnitGeosetBinding` 的 `runtimeModelPtr == nullptr` 前置会挡住 | ⇒ 同 R2 的第二处放宽；并须重新评估 key3 修复（P2）是否还需要 |
| **R4** | 日志里出现 `model bound`（说明 key3/key4 的 model 侧其实非空） | 与实机 `shadowModelResourceCount==0` 矛盾 ⇒ 说明是**采样时刻差异**，必须以同一 boundary 快照为准 | ⇒ 回到 §2.3 重新对拍；P1 之后仍需观察 key3/key4 |
| **R5** | `skip resource-miss` 一行都没有，但 `resolved == 0` | `SemanticCoreTraceEnabled()` 未生效（值不是 `1/true/TRUE`，例如写了 `on`）或限流偏移 | ⇒ 重新确认 env 字面值；P0 未成立，不得进入 P1 |

**所有 R 分流的原始材料一律落到 `C:\Windows\Temp\...` 的独立目录**，与 P0 组合候选的目录分开，
避免与已冻结的实机事务产物混淆。

### 2.5 "这一步不改渲染语义"的论证

`[已核实-静态]`

1. `DXVK_WAR3_SEMANTIC_SHADOW_TRACE` 只被 `SemanticCoreTraceEnabled()` 读取，全部使用点都是
   `if (SemanticCoreTraceEnabled()) { ... war3dbg::Print(...); }` 形式（`logSkippedRecord`、
   build-progress、build 完成行、no-pose detail、explicit-blend miss、runtime-group miss、
   dynamic-mesh rescue 等）——**只读计数器、只打印，不写任何共享状态**
   （唯一例外是各打印点的 `static std::atomic<uint32_t>` 限流计数器，与渲染无关）。
2. `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD=1` 只改变 device.cpp 中
   `capturePeriodElapsed` 的判定条件（`m_war3ShadowPersistentFrameSerial >= m_war3SemanticSceneLastCaptureFrameSerial + kSceneContractCaptureSteadyFramePeriod`），
   使 **capture 更频繁**。它**不改变** capture 的任何证明/拒绝分支，**不改变** resolve 的四键查找，
   **不改变**提交门；代价是 CPU（ManifestCopy / DemandFill / ResourceStoreBuild 次数上升），
   而不是渲染结果。
3. `PUBLISH_REGISTRIES_BEFORE_SCENE` 的 `endFrame()` 调用按 §1.6 论证是幂等发布操作
   （`war3_renderer.cpp` 的注释原文：`The individual endFrame calls are idempotent publish/freshness operations; they do not clear the current frame data.`），
   且 `ShadowModelResourceCache::endFrame()` 为空函数。
4. **风险与边界（如实声明）**：`CONTRACT_CAPTURE_PERIOD=1` 会显著抬高每帧 CPU；
   在 2048×1152 的隔离桌面上可能引入帧时间抖动，从而**改变**依赖帧时序的诊断量
   （例如 `semanticCoreFrameFresh` 的 `frameLag`）。因此 P0 的结论只用于**键匹配的身份判定**，
   **不得**用于任何性能或画面结论。若抖动导致看不到稳定的 build 完成行，退回
   `CONTRACT_CAPTURE_PERIOD=4` 重试，并在报告中写明实际值。

---

## 3. P1 候选设计（主修复，一处谓词）

### 3.1 现有谓词逐条列出

`[已核实-静态]` contract.cpp 的
`bool IsContractUnitCandidate(const ShadowRenderableRecord& record)`（唯一子串
`bool IsContractUnitCandidate(`；当前 450-454 行）：

```cpp
bool IsContractUnitCandidate(const ShadowRenderableRecord& record) {
  return record.objectKind == render::ObjectKind::Unit &&
         record.groupIdx <= 0 && record.unitPtr != nullptr &&
         (record.jHandle != 0u || record.rawcode != 0u);
}
```

逐条与实机的对照：

| 条件 | 语义 | 实机 | 后果 |
| --- | --- | --- | --- |
| `objectKind == ObjectKind::Unit` | 必须是单位 | **否**（`Unknown`，见 §1.4.1） | **第一刀即失败** |
| `groupIdx <= 0` | 排除分组子部件（`groupIdx` 默认 -1） | 未知（`[待验证]`） | 建议保留 |
| `unitPtr != nullptr` | 必须有 CUnit 指针 | **否**（`nullptr`） | 失败 |
| `jHandle != 0 \|\| rawcode != 0` | 必须有 handle 或 rawcode | 未知（`[待验证]`；`hasStableIdentity()` 为真说明**某一项**非空，但不一定是这两项） | 可能失败 |

调用者只有两处（都在 contract.cpp）：
`TryPublishMissingVisibleUnitGeosetBinding` 的首行判据、以及 `DemandFillVisibleUnitGeosetBindings`
主循环里的 `if (!IsContractUnitCandidate(record)) continue;`。
**放宽这个谓词不会影响** `IsVisibleDirectGeosetUnitCandidate` /
`VisibleDirectGeosetUnitRejectReason`（它们只在
`if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly)` 块内使用，
而 `kShadowSemanticCoreSceneUnitsOnly = false` ⇒ 该块是死代码）。

### 3.2 精确改动草案（diff 形式，只改谓词 + 必要注释）

**A 方案（推荐；前提 = P0 的 R1）**

```diff
 bool IsContractUnitCandidate(const ShadowRenderableRecord& record) {
-  return record.objectKind == render::ObjectKind::Unit &&
-         record.groupIdx <= 0 && record.unitPtr != nullptr &&
-         (record.jHandle != 0u || record.rawcode != 0u);
+  // 2026-09-17 Gap B 资源键修复（P1）：
+  // 旧条件要求 ObjectKind::Unit + unitPtr + (jHandle||rawcode)。实机 manifest 记录
+  // 全部是 ObjectKind::Unknown 且 unitPtr==nullptr（ConvertVisible 的 kind 提升未发生），
+  // 使本谓词恒 false => TryPublishMissingVisibleUnitGeosetBinding 与
+  // DemandFillVisibleUnitGeosetBindings 整段不可达 => noteRuntimeGeosetBinding 三个
+  // 调用者全部不执行 => ShadowModelResourceStore 的 key1/key2 缺少对应 geoset 源 =>
+  // ShadowRendererCore::resolveRecord 的四键查找全 miss（skippedResourceMiss=considered）。
+  // 收窄为“已解析 geoset 的场景记录”：只要求有稳定场景身份 + hasResolvedGeoset()，
+  // 保留 groupIdx<=0 以继续排除分组子部件。不改变任何几何抓取/去重/预算逻辑。
+  const bool sceneIdentity =
+      record.worldObjectEntry != nullptr || record.sceneNode != nullptr ||
+      record.runtimeModelPtr != nullptr || record.modelResourcePtr != nullptr ||
+      record.jHandle != 0u || record.rawcode != 0u;
+  return record.groupIdx <= 0 && record.hasResolvedGeoset() && sceneIdentity;
 }
```

**B 方案（仅当 P0 落在 R2/R3 时才需要；会把"一处谓词"变成"两处"）**

若日志显示记录的 `geosetIndex == kInvalidShadowContractGeosetIndex` 或 `runtimeModelPtr == nullptr`，
则除 A 方案的谓词外，`DemandFillVisibleUnitGeosetBindings` 主循环与
`TryPublishMissingVisibleUnitGeosetBinding` 前置的三重判空也必须相应放宽（例如只要求
`runtimeGeosetDataPtr != nullptr`；cache.cpp 的 `noteRuntimeGeosetBinding` 自身已允许
`runtimeGeosetPtr == nullptr` 而仅凭 `geosetDataPtr` 落库，见其首行
`if (runtimeGeosetPtr == nullptr && runtimeGeosetDataPtr == nullptr) return;`）。
**该方案的 diff 与风险必须另立文档**，不在本工单内定稿（进 §5-Q2）。

**静态测试影响**：`[已核实-静态]`
`AutoTest/test_visible_geoset_demand_fill_dedup_static.py` 用
`"bool TryPublishMissingVisibleUnitGeosetBinding(" -> "void DemandFillVisibleUnitGeosetBindings"` 与
`"void DemandFillVisibleUnitGeosetBindings" -> "ShadowRenderableRecord ConvertVisible"` 两个区间切片，
`AutoTest/test_manifest_ready_geoset_binding_static.py` 用
`"bool BackfillVisibleUnitGeosetBindingFromCache(" -> "bool TryPublishMissingVisibleUnitGeosetBinding"`。
`IsContractUnitCandidate` 位于这三个区间**之外** ⇒ A 方案**不需要改任何静态锚点**；
但仍必须重跑这两个脚本 + 全量静态。

### 3.3 为什么有效：DemandFill 在 `buildResourceStore` 之前的次序证明

`[已核实-静态]` contract.cpp 的 contract capture 段顺序如下，**每一步都是同步调用，无并发间隙**：

1. **ManifestCopy**：`const auto& visibleRecords = visibleRegistry.getAllVisibleView();` ->
   `manifest.records.push_back(ConvertVisible(record, ...))`。
   `ConvertVisible` 末尾会解析 geoset：`ResolveCurrentRuntimeGeosetFromData(dst, resolveDiagnostics)`
   （同一函数内，`if (dst.runtimeGeosetDataPtr == nullptr && LooksLikeGeosetDataPtrForContract(dst.meshData)) ...`
   之后）⇒ 到这里 `runtimeModelPtr / runtimeGeosetDataPtr / runtimeGeosetPtr / geosetIndex`
   已是 manifest 记录的最终取值。
2. **`DemandFillVisibleUnitGeosetBindings(manifest);`**（紧随 ManifestCopy 之后一行）。
   A 方案生效后，主循环对 `IsContractUnitCandidate` 为真的记录走
   `BackfillVisibleUnitGeosetBindingFromCache` → 未命中则
   `TryPublishMissingVisibleUnitGeosetBinding` → `resourceCache.noteRuntimeGeosetBinding(record.runtimeModelPtr, record.geosetIndex, record.runtimeGeosetPtr, record.runtimeGeosetDataPtr, record.modelResourcePtr, record.modelKey)`。
   该调用在 cache.cpp 内落到 `storeGeosetRecord(geosetRecord)`，于是缓存里出现一条
   `geosetPtr/geosetDataPtr/geosetIndex/modelResourcePtr` 与 manifest 记录**逐字相等**的 geoset 记录。
3. **`resourceRevision = resourceCache.revision();`**（DemandFill 之后立即重读 revision）。
4. **复用判定**：
   `const bool resourceStoreLooksUsable = m_resources != nullptr && !m_resources->records().empty();`
   `const bool resourceRevisionStable = m_resourceRevision == resourceRevision;`
   `const bool allowCooldownReuse = !dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled();`
   然后 `if (resourceStoreLooksUsable && (resourceRevisionStable || resourceRefreshCoolingDown)) resourcesPtr = m_resources;`。
   ⇒ 只要第 2 步真的写入了新 geoset，revision 就变化 ⇒ `resourceRevisionStable == false`；
   而实机 `IsSemanticSceneSubmissionRuntimeEnabled()` 为 **true**（§1.2 边界）⇒
   `allowCooldownReuse == false` ⇒ `resourceRefreshCoolingDown == false`
   ⇒ **旧 store 不被复用**。
5. **覆盖度兜底**（同一函数内，`ContractCpuScope("War3SemanticScene/CaptureContract/ResourceCoverage")`）：
   `if (resourcesPtr != nullptr && dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() && !ResourceStoreHasReadyManifestCoverage(*resourcesPtr, manifest)) resourcesPtr.reset();`
   ⇒ 即便第 4 步复用了旧 store，只要它对 manifest 记录**没有就绪覆盖**，就会被丢弃。
6. **构建**：
   `if (resourcesPtr == nullptr) { resourcesPtr = buildResourceStore(); rebuiltResourceStore = true; }`；
   `buildResourceStore` 内 `resourceCache.forEachGeosetContractSource(...)` 遍历缓存
   ⇒ **第 2 步刚写入的 geoset 记录必然被 `add()` 消费** ⇒ `m_byRuntimeGeoset[runtimeGeosetPtr] = i`
   与 `m_byRuntimeGeosetData[runtimeGeosetDataPtr] = i` 落位。
7. `snapshotBundleShared()` 把含新 store 的 bundle 发布给 `ShadowValidationRuntime`；
   core 在**同一帧或下一帧**的 `ShadowRendererCore::resolveRecord` 用同一批指针查 key1/key2
   ⇒ **命中 `resource != nullptr`** ⇒ 越过 `7581-7589`。

⇒ 次序上，**DemandFill 一定先于本轮 store 构建**，而 store 构建一定读取 DemandFill 的产物。
这就是"改一处谓词即可让 key1/key2 命中"的机械证明。
（`[待验证]` 的残余：第 2 步是否真能写成功，取决于 P0 的 R1/R2/R3 —— 见 §3.4。）

### 3.4 P1 的残余前置（P0 必须确认，否则 P1 可能无效）

`noteRuntimeGeosetBinding` 的成功需要 `CaptureGeosetRecordFromKnownPtrs(runtimeGeosetPtr, runtimeGeosetDataPtr, liveRuntimeRecord)`
返回 true（cache.cpp 内），而 DemandFill 侧的入口守卫是
`runtimeModelPtr != nullptr && runtimeGeosetDataPtr != nullptr && geosetIndex != kInvalidShadowContractGeosetIndex`
（`[已核实-静态]`，两处）。
因此 A 方案**只在下列条件同时成立时**有效：

- `runtimeGeosetDataPtr != nullptr`（严格必要：`noteRuntimeGeosetBinding` 首行即要求二者至少一个非空）；
- `geosetIndex != kInvalidShadowContractGeosetIndex`（DemandFill 入口守卫要求）；
- `runtimeModelPtr != nullptr`（DemandFill 入口守卫要求；同时决定 key3 是否可能可用）；
- `CaptureGeosetRecordFromKnownPtrs` 能从该 geosetData 指针读出可用几何
  （否则 `noteRuntimeGeosetBinding` 在 `if (!alreadyRefreshedThisFrame) { ... } else { return; }`
  处返回，什么都不写）。

`[待验证]` 上述四项在实机 manifest 两条记录上的取值 —— **这正是 P0（§2）存在的唯一理由**。
若任一不成立，走 B 方案（§3.2）并升级裁定（§5-Q2）。

### 3.5 风险清单

| # | 风险 | 证据 / 机制 | 缓解 |
| --- | --- | --- | --- |
| **P1-R1** | **按需几何抓取 CPU storm 回归** | `TryPublishMissingVisibleUnitGeosetBinding` 的注释原文（contract.cpp 内）：`This is intentionally a demand-fill, not a per-frame refresh. ... copying that geometry on every contract conversion caused the 5-13ms ManifestCopy storm.` 谓词放宽后，**能进入抓取的记录类扩大**；每次新 geosetData 指针会走一次完整几何捕获（positions/indices/UVs/group slots）。 | 保持既有预算：`kMaxDemandFillPerCapture = 64u`、`seenMissingGeosetData` 去重、`s_demandFillCursor` 轮转、`readyBinding` 尾扫。**P1 不得改这些**（也是静态测试钉死的）。实机必须单列 `War3SemanticScene/CaptureContract/VisibleGeosetDemandFill` 与 `ManifestCopy` 的 CPU scope，并测 storm 是否回来。`[待验证]`：代码注释里 "5-13ms" 的**原始测量记录**在本树 `docs/` 内未找到（只找到该注释本身），实施前应补齐一手出处或重测。 |
| **P1-R2** | **新增 caster / 画面变化** | 谓词放宽后，原本"解析不了所以被静默丢弃"的记录变成 `resolved`，进而进入 `SemanticCoreShouldSubmitResolvedPacket`（core.cpp，仅拦 path-blocker / 地下扁平 marker）与设备侧 `War3ShouldSubmitSemanticPacketFast`（device.cpp）。后者在 `unitsOnly=false`（实机默认，`kShadowSemanticCoreSceneUnitsOnly=false`）下允许 `War3IsEligibleSemanticDynamicUnit` / `War3IsEligibleSemanticStaticWorldCaster` / **`explicitUnknownRigid`**（`resolvedObjectKind==Unknown && path==Rigid && worldObjectEntry && sceneNode && pose.hasWorldTransform && hasRenderableGeoset && hasPacketGeometry && War3SemanticMaterialIsSafeOpaqueWorldCaster && War3SemanticDirectPacketHasMainWorldVisibleBacking`）三条通路。 | **必须做对象级/画面级复核**，不能只看计数。要求给出"合法对象仍投影"的**正向面**（施工图见 §3.6）；对新增 caster 做 rawcode/kind 分类统计。 |
| **P1-R3** | **非 Unit 记录引入 doodad / 地形 caster 的分类复核** | 放宽后候选集合从"Unit"变成"有场景身份的已解析 geoset 记录"；`ObjectKind::Unknown` 的记录在设备侧会被 `War3ResolveSemanticPacketObjectKindFast` 反查 `RenderObjectRegistry`（按 `sceneNode`/`worldObjectEntry`），可能被重新分类为 Building/Destructible ⇒ 走静态世界 caster 通路。 | 分类复核必须覆盖 `unknownCount` / `unitCount` / `buildingCount` / `destructibleCount` / `recordsWithModelResource` 的**分段增量**，以及新增 caster 的 rawcode 直方图；任何 doodad/地形类 caster 进入 shadow map 都要单独记录并回退。 |
| **P1-R4** | **身份 / 优先级退化** | `noteRuntimeGeosetBinding` 会把该 geoset 记录标记为 `geosetRecord.prefersRuntimeContract = true;` 并写 `lastRuntimeRefreshFrame`；而 `ShadowModelResourceStore::add()` 对同一 `(modelResourcePtr, geosetIndex)` 有**偏好规则**：`if (itExisting == end() \|\| itExisting->second >= m_records.size() \|\| !m_records[itExisting->second].prefersRuntimeContract \|\| stored.prefersRuntimeContract) m_byModelResource[key] = index;`。即 runtime-preferred 记录会**替换**同键的不变记录。 | 风险面是"某个不变 geoset 记录被 runtime 记录顶掉后，别的消费者的几何/就绪度变化"。缓解：P1 只放宽**谓词**，不改 `add()` 的偏好规则；实机必须对比 `shadowReadyGeosetCount` / `shadowGeosetResourceCount` 与 Arena 使用的分段增量，确认没有"老的可用记录被顶掉"。 |
| **P1-R5** | **`skippedResourceNotReady` 上升** | 谓词放宽 ⇒ `noteRuntimeGeosetBinding` 可能落库一条 `readyForShadowConsumer()` 为 false 的 geoset（几何没抓全）⇒ key1/key2 命中的是一条 **not-ready** 记录 ⇒ 走到 core.cpp 的 `resource-not-ready` 分支，`skippedResourceNotReady++`。 | §3.6 明确把 `skippedResourceNotReady` 仍为 0 列为**通过条件**；非 0 即判退，不得用 `resolved>0` 掩盖。 |
| **P1-R6** | **`resolved>0` 但 `submittedDrawCount` 仍为 0** | 两道门是**独立**的：core 的 `SemanticCoreShouldSubmitResolvedPacket`（只拦 path-blocker）与设备侧提交门。P1 只开前一道。 | §3.6 把 `submittedDrawCount>0` 单列；若 `resolved>0 && submittedDrawCount==0`，必须用设备侧 `DXVK SemanticHostReject:` / `DXVK SemanticHostRejectReason:` 日志（`War3DxvkSemanticShadowHost::shouldSubmitDraw`，只对 `path == Skinned` 的拒绝打印、前 64 条）定位第二道门，**不得**把 `resolved>0` 记作通过。 |
| **P1-R7** | **`shadowArenaFrameIncomplete` / deviceLost 上升** | 新增 caster ⇒ Arena 容量与 fence 压力上升。 | §3.6 硬条件。 |
| **P1-R8** | **上游 starvation 被误当作 P1 未生效** | P1 不改变 manifest 条数（§1.5）。若 manifest 仍是 1-4 条，`considered` 仍可能很小。 | §3.6 的判据用"**归零/大于零**"而不是"绝对值大"：`skippedResourceMiss -> 0`（在 `considered>0` 的前提下）、`resolved > 0`。**不得**要求 `considered` 变大。 |

### 3.6 验收口径（必须与用户三证明口径对齐）

> **口径纪律**：以下判据缺一不可，且**"拒绝/新增计数上升不算通过"**。
> 任何未达成的条目一律写"未通过"，不得用文字淡化。

**A. 主判据（同一 DLL、ABBA 交替、独立进程、隔离桌面）**

| # | 判据 | 读取键 | 通过条件 |
| --- | --- | --- | --- |
| A1 | core 能解析 | `semanticCoreResolved` | **> 0**（严格大于零；分段增量或 boundary 快照值，注明口径） |
| A2 | 四键失配消失 | `semanticCoreSkippedResourceMiss` | **-> 0**（同一 boundary 快照上为 0；若因 starvation 导致 `considered==0` 则本项**判为未覆盖**，不是通过） |
| A3 | 就绪度未退化 | `semanticCoreSkippedResourceNotReady` | **仍为 0** |
| A4 | 提交面出现 | `semanticCoreSubmittedDrawCount` | **> 0**（`[已核实-静态]`：`kShadowSemanticCoreSceneSubmissionEnabled = true` ⇒ core.cpp 中该值取 `buildWork->frame.draws.size()`，即 core 的 draw packet 数） |
| A5 | Arena 未溢出 | `shadowArenaFrameIncomplete` | **== 0** |
| A6 | 设备未丢 | 驱动 `deviceLost` / device.cpp 的 `m_deviceLostState` | **== false / Ok** |
| A7 | 交替有效性 | A/B/B/A 四段独立进程，除 P1 开关外矩阵相同 | 四段均 exit 0；`dllUnchanged`、`restoreOk` 全 true |

**B. 正向面（"合法对象仍投影"必须独立成立）**

- B1：**同对象/同 part/同帧关联**——必须给出"某个已知合法单位对象在 P1 后进入 `resolved` 且进入
  `draws`"的对象级证据（用 `slowestRecord*Ptr` 系列键 + 对象身份键，或控制面
  `MeetsHotFrameRequirements` 的 `minSemanticResolved` / `minSemanticSkinnedResolved`）。
  仅聚合计数上涨**不构成** B1。
- B2：`semanticCoreSkinnedResolved` 与 `semanticCoreRigidResolved` 分别给出，避免"全部来自静态世界
  caster、蒙皮单位反而消失"的情况被聚合值掩盖。
- B3：画面/user 三证明口径（"错误矩阵不再被使用 / 合法对象仍有替代路径 / 失败后可恢复"）
  按 `docs/plan/2026-09-17-p0-real-machine-execution-record.md` §3 的**同一口径**重判；
  P1 只可能改善"合法对象仍有替代路径"这一项，**不得**据此宣布另两项通过。

**C. 明确不算通过的情形**

1. `semanticSceneSubmitted > 0`（设备侧它**修复前就已非零**：P0 记录 §2.4 显示 max 178/340/340）
   —— **不得**当作 A4 的替代判据。
2. `skippedResourceMiss` 下降但 `resolved == 0`。
3. 拒绝类/新增类计数上升（含 `skippedResourceNotReady`、`skippedNoRuntimeGroupPalette`、
   `manifestCopyRejectedSkipped`）。
4. 只有静态/接线证据（`[已核实-静态]`）而没有同 DLL 实机数据。
5. `manifest` 条数上升（那是上游 starvation，属 P3，与 P1 无因果）。

**D. 必须同时记录的负面观察**

- CPU：`War3SemanticScene/CaptureContract/VisibleGeosetDemandFill` 与 `.../ManifestCopy` 的 max/总量（P1-R1）。
- 缓存规模：`shadowGeosetResourceCount` / `shadowReadyGeosetCount` / `shadowModelResourceCount` /
  `shadowRuntimeModelCount` 的分段增量（P1-R4）。
- 新增 caster 的 rawcode/kind 分类（P1-R3）。
- 新崩溃/dump、GPU incident/event 计数（沿用 P0 记录 §1 的同一套）。

### 3.7 P1 明确不做

1. **不改** `kMaxDemandFillPerCapture` / `seenMissingGeosetData` / `readyBinding` / `s_demandFillCursor`
   的语义与结构（静态测试钉死，且 P1-R1 的缓解全靠它们）。
2. **不改** `ShadowModelResourceStore::add()` 的偏好规则与 key4 语义。
3. **不改** `bindRuntimeModelAlias()`（key3 属 P2）。
4. **不改** core.cpp 的 `resolveRecord` 四键**顺序**与任何计数器语义（改动会让"四键 miss"的
   诊断指纹失效）。
5. **不改** `war3_internal_test_config.h` 的任何常量（那会打开 §1.4 中其它同样有风险的死代码）。
6. **不改**设备侧提交门（`War3ShouldSubmitSemanticPacket*`）——新增 caster 的分类问题以"观察 + 回退"
   处理，不顺手放宽提交面。

---

## 4. P2 / P3 可选硬化（各自独立验收，**不得与 P1 混验收**）

### 4.1 P2：key3 精确等值 alias

**问题**：key3 `(runtimeModelPtr, geosetIndex)` 的唯一写入者 `bindRuntimeModelAlias` 依赖
`forEachResourceStoreAlias`（缓存 `m_byModelResource` + `m_byRuntimeModel`）。实机两者都空
⇒ key3 结构性不可用（§1.3）。即使 P1 让 key1/key2 命中，key3 仍是死键；当某条记录的
`runtimeGeosetPtr`/`Data` 与缓存不一致（例如活体 geoset 槽位重排）时，key3 本可以作为回退。

**方向**（仅为候选，需独立设计文档）：在 `bindRuntimeModelAlias` 之外，允许用**精确等值**建立
runtime-model -> geoset 记录映射，即当 `(runtimeModelPtr, geosetIndex)` 的
`runtimeGeosetPtr`/`runtimeGeosetDataPtr` 与某条 geoset 记录**逐字相等**时才绑定，
并保留 `add()` 对 key4 的偏好规则不变。

**硬边界**：

- 不得引入"按 geosetIndex 猜"的回退（现有 `bindRuntimeModelAlias` 在
  `modelResourcePtr == nullptr` 时已有两轮按 `geosetIndex` 扫描的**猜测**路径；
  P2 **不得**扩大这两轮猜测的适用范围，否则就是把 §1.4 关掉的危险通路换个入口打开）。
- 不得让 key3 命中一条 `readyForShadowConsumer()` 为 false 的记录。

**独立验收**：只需 `shadowRuntimeModelCount > 0` 且 key3 命中路径可被计数观测；
**不**要求 `resolved>0`（那是 P1 的判据），也**不**允许用 P2 的结果宣布 P1 通过。

### 4.2 P3：上游 starvation（manifest 规模）修复

**问题**：manifest 抓取点读到的是**本帧写快照**（§1.5），条数 1-4 与本帧真实可见量严重不符。
交接记录称同刻已发布快照 236 条 `[待复核]`。

**方向**（仅为候选，需独立设计文档）：把 semantic core 的消费点从
`VisibleRenderableRegistry::getAllVisibleView()`（= 渲染线程写快照）改为
**已发布的只读快照**（`readSnapshot()` 语义），或把 contract capture 移到
`VisibleRenderableRegistry::endFrame()` 发布之后的安全点。
**注意**：`getAllVisibleView()` 的线程判定（`snapshotForThread()`）是为渲染线程性能设计的，
改动会影响 ManifestCopy 的成本与帧时序 ⇒ 必须独立立项、独立性能门。

**硬边界**：

- 不得改变"跨地图不复用"的 epoch 语义。
- 不得让 capture 读到**半写**的 `m_snapshots[m_writeIndex]`（双缓冲/发布序必须保持 acquire/release 配对）。
- **独立验收**：`visibleRenderableCount` / frame `visibleCount` 与 `considered` 的规模关系
  （例如 `considered` 进入与已发布可见量同量级）；**不**要求 `resolved>0`、**不**要求画面变化。
- **明确**：P3 通过**不能**用来宣布 P1 通过，反之亦然（§0.2-6）。

---

## 5. 需要上级裁定的问题清单

| Q | 问题 | 备选 | 建议 |
| --- | --- | --- | --- |
| **Q1** | P1 是**直接改默认行为**，还是先做成 **dev 门控候选**（例如新增一个默认关的 env / 编译期开关）再实机？ | (a) 直接改默认（代价：一旦有画面回归，回退要换 DLL）；(b) 先 dev 门控，ABBA 同 DLL 对照后再晋升默认 | **建议 (b)**：与库内既有 dev-only / 同 DLL A/B 的做法一致；且 Q1 的结论会改变 P1 的 diff 形态（门控分支） |
| **Q2** | 若 P0 落在 **R2/R3**（`geosetIndex` 无效或 `runtimeModelPtr == nullptr`），是否授权把 P1 从"一处谓词"扩为"两处放宽"（DemandFill 入口守卫）？ | (a) 只允许一处谓词，R2/R3 则 P1 判不可行、先修 geoset 索引解析；(b) 允许第二处最小放宽 | 需上级裁定；**(b) 必须先出独立 diff + 风险清单**，且不得顺手改静态测试锚点区间 |
| **Q3** | P1 实机是否允许用 `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD=1`（P0 的 trace 配置）连同 P1 一起跑，以放大样本？ | (a) 允许（样本更多，但帧时序被扰动）；(b) 禁止（P1 验收必须用生产默认周期） | **建议 (b)**：P1-R1（CPU storm）与 A1-A4 都必须在**生产周期**下测；`=1` 只用于 P0 的键匹配判定 |
| **Q4** | P1 的实机是否可以接受"`resolved>0` 但 `submittedDrawCount==0`"作为**阶段性**成果（并把第二道门另立工单）？ | (a) 不接受，P1 必须一次到 `submittedDrawCount>0`；(b) 接受分段，但要写明"未通过" | **建议 (a)**：用户三证明口径里 `submittedDrawCount>0` 是硬条件；分段只在**内部**记"P1a 部分达成"，对外一律记未通过 |
| **Q5** | 若 P1 引入新增 caster 且分类复核发现 doodad/地形被投影，是否立即回退 P1，还是先收窄谓词（例如补 `objectKind != Unknown`）再测？ | (a) 立即回退；(b) 收窄后重测 | 需上级裁定；**(b) 会产生新的谓词版本**，须升级为独立候选并重新走 P0 对拍 |
| **Q6** | P0 是否需要**额外**的一次性诊断出口（逐键枚举）来替代 §2.3 的"基数 + 可达性"对拍？ | (a) 不改代码，接受 §2.3 的能力边界；(b) 加一个默认关的 per-record 键 dump | **建议 (a)**：P0 的价值就是零风险；逐键枚举可作为 P1 候选**一并**落地的诊断（改代码 ⇒ 必须走候选流程） |

---

## 6. 附录 A：一手位置索引

> 全部以**函数名 / 唯一子串**为锚；行号为 2026-09-17 读码快照，会漂移。

### 6.1 `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`

| 锚（唯一子串 / 函数） | 内容 |
| --- | --- |
| `bool SemanticCoreTraceEnabled()` | 读 `DXVK_WAR3_SEMANTIC_SHADOW_TRACE`，只认 `"1"/"true"/"TRUE"` |
| `bool SemanticCoreShouldSubmitResolvedPacket(` | core 侧提交前唯一门（path-blocker / 地下 marker） |
| `auto logSkippedRecord = [&](const char* reason)` | skip 行模板（含四键字段），限流 32 + 1/512 |
| `bool ShadowRendererCore::resolveRecord(` | 四键查找与全部 skip 计数器（`skippedResourceMiss` 唯一自增点） |
| `bool ShadowRendererCore::buildFrameChunk(` | `ioStats.considered++` 与逐 record resolve |
| `bool IsMissOnlyPreviewBuild(` | `kMinMissOnlyRecordsBeforeSupersede = 64u` 的 miss-only 抑制（本场景 `considered=2`，不触发） |
| `buildWork->stats.submittedDrawCount = buildWork->frame.draws.size();` | `kShadowSemanticCoreSceneSubmissionEnabled = true` 时 core 的 submitted 取 draws.size() |
| `"DXVK SemanticCore: build-progress` | build 进度行 |
| `"DXVK SemanticCore: build manifest=` | build 完成行（**不含** skippedResourceMiss） |
| `void AppendPoseResourcePreviewSeeds(` | 由 `kShadowSemanticCoreScenePoseResourcePreviewSeedsEnabled` 守卫（= false） |

### 6.2 `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`

| 锚 | 内容 |
| --- | --- |
| `struct ManifestResolveDiagnostics` | `legacyCacheHitCount / rawScanCount / rawScanMissCount / sourceCompleteSkipCount` 等 |
| `const ManifestSourceBackingConfig& GetManifestSourceBackingConfig()` | `DXVK_WAR3_MANIFEST_SOURCE_BACKING_FAST_PATH`（必须 `"1"` 才开；默认关） |
| `const ManifestModelResourceCacheConfig& GetManifestModelResourceCacheConfig()` | `DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE`（默认 `enabled=true`） |
| `void ResolveCurrentRuntimeGeosetFromDataLegacy(` | 活体 geoset 解析：cache 命中 / raw scan / rawScanMiss；**要求 runtimeModelPtr 与 runtimeGeosetDataPtr 均非空** |
| `void ResolveCurrentRuntimeGeosetFromData(` | source-complete 短路（默认关）与可选 verifier |
| `bool IsContractUnitCandidate(` | **P1 目标谓词** |
| `bool BackfillVisibleUnitGeosetBindingFromCache(` | 只读 ready-binding 投影（静态测试钉死区间） |
| `bool TryPublishMissingVisibleUnitGeosetBinding(` | `noteRuntimeGeosetBinding` 调用者 ①（静态测试钉死区间） |
| `void DemandFillVisibleUnitGeosetBindings(` | 主循环（谓词 + 三重判空 + 64 预算 + readyBinding + 尾扫） |
| `ShadowRenderableRecord ConvertVisible(` | manifest 记录构造；末尾 `ResolveCurrentRuntimeGeosetFromData(dst, ...)` |
| `ShadowModelResourceRecord ConvertGeoset(` | geoset -> store 记录投影（key1/key2/key4 的取值来源） |
| `const auto& visibleRecords = visibleRegistry.getAllVisibleView();` | ManifestCopy 与 manifest 条数来源 |
| `const bool needsResourceBootstrap =` | `noteRuntimeGeosetBinding` 调用者 ②（bootstrap） |
| `DemandFillVisibleUnitGeosetBindings(manifest);` | **次序证明的第 2 步** |
| `resourceRevision = resourceCache.revision();` | 次序证明的第 3 步 |
| `auto buildResourceStore = [&resourceCache, &manifest]()` | store 构建（`forEachGeosetContractSource` + `forEachResourceStoreAlias`） |
| `constexpr uint64_t kResourceStoreRefreshIntervalFrames = 30u;` | 复用/冷却判定 |
| `if (resourcesPtr == nullptr) { resourcesPtr = buildResourceStore();` | 次序证明的第 6 步 |
| `kShadowSemanticCoreSceneRootUnitSupplementEnabled`（两处） | RootUnitSupplement 死代码门 |
| `void ShadowModelResourceStore::add(` / `bindRuntimeModelAlias(` / `findByRuntimeGeoset(` / `findByRuntimeGeosetData(` / `findByRuntimeModel(` / `findByModelResource(` | 四键写入与查找 |

### 6.3 `src/d3d9/war3/render/war3_visible_renderables.cpp`

| 锚 | 内容 |
| --- | --- |
| `void ResolveGeosetMetadata(` | `noteRuntimeGeosetBinding` 调用者 ③ |
| `void FinalizeVisibleRecord(` | 唯一调用点在 `appendRecord` 的 lightweight `if constexpr` 内 |
| `void VisibleRenderableRegistry::appendRecord(` | `kWar3RuntimeConfigLightweightSemanticVisibleRenderableWrites` 门 |
| `void HydrateVisibleSnapshotBasicFields(` | unit geoset hydrate（`kWar3RuntimeConfigSemanticVisibleEndFrameUnitGeosetHydrate=false`）与 `kHydrateMaxRecords = 128u` |
| `bool ShouldHydrateStaticSemanticRecord(` / `void HydrateVisibleSnapshotStaticSemanticFields(` | static hydrate（`...StaticHydrate=false`） |
| `const Snapshot &VisibleRenderableRegistry::readSnapshot() const` / `snapshotForThread()` / `writeSnapshot()` | 双缓冲快照与线程判定 |
| `void VisibleRenderableRegistry::beginFrame()` | 写快照 `records.clear()` |
| `void VisibleRenderableRegistry::endFrame()` | hydrate 门 + `m_publishedIndex.store(m_writeIndex, ...)` 发布 |
| `VisibleRenderableRegistry::getAllVisibleView() const` | manifest 条数来源（render 线程 = 写快照） |

### 6.4 `src/d3d9/war3/model/war3_model_resource_cache.{h,cpp}`

| 锚 | 内容 |
| --- | --- |
| `void ShadowModelResourceCache::noteRuntimeGeosetBinding(` | 唯一能写 geosetIndex+modelResourcePtr 的入口 |
| `void ShadowModelResourceCache::noteModelResourceBinding(` | `model bound` / `model bind rejected` 两条日志；`nullptr` 静默返回 |
| `void ShadowModelResourceCache::endFrame()` | **空函数**（仅取锁） |
| `void forEachGeosetContractSource(Fn&& fn) const` | store `add()` 的输入枚举 |
| `void forEachResourceStoreAlias(Fn&& fn) const` | `bindRuntimeModelAlias` 的输入枚举（只遍历 `m_byModelResource` / `m_byRuntimeModel`） |
| `std::vector<ShadowGeosetResourceRecord> snapshotGeosets() const;` | 已声明实现但**无调用者**（无逐键枚举出口） |
| `size_t geosetRecordCount()/readyGeosetCount()/modelResourceCount()/runtimeModelRecordCount() const` | `shadowGeosetResourceCount` 等 JSON 键的来源 |

### 6.5 `src/d3d9/war3/core/war3_internal_test_config.h`

| 常量 | 值 | 影响 |
| --- | --- | --- |
| `kWar3RuntimeConfigLightweightSemanticVisibleRenderableWrites` | **true** | `FinalizeVisibleRecord` 死代码 |
| `kWar3RuntimeConfigDisableSemanticVisibleFinalizeGeosetMetadata` | **true** | finalize 内 geoset 解析关闭 |
| `kWar3RuntimeConfigDisableSemanticVisibleFinalizeModelMetadata` | **true** | finalize 内 model 解析关闭 |
| `kWar3RuntimeConfigSemanticVisibleEndFrameUnitGeosetHydrate` | **false** | unit geoset hydrate 关闭 |
| `kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate` | **false** | static hydrate 关闭 |
| `kWar3RuntimeConfigSemanticVisibleEndFrameBasicHydrate` | **true** | 基础 hydrate 开启（数量 <=128 时才执行） |
| `kShadowSemanticCoreSceneSubmissionEnabled` | **true** | core `submittedDrawCount = draws.size()` |
| `kShadowSemanticCoreSceneUnitsOnly` | **false** | 设备侧提交走 `!unitsOnly` 三分支 |
| `kShadowSemanticCoreScenePoseResourcePreviewSeedsEnabled` | **false** | 预览 seed 死代码 |
| `kShadowSemanticCoreSceneRootUnitSupplementEnabled` | **false** | RootUnitSupplement 死代码 |

### 6.6 `src/d3d9/d3d9_device.cpp` 与 gate / 工具链

| 锚 | 内容 |
| --- | --- |
| `bool War3SemanticPublishRegistriesBeforeSceneRuntime()` | `DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE`（默认 0） |
| `uint64_t War3SemanticContractCapturePeriodRuntime()` | `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD`（默认 240） |
| `const uint64_t kSceneContractCaptureSteadyFramePeriod =` | 引用上者（capture 触发条件） |
| `if (War3SemanticPublishRegistriesBeforeSceneRuntime())` | 提前发布点 |
| `const auto preCaptureSemanticStats =` | `resolve.skippedResourceMiss != 0u` 的覆盖恢复判据 |
| `bool War3IsSemanticUnitObject(` | `objectKind == Unit` 的严格判定 |
| `bool War3IsEligibleSemanticDynamicUnit(` | 动态单位提交通路 |
| `bool War3IsEligibleSemanticStaticWorldCaster(` | 仅 Building/Destructible |
| `bool War3ShouldSubmitSemanticPacket(` | `unitsOnly` 三分支 + `explicitUnknownRigid` 逃生舱 |
| `bool War3ShouldSubmitSemanticPacketFast(` | 设备侧 `shouldSubmitDraw` 的实际谓词 |
| `bool shouldSubmitDraw(`（`War3DxvkSemanticShadowHost`） | `DXVK SemanticHostReject` 日志（仅 Skinned、前 64 条） |
| `void War3PublishSemanticRegistriesForScene()` | 转发到 `War3Renderer::PublishSemanticRegistriesForScene()` |
| `War3TryPopulateSemanticShadowScene(` / `War3ExecuteSemanticShadowSceneForValidation(` | `unitsOnly` 传入链（最终 `kShadowSemanticCoreSceneUnitsOnly`=false） |
| `m_war3SemanticDxvkBackend.configureHost(&host, unitsOnly,` | backend 提交门接线 |
| `bool War3Renderer::PublishSemanticRegistriesForScene()`（`war3_renderer.cpp`） | 内含 `ShadowModelResourceCache::instance().endFrame()`（空函数） |
| `bool IsSemanticSceneSubmissionRuntimeEnabled()`（`war3_semantic_shadow_gate.cpp`） | 默认 true（排除 legacy 兜底的关键前提） |
| `inline bool PathBlockerShouldSubmitPacket(`（`war3_path_blocker_evidence.h`） | core 侧提交门的实际实现 |

### 6.7 工具 / 出口 / 文档

| 锚 | 内容 |
| --- | --- |
| `json ToJson(const War3FrameManifestSummary& summary)`（`war3_control_plane.cpp`） | `recordsWithRuntimeModel` / `recordsWithModelResource` / `unknownCount` / `unitCount` 等键 |
| `summary.shadowGeosetResourceCount =`（`war3_shadow_runtime_bridge.cpp`） | 缓存规模键 |
| `summary.semanticCoreSkippedResourceMiss =`（同上） | 四键 miss 键 |
| `bool MeetsHotFrameRequirements(`（`war3_control_plane.cpp`） | 既有实机等待/验收谓词（`minVisibleCount` / `minStableIdentityCount` / `minResolvedGeosetCount` / `minUnitCount` / `minRuntimeModelCount` / `minModelResourceCount` / `minSemanticResolved` / `minSemanticSkinnedResolved` / `requireSemanticFrameFresh` / `requireSemanticSceneConsumed`） |
| `AutoTest/test_visible_geoset_demand_fill_dedup_static.py` | 钉死 DemandFill / TryPublish 区间 |
| `AutoTest/test_manifest_ready_geoset_binding_static.py` | 钉死 Backfill / TryPublish 区间 |
| `docs/plan/2026-09-17-p0-real-machine-execution-record.md` §2.3/§2.4/§5 | 实机现象、三轮计数、证据路径 |
| `docs/plan/2026-09-16-merge-execution-ledger.md`（2026-09-17 段） | 根因定位与"下一步顺序"（含 236 条 starvation 的说法，一手性 `[待复核]`） |
| `docs/agent-history/2026-09-17-overnight-session-summary.md` §3/§6 | 两个结构性阻塞与工单入口 |
| `docs/agent-history/2026-08-12-visible-geoset-demand-fill-dedup.md` / `2026-08-12-manifest-ready-geoset-binding.md` | DemandFill 去重与 ready-binding 投影的历史边界 |

### 6.8 本文的 `[待验证]` 汇总（实施前必须补齐）

1. §1.1 OFF 轮 `considered` / `skippedResourceMiss` 的逐值（回查 `round3_off_m2` 产物）。
2. §1.5 "已发布快照 236 条"的一手出处（`visibleRenderableCount` / `visibleCount` 快照）。
3. §2.4-R1/R2/R3：manifest 两条记录的 `runtimeGeo` / `runtimeGeoData` / `geoIdx` / `runtime` 实际取值。
4. §3.4：`geosetIndex` 是否有效、`runtimeModelPtr` 是否非空、`CaptureGeosetRecordFromKnownPtrs` 是否成功。
5. §3.5-P1-R1："5-13ms ManifestCopy storm"的原始测量记录（本树 `docs/` 内未找到，仅有 contract.cpp 注释）。
6. §1.4 调用者 ②（bootstrap）在实机是否真的被跳过，以及 `modelRegistry.recordCount()` 的值。

---

*本文档为工单/设计文档，不触发任何代码变更，不构成任何"已修复"声明。*
