# E0：宿主 Present 的 Sleep(1) 轮询改为阻塞等待 + 1 ms 定时器请求

日期：2026-11-17。范围：`tools/render_host/dxvk_core/d3d9_backend.cpp`（`waitPresentRetired` 与 Present 的队列等待）。

开关 `WARVK_D3D9_PRESENT_WAIT`，默认关（行为与现状相同）：
- `block`：常开；
- `alternate:N`：按宿主 Present 计数每 N 次切换，奇数块开启。

没有改期限、退休语义或任何故障路径。阻塞等待仍按 10 ms 切片复查 `healthy()` 和原有期限。

## 1. 改法

**现状**：Present 等待提交队列和呈现退休时，用 `Sleep(1)` 轮询。

**开启后**：
1. 队列等待改用 `DxvkDevice::waitForSubmissionUntil(status, min(deadline, now+10ms))`。
2. 呈现退休用 `sync::CallbackFence::setCallback` 设一个 Win32 事件，然后 `WaitForSingleObject` 按切片等待。
3. 开启相位期间由 `TimerResolution` 请求 `timeBeginPeriod(1)`；winmm 只从 System32 加载。
4. 关闭相位调用 `timeEndPeriod`。

A/B 测的是“阻塞等待 + 1 ms 定时器”这个整体。1 ms 定时器在 Windows 11 上按进程生效，也会影响宿主里其它线程的定时等待。

## 2. 同一运行交替 A/B（Lost Temple）

**证据**：`AutoTest/artifacts/x64-e0-present-wait-20261117-lt-ab-r1/`，150 s，`alternate:150`。
- 宿主：`build64_render_host_p0_20261117_dev`（`9ecf4a47b2c8e6c1`）。
- 前端：`b8944a94af92483f`。
- 运行前 CPU 负载约 51%（本机有其它会话负载）。
- 分析：`scratchpad/ab_analyze.py`。

| 指标 | 关（Sleep(1) 轮询） | 开（阻塞 + 1 ms） |
|---|---|---|
| 宿主 Present 数 | 2,674 | 2,550 |
| 队列等待 µs/Present | 1,565 | 334 |
| 宿主 Present 间隔均值 ms | 31.72 | 25.38 |
| 宿主间隔 p50/p95/p99 ms | 27 / 40 / 100 | 25 / 34 / 41 |
| 宿主间隔 >100 ms 次数 | 30 | 5 |
| 前端稳态帧均值 ms（去掉前 3 块和末块） | 34.00 | 26.62 |
| 前端稳态 p50/p95/p99 ms | 28.16 / 38.39 / 84.66 | 26.20 / 34.11 / 39.32 |
| 前端 >900 ms 帧（丢失唤醒停顿） | 9 | 3 |

丢失唤醒的约 1 s 停顿（见 `2026-11-17-x64-lost-wakeup.md`）在两相位间分布不均（9 比 3），会抬高关闭相位的均值和 p99。

扣除这些停顿帧后，均值约为 28.4 ms 对 24.2 ms。p50 的 28.16 对 26.20 不受停顿影响。

## 3. 结论与未做

- 单轮同一运行 A/B 显示：宿主队列等待减少约 79%；扣除停顿后前端帧时间改善约 7–15%。
- 机器负载有噪声，需要重复，并且要在 `WARVK_D3D9_RING_WAKE_FIX` 的影响被拿掉之后再测一次。
- 默认值未改。1 ms 定时器请求会增加宿主进程的功耗和调度频率，是否默认开启由审核决定。
