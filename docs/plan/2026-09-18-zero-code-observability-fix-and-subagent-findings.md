# 🎯 零代码修复链可观测性（evicted=0，palette 事件 34 倍）+ 子线程 A/B 结论 — 2026-09-18

## 0. 一句话

**关掉 `DXVK_WAR3_FRAME_EVIDENCE_DRAWS`**（kind=18 `DirectionalDraw`，实测占共享环流量 ≈98%），
共享环从"淘汰 745484 条"变成 **`evicted=0`**，palette 事件从导出 107 条变为 **3691 条全部保留**，
并且**首次在实机看到完整的 FirstSight → Enqueued → Drawn 链**。
**这是环境变量层面的修复，未改一行源码。**

## 1. 我先纠正自己的一个错误（子线程 B 指出）

我把 `DXVK_WAR3_FRAME_EVIDENCE_OUTPUT` 当成第四个"域门"设成了 `"0"`。
**它不是域门，是"输出目录绝对路径"**（`war3_frame_evidence.cpp:214`，219 行强制校验长度≥4 且必须 `X:\` 形式）。
设成 `"0"` ⇒ `OutputDirectory()` 返回空 ⇒ `outputPath()` 空 ⇒ **392 行短路** ⇒ 393 行报
`CreateNew export failed; frozen evidence retained`。

⇒ **我那次"减少竞争"的实验根本没有测到假设**；错误在实验设计，不在被测对象。
（真域门只有 `CASTERS` / `DRAWS` / `INPUTS`，代码上**不可能**让导出失败。）

## 2. 子线程 A：机制被完全定位

| 结论 | 证据 |
| --- | --- |
| palette 事件进环**唯一漏斗** | `war3_frame_evidence.cpp:253` `s->ring.append`；实现 `war3_frame_evidence_core.h:57-86` |
| 环**完全不看 label/kind** | `core.h:57-86` 无任何标签判断 ⇒ 被淘汰的永远是最早写入的 |
| 淘汰是**按圈覆盖** | `m_pre = capacity - capacity/4`(`core.h:52`)；`index=(sequence-1)%m_pre`(`:70`)；覆盖非空格即 `m_evicted++`(`:76`) |
| 账目可闭合验证 | `942092 - 745484 = 196608 = m_pre` ⇒ capacity 必为 262144 |
| **无标签 kind=18 = `DirectionalDraw`** | `core.h:19`；唯一生产者 `d3d9_war3_shadow.cpp:5176-5199`；由 **`_DRAWS`** 控制 |
| **palette 头块看不见这种丢失** | `ringEvictedAfterRecord` 只在 `Record()==0` 时累加（`sink.cpp:62-63`），而按圈覆盖**返回正常 sequence** ⇒ 参数字段读数为 0 |
| 四个预算常量是 palette **私有**的 | `war3_palette_object_evidence.h:119/120/124/126`，检查点集中在 `CanEmit()`(841-854) 与 `AccountDrop()`(855-865) |

**候选方案（含影响面）**：A 在 `Store` 内增设专用 palette 环（4096 格 ≈1.5 MiB 即零淘汰）；
B 在共享环内做保留区（**不建议**：会破坏 `accepted == evicted + events` 恒等式，直接打死 `test_frame_evidence_core.cpp:35` 与 `analyze_frame_evidence.py:128`）；
C 记录器自带镜像（**必须抬块版本**，会触发三方读方拒绝）；
**D 关掉 `_DRAWS`** —— 零源码改动。

## 3. 决定性实验：方案 D 成功

```
全开              : events=196608  accepted=622802  evicted=426194  palette-object/v1=107
域精简(DRAWS=0等) : events=19650   accepted=19650   evicted=**0**   palette-object/v1=**3691**
```

`emitted=3691` == 保留 3691 ⇒ **发出即保留、零丢失**（34 倍改善）；`accepted=19650 < 262144` ⇒ 环从未绕圈。

### 3.1 链的阶段也回来了

```
eventCount : 1..101（此前恒为 1）
stages sets: 75 条 ["Drawn"] ／ **32 条 ["Drawn","Enqueued","FirstSight"]**
missingStages: 仅 rejectedStageMissingFromStream（75 条）
```

## 4. 解析器行为被证明是正确的（不是它在捣乱）

`analyze_palette_object_evidence.py:481-486`：

```python
# 版本 3 的**正常观察链**以 FirstSight 为链首，该链**不存在**拒绝事实，
# 因此"缺 Rejected"在此时不是截断。版本 1/2 无 FirstSight 阶段，规则一字不变。
if i_reject is None and i_firstsight is None:
    truncation.append('rejectedStageMissingFromStream')
```

⇒ 有 FirstSight 的 32 条**没有**被标记截断；被标记的 75 条正是**没保留到 FirstSight**的那些。
**解析器已经在按"是否首见链"区分**，符合"不伪造 Rejected"的约束。

## 5. 剩余问题（已派子线程 D 调查）

```
已知：firstSightInserted=107、firstSightEmitted=160、evicted=0、链=107
现象：只有 32 条链在导出中带 FirstSight 事件
问题：零淘汰的前提下，为何 75 条链没有 FirstSight 事件？
```

候选解释（待证）：首见事件在 arm 之前发出因而落在环外；或首次观察走另一条不写 label 的路径；
或解析器按 key 归并时把首见分到别的链；或 stage 编码与 `STAGES` 表不一致。

## 6. 已固化到驱动

`AutoTest/live_contrast_palette_objects.py` 的默认 env 现在包含：

```python
DXVK_WAR3_FRAME_EVIDENCE_DRAWS   = "0"    # 消掉 ≈98% 环流量
DXVK_WAR3_FRAME_EVIDENCE_CASTERS = "0"
DXVK_WAR3_FRAME_EVIDENCE_INPUTS  = "0"
# 注释里明确写了：_OUTPUT 是输出目录路径、不是布尔域门，设 "0" 会让导出失败
```

## 7. 子线程调度（回应你的问题）

| | 任务 | 状态 |
| --- | --- | --- |
| A `4de9539a` | 共享环淘汰机制与独立保留候选 | ✅ 已交付 |
| B `71c90321` | 导出失败分支与域开关语义 | ✅ 已交付（并指出我的错误） |
| C `2faf5d8e` | 对 ③ 的对抗性审计 | ⏳ running |
| D `f988ad34` | 为何 75 条链缺 FirstSight 事件 | ⏳ running |

## 8. 现场

```
d3d9.dll = 基线 F275545B…5CF07FF3（每次运行均 rename-park 恢复并核对）
无 War3 进程；无 dxvk.conf；限帧默认 30 FPS
未提交、未部署、未晋升稳定。
```