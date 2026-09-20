# 阶段 C · K3：反例**实测确认**（`Unclosed`，`closedRecovered=0`）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。

## 1. 观测方法（先看数，再改）

新增 `Case 30`，**只打印不断言**（门禁保持绿），驱动 round 29 的反例：

```
第 1 轮: NoteReject(1000) -> NoteServed(1001) -> NoteEnqueued(1002) -> NoteDrawn(1003)
第 2 轮: NoteReject(1010) -> NoteServed(1011) -> NoteEnqueued(1012) -> NoteDrawn(1013)
CloseWindow(1100)
```

## 2. 观测结果（**实测，非推断**）

```
[K3-30] twoAttempts terminal=Unclosed terminals=1 emitted=9
        closedRecovered=0 closedUnclosed=1 closedWindowExpired=0
```

⇒ 两次**完整**尝试之后，终态是 **Unclosed**，而**不是** Recovered。
⇒ round 29 从 `:849-856` 推出的反例**成立**：
第二轮 `Rejected`（语义秩 1）小于终身单调的 `maxStage`（4）⇒ `orderViolation = true`
⇒ 该条目不得判 Recovered。

**这把 K3 从"代码推论"变成了"可观测缺陷"** —— 也是本轮真正的产出。

## 3. 实施时新发现的一个约束（对 round 29 规格的修正）

round 29 我写"`PaletteObjectFrames` 增 `attemptSerial`（按值携带）"。
本轮实施前核对了**由谁提供这个值**，发现一个此前没写明的约束：

- `PaletteObjectFrames` 由**调用方**构造（测试里是 `MakeFrames(frame)`，生产里在渲染管线侧）；
- 若只是给结构体加字段并让**旧调用方**默认给 0 ⇒ 所有事件共享同一尝试号 ⇒
  **永不会触发重置** ⇒ 反例**依然成立**（等于没修）；
- 若让测试里 `MakeFrames` 用 `frame` 当尝试号 ⇒ 那就退化成"**每帧清零**"，
  正是裁定**明确否定**的方向（同一帧内的真实回退会不再被发现）。

⇒ **结论：`attemptSerial` 必须由调用方在一次明确关联的尝试开始时显式给出**，
不能从帧号派生、也不能默认 0 了事。这意味着 K3 的落地**必须同时改调用方**
（测试的 `MakeFrames` 与生产侧构造 `PaletteObjectFrames` 的位置），
不是 `MarkStage` 一个函数内能闭合的改动。

## 4. 修正后的实施顺序

```
① 查清生产侧**全部**构造 PaletteObjectFrames 的位置，确定"一次尝试"在那里如何界定
   （是每 draw 调用？每次 palette 命中？还是每帧的候选集合？）—— 这决定 attemptSerial 的语义；
② 定下语义后再加字段与 MarkStage 判序；
③ 测试侧 MakeFrames 显式接收 attemptSerial（**不得**用帧号冒充）；
④ 两侧断言：反例（两个不同尝试号）⇒ **必须 Recovered**；
   同一尝试号内的回退（如 Drawn 之后再 Rejected）⇒ **仍必须** orderViolation；
⑤ 反向变异：把 MarkStage 改回终身单调 ⇒ ④ 的第一条必须失败。
```

**第 ① 步是本轮新暴露的前置条件** —— 我原本以为可以先改判序再管调用方，
实测与代码核对后确认**不能**。

## 5. 状态

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 30/0
分析套件 105 OK
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

（Case 30 是**只打印**的诊断用例，不是断言；它计入用例数是既有测试框架的行为。）

## 6. 不声称

- **不**声称 K3 已修（**未修**：终态仍是 Unclosed）；
- **不**声称 `Unclosed` 在**实机**上一定代表缺陷（本轮是**夹具驱动**的反例，
  且 D 批次完成前不新增实机因果结论）；
- **不**声称"两次尝试"在真实管线里就是 K3 需要支持的形状 —— §4① 正是要先把这件事问清楚；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。