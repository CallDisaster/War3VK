# M2-5 抽取记录：skinned palette taxonomy 发射块 → 模块内纯函数

> 状态：**完成（§1–§12 全部闭合）**。迁移逐字节落地；legacy 参考、宿主差分
> （34 字段逐调用整数精确相等 + 段末 memcmp + 17 场景独立期望值表）、预算门禁
> `FROZEN_M2_5`、独立 M2-5 等价门禁与 **2 条变异真跑**全部闭合；全量门禁在**最终树**
> 复跑留档。
> 本轮**不部署、不启动游戏、不 git 写、不触碰 `E:\Work` 下任何文件、不称稳定版**。
> 原始门禁输出：`docs/plan/2026-09-18-m2-5-full-gate-rerun.log`；
> 变异原始输出：`docs/plan/2026-09-18-m2-5-mutation-raw-output.log`。

工单：`docs/plan/2026-09-18-overnight-execution-plan.md` §2「M2-4/M2-5（富余）」、
`docs/plan/2026-09-18-deepseek-overnight-handoff.md` §4 阶段 4 第 1 条、§8 关键路径。
设计与清册：`docs/plan/2026-09-18-m2-5-taxonomy-and-remaining-scope.md` §1 表 B 的 **B3**、
§2（门禁设计）、§3.2（跨帧探针表是 M2-5 与前三片本质不同之处）、§4-3（块边界未做字符级校验）。
同构先例：`…-m2-1-…`、`…-m2-2-…`、`…-m2-3-migration-equivalence-record.md`。

## 0. 结论摘要

- device.cpp 的 taxonomy 发射块 **:21202-21580（379 行 / UTF-8 19,359 B）逐字节迁入**
  新模块 `src/d3d9/war3/semantic/war3_palette_taxonomy_emission.{h,cpp}` 的纯函数
  `dxvk::war3::semantic::War3EmitSemanticPaletteTaxonomy(...)`。
- **唯一机械差异一行**：删除块首的别名 `auto& stats = m_war3Scene.shadowStats;`，
  改为函数第一个形参 `dxvk::War3ShadowCaptureStats& stats`；其余 378 行（含 3 张探针表）
  **逐字节相同**，模块正文与 legacy 参考正文 18,937 B **双向 fail-closed 证明一致**。
- 所有输入**显式传参**（11 个）；模块内**不出现**任何 device 成员、registry / hook 全局，
  唯一外部函数调用是既有纯静态哈希 `VisibleRenderableRegistry::computeShadowManifestPartKey`。
- **34 个 stats 字段**的写入条件、顺序与计数增量逐点不变：宿主差分在 `DXVK_WAR3_SEMANTIC_
  PALETTE_DIAGNOSTICS=1` 下 **30,443,383/30,443,383 checks passed**（逐调用 34 字段 +
  段末整结构 memcmp），默认 env **30,443,384/30,443,384**。
- 两条变异真跑：条件反转 ⇒ 差分 30,116,847/30,443,383（326,536 条 DIFF）；可编译回流 ⇒
  预算门禁点名 `War3EmitSemanticPaletteTaxonomy: device.cpp=1 > budget=0`。两条都完成
  「改→构建→跑→红→还原→touch→真实重编译→复跑绿」，源码 SHA-256 逐字节还原。
- 全量门禁在最终树全绿（`ninja -n` no work、构建 exit 0、预算/M1/M2/M2-3/M2-5 门禁 exit 0、
  meson 83/83、全量静态 **251/251**、记录器 23/23、成本 PASS(38/38)、生命周期 187/187、
  解析器静态 94/94、根读方 55/55、往返 CERTIFIED）。
- **device.cpp 侧调用点编排（B1/B2 等）本轮一个字节没迁**——那是 M2-5 的其余条目。

## 1. 迁出范围与落点

### 1.1 逐字节范围

| 项 | 值 |
| --- | --- |
| 块范围（pre-M2-5 device.cpp） | `:21202`（`if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {`）到 `:21580`（其配对 `}`） |
| 块规模 | **379 行 / UTF-8 19,359 B**（378 个行尾；含中文注释） |
| 花括号配对复核 | 从 :21202 首个 `{` 起做深度配对，终于 :21580 ⇒ 与清册 §1.2 B3 登记的范围一致（解答 §4-3 的不确定项：该块**未**含带花括号的字符串字面量，配对结果唯一） |
| 迁出后 device.cpp | 2,273,048 B → **2,254,826 B**（-18,222 B，逐字节账目闭合：-19,359 + 59 include + 285 注释 + 793 调用点）= **0**） |
| 迁入后模块 .cpp | **21,020 B / 422 行**，`E8C6F74A44F6F311ABA04759F358957C979FA4FE2A52DD09B378BD7405BFE436` |
| 迁入后模块 .h | **2,964 B / 62 行**，`5F8C3F29B78A9403017AF08CA88702C163A10F5C078785691A631D0B9B0205A8` |

块内含：来源分桶 `switch`、provenance 细分 `switch`、stable-part 身份推导
（`stablePartKey` / `strictSliceKey`）、三张 8192 项跨帧 `thread_local` 探针表及其全部
读改写、环形窗口 uniqueness 计算、delta 三档分桶与 stale→live 归因、lease-key 多值归因。

### 1.2 唯一机械差异

删除块的第 2 行（pre-M2-5 `:21203`）：

```cpp
      auto& stats = m_war3Scene.shadowStats;
```

该引用改为函数第一个形参 `stats`。除这一行（以及为放进函数所需的换行/缩进**不变**——块本身
就在 4/6/8 空格缩进层，函数体复用同一缩进）之外，**没有任何 token 被改写**：
判定顺序、`switch` 分支、比较对象、计数增量、`static thread_local` 声明与位掩码取槽全部原文。

### 1.3 device.cpp 侧留下的调用点（原文，CRLF）

```cpp
    // M2-5（2026-09-18）：taxonomy 发射块（34 个 stats 字段 + 3 张跨帧 thread_local
    // 探针表）已逐字节迁往 src/d3d9/war3/semantic/war3_palette_taxonomy_emission.{h,cpp}
    // 的 War3EmitSemanticPaletteTaxonomy——skinned 条件与诊断门本身随块迁移。
    // 调用点编排留在 device.cpp 原位；等价证据见
    // docs/plan/2026-09-18-m2-5-taxonomy-extraction-record.md。
    War3EmitSemanticPaletteTaxonomy(
        m_war3Scene.shadowStats, skinned, paletteSourceThisSubmit,
        currentDrawSample, drawTimeCapturedPaletteProvenance,
        effectiveCanonicalPalette, effectiveCanonicalPaletteCount,
        paletteSlotIndexThisSubmit, submittedPaletteHash,
        fromStalePoseRestore, m_war3ShadowPersistentFrameSerial);
```

另有 1 行 include（`war3/semantic/war3_palette_taxonomy_emission.h`，紧跟 M2 模块 include）
与 4 行 using-directive 注释。**逐行 diff 复核**：全文件仅这 3 处 hunk（`difflib` 归一 LF 后
381 行删除 / 14 行新增，含 2 行上下文），无任何其它漂移。

### 1.4 落点与命名理由

- 落点：**新文件** `src/d3d9/war3/semantic/war3_palette_taxonomy_emission.{h,cpp}`。
  理由：(a) 与已迁模块同目录、同命名空间 `dxvk::war3::semantic`、同 using-directive 解析机制；
  (b) 该块是**发射/记账**职责，与 `war3_live_palette_selection.*`（选择链）是不同的单一职责，
  单独成 TU 后三张探针表的存储域、初始化时机与副作用面被精确限定在一个翻译单元内，
  便于审计与将来回退；(c) 一处迁移 = 一处 provenance，`.inc` 与模块正文的一致性证明只需
  比对同一段文本。
- 函数名：`War3EmitSemanticPaletteTaxonomy`。**注意它不匹配**预算门禁的
  `SEMANTIC_FAMILY_RE` 语义选择命名族（无 Resolve/Select/Runtime/Key/… 后缀）⇒
  判定 3（回流）对它盲，必须显式登记 `FROZEN_M2_5`（见 §2、§8 变异 B）。

## 2. provenance（fail-closed）与 baseline 实测

| 项 | 值 |
| --- | --- |
| pre-M2-5 工作树 `src/d3d9/d3d9_device.cpp` | **2,273,048 B** / `D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE` |
| 捕获时间（机器本地时钟） | `2026-09-18T03:05:30` |
| `%TEMP%` 备份 | `%TEMP%\device_pre_m2_5.cpp`（2,273,048 B / 同 SHA，脚本比对 `match True`） |
| 交叉验证 | 该 SHA **逐字节等于** M2-3 记录 §2 的「迁移后 device.cpp」⇒ M2-5 的迁移前状态就是 post-M2-3 工作树，闭合 |
| worktree-vs-git-blob | 与前四片相同：工作树文件哈希，**不是** git blob（M1–M2-3 均未提交） |

同一次捕获的其它备份（`%TEMP%`，供变异还原与「未改动」自证）：
`module_pre_m2_5_live_palette_selection.{h,cpp}`、`test_pre_m2_5_war3_live_palette_selection_test.cpp`、
`gen_pre_m2_5_legacy_reference.py`、`budget_pre_m2_5_static.py`、`export_pre_m2_5_static.py`、
`hotpath_pre_m2_5_static.py`、`meson_pre_m2_5_d3d9.build`。

**baseline 实测（FROZEN_M2_5 = `{"War3EmitSemanticPaletteTaxonomy": (0, 0)}`）**：
用预算门禁**同一个** `DEF_RE` 与同一「第 0 列起始、非注释行」规则扫 pre-M2-5 快照的
`:21202-21580` ⇒ **0 个行首站点**（块内所有声明都在缩进层；三个 `struct *Entry` 以 `{` 结尾
本就不匹配 `DEF_RE`）；且 pre-M2-5 device.cpp 全文中 `War3EmitSemanticPaletteTaxonomy`
出现 **0** 次。⇒ `baseline = 0`、`budget = 0`，与迁移**同一次改动**落地（budget=0 只在
「块已迁出且函数不在 device.cpp」时成立）。

## 3. 纯函数的输入清单（全部显式传参）

### 3.1 签名（模块 .h 声明与 .inc legacy 包装同形）

```cpp
void War3EmitSemanticPaletteTaxonomy(
    dxvk::War3ShadowCaptureStats& stats, bool skinned,
    War3SemanticPaletteSource paletteSourceThisSubmit,
    const dxvk::war3::render::CurrentDrawAuthoritativeSample* currentDrawSample,
    dxvk::war3::render::PaletteProvenance drawTimeCapturedPaletteProvenance,
    const std::vector<Matrix4>* effectiveCanonicalPalette,
    uint32_t effectiveCanonicalPaletteCount,
    uint32_t paletteSlotIndexThisSubmit, uint64_t submittedPaletteHash,
    bool fromStalePoseRestore,
    uint64_t m_war3ShadowPersistentFrameSerial);
```

返回 `void`：唯一输出面就是 `stats`（引用参数）。

### 3.2 形参 ↔ 迁移前块内引用 对照

| # | 形参 | 迁移前块内用法 | 类型 | device.cpp 侧来源（行号为迁移前） |
| --- | --- | --- | --- | --- |
| 1 | `stats` | `stats.semanticScene*` 全部 34 字段读写 | `War3ShadowCaptureStats&` | 块首别名 `m_war3Scene.shadowStats`（已删除，改传参） |
| 2 | `skinned` | `if (skinned && War3SemanticPaletteDiagnosticsRuntime())` | `bool` | 调用函数内局部（`packet.path == …::Skinned`） |
| 3 | `paletteSourceThisSubmit` | 来源分桶 switch + source churn 比较 | `War3SemanticPaletteSource` | :20834 声明 / :20853 赋值 |
| 4 | `currentDrawSample` | `!= nullptr`（两处）与 `->contract` 三字段 | `const CurrentDrawAuthoritativeSample*` | :20307 |
| 5 | `drawTimeCapturedPaletteProvenance` | provenance 细分 switch | `PaletteProvenance` | :20368 / :20505 |
| 6 | `effectiveCanonicalPalette` | `!= nullptr` / `->empty()` / `[0]`（两处 first-matrix） | `const std::vector<Matrix4>*` | :21158 |
| 7 | `effectiveCanonicalPaletteCount` | count churn 比较与 entry 记录 | `uint32_t` | :21160 |
| 8 | `paletteSlotIndexThisSubmit` | slot churn 判定与 window 记录 | `uint32_t` | :20835 / :20854 |
| 9 | `submittedPaletteHash` | hash churn / window / 早退条件 | `uint64_t` | :21184 |
| 10 | `fromStalePoseRestore` | stale→live 归因与 entry 落值 | `bool` | `War3TryAppendSemanticShadowPacket` 形参 :19921 |
| 11 | `m_war3ShadowPersistentFrameSerial` | `entry.lastFrame` / lease-key 帧比较 | `uint64_t` | `d3d9_device.h:2556` 成员 |

形参名与迁移前块内标识符**逐字相同**（含 `m_war3ShadowPersistentFrameSerial`）——这样模块正文
与 legacy 正文才能逐字节相同，差分才真正隔离「被搬动的文本」。3 个 `dxvk::` / `dxvk::war3::render::`
限定名保持不变。

块内**无**限定的 `War3SemanticPaletteDiagnosticsRuntime()` 是 M2-1 已迁出的 env getter：
模块侧解析到模块自身声明，legacy 侧解析到 `m2_legacy_reference` 里 M2-1 的 legacy 副本
（与 pre-M2-5 device.cpp 经 using-directive 的真实调用目标一致）。

### 3.3 探针表的归属决定（设计裁定，必须显式说明）

清册 §3.2 指出：这三张跨帧 `thread_local` 表「不在任何可构造参数里」，要能测就必须**把表的
所有者一起搬进模块并保持 `thread_local` 语义**。本轮采用这一裁定：

- 表**留在块内**（因此正文逐字节），即函数内 `static thread_local std::array<…>`；
- 存储域随块迁入新翻译单元；**初始化时机**（首次执行到该声明）与**存活域**（线程）不变；
- 因此**没有**把它们改写成参数、普通 `static` 或 `std::unordered_map`（清册 §2.3 失败模式 3）。

这样 host 差分才能像 M2-3 一样：两侧各持**独立**的表副本，由同一调用序列驱动，状态演化必须逐点一致。

### 3.4 无 device 成员 / registry / hook 全局访问的自证

迁移脚本在抽取时对正文做 fail-closed 扫描：出现 `m_war3Scene` / `m_war3Semantic` / `m_state` /
`VisibleRenderableRegistry::instance` 任一即拒绝生成（本轮 0 命中）。模块 .cpp 全文亦无这些 token。
唯一外部函数调用是 `dxvk::war3::render::VisibleRenderableRegistry::computeShadowManifestPartKey`
——它是**不读注册表实例状态的纯静态哈希**（`war3_visible_renderables.h:219` 声明为 `static`），
不是 registry 全局访问。

## 4. 34 字段对照表

口径：字段名去重后 **34** 条（与清册 §1.2 B3 的「34 个 `stats.` 字段」一致，本轮用
`stats\s*\.\s*(semanticScene…+)` 正则独立复算得到同一数字）。行号：迁移前 = pre-M2-5
device.cpp 块内；迁移后 = 模块 .cpp。**多累加点一律逐点列出**（清册 §2.3 失败模式 2）。

| # | 字段 | 迁移前 device.cpp 累加/写入行 | 迁移后模块 .cpp 行 |
| --- | --- | --- | --- |
| 0 | `semanticSceneSubmittedSkinnedPaletteSourceNoneCount` | 21207 | 45 |
| 1 | `semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount` | 21210 | 48 |
| 2 | `semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount` | 21239 | 77 |
| 3 | `semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount` | 21242 | 80 |
| 4 | `semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount` | 21245 | 83 |
| 5 | `semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount` | 21248 | 86 |
| 6 | `semanticSceneSubmittedSkinnedPaletteSourceOwnedPartSnapshotCount` | 21251 | 89 |
| 7 | `semanticSceneSubmittedSkinnedPaletteSourceChurnCount` | 21337 | 175 |
| 8 | `semanticSceneSubmittedSkinnedPaletteProvenanceTrustedBlendedWriterCount` | 21216 | 54 |
| 9 | `semanticSceneSubmittedSkinnedPaletteProvenanceRawGlobalArenaCount` | 21219 | 57 |
| 10 | `semanticSceneSubmittedSkinnedPaletteProvenanceProducerPartPacketCount` | 21222 / 21254 | 60 / 92 |
| 11 | `semanticSceneSubmittedSkinnedPaletteProvenanceRangeCopyPoseRebuildCount` | 21225 | 63 |
| 12 | `semanticSceneSubmittedSkinnedPaletteProvenanceCModelFallbackCount` | 21228 | 66 |
| 13 | `semanticSceneSubmittedSkinnedPaletteProvenanceUnknownCount` | 21231 / 21235 | 69 / 73 |
| 14 | `semanticSceneSubmittedSkinnedPaletteStablePartSampleCount` | 21332 | 170 |
| 15 | `semanticSceneSubmittedSkinnedPaletteHashChurnCount` | 21334 | 172 |
| 16 | `semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount` | 21344 | 182 |
| 17 | `semanticSceneSubmittedSkinnedPaletteCountChurnCount` | 21382 | 220 |
| 18 | `semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax` | 21427 / 21428 | 265 / 266 |
| 19 | `semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax` | 21433 / 21435 | 271 / 273 |
| 20 | `semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount` | 21377 | 215 |
| 21 | `semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount` | 21373 | 211 |
| 22 | `semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount` | 21358 | 196 |
| 23 | `semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount` | 21365 | 203 |
| 24 | `semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount` | 21368 | 206 |
| 25 | `semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount` | 21455 | 293 |
| 26 | `semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount` | 21490 | 328 |
| 27 | `semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount` | 21496 | 334 |
| 28 | `semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount` | 21539 | 377 |
| 29 | `semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount` | 21542 | 380 |
| 30 | `semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount` | 21564 | 402 |
| 31 | `semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount` | 21558 | 396 |
| 32 | `semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount` | 21555 | 393 |
| 33 | `semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount` | 21552 | 390 |

**单一实现证明**：34 个字段名在 device.cpp 出现 **0** 次（清册 §2.3 失败模式 5：双重计数）；
在 `d3d9_war3_scene.h` 各 **1** 次（定义）；在模块 .cpp 的出现次数为上表行数：其中 4 个字段各
**2** 次（`ProvenanceProducerPartPacket` 的双累加点、`ProvenanceUnknown` 的 switch-default 与
null-sample else、两个 `UniqueInWindowMax` 的「读 + 写」），其余各 1 次。该逐名次数表由
`AutoTest/test_war3_palette_taxonomy_equivalence_static.py` fail-closed 钉死。

## 5. 三张跨帧 thread_local 探针表

| 表 | 结构 | 容量 | 模块 .cpp 行 |
| --- | --- | --- | --- |
| Palette hash/slot 探针 | `PaletteProbeEntry` + keys | 8192 | `kPaletteProbeEntries` :145；keys :146；entries :148 |
| lease-key 归因 | `LeaseKeyAttributionEntry` | 8192 | `kLeaseAttrEntries` :309；entries :310 |
| strict-slice 探针 | `StrictProbeEntry` + keys | 8192 | `kStrictProbeEntries` :354；keys :355；entries :357 |

共 **5 个 `static thread_local std::array`**（2+1+2）。三者都保留 `size_t(key) & (k…Entries - 1u)`
位掩码取槽、`entry = Entry{}` 重置、以及「新 key 或被别的 key 冲掉的 slot」语义。
静态门禁钉死：容量常量文本、5 个 `static thread_local std::array<`、三处位掩码表达式。

## 6. legacy 参考与双向 fail-closed 生成器

- 新文件 `src/d3d9/war3/semantic/tests/war3_palette_taxonomy_emission_legacy_reference.inc`
  = **22,511 B / 437 行**，
  `A5333AC7E7F5A080324F5D83668D394C53F26E7ADEEEC5DA2C76E9BFEE26105E`。
- 由入库生成器 `AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-5` 产出；
  **M2-1/M2-2/M2-3 三份 .inc 与其 SHA 一字未动**。
- 生成器 fail-closed 三步（比前三片多一条）：(1) 快照 SHA-256/字节数必须等于登记常量；
  (2) 块行号必须等于 `:21202-21580`、`stats` 别名行必须恰好 1 行、正文不得含 device/registry token；
  (3) **模块 .cpp 的 BEGIN/END 标记之间正文必须与本 .inc 正文逐字节相同**
  （实测两侧均为 **18,937 B**）——这使「参考 = 实现」不再依赖人工比对，任何一侧漂移都会在
  生成时立刻失败。
- 命名空间改写规则（除 LF 归一外唯一的机械编辑）：外层改为 `m2_legacy_reference`；块内
  无限定的 `War3SemanticPaletteDiagnosticsRuntime()` 解析到 M2-1 legacy 副本；
  无限定的 `War3SemanticPaletteSource` 解析到 M2-2 legacy 枚举副本（两侧枚举底层值相同，
  测试按数值驱动）；`War3ShadowCaptureStats` / `CurrentDrawAuthoritativeSample` /
  `PaletteProvenance` 是**共享真实契约类型**（两侧同一份头文件，刻意不重写）。
  正文本身的 token **一个都没改**。

## 7. 宿主差分等价测试

差分测试：`src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp`
（M2-1/M2-2/M2-3 既有断言一字未动的**加法式扩展**；meson 目标 `war3_live_palette_selection_test`
新增链接真实模块 `.cpp`，目标名与既有 test 名未变 ⇒ meson 目标数不变）。

### 7.1 结构与比较口径

- 两侧各持独立 `War3ShadowCaptureStats`；三张探针表是两侧**各自编译**的独立副本。
- **逐调用**比较 34 个字段（整数精确相等，容差 0）；每个场景段结束对整个
  `War3ShadowCaptureStats` 做 `memcmp`（结构 `static_assert` 平凡可复制），以捕获
  「写到了这 34 个字段之外」。
- 电池需要 `VisibleRenderableRegistry::computeShadowManifestPartKey` 的**宿主机替身**
  （该静态哈希的定义在 `war3_visible_renderables.cpp`，本宿主机测试不链接那个 TU——M1
  「测试自己提供设备层原语替身」的同一先例）。替身对 `jHandle`/`unitPtr` 做 splitmix 混合，
  两侧共用同一替身 ⇒ 差分隔离出的正是 M2-5 搬动的文本；替身本身**不在**覆盖范围内（§10）。
- **诊断门 OFF 的绝对断言**：默认 env 下两侧都必须保持 34 字段全 0（不靠双侧互证），
  即钉死「第一道门仍是 `skinned && getter`」。

### 7.2 场景电池

| 电池 | 规模 | 覆盖 |
| --- | --- | --- |
| 系统网格 | **127,008 次调用** | `skinned{0,1}` × 来源 7 × provenance {0,1,2,3,4,5,越界 9} × palette {nullptr,空,1 矩阵,2 矩阵} × count{0,1,2} × hash{0,1,0x1234…} × slot{0xFFFFFFFF,0,5} × stale{0,1} × frame{0,1} × 样本{nullptr,2 个 key} |
| 定长序列 | 10 帧同 key + 5 步 lease-key + 同槽异 key 4 步 | 环形窗口填满/回绕与两个 `UniqueInWindowMax`、hash/slot/count/source churn、delta 三档、stale→live 与 live→live 归因、lease-key 多值、不同 key 命中同一槽位的替换路径 |
| 定种子随机世界 | 9,000 个互异 key 顺序填满/越过 8192 槽 + **250,000 轮** splitmix64 | key 池 1700（sameKey 与 churn）、ptr 空指针注入、hash=0 早退、越界 provenance、随机帧号 |

合计 **386,008 次两侧调用**；每次 34 个字段检查 ⇒ 13,124,272 条 M2-5 断言；
另有 4,053 次段末整结构 `memcmp`。

### 7.3 实测（最终树，四矩阵）

| env 矩阵 | 结果 |
| --- | --- |
| default（诊断门 OFF） | `30443384/30443384 checks passed`，EXIT=0 |
| `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1` | `30443383/30443383 checks passed`，EXIT=0 |
| `DXVK_WAR3_SKIN_PALETTE_CONTRACT=1` | `26955272/26955272 checks passed`，EXIT=0 |
| `…CONTRACT=1` + `…DIAGNOSTICS=1` | `26955271/26955271 checks passed`，EXIT=0 |

（检查数是**全文件累计**，含 M2-1/M2-2/M2-3 既有电池；M2-5 独占约 13.1M。）

### 7.4 `--probe-taxonomy` 独立期望值表

16 个固定场景 × 2 组 env（`diagnostics=enabled` / `default`），逐场景要求
module == legacy **全部 34 字段**相等，并与门禁内**独立登记**的期望值表逐字段核对
（不是与 legacy 互相印证）。诊断门 OFF 时 16 个场景必须**全 0**。

覆盖到全部 34 个字段的非零语义（来源 8 桶 / provenance 6 桶 / stable·churn 20 桶 /
lease-key 2 桶 / strict-slice 6 桶）。**诚实记录**：期望值表先按源码逐字段手工推导，
再与实跑对照；对照暴露出我 2 处手工推导错误（`source-churn` 漏了 `SourceChurnCount` 本身、
`medium-delta` 误把 `LiveToLiveLarge` 算进中档——它只在 `deltaSq > 1.0f` 分支内），
**以源码重新推导后修正**，最终 16 行全部与手工期望一致才写入门禁。

### 7.5 关于「44 条计数器」：本轮抽取覆盖其中 34 条

清册 §2.1 的 44 条 = B3 块内 **34** 条 + B4 每帧 skinned palette 聚合 **7** 条
（dev :22678-22724）+ B5 direct-caster churn **1** 条（dev :22774）+ 护栏 **2** 条
（`budgetExceeded` / `m_war3ShadowFallbackBudgetExceeded`，属 Arena/budget 主题）。
本轮抽出的就是 B3，故差分覆盖 **34/44**：

- 34 条：逐字段整数精确相等 + 段末 `memcmp`（§7.3）**已覆盖**；
- B4 7 条 / B5 1 条：属**调用点编排与每帧聚合**，本轮**未迁、未改、未被本电池覆盖**
  （它们在 device.cpp 原位，属 M2-5 余项）；
- 护栏 2 条：B3 块**不写**这两个字段；本轮没有新增/移动任何写入点（见 §9 门禁表与
  `test_shadow_arena_memory_budget_static.py` 全量静态通过）。

## 8. 变异验证（2 条真跑，原始输出见 `…-mutation-raw-output.log`）

### 8.1 变异 A：模块侧计数条件反转（差分必须红）

- 改：模块 .cpp 的 `if (entry.lastHash != submittedPaletteHash) {` → `==`
  （stable-part hash churn 的计数条件反转）。变异后 SHA `D9E1AD3E…C976B88`。
- 构建：`NINJA_EXIT=0`（**能编译**——正是「语义漂移但不报错」的回归类型）。
- 跑（红）：`DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1` ⇒ EXIT=1，
  `30116847/30443383 checks passed`，stderr **326,536 条 DIFF**，首条：
  `DIFF line 3141 TaxGrid.hashChurn: module=1 legacy=0`。
- 独立 M2-5 门禁同时红（退出 1）：`模块 .cpp 哈希与登记值不一致（迁移文本已漂移）`
  ——该门禁的**模块身份 pin** 比动态差分更早咬住；差分本身的红见上一条。
- 还原：`%TEMP%\module_post_m2_5_taxonomy_emission.cpp` 复制回 + `os.utime` 置为当前时刻；
  SHA `E8C6F74A…E436` **逐字节相同（MATCH True）**；真实重编译后复跑
  `30443383/30443383 checks passed`、EXIT=0。

### 8.2 变异 B：职责回流（预算门禁必须点名）

- 改：在 device.cpp 的 `using namespace dxvk::war3::semantic;` 之后插入
  `namespace warvk_m2_5_reflow_probe { <模块正文逐字节> }`（定义行从第 0 列开始，
  与 M2-3 变异 2b 同形；具名命名空间避免与 using-directive 导入的模块声明歧义）。
  变异后 device.cpp = 2,274,802 B / `62F71A82…C603160`。
- 构建：`NINJA_EXIT=0`，日志尾 `[3/3] Linking target src/d3d9/d3d9.dll`
  ⇒ **可编译回流**（编译器不拦）。
- 预算门禁（红并**点名**）：exit 1，
  `AssertionError: d3d9_device.cpp 中语义选择职责符号超过冻结预算（职责回流）：` /
  `  War3EmitSemanticPaletteTaxonomy: device.cpp=1 > budget=0`。
- 独立 M2-5 门禁**也**红（exit 1）：`device.cpp 仍出现已迁出字段名（双重计数风险）：…SourceNoneCount`
  ⇒ 本轮这条回流有**两个**捕手（预算门禁判定 1 + M2-5 门禁的字段单一实现断言）；
  即使 M2-5 门禁不存在，`FROZEN_M2_5` 的判定 1 仍能单独点名（这正是必须登记的原因：
  该函数名不匹配语义命名族，判定 3 盲）。
- 还原：SHA `7391F307…8A0844` **逐字节相同（MATCH True）**；真实重编译
  （日志尾 `[2/2] Linking target src/d3d9/d3d9.dll`）后预算门禁 exit 0
  （冻结符号 150；行首站点 迁移前 159 → 上限 115 → 当前 115；已迁出 38；device.cpp 站点 577）。

### 8.3 还原完整性（SHA-256 证明）

| 被变异文件 | 变异后 SHA-256 | 还原后 SHA-256 | 逐字节相同 |
| --- | --- | --- | --- |
| 模块 `war3_palette_taxonomy_emission.cpp` | `D9E1AD3E…C976B88` | `E8C6F74A44F6F311ABA04759F358957C979FA4FE2A52DD09B378BD7405BFE436` | **是** |
| `d3d9_device.cpp` | `62F71A82…C603160` | `7391F3071F7D2CF32583C57FF1D914FC59AA765A5EDD5749F5D3A8FCBD8A0844` | **是** |

**构建陷阱（M2-3 已实测，本轮复核并规避）**：`Copy-Item`/`copy2` 保留源文件 mtime 会让
ninja 判 `no work`，只比源码 SHA 会得到假绿/假红。本轮还原一律 `copy2` + `os.utime` 显式 touch，
并以构建日志里的真实编译/链接行确认二进制来自还原后的源码。

## 9. 全量门禁复跑（最终树）

窗口：两条变异均已还原、源码 SHA 与 §2/§8.3 一致。原始输出 `…-full-gate-rerun.log`。

| # | 门禁 | 原始结果 |
| --- | --- | --- |
| 0 | 源码身份 | device.cpp 2,254,826 B / `7391F307…`；模块 .cpp 21,020 B / `E8C6F74A…`；模块 .h 2,964 B / `5F8C3F29…`；.inc 22,511 B / `A5333AC7…`；测试 139,504 B / `098A34E2…` |
| 1 | `ninja -C build32 -n` | `ninja: no work to do.`，EXIT=0 |
| 2 | 构建 `build32/src/d3d9/d3d9.dll`（Below Normal + `-j2`，`run_ninja_m2_5.cmd`） | `NINJA_EXIT=0` |
| 3 | 预算门禁 | EXIT 0：`冻结符号 150；行首站点 迁移前 159 -> 迁出后上限 115 -> 当前 115；已迁出 38 个符号；device.cpp 行首定义站点总数 577`（M2-3 后为 149/159→115/38/577；本轮唯一变化是冻结集合 +1、baseline/budget 各 +0） |
| 4 | M1 等价门禁（**一个字节不改**） | EXIT 0：`已迁出符号 26 个全部有 legacy 差分覆盖；legacy 参考 43452 B / SHA-256 2EA43F9782BE388E…` |
| 5 | M2 等价门禁（M2-3 扩展后） | EXIT 0：M2-1 8 个 + M2-2 链 1 个全部有 legacy 差分覆盖；M2-1 参考 7702 B / `D458C1AE…`；链参考 36025 B / `02FF8AFE…`；probe 6 + probe-chain 10；M2-3 motion 3 个 + 2 Entry |
| 6 | M2-3 等价门禁 | EXIT 0：motion 3 符号 + 2 Entry；motion 参考 8406 B / `7DF61688…`；pre-M2-3 device.cpp 2,278,494 B / `ECC4B828…`；probe-motion 15 场景；`motion-on 差分 30443383 断言（下限 12000000）` |
| 7 | **M2-5 等价门禁（本轮新增）** | EXIT 0：`M2-5 已迁出符号 1 个；34 个 taxonomy 字段单一实现；legacy 参考 22511 B / A5333AC7…；模块正文与 legacy 正文逐字节相同 18937 B；pre-M2-5 device.cpp 2273048 B / D8211864…；probe-taxonomy 场景 16 × 2 组 env；diagnostics-on 差分 30443383 断言（下限 21000000）` |
| 8 | `meson test -C build32 --num-processes 2` | `Ok: 83  Fail: 0`，EXIT=0（**83→83**：本轮是既有目标的加法式扩展，未新增 meson 目标） |
| 9 | 全量静态 `AutoTest/test_*_static.py` 逐个 | `STATIC_TOTAL=251 STATIC_PASS=251 STATIC_FAIL=0`（**250→251**：新增 M2-5 等价证据门禁 1 个） |
| 10 | 记录器 `war3_palette_object_evidence_test` | `SUMMARY: 23 passed, 0 failed`，EXIT=0 |
| 11 | 成本 `war3_palette_object_evidence_cost_test` | `COST_VERDICT=PASS checks=38 failures=0`，EXIT=0 |
| 12 | 生命周期 `war3_shadow_build_lifecycle_test` | `SUMMARY: … 187/187 case(s) passed`，EXIT=0 |
| 13 | 解析器静态 `AutoTest/test_palette_object_evidence_analysis_static.py` | `Ran 94 tests … OK`，EXIT=0 |
| 14 | 根读方 `AutoTest/test_analyze_frame_evidence.py` | `Ran 55 tests … OK`，EXIT=0 |
| 15 | 往返 `py AutoTest/test_palette_object_wire_roundtrip.py …` | `CHECKS=1107 FAILURES=0`；`ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`（A–F）；EXIT=0 |
| 16 | 差分四矩阵 | §7.3（全部 EXIT=0） |

**跟随迁移改锚的既有门禁（断言语义未削弱）**：
`test_active_path_palette_instrumentation_export_static.py` 与
`test_semantic_palette_diagnostics_hotpath_static.py` 原先从 device.cpp 断言
`if (skinned && War3SemanticPaletteDiagnosticsRuntime())`；本轮把读取锚改到模块 .cpp，
并**加严**为「该字符串在模块中恰好 1 处、device.cpp 中 0 处、device.cpp 调用点恰好 1 处」。

**一个需要点名的新事实**：M2-3 门禁打印的 `motion-on 差分` 数字本轮从 17,314,411 变成
30,443,383——因为它统计的是**全文件累计**检查数。该门禁的下限（12,000,000）仍满足，
M2-3 语义覆盖不受影响，但该数字**不再等价于**「M2-3 专项覆盖量」。

### 9.1 DLL 身份与可复现性

- 最终产物 `build32/src/d3d9/d3d9.dll` = **36,272,891 B**，
  SHA-256 `34FB05C517B178AFF8D1154DF95624353B4EE6CA84DD3E3959D04E1709512117`。
- 对照 M2-3 的最终 DLL（36,271,207 B / `0090B354…`）⇒ 尺寸 **+1,684 B**：device.cpp 净减
  19,359 B 定义 + 1,137 B 调用点/注释、模块新增一个 TU（21,020 B 源码，含同样的代码与
  同样的 5 个 thread_local 数组，但不再与 device.cpp 同 TU 共享内联机会）。
- **构建可复现性实测（诚实标注）**：同一源码在两次链接后得到**同尺寸、不同 SHA** 的 DLL。
  强制重链一次做逐字节比对：36,272,891 B 相同，**仅 6 个字节不同**，分别位于
  PE 头 `TimeDateStamp`（偏移 136，`0x6AAC3CEA` → `0x6AAC3DD2`，即链接时刻）、
  偏移 216 的第二个 COFF/调试时间戳、偏移 30,834,692 的第三个时间戳字段。
  ⇒ 「同源码 ⇒ 同尺寸」成立、「同源码 ⇒ 同 SHA」**不成立**（MinGW ld 默认写入链接时间）。
  本记录登记的是**最终树最后一次真实构建**的身份；此前同一最终源码曾产出
  `8253BE9E…` 与 `43B63F92…`（同样 36,272,891 B）。
- **未部署**：本轮没有覆盖任何游戏目录文件；未启动游戏或编辑器；`E:\Work` 下未触碰任何文件。

## 10. 未覆盖项（如实声明）

1. **device.cpp 调用点编排（B1/B2 等）不在本轮范围**：live palette refresh 块、paletteSource/
   slotIndex 装配、`War3TryBuildLiveRuntimeGroupPalette` 提交点、`War3NoteLivePaletteMotion`
   调用点等一个字节没改，也**没有**被本电池覆盖（属 M2-5 余项）。
2. **调用点本身是新增文本**：参数是原局部量、语义等价，但「调用点文本」是新的，
   差分不覆盖「device.cpp 取到的实参是否与原块一致」——这只能由代码审阅 + 编译 + 后续实机门覆盖。
3. **34 字段的出口链未由本门禁覆盖**（bridge / hub / control plane / perf ×2 写手 / 报告）；
   由既有 `test_active_path_palette_instrumentation_export_static.py`、
   `test_skin_palette_contract_static.py` 等静态门禁与全量静态承担。
4. **`VisibleRenderableRegistry::computeShadowManifestPartKey` 在宿主机是替身**：
   差分/probe 证明「同一输入下两个实现一致」，**不证明**该渲染侧哈希本身；该哈希本轮未迁、未改。
5. **单线程**：三张表的 `thread_local` 语义由「原文未改 + 静态门禁钉死声明形态」保证，
   但本轮**没有**做多线程/多设备并行差分（清册 §2.3 失败模式 3 只被静态钉死，未被动态覆盖）。
6. **env 覆盖为 4 个矩阵**而非穷举；env 取值形态（0/1/未设置/十六进制/非数字/空串）由 M2-1
   的 `--probe` 表承担，本电池只覆盖诊断门 ON/OFF。
7. **B4 7 条 / B5 1 条 / 护栏 2 条不在本轮覆盖**（见 §7.5）：本轮既未迁它们，也未为它们
   新增差分；清册 §2.1 的分组 4/5/6 仍是 M2-5 余项。
8. **变异 B 只证明门禁的点名能力**：那段回流代码没有任何调用者，因此不覆盖「回流后的运行行为」；
   它证明的是「可编译回流也不会漏过判定 1（+ 本轮的 M2-5 门禁）」。
9. **`--probe-taxonomy` 的期望值表**是「先手工推导、再与实跑对照修正」得到的回归 pin，
   不是独立第三方来源；它的价值是钉死绝对语义与发现「两侧同步漂移」，不是数学证明。
10. **本测试不覆盖 GPU / Vulkan / 实机画面 / 性能**；未部署、未启动游戏、未做玩家前台验收。
    本轮全部证据为宿主机差分 + 静态门禁，**不构成稳定版依据**，也不得表述为
    「palette 侧职责已迁完」（M2-4 与 M2-5 余项仍未做）。
11. **M2-3 门禁打印的 check 数已变为全文件累计值**（§9），该数字的解释需更新（门禁本身未改）。

## 11. 与其它文档的边界

- 本文**不改**`2026-09-18-m2-5-taxonomy-and-remaining-scope.md` 的任何结论；它是设计与清册。
- 本文**不改** M2-1/M2-2/M2-3 的等价记录、`.inc`、既有门禁的语义（只按先例改锚并加严）。
- 本文**不是**「M2-5 已完成」：只完成了 M2-5 的 taxonomy 发射块这一项（清册表 B 的 B3）；
  B1/B2/B4/B5/B6–B11 与 M2-4 全部未做。
- 本轮**未** git 写、未部署、未启动游戏、未称稳定版。

## 12. 文档写入后的复跑

本记录 §1–§11 与进度日志 / 开发台账写入完成后再跑一次预算门禁、M1/M2/M2-3/M2-5 等价门禁
与 `ninja -n`，要求全部 exit 0（M2-3 同型）。结果见本节末尾的复跑记录。

- 复跑窗口（机器本地时钟）：2026-09-18T03:26:27。
- `ninja -C build32 -n` → `ninja: no work to do.`，exit 0。
- 预算门禁 exit 0：`冻结符号 150；行首站点 迁移前 159 -> 迁出后上限 115 -> 当前 115；已迁出 38 个符号；device.cpp 行首定义站点总数 577`。
- M1 等价门禁 exit 0；M2 等价门禁 exit 0；M2-3 等价门禁 exit 0。
- 新 M2-5 等价门禁 exit 0：模块正文与 legacy 正文逐字节相同 **18,937 B**、probe-taxonomy 16 场景 × 2 组 env、diagnostics-on 差分 30,443,383 断言（下限 21,000,000）。
- 改锚后的 `test_active_path_palette_instrumentation_export_static.py` 与 `test_semantic_palette_diagnostics_hotpath_static.py` exit 0。
- ⇒ §9 的数字来自**含本文档最终内容**的树；本轮未再改动任何源码或门禁。
