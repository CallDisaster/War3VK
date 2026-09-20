# D4/D5 只读分账实施规格（2026-09-18 主线程制定）

> 目的：把出站交接 §2 第 2 条（「D4/D5 只读分账」）变成可直接实施的规格，避免下一班重新摸索范围与红线。
> 性质：**计划文档**。本夜未实施；树保持冻结。

## 0. D4/D5 现状（来自 T7 勘察，实测确认）

- **D4 字节账/cap**：`m_war3ShadowPersistentBytesUsed` 仍**单池**，cap 判定未改 ⇒ **无法按 domain 读出占用**。
- **D5 expiry 队列**：token 仍为 `{lastSeenFrame, geometryId}`（无 domain），GC 仍**全局按 age** ⇒ **无法按 domain 读出待淘汰量**。
- 两者今晚都**没有**任何可出口的域维度字段 —— 出口块**如实未伪造**（见 `2026-09-18-registry-domain-count-export-record.md` §7）。

## 1. 本轮红线（先写死，防止顺手改策略）

1. **不改 cap 判定**：总字节上限语义、触发时机、拒绝后的回退路径一律不变；
2. **不改 age/expiry 策略**：淘汰顺序与阈值不变，仍是全局按 age；
3. **不改退役（retirement）语义**：Present 整会话退役仍整体 move（GPU fence 所有权不可拆）；
4. **只加"可分别读出"**：新增的是**观测字段**（per-domain 计数/字节累加），**不是**新的准入或淘汰规则；
5. **不加 env**、不改 Stage13 准入默认值。

## 2. 要做的最小集合

### 2.1 D4：按 domain 的字节与条目读数（只读）

在既有的 per-frame 诊断结构里新增（命名与既有 44 条族一致，聚合字段用 `persistent*` 前缀）：

| 建议字段 | 含义 | 累加位置 |
| --- | --- | --- |
| `persistentBytesUsedByDomain`（按 domain 的 3 元组：Generic / S1Terrain / Stage13Exact） | 各 domain 当前占用字节 | 每次增/减账处**旁路只读累加**（不替代原单池账） |
| `persistentEntryCountByDomain`（3 元组） | 各 domain 条目数 | 插入/擦除处旁路累加 |

**守恒断言（必须做）**：三域之和 **==** 原单池 `m_war3ShadowPersistentBytesUsed`（同帧、同一时刻取样）——这是本块的**核心判据**，任何不等价都必须判红。

### 2.2 D5：按 domain 的 expiry 读数（只读）

| 建议字段 | 含义 |
| --- | --- |
| `persistentExpiryQueueSizeByDomain`（3 元组） | 各 domain 在过期队列中的条目数 |
| `persistentExpiryAgeOldestByDomain`（3 元组） | 各 domain 最老条目的 age |

**注意**：token **不必**加 domain 字段（那会动 D5 的结构）；可以**在只读侧按条目当前 domain 归类**统计。若判定结构上无法在不改 token 的前提下归类，**如实报告该字段不可出口**，不要硬改 token。

## 3. 出口接线（照本夜既有路径，不自创通道）

1. per-frame 结构 → Present 安全点 `std::exchange` 取区间 → **bridge**（`d3d9_device.cpp` 的 `stats.X = completed.X` 恒等段）→ perf monitor（`FrameWorkloadSnapshot` / `PersistentGeometryFrameStats` / `ShadowBudgetAggregate`）→ **两个 JSON 写手**（`shadowBudgetSummary` / `shadowRuntimeV2Summary`，各 1 键 1 值）+ `workloadSeriesColumns` 列与行；
2. **不要**接到 `runtime_status.json`（本族实测 0 命中，既有等价出口是 `war3_perf_report.html` 内嵌 JSON）。

## 4. 证据要求（照本夜范式，缺一不可）

1. **provenance**：逐文件 pre 快照 SHA-256 + 字节 + 捕获时间 + `%TEMP%` 备份；改动可反向逐字节重建；
2. **静态出口门禁**：逐字段「允许处数」断言（定义 1 / `device.cpp` 重复定义 0 / 累加点按路径冻结所属函数 / bridge 1 / perf_monitor 三处 / 两写手各 1 键 1 值 / 列 1 行 1 / 不加 env 冻结 `DXVK_WAR3_` 字面量数）；
3. **定向测试**：① **守恒断言**（三域字节之和 == 单池）；② 稳态下各域读数与人工构造场景一致；③ 无越权时域拒绝计数仍恒 0；
4. **≥2 条变异真跑**（改→构建→跑→红→还原 SHA MATCH→`touch` 真实重编译→复绿）：① 把某域字节累加删掉 ⇒ 守恒断红；② 把某 domain 归类写成恒定值（例如都算 Generic）⇒ 守恒或定向断红；
5. **全门禁在最终树上取数**：`ninja -n` no work、构建 exit 0、预算门禁（冻结/已迁出/站点）、**七条等价门禁**、域隔离门禁、出口门禁、meson、全量静态、记录器 23/23、成本 PASS(38/38)、生命周期 187/187、解析器、根读方、往返 CERTIFIED；DLL 字节 + SHA-256（登记"最终树最后一次真实构建"）。

## 5. 完成后能解锁什么

- `2026-09-18-pre-u5-baseline-collection-protocol.md` §1.2 第 3 项（字节账"非单调增长且可观察退役"）**首次可测**；
- F3-9 判据可执行；
- 同时为 **D1 形态 (b)+D7 合并**（出站交接 §2 第 3 条）提供分账基线，避免"key 隔离了但账仍单池"的中间态。

## 6. 与 D1 的关系（重申建议）

建议顺序：**D4/D5（本规格）→ 再 (b)+D7**。理由见 `2026-09-18-d1-domain-form-decision-brief.md` §4。
