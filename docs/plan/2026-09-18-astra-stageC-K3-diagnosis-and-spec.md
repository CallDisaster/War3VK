# 阶段 C · K3 诊断与实现规格：跨帧判序按「一次尝试」而非终身单调 — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读诊断，本轮无代码改动。**

## 1. 现状（已核对的代码）

```cpp
// :763  Entry 成员
uint32_t maxStage = 0u;          // **终身单调**，从不重置

// :849-856  判序
// 阶段顺序：只允许单向推进；回退记为顺序违规（该条目不得被判 Recovered）。
void MarkStage(Entry* e, PaletteObjectStage stage) {
  const uint32_t s = StageRank(stage);
  if (s < e->maxStage)
    e->orderViolation = true;
  if (s > e->maxStage)
    e->maxStage = s;
}
```

`StageRank`（:819-847）已修掉**枚举值**造成的假违规：

| 阶段 | 枚值 | 语义秩 |
| --- | --- | --- |
| FirstSight | 5 | **0** |
| Rejected | 1 | 1 |
| ServedCandidate | 2 | 2 |
| Enqueued | 3 | 3 |
| Drawn | 4 | 4 |

（:816-818 的注释即为此而写：FirstSight 枚举值取大是为了让 v1/v2 读方直接拒绝。）

⇒ **K3 剩下的不是"秩"的问题，而是"判序作用域"的问题。**

## 2. 具体反例（裁定要修的就是它）

同一对象：

```
帧 N   : Rejected(1) -> Served(2) -> Enqueued(3) -> Drawn(4)     maxStage = 4
帧 N+1 : Rejected(1)                                           1 < 4 ⇒ orderViolation = true
```

⇒ 第 N+1 帧的**一次合法新尝试**被判成"阶段回退"，该条目**永远不得再被判 Recovered**。

这不是"缺少事件"，而是**判序把两次不同的尝试混成了一次** ——
正是裁定「跨帧判序改为按**一次明确关联的尝试**（attemptSerial 按值携带），
不采用每帧清零」所要修的。

**注意**：修复**不是**"每帧清零"。每帧清零会让**同一帧内**跨阶段的真实回退（如同一帧内
Drawn 之后又一 Rejected）不再被发现 —— 裁定明确否定了这个方向。

## 3. 实现规格（下一轮实施）

### 3.1 载体（**按值携带**）

在 `PaletteObjectFrames` 增加 `uint32_t attemptSerial`（与 `renderFrame` 同路、按值传），
由**调用方**在一次明确关联的尝试开始时给出（同一尝试的所有 S/E/D 共用同一个值）。

### 3.2 条目侧

```cpp
uint32_t lastAttemptSerial = 0u;   // 该条目最近一次记录的尝试
uint32_t attemptStage     = 0u;    // **本次尝试内**的最高秩（不再是终身 maxStage）
```

### 3.3 判序改为「尝试内单调」

```cpp
void MarkStage(Entry* e, uint32_t attemptSerial, PaletteObjectStage stage) {
  const uint32_t s = StageRank(stage);
  if (attemptSerial != e->lastAttemptSerial) {
    // 新的明确关联尝试 ⇒ 重置**本次尝试**的基线，并**不得**据此判违规。
    e->lastAttemptSerial = attemptSerial;
    e->attemptStage = s;
    return;
  }
  if (s < e->attemptStage)
    e->orderViolation = true;   // 同一尝试内回退：仍然违规
  if (s > e->attemptStage)
    e->attemptStage = s;
}
```

**保留** `orderViolation` 的后果（该条目不得判 Recovered）—— 本轮不动终态语义。

### 3.4 必须同步的点

1. `MarkStage` 的全部调用点（`PrepareStage` 内）⇒ 需要把 `attemptSerial` 传进去；
2. `Entry` 的 `maxStage` 字段与 `:304` 的 `StageOfRank(e.maxStage)` 引用；
3. 测试：反例必须**不再**产生 `orderViolation`；**同一尝试内的回退仍必须**产生它
   （两侧都钉，否则"修好了"与"把检查删了"分不出来——与 round 28 的双探针同一理由）；
4. 读方是否需要在 wire 上见到 `attemptSerial`（**本轮未判定**，见 §4）。

## 4. 未判定 / 不声称

- **不**声称 `attemptSerial` 必须上 wire：裁定说的是"按值携带"，未说明是否导出。
  在写方落地并观察读方是否需要它之前，**不预先扩 v4 字段集**；
- **不**声称改用尝试判序后 `Recovered` 的**实机**覆盖面会改善（本轮无任何实机数据，
  且 D 批次完成前不新增实机因果结论）；
- **不**声称反例已在当前代码上**实测**复现 —— §2 是从 `:849-856` 的代码推出的，
  **尚未写用例观测**（下一轮第一步就是把它写成可失败的用例）；
- **不**声称 K3 有任何进展（本轮**无代码改动**）。

## 5. 状态

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 29/0
分析套件 105 OK ; DLL 01230C1F70F6E67B1FEFD8066AFC8521D818852927934214B1DDDE17F330311D（未变）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```