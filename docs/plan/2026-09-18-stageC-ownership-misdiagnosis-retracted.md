# 阶段 C · 我的**误诊**："FindOwner 导致假阴性"——已撤回 — 2026-09-18（round 66）

> 本文撤回本轮早期我做出的一个**缺陷声明**。按本项目已有的惯例
> （round 20/21），误诊必须明写撤回，不得惄惄修改。

## 1. 我声称了什么

「同一对象同时持有两条链时，`NoteServed` 被 `FindOwner`（优先 Observation）
路由到观察链 ⇒ 拒绝链**看不到恢复** ⇒ 真实恢复被误报为 `WindowExpired`
（**假阴性**）」。我还据此改了 `FindOwner`、加了两条**具名红**断言。

## 2. 实测数据说了什么

**修改前**（原实现）：

    [OWN-35] recovered=0 unclosed=0 windowExpired=1 obsClosed=1 stored=5
      rec[0] stage=1 terminal=0 chain=0   ← Rejected（建立拒绝链）
      rec[1] stage=5 terminal=0 chain=1   ← FirstSight（建立观察链）
      rec[2] stage=2 terminal=0 chain=1   ← Served → **观察链**
      rec[3] stage=4 terminal=2 chain=0   ← 拒绝链终态 WindowExpired

**我的修改后**：

    [OWN-35] recovered=0 unclosed=1 windowExpired=1 obsClosed=0
      rec[2] stage=2 terminal=0 chain=0   ← Served → **拒绝链**（我的路由修法生效）
      rec[3] stage=4 terminal=6 chain=0   ← 拒绝链终态 **Unclosed**

⇒ **路由事实我说对了**（服务确实去了观察链）；
但 **我对"正确结果"的断言是错的**。

## 3. 我错在哪里

我的诊断里**只调了 `NoteServed`，没有调 `NoteDrawn`**。
把路由修到拒绝链之后，拒绝链的终态是 **`Unclosed`(6)**，**不是** `Recovered`。

    ⇒ `Recovered` 需要拒绝之后真的**被绘制**（Drawn），而不只是"被服务过"。

我把一个**未验证的语义假设**写成了断言（"被服务 ⇒ Recovered"）。
这是掉进了自己反复记过的坑：**先写断言、后理解语义**
（round 22 教训：先打印实测数字）。我这次虽然先打了数字，
**却又把"正确值"**当成已知。打印了数据 ≠ 理解了语义。

## 4. 处置

- 回退 `FindOwner` 到原实现（核对：`rejection->hitCount == 0u` 已不存在）；
- 移除两条基于错误前提的具名红断言；
- **保留 Case 35 作为 print-only 诊断**（`0 checks`）—— 它记录的**路由事实**是真的，
  只是不能从中推出缺陷；
- 树回到**全绿**（见 §6）。

## 5. 依然真实存在的一个**开放语义问题**（需裁定，我不自己定）

「同一对象**同时**持有两条链时，`Served/Enqueued/Drawn` 应归**哪一条**？」

| 选项 | 后果 |
| --- | --- |
| **A** 保持现状（优先观察链） | 拒绝链在双链并存时**不会**因服务/绘制而恢复；它只能结算为 WindowExpired/Unclosed |
| **B** 改为优先拒绝链 | 我实测过：**不会弄坏其它用例**（33/0 仍绿）；但会改变已验收形状的路由 |
| **C** 两条链**各自**都记一份 | 需改接口（`Note*` 目前只接一个 key）或在 sink 里调两次 |

⇒ 裁定未定义这个路由语义。我**不自己定**（它会改变已验收形状的含义）。

## 6. 状态

```

=== meson ===
Ok:                85
Fail:              0
[COUNTERS] shared-recorder snapshot before SUMMARY: emitted=5 terminalEmitted=2 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 ringEvictedAfterRecord=0 closedRecovered=0 closedWindowExpired=1 closedObjectGone=0 closedTableFull=0 closedEventLost=0 closedUnclosed=0 weakIdentityRecords=0 epochUnknownRecords=0 watchCount=0
SUMMARY: 34 passed, 0 failed
wire=0
CHECKS=1160 FAILURES=0
DLL=56A72D2A75238E685109E726367B7B102D57EE8310D9570303227BDA4E3EB227
DLL = 56A72D2A75238E685109E726367B7B102D57EE8310D9570303227BDA4E3EB227
站点 = E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  = 无写操作
```

## 7. 不声明

- **不**声明存在那个假阴性（**已撤回**）；
- **不**声明拒绝链在双链并存时一定**无法**恢复（我只测了"只有 Served"这一种形状）；
- **不**声明选项 A/B/C 哪个正确（**需裁定**）；
- **不**声明本轮有任何产品改动留下（**已全部回退**）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。