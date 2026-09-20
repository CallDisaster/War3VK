# 2026-09-18 — M2-5 设计与调色板侧余项清册（**设计 + 只读勘察；未实现**）

> 状态：**设计文档 + 只读清册。零代码改动、零构建、零 git 写、零部署、未启动游戏、未触碰 \`E:\Work\`。**
> 本文的每一处行号/符号/计数都在**本文观测的那一个 device.cpp 版本**上逐条读过；
> 行号会漂移，引用请以**符号名 + 唯一子串**为锚（沿用交接文档 §5-8 纪律）。
> 工单：\`docs/plan/2026-09-18-overnight-execution-plan.md\` §2「M2-4/M2-5（富余）」、
> \`docs/plan/2026-09-18-deepseek-overnight-handoff.md\` §4 阶段 4 第 1 条、§8 关键路径。
> 上位设计：\`docs/plan/2026-09-16-device-semantic-responsibility-migration.md\` §3 M2、§4-②。
> 同构先例：\`docs/plan/2026-09-18-m2-1-migration-equivalence-record.md\`、
> \`…-m2-2-…\`、\`…-m2-3-…\`。
>
> **本文不是"M2-5 已实现"，也不是"M2-4 已完成"。** 第 1 节是清册（哪里还有什么没迁），
> 第 2 节是**门禁设计**（如果要迁，用什么口径证明没劣化），第 3/4 节是依赖与不确定项。

---

## 0. 观测基线（本文所有行号的唯一依据）

本节数字由本轮**只读**复核（读文件 + 跑只读静态门禁），不是转述。

| 项 | 值 |
| --- | --- |
| \`src/d3d9/d3d9_device.cpp\` | **2,273,048 B / SHA-256 \`D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE\` / 50,462 行** |
| 与 M2-3 记录的交叉验证 | 该 SHA **逐字节等于** M2-3 记录 §2 的"迁移后 device.cpp" ⇒ 本树自 M2-3 完成以来未再改动；M2-4/M2-5 **未开始** |
| \`src/d3d9/d3d9_war3_scene.h\` | 121,062 B / \`8B5B33DC47062B2D01DA585695EBA3B8DC523F5FA54FD7567146F7F31BC5DDEC\` |
| \`src/d3d9/d3d9_device.h\` | 134,216 B / \`E6F97012AFCC3003C572DEC5F63F0EE90264810BE9B92292D602D3DEDE9F0AE3\` |
| 模块 \`war3/semantic/war3_live_palette_selection.h\` | 9,517 B / \`EC3CFB5CB25EF85A3D8DC35DAE43D4E9126BD8EE1153C63CF0566D017A49EB82\`（与 M2-3 记录一致） |
| 模块 \`war3/semantic/war3_live_palette_selection.cpp\` | 41,008 B / \`521BB85D32FEF05FC20114B40B8D484AD9790D58DE8626E3DF09DDF99B047359\`（与 M2-3 记录一致） |
| 预算门禁本轮实测（只读执行） | \`EXIT=0\`：**冻结符号 149；行首站点 迁移前 159 → 迁出后上限 115 → 当前 115；已迁出 38 个符号；device.cpp 行首定义站点总数 577** |
| \`AutoTest/test_device_semantic_responsibility_budget_static.py\` | 19,461 B / \`266967239132D9CE2FF9E6D0F55A21DF5339D4B6CA8395EC953DE61C297F2BF1\` |
| \`AutoTest/test_active_path_palette_instrumentation_export_static.py\` | 9,213 B / \`06C2C429959C3014AD2C787A50A0EE8440F4ECDD6F02B2F2C206413B97392836\` |
| \`AutoTest/test_stage13_retention_lazy_hash_static.py\` | 3,594 B / \`0B308DFDEEE65884CF6ECE81E83A0B5BE81B81EF4EDA224443C7094D3AA2AE85\` |

**判据口径（与预算门禁同一套，本文不另立）**：行首站点 = 源文件**第 0 列**开始、非注释行、
匹配 \`DEF_RE\`（\`^(?:inline |static |constexpr |virtual |explicit )*…名称(\`）的行；
它同时覆盖**定义与前置声明**。命名族 = 门禁的 \`SEMANTIC_FAMILY_RE\`
（\`^(?:War3|IsLosBlocker).*(Resolve|Select|Choose|Decide|Classify|Score|Should|TryBuild|TryFind|TryPopulate|Promote|CanPromote|IsEligible|Runtime|SelectionKey|Key|ObjectKind|PathBlocker|LosBlocker)\`），
它是**判定 3（回流）**的唯一捕手。**命名族不匹配的符号 = 判定 3 盲区**（M2-3 已实测该盲区，
见 M2-3 记录 §2 末段）。

---

## 1. 余项清册（可执行）

清册口径：`device.cpp` 里仍与"调色板来源选择"职责相关、且**尚未迁入**
\`dxvk::war3::semantic\` 模块的符号 / 职责块。每条给出：**符号名 + 行号 + 类型 +
归类 + 我实际核实到的依据**。类型取值：定义 / 调用点编排 / taxonomy 发射 / 计数器 /
资源原语 / 死代码。

**清册条目合计 28 条**：M2-4 **10 条**、M2-5 **11 条**、明确"不属本族"**7 条**。
其中真正属"调色板选择职责"的是 A+B 共 **21 条**。

### 1.1 表 A —— M2-4（可继续机械迁出，M2-1/2/3 同范式）

判定"可机械迁出"的三个条件，逐条都满足才列入本表：
(i) 无 \`D3D9DeviceEx\` 成员依赖（不碰 \`m_war3*\` / \`m_state\`）；
(ii) 判定结果与判定顺序可逐字节保留；
(iii) 有确定性的调用点集合。

| # | 符号 | 行号 | 类型 | 依据（本轮实测） | 迁出时要一起处理的 |
| --- | --- | --- | --- | --- | --- |
| A1 | \`War3SemanticPaletteInPlaceAppendRuntime\` | 定义 :1297；调用点 :18636 | 定义（env getter）+ 编排 | \`inline bool\`，函数内 static 读 \`DXVK_WAR3_SEMANTIC_PALETTE_IN_PLACE_APPEND\`（默认 \`1u\`）；**门禁内已冻结 (1,1)**；命名族=**匹配** | 与 M2-1 的 4 个 env getter 完全同型；迁出后 FROZEN 记 (1,0) 并进 \`FROZEN_M2_4\` |
| A2 | \`War3SemanticDrawTimePoseRuntime\` | 定义 :2074；调用点 :19088 | 定义（env getter） | env getter，**已冻结 (1,1)**；命名族=**匹配**。调用点落在 \`War3TryPublishSemanticDrawTimePose\`（M4 领地） | getter 本身是纯配置读取，可迁；**调用点留原位**（M4 未动） |
| A3 | \`War3SemanticPaletteStorageReadable\` | 定义 :7184；调用点 :7264 | 定义（纯判定） | 只做 \`empty / >256 / IsReadableRange\` 三判断，唯二出现即本定义与其在 A4 内的调用；**命名族=不匹配（判定 3 盲）** | 必须**显式登记 FROZEN**（否则回流漏检）；依赖 \`dxvk::war3::IsReadableRange\` |
| A4 | \`War3SemanticPaletteLooksModelLocal\`（**两个重载**） | 定义 :7204、:7260 | 定义（纯判定） | 调用点 :7266（重载内互调）、:7277（A9 内）、:18703/:18709（\`War3GetOrCreateSemanticShadowPalette\` 内，仅诊断）；**命名族=不匹配** | 依赖 \`War3SemanticBoundsRadiusForObjectKind\`（见 A8，**未迁**）与 A6/A7；FROZEN 需登记 **baseline=2**（两个重载各占 1 个行首站点） |
| A5 | \`War3SemanticHashMatrix4\` | 定义 :7287；调用点 :18713 | 定义（纯计算） | 单矩阵 4×4 FNV-1a；是已迁出的 \`War3SemanticHashMatrixPalette\` 的**姊妹**；**命名族=不匹配** | FROZEN 显式登记；与已迁的 \`War3SemanticHashMatrixPalette\` 同模块自然归位 |
| A6 | \`War3SemanticTranslationFinite\` | 定义 :7179；调用 :7214、:7234 | 定义（纯判定，supporting） | 全仓只有这 3 处；**只被 A4 使用** | 随 A4 整块搬迁（保持逐字节相邻） |
| A7 | \`War3SemanticTranslationDistanceSq\` | 定义 :7172；调用 :7243 | 定义（纯计算，supporting） | 全仓只有这 2 处；**只被 A4 使用** | 随 A4 整块搬迁 |
| A8 | \`War3SemanticBoundsRadiusForObjectKind\` | 定义 :7139 | 定义（纯查表） | switch(ObjectKind)→半径；**已冻结 (1,1)**；命名族=**匹配**（含 ObjectKind）；**全仓 1 定义 + 7 个调用点（本轮全部定位）**：:7223（A4 内）、:22327、:22339、:22432、:23893、:27734、:45727 | **不建议单独迁**：7 个调用点里只有 1 个（:7223）属调色板族，其余 6 个属 bounds。若 A4 要迁，应改为"模块声明 + 定义留 device"（M1 前置声明先例）或把 A8 单列 M2-4 子项**
| A9 | \`War3SemanticBuildWorldPaletteIfNeeded\` | 定义 :7271 | **死代码** | \`[[maybe_unused]]\`，**全 \`src\` 只有定义这一处**（0 调用） | 不构成迁移项；建议**单独裁定**（删除 vs 迁移），不要夹带进 M2-4 |
| A10 | \`War3SemanticVectorStorageReadable\`（template） | 定义 :7194 | **未实例化死代码** | 全 \`src\` 只有定义这一处（0 实例化） | 同上，单独裁定 |

**M2-4 的硬提醒（本轮实测）**：A3/A4/A5 三个符号**都不匹配 \`SEMANTIC_FAMILY_RE\`**（已用门禁同一 regex 逐个跑过），
因此**判定 3 对它们是盲的**；不登记 \`FROZEN_M2_4\` 就把它们内联回 device.cpp 不会被任何一条判定咬住
——这与 M2-3 的 \`War3Note*Motion\` 是同一类漏检（M2-3 记录 §2 已实测点名）。

### 1.2 表 B —— M2-5（调用点编排 / taxonomy 发射；**只动编排，不动判定结果**）

| # | 职责块 / 符号 | 行号（\`War3TryAppendSemanticShadowPacket\` 内，定义 :19917） | 类型 | 依据（本轮实测） |
| --- | --- | --- | --- | --- |
| B1 | live palette refresh 编排块 | :20838–:20904（\`if (skinned && War3SemanticLivePaletteRefreshRuntime()) {\` → 块尾） | 调用点编排 | 含三分支：\`drawTimeCapturedPaletteReady\` / \`shouldAvoidLegacyLivePaletteFallback\` / 兜底构建；**唯一** \`War3TryBuildLiveRuntimeGroupPalette\` 提交点 :20868；块尾命中/未命中计数 :20884/:20902；\`War3NoteLivePaletteMotion\` 调用 :20895 |
| B2 | 提交来源状态装配 | :20831–:20837（声明）+ :20905–:20912（生效值与上界） | 调用点编排 | \`paletteSourceThisSubmit\` / \`paletteSlotIndexThisSubmit\` / \`paletteMin/MaxFrameTagThisSubmit\`；\`effectiveRuntimeGroupPalette\` / \`effectiveMaxVertexGroupSlot\` 由 refresh 结果决定 |
| B3 | **taxonomy 发射块** | :21202–:21580（\`if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {\` → 块尾） | taxonomy 发射 | 本轮以花括号配对实测块范围；块内触及 **34 个 \`stats.\` 字段**（见 §2 清单）；内含 3 张 \`thread_local\` 探针表：\`PaletteProbeEntry\`（:21281，8192 项哈希槽）、\`LeaseKeyAttributionEntry\`（:21463，8192 项）、\`StrictProbeEntry\`（:21507）；**探针表状态跨帧存活**，是 churn 计数的状态源 |
| B4 | 每帧 skinned palette 聚合 | :22678–:22724 | taxonomy 发射 | \`CombinedHash\` / \`FirstSubmittedHash\` / \`DistinctSampleCount\` / \`ConsecutiveSameHashCountMax\` / \`ZeroHashCount\` 的滚动聚合（\`bit::fnv1a_iter\`） |
| B5 | direct-caster palette churn | :22742–:22784（计数器 :22774） | taxonomy 发射 + 计数器 | \`semanticSceneDirectPaletteHashChurnCount\`（定义 \`d3d9_war3_scene.h:1628\`）经局部 \`updateChurn\` lambda 累加；与 palette0 delta 分桶同块 |
| B6 | \`War3TryBuildLiveRuntimeGroupPalette\` 调用点 | :20473、:20868、:26651、:26731 | 调用点编排 | M2-2 已迁定义；调用点文本一个字符未改（M2-2 记录 §1） |
| B7 | \`War3NoteLivePaletteMotion\` 调用点 | :20895、:26706、:26761 | 调用点编排 | M2-3 已迁定义；调用点未改（M2-3 记录 §1） |
| B8 | \`War3NoteDrawTimePoseMotion\` 调用点 | :19207 | 调用点编排 | 同上；落在 \`War3TryPublishSemanticDrawTimePose\`（M4 领地）内 |
| B9 | \`War3NoteSubmittedPaletteMotion\` 调用点 | :21191 | 调用点编排 | 同上 |
| B10 | \`War3SemanticHashMatrixPalette\` 调用点 | :18772、:20345、:20496、:21188 | 调用点编排 | M2-1 已迁定义（另 :6891 是 module 内/旧注释面） |
| B11 | \`War3GetOrCreateSemanticShadowPalette\` | 定义 :18669–:18788；调用点 :21586 | 定义（**混合**：compose-policy 判定 + device frame-local 缓存 + 资源池查找） | 内部做 A4/A5 判定（:18703/:18709/:18713）、\`m_war3SemanticPaletteCache\` 帧内缓存（:18736/:18785）、\`War3GetOrCreateShadowMatrixPaletteFromData\`（C2）资源创建。**归类待裁定**：判定侧属 M2，资源侧属 M3 —— 见 §4-1 |

### 1.3 表 C —— 明确**不属**本族（M3/M4 或其它职责；登记在此以免被误当余项）

| # | 符号 / 职责 | 行号 | 分类 | 理由（本轮实测） |
| --- | --- | --- | --- | --- |
| C1 | \`War3GetOrCreateShadowMatrixPalette\` | 定义 :18529；调用点 :45067 | 资源原语 | D3D9 fixed-function WORLDMATRIX 0..255 的 **GPU palette 池**（hash 去重 + memcmp 确认 + \`m_war3Scene.shadowPalettes\`），是资源生命周期不是"哪个调色板权威" |
| C2 | \`War3GetOrCreateShadowMatrixPaletteFromData\` | 定义 :18590；调用点 :18779、:45147、:45836 | 资源原语 | 同上；含 \`War3SemanticPaletteInPlaceAppendRuntime\`（A1）门与 4 个 \`PaletteStorage*\` 阶段计时 |
| C3 | \`War3TryPublishSemanticDrawTimePose\` | 定义 :19086–:19211；调用点 :41393 | 定义（draw-time producer） | 设计 §3 明确把"CurrentDraw 绑定与 draw-time producer 填充"列为 **M4**（风险最高、押后） |
| C4 | \`budgetExceeded\` 写入 | **9 处**：:18880、:33992、:49516、:49528、:49553、:49970、:50050、:50063、:50135 | 计数器（资源账） | 全部在 \`War3AllocFreezeBuffer\` / Arena / mapped allocator 预算失败路径；属"**Arena/budget**"主题（设计 §1），不属调色板族。**但它是 §2 的护栏计数器**（设计 §4-② 点名） |
| C5 | \`QueryDevicePaletteSlotCache{ServedAfterConfirm,RejectedStale}Count\` | :1161、:1165 | 定义（war3_diag 导出边界） | M2-2 记录 §1 **明确决定**访问器留 device.cpp 原位（计数器本体已随迁）；**不是余项** |
| C6 | \`SetPaletteEntries\` / \`GetPaletteEntries\` / \`SetCurrentTexturePalette\` / \`GetCurrentTexturePalette\` | :16909 / :16940 / :16962 / :16975 | D3D9 API 面 | 纹理调色板（PALETTEENTRY）D3D9 接口，与 shadow 语义调色板无关 |
| C7 | \`m_war3ShadowGeometryRegistry\` / \`m_war3ShadowPersistentGeometries\` 及其 GC/reset | :19672–:19860 等（详见 T7 文档） | 资源注册表 | 属 persistent geometry registry；domain 隔离清册见 \`docs/plan/2026-09-18-t7-registry-stage13-survey.md\` |

### 1.4 清册的"执行顺序建议"（不是本文的授权）

1. **M2-4a（低风险，M2-1 同型）**：A1、A2 —— 两个 env getter，已冻结，迁出即改 FROZEN 为 (1,0)。
2. **M2-4b**：A3、A5、A6、A7 —— 纯判定/纯计算，需**新增 \`FROZEN_M2_4\`**（判定 3 盲区）。
3. **M2-4c（需先定依赖）**：A4 + A8 的依赖处理（§4-2）。
4. **M2-5**：B1–B11，**必须在 M2-4 之后**（§3.3）。
5. A9/A10：单独裁定（死代码，不属于迁移）。

---

## 2. M2-5 设计：taxonomy 计数对照门禁

### 2.1 迁移前后必须不劣化的计数器清单（**共 44 条**）

计数规则：**去重后的具名计数器/字段**；同一名字在定义处计 1 条，不按累加点重复计。
`—` 表示本轮未在该列核实到内容（不得脑补）。

**分组 1：\`semanticSceneSubmittedSkinnedPaletteSource*\` 来源桶（8 条）** — 定义 \`d3d9_war3_scene.h\`；
累加 \`device.cpp\`（B3 块内）：

| 计数器 | 定义 | 累加点 | 当前门禁覆盖 | 对照门禁应如何钉死 |
| --- | --- | --- | --- | --- |
| \`…SourceNoneCount\` | scene.h:1451 | dev:21207 | 仅"名字在 scene.h 出现 8 次"（export 门禁 :168） | 宿主差分逐调用对照 + 静态断言 device.cpp 中该名字的 \`++\` 计数为 0/模块为 1 |
| \`…SourceDrawTimeCapturedCount\` | scene.h:1452 | dev:21210 | 同上 | 同上；另需断言 provenance 子分支仍嵌在 \`case DrawTimeCaptured\` 内 |
| \`…SourceSubmitTimeGlobalSlotCount\` | scene.h:1453 | dev:21239 | 同上 | 同上 |
| \`…SourceSubmitTimeBlendedCacheCount\` | scene.h:1454 | dev:21242 | 同上 | 同上 |
| \`…SourceSubmitTimePublishedRegistryCount\` | scene.h:1455 | dev:21245 | 同上 | 同上 |
| \`…SourceSubmitTimeCModelFallbackCount\` | scene.h:1456 | dev:21248 | 同上 | 同上 |
| \`…SourceOwnedPartSnapshotCount\` | scene.h:1457 | dev:21251 | \`test_skin_palette_contract_static.py:86-93\`：只断言该名字在 scene.h/bridge/hub/control_plane 存在 + perf JSON 写手 == 2；**无数值/自增点断言** | 同上 + 该名字必须仍同时落 \`ProvenanceProducerPartPacketCount\`（:21254 的成对累加不得拆散） |
| \`…SourceChurnCount\` | scene.h:1483 | dev:21337 | 无（仅被 export 门禁的注释当作"先例"提及） | 同上；并断言 \`if (entry.lastSource != paletteSourceThisSubmit)\` 的条件与比较对象未变 |

**分组 2：\`…PaletteProvenance*\` 六桶（6 条）** — 定义 scene.h:1549–1554；
累加 dev:21216 / :21219 / :21222+21254 / :21225 / :21228 / :21231+21235：

\`TrustedBlendedWriter\`、\`RawGlobalArena\`、\`ProducerPartPacket\`（**两处**：:21222 与 :21254）、
\`RangeCopyPoseRebuild\`、\`CModelFallback\`、\`Unknown\`（**两处**：:21231 与 :21235）。
**多累加点必须逐点钉死**——只对总数会漏掉"DTC 桶与 Owned 桶互相吞并"这类等量错配。

**分组 3：稳定性 / churn / 窗口 / stale 归因（20 条）**

| 计数器 | 定义 | 累加点 |
| --- | --- | --- |
| \`…PaletteStablePartSampleCount\` | scene.h:1481 | dev:21332 |
| \`…PaletteHashChurnCount\` | scene.h:1482 | dev:21334 |
| \`…PaletteSlotIndexChurnCount\` | scene.h:1484 | dev:21344 |
| \`…PaletteCountChurnCount\` | scene.h:1493 | dev:21382 |
| \`…PaletteHashUniqueInWindowMax\` | scene.h:1485 | dev:21428（**max 语义**，非累加） |
| \`…PaletteSlotIndexUniqueInWindowMax\` | scene.h:1486 | dev:21434（**max 语义**） |
| \`…PaletteFirstMatrixSmallDeltaCount\` | scene.h:1490 | dev:21376 |
| \`…PaletteFirstMatrixMediumDeltaCount\` | scene.h:1491 | dev:21372 |
| \`…PaletteFirstMatrixLargeDeltaCount\` | scene.h:1492 | dev:21357 |
| \`…PaletteAfterStaleRestoreLargeDeltaCount\` | scene.h:1519 | dev:21364 |
| \`…PaletteLiveToLiveLargeDeltaCount\` | scene.h:1520 | dev:21367 |
| \`…PaletteStaleRestoreSubmittedCount\` | scene.h:1518 | dev:21454 |
| \`…PaletteLeaseKeyPayload11CMultiValueCount\` | scene.h:1497 | dev:21489 |
| \`…PaletteLeaseKeyPaletteCountMultiValueCount\` | scene.h:1498 | dev:21495 |
| \`…PaletteStrictSliceSampleCount\` | scene.h:1501 | dev:21539 |
| \`…PaletteStrictSliceHashChurnCount\` | scene.h:1502 | dev:21541 |
| \`…PaletteStrictSliceCountChurnCount\` | scene.h:1503 | dev:21563 |
| \`…PaletteStrictSliceFirstMatrixSmallDeltaCount\` | scene.h:1504 | dev:21557 |
| \`…PaletteStrictSliceFirstMatrixMediumDeltaCount\` | scene.h:1505 | dev:21554 |
| \`…PaletteStrictSliceFirstMatrixLargeDeltaCount\` | scene.h:1506 | dev:21551 |

**分组 4：每帧 skinned palette 聚合（7 条）** — 定义 scene.h:1472–1479；累加 dev:22678–22724：

\`CombinedHash\`(1472)、\`FirstSubmittedHash\`(1473)、\`DistinctSampleCount\`(1474)、
\`ConsecutiveSameHashCountMax\`(1475)、\`ZeroHashCount\`(1476)、以及两个**内部 scratch**
\`RunningLastHash\`(1478)、\`RunningSameHashRun\`(1479)。
\`CombinedHash\` 是**顺序敏感的滚动 FNV**（先 lo 后 hi，见 :22704-22708）：迁移不得改变累加顺序，
否则即使集合相同哈希也不同。

**分组 5：direct-caster palette churn（1 条）**：\`semanticSceneDirectPaletteHashChurnCount\`（scene.h:1628，累加 dev:22774）。

**分组 6：护栏计数器（2 条，不属调色板族但设计 §4-② 点名必须不劣化）**：

| 计数器 | 定义 | 累加/写入 | 当前门禁覆盖 | 对照门禁应如何钉死 |
| --- | --- | --- | --- | --- |
| \`budgetExceeded\`（当帧） | \`d3d9_war3_scene.h:1890\` | **9 处**（C4） | 无静态断言（\`test_shadow_arena_memory_budget_static.py\` 存在但本轮未核对其是否断言写入点集合） | **写入点集合与数量必须不变（9）**；M2-4/M2-5 不得新增/移动任何一个写入点；实机对照要求 \`framesBudgetExceeded\` 相对基线**不上升** |
| \`m_war3ShadowFallbackBudgetExceeded\` | \`d3d9_device.h\`（本轮未逐一核实行号） | 10 处（:18883 等，与上一条成对） | 无 | 同上；**成对关系不得拆散**（每个 \`budgetExceeded=1\` 旁必须有 \`=true\`） |

> **诚实标注**：上表"当前门禁覆盖"列只写了我**实际读过的**门禁。
> \`AutoTest\` 下还有 \`test_shadow_arena_memory_budget_static.py\`、
> \`test_shadow_capture_post_breakdown_static.py\` 等可能间接覆盖预算/分类桶，
> 本轮**未逐份读完**（见 §4-5）——设计门禁时必须先做这一步，不得假设"没有"。

### 2.2 读取路径（从累加点到可比对的值）

本轮逐跳核实：

1. 当帧字段：\`War3ShadowCaptureStats\`（\`d3d9_war3_scene.h\`），随
   \`m_war3Scene = War3FrameScene{}\` **每帧整体重建**（export 门禁 :179 断言该语句存在）。
2. Present 安全点发布：\`dxvk::war3::render::NoteShadowSceneStats(m_war3Scene.shadowStats)\`
   —— device.cpp **7 处**：:17999、:29775、:29785、:29827、:29927、:29933、:31044。
3. bridge 汇总：\`war3_shadow_runtime_bridge.cpp\` 以
   \`War3ShadowCaptureStats merged = stats; g_shadowSceneStats = merged;\` 合并
   （export 门禁 :181-182 断言），字段逐个拷贝（如 \`…SourceChurnCount\` 在 :6746-6748）。
4. hub 汇总：\`war3_diagnostics_hub.cpp\` \`summary.X = bridgeSummary.X\`（如 :2032-2033），
   并在 JSON 键值表里再导出一次（如 :3257-3259）。
5. control plane：\`war3_control_plane.cpp:2318-2323\` 的 \`{name, value}\` 表（\`get_runtime_status\` 通路）。
6. perf monitor：区间**累加**（\`agg.X += stats.X\`，:1895-1915）+ 两个 JSON 写手（:6919-6929、:8491-8500）
   + \`"shadowBudgetSummary"\` 段（:6070）；报告落 \`war3_perf_report.html\`。
7. 运行期快照文件：\`WarVK/Temp/runtime_status.json\`（\`war3_diagnostics_hub.h:1077\` 注释声明）。

**结论（对门禁设计最关键）**：这些桶从 device 到报告**全程是整数拷贝/累加，没有任何采样、滤波或取整**。
因此"同场景不劣化"的正确口径是**整数精确相等**，不是"近似接近"。

### 2.3 比较口径、容差与失败模式

**口径（按可信度从高到低，M2-5 应优先用前者）**

- **口径 1（首选，宿主确定性差分）**：把 B3 发射块抽成模块里的**纯函数**
  （输入：\`War3ShadowCaptureStats& stats\` + B1/B2 的输入量；输出：无，只写 stats），
  在宿主测试里用**同一调用序列**分别驱动"legacy 参考实现"（\`.inc\`，M2-1/2/3 同范式）与模块实现，
  每步对照全部 34 个字段，段末对整个 \`War3ShadowCaptureStats\` 做 \`memcmp\`。
  **容差 = 0**（逐字段整数相等）。
  可行性依据：M2-3 已对同结构体做过 \`static_assert(std::is_trivially_copyable<…>)\` + \`memcmp\`（M2-3 记录 §4.1），
  证明该结构可整体按字节对照。
- **口径 2（同 DLL 同场景实机/隔离桌面）**：同一 DLL、同一地图、同一相机轨迹、同一 env 矩阵，
  迁移前跑一次基线、迁移后跑一次；对照 \`runtime_status.json\` / control plane / perf 报告的**区间累计值**。
  这里必须用**比值**而非绝对值（帧数不同）：每个来源桶除以 \`semanticSceneSubmittedSkinnedCount\`（分母同源），
  \`SourceChurnCount\` 除以 \`PaletteStablePartSampleCount\`。
  **容差建议**：分母相等时要求分子精确相等；帧数不可控时按现有 AB 工具的做法
  （\`analyze_semantic_stats_ab.py\` 的 MAD 过滤 + 配对相对变化）要求**单侧不劣化**（churn 与
  budget 不上升），并对来源分布给 **±2 个百分点**的绝对带（该数字是**工程选择**，不是实测得出——
  见 §4-4）。**\`budgetExceeded\`/\`framesBudgetExceeded\` 不接受任何容差：必须为 0 或 ≤ 基线。**
- **口径 3（静态，fail-closed，永远可跑）**：见 §2.4 G2。

**失败模式清单（门禁必须至少杀死这 6 类）**

1. **诊断门被跨越**：发射块整块被移到
   \`War3SemanticPaletteDiagnosticsRuntime()\` 之外 ⇒ 诊断 OFF 时不再全 0。
   捕手：静态断言 \`"if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {"\` 仍存在
   （export 门禁 :176 已有此断言）+ 宿主差分在 \`PALETTE_DIAG=0\` 矩阵下必须全 0。
2. **桶归属互换**：\`DrawTimeCaptured\` 与 \`OwnedPartSnapshot\` 互相吞并——**总数不变、分布错**。
   捕手：必须**逐桶**对照，禁止只对"7 桶求和"。
3. **探针表状态被重置/搬动**：B3 含 3 张跨帧存活的 \`thread_local\` 表（\`PaletteProbeEntry\` 8192、
   \`LeaseKeyAttributionEntry\` 8192、\`StrictProbeEntry\`）。把表搬进模块会改变
   **同一进程内的初始化时机与存活域**；若实现者"顺手"改成普通 \`static\` 或
   \`std::unordered_map\`，\`HashChurn\`/\`SourceChurn\`/\`StrictSlice*Churn\` 会整体漂移。
   捕手：静态断言这三张表仍是 \`thread_local\` + 定长 \`std::array\` + 位掩码取槽
   （\`\& (k…Entries - 1u)\`）；差分电池按调用序驱动。
4. **顺序敏感的 \`CombinedHash\` 累加顺序被改写**：捕手 = 差分逐字段对照（口径 1）。
5. **双重计数**：迁移期同时留下旧实现与新实现（或 \`++\` 被复制）⇒ 所有桶翻倍。
   捕手：静态断言每个桶名在 \`device.cpp\` + 模块合计**只有一处 \`++\`**
   （M2-3 变异 2 已证明"能编译的回流"只有预算门禁能点名，值类断言必须另加）。
6. **\`budgetExceeded\` 被波及**：该字段写入点在 Arena/mapped allocator 路径，
   B3 块并不写它；但迁移若顺手改了资源账结构就可能踩到。
   捕手：写入点集合与数量（9）静态钉死。

### 2.4 门禁形态（建议 G1/G2/G3）

- **G1（宿主确定性差分，M2-1/2/3 同范式，主证据）**
  - 新增 legacy 参考 \`.inc\`（由入库生成器从 pre-M2-5 快照 fail-closed 抽取，登记 SHA/字节数）。
  - 扩展既有 \`src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp\`（**加法式**，
    不得改动 M2-1/2/3 既有断言）。
  - 场景电池：诊断门 ON/OFF × 7 种来源 × provenance 6 桶 × 同 key 连续帧序列
    （new→stable→raw-changed→group-changed→source-changed→slot-changed）× 窗口填满与回绕 ×
    stale→live 过渡 × strict-slice 命中/未命中；再加定种子随机世界驱动三张表**填满并越过替换游标**。
  - 断言：逐字段相等 + 段末 \`memcmp\`；下限断言按实测的 ~70%（沿用 M2-2/M2-3 的登记方式）。
- **G2（静态 fail-closed，最强回归护栏）**
  - 预算门禁新增 \`FROZEN_M2_4\` / \`FROZEN_M2_5\`（**独立成表再 \`update\`**，
    理由与 M2-1/2/3 相同：M1 等价门禁按 \`budget<baseline\` 推导自己的 26 个 COVERED）。
  - 新静态门禁（或扩展 \`test_active_path_palette_instrumentation_export_static.py\`）：
    (a) 44 条计数器逐名在 \`scene.h\` 的存在性与类型（\`uint32_t/uint64_t … = 0;\`）；
    (b) 每条在 \`device.cpp + 模块\` 的 \`++\`/\`+= max\` 合计为**迁移后应有的单一实现**；
    (c) 每个计数器沿 §2.2 的**完整出口链**（bridge/hub/control plane/perf ×2 写手）仍可查；
    (d) \`budgetExceeded\` 写入点 == 9 且全部在预算失败路径；
    (e) 三张探针表仍为 \`thread_local\` 定长数组。
- **G3（实机/隔离桌面不劣化，非确定性）**
  - 复用既有取证启动器与报告（\`runtime_status.json\` / perf HTML 的 \`shadowBudgetSummary\`）。
  - **本轮已核实的前置缺口**：本树**没有** pre-M2-5 的 taxonomy 基线快照
    （既没有专门的 palette-taxonomy 导出，也没有已登记的基线 JSON）。
    ⇒ G3 若要成立，必须先做一次"迁移前基线采集"并登记为证据；否则 G3 只是"迁移后看起来正常"，
    不构成"不劣化"。

---

## 3. 风险与依赖

### 3.1 M2-5 若要在实机对照，需要什么

- **必须有**：同一 DLL 的迁移前/迁移后两次采集、同一地图与同一相机轨迹、同一 env 矩阵
  （尤其 \`DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1\`，否则 B3 整块不执行、全 0，什么也证明不了）。
- **本树是否有离线等价物**：**没有**。核实依据：
  - 宿主差分目标 \`war3_live_palette_selection_test.exe\` 只链接**模块 .cpp**，
    **不链接 \`d3d9_device.cpp\`**（M2-1 记录 §3.1 明说），因此今天无法在宿主驱动 B3；
  - \`test_device_semantic_responsibility_budget_static.py\` 只看符号站点，不看值；
  - \`test_active_path_palette_instrumentation_export_static.py\` / \`test_skin_palette_contract_static.py\`
    只看**字段名与出口存在性**，不看值；
  - \`AutoTest/_archive/...\` 下有历史分析脚本（如 \`phase720_hot_shadow_poll.py\`）会读这些桶，
    但它们是**已归档的一次性脚本**，不是可复跑门禁。
  ⇒ **离线等价物目前只能靠"先把发射抽成纯函数"造出来**（即 G1）。这本身是 M2-5 设计的一部分，
    也是**建议主线程下一步做的唯一一件最重的事**（见文末建议）。
- **结论**：M2-5 的"同场景不劣化"在今天**只能靠实机**（口径 2）或**先造离线差分**（口径 1）。
  推荐先造口径 1，理由是它在**迁移前**就能跑、且不需要游戏窗口、失败可复现；
  实机口径 2 只能作为**补充**（它无法区分"迁移噪声"与"场景差异"）。

### 3.2 为什么 M2-5 的风险高于 M2-4

M2-1/2/3 迁的都是"输入→输出"的函数体：差分电池构造输入即可。
M2-5 迁的是**状态机 + 跨帧探针表 + 顺序敏感哈希**，其中 B3 块的状态（三张 \`thread_local\` 表）
**不在任何可构造参数里**——要把它测出来，必须显式把"表的所有者"一起搬进模块并保持
\`thread_local\` 语义。这是 M2-5 与前三片**本质不同**的地方，也是本文把它单列的原因。

### 3.3 为什么 M2-5 应排在 M2-4 之后

1. **B3/B4/B5 的输入量由 A4/A5 与选择链的返回值决定**（如 \`paletteSourceThisSubmit\`、
   \`submittedPaletteHash\`）。若先动 M2-5 的编排，再改 M2-4 的判定实现，两次改动会叠加，
   "计数变了"无法归因到哪一次。
2. **G1 的宿主差分需要模块侧有稳定的纯函数边界**；A1–A7 迁完后，B3 可依赖的模块内符号集合才会固定。
3. **预算门禁的点名能力**：M2-4 的 \`FROZEN_M2_4\` 登记完成后，判定 3 的盲区（A3/A4/A5 命名族不匹配）
   才被补上；否则 M2-5 期间任何一次回流都可能同时踩两个盲区，红点难以定位。
4. **最小可回退粒度**：M2-4 每项可独立回退（纯函数搬家）；M2-5 一旦动了跨帧表，回退要同时处理
   "表状态在哪"的问题。先易后难。

---

## 4. 未验证 / 不确定项（**不得当作已验证**）

1. **B11 的归类未定**：\`War3GetOrCreateSemanticShadowPalette\` 同时含 compose-policy 判定与
   device 资源缓存。我在本轮**没有**把它拆开核实（未逐行确认哪部分属"判定"、哪部分属"资源账"）。
   本文把它列 B11 并标注"归类待裁定"，**不主张**它属 M2-4。
2. **A8 的迁出方式未定**：\`War3SemanticBoundsRadiusForObjectKind\` 的 **1 定义 + 7 调用点本轮已全部定位**（见表 A8）；
   但迁 A4 时如何供应该符号（模块声明 + 定义留 device / 一并迁出 / 复制）**未做决定**，
   也未评估那 6 个 bounds 调用点若随之改锚对既有断言的影响。
3. **B3 块内的 34 个字段**是用"花括号配对 + 正则扫 \`stats.\`/\`st.\`"得到的；
   花括号配对未做字符串/注释感知，若块内有含花括号的字符串字面量，块边界可能偏差。
   我抽查了块的头部（:21202）与尾部（:21574-21580）文本，未发现此类字面量，但**未做全块字符级校验**。
4. **\`±2 个百分点\`的实机容差是工程选择，不是实测值**。本轮没有跑过任何实机基线，
   也没有可用的历史基线分布；该数字必须在首次基线采集后按真实方差重新裁定。
5. **\`budgetExceeded\` 的"当前门禁覆盖"未查全**：我只读了
   \`test_active_path_palette_instrumentation_export_static.py\` 与
   \`test_skin_palette_contract_static.py\`，**没有逐份读** \`AutoTest\` 下
   \`test_shadow_arena_memory_budget_static.py\` / \`test_shadow_capture_post_breakdown_static.py\`
   等可能间接断言预算字段的门禁。§2.1 分组 6 的"无静态断言"是**未查全**，不是"确认没有"。
6. **\`m_war3ShadowFallbackBudgetExceeded\` 的定义行号未核实**（只核实了 10 个写入点，
   且未逐点确认它总与 \`budgetExceeded\` 成对）。
7. **A9/A10 是否是"真死代码"**：我做了**全 \`src\`** 的名字出现次数统计（各 1 次，仅定义），
   因此在本仓库范围内确为 0 调用/0 实例化；但**未检查**构建系统是否在某处 \`.inc\` 包含或
   宏展开中引用（例如 \`#include\` 到 device.cpp 的生成片段）。这是低概率但未排除。
8. **本文没有做任何构建、没有跑 meson/全量静态、没有跑实机**。§0 的门禁数字只来自
   **一个只读静态门禁**的实测输出；其余"当前门禁覆盖"结论来自**读源码**。
9. **编号为 \`2026-09-18-m2-4-…\` 的迁移记录不存在**：本轮没有做 M2-4，
   本文提到 M2-4 时一律是"可继续迁出的候选"，不是"已完成"。

---

## 5. 与其它文档的边界

- 本文**不改** \`docs/plan/2026-09-16-device-semantic-responsibility-migration.md\` 的任何结论；
  它仍是"清册与方案"。本文是它的 M2-5 落地设计草案。
- M2-1/2/3 的等价记录、\`.inc\`、差分测试、既有门禁的 SHA 在本文观测时**均未改动**
  （模块 .h/.cpp 的 SHA 与 M2-3 记录逐字节一致，见 §0）。
- 本文**不是**稳定版依据，**不得**被引用为"M2-4/M2-5 已完成"或"palette 侧职责已迁完"。
