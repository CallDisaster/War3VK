# 2026-09-18 — T7 只读勘察：Registry domain 隔离 → Stage13 现状清册与迁移建议

> **状态：勘察与建议。未实施。** 本文**不含任何代码改动、不含任何构建、不含任何测试执行**
> （只做了只读 grep/读文件 + 一次只读静态门禁，见 §5）。
> **本文不得被引用为"Registry domain 隔离已完成"、"Stage13 已迁移"或"Stage13 已批准"。**
> 引用本文时须同时引用 §5 的未实施声明。
>
> 输入：\`docs/plan/2026-09-16-stage13-port-audit.md\`（A 88089cd → B 移植核对摘要）、
> \`docs/plan/2026-09-16-stage13-retention-verification-prereq.md\`（占用与回收前置条款）、
> \`docs/agent-history/2026-08-26-native-static-shadow-path-blocker-bridge-cache.md\`（A 侧 checkpoint 叙述）、
> \`docs/plan/2026-09-18-deepseek-overnight-handoff.md\` §4 阶段 4 第 2 条。
>
> 观测 revision：与 \`docs/plan/2026-09-18-m2-5-taxonomy-and-remaining-scope.md\` §0 同一版本
> （\`d3d9_device.cpp\` 2,273,048 B / SHA-256 \`D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE\`；
> \`d3d9_device.h\` 134,216 B / \`E6F97012AFCC3003C572DEC5F63F0EE90264810BE9B92292D602D3DEDE9F0AE3\`；
> \`d3d9_war3_scene.h\` 121,062 B / \`8B5B33DC47062B2D01DA585695EBA3B8DC523F5FA54FD7567146F7F31BC5DDEC\`）。
> 按交接纪律：行号会漂移，引用以**符号名**为锚。

---

## 0. 结论摘要（五条，每条都可在本树复核）

1. **B 树的 Registry domain 隔离：未开始。** 全 \`src\` 搜索 \`IdentityDomain\` = **0**、
   \`Stage13Exact\` = **0**、\`PersistentGeometryDomain\` = **0**、\`OwnerCheck/owner-check\` = **0**、
   \`kWar3Stage13…ProofCap\` = **0**。审计文档 U5「Registry publish/GC/reset domain 隔离
   （Generic vs Stage13Exact）」**在 B 树没有任何代码级痕迹**。
2. **B 树只有一个共享 registry**：\`m_war3ShadowGeometryRegistry\`（key → \`{geometryId, instances, instanceable}\`）
   + \`m_war3ShadowPersistentGeometries\`（geometryId → 条目），**两者都不带 domain 字段**。
   domain 今天只以**隐式魔法数写进 layoutHash**（S1 用 \`0x53310001u\`、Stage13 用 \`0x53314301u\`）
   与 \`key.mode\`（\`War3ShadowReplayMode\`）间接表达 ⇒ 结构上**不能阻止跨 domain 命中**。
3. **Stage13 在 B 是 opt-in 的 CPU 常驻，不是 A 的"registry-owned content-persistent geometry"**：
   五个 Stage13 门默认全 \`0u\`（\`STATIC_RETENTION\`/\`SOURCE_GENERATION_VERIFY\`/\`SORT_UNIQUE_READS\`/
   \`LATE_DESCRIPTOR_CACHE\`/\`UNIQUE_SEMANTIC_CACHE\`/\`COMPONENT_DIAGNOSTICS\`）；\`STATIC_RETENTION_FRAMES\` 默认
   **240**（不是 A 的 3600）、\`STATIC_RETENTION_CAP\` 默认 64。Stage13 常驻字节是
   **CPU 私有**（\`positionBytes\`），**不计入** \`m_war3ShadowPersistentBytesUsed\`，也没有 A 的 64 MiB proof cap。
4. **stage13 对本 registry 的真实依赖点共 6 类**（§2）：共享 key 类型、共享 registry 擦除/性别名联动、
   共享 persistent 字节账与 cap、共享 expiry 队列、共享 Present 安全点整会话退役、以及
   隐式 tag 的 domain 语义。
5. **入口与顺序：先做审计批次 4（U5 domain 隔离），批次 6（U4 content-persistent）不得先行。**
   本树**没有**可用于 U5 的实机基线快照；唯一的离线等价物是
   \`AutoTest/test_persistent_expiry_queue_model.py\`（纯 Python 的 expiry 模型，非 registry domain 模型）。

---

## 1. 现状清册

### 1.1 B 树 registry 的实际形状（逐项读码核实）

| 项 | 位置 | 内容 |
| --- | --- | --- |
| 键类型 | \`d3d9_war3_scene.h:678-688\` \`War3ShadowGeometryKey\` | \`{uint64_t sourceHash; uint64_t layoutHash; War3ShadowReplayMode mode;}\` + \`operator==\`（三字段全比） |
| registry key 别名 | \`d3d9_device.h:2340\` | \`using War3ShadowGeometryRegistryKey = War3ShadowGeometryKey;\` |
| key hash | \`d3d9_device.h:2341-2349\` | \`sourceHash/layoutHash/mode\` 三者混合 |
| registry 条目 | \`d3d9_device.h:2350-2354\` \`War3ShadowGeometryRegistryEntry\` | \`{uint32_t geometryId; uint32_t instances; bool instanceable;}\` —— **无 domain / 无 owner / 无 proof 字段** |
| registry 容器 | \`d3d9_device.h:2503-2506\` \`War3ShadowGeometryRegistry\`；成员 \`d3d9_device.h:2541\` | \`unordered_map<key, RegistryEntry, KeyHash>\` |
| persistent 条目 | \`d3d9_device.h:2444-2449\` \`War3ShadowPersistentGeometryEntry\` | \`{key, War3ShadowPersistentGeometry geometry, uint64_t totalBytes, uint64_t lastSeenFrame}\` |
| persistent 容器 | \`d3d9_device.h:2511-2512\`；成员 \`d3d9_device.h:2542\` | \`unordered_map<uint32_t geometryId, Entry>\` |
| 字节账 | \`d3d9_device.h:2980\` \`m_war3ShadowPersistentBytesUsed\` | 加 :19855；减 :19626-19629 / :19644-19647 |
| 过期队列 | \`d3d9_device.h:2450-2459\` + 成员 :2551-2554 | 惰性令牌堆（每 geometry 一条权威 age token） |
| Stage13 常驻表 | \`d3d9_device.h:2519-2532\` \`War3Stage13RetainedCasterEntry\` / \`War3Stage13RetainedCasterMap\`；成员 :2543 | \`{draw, positionBytes(CPU), contentHash, sourceIdentityHash, worldMatrixHash, materialHash, layoutHash, lastSeenFrame}\`；**与 registry 共用同一 key 类型，但是独立容器** |
| S1 地形 early cache | \`d3d9_device.h:2923\` \`m_war3S1TerrainEarlyCache\`；反向索引 \`:2938\` \`m_war3S1TerrainEarlyKeysByPersistentGeometryId\`；\`:2939\` \`…LastGcFrame\`；\`:2942\` \`…LogicalReferencedBytes\` | 按 persistentGeometryId 反查并联动擦除 |

### 1.2 domain 今天是怎么"隐式"表达的

- **S1 地形**：\`device.cpp:48583\` —— \`if (s1TerrainPersistentPath) layoutHash = fnv1a_iter(layoutHash, uint32_t(0x53310001u)); // S1 tag\`。
- **Stage13**：\`device.cpp:47958\` —— \`constexpr uint32_t kStage13ReferencedContentTag = 0x53314301u;\`，
  经 \`buildStage13RetentionKey(identityTag, identityHash)\`（:47583-47616）写进 sourceHash 与 layoutHash。
- **mode**：\`key.mode\` 取 \`War3ShadowReplayMode::FixedWorld / PaletteSkinnedFF / SnapshotFallback / Unsupported\`
  （如 :20740-20741、:45939-45940、:47614），是**回放模式**，不是 domain。
- **结论**：domain 语义今天只靠"两个魔法常数 + 不同的 hash 输入"区分。
  两个不同来源若把同一 hash 组合算成同一 \`(sourceHash, layoutHash, mode)\`，
  registry 会**直接命中对方的 geometry** —— 这正是审计文档 U5 要消除的风险
  （原文：*"U5 domain 隔离必须先于 U4（否则 S1 会把 Stage13Exact 当 generic 命中）"*）。

### 1.3 与 A 侧叙述的差异（重要，避免把 A 的 checkpoint 当 B 的现状）

\`AGENTS.md\` 中 2026-08-26/08-30 那段（"Stage13 content-persistent geometry：首次 miss 用精确
referenced-vertex 内容快照创建 **registry-owned** 非索引常驻几何… registry publish/GC/reset 又增加
domain 隔离、三阶段异常回滚、owner-check 与 64 MiB CPU proof 上限…"）描述的是**另一棵树的
checkpoint**（该段自身给出候选 DLL 34,362,820 B 的身份）。**B 树现状**：

| A 侧描述的能力 | B 树状态（本轮核实） |
| --- | --- |
| registry-owned content-persistent geometry | **无**（\`ContentPersistent/contentPersistent\` 搜索 = 0；B 只有 CPU \`positionBytes\` 常驻 + 当帧 mapped freeze 上传） |
| registry domain 隔离（Generic vs Stage13Exact） | **无**（§1.3 全部标识符 = 0） |
| 三阶段异常回滚 | **未核实**（本轮未在这个 registry 里找到三阶段回滚标识；不排除以别的方式实现） |
| owner-check | **无**（\`OwnerCheck/owner-check\` = 0） |
| 64 MiB CPU proof 上限 | **无**（\`kWar3Stage13…ProofCap\` = 0；仅 Arena/TLSF 的 64 MiB **页大小**常量 \`war3_shadow_arena.cpp:21-22\`、\`war3_tlsf_pool.cpp:36\` 同名不同义） |
| 常驻 idle 240 → 3600 | **未采用**（B 默认 240，见 §1.4；与前置条款 §1 的建议一致） |

### 1.4 B 树 Stage13 门与默认值（逐条读码：\`device.cpp:2146-2244\`）

| 门 | env | 默认 |
| --- | --- | --- |
| \`War3Stage13StaticRetentionRuntime\` :2146 | \`DXVK_WAR3_STAGE13_STATIC_RETENTION\` | **0u**（注释明写：必须有精确 write-time content generation 才可 opt-in） |
| \`War3Stage13SourceGenerationVerifyRuntime\` :2159 | \`…_SOURCE_GENERATION_VERIFY\` | **0u** |
| \`War3Stage13SortUniqueReadsRuntime\` :2168 | \`…_SORT_UNIQUE_READS\` | **0u**（已判退的局部性实验） |
| \`War3Stage13LateDescriptorCacheRuntime\` :2178 | \`…_LATE_DESCRIPTOR_CACHE\` | **0u**（不安全诊断） |
| \`War3Stage13UniqueSemanticCacheRuntime\` :2187 | \`…_UNIQUE_SEMANTIC_CACHE\` | **0u**（已判退：真实 content mismatch） |
| \`War3Stage13ComponentDiagnosticsRuntime\` :2216 | \`…_COMPONENT_DIAGNOSTICS\` | **0u** |
| \`War3Stage13LateFullIndexFingerprintRuntime\` :2206 | \`…_LATE_FULL_INDEX_FINGERPRINT\` | 1u |
| \`War3Stage13StaticRetentionFramesRuntime\` :2225 | \`…_STATIC_RETENTION_FRAMES\` | **240**（clamp 1..1200） |
| \`War3Stage13StaticRetentionCapRuntime\` :2235 | \`…_STATIC_RETENTION_CAP\` | **64**（clamp 1..2048） |
| \`War3Stage13LateDescriptorSampleCountRuntime\` :2197 | \`…_LATE_SAMPLE_COUNT\` | 32（clamp 4..32） |

既有静态门禁 \`AutoTest/test_stage13_retention_lazy_hash_static.py\`（3,594 B /
\`0B308DFDEEE65884CF6ECE81E83A0B5BE81B81EF4EDA224443C7094D3AA2AE85\`）**已经**钉死
上述四个 \`0u\` 默认与"world/material/content hash 只在 base-eligible 分支内计算"——
它是本次迁移**必须保持绿色**的既有护栏。

---

## 2. Stage13 对 Registry（及 domain 语义）的依赖点

逐项列出"Stage13 的代码路径碰到 registry/domain 的地方"。**这些就是 U5 一旦实施必须同步处理的点。**

| # | 依赖点 | 位置 | 为什么与 domain 有关 |
| --- | --- | --- | --- |
| D1 | **共享 key 类型** | \`War3Stage13RetainedCasterMap\`（\`device.h:2529-2532\`）用 \`War3ShadowGeometryRegistryKey\` | Stage13 常驻表与 registry 用同一 \`{sourceHash, layoutHash, mode}\`；domain 若只加在 registry 条目而不加在 key，Stage13 表仍可能与非 Stage13 键碰撞 |
| D2 | **共享 lookup/create** | \`War3TryFindShadowPersistentGeometry\` :19672-19696、\`War3CreateShadowPersistentGeometryAfterMiss\` :19712-19866、\`War3FindOrCreateShadowPersistentGeometry\` :19698-19710 | 这三个函数是**唯一**的 registry 读写口；domain 隔离必须落在这里（或落在一个新的 domain-scoped 包装上），否则调用方各自绕过 |
| D3 | **S1 别名联动擦除** | :19691-19692（lookup 失败时）与 :19624、:19642（GC 时）调 \`War3EraseS1TerrainEarlyAliasesForPersistentGeometry\`（定义 :19437） | S1 early cache 按 persistentGeometryId 反查 registry；domain 隔离若只改 registry 而不同步这里的反向索引，会出现"geometry 已按 domain 拒绝但 S1 别名仍指向它" |
| D4 | **共享 persistent 字节账与 cap** | 加 :19855；cap 判定 :19749-19756；GC 减账 :19626-19647；报告 :22964/:34108/:46164 | 若 Stage13Exact 也进同一池，它要么吃 GPU 池上限（错的：它今天只占 CPU），要么必须建立独立账户——这正是前置条款 §3 第 1/3/5 条要求的分账 |
| D5 | **共享 expiry 队列** | 入队 :19856-19857；惰性 GC :19596-19634；紧急预算回收 :19636-19669 | domain 隔离后，过期策略可能按 domain 不同（GPU 常驻按 cap/age，CPU 常驻按 cap/帧数）——今天两者混在一个堆里 |
| D6 | **共享 Present 安全点整会话退役** | \`War3ResetShadowSessionState\` :24187-24300：registry/persistent/stage13 retained 一起 \`std::move\` 进 \`War3RetiredShadowSession\`（:24201-24212）、一起计数（Stage13 CPU 字节单独进 \`retired.cpuOwnedBytes\`，:24251-24256）、一起清空（:24279-24290） | domain 隔离若只做 lookup 不做 publish/GC/reset，会留下"按 domain 该被清掉但被一起 move 走"的条目；审计的 U5 名字里明确含 **publish/GC/reset** |
| D7 | **Stage13 自己的两处回收路径（不受 registry 管）** | 年龄回收 :17886-17915（无可用相机即 \`clear()\` :17895）；cap 回收 :50339-50349（\`min_element\` 最旧淘汰）；插入 :50350-50362 | 这两条今天**完全绕过** registry 的 cap/expiry/bytes 账 ⇒ 若将来把 Stage13Exact 纳入 registry，必须先把它们接进同一 domain 的回收条款，否则出现两个真相 |
| D8 | **隐式 tag** | S1 \`0x53310001u\`（:48583）、Stage13 \`0x53314301u\`（:47958） | 这两个常数是当前 domain 的**唯一**表达；U5 应把它们提升为显式 domain（或至少由门禁钉死其唯一性） |

**没有被 Stage13 依赖、但属同一 registry 的调用方**（domain 隔离时不得误伤）：
\`device.cpp:20588\`（append 主路径 geometry key）、\`45898\`（另一条 build 路径）、\`47585/47959/48585\`（Stage13 与 S1）。

---

## 3. 迁移入口与顺序（建议；**未实施**）

### 3.1 入口（最小改动面）

1. **数据面**：给 registry 条目（或 key）增加显式 domain 标识。两个可选形态：
   - (a) 在 \`War3ShadowGeometryRegistryEntry\`（\`device.h:2350\`）加 \`domain\` 字段，lookup 时比对；
   - (b) 把 domain 混进 \`War3ShadowGeometryRegistryKey\`（\`scene.h:678\`）成为第 4 个字段，
     使不同 domain 天然不共享槽位。
   **(b) 更 fail-closed**（碰撞不可能发生），但会改 key 的 \`operator==\`/hash，影响 §3.1-D1 的
   Stage13 表与所有 key 构造点；**(a) 改动面小但需要每个读取点都记得比对**。
   本勘察**不建议**在此二选一——需上级裁定（见 §5-3）。
2. **控制面**：\`War3TryFindShadowPersistentGeometry\`（:19672）增加 domain 入参与比对；
   \`War3CreateShadowPersistentGeometryAfterMiss\`（:19712）写入 domain 并纳入分账/分队列。
3. **生命周期面**：\`War3GcShadowPersistentGeometry\`（:19585）、\`War3ResetShadowSessionState\`（:24187）
   按 domain 分别计量与退役（至少先分账，再考虑分策略）。
4. **S1 联动面**：\`War3EraseS1TerrainEarlyAliasesForPersistentGeometry\`（:19437）与
   \`War3StoreS1TerrainEarlyCacheEntry\`（:19337）的反向索引必须与 domain 一致。
5. **门禁面**：见 §4。

### 3.2 顺序（沿用审计文档的批次编号，落到 B 的具体锚点）

| 批次 | 内容 | B 树落点 | 前置依赖 |
| --- | --- | --- | --- |
| 0 | 观测先行（计数器/query/字段，不改默认、不安钩） | 现有 \`m_war3ShadowPersistentDiagnosticsFrame\`（\`device.h:2380-2443\`）已含大量 GC/cap 计数；缺的是 **domain 维度的计数** | 无 |
| 1 | Producer 默认治理（U1） | 与本 registry 无关 | 需上级裁定默认方向（审计已注明） |
| 2 | Path-blocker 提交闸（U2） | 与本 registry 无关 | — |
| 3 | Legacy EntryGate 收窄 + Stage13 blocker 预检 | 与本 registry 无关（但改的是同一 append 函数） | 不与 U1 捆绑 |
| **4** | **Registry domain 隔离（U5）** | §1.1 全部结构 + D2/D3/D6 | **无**（但需先定 (a)/(b) 形态） |
| 5 | Static alias migration（U3） | \`m_war3DrawTimeStaticAliasIndex\` 在 B **不存在**（审计已列"B 完全没有"） | 可与 4 并行 |
| **6** | **Stage13 content-persistent（U4）** | 需新建 registry-owned 几何 + 64 MiB CPU proof 账 | **批次 4 是硬前置**（审计 + 前置条款 §4 双重声明） |
| 7 | Type5 地址表（可选，默认不安钩） | — | — |

**为什么 4 必须先于 6（本树可复核的理由）**：今天 Stage13 常驻是 CPU 私有 + opt-in；
一旦批次 6 让 Stage13 创建 **registry-owned GPU 常驻几何**，D1/D2/D4/D5/D6 会立刻生效——
在 domain 未隔离时，S1（\`0x53310001u\`）与 Stage13Exact（\`0x53314301u\`）共用一个
无 domain 的 registry 与同一字节账；审计文档已经点名这个后果（"S1 会把 Stage13Exact 当 generic 命中"）。

---

## 4. "完成"的可验证判据（建议；**未实施**）

一次可声称"Registry domain 隔离完成"必须**同时**满足：

**F1 结构判据（静态，可离线）**
1. registry 条目/key 存在**显式** domain 标识（符号名可查，不再是魔法数）；
2. 全 \`src\` 中 \`0x53310001u\` / \`0x53314301u\` 的 domain 语义被门禁**钉死**
   （要么保留且断言唯一，要么被显式 domain 取代后断言常数消失）；
3. registry 的 **每一个** 读取点（§2 "没有被 Stage13 依赖但属同一 registry 的调用方" 全部列出）
   都携带 domain；用静态门禁断言调用点数与位置不变。

**F2 生命周期判据（静态 + 离线模型）**
4. GC（:19585）、lookup 失败擦除（:19691-19692）、S1 别名擦除（:19437 的调用点）、
   Present 退役（:24187）四条路径**都按 domain 分账**（计数可分别读出）；
5. **分账守恒**：\`m_war3ShadowPersistentBytesUsed\` 只统计 GPU 常驻；
   Stage13 CPU 常驻另立账户（今天只出现在 \`retired.cpuOwnedBytes\`，:24251-24256）；
   两者各自"不单调增长、可观察到实际退役"（前置条款 §3 第 3/5 条）；
6. 离线模型：**今天只有一个** \`AutoTest/test_persistent_expiry_queue_model.py\`（纯 Python，
   lazy 堆 vs 全扫描对照，164 行）。domain 隔离需要**扩展或新增**同型离线模型
   （域内/域间命中、过期、擦除、退役），否则只能靠实机。

**F3 行为判据（同 DLL / 同场景，非确定性）**
7. 同场景对照：\`budgetExceeded\` / \`framesBudgetExceeded\` **不上升**（前置条款 §3 第 4 条）；
8. cap 准入拒绝（\`capacityRejectAllCallers\` :19754、\`capacityFastReject\` :48663）**不激增**；
9. \`m_war3ShadowPersistentBytesUsed\` 峰值 ≤ cap 且稳态**不单调增长**，且能观察到退役
   （\`expiryAgeEvictions\` / \`evictedThisFrame\` 有非零）。
   **前置缺口（本轮核实）**：本树**没有**已登记的 pre-U5 基线快照 ⇒ F3 目前**无法执行**，
   必须先做一次基线采集并登记。

**F4 反例判据**
10. 必须能构造一个**跨 domain 碰撞反例**并证明它在隔离前会命中、隔离后不会
    （否则"隔离"没有可证伪的证据）；
11. 若任一指标劣化 ⇒ 按前置条款 §3 第 7 条如实记"未验证/劣化"，**不得**宣称改善。

**明确不在判据内**：Stage13 是否"改善累积超预算"——前置条款已裁定**不得**在取得占用/回收数据前
把两者绑定宣传。

---

## 5. 未实施声明与不确定项

### 5.1 明确未实施（**不得引用为已完成**）

- 本文**没有**实施任何 domain 隔离、没有新增任何 domain 字段、没有改任何 registry 结构。
- 本文**没有**构建、**没有**跑 meson、**没有**跑全量静态、**没有**启动游戏、**没有**部署、
  **没有** git 写。本轮只做了：只读 \`read\`/\`grep\`/\`glob\`，
  以及**一次只读**执行 \`py AutoTest/test_device_semantic_responsibility_budget_static.py\`
  （EXIT 0；冻结 149 / 159→115 / 已迁出 38 / 站点 577），用于与 M2-5 文档共享观测基线。
- 本文的批次顺序是**建议**，不是授权；批次 1 的默认值方向仍待上级裁定（审计文档已注明）。

### 5.2 未验证项

1. **三阶段异常回滚**：A 侧 checkpoint 声称有；我在 B 的这个 registry 路径里**没有找到**对应标识，
   但也**没有**逐行走完 \`War3CreateShadowPersistentGeometryAfterMiss\` 的全部失败分支去排除
   "以别的方式实现的回滚"。⇒ 记为**不确定**。
2. **U1/U2/U3 的 B 现状**：本文只核实了"\`m_war3DrawTimeStaticAliasIndex\` 在 B 不存在"
   （grep = 0）。其余 U1/U2 的 B 现状**未独立复核**，只引用审计文档结论。
3. **domain 形态 (a)/(b) 的取舍未定**；两种形态对既有静态门禁（尤其
   \`test_stage13_retention_lazy_hash_static.py\` 与 \`test_shadow_*cache*\`）的改锚成本**未评估**。
4. **\`m_war3S1TerrainEarlyKeysByPersistentGeometryId\` 的完整读写面未逐点核实**：
   本轮核实的调用点是 :19337（写入）、:19404-19434（EraseCacheEntry）、:19437-19470（EraseAliases）、
   :44500（命中）与退役 :24212。是否还有别的写入点**未穷举**。
5. **F3 的数值判据（cap 拒绝"不激增"的阈值）未定义**——必须先有基线才能定，不得凭空给数字。
6. **本文件之外**：T2/T3（wire/准入语义）按交接文档**不实施**；本文不涉及。
