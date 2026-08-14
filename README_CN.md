# WarVK 1.21.00

![平台](https://img.shields.io/badge/platform-Windows-lightgrey.svg)
![Vulkan](https://img.shields.io/badge/Vulkan-1.3+-red)
![Warcraft III](https://img.shields.io/badge/Warcraft%20III-1.27a-gold)

[English](README.md) · [更新日志](CHANGELOG.md) · [WarVK JAPI](WarVK/README.md)

WarVK 是面向 Warcraft III 1.27a 的画面增强运行时。它以 DXVK 派生的 D3D9 到 Vulkan 后端为基础，为经典客户端加入现代太阳阴影与点阴影、体积光与局部雾、后处理、运行时诊断，以及供地图作者使用的 JASS API。

1.21.00 是 1.x 系列的新功能小版本，集中解决阴影生产重复开销、Stage11 静态 Caster 闪烁、Vulkan device-lost 后的终态处理，并正式整合新一代 Froxel 体积光和有界局部雾。

> [!IMPORTANT]
> 安装前请把 NVIDIA、AMD 或 Intel 显卡驱动更新到最新官方版本。当前运行时请求 **Vulkan 1.3**（完整覆盖 Vulkan 1.2）；只暴露 Vulkan 1.2 而不支持 1.3 的旧驱动不满足本构建要求。

> [!NOTE]
> WarVK 优先保证画面质量与资源生命周期正确性。4096 CSM、点阴影、Froxel 体积效果与完整后处理都会产生真实的 CPU/GPU 开销。

## 版本编号

从 1.21.00 开始，版本号按 `1.小版本.修复版本` 使用：

- `1.x` 表示当前大版本架构系列；
- `1.21` 表示第 21 个功能小版本；
- 末尾 `00` 表示该小版本的首个稳定发布；后续纯修复依次为 `1.21.01`、`1.21.02`；
- 下一次带来成组新功能或正式性能路线时进入 `1.22.00`。

此前的 `1.2003` 使用旧编号规则。对外 Shader API 数字版本仍为 `1.2.0`，JASS 线协议仍为 `warvk:v1`；本次产品版本升级不要求地图作者迁移已有调用。

## 1.21.00 重点更新

### 阴影正确性与画质

- Directional CSM 改为逐 texel 比较后过滤的 compare-first PCF，使用固定对称核、receiver-plane 每 tap 深度修正和跨级联一致的 alpha cutoff，减少树叶、草地与细线阴影的规则爬动和级联轮廓跳变。
- Stage11 exact producer 保护当前活跃静态工作集，并以显式的 producer completeness 合同原子发布整份阴影；高压场景不再因缓存自我淘汰而周期性出现整批 Caster 阴影撕裂。
- Warcraft Transparent Type0 建造附件使用子模型当前 draw、精确 VB/IB/UV 与完整矩阵调色板。用户前台复测确认不死族和暗夜精灵建造动画阴影连续。
- 点阴影统一径向 receiver 深度域并修正 bias；用户前台复测确认此前地面和单位上的摩尔纹/连续条带不再复现。
- replay 在录制 Vulkan 命令前重新解析逻辑 buffer binding；资源 defrag/relocation 后不会继续绑定 capture-time 的旧物理 `VkBuffer`。
- CSM 深度、矩阵、资源 generation 和 receiver 参数作为完整 publication 一起生效；验证不完整时不会发布半份阴影。

### 体积光与局部雾

- 新增有界局部体积雾：最多 8 个 Sphere、Box 或 Cylinder 区域，可与全局介质独立组合。
- 新增 Froxel Medium/High；High 为默认体积后端，使用全视图对数 Z 分层、场景深度终止和受预算约束的光学积分。
- 方向体积阴影使用独立 base/refined Guide，在全分辨率 scene depth 上重建小 Caster 阴影柱；真实几何断层与体积阴影边缘由不同证据保护。
- 所有 Froxel 图片与 layout 发布保持事务式；预算、格式或资源证明不足时整套安全回退到 Legacy，不提交无界 GPU 工作。1080p 默认请求可进入，1440p/4K 的最坏请求允许按 admission 回退。
- 地图作者可控制体积光、全局高度雾和局部雾区域；公共 JAPI 仍使用有界句柄、有限数值和固定参数合同。

### 稳定性与诊断

- Vulkan logical device 一旦进入 `VK_ERROR_DEVICE_LOST`，D3D9 Reset/ResetEx、提交、Present、frame worker 和 pipeline compiler 不再尝试在旧设备上继续创建或提交 GPU 工作，而是完成 CPU 侧有界退役并返回 removed 语义。
- 真实 Vulkan 返回值与 synthetic fail-stop 分开记录；支持时会一次性采集有界 `VK_EXT_device_fault` 文本信息，不在 submission 线程等待，也不采集 vendor binary。
- Shadow Arena 保持 64 MiB 页、384 MiB/代际和 1.125 GiB 总上限；事务预留、GPU fence、map/device epoch 与最终 replay 验证继续 fail-closed。
- 32 位性能历史最多保留 4000 帧，避免长时间采集侵占 Warcraft III 地址空间；累计 workload 与错误计数仍覆盖完整运行时间。
- 发布构建在编译期关闭 legacy `warvk:cmd` 与未验收的 Consume/开发 observer 路线，环境变量不能绕过发布冻结。

### CPU 性能

- exact-owner publication 会在 DirectGrouped 进入昂贵 packet 构建前排除已由 earlier producer 完成的对象，并消除 resolved replay 的重复整表复制。
- 同场景 A-B-B-A 中，主线程 CPU 由 `6.135 ms` 降至 `5.778 ms`（`-0.357 ms / -5.82%`）；Populate `-76.27%`、DirectGrouped `-87.42%`、BuildEligible `-96.38%`。
- 上述数字证明生产者 CPU 路线收益，不等同于所有地图的绝对 FPS 承诺；体积效果、GPU 工作量、地图内容和物理前台状态仍会影响最终帧率。
- 提前联合剔除、Persistent Package、ReBAR、CPU-MT 蒙皮和 Canonical Queue Takeover 仍未达到发布门，1.21.00 不会用未验证路径换取表面帧率。

完整内容见 [CHANGELOG.md](CHANGELOG.md)，地图作者接口见 [WarVK/README.md](WarVK/README.md)。

## 运行要求

- Windows 10 或 Windows 11
- 支持 Vulkan 1.3 或更高版本的显卡与最新官方驱动
- Warcraft III 1.27a（32 位）

WarVK 只针对经典 1.27a 可执行文件与已验证的 `Game.dll` 布局。未知签名会安全拒绝 Hook，不会扫描猜测地址。

## 安装方法

1. 备份 Warcraft III 目录，尤其是已有的 `d3d9.dll`。
2. 将玩家发行包中的 `d3d9.dll` 复制到 `war3.exe` 同级目录。
3. 启动游戏，确认 `d3d9.log` 中出现 `DXVK: 1.21.00`。
4. 按 `Ctrl + F1` 打开或关闭 WarVK 设置面板。

WarVK 不是地图运行时 Loader。玩家目录只需要发行包明确列出的文件；作者包中的 JASS/YDWE Catalog 用于制作地图，不应整包复制到游戏目录。

## 常用设置

- 解锁帧率和后处理
- 太阳 CSM、阴影过滤与 TAA 模式
- 点光源与点阴影
- Legacy / Froxel Medium / Froxel High 体积后端
- 全局高度雾与局部体积雾
- 抗锯齿、Bloom、曝光与描边

## 已知边界（1.21.00）

- 同一 Warcraft III 进程退出地图后再进入其他地图仍未完成正式发布验收，可能出现持续性能下降、阴影异常或资源生命周期问题。建议退出地图后完整退出 Warcraft III，再重新启动并进入下一张地图；该问题继续由 [#6](https://github.com/CallDisaster/War3VK/issues/6) 跟踪。
- Issue #5 的提前联合剔除仍为默认关闭的开发观察路线。1.21.00 已消除本轮确认的 producer 重复工作与活跃缓存颠簸，但没有宣称 terrain/static/skinned 的前端剔除已经完成。
- 1440p/4K 的最坏 Froxel 请求可能因有界 admission 自动回退 Legacy。这是防止无界 GPU 工作的稳定性策略，不表示显卡或驱动故障。
- 极细树叶、草线和远距离 alpha silhouette 仍可能有少量亚像素变化；本版已移除已确认的周期旋转、错误深度过滤和跨级联 alpha 差异，但不以 TAA 历史拖影掩盖剩余运动。
- 隔离桌面数据只用于稳定性和相对 A/B；玩家前台绝对 FPS 应以相同地图、相同相机和相同设置测试。

## 故障排查与反馈

- 启动黑屏通常来自显卡驱动过旧，或游戏目录中存在冲突的第三方 `d3d9.dll`。
- 性能不足时优先降低或关闭体积效果，其次减少点阴影，再降低后处理。太阳 CSM 默认保持 4096，除非运行时发生安全的锁存式分配回退。
- 不要在同一游戏目录同时放入多个 D3D9 代理 DLL。
- 反馈时优先提供 `d3d9.log`、`runtime_status.json`、GPU incident JSON 和 WarVK 崩溃转储；公开前请移除个人路径或私有地图信息。
- 跨地图报告请说明地图进入顺序；普通单地图问题请从全新 Warcraft III 进程复现。

## 卸载

删除 WarVK 的 `d3d9.dll`，并在存在旧文件时恢复备份。日志与诊断文件可以单独清理。

## 给开发者

- `src/d3d9/`：D3D9 运行时、设置、阴影/光照管线与接入层
- `src/d3d9/war3/`：游戏 Hook、语义桥、资源生命周期、GPU 蒙皮、JAPI、数学与诊断
- `subprojects/war3fx/`：WarVK Shader
- `WarVK/`：JASS 库、YDWE Catalog、图标与作者文档
- `AutoTest/`：静态合同、Win32 runnable、性能门与取证

32 位构建：

```powershell
.\build32_safe.cmd src/d3d9/d3d9.dll -j2
ninja -C build32 -n src/d3d9/d3d9.dll
```

主产物为 `build32/src/d3d9/d3d9.dll`。发布包范围与排除项见 [docs/RELEASE_1.21.00.md](docs/RELEASE_1.21.00.md)。

## 许可证与致谢

WarVK 项目整体按 GPLv3 发布，详见 [LICENSE](LICENSE)、[COPYING](COPYING) 与 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。DXVK 派生代码及第三方组件继续保留各自许可证与版权声明。

感谢 DXVK、Dear ImGui、MinHook、Vulkan 生态项目及 Warcraft III 社区研究。WarVK 是非官方第三方项目，测试新版本前请保留游戏目录与存档备份。
