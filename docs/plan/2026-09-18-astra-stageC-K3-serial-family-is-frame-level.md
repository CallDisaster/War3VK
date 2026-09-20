# 阶段 C · K3 §4①②③ 定案：序号**同源**，但**帧级** ⇒ 仍不能作 attemptSerial — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**只读侦察，本轮无代码改动。**

## 1. 核实结果（赋值点）

```
d3d9_device.cpp:21516:  draw.shadowRecordFrameSerial = packet.renderable.frameSerial;
d3d9_device.cpp:13711:  result.frameSerial           = m_war3ShadowPersistentFrameSerial;
d3d9_device.h:2593:     uint64_t m_war3ShadowPersistentFrameSerial = 0;   // 多处 +1u 递增
```

⇒ **服务路径（`:22142`）与拒绝路径（`:6825`）用的是同一个量**（`renderable.frameSerial`）
—— **同源**，这两点没有分歧。

⇒ 三者同属一个 **`m_war3ShadowPersistentFrameSerial` 族**（设备级）。

## 2. 两个结论

### 2.1 首见路径确实是三者中的异类（round 34 的怀疑**加强**）

| 构造点 | 实参 | 族内？ |
| --- | --- | --- |
| `:22142` NoteServed | `draw.shadowRecordFrameSerial` = `renderable.frameSerial` | ✅ |
| `:6825` NoteReject | `renderable.frameSerial` | ✅ |
| `:23516` NoteFirstSight | **`manifestFrame`** | ❌ **异类** |

**但我不据此断言"接错线"** —— 只断言：**三者不一致，且首见路径是唯一的异类**。
是否应改为 `entry.exactSubmittedFrameSerial`（`m_war3ShadowPersistentFrameSerial` 族）
**需要独立裁定**（理由见 §2.3）。

### 2.2 ★ 但这个序号族**不能**作 attemptSerial

`m_war3ShadowPersistentFrameSerial` 是**设备级帧序号**（多处 `+1u`，与

`m_war3SceneRotatedFrameSerial` 等并列），`renderable.frameSerial` 由其派生。

⇒ 它的粒度就是**帧**。用它当 attemptSerial ⇒ **等价于每帧清零** ⇒
**正是裁定"不采用每帧清零"明令禁止的做法**。

⇒ **K3 不能用这个序号族。** 我 round 32 与 round 34 两次希望"复用现成序号"的路线，
至此**都被证据否掉**（第一次因为不同体系，第二次因为粒度是帧）。

### 2.3 因此首见路径的"异类"要**独立裁定**，不能顺手改

有两种解释，**证据不足以二选一**：

| 解释 | 后果 |
| --- | --- |
| **接线错误**（本应传提交序号） | 是一条**独立缺陷**；且修正后三处同族（仍是帧级，对 K3 无帮助） |
| **有意为之**（manifest 帧在那里才是"当时可得的最诚实来源"） | 不是缺陷；`:23517-23519` 的注释显示本处对"拿不到某帧"有**明确的诚实性纪律**，支持这种可能 |

⇒ **不得**为了"让 K3 好写"而顺手改它。若要做，**单独修、单独验证、单独出证据**。

## 3. K3 剩下什么

`attemptSerial` **必须**是一个**非帧级**的、由调用方在一次明确关联尝试开始时给出的量。
round 33 §4 的三候选现在只剩：

| 候选 | 状态 |
| --- | --- |
| A `draw` 的身份 | **仍可行**（draw 是一等对象，不随帧"清零"） |
| B 一次 shadow caster **候选集合** | **仍可行**，但需先核实首见路径 `entry` 与集合的对应 |
| ~~C 显式递增计数器~~ | 仍可行，但要新增状态；**优先度低于 A/B**（A/B 更贴近"一次尝试"的语义） |

**下一步**：读 `d3d9_device.cpp:22100-22140` 与 `:23470-23505` 的循环边界，
判定"同一 draw / 同一候选集合"在两条路径上是否**可被识别为同一个东西** ——
这直接决定 A 还是 B。

## 4. 状态（无代码改动）

```
STATIC=259/0 ; meson 85/0 ; no-work ; wire CHECKS=1160 FAILURES=0 ; evidence 30/0
分析套件 105 OK ; DLL 未变（01230C1F…）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 不声称

- **不**声称首见路径"接错线"（只断言**三者不一致且它是异类**；§2.3 的两种解释证据不足）；
- **不**声称 A/B 候选可行（**未核实**同一 draw / 同一集合在两条路径上可否识别为同一物）；
- **不**声称 K3 有进展（无代码改动，终态仍 Unclosed）；
- **不**声称窗口维度已进入查找键；**不**声称阶段 C 主体完成；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。