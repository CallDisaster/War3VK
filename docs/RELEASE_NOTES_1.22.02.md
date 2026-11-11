# WarVK v1.22.02 — 启动提示、显卡下限与维护更新

本版让启动失败不再静默，明确各厂商的驱动/显卡下限，并按显存自动选择画质档位；同时收入
9～11 月已可放出的稳定性与作者接口更新。Shader API 1.2.0、JASS wire `warvk:v1` 不变。

## 主要变化

- **启动失败会弹窗说明原因。** 以前缺少 Vulkan 或驱动太旧时游戏会直接退出。现在 WarVK 弹出
  中文对话框，列出失败原因、每块显卡的显存与当前驱动版本、需要的最低驱动版本与官方下载页、
  显卡本身是否支持，并提示“删除 d3d9.dll 即可恢复原版”。内容同时保存到
  `WarVK\Log\startup-error.txt`。关闭后魔兽争霸还会弹出自己的 DirectX 错误并退出。
- **显卡下限。** NVIDIA 驱动 **551.61** 起（GTX 750/900 及更新）；AMD Adrenalin **24.2.1** 起
  （RX 5000 及更新、Ryzen 6000 及更新核显）；Intel **32.0.101.6557** 起（Arc、Core Ultra）。
  GT 710/730、GTX 600/700（Kepler）、RX 400/500、Vega 不支持。详见包内 `WarVK_GPU_REQUIREMENTS.md`。
- **按显存自动选档。** ≤2.5 GB 或核显自动用“低”档（太阳阴影分辨率上限 2048，约省 240 MiB 显存），
  其余保持原设置。可在 `WarVK\warvk.ini` 的 `[Graphics]` 写 `VramPreset=低/中/高` 手动指定。
- **锁定 d3d9.dll。** 包内 `WarVK_LockD3D9.cmd` 防止对战平台或清理工具删除/替换 `d3d9.dll`；
  更新前先运行 `WarVK_UnlockD3D9.cmd`。
- **稳定性。** Storm `MDLDATAHash` 低地址保留（旧版低地址碎片报错）、每线程大表按需分配、
  NetEvent 安装时机、换图阴影会话 Reset、常驻阴影几何事务化，以及 64 位迁移中发现的 32 位
  共用问题修复。
- **作者接口。** 单位 Walk/移动动画控制；**天气预览**（雪片/雪雾/吹雪，默认关闭，画面与性能未
  验收）；`LocalizedString` 实数不再输出指数格式；实验性地图模块加载器默认拒绝。

## 安装

完全退出游戏和编辑器，备份旧的 `d3d9.dll`，把 **WarVK-1.22.02-win32.zip** 里的 `d3d9.dll`
放到 `war3.exe` 同目录。只支持 Warcraft III 1.27a / 32 位。作者另下载 author-kit。

## 验证范围（请如实阅读）

- 正式 DLL 通过断言开启的 CPU 测试 120/120、全量静态脚本 285/285、正式配置审计与
  精确 no-work 检查。
- 同一 DLL 在单块 RTX 4060 Ti（驱动 621.01）的隔离自动化中通过：启动失败对话框与显存档位 4 场景、
  天气功能（2560×1440）、单位 Walk、“生与死”高压路线 184 秒（无 GPU 事件），以及同进程
  A→B→A 换图（阴影会话每次切换只重置一次、三张截图正确；旧分析器因请求序号合并判为未通过，
  原因见 `docs/RELEASE_1.22.02.md`）。
- **这一个 DLL 还没有玩家实机反馈**；AMD/Intel 只依据公开驱动记录，没有实机验证。天气预览没有
  画面/性能验收。仍建议每张地图重开游戏。

---

Startup failures now show a dialog with the reason, each GPU's VRAM and driver
version, the minimum driver (NVIDIA 551.61, AMD 24.2.1, Intel 32.0.101.6557) and
whether the GPU is supported. A VRAM preset lowers sun-shadow resolution on
≤2.5 GB/integrated GPUs. Also: Storm low-address and per-thread memory fixes,
unit animation helpers, a default-off weather preview and lock/unlock scripts
for d3d9.dll. Tested on one NVIDIA GPU in isolated automation; this exact DLL
has no player acceptance yet. Back up your DLL and exit the game before upgrading.
