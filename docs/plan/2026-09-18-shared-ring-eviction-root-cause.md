# 🔴 根因更正（第二次）：共享环淘汰，不是每帧预算 — 2026-09-18

> 我上一轮把"实机链只剩终态"归因于**每帧预算**。做完结构排查后，**这个归因是错的**。
> 本文给出更正后的机制。两轮里我在这同一件事上错了两次（先是"链全部走完"，再是"每帧预算"），
> 所以这次把证据摆全。

## 1. 排查方法：先找事件到底在哪

我此前一直在 `paletteObject` 块里找事件 —— **找不到**。实际结构是：

```
root keys : [accepted, capabilities, capacity, captureComplete, effectiveConfiguration,
             events, evicted, paletteObject, postRemaining, processId, processNonce,
             producerLosses, qpcFrequency, reason, reserved, rootCauseReady, schema,
             session, state, triggerSequence]

paletteObject block keys : [counters, version, watchCount]      <-- 只有计数，没有事件
⇒ 事件在 **root["events"]**
```

⇒ 教训（第六条）：**"我找不到数据"时，先确认我在找的地方对不对**，不要急着解释缺失。

## 2. 实际账目

```
root events   : 196608
accepted      : 942092
evicted       : 745484        <-- 环淘汰了近 4/5
capacity      : 262144
postRemaining : 0

labels 分布：
  (无标签)                193248
  ShadowReceiver/VolumetricLight/SSAO/AA/ShaderPack/PipelineExecute   各 ~463
  PresentEntryCounterObserved  462
  skin-selection/v1          8
  palette-object/v1        108      <-- 只有 108 个！
```

## 3. 更正后的机制

**palette 事件与约 94 万个其他取证事件共用同一个环。**

```
palette 计数器 emitted = 3692      （记录器"发出"了这么多）
导出里 palette-object/v1  = 108     （只有这么多活下来）
=> 约 3584 个 palette 事件被环淘汰
```

幸存的 108 个恰好等于 `terminalEmitted` / `closedWindowExpired` ——
因为**终态在窗口关闭时最后发出**，位于环尾，所以躲过了淘汰。
这就是 `eventCount=1`（每条链只剩终态）与 `drawnFlagWithoutDrawnEvent` 的来源。

### 每帧预算 vs 环淘汰：两者都在，但主因是后者

| 机制 | 常量 | 在本案的作用 |
| --- | --- | --- |
| 会话总预算 | `kNormalBudget = 4096-512 = 3584` | 把 palette 非终态事件**封顶在 3584**（`emitted=3692` 正好 = 3584 + 108 终态，账目吻合） |
| 每帧预算 | `kPerFrameBudget = 64` | 限制**突发**，但本案事件率平缓，不是主要限制 |
| **共享环淘汰** | `capacity = 262144`，accepted 942092 | **主因** —— 把已发出的 3584 个 palette 非终态事件又淘汰掉，只留最后一小段 |

## 4. 为什么"迟 arm"救不了

```
事件接受速率 : 942092 / ~120s ≈ 7850 /s
环保留窗口   : 262144 / 7850 ≈ 33 秒
链路时间跨度 : FirstSight 在对象出生（地图加载早期）
               终态在窗口关闭（整局之后）
=> 链的跨度 >> 33 秒保留窗口  =>  **结构上不可能同时看到链的首尾**
```

⇒ 我上一轮把 `ARM_LEAD_SECONDS` 调到 20 秒，方向对但**不可能奏效**：
无论何时 arm，先发生的事情（FirstSight）总会先被挤出环。
这也解释了我"排除环容量假设"的推理错在哪 —— 我改的是**容量**（8192→262144），
但没改**竞争压力**；容量涨 32 倍，竞争压力没变，保留窗口仍只有约 33 秒。

## 5. 可行的修复方向（**未实施**）

| 方向 | 说明 | 代价 |
| --- | --- | --- |
| A. 独立保留 | 让 palette 链**不依赖共享环**：链在 watch 表里维护，关闭时把整条链快照到一个专用结构直到导出 | 需要改记录器与导出路径；语义变更需论证 |
| B. 减少竞争源 | 取证运行时**关掉其他取证域**（ShadowReceiver/SSAO/AA/Volumetric/PipelineExecute 等） | 属驱动/env 层，最便宜；但要确认这些域是否有独立开关 |
| C. 缩小链跨度 | 让一个"窗口"覆盖更短的时间（例如按帧窗口而非整局） | 改变"窗口"语义，影响拒绝恢复链的既有含义 |
| D. 如实记为已知边界 | 承认"实机只能观测到终态 + 标志位" | 零代价，但 ③ 的实机可观测性不闭合 |

我倾向**先做 B 的可行性确认**（查是否有分域开关），因为它是驱动层、零源码风险；
若没有分域开关，再评估 A。**不会为了好看去改预算或放宽解析器。**

## 6. 另一件必须坚持的事

解析器全程**没有**把这些截断链说成"正常完成"：它报 `Uncovered` / `chainComplete=false` /
`stageStreamTruncated` / `drawnFlagWithoutDrawnEvent`。
**判据是诚实的；不诚实的是我前两轮的归因。**

## 7. 现场

```
d3d9.dll = 基线 F275545B…5CF07FF3；无 War3 进程；无 dxvk.conf；停放残留已清理
限帧已接入（下次取证运行默认 30 FPS）
未提交、未部署、未晋升稳定。
```