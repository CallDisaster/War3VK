# 阶段 C · 双链路由：**特征化断言**落地（把待裁定行为显式锁住）— 2026-09-18（round 68）

> 接续 `2026-09-18-stageC-dualchain-loses-recovery-measured.md`。
> 本轮把 Case 35 从 **print-only** 变成**载荷用例**，做法是**特征化断言**（characterization）。

## 1. 为什么用"特征化"而不是"断言正确性"

事实：双链并存时拒绝链丢失恢复（**已由对照/处理实测固定**），但**裁定尚未定义该路由语义**
（A 保持现状 / C 两链各自记一份）。

⇒ 若写"正确性断言"（要求 Recovered），门禁会**一直红**，那是把**未裁定**当成已裁定；
  若写 print-only，则**没有任何守卫**，路由被人改动也没人知道。

**特征化断言**是第三条路：**锁定当前行为**，并在断言消息里**明说它不是正确性主张**。

## 2. Case 35 现有 3 条（全绿）

| # | 类型 | 内容 |
| --- | --- | --- |
| 1 | ✅ **正确性** | 对照（只有拒绝链）+ 完整序列 ⇒ `closedRecovered == 1 && closedWindowExpired == 0`。**`Recovered` 必须可达** |
| 2 | ⚠️ **特征化** | 处理（两链并存）+ **同样的完整序列** ⇒ 当前为 `closedRecovered == 0 && closedWindowExpired == 1`；消息里写明 *"This is NOT a claim that it is correct; if you change the routing to record on both chains, update this assertion deliberately and cite the ruling."* |
| 3 | ⚠️ **特征化** | 观察链拥有 served/enqueued/drawn 并结算为 `ObservationClosed == 1` |

## 3. 载荷性（**探针实测**）

把 `FindOwner` 改成优先拒绝链（选项 B）：

```
[FAIL] 35 diagnose both-chains served routing (3 checks, 2 failures)
计数器: closedRecovered=1 closedWindowExpired=1
```

⇒ **任何人改动路由都会被这条断言抓住**（这正是特征化的价值）；
⇒ **并且顺带实测证实了我对选项 B 的推论**：B 让对照仍恢复(1)，却**把假阴性搬到了观察链**
   （`windowExpired=1`）—— 这不再是预测，而是**当轮实测**。

## 4. 状态

```
ion=0 droppedTableFull=0 droppedProbeLimit=0 droppedDuplicatePerFrame=0 droppedPayloadConflict=0 droppedTerminalReserve=0 ringEvictedAfterRecord=0 closedRecovered=0 closedWindowExpired=1 closedObjectGone=0 closedTableFull=0 closedEventLost=0 closedUnclosed=0 weakIdentityRecords=0 epochUnknownRecords=0 watchCount=0
SUMMARY: 34 passed, 0 failed
site=F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
DLL=514266DB2536488691AD89C286056B0C2D0E0D3698BB9AC4388147C07F71F448  (36333949 B)
git  = 无写操作
```

## 5. 不声称

- **不**声称双链路由语义**已定**（**恰恰相反**：本轮是把"未定"显式锁住）；
- **不**声称特征化断言等于"行为正确"（消息里已明说不是）；
- **不**声称该缺陷在**实机**上发生过（人工构造形状测出；实机是否出现"同对象两链"**未核对**）；
- **不**声称 A/C 哪个正确（**需裁定**）；**不**声称 B 可用（**实测它只是搬家**）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。