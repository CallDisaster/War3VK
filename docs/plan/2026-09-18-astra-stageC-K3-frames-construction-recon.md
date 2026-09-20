# 阶段 C · K3 §4①：`PaletteObjectFrames` 的构造点侦察（完成定位，语义待定）— 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读侦察，本轮无代码改动。**

## 1. 结论：改动集**有界**（这是我上轮担心的问题的答案）

| 对象 | 位置 |
| --- | --- |
| `struct PaletteObjectFrames` | `war3_palette_object_evidence.h:43` |
| **唯一的工厂** `MakePaletteObjectFrames` | `war3_palette_object_capture.h:176` |

⇒ **所有**生产侧帧域都经这一个工厂构造 ⇒ `attemptSerial` 有**单一穿孔点**，
不需要在渲染管线里到处传参。这比我上轮设想的乐观。

## 2. 生产侧构造点（全部 4 处）

| 位置 | 构造方式 | 相邻的 Note* 调用 |
| --- | --- | --- |
| `d3d9_device.cpp:22121` + `:22139` | 先 `paletteObjectFrames{}`，后整体赋值 | `NoteServed`（`:22222`） |
| `d3d9_device.cpp:23515` | 直接构造 | `NoteFirstSight`（`:23528`） |
| `d3d9_war3_shadow.cpp:5241` | 直接构造 | （本轮未定位到相邻 Note*） |
| `war3_shadow_renderer_core.cpp:6821` | `const PaletteObjectFrames frames = …` | `NoteReject`（`:6828`） |

测试侧另有独立的构造辅助（`MakeFrames` / `Frames`）与若干直接调用，
**必须同步**（否则要么编译失败、要么用默认值静默绕过——后者正是我上轮警告的"等于没修"）。

测试侧的已知调用点：

- `war3_palette_object_evidence_test.cpp:248`（`MakeFrames`）；
- `war3_palette_object_evidence_cost_test.cpp:144`（`Frames`）；
- `war3_palette_object_wire_roundtrip_test.cpp:74`（直接调用，4 参）；
- `war3_palette_slot_recheck_evidence_test.cpp:380`（直接调用，4 参）。

## 3. 尚未确定的**语义**问题（本轮刻意不猜）

裁定说「按**一次明确关联的尝试**（attemptSerial 按值携带）」，但**没有定义"一次尝试"在
WarVK 里是什么**。四个构造点分处两条不同路径：

| 路径 | 构造点 | 候选语义 |
| --- | --- | --- |
| **palette 服务路径** | `d3d9_device.cpp:22139` / `:23515` | 每次 palette 命中？每个 draw 调用？每帧的候选集合？ |
| **阴影拒绝路径** | `war3_shadow_renderer_core.cpp:6821` | 每次 caster 拒绝判定？每个 shadow pass？ |

**这直接决定判序是否正确**：

- 若"一次尝试" = **每次 draw**，那么同一帧内多个 draw 会产生**不同**尝试号 ⇒
  同一帧内的真实回退（Drawn 之后又 Rejected）**将不再被发现** ⇒ 退化成裁定否定的方向；
- 若"一次尝试" = **每个候选集合**，K3 的反例（跨帧第二轮）**才**能被正确表达；
- 两条路径（服务与拒绝）**是否共用同一个尝试号体系**也未定 —— 它们写的是**同一个对象键**，
  若各用各的编号，`MarkStage` 的"尝试号变化即重置"会被**错误触发**。

⇒ **本轮到此为止**：定位已完成，语义**必须**先定清再改代码。
**我不在没有语义定义的情况下先加字段**（那正是 round 20 的教训：先写期望、后被推翻）。

## 4. 下一步（唯一正确的顺序）

```
① 读 d3d9_device.cpp:22100-22160 / 23490-23540 与 war3_shadow_renderer_core.cpp:6800-6840
   的实际上下文，弄清这两条路径各自"对一个对象的一次完整处理"从哪开始、到哪结束；
② 特别要弄清：**服务路径与拒绝路径是否可能对同一个对象键交替发生**
   （若是，两者必须共用同一编号体系，否则重置会被错误触发）；
③ 语义写进注释与测试之后，才加字段；
④ 两侧断言 + 反向变异（见 round 30 §4④⑤）。
```

## 5. 状态（无代码改动）

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 30/0
分析套件 105 OK ; DLL 未变（01230C1F…）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称 K3 有进展（**无代码改动**，终态仍是 Unclosed）；
- **不**声称"一次尝试"的语义已定（§3 是**待决问题**，不是结论）；
- **不**声称四个构造点里第三个（`d3d9_war3_shadow.cpp:5241`）的角色已查明 ——
  它没有相邻的 Note* 调用，**我尚未读它的上下文**；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。