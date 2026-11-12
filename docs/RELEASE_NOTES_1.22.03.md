# WarVK v1.22.03 — 面板设置保存与光影修复

小版本：Ctrl+F1 面板的设置现在会自动保存，并修复四个光影显示问题。Shader API 1.2.0、
JASS wire `warvk:v1` 不变，从 1.22.02 直接替换 `d3d9.dll` 即可。

## 主要变化

- **面板设置自动保存。** 在 Ctrl+F1 面板“内置渲染设置”里改过的项（光影、点光源、描边、AA、SSAO、
  体积光、Bloom、日夜色调等）会在停止调整约 1.5 秒后或关闭面板时写入 `WarVK\settings.ini`，
  下次启动沿用。只记录改过的项；面板里有“恢复默认设置”，删除该文件也可以恢复默认。
- **单位头像不再被光影带偏。** 旧版把世界的太阳方向和强度也套到了头像框里的模型上，头像会随
  时间过暗或过亮。现在太阳/环境光改写只作用于世界场景，头像使用游戏自己的灯光。
- **暂停或打开菜单时，阴影、体积光不再盖到界面上。** 单机按 F10 暂停后，阴影、体积光、SSAO、
  Bloom 等会叠在菜单和状态栏上，本版已修复。
- **有电脑玩家的对战局，阴影不再每秒左右闪一帧。** 对战 AI 脚本运行时，旧版会误以为换了地图
  而反复重置整套阴影（那一帧没有阴影和后处理，随后还要重建缓存）。1.22.02 同样受影响。
- **体积光更清晰、更明显。** 体积光专用阴影图 1536→2048 并收窄近层范围（近处每纹素
  5.5→2.9 世界单位）；2560×1440 和 4K 下光柱边缘的引导图不再直接掉到 1/8 分辨率。默认强度
  1.2→1.5、对比 2.1→2.5，被遮挡光柱的压暗上限 24%→36%，雾量不变。体积光仍默认关闭；开启后
  显存多占约 14 MiB。

## 安装

完全退出游戏和编辑器，备份旧的 `d3d9.dll`，把 **WarVK-1.22.03-win32.zip** 里的 `d3d9.dll`
放到 `war3.exe` 同目录。只支持 Warcraft III 1.27a / 32 位。如果用过 `WarVK_LockD3D9.cmd`，
更新前先运行 `WarVK_UnlockD3D9.cmd`。作者另下载 author-kit（接口与 1.22.02 相同）。

## 验证范围（请如实阅读）

- 正式 DLL 通过断言开启的 CPU 测试 122/122、全量静态脚本 287/287、正式配置审计与精确
  no-work 检查。
- 同一 DLL 在单块 RTX 4060 Ti（驱动 621.01）的隔离自动化中通过：启动诊断 4 场景、天气功能
  （2560×1440）、单位 Walk、“生与死”高压路线 184 秒（无 GPU 事件），以及同进程 A→B→A 换图
  （每次切换只重置一次阴影会话；旧分析器的判定与 1.22.02 相同，原因见
  `docs/RELEASE_1.22.03.md`）。
- 四个光影问题都在隔离沙盒里先复现、后验证：头像亮度在正午/午夜/黎明保持不变；暂停帧的菜单和
  状态栏与恢复后逐像素一致；含电脑玩家的对战图上阴影重置次数从“1200 帧 24 次”降为 0。
- **这一个 DLL 还没有玩家实机反馈。** 只在一块 NVIDIA 显卡上测试；暂停修复只验证了单机 F10。
  体积光新默认值是按一张对战图的截图调的，俯视角下仍以整体雾化为主，不同地图和时间段可以在
  面板里自行微调。仍建议每张地图重开游戏。

---

Panel settings changed in the Ctrl+F1 "built-in render settings" page are now
saved to `WarVK\settings.ini`. Fixes: unit portraits no longer get tinted by the
WarVK sun/ambient light; shadows and volumetric light no longer draw over the UI
while the game is paused; melee games with computer players no longer lose a
shadow frame about once a second (the AI script runner was mistaken for a map
change); volumetric light has a sharper shadow map (1536→2048, tighter near
layer) and stronger defaults. Tested on one NVIDIA GPU in isolated automation;
this exact DLL has no player acceptance yet. Back up your DLL and exit the game
before upgrading.
