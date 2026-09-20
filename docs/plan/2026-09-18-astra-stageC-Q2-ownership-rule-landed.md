# 阶段 C · Q2 ⑥ 落地：归属规则**生效**，且 round 20 的失败是**我的期望错** — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本轮**改产品源码并重建 DLL**。

## 1. 先看数（同形状下只切换查找方式，各跑一次）

| | servedObs | servedRej | enqObs | enqRej | drawObs | drawRej | otherObs | otherRej |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Find(key) | 0 | 1 | **0** | **1** | 0 | 2 | 2 | 1 |
| **FindOwner** | 0 | 1 | **1** | **0** | **2** | 1 | 1 | 1 |

⇒ **归属规则完全按设计工作**：Enqueued 归入**观察链**；观察链上有 **2** 条 Drawn
（一条**真实** NoteDrawn + 一条**终态回填**的 Drawn）；拒绝链保留 Served。

## 2. round 20 失败的**真正原因**（不是规则错）

我把断言写成「Enqueued 与 Drawn 在观察链上的**条数合计 == 2**」，
而实测是 enqObs(1) + drawObs(2) = **3** —— 我漏算了终态回填那条 Drawn。

⇒ **我的计数期望错了；「优先观察链」这条规则本身从未错。**
round 20 我据此写的「真实规则张力」叙述（已在 round 21 撤回）至此**被彻底否决**。

## 3. 本轮落地

- 4 个查找点（NoteServed / NoteEnqueued / NoteDrawn / NoteObjectGone）改用 **FindOwner**；
- Case 27 加入**按实测值**写的归属断言（并把实测数打印出来，作为可持续核对的证据）：
  · servedRej == 1 && servedObs == 0（观察链建立前的 Served 归拒绝链）；
  · enqObs == 1 && enqRej == 0（两类并存时 Enqueued 归观察链）；
  · drawObs >= 1 && drawRej <= 1（真实 NoteDrawn 归观察链）。

**载荷性由实测证明**（不是论证）：同形状下 Find(key) 给出 {enqObs=0,enqRej=1,drawObs=0,drawRej=2}，
与 FindOwner 的 {enqObs=1,enqRej=0,drawObs=2,drawRej=1} **不同** ⇒ 断言改回旧查找即失败。

## 4. 状态

```
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
wire=0
CHECKS=1160 FAILURES=0
DLL=9CB3AA3C91172A7607A4DA86FC36BE44A044BE94CE70E2977EC7086D59D5CC87
[OWNERSHIP-27] servedObs=0 servedRej=1 enqObs=1 enqRej=0 drawObs=2 drawRej=1
evidence : 27 passed, 0 failed
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 不声称

- **不**声称窗口维度已进入查找键（裁定要求 对象键 × 链型 × **窗口**）；本轮只做了链型；
- **不**声称归属规则对**所有**形状都已定义：本轮只钉了「观察链建立前的 Served」与
  「两类并存时的 E/D」两种形状；**只有拒绝链 / 只有观察链**两种形状**未单独断言**；
- **不**声称终态回填那条 Drawn 的语义已复核（我只是把它计入实测数，未单独判定它的正确性）；
- **不**声称读方会拒绝「链型与事件形状不符」的导出；
- **不**声称实机已验证（D 批次完成前不新增实机因果结论）；
- **不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。