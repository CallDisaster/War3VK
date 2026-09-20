# 阶段 C · Q2：Case 8 失败性质**已判定为行为差异**（不是夹具旧期望）— 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。**只读诊断，本轮无代码改动。**

## 1. 诊断对象

`war3_palette_object_evidence_test.cpp` `Case 8`（`bool Case8DuplicateRejectPerFrame()`，
`:849` 起）末段 `:932-946` —— **"跨帧首见去重"的回归锁**：

```cpp
rec.NoteFirstSight(key, MakeFrames(7u));
Require(g_stored - storedBeforeFirstSight == 1u, "the first NoteFirstSight of an entry must be emitted");
rec.NoteFirstSight(key, MakeFrames(9u));      // 跨帧重复
Require(g_stored - storedBeforeFirstSight == 1u, "…must NOT emit again (per entry, not per frame)");
Require(droppedDuplicatePerFrame == before + 1u, "…counted as droppedDuplicatePerFrame");
Require(firstSightEmitted == 1u, "exactly one first-sight emission per entry");
```

**关键前置**：`Case 8` 是"同帧重复拒绝"用例 ⇒ 走到这一段时，**`key` 已经有拒绝建立的条目**。

## 2. 判定结果

分区版（round 16 已全部回退）跑出的 3 条失败是：

| 断言 | 行 | 结果 |
| --- | --- | --- |
| 第一次首见必须发射 | `:935` | **通过** |
| 跨帧重复**不得**再发 | `:941` | **失败** |
| 重复必须计入 `droppedDuplicatePerFrame` | `:943` | **失败** |
| 每条目恰一次首见发射 | `:945` | **失败** |

⇒ **第二次调用又发射了一条事件**（于是 `firstSightEmitted` 变成 2，去重计数也未增长）。

**结论：这不是"夹具的旧期望需要更新"，而是"按条目去重"在分区后没有生效。**
因此修正的方向**不是**改夹具，而是查清分区版里 `NoteFirstSight` 的去重路径。

## 3. 为什么"按条目去重"会失效（待验证的假设）

`NoteFirstSight` 的去重是 `if (e != nullptr && e->sawFirstSight) { droppedDuplicatePerFrame++; return; }`，
其中 `e` 由查找得到。分区版把它改为 `FindChain(key, Observation)`。

**假设 A（最可能）**：分区后**第一次** `NoteFirstSight` 在 `Observation` 链型上**新建**了条目并发射，
但 `sawFirstSight` 的置位发生在**发射之后**的某条路径上，而该路径**只在"非新建"分支**里执行
（即旧代码里"复用已有条目"与"新建条目"两条路径对 `sawFirstSight` 的处理不同）。
⇒ 新建路径没置位 ⇒ 第二次查找仍看到 `sawFirstSight == false` ⇒ 再发一条。

**假设 B**：`NoteFirstSight` 内部**还有第二处**查找（我在 round 16 的替代只改了每个函数里
"第一处" `Entry* e = Find(key);`），那处仍是 `Find(key)` ⇒ 两条路径查到**不同条目**
（一处查到 Observation 新条目、一处查到旧的拒绝条目），于是去重判据查错了对象。

**假设 C**：`Insert` 在分区后建了第二条条目，但**哈希探测范围/墓碑**处理让 `Find` 只返回
**先插入**的那条（拒绝条目）⇒ 与假设 B 同效。

## 4. 下一步（按"一次一个变量"的原则）

```
① 打印 `NoteFirstSight` 的**全部**查找点（grep 该函数体内所有 Find/FindChain），确认只有一处；
② 在分区版里，用临时诊断打印：新建分支是否置位 sawFirstSight（验证假设 A）；
③ 若为假设 A ⇒ 修正置位路径（**与分区无关的既有缺陷**，应单独修并单独验证）；
④ 若为假设 B ⇒ 先统一查找点，再重新评估分区；
⑤ **分区只改两个入口**（NoteReject / NoteFirstSight），S/E/D 暂留 `Find(key)`，
   把"分区"与"归属规则"两个变量分开，观察 Case 8 是否仍失败。
```

**注意**：③ 若成立，则这是一个**在分区之前就存在的缺陷**（只是旧路径恰好掩盖了它）——
与 round 3 的成本测试夹具同型：**一个缺陷掩盖另一个缺陷**。

## 5. 状态（无代码改动）

```
静态 259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 26/0 ; 读方 OK
候选 DLL : 36,297,544 B  1C85084115534EF3312F259E5AAE4B668A689A00664FFF6C3E89187D71E5F333
站点     : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git      : 无写操作
```

## 6. 不声称

- **不**声称已找到根因（§3 是**待验证假设**，不是结论）；
- **不**声称 ⑤/⑥/⑧ 有进展；**不**声称阶段 C 主体完成；
- 本轮产出：**把 Case 8 的失败从"未定性"推进为"行为差异（去重未生效）"**，
  并给出可执行的三个假设与验证顺序。
