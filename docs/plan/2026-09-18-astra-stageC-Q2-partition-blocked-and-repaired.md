# 阶段 C：Q2 分区尝试**被 Case 8 拦下**；随后我**弄坏并修复**了记录器头 — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。**本轮没有留下前进性改动**，树已回到 round 15 的已验证状态。

## 1. 我尝试了什么（⑤+⑥+⑧ 一次落地）

- **C++**：新增 `FindChain(key, type)`（按链型匹配）与 `FindOwner(key)`（归属规则：优先 Observation，
  其次 RejectionRecovery），并把 6 个调用点改为：`NoteReject`→RejectionRecovery、
  `NoteFirstSight`→Observation、S/E/D/`ObjectGone`→`FindOwner`；
- **读方**：`chain_group_identity` 的分组键加入 `chainType`（插在分段标签**之前**，
  以免破坏下方 `item[0][-1]` 的排序语义）。

## 2. 为什么被拦下（**Case 8，3 处失败**）

```
FAIL: a repeated NoteFirstSight on a LATER frame must NOT emit again (the chain head is per entry, not per frame)
FAIL: the repeated first sight must be counted as droppedDuplicatePerFrame
FAIL: exactly one first-sight emission per entry
```

⇒ 分区之后"**每条目恰一个链首**"这条不变量在 Case 8 的形状上不再成立。**我没有查明原因**
（是夹具的旧期望、还是分区引入的真实回归），因此**不保留**这个半理解的语义改动 ——
与 round 4（"先到先得"被 Case 8 证伪）和 round 8（`ObservationClosed` 分支顺序被 Case 24 拦下）
同一处置原则。

## 3. ⚠️ 我自己造成的一次**严重破坏**（已修复）

回退是脚本化的，而我的**块删除边界算错了**：

```js
const first  = h.indexOf("\n  }\n", s);            // FindChain 结束
const second = h.indexOf("\n  }\n", first + 1);    // 我以为 = FindOwner 结束
h = h.slice(0, s) + h.slice(second + 5);            // 实际多删了约 1.7 KB
```

⇒ 被删掉的包含 **`Entry* Insert(...)` 的完整定义**（992 字符）⇒ 构建失败：

```
error: 'Insert' was not declared in this scope
```

**修复**：从 doc-r1 交付包中的**纯净副本**取出 `Insert`，补回我自己的两处改动
（`chainType` 形参 + `e.chainType = chainType;` 盖章），插回 `Find` 之前 ⇒
`ninja: no work to do` + `SUMMARY: 26 passed, 0 failed`。

**教训（与 round 3/11/12 同族）**：
1. **用"第 N 个 `\n  }\n`"做块边界是脆的** —— 它不知道函数体内可能已有同形状的行；
2. **脚本化删除必须先把待删文本打印出来确认**，而不是凭偏移量推断；
3. **不验证构建就继续**会让破坏累积（本轮我先跑了测试才发现构建失败）。

## 4. 状态（已回到 round 15 的已验证状态）

```
=== static ===
STATIC: 259 scripts, 0 failed
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== wire ===
exit=0
CHECKS=1160 FAILURES=0
=== 读方 ===
OK
=== DLL ===
36297544 B  1C85084115534EF3312F259E5AAE4B668A689A00664FFF6C3E89187D71E5F333
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 正确的下一步（⑤+⑥+⑧ 重做，但**先诊断 Case 8**）

```
① 先读 Case 8 的驱动序列，判定 3 处失败是"夹具旧期望"还是"分区的真实回归"：
   · 若为夹具旧期望 ⇒ 明确写出"分区后每条目仍恰一个链首"的证据，再改期望；
   · 若为真实回归 ⇒ 分区方案本身需要修正（例如链首去重必须**按 (对象键 × 链型)** 而非按条目）。
② 分区落地时**先只改 NoteReject/NoteFirstSight 两个入口**，S/E/D 暂留 Find(key)，
   观察 Case 8 是否仍失败 —— 以把"分区"与"归属规则"两个变量分开。
③ 读方分组键必须**同一次**落地（否则同对象两条链被并成一条）。
④ 每条改动**单独步进**（一次一个函数），不再做多文件脚本化大改。
```

## 6. 不声称

- **不**声称 ⑤/⑥/⑧ 有任何进展（**全部回退**）；
- **不**声称链型分区已实现、同对象可同时持有两条链；
- **不**声称 K3（跨帧判序）已修；**不**声称阶段 C 主体完成；
- 本轮的产出是**两条教训**（块边界脆性、脚本化删除必须先打印确认）与**一次已完成的修复**，
  **不计为前进性进展**。
