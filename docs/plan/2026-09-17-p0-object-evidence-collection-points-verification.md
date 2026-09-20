# 2026-09-17 — P0 对象级证据采集点只读核实（解除附录 B 第 12 项阻塞）

> 范围：B 树 `E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914`。
> 目的：响应 `docs/plan/2026-09-17-p0-object-level-evidence-workorder.md` 附录 B 第 12 项
> （「实际提交」③ 与「实际绘制」④ 的采集点是否存在、在哪一行）以及第 11、13、16 项。
> 方法：纯只读源码核实（grep + read），逐点给 file:line 与唯一子串锚。
> **行号会漂移**，因此每条锚都附函数名或唯一子串。
>
> **无代码变更声明**：本文档为本次核实的唯一新增文件。未修改任何 `src/`、测试、`meson` 或
> 其他文档；未构建、未部署、未启动游戏、未执行任何 git 写操作。文中「已核实」**仅**指
> 「该源码位置、字段与调用关系在本树静态存在」；一切**运行时行为**结论均标「待验证」。

---

## 结论摘要

| # | 问题 | 判定 | 一句话结论 |
| --- | --- | --- | --- |
| 1 | 提交（③）采集点 | **已核实（点存在，接线未做）** | `war3_shadow_renderer_core.cpp:7471 submitFrameLimited` 与 `d3d9_device.cpp:22044 War3TryAppendSemanticShadowPacket` 的 caster 构造/append 段**同时**持有对象键与「该 packet 被提交」事实；但当前**没有任何对象级事件**，只有聚合计数与两个 last-appended 标量。 |
| 2 | 绘制（④）采集点 | **点已核实；现有输出只能到聚合层** | `d3d9_war3_shadow.cpp:3713 renderShadowMap` 的剔除点（`:4795`）与真实 draw 调用（`:5190`）持有 `War3ShadowCasterDraw`（含 renderablePart/rawcode/jHandle/mapEpoch/deviceEpoch/Selection）；但导出的只有 `drawnCasters` / `culledPerCascade` / `objectBoundsWouldCullCount` 等**无对象关联的聚合值**。 |
| 3 | manifest / canonical 身份字段 | **已核实（可复用字段清单）；生命周期身份不足** | `ShadowFrameManifest::records` 就是 `ShadowRenderableRecord`，携带四元组定位分量与 `frameSerial`；`CanonicalDrawIdentity` 另带 `visibleFrameSerial/renderFrameIndex` 与 mesh 的 `mapEpoch/immutableModelGeneration`。两者都**不含** `sessionGeneration`、`deviceEpoch`，canonical item **不保存 `skin::Selection`**。 |
| 4 | lifecycleIdentity（附录 B #11） | **不足** | shadow-core 的 A 链记录点当场唯一可得的是**模型侧不可变代际**（`resource.immutableModelGeneration` + `resource.mapEpoch`）；`render::skin::Selection` 在 shadow-core 内**当场不可得**（`packet.paletteSelection` 在 `war3_shadow_renderer_core.cpp` 无任何赋值点）；`ModelInstanceRegistry` owner identity 需额外反查。 |
| 5 | manifestFrameSerial 当场可得性（附录 B #13） | **不足，①②④需补字段** | 三帧**同时**可得且可区分的唯一位置是 ③（`buildFrameChunk` 内 `manifest.frameSerial` + native `QueryCurrentPaletteFrameTag`）与 ④ 的 device 侧 caster 构造点（但 **④ 缺 manifest 帧**）。① 函数内部**无任何帧号**；② 只有 `record.frameSerial`，且 pose-only 路径会让它与 `manifest.frameSerial` 分叉。 |
| 6 | 覆盖路径（工单 §1.5） | **已核实（该路径是实机默认路径）；若 resolved=0 必须写「未覆盖」** | 采集点落在 `IsSemanticSceneSubmissionRuntimeEnabled()==true`（编译期 true + preview 默认 true + env 默认 true）的生产链上：`buildFrameChunk → (constexpr 分支不调 submitFrame) → device BeforeUi → submitFrameLimited → War3TryAppendSemanticShadowPacket → caster 构造 → shadowCasters → renderShadowMap`。旧 P0 轮 `resolved=0` 发生在 `resolveRecord` 的四键资源查找全 miss，因此新轮仍为 0 时**只能**写「未覆盖」。 |

---

## 1. 提交（③）：哪些位置能同时拿到对象键与「该 packet 被提交」

流程方向（按源码调用顺序）：
`buildFrameChunk` 生成 draws → （`kShadowSemanticCoreSceneSubmissionEnabled` 编译期 true，**core 自己不提交**）→
device `War3TryPopulateSemanticShadowScene` → `core.submitFrameLimited` → `DxvkValidationBackend::submitDraw` →
host `submitDrawPacket` → `War3SubmitSemanticShadowPacketForBackend` → `War3TryAppendSemanticShadowPacket` →
`War3ShadowCasterDraw` 构造 → `m_war3Scene.shadowCasters.emplace_back`。

| 位置（file:line + 函数名/唯一子串） | 可得字段 | 是否含对象键 | 线程 | 证明到哪一步 |
| --- | --- | --- | --- | --- |
| `war3_shadow_renderer_core.cpp:130` `bool SemanticCoreShouldSubmitResolvedPacket(ShadowDrawPacket& packet, ShadowResolveStats& ioStats)` | `packet` 全字段；`ioStats.skippedPathBlocker/…GeometryMarker` | **是**（`packet.renderable` = 四元组定位分量） | 调用者线程（构建者） | 只证明「该 packet 通过了提交前过滤」；**不是提交事实** |
| `war3_shadow_renderer_core.cpp:7244` `size_t ShadowRendererCore::buildFrameChunk(` 内 `:7277` `const auto& record = manifest.records[index];`、`:7278` `resolveRecord(record, …)`、`:7280` `ioFrame.draws.emplace_back` | `manifest.frameSerial`（= `ioFrame.frameSerial`，`:7250-7252`）、`record.*` 全字段、`ioStats.considered/resolved/skipped*` | **是**（四元组 + `frameSerial`）；**缺** `sessionGeneration` / `mapEpoch` / `deviceEpoch` 字段（mapEpoch 需另取） | **所有者线程**（`ensureFrameBuiltForContract:9586` → `consumePermissionGranted(true):9424` 取 `hooks::GetMainLoopThreadId()`；非所有者只计数拒绝 `:9436`） | 证明「该 record 进入本帧 build 的 draw 列表」；**不等于**提交给后端 |
| `war3_shadow_renderer_core.cpp:7466/7471` `submitFrame` / `bool ShadowRendererCore::submitFrameLimited(` 内 `:7477` `for (const auto& draw : frame.draws)`、`:7487` `backend.submitDraw(draw, …)`、`:7490` `++submittedCount` | `frame.frameSerial`（= manifest 帧）、`draw.renderable.*`、`draw.paletteSelection`、`draw.runtimeGroupPaletteSlotIndex/MinFrameTag/MaxFrameTag`（`war3_shadow_renderer_core.h:120-127`） | **是**（最完整的对象键之一） | 调用者线程；本树的三个调用者见下行 | 证明「该 packet 被后端 `submitDraw` 且 `submittedCount` 递增」；这是 ③ 语义最干净的采集点 |
| `war3_shadow_renderer_core.cpp:9734` `if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneSubmissionEnabled)` | 该分支内 `:9735` `submittedDrawCount = draws.size()`；**`:9738 m_core.submitFrame(...)` 属 else 死分支** | — | — | **已核实**：当前构建（`war3_internal_test_config.h:788` = `true`）下 shadow-core 自身不调用 `submitFrame`；旧轮 `submittedDrawCount` 的语义必须按此重新解释，**不得**把它当后端提交数 |
| `d3d9_device.cpp:8218` `bool submitDrawPacket(const ShadowDrawPacket& packet) override`（`:8228` 调 `m_device.War3SubmitSemanticShadowPacketForBackend(packet)`）；`d3d9_device.h:3187` 该函数 = `War3TryAppendSemanticShadowPacket(packet)` | `packet` 全字段 | **是** | 渲染线程（`BeforeUi` → `:20003 War3TryPopulateSemanticShadowScene`） | 证明「submitDraw 之后进入 append」 |
| `d3d9_device.cpp:22044` `bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(`（22044-24975）：`:23080 BuildCanonicalShadowDrawItem(`；`:24049` `War3ShadowCasterDraw draw = {};`；`:24050 draw.mapEpoch` / `:24051 draw.deviceEpoch` / `:24052 draw.shadowRenderablePart`；`:24062 draw.inputSkinSelection`；`:24381 draw.stage` / `:24383 draw.objectKind` / `:24385 draw.rawcode` / `:24386 draw.jHandle`；`:24640/24641` `shadowInstances/shadowCasters.emplace_back` | renderablePart、rawcode、jHandle、mapEpoch、deviceEpoch、layerIndex、stage、objectKind、`inputSkinSelection`（含 frameTag/slot/ownerEpoch/publicationTicket）、`inputEvidenceProvenance[6]`=immutableModelGeneration、pathBlocker | **是（最完整：四元组定位 + map/device epoch + Selection）** | 渲染线程（同上） | 证明「该 packet 被判定为 caster 并追加进 `shadowCasters` 绘制列表」。**这是生产 ③ 的最佳采集点** |
| `war3_shadow_backend_dxvk.cpp:134` `bool DxvkValidationBackend::submitDraw(` → `:146 noteSubmittedIdentity(packet);` 与 `war3_shadow_backend_dxvk.cpp:52-65`；访问器 `war3_shadow_backend_dxvk.h:40-51` | `submittedHandles`(jHandle) / `submittedWorldObjectEntries` / `submittedSceneNodes` / `submittedRuntimeModels` + `submittedDrawCount` | **部分**：四个**互相独立**的集合，彼此无配对，也不是逐 draw 记录 | 渲染线程 | 证明「本帧提交集合包含这些值」；**不足**：不能回答「某 jHandle 对应的那次提交是用哪个 runtimeModel/part」 |
| `d3d9_device.cpp:24646` `m_war3Scene.shadowStats.semanticSceneLastAppendedRenderablePart` / `:24648 …LastAppendedMeshData` | 仅 renderablePart、meshData | 仅 2 个标量 | 渲染线程 | **只能到聚合层**：只有「最后一个」，无逐对象序列、无 jHandle/rawcode、无帧号 |
| `war3_shadow_native_runtime.cpp:216` `bool NativeD3D9BackendRuntime::buildCanonicalFrame(`（`:222 outFrame.frameSerial`；`:234-239` packet 身份重建）→ `:386 m_core.submitFrame(canonicalFrame, m_backend)` | canonical identity 全字段 + `frameSerial` | **是** | `buildLatestFrame`（`:350`）的调用者，函数内持 `m_mutex` | 证明 native canonical 路径的提交点存在；与 `d3d9_device.cpp:32620` 是**不同**路径，必须分别判定，不得合并计数 |

**③ 判定**：
- 附录 B 第 12 项问「采集点是否存在、在哪一行」→ **存在**，主锚为
  `war3_shadow_renderer_core.cpp:7471 submitFrameLimited`（`:7477` 循环 / `:7487` submitDraw）
  与 `d3d9_device.cpp:22044 War3TryAppendSemanticShadowPacket`（`:24049` caster 构造 / `:24641` append）。
- 问「是否已具备」→ **不具备**：本树**没有任何对象级的提交事件**；只有聚合计数与两个 last-appended 标量。
- 口径提醒：`war3_shadow_renderer_core.cpp:9738` 的 `m_core.submitFrame` 在 `if constexpr` 下**被编译掉**，因此
  「shadow-core 提交计数」在本构建中**不代表后端提交**（§5.4「接线完成 ≠ 证明」）。

---

## 2. 绘制（④）：caster 是否真的进入 shadow map 绘制 / 被剔除

主锚：`d3d9_war3_shadow.cpp:3713` `bool War3ShadowReceiverPass::renderShadowMap(`（函数体覆盖 3713-5241+）。

| 位置（file:line + 函数名/唯一子串） | 可得字段 | 是否含对象键 | 线程 | 证明到哪一步 |
| --- | --- | --- | --- | --- |
| `d3d9_war3_shadow.cpp:3713` `renderShadowMap(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input, const std::vector<const War3ShadowCasterDraw*>* replayDrawOverride)`；`:3740 input.frameSerial` | `input.frameSerial`（渲染帧）+ 每条 `*replayDraws[i]` 的 `War3ShadowCasterDraw` 全字段 | **是**（每条 draw 一个对象键） | 命令录制线程（渲染线程） | 证明「caster 进入 CSM 渲染函数」 |
| 剔除决策：`d3d9_war3_shadow.cpp:4314` `const auto evaluateBoundsPolicy = [&](const War3ShadowCasterDraw& draw)`；`:4460 const auto boundsPolicy = evaluateBoundsPolicy(draw);`；`:4473-4483` `objectBoundsCandidateCount / objectBoundsProofAcceptedCount / objectBoundsFailVisibleCount / objectBoundsRejectReasonHistogram`；`:4485-4530` `actualMask`；`:4532 const auto cascadeVisible = [&](uint32_t drawIndex, uint32_t cascadeIdx)`；`:4795 if (!cascadeVisible(i, c)) { culled++; }` | `draw.boundsProvenance`（`d3d9_war3_scene.h:437`）、`draw.boundsIdentityProven`（`:441`）、`draw.boundsSourceGeneration/FrameSerial`、`draw.objectKind`、`draw.stage`、`draw.vertexBlendEnabled`，以及 `War3EvaluateBoundsCullEvidence(...)` 的 `mayCull` + `rejectReason`（**具名剔除依据**） | **是**（`draw` 含 renderablePart/rawcode/jHandle） | 命令录制线程 | 采集点存在且剔依据是**具名字段**；但现有输出**只能到聚合层**（无对象关联）。**待验证**：默认 `DXVK_WAR3_OBJECT_BOUNDS_CULL_CONSUME` = false（`:4305-4307`）⇒ 对象级 cull 默认只 Observe |
| 真实绘制：`d3d9_war3_shadow.cpp:5190` `ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex, draw.vertexOffset, 0);` / `:5194` `ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);` / `:5222 drawnCasters++; :5223 cascadeDrawn++` | `draw` 全字段（renderablePart `:401`、rawcode `:396`、jHandle `:397`、`inputSkinSelection` `:407`、mapEpoch/deviceEpoch `:262/263`、stage/objectKind `:386/390`） | **是** | 命令录制线程 | 这是 `renderShadowMap` 主 caster 循环内的**真实 shadow map draw 调用点**；但 `drawnCasters`（`:5522`）与 `drawnPerCascade`（`:5235`）**只能到聚合层**。**注意（自检 3 实测）**：`cmdDrawIndexed` 另在 `:5497/5500` 与 `:8579/8583` 出现 ⇒ 「实际绘制」不能只认 `:5190/5194` |
| 聚合导出链：`d3d9_war3_shadow.cpp:9472 reconciliation.shadowMapDrawnCasters`、`:9556-9563` 各级联；`d3d9_war3_scene.h:1705-1708 semanticSceneShadowMapCascade0..3CulledCount`；`d3d9_device.cpp:20242-20314 / 33122-33157` 搬运 | 无对象键 | **否** | — | **只能到聚合层** |
| 另一绘制列表（弱身份）：`d3d9_device.cpp:20076/20077` Stage13 retention replay 的 `shadowInstances/shadowCasters.emplace_back`；instance 字段见 `:20062-20075` | `geometryId/materialId/replayDrawIndex/batchHandle/paletteIndex/worldMatrix/boundsCenter/boundsRadius/category/objectKind` | **不足**（无 renderablePart/jHandle/rawcode） | 渲染线程 | 证明「有一段 caster 被重放」；**不能**关联到对象键 |

**④ 判定**：
- 「该 caster 真的进入 shadow map 绘制」的**真实调用点存在**：`d3d9_war3_shadow.cpp:5190`。
- 「被剔除」的**具名依据字段存在**：`boundsProvenance` + `War3EvaluateBoundsCullEvidence.rejectReason`，判定在 `:4460`/ `:4530`，消费在 `:4795`。
- 但**现有导出只能到聚合层**；要逐对象闭合必须新增采集点。
- **待验证**：按 `:4305-4316` 与 `AGENTS.md` 2026-08-09 条目，generation-backed 对象 bounds 默认 Observe-only（B 树 `AGENTS.md:198-203`，详见 `docs/agent-history/2026-08-09-object-bounds-fail-visible.md`），
  因此默认配置下「对象被实际剔除」**可能不发生**；报告中不得把 would-cull 写成 applied-cull。
- `evaluateBoundsPolicy(draw)` 在 `:4460` 与 `:4556` **各调用一次**（`:4556` 属 union-cull Observation-only 段，
  见 `:4540-4545` 注释「Observation-only admission stage … cannot change … any submitted draw」）⇒
  新增采集点必须显式选择其中一处；**Observe 段判定不等于实际剔除**。

---

## 3. manifest / canonical 侧：records 是否携带对象键、可复用身份字段

| 位置（file:line + 唯一子串） | 可得字段 | 是否含对象键 | 线程 | 证明到哪一步 |
| --- | --- | --- | --- | --- |
| `war3_shadow_runtime_contract.h:64 struct ShadowFrameManifest {`（`:70` `std::vector<ShadowRenderableRecord> records;`；`:65 frameSerial`、`:66 publishRevision`） | `frameSerial` + `publishRevision` + 逐 record 身份 | **是** | 发布者（capture 在渲染帧边界） | records **就是**对象键载体；同键反查可行（对 `records` 按 renderablePart/runtimeModelPtr 线性匹配，O(n)） |
| `war3_shadow_runtime_contract.h:20 struct ShadowRenderableRecord {` | `renderablePart`(`:24`)、`runtimeModelPtr`(`:28`)、`jHandle`(`:34`)、`rawcode`(`:35`)、`modelKey`(`:32`)、`unitPtr`(`:23`)、`worldObjectEntry`(`:21`)、`sceneNode`(`:22`)、`runtimeGeosetPtr`(`:30`)、`runtimeGeosetDataPtr`(`:31`)、`geosetIndex`(`:42`)、`frameSerial`(`:49`)、`objectKind`(`:43`)、`stage`(`:46`) | **是**（四元组齐全） | — | 提供 §2.1 的**定位分量**；**不含** `sessionGeneration` / `mapEpoch` / `deviceEpoch` |
| `war3_shadow_runtime_contract.h:78 struct ShadowModelResourceRecord {` | `mapEpoch`(`:95`)、`immutableModelGeneration`(`:96`)、`contentHash`(`:94`)、`modelKey`(`:82`)、`modelResourcePtr`(`:80`) | 需按 runtimeModelPtr/geoset 键反查 | — | 提供「会话/代际」中的 mapEpoch 与**模型不可变代际**（lifecycleIdentity 的候选 B，见 §4） |
| `war3_canonical_draw.h:64 struct CanonicalDrawIdentity {` | worldObjectEntry/sceneNode/unitPtr/renderablePart/meshDataPtr/runtimeModelPtr/modelResourcePtr/runtimeGeosetPtr/runtimeGeosetDataPtr/`jHandle`(`:74`)/`rawcode`(`:75`)/`modelKey`(`:76`)/objectKind(`:77`)/`frameSerial`(`:78`)/`visibleFrameSerial`(`:79`)/`renderFrameIndex`(`:80`)/poseMatrixCount(`:81`)/poseMatrixHash(`:82`) | **是**（四元组 + `frameSerial` + `visibleFrameSerial` + `renderFrameIndex`） | — | 三帧里可同时拿到 **record 帧** 与 **visible 帧**；**不含** session/device epoch |
| `war3_canonical_draw.h:267 struct CanonicalShadowDrawItem {`（`:268 instance`、`:269 legacyPath`、`:274 readinessReason`） | identity + mesh + material + skin + worldTransform + readinessReason | 继承 identity | — | `readinessReason` 是**具名拒绝原因**（`war3_canonical_draw.cpp:7-30` 有 `CanonicalShadowReadinessReasonName`） |
| `war3_canonical_draw.cpp:32 bool BuildCanonicalShadowDrawItem(` | `:50-64` 逐字段从 `packet.renderable` 复制；`:69 instance.identity.frameSerial = packet.renderable.frameSerial;`；`:73-77` visibleFrameSerial/renderFrameIndex 取自 `currentDrawSample->contract`；`:89-90 mesh.mapEpoch/immutableModelGeneration` | **是** | 调用者线程（device 渲染线程） | 直接从 packet 复制，无新增语义；`mesh.mapEpoch` 给出 map 代际 |
| `war3_canonical_draw.h:295 CanonicalShadowBuildInputs` 的 `:306 skin::Selection selectedPalette` + `:307 hasSelectedPalette`；使用点 `war3_canonical_draw.cpp:162-166`、`:220`、`:262-263` | Selection **只参与门控/校验** | — | — | **已核实**：`selectedPalette` **不写入** `out`（canonical item 无 Selection 字段）⇒ 不能从 canonical item 反查生命周期身份 |
| 发布/消费点：`war3_shadow_native_runtime.cpp:133 publishCanonicalDraw`、`:172 publishCanonicalDrawPrepared`；device 回调 `d3d9_device.cpp:24828 .publishCanonicalDrawPrepared(` | canonical item 全字段 + `m_canonicalFrame.frameSerial`（`:162/206`） | **是** | 调用者线程；`m_mutex` 保护 | 证明 canonical 侧「已发布」事实；帧号是 **canonical 帧**，不等于 manifest 帧（`:222-223 outFrame.sourcePublishRevision = m_canonicalFrame.frameSerial`，注意此处把 frameSerial 当 publishRevision 用） |

**③ 判定**：records 携带对象键（四元组定位分量 + record 帧）；canonical item 可复用
identity 全字段 + `visibleFrameSerial/renderFrameIndex` + `mesh.mapEpoch/immutableModelGeneration/contentHash`。
**不足**：无 `sessionGeneration` / `deviceEpoch`；无 `skin::Selection`。

---

## 4. lifecycleIdentity（附录 B 第 11 项）：哪一个既有契约可作「可证明的生命周期身份」

| 候选（file:line + 唯一子串） | 语义 | 它证明到哪里 | 它**不能**证明什么 | A 链当场可得？ |
| --- | --- | --- | --- | --- |
| **A. `render::skin::Selection`** `war3_skin_palette_selection.h:20 struct Selection {`；`:25 uint64_t ownerEpoch,publicationTicket,captureSerial,hash;`；`:27 slot/frameTag`；自述 `:16-19`「CPU publication contract, NOT an invented native slot lease」；`:37 OwnedSnapshotMatches`、`:54 CanReplace` | 一次 **CPU 发布拷贝**的票据：source/space/domain + runtimeModel/part + ownerEpoch（所有者代际）+ publicationTicket（命名不可变拷贝）+ captureSerial（CapturedWriter 来源）+ hash（已算好的摘要）+ frameTag（native palette 帧戳） | 同一 part/runtimeModel/ownerEpoch 下**同一份已发布拷贝**（hash 相等）；`OwnedSnapshotMatches` 可判「同帧同 slot 同 ownerEpoch」 | **arena 字节属于该对象**——`slotAllocationGeneration` 恒为 0（`:26`，无 native allocator witness）⇒ §5.6 ③ 仍不闭合；也**不能**证明『不是别人写的字节』 | **否**。`packet.paletteSelection` 在 `war3_shadow_renderer_core.cpp` **无任何赋值点**（grep 0 命中）；赋值只在 device 侧：`d3d9_device.cpp:7501/7518/7552/7568`、`:22500-22501`、`:23044`、`:28693`、`:32702` |
| **B. 模型不可变代际** `war3_shadow_runtime_contract.h:96 uint64_t immutableModelGeneration` + `:95 mapEpoch`；判等先例 `d3d9_device.cpp:5692/5808/5824`、`:22026-22031`（owner->mapEpoch/immutableModelGeneration/geosetIndex/modelKey/contentHash 全等才认租约） | 「同一 map 会话内，同一次不可变模型发布」 | 该**模型资源**（几何/索引）在同一 mapEpoch 下未被替换；可作为跨帧「模型身份未换代」的证据 | palette/arena 字节的所有权（代际只覆盖不可变模型资源，不覆盖 palette arena）；也不能定位到具体**游戏对象**（unit/jHandle 可能被复用） | **② 可得**：`TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord& resource, …, const ShadowRenderableRecord& renderable, …)`（`war3_shadow_renderer_core.cpp:6619`）参数含 `resource`。**① 不可得**：`FindOrUpdatePaletteSlotCache`（`:794`）参数只有 `renderablePart/currentSlotIndex/requiredPaletteCount` |
| **C. `ModelInstanceRegistry` owner identity** `war3_model_registry.h:302 void noteRuntimeOwnerIdentity(`；记录结构 `:33 struct ModelInstanceRecord {`（worldObjectEntry/sceneNode/unitPtr/spritePtr/jHandle/rawcode/modelKey）；写入点示例 `war3_model_hook.cpp:1810` | runtimeModelPtr → 游戏对象身份的既有映射 | 某 runtimeModel 的**所有者对象身份**（用于把模型实例绑到 unit/jHandle/rawcode） | **代际**：无 epoch/generation 字段；`:51-54` 注释明确 `lastSeenFrame` 不足以做同帧去重（会被多个 hook 刷新）⇒ 不能单独承担跨帧身份 | **需额外反查**（registry 锁 + 按 runtimeModelPtr 查询），不在 A 链现有字段中；引入锁与新开销 |
| **D. Stage13 常驻几何血统**（内容 key + bucket 提示 + 私有字段/字节 proof；锚 `docs/plan/2026-09-16-stage13-port-audit.md:12`「U4 Stage13 content-persistent geometry（默认开、内容快照+私有字段/字节 proof、64MiB CPU cap）」；桶提示口径见工单 `docs/plan/2026-09-17-p0-object-level-evidence-workorder.md:231-233`） | 静态桥/斜坡的**几何内容相等**证明 | 「视野重入时命中的是同一份内容快照」 | 不是通用的**对象/模型生命周期**身份；不覆盖动态单位/palette；本工单不得把它当 `lifecycleIdentity` | 不适用 |

**判定：不足。** 按 §2.1「哪个来源在记录点当场可得，须在 Step 1 冻结」：
- ②（`TryBuildRuntimeGroupPalette` 内 / 其直接调用者 `resolveRecord`）当场可得的是 **B**。
- ①（`FindOrUpdatePaletteSlotCache` 内）**A/B/C 全都不可得** ⇒ 按 §2.1 必须记 `identityWeak`，
  **不得**用于跨帧恢复验收，也不得单独作正例/反例。
- 要使用 **A**，必须在 Step 1 把 `skin::Selection` 从 device 侧（`d3d9_device.cpp:23044`）透传到 A 链或 ③ 的事件载荷（新增字段/参数）。
- `identityWeak` 比例必须在报告中**单独列出**（附录 B #11 明文要求）。

---

## 5. manifestFrameSerial 当场可得性（附录 B 第 13 项）

三帧来源（§2.1）：
- `currentFrameSerial` = `war3::state::RenderState::instance().getFrameIndex()`（先例 `d3d9_device.cpp:23157`）
- `manifestFrameSerial` = `manifest.frameSerial`（`war3_shadow_runtime_contract.cpp:3689 manifest.frameSerial = visibleRegistry.getFrameNumber();`；导出侧 `war3_shadow_runtime_bridge.cpp:7926 summary.semanticCoreManifestFrameSerial = publishedManifest->frameSerial;`）
- `nativePaletteFrameTag` = `QueryCurrentPaletteFrameTag()`（`war3_model_hook.cpp:9197` → `TryReadCurrentPaletteFrameTag` `:353`，读 `g_gameBase + 0xBDA4CCu` `:359`）

| 位置 | currentFrameSerial | manifestFrameSerial | nativePaletteFrameTag | 判定 |
| --- | --- | --- | --- | --- |
| ① `war3_shadow_renderer_core.cpp:794 FindOrUpdatePaletteSlotCache(` 内（`:820-901`） | **不可得**（函数内无 RenderState 调用） | **不可得**（无参数、无帧字段；`PaletteSlotCacheEntry::lastUpdateFrame` `:757` 在 `:817/910` 实际写入的是 **hit 计数**，不是帧号） | **可得**：`:850-852 QueryCurrentPaletteFrameTag(currentPaletteFrameTag)`；另 `:831 boundFrameTag`、`:858-860 slotRangeMin/Max/Missing` | **不足**：① 只能拿到 native 帧 + 逐槽帧范围 |
| ①' 调用方 `war3_shadow_renderer_core.cpp:6662 auto tryEngineDirectPosePalette = [&]() -> bool {`（`:6679 snapshotFrameTag`、`:6722 FindOrUpdatePaletteSlotCache(...)） | 需新增调用 | 只有 `renderable.frameSerial`（**可能 ≠ manifest.frameSerial**，见下行分叉） | **可得** | **不足**：无法把 record 帧与 manifest 帧区分 |
| ② `war3_shadow_renderer_core.cpp:6619 bool TryBuildRuntimeGroupPalette(`（参数含 `const ShadowRenderableRecord& renderable`） | 需新增调用 | 只有 `renderable.frameSerial` | **可得** | **不足**（同 ①'） |
| ③ `war3_shadow_renderer_core.cpp:7244 buildFrameChunk(`（`:7250 if (ioFrame.frameSerial != manifest.frameSerial)`） | 需新增调用（RenderState 可全局调用） | **当场可得**：`manifest.frameSerial` / `ioFrame.frameSerial` | **可得**（QueryCurrentPaletteFrameTag 可调用） | **可得且可区分**（三者同时可得） |
| ③ `war3_shadow_renderer_core.cpp:7471 submitFrameLimited(` | 需新增调用 | **当场可得**：`frame.frameSerial`（`:7474 backend.beginFrame(frame.frameSerial)`） | **可得且更细**：`draw.runtimeGroupPaletteMinFrameTag/MaxFrameTag`（`war3_shadow_renderer_core.h:125-126`）、`draw.paletteSelection.frameTag` | **最佳**：③ 三者齐全 |
| ④ device 侧 caster 构造 `d3d9_device.cpp:24049` | **可得**：`:24056 inputEvidenceProvenance[5] = m_war3ShadowPersistentFrameSerial`；`:23157 getFrameIndex()` 先例 | **不可得**（`War3ShadowCasterDraw` 无 manifest 帧字段；须新增） | **可得**：`:24062 draw.inputSkinSelection = selectedPalette`（含 `frameTag`），但**仅当 `war3::tools::evidence::InputsEnabled()`**（`:24053`） | **不足**：④ 缺 manifest 帧 |
| ④ CSM 渲染 `d3d9_war3_shadow.cpp:3713 renderShadowMap`（`:3740 input.frameSerial`）、剔除 `:4795`、draw `:5222` | **可得**：`input.frameSerial` | **不可得** | 间接：`draw.inputSkinSelection.frameTag`（同 ④ 侧，受 InputsEnabled 门控） | **不足**：只能到「渲染帧 + native 帧」两个 |

**关键分叉（必须写进报告口径）**：`war3_shadow_runtime_contract.cpp:4253-4254`
```cpp
auto liveManifest = std::make_shared<ShadowFrameManifest>(*manifestPtr);
liveManifest->frameSerial = poseFrameSerial;
```
pose-only 捕获会**改写 manifest 帧号而 records 保留旧 frameSerial**。因此
**不得**用 `record.frameSerial` / `packet.renderable.frameSerial` 冒充 `manifestFrameSerial`；
只有 ③ 的 `manifest.frameSerial` / `frame.frameSerial` 是该帧真正构建的 manifest 帧。
正常路径下 `ConvertVisible(record, manifest.frameSerial, …)`（`:3889`）使二者相等，
但该等式**依赖路径**，不能作为身份依据。

---

## 6. 覆盖路径（工单 §1.5）：新采集点是否落在实机真的会走的路径上

**实机默认性（已核实，静态）**：
`war3_semantic_shadow_gate.cpp:49 bool IsSemanticSceneSubmissionRuntimeEnabled()` =
`kShadowSemanticCoreSceneSubmissionEnabled`（`war3_internal_test_config.h:788` = `true`）
&& `IsSemanticShadowPreviewEnabled()`（`:40` 默认 `true`）
&& `EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION", true)`（`:53-54`）
⇒ **默认 true**。
口径提醒：`IsSemanticSceneEndFrameBuildRuntimeEnabled()`（`:69-72`，`ENDFRAME_BUILD` 默认 false）
**不是**本路径的前置；不得把 `ENDFRAME_BUILD`/`PUBLISH_REGISTRIES_BEFORE_SCENE` 的结论外推（工单 §5.4 Q-D）。

**实际链路（按源码调用顺序，即新采集点覆盖的路径）**：

1. `war3_shadow_runtime_contract.cpp:3674 captureLiveState()` → `:3689 manifest.frameSerial = visibleRegistry.getFrameNumber()`
2. `:3889 ConvertVisible(record, manifest.frameSerial, …)` → `:4172` 发布 manifest → `:4196 requestLatestFrameBuild()`
3. `war3_shadow_renderer_core.cpp:9468 ensureLatestFrameBuilt()` → `:9478 consumePermissionGranted(false)`（所有者门） → `:9579 ensureFrameBuiltForContract(` → `:9586` 门（`directEntry=true`）
4. `:9649 buildWork->nextRecordIndex = m_core.buildFrameChunk(` → `:7244 buildFrameChunk(` → `:7278 resolveRecord(record, …)`
5. `resolveRecord` 资源四键查找 `:7596-7615`；全 miss ⇒ `:7629 ioStats.skippedResourceMiss++` → `return false`（`:7632`）
6. **`:9734 if constexpr (kShadowSemanticCoreSceneSubmissionEnabled)`** ⇒ `:9735` 只置 `submittedDrawCount`，**不调 `submitFrame`**（`:9738` 是死分支）
7. `:9747-9750` 发布 `m_lastFrame` / `m_lastRenderableFrame`
8. `d3d9_device.cpp:31644 D3D9DeviceEx::War3TryPopulateSemanticShadowScene(` → `:32268/`:32293 snapshot` + `:32283-32300` 选帧 → `:32578 War3DxvkSemanticShadowHost host(*this);` → `:32620 core.submitFrameLimited(*frame, m_war3SemanticDxvkBackend, submitCap);`
9. `war3_shadow_backend_dxvk.cpp:134 submitDraw(` → `:143 m_host->submitDrawPacket(packet)` → `d3d9_device.cpp:8218 submitDrawPacket(` → `:8228`
10. `d3d9_device.h:3187 War3SubmitSemanticShadowPacketForBackend` = `War3TryAppendSemanticShadowPacket(packet)` → `d3d9_device.cpp:22044`
11. `:24049` caster 构造 → `:24641 m_war3Scene.shadowCasters.emplace_back(std::move(draw));`
12. `d3d9_war3_shadow.cpp:3713 renderShadowMap(` → 剔除 `:4795` / 真实 draw `:5190`/`:5194` / 计数 `:5222`

**旧 P0 轮 `resolved=0` 的落点**：第 5 步（`war3_shadow_renderer_core.cpp:7590-7633`）。
该步返回 false ⇒ `:7280` 不入 draws ⇒ 第 8 步 `frame->draws.empty()` ⇒ `submitFrameLimited` 循环 0 次
⇒ 第 11 步无 caster ⇒ 第 12 步无绘制。**这是「同一对象键四步」在旧轮整体缺失的原因**。

**如实写法（强制）**：
- 新轮若该路径仍 `resolved=0` / 无对象级事件 ⇒ 报告必须写「**未覆盖**」，
  **不得**写「拒绝计数为 0 所以没有错误矩阵」（工单 §1.5 第 3 条）。
- 新轮若有事件 ⇒ 必须同时给出当轮「覆盖路径」证据：`resolved` / `submitted` 计数
  与对象级事件数的**对应关系**，否则仍算未覆盖。
- **禁止的替代**：不得为产生样本而放宽 `IsContractUnitCandidate` 或
  `IsVisibleDirectGeosetUnitCandidate`（工单 §0.2 / §1.5 第 4 条）。

---

## 仍无法闭合的点（不得把「接线」写成「已证明」）

1. **③④ 的对象级事件在本树不存在。** 现有产物只有聚合计数（`drawnCasters`、`culledPerCascade`、
   `objectBoundsWouldCullCount`、`semanticSceneShadowMapCascade0..3CulledCount`）与两个
   last-appended 标量（`d3d9_device.cpp:24646-24649`）。不新增采集点就无法闭合 §5.2 第 3、4 条。
2. **① 内部拿不到对象键，也拿不到具名拒绝原因。** `FindOrUpdatePaletteSlotCache`（`war3_shadow_renderer_core.cpp:794`）
   只接收 `renderablePart`；R0..R3 的区分发生在函数内部（`:886-899`），返回给调用方（`:6722`）的只有
   `0xFFFFFFFF`。逐对象记录具名拒绝原因**必须新增 out 参数**（reason + boundSlotIndex/boundGroupCount/
   boundFrameTag/slotRangeMin/Max/Missing/currentPaletteFrameTag）。
3. **`deviceEpoch` 在 shadow-core 内不可得。** `m_war3GpuSkinDeviceEpoch` 是 `D3D9DeviceEx` 成员
   （`d3d9_device.h:1907`）；`War3GpuSkinResources::deviceEpoch()`（`war3_gpu_skin_resources.h:428`）
   需要句柄。⇒ ①②③ 只能记 0 + `epochUnknown`。注意**既有先例也记 0**：
   `d3d9_device.cpp:23158` 的 `event.key` 第三/四槽为 `mapEpoch, 0u`；而该处 `m_war3GpuSkinDeviceEpoch` 其实可访问。
4. **`manifestFrameSerial` 在 ① 与 ④ 不可得；② 的替代（`record.frameSerial`）会分叉。**
   `war3_shadow_runtime_contract.cpp:4253-4254` 会改写 `liveManifest->frameSerial` 而 records 保留旧值 ⇒
   二者不可互推。④ 的 `War3ShadowCasterDraw`（`d3d9_war3_scene.h:257`）没有 manifest 帧字段。
5. **`lifecycleIdentity` 当场不足。** A 链唯一当场可得的是模型 `immutableModelGeneration`（② 经 `resource` 参数）；
   `skin::Selection` 需要新增跨层传递（现在只用于门控，见 `war3_canonical_draw.cpp:262-263`）。
   两者都不可得时按 §2.1 记 `identityWeak`，其**比例必须单独列出**。
6. **④ 的「剔除」默认可能不发生。** `d3d9_war3_shadow.cpp:4305-4307` 的
   `DXVK_WAR3_OBJECT_BOUNDS_CULL_CONSUME` 默认 false ⇒ 对象级 bounds cull 默认只 Observe；
   `objectBoundsWouldCullCount` **不等于** applied cull。不得写成「该对象被剔除」。
7. **`slotAllocationGeneration` 恒 0**（`war3_skin_palette_selection.h:26`）⇒ 即使采到 Selection，
   §5.6 第 ③ 层（对象所有权）仍不能闭合；本轮也没有内容哈希可比对（裁定 4）。
8. **「无事件」不等于「无拒绝」。** 线程门拒绝只加计数不产生事件：
   `g_semanticBuildOffThreadRefusedCount`（`war3_shadow_renderer_core.cpp:9436`）、`…OwnerUnestablishedRefusedCount`（`:9433`）、
   `…DirectAdvanceRefusedCount`（`:9440`）。报告必须把这三者与对象级事件并列，否则会把「构建根本没推进」读成「路径健康」。
9. **旧轮 `resolved=0` 是否已修复：本轮未重跑，待验证。** 本核实只读源码，不得断言资源键已修好。
10. **`evidence::Record` 的 thread 戳自动写入**（`war3_frame_evidence.cpp:175 event.thread=GetCurrentThreadId();`）
    是**现成能力**，但「同一对象键的拒绝/提交/绘制事件发生在同一线程」仍需**逐轮实机原始线程 id** 记录后才能判读，
    不得由架构推断（工单 §4.2）。

---

## 给 Step 1 的最小落地建议

**先做（不改生产语义，只加默认关的记录）**：

| 优先 | 动作 | 锚点 |
| --- | --- | --- |
| P1 | ③ 采集点落在 `War3TryAppendSemanticShadowPacket` 的 caster 构造/append 段（`:24049` 之前取字段，`:24641` 之后发事件），因为此处对象键最全（renderablePart+rawcode+jHandle+mapEpoch+deviceEpoch）且 **append 本身就是提交事实** | `d3d9_device.cpp:24049`、`:24641` |
| P1 | ④ 采集点落在 `renderShadowMap` 的剔除判定（`:4530` 写 mask 后 / `:4795` 消费处，带 `boundsPolicy.rejectReason`）与真实 draw（`:5222 drawnCasters++` 处） | `d3d9_war3_shadow.cpp:4530`、`:4795`、`:5222` |
| P1 | 采集条件按 §2.2：**只对 watchlist 中的对象键**各阶段各记一条；不逐成功记录；环/探测上限独立计数 | 工单 §2.2/§2.5 |
| P2 | ③（核心侧）若要用 `submitFrameLimited` 作交叉核对，注意该函数在非 semantic 后端（native `war3_shadow_native_runtime.cpp:386`）也会跑，**两条路径各自计数**，不得合并 | `war3_shadow_renderer_core.cpp:7471`、`war3_shadow_native_runtime.cpp:386` |

**必须先补字段/签名（否则只能写「未覆盖 / 不足」）**：

| 优先 | 必须补的字段/签名 | 为什么 |
| --- | --- | --- |
| P0 | `FindOrUpdatePaletteSlotCache` 新增 out 参数：R0..R3 具名原因 + `boundSlotIndex/boundGroupCount/boundFrameTag/slotRangeMin/Max/Missing/currentPaletteFrameTag` | 现在调用方只看到 `0xFFFFFFFF`；不补就无法逐对象写具名拒绝原因（§2.1 拒绝原因 R0..R3） |
| P0 | 把 `manifest.frameSerial` 透传到 ①'②（`resolveRecord` → `TryBuildRuntimeGroupPalette`），或至少在 `resolveRecord` 入口记录一次供引用 | ② 现在只有 `renderable.frameSerial`，且 pose-only 路径会与 manifest 帧分叉（`war3_shadow_runtime_contract.cpp:4253-4254`） |
| P1 | `deviceEpoch` 透传到 shadow-core（或明确接受 0 + epochUnknown，并在报告中单列该比例） | ①②③ 现在不可得；`m_war3GpuSkinDeviceEpoch` 是 device 成员 |
| P1 | 把 `render::skin::Selection` 与 `m_war3GpuSkinDeviceEpoch` 写进 ③ 的事件载荷（device 侧 `:23044` 已有 Selection） | 现在 Selection 只参与门控；不传则 `lifecycleIdentity` 只能退化为模型 immutableGeneration 或 identityWeak |
| P2 | ④ 若要让「实际绘制」可回指对象，直接复用 draw 已有的 `shadowRenderablePart/rawcode/jHandle`，**不要**新增结构或哈希 | 裁定 4 禁止新增矩阵扫描哈希；且 `War3ShadowCasterDraw` 已含这些字段（`d3d9_war3_scene.h:396-407`） |

**本步明确不做**：不放宽 `IsContractUnitCandidate` / `IsVisibleDirectGeosetUnitCandidate`（工单 §0.2、§1.5 第 4 条）；
不新增矩阵扫描哈希（裁定 4）；不新增 JAPI 出口（裁定 7）；不改 `AutoTest/test_recorder_event_wire_golden.py`（裁定 2）。

---

## 自检：锚点复核命令与输出

以下命令在本树根目录执行（`Select-String` / `grep` 只读）。输出为本次核实后的原文粘贴。

### 自检 1 — ③ 提交侧锚点（`war3_shadow_renderer_core.cpp`）

```powershell
Select-String -Path src\d3d9\war3\shadow\war3_shadow_renderer_core.cpp -Pattern 'submitFrameLimited|m_core\.submitFrame\(buildWork->frame|ioFrame\.draws\.emplace_back|SemanticCoreShouldSubmitResolvedPacket\(packet, ioStats|kShadowSemanticCoreSceneSubmissionEnabled' | ForEach-Object { "$($_.LineNumber): $(($_.Line).Trim())" }
```

```text
6793: if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneSubmissionEnabled) {
7279: SemanticCoreShouldSubmitResolvedPacket(packet, ioStats))
7280: ioFrame.draws.emplace_back(std::move(packet));
7401: !SemanticCoreShouldSubmitResolvedPacket(packet, ioStats)) {
7417: ioFrame.draws.emplace_back(std::move(packet));
7468: return submitFrameLimited(frame, backend, 0u);
7471: bool ShadowRendererCore::submitFrameLimited(const ShadowSubmissionFrame& frame,
9734: if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneSubmissionEnabled) {
9738: m_core.submitFrame(buildWork->frame, backend);
```

> 复核结论：`7471` 与 `7280` 均存在；`9738` 位于 `9734` 的 `if constexpr` **else 分支**（编译期被排除），
> 与 §1 表格「constexpr 死分支」一致。

### 自检 2 — ③ 生产 caster 采集点（`d3d9_device.cpp` 22044-24700 区间）

```powershell
Select-String -Path src\d3d9\d3d9_device.cpp -Pattern 'War3ShadowCasterDraw draw = \{\};|shadowCasters\.emplace_back\(std::move\(draw\)\)|draw\.mapEpoch = |draw\.deviceEpoch = |draw\.shadowRenderablePart = |draw\.inputSkinSelection = |draw\.rawcode = |draw\.jHandle = ' | Where-Object { $_.LineNumber -ge 22044 -and $_.LineNumber -le 24700 } | ForEach-Object { "$($_.LineNumber): $(($_.Line).Trim())" }
```

```text
24049: War3ShadowCasterDraw draw = {};
24050: draw.mapEpoch = m_war3GpuSkinMapEpoch;
24051: draw.deviceEpoch = m_war3GpuSkinDeviceEpoch;
24052: draw.shadowRenderablePart = packet.renderable.renderablePart;
24062: draw.inputSkinSelection = selectedPalette;
24224: draw.inputSkinSelection = {}; // Semantic palette was not consumed by this native snapshot.
24385: draw.rawcode = canonicalIdentity.rawcode;
24386: draw.jHandle = canonicalIdentity.jHandle;
24641: m_war3Scene.shadowCasters.emplace_back(std::move(draw));
```

> 复核结论：对象键（renderablePart/rawcode/jHandle/mapEpoch/deviceEpoch/Selection）与 append 事实**同在**这一函数内。
> 新增注意（本轮核实发现）：`24224` 存在把 `inputSkinSelection` 清空的 native-snapshot 分支 ⇒ 取 Selection 前必须判空，
> 不得假定 `inputSkinSelection` 一定有效。

### 自检 3 — ④ 绘制/剔除锚点（`d3d9_war3_shadow.cpp`）

```powershell
Select-String -Path src\d3d9\d3d9_war3_shadow.cpp -Pattern 'bool War3ShadowReceiverPass::renderShadowMap|evaluateBoundsPolicy\(draw\)|!cascadeVisible\(i, c\)|ctx->cmdDrawIndexed\(draw\.indexCount|ctx->cmdDraw\(draw\.vertexCount|drawnCasters\+\+|reconciliation\.shadowMapDrawnCasters = drawnCasters' | ForEach-Object { "$($_.LineNumber): $(($_.Line).Trim())" }
```

```text
3713: bool War3ShadowReceiverPass::renderShadowMap(const Rc<DxvkCommandList> &ctx,
4460: const auto boundsPolicy = evaluateBoundsPolicy(draw);
4556: const auto boundsPolicy = evaluateBoundsPolicy(draw);
4795: if (!cascadeVisible(i, c)) {
5190: ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
5194: ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
5222: drawnCasters++;
5497: ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
5500: ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
5522: reconciliation.shadowMapDrawnCasters = drawnCasters;
8579: ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
8583: ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
```

> 复核结论（**对本报告口径的修正**）：`cmdDrawIndexed` 在 5497/5500 与 8579/8583 另有站点，
> 因此「实际绘制」不能只认 5190/5194；5190/5194 是 `renderShadowMap` 主 caster 循环内的站点。
> `evaluateBoundsPolicy(draw)` 在 4460 与 4556 各调用一次（`4556` 属 union-cull Observe 段）⇒
> 新增采集点必须明确选哪一处，且不得把 Observe 段的判定当成实际剔除。

### 自检 4 — ①② 与资源 miss（`war3_shadow_renderer_core.cpp`）

```powershell
Select-String -Path src\d3d9\war3\shadow\war3_shadow_renderer_core.cpp -Pattern 'struct PaletteSlotCacheEntry|kMaxPaletteSlotCacheEntries = 4096|static uint32_t FindOrUpdatePaletteSlotCache|g_paletteSlotCacheSessionGeneration\{|bool TryBuildRuntimeGroupPalette\(const ShadowModelResourceRecord|auto tryEngineDirectPosePalette|paletteSlotIndex = FindOrUpdatePaletteSlotCache|ioStats\.skippedResourceMiss\+\+' | ForEach-Object { "$($_.LineNumber): $(($_.Line).Trim())" }
```

```text
754: struct PaletteSlotCacheEntry {
759: static constexpr size_t kMaxPaletteSlotCacheEntries = 4096;
791: static std::atomic<uint64_t> g_paletteSlotCacheSessionGeneration{1u};
794: static uint32_t FindOrUpdatePaletteSlotCache(void* renderablePart,
6619: bool TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord& resource,
6662: auto tryEngineDirectPosePalette = [&]() -> bool {
6722: paletteSlotIndex = FindOrUpdatePaletteSlotCache(
7629: ioStats.skippedResourceMiss++;
```

> 复核结论：`794` 的签名**只收 renderablePart**（印证 §4 表格「① 内部拿不到对象键」）；
> `7629` 是旧 P0 轮 `resolved=0` 的落点（§6 第 5 步）。

### 自检 5 — 三帧来源与身份/代际契约

```powershell
Select-String -Path src\d3d9\war3\model\war3_model_hook.cpp,src\d3d9\war3\render\war3_skin_palette_selection.h,src\d3d9\war3\render\war3_canonical_draw.h,src\d3d9\war3\shadow\war3_shadow_runtime_contract.cpp -Pattern 'bool TryReadCurrentPaletteFrameTag|g_gameBase \+ 0xBDA4CCu|bool QueryCurrentPaletteFrameTag|struct Selection \{|ownerEpoch=0,publicationTicket|struct CanonicalDrawIdentity|liveManifest->frameSerial = poseFrameSerial|ConvertVisible\(record, manifest.frameSerial' | ForEach-Object { "$($_.Filename):$($_.LineNumber): $(($_.Line).Trim())" }
```

```text
war3_model_hook.cpp:353: bool TryReadCurrentPaletteFrameTag(uint32_t& outFrameTag) {
war3_model_hook.cpp:359: return SafeReadU32Fast(reinterpret_cast<const void*>(g_gameBase + 0xBDA4CCu),
war3_model_hook.cpp:9197: bool QueryCurrentPaletteFrameTag(uint32_t& outFrameTag) {
war3_skin_palette_selection.h:20: struct Selection {
war3_skin_palette_selection.h:25: uint64_t ownerEpoch=0,publicationTicket=0,captureSerial=0,hash=0;
war3_canonical_draw.h:64: struct CanonicalDrawIdentity {
war3_shadow_runtime_contract.cpp:3889: ConvertVisible(record, manifest.frameSerial, resolveDiagnostics));
war3_shadow_runtime_contract.cpp:4254: liveManifest->frameSerial = poseFrameSerial;
```

> 复核结论：三个帧来源（`getFrameIndex` / `manifest.frameSerial` / `QueryCurrentPaletteFrameTag`）与两份身份契约
> 均在本树存在；`4254` 印证 §5 的「record 帧与 manifest 帧会分叉」。
> `slotAllocationGeneration stays zero` 的唯一子串匹配未命中（该文字在 `war3_skin_palette_selection.h:16-19` 的
> 注释中以 `allocationGeneration stays zero` 形式出现，非逐字 `slotAllocationGeneration stays zero`）⇒
> 报告按**语义与字段**（`:26 uint64_t slotAllocationGeneration=0;`）引用，不按该子串。

### 复核总览

| 自检 | 结果 |
| --- | --- |
| 1（③ core 侧） | **通过**：5 个锚点全部定位；`9738` 确认为 constexpr 死分支 |
| 2（③ device caster） | **通过**：9 个锚点全部定位；新增发现 `24224` 清空 Selection 的分支 |
| 3（④ 绘制/剔除） | **通过（含口径修正）**：`cmdDraw` 有 3 组站点；`evaluateBoundsPolicy` 有 2 处调用 |
| 4（①② / 资源 miss） | **通过**：`794` 签名确证「无对象键」；`7629` 确证旧轮落点 |
| 5（三帧 / 身份契约） | **通过**：全部定位；1 条子串按语义而非逐字引用（已注明） |

> 若后续行号漂移，请以**函数名/唯一子串**重新定位；本报告已在每条锚点旁附函数名或唯一子串。
