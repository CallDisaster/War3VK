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
  的作者接口，新增 scalar 数学求值，并重整 YDWE 分类。2026-08-12 的局部雾候选已进一步接通
  最多 8 个 Sphere/Box/Cylinder、解析射线区间、独立全局介质开关、受预算约束的局部 half-res ROI，
  并以视角无关 CSM 光学证据替代阴影柱俯仰特判；77/77 静态、21/21 Win32 runnable、DLL 构建及
  no-work 通过，但尚未部署、真实 Catalog 回读或玩家前台物理验收，详见
  `docs/agent-history/2026-08-12-local-volumetric-fog-candidate.md`。
- 2026-08-13 的 Volumetric Lighting 2.0 候选新增可选 Froxel Medium/High；玩家回归已否决逐列
  `[L-D,L]` surface-tail 和其后 128/256 步过早回退。当前研究修复使用全屏统一 `[20,10000]`
  对数 Z、64/128 层，scene depth 只在独立 `1/8`/`1/4` effect 网格终止积分；太阳阴影使用整 ray
  1024/2048 步 shadow-texel 光学区间，并在耗尽后作有界密集回退。旧 RayMarch 仍为默认；450 万
  grid-cell 与 3.5 亿主 DDA pixel-step admission 失败会安全回退。78/78 静态、21/21 Win32 runnable、
  DLL 构建及 no-work 通过；已部署但尚未完成玩家前台视觉/4K 性能门，详见
  `docs/agent-history/2026-08-13-froxel-full-view-research-fix-candidate.md`。
- 上述部署候选的玩家回归仍显示低俯角/侧视亮缝、分层及阴影柱收缩。当前未部署修复已将错误的
  `0.0075` 归一化 volume-sun bias 改为 2 世界单位后按实际光空间跨度归一化，并让预算耗尽
  fallback 使用贯穿全部 Z slice 的共同采样格点；78/78 静态、DLL 构建及 no-work 通过。独立确认
  的上游离屏 Caster 提交缺失仍未修改，不能宣称本候选已彻底解决该类消失，详见
  `docs/agent-history/2026-08-13-volumetric-bias-and-fallback-phase-candidate.md`。
- 后续运行取证确认该玩家回归实际仍在 backend 0：地图只调用 `SetEnabled(true)`，没有调用
  `SetBackend`，日志也没有任何 Froxel 提交。当前未部署验收候选默认进入 Froxel High，并新增
  实际 backend 日志，显式 0/1/2 回退仍保留；78/78 静态、DLL 构建及 no-work 已通过，尚待
  玩家前台低视角 A/B，详见
  `docs/agent-history/2026-08-13-froxel-backend-admission-candidate.md`。
- 玩家已确认 Froxel High 不再复现上述几何错误，但阴影柱对比度几乎不可见。当前未部署候选已把
  旧路径受真实 CSM 光学证据约束、最大 24% 的可读性衰减移植到 Froxel alpha，不提高全局雾密度；
  78/78 静态、21/21 Win32 runnable、DLL 构建及 no-work 通过，尚待玩家前台视觉 A/B，详见
  `docs/agent-history/2026-08-13-froxel-shadow-readability-candidate.md`。
- 玩家回归确认上述可读性显著改善，但 `1/4` effect 自身强边 range weight 把轮廓放大成大块阶梯。
  当前未部署候选改为由全分辨率 scene depth 单独引导 RGBA 双线性重建，保护真实几何断层而不锁死
  低分辨率体积边；78/78 静态、21/21 Win32 runnable、DLL 构建及 no-work 通过，尚待视觉 A/B，
  详见 `docs/agent-history/2026-08-13-froxel-shadow-edge-aa-candidate.md`。
- 动态回归仍有整格跳变；根因是 integrator 只在 receiver-depth transition 过滤，Caster 横向
  silhouette 仍为单 shadow texel 二值判断。当前未部署候选改为每个有效区间一次 2×2 depth gather，
  对四个深度先解析比较再按亚 texel 覆盖率合并；不增加历史拖影或 DDA 上限；78/78 静态、
  21/21 Win32 runnable、DLL 构建及 no-work 通过，详见
  `docs/agent-history/2026-08-13-froxel-dynamic-shadow-comparison-pcf-candidate.md`。
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
- 2026-08-09 的对象 bounds 候选已由模型缓存的 map epoch 与 process-monotonic immutable
  generation 派生精确局部 geoset bounds，并以独立 identity proof 贯穿到最终 CSM；skinned/
  动态附件保持 fail-visible。对象 C2/C3 消费默认关闭，只允许先收集 Observe 证据，详见
  `docs/agent-history/2026-08-09-generation-backed-object-bounds-observe.md`。
- 2026-08-09 已确认最终 CSM 曾允许非地形 C2/C3 使用未证明的猜测包围球剔除；当前本地候选统一使用
  bounds provenance 授权，Unknown/Generic/Animated/Skinned 一律 fail-visible，并保留 would-cull 统计。
  该候选会增加远级联工作，尚未部署或通过低视角物理 A/B，详见
  `docs/agent-history/2026-08-09-object-bounds-fail-visible.md`。
- 2026-08-09 已修复 Transparent Type0 建造附件在 Stage11 被错误拒绝的问题：Type0 现在拥有常驻的
  exact CurrentDraw 边界，使用子部件身份和同帧 VB/IB/UV/完整矩阵调色板，并正确支持
  `D3DVBF_0WEIGHTS` 索引蒙皮。用户已确认不死族 UBirth 建造阴影不再闪烁；该路径没有重新开启
  全局跨帧 VB/IB cache。详细证据见
  `docs/agent-history/2026-08-09-stage11-type0-correctness-baseline.md`。
- 2026-08-09 的当前集成候选为 `203932c` / `codex/integrated-correctness-baseline-20260809`，
  build32 DLL SHA-256 为 `56566E0418E2C51AE22C7978E5934AC45ACDB32191F7E02E15C83EBB3FAF8190`。
  它把用户分别确认过的 Type0/UBirth 修复与点阴影 receiver-bias 修复合入同一源码线，并包含本轮
  跨地图 CPU 身份缓存失效；73 个静态脚本、18/18 Win32 runnable、DLL 构建和 no-work 已通过。
  该组合 DLL 尚未部署或完成前台物理回归，不能把两个旧候选的独立验收冒充组合验收。
- 2026-08-09 的本地运行时安全候选默认编译期禁用 legacy `warvk:cmd`，并闭合 JASS reset、异步
  settings 生命周期、active-device 发布、Reset device epoch 与 SceneCollector 早退身份残留风险；
  76/76 静态、20/20 Win32 runnable、32 位 DLL link 与 no-work 已通过，但尚未部署或完成 Reset/
  A→B→A 前台物理门。详细证据见
  `docs/agent-history/2026-08-09-runtime-safety-and-shadow-edge-research.md`。
- 同一调查确认现有 3794 帧取证全为 4096 DirectInline，阴影边缘持续爬动并非 Temporal 或自适应
  降档所致；首要源码嫌疑是 CSM 先线性过滤原始深度再比较，以及默认周期性世界坐标 Poisson 旋转。
  这些算法项仍是研究/执行方案，不能描述为已修复；详见上述研究文档。
- Issue #5 的地形级联剔除现为默认关闭的 `Off / Observe / Consume` 合同；只有同帧、同代且来自
  已验证 position span 的精确 bounds 才能授权 C2/C3 剔除，猜测或陈旧 bounds 一律 fail-visible。
  当前仅完成离线验证，未部署且未通过实机 A/B；详见
  `docs/agent-history/2026-08-09-issue5-terrain-bounds-observer.md`。
- Issue #6 已先闭合两个确定的跨地图 CPU 身份泄漏：caster tombstone 现在按 map epoch 隔离，且
  旧相机、per-draw upload、RT/DS fallback 会在 Present 安全点重置；显式禁用 producer stage 的
  进程级策略仍跨地图保留。该阶段仅通过离线合同和 Win32 runnable，尚未部署或完成 A→B→A
  物理验收，详见 `docs/agent-history/2026-08-09-issue6-map-identity-isolation.md`。
- 同一 Issue 的后续审计确认 `ModelRegistry`、实例、Pose、附件、ShadowObject 与已发布 semantic
  contract 原先都不会在地图切换时清空；现在由 `War3Renderer::ResetMapSession` 在 Present 安全点
  发布空快照并清除纯 CPU 指针/姿态表，旧 contract 仍由 `shared_ptr` 保护在途读者。该候选同样尚未
  部署或物理验收，详见 `docs/agent-history/2026-08-09-issue6-semantic-registry-reset.md`。
- Issue #6 的 retired-session census 现可分别报告在途缓存条目、allocator chunk、逻辑 GPU 引用和
  CPU backing，并在 completion serial 回收后递增 collected 计数；它不改变 fence 或释放时机，仍需
  冷启动 B / A→B / A→B→A 物理数据确认，详见
  `docs/agent-history/2026-08-09-issue6-retired-session-census.md`。
- Issue #6 的 Direct geoset、unit flags、材质、palette slot、terrain bounds、index slice、static
  mesh-data、runtime-geoset 及 render-hook CUnit 身份热缓存现已按 map epoch 失效；进程全局别名在 Present reset 清除，
  固定 TLS 容器拒绝旧条目。模型 Hook 也已将地图会话 reset 与进程 Shutdown 分离，换图只清理
  palette/pose/attachment 裸指针状态而不伪装卸载 Hook。该候选不改变 GPU 退役，仍未部署或完成
  跨地图物理门，详见 `docs/agent-history/2026-08-09-issue6-map-scoped-hot-caches.md`、
  `docs/agent-history/2026-08-09-issue6-shadow-core-cache-isolation.md` 与
  `docs/agent-history/2026-08-09-issue6-model-hook-map-session-reset.md`；render-hook 补充见
  `docs/agent-history/2026-08-09-issue6-render-hook-unit-cache.md`。后续还清除了 widget/rawcode/handle
  映射及 SceneCollector 的 CUnit→handle TLS，并让 runtime-model 正验证和 palette-slot TLS 随地图
  会话失效；细节见 `docs/agent-history/2026-08-09-issue6-render-identity-cache-reset.md` 与
  `docs/agent-history/2026-08-09-issue6-widget-scene-identity-reset.md`。
- DirectGrouped 的 Producer Claim 目前只有默认关闭的同帧 Observe 预测器；Consume 请求会被明确
  拒绝且不改变 caster。旧性能报告显示 BuildEligible 是主要 CPU 热点，但 reduced key 尚缺已证明的
  source/material/alpha 身份，必须先完成至少 10,000 帧零误判实机门，详见
  `docs/agent-history/2026-08-09-producer-claim-observe.md`。
- 当前 RTX 4060 Ti 的主 15.73 GiB device-local heap 不可 host-visible；唯一同时
  `DEVICE_LOCAL | HOST_VISIBLE` 的 heap 只有 214 MiB。因此 ReBAR direct-upload 实验不满足既定
  准入条件，保持未实现/默认关闭，不能占用小 BAR heap 冒充完整 ReBAR 收益。
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
- 修改阴影、滤波、抗锯齿、资源同步或其他图形学算法时，只要实现者不能从现有合同中完整证明公式与
  Vulkan 行为，就必须先查阅一手资料：原始/同行评审论文、Microsoft DirectX 指南、Khronos Vulkan
  规范或 GPU 厂商研究资料；在 `docs/research/` 记录公式、适用条件、来源链接及其到 WarVK 的映射。
  博客和二手总结只能作为检索线索，不能单独授权 Release 默认改动。
- Unreal Engine 官方源码只允许从 EULA 授权的 `EpicGames/UnrealEngine` 仓库检出到项目外部的
  `E:\Mycode\Source\References\UnrealEngine` 并作为只读架构参考。不得把 UE 源码、片段或资产复制、
  改写进 WarVK/DXVK 开源树或提交历史；WarVK 的实现、测试和注释必须保持独立表达与可追溯的一手依据。
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
