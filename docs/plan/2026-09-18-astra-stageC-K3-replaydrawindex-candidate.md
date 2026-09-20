# 阶段 C · K3：候选 A 具体化 —— `replayDrawIndex` 是"一次 draw"的标识 — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读侦察，本轮无代码改动。**

## 1. 关键读数（`d3d9_device.cpp:23457-23475`）

```cpp
War3ShadowInstanceRef instance = {};
instance.replayDrawIndex =
    static_cast<uint32_t>(m_war3Scene.shadowCasters.size());   // ← draw 的稳定索引
…
m_war3Scene.shadowCasters.emplace_back(std::move(draw));        // :23473
NoteShadowAppendRawcode(entry.rawcode);                        // :23475
…                                                              // :23528 首见事件在此发出
```

⇒ 首见路径**有 `draw` 对象在作用域内**；它被追加进 `shadowCasters` 时带有**稳定索引**
（`replayDrawIndex`，即"将成为第几条 caster"），而首见事件在**追加之后**才发出 ⇒ 索引当时已知。

## 2. 三处作用域对照（本轮读到的事实）

| 构造点 | 作用域内的一等对象 | 证据 |
| --- | --- | --- |
| `:22139` NoteServed | **`draw`** | 读了 `draw.shadowRecordFrameSerial`（`:22142`） |
| `:23515` NoteFirstSight | **`draw`** | `:23473` 已追加，`replayDrawIndex`（`:23460`）已知 |
| `:6821` NoteReject | **`renderable`** | 读了 `renderable.frameSerial`（`:6825`） |

## 3. 为什么 `replayDrawIndex` 符合裁定要的粒度

- 它标识**一次 draw / 一个 caster**，不是帧 ⇒ **不随帧清零** ✅；
- 同一次尝试的服务/入队/绘制都属同一个 draw ⇒ 天然共享同一个值 ✅（**待核实**）；
- 新的尝试 = 新的 draw = **新的索引** ⇒ `MarkStage` 的"尝试号变化即重置"语义自然成立 ✅（**待核实**）。

**`batchHandle` 是次选**：它标识一次批处理，粒度可能比 draw 粗（一个 batch 可含多个 draw）⇒
若同一 batch 内两次尝试会拿到同一个值，则不适合。**本轮未判定二者谁更合适。**

## 4. 剩下的**唯一**问题

拒绝路径（`:6821`）作用域里是 **`renderable`**，不是 `draw`。

⇒ 必须查清：**`renderable` 是否携带**与 `draw` 相同的身份（`replayDrawIndex` / `batchHandle` /
其他可用于关联的字段）？

```
① 查 renderable 的类型定义（在 :6821 处，已知含 .frameSerial / .renderablePart /
   .runtimeModelPtr / .jHandle / .rawcode / .mapEpoch）；
② 看它是否含 replayDrawIndex / batchHandle / 或与 draw 同源的关联字段；
③ 若有 ⇒ 候选 A 成立，attemptSerial = 该身份（**改动面：工厂 + 4 调用点**）；
④ 若无 ⇒ 需在追加 caster 时把索引写进 renderable，或退回候选 B/C。
```

## 5. 状态（无代码改动）

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 30/0
分析套件 105 OK ; DLL 未变（01230C1F…）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 6. 不声称

- **不**声称候选 A 成立（**第 4 节的问题未查**）；
- **不**声称同一尝试的 S/E/D 共享同一个 `replayDrawIndex`（**本轮未核实**，只是它"应当"如此）；
- **不**声称 `batchHandle` 或 `replayDrawIndex` 哪个更合适（**未判定**）；
- **不**声称 K3 有进展（无代码改动，终态仍 Unclosed）；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。