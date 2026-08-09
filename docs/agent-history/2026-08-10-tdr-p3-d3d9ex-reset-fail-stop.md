# TDR P3：D3D9Ex Reset 与 swapchain 的终态 fail-stop 候选

日期：2026-08-10

## 实现

- `D3D9DeviceEx::ResetEx` 在取得既有 device lock 后、任何参数验证、swapchain reset 或 GPU-skin
  device-epoch 操作之前检查 `CheckVulkanDeviceLostFailStop`。终态直接返回
  `D3DERR_DEVICEREMOVED`，不尝试在原逻辑设备上恢复。
- `CreateAdditionalSwapChainEx` 保持 `InitReturnPtr` 与空指针 `D3DERR_INVALIDCALL` 的优先级；通过后
  才检查 fail-stop。终态调用返回 `D3DERR_DEVICEREMOVED` 并保持非空输出参数为 null，早于 presentation
  field 检查、隐式 swapchain invalidation 和额外 swapchain 分配。
- 原有的非 Vulkan `IsDeviceLost()` 分支保持不变，用于历史 D3D9 focus/lost 语义。

## 验收边界

静态合同证明两处 gate 的顺序及终态输出行为。离线测试和 DLL 构建不构成真实 device-lost 注入、玩家
前台或驱动恢复验证；本阶段不能据此宣称 TDR 已修复。
