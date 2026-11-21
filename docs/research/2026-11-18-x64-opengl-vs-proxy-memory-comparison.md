# OpenGL vs. WarVK 进程内 vs. WarVK x64 代理内存占用对比实验报告

- 日期：2026-11-18
- 实验目录：[x64-va-gl-vs-proxy-20261118](file:///e:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/AutoTest/artifacts/x64-va-gl-vs-proxy-20261118)
- 实验环境：Warcraft III 1.27a 隔离沙盒（`E:\Work\War3_AutoTestSandbox`，非输入隔离桌面 `IsolatedRenderDesktop`，受限 Medium token，RTX 4060 Ti）
- 玩家目录保护：`E:\Work\Warcraft III\d3d9.dll` 实验前后只读校验均保持 `22,855,680 B`、SHA-256 `C7F16DBCE34944CD104DDD71AC04CF0D58DD4875A8B199AB456CE332415FCF8A` 未改。

---

## 1. 测试样本与被测版本身份

### 1.1 二进制身份

| 组件 | 大小 (Bytes) | SHA-256 | 说明 |
| --- | ---: | --- | --- |
| `f8 d3d9.dll` | 22,855,680 | `C7F16DBCE34944CD104DDD71AC04CF0D58DD4875A8B199AB456CE332415FCF8A` | `proxy` 与 `inproc` 共用同一份冻结产物 |
| `f8 front.dll` | 1,257,472 | `14E995B67B7435ABB5535F0019E855077D5F50EEBD602885CA0D0C0600609AB4` | `proxy` 前端桥接 DLL |
| `f8 warvk-d3d9-host-dev.exe` | 7,725,056 | `8C88C9D8D3DAACFEDDA0B5B5A708C55016D880D1FC9F147BA4429C7C500CADF2` | 64 位独立渲染宿主 |
| 沙盒 `Storm.dll` | 334,312 | `F8F519CFAA6275A5172A014F0ABED2212284390A33F1194677155A7D408E63EB` | 原版 1.27a Storm |
| 沙盒 `Game.dll` | 13,187,048 | `E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A` | 原版 1.27a Game |
| 沙盒 `War3.exe` | 514,536 | `EA8F5192EFEDE23F84BC60140C2D4A0085EA68B86F430C1D60C354A555922DEF` | `stock` 清除 PE `LARGE_ADDRESS_AWARE`，`laa` 保留 |

### 1.2 对比组设计

- `gl` / `glclean`：`war3.exe -window -opengl -loadfile MAP`。其中 `gl` 目录放有 `d3d9.dll`（实测 `Game.dll` 启动期短暂探测后立即卸载 `d3d9.dll`，从未 `CreateDevice`、也未挂载 StormBreaker）；`glclean` 为目录内完全不含 `d3d9.dll` 的纯净原版 `-opengl` 复核组。
- `inproc`：`war3.exe -window -loadfile MAP`，加载冻结版 `f8 d3d9.dll`（不启 64 位宿主，DXVK + `vulkan-1.dll` + NVIDIA `nvoglv32.dll` 全部运行在 32 位 `War3.exe` 进程内，启用内置 StormBreaker）。
- `proxy`：`war3.exe -window -loadfile MAP`，加载冻结版 `f8 d3d9.dll` + `front.dll` + `warvk-d3d9-host-dev.exe`（`product-async-delta` 配置，DXVK / Vulkan / GPU 驱动完全移至 64 位宿主进程，32 位游戏进程保留内置 StormBreaker）。
- `d3d`（诊断组）：无 `d3d9.dll` 的原生 D3D9 路径（见 §5 边界说明）。

所有度量均从外部只读采样（`VirtualQueryEx` 全 32 位地址空间分区、`ReadProcessMemory` 读取 `Storm.dll+0x5738C` `g_TotalAllocatedMemory`、遍历 `Storm.dll+0x56F80` 256 桶原生堆表、读取 `f8 d3d9.dll` 内置 TLSF `managedSize` 全局量），不注入额外探针。

---

## 2. 四张地图、双寻址模式（`stock` / `laa`）核心对比数据

### 2.1 能正常进图的三张地图稳态对比（`exe=stock`，标准 2 GiB 寻址上限）

| 地图 | 组别 | 轮次 | 退出码 | 句柄数 | 低 2 GiB 可用 `lowUsable` (MiB) | 运行最低 `minLowUsable` (MiB) | 最大连续空闲 `lowLargest` (MiB) | 32位 `image` 占用 (MiB) | 32位 `private` 提交/保留 (MiB) | `Storm+0x5738C` 计数器 (MiB) | TLSF 托管 (MiB) | Storm 堆实占 + TLSF (MiB) | 加载图形/桥接 DLL |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Lost Temple (0.23 MiB) | `inproc` | r1 | 0 | 3644 | 1030.5 | 1022.3 | 576.8 | 257.5 | 311.4 / 321.4 | 34.8 | 36.0 | 35.0 + 36.0 = **71.0** | `d3d9`, `vulkan-1`, `nvoglv32` |
| Lost Temple | `gl` | r1/r2 | 0/0 | 3644 | 1318.4 / 1318.4 | 1309.8 / 1309.8 | 829.0 / 829.0 | 225.9 | 142.2 / 297.2 | **210.7 / 210.6** | — | 72.1 + 0 = **72.1** | `opengl32`, `nvoglv32` |
| Lost Temple | `glclean` | r1 | 0 | 3644 | 1321.2 | 1312.6 | 829.0 | 224.6 | 142.7 / 295.4 | **210.6** | — | 72.1 + 0 = **72.1** | `opengl32`, `nvoglv32` |
| Lost Temple | **`proxy`** | r1/r2 | 0/0 | 3644 | **1551.8 / 1550.5** | **1545.4 / 1544.2** | **1000.4 / 1000.4** | **125.9** | **140.5 / 168.5** | **34.8 / 34.8** | 36.0 | 35.0 + 36.0 = **71.0** | `d3d9`, `front` (无 `nvoglv32`/`vulkan-1`) |
| EchoIsles (0.11 MiB) | `inproc` | r1 | 0 | 1985 | 1037.5 | 1029.9 | 583.8 | 257.5 | 308.9 / 316.3 | 30.6 | 31.7 | 30.2 + 31.7 = **61.9** | `d3d9`, `vulkan-1`, `nvoglv32` |
| EchoIsles | `gl` | r1/r2 | 0/0 | 1985 | 1331.7 / 1331.7 | 1322.9 / 1322.9 | 829.0 / 829.0 | 225.9 | 130.4 / 295.8 | **192.4 / 192.4** | — | 63.1 + 0 = **63.1** | `opengl32`, `nvoglv32` |
| EchoIsles | **`proxy`** | r1/r2 | 0/0 | 1985 | **1560.5 / 1560.6** | **1554.2 / 1554.2** | **1000.4 / 1000.4** | **125.9** | **133.8 / 166.5** | **30.6 / 30.6** | 31.7 | 30.2 + 31.7 = **61.9** | `d3d9`, `front` |
| 生与死 (59.4 MiB) | `inproc` | r1 | 0 | 19286 | **476.3** | **463.9** | **125.6** | 258.8 | 786.0 / 388.4 | 212.9 | 169.3 | 249.3 + 169.3 = **418.6** | `d3d9`, `vulkan-1`, `nvoglv32` |
| 生与死 | `gl` | r1/r2 | 0/0 | 19286 | 783.7 / 800.3 | 777.3 / 791.4 | 296.8 / 316.1 | 227.2 | 622.9 / 341.2 | **1382.0 / 1382.0** | — | 423.4 + 0 = **423.4** | `opengl32`, `nvoglv32` |
| 生与死 | `glclean` | r1 | 0 | 19286 | 804.3 | 792.0 | 316.2 | 225.9 | 604.2 / 340.9 | **1382.0** | — | 423.3 + 0 = **423.3** | `opengl32`, `nvoglv32` |
| 生与死 | **`proxy`** | r1/r2 | 0/0 | 19286 | **1042.1 / 1042.2** | **1037.0 / 1033.5** | **647.9 / 638.2** | **127.2** | **557.3 / 259.4** | **212.9 / 212.8** | 169.3 | 249.3 + 169.3 = **418.6** | `d3d9`, `front` (无 `nvoglv32`/`vulkan-1`) |

### 2.2 净收益对比（以 `《生与死》stock` 为例）

- **`proxy` 对比 `-opengl`（`gl` / `glclean`）**：
  - 低 2 GiB 可用虚拟地址空间：**多出 `+237.8 ~ +258.5 MiB`**（`LostTemple` 多 `+229.3 ~ +233.4 MiB`；`EchoIsles` 多 `+228.8 ~ +228.9 MiB`）。
  - 最大连续空闲块：**翻倍还多（`638.2 ~ 647.9 MiB` vs `296.8 ~ 316.2 MiB`，`+322 ~ +351 MiB`）**。
  - 32 位进程内驱动占坑来源：`-opengl` 仍把 NVIDIA `nvoglv32.dll` 塞入 32 位地址空间，DLL `image` 多占 `~99 ~ 100 MiB`，`private.reserved` 多占 `~82 ~ 87 MiB`，`private.committed` 多占 `~45 ~ 66 MiB`。
  - **`Storm.dll+0x5738C` 计数器**：`proxy` 仅为 **`212.8 ~ 212.9 MiB`**，而 `-opengl` 高达 **`1382.0 MiB`**（**高出 6.5 倍 / `+1169 MiB`**）；且 `proxy` 的大块（`169.3 MiB`）由 TLSF 池集中管理，Storm 外部大块直调仅 `2.6 MiB`（`-opengl` 散落 `173.7 MiB`）。
- **`proxy` 对比 `inproc`（完全同一份 `f8 d3d9.dll` 仅关闭 64 位代理）**：
  - 两者 Storm 计数器（`212.9 MiB`）与 TLSF 托管（`169.3 MiB`）完全一致，纯由 **64 位代理迁移图形栈** 单项贡献：低 2 GiB 可用空间暴增 **`+565.8 MiB`**（`476.3 -> 1042.1 MiB`），最大连续空洞放大 **`5.16x`**（`125.6 -> 647.9 MiB`），私有提交降低 **`~228 MiB`**、DLL 镜像降低 **`131.6 MiB`**、映射段降低 **`~75 MiB`**。

### 2.3 LAA (`exe=laa`，4 GiB 寻址) 与主菜单基线对照

| 场景 | 组别 | 低 2 GiB 可用 `lowUsable` (MiB) | 最大连续空闲 `lowLargest` (MiB) | 高 2 GiB 可用 `highUsable` (MiB) | `Storm+0x5738C` (MiB) | TLSF (MiB) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `LostTemple (laa)` | `gl` | 1320.4 | 829.0 | 2044.6 | 210.6 | — |
| `LostTemple (laa)` | `proxy` | **1559.4** (**+239.0**) | **1000.4** | 2040.4 | **34.8** | 36.0 |
| `EchoIsles (laa)` | `gl` | 1332.5 | 829.0 | 2044.6 | 192.4 | — |
| `EchoIsles (laa)` | `proxy` | **1566.8** (**+234.3**) | **1000.4** | 2040.4 | **30.6** | 31.7 |
| `生与死 (laa)` | `inproc` | 481.5 | 130.2 | — | 212.9 | 169.3 |
| `生与死 (laa)` | `gl` | 803.5 | 310.2 | 2044.6 | 1382.1 | — |
| `生与死 (laa)` | **`proxy`** | **1049.8** (**+246.3** vs GL / **+568.3** vs InProc) | **649.2** | 2040.4 | **212.9** | 169.3 |
| 主菜单空载 (`menu`) | `oct22` 旧进程内 (`laa`) | 129.2 | 46.4 | — | 9.0 | — |
| 主菜单空载 (`menu`) | `f8` `inproc` (`laa`) | 1175.5 | 738.6 | — | 9.0 | 9.1 |
| 主菜单空载 (`menu`) | `gl` (`laa`) | 1417.1 | 829.0 | — | 82.3 | — |
| 主菜单空载 (`menu`) | **`proxy`** (`stock`) | **1630.9** (**+213.8** vs GL) | **1000.4** | 0.0 | 9.0 | 9.1 |

`stock` 与 `laa` 的低 2 GiB 对比几乎重合，说明 `proxy` 在**根本不开 LAA**（无需承受 `Game.dll` 高地址指针崩溃风险）的原始 `stock` 环境下就已经比 `-opengl` 多出 `~230 ~ 258 MiB` 低 2 GiB 可用空间。

---

## 3. 击碎传言的完整因果链

1. **没有“地图写着色器导致不开 OpenGL 进不去”**：
   - `Game.dll` 静态反汇编统计了全部 `350` 个有名 Storm 调用源文件（`1449` 个 `SMemAlloc`/`SMemReAlloc` 调用点），其中涉及图形/渲染/地形/字体的仅 `33` 个文件 `68` 处调用（且主要是 `CGxDeviceD3d`、`CGxDeviceOpenGL` 固定管线引擎类和纹理/字体/地形载入，无地图自定义着色器）。
   - 实测《生与死》下，`glclean` 实际 Storm 存活请求字节 `423.3 MiB` 与 `proxy` 的 `Storm 249.3 + TLSF 169.3 = 418.6 MiB` 几乎完全一致——OpenGL 并未替魔兽省下任何地图级“着色器资源”。
2. **为什么玩家曾觉得“开 `-opengl` 更能进图”**：
   - 过去在 32 位进程内直接跑 DXVK + Vulkan（或部分在 32 位进程持有镜像/驱动堆的 D3D9 路径）时，如上表实测所示：仅 `f8` 进程内模式就会把 `《生与死》` 的低 2 GiB 可用空间压到 **`476.3 MiB`**（`oct22` 未优化版本更是只停在主菜单就仅剩 **`129.2 MiB`**！），而 `-opengl` 甩掉了 DXVK/Vulkan 这层，能在 `《生与死》` 留下 **`784 ~ 804 MiB`**，让一些边缘地图勉强跨过低 2 GiB 门槛。
   - **但这是“曾经比 32 位进程内包装轻量”，而非“OpenGL 本身最省”或“不可替代”**。在 WarVK 迁入 **64 位代理架构（`f8`）** 后，所有图形驱动、DXVK、Vulkan 和 GPU 缓存全部撤出 32 位 `War3.exe`，`proxy` 直接留下了 **`1042 ~ 1050 MiB`**，比 `-opengl` 本身还多出 **~250 MiB** 低 2 GiB 空间。
3. **开 `-opengl` 反而带来严重隐患（Storm 原生 2 GiB 计数器漏洞与碎片化）**：
   - 开启 `-opengl` 后 `War3.exe` 不会使用 `d3d9.dll` 渲染设备，也就不会加载我们的 StormBreaker TLSF 分配器与计数器修正。
   - 原版 `Storm.dll` 存在已验证的记账缺陷：超过 `0xFE7B`（`~64 KiB`）的大块分配累加完整申请体积到 `Storm.dll+0x5738C`（`g_TotalAllocatedMemory`），释放时却按占位小块头部扣除几十字节。这导致 `LostTemple` 实占 `72.1 MiB` 却记账 `210.7 MiB`，《生与死》实占 `423.4 MiB` 却虚胖至 **`1382.0 MiB`**，而 `proxy` 通过 StormBreaker 稳控在 **`34.8 MiB`** 与 **`212.9 MiB`**，并把上百 MiB 大块统一收进 TLSF 减少 `VirtualAlloc` 低 2 GiB 碎片。

---

## 4. 对 `0x7FFFFFFF` 崩溃机制的源码/反汇编级精确校正

针对提问中提到的“魔兽 3 有相当一部分函数看 `Storm` 累计内存是否超过 `0x7FFFFFFF` 然后自行报错崩溃”，静态反汇编 `Storm.dll` 与 `Game.dll` 后需精确区分两点：

1. **内部真正在看什么**：
   - `Storm.dll` 中只有 `4` 条 `.text` 指令访问 `0x1505738C`（`g_TotalAllocatedMemory`）：alloc 加 `Size`（`0x1502b46e`）、free 减 16 位头字段（`0x1502b544`）、realloc 加差值（`0x1502b5e8`），以及 ordinal `406` 返回它（`0x1502bf16`）。
   - `StormHeap_Alloc` 内部的 `if (Size > 0x7FFFFFFF)` 检查的是**单次分配请求大小 `Size`**，并非 `g_TotalAllocatedMemory`；`Game.dll` 也只在单一内部统计对（`0x6f7039a0`）读取 ordinal `406` 做差值累计，并没有大面积判断 `g_TotalAllocatedMemory > 0x7FFFFFFF` 后直接自爆的分支（本轮《骑马与砍杀》中 `-opengl` 组该计数器冲到 `3944.1 MiB` / `2146.4 MiB` 越过 `0x7FFFFFFF` 时也未在此处立即崩溃）。
2. **为什么玩家确实都在 `2 GiB / 0x7FFFFFFF` 红线崩溃**：
   - 真正把进程卡死在 `0x7FFFFFFF` 的是 **Win32 低 2 GiB 虚拟地址空间（`0x10000..0x7FFFFFFF`）被图形驱动、DLL 镜像和碎片化堆占满**——一旦 `VirtualAlloc` 在低 2 GiB 找不到足够大的连续空洞并返回 `NULL`，`StormHeap_AllocPage`/`StormHeap_Alloc` 便会调用 `Storm_AllocErrorHandler` 强制 `ExitProcess(1)`；即便打上 LAA 补丁，`Game.dll` 仍存在大量按有符号数（`< 0x80000000`）或 `~ptr` 编码处理的指针风险。

---

## 5. 实验边界与已观察异常（如实记录）

1. **`glsb` 对照说明**：在仅把 `StormBreaker.asi` 放入实例根目录而不加 `.asi` 注入器时，`va-glsb-sy-stock-r2` 实测模块列表未包含 `.asi`、`Storm+0x5738C` 仍为 `1382.1 MiB`，证实原生 `War3.exe` 不会直接加载根目录下的 standalone `.asi`（`va-glsb-*-r1` 目录因跨会话重复占用曾报 `output must be fresh` 错误，由 `r2` 补齐有效数据）。
2. **`d3d` 原版路径边界**：`va-d3d-lt-stock-r0smoke` 与 `va-d3d-lt-stock-diag` 在非输入隔离桌面上加载 `System32\d3d9.dll` 与 `nvd3dum.dll` 后于 `t=2.0s` 弹出 `#32770` 错误框并卸载图形驱动（`inGame=False`），因此本实验未拿到原生直连 Windows D3D9 进图稳态样本，所有结论仅直接对比 `-opengl`（`gl`/`glclean`）、`warvk-inprocess`（`inproc`）与 `warvk-proxy`（`proxy`）。
3. **`《骑马与砍杀-战火纷争 v7.2.4》`（`111.97 MiB`）**：无大地图补丁的原版 `-loadfile` 下 `gl` 与 `proxy` 均停留在主菜单未进图（`steadyInGame=False`）。尝试加载前 5 秒期间 `gl` 的 `Storm+0x5738C` 仍飙至 `3944.1 MiB`（`4,135,733,416 B`）后回落停在 `2144 ~ 2146 MiB`，`proxy` 保持 `7.8 MiB + TLSF 9.0 MiB`；须同时记明，`va-proxy-mb-*`、`va-proxy-menu-stock-r1` 及 `va-inproc-menu-*` 在**未进图即从主菜单直接 `WM_CLOSE` 退出**时均出现退出收尾期 `0xC0000005`（如 `Crash.txt` 中 `GameMain` 退场阶段访问已卸载地址，退出码 `3221225477`），而所有正常进入地图的对局（`LostTemple`、`EchoIsles`、`生与死`）全部以 `exitCode=0` 正常退出、零崩溃文件。
4. **硬件范围**：本机实测仅覆盖 NVIDIA GeForce RTX 4060 Ti 驱动（`nvoglv32.dll`），不直接外推 AMD / Intel 显卡的 32 位 OpenGL ICD 体积差异。
