# 阶段 C · K3：`replayDrawIndex` 在 **instance** 上，拒绝路径拿不到 — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读侦察，本轮无代码改动。**

## 1. 核实结果

```
d3d9_war3_scene.h:776:   uint32_t replayDrawIndex = ~0u;    // War3ShadowInstanceRef 的字段
d3d9_device.cpp:21993:   instance.replayDrawIndex = …       // 服务路径附近
d3d9_device.cpp:23460:   instance.replayDrawIndex = …       // 首见路径附近
另有 :17815 / :27371 / :44225 / :45742 / :48465 等处
```

⇒ `replayDrawIndex` 是 **`War3ShadowInstanceRef` 的字段**，全文件**只**出现在该 instance 上
（读侧另有 `d3d9_device.cpp:5859` / `d3d9_war3_shadow.cpp:1490` 用它索引 `shadowCasters`）。

## 2. 三处作用域对照（更新）

| 构造点 | 作用域内对象 | 可拿到 `replayDrawIndex`？ |
| --- | --- | --- |
| `:22139` NoteServed | `draw`，且近旁 `:21993` 有 `instance` | ✅ |
| `:23515` NoteFirstSight | `draw` + `instance`（`:23460`） | ✅ |
| `:6821` NoteReject | **`renderable`**（**不是** instance，也不是 `draw`） | ❌ **未见** |

⇒ **候选 A 需要一座桥**：拒绝路径当前拿不到该索引。

## 3. 这座桥可能是现成的（但**未核实**）

拒绝路径遍历的 `renderable` 与 instance 之间**可能**存在现成关联，因为：

- 追加时（`:23460-23473`）instance 与 caster 是**同一次 `emplace_back` 配对**产生的；
- 读侧（`d3d9_device.cpp:5861`）是 `noteReplayDraw(scene.shadowCasters[instance.replayDrawIndex])`
  —— 即**由 instance 索引 caster**，说明二者的对应关系在系统里是**已知**的。

若 caster 结构体本身带有它的索引（反向关联），则拒绝路径的 `renderable` 若就是 caster，
就能直接取到 ⇒ 候选 A 成立且**无需新增状态**。

**但这需要读 `renderable` 的类型定义才能确认 —— 本轮没有读到。**

## 4. 附带的重要事实：该字段的哨兵语义

`replayDrawIndex = ~0u` 表示**未知**。这正是 `attemptSerial` 应有的形状：
**必须能表达"我不知道这是哪次尝试"**。若一个序号没有未知值，实现者就不得不用 0 冒充，
而 0 会与"第一次尝试"混淆 ⇒ `MarkStage` 会据此**错误重置**。

⇒ 无论最终选哪个候选，**attemptSerial 都必须带未知哨兵**，且 `MarkStage` 必须把
"未知"当作**不重置也不判违规**（而不是当作一个新尝试号）。这一条**本轮首次提出**，
应写入实现规格。

## 5. 下一步

```
① 读 war3_shadow_renderer_core.cpp 中 renderable 的**声明处**（本轮未读），
   确认其类型是否携带 caster 索引或与 instance 的反向关联；
② 若携带 ⇒ 候选 A 成立（attemptSerial = 该 index，带 ~0u 未知哨兵）；
③ 若不携带 ⇒ 需在追加 caster 时写入索引（改动面 +1 处），或退回候选 B/C；
④ 无论哪条，实现规格中**必须**包含"未知哨兵不得触发重置"这一条。
```

## 6. 状态（无代码改动）

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 30/0
分析套件 105 OK ; DLL 未变（01230C1F…）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 7. 不声称

- **不**声称候选 A 需要新增状态（§3 的桥**可能现成**，未核实）；
- **不**声称 `renderable` 的类型已查明（**本轮未读其声明**）；
- **不**声称 K3 有进展（无代码改动，终态仍 Unclosed）；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。