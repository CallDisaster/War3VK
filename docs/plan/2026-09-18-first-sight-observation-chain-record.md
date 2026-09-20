# 批次 3：有版本的「正常观察链」（FirstSight）实施记录 — 2026-09-18

> 依据：外部独立复审批次 3 建议。**未提交 / 未部署 / 未晋升稳定。**

## 1. 复审要求 → 本实现如何满足

| 复审要求 | 实现 |
| --- | --- |
| 保留「拒绝后恢复」专用链 | `NoteReject → NoteServed → NoteEnqueued → NoteDrawn` **一字未改**；Case24 正面见证其终态仍为 `Recovered`、终态阶段仍为 `Drawn` |
| **增加**可抽样的正常观察链 | 新增阶段 `FirstSight=5` 与唯一入口 `NoteFirstSight()`；链形 = `FirstSight → Enqueued → 终态` |
| **有版本**的协议调整，旧版本含义不变 | 块版本由 `firstSightUsed() ? 3 : 2` 决定；未使用首见链的导出仍是版本 2，读方一位不变 |
| 写方 / 解析器 / 测试**同步**更新 | 写方：`war3_palette_object_evidence.h` + `war3_frame_evidence.cpp` + `d3d9_device.cpp`；读方：`analyze_palette_object_evidence.py`；测试：Case24 + 新静态门禁 |
| **不伪造 Rejected** | `NoteFirstSight` 不写 `Rejected` 阶段、不宣告 `TableFull` 终态、拒绝原因记 `NotChecked`（静态门禁逐条钉死） |
| 不让 `NoteEnqueued` 自动插表后放宽解析器 | **未**改动 `NoteEnqueued` 的插入行为；解析器新增的是**约束**（旧版本出现首见阶段必须具名判错），不是放宽 |
| 不把未知身份升级为已证明 | 首见事件身份证明种类仍由唯一推导规则 `DeriveIdentityProofKind()` 产生；Case24 断言生产形状键恒为 `NoIdentityProof(0)`，且 `identityWeak`/`epochUnknown` 标记**原样携带** |

## 2. 关键设计决定（含一处必须处理的语义缺陷）

### 2.1 `FirstSight` 取值 5 是**故意**的，顺序另用语义秩

- 取值 5 让版本 1/2 读方的阶段表无法解释它 ⇒ 读方**明确拒绝**整份导出，而不是静默忽略（fail-visible）；
- 但 `MarkStage` 原先按**枚举数值**判顺序，首见之后的 `ServedCandidate/Enqueued/Drawn`（2/3/4）都会小于 `maxStage(5)` 而被**误判 `orderViolation`**；
- ⇒ 引入 `StageRank()`（FirstSight=0, Rejected=1, ServedCandidate=2, Enqueued=3, Drawn=4），`MarkStage` 改用它。

### 2.2 发现并修正：终态记录原先**无条件**声称 `stage=Drawn`

`CloseWindow()` 里 `record.stage = PaletteObjectStage::Drawn;` 对只推进到入队的首见链，等于把「候选已入队」谎报成「绘制命令已记录」。

修正为：`record.stage = e.sawFirstSight ? StageOfRank(e.maxStage) : PaletteObjectStage::Drawn;`
—— **只对首见链**按实际最高秩回填，拒绝恢复链保持冻结形状（遵守「旧版本含义不变」）。

### 2.3 首见链的 `firstRejectFrame` 槽位

该槽位（words32[30..31]）在版本 3 里承载**链首帧**（= 首见帧）；首见链**不存在**拒绝帧。`deltaFrames` 因此 = 本帧 − 链首帧，含义自洽。字段名未改（改会破坏 v1/v2 wire）。

## 3. 改动清单

| 文件 | 改动 |
| --- | --- |
| `src/d3d9/war3/tools/war3_palette_object_evidence.h` | 阶段 `FirstSight=5`；`StageRank()` / `StageOfRank()`；`MarkStage` 改用秩；`Entry::lastFirstSightFrame`/`sawFirstSight`；`Counters::firstSightInserted`/`firstSightEmitted`；`NoteFirstSight()`；`firstSightUsed()`；`CloseWindow` 终态阶段分支 |
| `src/d3d9/war3/tools/war3_frame_evidence.cpp` | `version` 由 `firstSightUsed() ? 3 : 2` 决定；计数器暴露 `firstSightInserted`/`firstSightEmitted` |
| `src/d3d9/d3d9_device.cpp` | 生产采集点由 `NoteServed` 改为 `NoteFirstSight`（本处无 `selectedPalette`，发 ServedCandidate 是冒充） |
| `AutoTest/analyze_palette_object_evidence.py` | `PALETTE_OBJECT_FIRST_SIGHT_VERSION=3`；`STAGES` 增 `5:'FirstSight'`；`i_firstsight` 与链首规则；`firstSightStageUnderLegacyVersion`（旧版本 fail-visible）；`bothRejectedAndFirstSightHead`（两链互斥）；`servedCandidateBeforeFirstSight`；缺 `Rejected` 在首见链下不再判截断 |
| `src/d3d9/war3/render/tests/war3_palette_object_evidence_test.cpp` | **Case24**：两类链最小集成见证（25 项断言） |
| `AutoTest/test_first_sight_observation_chain_static.py` | 新增静态协议门禁：写方+读方+测试三方一致性 |

## 4. 验证（全量重建后、用最新二进制）

`ninja -C build32` 全量重建完成，`ninja -C build32 -n` = **no work**。

| 门禁 | 结果 |
| --- | --- |
| `war3_palette_object_evidence_test`（含 Case24） | **24 passed, 0 failed** |
| `war3_palette_object_wire_roundtrip_test` | `checks=116 failures=0 PASS` |
| `war3_palette_object_evidence_cost_test` | all checks passed |
| `war3_frame_evidence_runtime_test` | `checks=723 PASS` |
| `war3_frame_recorder_memory_control_test` | `87 PASS` |
| `war3_shadow_build_lifecycle_test` | `187/187` |
| `war3_shadow_geometry_domain_test` | `46/46` |
| `test_first_sight_observation_chain_static.py` | PASS（写方+读方+测试同步） |
| `test_palette_object_evidence_analysis_static.py` | **OK**（读方契约在 v3 改动后仍全通过 ⇒ v1/v2 语义未破坏） |
| 其余 palette 静态门禁 ×3 | PASS |

### Case24 断言的正面见证

- `FirstSight` **确实建立条目**（`watchCount` 0→1、`firstSightInserted=1`）——这正是复审所指「记录器回答不了正常对象走了什么路径」的修复点；
- 链首 `FirstSight`、`rejectReason=NotChecked`、非终态；
- 生产形状键：`identityProofKind==0` 且 `identityWeak`/`epochUnknown` **原样携带**、**未**被发明 `lifecycleIdentity`；
- 已认证键正向对照：`identityProofKind==1` ⇒ 首见**不降级**真证明；
- 首见链终态 = `WindowExpired`、**不是** `Recovered`，且终态**不谎报** `Drawn`；
- 恢复链：终态 `Recovered`、阶段 `Drawn`（冻结形状）；只跑恢复链的窗口 `firstSightUsed()==false` ⇒ 版本仍为 2。

## 5. 一次自查纠正（诚实记录）

写入解析器时，`if i_firstsight is not None and version<...` 一行**漏了 4 个空格缩进**（落在列 0），使 `judge_chain` 提前结束、其 `return` 变成模块级 ⇒ `SyntaxError: 'return' outside function`。
**为什么没被早发现**：我先用 `ast.parse` 做了语法检查，它**不执行**符号表检查，检测不到「函数外的 return」。已改用 `compile()` 复验，并修复缩进。
⇒ 教训：语法检查要用 `compile()`；`ast.parse` 通过**不等于**可编译。

## 6. 未完成 / 边界

- **全门禁未复跑**（全量静态 / meson 全量 / Win32 runnable 全量 / TDR / ABBA / 玩家前台门）；
- **批次 4 未开始**：尚未用**真实蒙皮选择入口**做正常/异常对照，也尚未把决定权迁出 `D3D9Ex`；
- 首见链目前是**记录器与宿主集成测试**级别的见证；**尚未在实机产生含 FirstSight 的非零导出**（这属于批次 4/实机步骤）；
- 未提交、未部署；现场 DLL 仍是 `F275545B…`（`519AFA69…` 备份在位）；未改用户视频设置。