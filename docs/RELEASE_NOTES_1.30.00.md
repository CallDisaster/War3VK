# WarVK v1.30.00 — 64 位渲染

大版本：Vulkan 渲染（DXVK、显卡驱动、阴影和后处理）移到一个独立的 64 位进程，魔兽争霸自己的 32 位
地址空间里只剩游戏和一个很薄的转发层，大地图、高清资源包不容易再因内存不足崩溃。同时带来按类别的
阴影设置、太阳圆盘、重新调过的一天光色、新的水面，以及战役过场和迷雾的一批修复。
Shader API 1.2.0、JASS wire `warvk:v1` 不变，现有地图脚本不用改。

## 性能（测试机：Ryzen 5 5600X、RTX 4060 Ti 16 GB、32 GB 内存）

光影为玩家默认设置（太阳光与阴影、Bloom、后处理、昼夜光色、新水面开；SSAO、体积光、点光源关），
窗口 1902×963，帧率按系统内核记录的每秒呈现次数计，每格两轮平均。

| 地图 | 原版（自带上限） | 原版（去掉帧率限制） | 1.30 单进程 | **1.30 64 位** |
|---|---:|---:|---:|---:|
| Lost Temple | 64 | 701 | 227 | **443** |
| EchoIsles | 63 | 547 | 153 | **317** |
| 《生与死》（大地图） | 64 | 116 | 43 | **67** |
| 《生与死》p99 帧时间 | 22.3 ms | 14.2 ms | 34.2 ms | **22.7 ms** |

游戏自己的 2 GiB 地址空间里还能分配的内存（三张图依次）：1.30 64 位 1556 / 1565 / 1048 MiB，
单进程 1033 / 1040 / 481 MiB，原版 OpenGL 方式 1326 / 1339 / 808 MiB。

原版 1.27a 的主循环按 15.6 ms 一跳的系统时钟等待，OpenGL 又默认等垂直同步，所以自己卡在约 64 帧。
“去掉帧率限制”一列只解除这两处（在测试实例的进程里改两个导入指针，不加载任何文件），画面仍是原版；
隔离桌面上原版只能测 OpenGL 方式。光影全开大约用掉去上限原版四成的帧率；大地图上 64 位方式的 p99
帧时间和原版自带上限时相当，单进程方式明显更卡。测法与更多数据（64 位迁移的内存、这一轮的优化、
Gemini 的高清资源包压力测试）见仓库首页“性能报告”和 `docs/RELEASE_1.30.00.md`。

## 主要变化

- **64 位渲染（默认开启）。** 玩家包里带着 `WarVK\x64` 文件夹就自动启用，任务管理器里多一个
  `warvk-d3d9-host.exe`。删除或改名 `WarVK\x64` 就回到和 1.22.03 一样的单进程方式；64 位进程起不来时
  也会自动退回。
- **按类别设置阴影**：单位、建筑、装饰物、可破坏物、其他各自可以关掉投影或限制影子长度；贴地的部分
  不再投影；视角压低、单位又多时阴影不再整片消失；去掉魔兽烘在地面上的静态阴影。
- **太阳圆盘与一天的光色**：可以拖动太阳或锁定时刻；暖光冷影、日出日落前后的暮色、偏蓝的月光。
- **新水面（64 位渲染，独立显卡默认开）**：水深染色、浅水折射与水底光斑、倒影和天空、长浪与浪峰闪光、
  岸边泡沫。水面风格 0 就是游戏原来的水。
- **修复**：地狱火的绿光不再被照成淡黄（64 位渲染）；64 位渲染测试期间发现并已修好的迷雾发亮、烟雾变成
  剪影、战役过场灰白、片头电影后黑屏。
- **画面取证默认关闭**；需要时用 `WarVK_EvidenceStart.cmd` 启动。

## 安装

完全退出游戏和编辑器，备份旧的 `d3d9.dll`，把 **WarVK-1.30.00-win32.zip** 的全部内容解压到 `war3.exe`
同级目录：`war3.exe` 旁边应当直接有 `d3d9.dll` 和 `WarVK` 文件夹（里面有 `x64`）。只支持 Warcraft III
1.27a / 32 位，需要 64 位 Windows 10/11。用过 `WarVK_LockD3D9.cmd` 的，更新前先运行
`WarVK_UnlockD3D9.cmd`。作者另下载 author-kit（接口与 1.22.03 相同）。

## 验证范围（请如实阅读）

- 正式 DLL 通过断言开启的 CPU 测试 122/122、全量静态脚本 300/300、正式配置审计与精确
  no-work 检查；64 位宿主在冻结源码里构建，前端为 11-18 冻结的 f8。
- 在单块 RTX 4060 Ti（驱动 621.01）的隔离自动化中通过：启动诊断 4 场景；64 位方式下的单位 Walk、
  同进程 A→B→A 换图、“生与死”高压路线 188 秒（无 GPU 事件、无设备丢失）、天气功能（2560×1440）；
  单进程方式的单位 Walk；以及上面的四种方式 × 三张图 × 两轮性能对照（24 次全部通过）。

- **这个正式构建还没有玩家实机反馈。** 只在一块 NVIDIA 显卡上做了隔离自动化测试；真实桌面上的
  Alt-Tab、改窗口大小、全屏、AMD/Intel 显卡、联机没有自动化覆盖。64 位渲染下请关闭 MSAA；画面比单进程
  方式略亮、阴影偶发撕裂，原因还没查明。完整边界见 `docs/RELEASE_1.30.00.md`。

---

WarVK 1.30 moves the Vulkan renderer (DXVK, the GPU driver, shadows and post-processing) into a separate
64-bit process, so the 32-bit game keeps about 500 MiB more of its 2 GiB address space; large maps and HD
asset packs are far less likely to run out of memory. Also new: per-category shadow settings, a sun dial,
retuned day/night light colours, a new water surface (64-bit mode), and fixes for campaign fades and fog.
On the test machine (Ryzen 5 5600X, RTX 4060 Ti) with lighting on, 64-bit mode runs Lost Temple /
EchoIsles / the large map 生与死 at 443 / 317 / 67 fps, against 227 / 153 / 43 fps in single-process
mode and the original's own ~64 fps cap (701 / 547 / 116 fps with only that cap lifted and no WarVK
effects). Extract the whole zip next to `war3.exe` (the `WarVK\x64` folder must be there); delete
or rename `WarVK\x64` to go back to the single-process mode of 1.22.03. Tested on one NVIDIA GPU in
isolated automation; this build has no player acceptance yet. Back up your DLL and exit the game before
upgrading.
