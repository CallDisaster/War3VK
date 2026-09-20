# 阶段 C · K3 **落地**：尝试内判序（默认惰性 + 两侧断言）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本轮**改产品源码并重建 DLL**。

## 1. 决断过程（round 39）

先做**走法甲**的一次定向查证：`war3_shadow_renderer_core.cpp:6690` 显示拒绝路径的
`renderable` 类型是 **`ShadowRenderableRecord`** —— 即 **manifest 记录**，与 caster/instance
是**不同对象族**，其字段中**没有** caster 索引。

⇒ **候选 A（replayDrawIndex）在拒绝路径上不可得** ⇒ 按预案走**候选 C**。

## 2. 落地方式：**默认惰性**（这是本轮的关键设计决定）

裁定要求 attemptSerial **按值携带**，但**"谁递增"尚未裁定**。若此时让生产调用方
随便传一个值（如帧号），就正是裁定否定的"每帧清零"。

⇒ 因此实现为：

```cpp
// PaletteObjectFrames
uint64_t attemptSerial = ~0ull;   // 哨兵 = **未知**

void MarkStage(Entry* e, uint64_t attemptSerial, PaletteObjectStage stage) {
  const uint32_t s = StageRank(stage);
  if (attemptSerial == ~0ull) {          // 未知 ⇒ **既有终身单调判序，行为不变**
    if (s < e->maxStage) e->orderViolation = true;
    if (s > e->maxStage) e->maxStage = s;
    return;
  }
  if (attemptSerial != e->attemptSerial) {   // 新的明确关联尝试 ⇒ 重置基线，不判违规
    e->attemptSerial = attemptSerial;
    e->attemptStage  = s;
    return;
  }
  if (s < e->attemptStage) e->orderViolation = true;   // 同一尝试内回退：**仍违规**
  if (s > e->attemptStage) e->attemptStage = s;
}
```

- `Entry` 增 `attemptSerial` / `attemptStage`；**保留** `maxStage` 供未知路径使用；
- 两个调用点（`:410` NoteReject、`PrepareStage` 内）改为传 `frames.attemptSerial`；
- **生产调用点一个都没改** ⇒ 全部走未知哨兵 ⇒ **行为一位不变**。

**为什么必须有未知哨兵**（round 37 首次提出，本轮落地）：若无未知值，实现者只能用 0
冒充，而 0 会与"第一次尝试"混淆 ⇒ `MarkStage` 会**错误重置**。

## 3. 证据（两侧 + 惰性，三条同时成立）

```
[K3-30] twoAttempts               terminal=Unclosed   closedRecovered=0   ← 未知哨兵 ⇒ 既有行为不变
[K3-31] twoAttemptsDistinctSerial terminal=Recovered  closedRecovered=1   ← 两个不同尝试号 ⇒ 反例被修
[K3-31] sameSerialRollback        terminal=Unclosed                       ← 同一尝试号内回退 ⇒ 仍违规
```

**第二行是本轮的目标**（round 30 实测的 Unclosed 反例被修）；
**第三行是"检查没有被废掉"的证据**（裁定否定的"每帧清零"未被引入）；
**第一行是"生产未受影响"的证据**（未显式给号 ⇒ 走老路径）。

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
=== 读方 ===
OK
DLL=CCB9DA2AE325039EEC008011520E1652157BA2BC3BFA8F9C3D6C4B3B0956ED50
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. ⚠️ 仍然未完成的部分（不得混淆）

**生产调用方仍未给出 `attemptSerial`。** 也就是说：

- 机制**已就位且有两侧证据**；
- 但**实机路径上判序仍是终身单调**（因为四处调用点都传未知哨兵）⇒
  **round 30 的反例在真实管线上仍然成立**。

要闭合这一步，必须先裁定**"一次尝试"由谁递增**（round 33 §4 的三候选，或新方案）。
本轮**不替裁定做这个决定**。

## 6. 不声称

- **不**声称 K3 已完整落地 —— **生产行为未变**，反例在实机上仍成立（见 §5）；
- **不**声称"一次尝试"的语义已定；
- **不**声称 read 方需要在 wire 上见到 `attemptSerial`（未扩 v4 字段集）；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，
  更不代表阴影已恢复。