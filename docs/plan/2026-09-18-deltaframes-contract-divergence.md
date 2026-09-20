# P1 定位：deltaFrames 的写方/读方契约分歧（成因与两条出路）— 2026-09-18

> 对抗性审计以探针 P2（两帧同一对象）复现了 `orderIssues=["frozenZeroDeltaViolated@3"]`。
> 本文把成因定位到**两侧都"有意为之"的契约分歧**，并给出出路。**本轮未改代码** —— 因为改动方向取决于一个尚未定论的语义问题。

## 1. 写方（`war3_palette_object_evidence.h:369-411`）

```cpp
void NoteFirstSight(key, frames) {
  if (m_windowClosed) return;
  BeginFrame(frames.renderFrame);
  Entry* e = Find(key);
  if (e == nullptr) {
    e = Insert(key, frames.renderFrame, NotChecked, frames);  // firstRejectFrame 槽位 = 首见帧
    m_counters.firstSightInserted++;
  } else if (e->lastFirstSightFrame == frames.renderFrame) {
    m_counters.droppedDuplicatePerFrame++;   // **只做同帧去重**
    return;
  }
  ...
  // 版本 3 契约：首见链的 firstRejectFrame 槽位承载的是**链首帧**（= 首见帧）；
  // 该链不存在拒绝帧。deltaFrames 因而 = 本帧 - 链首帧，含义自洽。
  record.deltaFrames = frames.renderFrame - e->firstRejectFrame;   // 第 2..N 次 > 0
  EmitUnchecked(record, false);
}
```

## 2. 读方（`analyze_palette_object_evidence.py:553-560`）

规定**非 Rejected / 非 ServedCandidate / 非终态的 live 事件，`deltaFrames` 必须为 0**。
这条规则是为**拒绝恢复链**写的 —— 在那里所有 live 事件共享同一个 `firstRejectFrame`，
所以 delta 恒为 0 是**恒等式**，用来证明事件确实同属一条链。

## 3. 分歧的本质

| | 对 `firstRejectFrame` 槽位的解释 | 于是 delta 应当是 |
| --- | --- | --- |
| 写方 | 首见链里它承载**链首帧**（非拒绝帧） | `本帧 − 链首帧` |
| 读方 | 它**始终**是拒绝帧；live 事件必须同帧 | `0` |

**两侧都不是笔误**：写方注释明确声明它重定义了该槽位的含义（"含义自洽"）；
读方规则也没改过。所以这是一个**真实的契约分歧**，不是谁写错了变量。

## 4. 为什么我的测试没抓到（审计已指出）

往返夹具 G 只跑**单帧**（`war3_palette_object_wire_roundtrip_test.cpp:222-228`）。
单帧时 `frames.renderFrame == e->firstRejectFrame` ⇒ `delta == 0` ⇒ **恰好满足读方规则**。
⇒ **用例的形状恰好掩盖了缺陷**；只有"两帧同一对象"才暴露。

## 5. 更深一层的成因：`NoteFirstSight` 每帧重发

```
实机：32 个对象 -> 1304 条 FirstSight 事件（≈41 条/对象）
代码：385 行**只做同帧去重**（e->lastFirstSightFrame == frames.renderFrame）
      ⇒ 跨帧时同一对象会**再发一次** FirstSight
```

**这同时解释了三件事**：

1. **delta 冲突**：第 2..N 次重发时 `本帧 − 链首帧 > 0` ⇒ 触发读方的 `frozenZeroDeltaViolated`；
2. **名实不符**：阶段叫 `FirstSight`（**首次**观察），行为却是"每帧再观测"；
3. **永久失去认证资格**：审计指出重发会让 `MarkStage` 记 `orderViolation`
   （`h:748-754`，rank 0 < maxStage 3），使该条目**永久**无法被判 `Recovered`（`h:245-248`）。

⇒ **delta 冲突是"每帧重发"这个更深问题的表征**，不是独立缺陷。

## 6. 两条出路（**本轮未实施**）

### 出路 A（推荐，但需先定语义）：让 `FirstSight` 真的只发一次

```cpp
// 只在 e == nullptr（首次建条目）时发射；已有条目直接返回
```

**一并解决**：delta 恒 0（满足读方）＋ 名实相符 ＋ 不再触发 `orderViolation` ＋
事件量从 1304 降到 32。

**风险/前提**：需要先确认"每帧重发"**不是**别的功能所依赖的（例如某处靠"本帧是否又看到该对象"
来判断存活）。在确认之前我不会改 —— 我此前已因为"看起来对就改"付出过代价。
子线程 D 正在查这一点。

### 出路 B：让读方的 delta 规则**按链型**区分

首见链的锚点就是首见帧 ⇒ 读方对首见链应要求 `delta == 本帧 − 首见帧`，而不是 0。

**我不倾向 B**：它把"每帧重发"这个可疑行为**固化为契约**，等于承认 `FirstSight` 可以重复。
这与 ③ 要的"首次观察"语义相悖。

## 7. 状态

```
已修：P0（stage=5 版本门控）+ 其行为测试（96/0）
待定：P1 deltaFrames —— 阻塞在"NoteFirstSight 每帧重发是否为本意"（子线程 D 调查中）
```

**我不在语义未定论时改动写方**。

## 8. 现场

```
d3d9.dll = 基线 F275545B…5CF07FF3；无 War3 进程；未提交、未部署。
```