# 阶段 C · Q2 ⑥ 补齐：归属规则**三种形状全部钉住** — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。

## 1. round 22 留下的缺口

round 22 我明确记下：归属规则只钉了「观察链建立前的 Served」与「两类并存时的 E/D」两种形状，
**「只有拒绝链 / 只有观察链」未单独断言**。本轮补齐。

## 2. 新增 Case 28（3 checks）

| 形状 | 构造 | 断言 |
| --- | --- | --- |
| 只有**拒绝**链 | NoteReject + Served + Enqueued + Drawn（无 FirstSight） | 4 条事件**全部**带 RejectionRecovery，**0** 条带 Observation |
| 只有**观察**链 | 仅 NoteFirstSight（无任何拒绝事实） | 链首带 Observation（1 条），RejectionRecovery **0** 条，且 **不得凭空造出 Served 阶段** |

实测打印：

```
[OWNERSHIP-28] rejectOnlyObs=0 rejectOnlyRej=4 obsOnlyObs=1 obsOnlyRej=0 obsOnlyServed=0
```

**为什么这两条是必要的**（不是凑数）：

- 「优先 Observation」是一条**优先级**规则。优先级规则最典型的失败模式就是
  「在只有一种链时把它弄丢」—— 第一组断言正是拦这个；
- 第二组拦住另一种失败模式：**为了满足优先级而伪造阶段**
  （观察链凭空出现 Served ⇒ 会让读方以为该对象被服务过）。

## 3. 三种形状的覆盖矩阵（⑥ 现状）

| 形状 | 用例 | 状态 |
| --- | --- | --- |
| 两类链并存 | Case 27 | ✅ 已断言 |
| 只有拒绝链 | Case 28 | ✅ 已断言 |
| 只有观察链 | Case 28 | ✅ 已断言 |

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
evidence : 28 passed, 0 failed（Case 27 = 6 checks, Case 28 = 3 checks）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 不声称

- **不**声称**窗口维度**已进入查找键（裁定要求 对象键 × 链型 × **窗口**）—— 仍只做了链型；
- **不**声称终态回填那条 Drawn 的语义已复核；
- **不**声称读方会拒绝「链型与事件形状不符」的导出；
- **不**声称实机已验证（D 批次完成前不新增实机因果结论）；
- **不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，观察完整不代表对象已证明，
  更不代表阴影已恢复。