# 阶段 C · **双链并存时拒绝链丢失恢复** —— 对照/处理实测证据（可决策）— 2026-09-18（round 67）

> 本文**接续** round 66 的撤回。round 66 我因断言错误而撤回；
> 本轮用**对照 + 处理**重测，缺陷**实测成立**。

## 1. 为什么 round 66 测错了

round 66 的诊断**只调 `NoteServed`**（无 `NoteEnqueued`/`NoteDrawn`）。
那种形状下**任何**链都不会 `Recovered`（`Recovered` 需要真的被绘制）
⇒ 对照组也不会绿，所以那个实验**无法区分"缺陷"与"语义如此"**。

**教训（进一步）**：一个没有**对照组**的实验，
无论打印多少数字，都不能证明异常。

## 2. 本轮的对照/处理（**同一个完整序列**）

两个形状都调：`NoteReject` → `NoteServed` → `NoteEnqueued` → `NoteDrawn` → `CloseWindow`。

形状 2（**对照**，只有拒绝链）：

    [OWN-35-control  ] recovered=1 unclosed=0 windowExpired=0 obsClosed=0

形状 1（**处理**，两链并存）：

    [OWN-35-dualchain] recovered=0 unclosed=0 windowExpired=1 obsClosed=1
      dual rec[0] stage=1 terminal=0 chain=0   ← Rejected（拒绝链）
      dual rec[1] stage=5 terminal=0 chain=1   ← FirstSight（观察链）
      dual rec[2] stage=2 terminal=0 chain=1   ← Served   → 观察链
      dual rec[3] stage=3 terminal=0 chain=1   ← Enqueued → 观察链
      dual rec[4] stage=4 terminal=0 chain=1   ← Drawn    → 观察链
      dual rec[5] stage=4 terminal=2 chain=0   ← 拒绝链终态 = **WindowExpired**
      dual rec[6] stage=4 terminal=7 chain=1   ← 观察链终态 = ObservationClosed

⇒ **同样的完整序列，只因多了一条观察链，拒绝链就丢失了全部恢复事实**；
  它**永远无法**达成 `Recovered`。

## 3. 需要裁定的选项（现在有实测支撑）

| 选项 | 行为 | 实测/推断 |
| --- | --- | --- |
| **A** 保持现状 | 服务/入队/绘制**全部**归观察链 | **已实测**：拒绝链丢失恢复（假阴性） |
| **B** 改为优先拒绝链 | 上述事实反向 | 可预测：**把假阴性移到观察链**（它也需 served/drawn）⇒ **不是解** |
| **C** 两条链**各自**都记一份 | 两条链各自独立完整 | 需裁定：会改变**事件数与导出形状**（分组键含 chainType ⇒ 链不会被并，但读方规则需同步） |

**关键推论**：B **不能**解决问题（只是把丢失换个链）⇒
若要修，语义上只能是 **C**（两链各自独立记录）。
而 C 与裁定「两类链各自独立」的措辞**方向一致**，但它确实会改变导出内容。

**我不自己选**—— round 66 已证明自己选会出错；现在证据到位，**等裁定**。

## 4. 为何本轮**不**把它变成红门禁

裁定允许检查点为红（只要**具名**）。但：

- 变红会阻止后续全量验证（实验室里我每轮都要跑它）；
- 而本轮的**结论已经被对照/处理数据固定**，
  不依赖一条断言是否存在；
- ⇒ **保留 Case 35 为 print-only**（0 checks），把**具名主张**写在本文（以上）。

## 5. 状态

```

[COUNTERS] shared-recorder snapshot before SUMMARY: emitted=7 terminalEmitted=2 droppedPerFrame=0 droppedPerSession=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 ringEvictedAfterRecord=0 closedRecovered=0 closedWindowExpired=1 closedObjectGone=0 closedTableFull=0 closedEventLost=0 closedUnclosed=0 weakIdentityRecords=0 epochUnknownRecords=0 watchCount=0
SUMMARY: 34 passed, 0 failed
=== meson ===
Ok:                85
Fail:              0
wire=0
CHECKS=1160 FAILURES=0
DLL=56A72D2A75238E685109E726367B7B102D57EE8310D9570303227BDA4E3EB227
DLL = 56A72D2A75238E685109E726367B7B102D57EE8310D9570303227BDA4E3EB227
站点 = E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  = 无写操作
```

## 6. 不声明

- **不**声明本轮有任何产品改动（**本轮只做测量**）；
- **不**声明该缺陷在**实机**上发生过（它由**宜人工构造**的形状测出；
  实机上是否出现"同一对象同时两链"尚**未核对**）；
- **不**声明 A/B/C 哪个正确（**需裁定**）；
- **不**声明本次发现影响阴影渲染（它只影响**证据链的终态标签**）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。