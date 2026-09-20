# 🔴 对抗性审计结论：我 ③ 的结论有 3 项被证伪 — 2026-09-18

> 由独立子线程（只读、未改文件、用仓库自带读方跑三类材料）出具。
> 我把它当作**对我此前"③ 实现完成"主张的正式推翻**，并原样记录，不做辩解。

## 1. 七项硬约束的审计裁决

| # | 约束 | 裁决 |
| --- | --- | --- |
| 1 | 有版本化（写方发 3、两读方注册并接受） | ✅ **成立** |
| 2 | 正常观察链存在（FirstSight→Enqueued→Drawn→WindowExpired） | ⚠️ 阶段表与可达路径**成立**，但**"首次"语义不成立** |
| 3 | **保留既有拒绝恢复链（v1/v2 语义未变）** | ❌ **不成立** |
| 4 | 不伪造 Rejected | ✅ 成立（生产路径）；有 1 个生产不可达的代码反例 |
| 5 | **不让 NoteEnqueued 自动插表后再放宽解析器** | ❌ **后半句被证伪**（前半句成立） |
| 6 | 不把未知身份升级为已证明 | ✅ 成立 |
| 7 | **测试同步（三方断言一致、无"改宽迁就实现"）** | ❌ **不成立** |

⇒ **7 项里 3 项不成立、1 项部分不成立。** 我此前"③ 实现完成"的说法是**过度声称**。

## 2. 第 3 项为什么不成立（最严重，正是复审明令禁止的）

`AutoTest/analyze_palette_object_evidence.py:289`：

```python
STAGES = {1:'Rejected', 2:'ServedCandidate', 3:'Enqueued', 4:'Drawn', 5:'FirstSight'}
#               ^^^ 这是**版本无关**的
```

改动前阶段表里**没有** 5 ⇒ 版本 1/2 的导出若含 stage=5，`require(value in table)` 会**整份拒绝**；
改动后读方**认识**这个值 ⇒ 同一个载荷变成"接受 + 软标记"。

**复现**（审计者探针 P4）：把只含 FirstSight/Enqueued/终态的链标成 `format_version=2` ⇒
读方**接受**、`coverageComplete=true`，只追加软性 `orderIssues=["firstSightStageUnderLegacyVersion"]`。

**而写方自己的设计注释声称相反**（`war3_palette_object_evidence.h:58-62`）：
> "版本 1/2 读方的阶段表不含它，因而会直接拒绝整份导出（fail-visible）"

⇒ 写方注释描述的防线**在读方并不存在**。fenced 的方式变成了"我记得加个软标记"。

## 3. 第 5 项后半句为什么被证伪

`analyze_palette_object_evidence.py:485`：

```python
if i_reject is None and i_firstsight is None:
    truncation.append('rejectedStageMissingFromStream')
```

审计者指出：这条**无条件**豁免"含 FirstSight 的任何链"，**没有**要求 `version>=3`。
结合第 3 项的版本无关阶段表 ⇒ 对 v1/v2 载荷同样生效。

新增的 `bothRejectedAndFirstSightHead` / `servedCandidateBeforeFirstSight` 确实是**加约束**，
但**抵不过**上述两处放宽。⇒ "不放宽解析器"这条**没有做到**。

## 4. 第 7 项：四处不一致 + 三处放宽

### 4.1 写方与读方**直接冲突**：deltaFrames

| 位置 | 规定 |
| --- | --- |
| 写方 `war3_palette_object_evidence.h:403-407` | 对**每条** FirstSight 写 `delta = 帧 − 链首帧` |
| 读方 `analyze:553-560` | 非 Rejected/Served/终态的 live 事件，delta **必须为 0** |

⇒ 审计者探针 P2（两帧同一对象）复现 `orderIssues=["frozenZeroDeltaViolated@3"]`。

**为什么我的测试没抓到**：往返夹具 G 只跑**单帧**（`war3_palette_object_wire_roundtrip_test.cpp:222-228`）
⇒ 单帧时 `delta` 恒为 0，冲突永远不显现。

⇒ 这是"测试用例形状恰好掩盖了缺陷"的典型。

### 4.2 v3 完成判据**结构性不可达**

`analyze:564-565` 要求 `len(stages)==4`，而正常链按设计**永不发** ServedCandidate
（`d3d9_device.cpp:23524-23527` 明说发 NoteServed 是"冒充 ServedCandidate"）
⇒ v3 正常链最多 3 个阶段 ⇒ **`stageObservationComplete` 永远为 false**。
这与我在实机上看到的 `stageObservationComplete=False(107/107)` 完全一致。

### 4.3 夹具自称"生产形状"但**不是**

`war3_palette_object_wire_roundtrip_test.cpp:221` 声明"与生产采集点的真实取值一致"，
而 `MakeKey`（`:84-90`）明确写 `epochUnknown=false` + 非零 deviceEpoch；
生产恒为 `epochUnknown=true` + deviceEpoch=0（`war3_palette_object_capture.h:161-164`）。

**真实 G 导出的计数器自证**：`epochUnknownRecords=0` 而 `weakIdentityRecords=1` ——
生产环境这两个计数必然**同时**非零，所以夹具形状确实不等价。

### 4.4 新计数器**从不被校验**

读方只要求 v3 块**存在** `firstSightInserted/firstSightEmitted`（`:662-666`），
从不与事件或其他计数比对。审计者探针 B：`firstSightInserted=0, firstSightEmitted=10^9` **被接受**且 `coverageComplete=true`。

### 4.5 三处"放宽痕迹"

| 痕迹 | 位置 |
| --- | --- |
| v3 从"非法版本"清单里**删除** | `test_palette_object_evidence_analysis_static.py:1317-1325`、`test_semantic_build_thread_gate_static.py:1975-1986`（后者注释直说"故移除 3"） |
| 唯一的"v3 正向用例"用的是 **v2 形状的恢复链 + format_version=3** | `test_palette_object_evidence_analysis_static.py:1304-1315` —— **根本没有 FirstSight 事件**，只断言"被接受/版本正确" |
| 场景 G 的门禁**极薄且跳过主体检查** | `test_palette_object_wire_roundtrip.py:917-930` 只断言 4 件事，且调用点 `:1121-1124` **跳过** A–F 都跑的 `check_header_block`/`check_decoded_events`/`check_chain` |

**关于第一处我要说清**：把 v3 从非法清单移除本身是**可辩护的**（v3 现在真的会发出）；
但它确实是"**负向断言被删除**"的实证，应当**同时**补一条正向的、有行为内容的用例 —— 而我没有做。

## 5. 审计者的两项额外发现

### 5.1 实机数据使**所有链**都是 Uncovered（我先前用它当"走完"证据是错的）

`analyze:228-229` 的 `LOSS_FIELDS` 含 `droppedPerFrame`/`droppedPerSession`，**非零即 `lossy`** ⇒
每条链 `covered=false`、`conclusion='Uncovered'`（`:610`）。
用我自报的实机计数（`droppedPerFrame=13547`、`droppedPerSession=2253427`）复现（探针 A）⇒ `Uncovered`。

**关键**：`3693 − 109 = 3584` 恰为 `kNormalBudget` ⇒ 普通事件预算**被打满**，2,253,427 次观测被丢弃。
⇒ "109 条链全部走完"只是"每条都有终态"，**不是可认证的观察**。这一点我在 round 213 已经更正过一次，
但审计者进一步指出：`dropped*` 非零本身就足以让**所有**链不可认证。

### 5.2 清表丢弃在途条目**无任何丢失计数**，且 `coverageComplete` 可为 true

`ResetForSessionTransition`（`h:206-223`，调用点 `d3d9_device.cpp:8699`、`:13580/:13607`）
清表但不结算终态、不计数；诊断计数器被**刻意排除出 wire**（`war3_frame_evidence.cpp:83-99`）。
读方对这种链只给 `orderIssues=['terminalMissing']`，而 `coverageComplete` 仍可为 **true**
（探针 P6/P7）⇒ **"缺少数据时默认成功"的分支在 `coverageComplete` 上确实存在**
（CLI 退出码仍为 2，故不是全链条 fail-open）。

## 6. 审计者最值得怀疑的三点（我完全接受）

1. **"正常观察链"的链首不是"首次"**：`NoteFirstSight` 每帧对同一 Entry 重发（`h:385-393`），
   重发还让 `MarkStage` 记 `orderViolation`（`h:748-754`）并使该条目**永久**失去被判 Recovered 的资格（`h:245-248`）；
   实机 `firstSightInserted=107 / firstSightEmitted=160` 正是重发证据，而新计数器**从不被校验**；
2. **版本 3 的门禁是空的**：G 只断言"版本被接受且未被报成 recovered"，跳过 wire 形状/计数器/阶段/终态；
   唯一"v3 正向用例"用的是 v2 恢复链；同时 v3 正常链在读方那里**结构上不可能 complete**；
3. **"v1/v2 语义未变"与"实机已验收"两处自证不成立**。

## 7. 我要做的（按优先级，**下一轮起**）

| 优先级 | 修正 | 依据 |
| --- | --- | --- |
| P0 | 让 stage=5 的接受**按版本门控**（读方在 v1/v2 遇 stage=5 必须**整份拒绝**） | 第 3 项；这是复审明令的"保留既有拒绝恢复链" |
| P0 | `:485` 的豁免加 `version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION` 条件 | 第 5 项 |
| P1 | 修 deltaFrames 契约冲突（写方 vs 读方二选一，并加**多帧**夹具) | 第 4.1；当前夹具只有单帧才掩盖了它 |
| P1 | v3 完成判据适配正常链（不再要求 ServedCandidate） | 第 4.2 |
| P2 | 夹具改成**真生产形状**（`epochUnknown=true`+deviceEpoch=0） | 第 4.3 |
| P2 | 新计数器加**行为**校验（而不是只查存在） | 第 4.4 |
| P2 | 场景 G 补齐 A–F 都跑的检查，不再跳过 | 第 4.5 |

⚠️ **我不会**用"把判据改宽"的方式让这些消失；P0 两条恰恰是**收紧**。

## 8. 现场

```
d3d9.dll = 基线 F275545B…5CF07FF3；无 War3 进程；未提交、未部署、未晋升稳定。
```