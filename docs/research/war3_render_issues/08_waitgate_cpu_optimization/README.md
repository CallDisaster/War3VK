# Warcraft III WaitGate 与 CPU 降本研究（08）

## 目标
- 用 IDA 证据链解释 `WaitGate` 的真实语义，避免把“等待时间”误判为“可优化热点”。
- 定位主循环活跃段（Active）里真正可降本的阶段，给出可执行优化路线。

## 核心结论（先看）
1. `WaitGate` 不是“逻辑运算热点”，它是引擎调度线程的等待闸门（`WaitForSingleObject`）。
2. 你看到的 `WaitGate` 高占比，主要是统计口径问题：它包含了帧节拍等待（Idle），不是纯计算。
3. 现阶段最值得降本的仍是“渲染提交与桥接路径”，其次才是引擎回调队列本身。

---

## IDA 逆向证据链（1.27.0.52240）

### 1) 入口与主循环归属
- `GameMain`：`0x6F0297D0`
  - 初始化后调用 `GameApplication_Init(0x6F027FB0)`。
  - 随后调用 `j_ResetMemoryManager_05E710 -> ResetMemoryManager_05E710(0x6F05E710)`。
- `ResetMemoryManager_05E710` 内部直接进入：
  - `sub_6F05F710((void*)1)`。
  - 这不是一次性函数，而是引擎主调度循环线程逻辑。

### 2) 调度线程与 WaitGate
- `sub_6F05F710`（核心循环）：
  - 取当前 worker/context：`sub_6F05DE80`
  - 计算下一个到期时间 `v5`（来自调度条目时间戳）
  - 等待路径：
    - `sub_6F158940(...)`（`WaitForSingleObject` 薄封装）
    - 或 `sub_6F1648A0(...)`（Sleep/高精度等待分支）
  - 到期后执行：
    - 回调执行：`sub_6F0603B0`
    - 消息分发：`sub_6F059B00 -> sub_6F05A310`
    - 队列收口：`sub_6F05FD10 / sub_6F05B080 / sub_6F05FC10 / sub_6F05EE90`

- `sub_6F158940(0x6F158940)`：
  - 代码就是 `WaitForSingleObject(*this, dwMilliseconds)`。
  - 明确是等待 API，不是计算工作函数。

### 3) 是否“固定 Sleep”
- `GameApplication_Init` 中调用：
  - `sub_6F057390(2, 0) -> sub_6F05E410(a1=2, a2=0)`。
- `a2=0` 写入 `dword_6FBB8964`，意味着主路径走 `WaitForSingleObject`，不是固定 `Sleep`。
- 结论：当前版本并非“强制固定睡眠锁帧”，而是“事件 + 超时”的调度等待模型。

---

## 为什么 WaitGate 看起来占比很大

## 现象
- 报表里 `War3MainLoop/Engine/WaitGate` 常接近或超过 `Avg CPU Frame` 的 90%。

## 原因
1. `WaitGate` 统计的是主循环节拍中的“等待段”，本质属于 Idle。
2. 一帧内可能出现多次调度等待（`calls/frame > 1`），导致累计值看起来很高。
3. 语义树 strict 视图是“在已归类节点内归一化”，不是“全进程真实 100% CPU”。

## 判定方式
- 优先看这三个数：
1. `Avg Active CPU Frame (ms)`：真正可优化上限。
2. `Avg Process CPU / MainThread CPU / Worker CPU`：线程级真实消耗。
3. `WaitGate/Cycle/Active` 与 `WaitGate/Cycle/Idle`：区分活跃与等待。

---

## 当前可降本热点（按优先级）

## P0（低风险，先做）
1. `Hook_FlushSortedItems/TakeoverFull` 路径收敛：
   - 已观测到其占用高于 Conservative 模式。
   - 建议优先回到 `TakeoverConservative` + 条件晋升 Full（只在“稳定不回退”的场景）。
2. `FQ_Dispatch_Opaque` 调用数继续压降：
   - 通过“同状态连续段”合并减少 dispatch 次数。
3. `Hook_UiRenderableRender` 早退策略再收紧：
   - UI 批次已高频（~100+/frame），每次少量开销累积明显。

## P1（中风险，需要更多证据）
1. `sub_6F0603B0`（RunCallbacks）细分：
   - 区分“脚本回调 / 渲染驱动回调 / 系统回调”。
2. `sub_6F05B080`（QueueFlush）二级拆分：
   - 重点看 `LoadResourceBlock_05AE90` 的 item 类型分布。
3. `sub_6F05A310` case5/case14 触发频率分段：
   - 这两个 case 都会走 `sub_6F059890`（主回调函数指针）。

## P2（高风险，不建议立即）
1. 直接 patch `WaitForSingleObject/Sleep` 参数或替换等待策略。
2. 改调度线程数/优先级策略（`sub_6F05E410` 参数链）。
3. 大范围重排主循环回调调用顺序。

---

## 下一轮逆向计划（可执行）
1. 锁定 `sub_6F05B080` 内 `LoadResourceBlock_05AE90` 的类型分桶。
2. 对 `sub_6F0603B0` 执行回调地址做 TopN 统计（按总耗时、调用次数双维度）。
3. 把 `sub_6F05A310` 的 case 分布接到性能报告（case5/14/其他）并关联 frame jank。

---

## 对当前优化方向的建议
- 不要把 `WaitGate` 当成“直接优化对象”。
- 应把它当作“节拍边界”：  
  `FrameTime ~= ActiveWork + IdleGate`  
  其中真正可提升 FPS 的是 `ActiveWork`。
- 对你当前项目，最实在的收益仍在：
1. 渲染提交路径（Dispatch/Flush）调用次数下降；
2. 合批命中率提升且不破坏透明/描边语义；
3. UI 高频 Hook 的早退与缓存。

---

## 结项判断标准（本专题）
满足以下三条即可认为本专题完成：
1. 团队对 `WaitGate` 语义达成一致：它是等待闸门，不是计算热点。
2. 报告中 `Active CPU`、`Process/Main/Worker`、`Cycle Active/Idle` 三口径一致解释。
3. 优化路线从“追 WaitGate”切换到“压 Active 热点（RenderQueue + 回调队列）”。

