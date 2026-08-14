# 2026-08-14 稳定性能线与方向体积阴影 Guide 发布候选

## 候选身份

- 分支：`codex/stable-optimization-integration-20260814`
- 体积阴影 Guide 原提交：`8f232cc`
- 稳定性能/体积光合并提交：`039a494`
- Fresh Release 构建目录：`build32_release_guide_20260814`
- DLL 大小：`35,243,960` bytes
- DLL SHA-256：`2EDF9FCD50D8D8329160809986A9D2FE17F33BD85DCDE158B9D3853B4CDD264D`

该候选在此前已通过长门的体积光、局部雾和阴影生产者 CPU 优化基线上，合并了最新的方向体积
阴影 Guide。默认 Release 仍关闭实验性剔除 Consume、Persistent Package Consume、ReBAR、RTS
shadow candidate 和开发 observer。

## 合并内容

- Froxel Integrate 除低分辨率 RGBA16F 散射场外，写入独立 `R16_SFLOAT` 方向阴影 Guide；
  有预算时仅对检测到边缘的区域生成 `1/2` refined guide。
- Composite 在全分辨率 scene depth 上重建体积效果，并使用独立 Guide 保留小 Caster 的体积阴影
  轮廓；真实几何断层与体积阴影边缘使用不同证据，不再由低分辨率强边共同放大。
- Vulkan 图片创建保持事务式：effect/base guide/refined guide 任一创建失败都不发布半套资源。
- effect/base/refined guide 使用各自的 owned layout state，barrier 先 plan、成功录制后 commit；不依赖
  硬编码旧 layout。
- shader-work admission 按 Composite 的 24 次纹理读取/全分辨率像素计费。总上限调整为
  350 Mi work units，使经过数值合同证明的 1080p 默认请求可进入；1440p/4K 最坏请求仍会
  fail-closed 回 Legacy，而不是提交无界工作。

## 离线验证

- 全部静态脚本：`216/216` 通过。
- Win32 Meson runnable：短路径 fresh test build 中 `50/50` 通过。
- Fresh 32 位 Release DLL 使用 `-j2` 构建通过，全部开发选项为 `false`。
- `ninja -C build32_release_guide_20260814 -n src/d3d9/d3d9.dll` 为 no-work。
- `git diff --check` 通过。

长路径全 test target 在第 47 个对象遇到 Windows/MinGW dependency-file 路径长度限制；同一源码在
`E:\\b\\wvk14` 短路径完成全部 50 个 runnable，因此这不是源码或测试失败。

## 隔离桌面实机 smoke

测试使用 `E:\\Work\\War3` 的未修改 `ShadowTest/光影测试.w3x`、`full_default`、High priority、
显式 `DXVK_WAR3_VOLUMETRIC_BACKEND=2`，运行在非交互隔离桌面；没有 `SwitchDesktop` 或全局输入。

- 采样：`3901` 帧 / `62.005` 秒；
- 隔离桌面统计：`63.942 FPS`、`15.639 ms/frame`、主线程 `6.353 ms`、GPU `3.874 ms`；
- VolumetricLight CPU scope：约 `0.061 ms/frame`，`3897` 次；
- Arena：平均 `27.355 MiB`、峰值 `29.601 MiB`；
- `3902` 个预算帧中 incomplete、budget exceeded、reuse-last-complete、current-partial 全为 `0`；
- 日志 device lost、freeze budget exceeded、shadow incomplete、partial、CSM fallback 全为 `0`；
- 新 crash incident、Windows GPU Event 153/4101 为 `0`。

隔离桌面 FPS 只证明运行稳定和相对工作量，不能当作玩家前台绝对性能。

## AutoTest 热阴影探针说明

两次热阴影轮询均观测到完整终态：

- `142/142` caster/replay、`568` 个四级联 draw；
- 另一轮为 `147/147`、`588` 个四级联 draw；
- producer stamp、replay、receiver、identity 与最终 publication 全部为 true，所有 producer
  failure 计数为零。

通用探针仍判超时，因为该光影图 FrameManifest 的 `unitCount=0`，而通用场景额外要求
`unitCount>=1`；即使 `--allow-final-shadow-publication-only` 也继续受这个 manifest 条件约束。
本候选没有为测试通过而修改渲染逻辑。正式 smoke 改用 `--no-hot-shadow` 完成采样，并以上述
runtime 终态单独证明阴影 publication 完整。

## 已继承的性能证据

Guide 合并前的同地图 A-B-B-A 已证明稳定性能线相对体积光父线：主线程 CPU
`6.135 -> 5.778 ms`（`-0.357 ms / -5.82%`），Populate `-76.27%`、DirectGrouped
`-87.42%`、BuildEligible `-96.38%`。Guide 会增加新的 GPU 工作，本轮没有完成同位置前台
A-B-B-A，因此不能把上述 CPU 收益外推成 Guide 候选的完整 FPS 提升。

## 发布前剩余物理门

1. 玩家前台确认 Froxel High 的动态小 Caster、静态大 Caster、最低俯角和侧视体积阴影清晰度；
2. 固定相机对比体积光开/关，确认表面 CSM、建筑阴影和树叶细影没有回归；
3. 1080p 与 4K 各记录一轮性能；4K 允许按已定义 admission 回退，但不能卡死或 device lost；
4. 跨地图问题仍属于下一版本范围，本候选不宣称修复。

通过这些前台门后才可更新 Release、CHANGELOG 和 GitHub；当前只建立本地 RC，不 push。
