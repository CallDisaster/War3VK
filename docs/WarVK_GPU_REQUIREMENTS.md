# WarVK 显卡与驱动要求

WarVK 通过 Vulkan 渲染。显卡和驱动必须支持 **Vulkan 1.3**，并提供 `VK_KHR_maintenance5`、`VK_KHR_maintenance6`、`VK_EXT_robustness2`（含 `nullDescriptor`）等扩展。完整列表见 `src/dxvk/dxvk_device_info.cpp`。

下面的“最低驱动”是第一个同时提供这些扩展的公开驱动。建议直接安装最新驱动。

## 最低驱动

| 厂商 | 最低驱动 | 下载 |
|---|---|---|
| NVIDIA | **551.61**（2024-02） | https://www.nvidia.cn/drivers/ |
| AMD | **Adrenalin 24.2.1**（2024-02） | https://www.amd.com/zh-cn/support/download/drivers.html |
| Intel | **32.0.101.6557** | https://www.intel.cn/content/www/cn/zh/download-center/home.html |

依据（2026-11 核对）：
- **NVIDIA**：551.23 只报告 Vulkan 1.3.271，没有 maintenance6；551.61 报告 1.3.277 并带 maintenance6。开发者测试版更早就有这两个扩展（maintenance5 自 532.34，maintenance6 自 538.09），但普通玩家装不到。
- **AMD**：Adrenalin 24.2.1 的发布说明里新增了 `VK_KHR_maintenance6`。
- **Intel**：Arc 驱动 32.0.101.6557 起列出 `VK_KHR_maintenance6`。11 代及更新酷睿的 Iris Xe 核显用同一个驱动分支，但**尚未实测**。

## 支持与不支持的显卡

| 厂商 | 支持 | 不支持 |
|---|---|---|
| NVIDIA | Maxwell 及更新的显卡：GTX 745/750/750 Ti、GTX 900/10/16 系列、RTX 全系列，以及 MX110/130/150/250/350/450/550/570 笔记本显卡 | Kepler 架构：GT 710/730、GTX 600 系列、GTX 700 系列的大部分型号、920M 等。驱动最高只到 474.xx（Vulkan 1.2） |
| AMD | RDNA 及更新的显卡：RX 5000/6000/7000/9000 系列；Ryzen 6000 及更新处理器的 Radeon 610M/660M/680M/740M/760M/780M/880M/890M 核显 | Polaris（RX 400/500 系列）、Vega 56/64、Radeon VII，以及带 Vega 核显的 Ryzen 处理器（如 5600G/5700G）：Windows 驱动缺少 maintenance5。HD 7000、R7/R9 200/300、Fury 等 GCN1～3 显卡：驱动停在 21.5.2（Vulkan 1.2） |
| Intel | Arc A/B 系列独立显卡、Core Ultra 处理器核显；11 代及更新酷睿的 Iris Xe 核显（未实测） | 6～10 代酷睿的 HD/UHD 6xx 核显和 Iris Plus 核显：旧驱动分支不提供所需扩展（未实测，按不支持处理） |

双显卡笔记本（如 Intel UHD 610 + MX150）只要独显受支持就能运行。游戏没有用上独显时，在 Windows“设置 → 系统 → 显示 → 图形”里把 `war3.exe` 设为“高性能”。

## 常见显卡显存与自动档位

WarVK 启动时从驱动读取**实际显存**，按下面的规则自动选档，不靠型号表。同一型号常有不同显存的版本，例如 GTX 1060 有 3 GB 和 6 GB，MX150 有 2 GB 和 4 GB。

- **低**：显存 ≤ 2.5 GB，或核显（与内存共享）
- **中**：显存 2.5～5.5 GB
- **高**：显存 > 5.5 GB

驱动报告的显存通常比标称值少一点，所以分界线不卡在整数上。

| 显卡 | 常见显存 | 档位 |
|---|---|---|
| GTX 750 / 750 Ti、GTX 950、GT 1030、MX110/130/150/250/350/450 | 2 GB | 低 |
| GTX 960、GTX 1050 | 2 GB / 4 GB 或 3 GB | 低 / 中 |
| GTX 970、GTX 980、GTX 1050 Ti、GTX 1650、MX150/MX550 4 GB 版、笔记本 RTX 3050 4 GB | 4 GB | 中 |
| GTX 1060 | 3 GB / 6 GB | 中 / 高 |
| GTX 980 Ti、GTX 1660 系列、RTX 2060 | 6 GB | 高 |
| GTX 1070/1080/1080 Ti、RTX 20/30/40/50 系列（除上面列出的） | 8 GB 及以上 | 高 |
| RX 5500 XT、RX 6500 XT | 4 GB / 8 GB | 中 / 高 |
| RX 6400、Arc A310 | 4 GB | 中 |
| RX 5600 XT、Arc A380 | 6 GB | 高 |
| RX 5700、RX 6600 及以上、RX 7000/9000 系列、Arc A580/A750/A770、B570/B580 | 8 GB 及以上 | 高 |
| Radeon 680M/780M/880M/890M、Core Ultra 核显、Iris Xe | 共享内存 | 低 |

### 各档位改动

| 档位 | 阴影贴图 | 说明 |
|---|---|---|
| 低 | 4 级 × 2048（约 80 MB） | 显存小的显卡不再常驻 4096 阴影贴图 |
| 中 / 高 | 4 级 × 4096（约 320 MB） | 启动时显存预算不够，仍会自动降到 2048 |

阴影 Arena 和 Stage11 快照池已经按驱动报告的显存预算动态增长，所以档位不再额外限制它们。中、高两档目前设置相同，留着给以后需要按显存区分的设置用。

检测结果写在游戏目录 `WarVK\Log\gpu.txt`，例如：

```
WarVK GPU: NVIDIA GeForce MX150 ... deviceLocalMiB=2048 ... tier=low source=auto csmResolutionCap=2048
```

### 手动指定档位

在游戏目录新建 `WarVK\warvk.ini`：

```ini
[Graphics]
; auto（默认）、low、medium、high，也可以写 低、中、高
VramPreset=auto
```

环境变量 `DXVK_WAR3_VRAM_PRESET` 优先于 ini。`DXVK_WAR3_SHADOW_RES` 等单项环境变量优先于档位。

## 启动失败时的提示

以前没有可用显卡或驱动太旧时，游戏会直接退出，不留任何提示。现在会弹出中文对话框，写明：

- 失败原因，例如“驱动缺少 WarVK 必需的 Vulkan 扩展 VK_KHR_maintenance6”；
- 检测到的每块显卡、显存和**当前驱动版本**；
- 需要安装的最低驱动版本和下载地址；
- 驱动已经够新却仍然失败时，说明这块显卡本身不在支持范围内；
- 双显卡笔记本怎样切到独显；
- 实在不行，删除 `d3d9.dll` 就能恢复原版画面。

同样的内容和技术细节保存在 `WarVK\Log\startup-error.txt`，反馈问题时附上这个文件即可。在对话框里按 Ctrl+C 可以复制全部文字。

对话框只在玩家正在使用的桌面上弹出，自动化测试使用的隔离桌面上不弹。环境变量 `DXVK_WAR3_STARTUP_DIALOG=0` 关闭对话框，`=force` 在任何桌面都弹。

## 防止平台删除 d3d9.dll

把 `WarVK_LockD3D9.cmd` 放到 `war3.exe` 所在目录，双击运行（需要管理员权限）。之后任何程序都不能删除、改名或覆盖 `d3d9.dll`，游戏照常加载它。更新 WarVK 之前先运行 `WarVK_UnlockD3D9.cmd`。

这个锁只防止文件被删除或替换。平台如果直接结束游戏进程，或者发现文件删不掉就拒绝启动，锁定解决不了。
