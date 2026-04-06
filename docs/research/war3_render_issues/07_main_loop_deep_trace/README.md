# Warcraft III 主循环深度追踪（07）

## 目标
- 还原 `GameMain -> GameApplication_Init -> Engine Loop` 的真实执行链路。
- 找到 `Other/Untracked` 的主要来源，避免继续在渲染层盲目优化。

## 已确认主链（1.27a）
- `GameMain`：`0x6F02984A`
  - 入口初始化后调用 `GameApplication_Init`，成功后进入 `sub_6F027660`。
- `sub_6F027660`：`0x6F027660`
  - 主要是退出/清理链路，不是逐帧主循环本体。
- 引擎主循环线程函数：`sub_6F05F710`（IDA 已还原）
  - 循环中依次执行：
  1. 选择 worker/context：`sub_6F05DE80`
  2. 等待/调度唤醒：`sub_6F158940` / `sub_6F1648A0` / `sub_6F05DEE0`
  3. 到期回调处理：`sub_6F0603B0`
  4. 消息泵分发：`sub_6F059B00 -> sub_6F05A310 -> case5/case14 -> sub_6F059890`
  5. Tick 收口：`sub_6F05FD10` / `sub_6F05B080` / `sub_6F05FC10` / `sub_6F05FB10`
  6. 任务重排：`sub_6F05EE90`

## 关键结论
- `EventPump/Dispatch` 只是主循环中的一个阶段，不能代表全部 CPU 开销。
- `Other/Untracked` 的主体更可能来自 Engine Loop 内部的回调处理与队列收口阶段。
- 深层 Wait API（尤其 `NtWait*`）可用于佐证等待行为，但不应作为首要优化方向。

## 已落地追踪点（代码）
- 新增可观测主循环阶段（透传 Hook，不改逻辑）：
  - `War3MainLoop/Engine/TlsPump` (`0x057470`)
  - `War3MainLoop/Engine/SelectWorker` (`0x05DE80`)
  - `War3MainLoop/Engine/RunCallbacks` (`0x0603B0`)
  - `War3MainLoop/Engine/QueueFlush` (`0x05B080`)
  - `War3MainLoop/Engine/FinalizeTick` (`0x05FB10`)
  - `War3MainLoop/Engine/Reschedule` (`0x05EE90`)

## 开关
- `kNativeMainLoopDeepPhaseHookEnabled`
  - 位置：`src/d3d9/war3/core/war3_internal_test_config.h`
  - 说明：主循环深度阶段 Hook 总开关，仅计时透传。

## 下一步
1. 跑 30~60 秒性能窗口，确认 `War3MainLoop/Engine/*` 是否显著吞掉 `Untracked`。
2. 若 `RunCallbacks/QueueFlush` 占比高，继续向其子调用扩展二级分段。
3. 若依旧存在大块 `Untracked`，转向 `sub_6F05F710` 周边未命名调用点做 IDA 递进拆分。
