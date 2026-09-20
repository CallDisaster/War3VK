# 裁定「终态表**与阶段表**必须按版本校验」：**两半都有行为用例**（否证结果）— 2026-09-18（round 76）

> 裁定原文：『终态表与阶段表必须按版本校验（不得只把 7 加进全局 TERMINALS）』。
> 我此前只核对了**终态表**这一半。本轮核对**阶段表**那一半。

## 1. 读方实现（两处都按版本，但**形式不同**）

```python
# :302-306  阶段表：三张表 + 内联条件选择
STAGES={1:'Rejected',2:'ServedCandidate',3:'Enqueued',4:'Drawn',5:'FirstSight'}
# 但 STAGES 是**版本无关**的，于是 v1/v2 载荷出现 stage=5 时只是被软标记而不是**整份拒绝**。
LEGACY_STAGES={1:'Rejected',2:'ServedCandidate',3:'Enqueued',4:'Drawn'}

# :442-444  解码处按版本选表
'stage': lookup(
    STAGES if version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION else LEGACY_STAGES, …)

# :316-318  终态表：**字典键版本**
TERMINALS_BY_VERSION={1:TERMINALS,2:TERMINALS,3:TERMINALS,4:TERMINALS_V4}
```

## 2. 两半的**行为用例**（都不是字符串锚点）

| 表 | 用例 | 它钉住什么 |
| --- | --- | --- |
| **阶段表** | `test_first_sight_stage_is_rejected_wholesale_under_legacy_version`（`…analysis_static.py:1396`） | 文档自述：「修复前 STAGES 版本无关 ⇒ 只标记不拒绝；修复后按版本选表（LEGACY_STAGES）⇒ lookup 的 require 抛 ValueError」；用例**双向**：v1/2/None 必须 `ValueError`，而 **v3 下同一形状合法** ⇒ 证明拒绝是**版本门控**而非"见 5 就拒" |
| **终态表** | `test_observation_closed_terminal_is_version_gated`（`:1454`） | 文档自述：「round 6 把终态表改成 TERMINALS_BY_VERSION，但当时**没有用例**证明那条能力是活的 —— 『一律接受 7』或『一律拒绝 7』的实现都能让既有用例通过。本用例**同时钉住两侧**」 |

⇒ **裁定这一条的两半都已满足，且都由行为用例（而非字符串锚点）锁住。**

## 3. 观察到的一处**形式不一致**（记录，不自行改）

- 终态表：**字典键版本**（`TERMINALS_BY_VERSION`）；
- 阶段表：**内联条件**（`STAGES if version>=X else LEGACY_STAGES`）。

两者**都**满足裁定，但形式不同 ⇒ **未来**新增版本时，容易只改一处（这正是本项目
反复出现的「版本无关用法」缺陷类的温床）。

**我为什么不改成统一形式**：

1. 它是**纯风格重构**，行为零变化 ⇒ 没有载荷测试可写（写字符串锁反而**弱于**已有的行为用例）；
2. 按纪律「没有载荷测试的改动不值得动文件」（同 round 70 对 `:830` 重复条件的处置）；
3. 是否统一 ⇒ **需裁定**（是风格偏好，不是缺陷）。

## 4. 状态（本轮**未改任何文件**）

```
k ===
ninja: no work to do.
STATIC=263/0
=== meson ===
Ok:                85
Fail:              0
[COUNTERS] shared-recorder snapshot before SUMMARY: emitted=7 terminalEmitted=2 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 ringEvictedAfterRecord=0 closedRecovered=0 closedWindowExpired=1 closedObjectGone=0 closedTableFull=0 closedEventLost=0 closedUnclosed=0 weakIdentityRecords=0 epochUnknownRecords=0 watchCount=0
SUMMARY: 34 passed, 0 failed
wire=0
CHECKS=1160 FAILURES=0
OK
site=F275545BAA65A015
站点 = F275545BAA65A015…（基线，未部署）
git  = 无写操作
```

## 5. 不声称

- **不**声称"阶段表校验"是我本轮**新增**的（它**早已存在**，本轮只是核对并具名其用例）；
- **不**声称两张表**所有**与版本相关的行为都被覆盖（我只核对了"表的选择"这一条；
  其它版本相关规则另有其用例，见 round 70 的读方审计）；
- **不**声称形式不一致会导致缺陷（它是**风险**描述，不是已发生的缺陷）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。