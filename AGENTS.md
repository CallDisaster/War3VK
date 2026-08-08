# WarVK / DXVK Agent Guide

> 最后整理：2026-08-09。本文是后续 agent 的快速入口，不记录逐轮实验、历史改动或路线图；
> 需要追溯时按需检索 [历史归档](docs/agent-history/README.md)。

## 项目是什么

WarVK 是一个面向 **Warcraft III 1.27a** 的 Windows 图形增强项目。它从 DXVK 派生，
以 32 位 `d3d9.dll` 将游戏的 D3D9 路径接到 Vulkan，并增加阴影、光照/体积效果、
后处理、运行时诊断及供地图作者使用的 WarVK JAPI。

项目只改变渲染和诊断，不应改变地图或游戏玩法。长期架构方向是从 Warcraft 上层运行时取得
对象身份、模型、姿态、材质和 draw-time 几何来构建阴影场景；旧 D3D9 draw capture/replay
仍是兼容与诊断路径，不可把它误认为唯一真相。

## 代码地图

| 位置 | 负责内容 |
| --- | --- |
| `src/d3d9/` | D3D9 设备、交换链、Present、WarVK 主运行时和游戏接入层。 |
| `src/d3d9/war3/render/`、`shadow/` | ShadowMap/receiver、最终 replay 验证、渲染阶段与资源发布。 |
| `src/d3d9/war3/hooks/`、`bridge/`、`native/` | Game.dll/JASS Hook、逻辑层到渲染层的对象语义桥及原生接口。 |
| `src/d3d9/war3/gpu_skin/`、`memory/`、`model/` | GPU skin、Arena/静态包、模型与资源生命周期。 |
| `src/d3d9/war3/japi/`、`math/` | 受限的 WarVK JAPI v1、数学表达式和曲线运行时。 |
| `WarVK/` | YDWE catalog、JASS 包装、地图作者文档与打包脚本。 |
| `AutoTest/` | 发布级自动化、性能基线、运行时取证；历史一次性脚本位于其 `_archive/`。 |
| `docs/` | 生命周期、逆向证据和专项设计资料；带日期的 plan/research 文档不是当前状态本身。 |

基础生命周期见 [docs/WAR3_LIFECYCLE.md](docs/WAR3_LIFECYCLE.md)，自动化边界见
[AutoTest/README.md](AutoTest/README.md)。这两份资料用于定位入口；涉及跨地图资源或阴影时，
仍须以当前代码和测试为准。

## 当前状态

- 当前分支：`codex/stage11-exact-attachment-fallback-20260809`。工作树另有用户未提交的外部子模块、
  PlayerCrash 与构建日志；
  保留它们，避免 reset、checkout 或覆盖式操作。
- 最近已提交的阴影基础已将地图/设备 epoch、Arena quarantine、fence retirement 与最终 replay
  验证接入生命周期。跨地图时旧资源不得发布给新地图；新地图在没有完整 CSM 前应安全退化为
  **无阴影**，不能采样未发布的深度图。
- 当前工作树含 **WarVK JAPI 1.2.0 Release 候选**：补齐点光位置、体积光和全局高度雾
  的作者接口，新增 scalar 数学求值，并重整 YDWE 分类。局部 Box/Sphere/Cylinder 雾体积并未实现，
  未发布功能也不能在 YDWE 菜单中伪装为可用。
- 产品、DLL 资源、外部 Shader API 与 JAPI 显示版本已统一为 `1.2.0 Release`；GitHub 源码、
  玩家包、地图作者包及明确排除项见 `docs/RELEASE_1.2.0.md`。
- 当前候选验证为：474/474 静态测试、15/15 Win32 runnable、真实 YDWE Catalog 35/35 回读与
  WTG/WCT 校验、Win32 DLL 构建及 `ninja -C build32 -n` no-work。DLL 为 33,745,880 bytes，
  SHA-256 `84112587871BD421A3B927C65119258851A3E471734633220167AA81877FFF80`；未部署或启动游戏。
- 2026-08-06 的玩家 dump 证明 1.2.0 崩溃处理器把可恢复的 InputHost/CoreMessaging 首机会
  异常同步写成约 59 MiB dump；旧 `g_dumpInProgress` 还会永久遮蔽之后真正的 fatal。当前
  诊断热修默认不再注册 VEH；显式 `DXVK_WAR3_CRASH_FIRST_CHANCE_TRACE=1` 时也只做锁自由
  内存计数。完整 dump 仅由 UEF 为未处理异常生成，`latest_crash.json` 只表示 fatal，且
  首机会状态与 fatal 一次性门已经分离。
- 首个诊断候选暴露出既有增量构建污染：`d3d9_device.cpp.obj` 的 Ninja 依赖表为 0，调用方
  按旧布局只为 `War3RenderPipeline` 分配 656 bytes，而新构造函数按 736-byte 布局写到
  `this+0x2D8`，因此启动即在 d3d9.dll 内越界。现在 pipeline 创建/销毁移入构造函数同一
  翻译单元，factory 符号编码 pipeline/settings 的 size+alignment；旧布局调用方会链接失败，
  不再生成混合对象 DLL。全新目录 clean build 后为 33,750,533 bytes，SHA-256
  `36AC64802D36BEC4327F85E7B3B0CB2878B12BDEAFF54CA2223063C609B90DAC`；483/483 静态测试、
  16/16 Win32 runnable 通过。高压光影图 AutoTest 两轮分别完成 969/1636 采样帧，均进入
  地图且 device lost、frame incomplete、budget exceeded 和新增 dump 为 0。clean DLL 已由
  AutoTest 部署到 `E:\Work\War3\d3d9.dll`；仍不能据此排除 ReShade/InputHost 自身问题。
- 2026-08-07 的单地图高压候选已区分 receiver 终态与 pre-receiver 占位发布，并为 direct geoset
  快取补齐 map/immutable generation 门。默认可见桌面下“生与死”完成 DirectInline 三轮及 TAA v2
  一轮各 10 分钟巡航，receiver 全零、Arena/replay 异常、incident 和 GPU 事件均为 0；504/504 静态、
  16/16 runnable 通过。部署 DLL SHA-256 为
  `9FE2F6132015D6BF5413B844915187F80D9F53E14F204564890CD9E71E12AED3`。详细证据见
  `docs/agent-history/2026-08-07-life-and-death-night-gate.md`。
- 2026-08-09 已修复 Transparent Type0 建造附件在 Stage11 被错误拒绝的问题：Type0 现在拥有常驻的
  exact CurrentDraw 边界，使用子部件身份和同帧 VB/IB/UV/完整矩阵调色板，并正确支持
  `D3DVBF_0WEIGHTS` 索引蒙皮。用户已确认不死族 UBirth 建造阴影不再闪烁；该路径没有重新开启
  全局跨帧 VB/IB cache。详细证据见
  `docs/agent-history/2026-08-09-stage11-type0-correctness-baseline.md`。
- 1.2.0 的发布范围限定为“新启动进程只进入一张地图”。同进程退出地图后再进入其他地图仍会造成
  性能下降、阴影异常或其他生命周期问题，已由用户决定延期到下一版本；点光开启点阴影后，部分
  地面/角度仍有摩尔纹或带状伪影。README/CHANGELOG 必须保留这两项已知问题，后续不得描述为已修复。
- 当前 JAPI/体积效果仍需用户地图物理验收；可见桌面 AutoTest 的低视角稳定门不能代替玩家前台
  视觉判断，也不能外推为跨地图或点阴影摩尔纹已经修复。

## 不可破坏的工程约束

- GPU 资源的 reset、Arena 回收、receiver/skin 状态切换必须由渲染所有者在 `PresentEx` 安全点
  合并执行。地图退出或 JASS/Hook 线程只能提出请求和清理 CPU 侧状态，不能直接释放仍可能被 GPU
  使用的资源。
- Shadow 生产、接收和 replay 都必须验证 map/device epoch、资源身份、范围、索引/顶点来源及有限
  数据。任何证明失败都 fail-closed：整份 candidate 不发布，必要时显示无阴影，绝不跨地图复用。
- Arena、冻结几何、persistent package 与 GPU-skin 的 fence/所有权不能互相冒充。producer 完成 fence
  不等于消费者的 last-use 权限。
- WarVK JAPI 是有界的外部输入面：保留 wire 长度、参数数、句柄、数值有限性、溢出和生命周期检查；
  不能为方便而让渲染线程回调 JASS 或暴露未实现 feature bit。
- 不要将 `AutoTest` 的 isolated desktop 数据宣称为玩家前台性能；涉及性能时按
  `AutoTest/README.md` 的前台基线和特性矩阵规则执行。
- 构建或测试不会自动授权部署 DLL、覆盖 YDWE/Warcraft 文件、启动/关闭编辑器或游戏。此类操作需有
  用户明确请求，并先检查目标进程与精确备份/哈希。

## 常用开发与验证入口

```powershell
# 在仓库根目录构建 32 位 DLL；该 helper 会处理 Windows 扩展路径和 imgui Meson shim。
.\build32_safe.cmd src/d3d9/d3d9.dll -j8

# 确认增量构建没有遗留工作。
ninja -C build32 -n

# 按改动范围运行对应静态检查；全量检查仅在需要时执行。
Get-ChildItem AutoTest -File -Filter 'test_*_static.py' | ForEach-Object { py $_.FullName }
```

主产物为 `build32/src/d3d9/d3d9.dll`。提交前至少运行相关静态测试、相关 Win32 runnable 和 DLL
构建；阴影/生命周期/性能改动还应执行与风险相称的 AutoTest 门。不要把当前工作树已有的测试
记录当作新改动的验证结果。

## 文档维护规则

- 根目录 `AGENTS.md` 保持为短入口：项目目的、代码地图、当前状态、硬约束、构建入口。不要添加逐轮
  日志、旧 SHA、完整 benchmark、实验细节或路线图。
- 长篇变更、旧方案、性能数据、回退信息和未来计划放入
  `docs/agent-history/`、`docs/research/` 或 `docs/plan/`，并用日期和主题命名。
- 完成一项会改变“当前状态”或硬约束的工作时，只在本文更新一条浓缩事实和验收边界；详细证据写入
  独立文档。普通任务不应要求全文读取历史归档。
