# 阶段 C（批次 1-b）逐条状态清单 —— 对照裁定原文 — 2026-09-18（round 43）

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本文是对照裁定的**逐条**清单，
> 目的是在第 43 轮保持可追溯；每条都给出**证据指针**或明确的"未做"。

## A. 存储与预算

| 裁定条目 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| 共用有界存储与预算 | ✅ 已具备 | `kWatchCapacity=1024 / kTotalBudget=4096 / kTerminalReserve=512 / kNormalBudget=3584 / kPerFrameBudget=64`；Case 7/14/29 锁定 |
| 两类链**独立且不可改类**的条目 | ✅ 已落地 | `FindChain`（链型进**匹配谓词**，round 19）；`NoteReject`→RejectionRecovery、`NoteFirstSight`→Observation；`Entry::chainType` 建立时落定 |
| 同一对象**可同时**持有两条链 | ✅ 已落地 | Case 27（两终态、两类事件都在、`firstSightInserted==1`） |
| 两类入口都必须**先 CanEmit 再 Insert** | ✅ 已上锁 | `NoteReject`→Case 25；`NoteFirstSight`→Case 29（+ round 26 变异探针：绕过预检 ⇒ 64→65 具名失败） |

## B. 查找维度

| 裁定条目 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| 对象键 × **链型** | ✅ 已落地 | `FindChain` / `FindOwner`（rounds 19/22） |
| 对象键 × 链型 × **窗口** | ❌ **未做** | 窗口维度**尚未**进入查找键；`windowSegment` 目前只是记录级载体（`data[3]`），不是查找维度 |

## C. 判序

| 裁定条目 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| 按**一次明确关联的尝试**（attemptSerial 按值携带） | 🟡 **机制已就位，生产未接线** | `PaletteObjectFrames::attemptSerial`（哨兵 `~0ull`）+ `MarkStage` 尝试内判序；Case 31 两侧断言 + round 41 变异探针；**但四处生产调用点全部传哨兵** ⇒ 实机判序**仍是终身单调** |
| **不采用每帧清零** | ✅ 已遵守 | 未知哨兵走既有路径；`sameSerialRollback` 断言"同尝试号内回退仍违规"；round 41 变异证明该断言独立驱动 |

## D. 终态

| 裁定条目 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| 新增 `ObservationClosed = 7`（0–6 不动） | ✅ 已落地 | 枚举尾部追加；桶 `closedObservationClosed`；Case 26（6→8 checks） |
| Observation **永不使用** Recovered | ✅ 写方 + 读方 | 写方：`hasRejectFact && closedChain` 才算 Recovered（含 `hitCount==0 && !sawServed → WindowExpired` **先于** Observation 分支）；读方：`observationChainMustNotUseRecovered`（round 24，变异探针） |
| Recovered **必须有真实拒绝事实** | ✅ | 同上（`hasRejectFact = firstReason != NotChecked`）；Case 26 |

## E. 版本与解析

| 裁定条目 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| 新写方**一律**输出 `version=4`（删除动态选版本） | ✅ | `result["version"]=4;` 常量；静态 `11p` 禁止动态形式 |
| v1/v2/v3 **仍按其版本**解析 | ✅ | `TERMINALS_BY_VERSION` / `PALETTE_OBJECT_BLOCK_FIELDS[version]` / `reserved_data_indices(version)` |
| 终态表与阶段表**必须按版本校验** | ✅ | 不得只把 7 加进全局 `TERMINALS`：`test_observation_closed_terminal_is_version_gated` 两侧断言 |
| 修我引入的误拒不变量（解析器 710/716 未排除终态） | ✅ | K1：`live_first_sight` 只数 `NoTerminal`；`backfilled <= emitted`（具名 `backfilled`） |

## F. 测试与交付

| 裁定条目 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| 写方/读方/测试同步 | ✅ | wire `CHECKS=1160`；evidence 31/0；分析套件 105 OK；静态 259/0 |
| 分别列项「生产编码往返测试」与「生产调用方传参测试」 | ✅ | `2026-09-18-astra-stageC-two-test-lists.md`（round 40） |

## G. 本轮新核实的一条"不声称"

**`attemptSerial` 确实未上 wire**（round 43 实测）：

- `attemptSerial` 在 `AutoTest/` 中出现 **0 次** ⇒ 读方完全不知道它；
- sink **只**写 `data[4] = record.chainType`，**无** `attemptSerial` 写入；
- 读方链相关槽位仅 `CHAIN_TYPE_SLOT=('data',4)`（`:236`）。

⇒ 裁定要求的 v4 字段集**未被本轮悄悄扩大**（"不声称"得到验证，而非仅口头声明）。

## H. 仍未闭合的三项（本清单的意义所在）

1. **窗口维度未进入查找键**（B 组第二行）—— 裁定明确要求；
2. **K3 生产侧未接线**（C 组第一行）—— 需要裁定"一次尝试由谁递增"；
3. 链型与**阶段形状**不符的第七种组合（已做：Observation 带 Rejected / RejectionRecovery 带 FirstSight）。

## I. 状态

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 31/0
分析套件 105 OK ; DLL B44F87F6E388284FDF7274EFE4A1F9C3F9C8DE4F1DAC962D8B14A6AB69D11130
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## J. 不声称

- **不**声称批次 1-b 已完成（H 组三项未闭合）；
- **不**声称全门禁通过（裁定禁止）；
- **不**声称两个 P0 已完成 ⇒ **不新增实机因果结论、不晋升稳定候选**；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。