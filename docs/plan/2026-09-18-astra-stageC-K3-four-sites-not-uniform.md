# 阶段 C · K3 §4① 定案：四处**不同体系** ⇒ `recordFrameSerial` 不能当 attemptSerial — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读侦察，本轮无代码改动。**

## 1. 实测到的四个构造点取值

| 构造点 | 相邻 Note* | `renderFrame` | **`recordFrameSerial`** |
| --- | --- | --- | --- |
| `d3d9_device.cpp:22139` | NoteServed | `RenderState::getFrameIndex()` | **`draw.shadowRecordFrameSerial`** |
| `d3d9_device.cpp:23515` | NoteFirstSight | `currentRenderFrameIndex` | **`uint64_t(manifestFrame)`** |
| `war3_shadow_renderer_core.cpp:6821` | NoteReject | `RenderState::getFrameIndex()` | **`renderable.frameSerial`** |
| `d3d9_war3_shadow.cpp:5241` | （无相邻 Note*） | — | **本轮仍未读** |

## 2. 结论：round 32 的假设**被证伪**

round 32 我提出「`recordFrameSerial` 已按值携带，可能就是 attemptSerial，
K3 或许**不需要**新字段」。核对三处取值后：

- 三者是**三种不同的量**（draw 记录序号 / manifest **帧号** / renderable 序号）；
- ⇒ **不存在**一个现成的、跨路径一致的"尝试序号"；
- ⇒ **K3 必须显式引入 `attemptSerial`**，改动面回到**跨文件**（工厂 + 四个调用点 + 测试辅助）。

**这正是"先核对再动手"的价值**：若我按 round 32 的假设直接改 `MarkStage`，
会得到一个"看起来在按尝试判序、实际在比较三种不同的量"的实现 ——
比不改更糟（它会让 `orderViolation` 变成随机的）。

## 3. 裁定的告诫在此得到印证

裁定写「attemptSerial 按值携带，**不采用每帧清零**」。核对后发现：
**首见路径手上只有 `manifestFrame`（帧号）**，服务路径手上是 draw 的记录序号 ——
也就是说，最省事的实现方式恰恰是"用帧号当尝试号"（= 每帧清零），
**而裁定预先禁止了它**。本轮的核对结果**支持**这条禁令：

- 帧号**不是**尝试的自然边界（同一帧内可以有多次尝试；跨帧也可以是同一次尝试）；
- 若用帧号，`MarkStage` 的"尝试号变化即重置"会退化成"每帧重置" ——
  同一帧内的真实回退（Drawn 之后又 Rejected）**不再被发现**。

## 4. 下一步（语义仍未定，但**范围已收窄**）

还需回答的**唯一**问题：**"一次尝试"在 WarVK 里由什么界定？**

三个候选（都必须先核实再选）：

| 候选 | 依据 | 风险 |
| --- | --- | --- |
| **A** `draw` 的身份（`draw` 的序号/地址） | 服务路径已有 `draw.shadowRecordFrameSerial` | 首见/拒绝路径拿不到同一个 draw |
| **B** 一次 **shadow caster 候选集合** | 拒绝路径每次判定都属于某个集合 | 首见路径 `entry` 与 draw 的关联需核实 |
| **C** 由调用方**显式递增**的会话级计数器 | 不依赖任何现成量 | 需要新增状态与明确的递增点 |

**① 先读 `d3d9_device.cpp:23500-23540` 的循环头**，弄清 FirstSight 的 `entry` 来自哪个集合、
该集合与 `draw`/`renderable` 是否同源 —— 这直接决定 A/B 是否可行；
**② 再读 `d3d9_war3_shadow.cpp:5241`**（唯一未读点）；
**③ 三候选都核实后**才选一个并写进注释与测试。

## 5. 状态（无代码改动）

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 30/0
分析套件 105 OK ; DLL 未变（01230C1F…）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称"一次尝试"的语义已定（§4 三候选**都未核实**）；
- **不**声称四个构造点全部读完（`d3d9_war3_shadow.cpp:5241` **仍未读**）；
- **不**声称 K3 有进展（无代码改动，终态仍 Unclosed）；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。