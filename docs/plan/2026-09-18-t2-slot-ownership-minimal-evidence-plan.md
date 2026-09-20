# 2026-09-18 — T2 槽位所有权最小证据方案（路线 B 落地设计）

> **状态：未实施 / 待上级批准。**
> 本文是证据**方案**，不是实施记录：不改任何源码、不构建、不跑测试、不做 git 操作。
> 依据 `docs/plan/2026-09-18-relay-m2-t2t3-workorder.md` §3 产出；所有 file:line 均为
> **2026-09-18 当前快照**（B 树 `dxvk-v1.22-integration-20260914`，HEAD `ae89054`，
> 工作树含未提交改动），以函数名 / 唯一子串为锚，**行号会漂移**。
> 措辞上限按上级（codex `01a02e0b`）2026-09-17 03:05 裁定：未实机不得写运行时肯定表述；
> 审计结论上限「**未发现其他写入路径**」。

## 0. 名词先界定（同名不同物，防止误联）

| 名词 | 位置 | 语义 | 与本文关系 |
| --- | --- | --- | --- |
| **palette 槽位所有权**（本文对象） | `s_slotBlendedPaletteCache`（`src/d3d9/war3/model/war3_model_hook.cpp` → `kSlotBlendedPaletteCacheSize = 65536u`）与全局 blended palette arena | 「arena 某 slot 区间的矩阵字节**属于哪个 renderablePart**」 | 本文待证命题 |
| **render-host SlotLedger / ReadPermit** | `docs/plan/2026-09-16-render-host-slot-ownership-contract.md`（ADR-RH1） | 32↔64 位宿主拆分中**帧数据槽**（4 槽 × 65536 B）的租约 / 本地消费凭据 | **同名不同物**：与 palette 槽位无任何共享代码、共享 generation 或共享语义；本文任何「slot / 所有权 / generation」表述**不得**与该合同互引为同一概念，反之亦然 |
| `skin::Selection::slotAllocationGeneration` | `src/d3d9/war3/render/war3_skin_palette_selection.h:26` | native 分配代际见证（**恒 0**） | 本文 §1.2、§4.5 |

## 1. 背景与缺口

### 1.1 五层证明第 ③ 层未闭合

按 `docs/plan/2026-09-17-p0-object-level-evidence-workorder.md` §5.6（裁定 10）的五层口径——
缓存标签 / 实际读取的 arena 字节 / **对象所有权** / 提交 / 绘制——第 ③ 层「对象所有权」
当前状态是**未闭合**：`slotAllocationGeneration` 恒 0（无 native allocator witness），
A5 通过只能证明「这些槽位的字节是本帧写的」，**不能证明写字节的人是该 renderablePart 的
producer**（同文档 §3.1）。

### 1.2 `slotAllocationGeneration` 恒 0 是结构性事实

2026-09-18 快照全树核读（grep `slotAllocationGeneration`）：

| 引用点 | 性质 |
| --- | --- |
| `war3_skin_palette_selection.h:26` `uint64_t slotAllocationGeneration=0;` | **唯一定义点**（带默认初始化 0） |
| `src/d3d9/d3d9_device.cpp:22034`（`s.slotAllocationGeneration`） | 只读（诊断打印 / 导出） |
| `src/d3d9/war3/tools/war3_frame_inputs.cpp:321` | 只读（JSON 导出字段） |
| `AutoTest/analyze_skin_palette_selection.py:18` `require(u64(s["slotAllocationGeneration"])==0, "unsupported native allocation witness")` | 离线硬校验（非零即拒绝） |
| `AutoTest/test_skin_palette_contract.cpp:51` `check(p.slotAllocationGeneration==0, "no fabricated native allocation generation")` | 宿主测试钉零 |

⇒ 运行时源码**仅 3 处引用（1 定义 + 2 读取）、全树无写入点**；另有 2 处测试 / 分析器钉零。
头文件 :16-19 注释自述合同：「allocationGeneration stays zero until an actual native
allocator witness exists. Zero never authorizes rereading the global arena.」
**「0 永不授权」是既有合同，本文方案保持该事实不变并静态钉死（§4.5）。**

### 1.3 触发形态（上级 K5）：A0-A5 全过仍可能供出别的对象的字节

A0-A5 全链位于 `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp` →
`FindOrUpdatePaletteSlotCache`（:816）的 else 分支，收束变量 `producerConfirmed`（:896-898）：

```text
A0 bindingHit                        QueryRenderablePartPaletteSlot(part, …)      :856-858
A1 slotDomainValid                   槽位域合法（溢出安全减法写法）                :862-865
A2 boundSlotIndexMatchesRemembered   boundSlotIndex == 记忆槽位                    :866-867
A3 groupCountSuffices                boundGroupCount >= requiredPaletteCount      :869-871
A4 bindingFrameFresh                 绑定帧与当前 native frameTag 同帧（kDelta=0） :877-881
A5 slotRangeFrameFresh               QueryBlendedPaletteFrameTagRange 逐槽帧读     :885-895
                                     && missing==0 && min==max==boundFrameTag && 同帧
```

`kDelta = kPaletteSlotCacheMaxFrameTagDelta = 0u`（:788，严格同帧）。

触发形态 = **FROZEN 携带 + 槽位重分配给别的 part**：

- FROZEN 携带：`war3_model_hook.cpp` → `CaptureRuntimeGroupPaletteBindings`（:2580）在
  `slotIndex == 0xFFFFFFFFu || slotIndex >= 0x3A98u` 时**沿用 bindings 表上次记录的 slot**
  （:2654-2671），随后仍以**本帧 frameTag** 走 `RecordRenderablePartPaletteBinding`（:2691-2695）。
- 同处注释（:2638-2653）自述依据是「**通常**属于同一个 CModel，它的逻辑 slot 位置**不会**在
  arena 里迁移」——这是**假设**，不是证明。
- 于是：只要 arena 同一槽位本帧被**别的 part** 重写，A3/A4/A5 全部满足 ⇒
  供出的字节属于别的对象，且**没有任何拒绝计数增长**。
- 上级裁定 K5（工单 §3.1）：所有权错误**可以在 A5 全通过时发生**；「先出现 R3 才允许调查
  所有权」的前置**已被删除**；反过来 `RejectedStale` 也不能被解释为「确认发现陈旧矩阵」。

## 2. 待证命题分层

| 层 | 命题 | 可证性 | 证据形态 |
| --- | --- | --- | --- |
| **P1（弱）** | A5 供出区间在同一 `nativePaletteFrameTag` 内**未被第二来源域重写** | 可观察、可证伪（本方案目标） | 路线 B 有界写者见证 + 离线判读 |
| **P2（中）** | A5 **通过样本**的 `[slot, slot+count)` 与多来源重写事件**不相交** | 调查线索级（同帧两次写且区间相交最多记 `writing-overlap-suspect`） | 两个导出计数 + 离线分析器 |
| **P3（强）** | 写入者是 renderablePart 的 **owner**（native 生产者身份） | **本轮不可证**：缺 CGeosetData（`nodePtr`）→ renderablePart 的一手逆向映射（工单 §3.2「待验证」），且无 native allocator witness（`slotAllocationGeneration` 恒 0） | 只能写「未闭合」或「已观察到重写-读取关系」 |

**P1/P2 与 P3 的界线是硬约束**：即便 P1 长期未观察到反例，也不得据此宣布 P3 成立
（「未观察到 ≠ 已证明」，§5）。

## 3. 路线史与上级裁定

### 3.1 路线 A（per-slot owner 戳）——未获批准（裁定 5），仅作可追溯备选

做法（工单 §3.2）：在 `BlendedPaletteEntry`（`war3_model_hook.cpp:295-300`，当前
`Matrix4 matrix; uint32_t frameTag; uint64_t writeSerial; bool valid;`，**全部非原子**）
增加 `std::atomic<uintptr_t> lastOwnerPart` 与 `std::atomic<uint32_t> lastOwnerFrameTag`，
并把 A5 升级为「区间内所有槽位的 owner 见证 == 本对象」。65536 槽 × 12 B ≈ **+768 KB**
（相对既有 ≈4.6 MB 约 +16%，工单 §3.2 自算值）。

未获批准的**三条理由**（工单 §3.2，本文原样保留）：

1. **盖戳难题**：`CaptureBlendedPaletteSlotRange`（:2369）有两个调用方——
   `CaptureRuntimeGroupPaletteBindings`（调用点 :2699，**知道** `partPtr`）与
   `Hook_RuntimeMatrixWrite`（调用点 :8020，`kRuntimeMatrixWriteRva = 0x12E600`，:203；
   **只知 `nodePtr`（CGeosetData）与 `destMatrixPtr` → slot**）。若只有一侧盖戳，
   另一侧写入必须**清除**戳记，否则戳记陈旧并冒充所有权。
2. **缺逆向依据**：从 `nodePtr`（CGeosetData）反推 renderablePart 的映射**待验证**，
   需要新的一手逆向证据；没有它，writer hook 侧无法盖出可信戳记。
3. **改判定风险**：引入新的「所有权」判定会**改变 caster 准入结果**（可能误杀合法帧），
   按纪律必须作为**独立候选 + 实机反例门**，且不得与路线 B 合并验收。

### 3.2 路线 B（有界观察）——已获批准（裁定 5），未落地

裁定原文（工单 §3.3）：上级**先批准 B 的有界观察，不批准 A 改准入**；B 只能记录
「重写 / 读取关系」与事件顺序，**不能凭区间重叠宣布错误矩阵被消费**。
冻结记录确认现状：`docs/plan/2026-09-17-candidate-freeze-record.md` §4——
「槽位所有权见证（路线 A/B 均未落地）」。

路线 B 的**精确定义**（工单 §3.3 + K4，2026-09-18 复核锚点不变）：

- **两类「写」必须分列（裁定 K4）**：
  - `BindingRead`：读 native arena 后**更新插件缓存**（`CaptureRuntimeGroupPaletteBindings`
    侧，知道 `partPtr`，读的是该 part 的 arena 区间）；
  - `WriterHook(0x12E600)`：`Hook_RuntimeMatrixWrite` 重新写 arena / 槽位矩阵
    （只有 `nodePtr` / `destMatrixPtr`）。
  - `writerSource ∈ { WriterHook(0x12E600), BindingRead }`，**必须分列、不得合并**；
    `BindingRead` **不是**「游戏重新写了 arena」，混为一类即伪造「重写」证据。
- **写入侧有界记录**：在 `CaptureBlendedPaletteSlotRange` 内记录
  `(startSlot, count, frameTag, writerIdentity, writerThreadId, writerSource, orderIndex, writeSerial)`；
  `writerIdentity` 在 writer hook 侧只能给 `nodePtr`、binding 侧给 `renderablePart`；
  `orderIndex` 为同帧事件顺序（记录点自增）；`writeSerial` **沿用条目既有序号**（见 §4.1），
  不得为取证新增序号。
- **读取侧记录**：A5 求值点记录
  `(renderablePart, boundSlotIndex, requiredCount, boundFrameTag, currentFrameSerial, nativePaletteFrameTag)`。
- **容量**：每帧固定数组 **≤256 条 × ≈40 B ≈ 10 KiB `thread_local`**；溢出即计数并
  **停止本帧记录**（不是丢单条）。
- **判据（收窄版）**：同一 `nativePaletteFrameTag` 内，同一槽位区间被 ≥2 个不同
  来源域 / writerIdentity 写入，且某条 A5 通过样本区间与之相交 ⇒ 最多记一条
  **调查线索 `writing-overlap-suspect`**；**不得**宣布「错误矩阵被消费」。

### 3.3 相关裁定速查（本文受其约束）

| 裁定 | 内容 | 对本文的约束 |
| --- | --- | --- |
| 裁定 4 | 不批准新增矩阵扫描哈希；**摘要 ≠ 所有权证明** | 本方案不新增任何内容哈希；writeSerial 只是顺序，不是身份 |
| 裁定 5 | 先批准 B 的有界观察，不批准 A 改准入 | 本方案 = B 的落地设计；零准入改动 |
| 裁定 10 | 五层证明分别书写 | 本方案只触及 ②③ 层的关系记录；③ 层只能写「未闭合 / 已观察到重写-读取关系」 |
| K4 | 两类写分列；nodePtr 与 part 不同身份域 | §3.2、§6 |
| K5 | 所有权错误可在 A5 全过时发生；删除 R3 前置 | §1.3；本方案不依赖任何拒绝计数出现 |
| K3 | 「表空即零成本」不成立；TLS + watch count 须测量关闭 / 开启成本 | §6.4 |

## 4. 最小改动集（待批准）

> 全部**零准入改动**：不改变 A0-A5 任何判定、不改变 caster 准入、不改变拒绝分布；
> env 默认关；门关时零记录、零导出语义变化。常量名 / env 名为**建议值**，实施前一次性冻结。

### 4.1 写入侧见证记录点（`CaptureBlendedPaletteSlotRange` 内）

- 位置：`war3_model_hook.cpp` → `CaptureBlendedPaletteSlotRange`（:2369-2411）。
  该函数是 `s_slotBlendedPaletteCache` 的**唯一捕获写入处**（:2400-2406 循环；
  全树其余引用均为读取，仅 `ResetMapSession` :10337-10338 做失效清理），
  两个调用方共用 ⇒ **一个记录点覆盖两条调用路径**；`writerSource` 由调用方以参数 /
  计数器身份区分（`BindingRead` 走 `g_runtimeSimpleGroupPaletteSlotCapturedCount`，
  `WriterHook` 走 `g_runtimeMatrixWriteBatchCapturedCount`，:2699-2702 / :8020-8024）。
- 记录字段（每条约 40 B）：`(startSlot, count, frameTag, writerIdentity, writerThreadId,
  writerSource, orderIndex, writeSerial)`。
- **writeSerial 复用既有序号，不新增**：条目 `writeSerial` 已由
  `s_slotBlendedPaletteWriteSerial`（:305）在 :2396-2399 fetch_add 派生（:2403
  `entry.writeSerial = baseSerial + i`）。
  - 注意（2026-09-18 复核）：该**计数器本体**除 fetch_add 外无读者；但其派生的
    per-entry `writeSerial` **有读者**——`QueryBlendedPaletteBySlotIndex`（:9036-9038）
    用它强制连续槽位严格递增（活调用点 `d3d9_device.cpp:7889`）。
    因此本方案**不得改变既有 fetch_add(count) 的递增模式**，只能在记录点**读取**
    当场已算出的 baseSerial，不得另起序号体系。
- 有界性：每帧 ≤256 条 thread_local；溢出计数并停止本帧记录；帧切换由既有 frameTag
  边界归零。

### 4.2 A5 求值点记录（消费侧）

- 位置：`war3_shadow_renderer_core.cpp` → `FindOrUpdatePaletteSlotCache`（:816）
  的 A5 判定处（:885-898），**无论 producerConfirmed 真假**均在场已算出全部局部值。
- 记录：`(renderablePart, boundSlotIndex, requiredCount, boundFrameTag,
  currentFrameSerial, nativePaletteFrameTag)`（`nativePaletteFrameTag` 即当场
  `currentPaletteFrameTag`，:872-876；三帧域分列纪律 K1 不变）。
- 只记录**当场已算出的局部值**，不新增任何 `Query*` 调用（沿用 R 点 POD 纪律）。

### 4.3 两个导出计数

- `多来源写入帧数`：同一 `nativePaletteFrameTag` 内出现 ≥2 个不同来源域 / writerIdentity
  写同一槽位区间的帧数。
- `与 A5 通过样本相交次数`：上述区间与 A5 **通过**样本的 `[slot, slot+count)` 相交的次数。
- 导出沿既有链先例（bridge summary → diagnostics hub → control plane JSON →
  perf monitor JSON；模板测试 `AutoTest/test_palette_slot_cache_counters_export_static.py`）；
  与 `RejectedStale` **不同分母**，不得并入、不得相加成「总拒绝」。

### 4.4 离线分析器判 `writing-overlap-suspect`

- 判据严格按 §3.2 收窄版；输出三态之一：`observed`（出现线索）/
  `not-observed`（长期 0）/ `not-covered`（记录溢出、门未开、会话缺链）。
- 线索输出必须携带：帧、区间、两个来源域、两个 writerIdentity（原始值）、
  相关 A5 样本的原始字段；**不得**输出「错误矩阵被消费」结论。

### 4.5 静态测试钉死

- 有界性：256 条 / ≈40 B / 溢出停止并计数，常量单源。
- `writerSource` 分列：`WriterHook` 与 `BindingRead` 在记录、计数、离线判读三处
  不得合并。
- **`slotAllocationGeneration` 保持 0 并静态钉死「0 永不授权」**：除既有钉零
  （`test_skin_palette_contract.cpp:51`、`analyze_skin_palette_selection.py:18`）外，
  静态断言全树不出现对 `slotAllocationGeneration` 的**写入点**（定义点默认初始化除外），
  且 `OwnedSnapshotMatches` / `Usable`（`war3_skin_palette_selection.h:37/47`）不以
  非零 generation 作为授权条件。本方案自身**不新增任何引用**。
- 既有门禁必须仍过：`test_palette_slot_cache_counters_export_static.py`、
  `test_palette_slot_cache_producer_confirmation_static.py` 等。

### 4.6 与 T3 的记录点合并（建议）

T3（`docs/plan/2026-09-18-t3-thread-writer-runtime-proof-plan.md`）的写者线程戳
与本方案 4.1 是**同一记录点**（`CaptureBlendedPaletteSlotRange`）。建议合并为
**一个**记录点设计（同一条记录同时携带 `writerThreadId`），但两份证明的
**判定口径独立**：所有权（T2）≠ 线程关系（T3），任何一份不通过不自动否决另一份的证据。
合并与否属实施细节，待上级裁定。

## 5. 验收标准

| 情形 | 允许写 | 禁止写 |
| --- | --- | --- |
| 复现地图长期 0 线索 | 「**未观察到**同帧多来源重写与 A5 通过样本相交」（证据，不是证明） | 「槽位所有权已证明」「错误矩阵不再被使用」「P3 成立」 |
| 出现线索 | 「已观察到 `writing-overlap-suspect` 线索 N 起」并回 §5.6 五层表述逐层定位 | 「错误矩阵已被消费」（最多是调查线索，裁定 5 / K4） |
| 五层第 ③ 层 | 只能写「**未闭合**」或「**已观察到重写-读取关系**」 | 「所有权已闭合 / 已证明」 |
| 计数面 | 两个导出计数按轮列出原始值 | 用拒绝计数（`RejectedStale` 等）增加来证明本方案正确性（§1.3 K5：拒绝计数不含对象身份；本方案不依赖其出现） |
| 覆盖 | 门未开 / 溢出 / 缺链的轮次记「**未覆盖**」 | 把 0 线索轮次与未覆盖轮次合并成「未观察到」 |

实施门禁（若获批准）：对应静态测试全过、`ninja -C build32` exit 0、
`ninja -C build32 -n` no-work、`meson test -C build32` 全过、AutoTest 静态全量；
实机轮次按上级 Q-D 先申请后跑。

## 6. 风险与不可证边界

1. **身份域不可对拍（K4）**：`nodePtr`（CGeosetData）与 `renderablePart` 属于不同身份域，
   **不可直接比较**；记录里两者只作为各自来源域的原始身份值携带，不做跨域关联。
2. **区间相交 ≠ 错误消费**：同帧两次写且区间相交最多是 `writing-overlap-suspect` 线索；
   升级为结论需分别闭合五层的 ②（实际读取的 arena 字节属于谁）与 ③（对象所有权），
   本方案**未获授权**用重叠代替这两层。
3. **摘要相等 ≠ 所有权（裁定 4）**：已算好的 hash 只支持「同一份已发布拷贝」；
   本方案不新增矩阵扫描哈希，也不把 writeSerial 顺序当身份。
4. **成本须实测（K3）**：路线 B 的 TLS 成本（每帧 ≤10 KiB thread_local 记录缓冲）与
   「表空但门开」的探测成本必须实测（关闭 / 开启两组），不得只声明「默认关所以零成本」；
   热路径 `Hook_RuntimeMatrixWrite` 正常 13K-30K 次/帧、极端 50K-100K 次/帧
   （上限 `kMaxCallsPerFrame = 20000u`，:7960），记录点在其调用链内。
5. **无 native allocator witness ⇒ generation 不得非 0**：本方案不改变
   `slotAllocationGeneration == 0` 的结构性事实（§1.2）；任何让它非零的改动是另一个
   候选（需 native 分配见证的一手证据），不在本文范围。
6. **FROZEN 携带假设仍在**：:2638-2653 注释的「通常属于同一 CModel、slot 不迁移」是
   假设；本方案只提供观察该假设是否被违反的**手段**，不修改携带行为本身。
7. **A5 的 UB 前提不在本方案闭合**：A5 逐槽帧读（`QueryBlendedPaletteFrameTagRange`，
   :9201-9238，无锁读非原子 `s_slotBlendedPaletteCache`）与写者的线程关系证明属 **T3**；
   在 T3 闭合前，本方案记录的 A5 通过样本只能作为「样本点」携带，其「帧同源」判定
   不得单独升级为证据（见 T3 文档 §1、§5）。

## 7. 与三项判定的关系

- **判定 ①（「错误矩阵不再被使用」）**：直接被本方案（T2）与 T3 **阻塞**。
  按上级 Q-E 规定措辞，当前只能写「已观察到记忆槽位复核拒绝路径；
  **『错误矩阵不再被使用』尚未证明**」。要推进 ①，需先有本方案的重写-读取关系证据
  （②③ 层）与 T3 的线程关系运行时见证（A5 前提），二者缺一，① 维持「尚未证明」。
- 本方案落地后即便长期 0 线索，① 也**不能**因此改写为「已证明」——只能写
  「未观察到」（§5）。
- 判定 ① 之外的其余两项判定不属于本文范围，按既有五层口径各自书写，不得用本方案的
  线索外推。

## 8. 明确不做（本方案范围外）

- 不改 A0-A5 任何判定、阈值（`kPaletteSlotCacheMaxFrameTagDelta` 保持 0）、准入与回退。
- 不实施路线 A（owner 戳改准入）；不为其准备盖戳 / 清戳代码。
- 不把 `slotAllocationGeneration` 变为非 0；不发明 native slot lease。
- 不新增矩阵扫描哈希、不记录矩阵字节内容（裁定 4）。
- 不新增 `Kind`、不改既有 wire 布局、不新增 JAPI / 玩家 API（裁定 2 / 7 的同类纪律）。
- 不构建、不部署、不启动游戏、不做 git 写操作；实机轮次须另行申请（上级 Q-D）。
- 不与 render-host SlotLedger / ReadPermit（ADR-RH1）互引为同一「slot 所有权」概念（§0）。

## 9. 交叉引用

- T3：`docs/plan/2026-09-18-t3-thread-writer-runtime-proof-plan.md`（同址记录点；线程关系运行时证明）。
- 工单：`docs/plan/2026-09-18-relay-m2-t2t3-workorder.md` §3。
- 路线 A/B 与裁定 4/5/10、K1-K6：`docs/plan/2026-09-17-p0-object-level-evidence-workorder.md`（§3.1-3.5、§5.6、§R.1/R.2）。
- 线程前提与「fail-closed 所以安全」驳回：`docs/plan/2026-09-17-p0-thread-relationship-proof.md`。
- 措辞收窄 C-1/C-2/C-3：`docs/plan/2026-09-17-candidate-freeze-record.md`。
- render-host SlotLedger / ReadPermit（同名不同物）：`docs/plan/2026-09-16-render-host-slot-ownership-contract.md`。
