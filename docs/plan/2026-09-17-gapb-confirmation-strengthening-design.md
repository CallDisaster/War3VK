# 2026-09-17 — Gap B 复核补强设计（groupCount 参与判定 + frameTag 帧新鲜度 + 快照参数补齐）

> **文档性质（先读）**：这是**设计文档**，不触发任何代码变更。
> 本文件写入期间 B 树处于 **P0 组合候选实机事务冻结**（见
> `docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md:21`：
> 实机事务期间暂停 B 树一切源码/构建修改）。
> 本文描述的补丁**会改变 DLL 哈希**，因此必须作为 **P0 实机结算之后的独立候选**
> 另行立项实施；不得在本轮冻结候选（35,984,240 bytes /
> `8CECC495595D3BC6EC9D25AFBD1F0BA9F44CF9B18BBFB7E784B60B74C9569ADE`）内顺手改。
>
> 裁定来源：上级审核（Gap B 已知局限，`docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md:71-76`）：
> "core.cpp:799-805 的复核取回了 `boundGroupCount` 但**未参与判定**，也未校验帧新鲜度；
> producer 快照优先调用未接收 frameTag。底层查询返回的是记录的绑定/快照，
> **不是当前原生 arena 槽位所有权证明**。"
>
> **本文档自身**的 SHA-256 只能在交付回复中给出（文件不能自引用自身哈希）。

## 2026-09-17 上级裁定与本次修订说明（置顶）

- **修订依据**：上级（codex 线程 `01a02e0b…`）2026-09-17 **明确批准**修订本文件，范围限定为
  **§8① 及本文件内与统一证明口径直接冲突的文字**，特别点名「把 S4、同帧标签或快照大小相等
  描述成**充分所有权证明**」的内容。统一口径 = 五层各自证明到哪一步（见下表；与
  `docs/plan/2026-09-17-p0-object-level-evidence-workorder.md` §5.6 裁定 10 同源）。
- **本次改了什么**（逐条锚点）：

| # | 位置 | 旧表述 → 处置 |
| --- | --- | --- |
| G-1 | §8 表 ① 行（原 712 行） | ~~「度量可对账 + 记忆槽位路径收紧」即视为「错误矩阵不再被使用」成立~~ → **已被上级驳回**：本项最高只到第 ①② 层，**「错误矩阵不再被使用」需要第 ③ 层证据，本候选不具备** |
| G-2 | §8 表头（原 705-708 行） | 补「五层分别写、不得跨层合并」前置；「完全排除」不得作为任一层结论 |
| G-3 | §4.2 S4（原 297-300 行） | ~~「把快照字节也来自本帧 arena **证明到位**」~~ → **已被上级驳回**：S4 只到第 ② 层的**区间来源**，**不是**所有权证明 |
| G-4 | §4.1 A5 说明（原 274-277 行） | 「A5 把绑定捕获帧与槽位字节帧钉在一起」「才是一具骨架」→ 就地限定为**帧同源**口径，**不是**「这些槽位属于该 part」 |
| G-5 | §5 H5 内联注释（原 488 行） | ~~「充分性需要 §4.2 S4 的槽位区间见证」~~ → **已被上级驳回**：S4 也不构成充分性 |
| G-6 | §2.4 表 ① 行（原 165 行） | 「（最强）」→ 限定为**槽位字节新鲜度**这一层最强，**不得**外推到对象所有权 |
| G-7 | §1 结论摘要 | 新增第 7 条：本文件全部判定链只在第 ①② 层给证据；③④⑤ 层本文件不证明 |

- **本次明确不改**：**全部旧运行证据原文一字未改**；**任何阈值一字未改**（`kPaletteSlotCacheMaxFrameTagDelta = 0u`
  仍是严格同帧；§4.3 的「不得 >2」不变）；不改 `src/`、测试、`meson`，不构建、不部署、不启动游戏，无 git 写。
- **被驳回的旧表述一律保留并加 ~~删除线~~ 或「已被上级驳回」标注，不静默删除。**
- **未闭合**：五层的 ④ 提交 / ⑤ 绘制在本文件内没有采集点设计（属对象级证据工单 Step 1 范围）；
  本节只声明「本文件不证明这两层」，**不得**读成「这两层已被证明」。

**五层证明口径（本文件统一采用；与工单 §5.6 同源）**：

| 层 | 名称 | 本文件（A0-A5 / S0-S3 / S4）能证明到什么 | 明确**不能**证明 |
| --- | --- | --- | --- |
| ① | **缓存标签** | 绑定 / 槽位 / 快照记录的标签存在、帧号可读且不等式成立（A0-A4、S0-S3）；A5 还要求区间标签**帧同源** | 标签自证 ≠ 所有权；**同帧 `frameTag` 相等**、`max == frameTag` 都**不能**证明 arena 字节属于该 renderablePart |
| ② | **实际读取的 arena 字节** | 能证明「读取被发起、读到哪个 slot 区间、当场区间标签是什么」；S4 额外把**快照字节**钉到同一 slot 区间 + 同一区间标签上 | 不能证明这些字节**属于**该 renderablePart（这正是 Gap B 缺口的定义）；**快照大小相等**（`outPalette.size() == requiredCount`）只是**数量**口径，**不是**来源或所有权证明 |
| ③ | **对象所有权** | **本文件完全不证明这一层**（路线 A 未获批准；`slotAllocationGeneration` 恒 0） | 不得用 ①② 的通过代替；**S4、同帧标签相等、快照大小相等都不是所有权证明** |
| ④ | **提交** | 本文件不涉及（无采集点设计） | 不得由 ①② 推出 |
| ⑤ | **绘制** | 本文件不涉及（无采集点设计） | 不得由 ①②④ 推出；也不得由「阴影未减少」推出 |

---

## 0. 范围与禁止项

**范围内（只做设计）**：
1. `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp` 的 Gap B 复核补强（groupCount 判定、
   frameTag 帧新鲜度、快照参数补齐、槽位区间上界）。
2. 相关计数器与既有报告出口接线需求。
3. 受影响静态测试的锚点更新方案。

**范围外（明确不做）**：
- 不改 `src/`（含注释），不构建，不跑 git 写操作，不部署，不动玩家现场。
- 不改 Gap A（device 侧 `useCachedEntry`，`src/d3d9/d3d9_device.cpp:8668-8698`）；
  Gap A 已经做了 `producerGroupCount >= requiredPaletteCount` 判定，是本次 Gap B 的
  **参照口径**而不是改造对象。
- 不改 `war3_model_hook.cpp` 的 producer 语义（绑定表/快照写入规则）；本文只**消费**既有 API。
- 不改变 `render::skin::ContractEnabled()` 合同 ON 路径（严格 fail-closed 路径在 device 侧，
  `src/d3d9/d3d9_device.cpp:8585-8603`，本设计完全不动）。
- 不引入负缓存/黑名单；不新增事件体系（沿用既有 palette 来源分类 + 5 个 Gap A/B 计数器的风格）。

---

## 1. 结论摘要

1. **groupCount 能否取得、怎么取得**：能。shadow-core 调用方 `tryEngineDirectPosePalette`
   在 lambda 顶部已有 `const uint32_t requiredCount = outMaxVertexGroupSlot + 1u;`
   （`war3_shadow_renderer_core.cpp:6553`），它就在**同一个 lambda 作用域**内，
   与 `FindOrUpdatePaletteSlotCache(...)` 调用点（同文件 6584）相距 30 行、无任何作用域障碍。
   今日只是**没有把它转发**给缓存函数（该函数签名只有 2 个参数，769 行）。
   → 补强 = 给 `FindOrUpdatePaletteSlotCache` 增加第 3 个参数 `requiredPaletteCount`，
   调用点传 `requiredCount`。与 device 侧 `d3d9_device.cpp:8581/8688` 完全同源口径。
2. **producer 绑定里的 groupCount 不是冗余字段**：对 `SimpleFallback` producer 它是**硬编码 1**
   （`war3_model_hook.cpp:2673`），对 wrapper 路径读不到 geosetData 时是 **0**
   （同 2673-2682）。今天忽略它 = 允许用一个"只保证 1 个矩阵"的绑定去授权
   `requiredCount`（可能远大于 1）个矩阵的 arena 读取。**这就是 Gap B 的实质缺口**。
3. **frameTag 帧新鲜度可行，但必须两级**：
   - 当前帧号来源：`dxvk::war3::model::QueryCurrentPaletteFrameTag(uint32_t&)`
     （声明 `war3_model_hook.h:488`，实现 `war3_model_hook.cpp:9197-9199`），
     内部 = `TryReadCurrentPaletteFrameTag`（同文件 353-361）= `SafeReadU32Fast(Game.dll + 0xBDA4CC)`。
   - **绑定条目的 frameTag 只是"生产者捕获帧"**，不是"arena 槽位字节帧"：FROZEN 携带路径
     （`war3_model_hook.cpp:2654-2671`，注释 2638-2653）在 `part+0x08` 未更新时复用**旧槽位**，
     却仍然把**当前** frameTag 写进绑定（2605-2606 读一次 → 2691 传入 → 2505 落库）。
     所以 `binding.frameTag == current` **不能**证明槽位字节本帧被写过。
   - **真正的槽位内容新鲜度见证**是 writer 侧逐槽缓存
     `s_slotBlendedPaletteCache[slot].frameTag`，导出 API =
     `QueryBlendedPaletteFrameTagRange(slot, count, min, max, missing)`
     （声明 `war3_model_hook.h:490-494`，实现 9201-9238）。device 侧**已经在用**它
     （`d3d9_device.cpp:8762-8764`）。
   - 因此补强判定链 = 绑定命中 → **槽位一致** → **groupCount ≥ required** →
     **绑定帧不旧于当前帧** → **逐槽区间帧同源且不旧于当前帧**。任一步失败即拒绝记忆槽位。
4. **快照路径同样要补帧新鲜度**，但快照存在**同类盲点**需要写清楚：
   `QueryRenderablePartPaletteSnapshot` 返回的 frameTag 也是**捕获帧**
   （`war3_model_hook.cpp:9342-9343` 读 `entry.paletteFrameTag`，由 2538 写入同一个捕获 frameTag），
   而快照字节同样可能来自 FROZEN 携带的旧槽位（2654-2671 → 2684-2692 重新按槽位读 arena →
   2531-2534 存入快照）。故**快照帧校验是必要但不充分的**；要堵住这一层必须再做
   `QueryRenderablePartPaletteSlot` 拿槽位 + `QueryBlendedPaletteFrameTagRange` 做区间见证
   （两次查询读同一个 `s_renderablePartPaletteBindings[cell]`，槽位字段同源：9277）。
5. **容差默认取 0（严格同帧）**：与库内最强先例 `capturedPaletteCurrentFrameProven`
   （`d3d9_device.cpp:22564-22572`：`min != 0 && min == max && QueryCurrentPaletteFrameTag(cur) && cur == min`）
   一致。放宽到 2 帧有先例（`ValidateBlendedPaletteBySlotIndexExact`，9073-9108，注释 9091-9095
   记录"delta≤1 时 24.4% miss"），但那是**放宽**，必须由本轮 P0 实机数据（拒绝分布 + 反例门）
   驱动，不能在补强候选里预先放宽。
6. 槽位合法性今日只查 `slotIndex < 0x3A98`（6587），随后按 `requiredCount` 读 arena
   （6606-6614）。应按 device 侧同一防御口径（`slotIndex + expectedCount > capacity → false`，
   `war3_model_hook.cpp:9078-9082`）补 `slot + requiredCount <= 0x3A98`。
7. **证明口径（2026-09-17 上级裁定，统一修订）**：本文全部判定链（A0-A5、S0-S3、S4）**只在
   「缓存标签 / 实际读取的 arena 字节」两层范围内**给证据；**对象所有权 / 提交 / 绘制三层本文件不证明**。
   **S4、同帧 `frameTag` 相等、快照大小相等（`outPalette.size() == requiredCount`）都不是所有权证明**；
   完整口径见置顶「五层证明口径」与 §8 修订后的 ① 行。

---

## 2. 已核实事实（逐条 file:line）

### 2.1 Gap B 今日实现（工单 §7 已落地形态）

`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`

| 位置 | 内容 | 现状 |
| --- | --- | --- |
| 750-754 | `struct PaletteSlotCacheEntry { void* renderablePart; uint32_t paletteSlotIndex; uint64_t lastUpdateFrame; }` | 无 groupCount/frameTag 字段 |
| 755-757 | `kMaxPaletteSlotCacheEntries = 4096`、TLS 数组、`s_paletteSlotCacheIndex` | 环形分配合同，必须保留 |
| 758-766 | `g_paletteSlotCacheHitCount/MissCount` + Gap B 三个新计数器 + `g_paletteSlotCacheSessionGeneration{1u}` | 三个新计数器已在 7092-7107 导出，且已在 bridge 接线 |
| **769** | `static uint32_t FindOrUpdatePaletteSlotCache(void* renderablePart, uint32_t currentSlotIndex)` | **签名不含 requiredPaletteCount** |
| 784-792 | 命中且 `currentSlotIndex` 合法 → 刷新并返回（快路径） | 不动 |
| **793-816** | else 分支：`QueryRenderablePartPaletteSlot(part, boundSlotIndex, &boundGroupCount, nullptr)` | **799-805**：`boundGroupCount` 取回但**未参与判定**；`outFrameTag` 传 **nullptr** |
| 806-811 | `if (producerConfirmed) { … ServedAfterConfirm … return 记忆槽位; }` | 判定只有"命中 && 槽位一致" |
| 812-815 | `MissCount++` / `RejectedStaleCount++` / `return 0xFFFFFFFFu` | 无原因细分 |
| 820-829 | 未命中 → 插入新条目（环形） | 不动 |
| 6552-6619 | `auto tryEngineDirectPosePalette = [&]() -> bool` | 6553 `requiredCount`；6560-6570 快照优先；6576-6585 槽位 + 缓存调用；6587 槽位校验；6590-6615 arena 读 |
| 7092-7107 | 三个 Query 访问器 | 已在 `war3_shadow_renderer_core.h:440-443` 声明 |
| 9652-9683 | `ShadowValidationRuntime::reset()` 推进 `g_paletteSlotCacheSessionGeneration` | 跨地图语义，必须保留（见 2.9） |

### 2.2 `QueryRenderablePartPaletteSlot` 签名与语义

声明 `src/d3d9/war3/model/war3_model_hook.h:499-502`：

```cpp
bool QueryRenderablePartPaletteSlot(void* renderablePart,
                                    uint32_t& outSlotIndex,
                                    uint32_t* outGroupCount = nullptr,
                                    uint32_t* outFrameTag = nullptr);
```

实现 `src/d3d9/war3/model/war3_model_hook.cpp:9240-9283`：

- 输出初始化：`outSlotIndex = 0xFFFFFFFFu`，`*outGroupCount = 0`，`*outFrameTag = 0`（9244-9248）。
- 空指针 / cell 忙（`render::skin::TryCell`，9259-9260）/ part 身份不符（9261-9265）→ false。
- 槽位越界（`== 0xFFFFFFFFu || >= 0x3A98u`）→ false（9269-9273）。
- 成功：`outGroupCount = entry.groupCount`（9277），`outFrameTag = entry.frameTag`（9279）。
- **语义**：返回"该 renderablePart **最后一次** producer 捕获时记录的绑定"，不是"当前原生槽位所有权"。
- 字段写入点 `RecordRenderablePartPaletteBinding`（2480-2578）：
  - `entry.groupCount.store(groupCount)`（2504）：来自 2673-2682 ——
    `contract ON && !captureSimpleFallbackSlots ? 0 : 1` 起步，wrapper 路径再尝试读
    `part->geosetData->+0xF0`；**`SimpleFallback` 生产者恒为 1**（2673，参数 `captureSimpleFallbackSlots=true` 来自 8123-8124）。
  - `entry.frameTag.store(frameTag)`（2505）：来自 2605-2606 **一次** `TryReadCurrentPaletteFrameTag`，
    即"本次捕获发生的 Game.dll 调色板帧"。
- 查询有命中/未命中计数 `g_renderablePartPaletteBindingQueryHitCount/MissCount`（9250/9262/9270/9280）。

### 2.3 `QueryRenderablePartPaletteSnapshot` 签名与语义

声明 `war3_model_hook.h:507-511`：

```cpp
bool QueryRenderablePartPaletteSnapshot(void* renderablePart,
                                        uint32_t expectedCount,
                                        void* outPaletteVec,
                                        uint64_t* outHash = nullptr,
                                        uint32_t* outFrameTag = nullptr);
```

实现 `war3_model_hook.cpp:9285-9358`：

- 前置门（9304-9308）：开关 `RenderablePartPaletteSnapshotEnabled()`（2413-2417，env
  `DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT` 默认 **true**）、part 非空、
  `0 < expectedCount <= kRenderablePartPaletteSnapshotMaxCount`。
  **`kRenderablePartPaletteSnapshotMaxCount = 64u`**（338），存储 `std::array<Matrix4,64>`（334）。
- 数量门（9324-9329）：`entry.paletteCount >= expectedCount` 才继续 → 输出**恰好** `expectedCount` 个。
- seqlock：`paletteWriteSerial` 前后一致且为偶数（9319-9322、9335-9340）。
- `*outFrameTag = entry.paletteFrameTag`（9342-9343、9353-9354）；`*outHash` 仅在
  `paletteCount == expectedCount` 时用存储 hash，否则重算（9344-9349）。
- **语义**：该 part 最近一次 producer 捕获时**按槽位从 arena 复制**的字节快照
  （写入路径 2509-2561，字节来源 2684-2692 `globalPaletteBuf + slot*48`）。
  它比"现场重读 arena"少了现场竞态，但**不自动等于本帧内容**（见 2.6）。
- 上限 64 的直接后果：**`requiredCount > 64` 的 caster 永远无法走快照路径**
  （6554 允许到 256）。此为本补强需要写明的覆盖上限，见 §9。

### 2.4 三个不同 frameTag 的区分（本设计的关键前提）

| # | frameTag | 写入位置 | 语义 | 能证明什么 |
| --- | --- | --- | --- | --- |
| ① writer 逐槽帧 | `s_slotBlendedPaletteCache[slot].frameTag` | `CaptureBlendedPaletteSlotRange`，2402；调用点 `Hook_RuntimeMatrixWrite` 批捕获 8019-8024 | "该绝对槽位**本次被 writer 写入**时**当时**的 Game.dll 帧" | **该槽位字节本帧是否被写过**（**仅限"槽位字节新鲜度"这一层**，是三个帧号中最强；**不是**对象所有权证明，见置顶五层口径第 ③ 层） |
| ② 绑定条目帧 | `RenderablePartPaletteBindingEntry.frameTag` | 2505，值来自 2605-2606 的捕获帧 | "producer **为这个 part 做捕获**时的 Game.dll 帧" | producer 本帧跑过；**不证明**该槽位字节本帧被写 |
| ③ 快照帧 | `entry.paletteFrameTag` | 2538，值同 ②（2691 传入的同一 `frameTag`） | 同 ② | 同 ② |
| ④ 当前帧 | `Game.dll + 0xBDA4CC` | 引擎自写，DLL 只读（353-361） | 引擎调色板帧计数器 | 判定基准 |

结论：只查 ② 是**必要但不充分**；要封堵"FROZEN 携带旧槽位 + 当前帧戳"的组合，必须补 ①。

### 2.5 当前帧号从哪里拿

- 公开 API：`bool dxvk::war3::model::QueryCurrentPaletteFrameTag(uint32_t& outFrameTag)`
  （`war3_model_hook.h:488`；实现 9197-9199 直接转发）。
- 底层：`static bool TryReadCurrentPaletteFrameTag(uint32_t&)`（353-361）：
  `g_gameBase == 0 → false`；否则 `SafeReadU32Fast(g_gameBase + 0xBDA4CCu, 0u, outFrameTag)`。
- shadow core 已经 `#include "../model/war3_model_hook.h"`（core.cpp:14），
  **无需新增头依赖**，也无需薄声明头（工单 §0.3:41-42 的顾虑在 Gap B 落地后已不成立）。
- 同一指针也被 writer/绑定捕获读（6634-6635、8019）、被 contract 诊断读
  （`war3_current_draw_contract.cpp:2183`），口径一致。

### 2.6 底层是否保证"当前帧"——证据与结论

**不保证。** 有三条独立证据：

1. **FROZEN 携带路径**（最重要）：`CaptureRuntimeGroupPaletteBindings` 在
   `part+0x08` 为 `0xFFFFFFFF`/`>=0x3A98` 时，**复用绑定表里的旧槽位**
   （2654-2671，注释 2638-2653 明确"引擎 8-帧 slot cadence 会让 +0x08 临时为 0xFFFFFFFF，
   bindings 表上次记录的 slotIndex 仍然正确"），随后：
   - 按**旧槽位**从 arena 复制字节进快照（2684-2692、2531-2534）；
   - 却把**本次捕获读到的当前帧**作为 `frameTag` 传入（2691）并落库（2505、2538）。
   ⇒ 存在 `binding.frameTag == current && 槽位字节并非本帧写入` 的合法状态。
2. **生产者整帧缺席**：引擎在 `|dt| < 2*FLT_EPSILON` 时跳过 pose 求值链
   （`NoteSpriteUberPreRenderDtBucket` 相关注释 6624-6658、工单 dt-gate 相关记录），
   此时 producer 不刷新，绑定条目帧滞后于当前帧——这**正是帧校验要拦的**情况，说明校验必要。
3. **帧号可读失败**：`SafeReadU32Fast` 失败或读到 0 都返回 false/0
   （353-361）；`CaptureBlendedPaletteSlotRange` 在 frameTag 不可读时会**照样**把
   `entry.valid = true` 并写入 `frameTag = 0`（8019：`frameTagOk ? frameTagProbe : 0u`，2400-2406）。
   ⇒ 槽位缓存里存在 `valid == true && frameTag == 0` 的条目，**必须视为不可证明**而不是"匹配任意帧"。

**另有一条必须先说清的边界**：`ResetMapSession`（`war3_model_hook.cpp:10327-10346`）注释
10333-10336 明确写着"这些缓存按原生指针/槽位索引，**下一张地图可能复用地址空间，
而且游戏的 palette frame tag 可能重启，所以单靠新鲜度检查无法跨地图隔离它们**"，
并据此 `entry.valid = false`（10337-10338）+ 清 `renderablePart`（10340-10346）。
⇒ 本设计的帧校验是**帧内**证明，**不得**被用来替代 map epoch / session generation 隔离；
两者必须同时存在（见 §4.3）。

### 2.7 调用方能否取得 `requiredPaletteCount`，怎么取得（工单必答项）

能，且**不需要新增数据通路**：

- `TryBuildRuntimeGroupPalette`（6509+）先算 `vertexGroupSlotCount = RuntimeVertexGroupSlotCount(resource)`
  （6536；helper 6412-6425），再在 6544-6548 扫描 `resource.vertexGroupIndices[0 .. vertexGroupSlotCount)`
  取 `outMaxVertexGroupSlot`。
- `tryEngineDirectPosePalette` lambda 顶部：`const uint32_t requiredCount = outMaxVertexGroupSlot + 1u;`
  （6553），并在 6554 校验 `requiredCount == 0 || > 256 → false`。
- 缓存调用点在 **6584-6585**，与 6553 同 lambda 作用域 ⇒ 直接传参即可。
- device 侧同口径：`d3d9_device.cpp:8577-8581` 扫 `vertexGroups` 得 `requiredPaletteCount = outMaxVertexGroupSlot + 1u`，
  8688 用于 `producerGroupCount >= requiredPaletteCount`。**Gap B 只需对齐到同一表达式。**

### 2.8 既有同类判定先例（本设计直接复用而不发明）

| 先例 | 位置 | 口径 |
| --- | --- | --- |
| Gap A 三要素复核 | `d3d9_device.cpp:8686-8697` | 命中 && 槽位一致 && `groupCount >= requiredPaletteCount`，否则 `0xFFFFFFFFu` |
| 逐槽区间帧范围查询 | `d3d9_device.cpp:8756-8774` + `war3_model_hook.cpp:9201-9238` | `QueryBlendedPaletteFrameTagRange(slot, requiredPaletteCount, min, max, missing)`，懒调用；失败则**不发布** min/max（不给假证据） |
| 最强"当前帧已证"判定 | `d3d9_device.cpp:22564-22572` | `min != 0 && min == max && QueryCurrentPaletteFrameTag(cur) && cur == min` |
| 严格 slot+count 越界防御 | `war3_model_hook.cpp:9076-9082` | `expectedCount == 0 || > 256`、`slot + count > capacity` → false（并计数） |
| 帧差容差 2 的先例与代价 | `war3_model_hook.cpp:9091-9105` | 注释明确记录"delta≤1 时 24.4% miss"才放宽到 2；**不得**再往 3+ 放宽 |

### 2.9 槽位上界 `0x3A98`

`0x3A98` 即 15000，在四处作为槽位合法上界出现：`war3_model_hook.cpp:9269`、
`d3d9_device.cpp:8628`（读）、`8670`（复核）、`8723`（插入）；写入侧另有
`0x3A98 - slotIndex` 的容量余量判定（`war3_model_hook.cpp:2686`）。
今日 shadow core 只判 `slotIndex < 0x3A98`（6587）后即读 `requiredCount` 个 48 字节矩阵
（6606-6614），**未判 `slot + requiredCount` 是否越过该上界**。
（待实施时核实：`0x3A98` 是"arena 容量"还是"保守上界"。即便是保守上界，
补该判定也只会收紧、不会放宽。）

---

## 3. 缺口分析

| ID | 缺口 | 现状证据 | 后果 |
| --- | --- | --- | --- |
| **B-1** | `boundGroupCount` 取回但不参与判定 | core.cpp:800 取回、803 传参、**805 判定式只用 boundSlotIndex** | `SimpleFallback` 绑定（groupCount 恒 1，`war3_model_hook.cpp:2673`）与"geosetData 读失败 → groupCount 0"的绑定都能授权 `requiredCount`（可达 256）个矩阵的 arena 读 ⇒ 越段读，读到同 part 其它组或邻近槽位的字节 |
| **B-2** | 无绑定帧新鲜度校验 | core.cpp:803 第 4 实参写死 `nullptr` | 上一帧（或更早）的绑定可授权今天的 arena 读；producer 整帧缺席（dt-gate）时完全无感 |
| **B-3** | 无槽位内容帧见证 | core.cpp 全文无 `QueryBlendedPaletteFrameTagRange`（grep 为空） | 无法区分"槽位字节本帧被 writer 写过"与"FROZEN 携带旧槽位但绑定帧是新的"（2.6 证据 1） |
| **B-4** | 快照路径未接收 frameTag/未校验帧，数量用 `>=` | core.cpp:6561-6564（`nullptr, nullptr` 与 `outPalette.size() >= requiredCount`） | 旧帧快照被当作本帧骨架；且 `>=` 掩盖了"调用方以为拿了 requiredCount 个"的口径（实际 API 保证恰好 `==`） |
| **B-5** | `slot + requiredCount` 未判上界 | core.cpp:6587 | 尾部槽位可越过 `0x3A98` 读进 arena 之外（依 `IsReadableRange` 兜底，但不构成所有权证明） |
| **B-6** | 拒绝无原因细分 | core.cpp:812-815 只有聚合 `RejectedStaleCount` | 实机无法区分"绑定未命中/数量不足/帧陈旧/区间不齐"，也就无法支撑"是否误杀"与"容差是否该放宽"的判定 |

---

## 4. 补丁设计

### 4.1 判定链（记忆槽位供出前，全部满足才供出）

```
A0  绑定命中            QueryRenderablePartPaletteSlot(part, slot, &groupCount, &frameTag) == true
A1  槽位域合法          slot != 0xFFFFFFFF && slot < 0x3A98 && slot + requiredPaletteCount <= 0x3A98
A2  槽位与记忆一致      slot == s_paletteSlotCache[i].paletteSlotIndex
A3  数量覆盖            groupCount >= requiredPaletteCount          // B-1，对齐 device:8688
A4  绑定帧不旧          当前帧可读 != 0 && frameTag != 0
                        && frameTag <= current && current - frameTag <= kDelta   // B-2
A5  区间帧同源且不旧      QueryBlendedPaletteFrameTagRange(slot, requiredPaletteCount, min, max, missing)
                        && missing == 0 && min == max && max == frameTag
                        && current - max <= kDelta                      // B-3
```

- A3 必须在 A4/A5 **之前**判：数量不足的绑定做帧查询是无谓开销。
- A5 要求 `min == max`：**区间内所有必需槽位必须来自同一个 writer 帧**才是"一具骨架"
  （**注（2026-09-17 上级裁定）**：这是**帧同源**口径，**不是**"这些槽位属于该 renderablePart"的
  所有权口径，见置顶五层口径第 ③ 层）；混合帧（部分槽位本帧、部分上一帧）本身就是撕裂指纹，必须整体拒绝。
- A5 的 `max == frameTag` 把"绑定捕获帧"与"槽位字节帧"钉在一起：
  若绑定是 FROZEN 携带（frameTag 新）而槽位字节旧（max 旧），此条失败 ⇒ 正是 B-3 要拦的形态。
  **注（2026-09-17 上级裁定）**：这只是**第 ①② 层**的"区间标签帧同源"，**不**表示
  `[slot, slot+requiredCount)` 这些槽位**属于**该 renderablePart；**同帧标签相等不是所有权证明**（置顶五层口径）。
- 失败路径：**不写任何状态**（无负缓存），聚合 `RejectedStaleCount++` 后返回 `0xFFFFFFFFu`。
  调用方随即跳过按 slot 键的 Game.dll arena 读（6587-6615 不进入），落到
  ① producer 快照（新增强化后的 6560+）→ ② 下游 CPU `resource.matrixIndices`/`pose.matrixPalette`
  构建（6629+，工单 §0.2:30-31 已核实其存在）。

### 4.2 快照路径判定链

```
S0  requiredCount <= 64                      // kRenderablePartPaletteSnapshotMaxCount（338）
S1  查询成功 && outPalette.size() == requiredCount   // B-4（原为 >=）
S2  当前帧可读且 != 0
S3  snapshotFrameTag != 0 && snapshotFrameTag <= current
    && current - snapshotFrameTag <= kDelta
S4  （等价强化，用于堵 FROZEN 旧槽位）
    QueryRenderablePartPaletteSlot(part, slot, nullptr, nullptr) 命中
    && QueryBlendedPaletteFrameTagRange(slot, requiredCount, min, max, missing)
    && missing == 0 && min == max && max == current
```

- S4 是**可选加强项（C 类）**：它把"快照字节也来自本帧 arena **槽位区间**"这一**第 ② 层**证据补齐。
  ~~原表述：它把"快照字节也来自本帧 arena"**证明到位**~~ —— **该措辞已被上级驳回（2026-09-17 03:05）**：
  "证明到位"会被读成**充分所有权证明**；S4 **只**把快照字节钉到「同一 slot 区间 + 同一区间标签」上，
  **不**证明这些槽位属于该 renderablePart（第 ③ 层，本设计无证据），也**不**证明提交 / 绘制（④⑤ 层）。
  **注**：判定链标题里的"等价强化"指**与 A5 等价的区间帧同源校验**，**不是**"与对象所有权等价"。
  代价是每帧每个 skinned caster 多两次查询（绑定查询 O(1)、区间查询 O(requiredCount) ≤256）。
  建议**先做 S0-S3（必做）**，S4 视 P0 实机"快照陈旧"证据再决定；
  但设计上必须写明"只做 S0-S3 时，快照路径仍未排除 FROZEN 携带旧槽位"，且**任何情况下都不得**用 S4 的通过
  宣布所有权或"错误矩阵不再被使用"（§8 ① 行、置顶五层口径）。
- S4 不加 `min==max` 与 `max==current` 之外的容差：快照是"复制瞬间"的见证，
  容差由 `kDelta` 统一控制，避免两套口径。

### 4.3 容差策略与跨地图语义

- 常量：`constexpr uint32_t kPaletteSlotCacheMaxFrameTagDelta = 0u;`（默认严格同帧）。
  放在 core.cpp:766 附近与其他 Gap B 常量一起，**单一来源**，两条路径共用。
- 默认 0 的根据：库内最强先例 `d3d9_device.cpp:22564-22572` 用严格相等；
  本项目纪律是 fail-closed，放宽必须由证据驱动。
- 若实机显示大量合法对象被 A4/A5 拒绝（看 §6 的细分计数器 + 反例门）：
  下一候选可把该常量改为 `2u`，依据是 `war3_model_hook.cpp:9091-9105` 的同类观测
  （pose pre-pass 领先 draw 1-2 帧）。**不得**超过 2，不得按对象/按帧动态取值。
- 跨地图：A4/A5 是帧内证明，**不替代** `g_paletteSlotCacheSessionGeneration`
  （core.cpp:766/774-781）与 producer 侧 `ResetMapSession`（`war3_model_hook.cpp:10327-10346`）
  的隔离语义。理由见 2.6（frameTag 可能随地图重启）。
  → 加固**不得**删改 generation 比对与清空逻辑（工单 §4.1 钉死的合同）。

### 4.4 明确不做

1. 不把绑定帧校验做成"仅当 frameTag==0 才跳过"的 fail-open；frameTag==0 一律**拒绝**（不可证明）。
2. 不给 `QueryRenderablePartPaletteSlot` 增加 `producerKind` 输出——A3 的 groupCount 已覆盖其语义，
   新增 out 参数会扩大 producer API 面而无新证明力。
3. 不缓存"本帧已证"结果跨帧复用（会产生新的陈旧窗口）。
4. 不在 shadow core 里重算 hash 作为帧权威（core.cpp:835 `HashMatrixPalette` 与
   `war3_model_hook.cpp:938 HashBytes` 观察到同一组 FNV-1a 常量
   1469598103934665603 / 1099511628211，但字节布局与调用点不同；
   **本设计不做二者等价的断言**，快照 hash 仅作可选诊断输出）。

---

## 5. 逐 hunk 补丁草案

> 形式：伪代码/diff 草案，**不是可直接套用的补丁**（实施前须重读当前文件并核对行号漂移）。
> 行号基于 2026-09-17 冻结期读码；H 顺序即建议实施顺序。

### H1 — 常量与计数器声明（core.cpp 758-766 区域）

```diff
 static std::atomic<uint64_t> g_paletteSlotCacheProducerSnapshotFallbackCount{0};
 static std::atomic<uint64_t> g_paletteSlotCacheSessionGeneration{1u};
+
+// 2026-09-17 Gap B 补强：帧新鲜度容差（单一来源，供绑定帧与槽位区间帧共用）。
+// 0 = 严格同帧，与 d3d9_device.cpp:22564-22572（capturedPaletteCurrentFrameProven）
+// 的最强口径一致。放宽到 2 需实机证据，依据见 war3_model_hook.cpp:9091-9105。
+constexpr uint32_t kPaletteSlotCacheMaxFrameTagDelta = 0u;
+
+// 拒绝原因细分（聚合仍由 g_paletteSlotCacheRejectedStaleCount 承担，勿替换）。
+static std::atomic<uint64_t> g_paletteSlotCacheBindingMissRejectCount{0};
+static std::atomic<uint64_t> g_paletteSlotCacheGroupShortRejectCount{0};
+static std::atomic<uint64_t> g_paletteSlotCacheBindingFrameStaleRejectCount{0};
+static std::atomic<uint64_t> g_paletteSlotCacheSlotRangeStaleRejectCount{0};
+static std::atomic<uint64_t> g_paletteSlotCacheFrameProofServedCount{0};
+static std::atomic<uint64_t> g_paletteSlotCacheSnapshotFrameStaleRejectCount{0};
```

### H2 — 缓存函数签名（core.cpp:769）

```diff
-static uint32_t FindOrUpdatePaletteSlotCache(void* renderablePart, uint32_t currentSlotIndex) {
+// requiredPaletteCount: 调用方本帧实际需要的矩阵数（= outMaxVertexGroupSlot + 1u，
+// 与 d3d9_device.cpp:8581 同源）。producer 绑定记录的 groupCount 必须覆盖它，
+// 才允许把该槽位解释为"可读 requiredPaletteCount 个矩阵的 arena 段"。
+static uint32_t FindOrUpdatePaletteSlotCache(void* renderablePart,
+                                            uint32_t currentSlotIndex,
+                                            uint32_t requiredPaletteCount) {
```

> 注意：静态测试用前缀 `static uint32_t FindOrUpdatePaletteSlotCache(` 定位函数
> （`test_palette_slot_cache_producer_confirmation_static.py:14`），
> 只要该前缀逐字保留即不需改锚点。

### H3 — else 分支判定链（core.cpp:793-816，**核心 hunk**）

替换 799-815 段落：

```diff
-        uint32_t boundSlotIndex = 0xFFFFFFFFu;
-        uint32_t boundGroupCount = 0u;
-        const bool producerConfirmed =
-            dxvk::war3::model::QueryRenderablePartPaletteSlot(
-                renderablePart, boundSlotIndex, &boundGroupCount, nullptr) &&
-            boundSlotIndex != 0xFFFFFFFFu && boundSlotIndex < 0x3A98u &&
-            boundSlotIndex == s_paletteSlotCache[i].paletteSlotIndex;
+        uint32_t boundSlotIndex = 0xFFFFFFFFu;
+        uint32_t boundGroupCount = 0u;
+        uint32_t boundFrameTag = 0u;
+        const bool bindingHit =
+            dxvk::war3::model::QueryRenderablePartPaletteSlot(
+                renderablePart, boundSlotIndex, &boundGroupCount, &boundFrameTag);
+        const bool slotDomainValid =
+            boundSlotIndex != 0xFFFFFFFFu && boundSlotIndex < 0x3A98u &&
+            boundSlotIndex + requiredPaletteCount <= 0x3A98u;
+        const bool boundSlotIndexMatchesRemembered =
+            boundSlotIndex == s_paletteSlotCache[i].paletteSlotIndex;
+        const bool groupCountSuffices =
+            requiredPaletteCount != 0u &&
+            boundGroupCount >= requiredPaletteCount;
+        uint32_t currentPaletteFrameTag = 0u;
+        const bool bindingFrameFresh =
+            dxvk::war3::model::QueryCurrentPaletteFrameTag(currentPaletteFrameTag) &&
+            currentPaletteFrameTag != 0u && boundFrameTag != 0u &&
+            boundFrameTag <= currentPaletteFrameTag &&
+            currentPaletteFrameTag - boundFrameTag <=
+                kPaletteSlotCacheMaxFrameTagDelta;
+        uint32_t slotRangeMinFrameTag = 0u;
+        uint32_t slotRangeMaxFrameTag = 0u;
+        uint32_t slotRangeMissingCount = 0u;
+        const bool slotRangeFrameFresh =
+            bindingHit && slotDomainValid &&
+            dxvk::war3::model::QueryBlendedPaletteFrameTagRange(
+                boundSlotIndex, requiredPaletteCount, slotRangeMinFrameTag,
+                slotRangeMaxFrameTag, slotRangeMissingCount) &&
+            slotRangeMissingCount == 0u &&
+            slotRangeMinFrameTag == slotRangeMaxFrameTag &&
+            slotRangeMaxFrameTag == boundFrameTag &&
+            currentPaletteFrameTag - slotRangeMaxFrameTag <=
+                kPaletteSlotCacheMaxFrameTagDelta;
+        const bool producerConfirmed =
+            bindingHit && slotDomainValid && boundSlotIndexMatchesRemembered &&
+            groupCountSuffices && bindingFrameFresh && slotRangeFrameFresh;
         if (producerConfirmed) {
           g_paletteSlotCacheHitCount.fetch_add(1u, std::memory_order_relaxed);
           g_paletteSlotCacheServedAfterConfirmCount.fetch_add(
               1u, std::memory_order_relaxed);
+          g_paletteSlotCacheFrameProofServedCount.fetch_add(
+              1u, std::memory_order_relaxed);
           return s_paletteSlotCache[i].paletteSlotIndex;
         }
         g_paletteSlotCacheMissCount.fetch_add(1u, std::memory_order_relaxed);
         g_paletteSlotCacheRejectedStaleCount.fetch_add(
             1u, std::memory_order_relaxed);
+        if (!bindingHit) {
+          g_paletteSlotCacheBindingMissRejectCount.fetch_add(
+              1u, std::memory_order_relaxed);
+        } else if (!slotDomainValid || !boundSlotIndexMatchesRemembered) {
+          g_paletteSlotCacheBindingMissRejectCount.fetch_add(
+              1u, std::memory_order_relaxed);
+        } else if (!groupCountSuffices) {
+          g_paletteSlotCacheGroupShortRejectCount.fetch_add(
+              1u, std::memory_order_relaxed);
+        } else if (!bindingFrameFresh) {
+          g_paletteSlotCacheBindingFrameStaleRejectCount.fetch_add(
+              1u, std::memory_order_relaxed);
+        } else {
+          g_paletteSlotCacheSlotRangeStaleRejectCount.fetch_add(
+              1u, std::memory_order_relaxed);
+        }
         return 0xFFFFFFFFu;
```

**设计取舍（为什么保留那三个字面量）**：
- `boundSlotIndex == s_paletteSlotCache[i].paletteSlotIndex` 被改名成
  `boundSlotIndexMatchesRemembered` 的赋值语句，**逐字保留了该子串**
  （`test_..._producer_confirmation_static.py:28` 断言该精确子串出现在
  `fn[branch:confirm]` 区间内）。
- 语句顺序刻意维持 `} else {` → `if (producerConfirmed) {` →
  `return s_paletteSlotCache[i].paletteSlotIndex;` → `g_paletteSlotCacheRejectedStaleCount.fetch_add(`
  → `return 0xFFFFFFFFu;`，使该测试 18-24 行的顺序断言**无需修改**。
- `slotRangeFrameFresh` 里重复判 `bindingHit && slotDomainValid` 是为了让
  "求值短路安全"（槽位非法时不做区间查询），不是冗余逻辑。

### H4 — 调用点转发 requiredPaletteCount + 槽位区间上界（core.cpp:6584-6588）

```diff
     paletteSlotIndex = FindOrUpdatePaletteSlotCache(
-        renderable.renderablePart, paletteSlotIndex);
+        renderable.renderablePart, paletteSlotIndex, requiredCount);
     
-    if (paletteSlotIndex == 0xFFFFFFFF || paletteSlotIndex >= 0x3A98)
+    // 2026-09-17 Gap B 补强：除上界外，还必须保证 [slot, slot+requiredCount)
+    // 整段落在合法槽位域内，避免把"尾部槽位 + 完整骨架长度"读成越段数据。
+    if (paletteSlotIndex == 0xFFFFFFFF || paletteSlotIndex >= 0x3A98 ||
+        paletteSlotIndex + requiredCount > 0x3A98u)
       return false;
```

### H5 — 快照优先路径补齐 expectedCount 语义与 frameTag（core.cpp:6560-6570）

```diff
     if (renderable.renderablePart != nullptr) {
-      if (dxvk::war3::model::QueryRenderablePartPaletteSnapshot(
-              renderable.renderablePart, requiredCount, &outPalette, nullptr,
-              nullptr) &&
-          outPalette.size() >= requiredCount) {
+      // 2026-09-17 Gap B 补强：快照必须接收并校验自己的 frameTag。
+      // 注意（证据）：快照的 frameTag 是"producer 捕获帧"，而 FROZEN 携带路径
+      // 会用旧槽位复制字节并重新盖当前帧戳（war3_model_hook.cpp:2654-2692 +
+      // 2531-2538），故本校验是必要但不充分的。
+      // 2026-09-17 上级裁定：~~"充分性需要 §4.2 S4 的槽位区间见证"~~ 已被驳回——
+      // S4 也只到"快照字节来自同一 slot 区间标签"这一层（第 ② 层），仍不是对象所有权证明（第 ③ 层）。
+      // 完整口径见本文置顶"五层证明口径"。
+      uint32_t snapshotFrameTag = 0u;
+      uint64_t snapshotPaletteHash = 0u;
+      uint32_t currentPaletteFrameTag = 0u;
+      const bool currentFrameReadable =
+          requiredCount <= 64u &&
+          dxvk::war3::model::QueryCurrentPaletteFrameTag(currentPaletteFrameTag) &&
+          currentPaletteFrameTag != 0u;
+      if (dxvk::war3::model::QueryRenderablePartPaletteSnapshot(
+              renderable.renderablePart, requiredCount, &outPalette,
+              &snapshotPaletteHash, &snapshotFrameTag) &&
+          outPalette.size() == size_t(requiredCount) &&
+          currentFrameReadable && snapshotFrameTag != 0u &&
+          snapshotFrameTag <= currentPaletteFrameTag &&
+          currentPaletteFrameTag - snapshotFrameTag <=
+              kPaletteSlotCacheMaxFrameTagDelta) {
         outUsesAveraging = false;
         g_paletteSlotCacheProducerSnapshotFallbackCount.fetch_add(
             1u, std::memory_order_relaxed);
         return true;
       }
+      if (currentFrameReadable && snapshotFrameTag != 0u &&
+          (snapshotFrameTag > currentPaletteFrameTag ||
+           currentPaletteFrameTag - snapshotFrameTag >
+               kPaletteSlotCacheMaxFrameTagDelta)) {
+        g_paletteSlotCacheSnapshotFrameStaleRejectCount.fetch_add(
+            1u, std::memory_order_relaxed);
+      }
     }
```

- `requiredCount <= 64u`：提前避开必然 miss 的查询
  （API 在 9304-9308 拒绝 `expectedCount > kRenderablePartPaletteSnapshotMaxCount(64)`）。
- `snapshotPaletteHash` 仅取值不使用（**不得**未经验证就当作权威 hash，见 §4.4-4）；
  实施时若不接线，可直接写 `nullptr` 并删除该局部量，本设计不强制。
- `currentFrameReadable` 把"requiredCount ≤ 64"和"当前帧可读"合成一个 bool，
  是为了让 S1/S3 失败不产生误报陈旧计数（帧不可读时不是"陈旧"，是"不可证明"）。

### H6 — 访问器（core.cpp 7092-7107 区域）

```diff
 uint64_t QueryPaletteSlotCacheProducerSnapshotFallbackCount() {
   return g_paletteSlotCacheProducerSnapshotFallbackCount.load(
       std::memory_order_relaxed);
 }
+uint64_t QueryPaletteSlotCacheFrameProofServedCount() {
+  return g_paletteSlotCacheFrameProofServedCount.load(
+      std::memory_order_relaxed);
+}
+uint64_t QueryPaletteSlotCacheGroupShortRejectCount() {
+  return g_paletteSlotCacheGroupShortRejectCount.load(
+      std::memory_order_relaxed);
+}
+uint64_t QueryPaletteSlotCacheBindingFrameStaleRejectCount() {
+  return g_paletteSlotCacheBindingFrameStaleRejectCount.load(
+      std::memory_order_relaxed);
+}
+uint64_t QueryPaletteSlotCacheSlotRangeStaleRejectCount() {
+  return g_paletteSlotCacheSlotRangeStaleRejectCount.load(
+      std::memory_order_relaxed);
+}
+uint64_t QueryPaletteSlotCacheSnapshotFrameStaleRejectCount() {
+  return g_paletteSlotCacheSnapshotFrameStaleRejectCount.load(
+      std::memory_order_relaxed);
+}
```

同步在 `war3_shadow_renderer_core.h:440-443` 声明区追加 5 个原型
（`BindingMissRejectCount` 是否导出见 §6 讨论：建议**不导出**，归入聚合即可）。

### H7 — 报告出口接线（4 处 + 结构体）

沿用既有 5 个 Gap A/B 计数器的**同一出口链**（`test_palette_slot_cache_counters_export_static.py:39-63` 已钉死）：

1. `src/d3d9/war3/render/war3_shadow_runtime_bridge.h`：summary 结构体新增 5 个
   `uint64_t <field> = 0;`（现有 5 字段位置见 `test_..._export_static.py:25` 附近的字段清单）。
2. `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:6749-6763`：填充段追加 5 行
   `summary.<field> = shadow::Query...Count();`。
3. `src/d3d9/war3/tools/war3_diagnostics_hub.h` + `.cpp`：字段 + 填充 + JSON 键。
4. `src/d3d9/war3/tools/war3_control_plane.cpp`：JSON 键。
5. `src/d3d9/war3/tools/war3_perf_monitor.cpp`：`shadowRuntimeV2Summary` JSON 直读键
   （C++ 源码中为转义形式 `\"key\"`）。

**禁止**新建平行事件体系（工单 §5:118-122）。

### H8 — （可选，C 类）快照路径的槽位区间见证（§4.2 S4）

在 H5 的成功分支内追加：先 `QueryRenderablePartPaletteSlot` 取槽位，
再 `QueryBlendedPaletteFrameTagRange(slot, requiredCount, min, max, missing)` 要求
`missing==0 && min==max==current`。失败则**不返回 true**（落到 arena 路径再由 A5 判定）。
本 hunk 建议**不在第一版实施**，理由见 §4.2。

### H9 — （可选，D 类）miss reason 扩展

`RuntimeGroupPaletteMissReason`（core.cpp:2677-2686）今日有
`None/NoSkinningData/NoPosePalette/NoVertexGroups/InvalidGroupTable/MatrixIndexOutOfRange/VertexGroupOutOfRange/FallbacksFailed`。
补强后 `tryEngineDirectPosePalette` 返回 false 的原因变成"帧/数量证明不足"，
语义更接近 `FallbacksFailed`。**建议不新增枚举值**（避免扩大报告面），
但在 `outMissDetail` 的注释里写明"FallbacksFailed 现包含 producer 帧证明不足"。
若实机诊断确需区分，再单独立项。

---

## 6. 计数器需求

### 6.1 新增（本地原子 + Query 访问器）

| 本地计数器 | 触发条件 | 证明用途 |
| --- | --- | --- |
| `g_paletteSlotCacheFrameProofServedCount` | A0-A5 全通过后供出 | "经完整帧证明仍命中快路径"——**未误杀**的直接证据 |
| `g_paletteSlotCacheGroupShortRejectCount` | A3 失败 | 量化 B-1 真实发生率（尤其 SimpleFallback 绑定） |
| `g_paletteSlotCacheBindingFrameStaleRejectCount` | A4 失败 | 量化 producer 整帧缺席/dt-gate 窗口 |
| `g_paletteSlotCacheSlotRangeStaleRejectCount` | A5 失败（含 FROZEN 携带旧槽位） | 量化 B-3；也是判断 `kDelta` 是否该放宽的**唯一依据** |
| `g_paletteSlotCacheSnapshotFrameStaleRejectCount` | 快照命中但帧不新鲜 | 量化 B-4；决定 H8 是否有必要 |
| `g_paletteSlotCacheBindingMissRejectCount` | 绑定未命中/槽位域非法/与记忆不一致 | 内部诊断用；**建议不导出**（与既有 `QueryRenderablePartPaletteBindingMissCount` 语义重叠） |

### 6.2 修改（不新增，语义澄清）

- `g_paletteSlotCacheServedAfterConfirmCount`（core.cpp:763）：语义从
  "producer 确认后命中"**收窄**为"生产者绑定命中且槽位一致"，
  与新的 `FrameProofServedCount`（更严）并存。
  理由：保持既有导出字段与守卫测试 100% 兼容；报告时以两者之差读作
  "绑定确认但帧证明未过"的量。**不得**把旧字段改名为新语义（会破坏既有静态测试与台账口径）。
- `g_paletteSlotCacheRejectedStaleCount`（core.cpp:764）：**保留为全部拒绝路径的聚合**
  （每条拒绝都 +1），新细分为其 breakdown，满足
  `sum(细分) == 聚合` 的可对账性。

### 6.3 导出字段名（bridge summary / hub / control plane / perf monitor 共用）

```
semanticSceneSkinnedPaletteSlotCacheShadowCoreFrameProofServedCount
semanticSceneSkinnedPaletteSlotCacheShadowCoreGroupShortRejectedCount
semanticSceneSkinnedPaletteSlotCacheShadowCoreBindingFrameStaleRejectedCount
semanticSceneSkinnedPaletteSlotCacheShadowCoreSlotRangeStaleRejectedCount
semanticSceneSkinnedPaletteSlotCacheShadowCoreSnapshotFrameStaleRejectedCount
```

与既有 5 个字段同前缀、同风格（`...ShadowCoreServedAfterConfirmCount` /
`...ShadowCoreRejectedStaleCount` / `...ShadowCoreProducerSnapshotFallbackCount`）。

---

## 7. 静态测试锚点更新方案

### 7.1 `AutoTest/test_palette_slot_cache_producer_confirmation_static.py`（**必改**）

| 行 | 现断言 | 补强后 | 处置 |
| --- | --- | --- | --- |
| 11 | `#include "../model/war3_model_hook.h"` in CORE | 仍成立 | 不动 |
| 14 | `index("static uint32_t FindOrUpdatePaletteSlotCache(")` | 前缀逐字保留 | 不动 |
| 18-24 | `branch < confirm < confirm_ret < rej_count < rej_ret` | H3 刻意维持语句顺序 | 不动 |
| 27 | `QueryRenderablePartPaletteSlot(` in fn[branch:confirm] | 仍成立 | 不动 |
| 28 | `boundSlotIndex == s_paletteSlotCache[i].paletteSlotIndex` in fn[branch:confirm] | 逐字保留该子串（改名后的赋值语句内） | 不动 |
| 30 | Served 计数在 confirm..confirm_ret | 仍成立 | 不动 |
| 31 | 聚合 fetch_add 在 rej_count..rej_ret | 聚合保留在首位 | 不动 |
| 34-35 | session generation + 清空循环 | 未动 | 不动 |
| 41 | requiredCount 字面量唯一 | H5 未复制该字面量 | 不动 |
| 43 | `lam.index("FindOrUpdatePaletteSlotCache(")` | 仍匹配（多一个实参） | 不动 |
| **47** | `outPalette.size() >= requiredCount` in lam[snapshot:slot_resolve] | H5 改成 `== size_t(requiredCount)` | **必改**：替换为 `assert "outPalette.size() == size_t(requiredCount)" in lam[snapshot:slot_resolve]`（语义**加强**：`==` 蕴含旧断言的 `>=`） |
| 48-49 | requiredCount 唯一性、256 上界 | 仍成立 | 不动 |
| 53-54 | CPU 替代路径（`!resource.hasSkinningData()`、`uniqueGroupSlots`） | 仍成立 | 不动 |
| 58-62 | 合同 ON 分支未改 | 仍成立 | 不动 |

**建议新增断言（不删旧断言，只加严）**：

```python
# A3：groupCount 必须参与判定（对齐 d3d9_device.cpp:8688）
assert "boundGroupCount >= requiredPaletteCount" in fn[branch:confirm]
# A4/A5：帧新鲜度必须问当前帧 + 逐槽区间
assert "QueryCurrentPaletteFrameTag(" in fn[branch:confirm]
assert "QueryBlendedPaletteFrameTagRange(" in fn[branch:confirm]
assert "slotRangeMissingCount == 0u" in fn[branch:confirm]
assert "slotRangeMinFrameTag == slotRangeMaxFrameTag" in fn[branch:confirm]
# requiredPaletteCount 必须从调用方转发
assert "renderable.renderablePart, paletteSlotIndex, requiredCount" in lam
# 快照必须接收 frameTag
assert "&snapshotPaletteHash, &snapshotFrameTag" in lam[snapshot:slot_resolve]
# 拒绝细分必须都在聚合之后（可对账）
assert fn.index("g_paletteSlotCacheRejectedStaleCount.fetch_add(") < fn.index(
    "g_paletteSlotCacheSlotRangeStaleRejectCount.fetch_add(")
```

### 7.2 `AutoTest/test_issue6_render_identity_cache_reset_static.py`（**应不动**）

锚点（49-60 行）：`g_paletteSlotCacheSessionGeneration{1u}`、
`source.index("FindOrUpdatePaletteSlotCache")`、`source.index("// 查找缓存", lookup)`、
`source.index("for (auto& entry : s_paletteSlotCache)", lookup)`（要求 clear < scan）、
`void ShadowValidationRuntime::reset()` 内 `fetch_add`。
H1-H9 **不触碰** 773-781 的清空段，也**不新增**第二个 `// 查找缓存` 注释在清空段之前
⇒ 无需改锚点。若实施时确实新增了含相同子串的语句，**只允许更新锚点，不得修改断言语义**
（工单 §4.1:104-109 硬约束）。

### 7.3 `AutoTest/test_palette_slot_cache_counters_export_static.py`（**必改**）

- 25 行 `FIELDS` 追加 §6.3 的 5 个字段名。
- 33-37 行：`Query...` 原型检查列表追加 5 个新访问器名（`.h` 与 `.cpp` 同时要求）。
- 40-63 行：因 FIELDS 驱动，字段在 BRIDGE_H/BRIDGE/HUB_H/HUB/CP/PM 的存在性自动覆盖。
  若 H7 只接了部分出口，本测试会**失败**——这正是它作为接线守卫的价值，不得放行。
- 该文件注释 6-8 行需同步从"Gap A 2 个 + Gap B 3 个"更新为
  "Gap A 2 个 + Gap B 3 个 + Gap B 补强 5 个"。

### 7.4 不受影响的既有测试（已核对锚点）

- `AutoTest/test_device_palette_slot_cache_producer_confirmation_static.py`：只读 device.cpp。
- `AutoTest/test_live_palette_slot_cache_accelerator_static.py`：只读 device.cpp。
- `AutoTest/test_live_palette_frame_tag_lazy_fallback_static.py`：只读 device.cpp
  （第 5 行 `DEVICE = .../d3d9_device.cpp`），其 `QueryBlendedPaletteFrameTagRange(`
  与 `QueryRenderablePartPaletteSnapshot(` 锚点都在 device 段内。
- `AutoTest/test_semantic_palette_diagnostics_hotpath_static.py:57`、
  `test_shadow_metadata_lifecycle_static.py:404`、
  `test_trusted_current_palette_rebuild_bypass_static.py:23`：均锚在 device 侧
  `QueryCurrentPaletteFrameTag(` 使用点，不受 shadow-core 影响。
- `AutoTest/data_collection_entrypoints.json:653` 含 `QueryRenderablePartPaletteSnapshot`：
  仅名称清单，签名无关。

---

## 8. 验收口径（承接 P0 组合候选方案，本补强候选沿用）

> **前置（2026-09-17 上级裁定，统一修订）**：本节四项**必须按置顶「五层证明口径」分别书写**
> （缓存标签 / 实际读取的 arena 字节 / 对象所有权 / 提交 / 绘制），**不得跨层合并成一句结论**，
> 也**不得**用 ①② 层的通过代替 ③④⑤ 层。特别是：**S4、同帧 `frameTag` 相等、快照大小相等
> 都不是所有权证明**；「错误矩阵不再被使用」本身属于第 ③ 层，本补强候选**不具备**该层证据。

本补强候选**不得**自称"完全排除了错误矩阵"，除非同时满足下列四项
（对齐 `docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md:78-85`）：

| 项 | 度量 | 反例/否决 |
| --- | --- | --- |
| ① 错误矩阵不再被使用 | ~~原表述：度量可对账 + 记忆槽位路径收紧，即视为本项成立~~ —— **已被上级驳回（2026-09-17 03:05）**。本项**只能**按五层口径写，且本候选**最高只到第 ①② 层**。可写的度量是：`ShadowCoreRejectedStaleCount` 的分段增量可被细分计数器完整对账（`sum(细分)==聚合`）；**记忆槽位**在 producer 未确认时不再供出（静态断言钉死 A0-A5 顺序 + 实机 `SlotRangeStale` 与画面 churn 的关联）。**「错误矩阵不再被使用」本身需要第 ③ 层（对象所有权）证据，本候选不具备，不得写入结论** | 只拿"拒绝数上升"当结论；**宣称"未经帧证明的 arena 读不可达"**（见下）；把 ①② 层结论写成"错误矩阵不再被使用"；用 S4 / 同帧标签相等 / 快照大小相等冒充所有权（见置顶五层口径） |
| ② 合法对象仍有替代路径 | `FrameProofServedCount`、`ProducerSnapshotFallbackCount`、`OwnedPartSnapshot`、`SubmitTimePublishedRegistry`、`DrawTimeCaptured` 的增量接住被拒量；`SourceNoneCount` **不得**同幅上升 | SourceNone 同幅上升 = 把拒绝当修复 |
| ③ 失败后可恢复 | 段 2 被拒对象在段 2 后期/段 3 重新出现 `FrameProofServed`/快照/registry 命中；`SourceNone` 不逐帧单调累积 | 永久黑名单或单调遗漏 |
| ④ 反例门（必须） | 高压低视角 + 往返移动下，正常单位/建筑/桥梁/装饰物投影数量不低于修复前基线 | 整体阴影减少而无替代路径证据 |

> **口径更正（2026-09-17 对抗性复核）**：A0-A5 只覆盖"命中条目后 `+0x08` 本帧无值 ⇒
> 用记忆槽位"这一条供出路径。以下两条路径**有意保留为未证兜底**，本轮一字未改
> （与 `docs/agent-history/2026-09-16-architecture-review-followup.md` 的
> "直读有效的快速路径行为不变"一致）：
> (a) 命中条目 + `currentSlotIndex` 合法 ⇒ 覆写记忆槽位并直接供出；
> (b) 未命中条目 + `currentSlotIndex` 合法 ⇒ 首见插入并直接供出。
> 因此本候选**不得**宣称"未经帧证明的 arena 读在代码路径上不可达"，只能宣称
> "记忆槽位路径已收紧到全链帧证明"。若要覆盖 (a)(b)，需另立候选
> （给它们加 A5 级区间帧证明，或让 producer 输出"本帧捕获/来自 FROZEN 携带"见证）。
> 另：`SnapshotFrameStaleRejectCount` 只统计"确认存在但帧号超差"，不含"帧不可读/帧号为 0"
> 的不可证明情形（后者同样拒绝但不计入，避免污染 `kDelta` 决策依据）。

**并且必须单独报告**：`GroupShortRejected` / `BindingFrameStaleRejected` /
`SlotRangeStaleRejected` / `SnapshotFrameStaleRejected` 四类增量的**相对分布**。
若 `SlotRangeStaleRejected` 占绝对多数且反例门不过 ⇒ 说明 `kDelta=0` 过严，
才允许按 §4.3 走 `kDelta=2` 的**下一**候选，并附该分布作为依据。

**基准与宣称限制**（承 P0 方案 §5:87-92）：不得把
`1C314090…`（已含 Gap B）当"完整修复前基线"；本补强候选与 P0 冻结候选之间是
"仅 Gap B 判定强度不同"的成对版本，只能用于因果对照，不得外推性能结论。

---

## 9. 风险、未决项与"待实施时核实"清单

**风险**
1. **过严风险（首要）**：`kDelta=0` 会让"shadow 构建落后引擎 1 帧"的合法对象被拒，
   落到 CPU 构建路径；该路径本身是合法替代（工单 §0.2:30-31），但成本与视觉质量
   需由反例门确认。缓解：细分计数器 + §8 的判定矩阵。
2. **开销风险**：A5 的 `QueryBlendedPaletteFrameTagRange` 是 O(requiredCount) ≤256 次
   数组读（`war3_model_hook.cpp:9219-9235`），只在**槽位路径**执行一次；
   device 侧同 API 已在生产路径（8756-8774）。若实测成为热点，
   应按 device 的**懒查询**模式（`slotFrameTagQueried` 闸）只在即将供出时查一次，
   不得为了省开销把 A5 降级或跳过。
3. **数据竞争口径**：`QueryBlendedPaletteFrameTagRange` 读的是**非原子**的
   `valid`/`frameTag` 字段（`BlendedPaletteEntry`，`war3_model_hook.cpp:295-300`），
   这是**既有**假设（device 侧已如此使用，`ValidateBlendedPaletteBySlotIndexExact`
   同样如此）。本设计不改变也不扩大该假设；但需在实施说明中记录调用线程
   （shadow core 的构建线程）与 device 侧是否同一线程。

**待实施时核实（不得凭本文臆断）**
1. `0x3A98` 是 arena 槽位容量还是保守上界（见 2.9）；若为容量，A1 的上界判定要
   与写入侧容量余量口径（`war3_model_hook.cpp:2686`）对齐。
2. `requiredCount > 64` 的 caster 在实机中的占比：若不可忽略，
   则快照路径（受 `kRenderablePartPaletteSnapshotMaxCount = 64` 限制，338）
   对这些对象**不可用**，S4/H8 也救不了，必须写进结论的覆盖上限。
3. shadow-core 调用 `QueryCurrentPaletteFrameTag` / `QueryBlendedPaletteFrameTagRange`
   的线程是否与 writer hook 同线程；若不同，需按项目纪律评估是否要
   `WARVK_DATA_SCOPE(PaletteCapture)` 之类的影响面。
4. `snapshotPaletteHash` 与 core.cpp:835 `HashMatrixPalette` 是否真同源
   （两者观察到同一组 FNV-1a 常量，但未做等价断言）；若不同源，禁止把快照 hash
   用于任何跨模块比对。
5. 当前 `g_paletteSlotCacheHitCount/MissCount` 的语义在新增细分计数后是否仍需保留
   双计（H3 保留了原 Hit/Miss 递增，勿删——它们可能被其它诊断读取）。
6. 静态全量测试的当前计数基线（写此文档时台账记录为 241/241，
   `docs/plan/2026-09-16-session-handoff.md:39/48`）在补强候选实施后必须重测，
   不得沿用。

---

## 10. 实施顺序与哈希纪律

1. **前置**：P0 组合候选实机事务**结算完成**（恢复现场 DLL 并哈希核验，
   见 P0 方案 §6:99-101），冻结窗口解除。
2. 在 B 树按 H1 → H6 → H4 → H3 → H5 → H7 顺序落地（先常量/访问器，再调用点，再核心分支，最后接线），
   每步保持可编译。
3. 跑门禁：`build32_safe.cmd` exit 0、`ninja -C build32 -n` no-work、
   `meson test -C build32`、全量 `AutoTest/test_*_static.py`（含 §7.1/§7.3 更新）。
4. **哈希纪律**：本补强**必然改变 DLL 哈希**；不得把新哈希冒充 P0 冻结候选
   `8CECC495…`。新哈希须按既有流程记录进
   `docs/agent-history/DEVELOPMENT_CHANGELOG.md` 与
   `docs/plan/2026-09-16-merge-execution-ledger.md`，并注明构建配置
   （当前 build32 为 F855 诊断配置，`docs/plan/2026-09-16-session-handoff.md:44-45`）。
5. **不得**在本文档内预写任何"已修复/已通过"结论。

---

## 附录 A — 引用索引（本文所有事实的一手位置）

| 主题 | 位置 |
| --- | --- |
| Gap B 现状分支 | `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp:793-816` |
| 缓存函数签名 | 同上 769 |
| 缓存条目结构 | 同上 750-754 |
| Gap B 计数器 | 同上 758-766；访问器 7092-7107；声明 `war3_shadow_renderer_core.h:440-443` |
| `tryEngineDirectPosePalette` | 同上 6552-6619（requiredCount 6553；快照 6560-6570；缓存调用 6584-6585；槽位校验 6587；arena 读 6590-6615） |
| 顶点组最大槽位来源 | 同上 6536-6548、`RuntimeVertexGroupSlotCount` 6412-6425 |
| miss reason 枚举 | 同上 2677-2686 |
| 会话/地图重置 | 同上 9652-9683；`war3_model_hook.cpp:10327-10346` |
| `QueryRenderablePartPaletteSlot` | 声明 `war3_model_hook.h:499-502`；实现 `war3_model_hook.cpp:9240-9283` |
| `QueryRenderablePartPaletteSnapshot` | 声明 507-511；实现 9285-9358；上限常量 338；开关 2413-2417 |
| `QueryCurrentPaletteFrameTag` | 声明 488；实现 9197-9199；底层 353-361（Game.dll+0xBDA4CC） |
| `QueryBlendedPaletteFrameTagRange` | 声明 490-494；实现 9201-9238 |
| 绑定记录写入 | `war3_model_hook.cpp:2480-2578`（groupCount 2504 / frameTag 2505 / 快照 2509-2561 / 严格复制 2542-2546） |
| 绑定捕获（含 FROZEN 携带） | 同上 2580-2712（frameTag 读 2605-2606；携带分支 2654-2671；groupCount 2673-2682；记录 2691） |
| writer 逐槽帧写入 | 同上 2369-2411；调用点 8013-8025（frameTag 8019） |
| 严格 slot+count 越界防御 | 同上 9073-9108（容差注释 9091-9105） |
| 跨地图不得靠新鲜度 | 同上 10333-10336（注释） |
| Gap A 参照实现 | `src/d3d9/d3d9_device.cpp:8581`、`8668-8698`、`8731-8741` |
| device 区间帧查询先例 | 同上 8756-8774 |
| 最强同帧先例 | 同上 22564-22572 |
| Gap B 缺陷裁定 | `docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md:71-76` |
| P0 冻结候选与事务纪律 | 同上 14-22、78-101 |
| Gap B 原始工单与已落地范围 | `docs/plan/2026-09-16-palette-gap-fix-workorder.md:27-31,55-61,135-146` |
| 门禁基线 | `docs/plan/2026-09-16-session-handoff.md:32-48` |

## 附录 B — 无代码变更声明

本文件写入过程**未**修改 `src/` 下任何文件，**未**构建，**未**执行任何 git 写操作，
**未**部署 DLL，**未**触碰 `E:\Work\War3\` 或玩家目录。
本文描述的全部补丁均为**草案**，须在 P0 实机结算后另立候选实施。
