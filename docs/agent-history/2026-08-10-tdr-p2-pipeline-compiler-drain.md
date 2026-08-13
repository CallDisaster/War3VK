# TDR P2：终态设备丢失后的编译器排空候选

日期：2026-08-10

## 范围

本阶段只处理已被 `DxvkDevice` 全局终态锁存的 `VK_ERROR_DEVICE_LOST`。它不根据
`vkCreate*Pipelines` 的编译结果推导 device loss，也不修改管线对象布局、shader ABI 或内建初始化管线。

## 实现

- `CreateVertexShader` 与 `CreatePixelShader` 在各自既有输出指针约定完成后检查 fail-stop，并在终态
  直接返回 `D3DERR_DEVICEREMOVED`，不再创建/补丁字节码、分配 shader 或注册编译任务。
- `DxvkDevice::registerShader` 和 `requestCompileShader` 在进入 PipelineManager 前检查全局终态，确保
  `requestCompileShader` 不会在终态下调用 `shader->notifyCompile()`。
- Pipeline worker 的两个入队接口在持有原锁时、启动 worker/计数/获取 graphics pipeline 引用之前
  拒绝终态请求。已出队任务在实际 compile 调用前再次检查：library 任务只计入完成，graphics 任务
  额外精确释放一次已持有的 pipeline 引用，然后同样计入完成。

## 验收边界

静态合同覆盖 D3D9 输出指针顺序、设备包装器、worker 入队边界和已出队 graphics 引用的恰好一次释放。
构建通过仅说明 CPU 终态排空源码候选闭合；本阶段没有执行真实 device-lost 注入、玩家前台或驱动恢复测试，
不能据此宣称 TDR 已修复。
