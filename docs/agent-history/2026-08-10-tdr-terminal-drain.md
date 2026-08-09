# TDR P1：终态设备丢失 CPU 排空候选

日期：2026-08-10
范围：D3D9 command-stream 同步、DXVK submission queue 与 presenter 的设备丢失收尾。

## 目标

`VK_ERROR_DEVICE_LOST` 是不可恢复的 device terminal state。本阶段在已知终态后停止新增 Vulkan
submit、present、timeline wait 与 present-wait，同时保持 CPU 侧命令引用、延迟 tracker 和 frame
signal 的完整排空。

## 实现

- `D3D9DeviceEx::SynchronizeCsThread` 在持锁后优先检查 fail-stop。它先让现有 `FlushCsChunk`
  丢弃未派发 chunk，再只等待 `m_csSeqNum`，避免等待 `GetCurrentSequenceNumber` 为非空 chunk 虚构的
  `m_csSeqNum + 1`。
- `DxvkSubmissionQueue` 在 submit 和 finish 前以 `DxvkDevice::getDeviceStatus` 的终态锁存为准。
  已丢失的 command entry 仍进入 finish queue，以 CPU 路径依次 `notifyObjects`、`reset`、回收
  command list；不会等待 timeline semaphore。queue-local error 一旦变为 `VK_ERROR_DEVICE_LOST`，
  后续普通错误不得覆盖它。
- 已知丢失的 present entry 调用 `Presenter::retireTerminalFrame`。该 helper 只清理
  `m_presentPending`、排入带 `VK_ERROR_DEVICE_LOST` 的 frame 与唤醒等待者，不执行 Vulkan/WSI 调用。
  没有 present-wait 时，finish 侧直接完成 signal/tracker。
- frame worker 在 `vkWaitForPresent*` 前检查终态；首次 WSI wait 返回设备丢失后，后续队列帧均只做
  CPU 排空、更新 completed/signal 状态并跳过 FPS limiter。`waitForSwapchainFence` 在任何 WSI fence
  wait/reset 前同样短路。

## 验收边界

本阶段的静态合同覆盖 phantom CS 序号、终态 submit/finish 的 CPU 回收、terminal present frame、
frame-worker/present-fence 的 WSI gate，以及 queue error 不可逆性。构建与离线测试只证明源码候选可编译，
**不证明驱动掉落已经解决**：仍需实际 device-lost 注入或玩家前台复现来验证真实 TDR 后的排空行为。
