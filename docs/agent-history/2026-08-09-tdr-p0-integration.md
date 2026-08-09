# 2026-08-09 TDR P0 修复候选交接

## 结论边界

本候选修复了研究报告中能够从源码确定的 Vulkan 非法使用与无界 GPU
工作量路径，并完成独立 Win32 Release 构建。它尚未部署或启动游戏，因此只可称为
“静态与构建通过的 P0 候选”，不能据此宣称四次真实 `VK_ERROR_DEVICE_LOST`
已经通过物理场景验收。

## 已合入的修复

- 方向阴影改为先逐 texel 比较、再过滤比较结果，默认关闭周期性世界坐标
  Poisson 旋转；不再对线性插值后的原始深度做一次二值比较。
- screen-space 与 geometry outline 在开始 rendering 前对整批 replay 做与 CSM
  相同的最终范围/epoch/所有权验证，任一失败整批 fail-closed。
- CSM、receiver copy、AA/TAA、SSAO、体积光、ShaderPack、outline 和点阴影 cube
  使用实际 `War3OwnedImageLayoutState`；创建/重建从 `UNDEFINED` 开始，转换后同步
  `DxvkImage::trackLayout`。点阴影按 24 个 face 独立追踪，neutral cube 首次采样前
  清为深度 1.0。
- caster bias 热改和 ShaderPack reload 的 pipeline 改由 command list 持有，GPU
  完成后才销毁。Release 默认编译禁用 raw SPIR-V ShaderPack，无环境变量绕过。
- Vulkan device lost 后 D3D9/WarVK 进入不可逆 fail-stop：停止新命令、资源创建和
  WarVK pass，防止 fence/Arena/现场继续被污染；这不是进程内设备恢复。
- 新增当帧共享 GPU 工作量门：主 CSM 优先，之后才是 volume-sun 与 point shadow；
  按 draw/vertex/index checked arithmetic 原子预留。拒绝发生在绘制前，不发布半个
  cascade 或 cube；点阴影只有完整同代六面身份完全相同时才能沿用上一份结果。
- Shadow Arena 在可信 `VK_EXT_memory_budget` 下把总驻留上限收紧为固定上限、
  `available/4`、`available-512 MiB` 三者最小值。它只拒绝新 64 MiB 页，不清理、
  回绕或复用 GPU 在途页；预算不可信时保持原 1.125 GiB 硬上限。
- 体积光准入同时核算 ray segments、方向 D32、点阴影 cube 和全分辨率 composite
  纹理读取。1080p 发布默认最坏模型为 329,832,000 work units，可通过 320 Mi
  上限；1440p 默认与 4K 高压配置在任何资源创建、copy、UBO 更新或 draw 前整段
  fail-closed。

## 验证

- 分支：`codex/tdr-p0-20260809`
- 源码提交：`294803a4aa80e87a87b816d917ac7e454c562e76`
- 全新构建目录：`E:\Work\WarVKBuild\tdr-p0-20260809-1840`
- Win32 Release DLL：34,132,237 bytes
- DLL SHA-256：`01086A2616045212564044249D311A2E1FCFD03AF9D92888D293D825F57A0895`
- 静态脚本：85/85 通过
- Win32 Meson runnable：27/27 通过
- `ninja -n src/d3d9/d3d9.dll`：no work
- `git diff --check`：通过

## 尚未并入或尚未证明

- 当前系统未注册 `VK_LAYER_KHRONOS_validation`，因此没有声称通过 sync/GPU-assisted
  validation。
- `VK_EXT_device_fault` 已完成独立设计与模拟合同，但生产翻译单元未完成验证，故未
  合入。本候选的 incident 仍不会包含驱动 fault address/vendor binary。
- 共享 shadow governor 与体积 shader admission 仍是两个独立预算域；后续可以统一
  成全帧预算，但本候选已保证各自不发布半成品。
- 1440p/4K 的高负载体积光会被安全跳过，这是明确的稳定性降级，不是最终性能优化。
- 必须用可见桌面真实地图复测；在完成前不能更新 Release 或关闭 TDR Issue。

