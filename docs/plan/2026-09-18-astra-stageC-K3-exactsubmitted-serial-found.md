# 阶段 C · K3 §4① 关键发现：`exactSubmittedFrameSerial` 就在首见调用点手边 — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读侦察，本轮无代码改动。**

## 1. 发现

FirstSight 路径（`d3d9_device.cpp:23505-23529`）遍历的是**语义场景条目 `entry`**，
位于 draw-time semantic producer 提交循环内。而**紧邻调用点之前**：

```cpp
// :23500
entry.exactSubmittedFrameSerial = m_war3ShadowPersistentFrameSerial;
```

⇒ **一个"每次提交"的序号就在手边**，且已写进 `entry`。

但 `:23516` 传给工厂第 2 参数（`recordFrameSerial`）的却是 **`uint64_t(manifestFrame)`**，
**不是**刚算出的那个序号。

## 2. 三处参数的命名族对照

| 构造点 | 传入 `recordFrameSerial` 的实参 | 命名族 |
| --- | --- | --- |
| `d3d9_device.cpp:22142`（NoteServed） | `draw.shadowRecordFrameSerial` | **shadow record frame serial** |
| `d3d9_device.cpp:23516`（NoteFirstSight） | **`manifestFrame`** | **manifest 帧**（不同族） |
| `war3_shadow_renderer_core.cpp:6825`（NoteReject） | `renderable.frameSerial` | frame serial |

两处是 **"shadow record frame serial"** 族，一处是 **manifest 帧**。

## 3. 由此产生的问题（**我不下结论**）

`recordFrameSerial` 在三处应当是**同一个字段语义**（工厂只有一个参数位）。
若首见路径本应传 `entry.exactSubmittedFrameSerial`（手边就有、且是"本次提交"的序号），
那么：

- 三处**可能本来就是同一体系** ⇒ round 32 的假设以**另一种形式**部分复活；
- 且当前首见路径的实参（`manifestFrame`）**可能是一处接错线**。

**这是本轮最有价值的产出：一个具体的、可核实的怀疑对象。**
但我**没有**核实 `m_war3ShadowPersistentFrameSerial` 与
`draw.shadowRecordFrameSerial` / `renderable.frameSerial` 是否同源 ——
名字相似**不等于**同源（本项目已经因为"名字像"而误判过多次）。

## 4. 下一步（把怀疑变成事实）

```
① 查 m_war3ShadowPersistentFrameSerial 的定义与全部赋值点；
② 查 draw.shadowRecordFrameSerial 的赋值点：是否与 ① 同源？
③ 查 renderable.frameSerial 的赋值点：是否与 ①/② 同源？
④ 若三者同源 ⇒ K3 的 attemptSerial **可能就是这个序号**，只需
   · 修正首见路径的实参（若确认接错线，**这是独立缺陷，应单独修 + 单独验证**）；
   · 改 MarkStage 判序作用域；
⑤ 若不同源 ⇒ 回到 round 33 §4 的三候选（A draw 身份 / B 候选集合 / C 显式计数器）。
```

**注意 (④)**：若确认首见路径接错线，那是一条**独立于 K3 的缺陷**。
按本项目纪律应**单独修、单独验证**，不得与 K3 混在一次改动里，
也不得因为"它顺便让 K3 好写"就降低它的证据要求。

## 5. 状态（无代码改动）

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 30/0
分析套件 105 OK ; DLL 未变（01230C1F…）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称首见路径接错线（**只是怀疑**，未核实三处是否同源）；
- **不**声称 `m_war3ShadowPersistentFrameSerial` 就是 attemptSerial；
- **不**声称"一次尝试"的语义已定；
- **不**声称 K3 有进展（无代码改动，终态仍 Unclosed）；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。