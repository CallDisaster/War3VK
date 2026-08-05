# WarVK 1.2.0 Release

![平台](https://img.shields.io/badge/platform-Windows-lightgrey.svg)
![Vulkan](https://img.shields.io/badge/Vulkan-1.3+-red)
![Warcraft III](https://img.shields.io/badge/Warcraft%20III-1.27a-gold)

[English](README.md) · [更新日志](CHANGELOG.md) · [WarVK JAPI](WarVK/README.md)

WarVK 是面向 Warcraft III 1.27a 的画面增强运行时。它以 DXVK 派生的 D3D9 到 Vulkan 后端为基础，为经典客户端加入现代太阳阴影与点阴影、体积光、后处理、运行时诊断，以及供地图作者使用的 JASS API。

1.2.0 是新版语义化渲染架构的第一个正式公开版本。WarVK 会综合读取 Warcraft 运行时的对象身份、模型、姿态、材质与当帧绘制证据，再构建自己的阴影场景；旧 D3D9 捕获仍保留为严格受控的兼容路径，但不再被视为唯一数据来源。

> [!IMPORTANT]
> 安装前请把 NVIDIA、AMD 或 Intel 显卡驱动更新到最新官方版本。当前运行时实际请求 **Vulkan 1.3**（已完整覆盖 Vulkan 1.2）；只暴露 Vulkan 1.2 而不支持 1.3 的旧驱动不满足本构建要求。

> [!NOTE]
> WarVK 优先保证画面质量与渲染正确性。4096 CSM、点阴影、体积效果与完整后处理都会产生真实的 CPU/GPU 开销。

## 1.2.0 重点更新

### 阴影与光照

- 太阳光使用四级联阴影，默认稳定锁定 4096 分辨率；只有资源确实无法安全分配时才锁存回退到 2048。
- 收紧 PCF 半径，缓解旧版本阴影边缘过度模糊的问题。
- 透明度测试树木、蒙皮单位、刚体、建筑与地形都采用更严格的当帧资源所有权与来源验证。
- 路径阻断器、原生建筑静态阴影与旧式单位圆形阴影不再进入 WarVK 阴影场景。
- 点阴影改为径向深度，并加入 receiver-plane bias、texel-center 采样与明确的深度同步，显著缓解此前地面和单位上的严重摩尔纹/条带伪影；少数表面和观察角度仍存在残留，见下方已知问题。
- 提供可选 TAA v2：包含方差裁剪、reactive feedback、历史状态诊断与单次失效机制。正式版默认仍为 DirectInline。
- 体积太阳光、体积点光与独立控制的全局高度雾现已接通运行时和作者 API。

### 稳定性与性能

- Shadow Arena 改为事务式 bundle：position、blend、UV、IB 要么整份提交，要么整份拒绝，不再产生半份 caster。
- Arena 代际必须等专用 GPU completion fence 完成后才能回收，禁止按帧号直接覆写仍在途的数据。
- 统一 map/device epoch 隔离 Manifest、缓存几何、GPU 蒙皮、点阴影任务、TAA 历史与 receiver 发布，避免旧地图资源污染新地图。
- 最终 CSM/点阴影 replay 在发出 Vulkan draw 前验证 VB/IB 范围、索引域、格式、代际、矩阵与蒙皮输入。
- 不完整 CSM 不会再“一个 caster 一个 caster”地逐步发布；只有完整的同地图 candidate 才原子生效，否则安全显示为无太阳阴影。
- 对 write-combined 索引缓冲采用有界批量读取，修复 exact-index 检查引入的明显 CPU 性能回归，同时不恢复危险的跨帧 VB/IB 缓存。
- Compact WorkTable、联合消费者剔除、Persistent GPU Package、持久点阴影规划器和 CPU 多线程蒙皮合同已经进入受控基础设施阶段；未通过完整门槛的 Consume 路线仍默认关闭。

### 地图作者 API

- WarVK JAPI 已内置在 `d3d9.dll` 中；WarVK 已作为代理运行时加载时，不需要额外编译 `war3map.dll`。
- 对外线协议继续保持 `warvk:v1`。高频纯数值调用可走经过 Game.dll 签名验证的强类型 Hashtable 通道，文本命令继续使用兼容字符串通道。
- 作者可以创建和移动点光、开启点阴影、控制体积光和全局高度雾，并将 WarVK 光照时钟与 Warcraft 玩法时间解耦。
- 闪电模板支持贴图、颜色、宽度、动画、分支、公式曲线与上传的点曲线。
- 有界 MathProgram/Curve 运行时支持 scalar、`vec2`、`vec3` 表达式，可向 JASS 返回实数/整数结果、查询导数与弧长，并驱动连续闪电 Ribbon。
- YDWE 菜单已按基础、诊断、太阳/CSM、光照时钟、点光、体积光、体积雾、闪电、模板、数学和曲线分类；模式参数提供可点击选择的 Trigger Type，不再要求作者记忆整数。

完整内容见 [CHANGELOG.md](CHANGELOG.md)，地图作者接口见 [WarVK/README.md](WarVK/README.md)。

## 运行要求

- Windows 10 或 Windows 11
- 支持 Vulkan 1.3 或更高版本的显卡与官方驱动
- Warcraft III 1.27a（32 位）

WarVK 只针对经典 1.27a 可执行文件与已验证的 `Game.dll` 布局。未知签名会安全拒绝 Hook，不会扫描猜测地址。

## 安装方法

1. 备份 Warcraft III 目录，尤其是已有的 `d3d9.dll`。
2. 将发行包中的 `d3d9.dll` 复制到 `war3.exe` 同级目录。
3. 启动游戏，确认 `d3d9.log` 中出现 `DXVK: 1.2.0 Release`。
4. 按 `Ctrl + F1` 打开或关闭 WarVK 设置面板。

玩家目录只需要发行包明确列出的文件。源码、测试工具、研究资料与 YDWE 作者包不应复制到游戏目录。

## 常用设置

- 解锁帧率
- 开启后处理
- 开启阴影并选择阴影/TAA 模式
- 调整阴影强度与过滤
- 配置点光源与点阴影
- 配置体积光与体积雾
- 调整抗锯齿、Bloom、曝光与描边

## 已知问题（1.2.0 Release）

- 点光源开启点阴影后，部分地面与观察角度仍可能出现摩尔纹或带状伪影。该问题尚未在 1.2.0 中完全修复；遇到明显伪影时，可以保留点光源并关闭该光源的点阴影。
- 同一 Warcraft III 进程中退出地图后继续进入另一张地图，已知可能造成持续性能下降、阴影异常或其他资源生命周期问题。1.2.0 不支持可靠的跨地图连续游玩，建议每次退出地图后完整退出 Warcraft III，再重新启动并进入下一张地图。
- 上述两项问题计划在后续版本继续修复，不影响“一次启动只游玩一张地图”的平台常见使用方式。

## 故障排查与反馈

- 启动黑屏通常来自显卡驱动过旧，或游戏目录中存在冲突的第三方 `d3d9.dll`。
- 性能不足时优先关闭体积效果，其次减少点阴影，再降低后处理。太阳 CSM 会保持 4096，除非运行时发生安全的锁存式分配回退。
- 不要在同一游戏目录同时放入多个 D3D9 代理或加载器。
- 反馈时优先提供 `d3d9.log`、`runtime_status.json`、GPU incident JSON 和 WarVK 崩溃转储；公开前请移除个人路径或私有地图信息。
- 若在未重启游戏的情况下切换过地图，请先完整退出 Warcraft III，再重新启动并只进入目标地图；反馈跨地图问题时请同时说明地图进入顺序与第一次异常时的证据。

## 卸载

删除 WarVK 的 `d3d9.dll`，并在存在旧文件时恢复备份。日志与诊断文件可以单独清理。

## 给开发者

主要代码区域：

- `src/d3d9/`：D3D9 运行时、设置、阴影/光照管线与接入层
- `src/d3d9/war3/`：游戏 Hook、语义桥、资源生命周期、GPU 蒙皮、JAPI、数学与诊断
- `subprojects/war3fx/`：WarVK Shader
- `WarVK/`：JASS 库、YDWE Catalog、加载资源、图标与作者文档
- `AutoTest/`：静态合同、Win32 runnable、性能门与 attach-only 取证

32 位构建：

```powershell
.\build32_safe.cmd src/d3d9/d3d9.dll -j8
ninja -C build32 -n
```

主产物为 `build32/src/d3d9/d3d9.dll`。发布包范围与排除项见 [docs/RELEASE_1.2.0.md](docs/RELEASE_1.2.0.md)。

## 许可证与致谢

WarVK 项目整体按 GPLv3 发布，详见 [LICENSE](LICENSE)、[COPYING](COPYING) 与 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。DXVK 派生代码及第三方组件继续保留各自许可证与版权声明。

感谢 DXVK、Dear ImGui、MinHook、Vulkan 生态项目及 Warcraft III 社区研究。WarVK 是非官方第三方项目，测试新版本前请保留游戏目录与存档备份。
