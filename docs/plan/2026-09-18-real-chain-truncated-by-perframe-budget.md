# 🔴 实机链不完整：**每帧预算丢弃**导致导出只剩终态 — 2026-09-18

> 我必须更正上一轮的结论。用**计数器**推断"109 条链全部走完"是**错的**：
> 计数器统计的是**发出**的事件，而导出里实际**保留**的事件是另一回事。

## 1. 排除环容量假设（做了实验）

第一版怀疑是环太小（`capacity=8192` vs 610 万事件），于是改成：

```
ARM_LEAD_SECONDS = 20      # 临近触发才 arm，避免被冲刷
ARM_CAPACITY     = 262144  # 上界（war3_frame_evidence.cpp:326）
```

结果：导出从 3.4 MB 涨到 **108 MB**（环确实保存了更多），**但链仍然不完整**。
⇒ **环容量不是根因**。

## 2. 真正的根因：每帧事件预算

```
counters: firstSightInserted = 108    firstSightEmitted = 160    terminalEmitted = 108
          droppedPerFrame    = 13823  droppedPerSession = 331477
          emitted            = 3692

chains = 108     recovered = 0
covered          = False   (108/108)
chainComplete    = False   (108/108)
stageObservationComplete = False (108/108)
conclusion       = Uncovered (108/108)
terminal         = WindowExpired (108/108)
stages           = ["Drawn"] (108/108)
eventCount       = 1         (108/108)   <-- 每条链在导出里只有 1 个事件

missingStages:
  rejectedStageMissingFromStream  108
  drawnFlagWithoutDrawnEvent       69
  submitFlagWithoutEnqueuedEvent   33
  chainSequenceGap                 69
```

**解释**：生产采集点（`d3d9_device.cpp:23505`）对**每个可渲染部件、每一帧**都触发，
事件率远超记录器的每帧预算。记录器按预算丢事件，只有**终态**因预留槽位（`droppedTerminalReserve`）幸存。
于是每条链在导出里只剩 **1 个事件**（终态），而 `Drawn` 只是一个**标志位**
（解析器的 `drawnFlagWithoutDrawnEvent` 正是识别这件事的）。

## 3. 这意味着什么

| 说法 | 判定 |
| --- | --- |
| 我上一轮"109 条链全部走完" | ❌ **错误** —— 那是**计数器**（发出量），不是**导出保留量** |
| 环容量是根因 | ❌ **已实验排除**（262144 后仍不完整） |
| 每帧预算丢弃是根因 | ✅ 有 `droppedPerFrame=13823`、`eventCount=1`、`drawnFlagWithoutDrawnEvent` 三重佐证 |
| 有版本正常观察链**已实现** | ✅ 写方/读方/测试同步，宿主机场景 G 端到端通过 |
| 有版本正常观察链**在实机可端到端观测** | ❌ **不成立** —— 中间阶段被预算丢弃 |

### 3.1 这不是"造假"，恰恰相反

解析器**没有**把这些链美化成"正常完成"：它报 `Uncovered` / `chainComplete=false` /
`stageStreamTruncated` / `drawnFlagWithoutDrawnEvent`。
⇒ **fail-visible 的判据是有效的** —— 它拒绝把"标志位"当成"事件"，拒绝把截断流当成完整链。
这正是复审要的那种诚实性。

### 3.2 为什么宿主机场景 G 通过、实机不通过

场景 G 是**受控写入**：少量事件、无预算压力 ⇒ 链完整。
实机是**每帧每部件**触发 ⇒ 预算丢弃 ⇒ 链被截断。
两者不矛盾：G 证明**协议与读写对称性**，实机暴露**记录预算下的可观测性上限**。

## 4. 结论与下一步

**③ 的状态修正**：

```
写方实现        ✅   版本 3、生产接线 NoteFirstSight
读方/解析器     ✅   注册 v3、按版本 counters、双读方接受实机导出
测试同步        ✅   套件绿 + 跨语言门禁 + 场景 G 端到端
实机端到端可观测 ❌   中间阶段被每帧预算丢弃，导出只剩终态
```

**下一步的候选方向**（未实施）：

1. 查记录器的每帧预算常量与预留策略，评估在"首见链"上提高预算或为首见/入队也留预留槽是否可行；
2. 或者承认这是低开销取证的固有限制，把"实机只能观测到终态 + 标志位"作为**已知边界**记录，
   而不是继续宣称链完整。

⚠️ 我倾向于**先如实记录**，不急着改预算 —— 改预算会影响取证开销，属于需要单独论证的改动。

## 5. 现场

```
d3d9.dll = 基线 F275545B…5CF07FF3（每次运行均由 rename-park 恢复并核对）
实机导出：cpu-10488-20682819178-1.json (108 MB, 本次)；cpu-30772、cpu-33280 (前两次)
未提交、未部署、未晋升稳定。
```