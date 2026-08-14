# WarVK v1.21.00

WarVK 1.21.00 是面向 Warcraft III 1.27a 的稳定功能更新。本版集中修复高压阴影生产、
Stage11 静态 Caster 闪烁和 Vulkan device-lost 后的终态处理，并正式整合 Froxel 体积光、
局部体积雾和方向体积阴影 Guide。

## 主要更新

- Directional CSM 使用 compare-first PCF、固定对称核、receiver-plane 每 tap 深度修正及跨级联
  一致的 alpha cutoff，减少树叶、草地和细线阴影的规则爬动。
- Stage11 活跃静态工作集受 GPU/scene 引用保护；producer completeness 与 replay validation
  阻止半份 Caster 列表清理或覆盖完整阴影图。
- Defrag 后在命令录制点重新解析当前 buffer backing，避免 replay 绑定旧物理资源。
- 用户已复测不死族/暗夜精灵建造动画阴影和点阴影摩尔纹修复。
- 新增最多 8 个 Sphere/Box/Cylinder 局部体积雾区域；Froxel High 成为默认体积后端。
- 方向体积阴影使用独立 base/refined Guide 和全分辨率 depth-guided composite，改善小 Caster
  阴影柱的连续性。
- Vulkan terminal drain、shader/pipeline fail-stop、真实 device-lost provenance 与有界
  `VK_EXT_device_fault` 文本诊断闭合；不采集 vendor binary。
- exact-owner prefilter 消除 DirectGrouped 重复 packet 构建。同场景 A-B-B-A 中主线程 CPU
  降低 `0.357 ms`（`5.82%`），Populate/DirectGrouped/BuildEligible 分别降低
  `76.27% / 87.42% / 96.38%`。
- 32 位性能历史限制为 4000 帧，避免长时间采集耗尽 Warcraft III 地址空间。

完整变更见 [CHANGELOG.md](../CHANGELOG.md)，安装、版本规则与使用说明见
[README_CN.md](../README_CN.md) / [README.md](../README.md)。

## 运行要求

- Warcraft III 1.27a（32 位）
- Windows 10 或 Windows 11
- 支持 Vulkan 1.3 的显卡与最新官方驱动

## 兼容性

- 产品/JAPI 显示版本为 `1.21.00`。
- 外部 Shader API 数字版本保持 `1.2.0`。
- JASS 线协议保持 `warvk:v1`，已有地图脚本无需迁移。
- WarVK 由游戏启动前安装的代理 `d3d9.dll` 提供，不包含地图内 DLL Loader。

## 已知边界

- 同进程跨地图仍不作为本版本承诺；退出地图后请完整退出并重启 Warcraft III。
- Issue #5 提前联合剔除仍默认关闭；本版已经降低 producer CPU 开销，但没有宣称所有前端剔除完成。
- 最坏 1440p/4K Froxel 请求可能安全回退 Legacy。
- 极细 alpha silhouette 仍可能出现少量亚像素变化。

## 验证摘要

- 216/216 静态脚本、50/50 Win32 runnable、Fresh Win32 Release 构建及 no-work 通过。
- 3901 帧体积光 smoke 与 603 秒“生与死”低视角压力门均无 device lost、Arena overflow、
  producer incomplete 或 partial publication。
- 上述隔离桌面数据只证明稳定性和相对 A/B；最终前台体验以玩家相同地图/相机测试为准。

本文件是发布正文草案。GitHub Release、标签和最终资产需在用户审核后创建。
