# WarVK

[English](README.md)

WarVK 是一个面向 Warcraft III 1.27a 的画面增强项目。它基于 DXVK 派生出的 D3D9 到 Vulkan 渲染路径，在经典客户端中加入更现代的阴影、后处理、运行时诊断，以及面向引擎研究的渲染改造能力。

当前最新实验分支已经有意抛弃旧的渲染思路：不再把魔兽争霸 3 当成一个只能被动观察的 D3D9 draw stream，然后试图把固定管线提交过的内容原样重放。新的方向是直接从魔兽争霸 3 的上层运行时拿数据，包括可见 renderable、对象身份、模型资源、pose/palette、材质信息和 draw-time 几何，再由 WarVK 自己构建 ShadowMap 与阴影接收流程。

> [!NOTE]
> WarVK 的目标是优先提升画面表现，而不是单纯追求更高帧率。开启高质量效果后，显卡负载通常会升高。

## 给玩家

### 这是什么

- 用 Vulkan 后端替换 Warcraft III 的传统 D3D9 渲染路径
- 在游戏内提供阴影、抗锯齿、Bloom、曝光等图形调节选项
- 只改变画面呈现，不改玩法规则，也不修改地图数据

### 静态阴影解决版重点

- 已解决 Warcraft III 原生建筑静态阴影残留：通过早期 Hook `CUnitUIManager_RecordSetStructureShadow`，阻断 UnitUI 类型记录中的 `buildingShadow(+0x50)` 写入
- 默认移除 `TerrainShadow_RenderListB` 旧版单位黑色圆影，避免原版 blob 阴影和 WarVK 阴影叠加
- 退役 `WriteMaskRegion / StaticStampPath / RegisterImage / DoodadStamp` 等历史静态阴影实验默认路径，减少误伤地形、雾、贴花的风险
- 保留静态阴影研究证据链，后续需要专项诊断时仍可回看历史路径

### 1.1.0 版本重点

- 打通了游戏逻辑层到渲染层的对象语义桥梁，增强对象身份追踪能力
- 将最新实验阴影路径迁移到“魔兽上层运行时语义数据驱动”，而不是只依赖旧 D3D9 draw replay
- 将 StormBreaker 直接内置进项目运行时
- 升级了 GPU Arena，降低因 VB/IB 冻结快照式阴影捕获导致的显卡驱动压力
- 接入了第一阶段静态模型缓存，减少 GPU Arena 的占用

### 当前已知问题

- 看到桥、斜坡等装饰性/静态几何时仍可能短暂卡顿。当前 static VB cache 与 LRU 只能缓解重复进入视野后的卡顿，第一次看到时仍可能发生短暂停顿，尚未根治。
- 路径阻断器的 CSM 阴影仍可能漏进 WarVK 的 ShadowMap。项目已经能通过 FourCC 识别一部分路径阻断器渲染对象，但仍有实例没有在最终 CSM caster 列表中被剔除。

### 适用人群与局限

- 对战玩家通常更需要极致清晰的信息呈现，Bloom 和软阴影可能会带来干扰
- 第三方对战平台可能会屏蔽或移除自定义 `d3d9.dll`，因此 WarVK 更适合本地、局域网和单机场景
- 地图作者目前不应假设玩家端一定能成功加载这个 DLL

### 运行要求

- Windows 10 或 Windows 11
- 支持 Vulkan 的显卡与较新的官方驱动
- Warcraft III 1.27a

### 安装方法

1. 先备份游戏目录，特别是已有的 `d3d9.dll`
2. 将发行包内容复制到 `war3.exe` 所在根目录
3. 启动游戏，确认根目录生成 `d3d9.log`

常见发行内容包括：

- `d3d9.dll`
- `shaderpacks/`
- 其他发行包附带的运行时文件

### 游戏内操作

- 按 `Ctrl + F1` 打开或关闭 WarVK 配置面板
- 常用设置包括：
  - `Unlock FPS`
  - `Enable Post-Processing`
  - `Enable Shadows`
  - `Shadow Quality`
  - `Shadow Intensity`
  - `Anti-Aliasing`
  - `Bloom`
  - `Exposure`

### 常见问题

- 如果黑屏或闪退，先查看 `d3d9.log`，同时确认目录中没有其他第三方 `d3d9.dll`
- 如果性能不理想，优先降低阴影质量、Bloom 和抗锯齿档位
- 如果个别地图出现闪烁、阴影异常或画面抖动，建议先关闭阴影或后处理再做定位

### 卸载

1. 从游戏根目录删除 `d3d9.dll`
2. 需要时再删除 `d3d9.log` 和 `shaderpacks/`

## 给开发者

### 当前阶段

当前 WarVK 里程碑：`静态阴影解决版 (2026-07-08)`

当前版本已经具备：

- 游戏逻辑层到渲染层的对象语义桥接
- StormBreaker 运行时整合
- GPU Arena 阴影捕获与运行时几何管理
- 第一阶段静态模型缓存

动态 Pose 接管仍在继续推进，目前还没有完全替代回退阴影路径。

当前活跃的实验阴影分支已经不再是简单的“捕获 D3D9 draw call 后延迟重放”。长期目标是语义化渲染：Hook 魔兽争霸 3 的上层渲染/运行时数据流，重建稳定的对象与 renderable 身份，在数据生产侧附近拿到模型、姿态、材质和几何事实，然后提交 WarVK 自己的 shadow scene。旧 draw capture 仍保留为兼容与诊断回退，但主方向是从魔兽运行时语义直接渲染 ShadowMap。

### 代码结构

- `src/d3d9/`
  WarVK 自身的 D3D9 运行时、游戏 Hook、阴影与后处理主逻辑
- `src/dxvk/`
  Vulkan 抽象与 DXVK 派生运行时层
- `src/dxso/`
  Shader 编译与转换支持
- `src/util/`
  日志、线程、配置与通用工具
- `src/spirv/`
  SPIR-V 处理逻辑
- `src/vulkan/`
  Vulkan 加载与公共辅助模块
- `src/wsi/`
  窗口系统集成
- `src/minhook/`
  游戏函数 Hook 依赖

### WarVK 当前新增的核心子系统

- 逻辑层到渲染层的对象身份桥
- UI、渲染、生命周期、JASS、阴影等分域 Hook
- GPU Arena 阴影捕获与运行时几何调度
- 静态模型缓存
- AutoTest、性能报告、运行时诊断链路

### 构建要求

- MinGW-w64 GCC 15.2.0 或更高版本
- Meson 0.58 或更高版本
- Ninja
- Vulkan SDK
- glslang

### 构建方式

```powershell
# 首次配置
meson setup --cross-file build-win32.txt build32

# 推荐增量构建方式
.\build32_safe.cmd src/d3d9/d3d9.dll -j8
```

主产物路径：

- `build32/src/d3d9/d3d9.dll`

### 开发说明

- `build32_safe.cmd` 用于绕开 Windows 扩展路径工作目录导致的 MinGW 增量构建异常
- `build32_safe.cmd` 还会自动恢复 `imgui` 子模块所需的本地 Meson shim，避免为了构建系统单独维护一个 Dear ImGui fork
- `CHANGELOG.md` 记录面向用户的版本更新内容
- `AGENTS.md` 记录当前工程状态、阶段目标和交接信息
- 自动测试与性能工具主要位于 `AutoTest/` 与 `src/d3d9/war3/tools/`

### 开源与许可证说明

WarVK 当前以项目整体层面按 GPLv3 发布。GPLv3 完整文本见 [LICENSE](LICENSE) 与 [COPYING](COPYING)。

同时，仓库中仍包含 DXVK 派生代码及多个第三方组件，这些部分继续按各自原始许可证提供。相关说明已整理在 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 中，后续分发时应一并保留原始版权与许可证声明。

## 路线图

- 完成基于 Pose 的动态阴影重建与缓存模型资源消费
- 继续降低阴影与投影器拦截热路径的 CPU 成本
- 将更多剔除与批处理逻辑迁移到更适合 GPU 的执行路径
- 在未来开放更清晰的外置 ShaderPack 工作流

## 致谢

- DXVK
- Dear ImGui
- MinHook
- MemHack 研究参考与逆向启发
- Asphodelus 与相关 JASS 引擎研究

## 免责声明

WarVK 是非官方第三方渲染插件。测试前请备份游戏目录与存档，并自行承担由兼容性问题带来的风险。
