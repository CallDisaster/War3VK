# 2026-09-16 — Stage13 常驻几何的前置验证清单（占用与回收）

> 用户裁定：**Stage13 常驻几何涉及资源保留，必须检查占用与回收，
> 不得未经验证就认定它能改善当前的累积超预算现象。**
> 本文把该约束变成可判定的验证条款；**它不是"Stage13 已批准"**。

## 1. 背景（为什么必须前置）

- P2 批次 6 计划移植 A 88089cd 的 Stage13 content-persistent geometry：
  miss 时用精确 referenced-vertex 内容快照创建 **registry-owned 非索引常驻几何**，
  再入视野按内容 key + 私有字段/字节 proof 命中实例路径。
- 该能力**主动延长资源生命周期**（A 还把 persistent idle 默认 240→3600 帧），
  因此它可能同时影响：shadow persistent 字节账、Arena 检疫/回收、以及"累积超预算"。
- **B 侧的 240→3600 变更不得照抄**：B 的 S1 地形 early cache GC 也走同一 maxAge，
  照抄会让地形常驻多活 15 倍帧（P2 审计风险点 8）。若确需，应拆独立 env。

## 2. 现成的度量面（主线程读码核实，均为 B 现状）

### 2.1 Shadow persistent 字节账与准入
| 项 | 位置 | 说明 |
| --- | --- | --- |
| 池上限 | `d3d9_device.cpp:1147 War3GetShadowPersistentPoolCapBytes()` | 由 `DXVK_WAR3_SHADOW_PERSISTENT_MB`，**默认 512** |
| 空闲年龄 | `d3d9_device.cpp:1154 War3GetShadowPersistentMaxAgeFrames()` | GC 依据 |
| 字节账 | `d3d9_device.h:2980 m_war3ShadowPersistentBytesUsed` | 增加 21926；减少 21698/21716 |
| 准入检查 | `d3d9_device.cpp:21820-21823` | `bytesUsed <= capBytes - bytesNeeded` |
| GC 点 | `d3d9_device.cpp:21550 / 21576 / 21658` | 按 maxAge 退役 |
| 报告口径 | `d3d9_device.cpp:20109-20117` | cap 与 used 已进报告 |
| capacity 拒绝 | `d3d9_device.h:2402 capacityRejectAllCallers`；`war3_perf_monitor.h:218/311` | 池/容量拒绝 |

### 2.2 累积超预算指标（判定的核心对照）
- `d3d9_war3_scene.h:1857 budgetExceeded`
- `war3_perf_monitor.h:651 framesBudgetExceeded`

### 2.3 Arena 检疫与提交
- `war3_diagnostics_hub.h:138 arenaQuarantineCount`、`:139 arenaQuarantinedRetireSerial`、`:143 arenaCommittedBytes`
- `war3_diagnostics_hub.h:835 shadowArenaQuarantineCount`、`:841 shadowArenaCommittedBytes`
- `d3d9_device.h:1875 m_war3ShadowArenaQuarantinedRetireSerial`

### 2.4 proof 预算（Stage13 会新增 CPU 侧证明字节）
- `d3d9_device.h:1870 m_war3PersistentPackageCaptureProofBudgetRejected`
- `war3_diagnostics_hub.h:784/793 persistentPackage{CurrentDraw,Capture}ProofBudgetRejected`
- A 版 Stage13 另设 `kWar3Stage13PersistentProofCapBytes = 64 MiB`（B 尚未移植，移植时须同时导出计量）

## 3. 验证清单（**全部满足**才可宣称"改善累积超预算"）

短时（单场景高压）：
1. `m_war3ShadowPersistentBytesUsed` 峰值 ≤ cap，且 **cap 准入拒绝数不激增**。
2. Stage13 新增保留后，命中路径不得造成同帧重复创建（create 计数与 hit 计数须可解释）。

长时（持续巡航）：
3. 稳态下 `m_war3ShadowPersistentBytesUsed` **不单调增长**；必须观察到实际退役（减少点被触发）。
4. `budgetExceeded` / `framesBudgetExceeded` 相对基线**不上升**。
5. CPU proof 字节 ≤ 其上限，且随 GC/换图归零（不得只增不减）。

对照与反例：
6. 必须给出**修复前基线**的同场景同指标快照；不得只给"命中率提升"。
7. 若 bytesUsed 单调上升、或 cap 拒绝增加、或超预算指标上升 → **不得宣称改善**，
   按纪律如实记录为"未验证/劣化"，并保留数据。

## 4. 与其它工作的关系

- 与 P0（palette 路由式修复）**解耦**：P0 是眼前发布问题，优先。
- 与本文档第 3 节 `docs/plan/2026-09-16-unified-data-selection-entry-design.md` 的 S4 有交叉：
  Stage13 常驻几何若依赖 legacy arena 读取，则 S4 的"默认关"会影响它——移植前必须澄清。
- P2 批次 4（registry domain 隔离）是批次 6 的**硬前置**（否则 S1 与 Stage13 共桶互吃）。

## 5. 明确不做

- 本文档不批准 Stage13 进入实施；不含任何代码改动。
- 不得用文档存在本身当作"已满足前置"。
- 不得在未取得占用/回收数据前把 Stage13 与"超预算改善"绑定宣传。
