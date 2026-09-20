> ⚠️ **历史文档 —— 已过期（2026-09-18 round 79 标注）**
>
> 本文描述的是**当时**的现场与计划。**当前权威事实**见 `2026-09-18-objective-evidence-ledger.md`：
>
> ```
> 站点 E:\Work\Warcraft III\d3d9.dll = 36,288,789 B / F275545BAA65A015…5CF07FF3 = 基线 ⇒ **未部署**
> 写方 = **恒定 paletteObject version=4**（已删除 firstSightUsed() 动态选版本）
> 实机采集 = **未获授权、零采集**（协议见 2026-09-18-stageE-live-capture-protocol-v4.md）
> 门禁 = 静态 263/0（**不含**门禁外 40 个文件里的 3 个既有红）
> 五项裁定待决（窗口维度导出层 / 双链路由 / 实机授权 / 构建可复现 / 门禁外三红与覆盖缺口）
> ```
>
> **若本文与上述冲突，以上述为准。** 尤其是：本文若说"现场 DLL 已部署/勿动"，
> 那是**当时**的状态；**今天站点是基线、未部署**。
>
> ---
>
# pre-U5 基线采集协议（2026-09-18 主线程制定）

> 背景：⑤ 的 F3（「不劣化」判据）**至今无法执行**，根因不是缺工具，而是**本树没有已登记的 pre-U5 基线快照**。本文件把「怎么采、采什么、什么算通过、什么算反例」固定下来，供实机窗口一到即可执行。
> 性质：**计划文档**。本夜未启动游戏、未部署、未采集任何实机数据。

## 0. 前置（缺一不可）

1. **能进图**：见 `2026-09-18-overnight-summary.md` 的 ① 判定 —— 需 (A) 人工进图后按一次 `Ctrl+Shift+C`，或 (B) 授权供给沙箱后走 `launch_war3_instance`。**未进图 ⇒ `InitializeRuntimeCore()` 不跑 ⇒ 控制面不存在**。
2. **子门开启**：`DXVK_WAR3_FRAME_EVIDENCE=1` 且 `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1`（注意：经 YDWE 启动器时环境变量可能被丢弃，需在游戏进程侧确认，见进度日志 2026-09-17 的 20:12 案例）。
3. **同源可比**：基线与被比较的候选必须是**同一份代码路径**的不同修订，且两次采集的**地图、巡航时长、视角、观测窗口**一致。

## 1. 采集什么（字段清单）

### 1.1 域维度拒绝计数（本夜新接出口，6 个）

在 perf monitor 的 `war3_perf_report.html` 内嵌 JSON 中读取（**注**：`runtime_status.json` 实测 0 命中本族，不要在那找）：

| 聚合字段 | 含义 | 期望（稳态） |
| --- | --- | --- |
| `persistentRejectDomainConflict` | publish 时域冲突（ShadowCapture 分桶，**唯一**进 `persistentRejectCreateOrBudgetDetailedTotal` 的域计数） | **0** |
| `persistentDomainLookupRejects` | lookup 命中但域不符（不改槽位/不刷新 lastSeen） | **0** |
| `persistentDomainPublishRejects` | 创建入口域拒绝 | **0** |
| `persistentDomainGcEraseRejects` | GC 擦除时的归属拒绝 | **0** |
| `persistentDomainResetPurgeRejects` | 两处 Stage13 域作用域清理的保留计数 | **0** |
| `persistentDomainResetOwnerRejects` | reset move 前归属校验丢弃的「无主别名」 | **0** |

> **F3 的第一判据即「这 6 个在正常场景应恒为 0」**：任何非 0 都意味着**域越权尝试真实发生**，必须先解释来源，才谈性能。

### 1.2 既有护栏与账目（对照用，来自 M2-5 清册的既有 44 条族）

- `budgetExceeded` / `m_war3ShadowFallbackBudgetExceeded`（**不得上升**）；
- cap 拒绝类（**不得激增**，需给出与基线的比值）；
- `m_war3ShadowPersistentBytesUsed` 与相应 evicted/bytes 账（**不得单调增长**，且退役可观察）。

## 2. 两段采样协议

| 段 | 场景 | 时长 | 目的 |
| --- | --- | --- | --- |
| **S-A 稳态** | 进入地图后静置巡航（无大规模战斗） | ≥ 3 分钟 | 取 6 个域计数的**零基线**与账目底数 |
| **S-B 高压** | 高压力光影场景（如 `(4)生与死v1.28读档bug修复.w3x` 的战斗波次） | ≥ 3 分钟 | 观察域计数是否仍为 0、护栏是否上升、账目是否收敛 |

**每段结束**：`frame_evidence_control.py --pid <PID> trigger --post-presents 8` → `status` → `export`；两次采样**分别留存**，文件名带段号与时间戳。

## 3. 判定（只允许三种措辞）

| 结论 | 条件 |
| --- | --- |
| **有证据（通过）** | 6 个域计数在两段中**恒为 0**；`budgetExceeded` 不上升；cap 拒绝不高于基线比值阈值；字节账收敛且退役可观察 |
| **仅趋势** | 只有单段数据，或窗口过短（< 3 分钟），或地图/时长与基线不一致 ⇒ **不得写成通过** |
| **未覆盖** | 未进图、无管道、无导出、字段缺失 ⇒ 如实记为未覆盖 |

## 4. 反例门（必须显式声明）

- 任一域计数 **> 0** ⇒ 判为**反例**，必须定位到具体路径（lookup/publish/GC/purge/reset）并解释；**不得**用「数字很小」搪塞。
- 域计数为 0 **但** 护栏上升 ⇒ 判为**不劣化未通过**（域隔离与性能是两件事，需分别结论）。
- **禁止外推**：CPU 侧 JSON 只能证明**记录与计数**，**不证明** GPU 提交、不证明像素/画面正确性、不证明阴影恢复。

## 5. 登记方式

1. 每次采集产出：`cpu-*.json`（导出）+ `war3_perf_report.html` 片段 + 采集元数据（进程 PID、DLL SHA-256、地图、时长、段号、时间戳）；
2. 基线快照落到 `docs/plan/2026-09-18-pre-u5-baseline-record.md`，附**文件 SHA-256**；
3. 被比较的候选 DLL 必须登记 `build32/src/d3d9/d3d9.dll` 的**字节 + SHA-256**，并注明「最终树最后一次真实构建」（**同源码重链接不逐字节复现**，见总结 §3 结论 2）。

## 6. 与 F3 判据的对应

- F3-7（`budgetExceeded` 不升）⇒ §1.2 第 1 项；
- F3-8（cap 拒绝不激增）⇒ §1.2 第 2 项；
- F3-9（字节账非单调增长且可观察退役）⇒ §1.2 第 3 项；
- 另加本夜新增的**域零基线**判据（§1.1）。

## 7. 执行时的纪律提醒

实机窗口内**冻结源码树**；Below Normal + `-j2` 且全机单构建；**不替换现场 DLL**（`A0A51AF2…`）；不 git 写；不称稳定版；隔离桌面数据**不得**当作玩家前台性能。
