# 开发态 GPU 地址绑定与 Device Fault 关联

## 目的

2026-08-11 的“生与死”低视角门触发了一次真实 NVIDIA 设备丢失。`VK_EXT_device_fault`
返回 `VK_INCOMPLETE`，其中包含 `READ_INVALID` 地址 `0x1ea04000`、精度 4096，但仅凭该地址无法
判断它属于 buffer、image、device memory，还是已经解除绑定的旧资源。

Khronos 将 `VK_EXT_device_address_binding_report` 定义为开发/崩溃复盘工具：实现会通过
`VK_EXT_debug_utils` 报告 GPU 虚拟地址区间与 Vulkan 对象的 bind/unbind 关系，可用于识别越界访问和
use-after-free。Unreal Engine 的 Vulkan RHI 同样把该扩展放在 GPU crash debugging 后，并以默认 0
的独立开关保护，不作为正常发布路径。

参考：

- <https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_address_binding_report.html>
- <https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceAddressBindingCallbackDataEXT.html>
- <https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceFaultAddressInfoKHR.html>
- 只读 UE 参考：`Engine/Source/Runtime/VulkanRHI/Private/VulkanExtensions.cpp`，release
  commit `71fe36aac5a8`

## 实现边界

- Meson 选项 `warvk_device_address_binding_report_dev` 默认 `false`。只有独立开发构建才定义
  `WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV=1`、保留 debug-utils instance extension 并启用
  `reportAddressBinding` device feature；DXVK 静态库和 D3D9 DLL 的翻译单元使用同一编译策略，避免
  固定容量 tracker 在模块边界出现不同类定义。
- 正式/default DLL 不创建 address-binding messenger，不启用 device feature，也不读取环境变量绕过
  编译策略。
- 驱动 callback 在独立开发构建中只写固定 16384 项的原子环；默认 Release 构建仅保留一个不可启用
  的占位槽。不分配、不加锁、不记录日志、不调用 Vulkan，也不写 incident 文件。竞争写失败只递增
  有界 drop 计数。设备 feature 在 `vkCreateDevice` 前开放准入，以免遗漏设备创建期间的绑定事件；
  实例没有 `VK_EXT_debug_utils` 时该 feature 强制关闭。
- 环中只保留值语义字段：sequence、bind/unbind、flags、base/size、首个 Vulkan object type/handle。
  object handle 仅作诊断标识，绝不回传给 Vulkan。
- `VK_EXT_device_fault` 查询完成后才在 CPU 侧将地址范围与环中事件关联。fault precision 严格按
  Khronos 公式计算：`lower = address & ~(precision-1)`，
  `upper = address | (precision-1)`；区间和加法均做防溢出处理。
- 最多把最近 32 个重叠事件按 sequence 倒序写入 enrichment incident；同时报告观察、drop、wrap/
  match 截断状态。基础 incident 和 submission/finish terminal drain 顺序不变。

## 本阶段不能证明的事项

该候选只是下一次复现的取证工具，不修复首次非法 GPU 读取，也不证明 TDR 已解决。当前唯一自然 TDR
配额已经耗尽，因此开发 DLL不部署、不启动游戏。下一次获授权的单次复现应先确认 report feature 和
messenger 均启用，再检查 `READ_INVALID` 地址是否命中：

1. 最近 unbind：优先审计资源 retirement/use-after-free；
2. 仍绑定 buffer 且 fault 位于区间边缘外：优先审计 indexed replay range/BaseVertexIndex；
3. image 或 device memory：按对象类型回溯 receiver/CSM/Arena backing；
4. 无匹配：保留 dropped/truncated 证据，不能据此排除生命周期错误。

静态/runnable/编译通过也不能替代真实 device-loss enrichment。
