# 2026-09-18 — T7 批次 4（U5）Registry domain 拒绝计数**对外出口**实施记录

> **状态：观测出口已接通并全门禁通过；本块不改任何准入/发布/淘汰/退役语义，不加 env，
> 不部署，不启动游戏，不做 git 写。本文不得被引用为「Registry domain 隔离已完成」、
> 「Stage13 已就绪」或「稳定版」。**
>
> 范围：`docs/plan/2026-09-18-registry-domain-isolation-record.md` §8-2 / §9-1 点名的缺口
> —— 6 个 domain 拒绝计数只写进 `War3ShadowPersistentDiagnosticsFrame`，没有接进
> `War3PerfMonitor` 的对外 dump。本块只把它们接到**既有** persistent-geometry 拒绝计数族的
> 出口链（`docs/plan/2026-09-18-m2-5-taxonomy-and-remaining-scope.md` §2.2 的第 6 跳），
> 不新创通道、不改任何判定。

---

## 0. 结论摘要（每条都可在本树复核）

1. **6 个计数全部接出**：`rejectDomainConflict` / `domainLookupRejects` / `domainPublishRejects` /
   `domainGcEraseRejects` / `domainResetPurgeRejects` / `domainResetOwnerRejects` 现在同时出现在
   * device.cpp → `War3PerfMonitor::PersistentGeometryFrameStats` 的 bridge 传递（每 Present 区间），
   * perf monitor 的 per-frame workload 快照（每帧 1 份），
   * perf monitor 的 `shadowBudgetAggregate` 区间累加，
   * **两个** JSON 写手段（`shadowBudgetSummary` 与 `shadowRuntimeV2Summary`），
   * `workloadSeriesColumns` / `workloadSeries` 逐帧序列（1 列 + 1 值）。
2. **命名与既有 44 条族一致**：device 侧沿用 `War3ShadowPersistentDiagnosticsFrame` 原名；
   perf monitor 的聚合字段用 `persistent*` 前缀（与 `persistentRejectCapacity` 同族）；
   JSON 键 = 聚合字段名；workloadSeries 列名 = 当帧字段名（与 `rejectCapacity` 同构）。
3. **口径**：`rejectDomainConflict` 是 legacy `persistentRejectCreateOrBudget` 的 ShadowCapture
   失败分桶之一，因此**唯一**被加进 `persistentRejectCreateOrBudgetDetailedTotal`；
   另外 5 个是「越权触碰被拒绝」的事件计数，**不**进入该求和（静态门禁 + 定向测试双向钉死）。
4. **出口位置如实说明**：本族（含既有 `persistentReject*`）**不在** `runtime_status.json` 里
   （`war3_diagnostics_hub.cpp` 全文没有任何 `persistentReject*` / `shadowBudgetSummary` 键，
   见 §2.7）。既有等价出口是 perf monitor 的 `war3_perf_report.html`（内嵌 JSON，由
   `generateJsonDataFromSnapshot` 产出）。因此本块接的是**既有等价出口**，而不是另开一条
   `runtime_status.json` 通道。
5. **测试**：新增静态出口门禁 14 tests、新增定向增量测试 5 tests；既有宿主机域模型测试
   从 41/41 扩到 **46/46**（补 reset 归属校验路径 + 6 个计数在纯域内稳态为 0）。
6. **两条变异真跑**：①删掉 `agg.persistentDomainLookupRejects += stats.domainLookupRejects;`
   → 可编译可链接但静态门禁红 + 定向测试红；②只在 `shadowBudgetSummary` 把
   `persistentDomainGcEraseRejects` 键改名 → 可编译可链接但两条门禁都在该写手区域变红。
   两次还原后 SHA MATCH + touch 真实重编译 + 复绿。
7. **全门禁在最终树上取数**（§6）：`ninja -n` no work、真实重编译 27 步 exit 0、预算门禁
   （冻结 157 / 165→112 / 当前 112 / 已迁出 46 / 站点 567）、七条等价门禁全绿、meson **84/84**、
   全量静态 **256/256**、记录器 23/23、成本 PASS(38/38)、生命周期 187/187、解析器 94/94、
   根读方 55/55、往返 CERTIFIED、DLL 见 §6.3。
8. **未做到 / 未验证**：没有任何实机/前台数据（F3 仍未执行）；perf monitor 出口**未被执行级
   测试覆盖**（本树无法链接 War3PerfMonitor 到宿主机）；域维度的字节/条目分账（D4/D5）与
   key 级 domain（D1）仍未做。逐条见 §7。

---

## 1. 字段清单与逐处行号（最终树）

行号取自最终树（2026-09-18T06:15 之后、文档写盘之前未再改源）。

| device 侧字段（`War3ShadowPersistentDiagnosticsFrame`） | perf monitor 聚合字段 | device.h 定义 | device.cpp 累加（所属函数） | device.cpp bridge | perf_monitor.h workload / frameStats / aggregate | perf_monitor.cpp per-frame / agg | JSON 键 shadowBudgetSummary / shadowRuntimeV2Summary | workloadSeries 列 / 行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `rejectDomainConflict` | `persistentRejectDomainConflict` | :2404 | :48628（`War3TryCaptureShadowCaster`，ShadowCapture 失败分桶 switch 的 `DomainConflict` case） | :33671 | :214 / :317 / :1274 | :2439 / :2510 | :7527 / :9060 | :9819 / :9941 |
| `domainLookupRejects` | `persistentDomainLookupRejects` | :2432 | :19591、:19602（`War3TryFindShadowPersistentGeometry`，两处互斥分支） | :33672 | :215 / :318 / :1275 | :2440 / :2511 | :7529 / :9062 | :9819 / :9941 |
| `domainPublishRejects` | `persistentDomainPublishRejects` | :2433 | :19686（`War3CreateShadowPersistentGeometryAfterMiss`） | :33673 | :216 / :319 / :1276 | :2441 / :2512 | :7531 / :9064 | :9820 / :9942 |
| `domainGcEraseRejects` | `persistentDomainGcEraseRejects` | :2434 | :19483（`War3GcShadowPersistentGeometry` 的 `eraseOwnedRegistrySlot`） | :33674 | :217 / :320 / :1277 | :2442 / :2513 | :7533 / :9066 | :9821 / :9942 |
| `domainResetPurgeRejects` | `persistentDomainResetPurgeRejects` | :2435 | :17769（`War3MaybeInsertBeforeUi`）、:24100（`War3DrainShadowCasterTombstones`） | :33675 | :218 / :321 / :1278 | :2443-2444 / :2514 | :7535 / :9068 | :9821 / :9943 |
| `domainResetOwnerRejects` | `persistentDomainResetOwnerRejects` | :2436 | :23748（`War3ResetShadowSessionState`，容器 move 之前） | :33676 | :219 / :322 / :1279 | :2445-2446 / :2515 | :7537 / :9070 | :9822 / :9943 |

文件：`src/d3d9/d3d9_device.h`、`src/d3d9/d3d9_device.cpp`、
`src/d3d9/war3/tools/war3_perf_monitor.h`、`src/d3d9/war3/tools/war3_perf_monitor.cpp`。

补充：`persistentRejectCreateOrBudgetDetailedTotal` 的求和式在 `war3_perf_monitor.cpp:7540-7547`
（shadowBudgetSummary）与 `:9073-9080`（shadowRuntimeV2Summary）各出现一次，最后一项是
`shadowAgg.persistentRejectDomainConflict`。

合约字符串 `persistentDomainCounterContract` 在两个写手各 1 条
（`:7525-7526` / `:9058-9059`）：`"per-Present registry domain owner-check rejects; steady state is 0;
rejectDomainConflict is the ShadowCapture bucket of the DomainConflict create failure,
domainPublishRejects covers every caller, and domainLookupRejects is lookup-only"`。

---

## 2. 出口链每段证据

### 2.1 当帧结构（device.h，本块**未改**这一层）

`War3ShadowPersistentDiagnosticsFrame`（`d3d9_device.h:2392`）在上一块已含 6 个字段（:2404、:2432-2436）。
本块不动它们的定义与累加点。

### 2.2 累加点与口径（本块**未改**）

6 个 `++` 落点与所属函数见 §1；每个落点都由 `AutoTest/test_registry_domain_count_export_static.py`
的 `FROZEN_ACCUMULATION_FUNCTIONS` 钉死（函数名 + 该函数内处数），越权/串径即红。

### 2.3 bridge 传递（device.cpp，**本块新增**）

`d3d9_device.cpp:33671-33676`：Present 安全点的 `ShadowPersistentDiagnosticsPublish` 段里，
`std::exchange(m_war3ShadowPersistentDiagnosticsFrame, {})` 取出「刚刚结束的 Present 区间」后，
逐字段恒等拷进 `war3::War3PerfMonitor::PersistentGeometryFrameStats stats`，
随后 `war3::War3PerfMonitor::instance().notePersistentGeometryFrame(stats);`（:33735）。

新增块（原文）：

```cpp
    // 2026-09-18 T7/U5: registry domain owner-check rejects. Observation-only
    // pass-through of the completed Present interval; no admission, publish,
    // eviction or reset semantics are changed here.
    stats.rejectDomainConflict = completed.rejectDomainConflict;
    stats.domainLookupRejects = completed.domainLookupRejects;
    stats.domainPublishRejects = completed.domainPublishRejects;
    stats.domainGcEraseRejects = completed.domainGcEraseRejects;
    stats.domainResetPurgeRejects = completed.domainResetPurgeRejects;
    stats.domainResetOwnerRejects = completed.domainResetOwnerRejects;
```

### 2.4 perf monitor 结构（perf_monitor.h，**本块新增**）

三处各 6 个字段（`= 0` 初值，与同族字段一致）：

* `FrameWorkloadSnapshot`（:214-219）—— 逐帧快照；
* `War3PerfMonitor::PersistentGeometryFrameStats`（:317-322）—— 每次 `notePersistentGeometryFrame` 的入参；
* `ShadowBudgetAggregate`（:1274-1279）—— 报告窗口累加（`persistent*` 前缀）。

### 2.5 perf monitor 累加（perf_monitor.cpp，**本块新增**）

* per-frame 恒等拷贝 `m_currentFrameWorkload.X = stats.X;`：:2439-2446（其中两个折行）；
* 区间累加 `agg.persistentX += stats.X;`：:2510-2515。

累加仍**只**在 `War3PerfMonitor::notePersistentGeometryFrame` 内（静态门禁用所属函数断言钉死）。

### 2.6 两个 JSON 写手 + 逐帧序列（perf_monitor.cpp，**本块新增**）

* `shadowBudgetSummary`（写手 A，段起点 :6070）：键 :7527/:7529/:7531/:7533/:7535/:7537，取值表达式紧随其后各 1 条；
* `shadowRuntimeV2Summary`（写手 B，段起点 :7704）：键 :9060/:9062/:9064/:9066/:9068/:9070，取值表达式紧随其后各 1 条；
* `workloadSeriesColumns`：:9819-9822 新增 6 列；`workloadSeries` 行取值：:9941-9943 新增 6 个值（顺序与列一一对应）；
* `persistentRejectCreateOrBudgetDetailedTotal`：两个写手都把 `persistentRejectDomainConflict` 加进求和（:7547 / :9080）。

静态门禁 `test_both_json_writers_export_every_counter` 把两个写手**分段**断言（各恰好 1 键 1 值），
因此「只写一个写手」或「键名与取值不一致」都会被点名（变异 2 已验证）。

### 2.7 为什么不是 runtime_status.json（如实说明）

`runtime_status.json` 由 `war3_diagnostics_hub.cpp` 写出；该文件全文**没有**任何
`persistentReject*` / `persistentPool*` / `persistentCapacity*` / `shadowBudgetSummary` 键
（`rg` 实测 0 命中）。也就是说既有 persistent-geometry 拒绝计数族本来就不走 hub/control plane，
它的对外出口就是 perf monitor 的 `war3_perf_report.html`（`processExportJob` →
`generateJsonDataFromSnapshot` → 内嵌 `const data = {...};`）。按「照既有路径接、不新创通道」的
要求，本块接的是这条既有等价出口；若将来要把本族搬进 `runtime_status.json`，那是**新通道**，
需要单独裁定。

### 2.8 语义中立性

新增代码全部是「读当帧字段 → 恒等拷贝 → 累加 → 打印」。没有新增/移动任何准入判定、
发布分支、淘汰条件、owner-check 判定或 fence/退役行为；没有新增任何 `DXVK_WAR3_*` env
（静态门禁 `test_no_new_environment_variable_is_introduced` 冻结被触碰文件的 `DXVK_WAR3_`
字面量数：device.cpp 138、perf_monitor.h 2、perf_monitor.cpp 108）。

---

## 3. provenance

### 3.1 逐文件 pre / post 身份与 %TEMP% 备份

捕获时间 **2026-09-18T06:02:37+08:00**；备份目录 `%TEMP%\registry_domain_count_pre\`
（= `D:\tmp\AppData\ADMINI~1\Local\Temp\registry_domain_count_pre\`）。
每个备份写入后立即回读 SHA-256 与源文件比对，全部 `MATCH=True`。
实现完成后又把**实现态**四个文件另存为 `impl__*` 前缀副本，用于变异还原的逐字节比对。

| 文件 | pre bytes / SHA-256 | post bytes / SHA-256 | 备份名 |
| --- | --- | --- | --- |
| `src/d3d9/d3d9_device.cpp` | 2,256,632 / `60CA6BCD170DF4022D58917A018E3594303E1564ADB10BEF93AF39CB9371F547` | 2,257,251 / `CC40687AB5EA8BD321A696038812216AB8842B521450CEF322FABD568182DBE8` | `src__d3d9__d3d9_device.cpp` / `impl__src__d3d9__d3d9_device.cpp` |
| `src/d3d9/war3/tools/war3_perf_monitor.h` | 76,392 / `F213EE29F0A1D7A3475ECC1A85C407FA7751C1FAEE6E405770BB20271B96616B` | 78,044 / `C5BB2466EE8CBC43B0AE3832EA5D7A05E471552A930A454EDD63D01648C5251B` | `src__d3d9__war3__tools__war3_perf_monitor.h` / `impl__…` |
| `src/d3d9/war3/tools/war3_perf_monitor.cpp` | 621,824 / `139D070BBFC759F34FDD35F2B65B37434BEDD42858FE509FE9230EC3DA29E052` | 625,322 / `7E4ED64A3A76C63169BAA4852183486D30341A3E28198B9384012A82C343007F` | `src__d3d9__war3__tools__war3_perf_monitor.cpp` / `impl__…` |
| `src/d3d9/war3/render/tests/war3_shadow_geometry_domain_test.cpp` | 21,323 / `FAF525DDED23C6E00026E6285B740AB5D50E08CCF8F9E55DDF20C8422E41A812` | 24,547 / `674B987CCE378B2A244D55B68F2CE89D837AA167A0A99CD5492A437B807AF721` | `src__d3d9__war3__render__tests__war3_shadow_geometry_domain_test.cpp` / `impl__…` |
| `docs/plan/2026-09-18-overnight-progress-log.md` | 67,692 / `93B3CDF08DFAF7125E02641C983AD1BB737562D5CF9E3B22C8703056FA677293` | 见 §6.4（追加后回读） | `docs__plan__2026-09-18-overnight-progress-log.md` |
| `docs/agent-history/DEVELOPMENT_CHANGELOG.md` | 324,829 / `7E85DE517D525153AE6209E38B9A970BF5710430AEA01BB9465027D3121969F0` | 见 §6.4（追加后回读） | `docs__agent-history__DEVELOPMENT_CHANGELOG.md` |

新增文件（pre = 不存在）：

| 文件 | bytes / SHA-256 |
| --- | --- |
| `AutoTest/test_registry_domain_count_export_static.py` | 14,700 / `7F4B39DAB663EAD12755C5BB7E0961CF25844FE5A359A4D386628EE65F5DF19F` |
| `AutoTest/test_registry_domain_count_export.py` | 12,867 / `C48902816D0575CD5E90BBA357F5649409C4FCFAE22416A2575F7A42B6F1E9A2` |
| `docs/plan/2026-09-18-registry-domain-count-export-mutation-raw-output.log` | 见 §6.4 |
| `docs/plan/2026-09-18-registry-domain-count-export-full-gate.log` | 见 §6.4 |
| `docs/plan/2026-09-18-registry-domain-count-export-record.md`（本文件） | 见 §6.4 |

注：`src/d3d9/d3d9_device.h` **未改**（pre/post 同为 137,213 B /
`D4E15762E915A8F49E27CA4AB505A50CE74AF1866659514E315C66F3201374F5`）。

### 3.2 反向逐字节重建

生产侧改动 = 4 个文件、共 **13 个纯插入块**（无删除、无改写），因此 pre 状态可由 post 反向重建：
按下列锚点删掉对应插入块即可。插入块全部以 `2026-09-18 T7/U5` 注释或字段名为标识。

| # | 文件 | 锚点（插入位置） | 插入内容 |
| --- | --- | --- | --- |
| 1 | `d3d9_device.cpp` | `stats.rejectOther = completed.rejectOther;` 之后、`stats.createAttempts` 之前 | §2.3 的 3 行注释 + 6 行恒等拷贝 |
| 2 | `war3_perf_monitor.h` | `FrameWorkloadSnapshot` 的 `uint64_t rejectOther = 0;` 之后 | 4 行注释 + 6 行 `uint64_t <name> = 0;` |
| 3 | `war3_perf_monitor.h` | `PersistentGeometryFrameStats` 的 `uint64_t rejectOther = 0;` 之后 | 同上（8 空格缩进） |
| 4 | `war3_perf_monitor.h` | `ShadowBudgetAggregate` 的 `uint64_t persistentRejectOther = 0;` 之后 | 3 行注释 + 6 行 `uint64_t persistent<Name> = 0;` |
| 5 | `war3_perf_monitor.cpp` | `m_currentFrameWorkload.rejectOther = stats.rejectOther;` 之后 | 6 行 `m_currentFrameWorkload.X = stats.X;`（两处折行） |
| 6 | `war3_perf_monitor.cpp` | `agg.persistentRejectOther += stats.rejectOther;` 之后 | 6 行 `agg.persistentX += stats.X;` |
| 7-8 | `war3_perf_monitor.cpp` | 两个 JSON 写手的 `persistentRejectOther` 键块之后（两处**同一文本**，一次替换命中 2 处） | 1 条 contract 键 + 6 对键/取值；同时把 `shadowAgg.persistentRejectDomainConflict` 加入 `persistentRejectCreateOrBudgetDetailedTotal` |
| 9 | `war3_perf_monitor.cpp` | `workloadSeriesColumns` 的 `"rejectOther", "createAttempts",` 之间 | 新增 4 行字符串字面量（列列表） |
| 10 | `war3_perf_monitor.cpp` | `workloadSeries` 行的 `w.rejectOther` 与 `w.createAttempts` 之间 | 新增 5 行取值表达式 |
| 11 | 宿主机测试 | 模型字段 `uint64_t domainPurgeRejects = 0u;` 之后 | `uint64_t domainResetOwnerRejects = 0u;` |
| 12 | 宿主机测试 | `PurgeDomain(...)` 之后、`size_t GeometryCount(...)` 之前 | `struct RetireResult` + `RetireResult RetireSession()` + `void InjectUnownedAlias(...)` |
| 13 | 宿主机测试 | C8 稳态断言与 `if (g_failures != 0u)` 之间 | C8 断言追加 `&& model.domainResetOwnerRejects == 0u`；新增 C9 块（5 个 Check）；头注释补一行 |

其它依据：

* 全部生产改动都由 `edit` 工具或等价文本替换完成，替换对（old → new）逐条落在 §1/§2 与两个新门禁的断言里；
* `ninja -C build32 -n` 在最终树上两次 `no work to do`（§6.1/§6.6），说明最终树与最终 DLL source-consistent；
* **未做任何 git 写**：`git` 只被用于只读查询；`git status --porcelain` 在本块前后都没有新增删除项；
* 未部署任何 DLL、未启动游戏、未触碰 `E:\Work`。

---

## 4. 静态门禁与定向测试

### 4.1 `AutoTest/test_registry_domain_count_export_static.py`（新，14 tests，fail-closed）

逐字段钉死的「允许处数」表（每个字段都一样）：

| 段 | 允许处数 |
| --- | --- |
| device.h `War3ShadowPersistentDiagnosticsFrame` 定义 | 1 |
| device.cpp **重复定义** | **0** |
| device.cpp `NAME++` 累加 | lookup 2、purge 2、其余各 1（并在 `FROZEN_ACCUMULATION_FUNCTIONS` 里钉死所属函数） |
| device.cpp bridge 恒等传递 | 1 |
| perf_monitor.h workload / frameStats / aggregate | 各 1 |
| perf_monitor.cpp per-frame 拷贝 / 区间累加 | 各 1（累加所属函数必须是 `notePersistentGeometryFrame`） |
| perf_monitor.cpp JSON 键 / 取值（**每个写手**） | 各 1 |
| perf_monitor.cpp workloadSeries 列 / 行 | 各 1 |
| `rejectDomainConflict` 进入 legacy 明细求和 | 1（两个写手各 1；其余 5 个 0） |
| 新 env | 0（冻结 `DXVK_WAR3_` 字面量数） |

### 4.2 `AutoTest/test_registry_domain_count_export.py`（新，5 tests，定向）

性质（如实声明）：被测的是**生产源文本**给出的映射（累加点/所属函数、bridge 恒等、per-frame 恒等、
区间累加、两个写手的键与取值），测试本身**不执行** C++ 出口代码（War3PerfMonitor 依赖 d3d9/DXVK
设备侧类型，不是宿主机可链接目标）。它按源码锚定的增量模型跑 7 个场景 × 2 个 Present 区间：

| 场景 | 事件 | 期望导出增量 |
| --- | --- | --- |
| S1 纯域内稳态 | 无越权 | 6 个计数全 0 |
| S2 lookup 跨域命中拒绝 | `War3TryFindShadowPersistentGeometry` ×1 | `domainLookupRejects`=1 |
| S3 publish 跨域拒绝（ShadowCapture 调用方） | `War3Create…AfterMiss` ×1 + `War3TryCaptureShadowCaster` ×1 | `domainPublishRejects`=1 且 `rejectDomainConflict`=1 |
| S4 publish 跨域拒绝（Semantic/UpperLayer 调用方） | `War3Create…AfterMiss` ×1 | 只有 `domainPublishRejects`=1 |
| S5 GC / 预算回收跨域拒绝 | `War3GcShadowPersistentGeometry` ×1 | `domainGcEraseRejects`=1 |
| S6 reset 整会话归属校验 | `War3ResetShadowSessionState` ×1 | `domainResetOwnerRejects`=1 |
| S7 Stage13 domain 作用域 clear 两个站点 | `War3MaybeInsertBeforeUi` ×1 + `War3DrainShadowCasterTombstones` ×1 | `domainResetPurgeRejects`=2 |

并要求「无越权区间」导出全 0（稳态为 0）、legacy 明细求和精确等于 create 失败域冲突数、
且另外 5 个桶不能污染该求和。

### 4.3 宿主机域模型测试（扩展，41 → 46/46）

`src/d3d9/war3/render/tests/war3_shadow_geometry_domain_test.cpp`（只包含产品判定内核
`war3_shadow_geometry_domain.h`）新增：

* `DomainRegistryModel::RetireSession()`：整会话退役**之前**的归属校验（与生产
  `War3ResetShadowSessionState` 同一 `ShadowGeometryOwnerAccepts` 判定），无主别名先丢弃并计数，
  之后仍整体退役（不能选择性拒绝退役）；
* `InjectUnownedAlias(...)`（仅测试用）构造「槽位域 ≠ 它指向的常驻条目域」；
* C9 五个断言：干净归属退役零拒绝 / 干净归属 5 个计数全 0 / 无主别名被丢弃并计数 / 真归属槽位仍退役 /
  别名不触碰其它计数；
* C8 稳态断言追加 `domainResetOwnerRejects == 0u`。

这样 lookup / publish / GC / reset 四条路径各有一条定向用例，且纯域内稳态为 0。

---

## 5. 变异真跑（≥2 条，原始输出见同目录 mutation-raw-output.log）

| 变异 | 内容 | 构建 | 门禁 | 还原 |
| --- | --- | --- | --- | --- |
| 1 | 删掉 `agg.persistentDomainLookupRejects += stats.domainLookupRejects;` | NINJA_EXIT=0（`[2/2] Linking target src/d3d9/d3d9.dll`） | 静态出口门禁 `FAILED (failures=1)` `AssertionError: 0 != 1 : domainLookupRejects`，GATE_EXIT=1；定向测试 `FAILED (failures=3)` `AssertionError: domainLookupRejects 没有区间累加`，DIRECTED_EXIT=1 | SHA MATCH（`7E4ED64A…`）+ touch 真实重编译（编译 perf_monitor.cpp + link，NINJA_EXIT=0）+ 复绿 14 OK / 5 OK |
| 2 | 只在 `shadowBudgetSummary` 把键 `persistentDomainGcEraseRejects` 改名为 `…Renamed`（取值表达式不动） | NINJA_EXIT=0 | 静态出口门禁 `FAILED (failures=1)` `AssertionError: 0 != 1 : shadowBudgetSummary 缺 persistentDomainGcEraseRejects 的 JSON 键或写重`，GATE_EXIT=1；定向测试 `FAILED (failures=2)` `AssertionError: persistentDomainGcEraseRejects: JSON 键数 != 1`，DIRECTED_EXIT=1 | SHA MATCH（`7E4ED64A…`）+ touch 真实重编译 + 复绿 14 OK / 5 OK + `ninja -n` no work |

逐字节身份：

| 变异 | 变异态 | 还原态 | 逐字节相同 |
| --- | --- | --- | --- |
| 1 | 625,255 B / `0694E8B455D1109548A3F113B5B308FAFB511B7BC5F21CE823BDD50589C18988` | 625,322 B / `7E4ED64A3A76C63169BAA4852183486D30341A3E28198B9384012A82C343007F` | 是（`SHA_MATCH=true`） |
| 2 | 625,329 B / `A3B732674E2C60C6E938B1B76A0C5A7F278F9B8777759A829E720C4B732A0974` | 同上 | 是（`SHA_MATCH=true`） |

两条变异都**能编译能链接**——即「能编译的回流」不会被编译器抓住，只有出口门禁/定向测试能点名
（符合 m2-5 §2.3 失败模式 5）。变异 2 只改一个写手，两条门禁都在**该写手区域**变红，
证明「两个写手各自恰好 1 键」不是对全局计数的重复断言。

---

## 6. 全门禁（最终树）

原始输出：`docs/plan/2026-09-18-registry-domain-count-export-full-gate.log`（本次跑，
2026-09-18T06:13:29 → 06:17:08）。

### 6.1 构建与 no-work

| 门禁 | 结果 |
| --- | --- |
| `ninja -C build32 -n`（前） | `ninja: no work to do.` EXIT=0 |
| 真实重编译（touch `war3_perf_monitor.h` + `d3d9_device.cpp` + 宿主机测试；Below Normal + `-j2`） | **27 步**（含 `[25/27] Linking target src/d3d9/war3_shadow_geometry_domain_test.exe`、`[27/27] Linking target src/d3d9/d3d9.dll`），NINJA_EXIT=0，elapsed 129.5 s |
| `ninja -C build32 -n`（后） | `ninja: no work to do.` EXIT=0 |

### 6.2 其它门禁

| 门禁 | 结果 |
| --- | --- |
| 预算门禁 | EXIT=0（冻结符号 157；行首站点 迁移前 165 → 上限 112 → 当前 112；已迁出 46；device.cpp 行首定义站点总数 567） |
| M1 等价门禁 | EXIT=0（26 个已迁出符号；legacy 参考 43,452 B / `2EA43F97…`；差分下限 3,000,000；probe 6 组） |
| M2 等价门禁 | EXIT=0（M2-1 8 + 链 1；`D458C1AE…` / `02FF8AFE…`；差分下限 2,000,000） |
| M2-3 等价门禁 | EXIT=0（3 符号 + 2 Entry；pre-M2-3 device.cpp 2,278,494 B / `ECC4B828…`；motion-on 差分 40,215,908） |
| M2-5 等价门禁 | EXIT=0（1 符号 / 34 taxonomy 字段；正文逐字节相同 18,937 B；pre-M2-5 2,273,048 B / `D8211864…`） |
| M2-4 等价门禁 | EXIT=0（8 符号 / 9 定义；pre-M2-4 2,254,826 B / `7391F307…`；电池 7,477,257） |
| M2-5B 等价门禁 | EXIT=0（1 符号 / 7 字段；正文逐字节相同 2,421 B；pre-M2-5B 2,250,786 B / `D0E80399…`） |
| A9 等价门禁 | EXIT=0（1 符号；相同 321 B；pre-A9 2,248,823 B / `FB4D2FF6…`；A9 电池 947,458） |
| meson test | `Ok: 84  Fail: 0`，EXIT=0（总数与上一块相同：本块**未新增 meson target**） |
| 全量静态 | `STATIC_TOTAL=256 STATIC_FAIL=0`，STATIC_EXIT=0（255 + 新增出口门禁 1） |
| **新** 出口静态门禁 | `Ran 14 tests … OK` EXIT=0 |
| **新** 出口定向测试 | `Ran 5 tests … OK` EXIT=0 |
| 域门禁（isolation static） | `Ran 13 tests … OK` EXIT=0 |
| 域离线模型 | `Ran 5 tests … OK` EXIT=0 |
| 宿主机域测试 | `SUMMARY: war3_shadow_geometry_domain_test 46/46 case(s) passed` EXIT=0 |
| stage13 retention lazy hash | `Ran 4 tests … OK` EXIT=0 |
| bridge/ramp shadow safety | `Ran 22 tests … OK` EXIT=0 |
| shadow lifecycle tombstone | `Ran 8 tests … OK` EXIT=0 |
| persistent expiry queue model | `Ran 2 tests … OK` EXIT=0 |
| 记录器（palette object evidence） | `SUMMARY: 23 passed, 0 failed` EXIT=0 |
| 成本 | `COST_VERDICT=PASS checks=38 failures=0` EXIT=0 |
| 生命周期 | `SUMMARY: war3_shadow_build_lifecycle_test 187/187 case(s) passed` EXIT=0 |
| 解析器静态 | `Ran 94 tests … OK` EXIT=0 |
| 根读方 | `Ran 55 tests … OK` EXIT=0 |
| 往返 | `ENCODER checks=73 failures=0 PASS` / `ROUNDTRIP checks=116 failures=0 PASS` / `ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`，EXIT=0 |

### 6.3 DLL 身份（最终树，Below Normal + `-j2`）

```
build32/src/d3d9/d3d9.dll|36283128|295C7FD25CB5549664963AB01A252BF423AD1791D9C004B9372BAB5B60CEFB1A|2026-09-18T06:15:39.0926249+08:00
build32/src/d3d9/war3_shadow_geometry_domain_test.exe|463611|674B663DFFC6A05E1A55D7C4FCDB79E12203158E527F04F516A24A2A969FB8A7|2026-09-18T06:15:37.7586109+08:00
```

对比：本块前最后已知 DLL 为 36,279,032 B /
`6D17B0382DEC628A1DF9A672BE0B207C1316F9DE0BB1C86A8C554FF0964DAB20`
（引用 `docs/plan/2026-09-18-registry-domain-isolation-record.md` §4.3，不是我实测）。
本块 **+4,096 B**（perf monitor 三处结构字段 + 累加 + 两个写手 + 逐帧序列 + 注释）。

### 6.4 post-doc 复核与写盘身份

见本文末尾「§9 post-doc 复核」段（记录/进度/CHANGELOG 写盘之后再取一次）。

---

## 7. 未覆盖项与已知缺口（如实记录）

1. **出口代码未被执行级测试覆盖**：没有任何宿主机测试真正调用
   `War3PerfMonitor::notePersistentGeometryFrame` 并读取其 JSON——该 TU 依赖 d3d9/DXVK 设备侧类型，
   不是宿主机可链接目标（本树所有 meson 宿主机测试都只包含轻量头）。因此：
   * 「字段被正确累加/打印」由**静态门禁 + 源码锚定定向模型**覆盖；
   * 「实机跑起来后 JSON 里真的有这些键且数值合理」**未验证**——需要一次实机/隔离桌面采集。
2. **F3 全长未执行**：没有 pre-U5 基线快照、没有启动游戏、没有部署，
   因此**不能**声称「不劣化」。本块只提供**能读出来**的手段。
3. **域维度分账（D4/D5）仍未做**：`m_war3ShadowPersistentBytesUsed` 仍是单池；
   expiry token 仍只有 `{lastSeenFrame, geometryId}`。所以「按 domain 分账的字节/条目」
   **没有**对应字段可出口——本块**没有**伪造这类字段。
4. **D1（key 级 domain）未做**：同 key 跨 domain 仍靠 owner-check 拒绝而不是天然分槽位。
5. ~~**未真跑的变异**（只由静态门禁的所属函数/恒等映射断言覆盖）：删掉 device.cpp 的 `++` 落点、
   删掉 bridge 传递行、把 per-frame 拷贝写成 `A = stats.B` 串线。~~
   **2026-09-18T06:39 已补齐（本缺口关闭）**：三条各真跑一次，见 §10 与 `docs/plan/2026-09-18-registry-domain-count-export-extra-mutations.log`：
   E1 删 `++` 落点（变异态 2,257,210 B / `03A88546…`）静态出口门禁 `0 != 1 : device.cpp 中 domainPublishRejects++ 的处数不是冻结值 1`、
   定向 `device.cpp 的累加站点/所属函数与冻结口径不符`；E2 删 bridge 行（2,257,185 B / `5173E985…`）静态门禁
   `bridge 传递缺失或重复：stats.domainGcEraseRejects = completed.domainGcEraseRejects;`；E3 per-frame 串线（625,321 B / `611F32C9…`）
   定向 `per-frame 未把 domainPublishRejects 恒等传出：domainLookupRejects`。三条都**能编译能链接**（NINJA_EXIT=0、DLL 确实重链），
   还原 SHA MATCH + touch 真实重编译 + 复绿；最终全门禁见 §10.4（全绿，DLL 36,283,128 B / `519AFA69…`）。
6. **`runtime_status.json` 不含本族**：见 §2.7。若上游要求把本族也放进 hub/control plane，
   那是新增通道，需要单独裁定与单独门禁。
7. **无实机门**：本块未部署、未启动游戏、未触碰 `E:\Work`；不声称任何视觉/性能/TDR 结论。

---

## 8. 未实施声明（不得引用为已完成）

* 本块**没有**宣称 Registry domain 隔离完成、**没有**宣称 Stage13 就绪、**没有**宣称稳定版；
* 本块**没有**改任何准入/发布/淘汰/退役语义，**没有**加 env，**没有**改 Stage13 准入默认值；
* 本块**没有** git 写、**没有**部署、**没有**启动游戏、**没有**触碰 `E:\Work`；
* 域维度的**字节/条目分账**与 **key 级 domain** 仍未做；F3 仍未执行。

## 9. post-doc 复核（记录/进度/CHANGELOG 写盘之后再取一次）

时间 **2026-09-18T06:19:09 → 06:20:08**（full-gate log §31）。这一步证明「最终树」不是取数后又被改动：

```
ninja -C build32 -n        -> ninja: no work to do.                       EXIT=0
full static suite          -> STATIC_TOTAL=256 STATIC_FAIL=0              STATIC_EXIT=0
count export static gate   -> Ran 14 tests, OK                            EXIT=0
count export directed test -> Ran 5 tests, OK                             EXIT=0
registry domain static gate-> Ran 13 tests, OK                            EXIT=0
war3_shadow_geometry_domain_test -> 46/46 case(s) passed                  HOST_EXIT=0
meson test                 -> Ok: 84  Fail: 0                            MESON_EXIT=0
build32/src/d3d9/d3d9.dll  -> 36,283,128 B /
                              295C7FD25CB5549664963AB01A252BF423AD1791D9C004B9372BAB5B60CEFB1A
                              （与 §6.3 完全一致：文档写入不改变二进制）
```

本轮写盘的文件身份（回读；本记录这一行取的是**填 §9 之前**的身份，填完 §9 后它会再变一次）：

| 文件 | bytes / SHA-256 |
| --- | --- |
| 本记录 | 28,160 / `50A347A5B2E0315AB87C8922C67CCEF94D25182ECC8D26F408D8BB6ED7401A1D`（填 §9 前） |
| `docs/plan/2026-09-18-registry-domain-count-export-mutation-raw-output.log` | 6,676 / `9E2EA910A5CBD45DA0E685C250B9614FB3E8469CB12E76417CBA46B6D08E561F` |
| `docs/plan/2026-09-18-registry-domain-count-export-full-gate.log`（含 §31） | 见该文件自身；§29–§31 追加后不再改动 |
| `docs/plan/2026-09-18-overnight-progress-log.md` | 72,227 / `DA79E0E73A13DA514CF9D4FBF6C273DA17DCA753DC0556C247D06EA0255F5957` |
| `docs/agent-history/DEVELOPMENT_CHANGELOG.md` | 328,946 / `6CA60D8CFFDA7FD9EB782B77696B64658787B859D74CB391F79BB2AB17E66544` |
| `AutoTest/test_registry_domain_count_export_static.py` | 14,700 / `7F4B39DAB663EAD12755C5BB7E0961CF25844FE5A359A4D386628EE65F5DF19F` |
| `AutoTest/test_registry_domain_count_export.py` | 12,867 / `C48902816D0575CD5E90BBA357F5649409C4FCFAE22416A2575F7A42B6F1E9A2` |

注：full-gate log 的 §0 身份表在真实重编译**之前**采集（mtime 之后被 touch 更新，**字节与 SHA-256 不变**），
§29/§31 的 DLL 身份在重编译之后采集，两者都对同一最终字节负责。

---

## 10. 证据补强（2026-09-18T06:25 → 06:39）：§7 第 5 条那三条变异各真跑一次

> 目的：把本记录自报「未真跑、只由静态门禁的所属函数/恒等映射断言覆盖」的三条变异补上。
> 原始输出：`docs/plan/2026-09-18-registry-domain-count-export-extra-mutations.log`。
> 纪律：只做变异，不留任何语义改动；四个被变异过的源文件与实现态备份逐字节一致（`RESIDUE_MISMATCHES=0`）；
> 未加 env、未 git 写、未部署、未启动游戏、未触碰 `E:\Work`、不称稳定版。

### 10.1 三条变异结果（每条：改 → 真实构建 → 门禁必须红 → 还原 SHA MATCH → touch 后真实重编译 → 复绿）

| # | 变异（确切落点） | 变异态源码 bytes / SHA-256 | 构建（Below Normal + `-j2`） | 红的断言原文 | 还原 / 复绿 |
| --- | --- | --- | --- | --- | --- |
| E1 | `d3d9_device.cpp` 删掉 `    diagnostics.domainPublishRejects++;`（`War3CreateShadowPersistentGeometryAfterMiss`，即 §1 表第 3 行的累加点） | 2,257,210 / `03A88546E940DF2E881795A759D52675C71C08BE9D7167E1CDFD22D3AD613AF5` | NINJA_EXIT=0（47.2 s，`[2/2] Linking target src/d3d9/d3d9.dll`；变异态 DLL 36,283,128 B / `4CBE849411220FFA4C5C8DBE2D410B7A54387EC8215AB92475920FF7FEF9777D`） | 静态出口门禁 `FAILED (failures=2)`：`AssertionError: 0 != 1 : device.cpp 中 domainPublishRejects++ 的处数不是冻结值 1`、`AssertionError: {} != {'War3CreateShadowPersistentGeometryAfterMiss': 1} : domainPublishRejects 的累加点所属函数与冻结口径不符：{}`；定向测试 `FAILED (failures=1)`：`AssertionError: … : device.cpp 的累加站点/所属函数与冻结口径不符`；域隔离静态门禁也红（failures=2，`'domainPublishRejects++' not found in …`） | SHA MATCH（2,257,251 B / `CC40687A…`）+ touch 真实重编译（47.1 s，DLL `2D49ED5A…`）+ 复绿 14 OK / 5 OK / 13 OK + `ninja -n` no work |
| E2 | 删掉 bridge 行 `    stats.domainGcEraseRejects = completed.domainGcEraseRejects;`（`d3d9_device.cpp:33674`，§2.3 六个恒等拷贝之一） | 2,257,185 / `5173E985ADA0451BE2C0CC952B82C8523C99E190098068D911592290AD567E60` | NINJA_EXIT=0（47.0 s；变异态 DLL `5A5688EC165148EAF39CE4794C688FB0AA937E9AEA23A4D6EF2EFA4CF8C2B0C6`） | 静态出口门禁 `FAILED (failures=1)`：`AssertionError: 0 != 1 : bridge 传递缺失或重复：stats.domainGcEraseRejects = completed.domainGcEraseRejects;`；定向测试 `FAILED (failures=3)`：`AssertionError: bridge 未把 domainGcEraseRejects 恒等传出：None`、`AssertionError: Items in the second set but not the first: 'domainGcEraseRejects' : bridge 拷贝集合不完整`；**域隔离静态门禁保持绿（EXIT=0）** | SHA MATCH（`CC40687A…`）+ touch 真实重编译（47.3 s，DLL `C52F66E6…`）+ 复绿 + no work |
| E3 | `war3_perf_monitor.cpp` per-frame 串线：`  m_currentFrameWorkload.domainPublishRejects = stats.domainPublishRejects;` → `… = stats.domainLookupRejects;`（:2441，§2.5） | 625,321 / `611F32C947EEF9BACB3410DE5CA79BA5BF21BAC718FC62F160D10F6243F66A43` | NINJA_EXIT=0（30.0 s；变异态 DLL `4FC34E8187F7767122A61F296B40AA6732E303A9981B16882903A13B58F2ADA3`） | 静态出口门禁 `FAILED (failures=1)`：`AssertionError: 0 != 1 : domainPublishRejects`；定向测试 `FAILED (failures=3)`：`AssertionError: per-frame 未把 domainPublishRejects 恒等传出：domainLookupRejects`、`AssertionError: 'domainLookupRejects' != 'domainPublishRejects'`；**域隔离静态门禁保持绿** | SHA MATCH（625,322 B / `7E4ED64A…`）+ touch 真实重编译（29.7 s，DLL `52DEDA9A…`）+ 复绿 + no work |

### 10.2 「可编译但被门禁咬住」成立（§7 第 5 条的判据）

* 三条的 NINJA_EXIT 都是 0，且**本树的** `build32/src/d3d9/d3d9.dll` 在变异态被真实重链（字节数不变 36,283,128 B，mtime 与 SHA 都变）：
  说明删 `++` 落点 / 删 bridge 行 / per-frame 串线**不会**被编译器或链接器抓住，只有出口静态门禁 + 定向测试会点名
  （符合 m2-5 §2.3 失败模式 5）。
* E1 与 E2 在域隔离静态门禁上的行为不同：E1 连它一起红（该门禁 assert `domainPublishRejects++` 字符串存在），E2 它保持绿
  ⇒ 两条门禁的覆盖边界不同，**不是**对同一件事的重复断言。
* E3 改的是字段配对：静态门禁按精确恒等文本点名（`0 != 1 : domainPublishRejects`），定向测试按恒等映射点名
  （`'domainLookupRejects' != 'domainPublishRejects'`），两者在同一字段上变红。

### 10.3 一次无效尝试与更正（如实记录，不计入上表）

* 第 1 次 E1 运行的**构建段无效**：pwsh 后台作业的默认工作目录是会话工作区
  `E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk`（另一棵 dxvk 树），而 PowerShell 给原生命令子进程用的是
  它自己的 location（不是脚本里 `[System.IO.Directory]::SetCurrentDirectory()` 改的 `Environment.CurrentDirectory`）；
  原 wrapper `%TEMP%\run_ninja_domain_count.cmd` 里的 `-C build32` 是相对路径，于是那次 50.5 s 的编译/链接落在**那棵树的 build32**，
  本树的 `d3d9_device.cpp.obj` / `d3d9.dll` / `.ninja_log` 时间戳仍是 06:14:23 / 06:15:39
  ⇒ 该次的「变异态 DLL 身份 + NINJA_EXIT=0」**不能**作为「E1 可编译」的证据，日志里读到的 33 行 ninja log 也是旧文件。
* 更正：新增 `%TEMP%\run_ninja_dc2.cmd`（同样 Below Normal + `-j2`，但 `-C` 用**绝对路径**指向本树 `build32`），三条各重跑一次；
  §10.1 与 §10.4 全部取自重跑。
* 副作用如实登记：那次无效构建只重编/重链了 `…\Graphics\dxvk` 那棵树自己的 DLL；**本 agent 未触碰该树任何源码**
  （也未触碰 `E:\Work`、未部署、未启动游戏）。
* 本树在重跑之前用正确 cwd 补做过一次真实重编译（06:28:29，DLL `72973953…`）并回到 `ninja -n: no work to do`。
* 编码更正：第 1 次捕获的门禁输出里中文断言被错误解码（U+FFFD）；E1 的变异源随后以**同一 SHA**（`03A88546…`）重放了一次
  **只读门禁、不重新构建**，以取回断言原文（日志中标题为 `E1 gate-text re-capture`）。

### 10.4 最终树复核（touch 后真实重编译 + 全门禁，2026-09-18T06:35:30 → 06:39:13）

| 门禁 | 结果 |
| --- | --- |
| 残留检查（4 个被变异源 vs 实现态备份） | `RESIDUE_MISMATCHES=0`（逐个 `IDENTICAL=True`） |
| `ninja -C build32 -n`（前） | `no work to do` EXIT=0 |
| touch 4 个源 + 真实重编译（Below Normal + `-j2`） | NINJA_EXIT=0，elapsed 129 s |
| `ninja -C build32 -n`（后） | `no work to do` EXIT=0 |
| 预算门禁 | EXIT=0（冻结 157；站点 165→112；已迁出 46；device.cpp 567） |
| 七条等价门禁（M1 / M2 / M2-3 / M2-5 / M2-4 / M2-5B / A9） | EXIT=0 ×7 |
| meson test | `Ok: 84   Fail: 0` EXIT=0 |
| 全量静态（`AutoTest/test_*_static.py`） | `STATIC_TOTAL=256 STATIC_FAIL=0` |
| 宿主域测试 | `46/46 case(s) passed` EXIT=0 |
| 新出口静态门禁 / 新定向测试 | `Ran 14 tests … OK` / `Ran 5 tests … OK` |
| 域隔离静态 / 域离线模型 | 13 OK / 5 OK |
| stage13 retention / bridge-ramp / tombstone / expiry | 4 OK / 22 OK / 8 OK / 2 OK |
| 记录器 | `SUMMARY: 23 passed, 0 failed` |
| 成本 | `COST_VERDICT=PASS checks=38 failures=0` |
| 生命周期 | `187/187 case(s) passed` |
| 解析器 / 根读方 / 往返 | 94 OK / 55 OK / `ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`（`CHECKS=1107 FAILURES=0`，六场景 `SPEC_SATISFIED=['A'..'F']`） |
| `ninja -C build32 -n`（最终） | `no work to do` EXIT=0 |

最终 DLL 身份（本树最后一次真实构建）：

```
build32/src/d3d9/d3d9.dll|36283128|519AFA6904F47E9C2E168FF0E5E0A17A470B27F9F9E714783D99C0333E397306|2026-09-18T06:37:39.4998254+08:00
build32/src/d3d9/war3_shadow_geometry_domain_test.exe|463611|765FCC82BAE43A6B11C289AEA633848D3423C88FB56A7DDA175272B1A38BB2E3|2026-09-18T06:37:37.9621187+08:00
```

**DLL SHA 说明（不得误读）**：字节数与本记录 §6.3 相同（36,283,128 B），但 SHA-256 不同 —— 这**不是语义差异**：
同一份源码两次真实重链接本就不逐字节复现：PE 头 `TimeDateStamp` 等于链接时刻（实测 `1789684659` = 2026-09-18T06:37:39+08:00），
且 `war3_perf_monitor.cpp:3839` 使用 `__DATE__/__TIME__` 构建戳。因此「DLL SHA 与上一块 §6.3 相同」不是本树的验收条件；
本树的一致性由「4 个源与实现态备份逐字节相同 + `ninja -n` no work + 全门禁复绿」证明。

### 10.5 仍未覆盖（不因本次补强而消失）

* 出口代码**仍未被可执行级测试覆盖**（`War3PerfMonitor` 非宿主机可链接目标）⇒「实机 JSON 里确有这些键且数值合理」仍未验证；§7 第 1/2/3/4/6/7 条不变。
* 本次补强只关闭 §7 第 5 条（三条未真跑的变异）；没有新增任何实机数据、没有部署、没有晋升稳定版。

